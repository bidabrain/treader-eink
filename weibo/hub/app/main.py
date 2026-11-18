"""weibo hub：网页面板（关注管理、验证码、设置、WebDAV）、设备接口、定时拉取、隧道。

weibo hub: web panel (follows, captcha, settings, WebDAV), device API, scheduled fetching and tunnel.

面板只在局域网用：从 Cloudflare 隧道进来的请求（带 CF-Connecting-IP）只能访问 /api/device/。
The panel is LAN only: requests arriving through Cloudflare (CF-Connecting-IP) may only reach /api/device/.
"""
from __future__ import annotations

import asyncio
import base64
import re
import secrets
import time
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI, HTTPException, Request, Response, WebSocket
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from . import auth, config, fetcher, images, store, tunnel, webdav
from .weibo.browser import browser_pool
from .weibo.captcha import captcha_manager
from .weibo.client import CaptchaRequired, SessionInvalid, WeiboError
from .weibo.session import new_client

WEB = Path(__file__).parent / "web"
ID_RE = re.compile(r"^\d{4,20}$")


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
    await browser_pool.shutdown()


app = FastAPI(title="weibo hub", version=config.VERSION, lifespan=lifespan, docs_url=None, redoc_url=None)


@app.middleware("http")
async def guard(request: Request, call_next):
    path = request.url.path
    if request.headers.get("cf-connecting-ip") and not path.startswith("/api/device/"):
        return JSONResponse({"error": "面板不对外网开放"}, status_code=403)
    if path.startswith("/api/panel/") and path not in ("/api/panel/state", "/api/panel/setup", "/api/panel/login"):
        if not auth.session_ok(request.cookies.get(auth.SESSION_COOKIE)):
            return JSONResponse({"error": "请先登录面板"}, status_code=401)
    response = await call_next(request)
    # 面板页面和脚本每次都向服务器确认（升级后浏览器不会还用旧脚本）。/ Make the browser revalidate the panel page and
    # scripts every time, so an upgrade never leaves an old script running.
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


# ---- 面板：状态 / Panel: status ----

@app.get("/api/panel/status")
async def panel_status():
    s = fetcher.state
    cfg = config.load()
    return {
        "version": config.VERSION,
        "fetch": {"running": s.running, "paused": s.paused, "current": s.current, "done": s.done, "total": s.total,
                  "new_follow": s.new_follow, "new_hot": s.new_hot, "last_round": s.last_round,
                  "next_round": s.next_round, "last_error": s.last_error, "visitor": s.visitor},
        "captcha": captcha_manager.pending,
        "users": len(store.user_ids()), "follow_posts": store.post_count("follow"), "hot_posts": store.post_count("hot"),
        "cache_mb": round(images.total_mb(), 1), "long_today": store.today("long"),
        "long_cap": cfg["long_daily_cap"], "tunnel": tunnel.state,
    }


@app.post("/api/panel/fetch/run")
async def fetch_run():
    """立即拉一轮；回一句话说明会发生什么。/ Run a round now; reply with what will happen."""
    if not (store.user_ids() or config.load()["hot_enabled"]):
        return {"ok": False, "message": "还没有关注，先去添加"}
    was_running = fetcher.state.running
    fetcher.resume()
    fetcher.note("面板点了立即拉一轮" + ("（这一轮结束后马上再拉）" if was_running else ""))
    return {"ok": True, "message": "正在拉取，这一轮结束后马上再拉一轮" if was_running else "已开始拉取"}


@app.get("/api/panel/logs")
async def panel_logs():
    return list(fetcher.log)


# ---- 面板：关注管理（照 weiboX）/ Panel: follows (as in weiboX) ----

class AddUser(BaseModel):
    link: str


def _uid_from(text: str) -> str:
    """从 UID、主页链接（weibo.com/u/123、m.weibo.cn/u/123、m.weibo.cn/profile/123、weibo.com/123）里取 UID。
    Pull a UID out of a bare UID or a profile link."""
    t = text.strip()
    if ID_RE.match(t):
        return t
    m = re.search(r"(?:/u/|/profile/|weibo\.com/|weibo\.cn/)(\d{5,20})", t)
    if not m:
        raise HTTPException(400, "认不出用户：填 UID，或粘贴 weibo.com/u/… 这样的主页链接")
    return m.group(1)


async def _weibo(call):
    """跑一次面板发起的微博请求，错误换成提示。/ Run one panel-initiated Weibo request, mapping errors to messages."""
    client = await new_client()
    try:
        return await call(client)
    except CaptchaRequired:
        raise HTTPException(409, "微博要求验证，稍后再试或等抓取时在“验证”页处理")
    except (SessionInvalid, WeiboError) as e:
        raise HTTPException(502, str(e))
    finally:
        await client.aclose()


@app.get("/api/panel/users")
async def users_list():
    return store.users()


@app.post("/api/panel/users")
async def users_add(body: AddUser):
    uid = _uid_from(body.link)
    info = await _weibo(lambda c: c.get_user_info(uid))
    store.add_user(info)
    fetcher.note(f"关注 {info.get('screen_name') or uid}")
    fetcher.wake()
    return info


