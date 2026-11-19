package com.treader.tomato.ui

import android.graphics.Paint
import android.graphics.Typeface
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
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
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.treader.tomato.AppVm
import com.treader.tomato.ReaderState
import com.treader.tomato.book.ChapterLayout
import com.treader.tomato.book.Style
import kotlinx.coroutines.delay

/*
 * 阅读页，照固件 app_book.c 的阅读视图：整屏是书，点左三成上一页、右三成下一页、中间开关工具条；左右滑也翻页，音量键也能翻。
 * 工具条：顶部“‹ 书库 · 书名”，底部本章名和字号、目录 / 排版 / 书库、A− 字号 A+、自动翻页、全书进度条（点进度条跳到那附近的章）。
 * The reader after the firmware's app_book.c reading view: the book fills the screen; tap the left 30% for the
 * previous page, the right 30% for the next, the middle toggles the toolbar; swipes and volume keys also turn pages.
 * Toolbar: "< Library · title" on top; at the bottom the chapter name and size, TOC / Layout / Library, A- size A+,
 * auto turn and a whole-book bar (tap it to jump near that chapter).
 */

/** 设备字号（像素）换成手机上的字号：×0.4 sp，48 约 19 sp。/ Device size (px) to phone size: x0.4 sp, 48 is ~19 sp. */
private fun pxToSp(px: Int) = px * 0.4f

private enum class Panel { NONE, TOOLS, TOC, LAYOUT }

@Composable
fun ReaderScreen(vm: AppVm, r: ReaderState) {
    var panel by remember { mutableStateOf(Panel.NONE) }
    var autoLevel by remember { mutableStateOf(0) }
    val style = vm.style
    val density = LocalDensity.current
    val turnSignal = vm.turnSignal

    BoxWithConstraints(Modifier.fillMaxSize().background(Paper)) {
        val marginPx = with(density) { intArrayOf(12, 20, 32)[style.margin.coerceIn(0, 2)].dp.toPx() }
        val topPx = with(density) { 16.dp.toPx() }
        val footPx = with(density) { 30.dp.toPx() }
        val wPx = constraints.maxWidth
        val hPx = constraints.maxHeight
        val bodyW = (wPx - 2 * marginPx).toInt()
        val bodyH = (hPx - topPx - footPx).toInt()
        val sizePx = with(density) { pxToSp(r.px).sp.toPx() }
        val paint = remember(sizePx, vm.serif) {
            Paint(Paint.ANTI_ALIAS_FLAG).apply {
                textSize = sizePx; color = android.graphics.Color.BLACK
                typeface = if (vm.serif) Typeface.SERIF else Typeface.DEFAULT
            }
        }
        val headingPaint = remember(sizePx, vm.serif) {
            Paint(paint).apply {
                textSize = sizePx + with(density) { pxToSp(8).sp.toPx() }
                typeface = Typeface.create(if (vm.serif) Typeface.SERIF else Typeface.DEFAULT, Typeface.BOLD)
            }
        }
        val rule = remember(density) { ChapterLayout.rulePaint(density.density) }
        val layout = remember(r.chapter, sizePx, bodyW, bodyH, style, vm.serif) {
            ChapterLayout(r.text(), r.blocks(), paint, headingPaint, bodyW, bodyH, style)
        }
        // 偏移 → 页；从后往前翻进这一章时停在最后一页。/ Offset to page; entering backwards lands on the last page.
        LaunchedEffect(layout, r.off, r.toEnd) {
            r.pageCount = layout.pageCount
            if (r.toEnd) {
                r.toEnd = false
                r.page = layout.pageCount - 1
                r.off = layout.pageStart(r.page)
            } else r.page = layout.pageFor(r.off)
            r.save()
        }

        fun turn(dir: Int) {
            if (panel != Panel.NONE) { panel = Panel.NONE; return }
            val next = r.page + dir
            when {
                next in 0 until layout.pageCount -> r.off = layout.pageStart(next)
                dir > 0 && r.chapter + 1 < r.source.chapterCount -> { r.chapter += 1; r.off = 0 }
                dir < 0 && r.chapter > 0 -> { r.chapter -= 1; r.off = 0; r.toEnd = true }
            }
        }
        LaunchedEffect(turnSignal.value) { turnSignal.value.second.let { if (it != 0) turn(it) } }
        // 自动翻页：3～30 秒一页，开着工具条时暂停，到书尾停下。/ Auto turn every 3-30 s; paused with a panel open.
        LaunchedEffect(autoLevel, panel, r.chapter, r.off) {
            if (autoLevel <= 0 || panel != Panel.NONE) return@LaunchedEffect
            delay(autoLevel * 3000L)
            if (r.chapter + 1 >= r.source.chapterCount && r.page + 1 >= layout.pageCount) autoLevel = 0 else turn(1)
        }

        Canvas(
            Modifier.fillMaxSize().pointerInput(panel) {
                detectTapGestures { p ->
                    when {
                        panel != Panel.NONE -> panel = Panel.NONE
                        p.x < size.width * 0.3f -> turn(-1)
                        p.x > size.width * 0.7f -> turn(1)
                        else -> panel = Panel.TOOLS
                    }
                }
            }.swipeTurn { turn(it) }
        ) {
            layout.draw(drawContext.canvas.nativeCanvas, r.page, marginPx, topPx, rule)
        }
        // 页脚：本章名，右边“页 / 页数　全书%”。/ Footer: chapter name, then "page / pages  book%".
        Row(Modifier.align(Alignment.BottomStart).fillMaxWidth().height(30.dp).padding(horizontal = 20.dp), verticalAlignment = Alignment.CenterVertically) {
            T(r.source.chapterTitle(r.chapter), 11.sp, color = Mid, maxLines = 1, modifier = Modifier.weight(1f))
            T("${r.page + 1} / ${layout.pageCount}　${r.pct()}%", 11.sp, color = Mid)
        }

        when (panel) {
            Panel.TOOLS -> Tools(vm, r, autoLevel, { autoLevel = it }) { panel = it }
            Panel.TOC -> Toc(r) { i -> r.chapter = i; r.off = 0; panel = Panel.NONE }
            Panel.LAYOUT -> LayoutPanel(vm) { panel = Panel.NONE }
            Panel.NONE -> {}
        }
    }
}

