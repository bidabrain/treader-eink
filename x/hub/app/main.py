"""x hub：网页面板（X 登录、调度设置、内容、日志）、noVNC 登录转发、设备接口、隧道。

x hub: web panel (X sign-in, schedule settings, content, logs), noVNC sign-in relay, device API and tunnel.

面板只在局域网用：从 Cloudflare 隧道进来的请求（带 CF-Connecting-IP）只能访问 /api/device/。VNC 能直接操作已登录
X 的浏览器，所以 noVNC 页面和它的 WebSocket 都要面板登录，且同样不对隧道开放。
The panel is LAN only: requests arriving through Cloudflare (CF-Connecting-IP) may only reach /api/device/. VNC drives
a browser signed in to X, so the noVNC page and its WebSocket need a panel session and are closed to the tunnel too.
"""
from __future__ import annotations

import asyncio
import json
import random
import re
import secrets
import time
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Any

from fastapi import FastAPI, HTTPException, Request, Response, WebSocket
from fastapi.responses import FileResponse, JSONResponse, RedirectResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from . import auth, config, fetcher, images, parser, store, tunnel
from .browser import VNC_PORT, AccountLocked, Busy, LoginRequired, RateLimited, browser

WEB = Path(__file__).parent / "web"
NOVNC = Path("/usr/share/novnc")
ID_RE = re.compile(r"^\d{5,25}$")
HM_RE = re.compile(r"^([01]?\d|2[0-3]):[0-5]\d$")


@asynccontextmanager
async def lifespan(app: FastAPI):
    config.ensure_dirs()
    config.load()
    store.db()
    task = asyncio.create_task(fetcher.loop())
    await tunnel.apply()
    yield
    task.cancel()
    await tunnel.stop()
    await browser.shutdown()


app = FastAPI(title="x hub", version=config.VERSION, lifespan=lifespan, docs_url=None, redoc_url=None)


@app.middleware("http")
async def guard(request: Request, call_next):
    path = request.url.path
    if request.headers.get("cf-connecting-ip") and not path.startswith("/api/device/"):
        return JSONResponse({"error": "面板不对外网开放"}, status_code=403)
    signed = auth.session_ok(request.cookies.get(auth.SESSION_COOKIE))
    if path.startswith("/novnc") and not signed:
        return RedirectResponse("/")
    if path.startswith("/api/panel/") and path not in ("/api/panel/state", "/api/panel/setup", "/api/panel/login"):
        if not signed:
            return JSONResponse({"error": "请先登录面板"}, status_code=401)
    response = await call_next(request)
    if path == "/" or path.startswith("/static/"):
        response.headers["Cache-Control"] = "no-cache"
    return response


# ---- 面板：登录 / Panel: sign-in ----

class Password(BaseModel):
    password: str


def _login(resp: Response) -> None:
    resp.set_cookie(auth.SESSION_COOKIE, auth.new_session(), httponly=True, samesite="strict", max_age=auth.SESSION_TTL)


@app.get("/api/panel/state")
async def panel_state(request: Request):
    return {"setup": not auth.has_password(),
            "signed_in": auth.session_ok(request.cookies.get(auth.SESSION_COOKIE)), "version": config.VERSION}


@app.post("/api/panel/setup")
async def panel_setup(body: Password, resp: Response):
    if auth.has_password():
        raise HTTPException(409, "已经设置过密码")
    if len(body.password) < 8:
        raise HTTPException(400, "密码至少 8 位")
    auth.set_password(body.password)
    _login(resp)
    return {"ok": True}


@app.post("/api/panel/login")
async def panel_login(body: Password, resp: Response):
    ok, why = auth.check_password(body.password)
    if not ok:
        raise HTTPException(403, why)
    _login(resp)
    return {"ok": True}


@app.post("/api/panel/logout")
async def panel_logout(request: Request, resp: Response):
    auth.drop_session(request.cookies.get(auth.SESSION_COOKIE))
    resp.delete_cookie(auth.SESSION_COOKIE)
    return {"ok": True}


@app.post("/api/panel/password")
async def panel_password(body: Password):
    if len(body.password) < 8:
        raise HTTPException(400, "密码至少 8 位")
    auth.set_password(body.password)
    return {"ok": True}


