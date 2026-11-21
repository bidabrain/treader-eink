package com.treader.auth

import android.app.Application
import android.os.SystemClock
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.treader.auth.data.Account
import com.treader.auth.data.AuthState
import com.treader.auth.data.AuthStore
import com.treader.auth.data.Parsed
import com.treader.auth.data.PicoLink
import com.treader.auth.data.VaultError
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/** 页面。/ Screens. */
sealed interface Screen {
    data object Main : Screen
    data class Pass(val mode: PassMode) : Screen
    data class Detail(val id: Int) : Screen
    data object Import : Screen
    data object Send : Screen
    data object Settings : Screen
}

enum class PassMode { UNLOCK, SET1, SET2, OLD, NEW1, NEW2 }

/** “发送到小纸 Pico”的进度。/ "Send to Read Pico" progress. */
sealed interface SendState {
    data object Idle : SendState
    data object Joining : SendState
    data object Sending : SendState
    data object Waiting : SendState
    data class Done(val ok: Boolean, val msg: String) : SendState
}

/*
 * 整个 App 的状态：会话、当前页面、口令流程、每秒一跳的时间、发送进度。口令只在内存里短暂停留，用完清掉。
 * The whole app's state: session, current screen, passphrase flow, a once-a-second clock and send progress. The
 * passphrase stays in memory only briefly and is cleared after use.
 */
class AppVm(app: Application) : AndroidViewModel(app) {
    val store = AuthStore(app)
    private val link = PicoLink(app)

    var screen by mutableStateOf<Screen>(Screen.Main)
    var msg by mutableStateOf("")
    var busy by mutableStateOf(false)
    var nowSec by mutableLongStateOf(System.currentTimeMillis() / 1000)
        private set
    /** 账号表变了就加一，触发重画。/ Bumped when the table changes, to redraw. */
    var revision by mutableIntStateOf(0)
        private set
    var send by mutableStateOf<SendState>(SendState.Idle)
    /** 正在扫码：这时 Activity 进后台不算离开。/ Scanning: the activity going to the background is not leaving. */
    var scanning = false

    private var first: CharArray? = null      // 第一遍或旧口令 / First entry or the old passphrase
    private var second: CharArray? = null     // 新口令第一遍 / First entry of the new passphrase
    private var passBack: Screen = Screen.Main
    private var leftAt = 0L
    private var sendJob: Job? = null
    // 每次上锁加一；口令运算期间上过锁，运算结果作废。/ Bumped on every lock; a key derivation that a lock interrupted is
    // discarded.
    private var lockEpoch = 0

    init {
        viewModelScope.launch {
            while (true) {
                nowSec = System.currentTimeMillis() / 1000
                delay(1000 - System.currentTimeMillis() % 1000)
            }
        }
        if (store.state == AuthState.LOCKED) screen = Screen.Pass(PassMode.UNLOCK)
    }

    val state: AuthState get() = store.state
    val accounts: List<Account> get() = store.table?.items ?: emptyList()
    fun account(id: Int) = accounts.firstOrNull { it.id == id }

    /* ---- 口令 / Passphrase ---- */

    fun openPass(mode: PassMode, back: Screen = screen) {
        passBack = back
        msg = ""
        screen = Screen.Pass(mode)
    }

    private fun wipeEntries() {
        first?.fill(' ')
        second?.fill(' ')
        first = null
        second = null
    }

    fun passBack() {
        wipeEntries()
        msg = ""
        screen = passBack
    }

    /** 提交一次输入。/ Submit one entry. */
    fun submit(mode: PassMode, text: CharArray) {
        msg = ""
        if (text.size > AuthStore.PASS_MAX) { msg = "口令最多 64 位"; text.fill(' '); return }
        when (mode) {
            PassMode.UNLOCK -> {
                if (store.waitS() > 0 || text.isEmpty()) { text.fill(' '); return }
                work("正在解锁…") {
                    try {
                        store.unlock(text)
                        null
                    } finally {
                        text.fill(' ')
                    }
                }
            }
            PassMode.SET1, PassMode.NEW1 -> {
                AuthStore.passProblem(text)?.let { msg = it; text.fill(' '); return }
                if (mode == PassMode.SET1) first = text else second = text
                screen = Screen.Pass(if (mode == PassMode.SET1) PassMode.SET2 else PassMode.NEW2)
            }
            PassMode.SET2, PassMode.NEW2 -> {
                val want = if (mode == PassMode.SET2) first else second
                if (want == null || !want.contentEquals(text)) {
                    text.fill(' ')
                    if (mode == PassMode.SET2) { first?.fill(' '); first = null } else { second?.fill(' '); second = null }
                    screen = Screen.Pass(if (mode == PassMode.SET2) PassMode.SET1 else PassMode.NEW1)
                    msg = "两次输入不一样，请重新输入"
                    return
                }
                text.fill(' ')
                if (mode == PassMode.SET2) {
                    val p = first!!
                    work("正在设置…") { store.setup(p); "口令已设置" }
                } else {
                    val old = first!!
                    val new = second!!
                    work("正在修改…") { store.change(old, new); "口令已修改" }
                }
            }
            PassMode.OLD -> {
                if (store.waitS() > 0 || text.isEmpty()) { text.fill(' '); return }
                first = text
                screen = Screen.Pass(PassMode.NEW1)
            }
        }
    }

