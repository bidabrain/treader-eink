"""rednote hub：网页面板、设备接口、定时拉取、隧道、浏览器桌面（noVNC）转发。

rednote hub: web panel, device API, scheduled fetching, tunnel and a proxied browser desktop (noVNC).

面板只在局域网用：从 Cloudflare 隧道进来的请求（带 CF-Connecting-IP）只能访问 /api/device/。
The panel is LAN only: requests arriving through Cloudflare (CF-Connecting-IP) may only reach /api/device/.
"""
from __future__ import annotations

import asyncio
import re
import time
from contextlib import asynccontextmanager
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import httpx
import websockets
from fastapi import FastAPI, HTTPException, Request, Response, WebSocket
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from . import auth, config, fetcher, images, mcp, rec, store, tunnel, webdav

WEB = Path(__file__).parent / "web"
ID_RE = re.compile(r"^[0-9a-f]{24}$")


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


app = FastAPI(title="rednote hub", version=config.VERSION, lifespan=lifespan, docs_url=None, redoc_url=None)


@app.middleware("http")
async def guard(request: Request, call_next):
    path = request.url.path
    if request.headers.get("cf-connecting-ip") and not path.startswith("/api/device/"):
        return JSONResponse({"error": "面板不对外网开放"}, status_code=403)
    if path.startswith("/api/panel/") and path not in ("/api/panel/state", "/api/panel/setup", "/api/panel/login"):
        if not auth.session_ok(request.cookies.get(auth.SESSION_COOKIE)):
            return JSONResponse({"error": "请先登录面板"}, status_code=401)
    if path.startswith("/desk") and not auth.session_ok(request.cookies.get(auth.SESSION_COOKIE)):
        return JSONResponse({"error": "请先登录面板"}, status_code=401)
    return await call_next(request)


# ---- 面板：登录 / Panel: sign-in ----

class Password(BaseModel):
    password: str


def _login(resp: Response) -> None:
    resp.set_cookie(auth.SESSION_COOKIE, auth.new_session(), httponly=True, samesite="strict",
                    max_age=auth.SESSION_TTL)


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


# ---- 面板：状态与小红书账号 / Panel: status and the Xiaohongshu account ----

@app.get("/api/panel/status")
async def panel_status():
    cfg = config.load()
    s = fetcher.state
    return {
        "version": config.VERSION, "mcp_up": await mcp.health(),
        "fetch": {"running": s.running, "paused": s.paused, "current": s.current, "last_round": s.last_round,
                  "next_round": s.next_round, "new_notes": s.new_notes},
        "requests_today": store.requests_today(), "daily_cap": cfg["daily_cap"],
        "rec_today": store.requests_today("rec"), "rec_daily_cap": cfg["rec_daily_cap"], "rec_enabled": cfg["rec_enabled"],
        "creators": len(store.creators()), "notes": store.note_count(), "cache_mb": round(images.total_mb(), 1),
        "tunnel": tunnel.state,
    }


@app.get("/api/panel/xhs/status")
async def xhs_status():
    try:
        return await mcp.login_status()
    except mcp.McpError as e:
        raise HTTPException(502, str(e))


@app.post("/api/panel/xhs/qrcode")
async def xhs_qrcode():
    try:
        return await mcp.login_qrcode()
    except mcp.McpError as e:
        raise HTTPException(502, str(e))


# ---- 面板：博主 / Panel: creators ----

class CreatorLink(BaseModel):
    link: str


def _parse_profile(link: str) -> tuple[str, str]:
    """从博主主页链接里取 user_id 和 xsec_token。/ Pull user_id and xsec_token from a profile link."""
    u = urlparse(link.strip())
    m = re.search(r"/user/profile/([0-9a-f]{24})", u.path)
    token = (parse_qs(u.query).get("xsec_token") or [""])[0]
    if not m or not token:
        raise HTTPException(400, "链接里要有 /user/profile/用户ID 和 xsec_token，请从小红书网页或分享里复制完整的博主主页链接")
    return m.group(1), token


@app.get("/api/panel/creators")
async def creators_list():
    return store.creators()


@app.post("/api/panel/creators")
async def creators_add(body: CreatorLink):
    user_id, token = _parse_profile(body.link)
    store.add_creator(user_id, token)
    fetcher.note(f"添加博主 {user_id}")
    return {"ok": True, "user_id": user_id}


class CreatorPick(BaseModel):
    user_id: str
    xsec_token: str
    nickname: str = ""


@app.post("/api/panel/creators/pick")
async def creators_pick(body: CreatorPick):
    if not ID_RE.match(body.user_id) or not body.xsec_token:
        raise HTTPException(400, "参数不对")
    store.add_creator(body.user_id, body.xsec_token, body.nickname)
    fetcher.note(f"添加博主 {body.nickname or body.user_id}")
    return {"ok": True}


@app.delete("/api/panel/creators/{user_id}")
async def creators_remove(user_id: str):
    store.remove_creator(user_id)
    return {"ok": True}


