package com.treader.x.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
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
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import coil.compose.AsyncImage
import coil.request.ImageRequest
import com.treader.x.AppVm

/*
 * 卡片用的部件，照微博 App 的卡片：头像圆（名字首字）、名字和 @用户名、时间，引用灰框、图片宫格、回复 / 转推 / 赞 /
 * 浏览四栏，以及转推、回复、视频这些小标记。
 * Card pieces after the Weibo app's cards: avatar circle (first character), name with @handle and time, the gray
 * quote box, the image grid, the reply / repost / like / view bar, and the small repost, reply and video marks.
 */

/** 引用框的底，和固件 QUOTE_GRAY 一样是 #EEEEEE。/ The quote box fill, #EEEEEE like the firmware's QUOTE_GRAY. */
val QuoteGray = Surface

/** 头像：#CCCCCC 实心圆里写名字的第一个字（固件 Material 3 头像）；dot 时右上角一个黑点（没看过）。/ Avatar: the
 *  name's first character on a filled #CCCCCC circle (the firmware's Material 3 avatar); dot adds a black dot at the
 *  top right (unseen). */
@Composable
fun Avatar(name: String, size: Dp = 40.dp, dot: Boolean = false) {
    Box(Modifier.size(size)) {
        Box(
            Modifier.fillMaxSize().clip(CircleShape).background(Selected),
            contentAlignment = Alignment.Center,
        ) {
            val first = name.ifEmpty { "X" }.let { it.substring(0, it.offsetByCodePoints(0, 1)) }
            T(first, (size.value * 0.42f).sp, bold = true)
        }
        if (dot) Box(
            Modifier.size(11.dp).align(Alignment.TopEnd).offset(x = 2.dp, y = (-2).dp).clip(CircleShape)
                .background(Paper).padding(2.dp).clip(CircleShape).background(Ink)
        )
    }
}

/** 头部：头像、粗体名字，下一行 @用户名和时间。/ Header: avatar, bold name, then @handle and time. */
@Composable
fun PostHead(user: String, screen: String, time: Double, long: Boolean = false, dot: Boolean = false) {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
        Avatar(user.ifEmpty { screen }, dot = dot)
        Column {
            T(user.ifEmpty { screen.ifEmpty { "X 用户" } }, 15.sp, bold = true, maxLines = 1)
            val t = fmtTime(time, long)
            T(listOf(if (screen.isNotEmpty()) "@$screen" else "", t).filter { it.isNotEmpty() }.joinToString(" · "),
                11.5.sp, color = Mid, maxLines = 1)
        }
    }
}

/** 卡片顶上的小字：“某某 转推”。/ The small line above a card: "<name> 转推" (reposted). */
@Composable
fun RepostedBy(name: String, modifier: Modifier = Modifier) {
    Row(modifier, verticalAlignment = Alignment.CenterVertically) {
        Glyph(0, 14.dp)
        T("$name 转推", 12.sp, color = Mid, maxLines = 1, modifier = Modifier.padding(start = 6.dp))
    }
}

/** 正文前的“回复 @某人”。/ The "回复 @handle" (replying to) line before the text. */
@Composable
fun ReplyTo(screen: String, modifier: Modifier = Modifier) {
    T("回复 @$screen", 12.5.sp, color = Mid, maxLines = 1, modifier = modifier)
}

/** 引用推文的灰框：“引用 名字 @用户名：”后接原文。/ The gray box for the quoted post: "引用 name @handle：" then
 *  its text. */
