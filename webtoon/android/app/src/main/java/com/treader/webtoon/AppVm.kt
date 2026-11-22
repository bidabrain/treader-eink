package com.treader.webtoon

import android.app.Application
import android.graphics.Bitmap
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import coil.ImageLoader
import com.treader.webtoon.data.Api
import com.treader.webtoon.data.Card
import com.treader.webtoon.data.Cbz
import com.treader.webtoon.data.Episode
import com.treader.webtoon.data.Mark
import com.treader.webtoon.data.Plan
import com.treader.webtoon.data.Prefs
import com.treader.webtoon.data.Progress
import com.treader.webtoon.data.Series
import com.treader.webtoon.data.Shelf
import com.treader.webtoon.data.Start
import com.treader.webtoon.data.StoreList
import com.treader.webtoon.data.Title
import com.treader.webtoon.data.UpdateStatus
import com.treader.webtoon.data.Updater
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import java.io.File

enum class Tab { SHELF, STORE, SETTINGS }

/** 发现页的四个分段，和服务器的 seg 一一对应。/ The store's four segments, one per server seg. */
val STORE_SEGS = listOf("rank" to "排行", "genre" to "类型", "day" to "星期", "done" to "完结")

/** 打开的作品详情。/ An open title detail. */
data class Detail(val id: String, val lang: String, val name: String, val title: Title? = null, val error: String = "")

/** 阅读中的一话。/ The episode being read. */
class Reading(val series: Series, val index: Int, val cbz: Cbz, startPage: Int) {
    val episode: Episode get() = series.episodes[index]
    var page by mutableStateOf(startPage.coerceIn(0, (cbz.count - 1).coerceAtLeast(0)))
    /** 翻过最后一页后的“本话完”页。/ The "end of episode" screen after the last page. */
    var atEnd by mutableStateOf(false)
    val next: Episode? get() = series.episodes.getOrNull(index + 1)
}

/**
 * 全局状态和动作。联网只在点的时候：进发现页的分段、搜索、打开详情、检查更新。检查更新同一时间只跑一个。
 * App-wide state and actions. Network use only on a tap: store segments, search, opening details and update checks.
 * One update check at a time.
 */
class AppVm(app: Application) : AndroidViewModel(app) {
    val prefs = Prefs(app)
    val api = Api(prefs)
    val shelfStore = Shelf(File(app.filesDir, "comics"))
    val progress = Progress(File(app.filesDir, "progress.json"))
    private val updater = Updater(api, shelfStore)

    // 封面带令牌；服务器出的封面不会变，忽略缓存头一直用磁盘缓存。/ Covers carry the token; server covers never
    // change, so ignore cache headers and keep them on disk.
    val images: ImageLoader = ImageLoader.Builder(app)
        .okHttpClient(OkHttpClient.Builder().addInterceptor { chain ->
            chain.proceed(chain.request().newBuilder().header(Api.TOKEN_HEADER, prefs.token).build())
        }.build())
        .respectCacheHeaders(false)
        .crossfade(false)
        .build()

    var tab by mutableStateOf(Tab.SHELF)
    var configured by mutableStateOf(prefs.configured)

    /* ---- 我的漫画 / Shelf ---- */

    var series by mutableStateOf<List<Series>>(emptyList())
    var shelfSeg by mutableStateOf(0)          // 0 全部 / 1 已订阅 / 0 all, 1 subscribed
    var shelfPage by mutableStateOf(0)
    var openSeries by mutableStateOf<File?>(null)
    var seriesPage by mutableStateOf(0)
    var update by mutableStateOf(UpdateStatus())
    private var updateJob: Job? = null

    fun refreshShelf() {
        viewModelScope.launch { series = withContext(Dispatchers.IO) { shelfStore.scan() } }
    }

    fun unread(s: Series): Int = s.episodes.count { progress.get(it.key) == null }

    fun currentSeries(): Series? = openSeries?.let { d -> series.firstOrNull { it.dir == d } }

