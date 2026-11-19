package com.treader.tomato.data

import kotlinx.coroutines.delay
import kotlinx.coroutines.ensureActive
import org.json.JSONObject
import java.io.File
import kotlin.coroutines.coroutineContext

enum class Op { CHECK, UPDATE, SEARCH, DOWNLOAD }
enum class Phase { IDLE, CHECKING, SEARCHING, QUEUED, DOWNLOADING, DONE, FAILED }

/** 一次操作的状态快照（固件 tomato_status_t）。/ Status snapshot of one operation (the firmware's tomato_status_t). */
data class OpStatus(
    val op: Op? = null,
    val running: Boolean = false,
    val phase: Phase = Phase.IDLE,
    val message: String = "",
    val current: Int = 0,
    val total: Int = 0,
    val bytes: Long = 0,
)

/**
 * 番茄的四个操作，照固件 components/tomato/tomato_sync.c：检查更新、下载全部更新、搜索、下载一本。同一时间只跑一个，
 * 都在用户点的时候才跑，不自动检查。
 * 更新规则：服务器章节数取 max(remote_total, local_total)，比本地 TXT 多才重新下载；不看服务器的 has_update。
 * The four Tomato operations after the firmware's tomato_sync.c: check, download all updates, search and download one
 * book. One at a time, only on a tap, never automatic.
 * Update rule: server chapters = max(remote_total, local_total); re-download only when that exceeds the local TXT;
 * never trust the server's has_update.
 */
