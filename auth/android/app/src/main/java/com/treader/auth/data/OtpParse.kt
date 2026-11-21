package com.treader.auth.data

import java.net.URLDecoder

/*
 * 解析 otpauth:// 和 otpauth-migration://（Google Authenticator 导出），逐行照搬固件导入网页
 * main/apps/auth_form.html 里的 parsers 段；还有小纸 Pico 热点二维码（WIFI:）。
 * Parse otpauth:// and otpauth-migration:// (Google Authenticator export), a line-by-line port of the parsers block in
 * the firmware import page main/apps/auth_form.html; plus the Read Pico hotspot QR code (WIFI:).
 */

/** 解析出来的一个账号；unsupported 非空表示不能导入及原因。/ One parsed account; a non-empty unsupported says why it
 *  cannot be imported. */
data class Parsed(
    val issuer: String,
    val name: String,
    val secret: String,
    val algo: String,
    val digits: Int,
    val period: Int,
    val unsupported: String = "",
    val notes: List<String> = emptyList(),
) {
    val label: String get() = if (issuer.isEmpty()) name else "$issuer · $name"
    val key: String get() = "$issuer\n$name\n$secret\n${if (unsupported.isEmpty()) "" else "x"}"

    /** 转成账号（编号稍后分配）；不能导入返回 null。/ Turn into an account (id assigned later); null when unsupported. */
    fun toAccount(): Account? {
        if (unsupported.isNotEmpty()) return null
        val s = Base32.decode(secret) ?: return null
        val a = Account(0, issuer, name, s, Algo.of(algo) ?: return null, digits, period)
        return if (a.valid()) a else null
    }
}

class ParseError(msg: String) : Exception(msg)

data class Batch(val size: Int, val index: Int, val id: Long)
data class Migration(val accounts: List<Parsed>, val version: Long, val batch: Batch)
data class Lines(val accounts: List<Parsed>, val batches: List<Batch>, val errors: List<Pair<Int, String>>)

object OtpParse {
    private val ALGOS = listOf("SHA1", "SHA256", "SHA512")

    private fun finish(a: Parsed): Parsed {
        var issuer = a.issuer.trim()
        var name = a.name.trim()
        val notes = a.notes.toMutableList()
        if (name.isEmpty()) name = issuer.ifEmpty { "未命名" }
        if (utf8Len(issuer) > MAX_TEXT) { issuer = utf8Cut(issuer, MAX_TEXT); notes += "发行方过长，已截断" }
        if (utf8Len(name) > MAX_TEXT) { name = utf8Cut(name, MAX_TEXT); notes += "名称过长，已截断" }
        return a.copy(issuer = issuer, name = name, notes = notes)
    }

    private fun check(a: Parsed): Parsed {
        if (a.unsupported.isNotEmpty()) return a
        val why = when {
            a.algo !in ALGOS -> "算法 ${a.algo} 不支持"
            a.digits != 6 && a.digits != 8 -> "${a.digits} 位验证码不支持"
            a.period != 30 && a.period != 60 -> "${a.period} 秒周期不支持"
            !base32Valid(a.secret) -> "密钥无效"
            else -> ""
        }
        return a.copy(unsupported = why)
    }

    fun base32Valid(s: String): Boolean {
        val n = Base32.norm(s)
        if (n.isEmpty() || !n.all { it in 'A'..'Z' || it in '2'..'7' }) return false
        return n.length * 5 / 8 in 1..MAX_SECRET
    }

    private fun pct(s: String): String = try {
        URLDecoder.decode(s.replace("+", "%2B"), "UTF-8")
    } catch (e: Exception) {
        s
    }

    private fun query(q: String?): Map<String, String> {
        val o = LinkedHashMap<String, String>()
        (q ?: "").split('&').forEach { kv ->
            if (kv.isEmpty()) return@forEach
            val i = kv.indexOf('=')
            val k = pct((if (i < 0) kv else kv.substring(0, i)).replace('+', ' ')).lowercase()
            val v = pct((if (i < 0) "" else kv.substring(i + 1)).replace('+', ' '))
            if (k !in o) o[k] = v
        }
        return o
    }

    private fun splitLabel(label: String): Pair<String, String> {
        val i = label.indexOf(':')
        return if (i < 0) "" to label.trim() else label.substring(0, i).trim() to label.substring(i + 1).trim()
    }

    private val OTPAUTH = Regex("^otpauth://([^/?#]*)/?([^?#]*)(?:\\?([^#]*))?", RegexOption.IGNORE_CASE)
    private val MIGRATION = Regex("^otpauth-migration://[^?#]*\\?(?:[^#]*&)?data=([^&#]*)", RegexOption.IGNORE_CASE)

