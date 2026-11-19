package com.treader.tomato.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.itemsIndexed
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.treader.tomato.AppVm
import com.treader.tomato.TomatoView

/*
 * 书城，照固件 app_tr_store.c：“榜单 日期 · 刷新”，男频 / 女频，分类胶囊；点分类进榜单（三列封面网格，“名次 书名”、
 * “在读 N” 或 “已在书架”），点书进详情下载。只在点“刷新”、打开没缓存的分类时联网。
 * The bookstore after the firmware's app_tr_store.c: "Ranks <date> · Refresh", male / female, category pills; a
 * category opens its rank list (a three-column cover grid with "rank title" and "readers" or "on shelf"), a book opens
 * its detail to download. Network use only on Refresh or an uncached category.
 */

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun StoreTab(vm: AppVm) {
    LaunchedEffect(Unit) { vm.storeEnter() }
    val cat = vm.storeCat
    if (cat != null) { RankScreen(vm); return }
    val index = vm.storeIndex
    Column(Modifier.fillMaxSize()) {
        TitleRow("番茄小说")
        Segment(listOf("书架", "书城"), vm.tomatoSeg, { vm.tomatoSeg = it }, Modifier.padding(horizontal = 20.dp))
        Row(Modifier.fillMaxWidth().padding(start = 20.dp, end = 20.dp, top = 12.dp), verticalAlignment = Alignment.CenterVertically) {
            T(if (index != null) "榜单 ${index.first}" else "榜单", 14.sp, modifier = Modifier.weight(1f))
            Row(Modifier.noRippleClick { vm.storeRefresh() }, verticalAlignment = Alignment.CenterVertically) {
                SyncIcon(Modifier.padding(end = 4.dp))
                T(if (vm.storeBusy) "刷新中…" else "刷新", 14.sp)
            }
        }
        Row(Modifier.padding(start = 20.dp, top = 12.dp), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            Chip("男频", vm.storeMale) { vm.storeMale = true }
            Chip("女频", !vm.storeMale) { vm.storeMale = false }
        }
        HLine(modifier = Modifier.padding(start = 20.dp, end = 20.dp, top = 12.dp))
        if (vm.storeMsg.isNotEmpty()) T(vm.storeMsg, 13.sp, modifier = Modifier.padding(20.dp))
        if (index == null) {
            if (vm.storeBusy) T("正在取榜单…", 14.sp, modifier = Modifier.padding(20.dp))
            return
        }
        FlowRow(
            Modifier.verticalScroll(rememberScrollState()).padding(20.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp), verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            index.second.filter { it.male == vm.storeMale }.forEach { c ->
                Chip(c.name, false) { vm.openCat(c) }
            }
        }
    }
}

@Composable
private fun RankScreen(vm: AppVm) {
    val cat = vm.storeCat ?: return
    Column(Modifier.fillMaxSize()) {
        Row(Modifier.fillMaxWidth().height(56.dp).padding(horizontal = 20.dp), verticalAlignment = Alignment.CenterVertically) {
            T("‹", 24.sp, modifier = Modifier.noRippleClick { vm.storeCat = null }.padding(end = 14.dp))
            T(cat.name, 18.sp, bold = true, maxLines = 1, modifier = Modifier.weight(1f))
            T("${if (cat.male) "男频" else "女频"} · ${vm.storeIndex?.first.orEmpty()}", 13.sp)
        }
        HLine(soft = false, modifier = Modifier.padding(horizontal = 20.dp))
        if (vm.storeMsg.isNotEmpty()) T(vm.storeMsg, 13.sp, modifier = Modifier.padding(20.dp))
        if (vm.storeBooks.isEmpty() && vm.storeBusy) T("正在取榜单…", 14.sp, modifier = Modifier.padding(20.dp))
        LazyVerticalGrid(
            GridCells.Fixed(3), Modifier.fillMaxSize().padding(horizontal = 14.dp),
            horizontalArrangement = Arrangement.spacedBy(10.dp), verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            itemsIndexed(vm.storeBooks, key = { _, b -> b.bookId }) { i, b ->
                Column(Modifier.padding(top = if (i < 3) 12.dp else 0.dp).noRippleClick { vm.tomatoView = TomatoView.Detail(vm.storeHit(b), fromStore = true) }) {
                    Box(Modifier.fillMaxWidth().aspectRatio(0.75f)) {
                        Cover(b.title, b.cover, Modifier.fillMaxSize(), 12.sp)
                    }
                    T("${i + 1} ${b.title}", 13.sp, bold = true, maxLines = 1, modifier = Modifier.padding(top = 4.dp))
                    T(if (vm.owned(b.bookId) != null) "已在书架" else "在读 ${b.reads}", 12.sp, maxLines = 1)
                }
            }
        }
    }
}
