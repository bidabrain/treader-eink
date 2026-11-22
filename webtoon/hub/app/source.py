"""从 webtoons.com 读公开页面：发现页列表、搜索、作品详情、免费话数、每一话的图片地址。

只读网页上公开免费的内容，不登录、不碰付费或抢先看的话。请求同时最多 2 个，列表和详情缓存一阵，图片之间随机等待。
网页改版时只需要改这个文件。

Read public pages from webtoons.com: store lists, search, title details, free episodes and each episode's image URLs.
Only content that is free and public on the web is read: no sign-in, no paid or early-access episodes. At most two
requests run at once, lists and details are cached for a while, and image downloads are spaced out. A site redesign
only touches this file.

编号：官方作品就是 title_no（如 "8420"），业余 Canvas 作品前面加 c（如 "c1060813"）。
Ids: official titles are the title_no ("8420"); amateur Canvas titles get a c prefix ("c1060813").
"""
from __future__ import annotations

import asyncio
import html
import random
import re
import time
from typing import Any, Callable

import httpx

from . import config

UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
      "Chrome/131.0 Safari/537.36")
REFERER = "https://www.webtoons.com/"
ID_RE = re.compile(r"^c?\d{1,10}$")
LANG_RE = re.compile(r"^[a-z]{2}(-[a-z]{4})?$")
LANGS = {"zh-hant": "繁體中文", "en": "English", "es": "Español", "fr": "Français", "de": "Deutsch",
         "th": "ภาษาไทย", "id": "Indonesia"}
RANKS = [("trending", "趋势"), ("popular", "人气"), ("originals", "原创")]
DAYS = [("monday", "周一"), ("tuesday", "周二"), ("wednesday", "周三"), ("thursday", "周四"), ("friday", "周五"),
        ("saturday", "周六"), ("sunday", "周日")]


class SourceError(Exception):
    def __init__(self, status: int, msg: str):
        super().__init__(msg)
        self.status = status


_sem = asyncio.Semaphore(2)
_cache: dict[str, tuple[float, Any]] = {}
CACHE_MAX = 300
_client: httpx.AsyncClient | None = None


def client() -> httpx.AsyncClient:
    global _client
    if _client is None:
        _client = httpx.AsyncClient(timeout=25.0, follow_redirects=True,
                                    headers={"User-Agent": UA, "Accept-Language": "zh-TW,zh;q=0.9,en;q=0.8"})
    return _client


def _remember(url: str, value: Any) -> None:
    """存进缓存：先扔掉过期的，还满就扔最早存的。/ Cache a value: drop expired entries first, then the oldest while
    still full."""
    now = time.time()
    longest = max(config.load()["list_cache_min"] * 60, 600)
    for k in [k for k, (t, _) in _cache.items() if now - t > longest]:
        del _cache[k]
    while len(_cache) >= CACHE_MAX:
        del _cache[next(iter(_cache))]
    _cache[url] = (now, value)


async def _get(url: str, *, json_body: bool = False, referer: bool = False, ttl: float | None = None,
               parse: Callable[[Any], Any] | None = None) -> Any:
    """取一个地址；ttl 秒内同一地址直接用缓存。parse 把网页解析成小结果，缓存里只存解析结果，不存整页。
    Fetch a URL; within ttl seconds the cached copy is reused. parse turns the page into a small result, and only that
    result is cached, not the whole page."""
    if ttl is None:
        ttl = config.load()["list_cache_min"] * 60
    hit = _cache.get(url)
    if hit and time.time() - hit[0] < ttl:
        return hit[1]
    async with _sem:
        try:
            r = await client().get(url, headers={"Referer": REFERER} if referer else None)
        except httpx.HTTPError as e:
            raise SourceError(502, f"连不上 WEBTOON：{e.__class__.__name__}") from e
    if r.status_code == 404:
        raise SourceError(404, "WEBTOON 上没有这个页面")
    if r.status_code >= 400:
        raise SourceError(502, f"WEBTOON 返回 HTTP {r.status_code}")
    value = r.json() if json_body else r.text
    if parse:
        value = parse(value)
    if ttl > 0:
        _remember(url, value)
    return value


async def get_image(url: str, pace: bool = True) -> bytes:
    """下载一张图（带 Referer，图片服务器没有它会拒绝）。pace 时先随机等一下（一话的原图用；封面很小不等）。
    Download an image (with the Referer, which the image server requires). With pace, wait a random moment first
    (for an episode's source images; small covers do not wait)."""
    cfg = config.load()
    if pace:
        await asyncio.sleep(random.uniform(cfg["delay_min"], cfg["delay_max"]))
    async with _sem:
        try:
            r = await client().get(url, headers={"Referer": REFERER})
        except httpx.HTTPError as e:
            raise SourceError(502, f"图片下载失败：{e.__class__.__name__}") from e
    if r.status_code >= 400:
        raise SourceError(502, f"图片下载失败：HTTP {r.status_code}")
    return r.content


