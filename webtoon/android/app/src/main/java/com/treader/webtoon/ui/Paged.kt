package com.treader.webtoon.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.Spacer
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import coil.ImageLoader
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.ui.unit.sp

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

/** 三列封面网格，分页。/ A three-column cover grid, paged. */
@Composable
fun <T> PagedGrid(
    items: List<T>, page: Int, onPage: (Int) -> Unit, modifier: Modifier = Modifier, cols: Int = 3,
    cell: @Composable (T) -> Unit,
) {
    BoxWithConstraints(modifier.fillMaxSize().padding(horizontal = 14.dp)) {
        val gap = 10.dp
        val cellW = (maxWidth - gap * (cols - 1)) / cols
        // 封面 3:4，下面两行字。/ A 3:4 cover with two text lines below.
        val cellH = cellW * 4 / 3 + 44.dp
        val rows = ((maxHeight - PAGER_H + gap) / (cellH + gap)).toInt().coerceAtLeast(1)
        val perPage = rows * cols
        val pages = ((items.size + perPage - 1) / perPage).coerceAtLeast(1)
        val p = page.coerceIn(0, pages - 1)
        LaunchedEffect(p, page) { if (p != page) onPage(p) }
        val turn = { d: Int -> (p + d).takeIf { it in 0 until pages }?.let(onPage) ?: Unit }
        Column(Modifier.fillMaxSize().swipeTurn { turn(it) }) {
            Column(Modifier.weight(1f).padding(top = 8.dp), verticalArrangement = Arrangement.spacedBy(gap)) {
                items.drop(p * perPage).take(perPage).chunked(cols).forEach { line ->
                    Row(Modifier.fillMaxWidth().height(cellH), horizontalArrangement = Arrangement.spacedBy(gap)) {
                        line.forEach { Box(Modifier.weight(1f)) { cell(it) } }
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
        T(title, 13.sp, bold = true, maxLines = 1, modifier = Modifier.padding(top = 4.dp))
        T(sub, 12.sp, bold = subBold, color = if (subBold) Ink else Mid, maxLines = 1)
    }
}

private val PAGER_H = 48.dp
