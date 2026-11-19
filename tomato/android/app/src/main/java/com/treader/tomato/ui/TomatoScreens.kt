package com.treader.tomato.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.treader.tomato.AppVm
import com.treader.tomato.TomatoView
import com.treader.tomato.data.Hit
import com.treader.tomato.data.Op
import com.treader.tomato.data.Phase
import com.treader.tomato.data.TomatoBook

/*
 * 番茄标签，照固件 app_tr_tomato.c：书架（检查更新、下载全部更新）、搜索、结果、详情。手机上列表直接上下滚动，不分页。
 * The Tomato tab after the firmware's app_tr_tomato.c: shelf (check, download all), search, results and detail. Lists
 * scroll on a phone instead of paging.
 */

@Composable
fun TomatoTab(vm: AppVm, onOpenBook: (TomatoBook) -> Unit, onSettings: () -> Unit) {
    when (val v = vm.tomatoView) {
        TomatoView.Shelf -> if (vm.tomatoSeg == 0) ShelfScreen(vm, onOpenBook, onSettings) else StoreTab(vm)
        TomatoView.Search -> SearchScreen(vm)
        TomatoView.Results -> ResultsScreen(vm)
        is TomatoView.Detail -> DetailScreen(vm, v.hit, v.fromStore, onOpenBook)
    }
}

/** 标题行：大标题，右边一个带图标的动作。/ Title row: a big title with an optional action on the right. */
@Composable
fun TitleRow(title: String, action: String? = null, onAction: () -> Unit = {}) {
    Row(Modifier.fillMaxWidth().padding(start = 20.dp, end = 20.dp, top = 14.dp, bottom = 8.dp), verticalAlignment = Alignment.CenterVertically) {
        T(title, 24.sp, bold = true, modifier = Modifier.weight(1f))
        if (action != null) Row(Modifier.noRippleClick(onAction), verticalAlignment = Alignment.CenterVertically) {
            SyncIcon(Modifier.padding(end = 4.dp))
            T(action, 14.sp)
        }
    }
}

@Composable
private fun ShelfScreen(vm: AppVm, onOpenBook: (TomatoBook) -> Unit, onSettings: () -> Unit) {
    val s = vm.status
    val working = vm.busy && (s.op == Op.CHECK || s.op == Op.UPDATE)
    Column(Modifier.fillMaxSize()) {
        TitleRow("番茄小说", if (vm.busy) null else "检查更新") { if (vm.configured) vm.check() }
        Segment(listOf("书架", "书城"), vm.tomatoSeg, { vm.tomatoSeg = it }, Modifier.padding(horizontal = 20.dp))
        val line = when {
            working -> "正在${if (s.op == Op.CHECK) "检查" else "更新"}，可以先去看书"
            (s.op == Op.CHECK || s.op == Op.UPDATE) && (s.phase == Phase.DONE || s.phase == Phase.FAILED) ->
                "${vm.shelf.size} 本 · ${s.message}"
            else -> "${vm.shelf.size} 本 · ${vm.pending} 本有新章节"
        }
        T(line, 13.sp, modifier = Modifier.padding(start = 20.dp, end = 20.dp, top = 10.dp))
        SearchField(Modifier.padding(start = 20.dp, end = 20.dp, top = 10.dp)) { vm.tomatoView = TomatoView.Search }
        if (working) {
            Row(Modifier.fillMaxWidth().padding(start = 20.dp, end = 20.dp, top = 14.dp), verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    T(if (s.total > 0) "${s.current} / ${s.total}　${s.message}" else s.message, 17.sp, bold = true, maxLines = 1)
                    T(if (s.phase == Phase.DOWNLOADING) "正在下载 · %.1f MB".format(s.bytes / 1048576.0)
                      else if (s.phase == Phase.QUEUED) "服务器正在生成 TXT" else "", 13.sp, color = Mid)
                }
                T("取消", 15.sp, modifier = Modifier.noRippleClick { vm.cancel() }.padding(8.dp))
            }
        } else if (vm.pending > 0) {
            Pill("下载全部更新（${vm.pending} 本）", true, Modifier.fillMaxWidth().padding(start = 20.dp, end = 20.dp, top = 14.dp)) {
                vm.updateAll()
            }
        }
        Spacer(Modifier.height(8.dp))
        if (vm.shelf.isEmpty()) {
            Column(Modifier.padding(20.dp)) {
                T(if (vm.configured) "还没有番茄小说。点上面的搜索框找书。" else "还没设置番茄服务器。", 15.sp, lineHeight = 23.sp)
                if (!vm.configured) Pill("去设置", true, Modifier.padding(top = 16.dp).width(160.dp)) { onSettings() }
            }
            return
        }
        LazyColumn(Modifier.fillMaxSize().padding(horizontal = 20.dp)) {
            items(vm.shelf, key = { it.bookId }) { b -> ShelfRow(vm, b) { onOpenBook(b) } }
        }
    }
}

