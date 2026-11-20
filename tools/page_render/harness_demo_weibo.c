/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：微博标签页的演示截图（README / 网站预览用）：页面摆成真实用户的样子。
 * English: Demo shots of the Weibo tab for the README / site previews, with the page in a realistic state.
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include <sys/stat.h>

#include "app_tr_weibo.c"   // 按 -I 找：当前源码 / Found via -I: current sources
#include "page_render.h"
#include "settings.h"

const char* hr_page = "demo_weibo";

/* ---- 替身 / Stand-ins ---- */

#define DEMO_DIR "/tmp/hrdemo_weibo"
#define NOW 1795275780   // 2026-11-21 15:43

esp_err_t tomato_wb_config_load(tomato_wb_config_t* out) { (void)out; return ESP_OK; }
bool tomato_sync_busy(void) { return false; }
bool tomato_sync_preemptible(void) { return false; }
uint32_t tomato_sync_seq(void) { return 1; }
void tomato_sync_cancel(void) {}
void tomato_sync_get_status(tomato_status_t* out) { memset(out, 0, sizeof(*out)); }
void tomato_json_use_psram(void) {}
void tomato_wb_post_dir(const char* id, char* out, size_t cap) { snprintf(out, cap, DEMO_DIR "/post_%s", id); }
void tomato_wb_img_path(const char* id, int n, char* out, size_t cap) { snprintf(out, cap, DEMO_DIR "/%s_%d.jpg", id, n); }
void tomato_wb_square_path(const char* id, int k, char* out, size_t cap) { snprintf(out, cap, DEMO_DIR "/%s_s%d.jpg", id, k); }
void tomato_wb_comments_path(const char* id, char* out, size_t cap) { snprintf(out, cap, DEMO_DIR "/%s_c.json", id); }

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

// 给一条微博建好它会用到的图片文件。/ Create the image files a post uses.
static void make_images(const char* id, int images, int pics) {
    char path[96];
    tomato_wb_img_path(id, 0, path, sizeof(path));
    mkjpg(path, 1080, 810);
    for (int k = 1; k <= images; k++) {
        tomato_wb_img_path(id, k, path, sizeof(path));
        mkjpg(path, 1080, 1440);
    }
    for (int k = 1; k <= pics; k++) {
        tomato_wb_square_path(id, k, path, sizeof(path));
        mkjpg(path, 400, 400);
    }
}

/* ---- 数据 / Data ---- */

typedef struct {
    const char *user, *text, *rt_user, *rt_text, *source;
    int ago_min, images, pics;
    float ratio;
    bool seen;
    int reposts, comments, likes;
} demo_post_t;

static void fill(const demo_post_t* p, int n, const char* prefix) {
    for (int i = 0; i < n; i++) {
        item_t* x = &s_items[i];
        memset(x, 0, sizeof(*x));
        snprintf(x->id, sizeof(x->id), "%s%d", prefix, i);
        snprintf(x->user, sizeof(x->user), "%s", p[i].user);
        snprintf(x->text, sizeof(x->text), "%s", p[i].text);
        snprintf(x->rt_user, sizeof(x->rt_user), "%s", p[i].rt_user ? p[i].rt_user : "");
        snprintf(x->rt_text, sizeof(x->rt_text), "%s", p[i].rt_text ? p[i].rt_text : "");
        snprintf(x->source, sizeof(x->source), "%s", p[i].source);
        x->time = NOW - p[i].ago_min * 60.0;
        x->images = (int16_t)p[i].images;
        x->pics = (int16_t)p[i].pics;
        x->ratio = p[i].ratio;
        x->seen = p[i].seen;
        x->reposts = p[i].reposts;
        x->comments = p[i].comments;
        x->likes = p[i].likes;
        make_images(x->id, p[i].images, p[i].pics);
    }
    s_item_count = n;
    s_list_page = 0;
    layout_list();
}

// 第一页放得下两张卡片：九宫格那条正文两行，转发那条正文一行。/ Page one holds two cards: the image post has two
// text lines and the repost one.
static const demo_post_t FOLLOW[] = {
    {"城市晚报", "【初雪来了】今天上午北京城区迎来今冬第一场雪，故宫、景山一带银装素裹。", NULL, NULL, "微博视频号", 28, 4,
     4, 1, false, 326, 1204, 8873},
    {"读书小站", "冬天很适合慢慢读这一本，推荐给大家。", "出版社书讯",
     "《额尔古纳河右岸》新版上市，附作者手写序言与全新插图。", "微博网页版", 64, 0, 0, 0, false, 54, 37, 612},
    {"旅行笔记本", "在大理住了一周，每天早上沿着洱海骑车，下午找家小店晒太阳看书。慢下来的日子真好。",
     NULL, NULL, "iPhone 客户端", 95, 1, 1, 0.75f, true, 18, 96, 1532},
    {"天气小助手", "周末降温明显，南方大部地区气温下降 6 到 8 度，记得添衣。", NULL, NULL, "微博网页版", 150, 0, 0, 0, true,
     212, 88, 2043},
    {"美食研究所", "冬至快到了，分享三种家常饺子馅：白菜猪肉、韭菜鸡蛋、玉米虾仁。", NULL, NULL, "Android 客户端", 210, 3,
     3, 1, true, 431, 902, 12000},
    {"城市晚报", "【地铁新线开通】今天起地铁新线正式运营，全线设站 18 座，早晚高峰 3 分钟一班。", NULL, NULL, "微博视频号",
     300, 1, 1, 0.75f, true, 2310, 6644, 52000},
    {"读书小站", "本周新书推荐：五本适合冬夜读的小说，篇幅都不长，一个周末就能读完。", NULL, NULL, "微博网页版", 420, 5, 5, 1,
     true, 97, 210, 3380},
    {"旅行笔记本", "苍山下的小村子，傍晚的光特别温柔。", NULL, NULL, "iPhone 客户端", 600, 2, 2, 1, true, 12, 45, 980},
};

