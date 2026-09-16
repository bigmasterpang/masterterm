#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""MasterTerm 云端同步服务（零依赖，Python 3.8+ 标准库）。

提供注册 / 登录 / 数据存取 / 历史快照四个接口，每用户一份同步数据（连接列表、
本地命令历史、软件设置组成的 JSON payload）。每次上传前自动保留旧数据快照，
支持查看和恢复历史版本。

接口：
  POST /api/register  {"username","password"}            -> {"token"}
  POST /api/login     {"username","password"}            -> {"token"}
  GET  /api/sync      Authorization: Bearer <token>      -> {"data":..., "updatedAt":...}
  PUT  /api/sync      Authorization: Bearer <token>      -> {"updatedAt":...}
                       body: {"data": { ...同步 payload... }}
  GET  /api/sync/history     -> {"snapshots":[{"seq":N,"updatedAt":...}]}
  POST /api/sync/restore     body {"seq":N}               -> {"updatedAt":...}

用法：
  python3 cloud-sync.py --port 8443 --cert fullchain.pem --key privkey.pem
  python3 cloud-sync.py --port 8080            # 仅 HTTP（客户端需配 http://）

建议用 Caddy/nginx 反代 443 并终止 TLS，或 certbot 证书 + 本服务直听 443。
"""

import argparse
import hashlib
import hmac
import json
import os
import secrets
import sqlite3
import ssl
import threading
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

MAX_BODY_BYTES = 5 * 1024 * 1024  # 5 MiB
DEFAULT_USER_MAX_SNAPSHOTS = 3    # 普通用户最多保留的历史版本数（防止恶意滥用）
ADMIN_MAX_SNAPSHOTS = 50          # 管理员用户保留的历史版本数
MAX_SNAPSHOTS = DEFAULT_USER_MAX_SNAPSHOTS
USERNAME_PATTERN_OK = lambda name: (3 <= len(name) <= 32
                                    and all(c.isalnum() or c in "_-" for c in name))
MIN_PASSWORD_LENGTH = 6

DB_SCHEMA = """
CREATE TABLE IF NOT EXISTS users (
    username TEXT PRIMARY KEY,
    salt TEXT NOT NULL,
    hash TEXT NOT NULL,
    role TEXT NOT NULL DEFAULT 'user',
    created_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS tokens (
    token TEXT PRIMARY KEY,
    username TEXT NOT NULL,
    created_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS sync_data (
    username TEXT PRIMARY KEY,
    payload TEXT,
    updated_at TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS sync_snapshots (
    username TEXT NOT NULL,
    seq INTEGER NOT NULL,
    payload TEXT,
    name TEXT NOT NULL DEFAULT '',
    updated_at TEXT NOT NULL,
    PRIMARY KEY (username, seq)
);
CREATE TABLE IF NOT EXISTS sync_seq (
    username TEXT PRIMARY KEY,
    seq INTEGER NOT NULL
);
"""


class SimpleRateLimiter:
    """简单的滑动窗口 IP 速率限制器（内存存储），防范密码爆破与算力耗尽。"""
    def __init__(self):
        self._lock = threading.Lock()
        self._buckets: dict[str, list[float]] = {}

    def is_allowed(self, key: str, max_requests: int, window_seconds: float) -> bool:
        now = time.time()
        with self._lock:
            timestamps = self._buckets.get(key, [])
            cutoff = now - window_seconds
            valid = [t for t in timestamps if t > cutoff]
            if len(valid) >= max_requests:
                self._buckets[key] = valid
                return False
            valid.append(now)
            self._buckets[key] = valid
            if len(self._buckets) > 10000:
                self._buckets = {k: v for k, v in self._buckets.items() if v and v[-1] > cutoff}
            return True


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def hash_password(password: str, salt: str) -> str:
    digest = hashlib.pbkdf2_hmac(
        "sha256", password.encode("utf-8"), salt.encode("utf-8"), 120000)
    return digest.hex()


class SyncDatabase:
    def __init__(self, path: str):
        self._lock = threading.Lock()
        self._db = sqlite3.connect(path, check_same_thread=False)
        self._db.executescript(DB_SCHEMA)
        # Existing installations created before snapshot names were added
        # need a lightweight in-place migration.
        columns = {row[1] for row in self._db.execute(
            "PRAGMA table_info(sync_snapshots)").fetchall()}
        if "name" not in columns:
            self._db.execute(
                "ALTER TABLE sync_snapshots ADD COLUMN name TEXT NOT NULL DEFAULT ''")
        user_columns = {row[1] for row in self._db.execute(
            "PRAGMA table_info(users)").fetchall()}
        if "role" not in user_columns:
            self._db.execute(
                "ALTER TABLE users ADD COLUMN role TEXT NOT NULL DEFAULT 'user'")
        self._db.commit()

    def get_quota(self, username: str) -> int:
        row = self._db.execute(
            "SELECT role FROM users WHERE username = ?", (username,)).fetchone()
        role = row[0] if row else "user"
        return ADMIN_MAX_SNAPSHOTS if role == "admin" else DEFAULT_USER_MAX_SNAPSHOTS

    def set_role(self, username: str, role: str) -> bool:
        with self._lock:
            cur = self._db.execute(
                "UPDATE users SET role = ? WHERE username = ?", (role, username))
            self._db.commit()
            return cur.rowcount > 0

    def register(self, username: str, password: str) -> str | None:
        """创建用户并返回新 token；用户名已存在返回 None。"""
        with self._lock:
            existing = self._db.execute(
                "SELECT 1 FROM users WHERE username = ?", (username,)).fetchone()
            if existing:
                return None
            salt = secrets.token_hex(16)
            now = utc_now()
            self._db.execute(
                "INSERT INTO users (username, salt, hash, role, created_at) VALUES (?,?,?,?,?)",
                (username, salt, hash_password(password, salt), "user", now))
            token = self._issue_token_locked(username, now)
            self._db.commit()
            return token

    def login(self, username: str, password: str) -> str | None:
        with self._lock:
            row = self._db.execute(
                "SELECT salt, hash FROM users WHERE username = ?", (username,)).fetchone()
            if not row:
                return None
            salt, stored = row
            if not hmac.compare_digest(stored, hash_password(password, salt)):
                return None
            token = self._issue_token_locked(username, utc_now())
            self._db.commit()
            return token

    def _issue_token_locked(self, username: str, now: str) -> str:
        token = secrets.token_hex(32)
        self._db.execute(
            "INSERT INTO tokens (token, username, created_at) VALUES (?,?,?)",
            (token, username, now))
        return token

    def user_for_token(self, token: str) -> str | None:
        row = self._db.execute(
            "SELECT username FROM tokens WHERE token = ?", (token,)).fetchone()
        return row[0] if row else None

    def load(self, username: str):
        row = self._db.execute(
            "SELECT payload, updated_at FROM sync_data WHERE username = ?",
            (username,)).fetchone()
        return (json.loads(row[0]) if row and row[0] else None,
                row[1] if row else None)

    def save(self, username: str, payload) -> tuple[str | None, str | None]:
        now = utc_now()
        with self._lock:
            # 覆盖前把当前数据存入历史快照（T2-4），严格受配额限制（普通用户最多 3 个）
            current = self._db.execute(
                "SELECT payload, updated_at FROM sync_data WHERE username = ?",
                (username,)).fetchone()
            quota = self.get_quota(username)
            if current and current[0] is not None:
                count_row = self._db.execute(
                    "SELECT COUNT(1) FROM sync_snapshots WHERE username = ?",
                    (username,)).fetchone()
                existing_count = count_row[0] if count_row else 0
                if existing_count >= quota:
                    return None, f"备份数量已达上限（普通用户最多保留 {quota} 个备份版本），请前往【版本历史】删除旧版本后再备份。"

                seq = self._next_seq_locked(username)
                self._db.execute(
                    "INSERT INTO sync_snapshots "
                    "(username, seq, payload, name, updated_at) VALUES (?,?,?,?,?)",
                    (username, seq, current[0], "", current[1]))
                self._db.execute(
                    "DELETE FROM sync_snapshots WHERE username = ? AND seq NOT IN ("
                    "SELECT seq FROM sync_snapshots WHERE username = ? "
                    "ORDER BY seq DESC LIMIT ?)",
                    (username, username, quota))
            self._db.execute(
                "INSERT INTO sync_data (username, payload, updated_at) VALUES (?,?,?) "
                "ON CONFLICT(username) DO UPDATE SET payload=excluded.payload, "
                "updated_at=excluded.updated_at",
                (username, json.dumps(payload, ensure_ascii=False), now))
            self._db.commit()
            return now, None

    def _next_seq_locked(self, username: str) -> int:
        row = self._db.execute(
            "SELECT seq FROM sync_seq WHERE username = ?", (username,)).fetchone()
        seq = (row[0] + 1) if row else 1
        self._db.execute(
            "INSERT INTO sync_seq (username, seq) VALUES (?,?) "
            "ON CONFLICT(username) DO UPDATE SET seq=excluded.seq",
            (username, seq))
        return seq

    def history(self, username: str):
        with self._lock:
            rows = self._db.execute(
                "SELECT seq, name, updated_at FROM sync_snapshots "
                "WHERE username = ? ORDER BY seq DESC",
                (username,)).fetchall()
            return [{"seq": row[0], "name": row[1] or "",
                     "updatedAt": row[2]} for row in rows]

    def rename(self, username: str, seq: int, name: str) -> bool:
        clean_name = str(name or "").strip()[:80]
        with self._lock:
            cursor = self._db.execute(
                "UPDATE sync_snapshots SET name = ? "
                "WHERE username = ? AND seq = ?",
                (clean_name, username, int(seq)))
            self._db.commit()
            return cursor.rowcount > 0

    def delete(self, username: str, seq: int) -> bool:
        with self._lock:
            cursor = self._db.execute(
                "DELETE FROM sync_snapshots WHERE username = ? AND seq = ?",
                (username, int(seq)))
            self._db.commit()
            return cursor.rowcount > 0

    def restore(self, username: str, seq: int) -> str | None:
        with self._lock:
            row = self._db.execute(
                "SELECT payload, updated_at FROM sync_snapshots "
                "WHERE username = ? AND seq = ?",
                (username, int(seq))).fetchone()
            if not row:
                return None
            self._db.execute(
                "INSERT INTO sync_data (username, payload, updated_at) VALUES (?,?,?) "
                "ON CONFLICT(username) DO UPDATE SET payload=excluded.payload, "
                "updated_at=excluded.updated_at",
                (username, row[0], row[1]))
            self._db.execute(
                "DELETE FROM sync_snapshots WHERE username = ? AND seq = ?",
                (username, int(seq)))
            self._db.commit()
            return row[1]


class SyncHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def restore(self, username: str, seq: int) -> str | None:
        with self._lock:
            row = self._db.execute(
                "SELECT payload, updated_at FROM sync_snapshots "
                "WHERE username = ? AND seq = ?",
                (username, int(seq))).fetchone()
            if not row:
                return None
            self._db.execute(
                "INSERT INTO sync_data (username, payload, updated_at) VALUES (?,?,?) "
                "ON CONFLICT(username) DO UPDATE SET payload=excluded.payload, "
                "updated_at=excluded.updated_at",
                (username, row[0], row[1]))
            self._db.execute(
                "DELETE FROM sync_snapshots WHERE username = ? AND seq = ?",
                (username, int(seq)))
            self._db.commit()
            return row[1]


class SyncHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "MasterTermCloudSync/1.0"

    def log_message(self, fmt, *args):  # 静默访问日志，仅记录错误
        pass

    def _send(self, status: int, payload):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _send_error(self, status: int, message: str):
        self._send(status, {"error": message})

    def _client_ip(self) -> str:
        forwarded = self.headers.get("X-Forwarded-For")
        if forwarded:
            return forwarded.split(",")[0].strip()
        return self.client_address[0] if self.client_address else "unknown"

    def _check_rate_limit(self, action: str, max_requests: int, window_seconds: float) -> bool:
        ip = self._client_ip()
        key = f"{action}:{ip}"
        if not self.server.limiter.is_allowed(key, max_requests, window_seconds):
            self._send_error(429, "请求过于频繁，请稍后再试")
            return False
        return True

    def _read_json_body(self):
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            return None
        if length <= 0 or length > MAX_BODY_BYTES:
            return None
        raw = self.rfile.read(length)
        try:
            return json.loads(raw.decode("utf-8"))
        except (json.JSONDecodeError, UnicodeDecodeError):
            return None

    def _require_token(self):
        header = self.headers.get("Authorization", "")
        if header.startswith("Bearer "):
            token = header[7:].strip()
            username = self.server.db.user_for_token(token)
            if username:
                return username
        self._send_error(401, "无效或过期的令牌")
        return None

    def _route_path(self) -> str:
        """返回去掉前缀后的 API 路径（/api/register 等）。

        服务支持挂在任意 URL 前缀下（如 --prefix /cloud-sync），避免与
        同一 nginx 上其它占用 /api/ 前缀的服务冲突。
        """
        path = urlparse(self.path).path
        prefix = self.server.prefix
        if prefix and path.startswith(prefix):
            path = path[len(prefix):]
        return path

    def _handle_register(self):
        if not self._check_rate_limit("register", 5, 60):
            return
        body = self._read_json_body()
        if not body or not isinstance(body, dict):
            self._send_error(400, "请求体必须是 JSON 对象")
            return
        username = str(body.get("username", "")).strip()
        password = str(body.get("password", ""))
        if not USERNAME_PATTERN_OK(username):
            self._send_error(400, "用户名需为 3-32 位字母、数字、下划线或短横线")
            return
        if len(password) < MIN_PASSWORD_LENGTH:
            self._send_error(400, "密码至少 %d 位" % MIN_PASSWORD_LENGTH)
            return
        token = self.server.db.register(username, password)
        if token is None:
            self._send_error(409, "用户名已存在")
            return
        self._send(200, {"token": token, "username": username})

    def _handle_login(self):
        if not self._check_rate_limit("login", 15, 60):
            return
        body = self._read_json_body()
        if not body or not isinstance(body, dict):
            self._send_error(400, "请求体必须是 JSON 对象")
            return
        username = str(body.get("username", "")).strip()
        password = str(body.get("password", ""))
        token = self.server.db.login(username, password)
        if token is None:
            self._send_error(401, "用户名或密码错误")
            return
        self._send(200, {"token": token, "username": username})

    def do_GET(self):
        path = self._route_path()
        if path == "/api/sync":
            self._handle_sync_get()
            return
        if path == "/api/sync/history":
            self._handle_history()
            return
        self._send_error(404, "接口不存在")

    def _handle_sync_get(self):
        username = self._require_token()
        if username is None:
            return
        payload, updated_at = self.server.db.load(username)
        self._send(200, {"data": payload, "updatedAt": updated_at})

    def _handle_history(self):
        username = self._require_token()
        if username is None:
            return
        self._send(200, {"snapshots": self.server.db.history(username)})

    def do_PUT(self):
        if self._route_path() != "/api/sync":
            self._send_error(404, "接口不存在")
            return
        if not self._check_rate_limit("sync_put", 30, 60):
            return
        username = self._require_token()
        if username is None:
            return
        body = self._read_json_body()
        if not body or "data" not in body or not isinstance(body.get("data"), dict):
            self._send_error(400, "请求体缺少有效的 data 对象")
            return
        updated_at, error = self.server.db.save(username, body["data"])
        if error:
            self._send_error(400, error)
            return
        self._send(200, {"updatedAt": updated_at})

    def do_POST(self):
        path = self._route_path()
        if path == "/api/register":
            self._handle_register()
        elif path == "/api/login":
            self._handle_login()
        elif path == "/api/sync/restore":
            self._handle_restore()
        elif path == "/api/sync/history/rename":
            self._handle_history_rename()
        elif path == "/api/sync/history/delete":
            self._handle_history_delete()
        else:
            self._send_error(404, "接口不存在")

    def _handle_restore(self):
        username = self._require_token()
        if username is None:
            return
        body = self._read_json_body()
        seq = self._history_seq(body)
        if seq is None:
            return
        updated_at = self.server.db.restore(username, seq)
        if updated_at is None:
            self._send_error(404, "历史版本不存在")
            return
        self._send(200, {"updatedAt": updated_at})

    def _history_seq(self, body):
        raw_seq = body.get("seq") if isinstance(body, dict) else None
        if isinstance(raw_seq, bool):
            seq = 0
        elif isinstance(raw_seq, int):
            seq = raw_seq
        elif isinstance(raw_seq, float) and raw_seq.is_integer():
            seq = int(raw_seq)
        elif isinstance(raw_seq, str) and raw_seq.strip().isdigit():
            seq = int(raw_seq.strip())
        else:
            seq = 0
        if seq <= 0:
            self._send_error(400, "请求体缺少有效的 seq 字段")
            return None
        return seq

    def _handle_history_rename(self):
        username = self._require_token()
        if username is None:
            return
        body = self._read_json_body()
        seq = self._history_seq(body)
        if seq is None:
            return
        if not isinstance(body.get("name"), str):
            self._send_error(400, "请求体缺少有效的 name 字段")
            return
        clean_name = "".join(c for c in body["name"] if c >= " " and c != "\x7f").strip()[:64]
        if not self.server.db.rename(username, seq, clean_name):
            self._send_error(404, "历史版本不存在")
            return
        self._send(200, {"ok": True})

    def _handle_history_delete(self):
        username = self._require_token()
        if username is None:
            return
        body = self._read_json_body()
        seq = self._history_seq(body)
        if seq is None:
            return
        if not self.server.db.delete(username, seq):
            self._send_error(404, "历史版本不存在")
            return
        self._send(200, {"ok": True})


def build_server(port: int, cert: str | None, key: str | None,
                 prefix: str = ""):
    server = ThreadingHTTPServer(("0.0.0.0", port), SyncHandler)
    server.db = SyncDatabase(
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "cloud-sync.db"))
    server.limiter = SimpleRateLimiter()
    server.prefix = prefix
    if cert and key:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
    return server


def main():
    parser = argparse.ArgumentParser(description="MasterTerm 云端同步服务")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--cert", help="TLS 证书（fullchain.pem）")
    parser.add_argument("--key", help="TLS 私钥（privkey.pem）")
    parser.add_argument("--prefix", default="",
                        help="URL 前缀（如 /cloud-sync），避免与同机其它服务冲突")
    parser.add_argument("--set-role", nargs=2, metavar=("USERNAME", "ROLE"),
                        help="设置用户角色（例如: --set-role admin admin）")
    args = parser.parse_args()
    if args.set_role:
        db = SyncDatabase(
            os.path.join(os.path.dirname(os.path.abspath(__file__)), "cloud-sync.db"))
        username, role = args.set_role
        if db.set_role(username, role):
            print(f"用户 {username} 的角色已更新为 {role}")
        else:
            print(f"用户 {username} 不存在")
        return
    server = build_server(args.port, args.cert, args.key, args.prefix)
    scheme = "https" if args.cert else "http"
    print("MasterTerm cloud sync listening on %s://0.0.0.0:%d%s"
          % (scheme, args.port, args.prefix or ""))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
