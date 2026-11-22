package com.treader.webtoon.ui

import android.graphics.Bitmap
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.treader.webtoon.AppVm
import com.treader.webtoon.Reading

/*
 * 阅读页，照固件漫画阅读：一屏一页（服务器已经切成 648×1000 的灰度页），点左三成上一页、右三成下一页、中间开关工具条；
 * 左右滑、音量键也翻页。翻过最后一页是“本话完”，可以直接看下一话。每翻一页记一次进度。
 * The reader after the firmware's comic reader: one page per screen (the server already cut 648x1000 grayscale
 * pages); tap the left 30% for the previous page, the right 30% for the next, the middle toggles the toolbar; swipes
 * and volume keys turn too. Past the last page comes "end of episode" with the next episode one tap away. Progress is
 * saved on every turn.
 */

@Composable
fun ReaderScreen(vm: AppVm, r: Reading) {
    var tools by remember(r) { mutableStateOf(false) }
    // 当前页和前后各一页的解码结果；翻页时先从这里拿，不用等解码。/ Decoded current and neighbouring pages; a turn
    // takes them from here without waiting for a decode.
    val cache = remember(r) { HashMap<Int, Bitmap>() }
    var shown by remember(r) { mutableStateOf<Bitmap?>(null) }
    var failed by remember(r) { mutableStateOf(false) }

    LaunchedEffect(r, r.page) {
        val p = r.page
        failed = false
        shown = cache[p] ?: vm.pageBitmap(r, p)?.also { cache[p] = it }
        if (shown == null) failed = true
        cache.keys.retainAll { it in p - 1..p + 1 }
        for (n in listOf(p + 1, p - 1)) {
            if (n in 0 until r.cbz.count && n !in cache) vm.pageBitmap(r, n)?.let { if (r.page in n - 1..n + 1) cache[n] = it }
        }
    }

    fun turn(dir: Int) {
        if (tools) { tools = false; return }
        when {
            r.atEnd -> if (dir < 0) r.atEnd = false else vm.readNext()
            r.page + dir in 0 until r.cbz.count -> { r.page += dir; vm.saveProgress() }
            dir > 0 -> { r.atEnd = true; vm.saveProgress() }
        }
    }
    val turnNow by rememberUpdatedState { dir: Int -> turn(dir) }
    // 只理会打开之后的音量键（信号里留着上次阅读的值）。/ Only volume keys pressed after opening count (the signal
    // still holds the last reading's value).
    val signalAtOpen = remember { vm.turnSignal.value.first }
    LaunchedEffect(vm.turnSignal.value) {
        val (n, dir) = vm.turnSignal.value
        if (n != signalAtOpen && dir != 0) turnNow(dir)
    }

    Box(
        Modifier.fillMaxSize().background(Paper).swipeTurn { turnNow(it) }.pointerInput(r) {
            detectTapGestures { p ->
                when {
                    p.x < size.width * 0.3f -> turnNow(-1)
                    p.x > size.width * 0.7f -> turnNow(1)
                    else -> tools = !tools
                }
            }
        },
        contentAlignment = Alignment.Center,
    ) {
        if (r.atEnd) {
            EndPanel(vm, r)
        } else {
            val bmp = shown
            if (bmp != null) Image(bmp.asImageBitmap(), null, Modifier.fillMaxSize(), contentScale = ContentScale.Fit)
            else if (failed) T("这一页打不开", 15.sp, color = Mid)
        }
        if (tools) Toolbar(vm, r) { tools = false }
    }
}

@Composable
private fun EndPanel(vm: AppVm, r: Reading) {
    Column(Modifier.fillMaxWidth().padding(28.dp), horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(14.dp)) {
        T("本话完", 22.sp, bold = true)
        T(epLabel(r.episode.no, r.episode.name), 14.sp, color = Mid, align = TextAlign.Center, maxLines = 2)
        val next = r.next
        if (next != null) {
            Pill("下一话：${epLabel(next.no, next.name)}", true, Modifier.fillMaxWidth()) { vm.readNext() }
        } else {
            T(if (r.series.subscribed) "已经是下载的最后一话。回我的漫画点“检查更新”看有没有新话。" else "已经是最后一话。",
                14.sp, color = Mid, align = TextAlign.Center, lineHeight = 20.sp)
        }
        Pill("回我的漫画", false, Modifier.fillMaxWidth()) { vm.closeReader() }
    }
}

/** 工具条：上面“‹ 我的漫画 · 第 N 话”，下面页码和进度条、上一话 / 下一话。/ Toolbar: "< Shelf · episode" on top, the page
 *  number, a progress bar and previous / next episode below. */
@Composable
private fun Toolbar(vm: AppVm, r: Reading, onClose: () -> Unit) {
    Column(Modifier.fillMaxSize()) {
        Row(Modifier.fillMaxWidth().height(56.dp).background(Paper).noRippleClick { vm.closeReader() }
            .padding(horizontal = 20.dp), verticalAlignment = Alignment.CenterVertically) {
            T("‹", 24.sp, modifier = Modifier.padding(end = 14.dp))
            T("我的漫画 · ${r.series.name} · ${epLabel(r.episode.no, r.episode.name)}", 16.sp, bold = true, maxLines = 1)
        }
        HLine(soft = false)
        Box(Modifier.weight(1f).fillMaxWidth().noRippleClick(onClose))
        HLine(soft = false)
        Column(Modifier.fillMaxWidth().background(Paper).padding(horizontal = 20.dp, vertical = 12.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp)) {
            T("${r.page + 1} / ${r.cbz.count}", 15.sp, bold = true)
            // 点进度条跳到那一页（照固件），点按区比细线高，好点中；工具条留着，可以接着点。
            // Tap the bar to jump to that page (as on the firmware); the tap area is taller than the thin line so it is
            // easy to hit, and the toolbar stays open for another tap.
            Box(Modifier.fillMaxWidth().height(36.dp).pointerInput(r, r.cbz.count) {
                detectTapGestures { o ->
                    val n = r.cbz.count
                    if (n > 1 && size.width > 0) {
                        r.page = Math.round(o.x / size.width * (n - 1)).coerceIn(0, n - 1)
                        r.atEnd = false
                        vm.saveProgress()
                    }
                }
            }, contentAlignment = Alignment.Center) {
                ProgressBar(if (r.cbz.count <= 1) 100 else r.page * 100 / (r.cbz.count - 1))
            }
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                val prev = r.series.episodes.getOrNull(r.index - 1)
                Pill("上一话", false, Modifier.weight(1f)) { if (prev != null) { vm.saveProgress(); vm.read(r.series, r.index - 1) } }
                Pill("第一页", false, Modifier.weight(1f)) { r.page = 0; vm.saveProgress(); onClose() }
                Pill("下一话", r.next != null, Modifier.weight(1f)) { vm.readNext() }
            }
        }
    }
}
