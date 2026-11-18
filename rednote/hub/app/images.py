"""把笔记图片处理成墨水屏能直接用的灰度 JPEG：不超过 648×1000、太长按屏切开、普通（基线）JPEG；另出一张 150×200 缩略图。

图片同时下载、处理 4 张。小红书给了每张图的宽高时，先按宽高算好每张切几屏、整篇共几页，这样正文可以先返回，
图片在后台做；取某一页时还没做好就等它（wait_page）。

Turn note images into grayscale JPEGs the e-ink device can show as is: at most 648x1000, very tall images split into
screens, baseline JPEG; plus a 150x200 thumbnail.

Four images download and process at once. When Xiaohongshu reports each image's size, the page split is planned up
front, so the note can be returned first while images finish in the background; a request for a page not ready yet
waits for it (wait_page).
"""
from __future__ import annotations

import asyncio
import io
import math
import shutil
import time
from pathlib import Path

import httpx
from PIL import Image, ImageOps

from . import config

MAX_W, MAX_H = 648, 1000
THUMB_W = 300
# 封面高宽比夹在 3:4（横）和 4:3（竖）之间，和小红书首页卡片一样。/ Cover height/width clamped between 3:4 (wide)
# and 4:3 (tall), like Xiaohongshu home cards.
RATIO_MIN, RATIO_MAX = 0.75, 4 / 3
# 屏幕可用区域的高宽比；比它高 1.5 倍以上的图按屏切开。/ Screen aspect; images 1.5x taller are split per screen.
SCREEN_RATIO = 1160 / 684
HEADERS = {"User-Agent": "Mozilla/5.0", "Referer": "https://www.xiaohongshu.com/"}
PARALLEL = 4
WAIT_PAGE_S = 40

# 正在后台处理的笔记 → 处理完成事件。/ Notes being processed in the background -> completion event.
_processing: dict[str, asyncio.Event] = {}


def note_dir(note_id: str) -> Path:
    return config.NOTES_DIR / note_id


def _save(img: Image.Image, path: Path) -> None:
    tmp = path.with_suffix(".tmp")
    img.save(tmp, "JPEG", quality=80, optimize=True, progressive=False)
    tmp.replace(path)


def cover_ratio(w: int, h: int) -> float:
    """封面高宽比（按原图，夹在 3:4～4:3）；不知道宽高就当 4:3 竖图。/ Cover height/width from the original, clamped
    to 3:4..4:3; unknown sizes count as a 4:3 portrait."""
    if not w or not h:
        return RATIO_MAX
    return round(min(RATIO_MAX, max(RATIO_MIN, h / w)), 3)


def cover(img: Image.Image) -> Image.Image:
    """宽 300、保留原图比例（夹在 3:4～4:3）的灰度封面。/ A 300-wide gray cover keeping the original ratio
    (clamped to 3:4..4:3)."""
    r = cover_ratio(*img.size)
    return ImageOps.fit(img, (THUMB_W, round(THUMB_W * r)), Image.Resampling.LANCZOS)


def split_count(w: int, h: int) -> int:
    """按宽高算一张图切几屏（只看高宽比）。/ How many screens an image splits into (aspect ratio only)."""
    if not w or not h or h / w <= SCREEN_RATIO * 1.5:
        return 1
    step = w * SCREEN_RATIO
    n = math.ceil(h / step)
    # 最后一小截并进上一屏。/ Merge a tiny last strip into the previous screen.
    if n > 1 and h - (n - 1) * step < step * 0.2:
        n -= 1
    return max(1, n)


def plan(image_list: list[dict]) -> list[int] | None:
    """小红书给的 imageList 里都有宽高时，返回每张切几屏；缺任何一张就返回 None。
    Pages per image when every imageList entry has a size; None if any is missing."""
    out = []
    for i in image_list:
        w, h = int(i.get("width") or 0), int(i.get("height") or 0)
        if not w or not h:
            return None
        out.append(split_count(w, h))
    return out


def _pages(img: Image.Image, n: int) -> list[Image.Image]:
    """切成正好 n 屏（等高）。/ Split into exactly n equal-height screens."""
    if n <= 1:
        return [img]
    w, h = img.size
    return [img.crop((0, h * k // n, w, h * (k + 1) // n)) for k in range(n)]


async def _fetch(client: httpx.AsyncClient, url: str) -> Image.Image | None:
    try:
        resp = await client.get(url)
        resp.raise_for_status()
        return await asyncio.to_thread(lambda: ImageOps.exif_transpose(Image.open(io.BytesIO(resp.content))).convert("L"))
    except (httpx.HTTPError, OSError):
        return None


def _write_pages(out: Path, img: Image.Image, first: int, n: int, thumb: bool) -> None:
    if thumb:
        _save(cover(img), out / "thumb.jpg")
    for k, page in enumerate(_pages(img, n)):
        page.thumbnail((MAX_W, MAX_H), Image.Resampling.LANCZOS)
        _save(page, out / f"{first + k}.jpg")


async def process(note_id: str, urls: list[str], counts: list[int] | None = None) -> int:
    """下载并处理图片，返回页数。counts 是预先算好的每张屏数（和 urls 一一对应），没有就按实际图算。
    Download and process images; returns the page count. counts is the planned screens per image (matching urls);
    without it the split follows the downloaded images."""
    out = note_dir(note_id)
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)
    sem = asyncio.Semaphore(PARALLEL)
    async with httpx.AsyncClient(timeout=30.0, headers=HEADERS, follow_redirects=True) as client:
        async def one(i: int, url: str) -> Image.Image | None:
            async with sem:
                img = await _fetch(client, url)
                if img is not None and counts is not None:
                    first = 1 + sum(counts[:i])
                    await asyncio.to_thread(_write_pages, out, img, first, counts[i], i == 0)
                    return None
                return img

        results = await asyncio.gather(*(one(i, u) for i, u in enumerate(urls)))
    if counts is not None:
        return sum(counts)
    # 没有预先的屏数：图都到了再按顺序编号。/ No plan: number pages in order once every image is in.
    page, thumb_done = 1, False
    for img in results:
        if img is None:
            continue
        n = split_count(*img.size)
        await asyncio.to_thread(_write_pages, out, img, page, n, not thumb_done)
        thumb_done = True
        page += n
    return page - 1


def start_background(note_id: str, urls: list[str], counts: list[int]) -> None:
    """在后台处理图片；取页时用 wait_page 等。/ Process images in the background; wait_page waits for pages."""
    event = asyncio.Event()
    _processing[note_id] = event

    async def run() -> None:
        try:
            await process(note_id, urls, counts)
        finally:
            event.set()
            _processing.pop(note_id, None)

    asyncio.create_task(run())


async def wait_page(note_id: str, path: Path) -> bool:
    """这页还在后台做：等它出现或整篇做完（最多 WAIT_PAGE_S 秒）。/ Wait for a page still being made, until it
    appears or the note finishes (at most WAIT_PAGE_S seconds)."""
    deadline = time.monotonic() + WAIT_PAGE_S
    while not path.exists():
        event = _processing.get(note_id)
        if event is None or time.monotonic() > deadline:
            return path.exists()
        try:
            await asyncio.wait_for(event.wait(), timeout=0.3)
        except asyncio.TimeoutError:
            pass
    return True


def remove(note_id: str) -> None:
    if note_id in _processing:
        return   # 正在做的不删 / Never remove a note being processed
    shutil.rmtree(note_dir(note_id), ignore_errors=True)


def total_mb() -> float:
    return sum(p.stat().st_size for p in config.NOTES_DIR.rglob("*.jpg")) / 1048576
