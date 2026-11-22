package com.treader.webtoon.data

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import org.json.JSONObject
import java.io.File

/** 我的漫画里的一话（一个 CBZ）。/ One episode on the shelf (one CBZ). */
data class Episode(val file: File, val no: Int?, val name: String) {
    /** 进度表里的键：“文件夹/文件名”。/ Key in the progress table: "folder/file". */
    val key get() = "${file.parentFile?.name}/${file.name}"
}

/** 我的漫画里的一部：comics/ 下的一个文件夹；有 .webtoon 就是订阅的。/ One series on the shelf: a folder under comics/;
 *  subscribed when it holds a .webtoon. */
data class Series(val dir: File, val mark: Mark?, val episodes: List<Episode>) {
    val name: String get() = mark?.title?.ifEmpty { null } ?: dir.name
    val subscribed get() = mark != null
}

/**
 * App 目录里的 comics/，结构照 webtoon/README.md 的“设备上的文件”：一部一个文件夹，里面 .webtoon 和“第NNNN话 标题.cbz”。
 * 订阅就是扫这些 .webtoon，没有别的数据库。
 * comics/ in the app's own folder, shaped like "Files on the device" in webtoon/README.md: one folder per series with
 * .webtoon and "第NNNN话 title.cbz". Subscriptions are found by scanning these .webtoon files; there is no other database.
 */
class Shelf(val root: File) {
    init { root.mkdirs() }

    fun scan(): List<Series> {
        val dirs = root.listFiles { f -> f.isDirectory }.orEmpty()
        return dirs.mapNotNull { d ->
            val mark = File(d, Names.MARK).takeIf { it.isFile }?.let { runCatching { Mark.parse(it.readText()) }.getOrNull() }
            val eps = episodes(d)
            if (mark == null && eps.isEmpty()) null else Series(d, mark, eps)
        }.sortedWith(compareByDescending<Series> { it.dir.lastModified() }.thenBy { it.name })
    }

    /** 文件夹里的 CBZ，按文件名自然顺序（话号补零了，就是话的顺序）。/ The folder's CBZ files in name order (padded
     *  numbers make that the episode order). */
    fun episodes(dir: File): List<Episode> = dir.listFiles { f -> f.isFile && f.name.endsWith(".cbz", ignoreCase = true) }
        .orEmpty().sortedBy { it.name }
        .map { Episode(it, Names.episodeNo(it.name), it.name.removeSuffix(".cbz").removeSuffix(".CBZ")) }

    /** 这部已经在卡上的话号。/ Episode numbers of this series already on disk. */
    fun have(dir: File): Set<Int> = episodes(dir).mapNotNull { it.no }.toSet()

    fun find(id: String): Series? = scan().firstOrNull { it.mark?.id == id }

    /** 订阅：建文件夹（同名文件夹属于别的作品时加上编号）并写 .webtoon。/ Subscribe: make the folder (adding the id
     *  when a same-named folder belongs to another title) and write .webtoon. */
    fun subscribe(mark: Mark): Series {
        find(mark.id)?.let { s -> writeMark(s.dir, s.mark!!.copy(last = mark.last, title = mark.title, author = mark.author)); return find(mark.id)!! }
        var dir = File(root, Names.folder(mark.title, mark.id))
        val other = File(dir, Names.MARK).takeIf { it.isFile }?.let { Mark.parse(it.readText()) }
        if (other != null && other.id != mark.id) dir = File(root, Names.sanitize("${Names.folder(mark.title, mark.id)} ${mark.id}"))
        dir.mkdirs()
        writeMark(dir, mark)
        return Series(dir, mark, episodes(dir))
    }

    /** 写标记：先写临时文件再改名。/ Write the mark via a temp file and a rename. */
    fun writeMark(dir: File, mark: Mark) {
        val tmp = File(dir, Names.MARK + ".tmp")
        tmp.writeText(mark.toJson())
        val dest = File(dir, Names.MARK)
        if (!tmp.renameTo(dest)) { dest.delete(); tmp.renameTo(dest) }
    }

