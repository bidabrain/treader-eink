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
VERSION = "0.1.0"

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
    # 设备访问令牌。/ Device access token.
    "device_token": "",
    # 外网访问：off / quick / token。/ Public access: off / quick / token.
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
