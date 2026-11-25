/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：微博标签页的渲染测试。/ English: Render bench for the Weibo tab.
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "app_tr_weibo.c"   // 按 -I 找：当前源码或 SRC_ROOT / Found via -I: current sources or SRC_ROOT
#include "page_render.h"

const char* hr_page = "weibo";

/* ---- 替身 / Stand-ins ---- */

static bool s_more_stub;   // “往后翻接着拉”开关 / The keep-pulling switch
bool tomato_feed_more_on(void) { return s_more_stub; }
esp_err_t tomato_feed_more_set(bool on) { s_more_stub = on; return ESP_OK; }

static bool s_cfg_ok = true;
esp_err_t tomato_wb_config_load(tomato_wb_config_t* out) { (void)out; return s_cfg_ok ? ESP_OK : ESP_FAIL; }
bool tomato_sync_busy(void) { return false; }
bool tomato_sync_preemptible(void) { return false; }
uint32_t tomato_sync_seq(void) { return 1; }
void tomato_sync_cancel(void) {}
void tomato_sync_get_status(tomato_status_t* out) { memset(out, 0, sizeof(*out)); }
void tomato_json_use_psram(void) {}
void tomato_wb_post_dir(const char* id, char* out, size_t cap) { snprintf(out, cap, "/nonexistent/wb/%s", id); }
void tomato_wb_img_path(const char* id, int n, char* out, size_t cap) { snprintf(out, cap, "/nonexistent/%s_%d.jpg", id, n); }
void tomato_wb_square_path(const char* id, int k, char* out, size_t cap) { snprintf(out, cap, "/nonexistent/%s_s%d.jpg", id, k); }
void tomato_wb_comments_path(const char* id, char* out, size_t cap) { snprintf(out, cap, "/nonexistent/%s_c.json", id); }
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

static void item(int i, const char* user, const char* text, const char* rt_user, const char* rt_text, int images,
                 int pics, float ratio, bool seen, int n) {
    item_t* x = &s_items[i];
    memset(x, 0, sizeof(*x));
    snprintf(x->id, sizeof(x->id), "id%d", i);
    snprintf(x->user, sizeof(x->user), "%s", user);
    snprintf(x->text, sizeof(x->text), "%s", text);
    snprintf(x->rt_user, sizeof(x->rt_user), "%s", rt_user);
    snprintf(x->rt_text, sizeof(x->rt_text), "%s", rt_text);
    snprintf(x->source, sizeof(x->source), "iPhone 16 Pro Max 客户端");
    x->time = 1767000000;
    x->images = (int16_t)images;
    x->pics = (int16_t)pics;
    x->ratio = ratio;
    x->seen = seen;
    x->reposts = n;
    x->comments = n * 3;
    x->likes = n * 17;
}

static void fill_list(void) {
    item(0, "一个名字非常非常非常非常非常长的微博用户名字", LONG, "", "", 0, 0, 0, false, 123456);
    item(1, "短名", "带一张图。", "", "", 1, 1, 1.4f, true, 12);
    item(2, "转发的人", "转发一下。", "原作者", LONG, 0, 0, 0, true, 9999);
    item(3, "九宫格", "好多图", "", "", 9, 9, 1, false, 99999999);
    item(4, "第五条", "第五条的文字", "", "", 0, 0, 0, true, 0);
    item(5, "第六条", LONG, "引用", "短的原文", 2, 2, 1, true, 5);
    s_item_count = 6;
    layout_list();
}

static void shot(app_ctx_t* ctx, const char* name, bool fit) {
    if (fit) hr_allow_fit();
    if (s_view != V_LIST) hr_no_bar_check();
    render(ctx, hr_fb);
    hr_shot(name);
}

static comment_t s_cm[COMMENTS_MAX];

