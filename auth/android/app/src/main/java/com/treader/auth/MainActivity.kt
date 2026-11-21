package com.treader.auth

import android.os.Bundle
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.ui.Modifier
import androidx.lifecycle.ViewModelProvider
import com.treader.auth.data.AuthState
import com.treader.auth.ui.DetailScreen
import com.treader.auth.ui.ImportScreen
import com.treader.auth.ui.MainScreen
import com.treader.auth.ui.Paper
import com.treader.auth.ui.PassScreen
import com.treader.auth.ui.SendScreen
import com.treader.auth.ui.SettingsScreen

/**
 * 一个页面栈：主页（列表或引导）→ 单个账号 / 导入 / 发送 / 设置 / 口令。整个窗口禁止截图和录屏，最近任务里也看不到。
 * One stack: home (list or intro) → single account / import / send / settings / passphrase. The whole window blocks
 * screenshots and screen recording and shows blank in recents.
 */
class MainActivity : ComponentActivity() {
    private lateinit var vm: AppVm

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // 发布版禁止截图；调试版放开，方便在模拟器里截图检查界面。/ Release builds block screenshots; debug builds
        // allow them so the UI can be checked on an emulator.
        if (!BuildConfig.DEBUG) window.setFlags(WindowManager.LayoutParams.FLAG_SECURE, WindowManager.LayoutParams.FLAG_SECURE)
        vm = ViewModelProvider(this)[AppVm::class.java]
        setContent {
            val s = vm.screen
            BackHandler(enabled = s != Screen.Main && !(s is Screen.Pass && s.mode == PassMode.UNLOCK) && !vm.busy) {
                when (s) {
                    is Screen.Pass -> vm.passBack()
                    Screen.Send -> { vm.cancelSend(); vm.screen = Screen.Main }
                    else -> vm.screen = Screen.Main
                }
            }
            Box(Modifier.fillMaxSize().background(Paper).safeDrawingPadding()) {
                when (s) {
                    Screen.Main -> MainScreen(vm)
                    is Screen.Pass -> PassScreen(vm, s.mode)
                    is Screen.Detail -> DetailScreen(vm, s.id)
                    Screen.Import -> ImportScreen(vm, this@MainActivity)
                    Screen.Send -> SendScreen(vm, this@MainActivity)
                    Screen.Settings -> SettingsScreen(vm)
                }
            }
        }
    }

    override fun onStart() {
        super.onStart()
        vm.onReturn()
    }

    override fun onStop() {
        super.onStop()
        vm.onLeave()
    }

    // 已上锁时，口令页之外的页面不该停着。/ Once locked, no screen other than the passphrase may stay.
    override fun onResume() {
        super.onResume()
        if (vm.state == AuthState.LOCKED && vm.screen !is Screen.Pass && vm.screen != Screen.Settings)
            vm.openPass(PassMode.UNLOCK, Screen.Main)
    }
}
