"""定时拉取（调度逻辑照 weiboX-server）：按“该更新了”的程度排关注的人，逐个读主页；再读一次热门。新微博去掉带视频的，
长微博补全文，图片下载后处理成墨水屏用的灰度 JPEG 存下来。碰到验证码就暂停等面板里处理；访客身份失效就重新生成。

Scheduled fetching (scheduling as in weiboX-server): rank followed users by how overdue they are and read each
profile, then read the hot feed once. New posts drop videos, long posts get their full text, and images are
downloaded and turned into e-ink grayscale JPEGs. A captcha pauses the round until it is solved in the panel; an
expired visitor identity is regenerated.
"""
from __future__ import annotations

import asyncio
import random
import time
from collections import deque
from typing import Any

from PIL import Image

from . import config, images, store
from .weibo.captcha import captcha_manager
from .weibo.client import CaptchaRequired, SessionInvalid, WeiboClient, WeiboError
from .weibo.session import effective_cookie

log: deque[str] = deque(maxlen=300)

# 没有历史时假设 6 小时发一条，估算最少按 30 分钟。/ Assumed posting gap without history; floor of 30 minutes.
DEFAULT_GAP_MS = 6 * 3600 * 1000
MIN_GAP_MS = 30 * 60 * 1000
RETRY_SESSION = object()


def note(msg: str) -> None:
    log.appendleft(time.strftime("%m-%d %H:%M:%S ") + msg)


class State:
    running = False
    paused = ""          # 非空时暂停，写着原因 / Paused with this reason
    current = ""
    done = 0
    total = 0
    new_follow = 0
    new_hot = 0
    last_round = 0.0
    next_round = 0.0
    last_error = ""
    visitor = True


state = State()
_wake = asyncio.Event()


def wake() -> None:
    _wake.set()


def resume() -> None:
    state.paused = ""
    wake()


# ---- 整理一条微博 / Prepare one post ----

def has_video(p: dict[str, Any]) -> bool:
    rt = p.get("retweet") or {}
    return bool(p.get("video") or rt.get("video"))


def _cover_ratio(note_id: str) -> float:
    try:
        with Image.open(images.note_dir(note_id) / "thumb.jpg") as im:
            return round(im.height / im.width, 3)
    except OSError:
        return 1.333


async def _long_text(client: WeiboClient, mid: str, cfg: dict[str, Any]) -> str:
    if store.today("long") >= int(cfg["long_daily_cap"]):
        return ""
    await asyncio.sleep(int(cfg["long_gap_s"]) + random.uniform(0, 3))
    store.count("long")
    try:
        return await client.get_long_text(mid)
    except (WeiboError, SessionInvalid, CaptchaRequired) as e:
        note(f"长文 {mid} 取不到：{e}")
        return ""


async def prepare(client: WeiboClient, p: dict[str, Any], cfg: dict[str, Any]) -> dict[str, Any]:
    """补全文、下图、处理图，返回入库用的记录。/ Fill long text, fetch and process images; return the stored row."""
    text = p.get("text") or ""
    if p.get("is_long"):
        text = await _long_text(client, p["id"], cfg) or text
    rt = p.get("retweet") or {}
    rt_text = rt.get("text") or ""
    if rt and rt.get("is_long"):
        rt_text = await _long_text(client, rt["id"], cfg) or rt_text
    # 本条的图在前，转发原微博的图接在后面。/ This post's images first, then the retweeted post's.
    urls = (list(p.get("pics") or []) + list(rt.get("pics") or []))[: int(cfg["max_images"])]
    pages = await images.process(p["id"], urls) if urls else 0
    return {
        "id": p["id"], "user_id": p.get("user_id", ""), "user_name": p.get("user_name", ""),
        "text": text, "created_ts": int(p.get("created_at_ts") or 0),
        "likes": int(p.get("likes_count") or 0), "comments": int(p.get("comments_count") or 0),
        "reposts": int(p.get("reposts_count") or 0), "source": p.get("source", ""),
        "rt_user": rt.get("user_name", "") if rt else "", "rt_text": rt_text,
        "images": pages, "ratio": _cover_ratio(p["id"]) if pages else 1.333,
    }


# ---- 一轮 / One round ----

def _candidates(cfg: dict[str, Any]) -> list[str]:
    """按“逾期程度”排序：距上次抓的时间 ÷ 这个人的平均发帖间隔。/ Sorted by overdue ratio."""
    now = time.time() * 1000
    min_check = int(cfg["min_check_min"]) * 60 * 1000
    stats = store.post_stats_by_user()
    scored = []
    for u in store.users():
        since = now - (u.get("last_fetched_at") or 0) * 1000
        if since < min_check:
            continue
        st = stats.get(u["id"])
        avg = (st["newest"] - st["oldest"]) / (st["count"] - 1) if st and st["count"] > 1 else DEFAULT_GAP_MS
        scored.append((u["id"], since / max(avg, MIN_GAP_MS)))
    scored.sort(key=lambda x: x[1], reverse=True)
    return [uid for uid, _ in scored]


async def _with_captcha(client: WeiboClient, call):
    """跑一次请求；要验证码就暂停等面板处理。返回结果 / None（放弃）/ RETRY_SESSION。
    Run one request; on a captcha pause for the panel. Returns the result, None (gave up) or RETRY_SESSION."""
    try:
        return await call()
    except SessionInvalid:
        return RETRY_SESSION
    except CaptchaRequired as e:
        state.paused = "微博要求验证：到面板“验证”页处理"
        note("需要验证，等待处理…")
        new_cookie = await captcha_manager.solve(e.captcha_url, client.export_cookie_string())
        if not new_cookie:
            state.paused = "验证没完成，本轮停止。处理好后点“立即拉一轮”"
            note(state.paused)
            return None
        state.paused = ""
        client.merge_cookies(new_cookie)
        if (config.load().get("cookie") or "").strip():
            config.save({"cookie": client.export_cookie_string()})
        note("验证完成，继续")
        try:
            return await call()
        except (CaptchaRequired, SessionInvalid):
            return None


