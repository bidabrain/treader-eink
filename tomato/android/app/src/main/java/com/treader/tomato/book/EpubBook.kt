package com.treader.tomato.book

import java.io.ByteArrayOutputStream
import java.io.File
import java.util.zip.ZipFile

/**
 * EPUB 书源，照搬固件 main/book/book_epub.c 和 html_text.c：同样的 XML 小解析器、container / OPF / NCX / NAV 规则，
 * 同样的 HTML 转文字（块之间一个换行、折叠空白、图片写成“[图片]”、h1～h3 是标题块）。章节起点是 spine 里前面各章
 * 原始 HTML 的字节和，和设备的进度单位一样。App 不显示插图（和固件一样先放占位，第五阶段再看要不要做）。
 *
 * EPUB source ported from the firmware's main/book/book_epub.c and html_text.c: the same small XML parser,
 * container / OPF / NCX / NAV rules and HTML-to-text conversion (one newline between blocks, collapsed blanks, images
 * as "[图片]", h1-h3 as heading blocks). A chapter starts at the sum of the preceding spine entries' source HTML
 * bytes, the device's progress unit. Images stay placeholders, as on the firmware.
 */
class EpubBook private constructor(
    private val zip: ZipFile,
    private val paths: List<String>,
    private val offsets: IntArray,
    private val titles: Array<String?>,
    override val total: Int,
) : BookSource {
    override val chapterCount get() = paths.size
    override fun chapterTitle(i: Int) = titles[i] ?: "第 ${i + 1} 节"
    override fun chapterSourceOffset(i: Int) = offsets[i]

    private var cacheIndex = -1
    private var cacheText = ByteArray(0)
    private var cacheBlocks = emptyList<Block>()

    private fun load(i: Int) {
        if (cacheIndex == i) return
        val html = zip.getInputStream(zip.getEntry(paths[i])).use { it.readBytes() }
        val r = HtmlText.convert(html)
        cacheText = r.first
        cacheBlocks = r.second
        cacheIndex = i
    }

    override fun chapterUtf8(i: Int): ByteArray { load(i); return cacheText }
    override fun chapterBlocks(i: Int): List<Block> { load(i); return cacheBlocks }

    class Bad(msg: String) : Exception(msg)

    companion object {
        private const val ITEMS_MAX = 32768
        private const val ENTRY_MAX = 2 * 1024 * 1024
        private const val OUTPUT_MAX = 4 * 1024 * 1024
        private const val TITLE_CAP = 160
        private const val PATH_CAP = 512

        fun open(file: File): EpubBook {
            val zip = ZipFile(file)
            try {
                val names = HashMap<String, java.util.zip.ZipEntry>()
                for (e in zip.entries()) names[e.name] = e
                fun read(path: String, limit: Int): ByteArray {
                    val e = names[path] ?: throw Bad("缺少 $path")
                    if (e.size > limit) throw Bad("$path 太大")
                    return zip.getInputStream(e).use { it.readBytes() }
                }
                val opf = containerPath(read("META-INF/container.xml", ENTRY_MAX))
                val pkg = packageParse(read(opf, OUTPUT_MAX), opf) { p -> names[p]?.size?.toInt() }
                val titles = arrayOfNulls<String>(pkg.chapters.size)
                // 同一个文件可能是好几章：路径 → 章节号列表。/ One file can back several chapters: path to indices.
                val heads = HashMap<String, MutableList<Int>>()
                pkg.chapters.forEachIndexed { i, p -> heads.getOrPut(p) { mutableListOf() }.add(i) }
                fun assign(base: String, href: String, title: String) {
                    val t = title.trimEnd(' ')
                    if (t.isEmpty() || href.isEmpty()) return
                    val path = resolve(base, href) ?: return
                    if (path !in names) return
                    heads[path]?.forEach { if (titles[it] == null) titles[it] = t }
                }
                // 导航损坏不影响阅读；NAV 补 NCX 没命名的章。/ Broken navigation never blocks reading; NAV fills gaps.
                for ((path, ncx) in listOf(pkg.ncx to true, pkg.nav to false)) {
                    if (path.isEmpty()) continue
                    val before = titles.copyOf()
                    val ok = runCatching { navigation(read(path, OUTPUT_MAX), ncx) { href, title -> assign(path, href, title) } }.getOrDefault(false)
                    if (!ok) for (i in titles.indices) if (before[i] == null) titles[i] = null
                }
                val offsets = IntArray(pkg.chapters.size)
                var off = 0
                pkg.chapters.forEachIndexed { i, p -> offsets[i] = off; off += names[p]!!.size.toInt() }
                return EpubBook(zip, pkg.chapters, offsets, titles, off)
            } catch (e: Exception) {
                zip.close()
                throw if (e is Bad) e else Bad("EPUB 格式不对（${e.message ?: e.javaClass.simpleName}）")
            }
        }

        /* ---- XML ---- */

        private enum class Kind { OPEN, CLOSE, TEXT }

        private class Tok(
            val kind: Kind, val name: String = "", val attrs: ByteArray = ByteArray(0),
            val text: ByteArray = ByteArray(0), val depth: Int, val empty: Boolean = false, val cdata: Boolean = false,
        )

        private fun sp(c: Byte) = c == ' '.code.toByte() || c == '\t'.code.toByte() || c == '\r'.code.toByte() || c == '\n'.code.toByte()
        private fun nameChar(c: Byte) = !sp(c) && c != '/'.code.toByte() && c != '>'.code.toByte() && c != '='.code.toByte() &&
            c != '<'.code.toByte() && c != '\''.code.toByte() && c != '"'.code.toByte()

        private class Xml(val b: ByteArray) {
            var p = 0
            val end = b.size
            val stack = ArrayList<String>()
            var roots = 0
            var failed = false

            init {
                if (b.any { it == 0.toByte() }) failed = true
                if (b.size >= 3 && b[0] == 0xEF.toByte() && b[1] == 0xBB.toByte() && b[2] == 0xBF.toByte()) p = 3
            }

            private fun begins(at: Int, s: String): Boolean {
                if (end - at < s.length) return false
                for (i in s.indices) if (b[at + i] != s[i].code.toByte()) return false
                return true
            }

            private fun find(from: Int, s: String): Int {
                var q = from
                while (end - q >= s.length) { if (begins(q, s)) return q; q++ }
                return -1
            }

            fun next(): Tok? {
                while (p < end && !failed) {
                    var q = p
                    if (b[q] != '<'.code.toByte()) {
                        val start = q
                        while (q < end && b[q] != '<'.code.toByte()) q++
                        if (stack.isEmpty()) {
                            var r = start
                            while (r < q && sp(b[r])) r++
                            if (r != q) break
                        }
                        p = q
                        return Tok(Kind.TEXT, text = b.copyOfRange(start, q), depth = stack.size)
                    }
                    if (begins(q, "<!--") || begins(q, "<?")) {
                        val comment = b[q + 1] == '!'.code.toByte()
                        val marker = if (comment) "-->" else "?>"
                        val e = find(q + if (comment) 4 else 2, marker)
                        if (e < 0) break
                        p = e + marker.length
                        continue
                    }
                    if (begins(q, "<![CDATA[")) {
                        if (stack.isEmpty()) break
                        val e = find(q + 9, "]]>")
                        if (e < 0) break
                        p = e + 3
                        return Tok(Kind.TEXT, text = b.copyOfRange(q + 9, e), depth = stack.size, cdata = true)
                    }
                    if (begins(q, "<!DOCTYPE")) {
                        var brackets = 0
                        var quote = 0.toByte()
                        q += 9
                        var done = false
                        while (q < end) {
                            val c = b[q++]
                            if (quote != 0.toByte()) { if (c == quote) quote = 0; continue }
                            if (c == '\''.code.toByte() || c == '"'.code.toByte()) quote = c
                            else if (c == '['.code.toByte()) brackets++
                            else if (c == ']'.code.toByte()) { if (brackets == 0) break; brackets-- }
                            else if (c == '>'.code.toByte() && brackets == 0) { p = q; done = true; break }
                        }
                        if (!done || quote != 0.toByte() || brackets != 0) break
                        continue
                    }
                    q++
                    val closing = q < end && b[q] == '/'.code.toByte()
                    if (closing) q++
                    val nameStart = q
                    while (q < end && nameChar(b[q])) q++
                    if (q == nameStart) break
                    val name = String(b, nameStart, q - nameStart, Charsets.UTF_8)
                    val attrStart = q
                    var quote = 0.toByte()
                    while (q < end) {
                        if (quote != 0.toByte()) { if (b[q] == quote) quote = 0 }
                        else if (b[q] == '\''.code.toByte() || b[q] == '"'.code.toByte()) quote = b[q]
                        else if (b[q] == '>'.code.toByte()) break
                        q++
                    }
                    if (q == end || quote != 0.toByte()) break
                    var e = q
                    while (e > attrStart && sp(b[e - 1])) e--
                    var empty = false
                    if (e > attrStart && b[e - 1] == '/'.code.toByte()) { empty = true; e-- }
                    val attrs = b.copyOfRange(attrStart, e)
                    if (!attrsWellFormed(attrs)) break
                    val tok: Tok
                    if (closing) {
                        if (empty || stack.isEmpty() || attrs.isNotEmpty()) {
                            var onlySpace = !empty && stack.isNotEmpty()
                            for (c in attrs) if (!sp(c)) onlySpace = false
                            if (!onlySpace) break
                        }
                        if (stack.last() != name) break
                        tok = Tok(Kind.CLOSE, name, attrs, depth = stack.size)
                        stack.removeAt(stack.size - 1)
                    } else {
                        if (stack.size == 64) break
                        if (stack.isEmpty() && ++roots != 1) break
                        tok = Tok(Kind.OPEN, name, attrs, depth = stack.size + 1, empty = empty)
                        if (!empty) stack.add(name)
                    }
                    p = q + 1
                    return tok
                }
                if (p != end || stack.isNotEmpty() || roots != 1) failed = true
                return null
            }
        }

        private fun localName(name: String, want: String) = name.substringAfter(':', name) == want

        /** 属性一个个取：(名字, 值的原始字节)；格式不对返回 null。/ Attributes as (name, raw value); null when malformed. */
        private fun attrList(a: ByteArray): List<Pair<String, ByteArray>>? {
            val out = ArrayList<Pair<String, ByteArray>>()
            var i = 0
            while (true) {
                while (i < a.size && sp(a[i])) i++
                if (i == a.size) return out
                val start = i
                while (i < a.size && nameChar(a[i])) i++
                if (i == start) return null
                val name = String(a, start, i - start, Charsets.UTF_8)
                while (i < a.size && sp(a[i])) i++
                if (i == a.size || a[i++] != '='.code.toByte()) return null
                while (i < a.size && sp(a[i])) i++
                if (i == a.size || (a[i] != '\''.code.toByte() && a[i] != '"'.code.toByte())) return null
                val quote = a[i++]
                val vs = i
                while (i < a.size && a[i] != quote) { if (a[i] == '<'.code.toByte()) return null; i++ }
                if (i == a.size) return null
                out += name to a.copyOfRange(vs, i)
                i++
            }
        }

        private fun attrsWellFormed(a: ByteArray) = attrList(a) != null

        /** 取一个属性；没有给 ""，重复或解码失败 null（固件 attribute 返回 false）。/ One attribute: "" when absent, null on
         *  duplicates or decode failure (the firmware's attribute returning false). */
        private fun attribute(t: Tok, want: String, cap: Int): String? {
            val list = attrList(t.attrs) ?: return null
            var found: String? = null
            for ((n, v) in list) if (n == want) {
                if (found != null) return null
                found = decode(v, "", cap, label = false, literal = false) ?: return null
            }
            return found ?: ""
        }

        private fun entity(s: String): Int? = when (s) {
            "amp" -> '&'.code; "lt" -> '<'.code; "gt" -> '>'.code; "quot" -> '"'.code; "apos" -> '\''.code; "nbsp" -> 160
            else -> {
                if (s.length > 1 && s[0] == '#') {
                    var i = 1
                    var base = 10
                    if (s[i] == 'x' || s[i] == 'X') { base = 16; i++ }
                    if (i == s.length) null
                    else {
                        var v = 0L
                        var ok = true
                        while (i < s.length) {
                            val d = Character.digit(s[i], 16)
                            if (d < 0 || d >= base || v > (0x10FFFF - d) / base) { ok = false; break }
                            v = v * base + d
                            i++
                        }
                        if (!ok || v == 0L || v > 0x10FFFF || v in 0xD800..0xDFFF) null else v.toInt()
                    }
                } else null
            }
        }

        /** 固件 decode：属性必须完整放下；目录标题折叠空白、可在字符边界截断。/ The firmware's decode: attributes must fit
         *  fully; labels collapse blanks and may truncate on a character boundary. */
        private fun decode(s: ByteArray, prefix: String, cap: Int, label: Boolean, literal: Boolean): String? {
            val out = ByteArrayOutputStream()
            out.write(prefix.toByteArray(Charsets.UTF_8))
            var full = false
            var i = 0
            while (i < s.size) {
                var cp: Int
                val n: Int
                if (!literal && s[i] == '&'.code.toByte()) {
                    var e = i + 1
                    while (e < s.size && s[e] != ';'.code.toByte() && e - i < 20) e++
                    if (e == s.size || s[e] != ';'.code.toByte()) return null
                    cp = entity(String(s, i + 1, e - i - 1, Charsets.UTF_8)) ?: return null
                    n = e - i + 1
                } else {
                    val (c, k) = TxtBook.utf8One(s, i, s.size - i)
                    if (c == 0xFFFD && !(k == 3 && (s[i].toInt() and 0xFF) == 0xEF)) return null
                    cp = c; n = k
                }
                i += n
                if (cp == 0 || (cp < 32 && cp != '\n'.code && cp != '\r'.code && cp != '\t'.code)) return null
                if (label && (cp == 160 || cp == ' '.code || cp == '\t'.code || cp == '\n'.code || cp == '\r'.code)) {
                    val cur = out.toByteArray()
                    if (cur.isEmpty() || cur.last() == ' '.code.toByte()) continue
                    cp = ' '.code
                }
                val bytes = ByteArrayOutputStream(4).also { TxtBook.encode(cp, it) }.toByteArray()
                if (full || out.size() + bytes.size >= cap) { if (!label) return null; full = true; continue }
                out.write(bytes)
            }
            return String(out.toByteArray(), Charsets.UTF_8)
        }

        private fun word(list: String, needle: String) = list.split(' ', '\t', '\r', '\n').any { it == needle }

        /** 照固件 resolve_path：相对 base 解析 href，解 %XX，去掉 #，拒绝绝对路径和越界的“..”。/ Like resolve_path. */
        fun resolve(base: String, href: String): String? {
            val dec = ByteArrayOutputStream()
            val h = href.toByteArray(Charsets.UTF_8)
            var i = 0
            while (i < h.size && h[i] != '#'.code.toByte()) {
                var c = h[i].toInt() and 0xFF
                if (c == '%'.code) {
                    if (i + 2 >= h.size) return null
                    val a = Character.digit(h[i + 1].toInt().toChar(), 16)
                    val b = Character.digit(h[i + 2].toInt().toChar(), 16)
                    if (a < 0 || b < 0) return null
                    c = (a shl 4) or b
                    i += 2
                }
                if (c < 32 || c == 127 || c == '\\'.code || c == ':'.code || c == '?'.code || dec.size() + 1 >= PATH_CAP) return null
                dec.write(c)
                i++
            }
            val d = String(dec.toByteArray(), Charsets.UTF_8)
            if (d.isEmpty()) {
                if (h.isEmpty() || h[0] != '#'.code.toByte() || base.isEmpty()) return null
                return base
            }
            if (d.startsWith("/")) return null
            val parts = ArrayList<String>()
            val slash = base.lastIndexOf('/')
            if (slash > 0) parts.addAll(base.substring(0, slash).split('/'))
            for (seg in d.split('/')) {
                if (seg.isEmpty() || seg == ".") continue
                if (seg == "..") { if (parts.isEmpty()) return null; parts.removeAt(parts.size - 1); continue }
                parts.add(seg)
            }
            val out = parts.joinToString("/")
            return out.ifEmpty { null }
        }

        private fun containerPath(text: ByteArray): String {
            val x = Xml(text)
            var root = false
            var found: String? = null
            while (true) {
                val t = x.next() ?: break
                if (t.kind != Kind.OPEN) continue
                if (t.depth == 1 && localName(t.name, "container")) root = true
                if (root && localName(t.name, "rootfile") && found == null) {
                    val href = attribute(t, "full-path", PATH_CAP)
                    val media = attribute(t, "media-type", 80)
                    if (href == null || media == null) { x.failed = true; break }
                    if ((media.isEmpty() || media == "application/oebps-package+xml") && href.isNotEmpty()) {
                        found = resolve("", href) ?: run { x.failed = true; null }
                        if (found == null) break
                    }
                }
            }
            if (x.failed || !root || found == null) throw Bad("container.xml 不对")
            return found
        }

        private class Pkg(val chapters: List<String>, val ncx: String, val nav: String)

        private class Item(val id: String, val path: String, val nav: Boolean, val ncx: Boolean, val html: Boolean)

        private fun packageParse(text: ByteArray, opf: String, size: (String) -> Int?): Pkg {
            val items = ArrayList<Item>()
            var ncx = ""
            var nav = ""
            var x = Xml(text)
            var manifest = 0
            var root = false
            while (true) {
                val t = x.next() ?: break
                if (t.kind == Kind.OPEN && t.depth == 1 && localName(t.name, "package")) root = true
                if (t.kind == Kind.OPEN && localName(t.name, "manifest")) manifest = t.depth
                if (t.kind == Kind.CLOSE && t.depth == manifest) manifest = 0
                if (t.kind != Kind.OPEN || manifest == 0 || t.depth != manifest + 1 || !localName(t.name, "item")) continue
                val id = attribute(t, "id", 128)
                val href = attribute(t, "href", PATH_CAP)
                val media = attribute(t, "media-type", 80)
                val props = attribute(t, "properties", 256)
                if (id == null || href == null || media == null || props == null || id.isEmpty() || href.isEmpty()) throw Bad("OPF 不对")
                val path = resolve(opf, href) ?: throw Bad("OPF 不对")
                val it = Item(id, path, word(props, "nav"), media == "application/x-dtbncx+xml",
                    media == "application/xhtml+xml" || media == "text/html")
                if (nav.isEmpty() && it.nav) nav = path
                if (ncx.isEmpty() && it.ncx) ncx = path
                items.add(it)
                if (items.size > ITEMS_MAX) throw Bad("EPUB 条目太多")
            }
            if (x.failed || !root || items.isEmpty()) throw Bad("OPF 不对")
            val byId = items.associateBy { it.id }
            if (byId.size != items.size) throw Bad("OPF 里有重复的 id")
            val chapters = ArrayList<String>()
            x = Xml(text)
            var spine = 0
            var seen = false
            while (true) {
                val t = x.next() ?: break
                if (t.kind == Kind.OPEN && localName(t.name, "spine")) {
                    if (seen) throw Bad("OPF 有两个 spine")
                    seen = true
                    spine = t.depth
                    val toc = attribute(t, "toc", 128) ?: throw Bad("OPF 不对")
                    byId[toc]?.takeIf { it.ncx }?.let { ncx = if (size(it.path) != null) it.path else "" }
                }
                if (t.kind == Kind.CLOSE && t.depth == spine) spine = 0
                if (t.kind != Kind.OPEN || spine == 0 || t.depth != spine + 1 || !localName(t.name, "itemref")) continue
                val idref = attribute(t, "idref", 128) ?: throw Bad("OPF 不对")
                val item = byId[idref]?.takeIf { it.html } ?: throw Bad("spine 里有不支持的条目")
                val bytes = size(item.path) ?: throw Bad("缺少 ${item.path}")
                if (bytes > ENTRY_MAX) throw Bad("${item.path} 太大")
                chapters.add(item.path)
                if (chapters.size > ITEMS_MAX) throw Bad("章节太多")
            }
            if (x.failed || chapters.isEmpty()) throw Bad("OPF 没有章节")
            return Pkg(chapters, ncx, nav)
        }

        private class Node(val depth: Int, var labelDepth: Int, var href: String, var title: String)

        /** 读 NCX 或 NAV 目录，每个节点结束时交出（href, 标题）；格式坏了返回 false。/ Read an NCX or NAV table of contents,
         *  reporting (href, title) as each node closes; false when malformed. */
        private fun navigation(text: ByteArray, ncx: Boolean, assign: (String, String) -> Unit): Boolean {
            val x = Xml(text)
            val nodes = ArrayList<Node>()
            var tocDepth = 0
            while (true) {
                val t = x.next() ?: break
                if (!ncx && t.kind == Kind.OPEN && localName(t.name, "nav")) {
                    val type = attribute(t, "epub:type", 128) ?: return false
                    if (!t.empty && word(type, "toc")) tocDepth = t.depth
                }
                if (!ncx && t.kind == Kind.CLOSE && t.depth == tocDepth) tocDepth = 0
                val open = t.kind == Kind.OPEN && (if (ncx) localName(t.name, "navPoint") else tocDepth != 0 && localName(t.name, "a"))
                if (open) {
                    if (nodes.size == 64) return false
                    val n = Node(t.depth, 0, "", "")
                    if (!ncx) { n.labelDepth = t.depth; n.href = attribute(t, "href", PATH_CAP) ?: return false }
                    if (!t.empty) nodes.add(n)
                    continue
                }
                if (nodes.isEmpty()) continue
                val node = nodes.last()
                if (ncx && t.kind == Kind.OPEN && localName(t.name, "navLabel")) node.labelDepth = t.depth
                if (t.kind == Kind.TEXT && node.labelDepth != 0)
                    node.title = decode(t.text, node.title, TITLE_CAP, label = true, literal = t.cdata) ?: return false
                if (ncx && t.kind == Kind.OPEN && localName(t.name, "content")) {
                    node.href = attribute(t, "src", PATH_CAP) ?: return false
                    assign(node.href, node.title)
                }
                if (t.kind == Kind.CLOSE && t.depth == node.labelDepth) node.labelDepth = 0
                if (t.kind == Kind.CLOSE && t.depth == node.depth) { assign(node.href, node.title); nodes.removeAt(nodes.size - 1) }
            }
            return !x.failed
        }
    }
}
