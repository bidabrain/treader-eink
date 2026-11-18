package com.treader.rednote.data

import android.content.Context
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import okhttp3.Request
import org.json.JSONObject
import java.io.IOException
import java.util.concurrent.TimeUnit

/**
 * rednote 服务器的设备接口（和固件 tomato_rednote.c 用的一样）：关注、推荐、笔记、图片，请求头带设备令牌。
 * The rednote server's device API (the same one the firmware's tomato_rednote.c uses): following, recommendations,
 * notes and images, with the device token in a header.
 */
class Api(private val prefs: Prefs) {
    class ApiError(message: String) : IOException(message)

    private val client = OkHttpClient.Builder()
        .connectTimeout(15, TimeUnit.SECONDS)
        .readTimeout(15, TimeUnit.SECONDS)
        .build()

    // 点开推荐时服务器要现开小红书，最长要等半分钟以上。/ Opening a recommendation makes the server visit
    // Xiaohongshu live, which can take over half a minute.
    private val slowClient = client.newBuilder().readTimeout(60, TimeUnit.SECONDS).build()

    fun imageUrl(id: String, n: Int): String = "${prefs.url}/api/device/v1/img/$id/${if (n <= 0) "thumb" else n}.jpg"
    fun coverUrl(id: String): String = "${prefs.url}/api/device/v1/rec/cover/$id.jpg"

    private suspend fun get(path: String, slow: Boolean = false): JSONObject = withContext(Dispatchers.IO) {
        if (prefs.url.isEmpty()) throw ApiError("还没设置 rednote 服务器")
        val req = Request.Builder().url(prefs.url + path).header(TOKEN_HEADER, prefs.token).build()
        val resp = try {
            (if (slow) slowClient else client).newCall(req).execute()
        } catch (e: IOException) {
            throw ApiError("连不上 rednote 服务器（${e.message ?: e.javaClass.simpleName}）")
        } catch (e: IllegalArgumentException) {
            throw ApiError("服务器地址格式不对")
        }
        resp.use {
            val text = it.body?.string().orEmpty()
            if (!it.isSuccessful) {
                val detail = runCatching { JSONObject(text).optString("detail") }.getOrNull().orEmpty()
                throw ApiError(detail.ifEmpty { if (it.code == 401) "设备令牌不对" else "服务器返回 HTTP ${it.code}" })
            }
            try {
                JSONObject(text)
            } catch (e: Exception) {
                throw ApiError("服务器返回的内容看不懂")
            }
        }
    }

    suspend fun feed(limit: Int = 100) = get("/api/device/v1/feed?limit=$limit")
    suspend fun rec(refresh: Boolean) = get(if (refresh) "/api/device/v1/rec?refresh=1" else "/api/device/v1/rec", slow = true)
    suspend fun note(id: String, rec: Boolean) =
        get(if (rec) "/api/device/v1/rec/note/$id" else "/api/device/v1/note/$id", slow = rec)

    companion object {
        const val TOKEN_HEADER = "x-rednote-token"
    }
}

/** 服务器地址和设备令牌，存在本机。/ Server address and device token, kept on this phone. */
class Prefs(context: Context) {
    private val sp = context.getSharedPreferences("rednote", Context.MODE_PRIVATE)
    var url: String
        get() = sp.getString("url", "").orEmpty()
        set(v) = sp.edit().putString("url", v.trim().trimEnd('/')).apply()
    var token: String
        get() = sp.getString("token", "").orEmpty()
        set(v) = sp.edit().putString("token", v.trim()).apply()
    val configured get() = url.isNotEmpty()
}
