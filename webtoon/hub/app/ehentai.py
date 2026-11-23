"""E-Hentai / ExHentai 通道：hub 自己读 e-hentai.org（或带登录 cookie 读 exhentai.org），不经过 Suwayomi 插件。

做法照 EhViewer：列表页只取画廊链接（gid + token），标题、分类、标签、页数、封面统一用官方 JSON 接口 gdata 取，不受网页
显示模式影响；图片先翻画廊详情页拿每张的查看页（/s/<key>/<gid>-<页>），再从查看页取图片地址；图片服务器失败时用 nl
换一台重试。一部画廊按 chunk 张一话切成几话，免得一话太大（设备一话最多等 6 分钟、CBZ 最大 60 MB）。

作品编号是 “me” 加 5 位 36 进制 gid 和 8 位 36 进制 token（共 15 个字母数字，设备的编号最长 15 个字符）；以 m 开头，
设备和 App 当它是插件作品，不用改客户端。编号里带着 token，服务器不用记任何映射。

只给自己用：登录 cookie 只存在服务器的 /data/config.json，面板不回显。请遵守站点条款，别调小请求间隔。

E-Hentai / ExHentai channel: the hub reads e-hentai.org itself (or exhentai.org with sign-in cookies), without the
Suwayomi extensions. Following EhViewer, list pages only yield gallery links (gid + token); title, category, tags, page
count and cover all come from the official gdata JSON API, independent of the site's display mode. Images come from the
gallery pages' viewer links (/s/<key>/<gid>-<page>), then the image URL on each viewer page; a failed image server is
retried once on another via nl. A gallery is split into episodes of chunk images so no episode gets too big (a device
waits at most 6 minutes per episode and takes CBZs up to 60 MB).

Title ids are "me" plus the gid in 5 base-36 digits and the token in 8 (15 alphanumerics; device ids hold at most 15).
They start with m, so devices and the app treat them as extension titles with no client change. The token rides in
the id, so the server keeps no mapping.

Personal use only: sign-in cookies stay in the server's /data/config.json and the panel never shows them back. Follow
the site's terms and do not shorten the request delays.
"""
from __future__ import annotations

import asyncio
import html
import math
import random
import re
import time
from typing import Any

import httpx

from . import config
from .source import UA, SourceError

KEY = "ehentai"                      # 来源 key（设备记住的来源）/ Source key (the device remembers it)
LANG = "ehentai"                     # 缓存路径用的“语言” / The "language" used in cache paths
API = "https://api.e-hentai.org/api.php"
ID_RE = re.compile(r"^me[0-9a-z]{13}$")
GALLERY_RE = re.compile(r"https?://(?:e-hentai|exhentai)\.org/g/(\d+)/([0-9a-f]{10})")
PAGE_RE = re.compile(r"https?://(?:e-hentai|exhentai)\.org/s/([0-9a-f]{10})/(\d+)-(\d+)")
NEXT_RE = re.compile(r"[?&;]next=(\d+)")
STORE_PAGES = 3
STORE_ITEMS = 60
SEGS = ("pop", "latest")
# 分类：名字 → f_cats 位（排除哪些就把位加起来）。/ Categories: name -> f_cats bit (excluded ones are summed).
CATS = {"Misc": 1, "Doujinshi": 2, "Manga": 4, "Artist CG": 8, "Game CG": 16, "Image Set": 32, "Cosplay": 64,
        "Asian Porn": 128, "Non-H": 256, "Western": 512}
CAT_ZH = {"Misc": "杂项", "Doujinshi": "同人志", "Manga": "漫画", "Artist CG": "画师 CG", "Game CG": "游戏 CG",
          "Image Set": "图集", "Cosplay": "Cosplay", "Asian Porn": "亚洲", "Non-H": "无 H", "Western": "欧美"}
QUOTA_HINT = "E-Hentai 图片额度用完了（509），过一阵额度恢复再下；登录账号额度更多"
BAN_HINT = "E-Hentai 暂时封了服务器 IP（请求太频繁），过一阵再试，别调小请求间隔"

