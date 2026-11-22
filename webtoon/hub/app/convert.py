"""把一话的竖长条原图转成墨水屏用的 CBZ：缩到屏幕宽度、转灰度、在分镜之间的空白处按屏幕高度切页。

原图按顺序一张张喂进来，内部只保留不到两屏高的滚动缓冲，不会把整条放进内存。切点在目标高度往上的一段范围里找：
整行颜色几乎一样（纯白或纯黑留白）的行算空白，选这段里最宽的一条空白带的中间切；没有空白才在目标高度硬切。
页首超过 40 行的空白压到 20 行，整页都是空白的不要，所以分镜之间很长的留白不会变成一页空页。

Turn one episode's tall strip images into an e-ink CBZ: scale to the screen width, gray, and cut pages at the screen
height in the gaps between panels. Source images are fed in order; only a rolling buffer under two screens tall is
kept. The cut is searched in a band above the target height: rows of nearly one color (white or black gutters) count
as blank, and the middle of the widest blank run in that band wins; without one the page is cut at the target height.
More than 40 blank rows at a page top shrink to 20 and all-blank pages are dropped, so long gutters never become
empty pages.
"""
from __future__ import annotations

import io
import zipfile
from typing import Callable
from xml.sax.saxutils import escape

from PIL import Image, ImageFilter, ImageOps

BLANK_COST = 1            # 一行的“墨”不超过它就算空白 / A row with at most this much ink counts as blank
SEARCH_FROM = 0.6         # 从目标高度的 60% 开始找切点 / Search for a cut from 60% of the target height
TOP_KEEP = 20             # 页首空白最多留几行 / Most blank rows kept at a page top
TOP_TRIM_FROM = 40        # 页首空白超过这么多行才压 / Only trim when the top blank run exceeds this


def _gray(data: bytes) -> Image.Image:
    im = Image.open(io.BytesIO(data))
    im.load()
    if im.mode in ("RGBA", "LA", "P"):
        im = im.convert("RGBA")
        bg = Image.new("RGBA", im.size, (255, 255, 255, 255))
        bg.alpha_composite(im)
        im = bg
    return ImageOps.exif_transpose(im).convert("L")


class Pager:
    """原图进，页面出。/ Source images in, pages out."""

    def __init__(self, width: int, height: int, sink: Callable[[Image.Image], None] | None = None):
        """sink 非空时，切好的页（除了最后两页，收尾要合并）马上交给它，不留在内存里。
        With a sink, finished pages (all but the last two, which the tail merge may need) go to it at once instead of
        staying in memory."""
        self.w, self.h = width, height
        self.sink = sink
        self.buf: Image.Image | None = None
        self.pages: list[Image.Image] = []
        self.count = 0                      # 一共切出几页（含交给 sink 的）/ Pages cut so far, sink included

    # -- 行判断 / Row tests --
    def _costs(self, y0: int, y1: int) -> list[int]:
        """每行有多少“墨”（0～255）：白底上的深色像素和黑底上的浅色像素取少的那个，纯白、纯黑的留白都是 0。
        How much ink each row holds (0-255): the smaller of dark pixels on white and light pixels on black, so plain
        white and plain black gutters both score 0."""
        y1 = min(y1, self.buf.height)
        if y1 <= y0:
            return []
        strip = self.buf.crop((0, y0, self.w, y1))
        dark = strip.point(lambda v: 255 if v < 225 else 0).resize((1, y1 - y0), Image.Resampling.BOX)
        light = strip.point(lambda v: 255 if v > 30 else 0).resize((1, y1 - y0), Image.Resampling.BOX)
        return [min(a, b) for a, b in zip(dark.tobytes(), light.tobytes())]

    def _trim_top(self) -> None:
        costs = self._costs(0, min(self.buf.height, self.h))
        n = 0
        while n < len(costs) and costs[n] <= BLANK_COST:
            n += 1
        if n > TOP_TRIM_FROM and n < self.buf.height:
            self.buf = self.buf.crop((0, n - TOP_KEEP, self.w, self.buf.height))

    def _cut_at(self) -> int:
        """这一页切在哪一行：目标高度往上那段里最“空”的一段的中间。/ Where this page ends: the middle of the emptiest
        run in the band above the target height."""
        start = int(self.h * SEARCH_FROM)
        costs = self._costs(start, self.h + 1)
        if not costs:
            return self.h
        limit = min(costs) + 2
        best, best_len, run = self.h, 0, 0
        for i, c in enumerate(costs):
            if c <= limit:
                run += 1
                # 同样宽取更靠下的（页面更满）。/ On a tie take the lower run (a fuller page).
                if run >= best_len:
                    best_len, best = run, start + i - run // 2
            else:
                run = 0
        return max(1, best)

    def _emit(self, cut: int) -> None:
        page = self.buf.crop((0, 0, self.w, cut))
        if max(self._costs(0, cut), default=0) > BLANK_COST:   # 整页空白的不要 / Drop all-blank pages
            self.pages.append(page)
            self.count += 1
            if self.sink and len(self.pages) > 2:
                self.sink(self.pages.pop(0))
        self.buf = self.buf.crop((0, cut, self.w, self.buf.height))

    # -- 喂图 / Feeding --
    def feed(self, data: bytes) -> None:
        im = _gray(data)
        if im.width != self.w:
            im = im.resize((self.w, max(1, round(im.height * self.w / im.width))), Image.Resampling.LANCZOS)
        if self.buf is None:
            self.buf = im
        else:
            joined = Image.new("L", (self.w, self.buf.height + im.height), 255)
            joined.paste(self.buf, (0, 0))
            joined.paste(im, (0, self.buf.height))
            self.buf = joined
        self._drain(final=False)

    def _drain(self, final: bool) -> None:
        while self.buf is not None and self.buf.height > 0:
            self._trim_top()
            # 还没攒够一屏多一点就等下一张（除非是最后）。/ Wait for the next image until a bit over a screen is
            # buffered (unless this is the end).
            if not final and self.buf.height < self.h + 4:
                return
            if self.buf.height <= self.h:
                self._emit(self.buf.height)
                return
            self._emit(self._cut_at())

    def finish(self) -> list[Image.Image]:
        self._drain(final=True)
        # 最后一小截并进上一页（太矮的一页看着别扭）。/ Merge a very short last page into the previous one.
        if len(self.pages) >= 2 and self.pages[-1].height < self.h * 0.15:
            a, b = self.pages[-2], self.pages.pop()
            merged = Image.new("L", (self.w, a.height + b.height), 255)
            merged.paste(a, (0, 0))
            merged.paste(b, (0, a.height))
            self.pages[-1] = merged
            self.count -= 1
        return self.pages


