package com.treader.tomato.data

import com.treader.tomato.book.BookSource
import com.treader.tomato.book.EpubBook
import com.treader.tomato.book.TxtBook
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import okhttp3.Credentials
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/**
 * WebDAV 同步，和设备共用同一个 treader-sync.json，规则照固件 components/tomato/tomato_webdav.c：
 * - 本机进度和上次同步的签名不同：本机改过，上传；本机没改、服务器那条在上次同步之后更新过：写回本机；
 *   本机从没同步过这本：以服务器为准。
 * - 番茄书架按 book_id 取并集，只在服务器上的书加进来等下载；“已删除”清单两边取并集。
 * - 服务器文件读得到却解析不了时不覆盖。书籍文件本身不传，删除不同步。
 * 另外两处修正（固件同时改）：
 * 1. 只在服务器上的进度：本机有这本书、从没同步过它，就算不比上次同步新也写回（先同步、后下载书的情况）。
 * 2. 写回前核对位置：章节号超出本机这本书的章节数、或偏移超出那一章，就不用（文件和备份时不一样，比如换了下载服务器）。
 *
 * WebDAV sync sharing the device's treader-sync.json, with the rules of the firmware's tomato_webdav.c:
 * - Local progress differing from its last-sync signature changed here and is uploaded; unchanged here and newer on
 *   the server is written back; a book never synced here takes the server's entry.
 * - The Tomato shelf is a union by book_id, remote-only books join pending download; the removed lists are merged.
 * - A server file that downloads but does not parse is never overwritten. Book files are not transferred and
 *   deletions are not synced.
 * Two fixes (made in the firmware too):
 * 1. Server-only progress is written back when the book exists here and was never synced, even if not newer than
 *    the last sync (sync first, download the book later).
 * 2. Positions are checked before writing back: a chapter past this book's chapter count, or an offset past that
 *    chapter, is skipped (the file differs from the backup, for example after switching download servers).
 */
class WebDav(private val prefs: Prefs, private val dirs: Dirs, private val shelf: ShelfStore, private val progress: ProgressStore) {
    private val stateFile = File(dirs.tomato, "webdav_state.json")

    private fun url() = prefs.davUrl.trimEnd('/') + "/treader-sync.json"

    private fun request(): Request.Builder {
        val b = Request.Builder().url(url())
        if (prefs.davUser.isNotEmpty()) b.header("Authorization", Credentials.basic(prefs.davUser, prefs.davPassword))
        return b
    }

    private fun sig(p: Progress) = "${p.size}:${p.chapter}:${p.off}"

    /** 本机这本书能不能放下这个位置。/ Whether this book here can hold the position. */
    private fun positionFits(f: File, chapter: Int, off: Int): Boolean = runCatching {
        val src: BookSource = if (f.name.endsWith(".epub", true)) EpubBook.open(f) else TxtBook.open(f)
        chapter < src.chapterCount && off <= src.chapterUtf8(chapter).size
    }.getOrDefault(false)

    private enum class Import { OK, MISSING, MISMATCH }

    private fun import(path: String, r: JSONObject): Import {
        if (!path.startsWith("/sdcard/books/")) return Import.MISSING
        val name = path.removePrefix("/sdcard/books/")
        if (!fileNameSafe(name)) return Import.MISSING
        val f = File(dirs.books, name)
        val size = r.optLong("size")
        // 本机文件不能比记录时小（番茄更新只追加章节）。/ The local file must be no smaller (updates only append).
        if (size <= 0 || !f.exists() || f.length() < size) return Import.MISSING
        val chapter = r.optInt("chapter")
        val off = r.optInt("off")
        if (!positionFits(f, chapter, off)) return Import.MISMATCH
        val px = r.optInt("px").takeIf { it in 36..72 && (it - 36) % 4 == 0 } ?: 40
        progress.import(ProgressStore.key(name), Progress(f.length(), chapter, off, px, r.optInt("pct").coerceIn(0, 100)))
        return Import.OK
    }