_client: httpx.AsyncClient | None = None
_cache: dict[str, tuple[float, Any]] = {}
CACHE_MAX = 400
_sem = asyncio.Semaphore(1)          # 同时只有一个请求 / One request at a time


# ---- 设置与连接 / Settings and connection ----

def on() -> bool:
    return bool(config.load().get("eh_on"))


def ex() -> bool:
    """用 ExHentai（要登录 cookie）。/ Use ExHentai (needs the sign-in cookies)."""
    cfg = config.load()
    return cfg.get("eh_site") == "ex" and bool(cfg.get("eh_member_id") and cfg.get("eh_pass_hash"))


def base() -> str:
    return "https://exhentai.org" if ex() else "https://e-hentai.org"


def name() -> str:
    return "ExHentai" if ex() else "E-Hentai"


def _cookies(host: str) -> str:
    """只发给 e-hentai.org / exhentai.org 本站；nw=1 跳过内容警告页。/ Only sent to e-hentai.org / exhentai.org
    themselves; nw=1 skips the content-warning page."""
    if not (host.endswith("e-hentai.org") or host.endswith("exhentai.org")):
        return ""
    cfg = config.load()
    parts = ["nw=1"]
    for k, c in (("eh_member_id", "ipb_member_id"), ("eh_pass_hash", "ipb_pass_hash"), ("eh_igneous", "igneous")):
        v = re.sub(r"[^0-9A-Za-z]", "", str(cfg.get(k) or ""))
        if v:
            parts.append(f"{c}={v}")
    return "; ".join(parts)


def client(direct: bool = False) -> httpx.AsyncClient:
    """direct 的那个只连下面表里的 IP：握手不带域名，证书对不上 IP，所以不验证。重定向自己跟，好再换成 IP。
    The direct client only connects to the IPs in the table below: the handshake carries no domain and the
    certificate cannot match an IP, so it is not verified. Redirects are followed by hand so they map to IPs again."""
    global _client, _direct
    if direct:
        if _direct is None:
            _direct = httpx.AsyncClient(timeout=httpx.Timeout(30.0, connect=10.0), follow_redirects=False,
                                        verify=False, headers={"User-Agent": UA, "Accept-Language": LANGS})
        return _direct
    if _client is None:
        _client = httpx.AsyncClient(timeout=httpx.Timeout(30.0, connect=15.0), follow_redirects=False,
                                    headers={"User-Agent": UA, "Accept-Language": LANGS})
    return _client


# 直连 IP（照 JHenTai 的域名前置）：有的网络按握手里的域名重置连接，直接连这些 IP、真域名只放 Host 头就能过。连不上的
# IP 5 分钟内不再用。图片服务器（*.hath.network）和封面（ehgt.org）不在表里，照常连。
# Direct IPs (JHenTai's domain fronting): some networks reset connections by the domain in the handshake; connecting
# to these IPs with the real domain only in the Host header gets through. An IP that fails is skipped for 5 minutes.
# Image servers (*.hath.network) and covers (ehgt.org) are not in the table and connect as usual.
HOST_IPS = {
    "e-hentai.org": ["172.66.132.196", "172.66.140.62"],
    "forums.e-hentai.org": ["172.66.132.196", "172.66.140.62"],
    "api.e-hentai.org": ["37.48.92.161", "212.7.202.51", "5.79.104.110", "37.48.81.204", "212.7.200.104"],
    "exhentai.org": ["178.175.128.251", "178.175.128.252", "178.175.128.253", "178.175.128.254", "178.175.129.251",
                     "178.175.129.252", "178.175.129.253", "178.175.129.254", "178.175.132.19", "178.175.132.20",
                     "178.175.132.21", "178.175.132.22"],
}
LANGS = "zh-CN,zh;q=0.9,en;q=0.8"
_direct: httpx.AsyncClient | None = None
_bad_ip: dict[str, float] = {}
_next_ip: dict[str, int] = {}