# ---- 面板：状态和拉取 / Panel: status and fetching ----

def _status() -> dict[str, Any]:
    s, cfg = fetcher.state, config.load()
    feeds = {}
    for k, fs in s.feeds.items():
        feeds[k] = {"name": config.FEEDS[k], "on": fetcher.feed_on(cfg, k), "enabled": bool(cfg[f"{k}_enabled"]),
                    "next_at": fs.next_at,
                    "last_at": fs.last_at, "last_new": fs.last_new, "last_total": fs.last_total,
                    "last_ads": fs.last_ads, "last_error": fs.last_error, "count": store.count(k)}
    return {"version": config.VERSION, "account": s.account, "paused": s.paused, "running": s.running,
            "sleeping": fetcher.sleeping(cfg), "feeds": feeds, "login": browser.login,
            "cache_mb": round(images.total_mb(), 1), "posts": store.count(), "tunnel": tunnel.state}


@app.get("/api/panel/status")
async def panel_status():
    return _status()


@app.post("/api/panel/fetch/{feed}")
async def fetch_now(feed: str):
    if feed not in config.FEEDS:
        raise HTTPException(400, "没有这条流")
    ok, msg = fetcher.request(feed)
    return {"ok": ok, "message": msg}


@app.post("/api/panel/resume")
async def fetch_resume():
    fetcher.resume()
    return {"ok": True}


@app.get("/api/panel/logs")
async def panel_logs():
    return list(fetcher.log)


# ---- 面板：X 登录 / Panel: X sign-in ----

@app.post("/api/panel/x/login")
async def x_login():
    if fetcher.state.running:
        fetcher.note("打开登录窗口：等当前拉取结束")
    browser.start_login(fetcher.login_done)
    fetcher.note("打开登录窗口")
    return {"ok": True}


@app.post("/api/panel/x/login/stop")
async def x_login_stop():
    await browser.stop_login()
    return {"ok": True}


class CookieText(BaseModel):
    text: str


def _cookies(text: str) -> list[dict[str, Any]]:
    """认 Cookie-Editor 之类导出的 JSON、cookies.txt，或 "auth_token=…; ct0=…" 这样的一行。/ Accepts JSON exports
    (e.g. Cookie-Editor), cookies.txt, or a single "auth_token=…; ct0=…" line."""
    text = text.strip()
    out: list[dict[str, Any]] = []

    def add(name: str, value: str, domain: str = ".x.com", path: str = "/", expires: float = -1,
            http_only: bool = False, secure: bool = True, same_site: str = "Lax") -> None:
        if not name or "twitter.com" in domain:
            return
        c = {"name": name, "value": value, "domain": domain if "x.com" in domain else ".x.com", "path": path or "/",
             "httpOnly": http_only, "secure": secure, "sameSite": same_site}
        if expires and expires > 0:
            c["expires"] = float(expires)
        out.append(c)

    if text.startswith("["):
        try:
            rows = json.loads(text)
        except ValueError:
            raise HTTPException(400, "JSON 读不了")
        for r in rows:
            ss = str(r.get("sameSite") or "lax").lower()
            add(r.get("name", ""), str(r.get("value", "")), r.get("domain", ".x.com"), r.get("path", "/"),
                float(r.get("expirationDate") or r.get("expires") or -1), bool(r.get("httpOnly")),
                bool(r.get("secure", True)), {"strict": "Strict", "no_restriction": "None", "none": "None"}.get(ss, "Lax"))
    elif "\t" in text:
        for line in text.splitlines():
            parts = line.split("\t")
            if line.startswith("#") and not line.startswith("#HttpOnly_") or len(parts) < 7:
                continue
            domain = parts[0].removeprefix("#HttpOnly_")
            add(parts[5], parts[6].strip(), domain, parts[2], float(parts[4] or -1), line.startswith("#HttpOnly_"),
                parts[3].upper() == "TRUE")
    else:
        for pair in text.split(";"):
            if "=" in pair:
                k, v = pair.split("=", 1)
                add(k.strip(), v.strip(), http_only=k.strip() == "auth_token")
    if not any(c["name"] == "auth_token" for c in out):
        raise HTTPException(400, "里面没有 auth_token")
    return out


