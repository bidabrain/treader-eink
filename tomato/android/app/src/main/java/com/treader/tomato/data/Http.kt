package com.treader.tomato.data

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import okhttp3.Request
import java.io.File
import java.util.concurrent.TimeUnit

/** 不带密码的公网请求：封面、书城榜单。/ Public requests without the password: covers and bookstore ranks. */
object Http {
    val client: OkHttpClient = OkHttpClient.Builder()
        .connectTimeout(15, TimeUnit.SECONDS)
        .readTimeout(30, TimeUnit.SECONDS)
        .build()

    /** 取一个文件（最多 maxBytes），成功返回 true；先写临时文件。/ Fetch a file up to maxBytes; true on success. */
    suspend fun fetch(url: String, dest: File, maxBytes: Long = 1024 * 1024, image: Boolean = true): Boolean =
        withContext(Dispatchers.IO) {
            runCatching {
                val b = Request.Builder().url(url)
                if (image) b.header("Accept", "image/jpeg,image/png;q=0.9")
                client.newCall(b.build()).execute().use { resp ->
                    if (!resp.isSuccessful) return@use false
                    val bytes = resp.body?.bytes() ?: return@use false
                    if (bytes.size > maxBytes || bytes.isEmpty()) return@use false
                    dest.parentFile?.mkdirs()
                    val tmp = File(dest.path + ".tmp")
                    tmp.writeBytes(bytes)
                    tmp.renameTo(dest)
                }
            }.getOrDefault(false)
        }

    /** 取文字（书城 JSON）。/ Fetch text (bookstore JSON). */
    suspend fun text(url: String): String = withContext(Dispatchers.IO) {
        client.newCall(Request.Builder().url(url).build()).execute().use { resp ->
            if (!resp.isSuccessful) throw TomatoApi.HttpError(resp.code, "书城返回 HTTP ${resp.code}")
            resp.body?.string().orEmpty()
        }
    }
}
