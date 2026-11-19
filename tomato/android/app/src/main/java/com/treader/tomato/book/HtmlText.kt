package com.treader.tomato.book

import java.io.ByteArrayOutputStream

/**
 * 章节 HTML 转文字，逐行照搬固件 main/book/html_text.c：跳过 head / style / script 和注释；p、div、li、tr、br、hr、
 * blockquote、h1～h6 断块，块之间一个换行；空白折叠成一个空格（块首不留）；h1～h3 里的文字是标题块；图片单独成块写
 * “[图片]”；只认 amp / lt / gt / quot / apos / nbsp 和数字实体。输出字节和设备一样，所以章内偏移能对上。
 *
 * Chapter HTML to text, ported line by line from the firmware's main/book/html_text.c: skip head / style / script and
 * comments; p, div, li, tr, br, hr, blockquote and h1-h6 break blocks, joined by one newline; blanks collapse to one
 * space (none at a block start); text inside h1-h3 forms heading blocks; images become their own "[图片]" block; only
 * amp / lt / gt / quot / apos / nbsp and numeric entities are known. The bytes equal the device's, so offsets line up.
 */
object HtmlText {
    private const val MAX_BYTES = 2 * 1024 * 1024
    private const val MAX_BLOCKS = 16384

    private class Writer {
        val out = ByteArrayOutputStream()
        val blocks = ArrayList<Block>()
        var start = 0
        var active = false
        var heading = false
        var blockHeading = false
        var space = false

        fun finish() {
            space = false
            if (!active) return
            if (blocks.size == MAX_BLOCKS) throw IllegalStateException("too many blocks")
            blocks.add(Block(start, out.size() - start, blockHeading))
            active = false
        }

        fun emit(cp: Int) {
            if (cp == 0xA0 || (cp < 128 && ascii(cp))) { if (active) space = true; return }
            val separator = !active && blocks.isNotEmpty()
            if (out.size() > MAX_BYTES) throw IllegalStateException("chapter too large")
            if (!active) {
                if (separator) out.write('\n'.code)
                start = out.size()
                active = true
                blockHeading = heading
            } else if (space) out.write(' '.code)
            space = false
            TxtBook.encode(cp, out)
        }
    }

    private fun ascii(c: Int) = c == ' '.code || c == '\t'.code || c == '\n'.code || c == '\r'.code || c == 0x0C
    private fun lower(c: Int) = if (c in 'A'.code..'Z'.code) c + 32 else c
    private fun nameChar(c: Int): Boolean { val l = lower(c); return l in 'a'.code..'z'.code || l in '0'.code..'9'.code || l == ':'.code || l == '-'.code || l == '_'.code }

    // 实体：最多看 32 字节；不认识的原样保留，数字非法换成 U+FFFD。/ Entities: look at most 32 bytes ahead; unknown names
    // stay as written, invalid numbers become U+FFFD.
    private fun entity(s: ByteArray, at: Int, len: Int): Pair<Int, Int> {
        var end = 1
        while (end < len && end <= 32) {
            val c = s[at + end].toInt() and 0xFF
            if (c == ';'.code || ascii(c) || c == '&'.code || c == '<'.code) break
            end++
        }
        if (end >= len || end > 32 || s[at + end] != ';'.code.toByte()) return 0 to 0
        if (end > 2 && s[at + 1] == '#'.code.toByte()) {
            var i = 2
            var base = 10
            if (i < end && (s[at + i] == 'x'.code.toByte() || s[at + i] == 'X'.code.toByte())) { base = 16; i++ }
            if (i == end) return 0 to 0
            var value = 0L
            var overflow = false
            while (i < end) {
                val c = lower(s[at + i].toInt() and 0xFF)
                val digit = if (c in '0'.code..'9'.code) c - '0'.code else if (c in 'a'.code..'f'.code) c - 'a'.code + 10 else 255
                if (digit >= base) return 0 to 0
                if (value > (0x10FFFF - digit) / base) overflow = true
                else if (!overflow) value = value * base + digit
                i++
            }
            val cp = if (overflow || value == 0L || value in 0xD800..0xDFFF) 0xFFFD else value.toInt()
            return cp to end + 1
        }
        val name = String(s, at + 1, end - 1, Charsets.ISO_8859_1)
        val cp = when (name) { "amp" -> '&'.code; "lt" -> '<'.code; "gt" -> '>'.code; "quot" -> '"'.code; "apos" -> '\''.code; "nbsp" -> ' '.code; else -> return 0 to 0 }
        return cp to end + 1
    }

