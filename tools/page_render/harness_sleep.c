/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：睡眠与唤醒页的渲染测试。/ English: Render bench for the sleep page.
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "app_sleep.c"   // 按 -I 找：当前源码或 SRC_ROOT / Found via -I: current sources or SRC_ROOT
#include "page_render.h"
#include "nvs.h"

const char* hr_page = "sleep";

app_wake_source_t app_last_wake_source(void) { return APP_WAKE_PICKUP; }
enum EpdDrawError update_display_full(EpdiyHighlevelState* hl) { (void)hl; return EPD_DRAW_SUCCESS; }
void guard_draw_result(EpdiyHighlevelState* hl, enum EpdDrawError result) { (void)hl; (void)result; }
esp_err_t nvs_open(const char* ns, nvs_open_mode_t m, nvs_handle_t* h) { (void)ns; (void)m; (void)h; return ESP_FAIL; }

void hr_scenes(void) {
    app_ctx_t ctx = {.fb = hr_fb};
    s_ui = (sleep_ui_t){.pickup_ok = true, .wake_reason = 5, .alarm_mode = 1, .wake_on_charge = 1, .pmu_ok = true};
    hr_no_bar_check();
    render(&ctx, hr_fb);
    hr_shot("default");
    s_ui.pmu_ok = false;
    app_settings_set_sleep_mode(APP_SLEEP_OFF);
    hr_no_bar_check();
    render(&ctx, hr_fb);
    hr_shot("off_no_pmu");
    // 试睡前的整屏提示（推屏是替身）。/ The trial-sleep notice (presenting is a stand-in).
    hr_no_bar_check();
    sleep_show_notice(&ctx, APP_SLEEP_DEEP);
    hr_shot("notice");
}
