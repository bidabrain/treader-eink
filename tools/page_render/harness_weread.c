/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：书城里微信读书页（说明、未登录、书架网格、同步中、扫码、详情和下载进度）的渲染测试。
 * English: Render bench for the store's WeRead page (notice, signed out, shelf grid, syncing, QR, detail, download).
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "app_tr_weread.c"   // 按 -I 找：当前源码或 SRC_ROOT / Found via -I: current sources or SRC_ROOT
#include "page_render.h"
#include "nvs.h"

const char* hr_page = "weread";

/* ---- 组件和后台任务的替身 / Component and worker stand-ins ---- */

// 设置写 NVS 时直接失败，开关只留在内存里。/ Settings writes fail at NVS, so switches stay in memory only.
esp_err_t nvs_open(const char* ns, nvs_open_mode_t m, nvs_handle_t* h) { (void)ns; (void)m; (void)h; return ESP_FAIL; }

static bool s_hr_busy, s_hr_logged, s_hr_accepted;
static int s_hr_count;
bool tomato_sync_busy(void) { return s_hr_busy; }
uint32_t tomato_sync_seq(void) { return 0; }
void book_cover_drop_pending(void) {}
bool book_cover_tick(void) { return false; }
bool weread_disclaimer_accepted(void) { return s_hr_accepted; }
bool weread_logged_in(void) { return s_hr_logged; }
int weread_shelf_count(void) { return s_hr_count; }
bool weread_shelf_get(int index, weread_book_t* out) {
    static const char* const T[] = {"三体", "明朝那些事儿：一个特别特别长的书名用来测试截断", "活着", "人类简史",
                                    "置身事内", "长安的荔枝", "额尔古纳河右岸", "我与地坛", "百年孤独"};
    if (index < 0 || index >= s_hr_count) return false;
    memset(out, 0, sizeof(*out));
    snprintf(out->id, sizeof(out->id), "%d", 695233 + index);
    snprintf(out->title, sizeof(out->title), "%s", T[index % 9]);
    snprintf(out->author, sizeof(out->author), "%s", index == 1 ? "名字特别长的作者用来测试截断" : "刘慈欣");
    if (index % 3 == 0) snprintf(out->local, sizeof(out->local), "/sdcard/books/%s-%s.epub", out->title, out->id);
    return true;
}
// 二维码画成一块棋盘格，只看位置和大小。/ The QR becomes a checkerboard; only its place and size matter.
void ui_wifi_qr_draw(uint8_t* fb, EpdRect area) {
    epd_fill_rect(area, 0xff, fb);
    const int cell = area.width / 29;
    for (int y = 4; y < 25; y++)
        for (int x = 4; x < 25; x++)
            if ((x * 7 + y * 3) % 5 < 2)
                epd_fill_rect((EpdRect){area.x + x * cell, area.y + y * cell, cell, cell}, 0, fb);
}

static void hr_reset(void) {
    s_view = V_SHELF;
    s_job = s_last = J_NONE;
    s_hr_busy = false;
    s_hr_logged = true;
    s_hr_accepted = true;
    s_hr_count = 0;
    s_page = 0;
    s_note[0] = 0;
    s_dl_id[0] = 0;
    s_tomato_on = false;
    memset(&s_status, 0, sizeof(s_status));
    memset(&s_snap, 0, sizeof(s_snap));
    app_settings_set_weread(true);
    static weread_book_t books[9];
    s_books = books;
}

static app_ctx_t s_ctx;

static void hr_render(const char* scene) {
    s_ctx.fb = hr_fb;
    load_page();
    render(&s_ctx, hr_fb);
    hr_shot(scene);
}

void hr_scenes(void) {
    hr_reset();
    s_hr_accepted = false;
    s_view = V_NOTICE;
    hr_render("notice");

    hr_reset();
    s_hr_logged = false;
    hr_render("signed_out");

    hr_reset();
    hr_render("shelf_empty");

    hr_reset();
    s_hr_count = 20;
    hr_allow_fit();
    hr_render("shelf");

    hr_reset();
    s_hr_count = 20;
    s_tomato_on = true;
    hr_allow_fit();
    hr_render("shelf_both_sources");

    hr_reset();
    s_hr_count = 20;
    s_hr_busy = true;
    s_job = J_SYNC;
    s_snap.stage = WEREAD_STAGE_COVERS;
    s_snap.done = 7;
    s_snap.total = 20;
    hr_allow_fit();
    hr_render("shelf_syncing");

    hr_reset();
    s_hr_count = 20;
    s_last = J_SYNC;
    s_status.phase = TOMATO_FAILED;
    snprintf(s_status.message, sizeof(s_status.message), "%s", "登录已过期，请重新扫码");
    hr_allow_fit();
    hr_render("shelf_failed");

    hr_reset();
    s_hr_busy = true;
    s_job = J_SYNC;
    s_view = V_QR;
    s_snap.stage = WEREAD_STAGE_QR;
    hr_render("qr");

    hr_reset();
    s_hr_count = 20;
    weread_shelf_get(1, &s_detail);
    s_view = V_DETAIL;
    hr_allow_fit();
    hr_render("detail");

    hr_reset();
    s_hr_count = 20;
    weread_shelf_get(0, &s_detail);
    s_view = V_DETAIL;
    hr_render("detail_downloaded");

    hr_reset();
    s_hr_count = 20;
    weread_shelf_get(1, &s_detail);
    s_view = V_DETAIL;
    s_hr_busy = true;
    s_job = J_DOWNLOAD;
    snprintf(s_dl_id, sizeof(s_dl_id), "%s", s_detail.id);
    s_snap.stage = WEREAD_STAGE_CHAPTERS;
    s_snap.done = 12;
    s_snap.total = 80;
    hr_allow_fit();
    hr_render("detail_downloading");
}
