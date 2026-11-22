package com.treader.webtoon.ui

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
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.composed
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.foundation.layout.RowScope
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
 * 和固件 tr_ui 一样的 Material 3 灰阶原语：黑、白和 TR_GRAY_* 那几级灰，没有颜色和动画。手机上大约按设备像素的一半换成 dp。
 * The firmware tr_ui primitives in Material 3 grays: black, white and the TR_GRAY_* levels, no colors or animation.
 * Device pixels map to roughly half as many dp on a phone.
 */

val Ink = Color.Black
val Paper = Color.White
/** 次要文字与描边（TR_GRAY_SECONDARY / TR_GRAY_OUTLINE）。/ Secondary text and outlines. */
val Mid = Color(0xFF555555)
val Outline = Mid
/** 细线（TR_GRAY_LINE）。/ Hairlines. */
val Line = Color(0xFFCCCCCC)
/** 卡片、标签栏、圆按钮底（TR_GRAY_SURFACE）。/ Cards, tab bar, round buttons. */
val Surface = Color(0xFFEEEEEE)
/** 选中的标签底、分段选中、按下的格子（TR_GRAY_SELECTED）。/ Active tab capsule, active segment, pressed tiles. */
val Selected = Color(0xFFCCCCCC)

/** 固件 2 像素描边在手机上的粗细。/ The firmware's 2 px outline on a phone. */
val OutlineWidth = 1.dp

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

/** 细线；soft 用浅灰。/ Hairline; soft uses light gray. */
@Composable
fun HLine(soft: Boolean = true, modifier: Modifier = Modifier) =
    Box(modifier.fillMaxWidth().height(1.dp).background(if (soft) Line else Ink))

/** 页标题（大号粗体，tr_title）。/ Page title, large bold (tr_title). */
@Composable
fun PageTitle(title: String, modifier: Modifier = Modifier) = T(title, 24.sp, bold = true, maxLines = 1, modifier = modifier)

/** 分段按钮（tr_segment）：深灰描边胶囊，选中段浅灰底、勾加粗体。/ Segmented button (tr_segment): an outlined capsule;
 *  the active segment gets a gray fill, a check mark and bold text. */
@Composable
fun Segment(labels: List<String>, active: Int, onPick: (Int) -> Unit, modifier: Modifier = Modifier) {
    val shape = RoundedCornerShape(50)
    Row(
        modifier.fillMaxWidth().height(36.dp).clip(shape).border(OutlineWidth, Outline, shape),
    ) {
        labels.forEachIndexed { i, label ->
            val on = i == active
            if (i > 0) Box(Modifier.width(OutlineWidth).fillMaxHeight().background(Outline))
            Row(
                Modifier.weight(1f).fillMaxHeight().background(if (on) Selected else Paper).noRippleClick { onPick(i) },
                horizontalArrangement = Arrangement.Center, verticalAlignment = Alignment.CenterVertically,
            ) {
                if (on) CheckIcon(Modifier.padding(end = 6.dp))
                T(label, 15.sp, bold = on, maxLines = 1)
            }
        }
    }
}

/** 选中段的勾。/ The check mark on the active segment. */
@Composable
fun CheckIcon(modifier: Modifier = Modifier, color: Color = Ink) = Canvas(modifier.size(12.dp)) {
    val s = size.width / 20f
    val w = 3f * s
    drawLine(color, Offset(0f, 10 * s), Offset(7 * s, 17 * s), w, StrokeCap.Round)
    drawLine(color, Offset(7 * s, 17 * s), Offset(20 * s, 3 * s), w, StrokeCap.Round)
}

/** 胶囊按钮（tr_pill）：filled 为黑底白字粗体，否则深灰描边黑字。/ Pill button (tr_pill): filled = black with bold
 *  white text, else a dark gray outline with black text. */
@Composable
fun Pill(label: String, filled: Boolean, modifier: Modifier = Modifier, height: Dp = 44.dp, onClick: () -> Unit) {
    val shape = RoundedCornerShape(height / 2)
    Box(
        modifier.height(height).clip(shape)
            .then(if (filled) Modifier.background(Ink) else Modifier.border(OutlineWidth, Outline, shape))
            .noRippleClick(onClick),
        contentAlignment = Alignment.Center,
    ) { T(label, 16.sp, bold = filled, color = if (filled) Paper else Ink, maxLines = 1) }
}

/** 小标记胶囊（tr_badge）：filled 为黑底白字，否则深灰描边黑字，都是粗体。/ Badge capsule (tr_badge): filled = black
 *  with white text, else a dark gray outline with black text; bold either way. */
