package com.treader.weibo.data

import android.content.Context
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import okhttp3.Request
import org.json.JSONObject
import java.io.IOException
import java.util.concurrent.TimeUnit

/**
 * weibo 服务器的设备接口（和固件 tomato_weibo.c 用的一样）：关注、热门、单条微博、评论、图片，请求头带设备令牌。
 * The weibo server's device API (the same one the firmware's tomato_weibo.c uses): following, hot, single posts,
 * comments and images, with the device token in a header.
 */
class Api(private val prefs: Prefs) {
    class ApiError(message: String) : IOException(message)

    private val client = OkHttpClient.Builder()
        .connectTimeout(15, TimeUnit.SECONDS)
        .readTimeout(15, TimeUnit.SECONDS)
        .build()

    // 评论第一次请求时服务器要去微博取。/ The first comments request makes the server ask Weibo.
    private val slowClient = client.newBuilder().readTimeout(45, TimeUnit.SECONDS).build()

    fun imageUrl(id: String, n: Int): String = "${prefs.url}/api/device/v1/img/$id/${if (n <= 0) "thumb" else n}.jpg"
    /** 第 k 张原图的方形小图（k 从 1 起，服务器 0.1.3 起才有）。/ Square small image of source image k (from 1). */
    fun squareUrl(id: String, k: Int): String = "${prefs.url}/api/device/v1/img/$id/s$k.jpg"

    private suspend fun get(path: String, slow: Boolean = false): JSONObject = withContext(Dispatchers.IO) {
        if (prefs.url.isEmpty()) throw ApiError("还没设置 weibo 服务器")
        val req = Request.Builder().url(prefs.url + path).header(TOKEN_HEADER, prefs.token).build()
        val resp = try {
            (if (slow) slowClient else client).newCall(req).execute()
        } catch (e: IOException) {
            throw ApiError("连不上 weibo 服务器（${e.message ?: e.javaClass.simpleName}）")
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

    suspend fun feed(hot: Boolean, limit: Int = 100) =
        get("/api/device/v1/feed?seg=${if (hot) "hot" else "follow"}&limit=$limit")
    suspend fun post(id: String) = get("/api/device/v1/post/$id")
    suspend fun comments(id: String) = get("/api/device/v1/post/$id/comments", slow = true)

    companion object {
        const val TOKEN_HEADER = "x-weibo-token"
    }
}

/** 服务器地址和设备令牌，存在本机。/ Server address and device token, kept on this phone. */
class Prefs(context: Context) {
    private val sp = context.getSharedPreferences("weibo", Context.MODE_PRIVATE)
    var url: String
        get() = sp.getString("url", "").orEmpty()
        set(v) = sp.edit().putString("url", v.trim().trimEnd('/')).apply()
    var token: String
        get() = sp.getString("token", "").orEmpty()
        set(v) = sp.edit().putString("token", v.trim()).apply()
    val configured get() = url.isNotEmpty()
}
