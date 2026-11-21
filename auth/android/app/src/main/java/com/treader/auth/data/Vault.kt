package com.treader.auth.data

import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.SecureRandom
import javax.crypto.AEADBadTagException
import javax.crypto.Cipher
import javax.crypto.SecretKeyFactory
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.PBEKeySpec
import javax.crypto.spec.SecretKeySpec

/*
 * 加密文件。格式（小端）和固件 totp_vault.c 同一套，只是 KDF 编号是 2：
 *   0 "TRVA"  4 版本 1  5 KDF 2  6 保留 2 字节  8 迭代次数 u32  12 盐 16 字节  28 GCM nonce 12 字节
 *   40 明文长度 u32  44 密文  然后 16 字节 GCM 标签；前 44 字节是附加数据。
 * 钥匙 = 设备钥匙 mix(PBKDF2-HMAC-SHA256(口令, 盐, 迭代))；手机上设备钥匙是 Android Keystore 里拿不出来的 HMAC 密钥，
 * 所以单有口令或单有文件都解不开。明文账号表的二进制格式和固件相同。写入先写 .tmp 再改名（覆盖是原子的）。
 * The encrypted file. Same little-endian layout as the firmware's totp_vault.c but with KDF id 2. Key = device key
 * mix(PBKDF2-HMAC-SHA256(passphrase, salt, iterations)); on the phone the device key is a non-exportable HMAC key in
 * the Android Keystore, so neither the passphrase nor the file alone opens it. The plaintext table uses the firmware's
 * binary layout. Writes go to .tmp and are renamed into place (an atomic overwrite).
 */

/** 和口令混合的设备钥匙（手机上是 Keystore，测试里是普通密钥）。/ The device key mixed with the passphrase (the
 *  Keystore on the phone, a plain key in tests). */
interface DeviceKey {
    /** 输入 32 字节，输出 32 字节。/ 32 bytes in, 32 bytes out. */
    fun mix(pk: ByteArray): ByteArray
}

/** 文件钥匙：盐和迭代次数随文件走。/ The file key; salt and iterations travel with the file. */
class FileKey(val key: ByteArray, val salt: ByteArray, val iters: Int) {
    fun wipe() = key.fill(0)
}

/** 解开的账号表。/ The decrypted table. */
class Table(var nextId: Int = 1, val items: MutableList<Account> = mutableListOf()) {
    fun wipe() = items.forEach { it.wipe() }

    /** 加一个，编号自动分配；满了、无效或已有返回 false。/ Add one with a new id; false when full, invalid or present. */
    fun add(a: Account): Boolean {
        if (!a.valid() || items.size >= MAX_ACCOUNTS || items.any { it.same(a) }) return false
        items += Account(nextId++, a.issuer, a.name, a.secret.copyOf(), a.algo, a.digits, a.period)
        return true
    }
}

class VaultError(val kind: Kind) : Exception(kind.name) {
    enum class Kind { NOENT, FORMAT, AUTH, IO }
}

object Vault {
    const val MIN_ITERS = 100_000
    private const val HDR = 44
    private const val TAG = 16
    private val MAGIC = byteArrayOf('T'.code.toByte(), 'R'.code.toByte(), 'V'.code.toByte(), 'A'.code.toByte())
    private val rng = SecureRandom()

    fun pbkdf2(pass: CharArray, salt: ByteArray, iters: Int): ByteArray {
        val spec = PBEKeySpec(pass, salt, iters, 256)
        try {
            return SecretKeyFactory.getInstance("PBKDF2WithHmacSHA256").generateSecret(spec).encoded
        } finally {
            spec.clearPassword()
        }
    }

    fun derive(pass: CharArray, dev: DeviceKey, salt: ByteArray, iters: Int): FileKey {
        val pk = pbkdf2(pass, salt, iters)
        try {
            return FileKey(dev.mix(pk), salt, iters)
        } finally {
            pk.fill(0)
        }
    }

    fun newKey(pass: CharArray, dev: DeviceKey, iters: Int): FileKey {
        val salt = ByteArray(16).also { rng.nextBytes(it) }
        return derive(pass, dev, salt, maxOf(iters, MIN_ITERS))
    }

    /** 量 n 次 PBKDF2 要多少纳秒。/ Nanoseconds for n PBKDF2 iterations. */
    fun time(n: Int): Long {
        val t0 = System.nanoTime()
        pbkdf2("calibrate".toCharArray(), ByteArray(16), n).fill(0)
        return System.nanoTime() - t0
    }

    /* ---- 明文 / Plaintext ---- */

    fun encode(t: Table): ByteArray {
        val o = ByteArrayOutputStream()
        fun u32(v: Int) { o.write(v and 0xff); o.write(v ushr 8 and 0xff); o.write(v ushr 16 and 0xff); o.write(v ushr 24 and 0xff) }
        o.write(1)
        u32(t.nextId)
        o.write(t.items.size and 0xff); o.write(t.items.size ushr 8 and 0xff)
        for (a in t.items) {
            u32(a.id)
            o.write(a.algo.id); o.write(a.digits); o.write(a.period); o.write(a.secret.size)
            o.write(a.secret)
            val i = a.issuer.toByteArray(Charsets.UTF_8)
            val n = a.name.toByteArray(Charsets.UTF_8)
            o.write(i.size); o.write(i)
            o.write(n.size); o.write(n)
        }
        return o.toByteArray()
    }