@Composable
fun Badge(label: String, filled: Boolean, modifier: Modifier = Modifier) {
    val shape = RoundedCornerShape(50)
    Box(
        modifier.height(22.dp).clip(shape)
            .then(if (filled) Modifier.background(Ink) else Modifier.border(OutlineWidth, Outline, shape))
            .padding(horizontal = 9.dp),
        contentAlignment = Alignment.Center,
    ) { T(label, 12.sp, bold = true, color = if (filled) Paper else Ink, maxLines = 1) }
}

/** 浅灰圆角卡片（固件卡片半径 24 像素）。/ A light gray rounded card (the firmware's 24 px radius). */
fun Modifier.card(radius: Dp = 12.dp): Modifier = this.clip(RoundedCornerShape(radius)).background(Surface)

/** 分页指示（tr_pager）：中间粗体“1 / 5”，两侧浅灰圆钮里画箭头。/ Pager (tr_pager): bold "1 / 5" between two light
 *  gray round buttons with chevrons. */
@Composable
fun Pager(page: Int, pages: Int, onTurn: (Int) -> Unit, modifier: Modifier = Modifier) {
    Row(modifier.fillMaxWidth().height(40.dp), horizontalArrangement = Arrangement.Center, verticalAlignment = Alignment.CenterVertically) {
        RoundChevron(-1) { onTurn(-1) }
        T("${page + 1} / $pages", 15.sp, bold = true, align = TextAlign.Center, modifier = Modifier.width(88.dp))
        RoundChevron(1) { onTurn(1) }
    }
}

/** 浅灰圆钮里的箭头；dir −1 朝左、1 朝右。/ A chevron in a light gray round button; dir -1 points left, 1 right. */
@Composable
fun RoundChevron(dir: Int, onClick: () -> Unit) = Canvas(Modifier.size(34.dp).noRippleClick(onClick)) {
    val s = size.width / 68f
    val c = Offset(size.width / 2, size.height / 2)
    drawCircle(Surface, size.width / 2, c)
    val w = 3f * s
    drawLine(Ink, Offset(c.x - 5 * s * dir, c.y - 11 * s), Offset(c.x + 6 * s * dir, c.y), w, StrokeCap.Round)
    drawLine(Ink, Offset(c.x + 6 * s * dir, c.y), Offset(c.x - 5 * s * dir, c.y + 11 * s), w, StrokeCap.Round)
}

/**
 * 左右滑动翻页：左滑下一页，右滑上一页。手势只装一次，回调总用最新的（不然列表翻页会一直从第一次的页码算）。
 * Horizontal swipes turn pages: left = next, right = previous. The gesture is installed once but always calls the
 * latest callback (otherwise list paging keeps counting from the first page number).
 */
