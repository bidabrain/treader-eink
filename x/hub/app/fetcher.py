"""调度：为你推荐和正在关注各有一个随机计时器，下一次在 [最小间隔, 最大间隔] 之间随机取；睡眠时段里自动拉取全部
暂停，醒来后第一次在 0～最小间隔之间随机推迟，不会准点。两条流不并发，同时到点时后一条再随机晚几分钟。手动拉取不受
间隔和睡眠限制，拉完那条流的计时器重新开始。

碰到登录失效、账号验证或 429，自动拉取立即停下，不自动重试：登录失效和账号验证在重新登录成功后恢复，429 要在面板上
点“恢复自动拉取”。

Scheduling: For You and Following each have a random timer; the next run falls between the minimum and maximum gap.
Inside the sleep window all automatic fetching pauses, and the first run after it is delayed by a random 0..minimum
gap, never on the hour. The two feeds never run together; when both are due the second waits a few random minutes.
Manual fetches ignore gaps and sleep, and restart that feed's timer.

A lost sign-in, an account check or a 429 stops automatic fetching at once with no automatic retry: sign-in problems
clear after a successful new sign-in, a 429 needs "resume automatic fetching" in the panel.
"""
from __future__ import annotations

import asyncio
import datetime as dt
import random
import time
from collections import deque
from typing import Any

from . import config, images, parser, store
from .browser import AccountLocked, Busy, LoginRequired, RateLimited, browser

log: deque[str] = deque(maxlen=400)


def note(msg: str) -> None:
    log.appendleft(time.strftime("%m-%d %H:%M:%S ") + msg)


class FeedState:
    def __init__(self) -> None:
        self.next_at = 0.0
        self.last_at = 0.0
        self.last_new = 0
        self.last_total = 0
        self.last_ads = 0
        self.last_error = ""


class State:
    def __init__(self) -> None:
        self.account = "unknown"      # unknown / ok / need_login / locked
        self.paused = ""              # 非空时自动拉取停着，写着原因 / Automatic fetching stopped, with the reason
        self.running = ""             # 正在拉的流 / Feed being fetched
        self.feeds = {k: FeedState() for k in config.FEEDS}


state = State()
_wake = asyncio.Event()
_manual: list[str] = []


def wake() -> None:
    _wake.set()


# ---- 时间 / Time ----

def _hm(text: str) -> dt.time:
    try:
        h, m = str(text).split(":")
        return dt.time(int(h) % 24, int(m) % 60)
    except ValueError:
        return dt.time(0, 0)


def sleeping(cfg: dict[str, Any], now: float | None = None) -> bool:
    if not cfg.get("sleep_enabled"):
        return False
    t = dt.datetime.fromtimestamp(now or time.time()).time()
    a, b = _hm(cfg["sleep_start"]), _hm(cfg["sleep_end"])
    if a == b:
        return False
    return a <= t < b if a < b else (t >= a or t < b)


def _wake_time(cfg: dict[str, Any], now: float) -> float:
    """下一次睡眠结束的时刻。/ When the current sleep window ends."""
    d = dt.datetime.fromtimestamp(now)
    end = d.replace(hour=_hm(cfg["sleep_end"]).hour, minute=_hm(cfg["sleep_end"]).minute, second=0, microsecond=0)
    if end <= d:
        end += dt.timedelta(days=1)
    return end.timestamp()


def _gap(cfg: dict[str, Any], feed: str) -> float:
    lo = max(config.FLOOR_MIN, int(cfg[f"{feed}_min"]))
    hi = max(lo, int(cfg[f"{feed}_max"]))
    return random.uniform(lo, hi) * 60


def reschedule(first: bool = False) -> None:
    """开机时排第一次（2 分钟～最小间隔之后）；改设置后不晚于新间隔。/ At boot schedule the first run (2 minutes
    to the minimum gap away); after a settings change never later than the new gap."""
    cfg, now = config.load(), time.time()
    for feed, fs in state.feeds.items():
        if first or fs.next_at < now:
            lo = max(config.FLOOR_MIN, int(cfg[f"{feed}_min"]))
            fs.next_at = now + random.uniform(2, lo) * 60
        else:
            fs.next_at = min(fs.next_at, now + _gap(cfg, feed))
    wake()


# ---- 手动 / Manual ----

def feed_on(cfg: dict[str, Any], feed: str) -> bool:
    """这条流总开关开着吗（为你推荐总是开）。/ Whether this feed's master switch is on (For You always is)."""
    return feed != "following" or bool(cfg.get("following_on", True))


def request(feed: str) -> tuple[bool, str]:
    if not feed_on(config.load(), feed):
        return False, "正在关注的总开关关着，先在设置里打开"
    if state.account in ("need_login", "locked"):
        return False, "X 需要重新登录，先到“登录”页处理"
    if browser.login["active"]:
        return False, "正在登录，关掉登录窗口后再拉"
    if feed in _manual or state.running == feed:
        return True, f"{config.FEEDS[feed]}已经在排队或正在拉"
    _manual.append(feed)
    wake()
    return True, f"已开始拉取{config.FEEDS[feed]}" if not state.running else f"排在当前拉取之后"


def resume() -> None:
    state.paused = ""
    note("恢复自动拉取")
    wake()


def login_done(ok: bool) -> None:
    if ok:
        state.account = "ok"
        if state.paused.startswith("X "):
            state.paused = ""
        note("X 登录成功，自动拉取恢复")
    else:
        state.account = "ok" if browser.signed_in_cache else "need_login"
    wake()


# ---- 拉一次 / One fetch ----