    fun decode(p: ByteArray): Table {
        fun bad(): Nothing = throw VaultError(VaultError.Kind.FORMAT)
        if (p.size < 7 || p[0].toInt() != 1) bad()
        val b = ByteBuffer.wrap(p).order(ByteOrder.LITTLE_ENDIAN)
        b.position(1)
        val t = Table(b.int)
        val count = (b.get().toInt() and 0xff) or ((b.get().toInt() and 0xff) shl 8)
        if (count > MAX_ACCOUNTS) bad()
        try {
            repeat(count) {
                val id = b.int
                val algo = Algo.of(b.get().toInt() and 0xff) ?: bad()
                val digits = b.get().toInt() and 0xff
                val period = b.get().toInt() and 0xff
                val sl = b.get().toInt() and 0xff
                if (sl > MAX_SECRET) bad()
                val s = ByteArray(sl).also { b.get(it) }
                val il = b.get().toInt() and 0xff
                if (il > MAX_TEXT) bad()
                val issuer = ByteArray(il).also { b.get(it) }.toString(Charsets.UTF_8)
                val nl = b.get().toInt() and 0xff
                if (nl > MAX_TEXT) bad()
                val name = ByteArray(nl).also { b.get(it) }.toString(Charsets.UTF_8)
                val a = Account(id, issuer, name, s, algo, digits, period)
                if (!a.valid()) bad()
                t.items += a
            }
        } catch (e: java.nio.BufferUnderflowException) {
            bad()
        }
        if (b.hasRemaining()) bad()
        return t
    }

    /* ---- 文件 / File ---- */

    private fun header(f: File): ByteArray {
        if (!f.exists()) throw VaultError(VaultError.Kind.NOENT)
        val all = try { f.readBytes() } catch (e: Exception) { throw VaultError(VaultError.Kind.IO) }
        if (all.size < HDR + TAG || !all.copyOfRange(0, 4).contentEquals(MAGIC) || all[4].toInt() != 1 || all[5].toInt() != 2)
            throw VaultError(VaultError.Kind.FORMAT)
        val len = ByteBuffer.wrap(all, 40, 4).order(ByteOrder.LITTLE_ENDIAN).int
        if (len < 0 || len.toLong() + HDR + TAG != all.size.toLong()) throw VaultError(VaultError.Kind.FORMAT)
        return all
    }

    /** 读文件头里的盐和迭代次数。/ Read salt and iterations from the header. */
    fun params(f: File): Pair<ByteArray, Int> {
        val all = header(f)
        return all.copyOfRange(12, 28) to ByteBuffer.wrap(all, 8, 4).order(ByteOrder.LITTLE_ENDIAN).int
    }

    fun load(f: File, k: FileKey): Table {
        val all = header(f)
        if (!all.copyOfRange(12, 28).contentEquals(k.salt) || ByteBuffer.wrap(all, 8, 4).order(ByteOrder.LITTLE_ENDIAN).int != k.iters)
            throw VaultError(VaultError.Kind.AUTH)
        val c = Cipher.getInstance("AES/GCM/NoPadding")
        c.init(Cipher.DECRYPT_MODE, SecretKeySpec(k.key, "AES"), GCMParameterSpec(128, all, 28, 12))
        c.updateAAD(all, 0, HDR)
        val plain = try {
            c.doFinal(all, HDR, all.size - HDR)
        } catch (e: AEADBadTagException) {
            throw VaultError(VaultError.Kind.AUTH)
        }
        try {
            return decode(plain)
        } finally {
            plain.fill(0)
        }
    }

    fun unlock(f: File, pass: CharArray, dev: DeviceKey): Pair<FileKey, Table> {
        val (salt, iters) = params(f)
        if (iters < 1) throw VaultError(VaultError.Kind.FORMAT)
        val k = derive(pass, dev, salt, iters)
        try {
            return k to load(f, k)
        } catch (e: Exception) {
            k.wipe()
            throw e
        }
    }

    fun save(f: File, k: FileKey, t: Table) {
        val plain = encode(t)
        val hdr = ByteBuffer.allocate(HDR).order(ByteOrder.LITTLE_ENDIAN)
        hdr.put(MAGIC).put(1).put(2).put(0).put(0).putInt(k.iters).put(k.salt)
        val nonce = ByteArray(12).also { rng.nextBytes(it) }
        hdr.put(nonce).putInt(plain.size)
        val head = hdr.array()
        val c = Cipher.getInstance("AES/GCM/NoPadding")
        c.init(Cipher.ENCRYPT_MODE, SecretKeySpec(k.key, "AES"), GCMParameterSpec(128, nonce))
        c.updateAAD(head)
        val ct = c.doFinal(plain)
        plain.fill(0)
        val tmp = File(f.path + ".tmp")
        try {
            tmp.outputStream().use { o ->
                o.write(head)
                o.write(ct)
                o.fd.sync()
            }
            java.nio.file.Files.move(tmp.toPath(), f.toPath(), java.nio.file.StandardCopyOption.ATOMIC_MOVE,
                java.nio.file.StandardCopyOption.REPLACE_EXISTING)
        } catch (e: VaultError) {
            tmp.delete()
            throw e
        } catch (e: Exception) {
            tmp.delete()
            throw VaultError(VaultError.Kind.IO)
        }
    }

    fun remove(f: File) {
        File(f.path + ".tmp").delete()
        f.delete()
    }
}
