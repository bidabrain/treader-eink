package com.treader.x.data

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
    /** 引用的推文作者和开头。/ Author and opening of the quoted post. */
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
    /** @用户名（不带 @）。/ The @handle, without the @. */
    val screen: String = "",
    val views: Int = 0,
    /** 谁转推了这条；空表示不是转推。/ Who reposted it; empty when it is not a repost. */
    val rtBy: String = "",
    /** 回复给谁（@用户名，不带 @）。/ Whom it replies to (handle without the @). */
    val replyTo: String = "",
    /** 是视频；图片是视频封面。/ A video; the images are its cover. */
    val video: Boolean = false,
)

data class Comment(val user: String, val text: String, val likes: Int, val screen: String = "")

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
    val screen: String = "",
    val views: Int = 0,
    val rtBy: String = "",
    val replyTo: String = "",
    val video: Boolean = false,
    /** 引用推文作者的 @用户名。/ The quoted author's handle. */
    val rtScreen: String = "",
)

data class Feed(val items: List<Item>, val now: Double)

/**
 * 刷新结果：feed 为 null 表示这条流的总开关关着（旧缓存不动）；followingOn 是服务器报的“正在关注”总开关，
 * 没报就是 null。
 * A refresh result: a null feed means the feed's master switch is off (the old cache stays); followingOn is the
 * server's Following master switch, null when not reported.
 */
data class Refreshed(val feed: Feed?, val followingOn: Boolean?)

/**
 * 缓存在应用目录里：为你推荐列表、正在关注列表（带“看过”标记）、看过的推文全文和回复，以及上次知道的“正在关注”
 * 总开关。图片交给 Coil 的磁盘缓存。不联网时照样能看缓存。
 * Caches in the app folder: the For You list, the Following list with "seen" marks, opened posts' full text and
 * replies, and the last known Following master switch. Images go to Coil's disk cache. Caches work offline.
 */
class Repo(context: Context, private val api: Api) {
    private val dir = File(context.filesDir, "x").apply { mkdirs() }
    private val postsDir = File(dir, "p").apply { mkdirs() }
    private val followingFile = File(dir, "following.json")
    private val foryouFile = File(dir, "foryou.json")
    private val stateFile = File(dir, "state.json")

    private fun read(f: File): JSONObject? = runCatching { JSONObject(f.readText()) }.getOrNull()

    private fun write(f: File, j: JSONObject) {
        val tmp = File(f.path + ".tmp")
        tmp.writeText(j.toString())
        tmp.renameTo(f)
    }

    private fun file(following: Boolean) = if (following) followingFile else foryouFile

    private fun items(arr: JSONArray?, following: Boolean): List<Item> = buildList {
        if (arr == null) return@buildList
        for (i in 0 until arr.length()) {
            val o = arr.optJSONObject(i) ?: continue
            val id = o.optString("id")
            if (id.isEmpty()) continue
            add(Item(id, o.optString("user"), o.optString("text"), o.optString("rt_user"), o.optString("rt_text"),
                o.optDouble("time", 0.0), o.optInt("images"), o.optInt("likes"), o.optInt("comments"),
                o.optInt("reposts"), !following || o.optBoolean("seen"), o.optString("source"), o.optInt("pics"),
                o.optDouble("ratio", 1.0).toFloat(), o.optString("screen"), o.optInt("views"), o.optString("rt_by"),
                o.optString("reply_to"), o.optBoolean("video")))
        }
    }

    fun cached(following: Boolean): Feed? =
        read(file(following))?.let { Feed(items(it.optJSONArray("items"), following), it.optDouble("now", 0.0)) }

    /** 上次知道的“正在关注”总开关；没探测过当作开着。/ The last known Following master switch; on until probed. */
    fun followingOn(): Boolean = read(stateFile)?.optBoolean("following_on", true) ?: true

    private fun saveFollowingOn(on: Boolean) = write(stateFile, JSONObject().put("following_on", on))

