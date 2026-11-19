package com.treader.tomato.data

import org.json.JSONObject
import java.io.File

/**
 * 一本书的阅读进度，字段和固件 book_progress_t 一样：文件大小、第几章、章内 UTF-8 字节偏移、字号（设备像素 36～72，
 * 步长 4）、全书百分比、阅读序号（越大越近）。
 * One book's reading progress with the firmware's book_progress_t fields: file size, chapter, UTF-8 byte offset within
 * the chapter, font size (device pixels 36-72 in steps of 4), whole-book percent and a reading sequence (larger is
 * more recent).
 */
data class Progress(
    val size: Long,
    val chapter: Int,
    val off: Int,
    val px: Int,
    val pct: Int,
    val seq: Long = 0,
)

/**
 * 进度存在 App 目录的 progress.json，键是固件的书路径（/sdcard/books/<文件名>），这样 WebDAV 同步文件里的键两边一样。
 * 规则照固件：文件大小对不上就不恢复；番茄更新追加章节后只改记录的大小（resize）。
 * Progress lives in progress.json in the app folder, keyed by the firmware's book path (/sdcard/books/<file name>) so
 * the WebDAV sync file uses the same keys on both sides. Firmware rules: a size mismatch is not restored; after a
 * Tomato update appends chapters only the recorded size changes (resize).
 */
class ProgressStore(private val file: File) {
    private var data: JSONObject = runCatching { JSONObject(file.readText()) }.getOrDefault(JSONObject())

    private val books: JSONObject get() = data.optJSONObject("books") ?: JSONObject().also { data.put("books", it) }

    @Synchronized
    private fun flush() {
        val tmp = File(file.path + ".tmp")
        tmp.writeText(data.toString())
        if (!tmp.renameTo(file)) { file.delete(); tmp.renameTo(file) }
    }

    @Synchronized
    fun raw(key: String): Progress? = books.optJSONObject(key)?.let {
        Progress(it.optLong("size"), it.optInt("chapter"), it.optInt("off"), it.optInt("px", 40), it.optInt("pct"), it.optLong("seq"))
    }

    /** 文件大小对得上才给（book_progress_load）。/ Only when the size matches (book_progress_load). */
    fun load(key: String, size: Long): Progress? = raw(key)?.takeIf { it.size == size }

    @Synchronized
    fun all(): Map<String, Progress> = books.keys().asSequence().mapNotNull { k -> raw(k)?.let { k to it } }.toMap()

    /** 保存并记一个新的阅读序号（这本成了最近读的）。/ Save with a new reading sequence (now the most recent). */
    @Synchronized
    fun save(key: String, p: Progress) {
        val seq = data.optLong("seq") + 1
        data.put("seq", seq)
        put(key, p.copy(seq = seq))
        data.put("last", key)
        flush()
    }

    /** WebDAV 导入：沿用原来的序号，不顶到最近（book_progress_import）。/ WebDAV import keeps the old sequence. */
    @Synchronized
    fun import(key: String, p: Progress) {
        put(key, p.copy(seq = raw(key)?.seq ?: 0))
        flush()
    }

    private fun put(key: String, p: Progress) {
        books.put(key, JSONObject().put("size", p.size).put("chapter", p.chapter).put("off", p.off).put("px", p.px)
            .put("pct", p.pct).put("seq", p.seq))
    }

    /** 番茄更新追加章节后改记录的大小，章节和偏移不动（book_progress_resize）。/ After an update appends chapters,
     *  change only the recorded size (book_progress_resize). */
    @Synchronized
    fun resize(key: String, oldSize: Long, newSize: Long) {
        val p = raw(key) ?: return
        if (p.size != oldSize) return
        put(key, p.copy(size = newSize))
        flush()
    }

    @Synchronized
    fun forget(key: String) {
        books.remove(key)
        if (data.optString("last") == key) data.remove("last")
        flush()
    }

    @Synchronized
    fun rename(from: String, to: String) {
        val p = raw(from) ?: return
        books.remove(from)
        put(to, p)
        if (data.optString("last") == from) data.put("last", to)
        flush()
    }

    val last: String? @Synchronized get() = data.optString("last").takeIf { it.isNotEmpty() }

    companion object {
        /** 固件里的书路径。/ The firmware's path for a book file. */
        fun key(fileName: String) = "/sdcard/books/$fileName"
    }
}
