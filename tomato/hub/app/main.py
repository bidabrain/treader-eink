"""tomato hub：放在官方番茄下载器前面。设备和安卓 App 的请求（/api/…、/download/…）核对设备密码后原样转给下载器；
网页面板（只在局域网）管设备密码和 Cloudflare 隧道；下载器自己的网页在 /webui/，要先登录面板。

tomato hub: sits in front of the official Tomato downloader. Device and Android-app requests (/api/..., /download/...)
are checked against the device password and forwarded unchanged; the web panel (LAN only) manages the device password
and the Cloudflare tunnel; the downloader's own web page lives at /webui/ behind the panel sign-in.

从 Cloudflare 隧道进来的请求（带 CF-Connecting-IP）只能访问设备接口。
Requests arriving through Cloudflare (CF-Connecting-IP) may only reach the device API.
"""
from __future__ import annotations

import asyncio
import re
import time
from collections import deque
from contextlib import asynccontextmanager
from pathlib import Path

import httpx
from fastapi import FastAPI, HTTPException, Request, Response
from fastapi.responses import FileResponse, JSONResponse, RedirectResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel
from starlette.background import BackgroundTask

from . import auth, config, tunnel

WEB = Path(__file__).parent / "web"
# 设备和 App 用到的下载器接口；配置、自更新这些只能从面板里的下载器网页改。
# Downloader endpoints the device and app use; config and self-update stay behind the panel's downloader page.
DEVICE_RE = re.compile(r"^(api/(search|jobs|library|updates|status|preview|history)(/.*)?|download/.+)$")
# 转发时去掉的头（逐跳头，和我们自己的密码头）。/ Headers dropped when forwarding (hop-by-hop, and our password).
HOP = {"host", "connection", "keep-alive", "transfer-encoding", "te", "trailer", "upgrade", "proxy-authorization",
       "proxy-authenticate", "content-length", "x-tomato-password", "cookie", "cf-connecting-ip"}

log: deque[str] = deque(maxlen=300)
_client: httpx.AsyncClient | None = None


def note(msg: str) -> None:
    log.appendleft(time.strftime("%m-%d %H:%M:%S ") + msg)


async def _seed() -> None:
    """第一次连上下载器时，把它的默认格式从 EPUB 改成 TXT（设备上番茄书用 TXT，更新只追加章节）。之后用户在下载器
    网页里怎么改都不再动。
    On first contact, switch the downloader's default format from EPUB to TXT (Tomato books are TXT on the device and
    updates only append chapters). Later changes in the downloader page are left alone."""
    for _ in range(60):
        if config.load().get("seeded"):
            return
        try:
            r = await _client.get(f"{config.UPSTREAM}/api/config/full", timeout=10)
            if r.status_code == 200:
                cfg = r.json()
                if cfg.get("novel_format") != "txt":
                    cfg["novel_format"] = "txt"
                    await _client.post(f"{config.UPSTREAM}/api/config/full", json=cfg, timeout=10)
                    note("已把下载器的默认格式设为 TXT")
                config.save({"seeded": True})
                return
        except httpx.HTTPError:
            pass
        await asyncio.sleep(5)


@asynccontextmanager
async def lifespan(app: FastAPI):
    global _client
    config.ensure_dirs()
    config.load()
    _client = httpx.AsyncClient(timeout=httpx.Timeout(10.0, read=300.0), follow_redirects=False)
    seed = asyncio.create_task(_seed())
    await tunnel.apply()
    yield
    seed.cancel()
    await tunnel.stop()
    await _client.aclose()


app = FastAPI(title="tomato hub", version=config.VERSION, lifespan=lifespan, docs_url=None, redoc_url=None)


@app.middleware("http")
async def guard(request: Request, call_next):
    path = request.url.path
    if request.headers.get("cf-connecting-ip") and not DEVICE_RE.match(path.lstrip("/")):
        return JSONResponse({"error": "面板不对外网开放"}, status_code=403)
    if path.startswith("/hub/api/") and path not in ("/hub/api/state", "/hub/api/setup", "/hub/api/login"):
        if not auth.session_ok(request.cookies.get(auth.SESSION_COOKIE)):
            return JSONResponse({"error": "请先登录面板"}, status_code=401)
    response = await call_next(request)
    # 面板页面和脚本每次都向服务器确认。/ Make the browser revalidate the panel page and scripts every time.
    if path == "/" or path.startswith("/hub/static/"):
        response.headers["Cache-Control"] = "no-cache"
    return response


# ---- 转发 / Forwarding ----

async def _forward(request: Request, upstream_path: str) -> Response:
    url = f"{config.UPSTREAM}/{upstream_path.lstrip('/')}"
    headers = {k: v for k, v in request.headers.items() if k.lower() not in HOP}
    body = await request.body()
    req = _client.build_request(request.method, url, params=request.query_params, headers=headers, content=body)
    try:
        resp = await _client.send(req, stream=True)
    except httpx.HTTPError as e:
        note(f"连不上下载器：{type(e).__name__} {request.method} {request.url.path}")
        return JSONResponse({"error": "番茄下载器没有响应，稍后再试"}, status_code=502)
    out = {k: v for k, v in resp.headers.items() if k.lower() not in HOP | {"content-encoding"}}
    return StreamingResponse(resp.aiter_raw(), status_code=resp.status_code, headers=out,
                             background=BackgroundTask(resp.aclose))


