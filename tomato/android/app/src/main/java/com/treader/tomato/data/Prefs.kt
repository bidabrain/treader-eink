package com.treader.tomato.data

import android.content.Context
import java.io.File

/**
 * 设置，存在本机：番茄服务器地址和密码、书城地址、WebDAV、最近搜索。和固件 tomato/config.json 的字段一一对应，
 * 但 App 里没有内置服务器，地址密码都要自己填。
 * Settings kept on this phone: Tomato server address and password, bookstore URL, WebDAV and recent searches. They
 * mirror the firmware's tomato/config.json fields, but the app has no built-in server: you fill everything in.
 */
class Prefs(context: Context) {
    private val sp = context.getSharedPreferences("tomato", Context.MODE_PRIVATE)

    var server: String
        get() = sp.getString("server", "").orEmpty()
        set(v) = sp.edit().putString("server", v.trim().trimEnd('/')).apply()
    var password: String
        get() = sp.getString("password", "").orEmpty()
        set(v) = sp.edit().putString("password", v.trim()).apply()
    val configured get() = server.isNotEmpty()

    /** 书城榜单地址；空就用默认（和 CI 固件一样的 FanqieRankTracker）。/ Bookstore URL; empty uses the default. */
    var storeUrl: String
        get() = sp.getString("store_url", "").orEmpty()
        set(v) = sp.edit().putString("store_url", v.trim().trimEnd('/')).apply()
    val storeBase get() = storeUrl.ifEmpty { DEFAULT_STORE }

    var davUrl: String
        get() = sp.getString("webdav_url", "").orEmpty()
        set(v) = sp.edit().putString("webdav_url", v.trim().trimEnd('/')).apply()
    var davUser: String
        get() = sp.getString("webdav_user", "").orEmpty()
        set(v) = sp.edit().putString("webdav_user", v.trim()).apply()
    var davPassword: String
        get() = sp.getString("webdav_password", "").orEmpty()
        set(v) = sp.edit().putString("webdav_password", v).apply()

    /** 最近搜索，最多 6 个，用过的移到最前（和固件一样）。/ Up to six recent queries, reused ones move to the front. */
    var recent: List<String>
        get() = sp.getString("recent", "").orEmpty().split('\n').filter { it.isNotBlank() }
        set(v) = sp.edit().putString("recent", v.take(6).joinToString("\n")).apply()

    fun remember(q: String) {
        recent = listOf(q) + recent.filter { it != q }
    }

    companion object {
        const val DEFAULT_STORE = "https://bidabrain.github.io/FanqieRankTracker"
    }
}

/**
 * App 自己目录里的文件，结构照固件 TF 卡：books/ 放书（番茄 TXT 也在这），tomato/ 放 shelf.json、removed.json、
 * covers/ 和书城缓存 store/。
 * Files in the app's own folder, shaped like the firmware's TF card: books/ holds the books (Tomato TXT too), and
 * tomato/ holds shelf.json, removed.json, covers/ and the bookstore cache store/.
 */
class Dirs(root: File) {
    constructor(context: Context) : this(context.filesDir)
    val books = File(root, "books").apply { mkdirs() }
    val tomato = File(root, "tomato").apply { mkdirs() }
    val covers = File(tomato, "covers").apply { mkdirs() }
    val store = File(tomato, "store").apply { mkdirs() }
    val shelf = File(tomato, "shelf.json")
    val removed = File(tomato, "removed.json")
}
