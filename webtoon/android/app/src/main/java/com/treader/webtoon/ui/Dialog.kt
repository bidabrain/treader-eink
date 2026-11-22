package com.treader.webtoon.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/** 对话框的一个按钮；filled 是黑底的主按钮。/ One dialog button; filled is the black primary one. */
data class Choice(val label: String, val filled: Boolean = false, val onClick: () -> Unit)

/**
 * Material 3 对话框：白底大圆角、深灰细框，和固件一样；按钮竖排，点外面关闭。
 * Material 3 dialog: white, large radius, thin dark gray outline as on the firmware; buttons stacked; a tap outside
 * closes it.
 */
@Composable
fun ChoiceDialog(title: String, text: String, choices: List<Choice>, onDismiss: () -> Unit) {
    val shape = RoundedCornerShape(16.dp)
    Box(Modifier.fillMaxSize().background(Paper.copy(alpha = 0.7f)).noRippleClick(onDismiss), contentAlignment = Alignment.Center) {
        Column(
            Modifier.padding(horizontal = 28.dp).fillMaxWidth().clip(shape).background(Paper)
                .border(OutlineWidth, Outline, shape).noRippleClick {}.verticalScroll(rememberScrollState()).padding(18.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp),
        ) {
            T(title, 18.sp, bold = true)
            if (text.isNotEmpty()) T(text, 14.sp, color = Mid, lineHeight = 20.sp)
            choices.forEach { c -> Pill(c.label, c.filled, Modifier.fillMaxWidth()) { c.onClick() } }
        }
    }
}
