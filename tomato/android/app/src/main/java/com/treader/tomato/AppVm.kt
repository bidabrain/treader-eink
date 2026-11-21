package com.treader.tomato

import android.app.Application
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import android.net.Uri
import com.treader.tomato.book.BookSource
import com.treader.tomato.book.EpubBook
import com.treader.tomato.book.Style
import com.treader.tomato.book.TxtBook
import com.treader.tomato.data.Dirs
import com.treader.tomato.data.LibBook
import com.treader.tomato.data.Library
import com.treader.tomato.data.ProgressStore
import com.treader.tomato.data.Hit
import com.treader.tomato.data.Op
import com.treader.tomato.data.OpStatus
import com.treader.tomato.data.Phase
import com.treader.tomato.data.Prefs
import com.treader.tomato.data.RankBook
import com.treader.tomato.data.RankCat
import com.treader.tomato.data.ShelfStore
import com.treader.tomato.data.Store
import com.treader.tomato.data.TomatoApi
import com.treader.tomato.data.TomatoBook
import com.treader.tomato.data.TomatoOps
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

enum class Tab { LIBRARY, TOMATO, SETTINGS }

/** 番茄标签里的页面（固件 app_tr_tomato.c 的 V_LIST / V_SEARCH / V_RESULTS / V_DETAIL）。/ Pages in the Tomato tab. */
sealed interface TomatoView {
    data object Shelf : TomatoView
    data object Search : TomatoView
    data object Results : TomatoView
    data class Detail(val hit: Hit, val fromStore: Boolean = false) : TomatoView
}

/**
 * 全局状态和动作。联网操作同一时间只跑一个（和固件的后台任务一样），都只在点击时运行。
 * App-wide state and actions. One network operation at a time, like the firmware's background task, and only on a tap.
 */
class AppVm(app: Application) : AndroidViewModel(app) {
    val prefs = Prefs(app)
    val dirs = Dirs(app)
    val shelfStore = ShelfStore(dirs)
    val api = TomatoApi(prefs)

    val progressStore = ProgressStore(java.io.File(app.filesDir, "progress.json"))
    val library = Library(dirs, shelfStore, progressStore)
    private val ui = app.getSharedPreferences("reader", android.content.Context.MODE_PRIVATE)

    var tab by mutableStateOf(Tab.LIBRARY)
    var books by mutableStateOf<List<LibBook>>(emptyList())
    var libMessage by mutableStateOf("")
    var reader by mutableStateOf<ReaderState?>(null)
    var opening by mutableStateOf(false)
    var style by mutableStateOf(loadStyle())
        private set
    var serif by mutableStateOf(ui.getBoolean("serif", true))
        private set
    /** 翻页区域，照固件：0 左右、1 中下翻页、2 点哪都翻。/ Tap zones after the firmware: 0 sides, 1 lower, 2 anywhere. */
    var turnZone by mutableStateOf(ui.getInt("turn_zone", 0).coerceIn(0, TURN_ZONES.size - 1))
        private set
    /** 音量键翻页：(次数, 方向)，次数变了阅读页就翻一次。/ Volume-key turns: (count, direction); each new count turns once. */
    val turnSignal = mutableStateOf(0 to 0)
    var tomatoView by mutableStateOf<TomatoView>(TomatoView.Shelf)
    var tomatoSeg by mutableStateOf(0)            // 0 书架 / 1 书城 / 0 shelf, 1 store
    var shelf by mutableStateOf(shelfStore.load())
    var status by mutableStateOf(OpStatus())
    var hits by mutableStateOf<List<Hit>>(emptyList())
    var query by mutableStateOf("")
    var configured by mutableStateOf(prefs.configured)

    /** 阅读器打开的文件名（更新时跳过它）。/ File name open in the reader (updates skip it). */
    var openFile: String? = null

    private var job: Job? = null

    private val ops = TomatoOps(api, dirs, shelfStore, { openFile }, ::bookFileChanged) { s -> status = s.copy(op = status.op, running = status.running) }

    /** 有操作在跑（看状态，界面能跟着变）。/ An operation is running (state-backed so the UI follows). */
    val busy get() = status.running
    val pending get() = shelf.count { it.pending }

    /** 书文件被下载覆盖了（更新追加章节）；阅读进度在书库模块里处理。/ A book file was replaced by a download (an
     *  update appends chapters); reading progress is handled by the library. */
    private fun bookFileChanged(file: File, oldSize: Long, newSize: Long) {
        onBookFileChanged?.invoke(file, oldSize, newSize)
    }
    var onBookFileChanged: ((File, Long, Long) -> Unit)? = null