def _ips(host: str) -> list[str]:
    """这次按顺序试的 IP（轮流起头，坏的放最后）。/ IPs to try this time (rotating start, bad ones last)."""
    ips = HOST_IPS.get(host) or []
    if not ips:
        return []
    i = _next_ip.get(host, 0) % len(ips)
    _next_ip[host] = i + 1
    order = ips[i:] + ips[:i]
    now = time.time()
    return [ip for ip in order if now - _bad_ip.get(ip, 0) > 300] + [ip for ip in order if now - _bad_ip.get(ip, 0) <= 300]


async def _send(method: str, url: str, headers: dict, **kw) -> httpx.Response:
    u = httpx.URL(url)
    ips = _ips(u.host) if config.load().get("eh_direct") and u.scheme == "https" else []
    if not ips:
        return await client().request(method, url, headers=headers, **kw)
    err: Exception | None = None
    for ip in ips[:3]:
        try:
            return await client(True).request(method, u.copy_with(host=ip), headers={**headers, "Host": u.host}, **kw)
        except httpx.HTTPError as e:
            _bad_ip[ip] = time.time()
            err = e
    raise err  # type: ignore[misc]


async def _request(method: str, url: str, **kw) -> httpx.Response:
    try:
        async with _sem:
            for _ in range(4):
                host = httpx.URL(url).host
                headers = {"Referer": base() + "/"}
                c = _cookies(host)
                if c:
                    headers["Cookie"] = c
                r = await _send(method, url, headers, **kw)
                if host.endswith("exhentai.org"):
                    _keep_igneous(r)
                loc = r.headers.get("location")
                if r.status_code not in (301, 302, 303, 307, 308) or not loc:
                    return r
                url = str(httpx.URL(url).join(loc))
                if r.status_code == 303 or (r.status_code in (301, 302) and method == "POST"):
                    method, kw = "GET", {}
            return r
    except httpx.HTTPError as e:
        hint = "；可以在面板打开“直连 IP”试试" if not config.load().get("eh_direct") else ""
        raise SourceError(502, f"连不上 {httpx.URL(url).host}（{e.__class__.__name__}）{hint}") from None


def _set_cookie(r: httpx.Response, name: str) -> str:
    for h in r.headers.get_list("set-cookie"):
        m = re.match(rf"\s*{name}=([^;]*)", h)
        if m:
            return m.group(1)
    return ""


def _keep_igneous(r: httpx.Response) -> None:
    """ExHentai 第一次访问时发 igneous，记下来以后带上；mystery 表示这个账号进不了。/ ExHentai hands out igneous on
    the first visit; keep it for later requests. mystery means the account has no access."""
    v = re.sub(r"[^0-9A-Za-z]", "", _set_cookie(r, "igneous"))
    if v and v != "mystery" and v != config.load().get("eh_igneous"):
        config.save({"eh_igneous": v})


# ---- 登录（面板）/ Sign-in (panel) ----

LOGIN = "https://forums.e-hentai.org/index.php?act=Login&CODE=01"