    fun otpauth(uri: String): Parsed {
        val m = OTPAUTH.find(uri.trim()) ?: throw ParseError("不是 otpauth 链接")
        val type = m.groupValues[1].lowercase()
        val (labIssuer, labName) = splitLabel(pct(m.groupValues[2]))
        val q = query(m.groups[3]?.value)
        var a = Parsed(
            issuer = q["issuer"]?.takeIf { it.isNotEmpty() } ?: labIssuer,
            name = labName,
            secret = Base32.norm(q["secret"] ?: ""),
            algo = q["algorithm"]?.takeIf { it.isNotEmpty() }?.uppercase()?.replaceFirst("-", "") ?: "SHA1",
            digits = q["digits"]?.takeIf { it.isNotEmpty() }?.let { it.trim().toIntOrNull() ?: -1 } ?: 6,
            period = q["period"]?.takeIf { it.isNotEmpty() }?.let { it.trim().toIntOrNull() ?: -1 } ?: 30,
        )
        if (type == "hotp") a = a.copy(unsupported = "计数器型（HOTP）不支持")
        else if (type != "totp") a = a.copy(unsupported = "类型 ${type.ifEmpty { "空" }} 不支持")
        return finish(check(a))
    }

    /** 宽松的 base64：可带 URL 编码、base64url、缺填充。/ Lenient base64: URL-encoded, base64url or unpadded. */
    fun b64(s: String): ByteArray {
        var t = pct(s).replace(' ', '+').replace('-', '+').replace('_', '/').filter { it.isLetterOrDigit() && it.code < 128 || it == '+' || it == '/' }
        while (t.length % 4 != 0) t += "="
        return java.util.Base64.getDecoder().decode(t)
    }

    /** 极简 protobuf 读取器。/ Minimal protobuf reader. */
    private class Pb(val b: ByteArray, var p: Int = 0, val end: Int = b.size) {
        fun more() = p < end
        fun varint(): Long {
            var v = 0L
            var shift = 0
            repeat(10) {
                if (p >= end) throw ParseError("数据不完整")
                val c = b[p++].toInt() and 0xff
                if (shift < 64) v = v or ((c and 0x7f).toLong() shl shift)
                if (c and 0x80 == 0) return v
                shift += 7
            }
            throw ParseError("数据损坏")
        }
        fun bytes(): ByteArray {
            val n = varint()
            if (n < 0 || n > end - p) throw ParseError("数据不完整")
            val out = b.copyOfRange(p, p + n.toInt())
            p += n.toInt()
            return out
        }
        fun skip(wire: Int) {
            when (wire) {
                0 -> varint()
                1 -> p += 8
                2 -> bytes()
                5 -> p += 4
                else -> throw ParseError("数据损坏")
            }
            if (p > end) throw ParseError("数据不完整")
        }
    }

    // int32 枚举：负数按 64 位补码读出来会很大，当未知值。/ Negative enums decode huge; treat them as unknown.
    private fun pbInt(v: Long) = if (v < 0 || v > 0x7fffffff) -1 else v.toInt()

    private fun readOtp(bytes: ByteArray): Parsed {
        val r = Pb(bytes)
        var secret = ByteArray(0)
        var name = ""
        var issuer = ""
        var algo = 0
        var digits = 0
        var type = 0
        while (r.more()) {
            val tag = r.varint()
            val field = (tag ushr 3).toInt()
            val wire = (tag and 7).toInt()
            when {
                field == 1 && wire == 2 -> secret = r.bytes()
                field == 2 && wire == 2 -> name = String(r.bytes(), Charsets.UTF_8)
                field == 3 && wire == 2 -> issuer = String(r.bytes(), Charsets.UTF_8)
                field == 4 && wire == 0 -> algo = pbInt(r.varint())
                field == 5 && wire == 0 -> digits = pbInt(r.varint())
                field == 6 && wire == 0 -> type = pbInt(r.varint())
                else -> r.skip(wire)
            }
        }
        issuer = issuer.trim()
        name = name.trim()
        if (issuer.isNotEmpty() && name.lowercase().startsWith(issuer.lowercase() + ":")) {
            name = name.substring(issuer.length + 1).trim()
        } else if (issuer.isEmpty() && name.indexOf(':') > 0) {
            val (i, n) = splitLabel(name)
            issuer = i
            name = n
        }
        var unsupported = ""
        var alg = "SHA1"
        var dig = 6
        when (algo) {
            2 -> alg = "SHA256"
            3 -> alg = "SHA512"
            4 -> unsupported = "算法 MD5 不支持"
            0, 1 -> {}
            else -> unsupported = "未知算法"
        }
        if (digits == 2) dig = 8 else if (digits != 0 && digits != 1 && unsupported.isEmpty()) unsupported = "未知位数"
        if (type == 1) unsupported = "计数器型（HOTP）不支持" else if (type != 0 && type != 2) unsupported = "未知类型"
        if (unsupported.isEmpty() && (secret.size < 1 || secret.size > MAX_SECRET)) unsupported = "密钥长度不支持"
        val a = Parsed(issuer, name, Base32.encode(secret), alg, dig, 30, unsupported)
        secret.fill(0)
        return finish(check(a))
    }

