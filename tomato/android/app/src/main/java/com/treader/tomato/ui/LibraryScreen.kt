package com.treader.tomato.ui

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.treader.tomato.AppVm
import com.treader.tomato.Tab
import com.treader.tomato.data.LibBook
import java.io.File

/*
 * 书库，照固件 app_book.c 的书架：标题行（搜索、导入），“N 本 · 排序”和番茄更新提示，全部 / 番茄 / 其他书筛选、
 * 按最近 / 按书名排序、管理；顶部“继续阅读”卡片；每本一行：封面、书名、来源、进度条。长按一本看详情、清进度、删除。
 * The library after the firmware's app_book.c shelf: title row (search, import), "N books · sort" and the Tomato
 * update hint, All / Tomato / Other filters, recent / title sort and manage; a "continue reading" card on top; one row
 * per book with cover, title, source and a progress bar. Long press a book for details, clearing progress or deleting.
 */

private val FILTERS = listOf("全部", "番茄", "其他书")

@Composable
fun LibraryTab(vm: AppVm) {
    var filter by remember { mutableStateOf(0) }
    var byTitle by remember { mutableStateOf(false) }
    var searching by remember { mutableStateOf(false) }
    var query by remember { mutableStateOf("") }
    var manage by remember { mutableStateOf(false) }
    var picked by remember { mutableStateOf(setOf<String>()) }
    var detail by remember { mutableStateOf<LibBook?>(null) }
    var confirm by remember { mutableStateOf<Pair<String, () -> Unit>?>(null) }
    val pick = rememberLauncherForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        if (uris.isNotEmpty()) vm.importBooks(uris)
    }

    val all = vm.books
    val shown = all.filter {
        when (filter) { 1 -> it.tomato != null; 2 -> it.tomato == null; else -> true }
    }.filter { query.isBlank() || matches(it, query) }
        .let { l -> if (byTitle) l.sortedBy { it.title } else l.sortedWith(compareByDescending<LibBook> { it.progress?.seq ?: 0 }.thenBy { it.title }) }
    val cont = all.firstOrNull { it.key == vm.progressStore.last && it.progress != null }

    Box(Modifier.fillMaxSize()) {
        Column(Modifier.fillMaxSize()) {
            Row(Modifier.fillMaxWidth().padding(start = 20.dp, end = 12.dp, top = 14.dp), verticalAlignment = Alignment.CenterVertically) {
                T("书库", 24.sp, bold = true, modifier = Modifier.weight(1f))
                Box(Modifier.noRippleClick { searching = !searching; if (!searching) query = "" }.padding(8.dp)) { SearchIcon() }
                Box(Modifier.noRippleClick { pick.launch(arrayOf("text/plain", "application/epub+zip", "application/octet-stream")) }.padding(8.dp)) {
                    T("导入", 15.sp)
                }
            }
            Row(Modifier.fillMaxWidth().padding(horizontal = 20.dp), verticalAlignment = Alignment.CenterVertically) {
                T("${all.size} 本 · ${if (byTitle) "按书名" else "按最近阅读"}", 13.sp, modifier = Modifier.weight(1f))
                val pending = vm.pending
                if (pending > 0) T("番茄 $pending 本有更新 ›", 13.sp, bold = true,
                    modifier = Modifier.noRippleClick { vm.tab = Tab.TOMATO })
            }
            if (searching) {
                Row(
                    Modifier.padding(start = 20.dp, end = 20.dp, top = 10.dp).fillMaxWidth().height(40.dp).clip(RoundedCornerShape(20.dp))
                        .border(1.5.dp, Ink, RoundedCornerShape(20.dp)).padding(horizontal = 14.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    SearchIcon(modifier = Modifier.padding(end = 8.dp))
                    BasicTextField(query, { query = it }, singleLine = true, textStyle = TextStyle(fontSize = 16.sp, color = Ink),
                        modifier = Modifier.weight(1f))
                }
            }
            Row(Modifier.padding(start = 20.dp, end = 20.dp, top = 10.dp), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Pill(FILTERS[filter], false, Modifier.width(84.dp), height = 32.dp) { filter = (filter + 1) % FILTERS.size }
                Pill(if (byTitle) "按书名" else "按最近", false, Modifier.width(84.dp), height = 32.dp) { byTitle = !byTitle }
                Pill(if (manage) "完成" else "管理", manage, Modifier.width(84.dp), height = 32.dp) { manage = !manage; picked = emptySet() }
            }
            if (vm.libMessage.isNotEmpty()) T(vm.libMessage, 13.sp, modifier = Modifier.padding(start = 20.dp, top = 8.dp))
            if (vm.opening) T("正在加载和排版…", 13.sp, modifier = Modifier.padding(start = 20.dp, top = 8.dp))
            if (manage) {
                Row(Modifier.padding(start = 20.dp, end = 20.dp, top = 10.dp), verticalAlignment = Alignment.CenterVertically) {
                    T("已选 ${picked.size} 本", 13.sp, modifier = Modifier.weight(1f))
                    T("清进度", 14.sp, modifier = Modifier.noRippleClick {
                        if (picked.isNotEmpty()) confirm = "清除 ${picked.size} 本的阅读进度？文件保留。" to {
                            vm.clearProgress(all.filter { it.name in picked }); picked = emptySet()
                        }
                    }.padding(horizontal = 10.dp))
                    T("删除所选", 14.sp, bold = true, modifier = Modifier.noRippleClick {
                        if (picked.isNotEmpty()) confirm = "删除 ${picked.size} 本图书？删除后文件无法恢复。" to {
                            vm.delete(all.filter { it.name in picked }); picked = emptySet()
                        }
                    })
                }
            }
            Spacer(Modifier.height(8.dp))
            if (all.isEmpty()) {
                T("暂无图书。在番茄里搜索下载，或者点右上角“导入”手机里的 TXT、EPUB。", 15.sp, lineHeight = 23.sp,
                    modifier = Modifier.padding(20.dp))
                return@Column
            }
            LazyColumn(Modifier.fillMaxSize().padding(horizontal = 20.dp)) {
                if (cont != null && !manage && query.isBlank() && filter == 0) item("continue") { ContinueCard(vm, cont) }
                items(shown, key = { it.name }) { b ->
                    BookRow(vm, b, manage, b.name in picked,
                        onClick = {
                            if (manage) picked = if (b.name in picked) picked - b.name else picked + b.name
                            else vm.open(b.file)
                        },
                        onLong = { detail = b })
                }
                if (shown.isEmpty()) item { T("当前筛选没有图书", 14.sp, modifier = Modifier.padding(vertical = 16.dp)) }
            }
        }
        detail?.let { b ->
            DetailDialog(vm, b, onClose = { detail = null },
                onClear = { confirm = "确认清除进度？仅清阅读进度，保留图书文件。" to { vm.clearProgress(listOf(b)) }; detail = null },
                onDelete = { confirm = "确认删除文件？删除后文件无法恢复。" to { vm.delete(listOf(b)) }; detail = null })
        }
        confirm?.let { (text, action) ->
            Dialog(onClose = { confirm = null }) {
                T(text, 15.sp, lineHeight = 22.sp)
                Row(Modifier.padding(top = 16.dp), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    Pill("取消", false, Modifier.weight(1f)) { confirm = null }
                    Pill("确认", true, Modifier.weight(1f)) { action(); confirm = null }
                }
            }
        }
    }
}

