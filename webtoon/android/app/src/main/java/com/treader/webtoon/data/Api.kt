package com.treader.webtoon.data

import android.content.Context
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import org.json.JSONObject
import java.io.File
import java.io.IOException
import java.net.URLEncoder
import java.util.concurrent.TimeUnit

/**
 * webtoon 服务器的设备接口（和固件发现漫画用的一样，见 webtoon/README.md）：发现页、搜索、详情、封面、检查更新、
 * 每话的转换状态和 CBZ 下载，请求头带设备令牌。
 * The webtoon server's device API (the same one the firmware's comic store uses, see webtoon/README.md): store,
 * search, details, covers, update checks, each episode's conversion state and CBZ download, with the device token in
 * a header.
 */
class Api(private val prefs: Prefs) {
    open class ApiError(message: String) : IOException(message)

    /** 连不上服务器（断网、域名查不到、超时）：和服务器说“不行”的错误分开，检查更新遇到它就停下。
     *  Cannot reach the server (offline, name lookup failed, timeout): kept apart from errors the server returns, so
     *  an update check stops on it. */
    class NetworkError(message: String) : ApiError(message)

    private val client = OkHttpClient.Builder()
        .connectTimeout(15, TimeUnit.SECONDS)
        .readTimeout(60, TimeUnit.SECONDS)
        .build()

    private fun enc(s: String) = URLEncoder.encode(s, "UTF-8")

    private val zh get() = if (prefs.simplified) "s" else "t"

    fun coverUrl(id: String, lang: String = prefs.lang): String = "${prefs.url}/api/device/v1/cover/${enc(id)}.jpg?lang=${enc(lang)}"

    private fun open(req: Request): okhttp3.Response {
        if (prefs.url.isEmpty()) throw ApiError("还没设置 webtoon 服务器")
        return try {
            client.newCall(req).execute()
        } catch (e: IOException) {
            throw NetworkError("连不上 webtoon 服务器（${e.message ?: e.javaClass.simpleName}）")
        }
    }

    private fun request(path: String): Request.Builder = try {
        Request.Builder().url(prefs.url + path).header(TOKEN_HEADER, prefs.token)
    } catch (e: IllegalArgumentException) {
        throw ApiError("服务器地址格式不对")
    }

    private fun fail(code: Int, text: String): Nothing {
        val detail = runCatching { JSONObject(text).optString("detail") }.getOrNull().orEmpty()
        throw ApiError(detail.ifEmpty { if (code == 401) "设备令牌不对" else "服务器返回 HTTP $code" })
    }

    private suspend fun json(req: Request): JSONObject = withContext(Dispatchers.IO) {
        open(req).use {
            val text = it.body?.string().orEmpty()
            if (!it.isSuccessful) fail(it.code, text)
            try { JSONObject(text) } catch (e: Exception) { throw ApiError("服务器返回的内容看不懂") }
        }
    }

    private suspend fun get(path: String) = json(request(path).build())

    // WEBTOON 不带 src（旧服务器也认）；插件来源带 &src=。/ No src for WEBTOON (old servers accept that too); &src=
    // for an extension source.
    private fun srcQuery(src: String) = if (src.isEmpty() || src == Source.WEBTOON) "" else "&src=${enc(src)}"

    suspend fun store(seg: String, key: String, lang: String = prefs.lang, src: String = Source.WEBTOON): StoreList =
        Parse.store(get("/api/device/v1/store?seg=${enc(seg)}&key=${enc(key)}&limit=120&lang=${enc(lang)}&zh=$zh${srcQuery(src)}"))

    suspend fun search(q: String, lang: String = prefs.lang, src: String = Source.WEBTOON): List<Card> =
        Parse.cards(get("/api/device/v1/search?q=${enc(q)}&lang=${enc(lang)}&zh=$zh${srcQuery(src)}").optJSONArray("items"))

    /** 服务器的来源；旧服务器没有这个接口时当作只有 WEBTOON。/ The server's sources; an older server without the
     *  endpoint counts as WEBTOON only. */
    suspend fun sources(): List<Source> = try {
        Parse.sources(get("/api/device/v1/sources"))
    } catch (e: NetworkError) {
        throw e
    } catch (e: ApiError) {
        emptyList()
    }

