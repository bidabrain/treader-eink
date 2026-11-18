"""配置与路径：所有状态都放在 /data（宿主机的 ./data），方便备份和迁移。

Settings and paths: all state lives in /data (./data on the host) so backup and migration are a folder copy.
"""
from __future__ import annotations

import json
import os
import secrets
import threading
from pathlib import Path
from typing import Any

DATA = Path(os.environ.get("DATA_DIR", "/data"))
NOTES_DIR = DATA / "posts"           # 每条微博的图片目录 / Image folder per post
DB_PATH = DATA / "weibo.db"
CONFIG_PATH = DATA / "config.json"
AUTH_PATH = DATA / "auth.json"
PORT = int(os.environ.get("PORT", "8092"))
VERSION = "0.1.3"

# m.weibo.cn 的热门频道（/api/config/list）。/ m.weibo.cn hot channels (from /api/config/list).
HOT_CHANNELS = {
    "102803_ctg1_8999_-_ctg1_8999_home": "榜单", "102803": "热门", "102803_ctg1_4188_-_ctg1_4188": "社会",
    "102803_ctg1_2088_-_ctg1_2088": "科技", "102803_ctg1_4288_-_ctg1_4288": "明星",
    "102803_ctg1_3288_-_ctg1_3288": "电影", "102803_ctg1_5288_-_ctg1_5288": "音乐",
    "102803_ctg1_1988_-_ctg1_1988": "情感", "102803_ctg1_4488_-_ctg1_4488": "时尚",
    "102803_ctg1_1588_-_ctg1_1588": "美妆",
}

DEFAULTS: dict[str, Any] = {
    # 抓取（默认值和 weiboX 一样）。/ Fetching (same defaults as weiboX).
    "interval_min": 30,          # 每轮间隔（分钟）/ Minutes between rounds
    "delay_min": 30,             # 两个用户之间随机等待（秒）/ Random wait between users (s)
    "delay_max": 60,
    "posts_per_user": 20,        # 每人每轮读几条 / Posts read per user per round
    "min_check_min": 5,          # 这么多分钟内抓过的人跳过 / Skip users fetched within this many minutes
    "hot_enabled": True,
    "hot_count": 30,
    # 热门读哪个频道。访客身份下“热门”（102803）几乎全是视频，“榜单”大多是图文。
    # Hot channel. For visitors plain "热门" (102803) is nearly all video; "榜单" is mostly image posts.
    "hot_channel": "102803_ctg1_8999_-_ctg1_8999_home",
    # 长微博补全文每天最多几次，请求之间至少隔几秒。/ Daily cap and minimum gap for long-text fetches.
    "long_daily_cap": 150,
    "long_gap_s": 3,
    "max_images": 18,            # 每条最多几张图 / Most images per post
    "keep_days": 30,
    "max_mb": 2000,
    # 可选的微博 Cookie：不填就用浏览器生成的访客身份。/ Optional Weibo cookie; empty = browser visitor identity.
    "cookie": "",
    # WebDAV 备份（和 weiboX 同一个文件 weibox_backup.json）。/ WebDAV backup (the weiboX weibox_backup.json file).
    "webdav_url": "",
    "webdav_user": "",
    "webdav_pass": "",
    "device_token": "",
    "tunnel_mode": "off",
    "tunnel_token": "",
}

_lock = threading.Lock()


def ensure_dirs() -> None:
    DATA.mkdir(parents=True, exist_ok=True)
    NOTES_DIR.mkdir(parents=True, exist_ok=True)


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
