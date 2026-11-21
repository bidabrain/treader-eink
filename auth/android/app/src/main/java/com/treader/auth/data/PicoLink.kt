package com.treader.auth.data

import android.content.Context
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.WifiNetworkSpecifier
import android.os.Build
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import org.json.JSONArray
import org.json.JSONObject
import java.util.concurrent.TimeUnit
import kotlin.coroutines.resume

/*
 * “发送到小纸 Pico”：连上设备导入页开的热点（安卓 10 起自动连，系统会弹窗确认；更早的系统先在手机设置里手动连），
 * 用固件导入网页同一个接口把账号发到 http://192.168.4.1/form，再等用户在设备上点“导入”。只走这个热点，不经过
 * 路由器和互联网；固件不用改。
 * "Send to Read Pico": join the hotspot the device's import page opens (automatic from Android 10 with a system
 * confirmation; older systems join it in phone settings first), post the accounts to http://192.168.4.1/form through
 * the same interface as the firmware import page, then wait for the user to tap Import on the device. Traffic stays
 * on that hotspot, never a router or the internet; no firmware change is needed.
 */
class PicoLink(private val ctx: Context) {
    private val cm = ctx.getSystemService(ConnectivityManager::class.java)
    private var callback: ConnectivityManager.NetworkCallback? = null
    private var network: Network? = null

    /** 连热点；成功返回 null，失败返回原因。/ Join the hotspot; null on success, else the reason. */
    suspend fun connect(ssid: String, pass: String): String? {
        close()
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return null   // 用户已手动连上 / Joined by hand
        val spec = WifiNetworkSpecifier.Builder().setSsid(ssid).apply { if (pass.isNotEmpty()) setWpa2Passphrase(pass) }.build()
        val req = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .setNetworkSpecifier(spec)
            .build()
        val got = withTimeoutOrNull(60_000) {
            suspendCancellableCoroutine { cont ->
                val cb = object : ConnectivityManager.NetworkCallback() {
                    override fun onAvailable(n: Network) {
                        network = n
                        if (cont.isActive) cont.resume(true)
                    }
                    override fun onUnavailable() {
                        if (cont.isActive) cont.resume(false)
                    }
                    override fun onLost(n: Network) {
                        if (network == n) network = null
                    }
                }
                callback = cb
                cm.requestNetwork(req, cb)
                cont.invokeOnCancellation { }
            }
        }
        return when (got) {
            true -> null
            false -> "没连上设备热点（被取消或找不到）"
            null -> "连设备热点超时"
        }
    }

    private fun client(): OkHttpClient {
        val b = OkHttpClient.Builder().connectTimeout(8, TimeUnit.SECONDS).readTimeout(15, TimeUnit.SECONDS)
        network?.let { b.socketFactory(it.socketFactory) }
        return b.build()
    }

    /** 发送；返回 null 表示设备已收下、等确认，否则是原因。/ Send; null means the device took it and awaits
     *  confirmation, else the reason. */
    suspend fun send(accounts: List<Account>): String? = withContext(Dispatchers.IO) {
        val list = JSONArray()
        accounts.forEach {
            list.put(JSONObject().put("issuer", it.issuer).put("name", it.name).put("secret", Base32.encode(it.secret))
                .put("algo", it.algo.label).put("digits", it.digits).put("period", it.period))
        }
        val body = JSONObject().put("op", "import").put("accounts", list).toString()
        try {
            client().newCall(
                Request.Builder().url(URL).post(body.toRequestBody("application/json".toMediaType())).build()
            ).execute().use { r ->
                if (r.isSuccessful) null
                else runCatching { JSONObject(r.body?.string() ?: "").optString("error") }.getOrNull()?.ifEmpty { null }
                    ?: "设备拒绝了（HTTP ${r.code}）"
            }
        } catch (e: Exception) {
            "连不上设备：先在设备的验证器里打开“导入”，并确认手机连的是设备热点"
        }
    }

    /** 等设备上确认；返回 (是否成功, 说明)。/ Wait for the confirmation on the device; returns (success, message). */
    suspend fun waitResult(): Pair<Boolean, String> = withContext(Dispatchers.IO) {
        val deadline = System.currentTimeMillis() + 10 * 60_000
        while (System.currentTimeMillis() < deadline) {
            delay(1500)
            val last = try {
                client().newCall(Request.Builder().url(URL).build()).execute().use { r ->
                    JSONObject(r.body?.string() ?: "{}").optJSONObject("last")
                }
            } catch (e: Exception) {
                null
            } ?: continue
            if (last.optString("op") != "import") continue
            when (last.optString("state")) {
                "pending" -> continue
                "done" -> return@withContext true to last.optString("msg").ifEmpty { "已导入 ${last.optInt("count")} 个账号" }
                "cancelled" -> return@withContext false to "已在设备上取消"
                else -> return@withContext false to last.optString("msg").ifEmpty { "设备没能导入" }
            }
        }
        false to "等太久了，设备上没有确认"
    }

    /** 断开设备热点，手机回到原来的网络。/ Leave the hotspot; the phone returns to its usual network. */
    fun close() {
        callback?.let { runCatching { cm.unregisterNetworkCallback(it) } }
        callback = null
        network = null
    }

    companion object {
        const val URL = "http://192.168.4.1/form"
    }
}
