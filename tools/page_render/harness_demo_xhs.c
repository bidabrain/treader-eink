/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：小红书标签页的演示截图（README / 网站预览用）：页面摆成真实用户的样子。
 * English: Demo shots of the Xiaohongshu tab for the README / site previews, with the page in a realistic state.
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include <sys/stat.h>

#include "app_tr_xhs.c"   // 按 -I 找：当前源码 / Found via -I: current sources
#include "page_render.h"

const char* hr_page = "demo_xhs";

/* ---- 替身 / Stand-ins ---- */

#define DEMO_DIR "/tmp/hrdemo_xhs"

esp_err_t tomato_red_config_load(tomato_red_config_t* out) { (void)out; return ESP_OK; }
bool tomato_sync_busy(void) { return false; }
bool tomato_sync_preemptible(void) { return false; }
uint32_t tomato_sync_seq(void) { return 1; }
void tomato_sync_cancel(void) {}
void tomato_sync_get_status(tomato_status_t* out) { memset(out, 0, sizeof(*out)); }
void tomato_json_use_psram(void) {}
void tomato_red_note_dir(const char* id, char* out, size_t cap) { snprintf(out, cap, DEMO_DIR "/note_%s", id); }
void tomato_red_img_path(const char* id, int n, char* out, size_t cap) { snprintf(out, cap, DEMO_DIR "/%s_%d.jpg", id, n); }
void tomato_red_cover_path(const char* id, char* out, size_t cap) { snprintf(out, cap, DEMO_DIR "/%s_c.jpg", id); }

// 解码结果用一个字节表里的位置带花样：同一路径同一张“照片”。/ The decoded pointer carries a seed as an offset in a byte
// table, so one path always gives the same "photo".
static uint8_t s_seeds[256];
const uint8_t* hupu_img_get(const char* jpg, int w, int h) {
    (void)w; (void)h;
    unsigned seed = 2166136261u;
    for (const char* c = jpg; *c; c++) seed = (seed ^ (unsigned char)*c) * 16777619u;
    return &s_seeds[(seed ^ (seed >> 13)) % 251];
}
void hupu_img_draw(uint8_t* fb, int x, int y, int w, int h, const uint8_t* px, bool border) {
    hr_gradient(fb, (EpdRect){x, y, w, h}, (int)(px - s_seeds));
    if (border) {
        epd_draw_rect((EpdRect){x, y, w, h}, 0, fb);
        epd_draw_rect((EpdRect){x + 1, y + 1, w - 2, h - 2}, 0, fb);
    }
}
bool hupu_img_failed(const char* jpg, int w, int h) { (void)jpg; (void)w; (void)h; return false; }
void hupu_img_drop_pending(void) {}
void hupu_img_release(void) {}
bool hupu_img_pending(void) { return false; }
bool hupu_img_tick(void) { return false; }

// 写一个只有文件头的 JPEG（页面只读宽高）。/ Write a header-only JPEG (the page only reads its size).
static void mkjpg(const char* path, int w, int h) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    const uint8_t b[] = {0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x11, 0x08, (uint8_t)(h >> 8), (uint8_t)h, (uint8_t)(w >> 8),
                         (uint8_t)w, 0x01, 0x01, 0x11, 0x00};
    fwrite(b, 1, sizeof(b), f);
    fclose(f);
}

/* ---- 数据 / Data ---- */

static void item(int i, const char* id, const char* title, const char* author, const char* likes, bool seen) {
    item_t* x = &s_items[i];
    memset(x, 0, sizeof(*x));
    snprintf(x->id, sizeof(x->id), "%s", id);
    snprintf(x->title, sizeof(x->title), "%s", title);
    snprintf(x->author, sizeof(x->author), "%s", author);
    snprintf(x->likes, sizeof(x->likes), "%s", likes);
    x->images = 4;
    x->seen = seen;
    char path[96];
    tomato_red_img_path(id, 0, path, sizeof(path));
    mkjpg(path, 900, 1200);
    tomato_red_cover_path(id, path, sizeof(path));
    mkjpg(path, 900, 1200);
}