static void post(const char* body, const char* rt, int images, int pics) {
    s_post_loaded = true;
    s_view = V_TEXT;
    s_tab = T_BODY;
    s_page = 0;
    snprintf(s_id, sizeof(s_id), "p1");
    s_user = "一个名字非常非常非常非常非常长的微博用户名字";
    s_source = "微博 weibo.com";
    s_body = body;
    s_rt = rt;
    s_time = 1767000000;
    s_images = images;
    s_pics = pics;
    s_likes = 12345;
    s_comment_total = 123456;
    s_reposts = 77;
    layout_body();
}

void hr_scenes(void) {
    app_ctx_t ctx = {.fb = hr_fb};
    s_cfg_ok = false;
    on_enter(&ctx);
    shot(&ctx, "nocfg", false);
    s_cfg_ok = true;
    on_enter(&ctx);
    shot(&ctx, "empty_follow", false);
    s_seg = SEG_HOT;
    shot(&ctx, "empty_hot", false);
    s_seg = SEG_FOLLOW;
    s_has_pending = true;
    s_pending = (tomato_wb_req_t){.job = TOMATO_WB_FEED};
    shot(&ctx, "loading", false);
    s_has_pending = false;
    snprintf(s_msg, sizeof(s_msg), "连不上服务器：请检查服务器地址和设备令牌，再点刷新重试一次");
    shot(&ctx, "error", false);
    s_msg[0] = 0;
    s_list_time = 1767000000;
    fill_list();
    shot(&ctx, "list_p1", true);
    s_list_page = 1;
    shot(&ctx, "list_p2", true);
    s_list_page = list_pages() - 1;
    shot(&ctx, "list_last", true);
    s_list_page = 0;
    s_wait = true;
    s_has_pending = true;
    s_pending = (tomato_wb_req_t){.job = TOMATO_WB_POST};
    shot(&ctx, "wait", true);
    s_has_pending = false;
    snprintf(s_msg, sizeof(s_msg), "服务器没能取到这条微博，可能已被删除或设为仅自己可见");
    shot(&ctx, "wait_failed", true);
    s_wait = false;
    s_msg[0] = 0;

    static char body[4096];
    snprintf(body, sizeof(body), "%s\n%s\n%s", LONG, LONG, LONG);
    post(body, "@原作者：这是被转发的原微博，内容也比较长，需要换行显示，看看灰框画得对不对。", 6, 6);
    shot(&ctx, "post_p1", true);
    s_page = s_page_count - 1;
    shot(&ctx, "post_last", true);
    post("短短一句。", "", 1, 0);
    shot(&ctx, "post_short", true);
    post("", "", 0, 0);
    shot(&ctx, "post_empty", true);
    post("短短一句。", "", 3, 3);
    s_tab = T_COMMENTS;
    s_comments_loaded = false;
    s_has_pending = true;
    s_pending = (tomato_wb_req_t){.job = TOMATO_WB_COMMENTS};
    shot(&ctx, "comments_loading", true);
    s_has_pending = false;
    shot(&ctx, "comments_missing", true);
    s_comments_loaded = true;
    s_comment_count = 0;
    layout_comments();
    shot(&ctx, "comments_none", true);
    for (int i = 0; i < 8; i++) {
        s_cm[i] = (comment_t){.user = i % 2 ? "评论的人" : "一个名字非常非常非常非常非常长的评论用户", .text = i % 3 ? LONG : "好",
                              .likes = i * 12345};
        s_comments[i] = s_cm[i];
    }
    s_comment_count = 8;
    layout_comments();
    shot(&ctx, "comments", true);
    s_page = s_comment_page_count - 1;
    shot(&ctx, "comments_last", true);

    s_view = V_IMAGE;
    s_image = 2;
    s_images = 6;
    shot(&ctx, "image", false);
    s_ws.shown = true;
    shot(&ctx, "image_toolbar", false);
    s_ws.shown = false;

    s_view = V_LIST;
    free_all();
    shot(&ctx, "nomem", false);
}
