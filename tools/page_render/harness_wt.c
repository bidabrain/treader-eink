/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：发现漫画（WEBTOON）的渲染测试。/ English: Render bench for the comic store (WEBTOON).
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "app_tr_wt.c"   // 按 -I 找：当前源码或 SRC_ROOT / Found via -I: current sources or SRC_ROOT
#include "page_render.h"

const char* hr_page = "wt";

/* ---- 替身 / Stand-ins ---- */

static bool s_cfg_ok = true, s_updating, s_sub;
esp_err_t tomato_wt_config_load(tomato_wt_config_t* out) { (void)out; return s_cfg_ok ? ESP_OK : ESP_FAIL; }
bool tomato_sync_busy(void) { return false; }
bool tomato_sync_preemptible(void) { return false; }
uint32_t tomato_sync_seq(void) { return 1; }
void tomato_sync_cancel(void) {}
esp_err_t tomato_sync_start_wt(const tomato_wt_req_t* r) { (void)r; return ESP_OK; }
void tomato_sync_get_status(tomato_status_t* out) {
    memset(out, 0, sizeof(*out));
    if (s_updating) { out->current = 3; out->total = 10; }
}
bool tomato_wt_updating(void) { return s_updating; }
void tomato_json_use_psram(void) {}
void tomato_wt_cover_path(const char* id, char* out, size_t cap) { snprintf(out, cap, "/nonexistent/%s.jpg", id); }
void tomato_wt_title_path(const char* id, char* out, size_t cap) { snprintf(out, cap, "/nonexistent/%s.json", id); }
bool tomato_wt_find(const char* id, char* folder, size_t cap) { (void)id; snprintf(folder, cap, "/x"); return s_sub; }
bool tomato_wt_mark_read(const char* folder, tomato_wt_mark_t* out) {
    (void)folder;
    memset(out, 0, sizeof(*out));
    out->last = 42;
    return s_sub;
}
bool book_cover_tick(void) { return false; }
void book_cover_drop_pending(void) {}
// 键盘画成浅灰块（键盘自己有测试）。/ The keyboard is a light gray block (it has its own bench).
void tr_kbd_draw(uint8_t* fb, const tr_kbd_t* kbd) {
    (void)kbd;
    epd_fill_rect((EpdRect){0, TR_KBD_TOP, TR_W, TR_H - TR_KBD_TOP}, 0xE0, fb);
}

/* ---- 场景 / Scenes ---- */

static void items(int n) {
    static const char* const T[] = {"我的虎系青梅竹马", "鲁蛇的登塔攻略", "一个名字非常非常非常长的漫画作品", "再婚皇后",
                                    "全知读者视角", "女神降临", "外貌至上主义", "恶役千金也要战斗"};
    static const char* const G[] = {"大人系", "奇幻冒险", "爱情", "欧式宫廷"};
    for (int i = 0; i < n; i++) {
        item_t* x = &s_items[i];
        memset(x, 0, sizeof(*x));
        snprintf(x->id, sizeof(x->id), "%d", 8000 + i);
        snprintf(x->title, sizeof(x->title), "%s", T[i % 8]);
        snprintf(x->sub, sizeof(x->sub), "%s", G[i % 4]);
        x->canvas = i == 5;
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
    hr_allow_fit();
    hr_no_bar_check();
    render(ctx, hr_fb);
    hr_shot(name);
}

void hr_scenes(void) {
    app_ctx_t ctx = {.fb = hr_fb};
    s_cfg_ok = false;
    on_enter(&ctx);
    shot(&ctx, "nocfg");
    s_cfg_ok = true;
    on_enter(&ctx);
    s_has_pending = false;
    shot(&ctx, "empty");

    static const char* const RANKS[] = {"趋势", "人气", "原创"};
    keys(RANKS, 3);
    items(20);
    s_seg = SEG_RANK;
    shot(&ctx, "rank");
    s_page = 1;
    shot(&ctx, "rank_p2");
    s_page = 0;

    static const char* const GENRES[] = {"爱情", "奇幻冒险", "校园", "影视化", "翻页漫画", "欧式宫廷", "台湾原创作品",
                                         "武侠", "动作", "剧情", "大人系", "喜剧", "惊悚/恐怖", "穿越/转生", "现代/职场",
                                         "悬疑推理", "疗愈/萌系", "少年", "古代宫廷", "小说"};
    keys(GENRES, 20);
    s_seg = SEG_GENRE;
    shot(&ctx, "genre");
    s_chip_page = 1;
    shot(&ctx, "genre_more");
    s_chip_page = 0;

    s_searching = true;
    snprintf(s_query, sizeof(s_query), "青梅竹马");
    s_key_count = 0;
    items(4);
    shot(&ctx, "search");
    s_view = V_SEARCH;
    memset(&s_kbd, 0, sizeof(s_kbd));
    snprintf(s_kbd.text, sizeof(s_kbd.text), "青梅竹马");
    shot(&ctx, "search_kbd");
    s_view = V_LIST;
    s_searching = false;
    s_item_count = -1;
    snprintf(s_msg, sizeof(s_msg), "连不上 webtoon 服务器（ESP_ERR_HTTP_CONNECT）");
    shot(&ctx, "error");
    s_msg[0] = 0;

    // 详情：没订阅、已订阅且在下载。/ Detail: not subscribed, and subscribed while downloading.
    s_view = V_DETAIL;
    snprintf(s_id, sizeof(s_id), "8420");
    snprintf(s_title, sizeof(s_title), "我的虎系青梅竹马");
    snprintf(s_author, sizeof(s_author), "GIMAN、ETER");
    snprintf(s_genre, sizeof(s_genre), "大人系");
    snprintf(s_summary, 2048, "【建议18岁以上阅读】\n在求职路上不顺的咸雅凛，某天面试失败后喝得烂醉如泥。一觉醒来，居然发现自己和"
             "青梅竹马白范荣躺在同一张床上！这个唯一能自在交谈的男生，居然对她说……“你要对我负责。”他的眼神像老虎般锐利，"
             "语气中满是警告。但问题是，咸雅凛完全不记得那晚发生了什么事……");
    s_ep_count = 66;
    s_latest = 66;
    s_done = false;
    s_detail_loaded = true;
    s_subscribed = false;
    shot(&ctx, "detail");
    s_subscribed = true;
    s_mark.last = 42;
    s_updating = true;
    s_confirm_unsub = true;
    shot(&ctx, "detail_subscribed");
    s_updating = false;
    s_confirm_unsub = false;
    s_detail_loaded = false;
    s_job_active = false;
    shot(&ctx, "detail_loading");
    s_view = V_LIST;
}