// 第一页：三行字带三张图，再一条三行纯文字。/ Page one: three text lines with three images, then a three-line text post.
static const demo_post_t HOT[] = {
    {"科技日报", "我国新一代气象卫星今天成功发射，将为天气预报提供分辨率更高的云图，预计明年投入业务运行，台风和暴雨的预报也会更准更及时。",
     NULL, NULL, "微博网页版", 40, 3, 3, 1, true, 5821, 13000, 186000},
    {"体育前线", "冬季长跑接力赛今早在滨江开跑，三千多名跑者参加，最小的选手只有九岁，最年长的已经七十一岁，沿途不少市民自发送上热水和加油声。",
     NULL, NULL, "iPhone 客户端", 75, 0, 0, 0, true, 977, 3105, 41000},
    {"城市晚报", "【地铁新线开通】今天起地铁新线正式运营，全线设站 18 座，早晚高峰 3 分钟一班。", NULL, NULL, "微博视频号",
     130, 1, 1, 0.75f, true, 2310, 6644, 52000},
    {"文化周刊", "博物馆冬季特展开幕，百余件宋代瓷器首次集中展出，展期到明年三月。", NULL, NULL, "微博网页版", 200, 2, 2, 1,
     true, 640, 1820, 27000},
    {"天气小助手", "北方多地迎来降雪，部分高速路段临时封闭，出行前请查看路况。", NULL, NULL, "微博网页版", 260, 0, 0, 0, true,
     1520, 2210, 33000},
    {"美食研究所", "一碗热汤面的五种做法，冬天吃最暖和。", NULL, NULL, "Android 客户端", 330, 4, 4, 1, true, 860, 1430, 21000},
};

static const char* BODY =
    "【初雪来了】今天上午，北京城区迎来今冬第一场雪，故宫、景山一带银装素裹，不少市民一早赶来拍照。\n"
    "气象部门预计，降雪将持续到傍晚，累计降雪量 2 到 4 毫米。夜间到明天早上路面可能结冰，"
    "开车请减速慢行，步行注意防滑；公交地铁运行正常。\n"
    "你那里下雪了吗？欢迎在评论区晒出你拍的雪景。";

static comment_t s_cm[COMMENTS_MAX];

static void post(void) {
    s_post_loaded = true;
    s_view = V_TEXT;
    s_page = 0;
    snprintf(s_id, sizeof(s_id), "p1");
    s_user = "城市晚报";
    s_source = "微博视频号";
    s_body = BODY;
    s_rt = "";
    s_time = NOW - 28 * 60;
    s_images = 4;
    s_pics = 4;
    s_likes = 8873;
    s_comment_total = 1204;
    s_reposts = 326;
    make_images("p1", 4, 4);
    layout_body();
    static const comment_t CM[] = {
        {"雪地里的松", "楼下已经白了一片，孩子一早就拉着我下去堆雪人。", 1532},
        {"晨跑打卡", "早上跑步的时候开始下的，跑完头发上全是雪花，在路灯下亮晶晶的，太浪漫了。", 864},
        {"城市晚报", "回复@晨跑打卡：注意保暖，路滑慢一点跑。", 412},
        {"小林同学", "南方人表示羡慕，今年一定要去北方看一次雪。", 297},
        {"慢慢看书", "下雪天最适合窝在家里看书，再泡一壶热茶。", 158},
        {"旅行笔记本", "故宫的雪景每年都看不够，周末准备去拍角楼。", 96},
        {"橘子汽水", "刚下班出来就看到雪，心情一下子好了。", 73},
        {"阿木", "求问景山几点开门？想去拍日出。", 41},
        {"城市晚报", "回复@阿木：景山公园早上六点开园，雪天请注意台阶防滑。", 38},
        {"晴天的云", "第一场雪总是让人特别开心。", 25},
    };
    const int n = sizeof(CM) / sizeof(CM[0]);
    for (int i = 0; i < n; i++) { s_cm[i] = CM[i]; s_comments[i] = s_cm[i]; }
    s_comment_count = n;
    s_comments_loaded = true;
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
    // 常用里放上微博，底部标签栏才有这一格（像常看微博的用户那样）。/ Pin Weibo so the bar has its cell, as a
    // regular Weibo reader would.
    static const uint8_t PINS[] = {TR_TAB_COMIC, TR_TAB_TOMATO, TR_TAB_XHS, TR_TAB_WEIBO};
    tr_tabs_configure(app_settings_hidden_tabs(), PINS, 4);
    on_enter(&ctx);
    s_view = V_LIST;
    s_list_time = NOW - 12 * 60;
    s_seg = SEG_FOLLOW;
    fill(FOLLOW, sizeof(FOLLOW) / sizeof(FOLLOW[0]), "f");
    shot(&ctx, "follow");

    s_seg = SEG_HOT;
    fill(HOT, sizeof(HOT) / sizeof(HOT[0]), "h");
    shot(&ctx, "hot");

    s_seg = SEG_FOLLOW;
    post();
    s_tab = T_BODY;
    shot(&ctx, "post");

    s_tab = T_COMMENTS;
    s_page = 0;
    shot(&ctx, "comments");
}