fun Modifier.swipeTurn(onTurn: (Int) -> Unit): Modifier = composed {
    val latest by rememberUpdatedState(onTurn)
    pointerInput(Unit) {
        var total = 0f
        detectHorizontalDragGestures(
            onDragStart = { total = 0f },
            onDragEnd = { if (total < -60f) latest(1) else if (total > 60f) latest(-1) },
        ) { _, dx -> total += dx }
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

/** 描边“新”角标（tr_badge 的描边样子）。/ The outlined "new" tag, styled like an outlined tr_badge. */
@Composable
fun NewTag() {
    Box(Modifier.padding(end = 6.dp).border(OutlineWidth, Outline, RoundedCornerShape(50)).padding(horizontal = 5.dp)) {
        T("新", 11.sp, bold = true)
    }
}

@Composable
fun rememberNoop() = remember { {} }

/* ---- 固件 tr_icon_on 的图标（40×40 网格，线宽 3）/ Firmware tr_icon_on glyphs on the 40x40 grid, 3-unit strokes ---- */

private fun DrawScope.grid(color: Color, bg: Color = Paper, block: GridPen.() -> Unit) {
    GridPen(this, size.width / 40f, color, bg).block()
}

// 坐标用 40 格；g() 把设计稿 24 格的十分之一（tr_ui.c 的 seg24 / G()）换成 40 格。
// Coordinates are on the 40 grid; g() converts tenths of the design's 24 grid (seg24 / G() in tr_ui.c) to it.
private class GridPen(val d: DrawScope, val s: Float, val c: Color, val bg: Color) {
    val w = 3f * s
    fun g(v: Int) = v / 6f
    fun line(x0: Float, y0: Float, x1: Float, y1: Float) =
        d.drawLine(c, Offset(x0 * s, y0 * s), Offset(x1 * s, y1 * s), w, StrokeCap.Round)
    fun seg(ax: Int, ay: Int, bx: Int, by: Int) = line(g(ax), g(ay), g(bx), g(by))
    fun circle(x: Float, y: Float, r: Float) = d.drawCircle(c, r * s, Offset(x * s, y * s), style = Stroke(w))
    fun dot(x: Float, y: Float, r: Float, color: Color = c) = d.drawCircle(color, r * s, Offset(x * s, y * s))
    fun rect(x: Float, y: Float, rw: Float, rh: Float, color: Color) =
        d.drawRect(color, Offset(x * s, y * s), Size(rw * s, rh * s))
    fun tri(x0: Float, y0: Float, x1: Float, y1: Float, x2: Float, y2: Float) = d.drawPath(
        Path().apply { moveTo(x0 * s, y0 * s); lineTo(x1 * s, y1 * s); lineTo(x2 * s, y2 * s); close() }, c,
    )
}

/** 发现页（固件 TR_ICON_BOOKS）：三本竖书和一本斜靠的。/ Store (the firmware's TR_ICON_BOOKS): three upright books and
 *  one leaning. */
@Composable
fun BooksIcon(color: Color = Ink, modifier: Modifier = Modifier) = Canvas(modifier.size(24.dp)) {
    grid(color) { seg(50, 40, 50, 200); seg(90, 40, 90, 200); seg(130, 40, 130, 200); seg(165, 45, 200, 195) }
}

/** 漫画（TR_ICON_COMIC）：一页分格漫画，圆角外框、横分格和竖分格。/ Comic: a manga page with a rounded frame, a
 *  horizontal and a vertical gutter. */
@Composable
fun ComicIcon(color: Color = Ink, modifier: Modifier = Modifier) = Canvas(modifier.size(24.dp)) {
    val s = size.width / 40f
    drawRoundRect(color, Offset(6.5f * s, 6.5f * s), Size(27f * s, 27f * s), CornerRadius(3f * s), style = Stroke(3f * s))
    grid(color) { seg(30, 120, 130, 120); seg(130, 30, 130, 210) }
}

/** 列表显示：三行，每行一个小方块和一条线。/ List view: three rows, each a small square and a line. */
@Composable
fun ListIcon(color: Color = Ink, modifier: Modifier = Modifier) = Canvas(modifier.size(24.dp)) {
    grid(color) {
        for (y in listOf(8f, 18f, 28f)) { rect(5f, y, 6f, 5f, color); rect(15f, y + 1.5f, 20f, 2.5f, color) }
    }
}

/** 网格显示：两行两列四个圆角方框，线宽和漫画图标一样。/ Grid view: four rounded squares in two rows and two columns,
 *  stroked like the comic icon. */
@Composable
fun GridIcon(color: Color = Ink, modifier: Modifier = Modifier) = Canvas(modifier.size(24.dp)) {
    val s = size.width / 40f
    for (x in listOf(7f, 23f)) for (y in listOf(7f, 23f))
        drawRoundRect(color, Offset(x * s, y * s), Size(10f * s, 10f * s), CornerRadius(2f * s), style = Stroke(3f * s))
}

/** 设置（TR_ICON_SETTINGS）：两根滑杆，各一个圆钮；bg 是圆钮镂空处的底色。/ Settings: two sliders with round knobs;
 *  bg fills the knob cut-outs. */
@Composable
fun GearIcon(color: Color = Ink, bg: Color = Paper, modifier: Modifier = Modifier) = Canvas(modifier.size(24.dp)) {
    grid(color, bg) {
        seg(40, 70, 125, 70); seg(175, 70, 200, 70); seg(40, 170, 65, 170); seg(115, 170, 200, 170)
        dot(g(150), g(70), 5f, bg); circle(g(150), g(70), 4f)
        dot(g(90), g(170), 5f, bg); circle(g(90), g(170), 4f)
    }
}

/** 设置图标（黑线白底）。/ Settings glyph, black on white. */
@Composable
fun SettingsIcon(modifier: Modifier = Modifier) = GearIcon(modifier = modifier)

/** 搜索（TR_ICON_SEARCH）：放大镜。/ Search: a magnifier. */
@Composable
fun SearchIcon(color: Color = Ink, modifier: Modifier = Modifier) = Canvas(modifier.size(20.dp)) {
    grid(color) { circle(g(110), g(110), 11f); seg(157, 157, 200, 200) }
}

/** 刷新（TR_ICON_SYNC）：两段带箭头的圆弧；bg 是缺口处的底色。/ Sync: two arrowed arcs; bg fills the gaps. */
@Composable
fun SyncIcon(modifier: Modifier = Modifier, color: Color = Ink, bg: Color = Paper) = Canvas(modifier.size(22.dp)) {
    grid(color, bg) {
        circle(20f, 20f, 12f)
        rect(24f, 6f, 12f, 10f, bg); rect(4f, 24f, 12f, 10f, bg)
        tri(26f, 6f, 36f, 12f, 28f, 18f); tri(14f, 34f, 4f, 28f, 12f, 22f)
    }
}

/** 底部标签栏（tr_tab_bar）：浅灰底；当前那格图标垫一块灰胶囊，名字黑色粗体，其余名字深灰。/ Bottom tab bar
 *  (tr_tab_bar): a light gray surface; the current icon sits on a gray capsule with a bold black label, the others
 *  have dark gray labels. */
@Composable
fun TabBar(active: Int, onPick: (Int) -> Unit) {
    val labels = listOf("我的漫画", "发现", "设置")
    Row(Modifier.fillMaxWidth().height(68.dp).background(Surface)) {
        labels.forEachIndexed { i, label ->
            val on = i == active
            val under = if (on) Selected else Surface
            Column(
                Modifier.weight(1f).fillMaxHeight().noRippleClick { onPick(i) },
                horizontalAlignment = Alignment.CenterHorizontally, verticalArrangement = Arrangement.Center,
            ) {
                Box(
                    Modifier.width(56.dp).height(30.dp).clip(RoundedCornerShape(50)).background(under),
                    contentAlignment = Alignment.Center,
                ) {
                    when (i) {
                        0 -> ComicIcon(Ink, Modifier.size(22.dp))
                        1 -> BooksIcon(Ink, Modifier.size(22.dp))
                        else -> GearIcon(Ink, under, Modifier.size(22.dp))
                    }
                }
                T(label, 12.sp, bold = on, color = if (on) Ink else Mid, modifier = Modifier.padding(top = 4.dp))
            }
        }
    }
}

/** 进度条（tr_progress）：已读部分是圆头黑条，剩余部分是一条深灰细轨道。/ Progress (tr_progress): a round-capped black
 *  bar for the read part on a thin dark gray track. */
@Composable
fun ProgressBar(pct: Int, modifier: Modifier = Modifier) = Canvas(modifier.fillMaxWidth().height(4.dp)) {
    val h = size.height
    val w = size.width * pct.coerceIn(0, 100) / 100f
    if (w < size.width) drawRect(Outline, Offset(w, h / 2 - 0.5.dp.toPx()), Size(size.width - w, 1.dp.toPx()))
    if (w > 0f) drawRoundRect(Ink, size = Size(w, h), cornerRadius = CornerRadius(h / 2))
}

/** 点按和长按（没有水波纹）。/ Tap and long press, without a ripple. */
@OptIn(androidx.compose.foundation.ExperimentalFoundationApi::class)
fun Modifier.longPress(onClick: () -> Unit, onLong: () -> Unit): Modifier = this.then(
    Modifier.combinedClickable(interactionSource = MutableInteractionSource(), indication = null, onClick = onClick, onLongClick = onLong)
)

/** 按文字宽度的胶囊（分类、频道）：选中黑底白字粗体，否则深灰描边（tr_pill）。/ A pill sized to its text (categories,
 *  channels): selected is black with bold white text, else a dark gray outline (tr_pill). */
@Composable
fun Chip(label: String, on: Boolean, onClick: () -> Unit) {
    val shape = RoundedCornerShape(50)
    Box(
        Modifier.height(34.dp).clip(shape)
            .then(if (on) Modifier.background(Ink) else Modifier.border(OutlineWidth, Outline, shape))
            .noRippleClick(onClick).padding(horizontal = 14.dp),
        contentAlignment = Alignment.Center,
    ) { T(label, 15.sp, bold = on, color = if (on) Paper else Ink) }
}

/** 浅灰底的搜索栏（Material 3）：放大镜和一行内容。/ A light gray Material 3 search bar: a magnifier and one line of
 *  content. */
@Composable
fun SearchBar(modifier: Modifier = Modifier, height: Dp = 44.dp, content: @Composable RowScope.() -> Unit) {
    Row(
        modifier.fillMaxWidth().height(height).clip(RoundedCornerShape(50)).background(Surface).padding(horizontal = 16.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        SearchIcon(modifier = Modifier.padding(end = 10.dp))
        content()
    }
}
