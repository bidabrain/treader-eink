package com.treader.tomato.ui

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
import coil.compose.SubcomposeAsyncImage
import java.io.File

/** 文字封面（固件 tr_cover / tr_mini_cover）：白底双线框、书名粗体居中；窄的小封面把前几个字竖排。dark 为黑底白字（读完）。
 *  Text cover after the firmware's tr_cover / tr_mini_cover: white with a double frame and the bold title centered;
 *  narrow small covers stack the first characters vertically. dark = inverted (finished). */
@Composable
fun TextCover(title: String, modifier: Modifier = Modifier, size: TextUnit = 13.sp, dark: Boolean = false) {
    val ink = if (dark) Paper else Ink
    BoxWithConstraints(modifier.background(if (dark) Ink else Paper).border(1.dp, Ink)) {
        if (maxWidth < 60.dp) {
            // 小封面：竖排前几个字。/ Small cover: first characters stacked vertically.
            val chars = title.codePoints().toArray().map { String(Character.toChars(it)) }
            val n = ((maxHeight - 6.dp) / (size.value * 1.15f).dp).toInt().coerceAtLeast(1)
            Column(Modifier.fillMaxSize(), verticalArrangement = Arrangement.Center, horizontalAlignment = Alignment.CenterHorizontally) {
                chars.take(n).forEach { T(it, size, bold = true, color = ink, lineHeight = size * 1.15f) }
            }
        } else {
            Box(Modifier.fillMaxSize().padding(4.dp).border(1.dp, ink).padding(6.dp), contentAlignment = Alignment.Center) {
                T(title, size, bold = true, color = ink, maxLines = 3, align = TextAlign.Center, lineHeight = size * 1.25f)
            }
        }
    }
}

/** 有图用图（本地文件或网址），没有或加载失败就是文字封面。/ The image when there is one (file or URL), else a text
 *  cover. */
@Composable
fun Cover(title: String, image: Any?, modifier: Modifier = Modifier, size: TextUnit = 13.sp, dark: Boolean = false) {
    val src = when (image) {
        is File -> image.takeIf { it.exists() }
        is String -> image.takeIf { it.isNotEmpty() }
        else -> null
    }
    if (src == null) { TextCover(title, modifier, size, dark); return }
    Box(modifier.border(1.dp, Outline)) {
        SubcomposeAsyncImage(
            model = src, contentDescription = null, contentScale = ContentScale.Crop,
            // 和墨水屏一样只显示灰度。/ Grayscale only, like the e-ink screen.
            colorFilter = ColorFilter.colorMatrix(ColorMatrix().apply { setToSaturation(0f) }),
            modifier = Modifier.fillMaxSize(),
            loading = { TextCover(title, Modifier.fillMaxSize(), size, dark) },
            error = { TextCover(title, Modifier.fillMaxSize(), size, dark) },
        )
    }
}
