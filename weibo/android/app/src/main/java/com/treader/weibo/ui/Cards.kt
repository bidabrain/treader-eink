package com.treader.weibo.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import coil.compose.AsyncImage
import coil.request.ImageRequest
import com.treader.weibo.AppVm

/*
 * 卡片用的部件，和固件微博标签的卡片一样：头像圆（名字首字）、名字和时间来源、转发灰框、图片宫格、转评赞三栏。
 * Card pieces matching the firmware Weibo tab: avatar circle (first character), name with time and source, the gray
 * repost box, the image grid and the three-column count bar.
 */

/** 转发框的底，和固件 QUOTE_GRAY 一样是卡片灰。/ The repost box fill, the card gray like the firmware's QUOTE_GRAY. */
val QuoteGray = Surface

/** 头像：浅灰实心圆里写名字的第一个字；dot 时右上角一个黑点（没看过）。/ Avatar: the name's first character on a
 *  filled light gray circle; dot adds a black dot at the top right (unseen). */
@Composable
fun Avatar(name: String, size: Dp = 40.dp, dot: Boolean = false) {
    Box(Modifier.size(size)) {
        Box(
            Modifier.fillMaxSize().clip(CircleShape).background(Selected),
            contentAlignment = Alignment.Center,
        ) {
            val first = name.ifEmpty { "微" }.let { it.substring(0, it.offsetByCodePoints(0, 1)) }
            T(first, (size.value * 0.42f).sp, bold = true)
        }
        if (dot) Box(
            Modifier.size(11.dp).align(Alignment.TopEnd).offset(x = 2.dp, y = (-2).dp).clip(CircleShape)
                .background(Paper).padding(2.dp).clip(CircleShape).background(Ink)
        )
    }
}

/** 头部：头像、粗体名字，下一行时间和来源。/ Header: avatar, bold name, then time and source. */
@Composable
fun PostHead(user: String, time: Double, source: String, long: Boolean = false, dot: Boolean = false) {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
        Avatar(user, dot = dot)
        Column {
            T(user.ifEmpty { "微博用户" }, 15.sp, bold = true, maxLines = 1)
            T(fmtTime(time, long) + if (source.isNotEmpty()) " · $source" else "", 11.5.sp, color = Mid, maxLines = 1)
        }
    }
}

/** 转发原文的灰框。/ The gray box holding the reposted original. */
@Composable
fun QuoteBox(user: String, text: String, maxLines: Int = Int.MAX_VALUE, modifier: Modifier = Modifier) {
    Box(modifier.fillMaxWidth().clip(RoundedCornerShape(7.dp)).background(QuoteGray).padding(horizontal = 10.dp, vertical = 8.dp)) {
        androidx.compose.material3.Text(
            androidx.compose.ui.text.buildAnnotatedString {
                pushStyle(androidx.compose.ui.text.SpanStyle(fontWeight = FontWeight.Bold))
                append("@${user.ifEmpty { "原微博" }}：")
                pop()
                append(text)
            },
            maxLines = maxLines, overflow = androidx.compose.ui.text.style.TextOverflow.Ellipsis,
            style = androidx.compose.ui.text.TextStyle(fontSize = 14.sp, lineHeight = 21.sp, color = Ink),
        )
    }
}

/**
 * 图片：1 张按封面比例放大（宽六成）；旧服务器（pics 为 0）也按一张封面；4 张排 2×2；其余按 3 列九宫格，最多 9 张。
 * 点第 k 张回调 k（从 0 起）。
 * Images: one is enlarged at the cover ratio (60% wide), as are posts from older servers (pics 0); four make a 2x2;
 * otherwise a 3-column grid of up to nine. Tapping image k calls back with k (0-based).
 */
