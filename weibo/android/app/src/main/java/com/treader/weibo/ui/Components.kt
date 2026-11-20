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
import androidx.compose.ui.geometry.Size
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
 * 和固件 tr_ui 一样的 Material 3 灰阶原语：黑、白和 tr_ui.h 里 TR_GRAY_* 那几级灰，没有颜色和动画。
 * 手机上大约按设备像素的一半换成 dp。
 * The firmware tr_ui primitives in Material 3 grays: black, white and the TR_GRAY_* levels from tr_ui.h, no colors
 * or animation. Device pixels map to roughly half as many dp on a phone.
 */

val Ink = Color.Black
val Paper = Color.White
/** 卡片、圆按钮、转发框的底（TR_GRAY_SURFACE）。/ Cards, round buttons and the repost box (TR_GRAY_SURFACE). */
val Surface = Color(0xFFEEEEEE)
/** 选中段、头像圆（TR_GRAY_SELECTED）。/ The active segment and avatar circles (TR_GRAY_SELECTED). */
val Selected = Color(0xFFCCCCCC)
/** 描边和次要文字（TR_GRAY_OUTLINE / TR_GRAY_SECONDARY）。/ Outlines and secondary text. */
val Mid = Color(0xFF555555)
/** 细分隔线（TR_GRAY_LINE）。/ Hairlines (TR_GRAY_LINE). */
val Line = Color(0xFFCCCCCC)

/** 固件 2 像素描边在手机上的粗细。/ The firmware's 2 px outline on a phone. */
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
    style = TextStyle(fontSize = size, fontWeight = if (bold) FontWeight.Bold else FontWeight.Medium, color = color,
        lineHeight = lineHeight),
)

/** 细线；soft 用浅灰（tr_hline）。/ Hairline; soft uses light gray (tr_hline). */
@Composable
fun HLine(soft: Boolean = true, modifier: Modifier = Modifier) =
    Box(modifier.fillMaxWidth().height(1.dp).background(if (soft) Line else Ink))

/** 选中段字左边的勾（固件 check_mark）。/ The check mark left of the active label (firmware check_mark). */
@Composable
private fun CheckMark() {
    Canvas(Modifier.size(width = 10.dp, height = 7.dp)) {
        val sx = size.width / 20f
        val sy = size.height / 14f
        val path = Path().apply { moveTo(0f, 7 * sy); lineTo(7 * sx, 14 * sy); lineTo(20 * sx, 0f) }
        drawPath(path, Ink, style = Stroke(1.5.dp.toPx(), cap = StrokeCap.Round, join = StrokeJoin.Round))
    }
}

/**
 * 分段按钮（tr_segment）：深灰描边的胶囊，段间深灰分隔线；选中段浅灰底，粗体字左边一个勾；其余常规黑字。
 * Segmented button (tr_segment): a dark gray outlined capsule with dividers; the active segment has a light gray fill
 * and a check left of its bold label; the others have regular black labels.
 */
@Composable
fun Segment(labels: List<String>, active: Int, onPick: (Int) -> Unit, modifier: Modifier = Modifier) {
    val shape = RoundedCornerShape(50)
    Row(modifier.fillMaxWidth().height(34.dp).clip(shape).border(OutlineW, Mid, shape)) {
        labels.forEachIndexed { i, label ->
            val on = i == active
            if (i > 0) Box(Modifier.width(OutlineW).fillMaxHeight().background(Mid))
            Row(
                Modifier.weight(1f).fillMaxHeight().background(if (on) Selected else Paper).noRippleClick { onPick(i) },
                horizontalArrangement = Arrangement.Center, verticalAlignment = Alignment.CenterVertically,
            ) {
                if (on) { CheckMark(); Spacer(Modifier.width(5.dp)) }
                T(label, 15.sp, bold = on, maxLines = 1)
            }
        }
    }
}

/** 胶囊按钮（tr_pill）：filled 黑底白色粗字，否则深灰描边黑字。/ Pill (tr_pill): filled = black with white bold
 *  text, otherwise a dark gray outline with black text. */
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

/** 小角标（tr_badge）：filled 黑底白字，否则深灰描边黑色粗字。/ Small badge (tr_badge): filled black with white
 *  text, otherwise a dark gray outline with black bold text. */
@Composable
fun Badge(label: String, filled: Boolean, modifier: Modifier = Modifier) {
    val shape = RoundedCornerShape(50)
    Box(
        modifier.clip(shape)
            .then(if (filled) Modifier.background(Ink) else Modifier.background(Paper).border(OutlineW, Mid, shape))
            .padding(horizontal = 6.dp, vertical = 1.dp),
        contentAlignment = Alignment.Center,
    ) { T(label, 11.sp, bold = true, color = if (filled) Paper else Ink) }
}

/** 浅灰圆钮里的箭头（tr_pager 两侧）；dir -1 向左，1 向右。/ A chevron in a light gray round button (either side of
 *  tr_pager); dir -1 points left, 1 right. */
