package com.treader.rednote.ui

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
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.ui.graphics.Path
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
 * 和固件 tr_ui 一样的 Material 3 灰阶原语：只用黑、白和下面几级灰，没有彩色和动画。手机上大约按设备像素的一半换成 dp。
 * The firmware tr_ui primitives in Material 3 grays: black, white and the gray levels below only, no color or
 * animation. Device pixels map to roughly half as many dp on a phone.
 */

val Ink = Color.Black
val Paper = Color.White
/** 次要文字和描边（TR_GRAY_SECONDARY / TR_GRAY_OUTLINE）。/ Secondary text and outlines. */
val Mid = Color(0xFF555555)
/** 细线（TR_GRAY_LINE）。/ Hairlines. */
val Line = Color(0xFFCCCCCC)
/** 选中段、选中标签底（TR_GRAY_SELECTED）。/ Active segment and selected tab fill. */
val Selected = Color(0xFFCCCCCC)
/** 卡片、圆按钮底（TR_GRAY_SURFACE）。/ Cards and round buttons. */
val Surface = Color(0xFFEEEEEE)
/** 描边色和粗细（固件两像素）。/ Outline color and width (2 px on the firmware). */
val Outline = Mid
val OutlineW = 1.5.dp

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

/** 细线；soft 用浅灰（固件 tr_hline）。/ Hairline; soft uses light gray (firmware tr_hline). */
@Composable
fun HLine(soft: Boolean = true, modifier: Modifier = Modifier) =
    Box(modifier.fillMaxWidth().height(1.dp).background(if (soft) Line else Ink))

/** 选中段左边的勾（固件 check_mark）。/ The check mark left of the active label (firmware check_mark). */
@Composable
fun CheckMark(modifier: Modifier = Modifier, color: Color = Ink) {
    Canvas(modifier.size(14.dp, 10.dp)) {
        // 固件里勾宽 20、高 14 像素，线宽 3。/ On the firmware the check is 20 x 14 px with 3 px strokes.
        val s = size.width / 20f
        val stroke = 3f * s * 0.8f
        drawLine(color, Offset(0f, 7 * s), Offset(7 * s, 14 * s), stroke, StrokeCap.Round)
        drawLine(color, Offset(7 * s, 14 * s), Offset(20 * s, 0f), stroke, StrokeCap.Round)
    }
}

/**
 * 分段按钮（固件 tr_segment）：深灰描边胶囊、竖分隔线；选中段浅灰底，勾在粗体字左边，其余常规黑字。
 * Segmented button (firmware tr_segment): a dark gray outlined capsule with dividers; the active segment gets a gray
 * fill and a check left of its bold label, the others regular black labels.
 */
@Composable
fun Segment(labels: List<String>, active: Int, onPick: (Int) -> Unit, modifier: Modifier = Modifier) {
    val shape = RoundedCornerShape(17.dp)
    Row(modifier.fillMaxWidth().height(34.dp).clip(shape).border(OutlineW, Outline, shape)) {
        labels.forEachIndexed { i, label ->
            val on = i == active
            if (i > 0) Box(Modifier.width(OutlineW).fillMaxHeight().background(Outline))
            Row(
                Modifier.weight(1f).fillMaxHeight().background(if (on) Selected else Paper).noRippleClick { onPick(i) },
                horizontalArrangement = Arrangement.Center,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                if (on) CheckMark(Modifier.padding(end = 6.dp))
                T(label, 15.sp, bold = on, maxLines = 1)
            }
        }
    }
}

/** 胶囊按钮（固件 tr_pill）：filled 黑底白粗字；否则深灰描边、黑字。/ Pill (firmware tr_pill): filled = black with
 *  white bold text; otherwise a dark gray outline with black text. */
@Composable
fun Pill(label: String, filled: Boolean, modifier: Modifier = Modifier, height: Dp = 44.dp, onClick: () -> Unit) {
    val shape = RoundedCornerShape(height / 2)
    Box(
        modifier.height(height).clip(shape)
            .then(if (filled) Modifier.background(Ink) else Modifier.background(Paper).border(OutlineW, Outline, shape))
            .noRippleClick(onClick),
        contentAlignment = Alignment.Center,
    ) { T(label, 16.sp, bold = filled, color = if (filled) Paper else Ink) }
}

/** 小胶囊角标（固件 tr_badge）：filled 黑底白字；否则白底深灰描边、黑粗字。/ Small capsule badge (firmware
 *  tr_badge): filled = black with white text; otherwise white with a dark gray outline and bold black text. */
@Composable
fun Badge(label: String, filled: Boolean, modifier: Modifier = Modifier, size: TextUnit = 11.sp) {
    val shape = RoundedCornerShape(50)
    Box(
        modifier.clip(shape)
            .then(if (filled) Modifier.background(Ink) else Modifier.background(Paper).border(1.dp, Outline, shape))
            .padding(horizontal = 8.dp, vertical = 1.dp),
        contentAlignment = Alignment.Center,
    ) { T(label, size, bold = true, color = if (filled) Paper else Ink, maxLines = 1) }
}

/** 左右箭头（固件翻页圆钮里的线条）；dir = -1 向左，1 向右。/ Chevron as in the firmware pager buttons; dir -1 left,
 *  1 right. */
@Composable
fun Chevron(dir: Int, modifier: Modifier = Modifier.size(12.dp, 22.dp), color: Color = Ink, stroke: Dp = 1.6.dp) {
    Canvas(modifier) {
        val w = size.width
        val h = size.height
        val a = if (dir < 0) w else 0f
        val b = if (dir < 0) 0f else w
        val sw = stroke.toPx()
        drawLine(color, Offset(a, 0f), Offset(b, h / 2), sw, StrokeCap.Round)
        drawLine(color, Offset(b, h / 2), Offset(a, h), sw, StrokeCap.Round)
    }
}

