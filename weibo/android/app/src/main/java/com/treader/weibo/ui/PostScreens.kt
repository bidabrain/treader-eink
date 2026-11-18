package com.treader.weibo.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import coil.compose.AsyncImage
import coil.request.ImageRequest
import com.treader.weibo.AppVm
import com.treader.weibo.Screen
import com.treader.weibo.data.Post
import java.text.SimpleDateFormat
import java.util.Calendar
import java.util.Date
import java.util.Locale

/** 数字缩成“1.2万”。/ Shorten counts to "1.2万". */
fun fmtCount(n: Int): String = when {
    n >= 100_000_000 -> String.format(Locale.CHINA, "%.1f亿", n / 100_000_000.0)
    n >= 10_000 -> String.format(Locale.CHINA, "%.1f万", n / 10_000.0)
    else -> n.toString()
}

/** 今天的写时分，别的写月日；long 时都写“月-日 时:分”。/ Today shows hours:minutes, other days month-day; long
 *  always shows "MM-dd HH:mm". */
fun fmtTime(t: Double, long: Boolean = false): String {
    if (t < 1e9) return ""
    val date = Date(((if (t > 1e11) t / 1000 else t) * 1000).toLong())
    if (long) return SimpleDateFormat("MM-dd HH:mm", Locale.CHINA).format(date)
    val now = Calendar.getInstance()
    val then = Calendar.getInstance().apply { time = date }
    val today = now.get(Calendar.YEAR) == then.get(Calendar.YEAR) && now.get(Calendar.DAY_OF_YEAR) == then.get(Calendar.DAY_OF_YEAR)
    return SimpleDateFormat(if (today) "HH:mm" else "MM-dd", Locale.CHINA).format(date)
}

/**
 * 正文和评论：顶部“正文 / 图片 / 评论”分段；手机上文字直接滚动（固件是分页）。转发的原文接在正文后面。只读。
 * Text and comments with a "Text / Images / Comments" segment; on a phone the text scrolls (the firmware pages it).
 * The reposted original follows the text. Read only.
 */
@Composable
fun TextScreen(vm: AppVm, s: Screen.Text) {
    val p = s.post
    Column(Modifier.fillMaxSize().background(Paper)) {
        Column(Modifier.padding(horizontal = 16.dp)) {
            T("‹ 返回", 16.sp, modifier = Modifier.padding(vertical = 10.dp).noRippleClick { vm.backToList() })
            PostHead(p.user, p.time, p.source, long = true)
            Segment(
                listOf("正文", "图片 ${p.images}", "评论 ${fmtCount(p.comments)}"), if (s.comments) 2 else 0,
                { i ->
                    when (i) {
                        0 -> vm.screen = s.copy(comments = false)
                        1 -> if (p.images > 0) vm.screen = Screen.Images(p, 0)
                        else -> vm.showComments(p)
                    }
                },
                Modifier.padding(top = 10.dp),
            )
        }
        Column(Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(horizontal = 16.dp, vertical = 14.dp)) {
            if (s.comments) Comments(vm) else Body(vm, p)
        }
    }
}

/** 正文、转发灰框、图片宫格（点哪张从哪张看）、三栏计数。/ Text, the gray repost box, the image grid (tap to open that
 *  image) and the count bar. */
@Composable
private fun Body(vm: AppVm, p: Post) {
    if (p.text.isNotBlank())
        Text(p.text, style = TextStyle(fontSize = 16.sp, lineHeight = 26.sp, fontFamily = FontFamily.Serif, color = Ink))
    else if (p.rtText.isEmpty()) T("这条没有文字。", 15.sp)
    if (p.rtText.isNotEmpty()) QuoteBox(p.rtUser, p.rtText, modifier = Modifier.padding(top = 10.dp))
    if (p.images > 0) Box(Modifier.padding(top = 12.dp)) {
        PicGrid(vm, p.id, p.images, p.pics, p.ratio) { k -> vm.screen = Screen.Images(p, vm.pageOf(p, k)) }
    }
    Spacer(Modifier.height(10.dp))
    HLine()
    CountBar(p.reposts, p.comments, p.likes)
}

