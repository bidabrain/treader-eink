/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：番茄书架的演示截图（README / 网站预览图用），内容像真实用户的设备。
 * English: Demo shot of the Tomato shelf (README / site previews), filled like a real user's device.
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "app_tr_tomato.c"   // 按 -I 找 / Found via -I
#include "page_render.h"

const char* hr_page = "demo_tomato";

/* ---- 后台任务的替身 / Worker stand-ins ---- */

bool tomato_sync_busy(void) { return false; }
uint32_t tomato_sync_seq(void) { return 0; }

/* ---- 书架 / Shelf ---- */

typedef struct {
    const char* title;
    const char* author;
    int chapters;
    int pending;   // 待下载的新章节 / New chapters not downloaded yet
    int fresh;     // 下载了还没看的新章节 / Downloaded but unopened
    int pct;       // -1 未读 / -1 unread
} demo_book_t;

// 有新章节的排前面，和 load_shelf 的顺序一致。/ Books with new chapters first, as load_shelf orders them.
static const demo_book_t BOOKS[] = {
    {"十日终焉", "杀虫队队员", 683, 12, 0, 71},
    {"我在精神病院学斩神", "三九音域", 1452, 3, 0, 58},
    {"宿命之环", "爱潜水的乌贼", 1186, 0, 2, 93},
    {"诡秘之主", "爱潜水的乌贼", 1432, 0, 0, 64},
    {"大奉打更人", "卖报小郎君", 1065, 0, 0, 100},
    {"道诡异仙", "狐尾的笔", 1120, 0, 0, 37},
    {"夜的命名术", "会说话的肘子", 1244, 0, 0, 12},
    {"深海余烬", "远瞳", 1316, 0, 0, -1},
    {"凡人修仙传", "忘语", 2453, 0, 0, 88},
    {"赤心巡天", "情何以甚", 2391, 0, 0, 23},
    {"灵境行者", "卖报小郎君", 1052, 0, 0, 100},
    {"光阴之外", "耳根", 1378, 0, 0, 45},
    {"玄鉴仙族", "季越人", 1129, 0, 0, -1},
    {"剑来", "烽火戏诸侯", 1192, 0, 0, 6},
};
#define BOOK_N ((int)(sizeof(BOOKS) / sizeof(BOOKS[0])))

static tomato_book_t s_demo_shelf[TOMATO_SHELF_MAX];

static void demo_shelf(void) {
    memset(s_demo_shelf, 0, sizeof(s_demo_shelf));
    s_pending = 0;
    for (int i = 0; i < BOOK_N; i++) {
        tomato_book_t* b = &s_demo_shelf[i];
        const demo_book_t* d = &BOOKS[i];
        snprintf(b->book_id, sizeof(b->book_id), "7%09d", 213450 + i * 7919);
        snprintf(b->title, sizeof(b->title), "%s", d->title);
        snprintf(b->author, sizeof(b->author), "%s", d->author);
        b->chapters = (uint16_t)d->chapters;
        b->remote = (uint16_t)(d->chapters + d->pending);
        b->seen = (uint16_t)(d->chapters - d->fresh);
        snprintf(b->file, sizeof(b->file), "%s.txt", d->title);
        s_order[i] = (uint8_t)i;
        s_pct[i] = (int8_t)d->pct;
        s_pending += d->pending > 0;
    }
    s_shelf = s_demo_shelf;
    s_count = BOOK_N;
    s_has_config = true;
    s_page = 0;
}

void hr_scenes(void) {
    app_ctx_t ctx = {.fb = hr_fb};
    s_view = V_LIST;
    s_op = s_last = OP_NONE;
    memset(&s_status, 0, sizeof(s_status));
    s_note[0] = 0;
    demo_shelf();
    hr_allow_fit();
    render(&ctx, hr_fb);
    hr_shot("shelf");
}
