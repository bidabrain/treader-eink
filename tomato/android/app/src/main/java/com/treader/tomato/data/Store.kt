package com.treader.tomato.data

import org.json.JSONObject
import java.io.File

/** 书城的一个分类（固件 tomato_rank_cat_t）。/ One bookstore category (the firmware's tomato_rank_cat_t). */
data class RankCat(val name: String, val url: String, val male: Boolean)

/** 榜单上的一本（固件 tomato_rank_book_t）。/ One rank-list book (the firmware's tomato_rank_book_t). */
data class RankBook(val bookId: String, val title: String, val author: String, val reads: String, val intro: String, val cover: String)

/**
 * 书城，照固件 components/tomato/tomato_rank.c：FanqieRankTracker 站点的 api/lastest/index.json 列分类，每个分类一个文件；
 * 缓存在 tomato/store/（index.json、c_<哈希>.json），榜单换了日期就删旧分类缓存。github.io 连不上时依次试 jsDelivr 的镜像，
 * 用通了的一直用。只在点“刷新”、打开没缓存的分类时联网。
 * The bookstore after the firmware's tomato_rank.c: a FanqieRankTracker site's api/lastest/index.json lists categories,
 * one file each; cached in tomato/store/ (index.json, c_<hash>.json), and old category caches go when the rank date
 * changes. When github.io fails, jsDelivr mirrors are tried in turn and the working one is kept. Network use happens
 * only on Refresh or opening an uncached category.
 */
class Store(private val prefs: Prefs, private val dirs: Dirs) {
    private var mirror = 0
    private val indexFile get() = File(dirs.store, "index.json")

    private fun base(): String {
        var b = prefs.storeBase
        b.indexOf("/api/").takeIf { it >= 0 }?.let { b = b.substring(0, it) }
        return b.trimEnd('/')
    }

    // https://OWNER.github.io/REPO → (owner, repo)。/ Split a github.io site into owner and repo.
    private fun githubRepo(b: String): Pair<String, String>? {
        val m = Regex("^https?://([^/.]+)\\.github\\.io/([^/]+)$").find(b) ?: return null
        return m.groupValues[1] to m.groupValues[2]
    }

    private fun bases(): List<String> {
        val b = base()
        val gh = githubRepo(b) ?: return listOf(b)
        return listOf(b) + MIRRORS.map { "$it/${gh.first}/${gh.second}@main" }
    }

    // 从当前用通的入口开始依次试。/ Try each entry point, starting from the one that last worked.
    private suspend fun fetch(path: (String) -> String): String {
        val list = bases()
        var last: Exception? = null
        for (k in list.indices) {
            val i = (mirror + k) % list.size
            try {
                val text = Http.text(path(list[i]))
                mirror = i
                return text
            } catch (e: Exception) { last = e }
        }
        throw TomatoApi.HttpError(0, "连不上书城（${last?.message ?: "网络错误"}）")
    }

    fun cachedIndex(): Pair<String, List<RankCat>>? = runCatching { parseIndex(JSONObject(indexFile.readText())) }.getOrNull()

    private fun parseIndex(root: JSONObject): Pair<String, List<RankCat>> {
        val arr = root.optJSONArray("types")
        val cats = buildList {
            for (i in 0 until (arr?.length() ?: 0)) {
                val o = arr!!.optJSONObject(i) ?: continue
                val name = o.optString("type")
                val channel = o.optString("channel")
                val url = o.optString("url")
                // “all” 是 1 MB 的全量文件，不用。/ "all" is the 1 MB full file, skipped.
                if (name.isEmpty() || name == "all" || channel.isEmpty() || url.isEmpty()) continue
                add(RankCat(name, url, channel == "male"))
            }
        }
        return root.optString("date") to cats
    }

    /** 重新取分类表；日期变了就清掉旧的分类缓存。/ Refetch the index; drop old category caches on a new date. */
    suspend fun refreshIndex(): Pair<String, List<RankCat>> {
        val text = fetch { "$it/api/lastest/index.json" }
        val parsed = parseIndex(JSONObject(text))
        val old = cachedIndex()?.first
        if (old != null && old != parsed.first) dirs.store.listFiles()?.filter { it.name.startsWith("c_") }?.forEach { it.delete() }
        indexFile.writeText(text)
        return parsed
    }

    private fun catFile(c: RankCat): File {
        var h = 2166136261L
        for (b in c.url.toByteArray(Charsets.UTF_8)) { h = h xor (b.toLong() and 0xFF); h = (h * 16777619L) and 0xFFFFFFFFL }
        return File(dirs.store, "c_%08x.json".format(h))
    }

    fun cachedBooks(c: RankCat): List<RankBook>? = runCatching { parseBooks(JSONObject(catFile(c).readText())) }.getOrNull()

    suspend fun refreshBooks(c: RankCat): List<RankBook> {
        val text = if (c.url.startsWith("http://") || c.url.startsWith("https://")) Http.text(c.url)
            else fetch { "$it/${c.url.trimStart('/')}" }
        val books = parseBooks(JSONObject(text))
        catFile(c).writeText(text)
        return books
    }

    private fun parseBooks(root: JSONObject): List<RankBook> {
        // 书可能在 books，也可能在 categories[0].books。/ Books may sit in books or categories[0].books.
        val arr = root.optJSONArray("books") ?: root.optJSONArray("categories")?.optJSONObject(0)?.optJSONArray("books")
        return buildList {
            for (i in 0 until (arr?.length() ?: 0)) {
                if (size >= BOOKS_MAX) break
                val o = arr!!.optJSONObject(i) ?: continue
                val id = Regex("/page/(\\d+)").find(o.optString("url"))?.groupValues?.get(1)?.take(23).orEmpty()
                val title = utf8Cut(o.optString("title"), Caps.TITLE)
                if (id.isEmpty() || title.isEmpty()) continue
                add(RankBook(id, title, utf8Cut(o.optString("author"), Caps.AUTHOR), o.optString("reads"),
                    utf8Cut(o.optString("intro"), 900), o.optString("cover").takeIf { it.length < 400 }.orEmpty()))
            }
        }
    }

    companion object {
        const val BOOKS_MAX = 20
        val MIRRORS = listOf("https://fastly.jsdelivr.net/gh", "https://gcore.jsdelivr.net/gh", "https://cdn.jsdelivr.net/gh")
    }
}
