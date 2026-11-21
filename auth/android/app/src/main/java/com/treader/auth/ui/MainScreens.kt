package com.treader.auth.ui

import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.treader.auth.AppVm
import com.treader.auth.PassMode
import com.treader.auth.Screen
import com.treader.auth.data.Account
import com.treader.auth.data.AuthState
import com.treader.auth.data.AuthStore
import com.treader.auth.data.Totp

/*
 * 主页（引导或验证码列表）、单个账号和口令页，排版照固件 app_tr_auth.c：每行发行方 · 名称、大号验证码、右边倒计时条，
 * 剩 5 秒内小字显示下一个码；列表按页翻，不滚动。
 * Home (intro or the code list), a single account and the passphrase screen, laid out like the firmware's
 * app_tr_auth.c: each row shows issuer · name, a large code and a countdown bar on the right, with the next code in
 * small text in the last 5 seconds; the list turns by pages instead of scrolling.
 */

private const val NEAR_S = 5

@Composable
fun MainScreen(vm: AppVm) {
    var confirmWipe by remember { mutableIntStateOf(0) }
    var deleting by remember { mutableStateOf<Account?>(null) }
    val st = vm.state
    @Suppress("UNUSED_VARIABLE") val rev = vm.revision
    Box(Modifier.fillMaxSize()) {
        Column(Modifier.fillMaxSize()) {
            Row(Modifier.fillMaxWidth().height(64.dp).padding(horizontal = 20.dp), verticalAlignment = Alignment.CenterVertically) {
                T("验证器", 26.sp, bold = true, modifier = Modifier.weight(1f))
                if (st == AuthState.OPEN) {
                    IconBtn({ ImportIcon() }) { vm.msg = ""; vm.screen = Screen.Import }
                    IconBtn({ SendIcon() }) { vm.msg = ""; vm.send = com.treader.auth.SendState.Idle; vm.screen = Screen.Send }
                    IconBtn({ LockIcon() }) { vm.lock() }
                }
                IconBtn({ SettingsIcon() }) { vm.msg = ""; vm.screen = Screen.Settings }
            }
            if (vm.msg.isNotEmpty()) T(vm.msg, 13.sp, color = Mid, modifier = Modifier.padding(horizontal = 20.dp))
            when (st) {
                AuthState.OPEN -> CodeList(vm) { deleting = it }
                AuthState.UNSET -> Intro(
                    "在手机上离线显示两步验证码，和 Google Authenticator 算出的一样，不需要服务器。\n\n" +
                        "先设一个验证器口令（至少 8 位，字母和数字都有）。账号加密存在这个 App 里，要口令加这台手机才能解开；" +
                        "忘了口令没有找回办法，只能清空重来。手机上的 Google Authenticator 请一直保留作备份。\n\n" +
                        "导入后还可以把账号发送到小纸 Pico。",
                    "设置口令",
                ) { vm.openPass(PassMode.SET1, Screen.Main) }
                AuthState.LOCKED -> Intro("验证器已上锁。", "解锁") { vm.openPass(PassMode.UNLOCK, Screen.Main) }
                AuthState.BROKEN -> Intro(
                    "验证器文件和这台手机里配套的密钥对不上（App 数据被清过一部分），解不开。可以清空后重新设置，再重新导入。",
                    "清空并重新设置",
                ) { confirmWipe = 1 }
            }
        }
        deleting?.let { a ->
            Dialog("删除账号", "删除“${a.label}”？手机上的 Google Authenticator 不受影响。", "删除",
                onCancel = { deleting = null }) { vm.delete(a.id); deleting = null }
        }
        WipeDialogs(confirmWipe, { confirmWipe = it }) { vm.wipeAll() }
    }
}

@Composable
fun WipeDialogs(step: Int, setStep: (Int) -> Unit, onWipe: () -> Unit) {
    when (step) {
        1 -> Dialog("清空验证器？", "会删除这个 App 里的加密文件和配套的密钥，所有账号都没了。之后要重新设置口令，再重新导入。", "清空",
            onCancel = { setStep(0) }) { setStep(2) }
        2 -> Dialog("再确认一次", "清空以后无法恢复。确定清空验证器？", "确认清空", onCancel = { setStep(0) }) { setStep(0); onWipe() }
    }
}

