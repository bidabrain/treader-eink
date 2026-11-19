"""把推文图片处理成墨水屏能直接用的灰度 JPEG（和微博 hub 一样）：大图不超过 648×1000、太长按屏切开、基线 JPEG；
一张 300 宽封面 thumb.jpg；每张原图一张 240 方形小图 s<k>.jpg。图片经浏览器同一出口下载。

Turn post images into grayscale JPEGs the e-ink device can show as is (same as the Weibo hub): pages at most 648x1000,
very tall images split per screen, baseline JPEG; a 300-wide cover thumb.jpg; and a 240 square s<k>.jpg per source
image. Images are downloaded through the browser's own exit.
"""
from __future__ import annotations

import asyncio
import io
import math
import shutil
from pathlib import Path
from typing import Awaitable, Callable

from PIL import Image, ImageOps

from . import config

MAX_W, MAX_H = 648, 1000
THUMB_W = 300
SQUARE = 240
RATIO_MIN, RATIO_MAX = 0.75, 4 / 3
SCREEN_RATIO = 1160 / 684
PARALLEL = 4

Fetch = Callable[[str], Awaitable[bytes | None]]


def post_dir(pid: str) -> Path:
    return config.POSTS_DIR / pid


def _save(img: Image.Image, path: Path) -> None:
    tmp = path.with_suffix(".tmp")
    img.save(tmp, "JPEG", quality=80, optimize=True, progressive=False)
    tmp.replace(path)


def cover_ratio(w: int, h: int) -> float:
    if not w or not h:
        return RATIO_MAX
    return round(min(RATIO_MAX, max(RATIO_MIN, h / w)), 3)


def split_count(w: int, h: int) -> int:
    if not w or not h or h / w <= SCREEN_RATIO * 1.5:
        return 1
    step = w * SCREEN_RATIO
    n = math.ceil(h / step)
    if n > 1 and h - (n - 1) * step < step * 0.2:
        n -= 1
    return max(1, n)


def _pages(img: Image.Image, n: int) -> list[Image.Image]:
    if n <= 1:
        return [img]
    w, h = img.size
    return [img.crop((0, h * k // n, w, h * (k + 1) // n)) for k in range(n)]


def _decode(data: bytes) -> Image.Image | None:
    try:
        return ImageOps.exif_transpose(Image.open(io.BytesIO(data))).convert("L")
    except OSError:
        return None


def _write(out: Path, img: Image.Image, first: int, n: int, k: int) -> None:
    if k == 1:
        r = cover_ratio(*img.size)
        _save(ImageOps.fit(img, (THUMB_W, round(THUMB_W * r)), Image.Resampling.LANCZOS), out / "thumb.jpg")
    _save(ImageOps.fit(img, (SQUARE, SQUARE), Image.Resampling.LANCZOS), out / f"s{k}.jpg")
    for i, page in enumerate(_pages(img, n)):
        page.thumbnail((MAX_W, MAX_H), Image.Resampling.LANCZOS)
        _save(page, out / f"{first + i}.jpg")


async def process_post(pid: str, urls: list[str], fetch: Fetch) -> tuple[int, list[int], float]:
    """下载并处理；返回页数、每张原图的第一页（从 0 起）和封面高宽比。下载失败的图不算。
    Download and process; returns the page count, each source image's first page (0-based) and the cover ratio.
    Failed downloads are skipped."""
    out = post_dir(pid)
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)
    sem = asyncio.Semaphore(PARALLEL)

    async def one(url: str) -> Image.Image | None:
        async with sem:
            data = await fetch(url)
            return await asyncio.to_thread(_decode, data) if data else None

    results = await asyncio.gather(*(one(u) for u in urls))
    page, starts, ratio = 1, [], RATIO_MAX
    for img in results:
        if img is None:
            continue
        if not starts:
            ratio = cover_ratio(*img.size)
        n = split_count(*img.size)
        await asyncio.to_thread(_write, out, img, page, n, len(starts) + 1)
        starts.append(page - 1)
        page += n
    if not starts:
        shutil.rmtree(out, ignore_errors=True)
    return page - 1, starts, ratio


def remove(pid: str) -> None:
    shutil.rmtree(post_dir(pid), ignore_errors=True)


def total_mb() -> float:
    return sum(p.stat().st_size for p in config.POSTS_DIR.rglob("*.jpg")) / 1048576
