package com.treader.rednote.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.staggeredgrid.LazyVerticalStaggeredGrid
import androidx.compose.foundation.lazy.staggeredgrid.StaggeredGridCells
import androidx.compose.foundation.lazy.staggeredgrid.items
import androidx.compose.foundation.lazy.staggeredgrid.rememberLazyStaggeredGridState
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
import com.treader.rednote.AppVm
import com.treader.rednote.Seg
import com.treader.rednote.data.Item
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * 小红书首页：标题、刷新、设置键，关注 / 推荐分段，两列瀑布流（封面按原图比例，上下滑动）。
 * Home: title, refresh, settings, the Following / Recommended segment, and a two-column waterfall (covers keep their
 * original ratio; scrolls vertically).
 */
@Composable
fun ListScreen(vm: AppVm, onSettings: () -> Unit) {
    Box(Modifier.fillMaxSize().background(Paper)) {
        Column(Modifier.fillMaxSize()) {
            Column(Modifier.padding(horizontal = 16.dp)) {
                Row(Modifier.fillMaxWidth().height(64.dp), verticalAlignment = Alignment.CenterVertically) {
                    T("小红书", 26.sp, bold = true, modifier = Modifier.weight(1f))
                    if (vm.configured) {
                        if (vm.loading) T("拉取中…", 15.sp)
                        else Row(Modifier.noRippleClick { vm.refresh() }, verticalAlignment = Alignment.CenterVertically) {
                            SyncIcon(Modifier.padding(end = 4.dp))
                            T(if (vm.seg == Seg.FOLLOW) "刷新" else "刷新推荐", 15.sp)
                        }
                    }
                    Box(Modifier.padding(start = 16.dp).noRippleClick(onSettings)) { SettingsIcon() }
                }
                Segment(listOf("关注", "推荐"), vm.seg.ordinal, { vm.showSeg(Seg.entries[it]) })
                if (!vm.configured) {
                    Spacer(Modifier.height(24.dp))
                    T("还没连接 rednote 服务器。先在电脑或 NAS 上部署 rednote（见项目 rednote/ 目录），再到“设置”填服务器地址和设备令牌。",
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
                    "还没有缓存。点右上角“刷新”，从服务器取关注博主的笔记；之后不联网也能看。"
                else "还没有推荐。点右上角“刷新推荐”，服务器会去读一次小红书首页（只留图文）。",
                    15.sp, lineHeight = 23.sp, modifier = Modifier.padding(16.dp))
                return@Column
            }
            // 换分段或刷新后回到顶部。/ Back to the top after switching segments or refreshing.
            val state = rememberLazyStaggeredGridState()
            LaunchedEffect(vm.seg, items.firstOrNull()?.id) { state.scrollToItem(0) }
            LazyVerticalStaggeredGrid(
                columns = StaggeredGridCells.Fixed(2),
                state = state,
                modifier = Modifier.weight(1f).fillMaxWidth(),
                contentPadding = PaddingValues(start = 12.dp, end = 12.dp, top = 6.dp, bottom = 24.dp),
                horizontalArrangement = Arrangement.spacedBy(10.dp),
                verticalItemSpacing = 14.dp,
            ) {
                items(items, key = { it.id }) { Card(vm, it) }
            }
        }
        if (vm.waiting != null) WaitDialog(vm)
    }
}

private fun statusLine(vm: AppVm): String {
    if (vm.loading) return if (vm.seg == Seg.REC) "服务器正在读小红书首页…" else "正在加载…"
    if (vm.msg.isNotEmpty()) return vm.msg
    if (vm.seg == Seg.FOLLOW) return "关注的博主 · ${vm.feed.size} 篇"
    val r = vm.rec ?: return "首页推荐"
    val time = if (r.fetchedAt > 1.7e9) " · " + SimpleDateFormat("HH:mm", Locale.CHINA).format(Date((r.fetchedAt * 1000).toLong())) else ""
    return if (r.left >= 0) "首页推荐$time · ${r.items.size} 篇 · 今天还能拉 ${r.left} 次" else "首页推荐$time · ${r.items.size} 篇"
}

/** 一张卡片：封面（原图比例、黑细框），标题两行，作者 · 赞；没看过的封面左上角标“新”。/ One card: cover (original
 *  ratio, thin black frame), two-line title, author · likes; unseen notes get a "new" tag on the cover. */
@Composable
private fun Card(vm: AppVm, x: Item) {
    val ctx = LocalContext.current
    Column(Modifier.fillMaxWidth().noRippleClick { vm.open(x) }) {
        val url = if (vm.seg == Seg.FOLLOW) vm.api.imageUrl(x.id, 0) else if (x.thumb) vm.api.coverUrl(x.id) else null
        Box(
            Modifier.fillMaxWidth().aspectRatio(1f / x.ratio).clip(RoundedCornerShape(6.dp))
                .border(1.dp, Ink, RoundedCornerShape(6.dp)).background(Line.copy(alpha = 0.35f))
        ) {
            if (url != null) AsyncImage(
                model = ImageRequest.Builder(ctx).data(url).build(), imageLoader = vm.images,
                contentDescription = null, contentScale = ContentScale.Crop, modifier = Modifier.fillMaxSize(),
            )
            if (!x.seen) Box(Modifier.padding(6.dp).background(Ink, RoundedCornerShape(3.dp)).padding(horizontal = 5.dp)) {
                T("新", 11.sp, bold = true, color = Paper)
            }
            if (vm.seg == Seg.REC && vm.noteCached(x.id)) Box(
                Modifier.align(Alignment.BottomEnd).padding(6.dp).background(Paper, RoundedCornerShape(3.dp))
                    .border(1.dp, Ink, RoundedCornerShape(3.dp)).padding(horizontal = 5.dp)
            ) { T("已下载", 10.sp, bold = true) }
        }
        T(x.title.ifEmpty { "（无标题）" }, 14.sp, bold = true, maxLines = 2, lineHeight = 19.sp,
            modifier = Modifier.padding(top = 7.dp, start = 2.dp, end = 2.dp))
        Row(Modifier.fillMaxWidth().padding(top = 5.dp, start = 2.dp, end = 2.dp), verticalAlignment = Alignment.CenterVertically) {
            T(x.author, 12.sp, color = Mid, maxLines = 1, modifier = Modifier.weight(1f))
            if (x.likes.isNotEmpty()) T("赞 ${x.likes}", 12.sp, color = Mid, maxLines = 1)
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
                T("没能打开这篇", 17.sp, bold = true)
                T(vm.waitError, 13.sp, align = TextAlign.Center, lineHeight = 19.sp)
                T("点框外关闭", 12.sp, color = Mid)
            } else {
                T("正在拉取这篇笔记…", 17.sp, bold = true)
                T("服务器要打开小红书，约 6～12 秒", 13.sp, color = Mid)
                T("点框外取消", 13.sp)
            }
        }
    }
}