@Composable
private fun IconBtn(icon: @Composable () -> Unit, onClick: () -> Unit) =
    Box(Modifier.size(44.dp).noRippleClick(onClick), contentAlignment = Alignment.Center) { icon() }

@Composable
private fun Intro(text: String, button: String, onClick: () -> Unit) {
    Column(Modifier.padding(horizontal = 20.dp, vertical = 16.dp), verticalArrangement = Arrangement.spacedBy(24.dp)) {
        T(text, 16.sp, lineHeight = 26.sp)
        Pill(button, true, Modifier.width(200.dp)) { onClick() }
    }
}

@Composable
private fun CodeList(vm: AppVm, onLongPress: (Account) -> Unit) {
    val list = vm.accounts
    val now = vm.nowSec
    if (list.isEmpty()) {
        T("还没有账号。点右上角的“导入”：扫 Google Authenticator 导出的二维码、粘贴 otpauth 链接，或手动填密钥。",
            16.sp, lineHeight = 26.sp, modifier = Modifier.padding(20.dp))
        return
    }
    var page by remember { mutableIntStateOf(0) }
    BoxWithConstraints(Modifier.fillMaxSize()) {
        val rowH = 92.dp
        val per = ((maxHeight - 56.dp) / rowH).toInt().coerceAtLeast(1)
        val pages = (list.size + per - 1) / per
        if (page >= pages) page = pages - 1
        val turn: (Int) -> Unit = { page = (page + it + pages) % pages }
        Column(Modifier.fillMaxSize().swipeTurn(turn)) {
            Column(Modifier.weight(1f).padding(horizontal = 20.dp)) {
                list.drop(page * per).take(per).forEach { a ->
                    CodeRow(a, now, Modifier.height(rowH).pointerInput(a.id) {
                        detectTapGestures(onTap = { vm.screen = Screen.Detail(a.id) }, onLongPress = { onLongPress(a) })
                    })
                    HLine()
                }
            }
            if (pages > 1) Pager(page, pages, turn, Modifier.padding(bottom = 8.dp))
        }
    }
}

@Composable
private fun CodeRow(a: Account, now: Long, modifier: Modifier) {
    val left = Totp.remaining(a, now)
    Column(modifier.fillMaxWidth(), verticalArrangement = Arrangement.Center) {
        T(a.label, 13.sp, maxLines = 1)
        Spacer(Modifier.height(4.dp))
        Row(verticalAlignment = Alignment.CenterVertically) {
            T(Totp.format(Totp.code(a, now), a.digits), 32.sp, bold = true, modifier = Modifier.weight(1f))
            Column(horizontalAlignment = Alignment.End) {
                CountdownBar(left, a.period, Modifier.width(64.dp))
                Spacer(Modifier.height(6.dp))
                T(if (left <= NEAR_S) "下一个 ${Totp.format(Totp.code(a, now + a.period), a.digits)}" else "", 12.sp, maxLines = 1)
            }
        }
    }
}