// 推荐里已下载的笔记：建好 note.json。/ A downloaded recommendation: create its note.json.
static void downloaded(const char* id) {
    char dir[64], path[96];
    tomato_red_note_dir(id, dir, sizeof(dir));
    mkdir(dir, 0755);
    snprintf(path, sizeof(path), "%s/note.json", dir);
    FILE* f = fopen(path, "w");
    if (f) { fputs("{}", f); fclose(f); }
}

static const char* FOLLOW_MORE[][3] = {
    {"周末去苏州，平江路的小店一家家逛过去", "慢慢走", "4127"},
    {"手冲咖啡入门：一把壶一包豆就够了", "豆子日记", "6.2万"},
    {"给阳台的绿萝换盆，三个月后长这样", "植物观察室", "1904"},
    {"秋冬通勤穿搭｜一件大衣穿出五种样子", "衣橱小记", "3.7万"},
    {"一周晚餐备菜，周日花两个小时", "厨房里的阿青", "1.5万"},
    {"带爸妈去黄山，这条路线不累", "旅行的意义", "8833"},
    {"年度书单｜今年读过最好的十本书", "周末读书会", "2.1万"},
    {"在家做的第一只戚风，没有塌", "烘焙新手", "2690"},
    {"冬天的护手霜我只回购这一支", "日常好物", "5418"},
    {"雨天的西湖，人少得像另一座城", "小鹿在路上", "1.3万"},
    {"给猫做了一个纸箱小屋", "橘猫一家", "7.9万"},
    {"练字第一百天，对比一下变化", "每天写一页", "6352"},
};

static void fill_follow(void) {
    item(0, "f0", "上海初冬银杏地图｜这 5 个地方黄得正好", "小鹿在路上", "2.3万", false);
    item(1, "f1", "一人食｜十分钟搞定的番茄牛腩饭", "厨房里的阿青", "8615", false);
    item(2, "f2", "读完《额尔古纳河右岸》，想去一次大兴安岭", "周末读书会", "3402", true);
    item(3, "f3", "墨水屏阅读器用了三个月的真实感受", "慢慢看书", "1.1万", true);
    for (int i = 0; i < 12; i++) {
        char id[8];
        snprintf(id, sizeof(id), "f%d", i + 4);
        item(i + 4, id, FOLLOW_MORE[i][0], FOLLOW_MORE[i][1], FOLLOW_MORE[i][2], true);
    }
    s_item_count = 16;
    s_list_page = 0;
}

static void fill_rec(void) {
    static const char* REC[][3] = {
        {"冬天的第一锅糖炒栗子，在家也能做", "栗子不是梨", "5.6万"},
        {"极简书桌改造｜小房间也能有阅读角", "收纳研究所", "1.8万"},
        {"去景德镇捏了一天陶，成品有点丑但很开心", "泥巴手作", "9271"},
        {"跑步一年，从 3 公里到半马的记录", "晨跑打卡", "2.4万"},
        {"北方的初雪，窗外一下子全白了", "雪地里的松", "3.1万"},
        {"在图书馆泡了一整天，效率很高", "考研的小周", "4580"},
        {"自制柚子茶，一次做一大罐", "厨房里的阿青", "1.2万"},
        {"骑行环滇池一圈，海鸥已经来了", "单车旅人", "6.7万"},
        {"毛线帽子编织教程，新手也能学会", "织毛衣的猫", "2.2万"},
        {"博物馆里的宋代青瓷，越看越喜欢", "看展的人", "8090"},
        {"给家里的老书柜刷了一遍漆", "旧物新生", "3366"},
        {"一碗热汤面，冬天的快乐很简单", "面条爱好者", "1.9万"},
    };
    for (int i = 0; i < 12; i++) {
        char id[8];
        snprintf(id, sizeof(id), "r%d", i);
        item(i, id, REC[i][0], REC[i][1], REC[i][2], true);
    }
    downloaded("r0");
    downloaded("r2");
    s_item_count = 12;
    s_list_page = 0;
}

