/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：书城（搜索栏、频道、分类胶囊、榜单网格、书籍详情和下载进度）的渲染测试。
 * English: Render bench for the bookstore (search bar, channels, category chips, rank grid, detail and download).
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "app_tr_store.c"   // 按 -I 找：当前源码或 SRC_ROOT / Found via -I: current sources or SRC_ROOT
#include "page_render.h"

const char* hr_page = "store";

/* ---- 后台任务和缓存的替身 / Worker and cache stand-ins ---- */

static bool s_hr_busy;
bool tomato_sync_busy(void) { return s_hr_busy; }
uint32_t tomato_sync_seq(void) { return 0; }
// 封面文件都不存在，网格画文字封面。/ No cover files exist, so the grid draws text covers.
void tomato_rank_cover_path(const char* book_id, char* out, size_t cap) {
    snprintf(out, cap, "/nonexistent/%s.jpg", book_id);
}
void book_cover_drop_pending(void) {}

static tomato_rank_cat_t s_hr_cats[TOMATO_RANK_CAT_MAX];
static tomato_rank_book_t s_hr_books[TOMATO_RANK_BOOK_MAX];
static tomato_book_t s_hr_shelf[4];

// 番茄榜单常见的分类名（两个频道）。/ Typical Tomato rank categories for both channels.
static const char* const MALE[] = {"都市日常", "都市修真", "都市高武", "玄幻脑洞", "传统玄幻", "历史古代", "科幻末世",
                                   "悬疑脑洞", "抗战谍战", "游戏体育", "动漫衍生", "西方奇幻", "东方仙侠", "战神赘婿",
                                   "都市种田", "历史脑洞", "悬疑灵异", "男频衍生", "体育", "末世"};
static const char* const FEMALE[] = {"古风世情", "现言脑洞", "宫斗宅斗", "豪门总裁", "玄幻言情", "星光璀璨",
                                     "青春甜宠", "年代", "种田", "快穿", "职场婚恋", "民国言情", "女频衍生",
                                     "女频悬疑", "双男主", "古言脑洞", "现代言情", "悬疑恋爱"};

static void hr_cats(int male_n, int female_n) {
    memset(s_hr_cats, 0, sizeof(s_hr_cats));
    int n = 0;
    for (int i = 0; i < male_n; i++, n++) {
        snprintf(s_hr_cats[n].name, sizeof(s_hr_cats[n].name), "%s", MALE[i]);
        s_hr_cats[n].male = true;
    }
    for (int i = 0; i < female_n; i++, n++) {
        snprintf(s_hr_cats[n].name, sizeof(s_hr_cats[n].name), "%s", FEMALE[i]);
        s_hr_cats[n].male = false;
    }
    s_cats = s_hr_cats;
    s_cat_count = n;
    snprintf(s_date, sizeof(s_date), "%s", "2026-11-20");
    s_cur = 2;
}

static void hr_books(int n) {
    static const char* const T[] = {"十日终焉", "我在精神病院学斩神：一个特别特别长的书名", "大奉打更人", "道诡异仙",
                                    "宿命之环", "夜的命名术", "深海余烬", "诡秘之主", "斗破苍穹"};
    memset(s_hr_books, 0, sizeof(s_hr_books));
    for (int i = 0; i < n; i++) {
        tomato_rank_book_t* b = &s_hr_books[i];
        snprintf(b->book_id, sizeof(b->book_id), "%d", 3000 + i);
        snprintf(b->title, sizeof(b->title), "%s", T[i % 9]);
        snprintf(b->author, sizeof(b->author), "%s", i == 1 ? "名字特别长的作者用来测试截断" : "杀虫队队员");
        snprintf(b->reads, sizeof(b->reads), "%s", i % 4 == 3 ? "" : "25.4万");
        snprintf(b->intro, sizeof(b->intro), "%s",
                 "陈陈醒来发现自己身处一座诡异的城市，十天之后所有人都会死去。为了活下去，他必须在一场场生死游戏里找到真相，"
                 "而每一次死亡都会让记忆重新开始。这是一段很长的简介，用来看八行以后有没有被正确截断。城市里的每个人都戴着"
                 "动物面具，参与者需要在规定时间内完成游戏，否则就会被淘汰。他慢慢发现，这座城市的秘密远比想象的更深。"
                 "简介继续写下去，直到超过八行为止，看看最后一行是不是被截断了，以及按钮上面还有没有足够的空白。");
    }
    s_books = s_hr_books;
    s_book_count = n;
    memset(s_hr_shelf, 0, sizeof(s_hr_shelf));
    snprintf(s_hr_shelf[0].book_id, sizeof(s_hr_shelf[0].book_id), "%s", "3002");
    s_shelf = s_hr_shelf;
    s_shelf_count = 1;
}

