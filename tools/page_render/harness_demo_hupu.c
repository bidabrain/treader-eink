/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：虎扑的演示截图（README / 网站预览图用）：推荐列表、帖子正文、比分、比赛评分。
 * English: Demo shots of Hupu (README / site previews): feed, thread body, scores and match ratings.
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "app_tr_hupu.c"   // 按 -I 找 / Found via -I
#include "page_render.h"

const char* hr_page = "demo_hupu";

/* ---- 替身 / Stand-ins ---- */

bool tomato_sync_busy(void) { return false; }
uint32_t tomato_sync_seq(void) { return 7; }
bool tomato_sync_preemptible(void) { return true; }
void tomato_sync_cancel(void) {}
void hupu_img_drop_pending(void) {}
void hupu_img_release(void) {}
bool hupu_img_pending(void) { return false; }
bool hupu_img_tick(void) { return false; }
void tomato_json_use_psram(void) {}

// 每张图返回一个不同的地址，画的时候用它在数组里的位置当渐变花样。
// Each image returns a different address; drawing uses its offset in the array as the gradient seed.
static uint8_t s_seeds[256];
const uint8_t* hupu_img_get(const char* jpg, int w, int h) {
    (void)w; (void)h;
    if (!jpg || !jpg[0]) return NULL;
    unsigned v = 0;
    for (const char* c = jpg; *c; c++) v = v * 131 + (unsigned char)*c;
    return &s_seeds[v % sizeof(s_seeds)];
}
void hupu_img_draw(uint8_t* fb, int x, int y, int w, int h, const uint8_t* px, bool border) {
    hr_gradient(fb, (EpdRect){x, y, w, h}, (int)(px - s_seeds) * 37 + 11);
    if (border) epd_draw_rect((EpdRect){x, y, w, h}, 0x60, fb);
}

/* ---- 推荐 / Feed ---- */

typedef struct {
    const char* title;
    const char* zone;
    int replies;
    bool thumb;
} demo_item_t;

static const demo_item_t ITEMS[] = {
    {"湖人主场 118:112 力克勇士，末节一波 12:0 直接打停对手", "湖人专区", 2563, true},
    {"凯尔特人豪取五连胜，球队三分命中率为什么能排联盟第一？", "NBA", 1847, true},
    {"【赛后】掘金加时险胜太阳，替补席合计贡献 46 分，主教练赛后点名表扬两位年轻球员", "掘金专区", 932, false},
    {"周末去爬了趟黄山，云海拍得还行，给大家分享几张", "步行街主干道", 418, true},
    {"新入手的降噪耳机用了一周，说说真实的使用感受", "数码区", 365, true},
    {"英超第 12 轮前瞻：阿森纳客场挑战纽卡斯尔", "英超", 774, true},
    {"冬天跑步膝盖总不舒服，大家一般怎么热身？", "跑步", 287, false},
    {"勇士背靠背作战，主教练赛后谈轮换安排", "勇士专区", 653, true},
    {"CBA 常规赛：辽宁主场大胜，外援砍下 35 分", "CBA", 541, true},
    {"家里老人想换手机，三千价位有什么推荐？", "数码区", 452, false},
    {"总决赛落幕，今年这届全球赛你打几分？", "英雄联盟", 2981, true},
    {"第一次自己做红烧肉，卖相还过得去吧", "步行街主干道", 196, true},
};
#define ITEM_N ((int)(sizeof(ITEMS) / sizeof(ITEMS[0])))

static void demo_feed(void) {
    s_item_count = 0;
    for (int i = 0; i < ITEM_N; i++) {
        item_t* x = &s_items[s_item_count++];
        snprintf(x->tid, sizeof(x->tid), "63%06d", 418230 + i * 977);
        snprintf(x->title, sizeof(x->title), "%s", ITEMS[i].title);
        snprintf(x->meta, sizeof(x->meta), "%s · %d 回复", ITEMS[i].zone, ITEMS[i].replies);
        if (ITEMS[i].thumb) snprintf(x->jpg, sizeof(x->jpg), "thumb_%d.jpg", i);
        else x->jpg[0] = 0;
    }
}

static void demo_home(seg_t seg) {
    s_view = V_HOME;
    s_seg = seg;
    s_msg[0] = 0;
    s_list_page = 0;
    s_job_active = false;
    s_has_pending = false;
}

/* ---- 帖子 / Post ---- */