    /** 检查更新；onlyId 只查这一部（刚订阅时）。/ Check for updates; onlyId checks just that title (right after
     *  subscribing). */
    fun checkUpdates(onlyId: String? = null) {
        if (update.running) return
        update = UpdateStatus(running = true, line = "正在检查…")
        updateJob = viewModelScope.launch {
            val msg = try {
                updater.run(onlyId, prefs.perCheck) { s ->
                    // 每下完一话就刷新我的漫画，新话马上能看。/ Refresh the shelf after each episode so it is readable at once.
                    if (s.done != update.done) refreshShelf()
                    update = s
                }
            } catch (e: CancellationException) {
                "已取消"
            } catch (e: Exception) {
                e.message ?: "出错了"
            }
            update = UpdateStatus(running = false, message = msg)
            series = withContext(Dispatchers.IO) { shelfStore.scan() }
        }
    }

    fun cancelUpdate() { updateJob?.cancel() }

    fun clearUpdateMessage() { if (!update.running) update = UpdateStatus() }

    fun deleteSeries(s: Series) {
        viewModelScope.launch {
            withContext(Dispatchers.IO) {
                progress.forget(s.dir.name + "/")
                shelfStore.delete(s)
            }
            openSeries = null
            refreshShelf()
        }
    }

    /* ---- 发现页 / Store ---- */

    var storeSeg by mutableStateOf(0)
    var storeKey by mutableStateOf("")
    var storeList by mutableStateOf<StoreList?>(null)
    var storeBusy by mutableStateOf(false)
    var storeMsg by mutableStateOf("")
    var storePage by mutableStateOf(0)
    var query by mutableStateOf("")
    var searchResults by mutableStateOf<List<Card>?>(null)
    private val storeCache = HashMap<String, StoreList>()
    private var storeJob: Job? = null

    private var storeGen = 0

    // 新请求顶掉旧的（前台优先）；旧请求收尾时不动新请求的状态。/ A new request replaces the old one (foreground
    // first); the old one's cleanup leaves the new request's state alone.
    private fun storeLaunch(block: suspend () -> Unit) {
        storeJob?.cancel()
        val gen = ++storeGen
        storeBusy = true
        storeMsg = ""
        storeJob = viewModelScope.launch {
            try {
                block()
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                if (gen == storeGen) storeMsg = e.message ?: "出错了"
            } finally {
                if (gen == storeGen) storeBusy = false
            }
        }
    }

    /** 进发现页、换分段或二级键时取列表（同一次运行里有缓存）。/ Fetch a list when the store opens or a segment / key
     *  changes (cached for this run). */
    fun loadStore(seg: Int = storeSeg, key: String = storeKey, force: Boolean = false) {
        if (!configured) return
        storeSeg = seg
        storeKey = key
        storePage = 0
        val cacheKey = "${prefs.lang}|${prefs.simplified}|${STORE_SEGS[seg].first}|$key"
        if (!force) storeCache[cacheKey]?.let { storeList = it; return }
        storeLaunch {
            val r = api.store(STORE_SEGS[seg].first, key)
            storeCache[cacheKey] = r
            // 服务器把空 key 换成了默认的那个，也记一份。/ The server replaced an empty key with the default; cache it too.
            storeCache["${prefs.lang}|${prefs.simplified}|${STORE_SEGS[seg].first}|${r.key}"] = r
            if (storeSeg == seg) { storeList = r; storeKey = r.key }
        }
    }

    fun storeEnter() { if (storeList == null && !storeBusy) loadStore() }

    fun search(q: String) {
        val t = q.trim()
        if (t.isEmpty()) { searchResults = null; return }
        query = t
        storePage = 0
        searchResults = emptyList()
        storeLaunch { searchResults = api.search(t) }
    }

    fun closeSearch() { searchResults = null; query = ""; storePage = 0; storeMsg = "" }

    /** 语言或简繁变了：清掉发现页缓存。/ Language or script changed: drop the store cache. */
    fun storeReset() {
        storeCache.clear()
        storeList = null
        storeKey = ""
        searchResults = null
    }

    /* ---- 详情 / Detail ---- */

    var detail by mutableStateOf<Detail?>(null)
    var detailBusy by mutableStateOf(false)

    fun openDetail(id: String, name: String, lang: String = prefs.lang) {
        detail = Detail(id, lang, name)
        detailBusy = true
        viewModelScope.launch {
            val d = try { Detail(id, lang, name, api.title(id, lang)) } catch (e: Exception) { Detail(id, lang, name, error = e.message ?: "出错了") }
            if (detail?.id == id) detail = d
            detailBusy = false
        }
    }

    fun subscribed(id: String): Series? = series.firstOrNull { it.mark?.id == id }