async def sign_in(user: str, password: str) -> dict:
    """照 JHenTai 用论坛的登录表单换 ipb_member_id 和 ipb_pass_hash；密码只发这一次，不保存。要验证码时服务器填
    不了，让用户在浏览器登录后粘贴 cookie。登录后去 ExHentai 取一次 igneous。
    Following JHenTai, trade the forum sign-in form for ipb_member_id and ipb_pass_hash; the password is sent once
    and never stored. A captcha cannot be filled in by the server, so the user signs in in a browser and pastes the
    cookies. After signing in, ExHentai is visited once for igneous."""
    if not user or not password:
        raise SourceError(400, "要填用户名和密码")
    form = {"referer": "https://forums.e-hentai.org/index.php?", "b": "", "bt": "", "UserName": user,
            "PassWord": password, "CookieDate": "365"}
    try:
        async with _sem:
            r = await _send("POST", LOGIN, {"Referer": "https://forums.e-hentai.org/"}, data=form)
    except httpx.HTTPError as e:
        hint = "；可以先打开“直连 IP”" if not config.load().get("eh_direct") else ""
        raise SourceError(502, f"连不上 forums.e-hentai.org（{e.__class__.__name__}）{hint}") from None
    member, hash_ = _set_cookie(r, "ipb_member_id"), _set_cookie(r, "ipb_pass_hash")
    if not re.fullmatch(r"\d+", member) or not re.fullmatch(r"[0-9a-f]{32}", hash_):
        if "captcha" in r.text.lower():
            raise SourceError(400, "E-Hentai 这次要验证码，服务器没法代填：请在浏览器登录后把 cookie 粘贴到下面")
        raise SourceError(400, "用户名或密码不对")
    config.save({"eh_member_id": member, "eh_pass_hash": hash_, "eh_igneous": ""})
    forget()
    ex_ok = False
    try:
        await _request("GET", "https://exhentai.org/")
        ex_ok = bool(config.load().get("eh_igneous"))
    except SourceError:
        pass
    return {"member_id": member, "exhentai": ex_ok}


def _check_page(r: httpx.Response) -> str:
    """网页请求的通用检查，返回正文。/ Common checks for a page request; returns the body."""
    text = r.text
    if "temporarily banned" in text[:500]:
        raise SourceError(503, BAN_HINT)
    if r.status_code == 404:
        raise SourceError(404, "E-Hentai 上没有这个页面")
    if r.status_code != 200:
        raise SourceError(502, f"E-Hentai 返回 HTTP {r.status_code}")
    if not text.strip() or r.headers.get("content-type", "").startswith("image/"):
        # ExHentai 没登录好时给空白页或熊猫图。/ ExHentai serves a blank page or the panda image when not signed in.
        raise SourceError(401, "ExHentai 没登录上：检查面板里的 cookie（可能过期了）")
    return text


async def _page(url: str) -> str:
    return _check_page(await _request("GET", url))


def _cached_get(key: str, ttl: float) -> Any:
    hit = _cache.get(key)
    return hit[1] if hit and time.time() - hit[0] < ttl else None


def _remember(key: str, value: Any) -> Any:
    if len(_cache) >= CACHE_MAX:
        now = time.time()
        for k in [k for k, (t, _) in _cache.items() if now - t > 3600]:
            del _cache[k]
        while len(_cache) >= CACHE_MAX:
            del _cache[next(iter(_cache))]
    _cache[key] = (time.time(), value)
    return value


def _ttl() -> float:
    return config.load()["list_cache_min"] * 60


def forget(tid: str = "") -> None:
    for k in [k for k in _cache if not tid or k.endswith(":" + tid)]:
        del _cache[k]


# ---- 编号 / Ids ----

def _b36(n: int, width: int) -> str:
    digits = "0123456789abcdefghijklmnopqrstuvwxyz"
    out = ""
    while n:
        n, r = divmod(n, 36)
        out = digits[r] + out
    return out.rjust(width, "0")


def make_id(gid: int, token: str) -> str:
    if not 0 < gid < 36 ** 5 or not re.fullmatch(r"[0-9a-f]{10}", token):
        raise SourceError(400, "画廊编号不对")
    return "me" + _b36(gid, 5) + _b36(int(token, 16), 8)


def split_id(tid: str) -> tuple[int, str]:
    if not ID_RE.match(tid or ""):
        raise SourceError(400, "作品编号不对")
    return int(tid[2:7], 36), f"{int(tid[7:], 36):010x}"


# ---- 解析 / Parsing ----

def parse_list(page: str) -> tuple[list[tuple[int, str]], str]:
    """列表页里的画廊（按出现顺序去重）和下一页的 next 游标。/ Galleries on a list page (deduplicated in order) and the
    next-page cursor."""
    seen, out = set(), []
    for gid, token in GALLERY_RE.findall(page):
        if gid not in seen:
            seen.add(gid)
            out.append((int(gid), token))
    nxt = ""
    m = re.search(r'id="dnext"[^>]*href="([^"]+)"', page) or re.search(r'id="unext"[^>]*href="([^"]+)"', page)
    if m:
        n = NEXT_RE.search(html.unescape(m.group(1)))
        nxt = n.group(1) if n else ""
    return out, nxt