class PickUser(BaseModel):
    id: str


@app.post("/api/panel/users/pick")
async def users_pick(body: PickUser):
    if not ID_RE.match(body.id):
        raise HTTPException(400, "UID 不对")
    info = await _weibo(lambda c: c.get_user_info(body.id))
    store.add_user(info)
    fetcher.note(f"关注 {info.get('screen_name') or body.id}")
    return info


@app.delete("/api/panel/users/{uid}")
async def users_remove(uid: str):
    store.remove_user(uid)
    return {"ok": True}


class Special(BaseModel):
    special: bool


@app.post("/api/panel/users/{uid}/special")
async def users_special(uid: str, body: Special):
    store.set_special(uid, body.special)
    return {"ok": True}


@app.get("/api/panel/search")
async def users_search(q: str):
    if not q.strip():
        return []
    rows = await _weibo(lambda c: c.search_users(q.strip()))
    mine = store.user_ids()
    for r in rows:
        r["followed"] = r.get("id") in mine
    return rows


# ---- 面板：内容、设置 / Panel: content and settings ----

@app.get("/api/panel/posts")
async def panel_posts(limit: int = 60):
    return store.latest(min(limit, 200))


@app.get("/api/panel/img/{pid}/{name}")
async def panel_img(pid: str, name: str):
    return _image(pid, name)


@app.get("/api/panel/settings")
async def settings_get():
    cfg = dict(config.load())
    cfg["has_cookie"] = bool((cfg.pop("cookie", "") or "").strip())
    cfg["has_webdav_pass"] = bool(cfg.pop("webdav_pass", ""))
    cfg.pop("tunnel_token", None)
    cfg["hot_channels"] = config.HOT_CHANNELS
    return cfg


INTS = {"interval_min": 30, "delay_min": 5, "delay_max": 5, "posts_per_user": 1, "min_check_min": 0,
        "hot_count": 1, "long_daily_cap": 0, "long_gap_s": 1, "max_images": 1, "keep_days": 1, "max_mb": 100}


@app.post("/api/panel/settings")
async def settings_set(body: dict):
    update = {}
    for key, low in INTS.items():
        if key in body:
            try:
                v = int(body[key])
            except (TypeError, ValueError):
                raise HTTPException(400, f"{key} 要填整数")
            if v < low:
                raise HTTPException(400, f"{key} 不能小于 {low}")
            update[key] = v
    if "hot_channel" in body:
        if body["hot_channel"] not in config.HOT_CHANNELS:
            raise HTTPException(400, "没有这个热门频道")
        update["hot_channel"] = body["hot_channel"]
    if "hot_enabled" in body:
        update["hot_enabled"] = bool(body["hot_enabled"])
    for key in ("webdav_url", "webdav_user"):
        if key in body:
            update[key] = str(body[key]).strip()
    # 密码类：留空不改。/ Secrets: empty means unchanged.
    if body.get("webdav_pass"):
        update["webdav_pass"] = str(body["webdav_pass"])
    if "cookie" in body and (body["cookie"] or body.get("clear_cookie")):
        update["cookie"] = str(body["cookie"]).strip()
    config.save(update)
    fetcher.wake()
    return await settings_get()


def _webdav_cfg() -> tuple[str, str, str]:
    cfg = config.load()
    if not cfg.get("webdav_url"):
        raise HTTPException(400, "先在设置里填 WebDAV 地址")
    return cfg["webdav_url"], cfg.get("webdav_user", ""), cfg.get("webdav_pass", "")


@app.post("/api/panel/webdav/backup")
async def webdav_backup():
    url, user, pw = _webdav_cfg()
    try:
        await asyncio.to_thread(webdav.backup, url, user, pw, store.users(), config.load().get("cookie", ""))
    except Exception as e:
        raise HTTPException(502, str(e))
    fetcher.note(f"已备份到 WebDAV：{len(store.user_ids())} 人")
    return {"ok": True, "users": len(store.user_ids())}


@app.post("/api/panel/webdav/restore")
async def webdav_restore():
    url, user, pw = _webdav_cfg()
    try:
        users, cookie = await asyncio.to_thread(webdav.restore, url, user, pw)
    except Exception as e:
        raise HTTPException(502, str(e))
    store.replace_users(users)
    if cookie.strip():
        config.save({"cookie": cookie.strip()})
    fetcher.note(f"已从 WebDAV 恢复：{len(users)} 人" + ("，含 Cookie" if cookie.strip() else ""))
    fetcher.wake()
    return {"ok": True, "users": len(users), "cookie": bool(cookie.strip())}


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


# ---- 验证码：截图流和点击转发（照 weiboX）/ Captcha: screenshot stream and input relay (as in weiboX) ----

