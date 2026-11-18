package com.treader.rednote.data

import android.content.Context
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/** 列表里的一篇。/ One note in a list. */
data class Item(
    val id: String,
    val title: String,
    val author: String,
    val likes: String,
    val images: Int,
    val seen: Boolean,
    val thumb: Boolean = true,
)

data class Comment(val user: String, val text: String, val likes: String)

data class Note(
    val id: String,
    val title: String,
    val author: String,
    val likes: String,
    val comments: String,
    val body: String,
    val time: Double,
    val images: Int,
    val list: List<Comment>,
)

data class RecList(val items: List<Item>, val fetchedAt: Double, val left: Int)

/**
 * 缓存在应用目录里（和固件 TF 卡 rednote/ 的结构一样）：关注列表带“看过”标记，推荐列表，看过的笔记正文。
 * 图片交给 Coil 的磁盘缓存。不联网时照样能看缓存。
 * Caches in the app folder (shaped like the firmware's rednote/ on the TF card): the following list with "seen"
 * marks, the recommendation list and opened notes' text. Images go to Coil's disk cache. Caches work offline.
 */
class Repo(context: Context, private val api: Api) {
    private val dir = File(context.filesDir, "rednote").apply { mkdirs() }
    private val notesDir = File(dir, "n").apply { mkdirs() }
    private val feedFile = File(dir, "feed.json")
    private val recFile = File(dir, "rec.json")

    private fun read(f: File): JSONObject? = runCatching { JSONObject(f.readText()) }.getOrNull()

    private fun write(f: File, j: JSONObject) {
        val tmp = File(f.path + ".tmp")
        tmp.writeText(j.toString())
        tmp.renameTo(f)
    }

    private fun str(o: JSONObject, k: String): String = when (val v = o.opt(k)) {
        null, JSONObject.NULL -> ""
        is Number -> if (v.toDouble() == Math.floor(v.toDouble())) v.toLong().toString() else v.toString()
        else -> v.toString()
    }

    private fun items(arr: JSONArray?, rec: Boolean): List<Item> = buildList {
        if (arr == null) return@buildList
        for (i in 0 until arr.length()) {
            val o = arr.optJSONObject(i) ?: continue
            val id = str(o, "id")
            if (id.isEmpty()) continue
            add(Item(id, str(o, "title"), str(o, "author"), str(o, "likes"), o.optInt("images"),
                rec || o.optBoolean("seen"), if (rec) o.optBoolean("thumb") else true))
        }
    }

    fun cachedFeed(): List<Item> = items(read(feedFile)?.optJSONArray("items"), false)

    fun cachedRec(): RecList? = read(recFile)?.let {
        RecList(items(it.optJSONArray("items"), true), it.optDouble("fetched_at", 0.0), it.optInt("left", -1))
    }

    fun noteCached(id: String) = File(notesDir, "$id.json").exists()

    fun cachedNote(id: String): Note? = read(File(notesDir, "$id.json"))?.let { n ->
        val list = n.optJSONArray("comment_list")
        // 小红书正文里的话题写成“#词[话题]#”，去掉“[话题]”标记。/ Topics appear as "#word[话题]#"; drop the marker.
        Note(str(n, "id"), str(n, "title"), str(n, "author"), str(n, "likes"), str(n, "comments"),
            str(n, "body").replace("[话题]", ""),
            n.optDouble("time", 0.0), n.optInt("images"),
            buildList {
                if (list != null) for (i in 0 until list.length()) {
                    val c = list.optJSONObject(i) ?: continue
                    add(Comment(str(c, "user"), str(c, "text"), str(c, "likes")))
                }
            })
    }

    /** 从服务器取关注列表，保留“看过”；第一次取到的都算看过。/ Fetch the following list keeping "seen" marks; a
     *  first fetch marks everything seen. */
    suspend fun refreshFeed(): List<Item> {
        val j = api.feed()
        return withContext(Dispatchers.IO) {
            val old = read(feedFile)
            val seen = cachedFeed().filter { it.seen }.map { it.id }.toSet()
            val notes = j.optJSONArray("notes") ?: JSONArray()
            val out = JSONArray()
            for (i in 0 until notes.length()) {
                val n = notes.optJSONObject(i) ?: continue
                val id = str(n, "id")
                if (id.isEmpty()) continue
                out.put(JSONObject().put("id", id).put("title", str(n, "title")).put("author", str(n, "author"))
                    .put("likes", str(n, "likes")).put("images", n.optInt("images"))
                    .put("seen", old == null || id in seen))
            }
            write(feedFile, JSONObject().put("items", out))
            cachedFeed()
        }
    }

    suspend fun refreshRec(refresh: Boolean): RecList {
        val j = api.rec(refresh)
        return withContext(Dispatchers.IO) {
            val notes = j.optJSONArray("notes") ?: JSONArray()
            val out = JSONArray()
            for (i in 0 until notes.length()) {
                val n = notes.optJSONObject(i) ?: continue
                out.put(JSONObject().put("id", str(n, "id")).put("title", str(n, "title"))
                    .put("author", str(n, "author")).put("likes", str(n, "likes")).put("thumb", n.optBoolean("thumb")))
            }
            write(recFile, JSONObject().put("items", out).put("fetched_at", j.optDouble("fetched_at", 0.0))
                .put("left", j.optInt("left_today", -1)))
            cachedRec()!!
        }
    }

    suspend fun fetchNote(id: String, rec: Boolean): Note {
        val j = api.note(id, rec)
        return withContext(Dispatchers.IO) {
            write(File(notesDir, "$id.json"), j)
            pruneNotes()
            cachedNote(id)!!
        }
    }

    suspend fun markSeen(id: String) = withContext(Dispatchers.IO) {
        val j = read(feedFile) ?: return@withContext
        val arr = j.optJSONArray("items") ?: return@withContext
        for (i in 0 until arr.length()) {
            val o = arr.optJSONObject(i) ?: continue
            if (o.optString("id") == id) { o.put("seen", true); break }
        }
        write(feedFile, j)
    }

    // 笔记正文最多留 200 篇，删最旧的。/ Keep at most 200 note texts, dropping the oldest.
    private fun pruneNotes() {
        val files = notesDir.listFiles()?.sortedBy { it.lastModified() } ?: return
        files.take((files.size - 200).coerceAtLeast(0)).forEach { it.delete() }
    }
}
