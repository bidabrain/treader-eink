"""面板上传的 CBZ / PDF：认文件类型、数页数、按顺序一页页给出图片字节，交给 convert 的 AutoPager 转灰度、切页、打包。

CBZ 按文件名自然顺序（2.jpg 在 10.jpg 前）取里面的图片，跳过 __MACOSX 和隐藏文件；PDF 用 pdfium 一页页渲染成灰度图，
分辨率按页面大小的 1.5 倍取，再交给切页缩小。这里不碰队列和缓存，由 jobs 在线程里调用。

CBZ / PDF uploaded in the panel: detect the type, count pages and yield each page's image bytes in order for convert's
AutoPager to gray, cut and pack. CBZ images are taken in natural file-name order (2.jpg before 10.jpg), skipping
__MACOSX and hidden files; PDF pages are rendered one by one to gray with pdfium at 1.5x the page box, then cut and
scaled down. No queue or cache here; jobs calls it in a thread.
"""
from __future__ import annotations

import io
import re
import zipfile
from pathlib import Path
from typing import Iterator

IMAGE_EXT = (".jpg", ".jpeg", ".png", ".webp", ".gif", ".bmp", ".avif")
ENTRY_MAX = 200 * 1048576        # ZIP 里单张图解开后的上限 / Largest unpacked image inside a ZIP
PDF_SCALE = 1.5                  # PDF 按页面框的几倍渲染 / PDF render size relative to the page box
PDF_PIXELS_MAX = 40_000_000      # 一页渲染出来最多多少像素 / Most pixels in one rendered PDF page


def kind_of(head: bytes) -> str:
    """按文件头认类型：cbz（ZIP）、pdf，别的给空。/ Detect the type from the header: cbz (ZIP), pdf, or empty."""
    if head.startswith(b"PK\x03\x04"):
        return "cbz"
    if head.startswith(b"%PDF"):
        return "pdf"
    return ""


def _natural(name: str) -> list:
    return [int(p) if p.isdigit() else p.lower() for p in re.split(r"(\d+)", name)]


def _entries(z: zipfile.ZipFile) -> list[zipfile.ZipInfo]:
    out = []
    for info in z.infolist():
        name = info.filename
        base = name.rsplit("/", 1)[-1]
        if info.is_dir() or name.startswith("__MACOSX/") or base.startswith(".") or info.file_size > ENTRY_MAX:
            continue
        if base.lower().endswith(IMAGE_EXT):
            out.append(info)
    out.sort(key=lambda i: _natural(i.filename))
    return out


def count(path: Path, kind: str) -> int:
    """有几页（CBZ 是图片数，PDF 是页数）；文件打不开时抛异常。/ How many pages (images in a CBZ, pages in a PDF);
    raises when the file cannot be opened."""
    if kind == "cbz":
        with zipfile.ZipFile(path) as z:
            return len(_entries(z))
    import pypdfium2 as pdfium
    pdf = pdfium.PdfDocument(str(path))
    try:
        return len(pdf)
    finally:
        pdf.close()


def pages(path: Path, kind: str, width: int, height: int) -> Iterator[bytes]:
    """按顺序给出每页的图片字节。/ Yield each page's image bytes in order."""
    if kind == "cbz":
        with zipfile.ZipFile(path) as z:
            for info in _entries(z):
                yield z.read(info)
        return
    import pypdfium2 as pdfium
    pdf = pdfium.PdfDocument(str(path))
    try:
        for i in range(len(pdf)):
            page = pdf[i]
            try:
                w, h = page.get_size()
                scale = PDF_SCALE * max(width / max(w, 1), height / max(h, 1))
                if w * h * scale * scale > PDF_PIXELS_MAX:
                    scale = (PDF_PIXELS_MAX / max(w * h, 1)) ** 0.5
                im = page.render(scale=scale, grayscale=True).to_pil()
            finally:
                page.close()
            out = io.BytesIO()
            im.save(out, "PNG", compress_level=1)
            del im
            yield out.getvalue()
    finally:
        pdf.close()
