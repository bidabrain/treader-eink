/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：虎扑的演示截图（README / 网站预览图和设计文档用）：推荐、关注、全部专区、专区帖子、帖子正文与亮了、看大图、
 * 比分、比赛评分。
 * English: Demo shots of Hupu (README / site previews and the design doc): feed, following, all zones, zone posts,
 * thread body and lit replies, image view, scores and match ratings.
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
    {"总决赛落幕，五局大战打满，今年这届全球赛你打几分？", "英雄联盟", 2981, true},
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

/* ---- 设计文档用的补充内容 / Extra content for the design doc ---- */

// 帖子正文后面再加两张图，看大图时是“图 n / 3”。/ Two more images after the body so the image view shows "n / 3".
static void demo_post_more_images(void) {
    static const char* const P4 = "赛后球员们在场边和球迷合影，给大家放两张现场照片。";
    block_t* b = &s_blocks[s_block_count++];
    memset(b, 0, sizeof(*b));
    b->text = P4;
    for (int i = 0; i < 2; i++) {
        b = &s_blocks[s_block_count++];
        memset(b, 0, sizeof(*b));
        b->image = true;
        snprintf(b->jpg, sizeof(b->jpg), "post_fans_%d.jpg", i);
        b->w = CW;
        b->h = 400;
    }
    layout_body();
}

// 亮了：几条热门回复，有长有短，带一条引用。/ Lit replies: long and short, one with a quote.
typedef struct {
    const char* user;
    const char* quote;
    const char* text;
    int lights;
} demo_reply_t;

static const demo_reply_t LIGHTS[] = {
    {"湖畔老球迷", NULL, "末节那波防守是真的硬，连续三个回合逼出对手失误，教练这次的换人终于换对了。", 862},
    {"紫金岁月", NULL, "替补今天打得很好，轮换终于有点样子了。", 691},
    {"篮下小钢炮", "湖畔老球迷：末节那波防守是真的硬",
     "同意，而且防守阵容上场以后篮板也护住了，前三节被抢了那么多前场板，末节一个都没让对面拿到。", 534},
    {"夜猫看球", NULL, "现场看的，气氛太好了，终场全场起立鼓掌，回家路上大家还在聊最后那波反击。", 418},
    {"三分雨中漫步", NULL, "这场的集锦我已经看了三遍了。", 307},
    {"西海岸的风", NULL,
     "说句公道话，勇士今天外线手感其实不错，第三节差点就被他们打出来了。最后输在体能上，背靠背的第二场，"
     "末节明显腿软。下一场我们也是背靠背客场打掘金，同样的问题也得注意，主力的上场时间要控制好，"
     "别到了季后赛又是一身伤。",
     256},
    {"老王说球", NULL, "下一场背靠背打掘金，体能是个考验，希望主力上场时间能控制一下。", 198},
};

static void demo_lights(void) {
    const int n = (int)(sizeof(LIGHTS) / sizeof(LIGHTS[0]));
    s_light_count = n;
    for (int i = 0; i < n; i++) {
        reply_t* r = &s_lights[i];
        r->user = LIGHTS[i].user;
        r->quote = LIGHTS[i].quote;
        r->text = LIGHTS[i].text;
        r->lights = LIGHTS[i].lights;
    }
    layout_replies(0);
}

// 关注：5 个专区的帖子轮流混排。/ Following: posts from five zones interleaved.
typedef struct {
    const char* title;
    int zone;
    int replies;
} demo_follow_t;

static const char* const FOLLOW_ZONES[] = {"步行街主干道", "湖人专区", "英雄联盟", "数码区", "跑步"};
static const demo_follow_t FOLLOW_POSTS[] = {
    {"下班路上捡到一只橘猫，带回家三个月了，现在它已经是家里的老大，晒几张照片给大家看看", 0, 2310},
    {"主场力克勇士，赛后更衣室里大家都很放松", 1, 856},
    {"总决赛落幕，五局大战打满，今年这届全球赛你打几分？", 2, 2981},
    {"用了三个月墨水屏阅读器，说说真实感受", 3, 421},
    {"第一次跑完半马，用时 2 小时 05 分，最后三公里真的是咬着牙跑下来的，纪念一下", 4, 312},
    {"大家每天花多少时间看手机？", 0, 1288},
    {"赛后采访：主教练谈最后两分钟的处理，称球员执行得很坚决", 1, 432},
    {"新版本上路野辅联动，大家觉得哪套阵容最强？", 2, 350},
    {"家里老人想换手机，三千价位有什么推荐？", 3, 452},
    {"冬天跑步膝盖总不舒服，大家一般怎么热身？", 4, 287},
    {"周末去爬了趟黄山，云海拍得还行", 0, 418},
    {"今晚轮换：替补阵容有调整", 1, 298},
    {"冬季转会窗口开启，各队阵容变化汇总", 2, 765},
    {"降噪耳机用了一周，说说真实的使用感受", 3, 365},
    {"晨跑还是夜跑？说说你的习惯", 4, 174},
    {"第一次自己做红烧肉，卖相还过得去吧", 0, 196},
    {"数据流：主场大胜，三人得分上二十", 1, 177},
    {"有没有一起开黑的，周末晚上固定车队", 2, 143},
    {"旧笔记本加了内存条，又能再战三年", 3, 209},
    {"给大家推荐几条城市里适合跑步的路线", 4, 131},
};

