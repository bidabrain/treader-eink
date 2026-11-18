"""WebDAV 备份 / 恢复，和 weiboX-server 用同一个文件：固定文件名 weibox_backup.json、version 2，里面是关注名单
（含“特别关注”）和 Cookie。两边可以互相恢复。格式照 weiboX-server 的 services/webdav.py。

WebDAV backup / restore sharing weiboX-server's file: weibox_backup.json, version 2, holding the follow list (with
"special" marks) and the cookie, so either side can restore the other's backup. Format follows weiboX-server's
services/webdav.py.
"""
from __future__ import annotations

import json
from datetime import datetime
from typing import List, Tuple

import httpx

BACKUP_FILENAME = "weibox_backup.json"


def _url(base_url: str) -> str:
    return f"{base_url.rstrip('/')}/{BACKUP_FILENAME}"


def _user(u: dict) -> dict:
    return {
        "id": str(u.get("id", "")),
        "screen_name": u.get("screen_name", "") or "",
        "description": u.get("description", "") or "",
        "avatar_url": u.get("avatar_url", "") or "",
        "cover_url": u.get("cover_url", "") or "",
        "followers_count": str(u.get("followers_count", "") or ""),
        "follow_count": int(u.get("follow_count", 0) or 0),
        "statuses_count": int(u.get("statuses_count", 0) or 0),
        "verified": bool(u.get("verified", False)),
        "verified_reason": u.get("verified_reason", "") or "",
        "special": bool(u.get("special", False)),
    }


def build(users: List[dict], cookie: str) -> str:
    arr = [_user(u) for u in users]
    payload = {
        "version": 2,
        "backup_time": datetime.now().strftime("%Y-%m-%dT%H:%M:%S"),
        "cookie": cookie,
        "users": arr,
        "special_followed": [u["id"] for u in arr if u["special"]],
    }
    return json.dumps(payload, ensure_ascii=False, indent=2)


def parse(text: str) -> Tuple[List[dict], str]:
    root = json.loads(text)
    special = {str(x) for x in root.get("special_followed", [])}
    users = []
    for u in root.get("users", []):
        row = _user(u)
        row["special"] = row["special"] or row["id"] in special
        if row["id"]:
            users.append(row)
    return users, root.get("cookie", "") or ""


def backup(base_url: str, username: str, password: str, users: List[dict], cookie: str) -> None:
    with httpx.Client(timeout=30.0) as c:
        resp = c.put(_url(base_url), content=build(users, cookie).encode("utf-8"),
                     headers={"Content-Type": "application/json; charset=utf-8"}, auth=(username, password))
    if resp.status_code not in (200, 201, 204):
        raise RuntimeError(f"WebDAV 上传失败（HTTP {resp.status_code}）")


def restore(base_url: str, username: str, password: str) -> Tuple[List[dict], str]:
    with httpx.Client(timeout=30.0, follow_redirects=True) as c:
        resp = c.get(_url(base_url), auth=(username, password))
    if resp.status_code != 200:
        raise RuntimeError(f"WebDAV 下载失败（HTTP {resp.status_code}）")
    return parse(resp.text)