/** 评论：每条一个小头像圆、粗体名字、右边心形和赞数。/ Comments: each with a small avatar, bold name, and a heart
 *  with the like count on the right. */
@Composable
private fun Comments(vm: AppVm) {
    val list = vm.comments
    if (list == null) {
        T(if (vm.commentsLoading) "正在取评论…（服务器第一次要去微博取，约几秒）" else vm.commentsError.ifEmpty { "评论还没取到" },
            15.sp, lineHeight = 22.sp)
        return
    }
    if (list.isEmpty()) { T("还没有评论", 15.sp); return }
    list.forEach { c ->
        Row(Modifier.fillMaxWidth().padding(vertical = 8.dp)) {
            Avatar(c.user, size = 32.dp)
            Column(Modifier.weight(1f).padding(start = 10.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    T(c.user.ifEmpty { "微博用户" }, 13.sp, bold = true, maxLines = 1, modifier = Modifier.weight(1f))
                    if (c.likes > 0) { Glyph(2, 14.dp); T(fmtCount(c.likes), 12.sp, modifier = Modifier.padding(start = 3.dp)) }
                }
                T(c.text.ifEmpty { "（空）" }, 15.sp, lineHeight = 22.sp, modifier = Modifier.padding(top = 3.dp))
            }
        }
        HLine(modifier = Modifier.padding(start = 42.dp))
    }
}

/**
 * 看图：一屏一张，左右三成点按或滑动翻图；第一张再往前回正文，最后一张再往后是评论；底栏“‹ 正文 · k / n · 评论 ›”。
 * Images: one per screen; tap the outer thirds or swipe; before the first comes the text and past the last the
 * comments. Bottom bar "‹ Text · k / n · Comments ›".
 */
@Composable
fun ImagesScreen(vm: AppVm, s: Screen.Images) {
    val ctx = LocalContext.current
    val p = s.post
    val turn = { d: Int ->
        val k = s.index + d
        when {
            k >= p.images -> vm.showComments(p)
            k < 0 -> vm.screen = Screen.Text(p, false)
            else -> vm.screen = s.copy(index = k)
        }
    }
    // 预取下一张。/ Prefetch the next image.
    LaunchedEffect(s.index) {
        if (s.index + 1 < p.images)
            vm.images.enqueue(ImageRequest.Builder(ctx).data(vm.api.imageUrl(p.id, s.index + 2)).build())
    }
    Column(Modifier.fillMaxSize().background(Paper)) {
        Box(Modifier.weight(1f).fillMaxWidth().swipeTurn(turn), contentAlignment = Alignment.Center) {
            AsyncImage(
                model = ImageRequest.Builder(ctx).data(vm.api.imageUrl(p.id, s.index + 1)).build(),
                imageLoader = vm.images, contentDescription = null, contentScale = ContentScale.Fit,
                modifier = Modifier.fillMaxSize(),
            )
            Row(Modifier.fillMaxSize()) {
                Box(Modifier.weight(3f).fillMaxHeight().noRippleClick { turn(-1) })
                Box(Modifier.weight(4f).fillMaxHeight())
                Box(Modifier.weight(3f).fillMaxHeight().noRippleClick { turn(1) })
            }
        }
        HLine(soft = false)
        Row(Modifier.fillMaxWidth().height(52.dp).padding(horizontal = 20.dp), verticalAlignment = Alignment.CenterVertically) {
            T("‹ 正文", 15.sp, modifier = Modifier.weight(1f).noRippleClick { vm.screen = Screen.Text(p, false) })
            T("${s.index + 1} / ${p.images}", 15.sp, bold = true)
            T("评论 ›", 15.sp, align = TextAlign.End, modifier = Modifier.weight(1f).noRippleClick { vm.showComments(p) })
        }
    }
}
