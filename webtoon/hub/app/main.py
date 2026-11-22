"""webtoon hub：网页面板和小纸 Pico 设备接口。服务器不保存订阅：订阅在设备的 TF 卡上，设备来问“有没有新话”。

webtoon hub: web panel and the Read Pico device API. The server keeps no subscriptions: they live on the device's TF
card and the device asks whether there are new episodes.

面板只在局域网用：从 Cloudflare 隧道进来的请求（带 CF-Connecting-IP）只能访问 /api/device/。
The panel is LAN only: requests arriving through Cloudflare (CF-Connecting-IP) may only reach /api/device/.

只拉网页上公开免费的话；自己部署、只给自己用，不提供公共服务。
Only free public episodes are fetched; self-hosted for personal use, never a public service.
"""
from __future__ import annotations

import asyncio
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI, HTTPException, Request, Response
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from . import auth, config, jobs, source, tunnel

WEB = Path(__file__).parent / "web"
LIST_MAX = 120
CHECK_MAX = 200


@asynccontextmanager
async def lifespan(app: FastAPI):
    config.ensure_dirs()
    config.load()
    tasks = [asyncio.create_task(jobs.worker()), asyncio.create_task(jobs.pruner())]
    await tunnel.apply()
    yield
    for t in tasks:
        t.cancel()
    await tunnel.stop()


app = FastAPI(title="webtoon hub", version=config.VERSION, lifespan=lifespan, docs_url=None, redoc_url=None)


@app.middleware("http")
async def guard(request: Request, call_next):
    path = request.url.path
    if request.headers.get("cf-connecting-ip") and not path.startswith("/api/device/"):
        return JSONResponse({"error": "面板不对外网开放"}, status_code=403)
    if path.startswith("/api/panel/") and path not in ("/api/panel/state", "/api/panel/setup", "/api/panel/login"):
        if not auth.session_ok(request.cookies.get(auth.SESSION_COOKIE)):
            return JSONResponse({"error": "请先登录面板"}, status_code=401)
    response = await call_next(request)
    if path == "/" or path.startswith("/static/"):
        response.headers["Cache-Control"] = "no-cache"
    return response


@app.exception_handler(source.SourceError)
async def source_error(_request: Request, e: source.SourceError):
    return JSONResponse({"detail": str(e)}, status_code=e.status)


# ---- 共用 / Shared ----

_covers: dict[tuple[str, str], str] = {}   # 记下列表里见过的封面地址 / Cover URLs seen in lists
COVERS_MAX = 5000


def _remember_cover(lang: str, tid: str, url: str) -> None:
    if len(_covers) >= COVERS_MAX:
        _covers.clear()          # 丢了也只是封面要重新查一次 / Losing them only means one more lookup per cover
    _covers[(lang, tid)] = url


def _simp(lang: str, zh: str):
    """中文版是繁体；zh=s 时文字转成简体显示（图里的字不变）。/ The Chinese site is traditional; with zh=s text is shown
    in simplified (text inside images stays as is)."""
    return source.to_simp if lang == "zh-hant" and zh == "s" else (lambda s: s)


def _cards(lang: str, items: list[dict], zh: str, limit: int) -> list[dict]:
    conv = _simp(lang, zh)
    out = []
    for c in items[:max(1, min(limit, LIST_MAX))]:
        _remember_cover(lang, c["id"], c["cover"])
        out.append({"id": c["id"], "title": conv(c["title"]), "author": conv(c["author"]), "genre": conv(c["genre"]),
                    "likes": c["likes"], "canvas": c["canvas"], "adult": c["adult"]})
    return out


async def _store(lang: str, seg: str, key: str, zh: str, limit: int) -> dict:
    r = await source.store(lang, seg, key)
    conv = _simp(lang, zh)
    return {"seg": r["seg"], "key": r["key"], "keys": [{"key": k["key"], "name": conv(k["name"])} for k in r["keys"]],
            "items": _cards(lang, r["items"], zh, limit)}


async def _search(lang: str, q: str, zh: str) -> dict:
    r = await source.search(lang, q)
    return {"q": r["q"], "items": _cards(lang, r["items"], zh, LIST_MAX)}


