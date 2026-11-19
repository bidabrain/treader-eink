package com.treader.tomato.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/*
 * 和固件 tr_ui 一样的黑白原语：只有黑、白和一级浅灰，没有颜色和动画。手机上大约按设备像素的一半换成 dp。
 * The firmware tr_ui primitives in black and white: black, white and one light gray, no colors or animation. Device
 * pixels map to roughly half as many dp on a phone.
 */

val Ink = Color.Black
val Paper = Color.White
val Mid = Color(0xFF6B6B6B)
val Line = Color(0xFFC8C8C8)

fun Modifier.noRippleClick(onClick: () -> Unit): Modifier = this.then(
    Modifier.clickable(interactionSource = MutableInteractionSource(), indication = null, onClick = onClick)
)

@Composable
fun T(
    text: String,
    size: TextUnit = 15.sp,
    bold: Boolean = false,
    color: Color = Ink,
    maxLines: Int = Int.MAX_VALUE,
    align: TextAlign? = null,
    lineHeight: TextUnit = TextUnit.Unspecified,
    modifier: Modifier = Modifier,
) = Text(
    text, modifier = modifier, maxLines = maxLines, overflow = TextOverflow.Ellipsis, textAlign = align,
    style = TextStyle(fontSize = size, fontWeight = if (bold) FontWeight.Bold else FontWeight.Medium, color = color,
        lineHeight = lineHeight),
)

/** 细线；soft 用浅灰。/ Hairline; soft uses light gray. */
@Composable
fun HLine(soft: Boolean = true, modifier: Modifier = Modifier) =
    Box(modifier.fillMaxWidth().height(1.dp).background(if (soft) Line else Ink))

/** 分段按钮，active 那段黑底白字（和固件 tr_segment 一样）。/ Segmented control, active segment filled. */
@Composable
fun Segment(labels: List<String>, active: Int, onPick: (Int) -> Unit, modifier: Modifier = Modifier) {
    Row(
        modifier.fillMaxWidth().height(34.dp).clip(RoundedCornerShape(17.dp)).border(1.5.dp, Ink, RoundedCornerShape(17.dp))
    ) {
        labels.forEachIndexed { i, label ->
            val on = i == active
            Box(
                Modifier.weight(1f).height(34.dp).clip(RoundedCornerShape(17.dp))
                    .background(if (on) Ink else Paper).noRippleClick { onPick(i) },
                contentAlignment = Alignment.Center,
            ) { T(label, 15.sp, bold = on, color = if (on) Paper else Ink) }
        }
    }
}

/** 胶囊按钮；filled 为黑底白字。/ Pill button; filled = black with white text. */
@Composable
fun Pill(label: String, filled: Boolean, modifier: Modifier = Modifier, height: Dp = 44.dp, onClick: () -> Unit) {
    Box(
        modifier.height(height).clip(RoundedCornerShape(height / 2))
            .background(if (filled) Ink else Paper)
            .border(1.5.dp, Ink, RoundedCornerShape(height / 2)).noRippleClick(onClick),
        contentAlignment = Alignment.Center,
    ) { T(label, 16.sp, bold = filled, color = if (filled) Paper else Ink) }
}

/** 分页指示“‹ 1 / 5 ›”。/ Pager "‹ 1 / 5 ›". */
@Composable
fun Pager(page: Int, pages: Int, onTurn: (Int) -> Unit, modifier: Modifier = Modifier) {
    Row(modifier.fillMaxWidth().height(40.dp), horizontalArrangement = Arrangement.Center, verticalAlignment = Alignment.CenterVertically) {
        T("‹", 20.sp, modifier = Modifier.width(64.dp).noRippleClick { onTurn(-1) }, align = TextAlign.Center)
        T("${page + 1} / $pages", 15.sp)
        T("›", 20.sp, modifier = Modifier.width(64.dp).noRippleClick { onTurn(1) }, align = TextAlign.Center)
    }
}

/** 左右滑动翻页：左滑下一页，右滑上一页。/ Horizontal swipes turn pages: left = next, right = previous. */
fun Modifier.swipeTurn(onTurn: (Int) -> Unit): Modifier = this.pointerInput(Unit) {
    var total = 0f
    detectHorizontalDragGestures(
        onDragStart = { total = 0f },
        onDragEnd = { if (total < -60f) onTurn(1) else if (total > 60f) onTurn(-1) },
    ) { _, dx -> total += dx }
}