def parse_pages(page: str) -> dict[int, str]:
    """画廊详情页里每张图的查看页：页号 → 地址。/ Viewer pages on a gallery page: page number -> URL."""
    out = {}
    for m in PAGE_RE.finditer(page):
        out.setdefault(int(m.group(3)), m.group(0))
    return out


def gallery_error(page: str) -> str:
    m = re.search(r'<div class="d">\s*<p>([^<]+)</p>', page)
    return html.unescape(m.group(1)).strip() if m else ""


def parse_viewer(page: str) -> tuple[str, str]:
    """查看页里的图片地址和换服务器用的 nl 键。/ The image URL on a viewer page and the nl key for another server."""
    m = re.search(r'<img[^>]*id="img"[^>]*src="([^"]+)"', page) or re.search(r'<img[^>]*src="([^"]+)" style', page)
    nl = re.search(r"return nl\('([^']+)'\)", page)
    return (html.unescape(m.group(1)) if m else ""), (nl.group(1) if nl else "")


def is_quota(url: str) -> bool:
    """额度用完时图片地址换成 509 提示图。/ With the quota used up the image URL becomes the 509 notice image."""
    return bool(re.search(r"/509s?\.gif", url))


def is_blocking(msg: str) -> bool:
    """这种失败说明整个来源暂时不能用（额度、封 IP、没登录），排着的话不用一话一话再试。/ Failures that make the
    whole source unusable for a while (quota, IP ban, not signed in); queued episodes need not each retry."""
    return msg.startswith(QUOTA_HINT[:12]) or msg.startswith(BAN_HINT[:12]) or msg.startswith("ExHentai 没登录")


def filter_tags(text: str) -> list[str]:
    """默认过滤里的 namespace:tag 词（人气榜不接受搜索，用它们在服务器上筛）。/ The namespace:tag words of the default
    filter (the popular list takes no search, so the server filters with them)."""
    out = []
    for w in re.findall(r'(-?\w+):"([^"]+)"\$?|(-?\w+):([^\s"$]+)\$?', text or ""):
        ns, val = (w[0], w[1]) if w[0] else (w[2], w[3])
        if not ns.startswith("-"):
            out.append(f"{ns.lower()}:{val.lower()}")
    return out


# ---- 元数据 / Metadata ----

async def gdata(pairs: list[tuple[int, str]]) -> dict[int, dict]:
    """官方接口一次最多 25 部；结果按 gid 缓存。/ The official API takes up to 25 per call; results cached by gid."""
    out, need = {}, []
    for gid, token in pairs:
        hit = _cached_get(f"g:{gid}", _ttl())
        if hit:
            out[gid] = hit
        else:
            need.append([gid, token])
    for i in range(0, len(need), 25):
        r = await _request("POST", API, json={"method": "gdata", "gidlist": need[i:i + 25], "namespace": 1})
        if r.status_code != 200:
            raise SourceError(502, f"E-Hentai 接口返回 HTTP {r.status_code}")
        try:
            items = r.json().get("gmetadata") or []
        except ValueError:
            raise SourceError(502, "E-Hentai 接口返回的不是 JSON") from None
        for m in items:
            if m.get("error") or not m.get("gid"):
                continue
            out[int(m["gid"])] = _remember(f"g:{m['gid']}", m)
    return out


def _title_of(m: dict) -> str:
    jpn = html.unescape(m.get("title_jpn") or "").strip()
    en = html.unescape(m.get("title") or "").strip()
    return (jpn or en) if config.load().get("eh_jpn_title", True) else (en or jpn)


