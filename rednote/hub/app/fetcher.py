"""定时拉取：逐个博主读主页 → 跳过视频 → 新的图文笔记拉详情 → 处理图片。请求之间随机等待，有每日上限，
碰到验证或登录失效就暂停，等面板处理。

Scheduled fetching: read each creator's profile, skip videos, fetch details of new image notes, process images.
Requests are spaced randomly with a daily cap; verification or an expired login pauses until the panel handles it.
"""
from __future__ import annotations

import asyncio
import random
import re
import time
from collections import deque
from typing import Any

from . import config, images, mcp, store

log: deque[str] = deque(maxlen=200)


def note(msg: str) -> None:
    log.appendleft(time.strftime("%m-%d %H:%M:%S ") + msg)


class State:
    running = False
    paused = ""          # 非空时暂停，写着原因 / Paused with this reason when not empty
    last_round = 0.0
    next_round = 0.0
    current = ""
    new_notes = 0


state = State()
_wake = asyncio.Event()

PAUSE_WORDS = ("验证", "captcha", "未登录", "登录", "login", "频繁", "安全")


def wake() -> None:
    _wake.set()


def resume() -> None:
    state.paused = ""
    wake()


async def _pause_if_blocked(err: str) -> bool:
    if any(w in err for w in PAUSE_WORDS):
        state.paused = f"小红书要求验证或登录：{err[:80]}"
        note("已暂停：" + state.paused)
        return True
    return False


async def _gap(cfg: dict[str, Any]) -> None:
    lo, hi = int(cfg["delay_min"]), int(cfg["delay_max"])
    await asyncio.sleep(random.uniform(lo, max(lo, hi)))


def _budget(cfg: dict[str, Any]) -> bool:
    return store.requests_today() < int(cfg["daily_cap"])


# 平时每篇存的评论条数；“加载更多评论”时存的条数。/ Comments kept per note normally, and after "load more".
COMMENTS_KEEP = 20
COMMENTS_MORE = 60


def parse_comments(data: dict[str, Any], keep: int) -> list[dict[str, Any]]:
    out = []
    for c in ((data.get("comments") or {}).get("list") or [])[:keep]:
        out.append({
            "user": (c.get("userInfo") or {}).get("nickname") or "",
            "text": c.get("content") or "",
            "likes": c.get("likeCount") or "",
        })
    return out


def count_value(s: Any) -> int:
    """小红书的计数文字（"12"、"1.2万"、"10+"）换成大约的数。/ Xiaohongshu count text ("12", "1.2万", "10+") as a number."""
    m = re.match(r"\s*([\d.]+)\s*(万|w|W)?", str(s or ""))
    if not m:
        return 0
    try:
        v = float(m.group(1))
    except ValueError:
        return 0
    return int(v * 10000) if m.group(2) else int(v)


async def fetch_note(note_id: str, token: str, cfg: dict[str, Any], source: str = "follow",
                     background: bool = False) -> bool:
    """拉一篇详情、处理图片并入库；视频返回 False。background 时只要小红书给了图片宽高，就先入库返回，图片在后台做。
    Fetch one detail, process images and save; False for videos. With background, as long as Xiaohongshu reports
    image sizes, save and return first and let the images finish in the background."""
    store.count_request("rec" if source == "rec" else "")
    data = await mcp.detail(note_id, token)
    n = data.get("note") or {}
    if (n.get("type") or "normal") != "normal":
        return False
    shots = [i for i in (n.get("imageList") or []) if i.get("urlDefault") or i.get("url")][: int(cfg["max_images"])]
    urls = [i.get("urlDefault") or i.get("url") for i in shots]
    counts = images.plan(shots)
    if background and counts is not None:
        pages = sum(counts)
        images.start_background(note_id, urls, counts)
    else:
        pages = await images.process(note_id, urls, counts)
    comments = parse_comments(data, COMMENTS_KEEP)
    user = n.get("user") or {}
    store.save_note({
        "id": note_id, "user_id": user.get("userId") or "", "author": user.get("nickname") or "",
        "title": n.get("title") or "", "body": n.get("desc") or "", "time": int(n.get("time") or 0),
        "likes": (n.get("interactInfo") or {}).get("likedCount") or "",
        "comment_count": (n.get("interactInfo") or {}).get("commentCount") or "",
        "comments": comments, "images": pages, "xsec_token": token,
        "ratio": images.cover_ratio(int((shots[0] if shots else {}).get("width") or 0),
                                    int((shots[0] if shots else {}).get("height") or 0)),
    }, source)
    return True