# ---- 简繁 / Simplified and traditional Chinese ----

_cc: dict[str, Any] = {}


def _opencc(kind: str):
    if kind not in _cc:
        try:
            from opencc import OpenCC  # opencc-python-reimplemented
            _cc[kind] = OpenCC(kind)
        except Exception:  # noqa: BLE001  没装就不转 / Not installed: no conversion
            _cc[kind] = None
    return _cc[kind]


def to_trad(s: str) -> str:
    cc = _opencc("s2t")
    return cc.convert(s) if cc and s else s


def to_simp(s: str) -> str:
    cc = _opencc("t2s")
    return cc.convert(s) if cc and s else s


# ---- 解析 / Parsing ----

def _text(s: str) -> str:
    s = re.sub(r"<br\s*/?>", "\n", s or "")
    s = re.sub(r"<[^>]+>", "", s)
    s = html.unescape(s)
    return "\n".join(" ".join(line.split()) for line in s.split("\n")).strip()


def _first(pattern: str, s: str) -> str:
    m = re.search(pattern, s, re.S)
    return _text(m.group(1)) if m else ""


def _num(s: str) -> int:
    """“95,421”“1.2M”“3.4萬”这类点赞数变成整数。/ Turn like counts such as "95,421", "1.2M" or "3.4萬" into ints."""
    s = (s or "").replace(",", "").strip()
    m = re.match(r"([\d.]+)\s*([KkMmBb萬万億亿]?)", s)
    if not m:
        return 0
    try:
        v = float(m.group(1))
    except ValueError:
        return 0
    mult = {"k": 1e3, "m": 1e6, "b": 1e9, "萬": 1e4, "万": 1e4, "億": 1e8, "亿": 1e8}.get(m.group(2).lower(), 1)
    return int(v * mult)


_CARD_RE = re.compile(r'<a\s+href="([^"]*?/list\?title_no=(\d+)[^"]*)"([^>]*)>(.*?)</a>', re.S)


def parse_cards(page: str) -> list[dict]:
    """列表页里的作品卡片（排行、类型、星期、完结、搜索通用）。/ Title cards on a list page (rankings, genres,
    weekdays, completed and search share the markup)."""
    out: list[dict] = []
    seen: set[str] = set()
    for href, no, attrs, body in _CARD_RE.findall(page):
        if "_episode" in attrs or "detail_list" in attrs:
            continue
        canvas = "/canvas/" in href or 'data-webtoon-type="CHALLENGE"' in attrs
        tid = ("c" if canvas else "") + no
        if tid in seen:
            continue
        title = _first(r'<strong class="title">(.*?)</strong>', body) or _first(r'<p class="subj">(.*?)</p>', body)
        if not title:
            continue
        seen.add(tid)
        out.append({
            "id": tid,
            "title": title,
            "author": _first(r'<div class="author">(.*?)</div>', body),
            "genre": _first(r'<div class="genre">(.*?)</div>', body),
            "likes": _num(_first(r'<div class="view_count[^"]*">(.*?)</div>', body)),
            "cover": (re.search(r'<img[^>]+src="([^"]+)"', body) or [None, ""])[1],
            "canvas": canvas,
            "adult": 'data-title-unsuitable-for-children="true"' in body,
        })
    return out


def parse_genres(page: str) -> list[dict]:
    found = re.findall(r'href="[^"]*/genres/([a-z_]+)\?sortOrder=[A-Z]+"[^>]*data-genre="[A-Z_]+"[^>]*>(.*?)</a>',
                       page, re.S)
    out, seen = [], set()
    for key, name in found:
        if key in seen:
            continue
        seen.add(key)
        out.append({"key": key, "name": _text(name)})
    return out


def parse_title(page: str) -> dict:
    author = _first(r'<div class="author_area">(.*?)<button', page) or _first(r'<h2 class="author">(.*?)</h2>', page)
    author = re.sub(r"\s*,\s*", "、", author.replace("\n", " ")).strip("、 ")
    return {
        "title": _first(r'<h1 class="subj"[^>]*>(.*?)</h1>', page) or _first(r'<h3 class="subj[^"]*"[^>]*>(.*?)</h3>', page),
        "summary": _first(r'<p class="summary">(.*?)</p>', page),
        "author": author,
        "genre": _first(r'<h2 class="genre[^"]*">(.*?)</h2>', page),
        "cover": (re.search(r'<meta property="og:image" content="([^"]+)"', page) or [None, ""])[1],
        "done": "ico_completed" in page or "完結" in _first(r'<p class="day_info">(.*?)</p>', page)
                or "COMPLETED" in _first(r'<p class="day_info">(.*?)</p>', page).upper(),
    }


