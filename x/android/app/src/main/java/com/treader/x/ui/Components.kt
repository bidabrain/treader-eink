package com.treader.x.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
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
 * 和固件 tr_ui 一样的 Material 3 灰阶原语：黑、白和三级灰（#EEEEEE 面、#CCCCCC 选中和细线、#555555 描边和次要字），
 * 没有颜色和动画。手机上大约按设备像素的一半换成 dp。
 * The firmware tr_ui primitives in Material 3 grays: black, white and three grays (#EEEEEE surface, #CCCCCC selected
 * and hairlines, #555555 outlines and secondary text), no colors or animation. Device pixels map to roughly half as
 * many dp on a phone.
 */

val Ink = Color.Black
val Paper = Color.White
/** 次要文字和描边（TR_GRAY_SECONDARY / TR_GRAY_OUTLINE）。/ Secondary text and outlines. */
val Mid = Color(0xFF555555)
/** 细线（TR_GRAY_LINE）。/ Hairlines. */
val Line = Color(0xFFCCCCCC)
/** 卡片、圆按钮、引用框的底（TR_GRAY_SURFACE）。/ Cards, round buttons and the quote box. */
val Surface = Color(0xFFEEEEEE)
/** 选中段、头像底（TR_GRAY_SELECTED）。/ The active segment and avatars. */
val Selected = Color(0xFFCCCCCC)

/** 固件 2 像素的描边换成手机上的线宽。/ The firmware's 2 px outline as a phone stroke. */
val OutlineW = 1.dp

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
    style = TextStyle(fontSize = size, fontWeight = if (bold) FontWeight.Bold else FontWeight.Normal, color = color,
        lineHeight = lineHeight),
)

/** 细线；soft 用 #CCCCCC。/ Hairline; soft uses #CCCCCC. */
@Composable
fun HLine(soft: Boolean = true, modifier: Modifier = Modifier) =
    Box(modifier.fillMaxWidth().height(1.dp).background(if (soft) Line else Ink))

/** 选中段的勾（固件 check_mark）。/ The check mark on the active segment (firmware check_mark). */
@Composable
fun CheckMark(size: Dp = 13.dp, color: Color = Ink) {
    Canvas(Modifier.size(size)) {
        val s = this.size.width / 20f
        val p = Path().apply { moveTo(0f, 10 * s); lineTo(7 * s, 17 * s); lineTo(20 * s, 3 * s) }
        drawPath(p, color, style = Stroke(1.6.dp.toPx(), cap = StrokeCap.Round, join = StrokeJoin.Round))
    }
}

/**
 * 分段按钮（tr_segment）：#555555 描边胶囊，段间分隔线；选中段铺 #CCCCCC，字左边一个勾、字加粗；disabled 里的段铺
 * #EEEEEE、灰字、点不了（固件“正在关注”在服务器关掉时的样子）。
 * Segmented button (tr_segment): a #555555 outlined capsule with dividers; the active segment is filled #CCCCCC
 * with a check left of a bold label; segments in disabled are filled #EEEEEE, grey and ignore taps (the firmware's
 * Following cell when the server turns it off).
 */
@Composable
fun Segment(
    labels: List<String>, active: Int, onPick: (Int) -> Unit, modifier: Modifier = Modifier,
    disabled: Set<Int> = emptySet(),
) {
    val shape = RoundedCornerShape(50)
    Row(modifier.fillMaxWidth().height(36.dp).clip(shape).border(OutlineW, Mid, shape)) {
        labels.forEachIndexed { i, label ->
            if (i > 0) Box(Modifier.width(OutlineW).fillMaxHeight().background(Mid))
            val off = i in disabled
            val on = i == active && !off
            Box(
                Modifier.weight(1f).fillMaxHeight()
                    .background(if (on) Selected else Paper)
                    .then(if (off) Modifier.padding(3.dp).clip(shape).background(Surface) else Modifier)
                    .noRippleClick { if (!off) onPick(i) },
                contentAlignment = Alignment.Center,
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    if (on) { CheckMark(); Spacer(Modifier.width(6.dp)) }
                    T(label, 15.sp, bold = on, color = if (off) Mid else Ink, maxLines = 1)
                }
            }
        }
    }
}

/** 胶囊按钮（tr_pill）：filled 黑底白粗字；否则 #555555 描边黑字。/ Pill button (tr_pill): filled = black with
 *  white bold text; otherwise a #555555 outline with black text. */
@Composable
fun Pill(label: String, filled: Boolean, modifier: Modifier = Modifier, height: Dp = 44.dp, onClick: () -> Unit) {
    val shape = RoundedCornerShape(height / 2)
    Box(
        modifier.height(height).clip(shape)
            .then(if (filled) Modifier.background(Ink) else Modifier.background(Paper).border(OutlineW, Mid, shape))
            .noRippleClick(onClick),
        contentAlignment = Alignment.Center,
    ) { T(label, 16.sp, bold = filled, color = if (filled) Paper else Ink) }
}

/** 左右箭头（tr_pager 圆钮里那种）。dir -1 向左，1 向右。/ A chevron like the ones in tr_pager's round buttons;
 *  dir -1 points left, 1 right. */