// 书名或文件名里找（拼音、首字母在第五阶段前先不做）。/ Match title or file name.
private fun matches(b: LibBook, q: String): Boolean {
    val t = q.trim().lowercase()
    return b.title.lowercase().contains(t) || b.name.lowercase().contains(t) ||
        (b.tomato?.author?.lowercase()?.contains(t) ?: false)
}

private fun source(b: LibBook): String = when {
    b.tomato != null -> "番茄 · ${b.tomato.author}"
    b.epub -> "EPUB"
    else -> "TXT"
}

@Composable
private fun BookCover(vm: AppVm, b: LibBook, modifier: Modifier) {
    val img = File(vm.dirs.covers, "${b.name}.img")
    Cover(b.title, if (img.exists()) img else null, modifier, 10.sp)
}

@Composable
private fun ContinueCard(vm: AppVm, b: LibBook) {
    Row(
        Modifier.fillMaxWidth().padding(bottom = 6.dp).clip(RoundedCornerShape(10.dp)).border(1.5.dp, Ink, RoundedCornerShape(10.dp))
            .noRippleClick { vm.open(b.file) }.padding(12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        BookCover(vm, b, Modifier.width(36.dp).height(48.dp))
        Column(Modifier.weight(1f).padding(start = 12.dp)) {
            Row { T("继续阅读", 12.sp, modifier = Modifier.weight(1f)); T("接着读 →", 12.sp, bold = true) }
            T(b.title, 16.sp, bold = true, maxLines = 1, modifier = Modifier.padding(top = 2.dp))
            Row(Modifier.padding(top = 6.dp), verticalAlignment = Alignment.CenterVertically) {
                ProgressBar(b.pct.coerceAtLeast(0), Modifier.weight(1f))
                T("${b.pct.coerceAtLeast(0)}%", 12.sp, modifier = Modifier.padding(start = 8.dp))
            }
        }
    }
}

@Composable
private fun BookRow(vm: AppVm, b: LibBook, manage: Boolean, picked: Boolean, onClick: () -> Unit, onLong: () -> Unit) {
    Column(Modifier.fillMaxWidth().longPress(onClick, onLong)) {
        Row(Modifier.fillMaxWidth().padding(vertical = 10.dp), verticalAlignment = Alignment.CenterVertically) {
            if (manage) Box(
                Modifier.padding(end = 12.dp).width(20.dp).height(20.dp).clip(RoundedCornerShape(4.dp))
                    .border(1.5.dp, Ink, RoundedCornerShape(4.dp)).background(if (picked) Ink else Paper),
            )
            BookCover(vm, b, Modifier.width(40.dp).height(54.dp))
            Column(Modifier.weight(1f).padding(start = 14.dp)) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    T(b.title, 17.sp, bold = true, maxLines = 1, modifier = Modifier.weight(1f))
                    b.tomato?.takeIf { it.pending }?.let { T("+${it.remote - it.chapters} 章", 13.sp, bold = true) }
                }
                T(source(b), 13.sp, maxLines = 1, modifier = Modifier.padding(top = 2.dp))
                Row(Modifier.padding(top = 6.dp), verticalAlignment = Alignment.CenterVertically) {
                    when {
                        b.pct < 0 -> T("未读", 12.sp, color = Mid)
                        b.pct >= 100 -> T("读完", 12.sp)
                        else -> {
                            ProgressBar(b.pct, Modifier.weight(1f))
                            T("${b.pct}%", 12.sp, modifier = Modifier.padding(start = 8.dp))
                        }
                    }
                }
            }
        }
        HLine()
    }
}