def _tags(m: dict, ns: str) -> list[str]:
    return [t.split(":", 1)[1] for t in m.get("tags") or [] if t.startswith(ns + ":")]


def _author(m: dict) -> str:
    return "、".join((_tags(m, "artist") or _tags(m, "group"))[:2]) or html.unescape(m.get("uploader") or "")


def _card(m: dict) -> dict:
    return {"id": make_id(int(m["gid"]), m["token"]), "title": _title_of(m), "author": _author(m),
            "genre": CAT_ZH.get(m.get("category", ""), m.get("category", "")), "likes": 0, "canvas": False,
            "adult": True, "src": name(), "cover": m.get("thumb") or ""}


def _keep(m: dict, cats: int, tags: list[str]) -> bool:
    if CATS.get(m.get("category", ""), 0) & cats:
        return False
    have = {t.lower() for t in m.get("tags") or []}
    return all(t in have for t in tags)


# ---- 发现与搜索 / Store and search ----

async def _list(url_of, filter_local: bool) -> list[dict]:
    cfg = config.load()
    cats, tags = int(cfg.get("eh_cats") or 0), filter_tags(cfg.get("eh_filter") or "")
    items, nxt, seen = [], "", set()
    for _ in range(STORE_PAGES):
        page = await _page(url_of(nxt))
        pairs, nxt = parse_list(page)
        pairs = [p for p in pairs if p[0] not in seen]       # 翻页时新上传会把旧的挤到下一页 / New uploads shift pages
        seen.update(p[0] for p in pairs)
        meta = await gdata(pairs)
        for gid, _token in pairs:
            m = meta.get(gid)
            if m and (not filter_local or _keep(m, cats, tags)):
                items.append(_card(m))
        if not nxt or len(items) >= STORE_ITEMS:
            break
    return items[:STORE_ITEMS]


def _search_url(q: str, nxt: str) -> str:
    cfg = config.load()
    params = {"f_search": " ".join(x for x in (q, cfg.get("eh_filter") or "") if x.strip()).strip()}
    if int(cfg.get("eh_cats") or 0):
        params["f_cats"] = str(int(cfg["eh_cats"]))
    if nxt:
        params["next"] = nxt
    return str(httpx.URL(base() + "/", params={k: v for k, v in params.items() if v}))


async def store(seg: str) -> dict:
    seg = seg if seg in SEGS else "pop"
    key = f"store:{base()}:{seg}"
    items = _cached_get(key, _ttl())
    if items is None:
        if seg == "pop":
            items = await _list(lambda _n: base() + "/popular", True)
        else:
            items = await _list(lambda n: _search_url("", n), False)
        _remember(key, items)
    return {"seg": seg, "key": "", "keys": [], "items": items}


async def search(q: str) -> dict:
    q = (q or "").strip()[:80]
    if not q:
        raise SourceError(400, "没有关键词")
    key = f"search:{base()}:{q}"
    items = _cached_get(key, 300)
    if items is None:
        items = _remember(key, await _list(lambda n: _search_url(q, n), False))
    return {"q": q, "items": items}


# ---- 作品与话 / Titles and episodes ----

def chunk() -> int:
    return max(20, min(500, int(config.load().get("eh_chunk") or 80)))


async def _meta(tid: str) -> dict:
    gid, token = split_id(tid)
    m = (await gdata([(gid, token)])).get(gid)
    if not m:
        raise SourceError(404, "E-Hentai 上没有这个画廊（可能被删了）")
    return m