/** 设置图标（两条线、两个圆，和固件的设置标签一样）。/ Settings glyph (two lines, two knobs, like the firmware tab). */
@Composable
fun SettingsIcon(modifier: Modifier = Modifier) {
    Canvas(modifier.size(24.dp)) {
        val w = size.width
        val s = w / 40f
        val stroke = 2.6f * s
        drawLine(Ink, Offset(7 * s, 12 * s), Offset(33 * s, 12 * s), stroke, StrokeCap.Round)
        drawLine(Ink, Offset(7 * s, 28 * s), Offset(33 * s, 28 * s), stroke, StrokeCap.Round)
        drawCircle(Paper, 5 * s, Offset(15 * s, 12 * s))
        drawCircle(Ink, 4 * s, Offset(15 * s, 12 * s), style = Stroke(stroke))
        drawCircle(Paper, 5 * s, Offset(26 * s, 28 * s))
        drawCircle(Ink, 4 * s, Offset(26 * s, 28 * s), style = Stroke(stroke))
    }
}

/** 刷新图标（两段箭头弧）。/ Refresh glyph (two arrowed arcs). */
@Composable
fun SyncIcon(modifier: Modifier = Modifier) {
    Canvas(modifier.size(18.dp)) {
        val r = size.width * 0.33f
        val c = Offset(size.width / 2, size.height / 2)
        val stroke = Stroke(size.width * 0.08f)
        drawArc(Ink, -20f, -150f, false, Offset(c.x - r, c.y - r), androidx.compose.ui.geometry.Size(2 * r, 2 * r), style = stroke)
        drawArc(Ink, 160f, -150f, false, Offset(c.x - r, c.y - r), androidx.compose.ui.geometry.Size(2 * r, 2 * r), style = stroke)
    }
}

/** 页标题区：返回箭头 + 标题（二级页用）。/ Back arrow + title header for second-level pages. */
@Composable
fun BackTitle(title: String, onBack: () -> Unit) {
    Row(Modifier.fillMaxWidth().height(56.dp).noRippleClick(onBack).padding(horizontal = 20.dp), verticalAlignment = Alignment.CenterVertically) {
        T("‹", 24.sp, modifier = Modifier.padding(end = 14.dp))
        T(title, 18.sp, bold = true, maxLines = 1)
    }
}

/** 黑框“新”角标。/ The boxed "new" tag. */
@Composable
fun NewTag() {
    Box(Modifier.padding(end = 6.dp).border(1.5.dp, Ink, RoundedCornerShape(3.dp)).padding(horizontal = 4.dp)) {
        T("新", 11.sp, bold = true)
    }
}

@Composable
fun rememberNoop() = remember { {} }

/* ---- 固件 tr_icon 的几个图标（40×40 网格）/ Firmware tr_icon glyphs on the 40x40 grid ---- */

private fun androidx.compose.ui.graphics.drawscope.DrawScope.grid(color: Color, block: GridPen.() -> Unit) {
    GridPen(this, size.width / 40f, color).block()
}

private class GridPen(val d: androidx.compose.ui.graphics.drawscope.DrawScope, val s: Float, val c: Color) {
    fun line(x0: Int, y0: Int, x1: Int, y1: Int) =
        d.drawLine(c, Offset(x0 * s, y0 * s), Offset(x1 * s, y1 * s), 2.6f * s, StrokeCap.Round)
    fun circle(x: Int, y: Int, r: Int) = d.drawCircle(c, r * s, Offset(x * s, y * s), style = Stroke(2.6f * s))
}

/** 书库：四本竖着的书。/ Library: four standing books. */
@Composable
fun BooksIcon(color: Color = Ink, modifier: Modifier = Modifier) = Canvas(modifier.size(24.dp)) {
    grid(color) { line(7, 8, 7, 33); line(13, 8, 13, 33); line(19, 9, 26, 33); line(27, 8, 33, 33) }
}

/** 番茄：圆、两片叶子和果柄。/ Tomato: a circle, two leaves and a stem. */
@Composable
fun TomatoIcon(color: Color = Ink, modifier: Modifier = Modifier) = Canvas(modifier.size(24.dp)) {
    grid(color) { circle(20, 22, 12); line(14, 10, 20, 14); line(26, 10, 20, 14); line(20, 14, 20, 7) }
}