@Composable
private fun DetailDialog(vm: AppVm, b: LibBook, onClose: () -> Unit, onClear: () -> Unit, onDelete: () -> Unit) {
    Dialog(onClose) {
        T("图书详情", 17.sp, bold = true)
        T(b.title, 16.sp, modifier = Modifier.padding(top = 10.dp))
        T("%s · %s · %.2f MB".format(source(b), b.name.substringAfterLast('.').uppercase(), b.file.length() / 1048576.0),
            13.sp, color = Mid, modifier = Modifier.padding(top = 4.dp))
        T(if (b.pct < 0) "尚无阅读进度" else "已读 ${b.pct}%", 13.sp, modifier = Modifier.padding(top = 4.dp))
        Row(Modifier.padding(top = 16.dp), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            Pill("清进度", false, Modifier.weight(1f), onClick = onClear)
            Pill("删除文件", false, Modifier.weight(1f), onClick = onDelete)
        }
        Pill("关闭", true, Modifier.fillMaxWidth().padding(top = 10.dp), onClick = onClose)
        @Suppress("UNUSED_EXPRESSION") vm
    }
}

/** 黑框白底的对话框，点外面关掉。/ A black-framed dialog; tapping outside closes it. */
@Composable
fun Dialog(onClose: () -> Unit, content: @Composable () -> Unit) {
    Box(Modifier.fillMaxSize().background(Paper.copy(alpha = 0.7f)).noRippleClick(onClose), contentAlignment = Alignment.Center) {
        Column(
            Modifier.padding(horizontal = 28.dp).fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(Paper)
                .border(2.dp, Ink, RoundedCornerShape(12.dp)).noRippleClick {}.padding(18.dp),
        ) { content() }
    }
}
