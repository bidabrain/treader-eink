package com.treader.auth.ui

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/*
 * 这个 App 自己的图标和控件：图标照固件 tr_ui.c 的 40 格画法（导入、上锁、钥匙），外加“发送到小纸 Pico”用的
 * 向上箭头；对话框和输入框照其他 App。
 * This app's own glyphs and widgets: icons follow the firmware tr_ui.c 40-unit drawings (import, lock, key) plus an
 * upward arrow for "Send to Read Pico"; the dialog and input box follow the other apps.
 */

private fun DrawScope.seg(p: Float, ax: Float, ay: Float, bx: Float, by: Float, c: Color) =
    drawLine(c, Offset(ax * p, ay * p), Offset(bx * p, by * p), 3 * p, StrokeCap.Square)

/** 导入（TR_ICON_IMPORT：向下箭头进托盘）。/ Import (TR_ICON_IMPORT: an arrow down into a tray). */
@Composable
fun ImportIcon(modifier: Modifier = Modifier, color: Color = Ink) = Canvas(modifier.size(24.dp)) {
    val p = size.width / 40f
    seg(p, 20f, 6f, 20f, 26f, color); seg(p, 12f, 18f, 20f, 26f, color); seg(p, 28f, 18f, 20f, 26f, color)
    seg(p, 7f, 28f, 7f, 33f, color); seg(p, 33f, 28f, 33f, 33f, color); seg(p, 7f, 33f, 33f, 33f, color)
}

/** 发送到小纸 Pico：向上箭头出托盘。/ Send to Read Pico: an arrow up out of a tray. */
@Composable
fun SendIcon(modifier: Modifier = Modifier, color: Color = Ink) = Canvas(modifier.size(24.dp)) {
    val p = size.width / 40f
    seg(p, 20f, 26f, 20f, 6f, color); seg(p, 12f, 14f, 20f, 6f, color); seg(p, 28f, 14f, 20f, 6f, color)
    seg(p, 7f, 28f, 7f, 33f, color); seg(p, 33f, 28f, 33f, 33f, color); seg(p, 7f, 33f, 33f, 33f, color)
}

/** 上锁（TR_ICON_LOCK：半圆锁环、锁身、锁孔）。/ Lock (TR_ICON_LOCK: half-ring shackle, body, keyhole). */
@Composable
fun LockIcon(modifier: Modifier = Modifier, color: Color = Ink) = Canvas(modifier.size(24.dp)) {
    val p = size.width / 40f
    drawArc(color, 180f, 180f, false, Offset(12 * p, 7 * p), Size(16 * p, 16 * p), style = Stroke(3 * p))
    seg(p, 12f, 15f, 12f, 19f, color); seg(p, 28f, 15f, 28f, 19f, color)
    drawRoundRect(color, Offset(8 * p, 19 * p), Size(24 * p, 17 * p), androidx.compose.ui.geometry.CornerRadius(3 * p),
        style = Stroke(2.5f * p))
    drawCircle(color, 2 * p, Offset(20 * p, 26 * p))
    seg(p, 20f, 26f, 20f, 31f, color)
}

/** 钥匙（TR_ICON_KEY）。/ Key (TR_ICON_KEY). */
@Composable
fun KeyIcon(modifier: Modifier = Modifier, color: Color = Ink) = Canvas(modifier.size(24.dp)) {
    val p = size.width / 40f
    drawCircle(color, 7 * p, Offset(12 * p, 20 * p), style = Stroke(3 * p))
    seg(p, 19f, 20f, 36f, 20f, color); seg(p, 30f, 20f, 30f, 27f, color); seg(p, 35f, 20f, 35f, 26f, color)
}

/** 倒计时条：黑框里按剩余时间填黑（和固件一样）。/ Countdown bar: black fill for the time left in a black frame,
 *  as on the firmware. */
@Composable
fun CountdownBar(left: Int, period: Int, modifier: Modifier = Modifier) = Canvas(modifier.height(7.dp)) {
    val s = 1.dp.toPx()
    drawRect(Ink, style = Stroke(s))
    val w = (size.width - 4 * s) * left / period
    if (w > 0) drawRect(Ink, Offset(2 * s, 2 * s), Size(w, size.height - 4 * s))
}

/** 输入框（口令时打点）。/ An input box (dots for passphrases). */
@Composable
fun Field(
    value: String, onChange: (String) -> Unit, secret: Boolean = false, singleLine: Boolean = true,
    minHeight: Int = 0, keyboard: KeyboardType = KeyboardType.Text, onDone: (() -> Unit)? = null,
    modifier: Modifier = Modifier,
) {
    Box(modifier.fillMaxWidth().border(OutlineW, Mid, RoundedCornerShape(8.dp)).padding(12.dp)) {
        BasicTextField(
            value = value, onValueChange = onChange, singleLine = singleLine,
            textStyle = TextStyle(fontSize = 16.sp, color = Ink),
            visualTransformation = if (secret) PasswordVisualTransformation() else VisualTransformation.None,
            keyboardOptions = KeyboardOptions(
                keyboardType = if (secret) KeyboardType.Password else keyboard,
                autoCorrectEnabled = false,
                imeAction = if (onDone != null) ImeAction.Done else ImeAction.Default,
            ),
            keyboardActions = KeyboardActions(onDone = { onDone?.invoke() }),
            modifier = Modifier.fillMaxWidth().then(if (minHeight > 0) Modifier.height(minHeight.dp) else Modifier),
        )
    }
}

/** 对话框：标题、说明、取消和确认。/ Dialog: title, text, cancel and confirm. */
@Composable
fun Dialog(title: String, text: String, ok: String, onCancel: () -> Unit, onOk: () -> Unit) {
    Box(Modifier.fillMaxSize().background(Paper.copy(alpha = 0.7f)).noRippleClick(onCancel), contentAlignment = Alignment.Center) {
        Column(
            Modifier.padding(horizontal = 24.dp).fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(Paper)
                .border(OutlineW, Mid, RoundedCornerShape(16.dp)).noRippleClick {}.padding(18.dp),
            verticalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            T(title, 18.sp, bold = true)
            T(text, 14.sp, lineHeight = 21.sp)
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                Pill("取消", false, Modifier.weight(1f)) { onCancel() }
                Pill(ok, true, Modifier.weight(1f)) { onOk() }
            }
        }
    }
}

/** 设置行：左边名字，右边值。/ A settings row: label on the left, value on the right. */
@Composable
fun SettingRow(label: String, value: String, onClick: () -> Unit) {
    Row(Modifier.fillMaxWidth().height(52.dp).noRippleClick(onClick), verticalAlignment = Alignment.CenterVertically) {
        T(label, 16.sp, modifier = Modifier.weight(1f))
        T(value, 14.sp, color = Mid, maxLines = 1, modifier = Modifier.weight(1.4f),
            align = androidx.compose.ui.text.style.TextAlign.End)
    }
}
