"""Mihon 插件来源：经同一个 compose 里的 Suwayomi-Server（运行 Mihon / Tachiyomi 插件）读漫画。

hub 不加载 APK，只调 Suwayomi 的 GraphQL（/api/graphql，basic auth），图片也从 Suwayomi 内网取。作品编号是 “m” 加
Suwayomi 的漫画 id。设备按整数话号工作，所以 hub 给章节分配话号并记在 /data/mihon/chapters.sqlite：第一次见到一部
作品时按来源顺序从旧到新编 1、2、3…，以后新出现的章节接在最大话号后面，分配过的话号永远不变。这张表只是缓存，丢了
按同样规则重建，服务器仍然不保存订阅。

本项目不提供也不推荐任何插件仓库；仓库由用户在面板里自己添加，只应添加自己有权访问的来源。

Mihon extension sources: comics read through Suwayomi-Server (which runs Mihon / Tachiyomi extensions) in the same
compose project. The hub never loads APKs; it calls Suwayomi's GraphQL (/api/graphql, basic auth) and fetches images
from Suwayomi on the internal network. Title ids are "m" plus Suwayomi's manga id. Devices work with integer episode
numbers, so the hub assigns them and keeps them in /data/mihon/chapters.sqlite: the first time a title is seen its
chapters are numbered 1, 2, 3... oldest first in source order; chapters that show up later go after the highest
number, and an assigned number never changes. The table is only a cache, rebuilt by the same rule if lost; the server
still keeps no subscriptions.

This project ships and recommends no extension repository; users add their own in the panel and should only add
sources they have the right to access.
"""
from __future__ import annotations

import os
import re
import secrets
import sqlite3
import threading
import time
from pathlib import Path
from typing import Any

import httpx

from . import config
from .source import SourceError

URL = os.environ.get("MIHON_URL", "http://suwayomi:4567").rstrip("/")
USER = "hub"
DIR = config.DATA / "mihon"
SECRET = DIR / "suwayomi.secret"
DB = DIR / "chapters.sqlite"
ID_RE = re.compile(r"^m\d{1,9}$")
SRC_RE = re.compile(r"^-?\d{1,20}$")
LANG = "mihon"                       # 缓存路径和 CBZ 里用的“语言” / The "language" used in cache paths and CBZs
STORE_PAGES = 5                      # 发现页最多连翻几页 / Most source pages fetched for one store list
STORE_ITEMS = 60
DONE = {"COMPLETED", "PUBLISHING_FINISHED", "CANCELLED"}
SEGS = {"pop": "POPULAR", "latest": "LATEST"}

_client: httpx.AsyncClient | None = None
_alive: tuple[float, bool] = (float("-inf"), False)   # 还没查过 / Never checked
_cache: dict[str, tuple[float, Any]] = {}
_db_lock = threading.Lock()


# ---- 连接 / Connection ----

def password() -> str:
    """和 Suwayomi 共用的密码：第一次启动时生成，写在 /data/mihon/suwayomi.secret，Suwayomi 用 AUTH_PASSWORD_FILE
    读同一个文件。/ The password shared with Suwayomi: generated on first start into /data/mihon/suwayomi.secret, which
    Suwayomi reads through AUTH_PASSWORD_FILE."""
    if not SECRET.exists():
        DIR.mkdir(parents=True, exist_ok=True)
        tmp = SECRET.with_suffix(".tmp")
        tmp.write_text(secrets.token_urlsafe(24), encoding="utf-8")
        os.chmod(tmp, 0o644)        # Suwayomi 容器的用户要能读 / Suwayomi's container user must read it
        os.replace(tmp, SECRET)
    return SECRET.read_text(encoding="utf-8").strip()


def client() -> httpx.AsyncClient:
    global _client
    if _client is None:
        _client = httpx.AsyncClient(base_url=URL, auth=(USER, password()), timeout=httpx.Timeout(90, connect=5))
    return _client


def _short(msg: str) -> str:
    """Suwayomi 的错误带整段 Java 堆栈，只留第一行。/ Suwayomi errors carry a whole Java stack; keep the first line."""
    line = (msg or "").strip().splitlines()[0] if msg else ""
    return re.sub(r"^Exception while fetching data \([^)]*\) : ", "", line)[:200] or "插件服务出错"


