"""服务器上唯一的那个浏览器：Xvfb 虚拟屏幕上的有界面 Chromium（持久化资料，登录状态存在 /data/browser）。
登录时开 x11vnc，面板里的 noVNC 经 hub 转发操作它；拉取时打开 x.com 首页切到对应标签、往下滚，旁听页面自己发出的
GraphQL 请求，拿返回的 JSON。请求头、指纹、签名全由 X 自己的网页生成，hub 不直接调接口。

同一时间只做一件事（登录 / 拉取 / 看回复），用一把锁排队。

The one browser on the server: a headed Chromium on an Xvfb screen with a persistent profile (sign-in state lives in
/data/browser). For sign-in x11vnc starts and the panel's noVNC drives it through the hub; for fetching the browser
opens the x.com home page, switches to the right tab and scrolls while listening to the GraphQL requests the page sends
itself, keeping the JSON. Headers, fingerprint and signatures all come from X's own web app; the hub never calls the
API directly.

One thing at a time (sign-in, fetch or replies), queued behind one lock.
"""
from __future__ import annotations

import asyncio
import json
import os
import random
import shutil
import sqlite3
import time
from pathlib import Path
from typing import Any, Callable
from urllib.parse import urlsplit

from . import config

LAUNCH_ARGS = ["--no-sandbox", "--disable-dev-shm-usage", "--disable-blink-features=AutomationControlled",
               "--window-position=0,0", "--window-size=1280,860", "--no-first-run", "--no-default-browser-check",
               "--password-store=basic"]
SCREEN = "1280x860x24"
VNC_PORT = 5900
CHROME = "/usr/bin/google-chrome"
LOGIN_TIMEOUT_S = 20 * 60
OPS = {"foryou": "HomeTimeline", "following": "HomeLatestTimeline"}
# 取一条回复最多花多少秒（设备等 75 秒、App 等 60 秒）。/ Longest a replies fetch may take (the device waits 75 s,
# the app 60 s).
DETAIL_BUDGET_S = 45


class LoginRequired(Exception):
    """没登录或登录失效。/ Not signed in, or the session expired."""


class AccountLocked(Exception):
    """X 要求验证账号（/account/access）。/ X wants the account verified (/account/access)."""


class RateLimited(Exception):
    """X 返回 429。/ X answered 429."""


class Busy(Exception):
    """浏览器正被别的事占着。/ The browser is busy with something else."""


def _proxy(raw: str) -> dict[str, str] | None:
    raw = (raw or "").strip()
    if not raw:
        return None
    u = urlsplit(raw if "://" in raw else "http://" + raw)
    out = {"server": f"{u.scheme}://{u.hostname}:{u.port}" if u.port else f"{u.scheme}://{u.hostname}"}
    if u.username:
        out["username"] = u.username
        out["password"] = u.password or ""
    return out


def _clear_locks() -> None:
    """上次没正常退出时 Chrome 留下的锁会让它拒绝启动。/ A stale lock from an unclean exit blocks Chrome."""
    for name in ("SingletonLock", "SingletonCookie", "SingletonSocket"):
        p = config.PROFILE_DIR / name
        if p.is_symlink() or p.exists():
            p.unlink(missing_ok=True)


def _auth_on_disk() -> bool:
    """浏览器资料的 Cookies 库里有没有 X 的 auth_token（复制一份再读，不碰正在用的文件）。Chrome 不开任何调试接口，
    网页看到的就是普通浏览器；它大约每 30 秒把 cookie 写一次盘。
    Whether the profile's Cookies database holds X's auth_token (read from a copy, never the live file). Chrome runs
    with no debugging interface at all, so pages see an ordinary browser; it writes cookies to disk about every 30 s."""
    for src in (config.PROFILE_DIR / "Default" / "Cookies", config.PROFILE_DIR / "Default" / "Network" / "Cookies"):
        if not src.exists():
            continue
        tmp = config.DATA / "cookies-check.db"
        try:
            shutil.copyfile(src, tmp)
            con = sqlite3.connect(f"file:{tmp}?mode=ro", uri=True)
            try:
                n = con.execute("SELECT COUNT(*) FROM cookies WHERE name='auth_token' AND "
                                "(host_key LIKE '%x.com' OR host_key LIKE '%twitter.com')").fetchone()[0]
            finally:
                con.close()
            if n:
                return True
        except (OSError, sqlite3.Error):
            pass
        finally:
            tmp.unlink(missing_ok=True)
    return False


