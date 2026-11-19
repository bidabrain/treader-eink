package com.treader.tomato.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ColorFilter
import androidx.compose.ui.graphics.ColorMatrix
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import coil.compose.SubcomposeAsyncImage
import java.io.File

/** 文字封面：灰色渐变，左上白字书名（固件 tr_cover / tr_mini_cover 的样子）。/ Text cover: a gray gradient with the title
 *  in white at the top left, like the firmware's tr_cover / tr_mini_cover. */
@Composable
fun TextCover(title: String, modifier: Modifier = Modifier, size: TextUnit = 13.sp) {
    Box(
        modifier.background(Brush.linearGradient(listOf(Color(0xFF3A3A3A), Color(0xFFBDBDBD), Color(0xFF4A4A4A))))
            .border(1.dp, Ink),
        contentAlignment = Alignment.TopStart,
    ) {
        T(title, size, bold = true, color = Paper, maxLines = 4, lineHeight = size * 1.3f, modifier = Modifier.padding(6.dp))
    }
}

/** 有图用图（本地文件或网址），没有或加载失败就是文字封面。/ The image when there is one (file or URL), else a text
 *  cover. */
@Composable
fun Cover(title: String, image: Any?, modifier: Modifier = Modifier, size: TextUnit = 13.sp) {
    val src = when (image) {
        is File -> image.takeIf { it.exists() }
        is String -> image.takeIf { it.isNotEmpty() }
        else -> null
    }
    if (src == null) { TextCover(title, modifier, size); return }
    Box(modifier.border(1.dp, Ink)) {
        SubcomposeAsyncImage(
            model = src, contentDescription = null, contentScale = ContentScale.Crop,
            // 和墨水屏一样只显示灰度。/ Grayscale only, like the e-ink screen.
            colorFilter = ColorFilter.colorMatrix(ColorMatrix().apply { setToSaturation(0f) }),
            modifier = Modifier.fillMaxSize(),
            loading = { TextCover(title, Modifier.fillMaxSize(), size) },
            error = { TextCover(title, Modifier.fillMaxSize(), size) },
        )
    }
}
