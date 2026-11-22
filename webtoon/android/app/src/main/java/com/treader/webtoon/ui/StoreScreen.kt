package com.treader.webtoon.ui

import androidx.compose.foundation.horizontalScroll
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
import androidx.compose.foundation.rememberScrollState
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
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.treader.webtoon.AppVm
import com.treader.webtoon.Detail
import com.treader.webtoon.STORE_SEGS
import com.treader.webtoon.Tab
import com.treader.webtoon.data.Card
import com.treader.webtoon.data.Start

/*
 * 发现页，照固件发现漫画：搜索栏（服务器把简体关键词转成繁体再搜）、排行 / 类型 / 星期 / 完结四个分段，二级分段用服务器
 * 返回的 keys，下面三列封面网格分页。只在进发现页、换分段、搜索时联网。
 * The store after the firmware's comic store: a search bar (the server turns simplified keywords into traditional),
 * the Rank / Genre / Day / Done segments with second-level keys from the server, and a paged three-column cover grid.
 * Network use only on entering, switching segments and searching.
 */

@Composable
fun StoreTab(vm: AppVm) {
    LaunchedEffect(vm.configured) { vm.storeEnter() }
    val focus = LocalFocusManager.current
    var text by remember { mutableStateOf(vm.query) }
    Column(Modifier.fillMaxSize()) {
        TitleRow("发现", if (vm.storeBusy) "加载中…" else "刷新") {
            if (vm.searchResults != null) vm.search(vm.query) else vm.loadStore(force = true)
        }
        SearchBar(Modifier.padding(horizontal = 20.dp)) {
            Box(Modifier.weight(1f)) {
                if (text.isEmpty()) T("搜作品名、作者", 15.sp, color = Mid)
                BasicTextField(
                    text, { text = it }, singleLine = true, textStyle = TextStyle(fontSize = 15.sp, color = Ink),
                    keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
                    keyboardActions = KeyboardActions(onSearch = { focus.clearFocus(); vm.search(text) }),
                    modifier = Modifier.fillMaxWidth(),
                )
            }
            if (vm.searchResults != null || text.isNotEmpty()) {
                T("清除", 14.sp, modifier = Modifier.padding(start = 10.dp).noRippleClick { text = ""; focus.clearFocus(); vm.closeSearch() })
            }
        }
        if (!vm.configured) {
            T("先到“设置”填 webtoon 服务器地址和设备令牌。", 14.sp, color = Mid, modifier = Modifier.padding(20.dp))
            Pill("去设置", true, Modifier.padding(horizontal = 20.dp).fillMaxWidth()) { vm.tab = Tab.SETTINGS }
            return
        }
        val results = vm.searchResults
        val items: List<Card>
        if (results != null) {
            T("搜索“${vm.query}”" + if (!vm.storeBusy) " · ${results.size} 部" else "", 14.sp, color = Mid,
                modifier = Modifier.padding(start = 20.dp, end = 20.dp, top = 12.dp))
            items = results
        } else {
            Segment(STORE_SEGS.map { it.second }, vm.storeSeg, { vm.loadStore(it, "") },
                Modifier.padding(start = 20.dp, end = 20.dp, top = 12.dp))
            val keys = vm.storeList?.keys.orEmpty()
            if (keys.isNotEmpty()) Row(
                Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(start = 20.dp, end = 20.dp, top = 10.dp),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                keys.forEach { k -> Chip(k.name, k.key == vm.storeKey) { vm.loadStore(vm.storeSeg, k.key) } }
            }
            items = vm.storeList?.items.orEmpty()
        }
        if (vm.storeMsg.isNotEmpty()) T(vm.storeMsg, 13.sp, color = Mid, modifier = Modifier.padding(horizontal = 20.dp, vertical = 8.dp))
        if (items.isEmpty()) {
            T(if (vm.storeBusy) "正在加载…" else if (results != null) "没有找到" else "", 14.sp, color = Mid, modifier = Modifier.padding(20.dp))
            return
        }
        PagedGrid(items, vm.storePage, { vm.storePage = it }) { c ->
            val owned = vm.subscribed(c.id) != null
            CoverCell(c.title, if (owned) "已订阅" else c.author.ifEmpty { c.genre }, vm.api.coverUrl(c.id), vm.images, subBold = owned) {
                vm.openDetail(c.id, c.title)
            }
        }
    }
}