async def gql(query: str, variables: dict | None = None, partial: bool = False) -> dict:
    """调一次 GraphQL；出错抛 SourceError。partial 为真时有数据就带着错误返回。/ One GraphQL call; raises SourceError on
    failure. With partial, data that came back with errors is still returned."""
    try:
        r = await client().post("/api/graphql", json={"query": query, "variables": variables or {}})
    except httpx.HTTPError as e:
        raise SourceError(502, f"连不上插件服务（{e.__class__.__name__}）") from None
    if r.status_code in (401, 403, 404) and "json" not in r.headers.get("content-type", ""):
        raise SourceError(502, "插件服务拒绝了 hub 的账号（密码文件不一致？）")
    if r.status_code >= 400 and "json" not in r.headers.get("content-type", ""):
        raise SourceError(502, f"插件服务返回 HTTP {r.status_code}")
    body = r.json()
    errors = body.get("errors") or []
    data = body.get("data") or {}
    if errors and not (partial and any(v for v in data.values())):
        raise SourceError(502, _short(errors[0].get("message", "")))
    if errors:
        data["_error"] = _short(errors[0].get("message", ""))
    return data


async def available() -> bool:
    """插件服务在不在（结果记 60 秒）。没开插件功能时 suwayomi 这个主机名不存在，很快就失败。/ Whether the extension
    service answers (remembered for 60 s). Without the plugin profile the suwayomi host does not exist and this fails
    fast."""
    global _alive
    now = time.monotonic()
    if now - _alive[0] < 60:
        return _alive[1]
    try:
        await gql("{ sources(first: 1) { totalCount } }")
        ok = True
    except SourceError:
        ok = False
    _alive = (now, ok)
    return ok


def _ttl() -> float:
    return config.load()["list_cache_min"] * 60


async def _cached(key: str, make, ttl: float | None = None):
    hit = _cache.get(key)
    if hit and time.monotonic() - hit[0] < (_ttl() if ttl is None else ttl):
        return hit[1]
    value = await make()
    if len(_cache) > 300:
        _cache.clear()
    _cache[key] = (time.monotonic(), value)
    return value


def forget(tid: str = "") -> None:
    for k in [k for k in _cache if not tid or k.endswith(":" + tid)]:
        del _cache[k]


# ---- 来源 / Sources ----

async def all_sources() -> list[dict]:
    """Suwayomi 里装好的全部来源（含本地来源 0）。/ Every installed source in Suwayomi (local source 0 included)."""
    d = await gql("{ sources { nodes { id name lang displayName supportsLatest contentWarning } } }")
    out = []
    for s in d["sources"]["nodes"]:
        local = str(s["id"]) == "0"
        out.append({"key": str(s["id"]), "name": "本地" if local else (s.get("displayName") or s["name"]),
                    "lang": "" if local else s.get("lang", ""), "latest": bool(s.get("supportsLatest")),
                    "nsfw": s.get("contentWarning") not in (None, "SAFE"), "local": local})
    return out


async def sources(for_device: bool) -> list[dict]:
    """设备看到的来源：只去掉用户在面板里自己勾掉的，按面板排的顺序。成人来源不另外过滤（nsfw 只是标记），由用户在
    客户端自己选。/ Sources a device sees: only those the user unticked in the panel are dropped, in the panel's
    order. Adult sources are not filtered (nsfw is only a flag); the user picks on the client."""
    cfg = config.load()
    items = await all_sources()
    hidden = set(cfg.get("mihon_hidden") or [])
    order = {k: i for i, k in enumerate(cfg.get("mihon_order") or [])}
    items.sort(key=lambda s: (order.get(s["key"], len(order)), s["name"]))
    if for_device:
        items = [s for s in items if s["key"] not in hidden]
    else:
        for s in items:
            s["hidden"] = s["key"] in hidden
    return items


def _card(m: dict, src_name: str) -> dict:
    return {"id": f"m{m['id']}", "title": m.get("title") or "", "author": m.get("author") or m.get("artist") or "",
            "genre": src_name, "likes": 0, "canvas": False, "adult": False, "src": src_name,
            "cover": "thumb" if m.get("thumbnailUrl") else ""}


async def _src_name(src: str) -> str:
    for s in await _cached("sources", all_sources, 300):
        if s["key"] == src:
            return s["name"]
    raise SourceError(404, "没有这个来源（插件可能卸载了）")


