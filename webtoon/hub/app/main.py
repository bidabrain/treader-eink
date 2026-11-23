"""webtoon hub：网页面板和小纸 Pico 设备接口。服务器不保存订阅：订阅在设备的 TF 卡上，设备来问“有没有新话”。

webtoon hub: web panel and the Read Pico device API. The server keeps no subscriptions: they live on the device's TF
card and the device asks whether there are new episodes.

面板只在局域网用：从 Cloudflare 隧道进来的请求（带 CF-Connecting-IP）只能访问 /api/device/。
The panel is LAN only: requests arriving through Cloudflare (CF-Connecting-IP) may only reach /api/device/.

只拉网页上公开免费的话；自己部署、只给自己用，不提供公共服务。开了插件功能时，编号以 m 开头的作品走 mihon.py
（Suwayomi 运行的 Mihon 插件）；以 me 开头的走 ehentai.py（面板里打开的 E-Hentai 通道）。
Only free public episodes are fetched; self-hosted for personal use, never a public service. With the plugin feature
on, titles whose id starts with m go through mihon.py (Mihon extensions run by Suwayomi); ids starting with me go
through ehentai.py (the E-Hentai channel switched on in the panel).
"""
from __future__ import annotations

import asyncio
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI, HTTPException, Request, Response
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from . import auth, config, convert, ehentai, jobs, mihon, source, tunnel

WEB = Path(__file__).parent / "web"
LIST_MAX = 120
CHECK_MAX = 200


@asynccontextmanager
async def lifespan(app: FastAPI):
    config.ensure_dirs()
    config.load()
    mihon.password()        # Suwayomi 启动前密码文件要在 / The password file must exist before Suwayomi starts
    tasks =[asyncio.create_task(jobs.worker()), asyncio.create_task(jobs.pruner())]
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

WEBTOON = "webtoon"     # 来源里 WEBTOON 自己的 key / WEBTOON's own key among the sources


def _check(lang: str, tid: str) -> str:
    """检查编号和语言，返回实际用的语言（插件作品一律是 mihon，设备发来的 lang 不管）。/ Validate the id and language
    and return the language actually used (always mihon for extension titles, whatever the device sent)."""
    if jobs.is_eh(tid):
        ehentai.split_id(tid)
        return ehentai.LANG
    if jobs.is_mihon(tid):
        mihon._mid(tid)
        return mihon.LANG
    source._check(lang, tid)
    return lang

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


def _eh_on() -> None:
    if not ehentai.on():
        raise HTTPException(404, "E-Hentai 通道没打开（面板 → E-Hentai）")


async def _store(lang: str, seg: str, key: str, zh: str, limit: int, src: str = WEBTOON) -> dict:
    if src == ehentai.KEY:
        _eh_on()
        r = await ehentai.store(seg)
        return {"seg": r["seg"], "key": "", "keys": [], "src": src, "items": _mcards(r["items"], limit)}
    if src != WEBTOON:
        r = await mihon.store(src, seg)
        return {"seg": r["seg"], "key": "", "keys": [], "src": src, "items": _mcards(r["items"], limit)}
    r = await source.store(lang, seg, key)
    conv = _simp(lang, zh)
    return {"seg": r["seg"], "key": r["key"], "keys": [{"key": k["key"], "name": conv(k["name"])} for k in r["keys"]],
            "items": _cards(lang, r["items"], zh, limit)}


def _mcards(items: list[dict], limit: int) -> list[dict]:
    return [{k: c[k] for k in ("id", "title", "author", "genre", "likes", "canvas", "adult", "src")}
            for c in items[:max(1, min(limit, LIST_MAX))]]


async def _search(lang: str, q: str, zh: str, src: str = WEBTOON) -> dict:
    if src == ehentai.KEY:
        _eh_on()
        r = await ehentai.search(q)
        return {"q": r["q"], "src": src, "items": _mcards(r["items"], LIST_MAX)}
    if src != WEBTOON:
        r = await mihon.search(src, q)
        return {"q": r["q"], "src": src, "items": _mcards(r["items"], LIST_MAX)}
    r = await source.search(lang, q)
    return {"q": r["q"], "items": _cards(lang, r["items"], zh, LIST_MAX)}


async def _title(lang: str, tid: str, zh: str) -> dict:
    if jobs.is_ext(tid):
        _check(lang, tid)
        t = await (ehentai.title(tid) if jobs.is_eh(tid) else mihon.title(tid))
        return {k: t[k] for k in ("id", "title", "author", "genre", "summary", "done", "latest", "src", "error")} | {
            "episodes": [{"no": e["no"], "title": e["title"], "date": e["date"]} for e in t["episodes"]]}
    t = await source.title(lang, tid)
    conv = _simp(lang, zh)
    _remember_cover(lang, tid, t["cover"])
    return {"id": tid, "title": conv(t["title"]), "author": conv(t["author"]), "genre": conv(t["genre"]),
            "summary": conv(t["summary"]), "done": t["done"], "latest": t["latest"],
            "episodes": [{"no": e["no"], "title": conv(e["title"]), "date": e["date"]} for e in t["episodes"]]}