@app.get("/api/panel/discover")
async def discover():
    """从首页推荐里列出作者，方便挑选关注（会请求一次小红书）。/ List authors from the home feed (one request)."""
    store.count_request()
    try:
        feeds = await mcp.feeds()
    except mcp.McpError as e:
        raise HTTPException(502, str(e))
    seen: dict[str, dict] = {}
    for f in feeds:
        nc = f.get("noteCard") or {}
        user = nc.get("user") or {}
        uid = user.get("userId")
        if uid and uid not in seen:
            seen[uid] = {"user_id": uid, "nickname": user.get("nickname") or user.get("nickName") or "",
                         "xsec_token": f.get("xsecToken") or "", "sample": nc.get("displayTitle") or "",
                         "type": nc.get("type")}
    return list(seen.values())


# ---- 面板：拉取、内容、设置 / Panel: fetching, content, settings ----

@app.post("/api/panel/fetch/run")
async def fetch_run():
    fetcher.resume()
    return {"ok": True}


@app.get("/api/panel/notes")
async def panel_notes(limit: int = 60):
    return store.notes(0, min(limit, 200))


@app.get("/api/panel/img/{note_id}/{name}")
async def panel_img(note_id: str, name: str):
    return await _image(note_id, name)


@app.get("/api/panel/logs")
async def panel_logs():
    return list(fetcher.log)


def _public(cfg: dict) -> dict:
    """面板看到的设置：WebDAV 密码不回显，只说有没有。/ Settings as the panel sees them: the WebDAV password is never
    echoed, only whether one is set."""
    out = dict(cfg)
    out["has_webdav_pass"] = bool(out.pop("webdav_pass", ""))
    return out


@app.get("/api/panel/settings")
async def settings_get():
    return _public(config.load())


@app.post("/api/panel/settings")
async def settings_set(body: dict):
    ints = ("interval_min", "delay_min", "delay_max", "per_creator", "daily_cap", "max_images", "keep_days", "max_mb",
            "rec_max", "rec_cache_min", "rec_daily_cap", "rec_gap_s")
    update = {}
    if "rec_enabled" in body:
        update["rec_enabled"] = bool(body["rec_enabled"])
    for key in ints:
        if key in body:
            try:
                update[key] = max(1, int(body[key]))
            except (TypeError, ValueError):
                raise HTTPException(400, f"{key} 要填正整数")
    if "delay_min" in update and update["delay_min"] < 5:
        raise HTTPException(400, "请求间隔至少 5 秒，太快容易被风控")
    if "interval_min" in update and update["interval_min"] < 30:
        raise HTTPException(400, "每轮间隔至少 30 分钟")
    if "rec_cache_min" in update and update["rec_cache_min"] < 5:
        raise HTTPException(400, "推荐列表至少缓存 5 分钟")
    if "rec_gap_s" in update and update["rec_gap_s"] < 3:
        raise HTTPException(400, "推荐请求间隔至少 3 秒")
    if "rec_max" in update:
        update["rec_max"] = min(update["rec_max"], 30)
    for key in ("webdav_url", "webdav_user"):
        if key in body:
            update[key] = str(body[key]).strip()
    # 密码留空表示不改。/ An empty password means unchanged.
    if body.get("webdav_pass"):
        update["webdav_pass"] = str(body["webdav_pass"])
    cfg = config.save(update)
    fetcher.wake()
    return _public(cfg)


def _webdav_cfg() -> tuple[str, str, str]:
    cfg = config.load()
    if not cfg.get("webdav_url"):
        raise HTTPException(400, "先在设置里填 WebDAV 地址")
    return cfg["webdav_url"], cfg.get("webdav_user", ""), cfg.get("webdav_pass", "")


@app.post("/api/panel/webdav/backup")
async def webdav_backup():
    url, user, pw = _webdav_cfg()
    rows = store.creators()
    try:
        await asyncio.to_thread(webdav.backup, url, user, pw, rows)
    except Exception as e:
        raise HTTPException(502, str(e))
    fetcher.note(f"已备份到 WebDAV：{len(rows)} 位博主")
    return {"ok": True, "creators": len(rows)}


@app.post("/api/panel/webdav/restore")
async def webdav_restore():
    url, user, pw = _webdav_cfg()
    try:
        rows = await asyncio.to_thread(webdav.restore, url, user, pw)
    except Exception as e:
        raise HTTPException(502, str(e))
    store.replace_creators(rows)
    fetcher.note(f"已从 WebDAV 恢复：{len(rows)} 位博主")
    fetcher.wake()
    return {"ok": True, "creators": len(rows)}


@app.post("/api/panel/device/rotate")
async def device_rotate():
    import secrets
    return _public(config.save({"device_token": secrets.token_urlsafe(24)}))


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


# ---- 设备接口 / Device API ----

def _device(request: Request) -> None:
    token = request.headers.get("x-rednote-token") or request.query_params.get("token")
    if not auth.device_ok(token):
        raise HTTPException(401, "设备令牌不对")


