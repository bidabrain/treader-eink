"""把笔记图片处理成墨水屏能直接用的灰度 JPEG：不超过 648×1000、太长按屏切开、普通（基线）JPEG；另出一张 150×200 缩略图。

Turn note images into grayscale JPEGs the e-ink device can show as is: at most 648x1000, very tall images split into
screens, baseline JPEG; plus a 150x200 thumbnail.
"""
from __future__ import annotations

import io
import shutil
from pathlib import Path

import httpx
from PIL import Image, ImageOps

from . import config

MAX_W, MAX_H = 648, 1000
THUMB = (150, 200)
# 屏幕可用区域的高宽比；比它高 1.5 倍以上的图按屏切开。/ Screen aspect; images 1.5x taller are split per screen.
SCREEN_RATIO = 1160 / 684
HEADERS = {"User-Agent": "Mozilla/5.0", "Referer": "https://www.xiaohongshu.com/"}


def note_dir(note_id: str) -> Path:
    return config.NOTES_DIR / note_id


def _save(img: Image.Image, path: Path) -> None:
    img.save(path, "JPEG", quality=80, optimize=True, progressive=False)


def _pages(img: Image.Image) -> list[Image.Image]:
    w, h = img.size
    if h / w <= SCREEN_RATIO * 1.5:
        return [img]
    step = round(w * SCREEN_RATIO)
    cuts = list(range(0, h, step))
    # 最后一小截并进上一屏。/ Merge a tiny last strip into the previous screen.
    if len(cuts) > 1 and h - cuts[-1] < step * 0.2:
        cuts.pop()
    return [img.crop((0, y, w, cuts[i + 1] if i + 1 < len(cuts) else h)) for i, y in enumerate(cuts)]


async def process(note_id: str, urls: list[str]) -> int:
    """下载并处理图片，返回生成的页数。/ Download and process images; returns the page count."""
    out = note_dir(note_id)
    tmp = out.with_suffix(".tmp")
    shutil.rmtree(tmp, ignore_errors=True)
    tmp.mkdir(parents=True)
    pages = 0
    thumb_done = False
    async with httpx.AsyncClient(timeout=30.0, headers=HEADERS, follow_redirects=True) as client:
        for url in urls:
            try:
                resp = await client.get(url)
                resp.raise_for_status()
                img = Image.open(io.BytesIO(resp.content))
                img = ImageOps.exif_transpose(img).convert("L")
            except (httpx.HTTPError, OSError):
                continue
            if not thumb_done:
                _save(ImageOps.fit(img, THUMB, Image.Resampling.LANCZOS), tmp / "thumb.jpg")
                thumb_done = True
            for page in _pages(img):
                page.thumbnail((MAX_W, MAX_H), Image.Resampling.LANCZOS)
                pages += 1
                _save(page, tmp / f"{pages}.jpg")
    shutil.rmtree(out, ignore_errors=True)
    tmp.rename(out)
    return pages


def remove(note_id: str) -> None:
    shutil.rmtree(note_dir(note_id), ignore_errors=True)


def total_mb() -> float:
    return sum(p.stat().st_size for p in config.NOTES_DIR.rglob("*.jpg")) / 1048576