async def _title(lang: str, tid: str, zh: str) -> dict:
    t = await source.title(lang, tid)
    conv = _simp(lang, zh)
    _remember_cover(lang, tid, t["cover"])
    return {"id": tid, "title": conv(t["title"]), "author": conv(t["author"]), "genre": conv(t["genre"]),
            "summary": conv(t["summary"]), "done": t["done"], "latest": t["latest"],
            "episodes": [{"no": e["no"], "title": conv(e["title"]), "date": e["date"]} for e in t["episodes"]]}


async def _cover(lang: str, tid: str) -> FileResponse:
    if not source.ID_RE.match(tid) or not source.LANG_RE.match(lang):
        raise HTTPException(400, "路径不对")
    url = _covers.get((lang, tid), "")
    if not url and not jobs.cover_path(lang, tid).exists():
        try:
            url = (await source.title(lang, tid))["cover"]
        except source.SourceError:
            url = ""
    path = await jobs.cover(lang, tid, url)
    if not path:
        raise HTTPException(404, "没有封面")
    return FileResponse(path, media_type="image/jpeg")


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


# ---- 面板：状态、发现页预览、设置 / Panel: status, store preview, settings ----

@app.get("/api/panel/status")
async def panel_status():
    return {"version": config.VERSION, "cache_mb": round(await asyncio.to_thread(jobs.cache_mb), 1),
            "queue": jobs._queue.qsize() if jobs._queue else 0, "recent": jobs.recent, "tunnel": tunnel.state}


@app.get("/api/panel/store")
async def panel_store(lang: str = "zh-hant", seg: str = "rank", key: str = "", zh: str = "s"):
    return await _store(lang, seg, key, zh, LIST_MAX)


@app.get("/api/panel/search")
async def panel_search(q: str, lang: str = "zh-hant", zh: str = "s"):
    return await _search(lang, q, zh)


@app.get("/api/panel/title/{tid}")
async def panel_title(tid: str, lang: str = "zh-hant", zh: str = "s"):
    return await _title(lang, tid, zh)


@app.get("/api/panel/cover/{tid}.jpg")
async def panel_cover(tid: str, lang: str = "zh-hant"):
    return await _cover(lang, tid)


@app.get("/api/panel/episode/{tid}/{no:int}")
async def panel_episode(tid: str, no: int, lang: str = "zh-hant"):
    """面板里“试转一话”：返回状态；转好后用 .cbz 地址下载。/ The panel's "try one episode": returns the state; download
    from the .cbz address once ready."""
    source._check(lang, tid)
    return jobs.status(lang, tid, no)


@app.get("/api/panel/episode/{tid}/{no:int}.cbz")
async def panel_episode_file(tid: str, no: int, lang: str = "zh-hant"):
    return _episode_file(lang, tid, no)


@app.get("/api/panel/settings")
async def panel_settings():
    cfg = config.load()
    return {k: cfg[k] for k in ("page_w", "page_h", "quality", "delay_min", "delay_max", "list_cache_min",
                                "keep_days", "max_mb")} | {"tunnel_mode": cfg["tunnel_mode"],
                                                            "device_token": cfg["device_token"]}


class Settings(BaseModel):
    page_w: int | None = None
    page_h: int | None = None
    quality: int | None = None
    delay_min: float | None = None
    delay_max: float | None = None
    list_cache_min: int | None = None
    keep_days: int | None = None
    max_mb: int | None = None


@app.post("/api/panel/settings")
async def panel_settings_set(body: Settings):
    up = {k: v for k, v in body.model_dump().items() if v is not None}
    if "page_w" in up and not 200 <= up["page_w"] <= 2000:
        raise HTTPException(400, "页面宽度要在 200～2000 之间")
    if "page_h" in up and not 200 <= up["page_h"] <= 3000:
        raise HTTPException(400, "页面高度要在 200～3000 之间")
    if "quality" in up and not 40 <= up["quality"] <= 95:
        raise HTTPException(400, "质量要在 40～95 之间")
    if up.get("delay_min", 0) < 0 or up.get("delay_max", 1) < up.get("delay_min", 0):
        raise HTTPException(400, "等待时间不对")
    config.save(up)
    return await panel_settings()


@app.post("/api/panel/cache/clear")
async def panel_cache_clear():
    import shutil
    for p in config.CACHE_DIR.iterdir():
        shutil.rmtree(p, ignore_errors=True) if p.is_dir() else p.unlink(missing_ok=True)
    return {"ok": True}


