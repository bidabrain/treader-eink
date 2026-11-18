package com.treader.weibo.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
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
