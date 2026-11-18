package com.treader.weibo

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.ui.Modifier
import androidx.lifecycle.viewmodel.compose.viewModel
import com.treader.weibo.ui.ImagesScreen
import com.treader.weibo.ui.ListScreen
import com.treader.weibo.ui.Paper
import com.treader.weibo.ui.SettingsScreen
import com.treader.weibo.ui.TextScreen

/** 只有一个页面栈：列表 → 正文 / 图片 / 评论；右上角设置键进设置。/ One stack: list → text / images / comments; the
 *  corner key opens settings. */
class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            val vm: AppVm = viewModel()
            BackHandler(enabled = vm.screen != Screen.List || vm.waiting != null) {
                if (vm.waiting != null) vm.cancelWait() else vm.backToList()
            }
            Box(Modifier.fillMaxSize().background(Paper).safeDrawingPadding()) {
                when (val s = vm.screen) {
                    Screen.List -> ListScreen(vm, onSettings = { vm.screen = Screen.Settings })
                    is Screen.Text -> TextScreen(vm, s)
                    is Screen.Images -> ImagesScreen(vm, s)
                    Screen.Settings -> SettingsScreen(vm, onBack = { vm.backToList() })
                }
            }
        }
    }
}