@Composable
fun QuoteBox(user: String, text: String, maxLines: Int = Int.MAX_VALUE, modifier: Modifier = Modifier, screen: String = "") {
    Box(modifier.fillMaxWidth().clip(RoundedCornerShape(7.dp)).background(QuoteGray).padding(horizontal = 10.dp, vertical = 8.dp)) {
        androidx.compose.material3.Text(
            androidx.compose.ui.text.buildAnnotatedString {
                pushStyle(androidx.compose.ui.text.SpanStyle(fontWeight = FontWeight.Bold))
                append("引用 ${user.ifEmpty { "推文" }}")
                if (screen.isNotEmpty()) append(" @$screen")
                append("：")
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
        // 没加载出来时是 #EEEEEE 圆角块，和固件 draw_pic 一样。/ An #EEEEEE rounded block until loaded, like the
        // firmware's draw_pic.
        modifier.clip(RoundedCornerShape(6.dp)).background(Surface).noRippleClick { onOpen(k) }
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
 * 转推、回复、赞、浏览的小图标（kind 0 / 1 / 2 / 3），#555555。前三个照固件 draw_glyph：两道横线带实心箭头、带尾巴
 * 的方框、实心心形；浏览固件没有，用同样粗细的四根竖条。
 * Small glyphs for reposts, replies, likes and views (kind 0 / 1 / 2 / 3) in #555555. The first three follow the
 * firmware's draw_glyph: two bars with filled arrowheads, a box with a tail and a filled heart; views has no firmware
 * glyph and uses four bars of the same weight.
 */
@Composable
fun Glyph(kind: Int, size: Dp = 17.dp, color: Color = Mid) {
    Canvas(Modifier.size(size)) {
        // 固件的图标约 28 像素宽，坐标以中心为原点。/ The firmware glyph is about 28 px wide, centered on the origin.
        val s = this.size.width / 28f
        val cx = this.size.width / 2
        val cy = this.size.height / 2
        fun o(x: Float, y: Float) = Offset(cx + x * s, cy + y * s)
        fun tri(a: Offset, b: Offset, c: Offset) =
            drawPath(Path().apply { moveTo(a.x, a.y); lineTo(b.x, b.y); lineTo(c.x, c.y); close() }, color)
        val w = 2.2f * s
        when (kind) {
            0 -> {
                drawLine(color, o(-12f, -4f), o(10f, -4f), w)
                drawLine(color, o(12f, 6f), o(-10f, 6f), w)
                tri(o(13f, -4f), o(6f, -10f), o(6f, 2f))
                tri(o(-13f, 6f), o(-6f, 0f), o(-6f, 12f))
            }
            1 -> {
                drawRect(color, o(-11f, -9f), androidx.compose.ui.geometry.Size(22f * s, 15f * s), style = Stroke(w))
                tri(o(-8f, 6f), o(-1f, 6f), o(-9f, 13f))
            }
            3 -> {
                val stroke = Stroke(w, cap = StrokeCap.Round, join = StrokeJoin.Round)
                val path = Path()
                listOf(-9f to 3f, -3f to -9f, 3f to -4f, 9f to 6f).forEach { (x, top) ->
                    path.moveTo(cx + x * s, cy + 11 * s); path.lineTo(cx + x * s, cy + top * s)
                }
                drawPath(path, color, style = stroke)
            }
            else -> {
                drawCircle(color, 6 * s, o(-5f, -3f))
                drawCircle(color, 6 * s, o(5f, -3f))
                tri(o(-11f, -1f), o(11f, -1f), o(0f, 11f))
            }
        }
    }
}

/** 底部四栏：回复、转推、赞、浏览；图标和数字都是 #555555。/ The four-column count bar: replies, reposts, likes,
 *  views; glyphs and numbers in #555555. */
@Composable
fun CountBar(reposts: Int, comments: Int, likes: Int, views: Int) {
    Row(Modifier.fillMaxWidth().height(38.dp)) {
        listOf(1 to comments, 0 to reposts, 2 to likes, 3 to views).forEach { (k, v) ->
            Row(Modifier.weight(1f).fillMaxSize(), horizontalArrangement = Arrangement.Center, verticalAlignment = Alignment.CenterVertically) {
                Glyph(k)
                T(fmtCount(v), 12.5.sp, color = Mid, modifier = Modifier.padding(start = 6.dp))
            }
        }
    }
}