/**
 * 作品详情：封面、作者、类型、免费话数、简介；订阅（选从第 1 话 / 只追新的 / 最近 5 话）或取消订阅（问要不要一起删掉
 * 已下载的话，默认保留）。
 * Title details: cover, author, genre, free episode count and summary; subscribe (from episode 1 / new only / latest
 * five) or unsubscribe (asking whether to delete downloaded episodes too, keeping them by default).
 */
@Composable
fun DetailScreen(vm: AppVm, d: Detail) {
    var ask by remember { mutableStateOf<String?>(null) }   // "sub" / "unsub"
    val sub = vm.subscribed(d.id)
    val t = d.title
    Box(Modifier.fillMaxSize()) {
        Column(Modifier.fillMaxSize()) {
            BackTitle(t?.title ?: d.name) { vm.detail = null }
            Column(Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(horizontal = 20.dp)) {
                Row {
                    Cover(t?.title ?: d.name, vm.api.coverUrl(d.id, d.lang), vm.images, Modifier.width(105.dp).height(140.dp), 13.sp)
                    Column(Modifier.padding(start = 16.dp).weight(1f), verticalArrangement = Arrangement.spacedBy(6.dp)) {
                        T(t?.title ?: d.name, 18.sp, bold = true, maxLines = 3)
                        if (t != null) {
                            if (t.author.isNotEmpty()) T(t.author, 14.sp, color = Mid, maxLines = 2)
                            if (t.genre.isNotEmpty()) T(t.genre, 14.sp, color = Mid, maxLines = 1)
                            T("免费 ${t.episodes.size} 话 · " + if (t.done) "已完结" else "连载中", 14.sp, maxLines = 1)
                            if (sub != null) T("已订阅 · 下载到第 ${sub.mark?.last ?: 0} 话", 14.sp, bold = true, maxLines = 1)
                        }
                    }
                }
                Spacer(Modifier.height(16.dp))
                when {
                    t != null -> T(t.summary, 14.sp, lineHeight = 22.sp)
                    d.error.isNotEmpty() -> T(d.error, 14.sp, color = Mid)
                    else -> T("正在加载…", 14.sp, color = Mid)
                }
                Spacer(Modifier.height(16.dp))
            }
            Row(Modifier.padding(start = 20.dp, end = 20.dp, bottom = 14.dp), horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                if (sub != null) {
                    Pill("去我的漫画", true, Modifier.weight(1f)) {
                        vm.detail = null; vm.tab = Tab.SHELF; vm.openSeries = sub.dir; vm.seriesPage = 0
                    }
                    Pill("取消订阅", false, Modifier.weight(1f)) { ask = "unsub" }
                } else if (t != null) {
                    Pill(if (t.episodes.isEmpty()) "订阅（还没有免费话）" else "订阅", true, Modifier.weight(1f)) { ask = "sub" }
                } else if (d.error.isNotEmpty()) {
                    Pill("重试", false, Modifier.weight(1f)) { vm.openDetail(d.id, d.name, d.lang) }
                }
            }
        }
        when (ask) {
            "sub" -> ChoiceDialog(
                "订阅“${t?.title ?: d.name}”", "从哪一话开始下载？选好就在后台开始下；以后有新话，在我的漫画点“检查更新”。",
                Start.entries.map { s ->
                    Choice(s.label, s == Start.LAST5) { ask = null; vm.subscribe(d, s) }
                } + Choice("取消") { ask = null },
            ) { ask = null }
            "unsub" -> ChoiceDialog(
                "取消订阅？", "已下载的 ${sub?.episodes?.size ?: 0} 话可以留在我的漫画里当本地漫画看。",
                listOf(
                    Choice("取消订阅，保留已下载的", true) { ask = null; sub?.let { vm.unsubscribe(it, false) } },
                    Choice("取消订阅并删除已下载的") { ask = null; sub?.let { vm.unsubscribe(it, true) } },
                    Choice("不取消") { ask = null },
                ),
            ) { ask = null }
        }
    }
}
