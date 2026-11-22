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


STRIP_RATIO = 2.0         # 原图高宽比中位数超过它就当竖长条 / A median height/width ratio above this means a tall strip
SNIFF = 3                 # 看前几张图来判断 / Images looked at before deciding
SPREAD_RATIO = 1.1        # 宽超过高的这么多倍算跨页 / Wider than this times the height counts as a spread
SPREADS = ("split_rtl", "split_ltr", "rotate", "fit")


def _autocrop(im: Image.Image) -> Image.Image:
    """裁掉四边接近纯白的边，墨水屏上能多放一点画面；裁剩太少就不裁。/ Trim near-white borders so more of the drawing fits
    the e-ink screen; skip when too little would remain."""
    box = im.point(lambda v: 255 if v < 235 else 0).getbbox()
    if not box:
        return im
    x0, y0, x1, y1 = box
    if (x1 - x0) * (y1 - y0) < im.width * im.height * 0.5:
        return im
    pad = 4
    return im.crop((max(0, x0 - pad), max(0, y0 - pad), min(im.width, x1 + pad), min(im.height, y1 + pad)))


class PagePager:
    """一张图就是一页的普通漫画：裁白边、等比缩进页面大小，跨页按设置拆开、旋转或整页缩小。和 Pager 用法一样。
    Ordinary one-image-per-page comics: trim white borders and fit inside the page box; spreads are split, rotated or
    shrunk whole per the setting. Used like Pager."""

    def __init__(self, width: int, height: int, sink: Callable[[Image.Image], None] | None = None,
                 spread: str = "split_rtl"):
        self.w, self.h = width, height
        self.sink = sink
        self.spread = spread if spread in SPREADS else "split_rtl"
        self.pages: list[Image.Image] = []
        self.count = 0

    def _fit(self, im: Image.Image) -> Image.Image:
        s = min(self.w / im.width, self.h / im.height)
        if abs(s - 1) > 0.001:
            im = im.resize((max(1, round(im.width * s)), max(1, round(im.height * s))), Image.Resampling.LANCZOS)
        return im

    def _out(self, page: Image.Image) -> None:
        self.count += 1
        if self.sink:
            self.sink(page)
        else:
            self.pages.append(page)

    def feed(self, data: bytes) -> None:
        im = _gray(data)
        if im.width > im.height * SPREAD_RATIO and self.spread != "fit":
            if self.spread == "rotate":
                self._out(self._fit(_autocrop(im.rotate(90, expand=True))))
                return
            half = im.width // 2
            left, right = im.crop((0, 0, half, im.height)), im.crop((half, 0, im.width, im.height))
            for part in ((right, left) if self.spread == "split_rtl" else (left, right)):
                self._out(self._fit(_autocrop(part)))
            return
        self._out(self._fit(_autocrop(im)))

    def finish(self) -> list[Image.Image]:
        return self.pages


class AutoPager:
    """先看前几张图的形状再决定：竖长条走 Pager（在空白处切），否则走 PagePager（一张一页）。
    Look at the first few images first: tall strips go to Pager (cut in gutters), anything else to PagePager (one
    image per page)."""

    def __init__(self, width: int, height: int, sink: Callable[[Image.Image], None] | None = None,
                 spread: str = "split_rtl"):
        self.args = (width, height, sink, spread)
        self.held: list[bytes] = []
        self.ratios: list[float] = []
        self.inner: Pager | PagePager | None = None

    @property
    def count(self) -> int:
        return self.inner.count if self.inner else 0

    @property
    def strip(self) -> bool:
        return isinstance(self.inner, Pager)

    def _decide(self) -> None:
        w, h, sink, spread = self.args
        r = sorted(self.ratios)[len(self.ratios) // 2] if self.ratios else 0
        self.inner = Pager(w, h, sink) if r > STRIP_RATIO else PagePager(w, h, sink, spread)
        held, self.held = self.held, []
        for data in held:
            self.inner.feed(data)

    def feed(self, data: bytes) -> None:
        if self.inner:
            self.inner.feed(data)
            return
        with Image.open(io.BytesIO(data)) as im:
            self.ratios.append(im.height / max(1, im.width))
        self.held.append(data)
        if len(self.held) >= SNIFF:
            self._decide()

    def finish(self) -> list[Image.Image]:
        if not self.inner:
            self._decide()
        return self.inner.finish()


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
            f"<PageCount>{self.n}</PageCount>"
            + (f"<Format>{escape(meta['format'])}</Format>" if meta.get("format") else "")
            + f"<Manga>{escape(meta.get('manga') or 'No')}</Manga></ComicInfo>\n")
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
