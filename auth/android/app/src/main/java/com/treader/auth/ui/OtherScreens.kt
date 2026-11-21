package com.treader.auth.ui

import android.content.ClipboardManager
import android.content.Context
import android.os.Build
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
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
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.journeyapps.barcodescanner.ScanContract
import com.journeyapps.barcodescanner.ScanOptions
import com.treader.auth.AppVm
import com.treader.auth.BuildConfig
import com.treader.auth.PassMode
import com.treader.auth.Screen
import com.treader.auth.SendState
import com.treader.auth.data.AuthState
import com.treader.auth.data.Base32
import com.treader.auth.data.OtpParse
import com.treader.auth.data.ParseError
import com.treader.auth.data.Parsed

/*
 * 导入、发送到小纸 Pico 和设置。导入的三种方式和固件导入网页一样（扫码代替选照片）；识别结果先列出来勾选，导入后清空
 * 所有输入。设置页和固件“设置 › 内容来源 › 验证器”一样：修改口令、自动上锁、清空。
 * Import, Send to Read Pico and settings. The three import ways match the firmware import page (scanning instead of
 * picking a photo); results are listed for ticking first and every input is cleared after importing. Settings match
 * the firmware's Settings > Content sources > Authenticator: change passphrase, auto-lock, clear.
 */

private fun scanOptions(prompt: String) = ScanOptions().apply {
    setDesiredBarcodeFormats(ScanOptions.QR_CODE)
    setPrompt(prompt)
    setBeepEnabled(false)
    setOrientationLocked(true)
}

@Composable
private fun Check(on: Boolean, enabled: Boolean = true) {
    val shape = RoundedCornerShape(4.dp)
    Box(
        Modifier.size(20.dp).clip(shape)
            .then(if (on) Modifier.background(Ink) else Modifier.border(OutlineW, if (enabled) Mid else Line, shape)),
        contentAlignment = Alignment.Center,
    ) { if (on) CheckMark(13.dp, Paper) }
}

