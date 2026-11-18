"""有效的微博身份：设置里填了 Cookie 就用它，否则用浏览器生成的访客身份（不登录）。移植自 weiboX-server。

The effective Weibo identity: the configured cookie if any, else a browser-made visitor identity (no login).
Ported from weiboX-server.
"""
from __future__ import annotations

from typing import Tuple

from .. import config
from .client import WeiboClient
from .visitor import visitor_session


async def effective_cookie(force_visitor: bool = False) -> Tuple[str, bool]:
    """返回 (cookie, 是否访客)。/ Returns (cookie, is_visitor)."""
    user_cookie = (config.load().get("cookie") or "").strip()
    if user_cookie and not force_visitor:
        return user_cookie, False
    vc = await visitor_session.get(force=force_visitor)
    return vc, True


async def new_client(force_visitor: bool = False) -> WeiboClient:
    cookie, _ = await effective_cookie(force_visitor=force_visitor)
    return WeiboClient(cookie)
