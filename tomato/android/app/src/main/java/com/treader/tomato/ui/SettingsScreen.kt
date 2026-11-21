package com.treader.tomato.ui

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
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.verticalScroll
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
import com.treader.tomato.AppVm
import com.treader.tomato.BuildConfig
import com.treader.tomato.TURN_ZONES
import kotlinx.coroutines.launch

/**
 * 设置，照固件“设置 › 内容来源 › 番茄小说”和“同步”：番茄服务器地址和密码、测试连接、书城榜单地址、WebDAV。
 * 手机能直接粘贴，所以没有固件的“手机网页填写”，换成输入框旁的“粘贴”。密码从不显示原值。App 里没有内置服务器。
 * Settings after the firmware's "Settings > Content sources > Tomato" and "Sync": server address and password,
 * connection test, bookstore URL and WebDAV. A phone can paste, so the firmware's web form becomes a "Paste" button.
 * Passwords are never shown. The app has no built-in server.
 */
@Composable
fun SettingsScreen(vm: AppVm) {
    val scope = rememberCoroutineScope()
    var edit by remember { mutableStateOf<String?>(null) }
    var result by remember { mutableStateOf("") }
    var testing by remember { mutableStateOf(false) }
    var server by remember { mutableStateOf(vm.prefs.server) }
    var hasPassword by remember { mutableStateOf(vm.prefs.password.isNotEmpty()) }
    var store by remember { mutableStateOf(vm.prefs.storeUrl) }
    var dav by remember { mutableStateOf(vm.prefs.davUrl) }
    var davUser by remember { mutableStateOf(vm.prefs.davUser) }
    var hasDavPassword by remember { mutableStateOf(vm.prefs.davPassword.isNotEmpty()) }

    Box(Modifier.fillMaxSize().background(Paper)) {
        Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState())) {
            PageTitle("设置", Modifier.padding(start = 20.dp, top = 14.dp, bottom = 8.dp))
            Column(Modifier.padding(horizontal = 20.dp)) {
                T("番茄小说", 13.sp, color = Mid, modifier = Modifier.padding(top = 8.dp, bottom = 4.dp))
                SettingRow("番茄服务器", if (server.isEmpty()) "未设置 ›" else server.substringAfter("://") + " ›") { edit = "url" }
                HLine()
                SettingRow("服务器密码", if (hasPassword) "已填写 ›" else "未填写 ›") { edit = "password" }
                HLine()
                SettingRow("测试连接", when {
                    testing -> "测试中…"
                    result.isNotEmpty() -> "$result ›"
                    server.isNotEmpty() -> "点一下测试 ›"
                    else -> "先填地址"
                }) {
                    if (server.isEmpty() || testing) return@SettingRow
                    testing = true
                    scope.launch { result = vm.testServer(); testing = false }
                }
                HLine()
                SettingRow("书城榜单", if (store.isEmpty()) "默认榜单 ›" else store.substringAfter("://") + " ›") { edit = "store" }
                T("地址填 tomato 面板“设备与外网”里的地址（局域网 http://服务器IP:18423，在外面用 Cloudflare 隧道填 https://你的域名），密码填那里的设备密码。自己部署的官方下载器也可以。",
                    12.sp, color = Mid, lineHeight = 18.sp, modifier = Modifier.padding(top = 12.dp))
                T("同步", 13.sp, color = Mid, modifier = Modifier.padding(top = 28.dp, bottom = 4.dp))
                SettingRow("WebDAV", if (dav.isEmpty()) "未设置 ›" else dav.substringAfter("://") + " ›") { edit = "dav" }
                HLine()
                SettingRow("账号", if (davUser.isEmpty()) "未填写 ›" else "$davUser ›") { edit = "davUser" }
                HLine()
                SettingRow("密码", if (hasDavPassword) "已填写 ›" else "未填写 ›") { edit = "davPassword" }
                HLine()
                SettingRow("立即同步", when {
                    vm.davBusy -> "同步中…"
                    vm.davMsg.isNotEmpty() -> vm.davMsg
                    dav.isEmpty() -> "先设置 WebDAV"
                    else -> "进度和番茄书架 ›"
                }) { if (dav.isNotEmpty()) vm.davSync() }
                T("和小纸 Pico 用同一个同步文件（目录下的 treader-sync.json）：阅读进度和番茄书架两边互通，书的文件不传。坚果云要用“应用密码”。",
                    12.sp, color = Mid, lineHeight = 18.sp, modifier = Modifier.padding(top = 12.dp))
                T("阅读", 13.sp, color = Mid, modifier = Modifier.padding(top = 28.dp, bottom = 4.dp))
                SettingRow("字体", if (vm.serif) "宋体 ›" else "黑体 ›") { vm.changeSerif(!vm.serif) }
                HLine()
                SettingRow("翻页区域", TURN_ZONES[vm.turnZone].first + " ›") { vm.changeTurnZone(vm.turnZone + 1) }
                HLine()
                T("看书时点屏幕：" + TURN_ZONES[vm.turnZone].second + "。点一下换一种，和小纸 Pico 的设置一样。", 12.sp, color = Mid,
                    lineHeight = 18.sp, modifier = Modifier.padding(top = 12.dp))
                T("关于", 13.sp, color = Mid, modifier = Modifier.padding(top = 28.dp, bottom = 4.dp))
                SettingRow("版本", BuildConfig.VERSION_NAME) {}
                HLine()
                T("和小纸 Pico 的 Treader 固件里的书库、番茄标签一样。", 12.sp, color = Mid,
                    lineHeight = 18.sp, modifier = Modifier.padding(top = 12.dp, bottom = 24.dp))
            }
        }
        when (edit) {
            "url" -> EditDialog("番茄服务器", "例如 http://192.168.1.20:18423", server.ifEmpty { "http://" }, secret = false,
                onCancel = { edit = null }) { v ->
                val t = v.trim()
                if (t.isNotEmpty() && t != "http://" && t != "https://" && !t.startsWith("http://") && !t.startsWith("https://"))
                    return@EditDialog "地址要以 http:// 或 https:// 开头"
                vm.saveServer(if (t == "http://" || t == "https://") "" else t, null)
                server = vm.prefs.server
                result = ""
                edit = null
                null
            }
            "password" -> EditDialog("服务器密码", if (hasPassword) "已经填过，不显示原值；留空表示不改" else "tomato 面板里的设备密码",
                "", secret = true, onCancel = { edit = null }) { v ->
                vm.saveServer(null, v)
                hasPassword = vm.prefs.password.isNotEmpty()
                result = ""
                edit = null
                null
            }
            "dav" -> EditDialog("WebDAV", "目录地址，例如 https://dav.jianguoyun.com/dav/treader", dav.ifEmpty { "https://" }, secret = false,
                onCancel = { edit = null }) { v ->
                val t = v.trim()
                if (t.isNotEmpty() && t != "https://" && !t.startsWith("http://") && !t.startsWith("https://")) return@EditDialog "地址要以 http:// 或 https:// 开头"
                vm.prefs.davUrl = if (t == "https://" || t == "http://") "" else t
                dav = vm.prefs.davUrl
                edit = null
                null
            }
            "davUser" -> EditDialog("WebDAV 账号", "坚果云填登录邮箱", davUser, secret = false, onCancel = { edit = null }) { v ->
                vm.prefs.davUser = v
                davUser = vm.prefs.davUser
                edit = null
                null
            }
            "davPassword" -> EditDialog("WebDAV 密码", if (hasDavPassword) "已经填过，不显示原值；留空表示不改" else "坚果云要用“应用密码”",
                "", secret = true, onCancel = { edit = null }) { v ->
                if (v.isNotBlank()) vm.prefs.davPassword = v.trim()
                hasDavPassword = vm.prefs.davPassword.isNotEmpty()
                edit = null
                null
            }
            "store" -> EditDialog("书城榜单", "FanqieRankTracker 的地址；留空用默认榜单", store, secret = false,
                onCancel = { edit = null }) { v ->
                val t = v.trim()
                if (t.isNotEmpty() && !t.startsWith("http://") && !t.startsWith("https://")) return@EditDialog "地址要以 http:// 或 https:// 开头"
                vm.prefs.storeUrl = t
                store = vm.prefs.storeUrl
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
            Modifier.padding(horizontal = 24.dp).fillMaxWidth().clip(RoundedCornerShape(20.dp)).background(Paper)
                .border(OutlineWidth, Outline, RoundedCornerShape(20.dp)).noRippleClick {}.padding(20.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            T(title, 18.sp, bold = true)
            Box(Modifier.fillMaxWidth().clip(RoundedCornerShape(12.dp)).background(Surface).padding(horizontal = 14.dp, vertical = 12.dp)) {
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