@Composable
fun ImportScreen(vm: AppVm, act: ComponentActivity) {
    val ctx = LocalContext.current
    val found = remember { mutableStateListOf<Parsed>() }
    val picked = remember { mutableStateListOf<String>() }
    var paste by remember { mutableStateOf("") }
    var note by remember { mutableStateOf("") }
    var manual by remember { mutableStateOf(false) }
    var mName by remember { mutableStateOf("") }
    var mIssuer by remember { mutableStateOf("") }
    var mSecret by remember { mutableStateOf("") }
    var mAlgo by remember { mutableIntStateOf(0) }
    var mDigits by remember { mutableIntStateOf(0) }
    var mPeriod by remember { mutableIntStateOf(0) }
    val batches = remember { mutableStateListOf<com.treader.auth.data.Batch>() }

    fun addAll(list: List<Parsed>) {
        val have = found.map { it.key }.toMutableSet()
        list.forEach { p -> if (have.add(p.key)) { found += p; if (p.unsupported.isEmpty()) picked += p.key } }
    }
    fun take(text: String): String {
        val r = OtpParse.lines(text)
        addAll(r.accounts)
        batches += r.batches
        val miss = OtpParse.missing(batches.toList())
        return buildString {
            append("识别到 ${r.accounts.size} 个账号")
            if (r.errors.isNotEmpty()) append("；${r.errors.size} 行没认出来：${r.errors.first().second}")
            miss.forEach { (size, m) -> append("；分批导出（共 $size 张）还缺第 ${m.joinToString("、")} 张") }
        }
    }
    val scan = rememberLauncherForActivityResult(ScanContract()) { r ->
        val text = r.contents ?: return@rememberLauncherForActivityResult
        note = if (text.startsWith("otpauth", ignoreCase = true)) take(text) else "这不是验证器二维码"
    }
    // 离开导入页就清掉输入和识别结果。/ Leaving the import screen clears the inputs and results.
    DisposableEffect(Unit) { onDispose { found.clear(); picked.clear(); paste = ""; mSecret = "" } }

    Column(Modifier.fillMaxSize()) {
        BackTitle("导入账号") { vm.screen = Screen.Main }
        Column(Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(horizontal = 20.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)) {
            T("扫码", 13.sp, color = Mid, modifier = Modifier.padding(top = 4.dp))
            T("在 Google Authenticator 里点“转移账号 → 导出账号”，用这里扫它显示的二维码（分好几张就一张张扫）。网站给的单个账号二维码也能扫。",
                13.sp, lineHeight = 20.sp)
            Pill("扫二维码", true, Modifier.fillMaxWidth()) {
                vm.scanning = true
                scan.launch(scanOptions("对准 Google Authenticator 导出的二维码"))
            }
            T("粘贴链接", 13.sp, color = Mid, modifier = Modifier.padding(top = 8.dp))
            Field(paste, { paste = it }, singleLine = false, minHeight = 72)
            Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                Pill("粘贴", false, Modifier.weight(1f), height = 40.dp) {
                    val cm = ctx.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                    cm.primaryClip?.getItemAt(0)?.coerceToText(ctx)?.toString()?.let { paste = it }
                }
                Pill("识别", false, Modifier.weight(1f), height = 40.dp) {
                    if (paste.isNotBlank()) { note = take(paste); paste = "" }
                }
            }
            T(if (manual) "手动添加 ▴" else "手动添加 ▾", 13.sp, color = Mid,
                modifier = Modifier.padding(top = 8.dp).noRippleClick { manual = !manual })
            if (manual) {
                T("名称", 13.sp); Field(mName, { mName = it })
                T("发行方（可不填）", 13.sp); Field(mIssuer, { mIssuer = it })
                T("密钥（base32）", 13.sp); Field(mSecret, { mSecret = it }, secret = true)
                Segment(listOf("SHA1", "SHA256", "SHA512"), mAlgo, { mAlgo = it })
                Segment(listOf("6 位", "8 位"), mDigits, { mDigits = it })
                Segment(listOf("30 秒", "60 秒"), mPeriod, { mPeriod = it })
                Pill("加到下面", false, Modifier.fillMaxWidth(), height = 40.dp) {
                    note = when {
                        mName.isBlank() -> "请填名称"
                        Base32.decode(mSecret) == null -> "密钥不对：只能有字母 A～Z 和数字 2～7"
                        else -> {
                            addAll(listOf(Parsed(mIssuer.trim(), mName.trim(), Base32.norm(mSecret),
                                listOf("SHA1", "SHA256", "SHA512")[mAlgo], if (mDigits == 0) 6 else 8, if (mPeriod == 0) 30 else 60)))
                            mName = ""; mIssuer = ""; mSecret = ""
                            "已加到下面的列表"
                        }
                    }
                }
            }
            if (note.isNotEmpty()) T(note, 13.sp, lineHeight = 20.sp)
            if (found.isNotEmpty()) {
                T("识别到的账号（勾上的会导入）", 13.sp, color = Mid, modifier = Modifier.padding(top = 8.dp))
                found.forEach { p ->
                    val ok = p.unsupported.isEmpty()
                    Row(Modifier.fillMaxWidth().padding(vertical = 6.dp).noRippleClick {
                        if (ok) { if (p.key in picked) picked.remove(p.key) else picked += p.key }
                    }, verticalAlignment = Alignment.CenterVertically) {
                        Check(p.key in picked, ok)
                        Spacer(Modifier.width(12.dp))
                        Column(Modifier.weight(1f)) {
                            T(p.label, 15.sp, color = if (ok) Ink else Mid, maxLines = 1)
                            val tags = listOfNotNull(
                                p.unsupported.ifEmpty { null },
                                if (ok && p.algo != "SHA1") p.algo else null,
                                if (ok && p.digits != 6) "${p.digits} 位" else null,
                                if (ok && p.period != 30) "${p.period} 秒" else null,
                            ) + p.notes
                            if (tags.isNotEmpty()) T(tags.joinToString(" · "), 12.sp, color = Mid)
                        }
                    }
                    HLine()
                }
            }
            Spacer(Modifier.height(8.dp))
        }
        if (found.isNotEmpty()) {
            Pill("导入 ${picked.size} 个", true, Modifier.fillMaxWidth().padding(20.dp)) {
                if (picked.isEmpty()) return@Pill
                vm.msg = vm.import(found.filter { it.key in picked })
                found.clear(); picked.clear(); batches.clear(); note = ""
                vm.screen = Screen.Main
            }
        }
    }
}