@app.api_route("/api/{rest:path}", methods=["GET", "POST", "PUT", "DELETE"])
async def device_api(request: Request, rest: str):
    if not DEVICE_RE.match(f"api/{rest}"):
        raise HTTPException(404, "没有这个接口")
    if not auth.device_ok(request.headers.get("x-tomato-password")):
        raise HTTPException(401, "设备密码不对")
    if request.method == "POST" and rest.startswith("jobs"):
        note(f"设备下载：{request.method} /api/{rest}")
    return await _forward(request, f"api/{rest}")


@app.get("/download/{rest:path}")
async def device_download(request: Request, rest: str):
    if not auth.device_ok(request.headers.get("x-tomato-password")):
        raise HTTPException(401, "设备密码不对")
    note(f"设备取书：/download/{rest[:80]}")
    return await _forward(request, f"download/{rest}")


# 下载器自己的网页：先登录面板。/ The downloader's own page: panel sign-in first.
@app.api_route("/webui/{rest:path}", methods=["GET", "POST", "PUT", "DELETE"])
async def webui(request: Request, rest: str):
    if not auth.session_ok(request.cookies.get(auth.SESSION_COOKIE)):
        return RedirectResponse("/")
    return await _forward(request, rest)


@app.get("/webui")
async def webui_root():
    return RedirectResponse("/webui/")


# ---- 面板：登录 / Panel: sign-in ----

class Password(BaseModel):
    password: str


def _login(resp: Response) -> None:
    resp.set_cookie(auth.SESSION_COOKIE, auth.new_session(), httponly=True, samesite="strict", max_age=auth.SESSION_TTL)


@app.get("/hub/api/state")
async def panel_state(request: Request):
    return {"setup": not auth.has_password(),
            "signed_in": auth.session_ok(request.cookies.get(auth.SESSION_COOKIE)), "version": config.VERSION}


@app.post("/hub/api/setup")
async def panel_setup(body: Password, resp: Response):
    if auth.has_password():
        raise HTTPException(409, "已经设置过密码")
    if len(body.password) < 8:
        raise HTTPException(400, "密码至少 8 位")
    auth.set_password(body.password)
    _login(resp)
    return {"ok": True}


@app.post("/hub/api/login")
async def panel_login(body: Password, resp: Response):
    ok, why = auth.check_password(body.password)
    if not ok:
        raise HTTPException(403, why)
    _login(resp)
    return {"ok": True}


@app.post("/hub/api/logout")
async def panel_logout(request: Request, resp: Response):
    auth.drop_session(request.cookies.get(auth.SESSION_COOKIE))
    resp.delete_cookie(auth.SESSION_COOKIE)
    return {"ok": True}


@app.post("/hub/api/password")
async def panel_password(body: Password):
    if len(body.password) < 8:
        raise HTTPException(400, "密码至少 8 位")
    auth.set_password(body.password)
    return {"ok": True}


# ---- 面板：状态、设备、隧道 / Panel: status, device, tunnel ----

@app.get("/hub/api/status")
async def panel_status():
    up = {"ok": False, "version": "", "save_dir": "", "books": None, "jobs": None, "error": ""}
    try:
        r = await _client.get(f"{config.UPSTREAM}/api/status", timeout=5)
        s = r.json()
        up.update(ok=r.status_code == 200, version=s.get("version", ""), save_dir=s.get("save_dir", ""))
        # 书库扫描是异步的：先开始扫，再等它扫完（最多约 3 秒）。/ The library scan is asynchronous: start it, then
        # wait for it to finish (about 3 s at most).
        lib = (await _client.get(f"{config.UPSTREAM}/api/library", params={"start": "true"}, timeout=10)).json()
        for _ in range(10):
            if not lib.get("running"):
                break
            await asyncio.sleep(0.3)
            lib = (await _client.get(f"{config.UPSTREAM}/api/library", params={"start": "false"}, timeout=10)).json()
        up["books"] = sum(1 for i in lib.get("items") or [] if i.get("kind") == "file")
        jobs = (await _client.get(f"{config.UPSTREAM}/api/jobs", timeout=10)).json()
        up["jobs"] = sum(1 for j in jobs.get("items") or [] if str(j.get("state", "")).lower() in ("running", "queued", "pending"))
    except (httpx.HTTPError, ValueError) as e:
        up["error"] = f"{type(e).__name__}"
    return {"version": config.VERSION, "downloader": up, "tunnel": tunnel.state}


@app.get("/hub/api/device")
async def device_info():
    cfg = config.load()
    return {"password": cfg["device_password"], "tunnel": tunnel.state, "tunnel_mode": cfg["tunnel_mode"]}


class DevicePassword(BaseModel):
    password: str = ""


@app.post("/hub/api/device/password")
async def device_password(body: DevicePassword):
    """换设备密码：留空就随机生成一个。/ Change the device password; empty generates a random one."""
    pw = body.password.strip() or config.new_password()
    if len(pw) < 4 or len(pw) > 63:
        raise HTTPException(400, "设备密码要 4～63 位")
    config.save({"device_password": pw})
    note("设备密码已更换")
    return {"password": pw}


class TunnelBody(BaseModel):
    mode: str
    token: str = ""


@app.post("/hub/api/tunnel")
async def tunnel_set(body: TunnelBody):
    if body.mode not in ("off", "quick", "token"):
        raise HTTPException(400, "方式不对")
    update = {"tunnel_mode": body.mode}
    if body.token:
        update["tunnel_token"] = body.token.strip()
    config.save(update)
    await tunnel.apply()
    return tunnel.state


@app.get("/hub/api/logs")
async def panel_logs():
    return list(log)


@app.get("/healthz")
async def healthz():
    return {"ok": True}


app.mount("/hub/static", StaticFiles(directory=WEB), name="static")


@app.get("/")
async def index():
    return FileResponse(WEB / "index.html")