BROWSE = """mutation($src: LongString!, $type: FetchSourceMangaType!, $page: Int!, $q: String) {
  fetchSourceManga(input: {source: $src, type: $type, page: $page, query: $q}) {
    hasNextPage mangas { id title author artist thumbnailUrl } } }"""


async def _browse(src: str, kind: str, q: str = "") -> list[dict]:
    if not SRC_RE.match(src or ""):
        raise SourceError(400, "来源不对")
    name = await _src_name(src)
    items, seen = [], set()
    for page in range(1, STORE_PAGES + 1):
        d = await gql(BROWSE, {"src": src, "type": kind, "page": page, "q": q or None})
        r = d["fetchSourceManga"]
        for m in r["mangas"]:
            if m["id"] not in seen:
                seen.add(m["id"])
                items.append(_card(m, name))
        if not r["hasNextPage"] or len(items) >= STORE_ITEMS:
            break
    return items


async def store(src: str, seg: str) -> dict:
    seg = seg if seg in SEGS else "pop"
    items = await _cached(f"store:{src}:{seg}", lambda: _browse(src, SEGS[seg]))
    return {"seg": seg, "key": "", "keys": [], "items": items}


async def search(src: str, q: str) -> dict:
    q = (q or "").strip()[:40]
    if not q:
        raise SourceError(400, "没有关键词")
    items = await _cached(f"search:{src}:{q}", lambda: _browse(src, "SEARCH", q), 300)
    return {"q": q, "items": items}


# ---- 话号表 / Episode numbers ----

def _db() -> sqlite3.Connection:
    DIR.mkdir(parents=True, exist_ok=True)
    con = sqlite3.connect(DB)
    con.execute("""CREATE TABLE IF NOT EXISTS chapters (manga INTEGER, url TEXT, no INTEGER, chapter INTEGER,
                   name TEXT, number REAL, date INTEGER, PRIMARY KEY (manga, url))""")
    con.execute("""CREATE TABLE IF NOT EXISTS mangas (manga INTEGER PRIMARY KEY, source TEXT, url TEXT,
                   title TEXT)""")
    return con


def _num(c: dict) -> float:
    n = c.get("chapterNumber")
    return float(n) if n is not None else -1.0


def _oldest_first(chapters: list[dict]) -> list[dict]:
    """从旧到新排章节。先按来源顺序（Suwayomi 的 sourceOrder 一般是旧的小；话号随它变小就倒过来），再在大半章节有章节号
    时按章节号排：有的插件交出来的顺序是按章节名的字符串排的（第100话、第101话…第10话、第110话），不能全信。没有章节号
    的（番外之类）跟在来源顺序里它前面那一章后面；章节号相同的（不同汉化组）保持来源顺序。
    Chapters oldest first. Start from source order (Suwayomi's sourceOrder is normally smaller for older chapters; flip
    it when chapter numbers fall as it rises), then, when most chapters carry a number, sort by chapter number: some
    extensions hand chapters over sorted by name as strings (第100话, 第101话 … 第10话, 第110话), which cannot be
    trusted. Chapters without a number (extras and such) follow the chapter before them in source order; chapters with
    the same number (other scanlators) keep their source order."""
    cs = sorted(chapters, key=lambda c: c.get("sourceOrder") or 0)
    nums = [_num(c) for c in cs if _num(c) >= 0]
    if len(nums) >= 2:
        down = sum(1 for a, b in zip(nums, nums[1:]) if b < a)
        up = sum(1 for a, b in zip(nums, nums[1:]) if b > a)
        if down > up:
            cs.reverse()
    if len(nums) * 2 < len(cs):
        return cs
    keyed, last, step = [], -1.0, 0
    for c in cs:
        n = _num(c)
        if n >= 0:
            last, step = n, 0
            keyed.append((n, c))
        else:
            step += 1
            keyed.append((last + step * 1e-4, c))
    keyed.sort(key=lambda kc: kc[0])     # 稳定排序：同号保持来源顺序 / Stable: equal numbers keep source order
    return [c for _, c in keyed]