def _brief(n: dict) -> dict:
    return {"id": n["id"], "title": n["title"], "author": n["author"], "time": n["time"], "images": n["images"],
            "likes": n["likes"], "comments": n["comment_count"], "fetched_at": n["fetched_at"],
            "ratio": n.get("ratio") or 1.333,
            "source": n.get("source") or "follow"}


@app.get("/api/device/v1/feed")
async def device_feed(request: Request, since: float = 0, limit: int = 50):
    _device(request)
    rows = store.notes(since, min(max(limit, 1), 100), source="follow")
    return {"now": time.time(), "notes": [_brief(n) for n in rows]}


@app.get("/api/device/v1/note/{note_id}")
async def device_note(request: Request, note_id: str):
    _device(request)
    n = store.note(note_id)
    if not n:
        raise HTTPException(404, "没有这篇")
    return {**_brief(n), "body": n["body"], "comment_list": n["comments"]}


async def _image(note_id: str, name: str) -> FileResponse:
    if not ID_RE.match(note_id) or not re.match(r"^(thumb|\d{1,3})\.jpg$", name):
        raise HTTPException(400, "路径不对")
    path = images.note_dir(note_id) / name
    # 这篇的图还在后台做：等这一页做好。/ This note's images are still being made: wait for the page.
    if not path.exists() and not await images.wait_page(note_id, path):
        raise HTTPException(404, "没有这张图")
    return FileResponse(path, media_type="image/jpeg")


@app.get("/api/device/v1/img/{note_id}/{name}")
async def device_img(request: Request, note_id: str, name: str):
    _device(request)
    return await _image(note_id, name)


# ---- 设备接口：推荐（按需代拉） / Device API: recommendations (fetched on demand) ----

@app.get("/api/device/v1/rec")
async def device_rec(request: Request, refresh: int = 0):
    _device(request)
    try:
        return await rec.refresh(bool(refresh))
    except rec.RecError as e:
        raise HTTPException(e.status, str(e))


@app.get("/api/device/v1/rec/cover/{note_id}.jpg")
async def device_rec_cover(request: Request, note_id: str):
    _device(request)
    if not ID_RE.match(note_id):
        raise HTTPException(400, "路径不对")
    path = rec.cover_path(note_id)
    if not path.exists():
        raise HTTPException(404, "没有这张封面")
    return FileResponse(path, media_type="image/jpeg")


@app.get("/api/device/v1/rec/note/{note_id}")
async def device_rec_note(request: Request, note_id: str):
    _device(request)
    if not ID_RE.match(note_id):
        raise HTTPException(400, "路径不对")
    try:
        n = await rec.open_note(note_id)
    except rec.RecError as e:
        raise HTTPException(e.status, str(e))
    if not n:
        raise HTTPException(404, "没有这篇")
    return {**_brief(n), "body": n["body"], "comment_list": n["comments"]}


# ---- 浏览器桌面（noVNC），只在面板里用 / Browser desktop (noVNC), panel only ----

@app.websocket("/desk/websockify")
async def desk_ws(ws: WebSocket):
    # http 中间件不管 WebSocket，这里同样拒绝从隧道进来的连接。
    # The http middleware does not see WebSockets, so reject tunnel connections here too.
    if ws.headers.get("cf-connecting-ip") or not auth.session_ok(ws.cookies.get(auth.SESSION_COOKIE)):
        await ws.close(code=4401)
        return
    await ws.accept(subprotocol="binary" if "binary" in (ws.scope.get("subprotocols") or []) else None)
    upstream = config.NOVNC_URL.replace("http", "ws", 1) + "/websockify"
    try:
        async with websockets.connect(upstream, subprotocols=["binary"], origin=config.NOVNC_URL,
                                      max_size=None) as up:
            async def down():
                async for msg in up:
                    if isinstance(msg, bytes):
                        await ws.send_bytes(msg)
                    else:
                        await ws.send_text(msg)

            async def upward():
                while True:
                    msg = await ws.receive()
                    if msg.get("type") == "websocket.disconnect":
                        break
                    if msg.get("bytes") is not None:
                        await up.send(msg["bytes"])
                    elif msg.get("text") is not None:
                        await up.send(msg["text"])

            tasks = [asyncio.create_task(down()), asyncio.create_task(upward())]
            await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
            for t in tasks:
                t.cancel()
    except Exception:
        pass
    finally:
        try:
            await ws.close()
        except Exception:
            pass


@app.get("/desk/{path:path}")
async def desk_http(path: str):
    async with httpx.AsyncClient(timeout=20.0) as client:
        try:
            r = await client.get(f"{config.NOVNC_URL}/{path}")
        except httpx.HTTPError:
            raise HTTPException(502, "浏览器桌面不可用（mcp 用的不是 novnc 镜像？）")
    return Response(r.content, status_code=r.status_code, media_type=r.headers.get("content-type"))


# ---- 页面 / Page ----

app.mount("/static", StaticFiles(directory=WEB), name="static")


@app.get("/")
async def index():
    return FileResponse(WEB / "index.html")
