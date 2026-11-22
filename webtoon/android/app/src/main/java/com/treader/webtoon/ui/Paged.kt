package com.treader.webtoon.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import coil.ImageLoader

/*
 * 分页列表和网格：照墨水屏，一屏能放几行就放几行，底下分页指示，左右滑也翻页，不上下滚动。
 * Paged lists and grids, like the e-ink screen: as many rows as fit, a pager below, swipes also turn; no scrolling.
 */

/** 一屏放几行的分页列表；page 越界时自动拉回。/ A paged list fitting as many rows as the screen holds; an
 *  out-of-range page is pulled back. */
@Composable
fun <T> PagedList(
    items: List<T>, rowHeight: Dp, page: Int, onPage: (Int) -> Unit, modifier: Modifier = Modifier,
    row: @Composable (T) -> Unit,
) {
    BoxWithConstraints(modifier.fillMaxSize()) {
        val perPage = ((maxHeight - PAGER_H) / rowHeight).toInt().coerceAtLeast(1)
        val pages = ((items.size + perPage - 1) / perPage).coerceAtLeast(1)
        val p = page.coerceIn(0, pages - 1)
        LaunchedEffect(p, page) { if (p != page) onPage(p) }
        val turn = { d: Int -> (p + d).takeIf { it in 0 until pages }?.let(onPage) ?: Unit }
        Column(Modifier.fillMaxSize().swipeTurn { turn(it) }) {
            Column(Modifier.weight(1f)) {
                items.drop(p * perPage).take(perPage).forEach { Box(Modifier.fillMaxWidth().height(rowHeight)) { row(it) } }
            }
            if (pages > 1) Pager(p, pages, { turn(it) }) else Spacer(Modifier.height(PAGER_H))
        }
    }
}

/**
 * 三列封面网格，分页。格子大小跟屏幕和系统字体走：两行字的高度用设备上真实的字体量出来（大字号、换过系统字体的
 * 墨水屏平板也不截字）；按列宽排得下几行就排几行，剩下的地方够把封面缩到七成以上再多排一行，就多排一行，免得下面
 * 空一大块。
 * A three-column cover grid, paged. Cells follow the screen and the system font: the two text lines are measured
 * with the device's real font (no clipped text with large font sizes or a replaced system font on e-ink tablets);
 * as many rows as fit at full column width, plus one more when shrinking the covers to at least 70% makes it fit,
 * instead of leaving a large gap below.
 */
@Composable
fun <T> PagedGrid(
    items: List<T>, page: Int, onPage: (Int) -> Unit, modifier: Modifier = Modifier, cols: Int = 3,
    cell: @Composable (T) -> Unit,
) {
    val measurer = rememberTextMeasurer()
    val density = LocalDensity.current
    val textH = with(density) {
        val title = measurer.measure("国 Ag", TextStyle(fontSize = TITLE_SP, fontWeight = FontWeight.Bold), maxLines = 1)
        val sub = measurer.measure("国 Ag", TextStyle(fontSize = SUB_SP, fontWeight = FontWeight.Bold), maxLines = 1)
        (title.size.height + sub.size.height).toDp()
    } + TITLE_TOP + 2.dp
    BoxWithConstraints(modifier.fillMaxSize().padding(horizontal = 14.dp)) {
        val gap = 10.dp
        val colW = (maxWidth - gap * (cols - 1)) / cols
        val avail = maxHeight - PAGER_H - 8.dp
        val fullH = colW * 4 / 3 + textH
        val fit = ((avail + gap) / (fullH + gap)).toInt().coerceAtLeast(1)
        // 多排一行时每格的高度；封面不小于整宽时的七成才多排。/ Cell height with one more row; only taken when covers
        // stay at least 70% of their full-width size.
        val moreH = (avail - gap * fit) / (fit + 1)
        val rows = if (moreH - textH >= (fullH - textH) * 0.7f) fit + 1 else fit
        val cellH = minOf(fullH, (avail - gap * (rows - 1)) / rows)
        val coverW = minOf(colW, (cellH - textH) * 3 / 4)
        val perPage = rows * cols
        val pages = ((items.size + perPage - 1) / perPage).coerceAtLeast(1)
        val p = page.coerceIn(0, pages - 1)
        LaunchedEffect(p, page) { if (p != page) onPage(p) }
        val turn = { d: Int -> (p + d).takeIf { it in 0 until pages }?.let(onPage) ?: Unit }
        Column(Modifier.fillMaxSize().swipeTurn { turn(it) }) {
            Column(Modifier.weight(1f).padding(top = 8.dp), verticalArrangement = Arrangement.spacedBy(gap)) {
                items.drop(p * perPage).take(perPage).chunked(cols).forEach { line ->
                    Row(Modifier.fillMaxWidth().height(cellH), horizontalArrangement = Arrangement.spacedBy(gap)) {
                        line.forEach {
                            Box(Modifier.weight(1f), contentAlignment = Alignment.TopCenter) {
                                Box(Modifier.width(coverW)) { cell(it) }
                            }
                        }
                        repeat(cols - line.size) { Spacer(Modifier.weight(1f)) }
                    }
                }
            }
            if (pages > 1) Pager(p, pages, { turn(it) }) else Spacer(Modifier.height(PAGER_H))
        }
    }
}

/** 网格里的一格：封面、书名、一行小字。/ One grid cell: cover, title and a small line. */
@Composable
fun CoverCell(title: String, sub: String, cover: String?, images: ImageLoader, subBold: Boolean = false, onClick: () -> Unit) {
    Column(Modifier.fillMaxWidth().noRippleClick(onClick)) {
        Cover(title, cover, images, Modifier.fillMaxWidth().aspectRatio(0.75f), 12.sp)
        T(title, TITLE_SP, bold = true, maxLines = 1, modifier = Modifier.padding(top = TITLE_TOP))
        T(sub, SUB_SP, bold = subBold, color = if (subBold) Ink else Mid, maxLines = 1)
    }
}

private val PAGER_H = 48.dp
private val TITLE_SP = 13.sp
private val SUB_SP = 12.sp
private val TITLE_TOP = 4.dp
