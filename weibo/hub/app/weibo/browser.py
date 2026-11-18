"""共享的 Playwright Chromium：访客身份生成和验证码共用一个浏览器，用完不关，空闲时不占资源。
移植自 weiboX-server。

Shared Playwright Chromium for the visitor identity and captchas. Ported from weiboX-server.
"""
from __future__ import annotations

import asyncio

LAUNCH_ARGS = ["--no-sandbox", "--disable-dev-shm-usage", "--disable-gpu"]


class BrowserPool:
    def __init__(self):
        self._playwright = None
        self._browser = None
        self._lock = asyncio.Lock()

    async def browser(self):
        async with self._lock:
            if self._browser is None or not self._browser.is_connected():
                from playwright.async_api import async_playwright
                if self._playwright is None:
                    self._playwright = await async_playwright().start()
                self._browser = await self._playwright.chromium.launch(headless=True, args=LAUNCH_ARGS)
        return self._browser

    async def shutdown(self):
        try:
            if self._browser:
                await self._browser.close()
                self._browser = None
            if self._playwright:
                await self._playwright.stop()
                self._playwright = None
        except Exception:
            pass


browser_pool = BrowserPool()
