package com.treader.tomato.book

import android.graphics.Canvas
import android.graphics.DashPathEffect
import android.graphics.Paint

/** 排版样式，和固件 app_book_style_t 一样的档位。/ Layout style with the firmware's app_book_style_t levels. */
data class Style(
    val line: Int = 1,        // 行距：0 紧凑 1 标准 2 宽松 / compact, standard, loose
    val para: Int = 1,        // 段距：0 无 1 小 2 大 / none, small, large
    val margin: Int = 1,      // 页边距：0 窄 1 中 2 宽 / narrow, medium, wide
    val indent: Boolean = false,
    val underline: Boolean = false,
) {
    val linePct get() = intArrayOf(130, 150, 180)[line.coerceIn(0, 2)]
}

/**
 * 一章的分页，照固件 main/book/book_layout.c：逐字量宽折行，遇换行（CRLF 算一个）断段；开首行缩进时去掉段首空白、
 * 统一空两个字宽；行高 = 字号 × 行距；段后间距按段距（标题段留半行）；一行放不下就换页。页的起点是本章 UTF-8 字节偏移，
 * 和设备的进度偏移是同一种数。
 * Pagination of one chapter after the firmware's main/book/book_layout.c: wrap glyph by glyph on measured width and
 * break paragraphs at newlines (CRLF counts once); with first-line indent, drop leading blanks and indent by two
 * characters; line height = size x spacing; paragraph gaps follow the setting (headings keep half a line); a line that
 * does not fit starts a new page. Page starts are UTF-8 byte offsets in the chapter, the same unit as the device's
 * progress offset.
 */
class ChapterLayout(
    private val text: ByteArray,
    private val blocks: List<Block>,
    private val paint: Paint,
    private val headingPaint: Paint,
    private val width: Int,
    private val height: Int,
    private val style: Style,
) {
    private val widths = HashMap<Int, Float>()
    private val headingWidths = HashMap<Int, Float>()
    val pages = ArrayList<Int>()

    init {
        build()
    }

    private class Line(val start: Int, val textFrom: Int, val end: Int, val next: Int, val paraEnd: Boolean, val heading: Boolean, val indent: Float)

    private fun lineHeight(p: Paint) = (p.textSize * style.linePct / 100f)

    private fun paraGap(lh: Float, heading: Boolean) =
        if (heading) lh / 2 else when (style.para) { 0 -> 0f; 1 -> lh / 3; else -> lh * 2 / 3 }

    private fun block(off: Int): Block? {
        if (blocks.isEmpty() || blocks[0].offset > off) return null
        var lo = 0
        var hi = blocks.size
        while (lo + 1 < hi) {
            val mid = (lo + hi) / 2
            if (blocks[mid].offset <= off) lo = mid else hi = mid
        }
        return blocks[lo]
    }

    private fun cpSize(at: Int): Int {
        val (_, k) = TxtBook.utf8One(text, at, text.size - at)
        return maxOf(k, 1)
    }

    private fun glyphWidth(at: Int, n: Int, heading: Boolean): Float {
        val key = cpKey(at, n)
        val cache = if (heading) headingWidths else widths
        return cache.getOrPut(key) { (if (heading) headingPaint else paint).measureText(String(text, at, n, Charsets.UTF_8)) }
    }

    private fun cpKey(at: Int, n: Int): Int {
        var k = 0
        for (i in 0 until n) k = (k shl 8) or (text[at + i].toInt() and 0xFF)
        return k
    }

    private fun blankSize(at: Int): Int {
        if (at >= text.size) return 0
        val b0 = text[at].toInt() and 0xFF
        if (b0 == ' '.code || b0 == '\t'.code) return 1
        if (at + 2 < text.size && b0 == 0xE3 && (text[at + 1].toInt() and 0xFF) == 0x80 && (text[at + 2].toInt() and 0xFF) == 0x80) return 3
        if (at + 1 < text.size && b0 == 0xC2 && (text[at + 1].toInt() and 0xFF) == 0xA0) return 2
        return 0
    }

    private fun isBreak(at: Int) = text[at] == '\r'.code.toByte() || text[at] == '\n'.code.toByte()

    private fun takeLine(off: Int): Line {
        val b = block(off)
        val heading = b?.heading == true
        val p = if (heading) headingPaint else paint
        val limit = if (b != null) b.offset + b.len else text.size
        var end = off
        var w = 0f
        var indent = 0f
        val paraStart = off == 0 || isBreak(off - 1) || (b != null && b.offset == off)
        var from = off
        if (style.indent && paraStart && !heading) {
            while (end < limit) { val n = blankSize(end); if (n == 0) break; end += n }
            from = end
            // 空行不缩进。/ Blank lines get no indent.
            if (end < limit && !isBreak(end)) {
                indent = 2 * p.measureText("中")
                if (indent * 2 > width) indent = 0f
                w = indent
            }
        }
        while (end < limit && !isBreak(end)) {
            val n = cpSize(end)
            val c = w + glyphWidth(end, n, heading)
            if (c > width) { if (end == from) { end += n }; break }
            w = c
            end += n
        }
        var next = end
        var paraEnd = false
        if (end < text.size && isBreak(end)) {
            next = end + 1
            if (text[end] == '\r'.code.toByte() && next < text.size && text[next] == '\n'.code.toByte()) next++
            paraEnd = true
        }
        if (next <= off) next = off + 1
        return Line(off, from, end, next, paraEnd, heading, indent)
    }

    private fun build() {
        pages.add(0)
        var used = 0f
        var scan = 0
        while (scan < text.size) {
            val l = takeLine(scan)
            val lh = lineHeight(if (l.heading) headingPaint else paint)
            if (used + lh > height && used > 0) { pages.add(scan); used = 0f }
            used += lh
            if (l.paraEnd) used += paraGap(lh, l.heading)
            scan = l.next
        }
    }

    val pageCount get() = pages.size

    fun pageStart(page: Int) = if (page < pages.size) pages[page] else text.size

    /** 偏移所在的页（book_layout_page_for_offset）。/ The page holding an offset. */
    fun pageFor(off: Int): Int {
        var lo = 0
        var hi = pages.size
        while (lo + 1 < hi) {
            val mid = (lo + hi) / 2
            if (pages[mid] <= off) lo = mid else hi = mid
        }
        return lo
    }

    fun draw(canvas: Canvas, page: Int, x: Float, y: Float, rule: Paint) {
        var off = pageStart(page)
        val end = pageStart(page + 1)
        var used = 0f
        while (off < end) {
            val l = takeLine(off)
            val p = if (l.heading) headingPaint else paint
            val lh = lineHeight(p)
            val base = y + used + (lh - (p.descent() - p.ascent())) / 2 - p.ascent()
            if (l.end > l.textFrom) canvas.drawText(String(text, l.textFrom, l.end - l.textFrom, Charsets.UTF_8), x + l.indent, base, p)
            if (style.underline && !l.heading) canvas.drawLine(x, base + p.descent() + 2, x + width, base + p.descent() + 2, rule)
            used += lh
            if (l.paraEnd) used += paraGap(lh, l.heading)
            off = l.next
        }
    }

    companion object {
        fun rulePaint(density: Float) = Paint().apply {
            color = 0xFF9A9A9A.toInt()
            strokeWidth = density
            pathEffect = DashPathEffect(floatArrayOf(3 * density, 3 * density), 0f)
        }
    }
}