async def _cover(lang: str, tid: str) -> FileResponse:
    if jobs.is_eh(tid):
        lang = _check(lang, tid)
        try:
            url = "" if jobs.cover_path(lang, tid).exists() else await ehentai.cover_url(tid)
        except source.SourceError:
            url = ""
    elif jobs.is_mihon(tid):
        lang, url = _check(lang, tid), mihon.cover_url(tid)
    elif not source.ID_RE.match(tid) or not source.LANG_RE.match(lang):
        raise HTTPException(400, "路径不对")
    else:
        url = _covers.get((lang, tid), "")
    if not url and not jobs.is_ext(tid) and not jobs.cover_path(lang, tid).exists():
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
async def panel_store(lang: str = "zh-hant", seg: str = "rank", key: str = "", zh: str = "s", src: str = WEBTOON):
    return await _store(lang, seg, key, zh, LIST_MAX, src)


@app.get("/api/panel/search")
async def panel_search(q: str, lang: str = "zh-hant", zh: str = "s", src: str = WEBTOON):
    return await _search(lang, q, zh, src)


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
    return jobs.status(_check(lang, tid), tid, no)


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


# ---- 面板：插件 / Panel: extensions ----

async def _sources(for_device: bool) -> dict:
    items = [{"key": WEBTOON, "name": "WEBTOON", "lang": "", "latest": False, "nsfw": False}]
    if ehentai.on():
        items.append({"key": ehentai.KEY, "name": ehentai.name(), "lang": "", "latest": True, "nsfw": True})
    on = await mihon.available()
    if on:
        items += await mihon.sources(for_device)
    return {"mihon": on, "items": items}


@app.get("/api/panel/mihon")
async def panel_mihon():
    cfg = config.load()
    return await _sources(False) | {k: cfg[k] for k in ("mihon_dedupe", "mihon_spread")}


@app.get("/api/panel/mihon/extensions")
async def panel_mihon_extensions():
    return await mihon.extensions()


class StoreBody(BaseModel):
    url: str
    remove: bool = False


@app.post("/api/panel/mihon/store")
async def panel_mihon_store(body: StoreBody):
    await (mihon.store_remove(body.url) if body.remove else mihon.store_add(body.url))
    return await mihon.extensions()


class ExtBody(BaseModel):
    pkg: str
    action: str


@app.post("/api/panel/mihon/extension")
async def panel_mihon_extension(body: ExtBody):
    await mihon.extension_do(body.pkg, body.action)
    return await mihon.extensions()


class MihonSettings(BaseModel):
    mihon_hidden: list[str] | None = None
    mihon_order: list[str] | None = None
    mihon_dedupe: bool | None = None
    mihon_spread: str | None = None


@app.post("/api/panel/mihon/settings")
async def panel_mihon_settings(body: MihonSettings):
    up = {k: v for k, v in body.model_dump().items() if v is not None}
    if "mihon_spread" in up and up["mihon_spread"] not in convert.SPREADS:
        raise HTTPException(400, "跨页方式不对")
    for k in ("mihon_hidden", "mihon_order"):
        if k in up:
            up[k] = [x for x in up[k] if mihon.SRC_RE.match(x)][:500]
    config.save(up)
    return await panel_mihon()


@app.post("/api/panel/mihon/renumber/{tid}")
async def panel_mihon_renumber(tid: str):
    """重新编号一部插件作品：忘掉它的话号、删掉这部转好的 CBZ，再按现在的规则编。/ Renumber one extension title: forget
    its episode numbers, delete its converted CBZs, and number it again by the current rules."""
    import shutil
    if not jobs.is_mihon(tid):
        raise HTTPException(400, "只有插件作品能重新编号")
    n = mihon.renumber(tid)
    shutil.rmtree(jobs.cbz_path(mihon.LANG, tid, 1).parent, ignore_errors=True)
    t = await _title(mihon.LANG, tid, "t")
    return {"forgot": n, "title": t}


# ---- 面板：E-Hentai / Panel: E-Hentai ----

EH_COOKIES = ("eh_member_id", "eh_pass_hash", "eh_igneous")


@app.get("/api/panel/eh")
async def panel_eh():
    """E-Hentai 设置；cookie 只告诉有没有填，不回显。/ E-Hentai settings; cookies only report whether they are set,
    never their values."""
    cfg = config.load()
    return {k: cfg[k] for k in ("eh_on", "eh_site", "eh_direct", "eh_filter", "eh_cats", "eh_jpn_title", "eh_chunk",
                                "eh_spread")} | {"signed_in": bool(cfg["eh_member_id"] and cfg["eh_pass_hash"]),
                                                 "igneous": bool(cfg["eh_igneous"]), "name": ehentai.name(),
                                                 "cats": list(ehentai.CATS.items())}


