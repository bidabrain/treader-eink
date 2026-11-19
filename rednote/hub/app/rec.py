"""推荐：设备点“刷新”时才代拉一次首页，点开哪篇才拉哪篇详情。视频在列表阶段就过滤掉。
有独立的每日上限、最短间隔和列表缓存；拉取暂停（要验证或登录失效）时一律拒绝。

Recommendations: the home feed is read only when the device asks, and a note's detail only when it is opened.
Videos are dropped at the list stage. Own daily cap, minimum gap and list cache; refused while fetching is paused.
"""
from __future__ import annotations

import asyncio
import io
import random
import time
from pathlib import Path
from typing import Any

import httpx
from PIL import Image, ImageOps

from . import config, fetcher, images, mcp, store

COVER_DIR = config.DATA / "rec"


class RecError(Exception):
    def __init__(self, status: int, msg: str):
        super().__init__(msg)
        self.status = status


class _List:
    fetched_at = 0.0
    items: list[dict[str, Any]] = []


_list = _List()
# 列表里每篇的 xsec_token，点开时要用；保留最近几轮的。/ Tokens for opening notes, kept across recent lists.
_tokens: dict[str, str] = {}
_refreshing: asyncio.Lock = asyncio.Lock()
_opening: dict[str, asyncio.Task] = {}


def cover_path(note_id: str) -> Path:
    return COVER_DIR / f"{note_id}.jpg"


def _check(cfg: dict[str, Any]) -> None:
    if not cfg.get("rec_enabled"):
        raise RecError(403, "推荐已在面板里关闭")
    if fetcher.state.paused:
        raise RecError(503, "小红书要求验证或登录，先在面板里处理")
    if store.requests_today("rec") >= int(cfg["rec_daily_cap"]):
        raise RecError(429, "今天的推荐次数用完了")
    if store.requests_today() >= int(cfg["daily_cap"]):
        raise RecError(429, "今天的请求总数用完了")


async def _space(cfg: dict[str, Any]) -> None:
    """和上一次请求小红书至少隔 rec_gap_s 秒。/ Keep rec_gap_s seconds after the previous call."""
    wait = mcp.last_call + int(cfg["rec_gap_s"]) + random.uniform(0, 3) - time.time()
    if wait > 0:
        await asyncio.sleep(wait)


async def _blocked(err: Exception) -> None:
    await fetcher._pause_if_blocked(str(err))


def _cover_url(card: dict[str, Any]) -> str:
    cover = card.get("cover") or {}
    url = cover.get("urlDefault") or cover.get("url") or ""
    if not url:
        for info in cover.get("infoList") or []:
            url = info.get("url") or url
    return url


async def _covers(items: list[dict[str, Any]], urls: dict[str, str]) -> None:
    """下载封面做成 150×200 灰度缩略图（走图片 CDN，不算小红书请求）。
    Download covers as 150x200 grayscale thumbnails (image CDN, not counted as Xiaohongshu requests).
    """
    COVER_DIR.mkdir(parents=True, exist_ok=True)

    async def one(client: httpx.AsyncClient, item: dict[str, Any]) -> None:
        path = cover_path(item["id"])
        if path.exists():
            item["thumb"] = True
            return
        url = urls.get(item["id"])
        if not url:
            return
        try:
            resp = await client.get(url)
            resp.raise_for_status()
            img = ImageOps.exif_transpose(Image.open(io.BytesIO(resp.content))).convert("L")
            thumb = images.cover(img)
            tmp = path.with_suffix(".tmp")
            thumb.save(tmp, "JPEG", quality=80, optimize=True, progressive=False)
            tmp.replace(path)
            item["thumb"] = True
        except (httpx.HTTPError, OSError):
            pass

    async with httpx.AsyncClient(timeout=20.0, headers=images.HEADERS, follow_redirects=True) as client:
        await asyncio.gather(*(one(client, it) for it in items))


def view() -> dict[str, Any]:
    cfg = config.load()
    for it in _list.items:
        it["ready"] = store.has_note(it["id"])
    return {
        "fetched_at": _list.fetched_at,
        "refresh_after": _list.fetched_at + int(cfg["rec_cache_min"]) * 60,
        "left_today": max(0, int(cfg["rec_daily_cap"]) - store.requests_today("rec")),
        "notes": _list.items,
    }


async def refresh(force: bool) -> dict[str, Any]:
    """读一次首页推荐；缓存期内直接返回上次的。/ Read the home feed once; within the cache window reuse the last list."""
    cfg = config.load()
    async with _refreshing:
        if not force or time.time() < _list.fetched_at + int(cfg["rec_cache_min"]) * 60:
            return view()
        _check(cfg)
        await _space(cfg)
        store.count_request("rec")
        try:
            feeds = await mcp.feeds()
        except mcp.McpError as e:
            await _blocked(e)
            raise RecError(502, f"读取推荐失败：{e}") from e
        items, urls = [], {}
        for f in feeds:
            card = f.get("noteCard") or {}
            # 只要图文；视频设备看不了。/ Image notes only; the device cannot play videos.
            if card.get("type") != "normal" or not f.get("id"):
                continue
            user = card.get("user") or {}
            cov = card.get("cover") or {}
            items.append({
                "id": f["id"], "title": card.get("displayTitle") or "",
                "author": user.get("nickname") or user.get("nickName") or "",
                "likes": (card.get("interactInfo") or {}).get("likedCount") or "",
                # 封面高宽比，给瀑布流排版用。/ Cover height/width for waterfall layouts.
                "ratio": images.cover_ratio(int(cov.get("width") or 0), int(cov.get("height") or 0)),
                "thumb": False, "ready": False,
            })
            _tokens[f["id"]] = f.get("xsecToken") or ""
            urls[f["id"]] = _cover_url(card)
            if len(items) >= int(cfg["rec_max"]):
                break
        while len(_tokens) > 500:
            _tokens.pop(next(iter(_tokens)))
        await _covers(items, urls)
        _list.items, _list.fetched_at = items, time.time()
        fetcher.note(f"推荐：设备刷新，{len(feeds)} 篇里有 {len(items)} 篇图文")
        _prune_covers()
        return view()


