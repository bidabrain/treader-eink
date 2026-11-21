package com.treader.auth

import com.treader.auth.data.Account
import com.treader.auth.data.Algo
import com.treader.auth.data.AuthStore
import com.treader.auth.data.Base32
import com.treader.auth.data.DeviceKey
import com.treader.auth.data.OtpParse
import com.treader.auth.data.Table
import com.treader.auth.data.Totp
import com.treader.auth.data.Vault
import com.treader.auth.data.VaultError
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.RandomAccessFile
import java.net.URLEncoder
import java.util.Base64
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

/*
 * 和固件相同的检查：RFC 6238 附录 B 向量、base32、otpauth / 迁移数据解析（迁移数据在测试里自己编码）、热点二维码、
 * 加密文件读写和损坏、口令规则和等待时间。
 * The same checks as the firmware: RFC 6238 appendix B vectors, base32, otpauth and migration parsing (migration
 * payloads are encoded here), the hotspot QR, the encrypted file and its failures, passphrase rules and waits.
 */
class AuthTest {
    private fun acct(secret: String, algo: Algo, digits: Int = 8) =
        Account(1, "", "t", secret.toByteArray(), algo, digits, 30)

    @Test fun rfc6238() {
        val v = listOf(
            longArrayOf(59, 94287082, 46119246, 90693936),
            longArrayOf(1111111109, 7081804, 68084774, 25091201),
            longArrayOf(1111111111, 14050471, 67062674, 99943326),
            longArrayOf(1234567890, 89005924, 91819424, 93441116),
            longArrayOf(2000000000, 69279037, 90698825, 38618901),
            longArrayOf(20000000000, 65353130, 77737706, 47863826),
        )
        val a1 = acct("12345678901234567890", Algo.SHA1)
        val a2 = acct("12345678901234567890123456789012", Algo.SHA256)
        val a5 = acct("1234567890123456789012345678901234567890123456789012345678901234", Algo.SHA512)
        for (r in v) {
            assertEquals(r[1].toInt(), Totp.code(a1, r[0]))
            assertEquals(r[2].toInt(), Totp.code(a2, r[0]))
            assertEquals(r[3].toInt(), Totp.code(a5, r[0]))
        }
        assertEquals(287082, Totp.code(acct("12345678901234567890", Algo.SHA1, 6), 59))
        assertEquals("0708 1804", Totp.format(7081804, 8))
        assertEquals("081 804", Totp.format(81804, 6))
        assertEquals(1, Totp.remaining(a1, 59))
        assertEquals(30, Totp.remaining(a1, 60))
    }

    @Test fun base32() {
        assertEquals("JBSWY3DPEE", Base32.encode("Hello!".toByteArray()))
        assertEquals("GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ", Base32.encode("12345678901234567890".toByteArray()))
        assertArrayEquals("12345678901234567890".toByteArray(), Base32.decode("gezd gnbv-gy3t qojq gezd gnbv gy3t qojq"))
        assertArrayEquals("Hello!".toByteArray(), Base32.decode("JBSWY3DPEE======"))
        assertNull(Base32.decode("JBSWY3DPE1"))
        assertNull(Base32.decode(""))
    }

    @Test fun otpauth() {
        val a = OtpParse.otpauth("otpauth://totp/Example:alice@google.com?secret=JBSWY3DPEHPK3PXP&issuer=Example")
        assertEquals("Example", a.issuer); assertEquals("alice@google.com", a.name); assertEquals("", a.unsupported)
        val b = OtpParse.otpauth("otpauth://totp/ACME%20Co%3Ajohn.doe%40email.com?secret=hxdm vjec&algorithm=SHA256&digits=8&period=60")
        assertEquals("ACME Co", b.issuer); assertEquals("john.doe@email.com", b.name)
        assertEquals("HXDMVJEC", b.secret); assertEquals("SHA256", b.algo); assertEquals(8, b.digits); assertEquals(60, b.period)
        val c = OtpParse.otpauth("otpauth://totp/Label:me?secret=JBSWY3DPEE&issuer=Real")
        assertEquals("Real", c.issuer)
        assertEquals("计数器型（HOTP）不支持", OtpParse.otpauth("otpauth://hotp/x?secret=JBSWY3DPEE&counter=1").unsupported)
        assertTrue(OtpParse.otpauth("otpauth://totp/x?secret=JBSWY3DPEE&digits=7").unsupported.isNotEmpty())
        assertTrue(OtpParse.otpauth("otpauth://totp/x?secret=1111").unsupported.isNotEmpty())
        val long = OtpParse.otpauth("otpauth://totp/" + "名".repeat(30) + "?secret=JBSWY3DPEE")
        assertTrue(long.name.toByteArray().size <= 64); assertTrue(long.notes.isNotEmpty())
        assertEquals("x", OtpParse.otpauth("otpauth://totp/?secret=JBSWY3DPEE&issuer=x").name)
    }

