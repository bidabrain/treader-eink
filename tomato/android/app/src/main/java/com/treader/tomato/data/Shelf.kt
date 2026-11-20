package com.treader.tomato.data

import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/** 番茄书架上的一本（字段和固件 tomato_book_t、shelf.json 一样）。/ One Tomato shelf book, same fields as the
 *  firmware's tomato_book_t and shelf.json. */
data class TomatoBook(
    val bookId: String,
    val title: String,
    val author: String,
    /** 本地 TXT 的章节数。/ Chapters in the local TXT. */
    val chapters: Int,
    /** 最近一次检查到的服务器章节数。/ Server chapters at the last check. */
    val remote: Int,
    /** books/ 下的文件名；空表示还没下载（同步来的）。/ File name under books/; empty when not downloaded yet. */
    val file: String,
    /** 上次打开时的章节数；旧文件缺省为 chapters。/ Chapters when last opened; defaults to chapters. */
    val seen: Int = chapters,
) {
    /** 检查到、还没下载。/ Found by a check, not downloaded yet. */
    val pending get() = remote > chapters
    /** 待下载的章节数。/ Chapters waiting to download. */
    val pendingCount get() = maxOf(0, remote - chapters)
    /** 下载了、打开后还没看的章节数。/ Chapters downloaded but not opened since. */
    val freshCount get() = maxOf(0, chapters - seen)
}

/** 搜索结果（固件 tomato_hit_t）。/ A search result (the firmware's tomato_hit_t). */
data class Hit(
    val bookId: String,
    val title: String,
    val author: String,
    val category: String,
    val chapters: Int,
    val words: Long,
    val finished: Boolean,
    val abstract: String,
    val thumb: String,
    /** 书城：在读人数，如“25.4万”。/ Bookstore: readers, e.g. "25.4万". */
    val reads: String = "",
    /** 书城：男频 / 女频 · 分类。/ Bookstore: channel and category. */
    val channel: String = "",
)

/**
 * shelf.json 和 removed.json 的读写，格式和固件一样（{"books":[…]}、{"removed":[…]}），写的时候先写临时文件再改名。
 * Reads and writes shelf.json and removed.json in the firmware's format ({"books":[...]}, {"removed":[...]}), via a
 * temporary file and a rename.
 */
class ShelfStore(private val dirs: Dirs) {
    @Synchronized
    fun load(): List<TomatoBook> {
        val arr = read(dirs.shelf)?.optJSONArray("books") ?: return emptyList()
        return buildList {
            for (i in 0 until arr.length()) {
                val o = arr.optJSONObject(i) ?: continue
                val id = utf8Cut(o.optString("book_id"), Caps.ID)
                if (id.isEmpty()) continue
                val chapters = o.optInt("chapters")
                // 旧版本写的书架没有 seen：当作都看过，只留下待下载的提示。/ Older shelves lack seen: treat as seen.
                add(TomatoBook(id, utf8Cut(o.optString("title"), Caps.TITLE), utf8Cut(o.optString("author"), Caps.AUTHOR),
                    chapters, o.optInt("remote"), utf8Cut(o.optString("file"), Caps.FILE),
                    if (o.has("seen")) o.optInt("seen") else chapters))
            }
        }
    }

    /**
     * 保存时和磁盘上的书架合并 seen（取大）：后台操作开始时读的旧副本不会冲掉期间记下的“已看”。
     * Saving merges seen with the shelf on disk (larger wins), so a copy read when an operation started cannot undo a
     * "seen" recorded meanwhile.
     */
    @Synchronized
    fun save(books: List<TomatoBook>) {
        val disk = read(dirs.shelf)?.optJSONArray("books")
        val diskSeen = buildMap {
            for (i in 0 until (disk?.length() ?: 0)) {
                val o = disk!!.optJSONObject(i) ?: continue
                if (o.has("seen")) put(o.optString("book_id"), o.optInt("seen"))
            }
        }
        val arr = JSONArray()
        books.forEach {
            arr.put(JSONObject().put("book_id", it.bookId).put("title", it.title).put("author", it.author)
                .put("file", it.file).put("chapters", it.chapters).put("remote", it.remote)
                .put("seen", maxOf(it.seen, diskSeen[it.bookId] ?: 0)))
        }
        write(dirs.shelf, JSONObject().put("books", arr))
    }

    /** 打开了这本书：seen 记成当前章节数；不是番茄书时什么也不做。返回书架是否变了。/ The book was opened: set seen
     *  to its chapters; no-op for other books. Returns whether the shelf changed. */
    @Synchronized
    fun markSeen(file: String): Boolean {
        if (file.isEmpty()) return false
        val books = load()
        if (books.none { it.file == file && it.seen < it.chapters }) return false
        save(books.map { if (it.file == file && it.seen < it.chapters) it.copy(seen = it.chapters) else it })
        return true
    }

    @Synchronized
    fun removed(): List<String> {
        val arr = read(dirs.removed)?.optJSONArray("removed") ?: return emptyList()
        return (0 until arr.length()).map { arr.optString(it) }.filter { it.isNotEmpty() }
    }

    @Synchronized
    fun saveRemoved(ids: List<String>) {
        write(dirs.removed, JSONObject().put("removed", JSONArray(ids.distinct().takeLast(REMOVED_MAX))))
    }

    /** 加入或移出已删除清单（固件 tomato_removed_mark）。/ Add to or drop from the removed list. */
    fun markRemoved(bookId: String, removed: Boolean) {
        val ids = removed()
        if (removed && bookId !in ids) saveRemoved(ids + bookId)
        if (!removed && bookId in ids) saveRemoved(ids - bookId)
    }

    /** 书库删掉一本书：从番茄书架去掉并记进已删除清单（固件 tomato_shelf_remove_now）。/ The library deleted a file:
     *  drop it from the Tomato shelf and record the book as removed. */
    fun removeFile(file: String) {
        val books = load()
        val gone = books.filter { it.file == file }
        if (gone.isEmpty()) return
        save(books - gone.toSet())
        gone.forEach { markRemoved(it.bookId, true); File(dirs.covers, "${it.file}.img").delete() }
    }

    private fun read(f: File): JSONObject? = runCatching { JSONObject(f.readText()) }.getOrNull()

    private fun write(f: File, j: JSONObject) {
        val tmp = File(f.path + ".tmp")
        tmp.writeText(j.toString(1))
        if (!tmp.renameTo(f)) { f.delete(); tmp.renameTo(f) }
    }

    companion object {
        const val SHELF_MAX = 64
        const val REMOVED_MAX = 64
    }
}