static const char* BODY =
    "这两天降温，银杏终于黄透了。周末骑车转了一圈，整理了几个人少景美的地方，供大家参考：\n"
    "\n"
    "1. 衡山路·宛平路：整条路金黄，早上八点前人最少。\n"
    "2. 华东政法大学长宁校区：老建筑配银杏，随手拍都好看。\n"
    "3. 世纪公园银杏大道：草坪开阔，适合带孩子野餐。\n"
    "4. 襄阳北路：小马路，梧桐和银杏混在一起，很安静。\n"
    "5. 上海植物园：落叶铺了一地，记得穿平底鞋。\n"
    "\n"
    "小贴士：阴天拍出来颜色更柔和，逆光拍叶子会透亮。周末人多，尽量工作日早上去。\n"
    "\n"
    "#上海银杏 #城市漫步 #初冬 #周末去哪儿";

static comment_t s_cm[COMMENTS_MAX];

static void note(void) {
    s_note_loaded = true;
    snprintf(s_id, sizeof(s_id), "n1");
    s_title = "上海初冬银杏地图｜这 5 个地方黄得正好";
    s_author = "小鹿在路上";
    s_likes = "2.3万";
    s_comment_total = "486";
    s_body = BODY;
    s_time = 1795275780 - 2 * 86400 - 5 * 3600;
    s_images = 6;
    s_comments_more = true;
    static const char* CM[][3] = {
        {"橘子汽水", "收藏了，下周就去衡山路！", "326"},
        {"阿木", "植物园那张太好看了，请问是几号去的？", "88"},
        {"小鹿在路上", "回复 阿木：上周六上午，现在去应该正好。", "41"},
        {"晴天的云", "华政真的很出片，就是周末要预约。", "57"},
    };
    for (int i = 0; i < 4; i++) {
        s_cm[i] = (comment_t){.user = CM[i][0], .text = CM[i][1], .likes = CM[i][2]};
        s_comments[i] = s_cm[i];
    }
    s_comment_count = 20;
    for (int i = 4; i < 20; i++) s_comments[i] = s_cm[i % 4];
    for (int k = 1; k <= 6; k++) {
        char path[96];
        tomato_red_img_path("n1", k, path, sizeof(path));
        mkjpg(path, 1080, 1440);
    }
    s_title_lines = title_lines();
    layout_body();
    layout_comments();
}

/* ---- 场景 / Scenes ---- */

static void shot(app_ctx_t* ctx, const char* name) {
    hr_allow_fit();
    if (s_view != V_LIST) hr_no_bar_check();
    render(ctx, hr_fb);
    hr_shot(name);
}

void hr_scenes(void) {
    mkdir(DEMO_DIR, 0755);
    app_ctx_t ctx = {.fb = hr_fb};
    on_enter(&ctx);
    s_view = V_LIST;
    s_seg = SEG_FOLLOW;
    fill_follow();
    shot(&ctx, "follow");

    s_seg = SEG_REC;
    fill_rec();
    s_rec_time = 1795275780 - 23 * 60;
    s_rec_left = 3;
    shot(&ctx, "recommend");
    shot(&ctx, "ui30");

    // 点开一篇还没下载的推荐：等待框。/ Opening a recommendation not yet downloaded: the wait dialog.
    s_wait = true;
    snprintf(s_wait_id, sizeof(s_wait_id), "r1");
    s_has_pending = true;
    s_pending = (tomato_red_req_t){.job = TOMATO_RED_NOTE};
    shot(&ctx, "ui31");
    s_wait = false;
    s_has_pending = false;

    s_seg = SEG_FOLLOW;
    fill_follow();
    shot(&ctx, "ui29");

    note();
    s_view = V_IMAGE;
    s_image = 0;
    shot(&ctx, "image");

    // 设计文档：第 2 张、工具条、存好的提示框。/ Design doc: image 2, the toolbar, the saved notice.
    s_image = 1;
    shot(&ctx, "ui32");
    tr_wallsave_open(&s_ws);
    shot(&ctx, "ui33");
    tr_wallsave_close(&s_ws);
    snprintf(s_ws.title, sizeof(s_ws.title), "已存为壁纸");
    snprintf(s_ws.line, sizeof(s_ws.line), "小红书_小鹿在路上_2.jpg\n到 设置 › 显示与阅读 › 锁屏壁纸 里选用");
    shot(&ctx, "ui34");
    tr_wallsave_close(&s_ws);

    s_view = V_TEXT;
    s_tab = T_BODY;
    s_page = 0;
    shot(&ctx, "note");
    shot(&ctx, "ui35");
}