    private fun utf8(s: ByteArray, at: Int, len: Int): Pair<Int, Int> {
        val b0 = s[at].toInt() and 0xFF
        if (b0 != 0 && b0 < 0x80) return b0 to 1
        val (cp, k) = TxtBook.utf8One(s, at, len)
        // 固件这里遇到 0 或非法字节就整章失败。/ The firmware fails the chapter on NUL or invalid bytes.
        if (b0 == 0 || (k <= 1 && b0 >= 0x80)) return 0 to 0
        return cp to k
    }

    private fun blockTag(n: String) = (n.length == 2 && n[0] == 'h' && n[1] in '1'..'6') ||
        n in setOf("p", "div", "li", "tr", "br", "hr", "blockquote")

    private fun imageTag(n: String) = n.substringAfterLast(':') == "img" || n.substringAfterLast(':') == "image"

    /** 返回（UTF-8 文字，非空块表）。/ Returns (UTF-8 text, nonempty blocks). */
    fun convert(html: ByteArray): Pair<ByteArray, List<Block>> {
        if (html.size > MAX_BYTES) throw IllegalStateException("chapter too large")
        val w = Writer()
        var skip = ""
        var resumeHead = false
        val len = html.size
        var pos = if (len >= 3 && html[0] == 0xEF.toByte() && html[1] == 0xBB.toByte() && html[2] == 0xBF.toByte()) 3 else 0
        fun startsAt(p: Int, s: String) = len - p >= s.length && s.indices.all { html[p + it] == s[it].code.toByte() }
        while (pos < len) {
            if ((skip.isEmpty() || skip == "head") && startsAt(pos, "<!--")) {
                pos += 4
                while (len - pos >= 3 && !startsAt(pos, "-->")) pos++
                pos = if (len - pos >= 3) pos + 3 else len
                continue
            }
            if (html[pos] == '<'.code.toByte()) {
                var at = pos + 1
                val closing = at < len && html[at] == '/'.code.toByte()
                if (closing) at++
                val start = at
                while (at < len && nameChar(html[at].toInt() and 0xFF)) at++
                val nameLen = at - start
                val name = if (nameLen < 16) String(ByteArray(nameLen) { lower(html[start + it].toInt() and 0xFF).toByte() }, Charsets.ISO_8859_1) else ""
                val headChild = skip == "head" && !closing && (name == "script" || name == "style")
                if (skip.isNotEmpty() && !headChild && (!closing || name != skip)) { pos++; continue }
                val declaration = start < len && (html[start] == '!'.code.toByte() || html[start] == '?'.code.toByte())
                val initial = if (start < len) lower(html[start].toInt() and 0xFF) else 0
                if ((nameLen > 0 && initial in 'a'.code..'z'.code) || declaration) {
                    var quote = 0
                    var end = at
                    while (end < len) {
                        val c = html[end].toInt() and 0xFF
                        if (quote != 0) { if (c == quote) quote = 0 }
                        else if (c == '\''.code || c == '"'.code) quote = c
                        else if (c == '>'.code) break
                        end++
                    }
                    if (end == len) throw IllegalStateException("unterminated tag")
                    val selfClosing = end > at && html[end - 1] == '/'.code.toByte()
                    pos = end + 1
                    if (skip.isNotEmpty()) {
                        if (headChild) { if (!selfClosing) { skip = name; resumeHead = true } }
                        else if (resumeHead) { skip = "head"; resumeHead = false }
                        else skip = ""
                        continue
                    }
                    if (!closing && !selfClosing && (name == "head" || name == "style" || name == "script")) {
                        skip = name
                    } else if (!closing && imageTag(name)) {
                        // 图片独立成块，写“[图片]”。/ Images form their own "[图片]" block.
                        w.finish()
                        val heading = w.heading
                        w.heading = false
                        for (ch in "[图片]") w.emit(ch.code)
                        w.finish()
                        w.heading = heading
                    } else if (blockTag(name)) {
                        w.finish()
                        if (name.length == 2 && name[0] == 'h' && name[1] in '1'..'3') w.heading = !closing && !selfClosing
                    }
                    continue
                }
            }
            if (skip.isNotEmpty()) { pos++; continue }
            var (cp, n) = if (html[pos] == '&'.code.toByte()) entity(html, pos, len - pos) else (0 to 0)
            if (n == 0) { val r = utf8(html, pos, len - pos); cp = r.first; n = r.second }
            if (n == 0) throw IllegalStateException("invalid UTF-8")
            w.emit(cp)
            pos += n
        }
        w.finish()
        return w.out.toByteArray() to w.blocks
    }
}