    private fun run(op: Op, block: suspend () -> String) {
        if (busy) return
        status = OpStatus(op = op, running = true, phase = if (op == Op.SEARCH) Phase.SEARCHING else Phase.CHECKING)
        job = viewModelScope.launch {
            try {
                val msg = block()
                status = status.copy(running = false, phase = Phase.DONE, message = msg)
            } catch (e: CancellationException) {
                status = status.copy(running = false, phase = Phase.FAILED, message = "已取消")
            } catch (e: Exception) {
                status = status.copy(running = false, phase = Phase.FAILED, message = e.message ?: "出错了")
            } finally {
                shelf = shelfStore.load()
                refreshLibrary()
            }
        }
    }

    fun cancel() { job?.cancel() }

    fun check() = run(Op.CHECK) { ops.check() }
    fun updateAll() = run(Op.UPDATE) { ops.update() }

    fun search(q: String) {
        val t = q.trim()
        if (t.isEmpty()) return
        // 前台优先：上一次搜索还没回来就取消它。/ Foreground first: cancel an earlier search still running.
        if (busy && status.op == Op.SEARCH) job?.cancel()
        if (busy) return
        query = t
        prefs.remember(t)
        hits = emptyList()
        tomatoView = TomatoView.Results
        run(Op.SEARCH) {
            hits = ops.search(t)
            if (hits.isEmpty()) "没有找到" else ""
        }
    }

    fun download(hit: Hit, coverUrl: String = hit.thumb) = run(Op.DOWNLOAD) { ops.download(hit, coverUrl) }

    /** 一本书的已读百分比，没读过 -1（书库模块接上前一律 -1）。/ Read percentage of a book, -1 when unread. */
    var progressPct: (String) -> Int = { -1 }

    fun owned(bookId: String): TomatoBook? = shelf.firstOrNull { it.bookId == bookId && it.file.isNotEmpty() }

    /** 番茄书架里点一本：已下载就打开阅读（第二阶段接上阅读器）。/ Tap a Tomato shelf book: open it when downloaded. */
    fun openTomatoBook(b: TomatoBook) {
        if (b.file.isEmpty()) {
            status = OpStatus(op = Op.CHECK, phase = Phase.DONE, message = "这本还没下载，点“下载全部更新”")
            return
        }
        openBook?.invoke(File(dirs.books, b.file))
    }
    var openBook: ((File) -> Unit)? = null

    /* ---- WebDAV ---- */

    val webdav = com.treader.tomato.data.WebDav(prefs, dirs, shelfStore, progressStore)
    var davBusy by mutableStateOf(false)
    var davMsg by mutableStateOf("")

    /** 立即同步：先存好正在读的进度。/ Sync now, saving the open book's progress first. */
    fun davSync() {
        if (davBusy) return
        davBusy = true
        davMsg = ""
        reader?.save()
        viewModelScope.launch {
            davMsg = try { webdav.run() } catch (e: Exception) { e.message ?: "同步失败" }
            davBusy = false
            shelf = shelfStore.load()
            refreshLibrary()
        }
    }

    /* ---- 书城 / Bookstore ---- */

    val store = Store(prefs, dirs)
    var storeIndex by mutableStateOf(store.cachedIndex())
    var storeMale by mutableStateOf(true)
    var storeCat by mutableStateOf<RankCat?>(null)
    var storeBooks by mutableStateOf<List<RankBook>>(emptyList())
    var storeBusy by mutableStateOf(false)
    var storeMsg by mutableStateOf("")

    private fun storeJob(block: suspend () -> Unit) {
        if (storeBusy) return
        storeBusy = true
        storeMsg = ""
        viewModelScope.launch {
            try { block() } catch (e: Exception) { storeMsg = e.message ?: "出错了" } finally { storeBusy = false }
        }
    }

    /** 第一次进书城、没缓存时取一次分类表。/ Fetch the index once when the store opens without a cache. */
    fun storeEnter() { if (storeIndex == null) storeRefresh() }

    fun storeRefresh() = storeJob {
        storeIndex = store.refreshIndex()
        storeCat?.let { c -> storeBooks = store.refreshBooks(c) }
    }

    fun openCat(c: RankCat) {
        storeCat = c
        storeBooks = store.cachedBooks(c) ?: emptyList()
        if (storeBooks.isEmpty()) storeJob { storeBooks = store.refreshBooks(c) }
    }

    fun storeHit(b: RankBook): Hit = Hit(b.bookId, b.title, b.author, storeCat?.name.orEmpty(), 0, 0, false, b.intro, b.cover,
        reads = b.reads, channel = listOfNotNull(if (storeCat?.male != false) "男频" else "女频", storeCat?.name).joinToString(" · "))

    /* ---- 书库和阅读 / Library and reading ---- */

    fun refreshLibrary() {
        viewModelScope.launch {
            books = withContext(Dispatchers.IO) { library.scan() }
        }
    }

