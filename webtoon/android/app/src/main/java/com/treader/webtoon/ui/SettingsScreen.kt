package com.treader.webtoon.ui

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
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
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
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.treader.webtoon.AppVm
import com.treader.webtoon.BuildConfig
import com.treader.webtoon.data.Prefs
import kotlinx.coroutines.launch

/**
 * 设置，照固件“设置 › 内容来源 › 漫画”：服务器地址、设备令牌、测试连接，加上语言版本、简体显示和每次最多下载几话。
 * 手机上可以直接粘贴，所以没有固件的“手机网页填写”，换成编辑框里的“粘贴”。令牌从不显示原值。
 * Settings after the firmware's "Settings > Content sources > Comics": server address, device token and connection
 * test, plus language edition, simplified display and the per-check download cap. A phone can paste directly, so the
 * firmware's web form becomes a "Paste" button. The token is never shown.
 */
@Composable
fun SettingsScreen(vm: AppVm) {
    val scope = rememberCoroutineScope()
    var edit by remember { mutableStateOf<String?>(null) }   // "url" / "token"
    var result by remember { mutableStateOf("") }
    var testing by remember { mutableStateOf(false) }
    var url by remember { mutableStateOf(vm.prefs.url) }
    var hasToken by remember { mutableStateOf(vm.prefs.token.isNotEmpty()) }
    var lang by remember { mutableStateOf(vm.prefs.lang) }
    var simp by remember { mutableStateOf(vm.prefs.simplified) }
    var perCheck by remember { mutableStateOf(vm.prefs.perCheck) }

    Box(Modifier.fillMaxSize().background(Paper)) {
        Column(Modifier.fillMaxSize()) {
            TitleRow("设置")
            Column(Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(horizontal = 20.dp)) {
                T("webtoon 服务器", 13.sp, color = Mid, modifier = Modifier.padding(top = 8.dp, bottom = 4.dp))
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
                T("地址和令牌在 webtoon 面板的“设备与外网”页。在外面用 Cloudflare 隧道的话，地址填 https://你的域名。",
                    12.sp, color = Mid, lineHeight = 18.sp, modifier = Modifier.padding(top = 12.dp))

                T("发现", 13.sp, color = Mid, modifier = Modifier.padding(top = 28.dp, bottom = 4.dp))
                SettingRow("语言版本", (Prefs.LANGS.firstOrNull { it.first == lang }?.second ?: lang) + " ›") {
                    val i = Prefs.LANGS.indexOfFirst { it.first == lang }
                    lang = Prefs.LANGS[(i + 1) % Prefs.LANGS.size].first
                    vm.prefs.lang = lang
                    vm.storeReset()
                }
                HLine()
                if (lang == "zh-hant") {
                    SettingRow("简体显示", if (simp) "开 ›" else "关 ›") {
                        simp = !simp
                        vm.prefs.simplified = simp
                        vm.storeReset()
                    }
                    HLine()
                }
                SettingRow("每次最多下载", (if (perCheck == 0) "全部" else "$perCheck 话") + " ›") {
                    val i = Prefs.PER_CHECK.indexOf(perCheck)
                    perCheck = Prefs.PER_CHECK[(i + 1) % Prefs.PER_CHECK.size]
                    vm.prefs.perCheck = perCheck
                }
                HLine()
                T("语言版本只影响发现页和以后的订阅，已订阅的作品还按订阅时的版本追更。简体显示把中文版的作品名、简介、话名转成简体，图里的字不变。“检查更新”一次最多下载这么多话，剩下的下次再点。",
                    12.sp, color = Mid, lineHeight = 18.sp, modifier = Modifier.padding(top = 12.dp))

                T("关于", 13.sp, color = Mid, modifier = Modifier.padding(top = 28.dp, bottom = 4.dp))
                SettingRow("版本", BuildConfig.VERSION_NAME) {}
                HLine()
                T("和小纸 Pico 固件里的发现漫画功能一样，读你自己部署的 webtoon 服务器。只有公开免费的话，只给自己离线看，不要分享下载的漫画。",
                    12.sp, color = Mid, lineHeight = 18.sp, modifier = Modifier.padding(top = 12.dp, bottom = 20.dp))
            }
        }
        when (edit) {
            "url" -> EditDialog("服务器地址", "例如 http://192.168.1.20:8095", url.ifEmpty { "http://" }, secret = false,
                onCancel = { edit = null }) { v ->
                val t = v.trim()
                if (t.isNotEmpty() && !t.startsWith("http://") && !t.startsWith("https://")) return@EditDialog "地址要以 http:// 或 https:// 开头"
                vm.saveServer(if (t == "http://" || t == "https://") "" else t, null)
                url = vm.prefs.url
                result = ""
                edit = null
                null
            }
            "token" -> EditDialog("设备令牌", if (hasToken) "已经填过，不显示原值；留空表示不改" else "粘贴 webtoon 面板里的设备令牌",
                "", secret = true, onCancel = { edit = null }) { v ->
                vm.saveServer(null, v)
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
        T(value, 14.sp, color = Mid, maxLines = 1, modifier = Modifier.weight(1.4f), align = TextAlign.End)
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
    val shape = RoundedCornerShape(16.dp)
    Box(Modifier.fillMaxSize().background(Paper.copy(alpha = 0.7f)).noRippleClick(onCancel), contentAlignment = Alignment.Center) {
        Column(
            Modifier.padding(horizontal = 24.dp).fillMaxWidth().clip(shape).background(Paper)
                .border(OutlineWidth, Outline, shape).noRippleClick {}.padding(18.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            T(title, 18.sp, bold = true)
            Box(Modifier.fillMaxWidth().border(OutlineWidth, Outline, RoundedCornerShape(8.dp)).padding(12.dp)) {
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
