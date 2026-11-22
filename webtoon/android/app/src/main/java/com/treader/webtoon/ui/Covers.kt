package com.treader.webtoon.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.graphics.ColorMatrix
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import coil.ImageLoader
import coil.compose.SubcomposeAsyncImage

/** 文字封面（固件 tr_cover / tr_mini_cover）：白底双线框、书名粗体居中；窄的小封面把前几个字竖排。
 *  Text cover after the firmware's tr_cover / tr_mini_cover: white with a double frame and the bold title centered;
 *  narrow small covers stack the first characters vertically. */
@Composable
fun TextCover(title: String, modifier: Modifier = Modifier, size: TextUnit = 13.sp) {
    BoxWithConstraints(modifier.background(Paper).border(1.dp, Ink)) {
        if (maxWidth < 60.dp) {
            // 小封面：竖排前几个字。/ Small cover: first characters stacked vertically.
            val chars = title.codePoints().toArray().map { String(Character.toChars(it)) }
            val n = ((maxHeight - 6.dp) / (size.value * 1.15f).dp).toInt().coerceAtLeast(1)
            Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.Center, horizontalAlignment = Alignment.CenterHorizontally) {
                chars.take(n).forEach { T(it, size, bold = true, lineHeight = size * 1.15f) }
            }
        } else {
            Box(Modifier.fillMaxSize().padding(4.dp).border(1.dp, Ink).padding(6.dp), contentAlignment = Alignment.Center) {
                T(title, size, bold = true, maxLines = 3, align = TextAlign.Center, lineHeight = size * 1.25f)
            }
        }
    }
}

/** 服务器的灰度封面（带令牌）；没有或加载失败就是文字封面。/ The server's grayscale cover (with the token); a text
 *  cover when there is none or it fails. */
@Composable
fun Cover(title: String, url: String?, images: ImageLoader, modifier: Modifier = Modifier, size: TextUnit = 13.sp) {
    if (url.isNullOrEmpty()) { TextCover(title, modifier, size); return }
    Box(modifier.border(1.dp, Outline)) {
        SubcomposeAsyncImage(
            model = url, contentDescription = null, imageLoader = images, contentScale = ContentScale.Crop,
            // 和墨水屏一样只显示灰度。/ Grayscale only, like the e-ink screen.
            colorFilter = ColorFilter.colorMatrix(ColorMatrix().apply { setToSaturation(0f) }),
            modifier = Modifier.fillMaxSize(),
            loading = { TextCover(title, Modifier.fillMaxSize(), size) },
            error = { TextCover(title, Modifier.fillMaxSize(), size) },
        )
    }
}