async def _store_posts(client: WeiboClient, posts: list[dict[str, Any]], cfg: dict[str, Any], hot_at: float = 0) -> int:
    new = 0
    for rank, p in enumerate(posts):
        if not p.get("id") or has_video(p):
            continue
        if store.has_post(p["id"]):
            store.update_counts(p["id"], int(p.get("likes_count") or 0), int(p.get("comments_count") or 0),
                                int(p.get("reposts_count") or 0))
            if hot_at:
                store.flag_hot(p["id"], hot_at, rank)
            else:
                store.flag_follow(p["id"])
            continue
        row = await prepare(client, p, cfg)
        row.update(in_follow=0 if hot_at else 1, in_hot=1 if hot_at else 0, hot_at=hot_at, hot_rank=rank)
        store.save_post(row)
        new += 1
    return new


async def run_round() -> None:
    cfg = config.load()
    if state.running:
        return
    state.running = True
    state.new_follow = state.new_hot = 0
    state.last_error = ""
    candidates = _candidates(cfg)
    state.total, state.done = len(candidates), 0
    note(f"开始一轮：{len(candidates)} 人" + ("，加热门" if cfg["hot_enabled"] else ""))
    try:
        cookie, state.visitor = await effective_cookie()
    except Exception as e:  # 浏览器起不来等 / Browser failed to start, etc.
        state.last_error = f"访客身份生成失败：{e}"
        note(state.last_error)
        state.running = False
        return
    client = WeiboClient(cookie)
    lo, hi = int(cfg["delay_min"]), int(cfg["delay_max"])
    try:
        for i, uid in enumerate(candidates):
            if state.paused:
                break
            if i:
                await asyncio.sleep(random.uniform(lo, max(lo, hi)))
            state.current = uid
            count = int(cfg["posts_per_user"])
            call = lambda: client.get_user_posts(uid, page=1, count=count)  # noqa: E731
            posts = await _with_captcha(client, call)
            if posts is RETRY_SESSION:
                await client.aclose()
                cookie, state.visitor = await effective_cookie(force_visitor=True)
                client = WeiboClient(cookie)
                posts = await _with_captcha(client, call)
                if posts is RETRY_SESSION:
                    posts = []
            if posts is None:
                break
            new = await _store_posts(client, posts, cfg)
            state.new_follow += new
            first = posts[0] if posts else {}
            store.mark_fetched(uid, "", first.get("user_name", ""), first.get("user_avatar", ""))
            if new:
                note(f"{first.get('user_name') or uid}：新 {new} 条")
            state.done = i + 1
        if cfg["hot_enabled"] and not state.paused:
            if candidates:
                await asyncio.sleep(random.uniform(lo, max(lo, hi)))
            state.current = "热门"
            channel = cfg["hot_channel"] if cfg["hot_channel"] in config.HOT_CHANNELS else "102803"
            hot_call = lambda: client.get_hot_posts(containerid=channel, count=int(cfg["hot_count"]))  # noqa: E731
            hot = await _with_captcha(client, hot_call)
            if hot is RETRY_SESSION:
                await client.aclose()
                cookie, state.visitor = await effective_cookie(force_visitor=True)
                client = WeiboClient(cookie)
                hot = await _with_captcha(client, hot_call)
            if isinstance(hot, list):
                state.new_hot = await _store_posts(client, hot, cfg, hot_at=time.time())
                videos = sum(1 for p in hot if has_video(p))
                note(f"热门（{config.HOT_CHANNELS.get(channel, channel)}）：{len(hot)} 条，视频跳过 {videos}，新 {state.new_hot} 条")
    except WeiboError as e:
        state.last_error = str(e)
        note(f"本轮出错：{e}")
    finally:
        await client.aclose()
        cleanup(cfg)
        state.running = False
        state.current = ""
        state.last_round = time.time()
        note(f"本轮结束：关注新 {state.new_follow} 条，热门新 {state.new_hot} 条")


def _drop(pid: str) -> None:
    images.remove(pid)
    store.delete_post(pid)


def cleanup(cfg: dict[str, Any]) -> None:
    """按天数、热门条数和总大小清理。/ Prune by age, hot-list length and total size."""
    for pid in store.old_posts(time.time() - int(cfg["keep_days"]) * 86400):
        _drop(pid)
    for pid in store.drop_hot_older(max(int(cfg["hot_count"]) * 4, 100)):
        _drop(pid)
    while images.total_mb() > int(cfg["max_mb"]) and store.post_count():
        for pid in store.oldest_posts(20):
            _drop(pid)


async def loop() -> None:
    while True:
        cfg = config.load()
        state.next_round = time.time() + int(cfg["interval_min"]) * 60
        # 先清再跑：本轮进行中收到的唤醒留给下一轮。/ Clear before running so a wake during the round is kept.
        _wake.clear()
        if not state.paused and (store.users() or cfg["hot_enabled"]):
            try:
                await run_round()
            except Exception as e:  # 一轮出错不影响下一轮 / One failed round must not stop the loop
                note(f"本轮异常：{type(e).__name__}: {e}")
                state.running = False
            state.next_round = time.time() + int(cfg["interval_min"]) * 60
        try:
            await asyncio.wait_for(_wake.wait(), timeout=max(5.0, state.next_round - time.time()))
        except asyncio.TimeoutError:
            pass
