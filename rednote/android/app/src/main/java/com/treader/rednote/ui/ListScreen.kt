package com.treader.rednote.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
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

private val ROW_H = 96.dp

/**
 * 小红书首页：标题、刷新、设置键，关注 / 推荐分段，按页显示的列表（和固件一样不滚动，左右滑或点“‹ ›”翻页）。
 * Home: title, refresh, settings, the Following / Recommended segment, and a paged list (like the firmware it does
 * not scroll; swipe or tap "‹ ›" to turn pages).
 */
@Composable
fun ListScreen(vm: AppVm, onSettings: () -> Unit) {
    Box(Modifier.fillMaxSize().background(Paper)) {
        Column(Modifier.fillMaxSize().padding(horizontal = 20.dp)) {
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
            T(statusLine(vm), 13.sp, color = Mid, maxLines = 2, modifier = Modifier.padding(top = 10.dp, bottom = 6.dp))
            val items = vm.items
            if (items.isEmpty()) {
                T(if (vm.loading) "加载中，请稍候。" else if (vm.seg == Seg.FOLLOW)
                    "还没有缓存。点右上角“刷新”，从服务器取关注博主的笔记；之后不联网也能看。"
                else "还没有推荐。点右上角“刷新推荐”，服务器会去读一次小红书首页（只留图文）。",
                    15.sp, lineHeight = 23.sp, modifier = Modifier.padding(top = 12.dp))
                return@Column
            }
            BoxWithConstraints(Modifier.weight(1f).fillMaxWidth()) {
                val rows = ((maxHeight - 40.dp) / ROW_H).toInt().coerceAtLeast(1)
                val pages = (items.size + rows - 1) / rows
                if (vm.page >= pages) vm.page = pages - 1
                val turn = { d: Int -> val n = vm.page + d; if (n in 0 until pages) vm.page = n }
                Column(Modifier.fillMaxSize().swipeTurn(turn)) {
                    Column(Modifier.weight(1f)) {
                        items.drop(vm.page * rows).take(rows).forEach { ItemRow(vm, it) }
                    }
                    Pager(vm.page, pages, turn)
                }
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
    return if (r.left >= 0) "首页推荐$time · 今天还能拉 ${r.left} 次" else "首页推荐$time"
}

@Composable
private fun ItemRow(vm: AppVm, x: Item) {
    val ctx = LocalContext.current
    Column(Modifier.fillMaxWidth().height(ROW_H).noRippleClick { vm.open(x) }) {
        Row(Modifier.weight(1f).padding(vertical = 8.dp)) {
            val url = if (vm.seg == Seg.FOLLOW) vm.api.imageUrl(x.id, 0) else if (x.thumb) vm.api.coverUrl(x.id) else null
            Box(Modifier.size(60.dp, 80.dp).border(1.dp, Ink)) {
                if (url != null) AsyncImage(
                    model = ImageRequest.Builder(ctx).data(url).build(), imageLoader = vm.images,
                    contentDescription = null, contentScale = ContentScale.Crop, modifier = Modifier.fillMaxSize(),
                )
            }
            Column(Modifier.padding(start = 12.dp).weight(1f)) {
                Row(verticalAlignment = Alignment.Top) {
                    if (!x.seen) Box(Modifier.padding(top = 2.dp)) { NewTag() }
                    T(x.title.ifEmpty { "（无标题）" }, 15.sp, bold = true, maxLines = 2, lineHeight = 21.sp)
                }
                Spacer(Modifier.weight(1f))
                val meta = if (vm.seg == Seg.FOLLOW)
                    "${x.author} · ${x.images} 页" + if (x.likes.isNotEmpty()) " · 赞 ${x.likes}" else ""
                else "${x.author} · " + if (vm.noteCached(x.id)) "已下载" else "点开后下载"
                T(meta, 12.sp, color = Mid, maxLines = 1)
            }
        }
        HLine()
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
                T("服务器要打开小红书，约 10～20 秒", 13.sp, color = Mid)
                T("点框外取消", 13.sp)
            }
        }
    }
}