@app.post("/api/panel/device/rotate")
async def device_rotate():
    import secrets
    config.save({"device_token": secrets.token_urlsafe(24)})
    return await panel_settings()


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
    token = request.headers.get("x-webtoon-token") or request.query_params.get("token")
    if not auth.device_ok(token):
        raise HTTPException(401, "设备令牌不对")


@app.get("/api/device/v1/store")
async def device_store(request: Request, lang: str = "zh-hant", seg: str = "rank", key: str = "", zh: str = "s",
                       limit: int = 60):
    """发现页列表。seg：rank（key 为 trending / popular / originals）、genre（key 为类型）、day（key 为星期）、done。
    返回 keys 给设备画二级分段。/ A store list. seg: rank (key trending / popular / originals), genre (key a genre),
    day (key a weekday) or done; keys feed the device's second-level segment."""
    _device(request)
    return await _store(lang, seg, key, zh, limit)


@app.get("/api/device/v1/search")
async def device_search(request: Request, q: str, lang: str = "zh-hant", zh: str = "s"):
    _device(request)
    return await _search(lang, q, zh)


@app.get("/api/device/v1/title/{tid}")
async def device_title(request: Request, tid: str, lang: str = "zh-hant", zh: str = "s"):
    """作品详情和全部免费话（从旧到新）。/ Title details and every free episode, oldest first."""
    _device(request)
    return await _title(lang, tid, zh)


@app.get("/api/device/v1/cover/{tid}.jpg")
async def device_cover(request: Request, tid: str, lang: str = "zh-hant"):
    _device(request)
    return await _cover(lang, tid)


class CheckTitle(BaseModel):
    id: str
    last: int = 0


class CheckBody(BaseModel):
    lang: str = "zh-hant"
    zh: str = "s"
    titles: list[CheckTitle]


@app.post("/api/device/v1/check")
async def device_check(request: Request, body: CheckBody):
    """设备一次发来所有订阅（作品和已下载到第几话），返回每部比 last 新的免费话。服务器不记这些。
    The device sends every subscription (title and the last downloaded episode) at once; returns each title's free
    episodes newer than last. The server keeps none of it."""
    _device(request)
    conv = _simp(body.lang, body.zh)
    out = []
    for t in body.titles[:CHECK_MAX]:
        try:
            eps = await source.episodes(t.id) if source.ID_RE.match(t.id) else []
            new = [{"no": e["no"], "title": conv(e["title"])} for e in eps if e["no"] > t.last]
            out.append({"id": t.id, "latest": eps[-1]["no"] if eps else 0, "new": new})
        except source.SourceError as e:
            out.append({"id": t.id, "error": str(e), "new": []})
    return {"titles": out}


@app.get("/api/device/v1/episode/{tid}/{no:int}")
async def device_episode(request: Request, tid: str, no: int, lang: str = "zh-hant"):
    """一话的状态：ready / pending（done、total 是原图进度）/ error；没转过就开始转。设备隔几秒再问，转好后用 .cbz
    地址下载。分开问是因为隧道约 100 秒就断开请求。/ An episode's state: ready / pending (done and total count source
    images) / error; starts converting when needed. The device asks again every few seconds and downloads from the
    .cbz address once ready; split because tunnels drop a request after about 100 s."""
    _device(request)
    source._check(lang, tid)
    if no <= 0:
        raise HTTPException(400, "话号不对")
    return jobs.status(lang, tid, no)


def _episode_file(lang: str, tid: str, no: int) -> FileResponse:
    source._check(lang, tid)
    path = jobs.cbz_path(lang, tid, no)
    if not path.exists():
        raise HTTPException(404, "这一话还没转好")
    path.touch()                      # 记成最近用过 / Mark as recently used
    return FileResponse(path, media_type="application/vnd.comicbook+zip", filename=f"{tid}_{no}.cbz")


@app.get("/api/device/v1/episode/{tid}/{no:int}.cbz")
async def device_episode_file(request: Request, tid: str, no: int, lang: str = "zh-hant"):
    _device(request)
    return _episode_file(lang, tid, no)


# ---- 面板页面 / Panel pages ----

app.mount("/static", StaticFiles(directory=WEB), name="static")


@app.get("/")
async def index():
    return FileResponse(WEB / "index.html")
