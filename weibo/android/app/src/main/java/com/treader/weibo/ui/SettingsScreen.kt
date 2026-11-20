package com.treader.weibo.ui

import android.content.ClipboardManager
import android.content.Context
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.treader.weibo.AppVm
import com.treader.weibo.BuildConfig
import kotlinx.coroutines.launch

/**
 * 设置，照固件“设置 › 内容来源 › 微博”：服务器地址、设备令牌、测试连接。手机上可以直接粘贴，所以没有固件的
 * “手机网页填写”，换成令牌框里的“粘贴”。令牌从不显示原值。
 * Settings, after the firmware's "Settings > Content sources > Weibo": server address, device token, connection
 * test. A phone can paste directly, so the firmware's web form becomes a "Paste" button. The token is never shown.
 */
@Composable
fun SettingsScreen(vm: AppVm, onBack: () -> Unit) {
    val scope = rememberCoroutineScope()
    var edit by remember { mutableStateOf<String?>(null) }   // "url" / "token"
    var result by remember { mutableStateOf("") }
    var testing by remember { mutableStateOf(false) }
    var url by remember { mutableStateOf(vm.prefs.url) }
    var hasToken by remember { mutableStateOf(vm.prefs.token.isNotEmpty()) }
    var listLimit by remember { mutableStateOf(vm.prefs.listLimit) }

    Box(Modifier.fillMaxSize().background(Paper)) {
        Column(Modifier.fillMaxSize()) {
            BackTitle("设置", onBack)
            Column(Modifier.padding(horizontal = 20.dp)) {
                T("微博", 13.sp, color = Mid, modifier = Modifier.padding(top = 8.dp, bottom = 4.dp))
                SettingRow("服务器地址", if (url.isEmpty()) "未设置 ›" else url.substringAfter("://") + " ›") { edit = "url" }
                HLine()
                SettingRow("设备令牌", if (hasToken) "已填写 ›" else "未填写 ›") { edit = "token" }
                HLine()
                SettingRow("测试连接", when {
                    testing -> "测试中…"
                    result.isNotEmpty() -> "$result ›"
                    url.isNotEmpty() -> "点一下测试 ›"
                    else -> "先填地址"
                }) {
                    if (url.isEmpty() || testing) return@SettingRow
                    testing = true
                    scope.launch { result = vm.testConnection(); testing = false }
                }
                HLine()
                SettingRow("列表条数", "$listLimit 条 ›") {
                    listLimit = if (listLimit >= 300) 100 else listLimit + 100
                    vm.prefs.listLimit = listLimit
                }
                T("地址和令牌在 weibo 面板的“设备与外网”页。在外面用 Cloudflare 隧道的话，地址填 https://你的域名。列表条数 100、200、300 轮换，下次刷新列表时生效。",
                    12.sp, color = Mid, lineHeight = 18.sp, modifier = Modifier.padding(top = 12.dp))
                T("关于", 13.sp, color = Mid, modifier = Modifier.padding(top = 28.dp, bottom = 4.dp))
                SettingRow("版本", BuildConfig.VERSION_NAME) {}
                HLine()
                T("和小纸 Pico 的 Treader 固件里的微博标签功能一样，只读，不点赞、不评论、不转发。", 12.sp, color = Mid,
                    lineHeight = 18.sp, modifier = Modifier.padding(top = 12.dp))
            }
        }
        when (edit) {
            "url" -> EditDialog("服务器地址", "例如 http://192.168.1.20:8092", url.ifEmpty { "http://" }, secret = false,
                onCancel = { edit = null }) { v ->
                val t = v.trim()
                if (t.isNotEmpty() && !t.startsWith("http://") && !t.startsWith("https://")) return@EditDialog "地址要以 http:// 或 https:// 开头"
                vm.saveSettings(if (t == "http://" || t == "https://") "" else t, null)
                url = vm.prefs.url
                result = ""
                edit = null
                null
            }
            "token" -> EditDialog("设备令牌", if (hasToken) "已经填过，不显示原值；留空表示不改" else "粘贴 weibo 面板里的设备令牌",
                "", secret = true, onCancel = { edit = null }) { v ->
                vm.saveSettings(null, v)
                hasToken = vm.prefs.token.isNotEmpty()
                result = ""
                edit = null
                null
            }
        }
    }
}

@Composable
private fun SettingRow(label: String, value: String, onClick: () -> Unit) {
    Row(Modifier.fillMaxWidth().height(52.dp).noRippleClick(onClick), verticalAlignment = Alignment.CenterVertically) {
        T(label, 16.sp, modifier = Modifier.weight(1f))
        T(value, 14.sp, color = Mid, maxLines = 1, modifier = Modifier.weight(1.4f), align = androidx.compose.ui.text.style.TextAlign.End)
    }
}

/** 编辑框：返回错误文字就留在框里，返回 null 表示保存好了。/ Edit box: a returned error keeps it open; null = saved. */
@Composable
private fun EditDialog(
    title: String, hint: String, initial: String, secret: Boolean, onCancel: () -> Unit, onSave: (String) -> String?,
) {
    val ctx = LocalContext.current
    var text by remember { mutableStateOf(initial) }
    var error by remember { mutableStateOf("") }
    var show by remember { mutableStateOf(false) }
    Box(Modifier.fillMaxSize().background(Paper.copy(alpha = 0.7f)).noRippleClick(onCancel), contentAlignment = Alignment.Center) {
        Column(
            Modifier.padding(horizontal = 24.dp).fillMaxWidth().clip(RoundedCornerShape(14.dp)).background(Paper)
                .border(OutlineW, Mid, RoundedCornerShape(14.dp)).noRippleClick {}.padding(18.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            T(title, 18.sp, bold = true)
            Box(Modifier.fillMaxWidth().border(OutlineW, Mid, RoundedCornerShape(8.dp)).padding(12.dp)) {
                BasicTextField(
                    value = text, onValueChange = { text = it; error = "" }, singleLine = true,
                    textStyle = TextStyle(fontSize = 16.sp, color = Ink),
                    visualTransformation = if (secret && !show) PasswordVisualTransformation() else VisualTransformation.None,
                    keyboardOptions = KeyboardOptions(keyboardType = if (secret) KeyboardType.Password else KeyboardType.Uri),
                    modifier = Modifier.fillMaxWidth(),
                )
            }
            T(error.ifEmpty { hint }, 12.sp, color = if (error.isEmpty()) Mid else Ink, lineHeight = 18.sp)
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                Pill("粘贴", false, Modifier.weight(1f), height = 40.dp) {
                    val cm = ctx.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                    cm.primaryClip?.getItemAt(0)?.coerceToText(ctx)?.toString()?.trim()?.let { text = it; error = "" }
                }
                if (secret) Pill(if (show) "隐藏" else "显示", false, Modifier.weight(1f), height = 40.dp) { show = !show }
            }
            Spacer(Modifier.height(2.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                Pill("取消", false, Modifier.weight(1f)) { onCancel() }
                Pill("保存", true, Modifier.weight(1f)) { onSave(text)?.let { error = it } }
            }
        }
    }
}