static void hr_reset(void) {
    s_view = V_HOME;
    s_op = s_last = OP_NONE;
    s_queued = Q_NONE;
#ifndef HR_BASELINE
    s_dl_index = -1;
#endif
    s_hr_busy = false;
    memset(&s_status, 0, sizeof(s_status));
    s_male = true;
    s_page = 0;
    s_cat_count = -1;
    s_book_count = -1;
    s_cur = -1;
    s_shelf_count = 0;
}

static app_ctx_t s_ctx;

static void hr_render(const char* scene) {
    s_ctx.fb = hr_fb;
    render(&s_ctx, hr_fb);
    hr_shot(scene);
}

// 左滑一下（分类多时翻页；改动前首页不理滑动）。/ One left swipe (turns chip pages; the old home ignored swipes).
static void hr_swipe(void) {
    ui_gesture_event_t ev = {0};
    ev.type = UI_GESTURE_SWIPE_L;
    on_gesture(&s_ctx, &ev);
}

void hr_scenes(void) {
    hr_reset();
    hr_render("home_no_cache");

    hr_reset();
    hr_cats(20, 18);
    hr_render("home_male");

    hr_reset();
    hr_cats(20, 18);
    hr_swipe();
    hr_render("home_male_swiped");

    hr_reset();
    hr_cats(20, 18);
    s_male = false;
    s_cur = 25;
    hr_render("home_female");

    hr_reset();
    hr_cats(8, 6);
    hr_render("home_few");

    hr_reset();
    hr_cats(20, 18);
    s_hr_busy = true;
    s_op = OP_RANK;
    s_status.phase = TOMATO_CONNECTING;
    hr_render("home_fetching");

    hr_reset();
    hr_cats(20, 18);
    s_last = OP_RANK;
    s_status.phase = TOMATO_FAILED;
    snprintf(s_status.message, sizeof(s_status.message), "%s", "获取榜单失败：连接超时，请检查网络后再点刷新");
    hr_render("home_failed");

    hr_reset();
    hr_cats(20, 18);
    hr_books(20);
    s_view = V_RANK;
    hr_allow_fit();
    hr_render("rank");

    hr_reset();
    hr_cats(20, 18);
    hr_books(20);
    s_view = V_RANK;
    s_page = 2;
    hr_allow_fit();
    hr_render("rank_last_page");

    hr_reset();
    hr_cats(20, 18);
    s_book_count = -1;
    s_view = V_RANK;
    hr_render("rank_missing");

    hr_reset();
    hr_cats(20, 18);
    hr_books(20);
    s_view = V_DETAIL;
    s_detail = 1;
    hr_allow_fit();
    hr_render("detail");

    hr_reset();
    hr_cats(20, 18);
    hr_books(20);
    s_view = V_DETAIL;
    s_detail = 2;
    hr_render("detail_owned");

    hr_reset();
    hr_cats(20, 18);
    hr_books(20);
    s_view = V_DETAIL;
    s_detail = 0;
    s_hr_busy = true;
    s_op = OP_DOWNLOAD;
#ifndef HR_BASELINE
    s_dl_index = 0;   // 进度只画在正在下载的那本上 / Progress shows on the downloading book only
    snprintf(s_dl_id, sizeof(s_dl_id), "%s", s_books[0].book_id);
#endif
    s_status.phase = TOMATO_QUEUED;
    hr_render("detail_downloading");

    hr_reset();
    hr_cats(20, 18);
    hr_books(20);
    s_view = V_DETAIL;
    s_detail = 0;
    s_last = OP_DOWNLOAD;
#ifndef HR_BASELINE
    s_dl_index = 0;   // 失败原因只画在下载的那本上 / The failure shows on the downloaded book only
    snprintf(s_dl_id, sizeof(s_dl_id), "%s", s_books[0].book_id);
#endif
    s_status.phase = TOMATO_FAILED;
    snprintf(s_status.message, sizeof(s_status.message), "%s", "下载失败：服务器生成 TXT 超时，可能被限流，过几分钟再试");
    hr_render("detail_failed");
#ifndef HR_BASELINE
    // 另一本在下时点“下载到书库”：说原因。/ Tapping "下载到书库" while another book downloads explains why.
    hr_reset();
    hr_cats(20, 18);
    hr_books(20);
    s_view = V_DETAIL;
    s_detail = 1;
    snprintf(s_detail_note, sizeof(s_detail_note), "%s", "正在下载另一本，下完再点");
    hr_allow_fit();   // 只截了作者名（内容）/ Only the author name (content) is cut
    hr_render("detail_busy_note");
#endif
}