@app.websocket("/api/panel/captcha/ws")
async def captcha_ws(ws: WebSocket):
    # http 中间件不管 WebSocket：这里同样拒绝隧道来的连接。/ The http middleware skips WebSockets; reject tunnels here.
    if ws.headers.get("cf-connecting-ip") or not auth.session_ok(ws.cookies.get(auth.SESSION_COOKIE)):
        await ws.close(code=4401)
        return
    await ws.accept()

    async def send_frames():
        while True:
            if captcha_manager.pending:
                img = await captcha_manager.screenshot()
                if img:
                    await ws.send_json({"type": "frame", "data": base64.b64encode(img).decode("ascii")})
                else:
                    await ws.send_json({"type": "state", "pending": True})
            else:
                await ws.send_json({"type": "state", "pending": False})
            await asyncio.sleep(0.6)

    async def recv_events():
        while True:
            msg = await ws.receive_json()
            t = msg.get("type")
            x, y = float(msg.get("x", 0)), float(msg.get("y", 0))
            if t == "down":
                await captcha_manager.pointer_down(x, y)
            elif t == "move":
                await captcha_manager.pointer_move(x, y)
            elif t == "up":
                await captcha_manager.pointer_up(x, y)
            elif t == "click":
                await captcha_manager.click(x, y)
            elif t == "reload":
                await captcha_manager.reload()
            elif t == "resolve":
                captcha_manager.resolve()
            elif t == "cancel":
                captcha_manager.cancel()

    tasks = [asyncio.create_task(send_frames()), asyncio.create_task(recv_events())]
    try:
        await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
    except Exception:
        pass
    finally:
        for t in tasks:
            t.cancel()


# ---- 设备接口 / Device API ----

def _device(request: Request) -> None:
    token = request.headers.get("x-weibo-token") or request.query_params.get("token")
    if not auth.device_ok(token):
        raise HTTPException(401, "设备令牌不对")


def _brief(p: dict) -> dict:
    text = p["text"] or ""
    # pics：有方形小图 s1..sN.jpg 的原图张数；0.1.3 之前拉的微博是 0（只有 thumb.jpg）。
    # pics: source images with square small images s1..sN.jpg; 0 for posts fetched before 0.1.3 (thumb.jpg only).
    return {"id": p["id"], "user": p["user_name"], "time": p["created_ts"], "text": text[:140],
            "more": len(text) > 140 or bool(p["rt_text"]), "images": p["images"], "ratio": p["ratio"],
            "pics": p.get("pics") or 0, "source": p.get("source") or "",
            "likes": p["likes"], "comments": p["comments"], "reposts": p["reposts"],
            "rt_user": p["rt_user"], "rt_text": (p["rt_text"] or "")[:80], "fetched_at": p["fetched_at"]}


@app.get("/api/device/v1/feed")
async def device_feed(request: Request, seg: str = "follow", since: float = 0, limit: int = 100):
    _device(request)
    if seg not in ("follow", "hot"):
        raise HTTPException(400, "seg 只能是 follow 或 hot")
    rows = store.feed(seg, since, min(max(limit, 1), 200))
    return {"now": time.time(), "posts": [_brief(p) for p in rows]}


@app.get("/api/device/v1/post/{pid}")
async def device_post(request: Request, pid: str):
    _device(request)
    p = store.post(pid) if ID_RE.match(pid) else None
    if not p:
        raise HTTPException(404, "没有这条")
    # starts：每张原图的第一页（从 0 起），点小图从对应那页看大图。/ starts: each source image's first page (0-based),
    # so tapping a small image opens the matching page.
    starts = [int(x) for x in (p.get("starts") or "").split(",") if x.strip().isdigit()]
    return {**_brief(p), "text": p["text"], "rt_text": p["rt_text"], "source": p["source"], "starts": starts}


@app.get("/api/device/v1/post/{pid}/comments")
async def device_comments(request: Request, pid: str):
    """评论：第一次请求时去微博取一次（热评前 20 条），存下来以后不再取。/ Comments: fetched from Weibo on the first
    request (top 20) and kept."""
    _device(request)
    if not ID_RE.match(pid) or not store.post(pid):
        raise HTTPException(404, "没有这条")
    cached = store.comments(pid)
    if cached is not None:
        return {"comments": cached}
    store.count("comments")
    rows, _ = await _weibo(lambda c: c.get_comments(pid))
    items = [{"user": c.get("user_name", ""), "text": c.get("text", ""), "likes": c.get("like_count", 0)}
             for c in rows[:20]]
    store.save_comments(pid, items)
    return {"comments": items}


def _image(pid: str, name: str) -> FileResponse:
    if not ID_RE.match(pid) or not re.match(r"^(thumb|\d{1,3}|s\d{1,2})\.jpg$", name):
        raise HTTPException(400, "路径不对")
    path = images.note_dir(pid) / name
    if not path.exists():
        raise HTTPException(404, "没有这张图")
    return FileResponse(path, media_type="image/jpeg")


@app.get("/api/device/v1/img/{pid}/{name}")
async def device_img(request: Request, pid: str, name: str):
    _device(request)
    return _image(pid, name)


@app.get("/healthz")
async def healthz():
    return {"ok": True}


app.mount("/static", StaticFiles(directory=WEB), name="static")


@app.get("/")
async def index():
    return FileResponse(WEB / "index.html")
