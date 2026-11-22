/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：漫画页的渲染测试。/ English: Render bench for the comics page.
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "app_comic.c"   // 按 -I 找：当前源码或 SRC_ROOT / Found via -I: current sources or SRC_ROOT
#include "page_render.h"

const char* hr_page = "comic";

// 图片替身：按给定大小画一块带边的灰图。/ Image stand-in: a bordered gray block at the given size.
void hupu_img_draw(uint8_t* fb, int x, int y, int w, int h, const uint8_t* px, bool border) {
    (void)px;
    epd_fill_rect((EpdRect){x, y, w, h}, 0xA0, fb);
    epd_draw_rect((EpdRect){x, y, w, h}, 0x00, fb);
    (void)border;
}

// 进度替身：按话序号给出未看 / 看完 / 在看。/ Progress stand-in: unread / finished / reading by episode index.
bool comic_progress_get(const char* path, comic_progress_t* out) {
    const char* digits = strrchr(path, '_');
    const int n = digits ? atoi(digits + 1) : 0;
    memset(out, 0, sizeof(*out));
    if (n % 4 == 3) return false;
    out->pages = 186;
    out->page = n % 4 == 1 ? 185 : 41;
    out->done = n % 4 == 1;
    out->stamp = (uint32_t)n;
    return true;
}

#ifndef HR_BASELINE   // WEBTOON 替身（改动前的代码没有）/ WEBTOON stand-ins (absent from the pre-change code)
static bool s_hr_updating;
bool tomato_wt_updating(void) { return s_hr_updating; }
void tomato_sync_get_status(tomato_status_t* out) {
    memset(out, 0, sizeof(*out));
    if (s_hr_updating) { out->current = 2; out->total = 6; }
}
#endif

static comic_entry_t s_fake_shelf[8];
static char s_fake_vols[12][COMIC_NAME_MAX];
static uint8_t s_fake_px[684 * 1160];

static void setup(void) {
    s_shelf = s_fake_shelf;
    s_vols = s_fake_vols;
    s_count = 8;
    for (int i = 0; i < 8; i++) {
        comic_entry_t* e = &s_shelf[i];
        memset(e, 0, sizeof(*e));
        snprintf(e->name, sizeof(e->name), i == 0 ? "一部名字很长很长的漫画系列作品" : "漫画 %d", i + 1);
        snprintf(e->path, sizeof(e->path), COMIC_ROOT "/series_%d", i);
        e->series = i % 2 == 0;
        e->volumes = e->series ? 128 : 1;
        e->done = e->series ? (i == 2 ? 128 : 37) : i == 3;
        e->stamp = i == 4 ? 0 : 10 - i;
        e->pct = 63;
        // 封面都算已缓存，画替身灰块。/ Treat every cover as cached so the stand-in block is drawn.
        snprintf(s_covers[i].key, sizeof(s_covers[i].key), "%s", e->path);
        s_covers[i].px = s_covers[i].mini = s_fake_px;
    }
    s_sort_name = false;
    s_page = 0;
    s_message[0] = 0;
    snprintf(s_last, sizeof(s_last), COMIC_ROOT "/series_0/第12话_2.cbz");
    snprintf(s_last_title, sizeof(s_last_title), "一部名字很长很长的漫画系列作品 · 第12话");
    snprintf(s_last_owner, sizeof(s_last_owner), COMIC_ROOT "/series_0");
    s_last_page = 122;
    s_last_pages = 186;
    s_series = 0;
    s_vol_count = 12;
    for (int i = 0; i < 12; i++) snprintf(s_vols[i], COMIC_NAME_MAX, "第%d话 很长的标题名字在这里_%d.cbz", i + 1, i);
    s_vol_page = 0;
    s_ser_done = 37;
    s_ser_resume = 4;
    s_ser_page = 41;
    s_ser_dir = 1;
}

static void shot(const char* name, bool fit) {
    app_ctx_t ctx = {.fb = hr_fb};
    if (fit) hr_allow_fit();
    render(&ctx, hr_fb);
    hr_shot(name);
}