@Composable
private fun Tools(vm: AppVm, r: ReaderState, autoLevel: Int, setAuto: (Int) -> Unit, show: (Panel) -> Unit) {
    Column(Modifier.fillMaxSize()) {
        Row(Modifier.fillMaxWidth().background(Paper).height(52.dp).padding(horizontal = 16.dp), verticalAlignment = Alignment.CenterVertically) {
            T("‹ 书库", 15.sp, modifier = Modifier.noRippleClick { vm.closeReader() }.padding(end = 12.dp))
            T(r.book.title, 16.sp, bold = true, maxLines = 1, align = TextAlign.Center, modifier = Modifier.weight(1f))
            Spacer(Modifier.width(56.dp))
        }
        HLine(soft = false)
        Box(Modifier.weight(1f).fillMaxWidth().noRippleClick { show(Panel.NONE) })
        HLine(soft = false)
        Column(Modifier.fillMaxWidth().background(Paper).padding(horizontal = 20.dp, vertical = 12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                T(r.source.chapterTitle(r.chapter), 14.sp, maxLines = 1, modifier = Modifier.weight(1f))
                T("字号 ${r.px}", 14.sp)
            }
            Row(Modifier.fillMaxWidth().padding(vertical = 12.dp), horizontalArrangement = Arrangement.SpaceAround) {
                ToolButton("目录", { TocIcon() }) { show(Panel.TOC) }
                ToolButton("排版", { T("Aa", 17.sp, bold = true) }) { show(Panel.LAYOUT) }
                ToolButton("书库", { BooksIcon() }) { vm.closeReader() }
            }
            Row(verticalAlignment = Alignment.CenterVertically) {
                Pill("A−", false, Modifier.weight(1f), height = 40.dp) { if (r.px > ReaderState.PX_MIN) { r.px -= 4; vm.setDefaultPx(r.px) } }
                T("${r.px}", 18.sp, bold = true, align = TextAlign.Center, modifier = Modifier.width(72.dp))
                Pill("A+", false, Modifier.weight(1f), height = 40.dp) { if (r.px < ReaderState.PX_MAX) { r.px += 4; vm.setDefaultPx(r.px) } }
            }
            Row(Modifier.padding(top = 12.dp), verticalAlignment = Alignment.CenterVertically) {
                T("自动翻页", 14.sp, modifier = Modifier.weight(1f))
                Pill("−", false, Modifier.width(52.dp), height = 34.dp) { setAuto((autoLevel - 1).coerceAtLeast(0)) }
                T(if (autoLevel == 0) "关" else "${autoLevel * 3} 秒", 16.sp, bold = true, align = TextAlign.Center, modifier = Modifier.width(72.dp))
                Pill("+", false, Modifier.width(52.dp), height = 34.dp) { setAuto((autoLevel + 1).coerceAtMost(10)) }
            }
            Row(Modifier.padding(top = 14.dp), verticalAlignment = Alignment.CenterVertically) {
                Box(Modifier.weight(1f).height(24.dp).pointerInput(r.chapter) {
                    detectTapGestures { p ->
                        // 点进度条：跳到原文这个位置所在的章。/ Tap the bar: jump to the chapter at that source position.
                        val at = (p.x / size.width * r.source.total).toInt()
                        var c = 0
                        while (c + 1 < r.source.chapterCount && r.source.chapterSourceOffset(c + 1) <= at) c++
                        r.chapter = c; r.off = 0
                    }
                }, contentAlignment = Alignment.Center) { ProgressBar(r.pct()) }
                T("全书 ${r.pct()}%", 13.sp, modifier = Modifier.padding(start = 10.dp))
            }
        }
    }
}