static void demo_post(void) {
    free_post();
    s_arena_cap = 1;
    s_arena = malloc(1);
    s_title = "湖人主场 118:112 力克勇士，末节一波 12:0 直接打停对手";
    s_author = "紫金名宿";
    s_time = "11-21 13:05";
    s_zone = "湖人专区";
    s_reply_total = 2563;
    static const char* const P1 =
        "今天这场看得太过瘾了。前三节两队咬得很紧，勇士外线手感火热，第三节结束时还领先 4 分。"
        "进入末节，湖人换上防守阵容，连续三个回合逼出对手失误，反击一波 12:0 直接把比赛打停。";
    static const char* const P2 =
        "最后五分钟球队的执行力很好，每个回合都能找到空位，篮板也护得住，终场全场起立鼓掌。";
    static const char* const P3 =
        "下一场背靠背客场打掘金，体能会是不小的考验。希望替补继续保持今天的状态，把这股劲儿带过去。";
    s_block_count = 0;
    block_t* b = &s_blocks[s_block_count++];
    memset(b, 0, sizeof(*b));
    b->text = P1;
    b = &s_blocks[s_block_count++];
    memset(b, 0, sizeof(*b));
    b->image = true;
    snprintf(b->jpg, sizeof(b->jpg), "%s", "post_court.jpg");
    b->w = CW;
    b->h = 330;
    b = &s_blocks[s_block_count++];
    memset(b, 0, sizeof(*b));
    b->text = P2;
    b = &s_blocks[s_block_count++];
    memset(b, 0, sizeof(*b));
    b->text = P3;

    static const char* const TEXTS[] = {
        "末节那波防守是真的硬，连续三次逼出 24 秒违例。",
        "替补今天打得很好，轮换终于有点样子了。",
        "现场看的，气氛太好了，终场全场都在喊。",
        "下一场背靠背打掘金，体能是个考验。",
    };
    static const char* const USERS[] = {"湖畔老球迷", "紫金岁月", "篮下小钢炮", "夜猫看球"};
    s_light_count = s_list_count = 4;
    for (int i = 0; i < 4; i++) {
        reply_t* r = &s_lights[i];
        r->user = USERS[i];
        r->quote = NULL;
        r->text = TEXTS[i];
        r->lights = 860 - i * 170;
        s_list[i] = *r;
    }
    snprintf(s_tid, sizeof(s_tid), "%s", "63418230");
    s_title_lines = title_lines();
    layout_body();
    layout_replies(0);
    layout_replies(1);
    s_post_loaded = true;
}

/* ---- 比分 / Scores ---- */

static const char* const TEAMS[][2] = {
    {"湖人", "勇士"}, {"凯尔特人", "尼克斯"}, {"掘金", "太阳"}, {"雄鹿", "76人"},
    {"独行侠", "快船"}, {"森林狼", "雷霆"}, {"热火", "公牛"},
};
static const char* const SCORES[][2] = {
    {"118", "112"}, {"109", "101"}, {"126", "121"}, {"97", "104"}, {"115", "110"}, {"102", "99"}, {"113", "120"},
};
static const char* const RATED[] = {"8.6万人评分", "5.2万人评分", "4.7万人评分", "3.1万人评分", "2.8万人评分",
                                    "2.4万人评分", "1.9万人评分"};
#define MATCH_N ((int)(sizeof(TEAMS) / sizeof(TEAMS[0])))

static void demo_schedule(void) {
    s_league = 0;
    s_day_count = 3;
    snprintf(s_days[0].label, sizeof(s_days[0].label), "%s", "11月20日 周五");
    snprintf(s_days[1].label, sizeof(s_days[1].label), "%s", "11月21日 周六 今天");
    snprintf(s_days[2].label, sizeof(s_days[2].label), "%s", "11月22日 周日");
    s_days[0].first = 0; s_days[0].count = 0;
    s_days[1].first = 0; s_days[1].count = (int16_t)MATCH_N;
    s_days[2].first = 0; s_days[2].count = 0;
    s_match_count = MATCH_N;
    for (int i = 0; i < MATCH_N; i++) {
        match_t* m = &s_matches[i];
        memset(m, 0, sizeof(*m));
        snprintf(m->a, sizeof(m->a), "%s", TEAMS[i][0]);
        snprintf(m->b, sizeof(m->b), "%s", TEAMS[i][1]);
        snprintf(m->as, sizeof(m->as), "%s", SCORES[i][0]);
        snprintf(m->bs, sizeof(m->bs), "%s", SCORES[i][1]);
        m->st = 2;
        snprintf(m->desc, sizeof(m->desc), "%s", "已结束");
        snprintf(m->count, sizeof(m->count), "%s", RATED[i]);
        snprintf(m->k, sizeof(m->k), "match_%d", i);
        snprintf(m->n, sizeof(m->n), "n%d", i);
        snprintf(m->ts, sizeof(m->ts), "%s", "1763683200000");
        snprintf(m->ai, sizeof(m->ai), "t%da", i);
        snprintf(m->bi, sizeof(m->bi), "t%db", i);
    }
    s_day = 1;
    s_day_page = 0;
}

