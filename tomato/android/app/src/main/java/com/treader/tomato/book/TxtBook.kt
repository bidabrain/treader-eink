package com.treader.tomato.book

import java.io.ByteArrayOutputStream
import java.io.File

/**
 * TXT 书源，逐行照搬固件 main/book/book_txt.c：同样的编码判断（BOM / UTF-8 / GBK）、同样的章节标题规则、同样的回退分段，
 * 每章转成同样的 UTF-8 字节。这样“第几章 + 章内字节偏移”的阅读进度和设备完全一致，WebDAV 同步才能接上。
 * GBK 用固件自己的码表（assets/gbk.bin，编译时由 main/book/gbk_table.h 生成），不用系统的 GBK 解码器。
 *
 * TXT source, ported line by line from the firmware's main/book/book_txt.c: the same encoding detection (BOM / UTF-8 /
 * GBK), chapter-title rules and fallback parts, and each chapter turns into the same UTF-8 bytes. "Chapter + byte offset
 * in the chapter" progress therefore matches the device exactly and WebDAV sync lines up. GBK uses the firmware's own
 * table (assets/gbk.bin, generated from main/book/gbk_table.h at build time), not the system decoder.
 */
class TxtBook private constructor(
    private val bytes: ByteArray,
    private val gbk: Boolean,
    private val bom: Int,
    val entries: List<Entry>,
) : BookSource {
    data class Entry(val offset: Int, val title: String)

    override val total get() = bytes.size
    override val chapterCount get() = entries.size
    override fun chapterTitle(i: Int) = entries[i].title
    override fun chapterSourceOffset(i: Int) = entries[i].offset

    /** 第 i 章的 UTF-8 字节（book_txt_load）。/ Chapter i as UTF-8 bytes (book_txt_load). */
    override fun chapterUtf8(i: Int): ByteArray {
        var start = entries[i].offset
        val end = if (i + 1 < entries.size) entries[i + 1].offset else bytes.size
        if (start == 0) start = bom
        val out = ByteArrayOutputStream((end - start) + 16)
        var pos = start
        while (pos < end) {
            val (cp, k) = next(pos)
            pos += k
            encode(cp, out)
        }
        return out.toByteArray()
    }

    private fun next(pos: Int): Pair<Int, Int> = next(bytes, pos, gbk)

    companion object {
        const val CHAPTER_MAX = 2048
        const val CHAPTER_BYTES_MAX = 96 * 1024
        private const val READ_BLOCK = 64 * 1024
        private const val PART_TARGET = 48 * 1024
        // 标题行最长字节数（约 40 个汉字），同固件。/ Longest title line in bytes (about 40 CJK characters), as in the
        // firmware.
        private const val TITLE_LINE_MAX = 120

        /** 固件的 GBK 表，App 启动时装一次。/ The firmware GBK table, loaded once at startup. */
        lateinit var gbkTable: CharArray

        fun open(file: File): TxtBook = parse(file.readBytes())

        fun parse(bytes: ByteArray): TxtBook {
            var bom = 0
            var gbk = false
            val n = minOf(bytes.size, READ_BLOCK + 3)
            if (n >= 3 && bytes[0] == 0xEF.toByte() && bytes[1] == 0xBB.toByte() && bytes[2] == 0xBF.toByte()) bom = 3
            else {
                var i = 0
                while (i < n && i < READ_BLOCK) {
                    val (_, k) = utf8One(bytes, i, n - i)
                    if (k == 1 && (bytes[i].toInt() and 0xFF) >= 0x80) { gbk = true; break }
                    i += k
                }
            }
            return TxtBook(bytes, gbk, bom, index(bytes, gbk, bom))
        }

        // 验证一个 UTF-8 标量；无效时只吃第一个字节，码点记 U+FFFD。/ Validate one UTF-8 scalar; on error consume one byte.
        fun utf8One(s: ByteArray, at: Int, n: Int): Pair<Int, Int> {
            if (n <= 0) return 0xFFFD to 0
            val b0 = s[at].toInt() and 0xFF
            if (b0 < 0x80) return b0 to 1
            val k = when (b0) { in 0xC2..0xDF -> 2; in 0xE0..0xEF -> 3; in 0xF0..0xF4 -> 4; else -> 0 }
            if (k == 0 || k > n) return 0xFFFD to 1
            var c = b0 and (0x7F shr k)
            for (j in 1 until k) {
                val b = s[at + j].toInt() and 0xFF
                if (b and 0xC0 != 0x80) return 0xFFFD to 1
                c = (c shl 6) or (b and 63)
            }
            if ((k == 2 && c < 0x80) || (k == 3 && c < 0x800) || (k == 4 && c < 0x10000) || c in 0xD800..0xDFFF || c > 0x10FFFF)
                return 0xFFFD to 1
            return c to k
        }

        private fun next(s: ByteArray, pos: Int, gbk: Boolean): Pair<Int, Int> {
            val n = s.size - pos
            if (!gbk) return utf8One(s, pos, n)
            val a = s[pos].toInt() and 0xFF
            if (a < 0x80) return a to 1
            if (a in 0x81..0xFE && n > 1) {
                val b = s[pos + 1].toInt() and 0xFF
                if (b in 0x40..0xFE && b != 0x7F) return gbkTable[(a - 0x81) * 190 + b - 0x40 - (if (b > 0x7F) 1 else 0)].code to 2
            }
            return 0xFFFD to 1
        }

        fun encode(cp: Int, out: ByteArrayOutputStream) {
            when {
                cp < 0x80 -> out.write(cp)
                cp < 0x800 -> { out.write(0xC0 or (cp shr 6)); out.write(0x80 or (cp and 63)) }
                cp < 0x10000 -> { out.write(0xE0 or (cp shr 12)); out.write(0x80 or ((cp shr 6) and 63)); out.write(0x80 or (cp and 63)) }
                else -> {
                    out.write(0xF0 or (cp shr 18)); out.write(0x80 or ((cp shr 12) and 63))
                    out.write(0x80 or ((cp shr 6) and 63)); out.write(0x80 or (cp and 63))
                }
            }
        }

        private const val NUMS = "零一二三四五六七八九十百千万两"
        private const val UNITS = "章节卷回集部篇"

        // 章节标题：“Chapter 数字”，或“第 + 数字 + 章/节/卷/回/集/部/篇”（看行首最多 40 字节）。
        // A chapter title: "Chapter <digit>", or "第 + number + 章/节/卷/回/集/部/篇" (judged on up to the first 40 bytes).
        private fun heading(line: ByteArray): Boolean {
            if (line.size >= 7 && String(line, 0, 7, Charsets.ISO_8859_1) == "Chapter") {
                var i = 7
                while (i < line.size && (line[i] == ' '.code.toByte() || line[i] == '\t'.code.toByte())) i++
                return i < line.size && line[i] in '0'.code.toByte()..'9'.code.toByte()
            }
            val di = "第".toByteArray(Charsets.UTF_8)
            if (line.size < 3 || line[0] != di[0] || line[1] != di[1] || line[2] != di[2]) return false
            var i = 3
            var number = false
            while (i < line.size) {
                val (cp, k) = utf8One(line, i, line.size - i)
                val token = String(line, i, k, Charsets.UTF_8)
                if (cp in '0'.code..'9'.code || (k == 3 && NUMS.contains(token))) { number = true; i += k }
                else return number && k == 3 && UNITS.contains(token)
            }
            return false
        }

        private fun index(s: ByteArray, gbk: Boolean, bom: Int): List<Entry> {
            val entries = ArrayList<Entry>()
            var tooMany = false
            fun add(offset: Int, title: String?): Boolean {
                if (entries.size == CHAPTER_MAX) return false
                entries.add(Entry(offset, title ?: "Part ${entries.size + 1}"))
                return true
            }
            // 行首空白（半角空格、Tab、全角空格）跳过；超过 40 字节的标题按前 40 字节判断和保存，整行超过
            // TITLE_LINE_MAX 的不算标题。
            // Leading blanks (space, tab, ideographic space) are skipped; a title past 40 bytes is judged and kept by
            // its first 40 bytes, and lines past TITLE_LINE_MAX are never titles.
            val line = ByteArrayOutputStream(48)
            var full = false
            var total = 0
            var start = bom
            var pos = bom
            fun endLine() {
                val l = line.toByteArray()
                if (total <= TITLE_LINE_MAX && heading(l) && !add(start, String(l, Charsets.UTF_8))) tooMany = true
            }
            while (pos < s.size) {
                val (cp, k) = next(s, pos, gbk)
                pos += k
                if (cp == '\n'.code) {
                    endLine()
                    start = pos; line.reset(); full = false; total = 0
                } else if (cp != '\r'.code && !(total == 0 && (cp == ' '.code || cp == '\t'.code || cp == 0x3000))) {
                    val tmp = ByteArrayOutputStream(4)
                    encode(cp, tmp)
                    if (line.size() + tmp.size() <= 40 && !full) line.write(tmp.toByteArray())
                    else full = true
                    total += tmp.size()
                }
            }
            endLine()
            var fallback = entries.size < 2 || tooMany
            if (entries.isNotEmpty()) entries[0] = entries[0].copy(offset = 0)
            for (i in entries.indices) {
                val end = if (i + 1 < entries.size) entries[i + 1].offset else s.size
                if (end - entries[i].offset > CHAPTER_BYTES_MAX) fallback = true
            }
            if (!fallback) return entries
            // 没有可靠的章节：在 48 KB 之后的第一个换行切段，没有换行就在上限前按字符切。
            // No reliable chapters: split at the first newline after 48 KB, or by character before the hard cap.
            entries.clear()
            add(0, null)
            pos = bom
            var part = 0
            while (pos < s.size) {
                val (cp, k) = next(s, pos, gbk)
                pos += k
                val size = pos - part
                if (pos < s.size && ((size >= PART_TARGET && cp == '\n'.code) || size >= CHAPTER_BYTES_MAX - 4)) {
                    if (!add(pos, null)) throw IllegalStateException("too many parts")
                    part = pos
                }
            }
            return entries
        }
    }
}

/** 书源：TXT 和 EPUB 共用（固件 book_source）。/ A book source shared by TXT and EPUB (the firmware's book_source). */
interface BookSource {
    val total: Int
    val chapterCount: Int
    fun chapterTitle(i: Int): String
    /** 这一章在原文件里的起点（算全书百分比用）。/ Where the chapter starts in the source (for whole-book percent). */
    fun chapterSourceOffset(i: Int): Int
    fun chapterUtf8(i: Int): ByteArray
    /** 章内的标题块（EPUB 用；TXT 没有）。/ Heading blocks within a chapter (EPUB only). */
    fun chapterBlocks(i: Int): List<Block> = emptyList()
}

/** 一段文字的样式块（固件 blk_t 的文字部分）。/ A styled text range (the text part of the firmware's blk_t). */
data class Block(val offset: Int, val len: Int, val heading: Boolean)
