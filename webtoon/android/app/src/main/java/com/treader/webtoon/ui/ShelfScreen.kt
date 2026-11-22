package com.treader.webtoon.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.treader.webtoon.AppVm
import com.treader.webtoon.Tab
import com.treader.webtoon.data.Series

/*
 * 我的漫画（主页），照固件我的漫画：全部 / 已订阅，一部一行（封面、名字、话数、“N 话新”）；右上角“检查更新”把全部订阅的
 * 新话下载下来，进度和结果显示在分段下面。点一部进它的话列表。
 * The shelf (home) after the firmware's comic shelf: All / Subscribed, one series per row (cover, name, episode count,
 * "N new"); "Check for updates" at the top right downloads every subscription's new episodes, with progress and the
 * result shown below the segments. Tap a series for its episode list.
 */

@Composable
fun TitleRow(title: String, action: String? = null, onAction: () -> Unit = {}) {
    Row(Modifier.fillMaxWidth().padding(start = 20.dp, end = 20.dp, top = 14.dp, bottom = 8.dp), verticalAlignment = Alignment.CenterVertically) {
        PageTitle(title, Modifier.weight(1f))
        if (action != null) Row(Modifier.noRippleClick(onAction), verticalAlignment = Alignment.CenterVertically) {
            SyncIcon(Modifier.padding(end = 8.dp).size(24.dp))
            T(action, 14.sp)
        }
    }
}

@Composable
fun ShelfTab(vm: AppVm) {
    if (vm.currentSeries() != null) { SeriesScreen(vm); return }
    val u = vm.update
    Column(Modifier.fillMaxSize()) {
        TitleRow("漫画", if (u.running) "取消" else "检查更新") {
            if (u.running) vm.cancelUpdate()
            else if (!vm.configured) vm.tab = Tab.SETTINGS
            else vm.checkUpdates()
        }
        Segment(listOf("全部", "已订阅"), vm.shelfSeg, { vm.shelfSeg = it; vm.shelfPage = 0 }, Modifier.padding(horizontal = 20.dp))
        UpdateCard(vm)
        val list = if (vm.shelfSeg == 1) vm.series.filter { it.subscribed } else vm.series
        if (list.isEmpty()) {
            T(
                when {
                    !vm.configured -> "先到“设置”填 webtoon 服务器地址和设备令牌，再到发现页找漫画订阅。"
                    vm.shelfSeg == 1 -> "还没有订阅。到发现页点一部，再点“订阅”。"
                    else -> "我的漫画是空的。到发现页找一部订阅，点“检查更新”下载。"
                },
                14.sp, color = Mid, lineHeight = 20.sp, modifier = Modifier.padding(20.dp),
            )
            return
        }
        HLine(modifier = Modifier.padding(start = 20.dp, end = 20.dp, top = 12.dp))
        PagedList(list, 96.dp, vm.shelfPage, { vm.shelfPage = it }) { s -> SeriesRow(vm, s) }
    }
}

