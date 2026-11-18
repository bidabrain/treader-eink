package com.treader.weibo

import android.app.Application
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import coil.ImageLoader
import com.treader.weibo.data.Api
import com.treader.weibo.data.Comment
import com.treader.weibo.data.Feed
import com.treader.weibo.data.Item
import com.treader.weibo.data.Post
import com.treader.weibo.data.Prefs
import com.treader.weibo.data.Repo
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import okhttp3.OkHttpClient

enum class Seg { FOLLOW, HOT }

sealed interface Screen {
    data object List : Screen
    data class Text(val post: Post, val comments: Boolean) : Screen
    data class Images(val post: Post, val index: Int) : Screen
    data object Settings : Screen
}

/**
 * 页面状态和动作，照固件 app_tr_weibo.c：只在点“刷新”、切到没缓存的分段、打开没缓存的微博或评论时联网。服务器自己
 * 定时去微博拉，手机只取服务器上已有的；只有评论第一次打开时服务器才去微博取一次。新请求顶掉本页的旧请求。
 * Page state and actions, after the firmware's app_tr_weibo.c: network only on Refresh, switching to an uncached
 * segment, or opening an uncached post or comments. The server fetches Weibo on its own schedule and the phone takes
 * only what the server has; only a post's first comments request makes the server ask Weibo. A new request replaces
 * the old one.
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
    var follow by mutableStateOf(repo.cached(false))
    var hot by mutableStateOf(repo.cached(true))
    var msg by mutableStateOf("")
    var loading by mutableStateOf(false)
    var waiting by mutableStateOf<String?>(null)     // 正在取全文的微博 / The post being fetched
    var waitError by mutableStateOf("")
    var configured by mutableStateOf(prefs.configured)

    // 当前打开的微博的评论：null 表示还没取到。/ Comments of the open post; null until fetched.
    var comments by mutableStateOf<List<Comment>?>(null)
    var commentsLoading by mutableStateOf(false)
    var commentsError by mutableStateOf("")

    private var job: Job? = null
    private var commentsJob: Job? = null

    val feed: Feed? get() = if (seg == Seg.FOLLOW) follow else hot
    val items: List<Item> get() = feed?.items.orEmpty()

    fun showSeg(s: Seg) {
        if (s == seg) return
        seg = s
        msg = ""
        // 这一段还没缓存：从服务器取一次。/ No cache for this segment yet: fetch once from the server.
        if (feed == null && configured) refresh()
    }

    fun refresh() {
        val wantHot = seg == Seg.HOT
        launchJob { repo.refresh(wantHot).also { if (wantHot) hot = it else follow = it } }
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
        if (seg == Seg.FOLLOW && !item.seen) {
            follow = follow?.let { f -> f.copy(items = f.items.map { if (it.id == item.id) it.copy(seen = true) else it }) }
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

    /** 切到评论；没缓存就去取。/ Switch to comments, fetching them when not cached. */
    fun showComments(post: Post) {
        screen = Screen.Text(post, true)
        if (comments != null || commentsLoading) return
        commentsLoading = true
        commentsError = ""
        commentsJob = viewModelScope.launch {
            try {
                comments = repo.fetchComments(post.id)
            } catch (e: Exception) {
                if (e !is CancellationException) commentsError = e.message ?: "评论没取到"
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
    }

    suspend fun testConnection(): String = try {
        val n = api.feed(false, 100).optJSONArray("posts")?.length() ?: 0
        if (n >= 100) "正常 · 关注 $n+ 条" else "正常 · 关注 $n 条"
    } catch (e: Exception) {
        e.message ?: "失败"
    }
}