@app.post("/api/panel/x/cookies")
async def x_cookies(body: CookieText):
    ok = await browser.import_cookies(_cookies(body.text))
    fetcher.login_done(ok)
    fetcher.note("导入了 cookie")
    return {"ok": ok}


@app.post("/api/panel/x/signout")
async def x_signout():
    await browser.sign_out()
    fetcher.state.account = "need_login"
    fetcher.note("清除了服务器浏览器里的 X 登录")
    return {"ok": True}


@app.websocket("/api/panel/vnc/ws")
async def vnc_ws(ws: WebSocket):
    """noVNC ↔ x11vnc 的转发；只在登录窗口打开时可用。/ Relay between noVNC and x11vnc; only while signing in."""
    if ws.headers.get("cf-connecting-ip") or not auth.session_ok(ws.cookies.get(auth.SESSION_COOKIE)):
        await ws.close(code=4401)
        return
    if not browser.vnc_running():
        await ws.close(code=4404)
        return
    try:
        reader, writer = await asyncio.open_connection("127.0.0.1", VNC_PORT)
    except OSError:
        await ws.close(code=4503)
        return
    sub = "binary" if "binary" in (ws.scope.get("subprotocols") or []) else None
    await ws.accept(subprotocol=sub)

    async def up() -> None:
        while True:
            msg = await ws.receive()
            if msg["type"] == "websocket.disconnect":
                return
            data = msg.get("bytes") or (msg.get("text") or "").encode()
            writer.write(data)
            await writer.drain()

    async def down() -> None:
        while True:
            data = await reader.read(65536)
            if not data:
                return
            await ws.send_bytes(data)

    tasks = [asyncio.create_task(up()), asyncio.create_task(down())]
    try:
        await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
    finally:
        for t in tasks:
            t.cancel()
        writer.close()
        try:
            await ws.close()
        except Exception:
            pass


# ---- 面板：内容和设置 / Panel: content and settings ----

@app.get("/api/panel/posts")
async def panel_posts(limit: int = 60):
    return store.latest(min(limit, 300))


@app.get("/api/panel/img/{pid}/{name}")
async def panel_img(pid: str, name: str):
    return _image(pid, name)


@app.get("/api/panel/settings")
async def settings_get():
    cfg = dict(config.load())
    cfg.pop("tunnel_token", None)
    cfg["floor_min"] = config.FLOOR_MIN
    return cfg


INTS = {"foryou_min": config.FLOOR_MIN, "foryou_max": config.FLOOR_MIN, "following_min": config.FLOOR_MIN,
        "following_max": config.FLOOR_MIN, "scroll_min": 1, "scroll_max": 1, "max_images": 1, "keep_days": 1,
        "foryou_keep": 50, "max_mb": 100, "reply_scroll_min": 0, "reply_scroll_max": 0, "reply_max": 5}
BOOLS = ("following_on", "foryou_enabled", "following_enabled", "sleep_enabled")


