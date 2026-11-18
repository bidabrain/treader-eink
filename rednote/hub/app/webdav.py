"""WebDAV 备份 / 恢复关注的博主：固定文件名 rednote_backup.json，里面是博主编号、昵称和 xsec_token（添加博主必须
带它）。小红书的登录状态在 xiaohongshu-mcp 容器里，不备份；换机器后重新扫码。

WebDAV backup / restore of followed creators: a fixed file rednote_backup.json holding each creator's id, nickname and
xsec_token (needed to add a creator). The Xiaohongshu sign-in lives in the xiaohongshu-mcp container and is not backed
up; scan the QR code again on a new machine.
"""
from __future__ import annotations

import json
from datetime import datetime
from typing import List

import httpx

BACKUP_FILENAME = "rednote_backup.json"


def _url(base_url: str) -> str:
    return f"{base_url.rstrip('/')}/{BACKUP_FILENAME}"


def build(creators: List[dict]) -> str:
    rows = [{"user_id": str(c.get("user_id", "")), "nickname": c.get("nickname") or "",
             "xsec_token": c.get("xsec_token") or ""} for c in creators if c.get("user_id")]
    payload = {"app": "rednote", "version": 1, "backup_time": datetime.now().strftime("%Y-%m-%dT%H:%M:%S"),
               "creators": rows}
    return json.dumps(payload, ensure_ascii=False, indent=2)


def parse(text: str) -> List[dict]:
    root = json.loads(text)
    if root.get("app") != "rednote":
        raise ValueError("这不是 rednote 的备份文件")
    out = []
    for c in root.get("creators", []):
        uid = str(c.get("user_id", "")).strip()
        if uid:
            out.append({"user_id": uid, "nickname": str(c.get("nickname") or ""),
                        "xsec_token": str(c.get("xsec_token") or "")})
    return out


def backup(base_url: str, username: str, password: str, creators: List[dict]) -> None:
    with httpx.Client(timeout=30.0) as c:
        resp = c.put(_url(base_url), content=build(creators).encode("utf-8"),
                     headers={"Content-Type": "application/json; charset=utf-8"}, auth=(username, password))
    if resp.status_code not in (200, 201, 204):
        raise RuntimeError(f"WebDAV 上传失败（HTTP {resp.status_code}）")


def restore(base_url: str, username: str, password: str) -> List[dict]:
    with httpx.Client(timeout=30.0, follow_redirects=True) as c:
        resp = c.get(_url(base_url), auth=(username, password))
    if resp.status_code == 404:
        raise RuntimeError("WebDAV 上还没有备份（rednote_backup.json）")
    if resp.status_code != 200:
        raise RuntimeError(f"WebDAV 下载失败（HTTP {resp.status_code}）")
    try:
        return parse(resp.text)
    except ValueError as e:
        raise RuntimeError(str(e) or "备份文件看不懂")