    /** 探测服务器状态，记下“正在关注”总开关并返回它。/ Probe the server status, remember the Following switch and
     *  return it. */
    suspend fun probeStatus(): Boolean? {
        val j = api.status()
        return withContext(Dispatchers.IO) {
            val f = j.optJSONObject("feeds")?.optJSONObject("following") ?: return@withContext null
            if (!f.has("on")) return@withContext null
            f.optBoolean("on", true).also { saveFollowingOn(it) }
        }
    }

    /** 从服务器取列表；正在关注列表保留“看过”，第一次取到的都算看过。总开关关着时不动旧缓存。
     *  Fetch a list; the Following list keeps "seen" marks and a first fetch marks everything seen. With the master
     *  switch off the old cache stays as is. */
    suspend fun refresh(following: Boolean): Refreshed {
        val j = api.feed(following)
        return withContext(Dispatchers.IO) {
            val fo = j.optJSONObject("feeds_on")?.takeIf { it.has("following") }?.optBoolean("following", true)
            fo?.let { saveFollowingOn(it) }
            if (!j.optBoolean("on", true)) return@withContext Refreshed(null, if (following) false else fo)
            val old = if (following) read(followingFile) else null
            val seen = if (following) cached(true)?.items.orEmpty().filter { it.seen }.map { it.id }.toSet() else emptySet()
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
                    .put("reposts", p.optInt("reposts")).put("seen", !following || old == null || id in seen)
                    .put("source", p.optString("source")).put("pics", p.optInt("pics"))
                    .put("ratio", p.optDouble("ratio", 1.0)).put("screen", p.optString("screen"))
                    .put("views", p.optInt("views")).put("rt_by", p.optString("rt_by"))
                    .put("reply_to", p.optString("reply_to")).put("video", p.optBoolean("video")))
            }
            write(file(following), JSONObject().put("items", out).put("now", j.optDouble("now", 0.0)))
            Refreshed(cached(following)!!, fo)
        }
    }

    private fun postFile(id: String) = File(postsDir, "$id.json")
    private fun commentsFile(id: String) = File(postsDir, "$id.c.json")

    fun cachedPost(id: String): Post? = read(postFile(id))?.let { p ->
        Post(p.optString("id"), p.optString("user"), p.optString("text"), p.optString("rt_user"),
            p.optString("rt_text"), p.optString("source"), p.optDouble("time", 0.0), p.optInt("images"),
            p.optInt("likes"), p.optInt("comments"), p.optInt("reposts"), p.optInt("pics"),
            p.optDouble("ratio", 1.0).toFloat(),
            p.optJSONArray("starts")?.let { a -> List(a.length()) { a.optInt(it) } } ?: emptyList(),
            p.optString("screen"), p.optInt("views"), p.optString("rt_by"), p.optString("reply_to"),
            p.optBoolean("video"), p.optString("rt_screen"))
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
                add(Comment(c.optString("user"), c.optString("text"), c.optInt("likes"), c.optString("screen")))
            }
        }
    }

    /** 回复：服务器第一次会开浏览器去 X 取一次并存下来。/ Replies: the server fetches them from X once and keeps
     *  them. */
    suspend fun fetchComments(id: String): List<Comment> {
        val j = api.comments(id)
        return withContext(Dispatchers.IO) {
            write(commentsFile(id), j)
            cachedComments(id)!!
        }
    }

    suspend fun markSeen(id: String) = withContext(Dispatchers.IO) {
        val j = read(followingFile) ?: return@withContext
        val arr = j.optJSONArray("items") ?: return@withContext
        for (i in 0 until arr.length()) {
            val o = arr.optJSONObject(i) ?: continue
            if (o.optString("id") == id) { o.put("seen", true); break }
        }
        write(followingFile, j)
    }

    // 全文和回复文件最多留 400 个，删最旧的。/ Keep at most 400 post and reply files, dropping the oldest.
    private fun prune() {
        val files = postsDir.listFiles()?.sortedBy { it.lastModified() } ?: return
        files.take((files.size - 400).coerceAtLeast(0)).forEach { it.delete() }
    }
}
