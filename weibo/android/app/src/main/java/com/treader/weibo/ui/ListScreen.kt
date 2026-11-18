package com.treader.weibo.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import coil.compose.AsyncImage
import coil.request.ImageRequest
import com.treader.weibo.AppVm
import com.treader.weibo.Seg
import com.treader.weibo.data.Item

/**
 * 微博首页：标题、刷新、设置键，关注 / 热门分段，一行一条（作者、时间、正文前四行、转评赞，有图的右边一张缩略图）。
 * Home: title, refresh, settings, the Following / Hot segment, and one post per row (author, time, four lines of
 * text, counts, and a thumbnail on the right when there are images).
 */
@Composable
fun ListScreen(vm: AppVm, onSettings: () -> Unit) {
    Box(Modifier.fillMaxSize().background(Paper)) {
        Column(Modifier.fillMaxSize()) {
            Column(Modifier.padding(horizontal = 16.dp)) {
                Row(Modifier.fillMaxWidth().height(64.dp), verticalAlignment = Alignment.CenterVertically) {
                    T("微博", 26.sp, bold = true, modifier = Modifier.weight(1f))
                    if (vm.configured) {
                        if (vm.loading) T("拉取中…", 15.sp)
                        else Row(Modifier.noRippleClick { vm.refresh() }, verticalAlignment = Alignment.CenterVertically) {
                            SyncIcon(Modifier.padding(end = 4.dp))
                            T("刷新", 15.sp)
                        }
                    }
                    Box(Modifier.padding(start = 16.dp).noRippleClick(onSettings)) { SettingsIcon() }
                }
                Segment(listOf("关注", "热门"), vm.seg.ordinal, { vm.showSeg(Seg.entries[it]) })
                if (!vm.configured) {
                    Spacer(Modifier.height(24.dp))
                    T("还没连接 weibo 服务器。先在电脑或 NAS 上部署 weibo（见项目 weibo/ 目录），再到“设置”填服务器地址和设备令牌。",
                        15.sp, lineHeight = 23.sp)
                    Spacer(Modifier.height(20.dp))
                    Pill("去设置", true, Modifier.width(140.dp), onClick = onSettings)
                    return@Column
                }
                T(statusLine(vm), 13.sp, color = Mid, maxLines = 2, modifier = Modifier.padding(top = 10.dp, bottom = 4.dp))
            }
            if (!vm.configured) return@Column
            val items = vm.items
            if (items.isEmpty()) {
                T(if (vm.loading) "加载中，请稍候。" else if (vm.seg == Seg.FOLLOW)
                    "还没有缓存。点右上角“刷新”，从服务器取关注的人的微博；之后不联网也能看。"
                else "还没有缓存。点右上角“刷新”，从服务器取热门微博。",
                    15.sp, lineHeight = 23.sp, modifier = Modifier.padding(16.dp))
                return@Column
            }
            // 换分段或刷新后回到顶部。/ Back to the top after switching segments or refreshing.
            val state = rememberLazyListState()
            LaunchedEffect(vm.seg, items.firstOrNull()?.id) { state.scrollToItem(0) }
            LazyColumn(
                state = state,
                modifier = Modifier.weight(1f).fillMaxWidth(),
                contentPadding = PaddingValues(start = 16.dp, end = 16.dp, top = 4.dp, bottom = 24.dp),
            ) {
                itemsIndexed(items, key = { _, it -> it.id }) { i, x ->
                    PostRow(vm, x)
                    if (i + 1 < items.size) HLine()
                }
            }
        }
        if (vm.waiting != null) WaitDialog(vm)
    }
}

private fun statusLine(vm: AppVm): String {
    if (vm.loading) return "正在加载…"
    if (vm.msg.isNotEmpty()) return vm.msg
    val f = vm.feed
    val what = if (vm.seg == Seg.FOLLOW) "关注的人" else "热门"
    val time = if (f != null && f.now > 1.7e9) " · 刷新于 " + fmtTime(f.now, long = true) else ""
    return "$what · ${f?.items?.size ?: 0} 条$time"
}

/** 一行：作者（没看过的前面“新”）、时间；正文前四行（转发的接原文）；转评赞；有图的右边一张缩略图。
 *  One row: author (a "new" tag before unseen posts) and time; four lines of text (reposts continue with the
 *  original); counts; a thumbnail on the right when there are images. */
@Composable
private fun PostRow(vm: AppVm, x: Item) {
    val ctx = LocalContext.current
    Row(Modifier.fillMaxWidth().noRippleClick { vm.open(x) }.padding(vertical = 12.dp)) {
        Column(Modifier.weight(1f)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                if (!x.seen) Box(Modifier.padding(end = 6.dp).background(Ink, RoundedCornerShape(3.dp)).padding(horizontal = 5.dp)) {
                    T("新", 11.sp, bold = true, color = Paper)
                }
                T(x.user.ifEmpty { "微博用户" }, 14.sp, bold = true, maxLines = 1, modifier = Modifier.weight(1f))
                T(fmtTime(x.time), 12.sp, color = Mid)
            }
            val text = if (x.rtText.isNotEmpty())
                x.text + (if (x.text.isNotEmpty()) " " else "") + "// @${x.rtUser}：${x.rtText}"
            else x.text.ifEmpty { "（没有文字）" }
            T(text, 15.sp, maxLines = 4, lineHeight = 21.sp, modifier = Modifier.padding(top = 6.dp))
            T("转发 ${fmtCount(x.reposts)} · 评论 ${fmtCount(x.comments)} · 赞 ${fmtCount(x.likes)}", 12.sp, color = Mid,
                maxLines = 1, modifier = Modifier.padding(top = 6.dp))
        }
        if (x.images > 0) Box(
            Modifier.padding(start = 12.dp).size(84.dp).clip(RoundedCornerShape(6.dp))
                .border(1.dp, Ink, RoundedCornerShape(6.dp)).background(Line.copy(alpha = 0.35f))
        ) {
            AsyncImage(
                model = ImageRequest.Builder(ctx).data(vm.api.imageUrl(x.id, 0)).build(), imageLoader = vm.images,
                contentDescription = null, contentScale = ContentScale.Crop, modifier = Modifier.fillMaxSize(),
            )
            if (x.images > 1) Box(
                Modifier.align(Alignment.BottomEnd).padding(4.dp).background(Ink, RoundedCornerShape(3.dp))
                    .padding(horizontal = 4.dp)
            ) { T("${x.images} 图", 10.sp, bold = true, color = Paper) }
        }
    }
}

@Composable
private fun WaitDialog(vm: AppVm) {
    Box(Modifier.fillMaxSize().background(Paper.copy(alpha = 0.6f)).noRippleClick { vm.cancelWait() }, contentAlignment = Alignment.Center) {
        Column(
            Modifier.padding(horizontal = 40.dp).fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(Paper)
                .border(2.dp, Ink, RoundedCornerShape(12.dp)).noRippleClick {}.padding(vertical = 22.dp, horizontal = 18.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            if (vm.waitError.isNotEmpty()) {
                T("没能打开这条", 17.sp, bold = true)
                T(vm.waitError, 13.sp, align = TextAlign.Center, lineHeight = 19.sp)
                T("点框外关闭", 12.sp, color = Mid)
            } else {
                T("正在加载这条微博…", 17.sp, bold = true)
                T("从服务器取全文", 13.sp, color = Mid)
                T("点框外取消", 13.sp)
            }
        }
    }
}