def assign(manga: int, chapters: list[dict], dedupe: bool = True) -> list[dict]:
    """给章节分配整数话号，返回从旧到新的 [{no,title,date,chapter}]。已分配的不变，新章节接在最大话号后面。dedupe 时
    同一话号数（chapterNumber）已有别的版本（别的汉化组）的新章节不列出（记成话号 0，以后也不会再分配）。
    Assign integer episode numbers and return [{no,title,date,chapter}] oldest first. Assigned numbers stay; new
    chapters go after the highest. With dedupe, a new chapter whose chapterNumber already has another version (another
    scanlator) is left out (stored as number 0 so it is never assigned later)."""
    with _db_lock:
        con = _db()
        try:
            known = {row[0]: row for row in con.execute(
                "SELECT url, no, chapter, name, number, date FROM chapters WHERE manga = ?", (manga,))}
            top = max([r[1] for r in known.values()], default=0)
            numbers = {r[4] for r in known.values() if r[1] > 0 and r[4] is not None and r[4] >= 0}
            for c in _oldest_first([c for c in chapters if c["url"] not in known]):
                num = _num(c)
                if dedupe and num >= 0 and num in numbers:
                    no = 0
                else:
                    top += 1
                    no = top
                    if num >= 0:
                        numbers.add(num)
                date = int(int(c.get("uploadDate") or 0) / 1000)
                con.execute("INSERT INTO chapters VALUES (?,?,?,?,?,?,?)",
                            (manga, c["url"], no, int(c["id"]), c.get("name") or "", num, date))
                known[c["url"]] = (c["url"], no, int(c["id"]), c.get("name") or "", num, date)
            # 章节 id、名字可能变（插件刷新），按 url 更新。/ Chapter ids and names may change on refresh; update by url.
            for c in chapters:
                row = known[c["url"]]
                if row[2] != int(c["id"]) or row[3] != (c.get("name") or ""):
                    con.execute("UPDATE chapters SET chapter = ?, name = ? WHERE manga = ? AND url = ?",
                                (int(c["id"]), c.get("name") or "", manga, c["url"]))
                    known[c["url"]] = (row[0], row[1], int(c["id"]), c.get("name") or "", row[4], row[5])
            con.commit()
        finally:
            con.close()
    current = {c["url"] for c in chapters}
    rows = sorted((r for r in known.values() if r[1] > 0 and r[0] in current), key=lambda r: r[1])
    return [{"no": r[1], "title": r[3] or f"#{r[1]}", "date": r[5], "chapter": r[2]} for r in rows]


def renumber(tid: str) -> int:
    """忘掉一部作品分配过的话号（编错了时用），下次取详情按现在的规则重新编。返回忘掉了几章。已经转好的这部的 CBZ
    由调用的一方删掉；设备上用旧话号下的话要取消订阅、删掉后重新订阅。
    Forget a title's assigned episode numbers (when they came out wrong) so the next detail fetch numbers them again by
    the current rules. Returns how many chapters were forgotten. The caller deletes this title's converted CBZs;
    episodes downloaded on devices under the old numbers need an unsubscribe, delete and fresh subscription."""
    mid = _mid(tid)
    with _db_lock:
        con = _db()
        try:
            n = con.execute("DELETE FROM chapters WHERE manga = ?", (mid,)).rowcount
            con.commit()
        finally:
            con.close()
    forget(tid)
    return n


def _check_identity(manga: dict) -> None:
    """Suwayomi 的数据库清空重建后，同一个 id 可能变成别的作品：记下来源和地址，对不上就报错，不下错的漫画。
    After Suwayomi's database is wiped the same id may become another title: remember source and URL and refuse on a
    mismatch rather than download the wrong comic."""
    with _db_lock:
        con = _db()
        try:
            row = con.execute("SELECT source, url FROM mangas WHERE manga = ?", (int(manga["id"]),)).fetchone()
            ident = (str(manga["sourceId"]), manga["url"])
            if row and tuple(row) != ident:
                raise SourceError(409, "插件服务的数据重置过，这个编号已经是别的作品了；请在设备上重新订阅")
            if not row:
                con.execute("INSERT INTO mangas VALUES (?,?,?,?)", (int(manga["id"]), *ident, manga.get("title")))
                con.commit()
        finally:
            con.close()


# ---- 作品与话 / Titles and episodes ----

DETAIL = """mutation($id: Int!) { fetchMangaAndChapters(input: {id: $id, fetchManga: true, fetchChapters: true}) {
  manga { id title author artist description genre status thumbnailUrl sourceId url realUrl source { displayName } }
  chapters { id name chapterNumber sourceOrder uploadDate url scanlator } } }"""


