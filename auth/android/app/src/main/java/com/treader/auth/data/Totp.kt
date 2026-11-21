package com.treader.auth.data

import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

/*
 * 验证码计算（RFC 4226 截断、RFC 6238 时间步）、base32 和账号，和固件 components/read_pico_totp/totp.c 一致。
 * Code math (RFC 4226 truncation, RFC 6238 time steps), base32 and accounts, matching the firmware's
 * components/read_pico_totp/totp.c.
 */

const val MAX_ACCOUNTS = 100
const val MAX_TEXT = 64
const val MAX_SECRET = 64

/** 算法编号和固件相同：0 SHA1、1 SHA256、2 SHA512。/ Algorithm ids match the firmware: 0 SHA1, 1 SHA256, 2 SHA512. */
enum class Algo(val id: Int, val label: String, val mac: String) {
    SHA1(0, "SHA1", "HmacSHA1"), SHA256(1, "SHA256", "HmacSHA256"), SHA512(2, "SHA512", "HmacSHA512");

    companion object {
        fun of(id: Int) = entries.firstOrNull { it.id == id }
        fun of(label: String) = entries.firstOrNull { it.label == label }
    }
}

/** 一个账号。secret 是原始字节。/ One account; secret holds the raw bytes. */
class Account(
    val id: Int,
    val issuer: String,
    val name: String,
    val secret: ByteArray,
    val algo: Algo = Algo.SHA1,
    val digits: Int = 6,
    val period: Int = 30,
) {
    val label: String get() = if (issuer.isEmpty()) name else "$issuer · $name"

    fun valid(): Boolean = secret.size in 1..MAX_SECRET && (digits == 6 || digits == 8) && (period == 30 || period == 60) &&
        name.isNotEmpty() && utf8Len(name) <= MAX_TEXT && utf8Len(issuer) <= MAX_TEXT

    /** 同一个账号：发行方、名称、密钥都一样。/ The same account: identical issuer, name and secret. */
    fun same(o: Account) = issuer == o.issuer && name == o.name && secret.contentEquals(o.secret)

    fun copy(id: Int = this.id, issuer: String = this.issuer, name: String = this.name) =
        Account(id, issuer, name, secret.copyOf(), algo, digits, period)

    /** 用完清掉密钥。/ Wipe the secret after use. */
    fun wipe() = secret.fill(0)
}

object Totp {
    /** unixSec（UTC 秒）时的验证码。/ The code at unixSec (UTC seconds). */
    fun code(a: Account, unixSec: Long): Int {
        var counter = unixSec / a.period
        val msg = ByteArray(8)
        for (i in 7 downTo 0) {
            msg[i] = counter.toByte()
            counter = counter ushr 8
        }
        val mac = Mac.getInstance(a.algo.mac)
        mac.init(SecretKeySpec(a.secret, a.algo.mac))
        val h = mac.doFinal(msg)
        val off = h[h.size - 1].toInt() and 0x0f
        val bin = ((h[off].toInt() and 0x7f) shl 24) or ((h[off + 1].toInt() and 0xff) shl 16) or
            ((h[off + 2].toInt() and 0xff) shl 8) or (h[off + 3].toInt() and 0xff)
        h.fill(0)
        return bin % (if (a.digits == 8) 100_000_000 else 1_000_000)
    }

    /** 6 位写成“123 456”，8 位写成“1234 5678”。/ "123 456" for six digits, "1234 5678" for eight. */
    fun format(code: Int, digits: Int): String =
        if (digits == 8) "%04d %04d".format(code / 10000, code % 10000) else "%03d %03d".format(code / 1000 % 1000, code % 1000)

    /** 本周期还剩几秒（1..period）。/ Seconds left in this period (1..period). */
    fun remaining(a: Account, unixSec: Long): Int = a.period - (unixSec % a.period).toInt()
}

object Base32 {
    private const val ALPHA = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"

    /** 去空格、连字符和“=”，转大写。/ Strip spaces, dashes and '=', uppercase. */
    fun norm(s: String) = s.filterNot { it.isWhitespace() || it == '=' || it == '-' }.uppercase()

    /** 解码；不合法或不在 1..64 字节返回 null。/ Decode; null when invalid or not 1..64 bytes. */
    fun decode(s: String): ByteArray? {
        val n = norm(s)
        if (n.isEmpty()) return null
        val out = java.io.ByteArrayOutputStream()
        var buf = 0
        var bits = 0
        for (c in n) {
            val v = ALPHA.indexOf(c)
            if (v < 0) return null
            buf = (buf shl 5) or v
            bits += 5
            if (bits >= 8) {
                bits -= 8
                out.write((buf shr bits) and 0xff)
            }
        }
        val b = out.toByteArray()
        return if (b.size in 1..MAX_SECRET) b else null
    }

    /** 编码，无填充。/ Encode without padding. */
    fun encode(b: ByteArray): String {
        val sb = StringBuilder()
        var buf = 0
        var bits = 0
        for (x in b) {
            buf = (buf shl 8) or (x.toInt() and 0xff)
            bits += 8
            while (bits >= 5) {
                sb.append(ALPHA[(buf shr (bits - 5)) and 31])
                bits -= 5
            }
            buf = buf and ((1 shl bits) - 1)
        }
        if (bits > 0) sb.append(ALPHA[(buf shl (5 - bits)) and 31])
        return sb.toString()
    }
}

fun utf8Len(s: String) = s.toByteArray(Charsets.UTF_8).size

/** 按 UTF-8 字节截断，不切开字符。/ Cut to a UTF-8 byte budget on code-point boundaries. */
fun utf8Cut(s: String, max: Int): String {
    val sb = StringBuilder()
    var used = 0
    var i = 0
    while (i < s.length) {
        val cp = s.codePointAt(i)
        val ch = String(Character.toChars(cp))
        val n = utf8Len(ch)
        if (used + n > max) break
        sb.append(ch)
        used += n
        i += Character.charCount(cp)
    }
    return sb.toString()
}