async def _store(feed: str, posts: list[dict[str, Any]], cfg: dict[str, Any]) -> tuple[int, int]:
    at, new, skipped = time.time(), 0, 0
    for rank, p in enumerate(posts):
        if p["video"] and cfg.get("video") == "skip":
            skipped += 1
            continue
        if store.has_post(p["id"]):
            store.refresh(p["id"], p, feed, at, rank)
            continue
        urls = (p["media"] + p["qt_media"])[: int(cfg["max_images"])]
        pages, starts, ratio = await images.process_post(p["id"], urls, browser.download) if urls else (0, [], 1.333)
        row = dict(p, images=pages, pics=len(starts), starts=",".join(str(s) for s in starts), ratio=ratio,
                   in_foryou=int(feed == "foryou"), foryou_at=at if feed == "foryou" else 0, foryou_rank=rank,
                   in_following=int(feed == "following"))
        store.save_post(row)
        new += 1
    return new, skipped


async def run(feed: str, manual: bool) -> None:
    cfg = config.load()
    fs = state.feeds[feed]
    state.running = feed
    fs.last_error = ""
    lo, hi = int(cfg["scroll_min"]), int(cfg["scroll_max"])
    scrolls = random.randint(min(lo, hi), max(lo, hi))
    note(f"{'手动' if manual else '定时'}拉取{config.FEEDS[feed]}（滚动 {scrolls} 次）")
    try:
        bodies = await browser.timeline(feed, scrolls)
        posts, ads = parser.timeline(bodies)
        new, skipped = await _store(feed, posts, cfg)
        state.account = "ok"
        if manual and state.paused:
            state.paused = ""
            note("手动拉取正常，自动拉取恢复")
        fs.last_new, fs.last_total, fs.last_ads = new, len(posts), ads
        note(f"{config.FEEDS[feed]}：{len(posts)} 条，新 {new} 条，去广告 {ads} 条"
             + (f"，跳过视频 {skipped} 条" if skipped else ""))
        if bodies and not posts:
            note("返回里没解析出推文：X 可能改了格式")
    except LoginRequired:
        state.account = "need_login"
        state.paused = "X 登录失效：到“登录”页重新登录"
        note(state.paused)
    except AccountLocked:
        state.account = "locked"
        state.paused = "X 要求验证账号：到“登录”页打开浏览器按提示处理"
        note(state.paused)
    except RateLimited:
        state.paused = "X 限流（429），自动拉取已停：过一段时间确认正常后点“恢复自动拉取”"
        note(state.paused)
    except Busy as e:
        fs.last_error = str(e)
        note(f"没拉：{e}")
    except Exception as e:
        fs.last_error = f"{type(e).__name__}: {e}"
        note(f"{config.FEEDS[feed]}出错：{fs.last_error}")
    finally:
        state.running = ""
        fs.last_at = time.time()
        fs.next_at = time.time() + _gap(cfg, feed)
        # 另一条流马上也要到点：随机错开几分钟。/ The other feed is about due: stagger it by a few random minutes.
        for other, ofs in state.feeds.items():
            if other != feed and ofs.next_at < time.time() + 120:
                ofs.next_at = time.time() + random.uniform(120, 360)
        cleanup(cfg)


def cleanup(cfg: dict[str, Any]) -> None:
    for pid in store.old_posts(time.time() - int(cfg["keep_days"]) * 86400):
        images.remove(pid)
        store.delete_post(pid)
    for pid in store.drop_foryou_older(int(cfg["foryou_keep"])):
        images.remove(pid)
        store.delete_post(pid)
    while images.total_mb() > int(cfg["max_mb"]) and store.count():
        for pid in store.oldest_posts(20):
            images.remove(pid)
            store.delete_post(pid)


def _next_job(cfg: dict[str, Any]) -> tuple[str, bool] | None:
    if browser.login["active"]:
        return None
    if _manual:
        return _manual.pop(0), True
    if state.paused or state.account != "ok":
        return None
    now = time.time()
    for feed, fs in sorted(state.feeds.items(), key=lambda kv: kv[1].next_at):
        if not feed_on(cfg, feed) or not cfg.get(f"{feed}_enabled") or fs.next_at > now:
            continue
        if sleeping(cfg, now):
            lo = max(config.FLOOR_MIN, int(cfg[f"{feed}_min"]))
            fs.next_at = _wake_time(cfg, now) + random.uniform(0, lo * 60)
            note(f"睡眠时段，{config.FEEDS[feed]}推到 {time.strftime('%m-%d %H:%M', time.localtime(fs.next_at))}")
            continue
        return feed, False
    return None


async def loop() -> None:
    try:
        await browser.start()
        state.account = "ok" if browser.signed_in_cache else "need_login"
        note("浏览器已启动，" + ("已登录 X" if browser.signed_in_cache else "还没登录 X：到“登录”页登录"))
    except Exception as e:
        note(f"浏览器启动失败：{type(e).__name__}: {e}")
    reschedule(first=True)
    while True:
        _wake.clear()
        cfg = config.load()
        job = _next_job(cfg)
        if job and not feed_on(cfg, job[0]):
            continue
        if job:
            try:
                await run(*job)
            except Exception as e:  # 一次出错不影响以后 / One failure must not stop the loop
                note(f"拉取异常：{type(e).__name__}: {e}")
                state.running = ""
            continue
        due = [fs.next_at for f, fs in state.feeds.items() if feed_on(cfg, f) and cfg.get(f"{f}_enabled")]
        timeout = min([60.0] + [max(1.0, t - time.time()) for t in due])
        try:
            await asyncio.wait_for(_wake.wait(), timeout=timeout)
        except asyncio.TimeoutError:
            pass