def _mid(tid: str) -> int:
    if not ID_RE.match(tid or ""):
        raise SourceError(400, "作品编号不对")
    return int(tid[1:])


async def _title(tid: str) -> dict:
    d = await gql(DETAIL, {"id": _mid(tid)}, partial=True)
    r = d.get("fetchMangaAndChapters") or {}
    m = r.get("manga")
    if not m:
        raise SourceError(404, "没有这部作品")
    _check_identity(m)
    eps = assign(int(m["id"]), r.get("chapters") or [], config.load().get("mihon_dedupe", True))
    try:
        src = await _src_name(str(m.get("sourceId")))     # 和列表里的来源名一样 / Same source name as the lists
    except SourceError:
        src = (m.get("source") or {}).get("displayName") or ""
    genre = "、".join((m.get("genre") or [])[:3])
    return {"id": tid, "title": m.get("title") or "", "author": m.get("author") or m.get("artist") or "",
            "genre": genre or src, "summary": (m.get("description") or "").strip(), "src": src,
            "done": (m.get("status") or "") in DONE, "cover": m.get("thumbnailUrl") or "",
            "web": m.get("realUrl") or "", "episodes": eps, "latest": eps[-1]["no"] if eps else 0,
            "error": d.get("_error", "")}


async def title(tid: str) -> dict:
    return await _cached(f"title:{tid}", lambda: _title(tid))


async def episodes(tid: str) -> list[dict]:
    return (await title(tid))["episodes"]


async def images(tid: str, no: int) -> list[str]:
    t = await title(tid)
    ep = next((e for e in t["episodes"] if e["no"] == int(no)), None)
    if not ep:
        forget(tid)            # 也许是刚出的新章节 / Maybe a chapter that just came out
        t = await title(tid)
        ep = next((e for e in t["episodes"] if e["no"] == int(no)), None)
    if not ep:
        raise SourceError(404, "没有这一话")
    d = await gql("mutation($c: Int!) { fetchChapterPages(input: {chapterId: $c}) { pages } }", {"c": ep["chapter"]})
    pages = d["fetchChapterPages"]["pages"] or []
    if not pages:
        raise SourceError(502, "这一话没有图片")
    return pages


async def get_image(url: str, pace: bool = True) -> bytes:
    """从 Suwayomi 取一张图（相对地址，带 basic auth）；节流交给插件自己。/ Fetch an image from Suwayomi (relative URL
    with basic auth); extensions do their own rate limiting."""
    try:
        r = await client().get(url)
    except httpx.HTTPError as e:
        raise SourceError(502, f"取图失败（{e.__class__.__name__}）") from None
    if r.status_code != 200:
        raise SourceError(502, f"取图失败（HTTP {r.status_code}）")
    return r.content


def cover_url(tid: str) -> str:
    return f"/api/v1/manga/{_mid(tid)}/thumbnail"


# ---- 插件管理（面板）/ Extension management (panel) ----

EXT_FIELDS = "pkgName name lang versionName isInstalled hasUpdate isObsolete contentWarning iconUrl storeIndexUrl"


async def extensions() -> dict:
    d = await gql(f"mutation {{ fetchExtensions(input: {{}}) {{ extensions {{ {EXT_FIELDS} }} "
                  "extensionStores { indexUrl name } } }")
    r = d["fetchExtensions"]
    forget()
    return {"extensions": r["extensions"] or [], "stores": r["extensionStores"] or []}


async def store_add(url: str) -> None:
    url = (url or "").strip()
    if not re.match(r"^https?://\S{4,500}$", url):
        raise SourceError(400, "仓库地址要以 http:// 或 https:// 开头")
    await gql("mutation($u: String!) { addExtensionStore(input: {indexUrl: $u}) { extensionStore { indexUrl } } }",
              {"u": url})


async def store_remove(url: str) -> None:
    await gql("mutation($u: String!) { removeExtensionStore(input: {indexUrl: $u}) { clientMutationId } }",
              {"u": url})


async def extension_do(pkg: str, action: str) -> None:
    if action not in ("install", "update", "uninstall") or not re.match(r"^[\w.]{3,200}$", pkg or ""):
        raise SourceError(400, "操作不对")
    await gql("mutation($p: String!, $patch: UpdateExtensionPatchInput!) { updateExtension(input: {id: $p, patch: "
              "$patch}) { clientMutationId } }", {"p": pkg, "patch": {action: True}})
    forget()