@Composable
fun Chevron(dir: Int, size: Dp = 14.dp, color: Color = Ink) {
    Canvas(Modifier.size(size)) {
        val w = this.size.width
        val h = this.size.height
        val x0 = if (dir < 0) w * 0.7f else w * 0.3f
        val x1 = if (dir < 0) w * 0.3f else w * 0.7f
        val p = Path().apply { moveTo(x0, 0.1f * h); lineTo(x1, 0.5f * h); lineTo(x0, 0.9f * h) }
        drawPath(p, color, style = Stroke(1.6.dp.toPx(), cap = StrokeCap.Round, join = StrokeJoin.Round))
    }
}

/** 分页（tr_pager）：中间粗体“1 / 5”，两边 #EEEEEE 圆钮里画箭头。/ Pager (tr_pager): bold "1 / 5" in the middle,
 *  chevrons in #EEEEEE round buttons on either side. */
@Composable
fun Pager(page: Int, pages: Int, onTurn: (Int) -> Unit, modifier: Modifier = Modifier) {
    Row(modifier.fillMaxWidth().height(48.dp), horizontalArrangement = Arrangement.Center, verticalAlignment = Alignment.CenterVertically) {
        Box(Modifier.size(36.dp).clip(CircleShape).background(Surface).noRippleClick { onTurn(-1) },
            contentAlignment = Alignment.Center) { Chevron(-1) }
        T("${page + 1} / $pages", 15.sp, bold = true, align = TextAlign.Center, modifier = Modifier.width(88.dp))
        Box(Modifier.size(36.dp).clip(CircleShape).background(Surface).noRippleClick { onTurn(1) },
            contentAlignment = Alignment.Center) { Chevron(1) }
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

/** 设置图标（TR_ICON_SETTINGS：两根滑杆，各一个圆钮）。/ Settings glyph (TR_ICON_SETTINGS: two sliders, each with a
 *  round knob). */
@Composable
fun SettingsIcon(modifier: Modifier = Modifier, color: Color = Ink) {
    Canvas(modifier.size(24.dp)) {
        // 24 格的十分之一换到画布上。/ Tenths of the 24-unit grid onto the canvas.
        val g = size.width / 240f
        val stroke = 3f / 40f * size.width
        fun seg(ax: Int, ay: Int, bx: Int, by: Int) =
            drawLine(color, Offset(ax * g, ay * g), Offset(bx * g, by * g), stroke, StrokeCap.Round)
        seg(40, 70, 125, 70); seg(175, 70, 200, 70)
        seg(40, 170, 65, 170); seg(115, 170, 200, 170)
        val r = size.width * 4f / 40f
        drawCircle(color, r, Offset(150 * g, 70 * g), style = Stroke(stroke))
        drawCircle(color, r, Offset(90 * g, 170 * g), style = Stroke(stroke))
    }
}

/** 刷新图标（TR_ICON_SYNC：缺两角的圆加两个实心箭头）。/ Refresh glyph (TR_ICON_SYNC: a circle with two gaps and two
 *  filled arrowheads). */
@Composable
fun SyncIcon(modifier: Modifier = Modifier, color: Color = Ink) {
    Canvas(modifier.size(22.dp)) {
        val p = size.width / 40f
        val c = Offset(size.width / 2, size.height / 2)
        val r = 12 * p
        val box = androidx.compose.ui.geometry.Size(2 * r, 2 * r)
        val tl = Offset(c.x - r, c.y - r)
        val stroke = Stroke(3 * p)
        // 右上和左下各留一个缺口（固件用底色方块盖掉的那两块）。/ Gaps at the top right and bottom left (where the
        // firmware paints background squares).
        drawArc(color, -20f, -130f, false, tl, box, style = stroke)
        drawArc(color, 160f, -130f, false, tl, box, style = stroke)
        fun tri(ax: Float, ay: Float, bx: Float, by: Float, cx: Float, cy: Float) = drawPath(Path().apply {
            moveTo(c.x + ax * p, c.y + ay * p); lineTo(c.x + bx * p, c.y + by * p); lineTo(c.x + cx * p, c.y + cy * p); close()
        }, color)
        tri(6f, -14f, 16f, -8f, 8f, -2f)
        tri(-6f, 14f, -16f, 8f, -8f, 2f)
    }
}

/** 页标题区：返回箭头 + 标题（二级页用）。/ Back arrow + title header for second-level pages. */
@Composable
fun BackTitle(title: String, onBack: () -> Unit) {
    Row(Modifier.fillMaxWidth().height(56.dp).noRippleClick(onBack).padding(horizontal = 20.dp), verticalAlignment = Alignment.CenterVertically) {
        Chevron(-1, 16.dp)
        Spacer(Modifier.width(12.dp))
        T(title, 20.sp, bold = true, maxLines = 1)
    }
}

/** 小角标（tr_badge），默认“新”：#555555 描边胶囊、黑色粗字；filled 时黑底白字。/ A small badge (tr_badge), "new"
 *  by default: a #555555 outlined capsule with bold black text; filled = black with white text. */
@Composable
fun NewTag(label: String = "新", filled: Boolean = false) {
    val shape = RoundedCornerShape(50)
    Box(
        Modifier.padding(end = 6.dp).clip(shape)
            .then(if (filled) Modifier.background(Ink) else Modifier.border(OutlineW, Mid, shape))
            .padding(horizontal = 8.dp, vertical = 1.dp)
    ) {
        T(label, 11.sp, bold = true, color = if (filled) Paper else Ink)
    }
}

@Composable
fun rememberNoop() = remember { {} }