    // 极简 protobuf 写入。/ Minimal protobuf writer.
    private class Pb {
        val o = ByteArrayOutputStream()
        fun varint(v: Long) { var x = v; while (x and 0x7f.inv().toLong() != 0L) { o.write(((x and 0x7f) or 0x80).toInt()); x = x ushr 7 }; o.write(x.toInt()) }
        fun tag(f: Int, w: Int) = varint(((f shl 3) or w).toLong())
        fun bytes(f: Int, b: ByteArray) { tag(f, 2); varint(b.size.toLong()); o.write(b) }
        fun int(f: Int, v: Long) { tag(f, 0); varint(v) }
    }

    private fun otp(secret: ByteArray, name: String, issuer: String, algo: Int, digits: Int, type: Int): ByteArray {
        val p = Pb()
        p.bytes(1, secret); p.bytes(2, name.toByteArray()); p.bytes(3, issuer.toByteArray())
        p.int(4, algo.toLong()); p.int(5, digits.toLong()); p.int(6, type.toLong())
        p.bytes(9, "unknown".toByteArray())
        return p.o.toByteArray()
    }

    @Test fun migration() {
        val p = Pb()
        p.bytes(1, otp("12345678901234567890".toByteArray(), "GitHub:me", "GitHub", 1, 1, 2))
        p.bytes(1, otp(ByteArray(32) { it.toByte() }, "box", "Box", 2, 2, 2))
        p.bytes(1, otp("abcdefghij".toByteArray(), "Old:hotp", "", 1, 1, 1))
        p.int(2, 1); p.int(3, 2); p.int(4, 0); p.int(5, 123456)
        val data = Base64.getEncoder().encodeToString(p.o.toByteArray())
        val uri = "otpauth-migration://offline?data=" + URLEncoder.encode(data, "UTF-8")
        val m = OtpParse.migration(uri)
        assertEquals(3, m.accounts.size)
        assertEquals("GitHub", m.accounts[0].issuer); assertEquals("me", m.accounts[0].name)
        assertEquals("GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ", m.accounts[0].secret)
        assertEquals("SHA256", m.accounts[1].algo); assertEquals(8, m.accounts[1].digits)
        assertEquals("Old", m.accounts[2].issuer); assertEquals("hotp", m.accounts[2].name)
        assertEquals("计数器型（HOTP）不支持", m.accounts[2].unsupported)
        assertEquals(2, m.batch.size); assertEquals(123456L, m.batch.id)
        assertEquals(listOf(2 to listOf(2)), OtpParse.missing(listOf(m.batch)))
        // 直接放 base64（不编码）、base64url 也认。/ Raw and base64url data parse too.
        assertEquals(3, OtpParse.migration("otpauth-migration://offline?data=$data").accounts.size)
        val url = data.replace('+', '-').replace('/', '_').trimEnd('=')
        assertEquals(3, OtpParse.migration("otpauth-migration://offline?data=$url").accounts.size)
        val lines = OtpParse.lines("$uri\notpauth://totp/a?secret=JBSWY3DPEE\nhello")
        assertEquals(4, lines.accounts.size); assertEquals(1, lines.errors.size); assertEquals(3, lines.errors[0].first)
        try { OtpParse.migration("otpauth-migration://offline?data="); fail() } catch (e: Exception) {}
        try { OtpParse.migration("otpauth-migration://offline?data=AAEC"); fail() } catch (e: Exception) {}
        // 导入转账号。/ Into accounts.
        val acc = m.accounts[0].toAccount()!!
        assertEquals(89005924 % 1000000, Totp.code(acc, 1234567890))
        assertNull(m.accounts[2].toAccount())
    }

