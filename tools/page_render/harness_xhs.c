/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：小红书标签页的渲染测试。/ English: Render bench for the Xiaohongshu tab.
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "app_tr_xhs.c"   // 按 -I 找：当前源码或 SRC_ROOT / Found via -I: current sources or SRC_ROOT
#include "page_render.h"

const char* hr_page = "xhs";

/* ---- 替身 / Stand-ins ---- */

static bool s_cfg_ok = true;
esp_err_t tomato_red_config_load(tomato_red_config_t* out) { (void)out; return s_cfg_ok ? ESP_OK : ESP_FAIL; }
bool tomato_sync_busy(void) { return false; }
bool tomato_sync_preemptible(void) { return false; }
uint32_t tomato_sync_seq(void) { return 1; }
void tomato_sync_cancel(void) {}
void tomato_sync_get_status(tomato_status_t* out) { memset(out, 0, sizeof(*out)); }
void tomato_json_use_psram(void) {}
void tomato_red_note_dir(const char* id, char* out, size_t cap) { snprintf(out, cap, "/nonexistent/red/%s", id); }
void tomato_red_img_path(const char* id, int n, char* out, size_t cap) { snprintf(out, cap, "/nonexistent/%s_%d.jpg", id, n); }
void tomato_red_cover_path(const char* id, char* out, size_t cap) { snprintf(out, cap, "/nonexistent/%s_c.jpg", id); }
const uint8_t* hupu_img_get(const char* jpg, int w, int h) { (void)jpg; (void)w; (void)h; return NULL; }
bool hupu_img_failed(const char* jpg, int w, int h) { (void)jpg; (void)w; (void)h; return false; }
void hupu_img_drop_pending(void) {}
void hupu_img_release(void) {}
bool hupu_img_pending(void) { return false; }
bool hupu_img_tick(void) { return false; }

/* ---- 场景 / Scenes ---- */

static const char* LONG = "今天去看了一场很长的展览，从早上一直看到下午，里面有很多以前没见过的东西，"
                          "拍了好多照片，晚点慢慢整理出来发给大家。顺便说一句，展馆的咖啡也很好喝，推荐。"
                          "下次还想再去一次，带上朋友一起。";

static void item(int i, const char* title, const char* author, const char* likes, bool seen) {
    item_t* x = &s_items[i];
    memset(x, 0, sizeof(*x));
    snprintf(x->id, sizeof(x->id), "id%d", i);
    snprintf(x->title, sizeof(x->title), "%s", title);
    snprintf(x->author, sizeof(x->author), "%s", author);
    snprintf(x->likes, sizeof(x->likes), "%s", likes);
    x->images = 3;
    x->seen = seen;
}

static void shot(app_ctx_t* ctx, const char* name, bool fit) {
    if (fit) hr_allow_fit();
    if (s_view != V_LIST) hr_no_bar_check();
    render(ctx, hr_fb);
    hr_shot(name);
}

static comment_t s_cm[COMMENTS_MAX];

static void note(const char* title, const char* body, int images) {
    s_note_loaded = true;
    s_view = V_TEXT;
    s_tab = T_BODY;
    s_page = 0;
    snprintf(s_id, sizeof(s_id), "n1");
    s_title = title;
    s_author = "一个名字非常非常非常非常非常长的小红书博主";
    s_likes = "1.2万";
    s_comment_total = "3456";
    s_body = body;
    s_time = 1767000000;
    s_images = images;
    s_comments_more = true;
    s_title_lines = title_lines();
    layout_body();
    layout_comments();
}

void hr_scenes(void) {
    app_ctx_t ctx = {.fb = hr_fb};
    s_cfg_ok = false;
    on_enter(&ctx);
    shot(&ctx, "nocfg", false);
    s_cfg_ok = true;
    on_enter(&ctx);
    shot(&ctx, "empty_follow", false);
    s_seg = SEG_REC;
    shot(&ctx, "empty_rec", false);
    s_rec_time = 1767000000;
    s_rec_left = 3;
    s_has_pending = true;
    s_pending = (tomato_red_req_t){.job = TOMATO_RED_REC, .refresh = true};
    s_job.refresh = true;
    shot(&ctx, "loading_rec", false);
    s_has_pending = false;
    s_seg = SEG_FOLLOW;
    snprintf(s_msg, sizeof(s_msg), "连不上服务器：请检查服务器地址和设备令牌，再点刷新重试一次");
    shot(&ctx, "error", false);
    s_msg[0] = 0;
    for (int i = 0; i < 7; i++)
        item(i, i % 2 ? "短标题" : "一个特别特别长的笔记标题，需要换两行显示还放不下的那种标题文字",
             i % 3 ? "博主" : "一个名字非常非常非常长的博主", i % 2 ? "12" : "1.2万", i % 3 != 0);
    s_item_count = 7;
    shot(&ctx, "list_p1", true);
    s_list_page = list_pages() - 1;
    shot(&ctx, "list_last", true);
    s_list_page = 0;
    s_seg = SEG_REC;
    shot(&ctx, "list_rec", true);
    s_wait = true;
    s_has_pending = true;
    s_pending = (tomato_red_req_t){.job = TOMATO_RED_NOTE};
    shot(&ctx, "wait", true);
    s_has_pending = false;
    snprintf(s_msg, sizeof(s_msg), "服务器没能取到这篇笔记，可能已被删除或设为仅自己可见");
    shot(&ctx, "wait_failed", true);
    s_wait = false;
    s_msg[0] = 0;
    s_seg = SEG_FOLLOW;

    static char body[4096];
    snprintf(body, sizeof(body), "%s\n%s\n%s\n%s", LONG, LONG, LONG, LONG);
    note("一个特别特别长的笔记标题，需要换两行显示还放不下的那种标题文字", body, 5);
    shot(&ctx, "note_p1", true);
    s_page = s_page_count - 1;
    shot(&ctx, "note_last", true);
    note("短标题", "", 0);
    shot(&ctx, "note_empty", true);
    note("短标题", "短短一句。", 2);
    s_tab = T_COMMENTS;
    s_comment_count = 0;
    layout_comments();
    shot(&ctx, "comments_none", true);
    for (int i = 0; i < 9; i++) {
        s_cm[i] = (comment_t){.user = i % 2 ? "评论的人" : "一个名字非常非常非常非常非常长的评论用户", .text = i % 3 ? LONG : "好",
                              .likes = i % 2 ? "1.2万" : "0"};
        s_comments[i] = s_cm[i];
    }
    s_comment_count = 9;
    layout_comments();
    shot(&ctx, "comments", true);
    s_page = s_comment_page_count - 1;
    shot(&ctx, "comments_last", true);
    s_comments_more = false;
    shot(&ctx, "comments_last_nomore", true);
    s_comments_more = true;
    s_more_wait = true;
    shot(&ctx, "more_wait", true);
    s_more_failed = true;
    snprintf(s_msg, sizeof(s_msg), "服务器打开小红书超时，稍后再试");
    shot(&ctx, "more_failed", true);
    s_more_wait = s_more_failed = false;
    s_msg[0] = 0;

    s_view = V_IMAGE;
    s_image = 1;
    s_images = 5;
    shot(&ctx, "image", false);
    s_ws.shown = true;
    shot(&ctx, "image_toolbar", false);
    s_ws.shown = false;

    s_view = V_LIST;
    free_all();
    shot(&ctx, "nomem", false);
}
