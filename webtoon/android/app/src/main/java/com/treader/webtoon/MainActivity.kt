package com.treader.webtoon

import android.os.Bundle
import android.view.KeyEvent
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.isImeVisible
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.ui.Modifier
import androidx.lifecycle.viewmodel.compose.viewModel
import com.treader.webtoon.ui.DetailScreen
import com.treader.webtoon.ui.Paper
import com.treader.webtoon.ui.ReaderScreen
import com.treader.webtoon.ui.SettingsScreen
import com.treader.webtoon.ui.ShelfTab
import com.treader.webtoon.ui.StoreTab
import com.treader.webtoon.ui.TabBar

/** 底部三个标签：我的漫画、发现页、设置（和固件的标签栏一样）；作品详情盖在当前标签上；阅读时整屏是漫画，不显示标签栏。
 *  Three bottom tabs: shelf, store and settings, like the firmware's tab bar; title details cover the current tab;
 *  while reading the comic fills the screen without it. */
@OptIn(ExperimentalLayoutApi::class)
class MainActivity : ComponentActivity() {
    private var vmRef: AppVm? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            val vm: AppVm = viewModel()
            vmRef = vm
            BackHandler(enabled = vm.reading != null) { vm.closeReader() }
            BackHandler(enabled = vm.reading == null && vm.detail != null) { vm.detail = null }
            BackHandler(enabled = vm.reading == null && vm.detail == null && vm.tab == Tab.SHELF && vm.openSeries != null) {
                vm.openSeries = null
            }
            BackHandler(enabled = vm.reading == null && vm.detail == null && vm.tab == Tab.STORE && vm.searchResults != null) {
                vm.closeSearch()
            }
            BackHandler(enabled = vm.reading == null && vm.detail == null && vm.tab != Tab.SHELF && vm.searchResults == null) {
                vm.tab = Tab.SHELF
            }
            val r = vm.reading
            if (r != null) {
                Box(Modifier.fillMaxSize().background(Paper).safeDrawingPadding()) { ReaderScreen(vm, r) }
                return@setContent
            }
            val d = vm.detail
            if (d != null) {
                Box(Modifier.fillMaxSize().background(Paper).safeDrawingPadding()) { DetailScreen(vm, d) }
                return@setContent
            }
            Column(Modifier.fillMaxSize().background(Paper).safeDrawingPadding()) {
                Box(Modifier.weight(1f)) {
                    when (vm.tab) {
                        Tab.SHELF -> ShelfTab(vm)
                        Tab.STORE -> StoreTab(vm)
                        Tab.SETTINGS -> SettingsScreen(vm)
                    }
                }
                // 键盘打开时不显示标签栏。/ Hide the tab bar while the keyboard is open.
                if (!WindowInsets.isImeVisible) TabBar(vm.tab.ordinal) { vm.detail = null; vm.tab = Tab.entries[it] }
            }
        }
    }

    // 阅读时音量键翻页：减小下一页，加大上一页。/ Volume keys turn pages while reading: down = next, up = previous.
    override fun onKeyDown(keyCode: Int, event: KeyEvent?): Boolean {
        val vm = vmRef
        if (vm?.reading != null && (keyCode == KeyEvent.KEYCODE_VOLUME_DOWN || keyCode == KeyEvent.KEYCODE_VOLUME_UP)) {
            vm.turn(if (keyCode == KeyEvent.KEYCODE_VOLUME_DOWN) 1 else -1)
            return true
        }
        return super.onKeyDown(keyCode, event)
    }

    override fun onPause() {
        vmRef?.flushProgress()
        super.onPause()
    }
}
