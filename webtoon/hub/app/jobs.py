"""转换队列和缓存：设备问某一话时，没转过就排队下载原图、切页、打包，转好的 CBZ 放在 /data/cache 一段时间。

同一话只转一次，任务一个一个做（不同时抢 WEBTOON）。原图转完就丢，只留 CBZ；超过保留天数或总大小时从最久没用的删。

Conversion queue and cache: when a device asks for an episode that is not converted yet, it is queued to download
the source images, cut pages and pack; converted CBZs stay under /data/cache for a while. Each episode converts once
and jobs run one at a time (never hammering WEBTOON). Source images are dropped after conversion, only the CBZ is
kept; past the retention days or size cap the least recently used go first.
"""
from __future__ import annotations

import asyncio
import itertools
import os
import time
from pathlib import Path

from . import config, convert, ehentai, mihon, source

_state: dict[tuple[str, str, int], dict] = {}
# 来源名 → (时间, 错误)：一个插件来源卡在 Cloudflare 验证上、或 E-Hentai 额度用完 / 封 IP / 没登录后，这段时间里它
# 排着的话直接失败，不再一话一话等超时，别的来源不被拖住。/ Source name -> (time, error): once an extension source
# fails a Cloudflare check, or E-Hentai runs out of quota / bans the IP / is not signed in, its queued episodes fail at
# once for a while instead of each waiting to time out, so other sources are not held up.
_cf_block: dict[str, tuple[float, str]] = {}
CF_PAUSE = 600
# 队列按 (优先级, 序号, 话) 排：设备和 App 要的是 0，面板选的是 1，所以面板一次排一大批也不会让设备等到超时。
# The queue is ordered by (priority, sequence, episode): device and app requests are 0 and panel picks 1, so a large
# panel batch never makes a device wait until it times out.
_queue: asyncio.PriorityQueue | None = None
_seq = itertools.count()
_running: tuple[str, str, int] | None = None   # 正在转的那一话 / The episode being converted
recent: list[dict] = []          # 面板显示最近做过的 / Recent jobs for the panel


def is_eh(tid: str) -> bool:
    return bool(ehentai.ID_RE.match(tid))


def is_mihon(tid: str) -> bool:
    return tid.startswith("m") and not is_eh(tid)


def is_ext(tid: str) -> bool:
    """不是 WEBTOON 的作品（插件或 E-Hentai）：编号以 m 开头。/ Not a WEBTOON title (extension or E-Hentai): the id
    starts with m."""
    return tid.startswith("m")


async def _info(lang: str, tid: str) -> dict:
    if is_eh(tid):
        return await ehentai.title(tid)
    return await (mihon.title(tid) if is_mihon(tid) else source.title(lang, tid))


async def _images(lang: str, tid: str, no: int) -> list[str]:
    if is_eh(tid):
        return await ehentai.images(tid, no)
    return await (mihon.images(tid, no) if is_mihon(tid) else source.images(lang, tid, no))


async def _fetch(tid: str, url: str, pace: bool = True) -> bytes:
    if is_eh(tid):
        return await ehentai.get_image(url, pace)
    return await (mihon.get_image(url, pace) if is_mihon(tid) else source.get_image(url, pace))


def _blocking(tid: str, msg: str) -> bool:
    return ehentai.is_blocking(msg) if is_eh(tid) else mihon.is_cloudflare(msg)


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


def _put(key: tuple[str, str, int], panel: bool) -> None:
    _queue.put_nowait((1 if panel else 0, next(_seq), key))


def status(lang: str, tid: str, no: int, start: bool = True, panel: bool = False) -> dict:
    """这一话的状态：ready（附大小）、pending（附进度）或 error；不在队列里又没转过时开始转。设备问到面板排着的话时
    把它提到前面。/ An episode's state: ready (with size), pending (with progress) or error; starts converting when
    neither queued nor converted. A device asking for a panel-queued episode moves it to the front."""
    path = cbz_path(lang, tid, no)
    if path.exists():
        return {"state": "ready", "size": path.stat().st_size}
    key = (lang, tid, int(no))
    st = _state.get(key)
    if st and st["state"] == "error" and time.time() - st["at"] > 120:
        st = None                    # 出错两分钟后允许重试 / Allow a retry two minutes after an error
    if st:
        if start and not panel and st["state"] == "pending" and st.get("panel") and key != _running:
            st["panel"] = False
            _put(key, False)         # 旧条目留在后面，轮到时会跳过 / The old entry stays behind and is skipped
        return {k: v for k, v in st.items() if k != "panel"}
    if not start:
        return {"state": "none"}
    st = {"state": "pending", "done": 0, "total": 0, "at": time.time(), "queued": pending(), "panel": panel}
    _state[key] = st
    _put(key, panel)
    return {k: v for k, v in st.items() if k != "panel"}