    suspend fun title(id: String, lang: String = prefs.lang): Title =
        Parse.title(get("/api/device/v1/title/${enc(id)}?lang=${enc(lang)}&zh=$zh"))

    /** 一次问一种语言的全部订阅。/ Ask about every subscription of one language at once. */
    suspend fun check(lang: String, titles: List<Pair<String, Int>>): List<CheckResult> {
        val body = Parse.checkBody(lang, zh, titles).toRequestBody("application/json".toMediaType())
        return Parse.check(json(request("/api/device/v1/check").post(body).build()))
    }

    /** 一话的状态；没转过服务器就开始转。/ An episode's state; the server starts converting when needed. */
    suspend fun episode(id: String, no: Int, lang: String): JSONObject =
        get("/api/device/v1/episode/${enc(id)}/$no?lang=${enc(lang)}")

    /** 下载转好的 CBZ：先写 .part，完整了再改名。/ Download the converted CBZ: write .part, rename once complete. */
    suspend fun download(id: String, no: Int, lang: String, dest: File) = withContext(Dispatchers.IO) {
        val part = File(dest.path + ".part")
        try {
            open(request("/api/device/v1/episode/${enc(id)}/$no.cbz?lang=${enc(lang)}").build()).use { resp ->
                if (!resp.isSuccessful) fail(resp.code, resp.body?.string().orEmpty())
                val body = resp.body ?: throw ApiError("服务器没有返回文件")
                dest.parentFile?.mkdirs()
                try {
                    part.outputStream().use { out -> body.byteStream().copyTo(out) }
                } catch (e: ApiError) {
                    throw e
                } catch (e: IOException) {
                    // 下载到一半断网。/ The connection dropped mid-download.
                    throw NetworkError("下载中断（${e.message ?: e.javaClass.simpleName}）")
                }
                val want = body.contentLength()
                if (want >= 0 && part.length() != want) throw ApiError("下载不完整")
            }
            if (dest.exists()) dest.delete()
            if (!part.renameTo(dest)) throw ApiError("存不下文件")
        } finally {
            if (part.exists()) part.delete()
        }
    }

    companion object {
        const val TOKEN_HEADER = "X-Webtoon-Token"
    }
}

/** 服务器地址、设备令牌和发现页选项，存在本机。/ Server address, device token and store options, kept on this phone. */
class Prefs(context: Context) {
    private val sp = context.getSharedPreferences("webtoon", Context.MODE_PRIVATE)
    var url: String
        get() = sp.getString("url", "").orEmpty()
        set(v) = sp.edit().putString("url", v.trim().trimEnd('/')).apply()
    var token: String
        get() = sp.getString("token", "").orEmpty()
        set(v) = sp.edit().putString("token", v.trim()).apply()
    val configured get() = url.isNotEmpty()

    /** 语言版本：zh-hant（默认）或 en。/ Language edition: zh-hant (default) or en. */
    var lang: String
        get() = sp.getString("lang", LANGS[0].first).orEmpty().let { l -> if (LANGS.any { it.first == l }) l else LANGS[0].first }
        set(v) = sp.edit().putString("lang", v).apply()

    /** 简体显示（中文版的作品名、简介转成简体）。/ Show the Chinese edition's text in simplified characters. */
    var simplified: Boolean
        get() = sp.getBoolean("simplified", true)
        set(v) = sp.edit().putBoolean("simplified", v).apply()

    /** 发现页选中的来源 key，默认 WEBTOON。/ The store's chosen source key, WEBTOON by default. */
    var src: String
        get() = sp.getString("src", Source.WEBTOON).orEmpty().ifEmpty { Source.WEBTOON }
        set(v) = sp.edit().putString("src", v).apply()

    /** 每次检查最多下载几话，0 表示不限。/ Episodes downloaded per check at most; 0 = no limit. */
    var perCheck: Int
        get() = sp.getInt("per_check", 10).let { if (it in PER_CHECK) it else 10 }
        set(v) = sp.edit().putInt("per_check", v).apply()

    companion object {
        val LANGS = listOf("zh-hant" to "繁体中文", "en" to "English")
        val PER_CHECK = listOf(5, 10, 20, 0)
    }
}