    /** 取消订阅：删掉 .webtoon；deleteFiles 时连已下载的话一起删，空了就删文件夹。/ Unsubscribe: remove .webtoon;
     *  with deleteFiles the downloaded episodes go too and an empty folder is removed. */
    fun unsubscribe(s: Series, deleteFiles: Boolean) {
        File(s.dir, Names.MARK).delete()
        if (deleteFiles) {
            episodes(s.dir).forEach { it.file.delete() }
            if (s.dir.listFiles().isNullOrEmpty()) s.dir.delete()
        }
    }

    /** 删一整部（不在订阅里的本地漫画）。/ Delete a whole series (local comics that are not subscribed). */
    fun delete(s: Series) { s.dir.deleteRecursively() }
}

/**
 * 每话读到第几页，存成一个 JSON：{"文件夹/文件名": {"page": 3, "pages": 20}}。没有记录就是没看过（我的漫画里的“N 话新”）。
 * Each episode's page, kept in one JSON: {"folder/file": {"page": 3, "pages": 20}}. No entry means unread (the
 * shelf's "N new").
 *
 * 改动先进内存，文件在后台线程写：连续翻页时等停下 [SAVE_DELAY_MS] 再写一次，[flush] 马上写（离开阅读、App 退到后台）。
 * 同一时间只有一个写文件的人（fileLock），写的是当时内存的完整快照，所以和检查更新、删除这些后台任务一起改也不会写乱。
 * Changes go to memory first and the file is written on a background thread: page turns in a row are written once
 * after a [SAVE_DELAY_MS] pause, and [flush] writes at once (leaving the reader, app going to the background). Only one
 * writer touches the file at a time (fileLock) and it writes a full snapshot of memory, so changes from background
 * jobs such as update checks or deletes cannot interleave badly.
 */
class Progress(private val file: File) {
    data class P(val page: Int, val pages: Int) {
        val finished get() = pages > 0 && page >= pages - 1
    }

    private var map: MutableMap<String, P> = load()
    private var version = 0L                 // 内存改了几次 / Memory changes so far
    private var written = 0L                 // 文件里是第几次的 / The change the file holds
    private val fileLock = Any()
    private val writer = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var pending: Job? = null

    private fun load(): MutableMap<String, P> = runCatching {
        val o = JSONObject(file.readText())
        o.keys().asSequence().associateWith { k ->
            val v = o.getJSONObject(k)
            P(v.optInt("page", 0), v.optInt("pages", 0))
        }.toMutableMap()
    }.getOrElse { mutableMapOf() }

    @Synchronized
    fun get(key: String): P? = map[key]

    @Synchronized
    fun set(key: String, page: Int, pages: Int) {
        if (map[key] == P(page, pages)) return
        map[key] = P(page, pages)
        version++
        schedule(SAVE_DELAY_MS)
    }

    @Synchronized
    fun forget(prefix: String) {
        if (!map.keys.removeAll { it.startsWith(prefix) }) return
        version++
        schedule(0)
    }

    /** 有没写的就马上在后台写。/ Write anything unsaved now, in the background. */
    @Synchronized
    fun flush() = schedule(0)

    @Synchronized
    private fun schedule(delayMs: Long) {
        pending?.cancel()
        pending = writer.launch {
            if (delayMs > 0) delay(delayMs)
            writeNow()
        }
    }

    /** 把内存写进文件（测试和后台线程用）；已经是最新的就不写。/ Write memory to the file (tests and the background
     *  thread); skip when the file is already current. */
    internal fun writeNow() {
        synchronized(fileLock) {
            val (v, text) = synchronized(this) {
                if (version == written) return
                val o = JSONObject()
                map.forEach { (k, p) -> o.put(k, JSONObject().put("page", p.page).put("pages", p.pages)) }
                version to o.toString()
            }
            val tmp = File(file.path + ".tmp")
            tmp.writeText(text)
            if (!tmp.renameTo(file)) { file.delete(); tmp.renameTo(file) }
            synchronized(this) { written = v }
        }
    }

    companion object {
        const val SAVE_DELAY_MS = 800L
    }
}
