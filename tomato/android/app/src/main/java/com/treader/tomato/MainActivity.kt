package com.treader.tomato

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.activity.compose.setContent
import android.view.KeyEvent
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.isImeVisible
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.ui.Modifier
import androidx.lifecycle.viewmodel.compose.viewModel
import com.treader.tomato.ui.LibraryTab
import com.treader.tomato.ui.Paper
import com.treader.tomato.ui.ReaderScreen
import com.treader.tomato.ui.SettingsScreen
import com.treader.tomato.ui.TabBar
import com.treader.tomato.ui.TomatoTab

/** 底部三个标签：书库、番茄、设置（和固件的标签栏一样）；阅读时整屏是书，不显示标签栏。/ Three bottom tabs: library,
 *  Tomato and settings, like the firmware's tab bar; while reading the book fills the screen without it. */
@OptIn(ExperimentalLayoutApi::class)
class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            val vm: AppVm = viewModel()
            vmRef = vm
            val inSub = vm.tab == Tab.TOMATO && vm.tomatoView != TomatoView.Shelf
            BackHandler(enabled = vm.reader != null) { vm.closeReader() }
            BackHandler(enabled = vm.reader == null && (inSub || vm.tab != Tab.LIBRARY)) {
                when {
                    inSub -> vm.tomatoView = when (vm.tomatoView) {
                        TomatoView.Results -> TomatoView.Search
                        is TomatoView.Detail -> if ((vm.tomatoView as TomatoView.Detail).fromStore) TomatoView.Shelf else TomatoView.Results
                        else -> TomatoView.Shelf
                    }
                    else -> vm.tab = Tab.LIBRARY
                }
            }
            val r = vm.reader
            if (r != null) {
                Box(Modifier.fillMaxSize().background(Paper).safeDrawingPadding()) { ReaderScreen(vm, r) }
                return@setContent
            }
            Column(Modifier.fillMaxSize().background(Paper).safeDrawingPadding()) {
                Box(Modifier.weight(1f)) {
                    when (vm.tab) {
                        Tab.LIBRARY -> LibraryTab(vm)
                        Tab.TOMATO -> TomatoTab(vm, onOpenBook = { vm.openTomatoBook(it) }, onSettings = { vm.tab = Tab.SETTINGS })
                        Tab.SETTINGS -> SettingsScreen(vm)
                    }
                }
                // 键盘打开时不显示标签栏。/ Hide the tab bar while the keyboard is open.
                if (!WindowInsets.isImeVisible) TabBar(vm.tab.ordinal) { vm.tab = Tab.entries[it] }
            }
        }
    }

    private var vmRef: AppVm? = null

    // 阅读时音量键翻页：减小下一页，加大上一页。/ Volume keys turn pages while reading: down = next, up = previous.
    override fun onKeyDown(keyCode: Int, event: KeyEvent?): Boolean {
        val vm = vmRef
        if (vm?.reader != null && (keyCode == KeyEvent.KEYCODE_VOLUME_DOWN || keyCode == KeyEvent.KEYCODE_VOLUME_UP)) {
            vm.turn(if (keyCode == KeyEvent.KEYCODE_VOLUME_DOWN) 1 else -1)
            return true
        }
        return super.onKeyDown(keyCode, event)
    }

    override fun onPause() {
        vmRef?.reader?.save()
        super.onPause()
    }
}