    fun open(f: java.io.File) {
        if (opening) return
        opening = true
        libMessage = ""
        viewModelScope.launch {
            try {
                val b = withContext(Dispatchers.IO) { library.scan().firstOrNull { it.file == f } } ?: throw IllegalStateException("找不到这本书")
                val src: BookSource = withContext(Dispatchers.IO) { if (b.epub) EpubBook.open(b.file) else TxtBook.open(b.file) }
                if (src.chapterCount == 0) throw IllegalStateException("这本书没有内容")
                val start = b.progress
                reader = ReaderState(b, src, progressStore, start).also {
                    if (start == null) it.px = ui.getInt("px", ReaderState.PX_DEFAULT)
                }
                openFile = b.name
                markSeen(b.tomato?.file)
            } catch (e: Exception) {
                libMessage = "打不开：${e.message ?: e.javaClass.simpleName}"
            } finally {
                opening = false
            }
        }
    }

    // 打开（或退出）番茄书算看过：书库和番茄书架上的“+N 章”消失。/ Opening (or leaving) a Tomato book counts as seen.
    private suspend fun markSeen(file: String?) {
        if (file.isNullOrEmpty()) return
        if (!withContext(Dispatchers.IO) { shelfStore.markSeen(file) }) return
        shelf = withContext(Dispatchers.IO) { shelfStore.load() }
        books = withContext(Dispatchers.IO) { library.scan() }
    }

    fun closeReader() {
        // 阅读时后台可能刚下完新章节，退出时再记一次。/ New chapters may have landed while reading; mark again on leaving.
        reader?.book?.tomato?.file?.let { f -> viewModelScope.launch { markSeen(f) } }
        reader?.save()
        reader = null
        openFile = null
        tab = Tab.LIBRARY
        refreshLibrary()
    }

    fun turn(dir: Int) { turnSignal.value = (turnSignal.value.first + 1) to dir }

    fun setDefaultPx(px: Int) = ui.edit().putInt("px", px).apply()

    private fun loadStyle() = Style(ui.getInt("line", 1), ui.getInt("para", 1), ui.getInt("margin", 1),
        ui.getBoolean("indent", false), ui.getBoolean("underline", false))

    fun changeStyle(s: Style) {
        style = s
        ui.edit().putInt("line", s.line).putInt("para", s.para).putInt("margin", s.margin)
            .putBoolean("indent", s.indent).putBoolean("underline", s.underline).apply()
    }

    fun changeSerif(on: Boolean) { serif = on; ui.edit().putBoolean("serif", on).apply() }

    fun changeTurnZone(z: Int) { turnZone = z % TURN_ZONES.size; ui.edit().putInt("turn_zone", turnZone).apply() }

    fun importBooks(uris: List<Uri>) {
        viewModelScope.launch {
            val msgs = withContext(Dispatchers.IO) { uris.map { library.import(getApplication(), it) } }
            libMessage = if (msgs.size == 1) msgs[0] else "导入了 ${msgs.count { it.startsWith("已导入") }} 本"
            refreshLibrary()
        }
    }

    fun delete(list: List<LibBook>) {
        viewModelScope.launch {
            withContext(Dispatchers.IO) { list.forEach { library.delete(it) } }
            shelf = shelfStore.load()
            refreshLibrary()
        }
    }

    fun clearProgress(list: List<LibBook>) {
        viewModelScope.launch {
            withContext(Dispatchers.IO) { list.forEach { library.clearProgress(it) } }
            refreshLibrary()
        }
    }

    fun saveServer(url: String?, password: String?) {
        url?.let { prefs.server = it }
        password?.let { if (it.isNotBlank()) prefs.password = it }
        configured = prefs.configured
    }

    /** 测试连接：问一次 /api/status。/ Connection test: one /api/status request. */
    suspend fun testServer(): String = try {
        val s = api.get("/api/status")
        val v = s.optString("version")
        if (v.isNotEmpty()) "正常 · 下载器 $v" else "正常"
    } catch (e: Exception) {
        e.message ?: "失败"
    }

    // 放在最后：上面的属性都初始化好了再接线。/ Last, so every property above is initialized before wiring.
    init {
        TxtBook.gbkTable = app.assets.open("gbk.bin").use { input ->
            val b = input.readBytes()
            CharArray(b.size / 2) { i -> ((b[2 * i].toInt() and 0xFF) or ((b[2 * i + 1].toInt() and 0xFF) shl 8)).toChar() }
        }
        onBookFileChanged = { file, oldSize, newSize -> progressStore.resize(ProgressStore.key(file.name), oldSize, newSize) }
        progressPct = { file -> books.firstOrNull { it.name == file }?.pct ?: -1 }
        openBook = { f -> open(f) }
        refreshLibrary()
    }
}

/** 翻页区域的名称和说明，顺序同固件 app_turn_zone_t。/ Tap-zone names and notes, in the firmware's app_turn_zone_t order. */
val TURN_ZONES = listOf(
    "左右" to "点左边上一页，右边下一页，中间开工具条",
    "中下翻页" to "点上面开工具条；下面左边一条上一页，其余下一页",
    "点哪都翻" to "点上面开工具条，其余都是下一页；上一页用右滑或音量键",
)
