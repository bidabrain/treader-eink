package com.treader.auth.data

import android.content.Context
import android.os.SystemClock
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import java.io.File
import java.security.KeyStore
import javax.crypto.KeyGenerator
import javax.crypto.Mac
import javax.crypto.SecretKey

/*
 * 验证器会话，和固件 main/app/tr_auth.c 对应：设备钥匙在 Android Keystore（别名 treader_auth_device，拿不出手机），
 * 账号表加密放在 App 私有目录 vault.bin；解锁后钥匙和账号表只在内存，上锁时清零。输错次数和自动上锁存
 * SharedPreferences（不进备份）。口令规则、等待时间和固件一样。
 * The authenticator session, matching the firmware's main/app/tr_auth.c: the device key lives in the Android
 * Keystore (alias treader_auth_device, never leaves the phone), the encrypted table in the app's private vault.bin;
 * once unlocked the key and table sit in memory only and are zeroed on lock. Failures and auto-lock live in
 * SharedPreferences (not backed up). Passphrase rules and waits match the firmware.
 */

/** Keystore 里的 HMAC 密钥。/ The HMAC key in the Keystore. */
class KeystoreDeviceKey : DeviceKey {
    private val alias = "treader_auth_device"

    private fun key(create: Boolean): SecretKey? {
        val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (ks.getKey(alias, null) as? SecretKey)?.let { return it }
        if (!create) return null
        val gen = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_HMAC_SHA256, "AndroidKeyStore")
        gen.init(KeyGenParameterSpec.Builder(alias, KeyProperties.PURPOSE_SIGN).build())
        return gen.generateKey()
    }

    fun exists() = key(false) != null
    fun create() { key(true) }
    fun delete() {
        val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        if (ks.containsAlias(alias)) ks.deleteEntry(alias)
    }

    override fun mix(pk: ByteArray): ByteArray {
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(key(false) ?: throw VaultError(VaultError.Kind.NOENT))
        mac.update("treader-auth-v1".toByteArray())
        return mac.doFinal(pk)
    }
}

enum class AuthState { UNSET, LOCKED, OPEN, BROKEN }

class AuthStore(ctx: Context) {
    private val file = File(ctx.filesDir, "vault.bin")
    private val prefs = ctx.getSharedPreferences("auth", Context.MODE_PRIVATE)
    private val dev = KeystoreDeviceKey()
    private var key: FileKey? = null
    var table: Table? = null
        private set
    // 进程开始时按已有的输错次数重新等（重启不清零）。/ On process start the wait restarts from the saved count.
    private var untilMs = SystemClock.elapsedRealtime() + waitFor(prefs.getInt("fails", 0)) * 1000L

    val state: AuthState
        get() = when {
            !file.exists() && !dev.exists() -> AuthState.UNSET
            !file.exists() || !dev.exists() -> AuthState.BROKEN
            table != null -> AuthState.OPEN
            else -> AuthState.LOCKED
        }

    val fails: Int get() = prefs.getInt("fails", 0)

    /** 还要等几秒。/ Seconds left to wait. */
    fun waitS(): Int = ((untilMs - SystemClock.elapsedRealtime() + 999) / 1000).toInt().coerceAtLeast(0)

    /** 离开就上锁（0）还是离开 5 分钟后（1）。/ Lock on leaving (0) or five minutes after leaving (1). */
    var autoLock: Int
        get() = prefs.getInt("auto", 0)
        set(v) = prefs.edit().putInt("auto", v).apply()

    private fun setFails(n: Int) {
        prefs.edit().putInt("fails", n.coerceAtMost(255)).apply()
        untilMs = SystemClock.elapsedRealtime() + waitFor(n) * 1000L
    }

    /** 第一次设置：建设备钥匙、量迭代次数、写空文件并解锁。/ First setup: create the device key, time the iterations,
     *  write an empty file and unlock. */
    fun setup(pass: CharArray) {
        dev.create()
        val k = Vault.newKey(pass, dev, pickIters())
        val t = Table()
        try {
            Vault.save(file, k, t)
        } catch (e: Exception) {
            k.wipe()
            dev.delete()
            throw e
        }
        setFails(0)
        install(k, t)
    }

    /** 解锁；输错计数。/ Unlock; wrong tries are counted. */
    fun unlock(pass: CharArray) {
        try {
            val (k, t) = Vault.unlock(file, pass, dev)
            if (fails > 0) setFails(0)
            install(k, t)
        } catch (e: VaultError) {
            if (e.kind == VaultError.Kind.AUTH) setFails(fails + 1)
            throw e
        }
    }

    /** 改口令：旧口令对了才换。/ Change the passphrase only with the right old one. */
    fun change(old: CharArray, new: CharArray) {
        val (k, t) = try {
            Vault.unlock(file, old, dev)
        } catch (e: VaultError) {
            if (e.kind == VaultError.Kind.AUTH) setFails(fails + 1)
            throw e
        }
        if (fails > 0) setFails(0)
        k.wipe()
        val nk = Vault.newKey(new, dev, pickIters())
        Vault.save(file, nk, t)
        if (table != null) install(nk, t) else { nk.wipe(); t.wipe() }
    }

    fun save() {
        val k = key ?: return
        val t = table ?: return
        Vault.save(file, k, t)
    }

    fun lock() {
        key?.wipe()
        table?.wipe()
        key = null
        table = null
    }

    /** 清空：删文件、删设备钥匙和计数。/ Clear: delete the file, the device key and the counters. */
    fun wipe() {
        lock()
        Vault.remove(file)
        dev.delete()
        prefs.edit().remove("fails").apply()
        untilMs = 0
    }

    private fun install(k: FileKey, t: Table) {
        lock()
        key = k
        table = t
    }

    // 解锁约 0.8 秒：先量 2 万次。/ About 0.8 s to unlock: time 20 000 iterations first.
    private fun pickIters(): Int {
        val ns = Vault.time(20_000).coerceAtLeast(1)
        return (20_000L * 800_000_000L / ns).coerceIn(Vault.MIN_ITERS.toLong(), 2_000_000L).toInt()
    }

    companion object {
        const val PASS_MIN = 8
        const val PASS_MAX = 64
        const val FREE_TRIES = 4

        /** 第 5 次起要等：30 秒、1 分、2 分……最长 15 分钟。/ From the fifth miss: 30 s, 1 min, 2 min... up to 15 min. */
        fun waitFor(fails: Int): Int {
            if (fails <= FREE_TRIES) return 0
            var s = 30
            for (i in FREE_TRIES + 1 until fails) { if (s >= 900) break; s *= 2 }
            return s.coerceAtMost(900)
        }

        /** 口令是否合格，不合格返回原因。/ Whether a passphrase qualifies; the reason otherwise. */
        fun passProblem(p: CharArray): String? = when {
            p.size < PASS_MIN -> "口令至少 8 位"
            p.size > PASS_MAX -> "口令最多 64 位"
            p.none { it in 'a'..'z' || it in 'A'..'Z' } || p.none { it in '0'..'9' } -> "口令要同时有字母和数字"
            else -> null
        }
    }
}