static void demo_follow(void) {
    s_follow_count = 5;
    for (int i = 0; i < 5; i++) {
        snprintf(s_follows[i].id, sizeof(s_follows[i].id), "%d", 30 + i);
        snprintf(s_follows[i].name, sizeof(s_follows[i].name), "%s", FOLLOW_ZONES[i]);
    }
    s_item_count = 0;
    for (int i = 0; i < (int)(sizeof(FOLLOW_POSTS) / sizeof(FOLLOW_POSTS[0])); i++) {
        item_t* x = &s_items[s_item_count++];
        snprintf(x->tid, sizeof(x->tid), "64%06d", 120400 + i * 613);
        snprintf(x->title, sizeof(x->title), "%s", FOLLOW_POSTS[i].title);
        snprintf(x->meta, sizeof(x->meta), "%s · %d 回复", FOLLOW_ZONES[FOLLOW_POSTS[i].zone], FOLLOW_POSTS[i].replies);
        x->jpg[0] = 0;   // 关注流不取缩略图 / The following feed has no thumbnails
    }
}

// 全部专区：选中 NBA 分类，5 个专区已关注。/ All zones: the NBA category, five zones followed.
static const char* const ZONE_CATS[] = {"步行街", "NBA", "CBA", "国际足球", "中国足球", "综合体育",
                                        "影视娱乐", "游戏", "二次元", "装备", "数码", "汽车"};
static const char* const NBA_ZONES[] = {
    "NBA", "湖人专区", "勇士专区", "凯尔特人", "火箭专区", "骑士专区", "马刺专区", "篮网专区", "雷霆专区",
    "掘金专区", "76人专区", "快船专区", "尼克斯", "独行侠", "国王专区", "热火专区", "公牛专区", "老鹰专区",
    "魔术专区", "太阳专区", "雄鹿专区", "森林狼", "开拓者", "爵士专区", "鹈鹕专区", "灰熊专区", "猛龙专区",
    "步行者", "活塞专区", "黄蜂专区", "奇才专区", "NBA 选秀", "篮球场", "篮球装备"};
// 其他分类只放几个，截图里看不到。/ Other categories get a few zones; they are not on screen.
static const char* const OTHER_ZONES[] = {"步行街主干道", "英雄联盟", "数码区", "跑步", "影视区", "汽车区"};

static void demo_zones(void) {
    const int nc = (int)(sizeof(ZONE_CATS) / sizeof(ZONE_CATS[0]));
    s_cat_count = nc;
    s_zone_count = 0;
    for (int c = 0; c < nc; c++) {
        snprintf(s_cats[c].name, sizeof(s_cats[c].name), "%s", ZONE_CATS[c]);
        s_cats[c].first = (int16_t)s_zone_count;
        const int n = c == 1 ? (int)(sizeof(NBA_ZONES) / sizeof(NBA_ZONES[0])) : 6;
        for (int z = 0; z < n; z++) {
            snprintf(s_zones[s_zone_count].id, sizeof(s_zones[0].id), "%d", 100 + s_zone_count);
            snprintf(s_zones[s_zone_count].name, sizeof(s_zones[0].name), "%s", c == 1 ? NBA_ZONES[z] : OTHER_ZONES[z]);
            s_zone_count++;
        }
        s_cats[c].count = (int16_t)n;
    }
    // 关注湖人、勇士和另外三个专区。/ Follow 湖人, 勇士 and three other zones.
    const int nba = s_cats[1].first;
    const int picks[] = {nba + 1, nba + 2, s_cats[0].first, s_cats[7].first + 1, s_cats[10].first + 2};
    s_follow_count = 5;
    for (int i = 0; i < 5; i++) {
        snprintf(s_follows[i].id, sizeof(s_follows[i].id), "%s", s_zones[picks[i]].id);
        snprintf(s_follows[i].name, sizeof(s_follows[i].name), "%s", s_zones[picks[i]].name);
    }
    s_cat = 1;
    s_zone_page = 0;
}

