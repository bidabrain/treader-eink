package com.treader.rednote

import android.app.Application
import androidx.compose.foundation.lazy.staggeredgrid.LazyStaggeredGridState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import coil.ImageLoader
import com.treader.rednote.data.Api
import com.treader.rednote.data.Item
import com.treader.rednote.data.Note
import com.treader.rednote.data.Prefs
import com.treader.rednote.data.Repo
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import okhttp3.OkHttpClient

enum class Seg { FOLLOW, REC }

sealed interface Screen {
    data object List : Screen
    data class Images(val note: Note, val index: Int) : Screen
    data class Text(val note: Note, val comments: Boolean) : Screen
    data object Settings : Screen
}

/**
 * 页面状态和动作，照固件 app_tr_xhs.c：只在点“刷新”、切到没缓存的推荐、打开没缓存的笔记时联网；推荐的“刷新推荐”
 * 才让服务器去读小红书。新请求顶掉本页的旧请求（前台优先）。
 * Page state and actions, after the firmware's app_tr_xhs.c: network only on Refresh, switching to uncached
 * recommendations or opening an uncached note; only "Refresh recommendations" makes the server read Xiaohongshu. A new
 * request replaces the old one (foreground first).
 */
class AppVm(app: Application) : AndroidViewModel(app) {
    val prefs = Prefs(app)
    val api = Api(prefs)
    private val repo = Repo(app, api)

    // 图片带令牌；服务器出的图不会变，忽略缓存头一直用磁盘缓存。/ Images carry the token; server images never
    // change, so ignore cache headers and keep them on disk.
    val images: ImageLoader = ImageLoader.Builder(app)
        .okHttpClient(OkHttpClient.Builder().addInterceptor { chain ->
            chain.proceed(chain.request().newBuilder().header(Api.TOKEN_HEADER, prefs.token).build())
        }.build())
        .respectCacheHeaders(false)
        .crossfade(false)
        .build()

    var screen by mutableStateOf<Screen>(Screen.List)
    var seg by mutableStateOf(Seg.FOLLOW)
    var feed by mutableStateOf(repo.cachedFeed())
    var rec by mutableStateOf(repo.cachedRec())
    var page by mutableStateOf(0)
    var msg by mutableStateOf("")
    var loading by mutableStateOf(false)
    var waiting by mutableStateOf<String?>(null)     // 等服务器现拉的推荐笔记 / A recommendation being fetched
    var waitError by mutableStateOf("")
    var configured by mutableStateOf(prefs.configured)
    var moreLoading by mutableStateOf(false)         // 等服务器拉更多评论 / Waiting for more comments
    var moreError by mutableStateOf("")

    private var job: Job? = null

    /** 每个分段自己的滚动位置，放在这里所以看完笔记回来还在原处。/ Each segment's own scroll position; kept here so
     *  coming back from a note lands where you were. */
    val scroll: Map<Seg, LazyStaggeredGridState> = Seg.entries.associateWith { LazyStaggeredGridState() }
    private var lastSegTap = 0L

    val items: List<Item> get() = if (seg == Seg.FOLLOW) feed else rec?.items.orEmpty()

    /** 点分段：换分段；双击当前分段回到最上面（最新的）。/ Tap a segment: switch; a double tap on the current one
     *  scrolls back to the top (newest). */
    fun tapSeg(s: Seg) {
        if (s != seg) { showSeg(s); return }
        val now = System.currentTimeMillis()
        if (now - lastSegTap < 500) { scroll.getValue(s).requestScrollToItem(0); lastSegTap = 0 } else lastSegTap = now
    }

    fun showSeg(s: Seg) {
        if (s == seg) return
        seg = s
        page = 0
        msg = ""
        // 推荐没缓存：先取服务器上已有的列表（不让服务器去读小红书）。/ No cached recommendations: fetch the
        // server's existing list without making it read Xiaohongshu.
        if (s == Seg.REC && rec == null && configured) launchJob { repo.refreshRec(false).also { rec = it } }
    }

    fun refresh() {
        // 刷新后回到最上面看新的。/ After a refresh, go back to the top for the new items.
        val which = seg
        launchJob {
            if (which == Seg.FOLLOW) feed = repo.refreshFeed() else rec = repo.refreshRec(true)
            page = 0
            scroll.getValue(which).requestScrollToItem(0)
        }
    }

    private fun launchJob(block: suspend () -> Unit) {
        job?.cancel()
        msg = ""
        loading = true
        job = viewModelScope.launch {
            try {
                block()
            } catch (e: Exception) {
                if (e !is kotlinx.coroutines.CancellationException) msg = e.message ?: "出错了"
            } finally {
                loading = false
            }
        }
    }

    fun open(item: Item) {
        msg = ""
        if (seg == Seg.FOLLOW && !item.seen) {
            feed = feed.map { if (it.id == item.id) it.copy(seen = true) else it }
            viewModelScope.launch { repo.markSeen(item.id) }
        }
        repo.cachedNote(item.id)?.let { enter(it); return }
        // 没缓存：关注的笔记服务器上已经有，很快；推荐的要现拉，弹等待框。
        // Not cached: following notes already sit on the server (fast); recommendations are fetched behind a dialog.
        waiting = item.id
        waitError = ""
        job?.cancel()
        job = viewModelScope.launch {
            try {
                val note = repo.fetchNote(item.id, seg == Seg.REC)
                if (waiting == item.id) { waiting = null; enter(note) }
            } catch (e: Exception) {
                if (e !is kotlinx.coroutines.CancellationException && waiting == item.id) waitError = e.message ?: "出错了"
            }
        }
    }

    /** 加载更多评论：弹等待框，拉完留在评论页。/ Load more comments behind a dialog, then stay on the comments. */
    fun loadMoreComments(note: Note) {
        if (!note.more || moreLoading) return
        moreLoading = true
        moreError = ""
        job?.cancel()
        job = viewModelScope.launch {
            try {
                val n = repo.loadMoreComments(note.id)
                if (moreLoading) {
                    moreLoading = false
                    (screen as? Screen.Text)?.let { if (it.note.id == n.id) screen = Screen.Text(n, true) }
                }
            } catch (e: Exception) {
                if (e !is kotlinx.coroutines.CancellationException && moreLoading) moreError = e.message ?: "出错了"
            }
        }
    }

    /** 关掉等待框；还在拉就取消（服务器那边照样会拉完存好）。/ Close the dialog, cancelling if still waiting (the
     *  server still finishes and keeps the comments). */
    fun closeMore() {
        if (moreLoading && moreError.isEmpty()) job?.cancel()
        moreLoading = false
        moreError = ""
    }

    fun cancelWait() {
        job?.cancel()
        waiting = null
        waitError = ""
    }

    private fun enter(note: Note) {
        screen = if (note.images > 0) Screen.Images(note, 0) else Screen.Text(note, false)
    }

    fun noteCached(id: String) = repo.noteCached(id)

    fun backToList() {
        if (moreLoading) closeMore()
        screen = Screen.List
    }

    fun saveSettings(url: String?, token: String?) {
        url?.let { prefs.url = it }
        token?.let { if (it.isNotBlank()) prefs.token = it }
        configured = prefs.configured
    }

    suspend fun testConnection(): String = try {
        val limit = prefs.listLimit
        val n = api.feed(limit).optJSONArray("notes")?.length() ?: 0
        if (n >= limit) "正常 · 关注 $n+ 篇" else "正常 · 关注 $n 篇"
    } catch (e: Exception) {
        e.message ?: "失败"
    }
}
