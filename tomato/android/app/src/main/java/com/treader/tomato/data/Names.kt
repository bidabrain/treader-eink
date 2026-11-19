package com.treader.tomato.data

import java.io.File

/*
 * 和固件逐字节一样的文字小工具：按 UTF-8 字节截断、书文件起名、URL 转义。书的文件名决定 WebDAV 进度能不能和设备对上，
 * 所以起名规则照抄 components/tomato/tomato_sync.c 的 safe_name。
 * Text helpers that match the firmware byte for byte: UTF-8 truncation, book file names and URL escaping. The file
 * name decides whether WebDAV progress lines up with the device, so naming copies safe_name in
 * components/tomato/tomato_sync.c.
 */

/** 固件字段长度（含结尾 0）。/ Firmware field sizes, including the terminating zero. */
object Caps {
    const val ID = 24
    const val TITLE = 96
    const val AUTHOR = 48
    const val FILE = TITLE + 8
}

/** 照 tomato_copy_utf8：超过 cap-1 字节就截断，退到字符开头。/ Like tomato_copy_utf8: cut past cap-1 bytes on a
 *  character start. */
fun utf8Cut(s: String, cap: Int): String {
    val b = s.toByteArray(Charsets.UTF_8)
    if (b.size < cap) return s
    var n = cap - 1
    while (n > 0 && (b[n].toInt() and 0xC0) == 0x80) n--
    return String(b, 0, n, Charsets.UTF_8)
}

/** 照 safe_name：FAT 不能用的字符换成下划线，按字节限长，再加尾巴和 .txt。/ Like safe_name: characters FAT cannot hold
 *  become underscores, the length is capped in bytes, then the suffix and .txt are added. */
fun safeName(title: String, suffix: String, cap: Int = Caps.FILE): String {
    val t = title.toByteArray(Charsets.UTF_8)
    val room = cap - suffix.toByteArray(Charsets.UTF_8).size - 5
    val out = ByteArray(t.size)
    var o = 0
    while (o < t.size && o < room) {
        val c = t[o].toInt() and 0xFF
        out[o] = if ("/\\:*?\"<>|".indexOf(c.toChar()) >= 0 && c < 0x80 || c < 32) '_'.code.toByte() else t[o]
        o++
    }
    // 停在字符中间就退回字符开头。/ If stopped mid-character, back up to its start.
    while (o > 0 && o < t.size && (t[o].toInt() and 0xC0) == 0x80) o--
    val base = if (o == 0) "book" else String(out, 0, o, Charsets.UTF_8)
    return utf8Cut(base + suffix, cap) + ".txt"
}

/** 新书起名：同名文件已存在就加 book_id 末 6 位。/ Name a new book; add the last six digits of book_id on a clash. */
fun newBookFile(title: String, bookId: String, dir: File): String {
    val plain = safeName(title, "")
    if (!File(dir, plain).exists()) return plain
    return safeName(title, "_" + bookId.takeLast(6))
}

/** 照 tomato_file_name_safe：同步来的文件名不可信。/ Like tomato_file_name_safe: synced names are untrusted. */
fun fileNameSafe(file: String): Boolean =
    file.isNotEmpty() && !file.contains("..") && file.none { it in "/\\:*?\"<>|" || it.code < 32 }

/** 照 tomato_url_escape。/ Like tomato_url_escape. */
fun urlEscape(s: String, keepSlash: Boolean = false): String = buildString {
    for (byte in s.toByteArray(Charsets.UTF_8)) {
        val c = byte.toInt() and 0xFF
        val plain = c in 'a'.code..'z'.code || c in 'A'.code..'Z'.code || c in '0'.code..'9'.code ||
            c == '-'.code || c == '_'.code || c == '.'.code || c == '~'.code || (keepSlash && c == '/'.code)
        if (plain) append(c.toChar()) else append('%').append("0123456789ABCDEF"[c shr 4]).append("0123456789ABCDEF"[c and 15])
    }
}
