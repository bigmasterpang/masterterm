#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""MasterTerm GitHub Release 自动化发布脚本。

功能：
  1. 自动检测版本号（支持指定版本，默认从 CMakeLists.txt 提取）。
  2. 自动检查并构建安装包（Setup.exe）与绿色便携包（ZIP）。
  3. 提取最近版本更新日志（RELEASE_NOTES.md）。
  4. 检查/创建 GitHub Release（支持幂等更新发布说明）。
  5. 上传安装包与绿色压缩包至 GitHub Release 资源列表。

用法：
  python tools/publish-github-release.py [--version 0.1.127] [--token <TOKEN>]
  或设置环境变量 GITHUB_TOKEN=...
"""

import argparse
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path


def log(msg: str):
    print(f"[*] {msg}", flush=True)


def parse_version_from_cmake(root_dir: Path) -> str:
    cmake_path = root_dir / "CMakeLists.txt"
    if not cmake_path.exists():
        raise FileNotFoundError(f"CMakeLists.txt not found at {cmake_path}")
    text = cmake_path.read_text(encoding="utf-8")
    match = re.search(r"project\(MasterTerm\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)", text)
    if not match:
        raise ValueError("Could not parse MasterTerm VERSION from CMakeLists.txt")
    return match.group(1)


def api_request(url: str, token: str, method: str = "GET", data=None, content_type: str = "application/json"):
    headers = {
        "Authorization": f"Bearer {token}",
        "Accept": "application/vnd.github+json",
        "User-Agent": "MasterTerm-Release-Script",
        "X-GitHub-Api-Version": "2022-11-28",
    }
    if content_type:
        headers["Content-Type"] = content_type

    req = urllib.request.Request(url, headers=headers, method=method)
    if data is not None:
        if isinstance(data, dict):
            req.data = json.dumps(data).encode("utf-8")
        elif isinstance(data, (bytes, bytearray)):
            req.data = data
        else:
            req.data = str(data).encode("utf-8")

    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            resp_body = resp.read()
            if not resp_body:
                return None
            return json.loads(resp_body.decode("utf-8"))
    except urllib.error.HTTPError as e:
        error_body = e.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"HTTP {e.code} for {method} {url}: {error_body}") from e


def main():
    parser = argparse.ArgumentParser(description="MasterTerm GitHub Release 发布工具")
    parser.add_argument("--version", help="发布版本号（默认从 CMakeLists.txt 读取，如 0.1.127）")
    parser.add_argument("--token", help="GitHub Personal Access Token (也可通过环境变量 GITHUB_TOKEN 提供)")
    parser.add_argument("--token-path", help="保存 GitHub Token 的文件路径 (默认优先检查环境变量与本机常见路径)")
    parser.add_argument("--repo", default="bigmasterpang/masterterm", help="GitHub 仓库名")
    args = parser.parse_args()

    root_dir = Path(__file__).resolve().parent.parent
    version = args.version or parse_version_from_cmake(root_dir)
    version = version.lstrip("v")
    tag_name = f"v{version}"
    repo = args.repo

    log(f"准备发布 GitHub Release: {repo} -> {tag_name}")

    token = args.token or os.environ.get("GITHUB_TOKEN")
    if not token and args.token_path:
        tpath = Path(args.token_path)
        if tpath.exists():
            token = tpath.read_text(encoding="utf-8").strip()
    if not token:
        # 本机无感回退检查常用路径，避免命令行中硬编码暴露
        candidate_paths = [
            Path(r"C:\opencode\github_tokens"),
            Path.home() / ".github_token",
            Path.home() / ".config" / "github_token",
        ]
        for cp in candidate_paths:
            if cp.exists():
                try:
                    c_val = cp.read_text(encoding="utf-8").strip()
                    if c_val:
                        token = c_val
                        break
                except Exception:
                    pass

    if not token:
        raise ValueError("未找到 GitHub Token。请通过 --token、--token-path 或环境变量 GITHUB_TOKEN 提供。")

    # 1. 检查构建构件（ZIP 与 Setup.exe）
    dist_dir = root_dir / "build-release" / "dist"
    zip_path = dist_dir / f"MasterTerm-{version}-windows-x64.zip"
    setup_path = dist_dir / f"MasterTerm-{version}-Setup.exe"

    if not zip_path.exists():
        log(f"便携版 ZIP 不存在，正在打包: {zip_path.name}")
        cmd = ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
               "-File", str(root_dir / "tools" / "package-release.ps1"), "-Version", version]
        subprocess.run(cmd, check=True, cwd=str(root_dir))

    if not setup_path.exists():
        log(f"安装包 Setup.exe 不存在，正在构建: {setup_path.name}")
        cmd = ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
               "-File", str(root_dir / "tools" / "package-installer.ps1"), "-Version", version]
        subprocess.run(cmd, check=True, cwd=str(root_dir))

    if not zip_path.exists():
        raise FileNotFoundError(f"缺少 ZIP 文件: {zip_path}")
    if not setup_path.exists():
        raise FileNotFoundError(f"缺少 Setup.exe 文件: {setup_path}")

    zip_size_mb = zip_path.stat().st_size / (1024 * 1024)
    setup_size_mb = setup_path.stat().st_size / (1024 * 1024)
    log(f"构件已就绪: {zip_path.name} ({zip_size_mb:.2f} MB)")
    log(f"构件已就绪: {setup_path.name} ({setup_size_mb:.2f} MB)")

    # 2. 提取 Release Notes
    release_notes_path = dist_dir / f"MasterTerm-{version}-windows-x64" / "RELEASE_NOTES.md"
    if release_notes_path.exists():
        body_text = release_notes_path.read_text(encoding="utf-8").strip()
    else:
        body_text = f"MasterTerm {tag_name} 正式发布，包含绿色便携版 ZIP 与一键安装包 Setup.exe。"

    # 3. 检查 Release 是否已存在
    release_tag_url = f"https://api.github.com/repos/{repo}/releases/tags/{tag_name}"
    existing_release = None
    try:
        existing_release = api_request(release_tag_url, token, method="GET")
    except RuntimeError as e:
        if "HTTP 404" not in str(e):
            raise

    if existing_release and "id" in existing_release:
        release_id = existing_release["id"]
        log(f"Release {tag_name} 已存在 (ID: {release_id})，正在同步更新发布说明...")
        update_url = f"https://api.github.com/repos/{repo}/releases/{release_id}"
        release = api_request(update_url, token, method="PATCH", data={
            "name": f"MasterTerm {tag_name}",
            "body": body_text,
        })
    else:
        log(f"正在创建新的 GitHub Release: {tag_name}...")
        create_url = f"https://api.github.com/repos/{repo}/releases"
        release = api_request(create_url, token, method="POST", data={
            "tag_name": tag_name,
            "name": f"MasterTerm {tag_name}",
            "body": body_text,
            "draft": False,
            "prerelease": False,
        })

    release_id = release["id"]
    upload_url_template = release["upload_url"]
    base_upload_url = upload_url_template.split("{")[0]
    log(f"Release ID: {release_id}, Upload URL: {base_upload_url}")

    # 4. 上传资产文件
    current_assets = {a["name"]: a["id"] for a in release.get("assets", [])}
    assets_to_upload = [setup_path, zip_path]

    for asset_path in assets_to_upload:
        filename = asset_path.name
        file_bytes = asset_path.read_bytes()
        file_size_mb = len(file_bytes) / (1024 * 1024)

        if filename in current_assets:
            old_asset_id = current_assets[filename]
            log(f"正在删除已有资产: {filename} (ID: {old_asset_id})...")
            delete_url = f"https://api.github.com/repos/{repo}/releases/assets/{old_asset_id}"
            api_request(delete_url, token, method="DELETE")

        log(f"正在上传资产: {filename} ({file_size_mb:.2f} MB)...")
        upload_url = f"{base_upload_url}?name={urllib.parse.quote(filename)}"
        uploaded = api_request(upload_url, token, method="POST", data=file_bytes, content_type="application/octet-stream")
        log(f"上传成功: {filename} (Asset ID: {uploaded.get('id')})")

    release_html_url = release.get("html_url") or f"https://github.com/{repo}/releases/tag/{tag_name}"
    print(f"\n==================================================")
    print(f"GITHUB_RELEASE_OK: {release_html_url}")
    print(f"Assets: {setup_path.name}, {zip_path.name}")
    print(f"==================================================")


if __name__ == "__main__":
    main()