@Composable
fun SendScreen(vm: AppVm, act: ComponentActivity) {
    val list = vm.accounts
    val picked = remember { mutableStateListOf<Int>().apply { addAll(list.map { it.id }) } }
    var note by remember { mutableStateOf("") }
    val old = Build.VERSION.SDK_INT < Build.VERSION_CODES.Q
    val scan = rememberLauncherForActivityResult(ScanContract()) { r ->
        val text = r.contents ?: return@rememberLauncherForActivityResult
        val wifi = OtpParse.wifi(text)
        if (wifi == null) note = "这不是小纸 Pico 的热点二维码" else { note = ""; vm.startSend(picked.toSet(), wifi.first, wifi.second) }
    }
    val s = vm.send
    Column(Modifier.fillMaxSize()) {
        BackTitle("发送到小纸 Pico") { vm.cancelSend(); vm.screen = Screen.Main }
        Column(Modifier.weight(1f).verticalScroll(rememberScrollState()).padding(horizontal = 20.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp)) {
            T(
                "1. 小纸 Pico 升级到 1.0.0 以上，打开验证器，点右上角的“导入”，屏幕上会出现热点二维码。\n" +
                    (if (old) "2. 在手机的 Wi-Fi 设置里连上设备热点（口令在设备屏幕上），再回到这里。\n3. 选好账号，点“发送”。\n"
                    else "2. 选好要发送的账号，点“扫设备二维码并发送”，手机会自动连上设备热点（系统会问一下）。\n") +
                    "最后在设备上点“导入”确认。账号只在手机和设备之间传，不经过路由器和互联网。",
                13.sp, lineHeight = 21.sp,
            )
            Row(verticalAlignment = Alignment.CenterVertically) {
                T("${picked.size} / ${list.size} 个", 13.sp, color = Mid, modifier = Modifier.weight(1f))
                T(if (picked.size == list.size) "全不选" else "全选", 13.sp, modifier = Modifier.noRippleClick {
                    if (picked.size == list.size) picked.clear() else { picked.clear(); picked.addAll(list.map { it.id }) }
                })
            }
            list.forEach { a ->
                Row(Modifier.fillMaxWidth().padding(vertical = 6.dp).noRippleClick {
                    if (a.id in picked) picked.remove(a.id) else picked += a.id
                }, verticalAlignment = Alignment.CenterVertically) {
                    Check(a.id in picked)
                    Spacer(Modifier.width(12.dp))
                    T(a.label, 15.sp, maxLines = 1)
                }
                HLine()
            }
            Spacer(Modifier.height(8.dp))
        }
        Column(Modifier.padding(20.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            val status = when (s) {
                SendState.Idle -> note
                SendState.Joining -> "正在连设备热点…"
                SendState.Sending -> "正在发送…"
                SendState.Waiting -> "请在小纸 Pico 屏幕上点“导入”确认"
                is SendState.Done -> s.msg
            }
            if (status.isNotEmpty()) T(status, 14.sp, bold = s is SendState.Waiting, lineHeight = 21.sp)
            when (s) {
                SendState.Idle, is SendState.Done -> Pill(if (old) "发送" else "扫设备二维码并发送", true, Modifier.fillMaxWidth()) {
                    if (picked.isEmpty()) { note = "先选要发送的账号"; vm.send = SendState.Idle; return@Pill }
                    vm.send = SendState.Idle
                    if (old) vm.startSend(picked.toSet(), "", "")
                    else { vm.scanning = true; scan.launch(scanOptions("对准小纸 Pico 屏幕上的热点二维码")) }
                }
                else -> Pill("取消", false, Modifier.fillMaxWidth()) { vm.cancelSend() }
            }
        }
    }
}

@Composable
fun SettingsScreen(vm: AppVm) {
    var wipe by remember { mutableIntStateOf(0) }
    var auto by remember { mutableIntStateOf(vm.store.autoLock) }
    val st = vm.state
    Box(Modifier.fillMaxSize()) {
        Column(Modifier.fillMaxSize()) {
            BackTitle("设置") { vm.screen = if (st == AuthState.LOCKED) Screen.Pass(PassMode.UNLOCK) else Screen.Main }
            Column(Modifier.padding(horizontal = 20.dp)) {
                val state = when (st) {
                    AuthState.OPEN -> "已解锁 · ${vm.accounts.size} 个账号"
                    AuthState.LOCKED -> "已上锁"
                    AuthState.UNSET -> "还没设置口令"
                    AuthState.BROKEN -> "需要清空后重新设置"
                }
                T(state, 13.sp, color = Mid, modifier = Modifier.padding(top = 8.dp, bottom = 4.dp))
                SettingRow("修改口令", if (st == AuthState.OPEN || st == AuthState.LOCKED) "›" else "未设置") {
                    if (st == AuthState.OPEN || st == AuthState.LOCKED) vm.openPass(PassMode.OLD, Screen.Settings)
                }
                HLine()
                SettingRow("自动上锁", if (auto == 1) "离开 5 分钟后" else "离开 App 就上锁") {
                    auto = 1 - auto
                    vm.store.autoLock = auto
                }
                HLine()
                SettingRow("清空验证器", "›") { if (st != AuthState.UNSET) wipe = 1 }
                if (vm.msg.isNotEmpty()) T(vm.msg, 13.sp, modifier = Modifier.padding(top = 8.dp))
                T("验证器口令只管这个 App。账号加密存在 App 里，解锁要口令和这台手机里的密钥一起，不进系统备份，换手机不会带走。" +
                    "忘了口令没有找回办法，只能清空验证器，再从 Google Authenticator 重新导入。自动上锁点一下切换。",
                    12.sp, color = Mid, lineHeight = 18.sp, modifier = Modifier.padding(top = 12.dp))
                T("关于", 13.sp, color = Mid, modifier = Modifier.padding(top = 28.dp, bottom = 4.dp))
                SettingRow("版本", BuildConfig.VERSION_NAME) {}
                HLine()
                T("离线算验证码，不连任何服务器；只在“发送到小纸 Pico”时连设备热点。扫码用 ZXing（Apache-2.0）。",
                    12.sp, color = Mid, lineHeight = 18.sp, modifier = Modifier.padding(top = 12.dp))
            }
        }
        WipeDialogs(wipe, { wipe = it }) { vm.wipeAll() }
    }
}