    suspend fun run(): String = withContext(Dispatchers.IO) {
        if (prefs.davUrl.isEmpty()) throw TomatoApi.HttpError(0, "还没设置 WebDAV")
        val get = try {
            Http.client.newCall(request().build()).execute()
        } catch (e: Exception) {
            throw TomatoApi.HttpError(0, "连不上 WebDAV 服务器")
        }
        val body = get.use {
            when (it.code) {
                200 -> it.body?.string().orEmpty()
                404 -> null
                401 -> throw TomatoApi.HttpError(401, "WebDAV 用户名或密码不对")
                else -> throw TomatoApi.HttpError(it.code, "连不上 WebDAV 服务器（HTTP ${it.code}）")
            }
        }
        val remote = if (body == null) JSONObject() else runCatching { JSONObject(body) }.getOrNull()
            // 文件在但读不懂：别用空内容覆盖别的设备的数据。/ Present but unreadable: never overwrite other devices' data.
            ?: throw TomatoApi.HttpError(0, "服务器上的同步文件损坏，未覆盖")
        val rp = remote.optJSONObject("progress") ?: JSONObject().also { remote.put("progress", it) }
        val state = runCatching { JSONObject(stateFile.readText()) }.getOrDefault(JSONObject())
        val last = state.optDouble("last", 0.0)
        val sigs = state.optJSONObject("sig") ?: JSONObject()
        val now = System.currentTimeMillis() / 1000
        var uploaded = 0
        var downloaded = 0
        var mismatched = 0

        // 本机 → 服务器，或服务器 → 本机。/ Local to remote, or remote to local.
        val local = progress.all()
        for ((path, p) in local) {
            val old = sigs.optString(path).takeIf { sigs.has(path) }
            val never = old == null
            val changed = !never && old != sig(p)
            val r = rp.optJSONObject(path)
            if (r != null && (never || (!changed && r.optDouble("time") > last))) {
                when (import(path, r)) { Import.OK -> downloaded++; Import.MISMATCH -> mismatched++; Import.MISSING -> {} }
                continue
            }
            if (r == null || changed) {
                rp.put(path, JSONObject().put("size", p.size).put("chapter", p.chapter).put("off", p.off).put("px", p.px)
                    .put("pct", p.pct).put("time", now))
                uploaded++
            }
        }
        // 只在服务器上的进度：比上次同步新，或本机从没同步过这本（修正 1）。/ Server-only entries: newer than the last
        // sync, or never synced here (fix 1).
        for (path in rp.keys().asSequence().toList()) {
            if (path in local) continue
            val r = rp.optJSONObject(path) ?: continue
            if (r.optDouble("time") <= last && sigs.has(path)) continue
            when (import(path, r)) { Import.OK -> downloaded++; Import.MISMATCH -> mismatched++; Import.MISSING -> {} }
        }

        // 番茄书架和已删除清单。/ Tomato shelf and the removed list.
        val removed = shelf.removed().toMutableList()
        val rr = remote.optJSONArray("removed")
        for (i in 0 until (rr?.length() ?: 0)) {
            val id = utf8Cut(rr!!.optString(i), Caps.ID)
            if (id.isNotEmpty() && id !in removed && removed.size < ShelfStore.REMOVED_MAX) removed += id
        }
        shelf.saveRemoved(removed)
        val books = shelf.load().toMutableList()
        var added = 0
        val rt = remote.optJSONArray("tomato")
        for (i in 0 until (rt?.length() ?: 0)) {
            val o = rt!!.optJSONObject(i) ?: continue
            val id = utf8Cut(o.optString("book_id"), Caps.ID)
            if (id.isEmpty() || id in removed) continue
            val total = maxOf(o.optInt("chapters"), o.optInt("remote"))
            val at = books.indexOfFirst { it.bookId == id }
            if (at < 0) {
                if (books.size >= ShelfStore.SHELF_MAX) continue
                // 文件名留空：本机还没有这本，下载时再起名。/ Empty file: not downloaded here yet; named at download.
                books += TomatoBook(id, utf8Cut(o.optString("title"), Caps.TITLE), utf8Cut(o.optString("author"), Caps.AUTHOR),
                    0, if (total > 0) total else 1, "")
                added++
            } else {
                if (total > books[at].remote) books[at] = books[at].copy(remote = total)
                // 另一台设备打开过的章节数，取大：在设备上看过，App 里的“+N 章”也消失。
                // Chapters seen on the other device, larger wins: reading on the device clears the badge here too.
                val seen = o.optInt("seen")
                if (seen > books[at].seen) books[at] = books[at].copy(seen = seen)
            }
        }
        // 另一台设备删掉的书，本机书架也去掉记录（文件不删）。/ Drop records another device removed; files stay.
        books.removeAll { it.bookId in removed }
        shelf.save(books)
        remote.put("tomato", JSONArray().apply {
            books.forEach {
                put(JSONObject().put("book_id", it.bookId).put("title", it.title).put("author", it.author).put("file", it.file)
                    .put("chapters", it.chapters).put("remote", it.remote).put("seen", it.seen))
            }
        })
        remote.put("removed", JSONArray(removed))
        remote.put("version", 1)
        remote.put("updated", now)

        val put = try {
            Http.client.newCall(request().put(remote.toString().toRequestBody("application/json".toMediaType())).build()).execute()
        } catch (e: Exception) {
            throw TomatoApi.HttpError(0, "上传到 WebDAV 失败")
        }
        put.use { if (!it.isSuccessful) throw TomatoApi.HttpError(it.code, "上传到 WebDAV 失败（HTTP ${it.code}）") }

        // 记下这次同步后的签名。/ Record signatures after this sync.
        val ns = JSONObject()
        progress.all().forEach { (k, p) -> ns.put(k, sig(p)) }
        stateFile.writeText(JSONObject().put("last", now).put("sig", ns).toString())

        buildString {
            append("已同步：上传 $uploaded 条，下载 $downloaded 条")
            if (added > 0) append("，书架有新书")
            if (mismatched > 0) append("；$mismatched 本书的文件和备份时不一样，进度没同步")
        }
    }
}