    fun subscribe(d: Detail, start: Start) {
        val t = d.title ?: return
        val mark = Mark(t.id.ifEmpty { d.id }, d.lang, t.title.ifEmpty { d.name }, t.author, Plan.startLast(t.episodes, start),
            System.currentTimeMillis() / 1000)
        viewModelScope.launch {
            withContext(Dispatchers.IO) { shelfStore.subscribe(mark) }
            series = withContext(Dispatchers.IO) { shelfStore.scan() }
            // 订阅后马上下这一部（只追新的就没有要下的）。/ Download this title right away (nothing for new-only).
            if (start != Start.NEW_ONLY) checkUpdates(mark.id)
        }
    }

    fun unsubscribe(s: Series, deleteFiles: Boolean) {
        viewModelScope.launch {
            withContext(Dispatchers.IO) {
                if (deleteFiles) progress.forget(s.dir.name + "/")
                shelfStore.unsubscribe(s, deleteFiles)
            }
            series = withContext(Dispatchers.IO) { shelfStore.scan() }
            if (openSeries != null && series.none { it.dir == openSeries }) openSeries = null
        }
    }

    /* ---- 阅读 / Reading ---- */

    var reading by mutableStateOf<Reading?>(null)
    var readError by mutableStateOf("")
    /** 音量键翻页：(次数, 方向)。/ Volume-key turns: (count, direction). */
    val turnSignal = mutableStateOf(0 to 0)

    // 正在打开的一话；连点上一话 / 下一话或关掉阅读时取消旧的，免得晚到的结果盖掉新的、或者漏关文件。
    // The episode being opened; cancelled by a newer open or by closing the reader, so a late result cannot replace a
    // newer one or leak its file.
    private var readJob: Job? = null

    fun read(s: Series, index: Int) {
        val ep = s.episodes.getOrNull(index) ?: return
        readError = ""
        readJob?.cancel()
        readJob = viewModelScope.launch {
            var cbz: Cbz? = null
            try {
                cbz = withContext(Dispatchers.IO) { Cbz(ep.file) }
                if (cbz.count == 0) throw IllegalStateException("这一话没有图片")
                val p = progress.get(ep.key)
                // 看完的从头再看。/ A finished episode starts over.
                val start = if (p == null || p.finished) 0 else p.page
                reading?.cbz?.close()
                reading = Reading(s, index, cbz, start)
                cbz = null                  // 交给 reading 了 / Now owned by reading
                saveProgress()
            } catch (e: CancellationException) {
                throw e
            } catch (e: Exception) {
                readError = "打不开：${e.message ?: e.javaClass.simpleName}"
            } finally {
                cbz?.close()
            }
        }
    }

    suspend fun pageBitmap(r: Reading, i: Int): Bitmap? = withContext(Dispatchers.IO) { runCatching { r.cbz.page(i) }.getOrNull() }

    fun saveProgress() {
        val r = reading ?: return
        progress.set(r.episode.key, r.page, r.cbz.count)
    }

    fun turn(dir: Int) { turnSignal.value = (turnSignal.value.first + 1) to dir }

    /** 离开阅读或 App 退到后台：进度马上写（在后台线程）。/ Leaving the reader or the app going to the background: write
     *  progress now (on a background thread). */
    fun flushProgress() {
        saveProgress()
        progress.flush()
    }

    fun closeReader() {
        readJob?.cancel()
        val r = reading ?: return
        flushProgress()
        r.cbz.close()
        reading = null
        openSeries = r.series.dir
        refreshShelf()
    }

    fun readNext() {
        val r = reading ?: return
        if (r.next == null) return
        saveProgress()
        read(r.series, r.index + 1)
    }

    /* ---- 设置 / Settings ---- */

    fun saveServer(url: String?, token: String?) {
        url?.let { prefs.url = it }
        token?.let { if (it.isNotBlank()) prefs.token = it }
        configured = prefs.configured
        storeReset()
    }

    /** 测试连接：取一次排行榜。/ Connection test: fetch the ranking once. */
    suspend fun testConnection(): String = try {
        val r = api.store("rank", "")
        "正常 · ${r.items.size} 部"
    } catch (e: Exception) {
        e.message ?: "失败"
    }

    init {
        refreshShelf()
    }

    override fun onCleared() {
        progress.flush()
        reading?.cbz?.close()
        super.onCleared()
    }
}
