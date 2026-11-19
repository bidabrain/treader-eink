package com.treader.rednote.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.ui.draw.clip
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
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.material3.Text
import coil.compose.AsyncImage
import coil.request.ImageRequest
import com.treader.rednote.AppVm
import com.treader.rednote.Screen
import com.treader.rednote.data.Note
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * 看图：一屏一张，左右三成点按或滑动翻图，最后一张再往后是正文；底栏“‹ 返回 · k / n · 正文 ›”（和固件一样）。
 * Images: one per screen; tap the outer thirds or swipe; past the last comes the text. Bottom bar
 * "‹ Back · k / n · Text ›", as on the firmware.
 */
@Composable
fun ImagesScreen(vm: AppVm, s: Screen.Images) {
    val ctx = LocalContext.current
    val n = s.note
    val turn = { d: Int ->
        val k = s.index + d
        when {
            k >= n.images -> vm.screen = Screen.Text(n, false)
            k >= 0 -> vm.screen = s.copy(index = k)
        }
    }
    // 点中间开关“保存图片”栏，翻页就收起。/ A middle tap toggles the save bar; turning the page hides it.
    var bar by remember(s.index) { mutableStateOf(false) }
    // 预取下一张。/ Prefetch the next image.
    LaunchedEffect(s.index) {
        if (s.index + 1 < n.images)
            vm.images.enqueue(ImageRequest.Builder(ctx).data(vm.api.imageUrl(n.id, s.index + 2)).build())
    }
    Column(Modifier.fillMaxSize().background(Paper)) {
        Box(Modifier.weight(1f).fillMaxWidth().swipeTurn(turn), contentAlignment = Alignment.Center) {
            AsyncImage(
                model = ImageRequest.Builder(ctx).data(vm.api.imageUrl(n.id, s.index + 1)).build(),
                imageLoader = vm.images, contentDescription = null, contentScale = ContentScale.Fit,
                modifier = Modifier.fillMaxSize(),
            )
            Row(Modifier.fillMaxSize()) {
                Box(Modifier.weight(3f).fillMaxHeight().noRippleClick { turn(-1) })
                Box(Modifier.weight(4f).fillMaxHeight().noRippleClick { bar = !bar })
                Box(Modifier.weight(3f).fillMaxHeight().noRippleClick { turn(1) })
            }
            if (bar) SaveBar(vm.images, vm.api.imageUrl(n.id, s.index + 1), "xhs_${n.id}_${s.index + 1}.jpg", "Treader 小红书",
                Modifier.align(Alignment.BottomCenter))
        }
        HLine(soft = false)
        Row(Modifier.fillMaxWidth().height(52.dp).padding(horizontal = 20.dp), verticalAlignment = Alignment.CenterVertically) {
            T("‹ 返回", 15.sp, modifier = Modifier.weight(1f).noRippleClick { vm.backToList() })
            T("${s.index + 1} / ${n.images}", 15.sp, bold = true)
            T("正文 ›", 15.sp, align = TextAlign.End,
                modifier = Modifier.weight(1f).noRippleClick { vm.screen = Screen.Text(n, false) })
        }
    }
}

/**
 * 正文和评论：顶部“图片 / 正文 / 评论”分段；手机上文字直接滚动（固件是分页）。只读，不点赞、不评论。
 * Text and comments with an "Images / Text / Comments" segment; on a phone the text scrolls (the firmware pages it).
 * Read only: no likes or comments.
 */
@Composable
fun TextScreen(vm: AppVm, s: Screen.Text) {
    val n = s.note
    Box(Modifier.fillMaxSize()) {
    Column(Modifier.fillMaxSize().background(Paper)) {
        BackTitle(n.author.ifEmpty { "返回" }) { vm.backToList() }
        Segment(
            listOf("图片 ${n.images}", "正文", "评论 ${n.list.size}"), if (s.comments) 2 else 1,
            { i ->
                when (i) {
                    0 -> if (n.images > 0) vm.screen = Screen.Images(n, 0)
                    1 -> vm.screen = s.copy(comments = false)
                    else -> vm.screen = s.copy(comments = true)
                }
            },
            Modifier.padding(horizontal = 20.dp),
        )
        Column(Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(horizontal = 20.dp, vertical = 16.dp)) {
            if (s.comments) Comments(n) { vm.loadMoreComments(n) } else Body(n)
        }
    }
    if (vm.moreLoading) MoreDialog(vm)
    }
}