def jpeg(page: Image.Image, quality: int) -> bytes:
    out = io.BytesIO()
    page = page.filter(ImageFilter.UnsharpMask(radius=1, percent=50, threshold=2))
    page.save(out, "JPEG", quality=quality, optimize=True, progressive=False)
    return out.getvalue()


class CbzWriter:
    """边切边写的 CBZ（不压缩的 ZIP）：0001.jpg 起顺序编号，最后附 ComicInfo.xml。/ A CBZ (uncompressed ZIP) written
    page by page, numbered from 0001.jpg, with ComicInfo.xml at the end."""

    def __init__(self, target, quality: int):
        self.z = zipfile.ZipFile(target, "w", zipfile.ZIP_STORED)
        self.quality = quality
        self.n = 0

    def add(self, page: Image.Image) -> None:
        self.n += 1
        self.z.writestr(f"{self.n:04d}.jpg", jpeg(page, self.quality))

    def close(self, meta: dict) -> None:
        info = (
            '<?xml version="1.0" encoding="utf-8"?>\n<ComicInfo>'
            f"<Series>{escape(meta.get('series', ''))}</Series><Number>{int(meta.get('no', 0))}</Number>"
            f"<Title>{escape(meta.get('title', ''))}</Title><Writer>{escape(meta.get('author', ''))}</Writer>"
            f"<Web>{escape(meta.get('web', ''))}</Web><LanguageISO>{escape(meta.get('lang', ''))}</LanguageISO>"
            f"<PageCount>{self.n}</PageCount><Manga>No</Manga></ComicInfo>\n")
        self.z.writestr("ComicInfo.xml", info)
        self.z.close()

    def abort(self) -> None:
        self.z.close()


def cbz(pages: list[Image.Image], quality: int, meta: dict) -> bytes:
    """整话一次打包（测试和小图用）。/ Pack a whole episode at once (tests and small inputs)."""
    out = io.BytesIO()
    w = CbzWriter(out, quality)
    for page in pages:
        w.add(page)
    w.close(meta)
    return out.getvalue()


def cover(data: bytes, w: int = 150, h: int = 200) -> bytes:
    """发现页封面：灰度、裁成 3:4、基线 JPEG。/ Store cover: gray, cropped to 3:4, baseline JPEG."""
    im = ImageOps.fit(_gray(data), (w, h), Image.Resampling.LANCZOS)
    out = io.BytesIO()
    im.save(out, "JPEG", quality=85, optimize=True, progressive=False)
    return out.getvalue()