def parse_images(page: str) -> list[str]:
    urls = re.findall(r'<img[^>]*class="_images[^"]*"[^>]*data-url="([^"]+)"', page)
    urls += [u for u in re.findall(r'<img[^>]*data-url="([^"]+)"[^>]*class="_images', page) if u not in urls]
    return [html.unescape(u) for u in urls]


# ---- 读取 / Reading ----

def _check(lang: str, tid: str | None = None) -> None:
    if not LANG_RE.match(lang or ""):
        raise SourceError(400, "语言不对")
    if tid is not None and not ID_RE.match(tid):
        raise SourceError(400, "作品编号不对")


async def store(lang: str, seg: str, key: str = "") -> dict:
    """发现页列表：seg 是 rank / genre / day / done，key 是榜单、类型或星期。/ A store list: seg is rank / genre / day /
    done; key is the ranking, genre or weekday."""
    _check(lang)
    keys: list[dict] = []
    if seg == "rank":
        keys = [{"key": k, "name": n} for k, n in RANKS]
        key = key if key in dict(RANKS) else "trending"
        url = f"{config.BASE}/{lang}/ranking/{key}"
    elif seg == "genre":
        keys = await _get(f"{config.BASE}/{lang}/genres", parse=parse_genres)
        if not keys:
            raise SourceError(502, "读不到类型列表（网页可能改版了）")
        key = key if any(k["key"] == key for k in keys) else keys[0]["key"]
        url = f"{config.BASE}/{lang}/genres/{key}?sortOrder=MANA"
    elif seg == "day":
        keys = [{"key": k, "name": n} for k, n in DAYS]
        key = key if key in dict(DAYS) else DAYS[time.localtime().tm_wday][0]
        url = f"{config.BASE}/{lang}/originals/{key}?sortOrder=MANA"
    elif seg == "done":
        url = f"{config.BASE}/{lang}/originals/complete?sortOrder=MANA"
    else:
        raise SourceError(400, "分段不对")
    items = [c for c in await _get(url, parse=parse_cards) if not c["canvas"]]
    return {"seg": seg, "key": key, "keys": keys, "items": items}


async def search(lang: str, q: str) -> dict:
    _check(lang)
    q = (q or "").strip()[:40]
    if not q:
        raise SourceError(400, "没有关键词")
    # 中文版是繁体：拼音输入法打出的简体先转成繁体再搜。/ The Chinese site is traditional: convert simplified input
    # from the pinyin keyboard before searching.
    query = to_trad(q) if lang == "zh-hant" else q
    items = await _get(f"{config.BASE}/{lang}/search?keyword={httpx.QueryParams({'k': query})['k']}", ttl=300,
                       parse=parse_cards)
    items = sorted(items, key=lambda c: c["canvas"])   # 官方作品排前面 / Official titles first
    return {"q": q, "items": items[:60]}


async def episodes(tid: str) -> list[dict]:
    """全部免费公开的话（从旧到新）。/ Every free public episode, oldest first."""
    kind = "canvas" if tid.startswith("c") else "webtoon"
    return await _get(f"{config.MOBILE}/api/v1/{kind}/{tid.lstrip('c')}/episodes?pageSize=99999", json_body=True,
                      ttl=600, parse=_parse_episodes)


def _parse_episodes(data: dict) -> list[dict]:
    out = []
    for e in (data.get("result") or {}).get("episodeList") or []:
        no = int(e.get("episodeNo") or 0)
        if no <= 0:
            continue
        out.append({"no": no, "title": _text(e.get("episodeTitle") or f"#{no}"),
                    "date": int((e.get("exposureDateMillis") or 0) / 1000)})
    out.sort(key=lambda e: e["no"])
    return out


def _list_url(lang: str, tid: str) -> str:
    if tid.startswith("c"):
        return f"{config.BASE}/{lang}/canvas/x/list?title_no={tid[1:]}"
    return f"{config.BASE}/{lang}/x/x/list?title_no={tid}"


async def title(lang: str, tid: str) -> dict:
    _check(lang, tid)
    info = dict(await _get(_list_url(lang, tid), parse=parse_title))
    if not info["title"]:
        raise SourceError(404, "没有这部作品")
    eps = await episodes(tid)
    return {"id": tid, **info, "episodes": eps, "latest": eps[-1]["no"] if eps else 0}


async def images(lang: str, tid: str, no: int) -> list[str]:
    _check(lang, tid)
    if tid.startswith("c"):
        url = f"{config.BASE}/{lang}/canvas/x/x/viewer?title_no={tid[1:]}&episode_no={int(no)}"
    else:
        url = f"{config.BASE}/{lang}/x/x/x/viewer?title_no={tid}&episode_no={int(no)}"
    urls = await _get(url, ttl=600, parse=parse_images)
    if not urls:
        raise SourceError(403, "这一话不能免费看，或者网页改版了")
    return urls