async def _pause(lo: float, hi: float) -> None:
    await asyncio.sleep(random.uniform(lo, hi))


class XBrowser:
    def __init__(self) -> None:
        self.lock = asyncio.Lock()
        self._xvfb: asyncio.subprocess.Process | None = None
        self._vnc: asyncio.subprocess.Process | None = None
        self._pw = None
        self._ctx = None
        self._proxy_used: str | None = None
        self._login_task: asyncio.Task | None = None
        self._login_stop = False
        # 登录会话的状态，面板显示。/ Sign-in session state for the panel.
        self.login = {"active": False, "message": ""}
        self.signed_in_cache = False

    # ---- 屏幕和浏览器 / Screen and browser ----

    async def _start_xvfb(self) -> None:
        if self._xvfb and self._xvfb.returncode is None:
            return
        n = config.DISPLAY.lstrip(":").split(".")[0]
        sock = Path(f"/tmp/.X11-unix/X{n}")
        lock = Path(f"/tmp/.X{n}-lock")
        if lock.exists() and not sock.exists():
            lock.unlink(missing_ok=True)
        if not sock.exists():
            self._xvfb = await asyncio.create_subprocess_exec(
                "Xvfb", config.DISPLAY, "-screen", "0", SCREEN, "-nolisten", "tcp",
                stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.DEVNULL)
            for _ in range(50):
                if sock.exists():
                    break
                await asyncio.sleep(0.1)

    async def _context(self):
        """浏览器没起或代理变了就（重新）启动。调用方须持有锁。/ (Re)start when not running or the proxy changed.
        Callers hold the lock."""
        proxy = config.load().get("proxy", "") or ""
        if self._ctx is not None and self._proxy_used == proxy:
            return self._ctx
        await self._close_context()
        await self._start_xvfb()
        from playwright.async_api import async_playwright
        if self._pw is None:
            self._pw = await async_playwright().start()
        _clear_locks()
        kw: dict[str, Any] = dict(
            user_data_dir=str(config.PROFILE_DIR), headless=False, args=LAUNCH_ARGS,
            ignore_default_args=["--enable-automation"], no_viewport=True,
            timezone_id=os.environ.get("TZ", "Asia/Shanghai"), env={**os.environ, "DISPLAY": config.DISPLAY})
        if Path(CHROME).exists():
            kw["executable_path"] = CHROME
        px = _proxy(proxy)
        if px:
            kw["proxy"] = px
        ctx = await self._pw.chromium.launch_persistent_context(**kw)
        self._ctx = ctx
        self._proxy_used = proxy

        # 浏览器意外退出时下次重新启动。/ If the browser dies, start it again next time.
        def closed(*_):
            if self._ctx is ctx:
                self._ctx = None
        ctx.on("close", closed)
        # 资料丢了但有备份：把登录 cookie 放回去。/ Profile lost but a backup exists: put the sign-in cookies back.
        if not await self._has_auth() and config.COOKIES_PATH.exists():
            try:
                await self._ctx.add_cookies(json.loads(config.COOKIES_PATH.read_text(encoding="utf-8")))
            except Exception:
                pass
        self.signed_in_cache = await self._has_auth()
        return self._ctx

    async def _close_context(self) -> None:
        if self._ctx is not None:
            try:
                await self._ctx.close()
            except Exception:
                pass
        self._ctx = None

    async def _page(self):
        ctx = await self._context()
        pages = [p for p in ctx.pages if not p.is_closed()]
        page = pages[0] if pages else await ctx.new_page()
        for extra in pages[1:]:
            try:
                await extra.close()
            except Exception:
                pass
        return page

    async def start(self) -> None:
        """开机时起屏幕和浏览器，读一次登录状态。/ Start the screen and browser at boot and read the sign-in state."""
        async with self.lock:
            await self._context()

    async def shutdown(self) -> None:
        await self.stop_login(wait=True)
        await self._stop_vnc()
        await self._close_context()
        if self._pw is not None:
            try:
                await self._pw.stop()
            except Exception:
                pass
            self._pw = None
        if self._xvfb and self._xvfb.returncode is None:
            self._xvfb.terminate()

    async def restart(self) -> None:
        """代理改了等情况：等手上的事做完再重启浏览器。/ Restart after e.g. a proxy change, once current work ends."""
        async with self.lock:
            await self._close_context()
            await self._context()

    # ---- 登录状态 / Sign-in state ----

    async def _has_auth(self) -> bool:
        if self._ctx is None:
            return False
        try:
            cookies = await self._ctx.cookies(["https://x.com"])
        except Exception:
            return False
        return any(c.get("name") == "auth_token" and c.get("value") for c in cookies)

    async def _backup_cookies(self) -> None:
        try:
            cookies = [c for c in await self._ctx.cookies() if c.get("domain", "").endswith(("x.com", "twitter.com"))]
            tmp = config.COOKIES_PATH.with_suffix(".tmp")
            tmp.write_text(json.dumps(cookies), encoding="utf-8")
            os.replace(tmp, config.COOKIES_PATH)
            os.chmod(config.COOKIES_PATH, 0o600)
        except Exception:
            pass

    async def import_cookies(self, cookies: list[dict[str, Any]]) -> bool:
        """备用入口：导入 cookie，返回导入后是否有登录态。/ Fallback: import cookies; returns whether signed in."""
        async with self.lock:
            ctx = await self._context()
            await ctx.add_cookies(cookies)
            self.signed_in_cache = await self._has_auth()
            if self.signed_in_cache:
                await self._backup_cookies()
            return self.signed_in_cache

    async def sign_out(self) -> None:
        """清掉这个浏览器里的 X 登录（不影响你别处的登录）。/ Drop the X sign-in in this browser only."""
        async with self.lock:
            ctx = await self._context()
            await ctx.clear_cookies()
            config.COOKIES_PATH.unlink(missing_ok=True)
            self.signed_in_cache = False

    # ---- VNC 登录 / VNC sign-in ----

    async def _start_vnc(self) -> None:
        if self._vnc and self._vnc.returncode is None:
            return
        self._vnc = await asyncio.create_subprocess_exec(
            "x11vnc", "-display", config.DISPLAY, "-rfbport", str(VNC_PORT), "-localhost", "-forever", "-shared",
            "-nopw", "-quiet", "-noxdamage", stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.DEVNULL)
        for _ in range(50):
            try:
                _, w = await asyncio.open_connection("127.0.0.1", VNC_PORT)
                w.close()
                return
            except OSError:
                await asyncio.sleep(0.1)

    async def _stop_vnc(self) -> None:
        if self._vnc and self._vnc.returncode is None:
            self._vnc.terminate()
            try:
                await asyncio.wait_for(self._vnc.wait(), 5)
            except asyncio.TimeoutError:
                self._vnc.kill()
        self._vnc = None

    def vnc_running(self) -> bool:
        return bool(self._vnc and self._vnc.returncode is None)

    def start_login(self, on_done: Callable[[bool], None]) -> None:
        if self._login_task and not self._login_task.done():
            return
        self._login_stop = False
        self.login.update(active=True, message="等浏览器空出来…")
        self._login_task = asyncio.create_task(self._login_session(on_done))

    async def stop_login(self, wait: bool = False) -> None:
        """让登录窗口收尾。面板点“登录好了”时不等（保存可能要一两分钟，面板自己刷新状态）；关机时等一会儿。
        Ask the sign-in window to finish. The panel's "done" does not wait (saving may take a minute or two and the
        panel polls the status); shutdown waits a little."""
        self._login_stop = True
        if wait and self._login_task and not self._login_task.done():
            try:
                await asyncio.wait_for(asyncio.shield(self._login_task), 20)
            except (asyncio.TimeoutError, Exception):
                self._login_task.cancel()

    async def _login_session(self, on_done: Callable[[bool], None]) -> None:
        """登录用一个完全普通的 Chrome 进程（同一份资料，不连 Playwright、不开调试端口），网页看不出是自动化浏览器。
        登录写进资料的 Cookies 库后自动收尾；点“登录好了”时也会先等它写盘，再让 Chrome 正常退出（慢机器多给时间），
        最后用 Playwright 打开同一份资料检查登录态。
        Sign-in uses a completely ordinary Chrome process (same profile, no Playwright, no debugging port), so the
        site sees no automation. Once the sign-in reaches the profile's Cookies database it finishes by itself; "done"
        also waits for that write first, then lets Chrome exit cleanly (slow machines get extra time), and finally
        Playwright reopens the profile to check the sign-in."""
        ok = False
        chrome: asyncio.subprocess.Process | None = None
        try:
            async with self.lock:
                await self._close_context()
                await self._start_xvfb()
                _clear_locks()
                had_auth = _auth_on_disk()
                args = [CHROME, f"--user-data-dir={config.PROFILE_DIR}", "--no-first-run",
                        "--no-default-browser-check", "--password-store=basic", "--no-sandbox",
                        "--disable-dev-shm-usage", "--window-position=0,0", "--window-size=1280,860"]
                px = _proxy(config.load().get("proxy", ""))
                if px:
                    args.append(f"--proxy-server={px['server']}")
                args.append("https://x.com/home" if self.signed_in_cache else "https://x.com/i/flow/login")
                chrome = await asyncio.create_subprocess_exec(
                    *args, env={**os.environ, "DISPLAY": config.DISPLAY},
                    stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.DEVNULL)
                await self._start_vnc()
                self.login["message"] = "在下面的画面里登录 X（或按提示完成验证），完成后点“登录好了”"
                deadline = time.monotonic() + LOGIN_TIMEOUT_S
                tick = 0
                while not self._login_stop and chrome.returncode is None and time.monotonic() < deadline:
                    await asyncio.sleep(1)
                    tick += 1
                    # 本来没登录、现在登录已经写进硬盘：自动收尾。/ Not signed in before and the sign-in is now on disk:
                    # finish by itself.
                    if not had_auth and tick % 3 == 0 and _auth_on_disk():
                        self.login["message"] = "检测到登录，正在保存…"
                        await asyncio.sleep(3)
                        break
                await self._stop_vnc()
                if chrome.returncode is None:
                    # 等浏览器把登录写进硬盘（最多约 45 秒），再让它正常退出；慢机器多给时间，尽量不强杀。
                    # Wait for the browser to write the sign-in to disk (about 45 s at most), then let it exit
                    # cleanly; slow machines get extra time and a hard kill is the last resort.
                    if not had_auth and not _auth_on_disk():
                        self.login["message"] = "正在等浏览器把登录写进硬盘…（最多约 45 秒）"
                        until = time.monotonic() + 45
                        while time.monotonic() < until and chrome.returncode is None and not _auth_on_disk():
                            await asyncio.sleep(2)
                    self.login["message"] = "正在保存登录…"
                    if chrome.returncode is None:
                        chrome.terminate()
                        try:
                            await asyncio.wait_for(chrome.wait(), 90)
                        except asyncio.TimeoutError:
                            chrome.kill()
                            await chrome.wait()
                await asyncio.sleep(1)
                await self._context()
                ok = await self._has_auth()
                self.signed_in_cache = ok
                if ok:
                    await self._backup_cookies()
                    self.login["message"] = "登录成功，已保存"
                else:
                    self.login["message"] = ("超过 20 分钟没完成，已关闭" if time.monotonic() >= deadline
                                             else "窗口已关闭，但没有检测到登录")
        except Exception as e:
            self.login["message"] = f"登录窗口出错：{type(e).__name__}: {e}"
        finally:
            await self._stop_vnc()
            if chrome and chrome.returncode is None:
                chrome.kill()
            self.login["active"] = False
            on_done(ok)

    # ---- 拉取 / Fetching ----

    def _check(self, page) -> None:
        path = urlsplit(page.url).path
        if path.startswith("/account/access"):
            raise AccountLocked("X 要求验证账号")
        if path.startswith(("/login", "/i/flow/login", "/logout", "/i/flow/signup")) or path in ("", "/"):
            raise LoginRequired("没有登录或登录已失效")

    async def _capture(self, url: str, op: str, work: Callable, first_wait: float = 30.0) -> list[dict]:
        """打开 url，收集名字是 op 的 GraphQL 返回，再跑 work(page, bodies)。/ Open url, collect GraphQL responses
        named op, then run work(page, bodies)."""
        if self.login["active"]:
            raise Busy("正在登录")
        page = await self._page()
        bodies: list[dict] = []
        limited: list[int] = []
        pending: list[asyncio.Task] = []

        async def grab(resp) -> None:
            try:
                if resp.status == 429:
                    limited.append(429)
                elif resp.ok:
                    bodies.append(await resp.json())
            except Exception:
                pass

        def on_response(resp) -> None:
            u = resp.url.split("?", 1)[0]
            if "/i/api/graphql/" in u and u.endswith("/" + op):
                pending.append(asyncio.create_task(grab(resp)))

        page.on("response", on_response)
        try:
            await page.goto(url, wait_until="domcontentloaded", timeout=60000)
            await _pause(1.0, 2.5)
            self._check(page)
            await work(page, bodies, limited)
            deadline = time.monotonic() + first_wait
            while not bodies and not limited and time.monotonic() < deadline:
                await asyncio.sleep(0.5)
            self._check(page)
        finally:
            page.remove_listener("response", on_response)
            if pending:
                await asyncio.gather(*pending, return_exceptions=True)
        self.signed_in_cache = await self._has_auth()
        if limited and not bodies:
            raise RateLimited("X 限流（429）")
        return bodies

    async def timeline(self, feed: str, scrolls: int) -> list[dict]:
        op = OPS[feed]

        async def work(page, bodies, limited) -> None:
            try:
                await page.wait_for_selector('[role="tab"]', timeout=30000)
            except Exception:
                self._check(page)
                raise
            tab = page.get_by_role("tab").nth(0 if feed == "foryou" else 1)
            if await tab.get_attribute("aria-selected") != "true":
                await _pause(1.5, 4.0)
                await tab.click()
            deadline = time.monotonic() + 30
            while not bodies and not limited and time.monotonic() < deadline:
                await asyncio.sleep(0.5)
            await page.mouse.move(random.randint(500, 800), random.randint(300, 600))
            for _ in range(scrolls):
                if limited:
                    break
                await _pause(3.0, 9.0)
                have = len(bodies)
                for _ in range(random.randint(3, 6)):
                    await page.mouse.wheel(0, random.randint(350, 750))
                    await _pause(0.15, 0.6)
                deadline = time.monotonic() + 15
                while len(bodies) == have and not limited and time.monotonic() < deadline:
                    await asyncio.sleep(0.5)
            await _pause(1.5, 4.0)

        async with self.lock:
            return await self._capture("https://x.com/home", op, work)

    async def detail(self, tid: str, scrolls: int, wait_lock: float = 15.0) -> list[dict]:
        started = time.monotonic()

        async def work(page, bodies, limited) -> None:
            deadline = time.monotonic() + 30
            while not bodies and not limited and time.monotonic() < deadline:
                await asyncio.sleep(0.5)
            await _pause(2.0, 4.0)
            await page.mouse.move(random.randint(500, 800), random.randint(300, 600))
            # 总共不超过 DETAIL_BUDGET_S 秒，免得设备和 App 等超时。/ Stay within DETAIL_BUDGET_S seconds so the device
            # and app do not time out.
            for _ in range(scrolls):
                if limited or time.monotonic() - started > DETAIL_BUDGET_S:
                    break
                have = len(bodies)
                for _ in range(random.randint(2, 4)):
                    await page.mouse.wheel(0, random.randint(350, 700))
                    await _pause(0.2, 0.6)
                deadline = time.monotonic() + 8
                while len(bodies) == have and not limited and time.monotonic() < deadline:
                    await asyncio.sleep(0.5)
                await _pause(1.0, 2.5)
            await _pause(1.0, 2.0)

        try:
            await asyncio.wait_for(self.lock.acquire(), wait_lock)
        except asyncio.TimeoutError:
            raise Busy("服务器正在拉取，稍后再试")
        try:
            return await self._capture(f"https://x.com/i/status/{tid}", "TweetDetail", work)
        finally:
            self.lock.release()

    async def download(self, url: str) -> bytes | None:
        """用浏览器同一出口（同一代理）下载图片。/ Download an image through the browser's own exit (same proxy)."""
        ctx = self._ctx
        if ctx is None:
            return None
        try:
            resp = await ctx.request.get(url, timeout=30000)
            if not resp.ok:
                return None
            return await resp.body()
        except Exception:
            return None


browser = XBrowser()
