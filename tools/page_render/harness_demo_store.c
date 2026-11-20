/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：书城的演示截图（README / 网站预览图用）：首页、榜单网格、书籍详情。
 * English: Demo shots of the bookstore (README / site previews): home, rank grid and book detail.
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "app_tr_store.c"   // 按 -I 找 / Found via -I
#include "page_render.h"

#include <sys/stat.h>

const char* hr_page = "demo_store";

/* ---- 后台任务和缓存的替身 / Worker and cache stand-ins ---- */

bool tomato_sync_busy(void) { return false; }
uint32_t tomato_sync_seq(void) { return 0; }
void book_cover_drop_pending(void) {}

// 封面放在临时目录里的小文件，页面 stat 得到就当有图片封面（演示模式画渐变）。
// Covers are small files in a temp dir; when the page can stat one it counts as an image cover (a gradient in demo mode).
#define DEMO_COVER_DIR "/tmp/hr_demo_covers"
void tomato_rank_cover_path(const char* book_id, char* out, size_t cap) {
    snprintf(out, cap, DEMO_COVER_DIR "/%s.jpg", book_id);
}

static void demo_cover_file(const char* book_id) {
    char path[96];
    tomato_rank_cover_path(book_id, path, sizeof(path));
    FILE* f = fopen(path, "wb");
    if (f) { fputs("jpg", f); fclose(f); }
}

/* ---- 数据 / Data ---- */

static tomato_rank_cat_t s_demo_cats[TOMATO_RANK_CAT_MAX];
static tomato_rank_book_t s_demo_books[TOMATO_RANK_BOOK_MAX];
static tomato_book_t s_demo_shelf[2];

// 番茄榜单常见的分类名。/ Typical Tomato rank categories.
static const char* const MALE[] = {"都市日常", "都市修真", "都市高武", "玄幻脑洞", "传统玄幻", "历史古代", "科幻末世",
                                   "悬疑脑洞", "抗战谍战", "游戏体育", "动漫衍生", "西方奇幻", "东方仙侠", "战神赘婿",
                                   "都市种田", "历史脑洞", "悬疑灵异", "男频衍生"};
static const char* const FEMALE[] = {"古风世情", "现言脑洞", "宫斗宅斗", "豪门总裁", "玄幻言情", "星光璀璨",
                                     "青春甜宠", "年代", "种田", "快穿", "职场婚恋", "民国言情"};

static void demo_cats(void) {
    memset(s_demo_cats, 0, sizeof(s_demo_cats));
    int n = 0;
    for (size_t i = 0; i < sizeof(MALE) / sizeof(MALE[0]); i++, n++) {
        snprintf(s_demo_cats[n].name, sizeof(s_demo_cats[n].name), "%s", MALE[i]);
        s_demo_cats[n].male = true;
    }
    for (size_t i = 0; i < sizeof(FEMALE) / sizeof(FEMALE[0]); i++, n++) {
        snprintf(s_demo_cats[n].name, sizeof(s_demo_cats[n].name), "%s", FEMALE[i]);
        s_demo_cats[n].male = false;
    }
    s_cats = s_demo_cats;
    s_cat_count = n;
    snprintf(s_date, sizeof(s_date), "%s", "2026-11-21");
    s_cur = 0;   // 都市日常 / Urban daily life
}

typedef struct {
    const char* title;
    const char* author;
    const char* reads;
} demo_book_t;

static const demo_book_t BOOKS[] = {
    {"大王饶命", "会说话的肘子", "86.3万"}, {"我不是戏神", "三九音域", "72.9万"},
    {"第一序列", "会说话的肘子", "54.1万"}, {"全职高手", "蝴蝶蓝", "48.6万"},
    {"灵境行者", "卖报小郎君", "41.2万"},   {"夜的命名术", "会说话的肘子", "37.8万"},
    {"超神机械师", "齐佩甲", "29.5万"},     {"万族之劫", "老鹰吃小鸡", "26.4万"},
    {"修真聊天群", "圣骑士的传说", "21.7万"},
};
#define BOOK_N ((int)(sizeof(BOOKS) / sizeof(BOOKS[0])))

static const char* const INTRO =
    "灵气复苏的年代，洛城高三学生吕树意外得到一个古怪的系统：别人冲他生出的负面情绪，都会变成他修行用的能量。"
    "于是这个嘴上从不吃亏的少年，一边供妹妹小丸子上学、在巷口摆摊卖煮鸡蛋，一边被卷进越来越大的风波。"
    "天罗地网、修行门派、遗迹与秘境接连出现，他也从街头少年一步步走到了时代的最前面。"
    "轻松的日常里藏着热血和成长，读起来很是解压。";

static void demo_books(void) {
    mkdir(DEMO_COVER_DIR, 0755);
    memset(s_demo_books, 0, sizeof(s_demo_books));
    for (int i = 0; i < BOOK_N; i++) {
        tomato_rank_book_t* b = &s_demo_books[i];
        snprintf(b->book_id, sizeof(b->book_id), "73%08d", 4417 + i * 15731);
        snprintf(b->title, sizeof(b->title), "%s", BOOKS[i].title);
        snprintf(b->author, sizeof(b->author), "%s", BOOKS[i].author);
        snprintf(b->reads, sizeof(b->reads), "%s", BOOKS[i].reads);
        snprintf(b->cover, sizeof(b->cover), "https://p3.example/%s.jpg", b->book_id);
        snprintf(b->intro, sizeof(b->intro), "%s", INTRO);
        demo_cover_file(b->book_id);
    }
    s_books = s_demo_books;
    s_book_count = BOOK_N;
    // 第一序列已经在番茄书架上。/ 第一序列 is already on the Tomato shelf.
    memset(s_demo_shelf, 0, sizeof(s_demo_shelf));
    snprintf(s_demo_shelf[0].book_id, sizeof(s_demo_shelf[0].book_id), "%s", s_demo_books[2].book_id);
    s_shelf = s_demo_shelf;
    s_shelf_count = 1;
}

static void demo_reset(void) {
    s_view = V_HOME;
    s_op = s_last = OP_NONE;
    s_queued = Q_NONE;
    s_dl_index = -1;
    s_dl_id[0] = 0;
    s_detail_note[0] = 0;
    memset(&s_status, 0, sizeof(s_status));
    s_male = true;
    s_page = 0;
    s_chip_page = 0;
    demo_cats();
    demo_books();
}

static void demo_shot(const char* scene) {
    app_ctx_t ctx = {.fb = hr_fb};
    render(&ctx, hr_fb);
    hr_shot(scene);
}

void hr_scenes(void) {
    demo_reset();
    demo_shot("home");

    demo_reset();
    s_view = V_RANK;
    hr_allow_fit();
    demo_shot("rank");

    demo_reset();
    s_view = V_DETAIL;
    s_detail = 0;
    hr_allow_fit();
    demo_shot("detail");
}
