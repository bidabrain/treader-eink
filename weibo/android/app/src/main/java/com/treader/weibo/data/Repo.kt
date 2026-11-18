package com.treader.weibo.data

import android.content.Context
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/** 列表里的一条。/ One post in a list. */
data class Item(
    val id: String,
    val user: String,
    val text: String,
    val rtUser: String,
    val rtText: String,
    /** 发布时间，毫秒或秒。/ Posting time in milliseconds or seconds. */
    val time: Double,
    val images: Int,
    val likes: Int,
    val comments: Int,
    val reposts: Int,
    val seen: Boolean,
    val source: String = "",
    /** 有方形小图的原图张数（旧服务器为 0，只有封面）。/ Source images with squares (0 from older servers). */
    val pics: Int = 0,
    /** 封面高宽比。/ Cover height/width. */
    val ratio: Float = 1f,
)

data class Comment(val user: String, val text: String, val likes: Int)

data class Post(
    val id: String,
    val user: String,
    val text: String,
    val rtUser: String,
    val rtText: String,
    val source: String,
    val time: Double,
    val images: Int,
    val likes: Int,
    val comments: Int,
    val reposts: Int,
    val pics: Int = 0,
    val ratio: Float = 1f,
    /** 每张原图的第一页（从 0 起）。/ Each source image's first page (0-based). */
    val starts: List<Int> = emptyList(),
)

data class Feed(val items: List<Item>, val now: Double)

/**
 * 缓存在应用目录里（和固件 TF 卡 weibo/ 的结构一样）：关注列表带“看过”标记、热门列表、看过的微博全文和评论。
 * 图片交给 Coil 的磁盘缓存。不联网时照样能看缓存。
 * Caches in the app folder (shaped like the firmware's weibo/ on the TF card): the following list with "seen" marks,
 * the hot list, and opened posts' full text and comments. Images go to Coil's disk cache. Caches work offline.
 */
class Repo(context: Context, private val api: Api) {
    private val dir = File(context.filesDir, "weibo").apply { mkdirs() }
    private val postsDir = File(dir, "p").apply { mkdirs() }
    private val followFile = File(dir, "follow.json")
    private val hotFile = File(dir, "hot.json")

    private fun read(f: File): JSONObject? = runCatching { JSONObject(f.readText()) }.getOrNull()

    private fun write(f: File, j: JSONObject) {
        val tmp = File(f.path + ".tmp")
        tmp.writeText(j.toString())
        tmp.renameTo(f)
    }

    private fun file(hot: Boolean) = if (hot) hotFile else followFile

    private fun items(arr: JSONArray?, hot: Boolean): List<Item> = buildList {
        if (arr == null) return@buildList
        for (i in 0 until arr.length()) {
            val o = arr.optJSONObject(i) ?: continue
            val id = o.optString("id")
            if (id.isEmpty()) continue
            add(Item(id, o.optString("user"), o.optString("text"), o.optString("rt_user"), o.optString("rt_text"),
                o.optDouble("time", 0.0), o.optInt("images"), o.optInt("likes"), o.optInt("comments"),
                o.optInt("reposts"), hot || o.optBoolean("seen"), o.optString("source"), o.optInt("pics"),
                o.optDouble("ratio", 1.0).toFloat()))
        }
    }

    fun cached(hot: Boolean): Feed? = read(file(hot))?.let { Feed(items(it.optJSONArray("items"), hot), it.optDouble("now", 0.0)) }

    /** 从服务器取列表；关注列表保留“看过”，第一次取到的都算看过。/ Fetch a list; the following list keeps "seen"
     *  marks and a first fetch marks everything seen. */
    suspend fun refresh(hot: Boolean): Feed {
        val j = api.feed(hot)
        return withContext(Dispatchers.IO) {
            val old = if (hot) null else read(followFile)
            val seen = if (hot) emptySet() else cached(false)?.items.orEmpty().filter { it.seen }.map { it.id }.toSet()
            val posts = j.optJSONArray("posts") ?: JSONArray()
            val out = JSONArray()
            for (i in 0 until posts.length()) {
                val p = posts.optJSONObject(i) ?: continue
                val id = p.optString("id")
                if (id.isEmpty()) continue
                out.put(JSONObject().put("id", id).put("user", p.optString("user")).put("text", p.optString("text"))
                    .put("rt_user", p.optString("rt_user")).put("rt_text", p.optString("rt_text"))
                    .put("time", p.optDouble("time", 0.0)).put("images", p.optInt("images"))
                    .put("likes", p.optInt("likes")).put("comments", p.optInt("comments"))
                    .put("reposts", p.optInt("reposts")).put("seen", hot || old == null || id in seen)
                    .put("source", p.optString("source")).put("pics", p.optInt("pics"))
                    .put("ratio", p.optDouble("ratio", 1.0)))
            }
            write(file(hot), JSONObject().put("items", out).put("now", j.optDouble("now", 0.0)))
            cached(hot)!!
        }
    }

    private fun postFile(id: String) = File(postsDir, "$id.json")
    private fun commentsFile(id: String) = File(postsDir, "$id.c.json")

    fun cachedPost(id: String): Post? = read(postFile(id))?.let { p ->
        Post(p.optString("id"), p.optString("user"), p.optString("text"), p.optString("rt_user"),
            p.optString("rt_text"), p.optString("source"), p.optDouble("time", 0.0), p.optInt("images"),
            p.optInt("likes"), p.optInt("comments"), p.optInt("reposts"), p.optInt("pics"),
            p.optDouble("ratio", 1.0).toFloat(),
            p.optJSONArray("starts")?.let { a -> List(a.length()) { a.optInt(it) } } ?: emptyList())
    }

    suspend fun fetchPost(id: String): Post {
        val j = api.post(id)
        return withContext(Dispatchers.IO) {
            write(postFile(id), j)
            prune()
            cachedPost(id)!!
        }
    }

    fun cachedComments(id: String): List<Comment>? = read(commentsFile(id))?.let { j ->
        val arr = j.optJSONArray("comments") ?: return@let emptyList()
        buildList {
            for (i in 0 until arr.length()) {
                val c = arr.optJSONObject(i) ?: continue
                add(Comment(c.optString("user"), c.optString("text"), c.optInt("likes")))
            }
        }
    }

    /** 评论：服务器第一次会去微博取一次并存下来。/ Comments: the server fetches them from Weibo once and keeps them. */
    suspend fun fetchComments(id: String): List<Comment> {
        val j = api.comments(id)
        return withContext(Dispatchers.IO) {
            write(commentsFile(id), j)
            cachedComments(id)!!
        }
    }

    suspend fun markSeen(id: String) = withContext(Dispatchers.IO) {
        val j = read(followFile) ?: return@withContext
        val arr = j.optJSONArray("items") ?: return@withContext
        for (i in 0 until arr.length()) {
            val o = arr.optJSONObject(i) ?: continue
            if (o.optString("id") == id) { o.put("seen", true); break }
        }
        write(followFile, j)
    }

    // 全文和评论文件最多留 400 个，删最旧的。/ Keep at most 400 post and comment files, dropping the oldest.
    private fun prune() {
        val files = postsDir.listFiles()?.sortedBy { it.lastModified() } ?: return
        files.take((files.size - 400).coerceAtLeast(0)).forEach { it.delete() }
    }
}