@Composable
private fun MoreDialog(vm: AppVm) {
    Box(Modifier.fillMaxSize().background(Paper.copy(alpha = 0.6f)).noRippleClick { vm.closeMore() }, contentAlignment = Alignment.Center) {
        Column(
            Modifier.padding(horizontal = 40.dp).fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(Paper)
                .border(2.dp, Ink, RoundedCornerShape(12.dp)).noRippleClick {}.padding(vertical = 22.dp, horizontal = 18.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            if (vm.moreError.isNotEmpty()) {
                T("没能加载更多评论", 17.sp, bold = true)
                T(vm.moreError, 13.sp, align = TextAlign.Center, lineHeight = 19.sp)
                T("点框外关闭", 12.sp, color = Mid)
            } else {
                T("正在加载更多评论…", 17.sp, bold = true)
                T("服务器要打开小红书往下翻，约 20～60 秒", 13.sp, color = Mid)
                T("点框外取消", 13.sp)
            }
        }
    }
}

@Composable
private fun Body(n: Note) {
    T(n.title.ifEmpty { "（无标题）" }, 19.sp, bold = true, lineHeight = 25.sp)
    val date = if (n.time > 1e9) {
        val sec = if (n.time > 1e11) n.time / 1000 else n.time
        " · " + SimpleDateFormat("MM-dd", Locale.CHINA).format(Date((sec * 1000).toLong()))
    } else ""
    val meta = n.author + date + (if (n.likes.isNotEmpty()) " · 赞 ${n.likes}" else "") +
        (if (n.comments.isNotEmpty()) " · 评论 ${n.comments}" else "")
    T(meta, 12.sp, color = Mid, modifier = Modifier.padding(top = 6.dp, bottom = 10.dp))
    HLine()
    Spacer(Modifier.height(10.dp))
    if (n.body.isBlank()) T("这篇没有文字。", 15.sp)
    else Text(n.body, style = TextStyle(fontSize = 16.sp, lineHeight = 26.sp, fontFamily = FontFamily.Serif, color = Ink))
}

@Composable
private fun Comments(n: Note, onMore: () -> Unit) {
    if (n.list.isEmpty()) T("服务器没有存这篇的评论", 15.sp, modifier = Modifier.padding(bottom = 16.dp))
    n.list.forEach { c ->
        Row(Modifier.fillMaxWidth()) {
            T(c.user.ifEmpty { "小红书用户" }, 13.sp, bold = true, maxLines = 1, modifier = Modifier.weight(1f))
            if (c.likes.isNotEmpty() && c.likes != "0") T("赞 ${c.likes}", 12.sp, bold = true)
        }
        T(c.text.ifEmpty { "（空）" }, 15.sp, lineHeight = 22.sp, modifier = Modifier.padding(top = 4.dp, bottom = 10.dp))
        HLine()
        Spacer(Modifier.height(10.dp).width(1.dp))
    }
    MoreButton(n.more, onMore)
}

/** 评论末尾的“加载更多评论”；不能加载时灰色，点了没反应。/ "Load more comments" at the end; gray and inert when
 *  nothing more can be loaded. */
@Composable
private fun MoreButton(enabled: Boolean, onClick: () -> Unit) {
    val shape = RoundedCornerShape(22.dp)
    Box(Modifier.fillMaxWidth().padding(top = 6.dp, bottom = 12.dp), contentAlignment = Alignment.Center) {
        Box(
            Modifier.width(220.dp).height(44.dp).clip(shape)
                .then(if (enabled) Modifier.border(1.5.dp, Ink, shape).noRippleClick(onClick) else Modifier.background(Line)),
            contentAlignment = Alignment.Center,
        ) { T("加载更多评论", 15.sp, bold = enabled, color = if (enabled) Ink else Mid) }
    }
}