def enqueue(lang: str, tid: str, no: int) -> str:
    """面板选话：转好了给 ready，已经在排给 pending，否则排进队列给 added（刚出错的也马上重排）。/ Panel selection:
    ready when converted, pending when already queued, otherwise queued and added (a fresh error is retried at once)."""
    path = cbz_path(lang, tid, no)
    if path.exists():
        return "ready"
    key = (lang, tid, int(no))
    st = _state.get(key)
    if st and st["state"] == "pending":
        return "pending"
    _state.pop(key, None)
    status(lang, tid, no, panel=True)
    return "added"


def pending() -> int:
    """排着和正在转的话数（取消的不算，队列里可能还留着它们的旧条目）。/ Episodes queued or converting (cancelled ones
    excluded, though their stale entries may still sit in the queue)."""
    return sum(1 for v in _state.values() if v["state"] == "pending")


def states(lang: str, tid: str) -> dict[int, dict]:
    """一部作品各话的状态：转好的、排着的、正在转的、出错的；别的话不出现。不会开始转。/ Per-episode state of one
    title: converted, queued, converting or failed; other episodes are absent. Never starts a conversion."""
    out: dict[int, dict] = {}
    d = config.CACHE_DIR / lang / tid
    if d.is_dir():
        for p in d.glob("*.cbz"):
            if p.stem.isdigit():
                out[int(p.stem)] = {"state": "ready", "size": p.stat().st_size}
    for (l, t, no), st in list(_state.items()):
        if l == lang and t == tid and no not in out:
            out[no] = {k: v for k, v in st.items() if k != "panel"}
            if (l, t, no) == _running:
                out[no]["running"] = True
    return out


def cancel(lang: str, tid: str) -> int:
    """取消这部作品面板排的、还没开始转的话；正在转的那一话和设备要的不动。返回取消了几话。/ Cancel this title's
    panel-queued episodes that have not started; the one converting and device requests stay. Returns how many."""
    keys = [k for k, v in _state.items()
            if k[0] == lang and k[1] == tid and v["state"] == "pending" and v.get("panel") and k != _running]
    for k in keys:
        del _state[k]
    return len(keys)


async def _convert(key: tuple[str, str, int], meta: dict) -> None:
    lang, tid, no = key
    st = _state[key]
    cfg = config.load()
    urls = await _images(lang, tid, no)
    st["total"] = len(urls)
    path = cbz_path(lang, tid, no)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".part")
    # 切好的页直接压成 JPEG 写进 .part，内存里只留正在切的那一段。/ Finished pages go straight into the .part as
    # JPEGs; only the strip being cut stays in memory.
    writer = convert.CbzWriter(tmp, cfg["quality"])
    try:
        # WEBTOON 一定是竖长条；插件和 E-Hentai 先看前几张图再决定。/ WEBTOON is always a tall strip; extension and
        # E-Hentai sources look at the first few images before deciding.
        spread = cfg["eh_spread"] if is_eh(tid) else cfg["mihon_spread"]
        if is_ext(tid):
            pager = convert.AutoPager(cfg["page_w"], cfg["page_h"], sink=writer.add, spread=spread)
        else:
            pager = convert.Pager(cfg["page_w"], cfg["page_h"], sink=writer.add)
        for i, url in enumerate(urls):
            data = await _fetch(tid, url)
            await asyncio.to_thread(pager.feed, data)
            del data
            st["done"] = i + 1
        rest = await asyncio.to_thread(pager.finish)
        if not pager.count:
            raise source.SourceError(502, "这一话没有可用的图片")
        for page in rest:
            await asyncio.to_thread(writer.add, page)
        strip = not isinstance(pager, convert.AutoPager) or pager.strip
        rtl = not strip and spread == "split_rtl"
        await asyncio.to_thread(writer.close, {**meta, "no": no, "lang": "" if is_ext(tid) else lang,
                                               "format": "Webtoon" if strip else "",
                                               "manga": "YesAndRightToLeft" if rtl else "No"})
        os.replace(tmp, path)
    except BaseException:
        writer.abort()
        tmp.unlink(missing_ok=True)
        raise


