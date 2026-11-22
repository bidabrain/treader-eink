/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：发现漫画（WEBTOON）的演示截图（README / 网站预览图和设计文档用）：排行、类型、作品详情、搜索。
 * English: Demo shots of the comic store (WEBTOON) for README / site previews and the design doc: rankings, genres,
 * title detail and search.
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include <sys/stat.h>

#include "app_tr_wt.c"   // 按 -I 找 / Found via -I
#include "page_render.h"

const char* hr_page = "demo_wt";

/* ---- 替身 / Stand-ins ---- */

static bool s_sub, s_updating;
esp_err_t tomato_wt_config_load(tomato_wt_config_t* out) { (void)out; return ESP_OK; }
bool tomato_sync_busy(void) { return false; }
bool tomato_sync_preemptible(void) { return false; }
uint32_t tomato_sync_seq(void) { return 1; }
void tomato_sync_cancel(void) {}
esp_err_t tomato_sync_start_wt(const tomato_wt_req_t* r) { (void)r; return ESP_OK; }
void tomato_sync_get_status(tomato_status_t* out) {
    memset(out, 0, sizeof(*out));
    if (s_updating) { out->current = 4; out->total = 10; }
}
bool tomato_wt_updating(void) { return s_updating; }
#ifndef HR_BASELINE   // 改动前的代码没有来源 / The pre-change code has no sources
// 来源：画面里直接改 s_src，这里只给默认值。/ Sources: scenes set s_src directly; these only give the default.
void tomato_wt_src(char* out, size_t cap) { snprintf(out, cap, "%s", TOMATO_WT_SRC_WEBTOON); }
esp_err_t tomato_wt_src_set(const char* key) { (void)key; return ESP_OK; }
bool tomato_wt_is_ext(const char* id) { return id && id[0] == 'm'; }
#endif
void tomato_json_use_psram(void) {}
bool book_cover_tick(void) { return false; }
void book_cover_drop_pending(void) {}
void tr_kbd_draw(uint8_t* fb, const tr_kbd_t* kbd) {
    (void)kbd;
    epd_fill_rect((EpdRect){0, TR_KBD_TOP, TR_W, TR_H - TR_KBD_TOP}, 0xE0, fb);
}
bool tomato_wt_find(const char* id, char* folder, size_t cap) { (void)id; snprintf(folder, cap, "/x"); return s_sub; }
bool tomato_wt_mark_read(const char* folder, tomato_wt_mark_t* out) {
    (void)folder;
    memset(out, 0, sizeof(*out));
    out->last = 66;
    return s_sub;
}
void tomato_wt_title_path(const char* id, char* out, size_t cap) { snprintf(out, cap, "/nonexistent/%s.json", id); }

// 封面放在临时目录里的小文件，页面 stat 得到就当有图片封面（演示模式画渐变）。/ Covers are small files in a temp
// dir; when the page can stat one it counts as an image cover (a gradient in demo mode).
#define DEMO_COVER_DIR "/tmp/hr_demo_wt"
void tomato_wt_cover_path(const char* id, char* out, size_t cap) { snprintf(out, cap, DEMO_COVER_DIR "/%s.jpg", id); }

static void demo_cover(const char* id) {
    char path[96];
    tomato_wt_cover_path(id, path, sizeof(path));
    FILE* f = fopen(path, "wb");
    if (f) { fputs("jpg", f); fclose(f); }
}

/* ---- 数据 / Data ---- */

typedef struct { const char* title; const char* genre; } demo_t;
static const demo_t TITLES[] = {
    {"海边的少年", "治愈/萌系"}, {"鲁蛇的登塔攻略", "奇幻冒险"}, {"月光下的剑客", "武侠"}, {"再婚皇后", "欧式宫廷"},
    {"图书馆里的猫", "疗愈/萌系"}, {"末日便利店", "惊悚/恐怖"}, {"星辰学院", "校园"}, {"雨季来临前", "爱情"},
    {"深夜食堂的老板", "剧情"}, {"第七个房间", "悬疑推理"}, {"龙与咖啡师", "奇幻冒险"}, {"山间来信", "剧情"},
};

static void fill(int n) {
    for (int i = 0; i < n; i++) {
        item_t* x = &s_items[i];
        memset(x, 0, sizeof(*x));
        snprintf(x->id, sizeof(x->id), "%d", 9100 + i);
        snprintf(x->title, sizeof(x->title), "%s", TITLES[i % 12].title);
        snprintf(x->sub, sizeof(x->sub), "%s", TITLES[i % 12].genre);
        demo_cover(x->id);
    }
    s_item_count = n;
}

static void keys(const char* const* names, int n) {
    for (int i = 0; i < n; i++) {
        snprintf(s_keys[i].key, sizeof(s_keys[i].key), "k%d", i);
        snprintf(s_keys[i].name, sizeof(s_keys[i].name), "%s", names[i]);
    }
    s_key_count = n;
    snprintf(s_key, sizeof(s_key), "k0");
}

static void shot(app_ctx_t* ctx, const char* name) {
    render(ctx, hr_fb);
    hr_shot(name);
}

void hr_scenes(void) {
    mkdir(DEMO_COVER_DIR, 0775);
    app_ctx_t ctx = {.fb = hr_fb};
    on_enter(&ctx);
    s_has_pending = false;
    s_job_active = false;

    static const char* const RANKS[] = {"趋势", "人气", "原创"};
    keys(RANKS, 3);
    fill(24);
    s_seg = SEG_RANK;
    shot(&ctx, "rank");

    static const char* const GENRES[] = {"爱情", "奇幻冒险", "校园", "影视化", "欧式宫廷", "武侠", "动作", "剧情",
                                         "大人系", "喜剧", "惊悚/恐怖", "穿越/转生", "现代/职场", "悬疑推理"};
    keys(GENRES, 14);
    s_seg = SEG_GENRE;
    s_page = 1;
    shot(&ctx, "genre");
    s_page = 0;

    s_view = V_SEARCH;
    memset(&s_kbd, 0, sizeof(s_kbd));
    snprintf(s_kbd.text, sizeof(s_kbd.text), "海边");
    shot(&ctx, "search");

    s_view = V_DETAIL;
    snprintf(s_id, sizeof(s_id), "9100");
    snprintf(s_title, sizeof(s_title), "海边的少年");
    snprintf(s_author, sizeof(s_author), "林小满");
    snprintf(s_genre, sizeof(s_genre), "治愈/萌系");
    snprintf(s_summary, 2048, "夏天结束前，少年阿澄回到外婆住的海边小镇。灯塔、旧书店、每天傍晚准时出现的渡船，"
             "还有一个总在码头画画的女孩。一个关于离开与留下的故事，每周六更新。");
    s_ep_count = 66;
    s_latest = 66;
    s_done = false;
    s_detail_loaded = true;
    s_subscribed = false;
    shot(&ctx, "detail");
    s_sub = true;
    s_subscribed = true;
    s_mark.last = 66;
    s_updating = true;
    shot(&ctx, "detail_subscribed");
    s_updating = false;
    s_view = V_LIST;
}