/** 设置（可换颜色）。/ Settings glyph with a color. */
@Composable
fun GearIcon(color: Color = Ink, bg: Color = Paper, modifier: Modifier = Modifier) = Canvas(modifier.size(24.dp)) {
    grid(color) { line(7, 12, 33, 12); line(7, 28, 33, 28) }
    val s = size.width / 40f
    drawCircle(bg, 5 * s, Offset(15 * s, 12 * s)); drawCircle(color, 4 * s, Offset(15 * s, 12 * s), style = Stroke(2.6f * s))
    drawCircle(bg, 5 * s, Offset(26 * s, 28 * s)); drawCircle(color, 4 * s, Offset(26 * s, 28 * s), style = Stroke(2.6f * s))
}

/** 搜索：放大镜。/ Search: a magnifier. */
@Composable
fun SearchIcon(color: Color = Ink, modifier: Modifier = Modifier) = Canvas(modifier.size(20.dp)) {
    grid(color) { circle(17, 17, 10); line(25, 25, 34, 34) }
}

/** 底部标签栏（和固件 tr_tab_bar 一样：当前那格黑底胶囊、白图标）。/ Bottom tab bar like the firmware's tr_tab_bar:
 *  the current tab is a black pill with a white glyph. */
@Composable
fun TabBar(active: Int, onPick: (Int) -> Unit) {
    val labels = listOf("书库", "番茄", "设置")
    Column(Modifier.fillMaxWidth().background(Paper)) {
        HLine(soft = false)
        Row(Modifier.fillMaxWidth().height(60.dp)) {
            labels.forEachIndexed { i, label ->
                val on = i == active
                Column(
                    Modifier.weight(1f).fillMaxHeight().noRippleClick { onPick(i) },
                    horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.Center,
                ) {
                    Box(
                        Modifier.width(48.dp).height(26.dp).clip(RoundedCornerShape(13.dp)).background(if (on) Ink else Paper),
                        contentAlignment = Alignment.Center,
                    ) {
                        val c = if (on) Paper else Ink
                        when (i) {
                            0 -> BooksIcon(c, Modifier.size(22.dp))
                            1 -> TomatoIcon(c, Modifier.size(22.dp))
                            else -> GearIcon(c, if (on) Ink else Paper, Modifier.size(22.dp))
                        }
                    }
                    T(label, 12.sp, bold = on, modifier = Modifier.padding(top = 2.dp))
                }
            }
        }
    }
}

/** 进度条：黑色已读段，后面是浅灰点线（和固件 tr_progress 一样）。/ Progress bar: a black read segment then a light
 *  dotted line, like the firmware's tr_progress. */
@Composable
fun ProgressBar(pct: Int, modifier: Modifier = Modifier) = Canvas(modifier.fillMaxWidth().height(4.dp)) {
    val w = size.width * pct.coerceIn(0, 100) / 100f
    drawRect(Ink, size = androidx.compose.ui.geometry.Size(w, size.height))
    var x = w + 6f
    while (x < size.width) { drawCircle(Line, size.height / 3, Offset(x, size.height / 2)); x += size.height * 2.2f }
}

/** 点按和长按（没有水波纹）。/ Tap and long press, without a ripple. */
@OptIn(androidx.compose.foundation.ExperimentalFoundationApi::class)
fun Modifier.longPress(onClick: () -> Unit, onLong: () -> Unit): Modifier = this.then(
    Modifier.combinedClickable(interactionSource = MutableInteractionSource(), indication = null, onClick = onClick, onLongClick = onLong)
)

/** 按文字宽度的胶囊（分类、频道）。/ A pill sized to its text (categories, channels). */
@Composable
fun Chip(label: String, on: Boolean, onClick: () -> Unit) {
    Box(
        Modifier.height(34.dp).clip(RoundedCornerShape(17.dp)).background(if (on) Ink else Paper)
            .border(1.5.dp, Ink, RoundedCornerShape(17.dp)).noRippleClick(onClick).padding(horizontal = 14.dp),
        contentAlignment = Alignment.Center,
    ) { T(label, 15.sp, bold = on, color = if (on) Paper else Ink) }
}