/* ---- 评分 / Ratings ---- */

typedef struct {
    const char* name;
    const char* min;
    const char* stats;
    float score;
    int count;
    bool coach;
} demo_player_t;

// 虚构球员。/ Fictional players.
static const demo_player_t HOME_PLAYERS[] = {
    {"马库斯·里德", "38分钟", "31分 8篮板 6助攻", 9.2f, 41260, false},
    {"泰勒·布鲁克斯", "35分钟", "22分 11篮板 2盖帽", 8.7f, 32815, false},
    {"德文·哈里斯", "33分钟", "16分 4篮板 9助攻", 8.4f, 27390, false},
    {"卡梅伦·华莱士", "29分钟", "12分 3抢断", 7.9f, 18544, false},
    {"乔丹·米勒", "24分钟", "14分 5篮板", 7.6f, 12087, false},
    {"伊桑·科尔", "21分钟", "9分 6篮板 2助攻", 7.1f, 8931, false},
    {"内森·格兰特", "18分钟", "8分 2助攻", 6.8f, 6420, false},
    {"迈克·安德森", "", "", 8.1f, 15302, true},
};
static const demo_player_t AWAY_PLAYERS[] = {
    {"布兰登·斯科特", "37分钟", "29分 4篮板 7助攻", 8.3f, 30112, false},
    {"凯文·帕克", "34分钟", "18分 9篮板", 7.4f, 19873, false},
    {"瑞安·摩尔", "30分钟", "13分 3助攻", 6.9f, 11204, false},
};

static void add_players(const demo_player_t* list, int n, const char* team) {
    for (int i = 0; i < n; i++) {
        player_t* p = &s_players[s_player_count++];
        memset(p, 0, sizeof(*p));
        snprintf(p->name, sizeof(p->name), "%s", list[i].name);
        snprintf(p->team, sizeof(p->team), "%s", team);
        snprintf(p->type, sizeof(p->type), "%s", list[i].coach ? "coach" : "player");
        snprintf(p->min, sizeof(p->min), "%s", list[i].min);
        snprintf(p->stats, sizeof(p->stats), "%s", list[i].stats);
        p->score = list[i].score;
        p->count = list[i].count;
    }
}

static void demo_rating(void) {
    s_rate_match = 0;
    s_player_count = 0;
    add_players(HOME_PLAYERS, (int)(sizeof(HOME_PLAYERS) / sizeof(HOME_PLAYERS[0])), s_matches[0].ai);
    add_players(AWAY_PLAYERS, (int)(sizeof(AWAY_PLAYERS) / sizeof(AWAY_PLAYERS[0])), s_matches[0].bi);
    s_rate_team = 1;
    s_rate_page = 0;
}

static void demo_shot(app_ctx_t* ctx, const char* name) {
    render(ctx, hr_fb);
    hr_shot(name);
}

void hr_scenes(void) {
    app_ctx_t ctx = {.fb = hr_fb};
    alloc_all();

    demo_home(SEG_FEED);
    demo_feed();
    hr_allow_fit();
    demo_shot(&ctx, "feed");

    s_view = V_POST;
    s_post_back = V_HOME;
    demo_post();
    s_tab = T_BODY;
    s_page = 0;
    hr_no_bar_check();
    hr_allow_fit();
    demo_shot(&ctx, "post");

    demo_home(SEG_SCORE);
    demo_schedule();
    hr_allow_fit();
    demo_shot(&ctx, "scores");

    s_view = V_RATE;
    demo_rating();
    hr_no_bar_check();
    hr_allow_fit();
    demo_shot(&ctx, "rate");
}
