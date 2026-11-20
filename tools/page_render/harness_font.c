/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：字体页的渲染测试。字体列表来自容器里的 /sdcard/fonts（见 run 命令）。/ English: Render bench for the font
 * picker. The list comes from /sdcard/fonts inside the container (see the run command).
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "app_font_pick.c"   // 按 -I 找：当前源码或 SRC_ROOT / Found via -I: current sources or SRC_ROOT
#include "page_render.h"
#include "nvs.h"

const char* hr_page = "font";

esp_err_t nvs_open(const char* ns, nvs_open_mode_t m, nvs_handle_t* h) { (void)ns; (void)m; (void)h; return ESP_FAIL; }
esp_err_t read_pico_sd_get_info(read_pico_sd_info_t* info) {
    memset(info, 0, sizeof(*info));
    info->present = info->mounted = true;
    return ESP_OK;
}

void hr_scenes(void) {
    app_ctx_t ctx = {.fb = hr_fb};
    hr_no_bar_check();
    render(&ctx, hr_fb);
    hr_shot("first_page");
    ctx.leaf = 1;
    hr_no_bar_check();
    render(&ctx, hr_fb);
    hr_shot("second_page");
    ctx.leaf = 99;
    hr_no_bar_check();
    render(&ctx, hr_fb);
    hr_shot("last_page");
}