@Composable
private fun ToolButton(label: String, icon: @Composable () -> Unit, onClick: () -> Unit) {
    Column(Modifier.width(72.dp).noRippleClick(onClick), horizontalAlignment = Alignment.CenterHorizontally) {
        Box(Modifier.height(26.dp), contentAlignment = Alignment.Center) { icon() }
        T(label, 13.sp, modifier = Modifier.padding(top = 2.dp))
    }
}

@Composable
private fun Toc(r: ReaderState, onPick: (Int) -> Unit) {
    val list = rememberLazyListState(initialFirstVisibleItemIndex = (r.chapter - 5).coerceAtLeast(0))
    Column(Modifier.fillMaxSize().background(Paper)) {
        Row(Modifier.fillMaxWidth().height(52.dp).padding(horizontal = 20.dp), verticalAlignment = Alignment.CenterVertically) {
            T("目录", 18.sp, bold = true, modifier = Modifier.weight(1f))
            T("${r.source.chapterCount} 章", 13.sp)
        }
        HLine(soft = false)
        LazyColumn(Modifier.fillMaxSize().padding(horizontal = 20.dp), state = list) {
            items(r.source.chapterCount) { i ->
                Column(Modifier.fillMaxWidth().noRippleClick { onPick(i) }) {
                    T(r.source.chapterTitle(i), 15.sp, bold = i == r.chapter, maxLines = 1, modifier = Modifier.padding(vertical = 12.dp))
                    HLine()
                }
            }
        }
    }
}

@Composable
private fun LayoutPanel(vm: AppVm, onDone: () -> Unit) {
    val s = vm.style
    Column(Modifier.fillMaxSize()) {
        Box(Modifier.weight(1f).fillMaxWidth().noRippleClick(onDone))
        HLine(soft = false)
        Column(Modifier.fillMaxWidth().background(Paper).padding(horizontal = 20.dp, vertical = 12.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                T("排版", 17.sp, bold = true, modifier = Modifier.weight(1f))
                Pill("完成", true, Modifier.width(80.dp), height = 34.dp, onClick = onDone)
            }
            val rows = listOf(
                Triple("行距", listOf("紧凑", "标准", "宽松"), s.line),
                Triple("段距", listOf("无", "小", "大"), s.para),
                Triple("页边距", listOf("窄", "中", "宽"), s.margin),
                Triple("首行缩进", listOf("按原文", "缩进两字"), if (s.indent) 1 else 0),
                Triple("下划线", listOf("无", "有"), if (s.underline) 1 else 0),
            )
            rows.forEachIndexed { row, (name, choices, value) ->
                Row(Modifier.padding(top = 10.dp), verticalAlignment = Alignment.CenterVertically) {
                    T(name, 14.sp, modifier = Modifier.width(80.dp))
                    choices.forEachIndexed { opt, label ->
                        Pill(label, opt == value, Modifier.weight(1f).padding(start = if (opt > 0) 8.dp else 0.dp), height = 34.dp) {
                            vm.changeStyle(when (row) {
                                0 -> s.copy(line = opt); 1 -> s.copy(para = opt); 2 -> s.copy(margin = opt)
                                3 -> s.copy(indent = opt == 1); else -> s.copy(underline = opt == 1)
                            })
                        }
                    }
                    if (choices.size == 2) Spacer(Modifier.weight(1f).padding(start = 8.dp))
                }
            }
        }
    }
}

/** 目录图标：三个点三条线。/ TOC glyph: three dots and three lines. */
@Composable
fun TocIcon() = Canvas(Modifier.width(24.dp).height(24.dp)) {
    val s = size.width / 40f
    for (i in 0 until 3) {
        drawCircle(Ink, 2.2f * s, androidx.compose.ui.geometry.Offset(8 * s, (10 + i * 10) * s))
        drawLine(Ink, androidx.compose.ui.geometry.Offset(15 * s, (10 + i * 10) * s), androidx.compose.ui.geometry.Offset(34 * s, (10 + i * 10) * s), 2.6f * s)
    }
}
