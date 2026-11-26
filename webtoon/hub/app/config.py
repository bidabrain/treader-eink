"""配置与路径：所有状态都放在 /data（宿主机的 ./data），方便备份和迁移。服务器不保存任何人的订阅。

Settings and paths: all state lives in /data (./data on the host). The server keeps nobody's subscriptions.
"""
from __future__ import annotations

import json
import os
import secrets
import threading
from pathlib import Path
from typing import Any

DATA = Path(os.environ.get("DATA_DIR", "/data"))
CACHE_DIR = DATA / "cache"
CONFIG_PATH = DATA / "config.json"
AUTH_PATH = DATA / "auth.json"
PORT = int(os.environ.get("PORT", "8095"))
VERSION = "0.3.4"
BASE = os.environ.get("WEBTOON_BASE", "https://www.webtoons.com").rstrip("/")
MOBILE = os.environ.get("WEBTOON_MOBILE", "https://m.webtoons.com").rstrip("/")

DEFAULTS: dict[str, Any] = {
    # 设备访问令牌。/ Device access token.
    "device_token": "",
    # 外网访问：off / quick / token。/ Public access: off / quick / token.
    "tunnel_mode": "off",
    "tunnel_token": "",
    # 页面尺寸：缩到这个宽度，按这个高度切页（小纸 Pico 漫画区域）。/ Page size: scale to this width and cut at this
    # height (the Read Pico comic area).
    "page_w": 648,
    "page_h": 1000,
    # 每页 JPEG 质量。/ JPEG quality per page.
    "quality": 85,
    # 两次下载原图之间随机等待的秒数。/ Random wait between source image downloads, in seconds.
    "delay_min": 0.4,
    "delay_max": 1.2,
    # 列表和详情在服务器上缓存几分钟。/ Minutes lists and details stay cached on the server.
    "list_cache_min": 30,
    # 转好的 CBZ 保留天数和总大小上限（MB）。/ Converted CBZ retention in days and size cap in MB.
    "keep_days": 14,
    "max_mb": 3000,
    # Mihon 插件来源：设备上隐藏的来源（用户自己勾的）、来源顺序、同一话多个版本只留一个、跨页怎么放。服务器不按内容
    # 分级替用户过滤来源，成人来源也照样发给设备，由用户在客户端自己选。
    # Mihon extension sources: sources hidden on devices (the user's own choice), source order, keep one version per
    # chapter number, and how spreads are laid out. The server never filters sources by content rating for the user;
    # adult sources reach devices too and the user picks on the client.
    "mihon_hidden": [],
    "mihon_order": [],
    "mihon_dedupe": True,
    "mihon_spread": "split_rtl",
    # E-Hentai 通道（ehentai.py）：开关、站点（e / ex）、直连 IP、登录 cookie、默认搜索过滤（EH 搜索语法）、排除的分类
    # （f_cats 位）、优先日文标题、一话几张、跨页怎么放。
    # E-Hentai channel (ehentai.py): switch, site (e / ex), direct IP, sign-in cookies, default search filter (EH search
    # syntax), excluded categories (f_cats bits), prefer the Japanese title, images per episode, and spread layout.
    "eh_on": False,
    "eh_site": "e",
    "eh_direct": False,
    "eh_member_id": "",
    "eh_pass_hash": "",
    "eh_igneous": "",
    "eh_filter": "",
    "eh_cats": 0,
    "eh_jpn_title": True,
    "eh_chunk": 80,
    "eh_spread": "split_rtl",
}

_lock = threading.Lock()


def ensure_dirs() -> None:
    DATA.mkdir(parents=True, exist_ok=True)
    CACHE_DIR.mkdir(parents=True, exist_ok=True)


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
    DATA.mkdir(parents=True, exist_ok=True)
    tmp = CONFIG_PATH.with_suffix(".tmp")
    tmp.write_text(json.dumps(cfg, ensure_ascii=False, indent=2), encoding="utf-8")
    os.replace(tmp, CONFIG_PATH)