class TomatoOps(
    private val api: TomatoApi,
    private val dirs: Dirs,
    private val shelf: ShelfStore,
    /** 正在阅读的书（文件名），更新时跳过。/ The book open in the reader (file name), skipped by updates. */
    private val openFile: () -> String?,
    /** 书文件换了：旧大小、新大小（阅读进度据此处理）。/ A book file changed: old and new size (progress uses it). */
    private val onFileChanged: (File, Long, Long) -> Unit,
    private val report: (OpStatus) -> Unit,
) {
    private var status = OpStatus()

    private fun set(block: OpStatus.() -> OpStatus) {
        status = status.block()
        report(status)
    }

    private suspend fun waitPoll() = delay(POLL_MS)

    private fun hardError(status: Int) = status == 401 || status == 403 || (status in 400..499 && status != 429)

    /* ---- 下载一本 / Fetch one book ---- */

    // 在书库里找书名对应的 TXT 相对路径；可能在书名子目录里。/ Find the TXT path for a title, maybe in a title folder.
    private suspend fun findLibraryFile(title: String): String {
        val esc = urlEscape(title)
        for (attempt in 0 until SCAN_WAIT_POLLS) {
            if (attempt > 0) waitPoll()
            val root = try {
                api.get("/api/library?name=$esc&start=${if (attempt > 0) "false" else "true"}")
            } catch (e: TomatoApi.HttpError) {
                if (hardError(e.status)) throw e
                continue
            }
            if (root.optBoolean("running")) continue
            var dir = ""
            val items = root.optJSONArray("items")
            for (i in 0 until (items?.length() ?: 0)) {
                val it = items!!.optJSONObject(i) ?: continue
                if (it.optString("kind") == "file" && it.optString("ext") == "txt") return it.optString("rel_path")
                if (it.optString("kind") == "dir" && dir.isEmpty()) dir = it.optString("rel_path")
            }
            if (dir.isEmpty()) throw TomatoApi.HttpError(404, "服务器书库里找不到这本书")
            val sub = api.get("/api/library?path=${urlEscape(dir)}&name=$esc").optJSONArray("items")
            for (i in 0 until (sub?.length() ?: 0)) {
                val it = sub!!.optJSONObject(i) ?: continue
                if (it.optString("kind") == "file" && it.optString("ext") == "txt") return it.optString("rel_path")
            }
            throw TomatoApi.HttpError(404, "服务器书库里找不到这本书")
        }
        throw TomatoApi.HttpError(0, "服务器扫描书库超时")
    }

    /** 让服务器生成 TXT 并下载进 books/；返回更新后的书。/ Have the server build the TXT and fetch it into books/;
     *  returns the updated book. */
    private suspend fun fetchBook(book: TomatoBook): TomatoBook {
        set { copy(phase = Phase.QUEUED, message = "服务器准备中", bytes = 0) }
        var job = -1
        val body = JSONObject().put("book_id", book.bookId).toString()
        for (i in 0 until BUSY_RETRIES) {
            try {
                job = api.post("/api/jobs", body).optInt("id", -1)
                break
            } catch (e: TomatoApi.HttpError) {
                if (e.status != 429) throw e
                waitPoll(); waitPoll()
            }
        }
        if (job < 0) throw TomatoApi.HttpError(0, "服务器一直忙，稍后再试")

        var title = ""
        var chapterTotal = 0
        var done = false
        var poll = 0
        while (poll++ < JOB_WAIT_POLLS && !done) {
            waitPoll()
            val root = try {
                api.get("/api/jobs?id=$job")
            } catch (e: TomatoApi.HttpError) {
                if (hardError(e.status)) throw e
                continue
            }
            val item = root.optJSONArray("items")?.optJSONObject(0) ?: JSONObject()
            val state = item.optString("state")
            title = utf8Cut(item.optString("title"), Caps.TITLE)
            chapterTotal = item.optJSONObject("progress")?.optInt("chapter_total") ?: 0
            if (state == "failed") throw TomatoApi.HttpError(0, "服务器生成失败")
            done = state == "done"
            // 下载器在问格式或书名：要 TXT、书名用默认。/ The downloader asks for a format or a name: TXT, default name.
            if (!item.isNull("format_options") && item.has("format_options"))
                runCatching { api.post("/api/jobs/$job/format", "{\"value\":\"txt\"}") }
            if (!item.isNull("book_name_options") && item.has("book_name_options"))
                runCatching { api.post("/api/jobs/$job/book_name", "{\"value\":null}") }
        }
        if (!done) throw TomatoApi.HttpError(0, "服务器生成超时")
        if (title.isEmpty()) title = book.title

        val rel = findLibraryFile(title)
        // 更新沿用原文件名，进度跟着文件走；新书起新名字，不覆盖别的书。/ Updates keep the old name so progress follows
        // it; a new book gets a fresh name and never overwrites another.
        val file = if (book.file.isNotEmpty() && fileNameSafe(book.file)) book.file else newBookFile(title, book.bookId, dirs.books)
        val dest = File(dirs.books, file)
        val oldSize = if (dest.exists()) dest.length() else 0L
        set { copy(phase = Phase.DOWNLOADING, message = title) }
        api.download("/download/" + urlEscape(rel, keepSlash = true), dest) { n -> set { copy(bytes = n) } }
        onFileChanged(dest, oldSize, dest.length())
        var chapters = maxOf(book.chapters, chapterTotal)
        if (book.remote > chapters) chapters = book.remote
        return book.copy(title = title, file = file, chapters = chapters)
    }

    /* ---- 检查更新 / Check ---- */

    private suspend fun remoteTotals(books: List<TomatoBook>): Map<String, Int> {
        set { copy(phase = Phase.CHECKING, message = "检查更新") }
        var root = api.get("/api/updates?start=true")
        var polls = 0
        while (root.optBoolean("running") && polls++ < SCAN_WAIT_POLLS) {
            waitPoll()
            root = api.get("/api/updates?start=false")
        }
        // 等满 60 秒还没扫完：别拿半截结果判断。/ Still scanning after 60 s: don't decide from partial results.
        if (root.optBoolean("running")) throw TomatoApi.HttpError(0, "服务器扫描超时，稍后再试")
        val out = mutableMapOf<String, Int>()
        for (list in listOf("updates", "no_updates")) {
            val arr = root.optJSONArray(list) ?: continue
            for (i in 0 until arr.length()) {
                val row = arr.optJSONObject(i) ?: continue
                val total = maxOf(row.optInt("remote_total"), row.optInt("local_total"))
                if (total > 0) out[row.optString("book_id")] = total
            }
        }
        return out.filterKeys { id -> books.any { it.bookId == id } }
    }

    suspend fun check(): String {
        val books = shelf.load()
        if (books.isEmpty()) return "书架上还没有番茄小说"
        val totals = remoteTotals(books)
        val out = books.mapIndexed { i, b ->
            coroutineContext.ensureActive()
            val t = totals[b.bookId]
            if (t != null) b.copy(remote = t)
            else {
                // 服务器扫描里没有的书：直接数番茄目录的章节。/ Missing from the scan: count the Tomato directory.
                set { copy(current = i + 1, total = books.size) }
                val n = api.fanqieChapters(b.bookId)
                if (n > 0) b.copy(remote = n) else b
            }
        }
        shelf.save(out)
        val pending = out.count { it.pending }
        return if (pending > 0) "$pending 本有新章节" else "都是最新的"
    }

    /* ---- 下载全部更新 / Download all updates ---- */

    suspend fun update(): String {
        var books = shelf.load()
        val pending = books.count { it.pending }
        if (pending == 0) return "没有待下载的更新"
        var done = 0
        var updated = 0
        var skipped = 0
        for (b in books.filter { it.pending }) {
            coroutineContext.ensureActive()
            set { copy(current = ++done, total = pending) }
            if (b.file.isNotEmpty() && b.file == openFile()) { skipped++; continue }
            try {
                val nb = fetchBook(b)
                books = shelf.load().map { if (it.bookId == nb.bookId) nb else it }
                shelf.save(books)
                updated++
            } catch (e: TomatoApi.HttpError) {
                if (e.status == 401 || e.status == 403) throw e
            }
        }
        val failed = pending - updated - skipped
        return when {
            skipped > 0 && failed == 0 -> "更新了 $updated 本，正在读的那本稍后再更新"
            failed == 0 -> "更新了 $updated 本"
            else -> "更新了 $updated 本，$failed 本失败"
        }
    }

    /* ---- 搜索 / Search ---- */

    suspend fun search(q: String): List<Hit> {
        set { copy(phase = Phase.SEARCHING, message = q) }
        val root = api.get("/api/search?q=" + urlEscape(q))
        val items = root.optJSONArray("items") ?: return emptyList()
        return buildList {
            for (i in 0 until items.length()) {
                if (size >= SEARCH_MAX) break
                val it = items.optJSONObject(i) ?: continue
                val raw = it.optJSONObject("raw") ?: JSONObject()
                val h = Hit(
                    bookId = utf8Cut(it.optString("book_id"), Caps.ID),
                    title = utf8Cut(raw.optString("book_name"), Caps.TITLE),
                    author = utf8Cut(it.optString("author"), Caps.AUTHOR),
                    category = utf8Cut(raw.optString("category"), 16),
                    chapters = raw.optInt("serial_count"),
                    words = raw.optLong("word_number"),
                    // update_status 为 "0" 表示已完结。/ update_status "0" means completed.
                    finished = raw.optString("update_status") == "0",
                    abstract = utf8Cut(raw.optString("abstract"), 320),
                    thumb = raw.optString("thumb_url").takeIf { u -> u.length < 400 }.orEmpty(),
                )
                if (h.bookId.isNotEmpty() && h.title.isNotEmpty()) add(h)
            }
        }
    }

    /* ---- 下载一本 / Download one ---- */

    suspend fun download(hit: Hit, coverUrl: String): String {
        var books = shelf.load()
        var b = books.firstOrNull { it.bookId == hit.bookId }
        if (b == null) {
            if (books.size >= ShelfStore.SHELF_MAX) throw TomatoApi.HttpError(0, "书架已满")
            b = TomatoBook(hit.bookId, hit.title, hit.author, 0, hit.chapters, "")
        }
        if (b.file.isNotEmpty() && b.file == openFile()) throw TomatoApi.HttpError(0, "这本书正在阅读，先退出再更新")
        set { copy(current = 1, total = 1) }
        val nb = fetchBook(b)
        books = shelf.load()
        books = if (books.any { it.bookId == nb.bookId }) books.map { if (it.bookId == nb.bookId) nb else it } else books + nb
        shelf.save(books)
        // 重新下载的书不再算“已删除”。/ A re-downloaded book is no longer "removed".
        shelf.markRemoved(nb.bookId, false)
        fetchCover(nb, coverUrl)
        return "已加入书架"
    }

    // 封面尽力而为：covers/<文件名>.img。/ Covers are best effort: covers/<file name>.img.
    private suspend fun fetchCover(book: TomatoBook, url: String) {
        if (url.isEmpty()) return
        val dest = File(dirs.covers, "${book.file}.img")
        if (dest.exists()) return
        val cached = File(dirs.store, "c/${book.bookId}.jpg")
        if (cached.exists()) { runCatching { cached.copyTo(dest, overwrite = true) }; return }
        set { copy(phase = Phase.DOWNLOADING, message = "下载封面") }
        if (Http.fetch(url, dest)) return
        // 图床有时按格式协商给 WEBP；去掉处理参数再要一次原图。/ Hosts may hand back WEBP; retry the bare image.
        val cut = url.indexOfFirst { it == '~' || it == '?' }
        if (cut > 0) Http.fetch(url.substring(0, cut), dest)
    }

    companion object {
        const val POLL_MS = 2000L
        const val SCAN_WAIT_POLLS = 30
        const val JOB_WAIT_POLLS = 300
        const val BUSY_RETRIES = 6
        const val SEARCH_MAX = 12
    }
}