@Composable
fun PicGrid(vm: AppVm, id: String, images: Int, pics: Int, ratio: Float, onOpen: (Int) -> Unit) {
    if (images <= 0) return
    val ctx = LocalContext.current
    @Composable
    fun pic(url: String, k: Int, modifier: Modifier) = Box(
        // 固件小图有一圈黑框，没下载时是卡片灰。/ Firmware thumbnails have a black frame and show card gray until loaded.
        modifier.background(Surface).border(OutlineW, Ink).noRippleClick { onOpen(k) }
    ) {
        AsyncImage(
            model = ImageRequest.Builder(ctx).data(url).build(), imageLoader = vm.images, contentDescription = null,
            contentScale = ContentScale.Crop, modifier = Modifier.fillMaxSize(),
        )
    }
    if (pics <= 1) {
        val r = ratio.coerceIn(0.75f, 4f / 3)
        pic(vm.api.imageUrl(id, 0), 0, Modifier.fillMaxWidth(0.6f).aspectRatio(1f / r))
        return
    }
    val n = pics.coerceAtMost(9)
    val cols = if (n == 4) 2 else 3
    Column(Modifier.fillMaxWidth(if (cols == 2) 0.67f else 1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
        for (row in 0 until (n + cols - 1) / cols) {
            Row(horizontalArrangement = Arrangement.spacedBy(4.dp)) {
                for (c in 0 until cols) {
                    val k = row * cols + c
                    if (k < n) pic(vm.api.squareUrl(id, k + 1), k, Modifier.weight(1f).aspectRatio(1f))
                    else Box(Modifier.weight(1f))
                }
            }
        }
    }
}

/**
 * 转发、评论、赞的深灰小图标，照固件 draw_glyph：两道带实心箭头的横线、方框带尾巴、实心心形。坐标以中心为原点，约 28 格。
 * Dark gray repost, comment and like glyphs after the firmware's draw_glyph: two lines with solid arrowheads, a box
 * with a tail, a solid heart. Coordinates are around the center on a grid of about 28 units.
 */
@Composable
fun Glyph(kind: Int, size: Dp = 15.dp) {
    Canvas(Modifier.size(size)) {
        val s = this.size.width / 28f
        val c = Offset(this.size.width / 2, this.size.height / 2)
        fun p(x: Float, y: Float) = Offset(c.x + x * s, c.y + y * s)
        fun tri(a: Offset, b: Offset, q: Offset) =
            drawPath(Path().apply { moveTo(a.x, a.y); lineTo(b.x, b.y); lineTo(q.x, q.y); close() }, Mid)
        when (kind) {
            0 -> {
                drawLine(Mid, p(-12f, -4.5f), p(10f, -4.5f), 2 * s)
                drawLine(Mid, p(12f, 5.5f), p(-10f, 5.5f), 2 * s)
                tri(p(13f, -4f), p(6f, -10f), p(6f, 2f))
                tri(p(-13f, 6f), p(-6f, 0f), p(-6f, 12f))
            }
            1 -> {
                drawRect(Mid, p(-11f, -9f), androidx.compose.ui.geometry.Size(22 * s, 15 * s), style = Stroke(2 * s))
                tri(p(-8f, 6f), p(-1f, 6f), p(-9f, 13f))
            }
            else -> {
                drawCircle(Mid, 6 * s, p(-5f, -3f))
                drawCircle(Mid, 6 * s, p(5f, -3f))
                tri(p(-11f, -1f), p(11f, -1f), p(0f, 11f))
            }
        }
    }
}

/** 底部三栏：转发、评论、赞，深灰图标和数字。/ The three-column count bar: reposts, comments, likes in dark gray. */
@Composable
fun CountBar(reposts: Int, comments: Int, likes: Int) {
    Row(Modifier.fillMaxWidth().height(38.dp)) {
        listOf(reposts, comments, likes).forEachIndexed { k, v ->
            Row(Modifier.weight(1f).fillMaxSize(), horizontalArrangement = Arrangement.Center, verticalAlignment = Alignment.CenterVertically) {
                Glyph(k)
                T(fmtCount(v), 12.sp, color = Mid, modifier = Modifier.padding(start = 6.dp))
            }
        }
    }
}
