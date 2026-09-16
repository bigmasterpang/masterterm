# MasterTerm 云端同步服务部署

`cloud-sync.py` 是零依赖的 Python 3（标准库）同步服务：注册、登录、按用户保存一份
同步数据（连接列表、本地命令历史、软件设置）。数据存储在服务同目录的
`cloud-sync.db`（SQLite）。

## 接口

| 方法 | 路径 | 说明 |
| ---- | ---- | ---- |
| POST | `/api/register` | `{"username","password"}` → `{"token","username"}` |
| POST | `/api/login` | 同上，→ `{"token","username"}` |
| GET  | `/api/sync` | `Authorization: Bearer <token>` → `{"data","updatedAt"}` |
| PUT  | `/api/sync` | `Authorization: Bearer <token>`，body `{"data":{...}}` → `{"updatedAt"}` |
| GET  | `/api/sync/history` | `Authorization: Bearer <token>` → 最近的历史快照列表 |
| POST | `/api/sync/restore` | `Authorization: Bearer <token>`，body `{"seq":N}` → 恢复指定快照 |
| POST | `/api/sync/history/rename` | `Authorization: Bearer <token>`，body `{"seq":N,"name":"..."}` → 修改快照名称 |
| POST | `/api/sync/history/delete` | `Authorization: Bearer <token>`，body `{"seq":N}` → 删除快照 |

用户名：3-32 位字母/数字/下划线/短横线；密码至少 6 位。密码以 PBKDF2-SHA256
存储；登录后签发 64 位随机 token（当前无过期时间）。

## 在云服务器上部署

```bash
# 1. 上传
scp cloud-sync.py user@your-server.com:/opt/masterterm-cloud/

# 2. 方式 A（推荐）：Caddy 自动 HTTPS 反代
#    安装 caddy 后，Caddyfile：
#       sync.example.com {
#           reverse_proxy 127.0.0.1:8081
#       }
#    Caddy 自动申请证书，客户端服务器地址填 https://sync.example.com/cloud-sync

# 2'. 方式 B（已有 nginx 时）：监听本机端口 + nginx 反代 /cloud-sync/
python3 /opt/masterterm-cloud/cloud-sync.py --port 8081 --prefix /cloud-sync
# nginx 站点配置（在 443 server 块内加；务必使用 /cloud-sync/ 独立前缀，
# 避免与同机上其它占用 /api/ 的服务冲突）：
#   location /cloud-sync/ {
#       proxy_pass http://127.0.0.1:8081;
#       proxy_set_header Host $host;
#       proxy_set_header X-Real-IP $remote_addr;
#       proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
#       proxy_set_header X-Forwarded-Proto $scheme;
#   }
# 注意 proxy_pass 不要带 URI 后缀（保留 /cloud-sync/api/register 原路径）。
# 客户端服务器地址填：https://你的域名/cloud-sync
# 选择端口时避开已被占用的端口（如本服务器 8080 已被其它服务占用）。
# 自动更新包也可以由同一站点直接提供：
#   location ^~ /updates/ {
#       alias /opt/masterterm-updates/;
#       add_header Cache-Control "no-cache";
#   }

# 3. 方式 C：certbot 证书 + 服务直听 443（不经过 Nginx 时使用）
certbot certonly --standalone -d sync.example.com
python3 /opt/masterterm-cloud/cloud-sync.py --port 443 \
    --cert /etc/letsencrypt/live/sync.example.com/fullchain.pem \
    --key  /etc/letsencrypt/live/sync.example.com/privkey.pem \
    --prefix /cloud-sync

# 4. systemd 服务（方式 B 时监听 8081 即可）
cat > /etc/systemd/system/masterterm-cloud.service <<'EOF'
[Unit]
Description=MasterTerm cloud sync
After=network.target

[Service]
WorkingDirectory=/opt/masterterm-cloud
ExecStart=/usr/bin/python3 /opt/masterterm-cloud/cloud-sync.py --port 8081 --prefix /cloud-sync
Restart=on-failure
RestartSec=3

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload && systemctl enable --now masterterm-cloud
```

防火墙放行相应端口（反代方式只需放行 80/443）。

## 客户端使用

1. 设置 → 云同步：填写服务器地址（默认 `https://vm.dapang.wang/cloud-sync`）、用户名、密码。
2. 「注册账号」创建新账号（用户名已存在会提示）；之后用「登录」。
3. 「上传全部到云端」：把连接列表（含密码/私钥口令/跳板机密码）、本地命令历史、
   软件设置整体推送到云端（整体覆盖）。
4. 「从云端下载」：把云端数据合并到本机——连接按「类型+地址」更新或新增（不删除
   本机任何配置），密码写入 Windows 凭据管理器；历史命令去重合并；软件设置覆盖本机。

## 安全说明

- 同步数据中的密码与**私钥文件**以明文保存在云端 SQLite（用户已确认接受此方案，
  服务器视为可信）。私钥下载到新机器后写入本机数据目录 `keys/`。
- 传输全程 HTTPS（自签名证书不会被信任，请使用受信任证书或 Caddy 反代）。
- token 保存在客户端 localStorage；退出登录即清除。
- 建议定期备份 `cloud-sync.db`，并仅在可信服务器上部署。