@Composable
private fun ShelfRow(vm: AppVm, b: TomatoBook, onClick: () -> Unit) {
    val pct = vm.progressPct(b.file)
    val mark = when {
        b.pending -> "+${b.remote - b.chapters} 章"
        b.file.isEmpty() -> "未下载"
        pct < 0 -> "未读"
        pct >= 100 -> "读完"
        else -> "已读 $pct%"
    }
    Column(Modifier.fillMaxWidth().noRippleClick(onClick)) {
        Row(Modifier.fillMaxWidth().padding(vertical = 9.dp), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                T(b.title, 17.sp, bold = b.pending, maxLines = 1)
                T("${b.author} · ${b.chapters} 章", 13.sp, maxLines = 1, modifier = Modifier.padding(top = 2.dp))
            }
            T(mark, 14.sp, bold = b.pending, modifier = Modifier.padding(start = 12.dp))
        }
        HLine()
    }
}

/** 假的搜索框：点了进搜索页。/ A fake search box that opens the search page. */
@Composable
fun SearchField(modifier: Modifier = Modifier, onClick: () -> Unit) {
    Row(
        modifier.fillMaxWidth().height(42.dp).clip(RoundedCornerShape(21.dp)).border(1.5.dp, Ink, RoundedCornerShape(21.dp))
            .noRippleClick(onClick).padding(horizontal = 16.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        SearchIcon(modifier = Modifier.padding(end = 10.dp))
        T("搜索书名或作者", 15.sp, color = Mid)
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun SearchScreen(vm: AppVm) {
    var text by remember { mutableStateOf(vm.query) }
    val focus = remember { FocusRequester() }
    LaunchedEffect(Unit) { focus.requestFocus() }
    Column(Modifier.fillMaxSize()) {
        Row(Modifier.fillMaxWidth().padding(start = 12.dp, end = 20.dp, top = 12.dp), verticalAlignment = Alignment.CenterVertically) {
            T("‹", 26.sp, modifier = Modifier.noRippleClick { vm.tomatoView = TomatoView.Shelf }.padding(horizontal = 8.dp))
            Row(
                Modifier.weight(1f).height(44.dp).clip(RoundedCornerShape(22.dp)).border(1.5.dp, Ink, RoundedCornerShape(22.dp))
                    .padding(horizontal = 16.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                SearchIcon(modifier = Modifier.padding(end = 10.dp))
                BasicTextField(
                    text, { text = it }, singleLine = true, textStyle = TextStyle(fontSize = 17.sp, color = Ink),
                    keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
                    keyboardActions = KeyboardActions(onSearch = { vm.search(text) }),
                    modifier = Modifier.weight(1f).focusRequester(focus),
                )
            }
            T("搜索", 16.sp, bold = true, modifier = Modifier.noRippleClick { vm.search(text) }.padding(start = 12.dp))
        }
        val recent = vm.prefs.recent
        if (recent.isNotEmpty()) {
            T("最近搜索", 14.sp, modifier = Modifier.padding(start = 20.dp, top = 20.dp, bottom = 8.dp))
            FlowRow(Modifier.padding(horizontal = 20.dp), horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                recent.forEach { q ->
                    Box(
                        Modifier.height(32.dp).clip(RoundedCornerShape(16.dp)).border(1.5.dp, Ink, RoundedCornerShape(16.dp))
                            .noRippleClick { text = q; vm.search(q) }.padding(horizontal = 14.dp),
                        contentAlignment = Alignment.Center,
                    ) { T(q, 14.sp) }
                }
            }
        }
    }
}

private fun meta(h: Hit): String {
    val words = if (h.words >= 10000) " · ${h.words / 10000} 万字" else ""
    return "${if (h.finished) "完结" else "连载"} · ${h.chapters} 章$words"
}

@Composable
private fun ResultsScreen(vm: AppVm) {
    val s = vm.status
    val searching = vm.busy && s.op == Op.SEARCH
    Column(Modifier.fillMaxSize()) {
        BackTitle("“${vm.query}”") { vm.tomatoView = TomatoView.Search }
        val line = when {
            searching -> "正在搜索…"
            s.op == Op.SEARCH && s.phase == Phase.FAILED -> s.message
            vm.hits.isEmpty() -> "没有找到"
            else -> "${vm.hits.size} 个结果"
        }
        T(line, 13.sp, modifier = Modifier.padding(start = 20.dp, bottom = 8.dp))
        LazyColumn(Modifier.fillMaxSize().padding(horizontal = 20.dp)) {
            items(vm.hits, key = { it.bookId }) { h ->
                Column(Modifier.fillMaxWidth().noRippleClick { vm.tomatoView = TomatoView.Detail(h) }) {
                    Row(Modifier.fillMaxWidth().padding(vertical = 10.dp)) {
                        Cover(h.title, h.thumb, Modifier.width(52.dp).height(70.dp), 10.sp)
                        Column(Modifier.weight(1f).padding(start = 14.dp)) {
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                T(h.title, 17.sp, bold = true, maxLines = 1, modifier = Modifier.weight(1f))
                                if (vm.owned(h.bookId) != null) T("已在书库", 12.sp, modifier = Modifier.padding(start = 8.dp))
                            }
                            T(h.author + if (h.category.isNotEmpty()) " · ${h.category}" else "", 13.sp, maxLines = 1,
                                modifier = Modifier.padding(top = 4.dp))
                            T(meta(h), 13.sp, modifier = Modifier.padding(top = 2.dp))
                        }
                    }
                    HLine()
                }
            }
        }
    }
}

@Composable
private fun DetailScreen(vm: AppVm, h: Hit, fromStore: Boolean, onOpenBook: (TomatoBook) -> Unit) {
    val s = vm.status
    val downloading = vm.busy && s.op == Op.DOWNLOAD
    val failed = s.op == Op.DOWNLOAD && s.phase == Phase.FAILED && !downloading
    val owned = vm.owned(h.bookId)
    Column(Modifier.fillMaxSize()) {
        BackTitle(if (fromStore) "书城" else "搜索结果") {
            vm.tomatoView = if (fromStore) TomatoView.Shelf else TomatoView.Results
        }
        Column(Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(horizontal = 20.dp), horizontalAlignment = Alignment.CenterHorizontally) {
            Cover(h.title, h.thumb, Modifier.padding(top = 8.dp).width(120.dp).height(160.dp), 16.sp)
            T(h.title, 22.sp, bold = true, align = TextAlign.Center, modifier = Modifier.padding(top = 14.dp))
            T(h.author, 15.sp, modifier = Modifier.padding(top = 6.dp))
            if (fromStore) {
                if (h.reads.isNotEmpty()) T("在读 ${h.reads}", 14.sp, modifier = Modifier.padding(top = 6.dp))
                if (h.channel.isNotEmpty()) T(h.channel, 14.sp, modifier = Modifier.padding(top = 4.dp))
            }
            Spacer(Modifier.height(16.dp))
            HLine()
            if (!fromStore) Row(Modifier.fillMaxWidth().padding(vertical = 10.dp)) {
                val words = if (h.words >= 10000) "${h.words / 10000} 万" else "${h.words}"
                listOf("分类" to h.category.ifEmpty { "—" }, "状态" to if (h.finished) "完结" else "连载",
                    "章节" to "${h.chapters}", "字数" to words).forEachIndexed { i, (k, v) ->
                    if (i > 0) Box(Modifier.width(1.dp).height(40.dp).background(Line).align(Alignment.CenterVertically))
                    Column(Modifier.weight(1f), horizontalAlignment = Alignment.CenterHorizontally) {
                        T(k, 12.sp)
                        T(v, 16.sp, bold = true, maxLines = 1, modifier = Modifier.padding(top = 4.dp))
                    }
                }
            }
            if (!fromStore) HLine()
            if (h.abstract.isNotEmpty())
                T(h.abstract, 15.sp, lineHeight = 23.sp, modifier = Modifier.fillMaxWidth().padding(top = 14.dp, bottom = 14.dp))
        }
        Column(Modifier.padding(20.dp)) {
            if (downloading) {
                val what = if (s.phase == Phase.QUEUED) "服务器正在生成 TXT…"
                    else if (s.phase == Phase.DOWNLOADING) "正在下载 · %.1f MB".format(s.bytes / 1048576.0) else "正在下载…"
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        T(what, 14.sp)
                        ProgressBar(if (s.phase == Phase.DOWNLOADING) 80 else if (s.phase == Phase.QUEUED) 40 else 10,
                            Modifier.padding(top = 8.dp))
                    }
                    T("取消", 15.sp, modifier = Modifier.noRippleClick { vm.cancel() }.padding(start = 16.dp))
                }
                return@Column
            }
            if (failed) T(s.message, 13.sp, modifier = Modifier.padding(bottom = 8.dp))
            Pill(if (owned != null) "开始阅读" else "下载到书库", true, Modifier.fillMaxWidth().height(48.dp)) {
                if (owned != null) onOpenBook(owned) else if (!vm.busy) vm.download(h)
            }
            if (fromStore) T("经番茄服务器生成 TXT，下载后在书库和番茄书架里都能看到", 12.sp, align = TextAlign.Center,
                modifier = Modifier.fillMaxWidth().padding(top = 8.dp))
        }
    }
}
