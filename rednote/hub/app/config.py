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
NOTES_DIR = DATA / "notes"
DB_PATH = DATA / "rednote.db"
CONFIG_PATH = DATA / "config.json"
AUTH_PATH = DATA / "auth.json"
MCP_URL = os.environ.get("MCP_URL", "http://mcp:18060").rstrip("/")
NOVNC_URL = os.environ.get("NOVNC_URL", "http://mcp:6080").rstrip("/")
PORT = int(os.environ.get("PORT", "8091"))
VERSION = "0.3.4"

DEFAULTS: dict[str, Any] = {
    # 每轮之间隔多少分钟。/ Minutes between rounds.
    "interval_min": 120,
    # 两次小红书请求之间随机等待的秒数。/ Random wait between Xiaohongshu requests, in seconds.
    "delay_min": 15,
    "delay_max": 30,
    # 每个博主每轮最多新拉几篇。/ Most new notes per creator per round.
    "per_creator": 5,
    # 每天最多请求小红书多少次（详情和主页都算）。/ Daily cap on Xiaohongshu requests (profiles and details).
    "daily_cap": 200,
    # 每篇最多处理几张图。/ Most images processed per note.
    "max_images": 18,
    # 缓存保留天数和总大小上限（MB）。/ Cache retention in days and size cap in MB.
    "keep_days": 30,
    "max_mb": 2000,
    # 推荐：设备点“刷新”时才由服务器代拉首页，点开哪篇才拉哪篇详情。视频一律跳过。
    # Recommendations: the server reads the home feed only when the device asks, and a note's detail only when opened.
    # Videos are always skipped.
    "rec_enabled": True,
    # 一次推荐列表最多几篇图文。/ Most image notes in one recommendation list.
    "rec_max": 12,
    # 这么多分钟内再刷新，直接返回上次的列表。/ Refreshing within this many minutes returns the previous list.
    "rec_cache_min": 10,
    # 推荐每天最多请求小红书几次（刷新和点开都算，同时也计入总上限）。
    # Daily cap on recommendation requests (refreshes and opens; they also count toward daily_cap).
    "rec_daily_cap": 60,
    # 两次推荐请求之间至少隔几秒（另加 0～3 秒随机）。/ Minimum seconds between recommendation requests (plus 0-3 s jitter).
    "rec_gap_s": 3,
    # 设备访问令牌。/ Device access token.
    "device_token": "",
    # 外网访问：off / quick / token。/ Public access: off / quick / token.
    "tunnel_mode": "off",
    "tunnel_token": "",
    # WebDAV 备份关注的博主（rednote_backup.json）。/ WebDAV backup of followed creators (rednote_backup.json).
    "webdav_url": "",
    "webdav_user": "",
    "webdav_pass": "",
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
        changed = False
        if not cfg.get("device_token"):
            cfg["device_token"] = secrets.token_urlsafe(24)
            changed = True
        # 旧默认的 8 秒间隔改成新默认 3 秒（只迁移一次，之后用户改成多少就是多少）。/ Move the old 8 s default gap to
        # the new 3 s default once; afterwards whatever the user sets stays.
        if not cfg.get("_gap_v2"):
            if cfg.get("rec_gap_s") == 8:
                cfg["rec_gap_s"] = 3
            cfg["_gap_v2"] = True
            changed = True
        if changed:
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