/** 翻页（固件 tr_pager）：中间粗体“1 / 5”，两侧浅灰圆钮带箭头。/ Pager (firmware tr_pager): bold "1 / 5" in the
 *  middle with gray round chevron buttons either side. */
@Composable
fun Pager(page: Int, pages: Int, onTurn: (Int) -> Unit, modifier: Modifier = Modifier) {
    Row(modifier.fillMaxWidth().height(40.dp), horizontalArrangement = Arrangement.Center, verticalAlignment = Alignment.CenterVertically) {
        for (d in listOf(-1, 1)) {
            if (d == 1) T("${page + 1} / $pages", 15.sp, bold = true, align = TextAlign.Center, modifier = Modifier.width(88.dp))
            Box(Modifier.size(34.dp).clip(CircleShape).background(Surface).noRippleClick { onTurn(d) },
                contentAlignment = Alignment.Center) { Chevron(d, Modifier.size(6.dp, 11.dp)) }
        }
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

// 固件图标都画在 40 格上、线宽 3（tr_icon_on）；这里按 Canvas 宽度换算。
// Firmware icons live on a 40-unit grid with 3-unit strokes (tr_icon_on); scaled here to the canvas width.

/** 设置图标（固件 TR_ICON_SETTINGS：两根滑杆各一个圆钮）。/ Settings glyph (firmware TR_ICON_SETTINGS: two sliders,
 *  each with a round knob). */
@Composable
fun SettingsIcon(modifier: Modifier = Modifier) {
    Canvas(modifier.size(24.dp)) {
        val s = size.width / 40f
        val g = s * 40f / 240f   // 24 格的十分之一 / tenths of the 24-unit grid
        val stroke = 3f * s
        fun seg(ax: Int, ay: Int, bx: Int, by: Int) =
            drawLine(Ink, Offset(ax * g, ay * g), Offset(bx * g, by * g), stroke, StrokeCap.Round)
        seg(40, 70, 125, 70)
        seg(175, 70, 200, 70)
        seg(40, 170, 65, 170)
        seg(115, 170, 200, 170)
        drawCircle(Ink, 4 * s, Offset(150 * g, 70 * g), style = Stroke(stroke))
        drawCircle(Ink, 4 * s, Offset(90 * g, 170 * g), style = Stroke(stroke))
    }
}

/** 刷新图标（固件 TR_ICON_SYNC：两段圆弧，右上和左下各一个实心箭头）。/ Refresh glyph (firmware TR_ICON_SYNC: two
 *  arcs with solid arrowheads at the top right and bottom left). */
@Composable
fun SyncIcon(modifier: Modifier = Modifier) {
    Canvas(modifier.size(20.dp)) {
        val s = size.width / 40f
        val c = Offset(size.width / 2, size.height / 2)
        val r = 12f * s
        val tl = Offset(c.x - r, c.y - r)
        val box = androidx.compose.ui.geometry.Size(2 * r, 2 * r)
        val stroke = Stroke(3f * s)
        // 固件在圆上挖掉右上、左下两块，剩下两段各约 129 度。/ The firmware masks the top right and bottom left of the
        // circle, leaving two arcs of about 129 degrees.
        drawArc(Ink, -19.5f, 129f, false, tl, box, style = stroke)
        drawArc(Ink, 160.5f, 129f, false, tl, box, style = stroke)
        fun tri(ax: Float, ay: Float, bx: Float, by: Float, cx: Float, cy: Float) = drawPath(Path().apply {
            moveTo(c.x + ax * s, c.y + ay * s); lineTo(c.x + bx * s, c.y + by * s); lineTo(c.x + cx * s, c.y + cy * s); close()
        }, Ink)
        tri(6f, -14f, 16f, -8f, 8f, -2f)
        tri(-6f, 14f, -16f, 8f, -8f, 2f)
    }
}

/** 页标题区：返回箭头 + 标题（二级页用）。/ Back chevron + title header for second-level pages. */
@Composable
fun BackTitle(title: String, bold: Boolean = true, onBack: () -> Unit) {
    Row(Modifier.fillMaxWidth().height(56.dp).noRippleClick(onBack).padding(horizontal = 20.dp), verticalAlignment = Alignment.CenterVertically) {
        Chevron(-1, Modifier.padding(end = 10.dp).size(7.dp, 13.dp))
        T(title, if (bold) 18.sp else 16.sp, bold = bold, maxLines = 1)
    }
}

/** Material 3 对话框：白底大圆角、深灰细框（固件 draw_wait）；点框外 onOutside。/ Material 3 dialog: white, large
 *  radius, thin dark gray outline (firmware draw_wait); tapping outside calls onOutside. */
@Composable
fun TrDialog(onOutside: () -> Unit, hPad: Dp = 40.dp, content: @Composable () -> Unit) {
    val shape = RoundedCornerShape(16.dp)
    Box(Modifier.fillMaxSize().background(Paper.copy(alpha = 0.7f)).noRippleClick(onOutside), contentAlignment = Alignment.Center) {
        Column(
            Modifier.padding(horizontal = hPad).fillMaxWidth().clip(shape).background(Paper)
                .border(OutlineW, Outline, shape).noRippleClick {}.padding(vertical = 22.dp, horizontal = 18.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) { content() }
    }
}