@Composable
fun DetailScreen(vm: AppVm, id: Int) {
    val a = vm.account(id) ?: run { vm.screen = Screen.Main; return }
    val now = vm.nowSec
    var deleting by remember { mutableStateOf(false) }
    val left = Totp.remaining(a, now)
    Box(Modifier.fillMaxSize()) {
        Column(Modifier.fillMaxSize().pointerInput(id) { detectTapGestures(onLongPress = { deleting = true }) }) {
            BackTitle(a.issuer.ifEmpty { a.name }) { vm.screen = Screen.Main }
            Column(Modifier.fillMaxWidth().padding(horizontal = 20.dp), horizontalAlignment = Alignment.CenterHorizontally) {
                if (a.issuer.isNotEmpty()) T(a.name, 15.sp, maxLines = 1, modifier = Modifier.padding(top = 24.dp))
                T(Totp.format(Totp.code(a, now), a.digits), 56.sp, bold = true, maxLines = 1,
                    modifier = Modifier.padding(top = 36.dp))
                CountdownBar(left, a.period, Modifier.fillMaxWidth(0.8f).padding(top = 28.dp))
                T("还剩 $left 秒", 14.sp, modifier = Modifier.padding(top = 12.dp))
                T("下一个 ${Totp.format(Totp.code(a, now + a.period), a.digits)}", 15.sp, modifier = Modifier.padding(top = 16.dp))
                T("长按删除", 12.sp, color = Mid, modifier = Modifier.padding(top = 40.dp))
            }
        }
        if (deleting) Dialog("删除账号", "删除“${a.label}”？手机上的 Google Authenticator 不受影响。", "删除",
            onCancel = { deleting = false }) { deleting = false; vm.delete(a.id) }
    }
}

@Composable
fun PassScreen(vm: AppVm, mode: PassMode) {
    var text by remember(mode) { mutableStateOf("") }
    val focus = remember { FocusRequester() }
    @Suppress("UNUSED_VARIABLE") val tick = vm.nowSec
    val wait = vm.store.waitS()
    val title = when (mode) {
        PassMode.UNLOCK -> "输入验证器口令"
        PassMode.SET1 -> "设置验证器口令"
        PassMode.SET2 -> "再输一次口令"
        PassMode.OLD -> "输入原来的口令"
        PassMode.NEW1 -> "输入新口令"
        PassMode.NEW2 -> "再输一次新口令"
    }
    val go = when (mode) {
        PassMode.UNLOCK -> "解锁"
        PassMode.SET1, PassMode.OLD, PassMode.NEW1 -> "下一步"
        else -> "确定"
    }
    val submit = {
        if (!vm.busy) {
            val chars = text.toCharArray()
            text = ""
            vm.submit(mode, chars)
        }
    }
    LaunchedEffect(mode) { runCatching { focus.requestFocus() } }
    Column(Modifier.fillMaxSize()) {
        if (mode == PassMode.UNLOCK) {
            Row(Modifier.fillMaxWidth().height(64.dp).padding(horizontal = 20.dp), verticalAlignment = Alignment.CenterVertically) {
                T("验证器", 26.sp, bold = true, modifier = Modifier.weight(1f))
                // 忘了口令时从这里去清空。/ The way to clearing when the passphrase is forgotten.
                IconBtn({ SettingsIcon() }) { if (!vm.busy) { vm.msg = ""; vm.screen = Screen.Settings } }
            }
        } else {
            BackTitle("验证器") { if (!vm.busy) vm.passBack() }
        }
        Column(Modifier.padding(horizontal = 20.dp), verticalArrangement = Arrangement.spacedBy(14.dp)) {
            T(title, 17.sp, bold = true, modifier = Modifier.padding(top = 8.dp))
            Field(text, { text = it; if (!vm.busy) vm.msg = "" }, secret = true, onDone = submit,
                modifier = Modifier.focusRequester(focus))
            val hint = when {
                vm.msg.isNotEmpty() -> vm.msg
                wait > 0 && (mode == PassMode.UNLOCK || mode == PassMode.OLD) -> "输错太多次，请等 $wait 秒再试"
                mode == PassMode.SET1 || mode == PassMode.NEW1 -> "至少 ${AuthStore.PASS_MIN} 位，字母和数字都要有。忘了口令只能清空验证器，再重新导入。"
                mode == PassMode.UNLOCK -> "口令只在这台手机上用，不会发到任何地方。"
                else -> ""
            }
            T(hint, 13.sp, color = Mid, lineHeight = 20.sp)
            Pill(go, true, Modifier.fillMaxWidth()) { submit() }
            if (mode == PassMode.UNLOCK) T("忘了口令：设置 › 清空验证器，再重新导入。", 12.sp, color = Mid,
                align = TextAlign.Start, modifier = Modifier.padding(top = 8.dp))
        }
    }
}