void hr_scenes(void) {
    setup();
    s_view = V_SHELF;
    shot("shelf", true);
    snprintf(s_message, sizeof(s_message), "打不开这个 CBZ（文件损坏或不是 ZIP）");
    s_sort_name = true;
    s_page = 1;
    shot("shelf_page2_msg", true);
    s_count = 0;
    s_message[0] = 0;
    shot("shelf_empty", false);

    setup();
    s_view = V_SERIES;
    shot("series", true);
    s_vol_page = 1;
    shot("series_page2", true);

    setup();
    s_view = V_READ;
    s_buf[0] = (pagebuf_t){.idx = 0, .w = 640, .h = 1160, .px = s_fake_px};
    s_buf[1].idx = -1;
    snprintf(s_book_title, sizeof(s_book_title), "第12话 很长的标题名字在这里");
    s_cur = 41;
    s_page_count = 186;
    s_vol_index = 4;
    s_rtl = true;
    s_toolbar = false;
    shot("reader", true);
    snprintf(s_message, sizeof(s_message), "已经是最后一页");
    shot("reader_msg", true);
    s_toolbar = true;
    s_auto_level = 4;
    snprintf(s_message, sizeof(s_message), "已经是最后一话");
    shot("toolbar_series", true);
    s_vol_index = -1;
    s_series = -1;
    s_rtl = false;
    s_auto_level = 0;
    s_message[0] = 0;
    shot("toolbar_single", true);
    s_toolbar = false;
    s_buf[0] = (pagebuf_t){.idx = 0, .failed = true};
    shot("reader_failed", true);
    s_buf[0].idx = -1;

    setup();
    s_view = V_DETAIL;
    snprintf(s_detail_name, sizeof(s_detail_name), "一部名字很长很长的漫画系列作品，名字长到要折好几行才放得下的那种");
    s_detail_series = true;
    snprintf(s_detail_info[0], sizeof(s_detail_info[0]), "系列 · 128 话");
    s_detail_info[1][0] = 0;
    s_confirm_delete = false;
    shot("detail_series", true);
    s_detail_series = false;
    snprintf(s_detail_info[0], sizeof(s_detail_info[0]), "CBZ · 123.4 MB");
    snprintf(s_detail_info[1], sizeof(s_detail_info[1]), "看到第 122 / 186 页");
    s_confirm_delete = true;
    snprintf(s_message, sizeof(s_message), "删除没有完成，检查 TF 卡");
    shot("detail_single", true);

#ifndef HR_BASELINE   // 改动前没有 WEBTOON / No WEBTOON before this change
    // 我的漫画：设置了漫画服务器，两部订阅（一部有新话、一部还没下载），在更新。/ Shelf with a comic server, two
    // subscriptions (one with new episodes, one not downloaded yet), updating.
    setup();
    s_view = V_SHELF;
    s_wt_cfg = true;
    s_wt_series = 2;
    s_shelf[0].webtoon = true;
    s_shelf[0].fresh = 3;
    s_shelf[2].webtoon = true;
    s_shelf[2].volumes = 0;
    s_shelf[2].done = 0;
    shot("shelf_wt", true);
    s_hr_updating = true;
    s_src = 2;
    snprintf(s_message, sizeof(s_message), "下载了 3 话，1 话失败：服务器转换太久了");
    shot("shelf_wt_updating", true);
    s_hr_updating = false;
    s_src = 0;
    s_message[0] = 0;

    // 订阅系列的详情。/ A subscribed series' detail.
    s_view = V_DETAIL;
    snprintf(s_detail_name, sizeof(s_detail_name), "我的虎系青梅竹马");
    s_detail_series = true;
    s_detail_wt = true;
    snprintf(s_detail_info[0], sizeof(s_detail_info[0]), "系列 · 24 话");
    snprintf(s_detail_info[1], sizeof(s_detail_info[1]), "WEBTOON 订阅 · 下载到第 66 话");
    s_confirm_delete = false;
    s_confirm_unsub = true;
    shot("detail_wt", true);
    s_hr_updating = true;
    s_confirm_unsub = false;
    shot("detail_wt_updating", true);
    s_hr_updating = false;
    s_detail_wt = false;
    s_wt_cfg = false;
    s_wt_series = 0;
#endif
}