@app.post("/api/panel/settings")
async def settings_set(body: dict):
    update: dict[str, Any] = {}
    for key, low in INTS.items():
        if key in body:
            try:
                v = int(body[key])
            except (TypeError, ValueError):
                raise HTTPException(400, f"{key} 要填整数")
            if v < low:
                raise HTTPException(400, f"{key} 不能小于 {low}")
            update[key] = v
    cur = {**config.load(), **update}
    for feed in config.FEEDS:
        if cur[f"{feed}_max"] < cur[f"{feed}_min"]:
            raise HTTPException(400, f"{config.FEEDS[feed]}的最大间隔不能小于最小间隔")
    if cur["scroll_max"] < cur["scroll_min"] or cur["scroll_max"] > 10:
        raise HTTPException(400, "滚动次数：最大不能小于最小，也不超过 10")
    if cur["reply_scroll_max"] < cur["reply_scroll_min"] or cur["reply_scroll_max"] > 15:
        raise HTTPException(400, "取回复的滚动次数：最大不能小于最小，也不超过 15")
    if cur["reply_max"] > 200:
        raise HTTPException(400, "回复最多保留 200 条")
    for key in BOOLS:
        if key in body:
            update[key] = bool(body[key])
    for key in ("sleep_start", "sleep_end"):
        if key in body:
            if not HM_RE.match(str(body[key]).strip()):
                raise HTTPException(400, "睡眠时间要写成 01:00 这样")
            h, m = str(body[key]).strip().split(":")
            update[key] = f"{int(h):02d}:{m}"
    if "video" in body:
        if body["video"] not in ("cover", "skip"):
            raise HTTPException(400, "视频选项不对")
        update["video"] = body["video"]
    proxy_changed = False
    if "proxy" in body:
        proxy = str(body["proxy"]).strip()
        if proxy and not re.match(r"^(https?|socks5)://[^\s]+$", proxy):
            raise HTTPException(400, "代理写成 http://主机:端口 或 socks5://主机:端口")
        proxy_changed = proxy != (config.load().get("proxy") or "")
        update["proxy"] = proxy
    config.save(update)
    if proxy_changed:
        fetcher.note("代理改了，浏览器空闲时重启")
        asyncio.create_task(browser.restart())
    fetcher.reschedule()
    return await settings_get()


@app.post("/api/panel/device/rotate")
async def device_rotate():
    config.save({"device_token": secrets.token_urlsafe(24)})
    return {"ok": True}


class TunnelBody(BaseModel):
    mode: str
    token: str = ""


@app.post("/api/panel/tunnel")
async def tunnel_set(body: TunnelBody):
    if body.mode not in ("off", "quick", "token"):
        raise HTTPException(400, "方式不对")
    update = {"tunnel_mode": body.mode}
    if body.token:
        update["tunnel_token"] = body.token.strip()
    config.save(update)
    await tunnel.apply()
    return tunnel.state


@app.get("/api/panel/device")
async def device_info():
    return {"device_token": config.load()["device_token"], "tunnel": tunnel.state}


# ---- 设备接口（字段和微博 hub 一样）/ Device API (same fields as the Weibo hub) ----

def _device(request: Request) -> None:
    token = request.headers.get("x-hub-token") or request.query_params.get("token")
    if not auth.device_ok(token):
        raise HTTPException(401, "设备令牌不对")


def _brief(p: dict) -> dict:
    text = p["text"] or ""
    return {"id": p["id"], "user": p["user_name"], "screen": p["screen_name"], "time": p["created_ts"],
            "text": text[:140], "more": len(text) > 140 or bool(p["qt_text"]), "images": p["images"],
            "ratio": p["ratio"], "pics": p["pics"], "source": "", "likes": p["likes"], "comments": p["replies"],
            "reposts": p["retweets"], "views": p["views"], "rt_user": p["qt_user"], "rt_text": (p["qt_text"] or "")[:80],
            "rt_by": p["rt_by"], "reply_to": p["reply_to"], "video": bool(p["video"]), "fetched_at": p["fetched_at"]}


@app.get("/api/device/v1/feed")
async def device_feed(request: Request, seg: str = "foryou", since: float = 0, limit: int = 100, before: str = ""):
    """before 是上一批回复里的 next：从那里往后再要一批。next 为空表示没有更早的了。/ before is the previous
    reply's next, asking for the batch after it; an empty next means nothing older is left."""
    _device(request)
    if seg not in config.FEEDS:
        raise HTTPException(400, "seg 只能是 foryou 或 following")
    cfg = config.load()
    # 总开关关着：返回空列表并带 on=false，客户端据此把标签变灰。/ Master switch off: an empty list with on=false,
    # so clients grey out the tab.
    on = fetcher.feed_on(cfg, seg)
    limit = min(max(limit, 1), 300)
    try:
        rows = store.feed(seg, since, limit + 1, before) if on else []
    except ValueError:
        raise HTTPException(400, "before 不对")
    more = len(rows) > limit
    rows = rows[:limit]
    return {"now": time.time(), "on": on, "feeds_on": {k: fetcher.feed_on(cfg, k) for k in config.FEEDS},
            "posts": [_brief(p) for p in rows], "next": store.cursor(seg, rows[-1]) if more and rows else ""}


