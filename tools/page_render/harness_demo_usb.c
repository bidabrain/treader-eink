/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：USB 传书页的演示图：已连电脑、正在拷书。/ English: Demo shot of the USB transfer page: connected, copying books.
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "app_usb.c"   // 按 -I 找：当前源码 / Found via -I: current sources
#include "page_render.h"

const char* hr_page = "demo_usb";

esp_err_t read_pico_sd_get_info(read_pico_sd_info_t* info) {
    memset(info, 0, sizeof(*info));
    info->present = info->mounted = true;
    info->free_bytes = 21ull * 1024 * 1024 * 1024 + 612000000ull;
    return ESP_OK;
}

void hr_scenes(void) {
    app_ctx_t ctx = {.fb = hr_fb};
    on_enter(&ctx);
    s_view = V_RUN;
    s_failed = false;
    s_msg[0] = 0;
    memset(&s_st, 0, sizeof(s_st));
    s_st.state = READ_PICO_USB_CONNECTED;
    s_st.write_bytes = 286ull * 1048576 + 420000ull;
    s_st.read_bytes = 14ull * 1048576 + 700000ull;
    // 刚有读写：状态行显示“正在读写…”。/ Fresh I/O: the state row shows "reading/writing".
    s_st.io_ms = esp_timer_get_time() / 1000;
    s_power = true;
    hr_no_bar_check();
    render(&ctx, hr_fb);
    hr_shot("running");

    // 设计文档 ui.html 的 USB 传书图。/ USB transfer figures for the ui.html design doc.
    on_enter(&ctx);
    hr_no_bar_check();
    render(&ctx, hr_fb);
    hr_shot("ui18");

    s_view = V_RUN;
    s_failed = false;
    s_msg[0] = 0;
    memset(&s_st, 0, sizeof(s_st));
    s_st.state = READ_PICO_USB_CONNECTED;
    s_st.write_bytes = 286ull * 1048576 + 420000ull;
    s_st.read_bytes = 12ull * 1048576 + 300000ull;
    s_st.io_ms = esp_timer_get_time() / 1000;
    s_power = true;
    hr_no_bar_check();
    render(&ctx, hr_fb);
    hr_shot("ui19");

    // 没弹出就点结束：读写已停，确认框盖在传输页上。/ End before ejecting: I/O idle, the dialog over the run view.
    s_st.write_bytes = 1240ull * 1048576 + 512000ull;
    s_st.read_bytes = 18ull * 1048576 + 600000ull;
    s_st.io_ms = 0;
    s_view = V_CONFIRM;
    hr_no_bar_check();
    render(&ctx, hr_fb);
    hr_shot("ui20");

    s_view = V_RESTART;
    hr_no_bar_check();
    render(&ctx, hr_fb);
    hr_shot("ui21");
}