    @Test fun wifi() {
        assertEquals("ReadPico-3A7F" to "k7pq2mxa9tvr", OtpParse.wifi("WIFI:T:WPA;S:ReadPico-3A7F;P:k7pq2mxa9tvr;;"))
        assertEquals("a;b" to "c:d", OtpParse.wifi("WIFI:S:a\\;b;T:WPA;P:c\\:d;;"))
        assertNull(OtpParse.wifi("otpauth://totp/x"))
    }

    private val devA = object : DeviceKey {
        override fun mix(pk: ByteArray): ByteArray = Mac.getInstance("HmacSHA256").run {
            init(SecretKeySpec(ByteArray(32) { 7 }, "HmacSHA256")); doFinal(pk)
        }
    }
    private val devB = object : DeviceKey {
        override fun mix(pk: ByteArray): ByteArray = Mac.getInstance("HmacSHA256").run {
            init(SecretKeySpec(ByteArray(32) { 8 }, "HmacSHA256")); doFinal(pk)
        }
    }

    private fun flip(f: File, off: Long) = RandomAccessFile(f, "rw").use { r ->
        r.seek(off); val c = r.read(); r.seek(off); r.write(c xor 1)
    }

    private fun kind(block: () -> Unit): VaultError.Kind? = try { block(); null } catch (e: VaultError) { e.kind }

    @Test fun vault() {
        val f = File.createTempFile("vault", ".bin")
        f.delete()
        assertEquals(VaultError.Kind.NOENT, kind { Vault.unlock(f, "abc12345".toCharArray(), devA) })
        val k = Vault.newKey("abc12345".toCharArray(), devA, 1000)
        assertEquals(Vault.MIN_ITERS, k.iters)
        val t = Table()
        assertTrue(t.add(Account(0, "示例", "me@example.com", "12345678901234567890".toByteArray(), Algo.SHA256, 8, 30)))
        assertTrue(t.add(Account(0, "", "second", ByteArray(10) { 1 }, Algo.SHA1, 6, 60)))
        assertFalse(t.add(Account(0, "", "second", ByteArray(10) { 1 }, Algo.SHA1, 6, 60)))
        Vault.save(f, k, t)
        val (k2, r) = Vault.unlock(f, "abc12345".toCharArray(), devA)
        assertArrayEquals(k.key, k2.key)
        assertEquals(2, r.items.size); assertEquals(3, r.nextId)
        assertEquals("示例", r.items[0].issuer); assertEquals(60, r.items[1].period)
        assertEquals(Totp.code(t.items[0], 1234567890), Totp.code(r.items[0], 1234567890))
        assertEquals(VaultError.Kind.AUTH, kind { Vault.unlock(f, "abc12346".toCharArray(), devA) })
        assertEquals(VaultError.Kind.AUTH, kind { Vault.unlock(f, "abc12345".toCharArray(), devB) })
        flip(f, 50)
        assertEquals(VaultError.Kind.AUTH, kind { Vault.load(f, k) })
        flip(f, 50)
        flip(f, 6)
        assertEquals(VaultError.Kind.AUTH, kind { Vault.load(f, k) })
        flip(f, 6)
        assertEquals(2, Vault.load(f, k).items.size)
        RandomAccessFile(f, "rw").use { it.setLength(60) }
        assertEquals(VaultError.Kind.FORMAT, kind { Vault.load(f, k) })
        // 换口令后旧口令解不开。/ After a passphrase change the old one fails.
        Vault.save(f, Vault.newKey("new98765".toCharArray(), devA, 1), t)
        assertEquals(VaultError.Kind.AUTH, kind { Vault.unlock(f, "abc12345".toCharArray(), devA) })
        assertEquals(2, Vault.unlock(f, "new98765".toCharArray(), devA).second.items.size)
        Vault.remove(f)
        assertFalse(f.exists())
    }

    @Test fun rules() {
        assertEquals("口令至少 8 位", AuthStore.passProblem("abc1234".toCharArray()))
        assertEquals("口令要同时有字母和数字", AuthStore.passProblem("abcdefgh".toCharArray()))
        assertNull(AuthStore.passProblem("abcd1234".toCharArray()))
        assertEquals(0, AuthStore.waitFor(4)); assertEquals(30, AuthStore.waitFor(5)); assertEquals(60, AuthStore.waitFor(6))
        assertEquals(900, AuthStore.waitFor(20))
    }
}
