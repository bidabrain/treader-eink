/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：USB 传书页的渲染测试。/ English: Render bench for the USB transfer page.
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "app_usb.c"   // 按 -I 找：当前源码或 SRC_ROOT / Found via -I: current sources or SRC_ROOT
#include "page_render.h"

const char* hr_page = "usb";

static bool s_card = true;
esp_err_t read_pico_sd_get_info(read_pico_sd_info_t* info) {
    memset(info, 0, sizeof(*info));
    info->present = info->mounted = s_card;
    info->free_bytes = 12ull * 1024 * 1024 * 1024 + 345678901ull;
    return ESP_OK;
}

static void shot(app_ctx_t* ctx, const char* name) {
    hr_no_bar_check();
    render(ctx, hr_fb);
    hr_shot(name);
}

void hr_scenes(void) {
    app_ctx_t ctx = {.fb = hr_fb};
    on_enter(&ctx);
    s_card = true;
    shot(&ctx, "intro");
    snprintf(s_msg, sizeof(s_msg), "后台正在下载或同步，完成后再用 USB 传书。");
    shot(&ctx, "intro_msg");
    s_msg[0] = 0;
    s_card = false;
    shot(&ctx, "intro_nocard");
    s_card = true;
    s_view = V_RUN;
    s_failed = false;
    memset(&s_st, 0, sizeof(s_st));
    s_st.state = READ_PICO_USB_WAITING;
    shot(&ctx, "run_waiting");
    s_st.state = READ_PICO_USB_CONNECTED;
    s_st.write_bytes = 9876543210ull;
    s_st.read_bytes = 123456789ull;
    shot(&ctx, "run_connected");
    s_st.state = READ_PICO_USB_GONE;
    s_power = true;
    shot(&ctx, "run_suspended");
    s_st.ejected = true;
    shot(&ctx, "run_ejected");
    s_failed = true;
    snprintf(s_msg, sizeof(s_msg), "（ESP_ERR_INVALID_STATE）");
    shot(&ctx, "run_failed");
    s_failed = false;
    s_st.ejected = false;
    s_st.state = READ_PICO_USB_CONNECTED;
    s_view = V_CONFIRM;
    shot(&ctx, "confirm");
    s_view = V_RESTART;
    shot(&ctx, "restart");
}