@Composable
private fun UpdateCard(vm: AppVm) {
    val u = vm.update
    if (!u.running && u.message.isEmpty()) return
    Column(
        Modifier.padding(start = 20.dp, end = 20.dp, top = 12.dp).fillMaxWidth().card()
            .noRippleClick { vm.clearUpdateMessage() }.padding(14.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        if (u.running) {
            T(if (u.total > 0) "下载 ${u.done} / ${u.total}" else "检查更新", 14.sp, bold = true)
            T(u.line, 13.sp, color = Mid, maxLines = 2)
            if (u.total > 0) ProgressBar(u.done * 100 / u.total)
        } else {
            T(u.message, 13.sp, lineHeight = 19.sp, maxLines = 8)
            T("点一下收起", 12.sp, color = Mid)
        }
    }
}

@Composable
private fun SeriesRow(vm: AppVm, s: Series) {
    val unread = vm.unread(s)
    Column(Modifier.fillMaxSize().padding(horizontal = 20.dp).noRippleClick { vm.openSeries = s.dir; vm.seriesPage = 0 }) {
        Row(Modifier.weight(1f).fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
            val m = s.mark
            Cover(s.name, m?.let { vm.api.coverUrl(it.id, it.lang) }, vm.images, Modifier.width(54.dp).height(72.dp), 11.sp)
            Column(Modifier.weight(1f).padding(start = 14.dp)) {
                T(s.name, 16.sp, bold = true, maxLines = 1)
                T(
                    listOfNotNull(
                        if (m != null) "已订阅" else "本地",
                        "${s.episodes.size} 话",
                        m?.takeIf { it.last > 0 && s.episodes.isNotEmpty() }?.let { "下载到第 ${it.last} 话" },
                    ).joinToString(" · "),
                    13.sp, color = Mid, maxLines = 1, modifier = Modifier.padding(top = 4.dp),
                )
                if (m != null && m.author.isNotEmpty()) T(m.author, 12.sp, color = Mid, maxLines = 1, modifier = Modifier.padding(top = 2.dp))
            }
            if (unread > 0) Badge("$unread 话新", true)
        }
        HLine()
    }
}

/** 一部作品的话列表：继续看、详情（订阅的）或删除（本地的）。/ A series' episode list: continue, details
 *  (subscribed) or delete (local). */
@Composable
private fun SeriesScreen(vm: AppVm) {
    val s = vm.currentSeries() ?: return
    var confirmDelete by remember { mutableStateOf(false) }
    Box(Modifier.fillMaxSize()) {
        Column(Modifier.fillMaxSize()) {
            BackTitle(s.name) { vm.openSeries = null }
            val resume = s.episodes.indexOfFirst { vm.progress.get(it.key)?.finished != true }.takeIf { it >= 0 }
            Row(Modifier.padding(horizontal = 20.dp), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                if (s.episodes.isNotEmpty()) {
                    val i = resume ?: 0
                    Pill(if (resume == null) "从头再看" else "继续看 ${epLabel(s.episodes[i].no, s.episodes[i].name)}", true,
                        Modifier.weight(1.6f)) { vm.read(s, i) }
                }
                val m = s.mark
                if (m != null) Pill("详情", false, Modifier.weight(1f)) { vm.openDetail(m.id, s.name, m.lang) }
                else Pill("删除", false, Modifier.weight(1f)) { confirmDelete = true }
            }
            if (vm.readError.isNotEmpty()) T(vm.readError, 13.sp, color = Mid, modifier = Modifier.padding(horizontal = 20.dp, vertical = 8.dp))
            if (s.episodes.isEmpty()) {
                T(if (s.subscribed) "还没有下载的话。回我的漫画点“检查更新”。" else "这个文件夹里没有 CBZ。", 14.sp, color = Mid,
                    modifier = Modifier.padding(20.dp))
                return@Column
            }
            HLine(modifier = Modifier.padding(start = 20.dp, end = 20.dp, top = 12.dp))
            PagedList(s.episodes.indices.toList(), 56.dp, vm.seriesPage, { vm.seriesPage = it }) { i ->
                val e = s.episodes[i]
                val p = vm.progress.get(e.key)
                Column(Modifier.fillMaxSize().padding(horizontal = 20.dp).noRippleClick { vm.read(s, i) }) {
                    Row(Modifier.weight(1f).fillMaxWidth(), verticalAlignment = Alignment.CenterVertically) {
                        if (p == null) NewTag()
                        T(epLabel(e.no, e.name), 15.sp, bold = p == null, maxLines = 1, modifier = Modifier.weight(1f))
                        T(
                            when {
                                p == null -> ""
                                p.finished -> "看完"
                                else -> "${p.page + 1} / ${p.pages}"
                            },
                            13.sp, color = Mid, modifier = Modifier.padding(start = 10.dp),
                        )
                    }
                    HLine()
                }
            }
        }
        if (confirmDelete) ChoiceDialog(
            "删除这部漫画？", "会删掉“${s.name}”的 ${s.episodes.size} 话，不能恢复。",
            listOf(
                Choice("删除", true) { confirmDelete = false; vm.deleteSeries(s) },
                Choice("取消") { confirmDelete = false },
            ),
        ) { confirmDelete = false }
    }
}

/** 列表里的一话：“第 12 话 标题”（文件名里的补零去掉）。/ An episode in lists: "第 12 话 title" without the padding. */
fun epLabel(no: Int?, name: String): String {
    if (no == null) return name
    val rest = name.replaceFirst(Regex("""^第\d+话\s*"""), "")
    return if (rest.isEmpty()) "第 $no 话" else "第 $no 话 $rest"
}
