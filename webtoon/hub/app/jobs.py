"""转换队列和缓存：设备问某一话时，没转过就排队下载原图、切页、打包，转好的 CBZ 放在 /data/cache 一段时间。

同一话只转一次，任务一个一个做（不同时抢 WEBTOON）。原图转完就丢，只留 CBZ；超过保留天数或总大小时从最久没用的删。

Conversion queue and cache: when a device asks for an episode that is not converted yet, it is queued to download
the source images, cut pages and pack; converted CBZs stay under /data/cache for a while. Each episode converts once
and jobs run one at a time (never hammering WEBTOON). Source images are dropped after conversion, only the CBZ is
kept; past the retention days or size cap the least recently used go first.
"""
from __future__ import annotations

import asyncio
import os
import time
from pathlib import Path

from . import config, convert, source

_state: dict[tuple[str, str, int], dict] = {}
_queue: asyncio.Queue | None = None
recent: list[dict] = []          # 面板显示最近做过的 / Recent jobs for the panel


def cbz_path(lang: str, tid: str, no: int) -> Path:
    return config.CACHE_DIR / lang / tid / f"{int(no)}.cbz"


def cover_path(lang: str, tid: str) -> Path:
    return config.CACHE_DIR / "covers" / f"{lang}_{tid}.jpg"


def _forget_errors() -> None:
    """出错记录两分钟后就没用了（会重试），清掉免得越积越多。/ Error records are useless after two minutes (a retry
    replaces them); drop them so they do not pile up."""
    now = time.time()
    for k in [k for k, v in _state.items() if v["state"] == "error" and now - v["at"] > 120]:
        del _state[k]


def status(lang: str, tid: str, no: int, start: bool = True) -> dict:
    """这一话的状态：ready（附大小）、pending（附进度）或 error；不在队列里又没转过时开始转。
    An episode's state: ready (with size), pending (with progress) or error; starts converting when neither queued
    nor converted."""
    path = cbz_path(lang, tid, no)
    if path.exists():
        return {"state": "ready", "size": path.stat().st_size}
    key = (lang, tid, int(no))
    st = _state.get(key)
    if st and st["state"] == "error" and time.time() - st["at"] > 120:
        st = None                    # 出错两分钟后允许重试 / Allow a retry two minutes after an error
    if st:
        return dict(st)
    if not start:
        return {"state": "none"}
    st = {"state": "pending", "done": 0, "total": 0, "at": time.time(), "queued": _queue.qsize() if _queue else 0}
    _state[key] = st
    _queue.put_nowait(key)
    return dict(st)


async def _convert(key: tuple[str, str, int], meta: dict) -> None:
    lang, tid, no = key
    st = _state[key]
    cfg = config.load()
    urls = await source.images(lang, tid, no)
    st["total"] = len(urls)
    path = cbz_path(lang, tid, no)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".part")
    # 切好的页直接压成 JPEG 写进 .part，内存里只留正在切的那一段。/ Finished pages go straight into the .part as
    # JPEGs; only the strip being cut stays in memory.
    writer = convert.CbzWriter(tmp, cfg["quality"])
    try:
        pager = convert.Pager(cfg["page_w"], cfg["page_h"], sink=writer.add)
        for i, url in enumerate(urls):
            data = await source.get_image(url)
            await asyncio.to_thread(pager.feed, data)
            del data
            st["done"] = i + 1
        rest = await asyncio.to_thread(pager.finish)
        if not pager.count:
            raise source.SourceError(502, "这一话没有可用的图片")
        for page in rest:
            await asyncio.to_thread(writer.add, page)
        await asyncio.to_thread(writer.close, {**meta, "no": no, "lang": lang})
        os.replace(tmp, path)
    except BaseException:
        writer.abort()
        tmp.unlink(missing_ok=True)
        raise


async def worker() -> None:
    global _queue
    _queue = asyncio.Queue()
    while True:
        key = await _queue.get()
        lang, tid, no = key
        t0 = time.time()
        meta = {"web": f"{config.BASE}/{lang}/x/x/list?title_no={tid.lstrip('c')}"}
        try:
            try:
                info = await source.title(lang, tid)
                ep = next((e for e in info["episodes"] if e["no"] == no), None)
                meta.update(series=info["title"], author=info["author"], title=ep["title"] if ep else "")
            except source.SourceError:
                pass
            await _convert(key, meta)
            _state.pop(key, None)
            note = "完成"
        except source.SourceError as e:
            _state[key].update(state="error", error=str(e), at=time.time())
            note = f"失败：{e}"
        except Exception as e:  # noqa: BLE001
            _state[key].update(state="error", error=f"转换出错：{e.__class__.__name__}", at=time.time())
            note = f"出错：{e!r}"
        _forget_errors()
        recent.insert(0, {"lang": lang, "id": tid, "no": no, "title": meta.get("series", ""), "note": note,
                          "sec": round(time.time() - t0, 1), "at": int(time.time())})
        del recent[30:]


def cache_mb() -> float:
    return sum(p.stat().st_size for p in config.CACHE_DIR.rglob("*") if p.is_file()) / 1048576


def prune() -> int:
    """删掉超过保留天数的，再按最久没用删到总大小以内。返回删了几个。/ Delete files past the retention days, then the
    least recently used until under the size cap. Returns how many were deleted."""
    cfg = config.load()
    files = [p for p in config.CACHE_DIR.rglob("*") if p.is_file() and p.suffix in (".cbz", ".jpg", ".part")]
    now, n = time.time(), 0
    keep = []
    for p in files:
        st = p.stat()
        if now - max(st.st_mtime, st.st_atime) > cfg["keep_days"] * 86400:
            p.unlink(missing_ok=True)
            n += 1
        else:
            keep.append((max(st.st_mtime, st.st_atime), st.st_size, p))
    total = sum(s for _, s, _ in keep)
    for _, size, p in sorted(keep):
        if total <= cfg["max_mb"] * 1048576:
            break
        p.unlink(missing_ok=True)
        total -= size
        n += 1
    return n


async def pruner() -> None:
    while True:
        try:
            await asyncio.to_thread(prune)
        except Exception as e:  # noqa: BLE001
            print("[webtoon] 清理缓存失败:", e, flush=True)
        await asyncio.sleep(3600)


async def cover(lang: str, tid: str, url: str) -> Path | None:
    """发现页封面：第一次要时下载并转成灰度小图。/ Store cover: downloaded and grayed the first time it is asked for."""
    path = cover_path(lang, tid)
    if path.exists():
        return path
    if not url:
        return None
    try:
        data = await source.get_image(url, pace=False)
        out = await asyncio.to_thread(convert.cover, data)
    except Exception:  # noqa: BLE001
        return None
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(out)
    return path