async def run_round() -> None:
    cfg = config.load()
    state.running = True
    state.new_notes = 0
    note("开始一轮拉取")
    try:
        for c in store.creators():
            if state.paused or not _budget(cfg):
                break
            state.current = c["nickname"] or c["user_id"]
            try:
                store.count_request()
                prof = await mcp.profile(c["user_id"], c["xsec_token"])
            except mcp.McpError as e:
                store.mark_creator(c["user_id"], None, str(e))
                note(f"{state.current}：主页读取失败 {e}")
                if await _pause_if_blocked(str(e)):
                    break
                await _gap(cfg)
                continue
            nickname = (prof.get("userBasicInfo") or {}).get("nickname")
            feeds = prof.get("feeds") or []
            # 列表阶段就跳过视频。/ Skip videos at the list stage.
            images_only = [f for f in feeds if (f.get("noteCard") or {}).get("type") == "normal"]
            for f in images_only:
                if store.note_source(f["id"]) == "rec":
                    store.set_source(f["id"], "follow")
                # 旧笔记补上令牌，之后才能加载更多评论。/ Give old notes a token so more comments can be loaded.
                if f.get("xsecToken"):
                    store.fill_token(f["id"], f["xsecToken"])
            fresh = [f for f in images_only if not store.has_note(f["id"])]
            fresh = fresh[: int(cfg["per_creator"])]
            # 记下最新一篇的令牌，主页令牌过期时可以用它。/ Keep a recent token in case the profile token expires.
            store.mark_creator(c["user_id"], nickname, "", feeds[0].get("xsecToken") if feeds else None)
            note(f"{nickname or state.current}：{len(feeds)} 篇，其中 {len(fresh)} 篇新图文")
            for item in fresh:
                if state.paused or not _budget(cfg):
                    break
                await _gap(cfg)
                try:
                    if await fetch_note(item["id"], item.get("xsecToken") or "", cfg):
                        state.new_notes += 1
                except mcp.McpError as e:
                    note(f"笔记 {item['id']} 失败：{e}")
                    if await _pause_if_blocked(str(e)):
                        break
            await _gap(cfg)
        cleanup(cfg)
    finally:
        state.running = False
        state.current = ""
        state.last_round = time.time()
        note(f"本轮结束，新增 {state.new_notes} 篇" + ("（已到今日上限）" if not _budget(cfg) else ""))


def cleanup(cfg: dict[str, Any]) -> None:
    """按天数和总大小清理旧笔记。/ Drop old notes by age and total size."""
    for note_id in store.old_notes(time.time() - int(cfg["keep_days"]) * 86400):
        images.remove(note_id)
        store.delete_note(note_id)
    while images.total_mb() > int(cfg["max_mb"]) and store.note_count():
        for note_id in store.oldest_notes(10):
            images.remove(note_id)
            store.delete_note(note_id)


async def loop() -> None:
    while True:
        cfg = config.load()
        state.next_round = time.time() + int(cfg["interval_min"]) * 60
        # 先清再跑：本轮进行中点的“立即拉一轮”留给下一轮，不会被丢掉。/ Clear before running so a "run now" pressed
        # during the round starts the next one instead of being lost.
        _wake.clear()
        if not state.paused and store.creators():
            try:
                await run_round()
            except Exception as e:  # 一轮出错不影响下一轮 / One failed round must not stop the loop
                note(f"本轮异常：{type(e).__name__}: {e}")
            state.next_round = time.time() + int(cfg["interval_min"]) * 60
        try:
            await asyncio.wait_for(_wake.wait(), timeout=max(5.0, state.next_round - time.time()))
        except asyncio.TimeoutError:
            pass