@Composable
fun ChevronButton(dir: Int, modifier: Modifier = Modifier, onClick: () -> Unit) {
    Canvas(modifier.size(34.dp).clip(CircleShape).background(Surface).noRippleClick(onClick)) {
        val u = size.width / 68f
        val c = Offset(size.width / 2, size.height / 2)
        val path = Path().apply {
            moveTo(c.x - 5 * u * dir, c.y - 11 * u); lineTo(c.x + 6 * u * dir, c.y); lineTo(c.x - 5 * u * dir, c.y + 11 * u)
        }
        drawPath(path, Ink, style = Stroke(1.5.dp.toPx(), cap = StrokeCap.Round, join = StrokeJoin.Round))
    }
}

/** 分页（tr_pager）：中间粗体“1 / 5”，两侧浅灰圆钮。/ Pager (tr_pager): a bold "1 / 5" between two gray round
 *  buttons. */
@Composable
fun Pager(page: Int, pages: Int, onTurn: (Int) -> Unit, modifier: Modifier = Modifier) {
    Row(modifier.fillMaxWidth().height(44.dp), horizontalArrangement = Arrangement.Center, verticalAlignment = Alignment.CenterVertically) {
        ChevronButton(-1) { onTurn(-1) }
        T("${page + 1} / $pages", 15.sp, bold = true, align = TextAlign.Center, modifier = Modifier.width(86.dp))
        ChevronButton(1) { onTurn(1) }
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

/* ---- 固件线条图标 / Firmware line icons ---- */

// 坐标照抄固件 tr_icon_on：40 格上的长度，或 24 格的十分之一（g() 换到 40 格）；线宽 3 格。
// Coordinates copied from the firmware's tr_icon_on: lengths on the 40-unit grid, or tenths of the 24-unit grid
// (g() maps them onto the 40 grid); strokes are 3 units wide.
private fun g(v: Int) = v / 6f

/** 设置图标（TR_ICON_SETTINGS：两根滑杆，各一个圆钮）。/ Settings icon (TR_ICON_SETTINGS: two sliders with knobs). */
@Composable
fun SettingsIcon(modifier: Modifier = Modifier, bg: Color = Paper) {
    Canvas(modifier.size(24.dp)) {
        val s = size.width / 40f
        val w = 3 * s
        fun seg(ax: Int, ay: Int, bx: Int, by: Int) =
            drawLine(Ink, Offset(g(ax) * s, g(ay) * s), Offset(g(bx) * s, g(by) * s), w, StrokeCap.Round)
        seg(40, 70, 125, 70); seg(175, 70, 200, 70)
        seg(40, 170, 65, 170); seg(115, 170, 200, 170)
        for ((cx, cy) in listOf(150 to 70, 90 to 170)) {
            val c = Offset(g(cx) * s, g(cy) * s)
            drawCircle(bg, 5 * s, c)
            drawCircle(Ink, 4 * s, c, style = Stroke(w))
        }
    }
}

/** 刷新图标（TR_ICON_SYNC：缺两角的圆和两个箭头）。/ Refresh icon (TR_ICON_SYNC: a circle with two gaps and two
 *  arrowheads). */
@Composable
fun SyncIcon(modifier: Modifier = Modifier, bg: Color = Paper) {
    Canvas(modifier.size(20.dp)) {
        val s = size.width / 40f
        val c = Offset(20 * s, 20 * s)
        drawCircle(Ink, 12 * s, c, style = Stroke(3 * s))
        drawRect(bg, Offset(c.x + 4 * s, c.y - 14 * s), Size(12 * s, 10 * s))
        drawRect(bg, Offset(c.x - 16 * s, c.y + 4 * s), Size(12 * s, 10 * s))
        fun tri(ax: Float, ay: Float, bx: Float, by: Float, qx: Float, qy: Float) = drawPath(Path().apply {
            moveTo(c.x + ax * s, c.y + ay * s); lineTo(c.x + bx * s, c.y + by * s); lineTo(c.x + qx * s, c.y + qy * s)
            close()
        }, Ink)
        tri(6f, -14f, 16f, -8f, 8f, -2f)
        tri(-6f, 14f, -16f, 8f, -8f, 2f)
    }
}

/** 页标题区：返回箭头 + 粗体标题（二级页用）。/ Back arrow + bold title header for second-level pages. */
@Composable
fun BackTitle(title: String, onBack: () -> Unit) {
    Row(Modifier.fillMaxWidth().height(56.dp).noRippleClick(onBack).padding(horizontal = 20.dp), verticalAlignment = Alignment.CenterVertically) {
        T("‹", 24.sp, modifier = Modifier.padding(end = 14.dp))
        T(title, 18.sp, bold = true, maxLines = 1)
    }
}

/** “新”角标，用描边小角标。/ The "new" tag, as an outlined badge. */
@Composable
fun NewTag() = Badge("新", filled = false, modifier = Modifier.padding(end = 6.dp))

@Composable
fun rememberNoop() = remember { {} }
