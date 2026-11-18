"""配置与路径：hub 自己的状态放在 /data（宿主机的 ./data/hub）；番茄下载器在同一个 compose 里，只在内网。

Settings and paths: the hub keeps its state in /data (./data/hub on the host); the Tomato downloader runs in the same
compose project, on the internal network only.
"""
from __future__ import annotations

import json
import os
import secrets
import threading
from pathlib import Path
from typing import Any

DATA = Path(os.environ.get("DATA_DIR", "/data"))
CONFIG_PATH = DATA / "config.json"
AUTH_PATH = DATA / "auth.json"
PORT = int(os.environ.get("PORT", "18423"))
VERSION = "0.1.0"
# 下载器的地址（带它的子路径 /webui）。/ The downloader's address, including its /webui base path.
UPSTREAM = os.environ.get("TOMATO_UPSTREAM", "http://downloader:18423/webui").rstrip("/")
WEBUI_PREFIX = "/webui"

DEFAULTS: dict[str, Any] = {
    # 设备和安卓 App 填的密码（请求头 x-tomato-password）。/ Password for the device and Android app (x-tomato-password).
    "device_password": "",
    # 第一次连上下载器时把默认格式改成 TXT（设备上番茄书用 TXT）。/ On first contact, switch the downloader's default
    # format to TXT (Tomato books are TXT on the device).
    "seeded": False,
    # 外网访问：off / quick / token。/ Public access: off / quick / token.
    "tunnel_mode": "off",
    "tunnel_token": "",
}

_lock = threading.Lock()


def ensure_dirs() -> None:
    DATA.mkdir(parents=True, exist_ok=True)


def new_password() -> str:
    """8 位小写字母和数字，去掉容易看错的 0、o、1、l，方便在设备键盘上输入。/ Eight lowercase letters and digits
    without the look-alikes 0, o, 1 and l, easy to type on the device keyboard."""
    alphabet = "abcdefghijkmnpqrstuvwxyz23456789"
    return "".join(secrets.choice(alphabet) for _ in range(8))


def load() -> dict[str, Any]:
    with _lock:
        cfg = dict(DEFAULTS)
        if CONFIG_PATH.exists():
            try:
                cfg.update(json.loads(CONFIG_PATH.read_text(encoding="utf-8")))
            except (OSError, ValueError):
                pass
        if not cfg.get("device_password"):
            cfg["device_password"] = new_password()
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