class EhSettings(BaseModel):
    eh_on: bool | None = None
    eh_site: str | None = None
    eh_direct: bool | None = None
    eh_member_id: str | None = None
    eh_pass_hash: str | None = None
    eh_igneous: str | None = None
    eh_filter: str | None = None
    eh_cats: int | None = None
    eh_jpn_title: bool | None = None
    eh_chunk: int | None = None
    eh_spread: str | None = None


@app.post("/api/panel/eh")
async def panel_eh_set(body: EhSettings):
    """没给的不动；cookie 给空字符串就是清掉。/ Missing fields stay; an empty string clears a cookie."""
    up = {k: v for k, v in body.model_dump().items() if v is not None}
    if "eh_site" in up and up["eh_site"] not in ("e", "ex"):
        raise HTTPException(400, "站点不对")
    if "eh_spread" in up and up["eh_spread"] not in convert.SPREADS:
        raise HTTPException(400, "跨页方式不对")
    if "eh_chunk" in up and not 20 <= up["eh_chunk"] <= 500:
        raise HTTPException(400, "一话张数要在 20～500 之间")
    if "eh_cats" in up and not 0 <= up["eh_cats"] <= 1023:
        raise HTTPException(400, "分类不对")
    if "eh_filter" in up:
        up["eh_filter"] = up["eh_filter"].strip()[:200]
    for k in EH_COOKIES:
        if k in up:
            up[k] = "".join(ch for ch in up[k] if ch.isascii() and ch.isalnum())[:100]
    # 换了账号又没给 igneous：旧的 igneous 属于旧账号，清掉让 ExHentai 重新发。/ A new account without igneous: the
    # old igneous belongs to the old account, so clear it and let ExHentai hand out a new one.
    if up.get("eh_member_id") and "eh_igneous" not in up and up["eh_member_id"] != config.load()["eh_member_id"]:
        up["eh_igneous"] = ""
    config.save(up)
    ehentai.forget()          # 站点、过滤、标题语言变了，列表要重新取 / Site, filter or title language changed
    out = await panel_eh()
    if any(up.get(k) for k in EH_COOKIES):
        # 填了 cookie 就当场试一次，告诉面板能不能用。/ Freshly entered cookies are tried at once.
        out["check"] = await ehentai.verify()
        out |= {"igneous": bool(config.load()["eh_igneous"])}
    return out


# ---- 设备接口 / Device API ----

def _device(request: Request) -> None:
    token = request.headers.get("x-webtoon-token") or request.query_params.get("token")
    if not auth.device_ok(token):
        raise HTTPException(401, "设备令牌不对")


@app.get("/api/device/v1/sources")
async def device_sources(request: Request):
    """可选的来源：WEBTOON 总在第一个；开了插件功能时后面是面板里没隐藏的插件来源。/ Available sources: WEBTOON
    always first, then the extension sources not hidden in the panel when the plugin feature is on."""
    _device(request)
    return await _sources(True)


@app.get("/api/device/v1/store")
async def device_store(request: Request, lang: str = "zh-hant", seg: str = "rank", key: str = "", zh: str = "s",
                       limit: int = 60, src: str = WEBTOON):
    """发现页列表。seg：rank（key 为 trending / popular / originals）、genre（key 为类型）、day（key 为星期）、done。
    返回 keys 给设备画二级分段。/ A store list. seg: rank (key trending / popular / originals), genre (key a genre),
    day (key a weekday) or done; keys feed the device's second-level segment. src 是插件来源时 seg 是 pop / latest。
    With an extension source in src, seg is pop / latest."""
    _device(request)
    return await _store(lang, seg, key, zh, limit, src)


@app.get("/api/device/v1/search")
async def device_search(request: Request, q: str, lang: str = "zh-hant", zh: str = "s", src: str = WEBTOON):
    _device(request)
    return await _search(lang, q, zh, src)


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
            if jobs.is_eh(t.id):
                eps, cv = await ehentai.episodes(t.id), (lambda s: s)
            elif mihon.ID_RE.match(t.id):
                mihon.forget(t.id)       # 检查更新要最新的章节表 / An update check needs the current chapter list
                eps, cv = await mihon.episodes(t.id), (lambda s: s)
            else:
                eps, cv = (await source.episodes(t.id) if source.ID_RE.match(t.id) else []), conv
            new = [{"no": e["no"], "title": cv(e["title"])} for e in eps if e["no"] > t.last]
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
    lang = _check(lang, tid)
    if no <= 0:
        raise HTTPException(400, "话号不对")
    return jobs.status(lang, tid, no)


def _episode_file(lang: str, tid: str, no: int) -> FileResponse:
    lang = _check(lang, tid)
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