async def _open(note_id: str) -> None:
    cfg = config.load()
    _check(cfg)
    token = _tokens.get(note_id)
    if token is None:
        raise RecError(404, "这篇不在推荐列表里了，请刷新推荐")
    await _space(cfg)
    try:
        # 正文先返回，图片在后台做；设备取图时服务器会等那一页做好。/ Return the text first; images finish in the
        # background and an image request waits for its page.
        ok = await fetcher.fetch_note(note_id, token, cfg, source="rec", background=True)
    except mcp.McpError as e:
        await _blocked(e)
        raise RecError(502, f"读取笔记失败：{e}") from e
    if not ok:
        raise RecError(415, "这篇是视频，设备看不了")
    fetcher.note(f"推荐：设备打开 {note_id}")


async def open_note(note_id: str) -> dict[str, Any] | None:
    """已经拉过就直接返回，否则现拉；同一篇并发只拉一次。/ Return cached, else fetch once even if asked concurrently."""
    n = store.note(note_id)
    if n:
        return n
    task = _opening.get(note_id)
    if task is None:
        task = asyncio.create_task(_open(note_id))
        _opening[note_id] = task
        task.add_done_callback(lambda _t: _opening.pop(note_id, None))
    await asyncio.shield(task)
    return store.note(note_id)


def _prune_covers() -> None:
    """封面只留当前和最近列表用得到的。/ Keep covers only for notes still in recent lists."""
    keep = set(_tokens)
    for p in COVER_DIR.glob("*.jpg"):
        if p.stem not in keep:
            p.unlink(missing_ok=True)


# ---- 加载更多评论（设备按需） / Load more comments (on demand from the device) ----

_more: dict[str, asyncio.Task] = {}


def can_more(n: dict[str, Any]) -> bool:
    """还能不能加载更多评论：有令牌、没加载过、评论总数比已存的多。
    Whether more comments can be loaded: a token, not loaded yet, and more comments than are stored."""
    return bool(n.get("xsec_token")) and not n.get("more_comments") and \
        fetcher.count_value(n.get("comment_count")) > len(n.get("comments") or [])


async def _load_more(note_id: str) -> None:
    cfg = config.load()
    n = store.note(note_id)
    if not n:
        raise RecError(404, "没有这篇")
    if not can_more(n):
        return
    if fetcher.state.paused:
        raise RecError(503, "小红书要求验证或登录，先在面板里处理")
    if store.requests_today("rec") >= int(cfg["rec_daily_cap"]):
        raise RecError(429, "今天的按需请求次数用完了")
    if store.requests_today() >= int(cfg["daily_cap"]):
        raise RecError(429, "今天的请求总数用完了")
    await _space(cfg)
    # 和推荐一样算按需请求。/ Counted as an on-demand request, like recommendations.
    store.count_request("rec")
    try:
        data = await mcp.detail(note_id, n["xsec_token"], fetcher.COMMENTS_MORE)
    except mcp.McpError as e:
        await _blocked(e)
        raise RecError(502, f"加载评论失败：{e}") from e
    comments = fetcher.parse_comments(data, fetcher.COMMENTS_MORE)
    count = ((data.get("note") or {}).get("interactInfo") or {}).get("commentCount") or n["comment_count"]
    # 拿到的比原来还少（页面没加载出来）就保留原来的。/ Keep the old list if fewer came back (the page did not load).
    if len(comments) < len(n["comments"]):
        comments = n["comments"]
    store.save_comments(note_id, count, comments)
    fetcher.note(f"设备加载更多评论 {note_id}：{len(comments)} 条")


_more_err: dict[str, tuple[int, str]] = {}


def more_start(note_id: str) -> None:
    """在后台开始加载；已经在加载就不重复。/ Start loading in the background unless it already is."""
    if note_id in _more:
        return
    _more_err.pop(note_id, None)
    task = asyncio.create_task(_load_more(note_id))
    _more[note_id] = task

    def done(t: asyncio.Task) -> None:
        _more.pop(note_id, None)
        e = None if t.cancelled() else t.exception()
        if isinstance(e, RecError):
            _more_err[note_id] = (e.status, str(e))
        elif e is not None:
            _more_err[note_id] = (500, f"加载评论失败：{type(e).__name__}")
        while len(_more_err) > 50:
            _more_err.pop(next(iter(_more_err)))

    task.add_done_callback(done)


async def more_wait(note_id: str, seconds: float) -> tuple[bool, tuple[int, str] | None]:
    """最多等 seconds 秒；返回（还在加载、上次的错误）。/ Wait up to seconds; returns (still loading, last error)."""
    task = _more.get(note_id)
    if task is not None:
        await asyncio.wait({task}, timeout=seconds)
    return note_id in _more, _more_err.get(note_id)