async def worker() -> None:
    global _queue, _running
    _queue = asyncio.PriorityQueue()
    while True:
        _, _, key = await _queue.get()
        lang, tid, no = key
        # 取消了的、或者同一话重排过已经转好的，跳过。/ Skip cancelled entries and re-queued duplicates already done.
        st = _state.get(key)
        if not st or st["state"] != "pending" or cbz_path(lang, tid, no).exists():
            if st and st["state"] == "pending":
                _state.pop(key, None)
            continue
        _running = key
        t0 = time.time()
        meta = {"web": "" if is_ext(tid) else f"{config.BASE}/{lang}/x/x/list?title_no={tid.lstrip('c')}"}
        src = ""
        try:
            try:
                info = await _info(lang, tid)
                meta["web"] = meta["web"] or info.get("web", "")
                ep = next((e for e in info["episodes"] if e["no"] == no), None)
                meta.update(series=info["title"], author=info["author"], title=ep["title"] if ep else "")
                src = info.get("src", "") if is_ext(tid) else ""
            except source.SourceError:
                pass
            blocked = _cf_block.get(src) if src else None
            if blocked and time.time() - blocked[0] < CF_PAUSE:
                left = max(1, round((CF_PAUSE - (time.time() - blocked[0])) / 60))
                raise source.SourceError(502, f"{blocked[1]}；约 {left} 分钟后再试")
            try:
                await _convert(key, meta)
            except source.SourceError as e:
                if src and _blocking(tid, str(e)):
                    _cf_block[src] = (time.time(), str(e))
                raise
            _state.pop(key, None)
            note = "完成"
        except source.SourceError as e:
            _state[key].update(state="error", error=str(e), at=time.time())
            note = f"失败：{e}"
        except Exception as e:  # noqa: BLE001
            _state[key].update(state="error", error=f"转换出错：{e.__class__.__name__}", at=time.time())
            note = f"出错：{e!r}"
        _running = None
        _forget_errors()
        recent.insert(0, {"lang": lang, "id": tid, "no": no, "title": meta.get("series", ""), "note": note,
                          "sec": round(time.time() - t0, 1), "at": int(time.time())})
        del recent[30:]


def cache_mb() -> float:
    return sum(p.stat().st_size for p in config.CACHE_DIR.rglob("*") if p.is_file()) / 1048576


_meta_seen: dict[str, tuple[float, int, dict]] = {}   # 路径 → (修改时间, 大小, 信息)，少开几次 ZIP / path -> (mtime, size, info)


def _comic_info(p: Path, st: os.stat_result) -> dict:
    """读 CBZ 末尾 ComicInfo.xml 里的系列名、话名、页数；读不出来就给空。/ Series, episode title and page count from the
    CBZ's ComicInfo.xml; empty when unreadable."""
    hit = _meta_seen.get(str(p))
    if hit and hit[0] == st.st_mtime and hit[1] == st.st_size:
        return hit[2]
    info = {"series": "", "title": "", "pages": 0}
    try:
        import zipfile
        from xml.etree import ElementTree
        with zipfile.ZipFile(p) as z:
            root = ElementTree.fromstring(z.read("ComicInfo.xml"))
            info = {"series": root.findtext("Series") or "", "title": root.findtext("Title") or "",
                    "pages": int(root.findtext("PageCount") or 0)}
    except Exception:  # noqa: BLE001
        pass
    _meta_seen[str(p)] = (st.st_mtime, st.st_size, info)
    return info


def list_cache() -> list[dict]:
    """缓存里所有转好的话，新的在前。路径是 缓存/语言/作品编号/话号.cbz，封面和 .part 不算。/ Every converted episode in
    the cache, newest first. Layout is cache/lang/id/no.cbz; covers and .part files are not episodes."""
    items = []
    for p in config.CACHE_DIR.glob("*/*/*.cbz"):
        if p.parent.parent.name == "covers" or not p.stem.isdigit():
            continue
        try:
            st = p.stat()
        except OSError:
            continue
        items.append({"lang": p.parent.parent.name, "id": p.parent.name, "no": int(p.stem), "mb": round(st.st_size / 1048576, 1),
                      "at": int(st.st_mtime), **_comic_info(p, st)})
    items.sort(key=lambda i: -i["at"])
    live = {str(p) for p in config.CACHE_DIR.glob("*/*/*.cbz")}
    for k in [k for k in _meta_seen if k not in live]:
        del _meta_seen[k]
    return items


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
        data = await _fetch(tid, url, pace=False)
        out = await asyncio.to_thread(convert.cover, data)
    except Exception:  # noqa: BLE001
        return None
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(out)
    return path