// 专区帖子：湖人专区，已关注，按最新回复。/ Zone posts: 湖人专区, followed, latest reply first.
typedef struct {
    const char* title;
    const char* user;
    const char* when;
    int replies;
} demo_zone_post_t;

static const demo_zone_post_t ZONE_POSTS[] = {
    {"湖人主场 118:112 力克勇士，末节一波 12:0 直接打停对手", "紫金名宿", "2小时前", 2563},
    {"赛后采访：主教练谈最后两分钟的处理", "湖人老球迷", "3小时前", 432},
    {"今晚的替补席贡献了 38 分，两位年轻球员都打出了赛季最佳，轮换终于理顺了？", "洛城之光", "4小时前", 298},
    {"数据流：三人得分上二十，篮板 52 比 41", "JR数据组", "5小时前", 177},
    {"有没有懂哥分析一下现在的防守体系，为什么末节换上这套阵容以后效果这么好", "篮下小钢炮", "6小时前", 640},
    {"下一场背靠背客场打掘金，大家怎么看？", "夜猫看球", "7小时前", 351},
};

static void demo_zone(void) {
    snprintf(s_zone_id, sizeof(s_zone_id), "%s", s_zones[s_cats[1].first + 1].id);   // 已关注 / Followed
    snprintf(s_zone_name, sizeof(s_zone_name), "%s", "湖人专区");
    s_zone_sort = 0;
    s_list_end = false;
    s_item_count = 0;
    const int n = (int)(sizeof(ZONE_POSTS) / sizeof(ZONE_POSTS[0]));
    for (int k = 0; k < 4 * n; k++) {
        const demo_zone_post_t* p = &ZONE_POSTS[k % n];
        item_t* x = &s_items[s_item_count++];
        snprintf(x->tid, sizeof(x->tid), "65%06d", 300100 + k * 211);
        snprintf(x->title, sizeof(x->title), "%s", p->title);
        snprintf(x->meta, sizeof(x->meta), "%s · %s · %d 回复", p->user, p->when, p->replies);
        x->jpg[0] = 0;
    }
    s_list_page = 0;
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

    /* ---- 设计文档 / Design doc ---- */

    // ㉕ 推荐。/ ㉕ Feed.
    demo_home(SEG_FEED);
    demo_feed();
    hr_allow_fit();
    demo_shot(&ctx, "ui40");

    // ㉖ 帖子正文（帖子里共 3 张图）。/ ㉖ Post body (three images in the post).
    s_view = V_POST;
    s_post_back = V_HOME;
    demo_post();
    demo_post_more_images();
    demo_lights();
    s_tab = T_BODY;
    s_page = 0;
    hr_no_bar_check();
    hr_allow_fit();
    demo_shot(&ctx, "ui41");

    // ㉗ 亮了。/ ㉗ Lit replies.
    s_tab = T_LIGHTS;
    s_page = 0;
    hr_no_bar_check();
    hr_allow_fit();
    demo_shot(&ctx, "ui42");

    // ㉘ 看大图。/ ㉘ Image view.
    s_view = V_IMAGE;
    s_image = 0;
    hr_no_bar_check();
    demo_shot(&ctx, "ui43");

    // ㉙ 关注。/ ㉙ Following.
    demo_home(SEG_FOLLOW);
    demo_follow();
    hr_allow_fit();
    demo_shot(&ctx, "ui44");

    // ㉚ 全部专区。/ ㉚ All zones.
    s_view = V_ZONES;
    s_msg[0] = 0;
    demo_zones();
    hr_allow_fit();
    demo_shot(&ctx, "ui45");

    // ㉛ 专区帖子。/ ㉛ Zone posts.
    s_view = V_ZONE;
    s_msg[0] = 0;
    demo_zone();
    hr_allow_fit();
    demo_shot(&ctx, "ui46");

    // ㉜ 比分。/ ㉜ Scores.
    demo_home(SEG_SCORE);
    demo_schedule();
    hr_allow_fit();
    demo_shot(&ctx, "ui47");

    // ㉝ 比赛评分。/ ㉝ Match ratings.
    s_view = V_RATE;
    demo_rating();
    hr_no_bar_check();
    hr_allow_fit();
    demo_shot(&ctx, "ui48");
}