@app.get("/api/device/v1/status")
async def device_status(request: Request):
    """客户端开页时探测：哪些流开着、账号状态、上次和下次拉取。/ Probed by clients: which feeds are on, account
    state, last and next fetch."""
    _device(request)
    s, cfg = fetcher.state, config.load()
    return {"account": s.account, "paused": s.paused, "running": s.running,
            "feeds": {k: {"on": fetcher.feed_on(cfg, k), "last_at": fs.last_at, "next_at": fs.next_at}
                      for k, fs in s.feeds.items()}}


@app.post("/api/device/v1/refresh")
async def device_refresh(request: Request, seg: str = "foryou"):
    _device(request)
    if seg not in config.FEEDS:
        raise HTTPException(400, "seg 只能是 foryou 或 following")
    ok, msg = fetcher.request(seg)
    return {"ok": ok, "message": msg}


@app.get("/api/device/v1/post/{pid}")
async def device_post(request: Request, pid: str):
    _device(request)
    p = store.post(pid) if ID_RE.match(pid) else None
    if not p:
        raise HTTPException(404, "没有这条")
    starts = [int(x) for x in (p.get("starts") or "").split(",") if x.strip().isdigit()]
    return {**_brief(p), "text": p["text"], "rt_text": p["qt_text"], "rt_screen": p["qt_screen"], "starts": starts}


# 正在后台取回复的推文 → 任务；取失败的原因（下一次请求拿走一次）。/ Posts whose replies are being fetched -> task;
# failure reasons (handed out once on the next request).
_reply_tasks: dict[str, asyncio.Task] = {}
_reply_errors: dict[str, str] = {}


async def _fetch_replies(pid: str) -> None:
    cfg = config.load()
    lo, hi = int(cfg["reply_scroll_min"]), int(cfg["reply_scroll_max"])
    try:
        # 后台取，不让设备挂着等，所以可以多等一会儿浏览器。/ Fetched in the background with no device waiting on
        # the line, so it may wait longer for the browser.
        bodies = await browser.detail(pid, random.randint(min(lo, hi), max(lo, hi)), wait_lock=120.0)
    except Busy as e:
        _reply_errors[pid] = str(e)
        return
    except LoginRequired:
        fetcher.state.account = "need_login"
        fetcher.state.paused = "X 登录失效：到“登录”页重新登录"
        _reply_errors[pid] = "服务器上的 X 需要重新登录"
        return
    except AccountLocked:
        fetcher.state.account = "locked"
        fetcher.state.paused = "X 要求验证账号：到“登录”页打开浏览器按提示处理"
        _reply_errors[pid] = "服务器上的 X 需要验证账号"
        return
    except RateLimited:
        fetcher.state.paused = "X 限流（429），自动拉取已停：过一段时间确认正常后点“恢复自动拉取”"
        _reply_errors[pid] = "X 限流，稍后再试"
        return
    except Exception as e:
        _reply_errors[pid] = f"回复没取到：{type(e).__name__}"
        return
    _, rows = parser.replies(bodies, pid)
    items = [{"user": r["user_name"], "screen": r["screen_name"], "text": r["text"], "likes": r["likes"]}
             for r in rows[: int(cfg["reply_max"])]]
    store.save_replies(pid, items)
    fetcher.note(f"取回复 {pid}：{len(items)} 条")


def _reply_task(pid: str) -> asyncio.Task:
    task = _reply_tasks.get(pid)
    if task is None or task.done():
        _reply_errors.pop(pid, None)
        task = asyncio.create_task(_fetch_replies(pid))
        _reply_tasks[pid] = task
        task.add_done_callback(lambda t, p=pid: _reply_tasks.pop(p, None) if _reply_tasks.get(p) is t else None)
    return task