async def title(tid: str) -> dict:
    m = await _meta(tid)
    n, size = int(m.get("filecount") or 0), chunk()
    count = max(1, math.ceil(n / size)) if n else 0
    posted = int(m.get("posted") or 0)
    eps = []
    for i in range(count):
        a, b = i * size + 1, min(n, (i + 1) * size)
        eps.append({"no": i + 1, "title": "全本" if count == 1 else f"第 {a}–{b} 张", "date": posted})
    uploader = html.unescape(m.get("uploader") or "")
    head = [f"{n} 张" if n else "", "、".join(_tags(m, "language")[:2]), f"上传：{uploader}" if uploader else ""]
    tags = [t for t in m.get("tags") or [] if not t.startswith(("language:", "artist:", "group:"))][:24]
    summary = " · ".join(x for x in head if x) + ("\n" + "  ".join(tags) if tags else "")
    gid, token = split_id(tid)
    return {"id": tid, "title": _title_of(m), "author": _author(m),
            "genre": CAT_ZH.get(m.get("category", ""), m.get("category", "")), "summary": summary, "src": name(),
            "done": True, "cover": m.get("thumb") or "", "web": f"{base()}/g/{gid}/{token}/", "episodes": eps,
            "latest": count, "error": "已被隐藏（expunged）" if m.get("expunged") else ""}


async def episodes(tid: str) -> list[dict]:
    return (await title(tid))["episodes"]


async def cover_url(tid: str) -> str:
    return (await _meta(tid)).get("thumb") or ""


async def _gallery_page(gid: int, token: str, p: int) -> dict[int, str]:
    key = f"gp:{gid}:{p}"
    hit = _cached_get(key, 3600)
    if hit is not None:
        return hit
    page = await _page(f"{base()}/g/{gid}/{token}/?p={p}")
    pages = parse_pages(page)
    if not pages:
        err = gallery_error(page)
        raise SourceError(404 if err else 502, f"E-Hentai：{err}" if err else "画廊页里没找到图片（网页可能改版了）")
    return _remember(key, pages)


async def images(tid: str, no: int) -> list[str]:
    """第 no 话那几张图的查看页地址（按页号）。/ Viewer-page URLs of episode no's images, in page order."""
    gid, token = split_id(tid)
    m = await _meta(tid)
    n, size = int(m.get("filecount") or 0), chunk()
    a, b = (int(no) - 1) * size + 1, min(n, int(no) * size)
    if no <= 0 or a > b:
        raise SourceError(404, "没有这一话")
    first = await _gallery_page(gid, token, 0)
    per = max(1, len(first))            # 一页详情页有几张（20 或 40，看账号设置）/ Images per gallery page (20 or 40)
    found: dict[int, str] = dict(first)
    for p in range((a - 1) // per, (b - 1) // per + 1):
        if p and not all(i in found for i in range(p * per + 1, min(n, (p + 1) * per) + 1)):
            found.update(await _gallery_page(gid, token, p))
    out = [found[i] for i in range(a, b + 1) if i in found]
    if not out:
        raise SourceError(502, "这一话没找到图片")
    return out


async def _download(url: str) -> bytes:
    if is_quota(url):
        raise SourceError(509, QUOTA_HINT)
    r = await _request("GET", url)
    if r.status_code != 200 or not r.content:
        raise SourceError(502, f"取图失败（HTTP {r.status_code}）")
    if not r.headers.get("content-type", "image/").startswith("image/"):
        _check_page(r)
        raise SourceError(502, "取图失败（返回的不是图片）")
    return r.content


async def get_image(url: str, pace: bool = True) -> bytes:
    """查看页地址先解析成图片地址再下；封面这类直接是图片的地址直接下。图片服务器失败时换一台再试一次。
    A viewer-page URL is resolved to its image first; direct image URLs such as covers are fetched as is. A failed
    image server is retried once on another."""
    if pace:
        cfg = config.load()
        await asyncio.sleep(random.uniform(cfg["delay_min"], cfg["delay_max"]))
    if not PAGE_RE.match(url):
        return await _download(url)
    img, nl = parse_viewer(await _page(url))
    if not img:
        raise SourceError(502, "查看页里没找到图片（网页可能改版了）")
    try:
        return await _download(img)
    except SourceError as e:
        if e.status == 509 or not nl:
            raise
    img, _ = parse_viewer(await _page(url + ("&" if "?" in url else "?") + "nl=" + nl))
    if not img:
        raise SourceError(502, "换图片服务器后还是没找到图片")
    return await _download(img)
