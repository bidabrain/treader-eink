package com.treader.tomato.data

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.withContext
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.util.concurrent.TimeUnit
import kotlin.coroutines.coroutineContext

/**
 * 番茄下载服务器（Tomato-Novel-Downloader 的 Web UI 模式，或 tomato/ 一键部署）的接口，和固件 tomato_http.c 一样：
 * 每个请求带 x-tomato-password；非 2xx 一律报错并带回状态码。
 * The Tomato downloader server API (Tomato-Novel-Downloader in Web UI mode, or the tomato/ deployment), as in the
 * firmware's tomato_http.c: every request carries x-tomato-password, and any non-2xx is an error with its status.
 */
class TomatoApi(private val prefs: Prefs) {
    class HttpError(val status: Int, message: String) : IOException(message)

    private val client = OkHttpClient.Builder()
        .connectTimeout(15, TimeUnit.SECONDS)
        .readTimeout(30, TimeUnit.SECONDS)
        .build()

    private fun req(path: String): Request.Builder {
        if (prefs.server.isEmpty()) throw HttpError(0, "还没设置番茄服务器")
        val b = try {
            Request.Builder().url(prefs.server + path)
        } catch (e: IllegalArgumentException) {
            throw HttpError(0, "服务器地址格式不对")
        }
        if (prefs.password.isNotEmpty()) b.header("x-tomato-password", prefs.password)
        return b
    }

    private suspend fun call(r: Request): JSONObject = withContext(Dispatchers.IO) {
        val resp = try {
            client.newCall(r).execute()
        } catch (e: IOException) {
            throw HttpError(0, "连不上番茄服务器（${e.message ?: e.javaClass.simpleName}）")
        }
        resp.use {
            val text = it.body?.string().orEmpty()
            if (!it.isSuccessful) throw HttpError(it.code, errorText(it.code))
            try {
                JSONObject(text)
            } catch (e: Exception) {
                throw HttpError(it.code, "服务器返回的内容看不懂")
            }
        }
    }

    suspend fun get(path: String): JSONObject = call(req(path).build())

    suspend fun post(path: String, body: String): JSONObject =
        call(req(path).post(body.toRequestBody("application/json".toMediaType())).build())

    /** 流式下载到临时文件，完成后替换 dest；onBytes 报告已收字节。/ Stream into a temporary file, then replace dest;
     *  onBytes reports bytes received. */
    suspend fun download(path: String, dest: File, onBytes: (Long) -> Unit) = withContext(Dispatchers.IO) {
        val r = req(path).build()
        val tmp = File(dest.path + ".part")
        try {
            client.newBuilder().readTimeout(60, TimeUnit.SECONDS).build().newCall(r).execute().use { resp ->
                if (!resp.isSuccessful) throw HttpError(resp.code, errorText(resp.code))
                val body = resp.body ?: throw HttpError(resp.code, "服务器没有返回文件")
                body.byteStream().use { input ->
                    tmp.outputStream().use { out ->
                        val buf = ByteArray(16 * 1024)
                        var total = 0L
                        while (true) {
                            coroutineContext.ensureActive()
                            val n = input.read(buf)
                            if (n < 0) break
                            out.write(buf, 0, n)
                            total += n
                            onBytes(total)
                        }
                    }
                }
            }
            if (!tmp.renameTo(dest)) { dest.delete(); if (!tmp.renameTo(dest)) throw IOException("写不进文件") }
        } catch (e: IOException) {
            tmp.delete()
            if (e is HttpError) throw e
            throw HttpError(0, "下载中断（${e.message ?: e.javaClass.simpleName}）")
        } finally {
            tmp.delete()
        }
    }

    /** 数番茄目录接口里的章节（"itemId" 和 "item_id" 取多的那个），检查更新时服务器扫描里没有的书用。
     *  Count chapters in the Tomato directory API ("itemId" or "item_id", whichever is more), for books missing from the
     *  server's update scan. */
    suspend fun fanqieChapters(bookId: String): Int = withContext(Dispatchers.IO) {
        val r = Request.Builder().url("https://fanqienovel.com/api/reader/directory/detail?bookId=$bookId").build()
        runCatching {
            client.newCall(r).execute().use { resp ->
                val text = resp.body?.string().orEmpty()
                maxOf(text.windowed("\"itemId\"").count(), text.windowed("\"item_id\"").count())
            }
        }.getOrDefault(0)
    }

    companion object {
        fun errorText(status: Int) = when (status) {
            401, 403 -> "服务器密码不对"
            429 -> "服务器忙，稍后再试"
            else -> "服务器返回 HTTP $status"
        }

        private fun String.windowed(key: String): Sequence<Int> =
            generateSequence(indexOf(key).takeIf { it >= 0 }) { i -> indexOf(key, i + key.length).takeIf { it >= 0 } }
    }
}
