"""配置与路径：所有状态都放在 /data（宿主机的 ./data），包括服务器上那个浏览器的登录资料。

Settings and paths: all state lives in /data (./data on the host), including the server browser's signed-in profile.
"""
from __future__ import annotations

import json
import os
import secrets
import threading
from pathlib import Path
from typing import Any

DATA = Path(os.environ.get("DATA_DIR", "/data"))
POSTS_DIR = DATA / "posts"           # 每条推文的图片目录 / Image folder per post
PROFILE_DIR = DATA / "browser"       # 浏览器资料（登录状态）/ Browser profile (sign-in state)
COOKIES_PATH = DATA / "x_cookies.json"   # 登录 cookie 的备份 / Backup of the sign-in cookies
DB_PATH = DATA / "x.db"
CONFIG_PATH = DATA / "config.json"
AUTH_PATH = DATA / "auth.json"
PORT = int(os.environ.get("PORT", "8093"))
DISPLAY = os.environ.get("DISPLAY", ":99")
VERSION = "0.1.4"

FEEDS = {"foryou": "为你推荐", "following": "正在关注"}
# 自动拉取的最小间隔不能低于这么多分钟。/ Automatic fetching never runs closer together than this.
FLOOR_MIN = 30

DEFAULTS: dict[str, Any] = {
    # 每条流各自的随机间隔（分钟）：下一次在 [最小, 最大] 之间随机取。/ Per-feed random gap in minutes.
    "foryou_enabled": True,
    "foryou_min": 120,
    "foryou_max": 240,
    # 正在关注总开关：关掉后手动、自动都不拉，设备和 App 上这个标签变灰。/ Following master switch: when off nothing
    # fetches it, manual or automatic, and the device and app grey out its tab.
    "following_on": True,
    "following_enabled": True,
    "following_min": 60,
    "following_max": 180,
    # 睡眠时段：这段时间里自动拉取全部暂停，手动不受影响。/ Sleep window: automatic fetching pauses; manual is free.
    "sleep_enabled": True,
    "sleep_start": "01:00",
    "sleep_end": "08:00",
    # 每次拉取往下滚几次（随机取这两者之间）。/ Scrolls per fetch (random between these).
    "scroll_min": 2,
    "scroll_max": 4,
    # 取一条推文的回复时往下滚几次（随机取这两者之间），最多存几条。/ Scrolls when fetching a post's replies (random
    # between these) and how many replies are kept.
    "reply_scroll_min": 3,
    "reply_scroll_max": 5,
    "reply_max": 30,
    # 带视频的推文：cover = 留封面，skip = 不要。/ Posts with video: keep the cover, or skip them.
    "video": "cover",
    "max_images": 8,             # 每条最多几张图（含引用推文）/ Most images per post, quoted post included
    "keep_days": 14,
    "foryou_keep": 400,          # 为你推荐只留最近这么多条 / Most recent For You posts kept
    "max_mb": 2000,
    # 浏览器和图片下载走的代理，如 http://host:7890 或 socks5://host:1080。/ Proxy for the browser and images.
    "proxy": "",
    "device_token": "",
    "tunnel_mode": "off",
    "tunnel_token": "",
}

_lock = threading.Lock()


def ensure_dirs() -> None:
    DATA.mkdir(parents=True, exist_ok=True)
    POSTS_DIR.mkdir(parents=True, exist_ok=True)
    PROFILE_DIR.mkdir(parents=True, exist_ok=True)


def load() -> dict[str, Any]:
    with _lock:
        cfg = dict(DEFAULTS)
        if CONFIG_PATH.exists():
            try:
                cfg.update(json.loads(CONFIG_PATH.read_text(encoding="utf-8")))
            except (OSError, ValueError):
                pass
        if not cfg.get("device_token"):
            cfg["device_token"] = secrets.token_urlsafe(24)
            _write(cfg)
        return cfg


def save(update: dict[str, Any]) -> dict[str, Any]:
    cfg = load()
    with _lock:
        for key, value in update.items():
            if key in DEFAULTS:
                cfg[key] = value
        _write(cfg)
    return cfg


def _write(cfg: dict[str, Any]) -> None:
    tmp = CONFIG_PATH.with_suffix(".tmp")
    tmp.write_text(json.dumps(cfg, ensure_ascii=False, indent=2), encoding="utf-8")
    os.replace(tmp, CONFIG_PATH)