@app.get("/api/device/v1/post/{pid}/comments")
async def device_comments(request: Request, pid: str, wait: int = 1):
    """回复：第一次请求时服务器在后台用浏览器打开这条取一次，存下来以后不再取。wait=0（设备用）立刻返回
    {"pending": true}，过几秒再来问，设备的后台任务不会被挂住；默认（App 用）等结果，最多约 70 秒。
    Replies: on the first request the server opens the post in its browser in the background, once, and keeps them.
    wait=0 (the device) answers {"pending": true} at once so the device's worker is never held; ask again a few
    seconds later. The default (the app) waits for the result, about 70 seconds at most."""
    _device(request)
    if not ID_RE.match(pid) or not store.post(pid):
        raise HTTPException(404, "没有这条")
    cached = store.replies(pid)
    if cached is not None:
        return {"comments": cached}
    running = pid in _reply_tasks and not _reply_tasks[pid].done()
    if not running and pid in _reply_errors:
        raise HTTPException(503, _reply_errors.pop(pid))
    if not running and fetcher.state.account != "ok":
        raise HTTPException(503, "服务器上的 X 需要重新登录")
    task = _reply_task(pid)
    if not wait:
        return {"pending": True}
    try:
        await asyncio.wait_for(asyncio.shield(task), 70)
    except asyncio.TimeoutError:
        raise HTTPException(503, "服务器还在取回复，稍后再打开")
    cached = store.replies(pid)
    if cached is not None:
        return {"comments": cached}
    raise HTTPException(503, _reply_errors.pop(pid, "回复没取到"))


def _image(pid: str, name: str) -> FileResponse:
    if not ID_RE.match(pid) or not re.match(r"^(thumb|\d{1,3}|s\d{1,2})\.jpg$", name):
        raise HTTPException(400, "路径不对")
    path = images.post_dir(pid) / name
    if not path.exists():
        raise HTTPException(404, "没有这张图")
    return FileResponse(path, media_type="image/jpeg")


@app.get("/api/device/v1/img/{pid}/{name}")
async def device_img(request: Request, pid: str, name: str):
    _device(request)
    return _image(pid, name)


# 一次取多张列表小图（设备用，省掉逐张的往返）：请求体 {"paths": [...]}，每项是相对 /api/device/v1/ 的图片地址
# （img/<id>/thumb.jpg、img/<id>/s<k>.jpg / img/<id>/thumb.jpg, img/<id>/s<k>.jpg）；回一串 [4 字节大端长度][JPEG]，顺序和请求一样，没有的图长度为 0。不等还在做的图。
# Several list images at once (the device, saving a round trip per image): body {"paths": [...]} with image paths
# under /api/device/v1/ (img/<id>/thumb.jpg、img/<id>/s<k>.jpg / img/<id>/thumb.jpg, img/<id>/s<k>.jpg); the reply is a run of [4-byte big-endian length][JPEG] in request order, length 0
# for images not there. Images still being made are not waited for.
BATCH_MAX = 40
_BATCH_RULES = [
    (re.compile(r"^img/(\d{5,25})/((?:thumb|s\d{1,2})\.jpg)$"), lambda m: images.post_dir(m[1]) / m[2]),
]


def _batch_file(rel: str) -> Path | None:
    for rule, where in _BATCH_RULES:
        m = rule.match(rel)
        if m:
            return where(m)
    return None


def _batch_read(paths: list[str]) -> bytes:
    out = bytearray()
    for rel in paths:
        f = _batch_file(rel) if isinstance(rel, str) else None
        data = f.read_bytes() if f is not None and f.is_file() else b""
        out += len(data).to_bytes(4, "big") + data
    return bytes(out)


@app.post("/api/device/v1/imgs")
async def device_imgs(request: Request):
    _device(request)
    try:
        body = await request.json()
    except ValueError:
        body = None
    paths = body.get("paths") if isinstance(body, dict) else None
    if not isinstance(paths, list) or len(paths) > BATCH_MAX:
        raise HTTPException(400, f"paths 要是不超过 {BATCH_MAX} 项的列表")
    return Response(await asyncio.to_thread(_batch_read, paths), media_type="application/octet-stream")


@app.get("/healthz")
async def healthz():
    return {"ok": True}


app.mount("/static", StaticFiles(directory=WEB), name="static")
if NOVNC.is_dir():
    app.mount("/novnc", StaticFiles(directory=NOVNC, follow_symlink=True), name="novnc")


@app.get("/")
async def index():
    return FileResponse(WEB / "index.html")
