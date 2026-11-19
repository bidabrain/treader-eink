package com.treader.x

import android.app.Application
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import coil.ImageLoader
import com.treader.x.data.Api
import com.treader.x.data.Comment
import com.treader.x.data.Feed
import com.treader.x.data.Item
import com.treader.x.data.Post
import com.treader.x.data.Prefs
import com.treader.x.data.Repo
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import okhttp3.OkHttpClient

/** 为你推荐在前（默认），正在关注在后。/ For You first (the default), then Following. */
enum class Seg { FORYOU, FOLLOWING }

sealed interface Screen {
    data object List : Screen
    data class Text(val post: Post, val comments: Boolean) : Screen
    data class Images(val post: Post, val index: Int) : Screen
    data object Settings : Screen
}

/**
 * 页面状态和动作，照微博 App：只在点“刷新”、切到没缓存的分段、打开没缓存的推文或回复时联网，另外打开 App 和刷新时
 * 探测一次服务器状态。服务器自己定时去 X 拉，手机只取服务器上已有的；只有回复第一次打开时服务器才去 X 取一次。
 * 新请求顶掉本页的旧请求。“正在关注”总开关关着时那一段变灰、点不了。
 * Page state and actions, after the Weibo app: network only on Refresh, switching to an uncached segment, or opening
 * an uncached post or replies, plus one status probe on app open and on refresh. The server fetches X on its own
 * schedule and the phone takes only what the server has; only a post's first replies request makes the server ask X.
 * A new request replaces the old one. With the Following master switch off that segment is grey and cannot be picked.
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
    var seg by mutableStateOf(Seg.FORYOU)
    var foryou by mutableStateOf(repo.cached(false))
    var following by mutableStateOf(repo.cached(true))
    /** 服务器的“正在关注”总开关，上次知道的值跟缓存一起存着。/ The server's Following master switch; the last
     *  known value is kept with the cache. */
    var followingOn by mutableStateOf(repo.followingOn())
        private set
    var msg by mutableStateOf("")
    var loading by mutableStateOf(false)
    var waiting by mutableStateOf<String?>(null)     // 正在取全文的推文 / The post being fetched
    var waitError by mutableStateOf("")
    var configured by mutableStateOf(prefs.configured)

    // 当前打开的推文的回复：null 表示还没取到。/ Replies of the open post; null until fetched.
    var comments by mutableStateOf<List<Comment>?>(null)
    var commentsLoading by mutableStateOf(false)
    var commentsError by mutableStateOf("")

    private var job: Job? = null
    private var commentsJob: Job? = null

    init {
        probe()
    }

    /** 记下总开关；关了就离开“正在关注”。/ Record the master switch; leave Following when it is off. */
    private fun applyFollowingOn(on: Boolean?) {
        if (on == null) return
        followingOn = on
        if (!on && seg == Seg.FOLLOWING) seg = Seg.FORYOU
    }

    /** 探测服务器状态；失败不打扰，沿用上次的值。/ Probe the server status; failures stay quiet and keep the last
     *  value. */
    private fun probe() {
        if (!configured) return
        viewModelScope.launch {
            try {
                applyFollowingOn(repo.probeStatus())
            } catch (_: Exception) {
            }
        }
    }

    /** 每个分段自己的滚动位置，放在这里所以看完推文回来还在原处。/ Each segment's own scroll position; kept here so
     *  coming back from a post lands where you were. */
    val scroll: Map<Seg, LazyListState> = Seg.entries.associateWith { LazyListState() }
    private var lastSegTap = 0L

    /** 点分段：换分段；双击当前分段回到最上面（最新的）。/ Tap a segment: switch; a double tap on the current one
     *  scrolls back to the top (newest). */
    fun tapSeg(s: Seg) {
        if (s == Seg.FOLLOWING && !followingOn) return
        if (s != seg) { showSeg(s); return }
        val now = System.currentTimeMillis()
        if (now - lastSegTap < 500) { scroll.getValue(s).requestScrollToItem(0); lastSegTap = 0 } else lastSegTap = now
    }

    val feed: Feed? get() = if (seg == Seg.FOLLOWING) following else foryou
    val items: List<Item> get() = feed?.items.orEmpty()

    fun showSeg(s: Seg) {
        if (s == seg || (s == Seg.FOLLOWING && !followingOn)) return
        seg = s
        msg = ""
        // 这一段还没缓存：从服务器取一次。/ No cache for this segment yet: fetch once from the server.
        if (feed == null && configured) refresh()
    }

    fun refresh() {
        val wantFollowing = seg == Seg.FOLLOWING
        probe()
        // 刷新后回到最上面看新的。/ After a refresh, go back to the top for the new posts.
        launchJob {
            val r = repo.refresh(wantFollowing)
            applyFollowingOn(r.followingOn)
            val f = r.feed
            if (f == null) {
                msg = "正在关注的总开关关着，先在 x 面板的设置里打开"
                return@launchJob
            }
            if (wantFollowing) following = f else foryou = f
            scroll.getValue(if (wantFollowing) Seg.FOLLOWING else Seg.FORYOU).requestScrollToItem(0)
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
                if (e !is CancellationException) msg = e.message ?: "出错了"
            } finally {
                loading = false
            }
        }
    }

    /** 打开一条；image 不为空时直接从第 image 张原图看大图。/ Open a post; with image set, go straight to that
     *  source image's full view. */
    fun open(item: Item, image: Int? = null) {
        msg = ""
        if (seg == Seg.FOLLOWING && !item.seen) {
            following = following?.let { f -> f.copy(items = f.items.map { if (it.id == item.id) it.copy(seen = true) else it }) }
            viewModelScope.launch { repo.markSeen(item.id) }
        }
        repo.cachedPost(item.id)?.let { enter(it, image); return }
        waiting = item.id
        waitError = ""
        job?.cancel()
        job = viewModelScope.launch {
            try {
                val post = repo.fetchPost(item.id)
                if (waiting == item.id) { waiting = null; enter(post, image) }
            } catch (e: Exception) {
                if (e !is CancellationException && waiting == item.id) waitError = e.message ?: "出错了"
            }
        }
    }

    fun cancelWait() {
        job?.cancel()
        waiting = null
        waitError = ""
    }

    private fun enter(post: Post, image: Int? = null) {
        commentsJob?.cancel()
        comments = repo.cachedComments(post.id)
        commentsLoading = false
        commentsError = ""
        screen = if (image != null && post.images > 0) Screen.Images(post, pageOf(post, image)) else Screen.Text(post, false)
    }

    /** 第 k 张原图的第一页（旧服务器没有 starts 就从第一页开始）。/ First page of source image k (the first page
     *  when an older server sent no starts). */
    fun pageOf(post: Post, k: Int): Int = post.starts.getOrNull(k)?.coerceIn(0, (post.images - 1).coerceAtLeast(0)) ?: 0

    /** 切到回复；没缓存就去取。/ Switch to replies, fetching them when not cached. */
    fun showComments(post: Post) {
        screen = Screen.Text(post, true)
        if (comments != null || commentsLoading) return
        commentsLoading = true
        commentsError = ""
        commentsJob = viewModelScope.launch {
            try {
                comments = repo.fetchComments(post.id)
            } catch (e: Exception) {
                if (e !is CancellationException) commentsError = e.message ?: "回复没取到"
            } finally {
                commentsLoading = false
            }
        }
    }

    fun backToList() {
        commentsJob?.cancel()
        screen = Screen.List
    }

    fun saveSettings(url: String?, token: String?) {
        url?.let { prefs.url = it }
        token?.let { if (it.isNotBlank()) prefs.token = it }
        configured = prefs.configured
        probe()
    }

    suspend fun testConnection(): String = try {
        val limit = prefs.listLimit
        val n = api.feed(false, limit).optJSONArray("posts")?.length() ?: 0
        applyFollowingOn(runCatching { repo.probeStatus() }.getOrNull())
        if (n >= limit) "正常 · 推荐 $n+ 条" else "正常 · 推荐 $n 条"
    } catch (e: Exception) {
        e.message ?: "失败"
    }
}