    fun migration(uri: String): Migration {
        val m = MIGRATION.find(uri.trim())
        if (m == null || m.groupValues[1].isEmpty()) throw ParseError("导出链接里没有数据")
        val bytes = try { b64(m.groupValues[1]) } catch (e: Exception) { throw ParseError("导出数据不是有效的 base64") }
        val r = Pb(bytes)
        val list = mutableListOf<Parsed>()
        var version = 0L
        var size = 1L
        var index = 0L
        var id = 0L
        while (r.more()) {
            val tag = r.varint()
            val field = (tag ushr 3).toInt()
            val wire = (tag and 7).toInt()
            when {
                field == 1 && wire == 2 -> list += readOtp(r.bytes())
                field == 2 && wire == 0 -> version = r.varint()
                field == 3 && wire == 0 -> size = r.varint()
                field == 4 && wire == 0 -> index = r.varint()
                field == 5 && wire == 0 -> id = r.varint()
                else -> r.skip(wire)
            }
        }
        bytes.fill(0)
        if (list.isEmpty()) throw ParseError("导出数据里没有账号")
        return Migration(list, version, Batch(if (size < 1) 1 else size.toInt(), index.toInt(), id))
    }

    /** 一段文本，每行一个链接。/ Text with one link per line. */
    fun lines(text: String): Lines {
        val accounts = mutableListOf<Parsed>()
        val batches = mutableListOf<Batch>()
        val errors = mutableListOf<Pair<Int, String>>()
        text.split(Regex("\\r?\\n")).forEachIndexed { i, raw ->
            val line = raw.trim()
            if (line.isEmpty()) return@forEachIndexed
            try {
                when {
                    line.startsWith("otpauth-migration:", ignoreCase = true) -> {
                        val mg = migration(line)
                        accounts += mg.accounts
                        batches += mg.batch
                    }
                    line.startsWith("otpauth:", ignoreCase = true) -> accounts += otpauth(line)
                    else -> throw ParseError("不是 otpauth 链接")
                }
            } catch (e: ParseError) {
                errors += (i + 1) to (e.message ?: "")
            }
        }
        return Lines(accounts, batches, errors)
    }

    /** 分批导出缺了哪几张（1 起）。/ Missing parts of multi-part exports, 1-based. */
    fun missing(batches: List<Batch>): List<Pair<Int, List<Int>>> =
        batches.filter { it.size > 1 }.groupBy { it.id }.mapNotNull { (_, g) ->
            val seen = g.map { it.index }.toSet()
            val miss = (0 until g[0].size).filter { it !in seen }.map { it + 1 }
            if (miss.isEmpty()) null else g[0].size to miss
        }

    /** 小纸 Pico 屏幕上的热点二维码：WIFI:T:WPA;S:名称;P:口令;;（\ 转义）。/ The hotspot QR on the Read Pico screen:
     *  WIFI:T:WPA;S:name;P:pass;; with backslash escapes. */
    fun wifi(text: String): Pair<String, String>? {
        if (!text.startsWith("WIFI:", ignoreCase = true)) return null
        val fields = HashMap<String, String>()
        var i = 5
        while (i < text.length) {
            val colon = text.indexOf(':', i)
            if (colon < 0) break
            val key = text.substring(i, colon).uppercase()
            val sb = StringBuilder()
            var j = colon + 1
            while (j < text.length && text[j] != ';') {
                if (text[j] == '\\' && j + 1 < text.length) j++
                sb.append(text[j])
                j++
            }
            fields[key] = sb.toString()
            i = j + 1
            if (i < text.length && text[i] == ';') break
        }
        val ssid = fields["S"] ?: return null
        return ssid to (fields["P"] ?: "")
    }
}