    // 口令运算约 1 秒，放后台线程。/ Key derivation takes about a second; run it off the main thread.
    private fun work(label: String, job: () -> String?) {
        busy = true
        msg = label
        val mode = (screen as? Screen.Pass)?.mode
        val epoch = lockEpoch
        viewModelScope.launch {
            val result = withContext(Dispatchers.Default) { runCatching(job) }
            busy = false
            if (epoch != lockEpoch) {
                // 运算时已经离开 App：保持上锁。/ The app was left during the work: stay locked.
                store.lock()
                wipeEntries()
                msg = ""
                revision++
                screen = if (store.state == AuthState.LOCKED) Screen.Pass(PassMode.UNLOCK) else Screen.Main
                return@launch
            }
            wipeEntries()
            result.onSuccess { done ->
                msg = done ?: ""
                revision++
                screen = if (mode == PassMode.UNLOCK) Screen.Main else if (mode == PassMode.NEW2) passBack else Screen.Main
            }.onFailure { e ->
                val kind = (e as? VaultError)?.kind
                msg = when {
                    kind == VaultError.Kind.AUTH -> {
                        val left = AuthStore.FREE_TRIES - store.fails
                        val what = if (mode == PassMode.NEW2) "原来的口令不对" else "口令不对"
                        if (left > 0) "$what，还能再试 $left 次" else "$what，请等 ${store.waitS()} 秒再试"
                    }
                    kind == VaultError.Kind.FORMAT -> "验证器文件损坏"
                    kind == VaultError.Kind.NOENT -> "找不到验证器文件"
                    else -> "出错了，请重试"
                }
                screen = Screen.Pass(when (mode) {
                    PassMode.SET2 -> PassMode.SET1
                    PassMode.NEW2 -> PassMode.OLD
                    else -> PassMode.UNLOCK
                })
            }
        }
    }

    /* ---- 上锁 / Locking ---- */

    fun lock() {
        lockEpoch++
        store.lock()
        revision++
        if (store.state == AuthState.LOCKED) openPass(PassMode.UNLOCK, Screen.Main)
    }

    /** Activity 到后台。/ The activity went to the background. */
    fun onLeave() {
        if (scanning) return
        leftAt = SystemClock.elapsedRealtime()
        if (store.autoLock == 0) lock()
    }

    /** Activity 回到前台。/ The activity came back. */
    fun onReturn() {
        scanning = false
        if (store.state == AuthState.OPEN && store.autoLock == 1 && leftAt > 0 &&
            SystemClock.elapsedRealtime() - leftAt > 5 * 60_000) lock()
        leftAt = 0
        if (store.state == AuthState.LOCKED && screen !is Screen.Pass) openPass(PassMode.UNLOCK, Screen.Main)
    }

    /* ---- 账号 / Accounts ---- */

    fun delete(id: Int) {
        val t = store.table ?: return
        val i = t.items.indexOfFirst { it.id == id }
        if (i < 0) return
        t.items.removeAt(i).wipe()
        msg = try { store.save(); "已删除" } catch (e: Exception) { "删除没能保存" }
        revision++
        if (screen is Screen.Detail) screen = Screen.Main
    }

    /** 导入；返回提示。/ Import; returns a message. */
    fun import(list: List<Parsed>): String {
        val t = store.table ?: return "验证器已上锁"
        var added = 0
        var dup = 0
        var full = false
        for (p in list) {
            val a = p.toAccount() ?: continue
            when {
                t.items.any { it.same(a) } -> dup++
                t.add(a) -> added++
                else -> full = true
            }
            a.wipe()
        }
        if (added > 0) try { store.save() } catch (e: Exception) { return "保存失败" }
        revision++
        return buildString {
            append("已导入 $added 个账号")
            if (dup > 0) append("，$dup 个已经有了")
            if (full) append("，超过 100 个的没导入")
        }
    }

    /* ---- 发送到小纸 Pico / Send to Read Pico ---- */

    fun startSend(ids: Set<Int>, ssid: String, pass: String) {
        val list = accounts.filter { it.id in ids }.map { it.copy() }
        if (list.isEmpty()) return
        sendJob?.cancel()
        sendJob = viewModelScope.launch {
            try {
                send = SendState.Joining
                link.connect(ssid, pass)?.let { send = SendState.Done(false, it); return@launch }
                send = SendState.Sending
                link.send(list)?.let { send = SendState.Done(false, it); return@launch }
                send = SendState.Waiting
                val (ok, m) = link.waitResult()
                send = SendState.Done(ok, m)
            } finally {
                list.forEach { it.wipe() }
                link.close()
            }
        }
    }

    fun cancelSend() {
        sendJob?.cancel()
        sendJob = null
        link.close()
        send = SendState.Idle
    }

    fun wipeAll() {
        cancelSend()
        store.wipe()
        wipeEntries()
        revision++
        msg = "验证器已清空"
        screen = Screen.Main
    }

    override fun onCleared() {
        link.close()
        store.lock()
        wipeEntries()
    }
}
