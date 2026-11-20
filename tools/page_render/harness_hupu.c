/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：虎扑页的渲染测试。/ English: Render bench for the Hupu page.
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "app_tr_hupu.c"   // 按 -I 找：当前源码或 SRC_ROOT / Found via -I: current sources or SRC_ROOT
#include "page_render.h"

const char* hr_page = "hupu";

/* ---- 替身 / Stand-ins ---- */

static bool s_mock_busy;
bool tomato_sync_busy(void) { return s_mock_busy; }
uint32_t tomato_sync_seq(void) { return 7; }
bool tomato_sync_preemptible(void) { return true; }
void tomato_sync_cancel(void) {}
void hupu_img_drop_pending(void) {}
void hupu_img_release(void) {}
bool hupu_img_pending(void) { return false; }
bool hupu_img_tick(void) { return false; }
void tomato_json_use_psram(void) {}

// 以 "ok" 开头的文件当作已解码，其余没有。/ Files starting with "ok" count as decoded; others are missing.
static uint8_t s_px[1];
const uint8_t* hupu_img_get(const char* jpg, int w, int h) {
    (void)w; (void)h;
    return jpg && !strncmp(jpg, "ok", 2) ? s_px : NULL;
}
void hupu_img_draw(uint8_t* fb, int x, int y, int w, int h, const uint8_t* px, bool border) {
    (void)px;
    epd_fill_rect((EpdRect){x, y, w, h}, 0x80, fb);
    if (border) epd_draw_rect((EpdRect){x, y, w, h}, 0x00, fb);
}

/* ---- 数据 / Data ---- */

static const char* LONG_TITLE = "詹姆斯赛后谈到球队最近的状态：我们还需要更多的时间去磨合，但大家都很有信心，季后赛见分晓吧兄弟们";

static void fill_items(int n, bool thumbs) {
    s_item_count = 0;
    for (int i = 0; i < n; i++) {
        item_t* x = &s_items[s_item_count++];
        snprintf(x->tid, sizeof(x->tid), "%d", 1000 + i);
        snprintf(x->title, sizeof(x->title), "%s", i % 2 ? LONG_TITLE : "湖人险胜勇士，詹姆斯砍下 30 分");
        snprintf(x->meta, sizeof(x->meta), "%s", i % 2 ? "湿乎乎的话题 · 1234 回复" : "篮球场边的一位非常长名字的老哥 · 刚刚 · 99999 回复");
        if (thumbs && i % 3 != 2) snprintf(x->jpg, sizeof(x->jpg), "%s", i % 3 ? "missing.jpg" : "ok.jpg");
        else x->jpg[0] = 0;
    }
}

static void home(seg_t seg) {
    s_view = V_HOME;
    s_seg = seg;
    s_msg[0] = 0;
    s_list_page = 0;
    s_job_active = false;
    s_has_pending = false;
    s_mock_busy = false;
}

static void shot(app_ctx_t* ctx, const char* name) {
    render(ctx, hr_fb);
    hr_shot(name);
}

static void fill_schedule(void) {
    s_day_count = 2;
    snprintf(s_days[0].label, sizeof(s_days[0].label), "11月20日 周四");
    snprintf(s_days[1].label, sizeof(s_days[1].label), "11月21日 周五 今天");
    s_days[0].first = 0; s_days[0].count = 0;
    s_days[1].first = 0; s_days[1].count = 8;
    s_match_count = 8;
    for (int i = 0; i < 8; i++) {
        match_t* m = &s_matches[i];
        memset(m, 0, sizeof(*m));
        snprintf(m->a, sizeof(m->a), "%s", i % 2 ? "金州勇士队伍" : "湖人");
        snprintf(m->b, sizeof(m->b), "%s", i % 2 ? "明尼苏达森林狼" : "凯尔特人");
        m->st = (uint8_t)(i % 3);
        if (m->st) { snprintf(m->as, sizeof(m->as), "112"); snprintf(m->bs, sizeof(m->bs), "108"); }
        snprintf(m->desc, sizeof(m->desc), "%s", m->st == 1 ? "第四节 02:31" : "已结束");
        snprintf(m->count, sizeof(m->count), "12.3万人评分");
        snprintf(m->k, sizeof(m->k), "k%d", i);
        snprintf(m->n, sizeof(m->n), "n%d", i);
        snprintf(m->ts, sizeof(m->ts), "1763700000000");
        snprintf(m->ai, sizeof(m->ai), "A");
        snprintf(m->bi, sizeof(m->bi), "B");
    }
    s_day = 1;
    s_day_page = 0;
}

static void fill_post(void) {
    free_post();
    s_arena_cap = 1;
    s_arena = malloc(1);
    s_title = LONG_TITLE;
    s_author = "篮球老哥";
    s_time = "11-21 10:32";
    s_zone = "湿乎乎的话题";
    s_reply_total = 512;
    static const char* para =
        "昨晚这场比赛真的太精彩了，第四节最后两分钟双方来回拉锯，每一个回合都让人屏住呼吸。"
        "詹姆斯在关键时刻的突破分球，还有库里那记超远三分，都足以载入本赛季的集锦。\n"
        "赛后采访里，主教练也提到了球队防守端的进步，认为这是赢球的关键。";
    s_block_count = 0;
    for (int i = 0; i < 6; i++) {
        block_t* b = &s_blocks[s_block_count++];
        memset(b, 0, sizeof(*b));
        if (i == 1 || i == 4) {
            b->image = true;
            snprintf(b->jpg, sizeof(b->jpg), "%s", i == 1 ? "ok.jpg" : "missing.jpg");
            b->w = CW;
            b->h = 380;
        } else {
            b->text = para;
        }
    }
    static const char* texts[] = {
        "说得太对了，这场比赛就是看防守。",
        "我觉得还是要看替补的发挥，首发打得再好替补一上来就崩也没用啊，上赛季就是这样，季后赛直接被横扫，"
        "希望今年能有所改变吧，不然真的看不下去了。再说一句，教练的轮换也很迷，关键时刻总是让那几个人上场。",
        "",
    };
    s_light_count = s_list_count = 7;
    for (int i = 0; i < 7; i++) {
        reply_t* r = &s_lights[i];
        r->user = i % 2 ? "一个名字非常非常长的虎扑老用户而且还要更长一点" : "JR小明";
        r->quote = i % 3 == 1 ? "楼上说的不对，明明是进攻端的问题，看看命中率就知道了" : NULL;
        r->text = texts[i % 3];
        r->lights = i % 3 == 2 ? 0 : 1234 * (i + 1);
        s_list[i] = *r;
    }
    s_tid[0] = '1';
    s_tid[1] = 0;
    s_title_lines = title_lines();
    layout_body();
    layout_replies(0);
    layout_replies(1);
    s_post_loaded = true;
}

static void fill_rating(void) {
    s_rate_match = 1;
    s_player_count = 16;
    for (int i = 0; i < 16; i++) {
        player_t* p = &s_players[i];
        memset(p, 0, sizeof(*p));
        snprintf(p->name, sizeof(p->name), "%s", i % 2 ? "安东尼·爱德华兹-超长名字" : "库里");
        snprintf(p->team, sizeof(p->team), "%s", i < 8 ? "A" : "B");
        snprintf(p->type, sizeof(p->type), "%s", i == 7 ? "coach" : "player");
        snprintf(p->min, sizeof(p->min), "%d分钟", 30 + i);
        snprintf(p->stats, sizeof(p->stats), "%s", i % 2 ? "28分 7篮板 9助攻 3抢断" : "12分");
        p->score = 9.5f - i * 0.3f;
        p->count = i % 2 ? 123456 : 999;
    }
    s_rate_team = 1;
    s_rate_page = 0;
}

void hr_scenes(void) {
    app_ctx_t ctx = {.fb = hr_fb};
    s_seg = SEG_FEED;
    alloc_all();

    // 推荐 / Feed
    home(SEG_FEED);
    fill_items(12, true);
    hr_allow_fit();
    shot(&ctx, "feed");
    s_list_page = 2;
    hr_allow_fit();
    shot(&ctx, "feed_last_page");
    s_item_count = 0;
    home(SEG_FEED);
    shot(&ctx, "feed_empty");
    s_job_active = true;
    s_job_seq = 7;
    s_job.job = TOMATO_HUPU_HOME;
    s_mock_busy = true;
    shot(&ctx, "feed_loading");
    home(SEG_FEED);
    snprintf(s_msg, sizeof(s_msg), "网络连接失败：请检查 Wi-Fi 设置后再点刷新重新加载");
    fill_items(5, true);
    hr_allow_fit();
    shot(&ctx, "feed_error");

    // 关注 / Following
    home(SEG_FOLLOW);
    s_follow_count = 0;
    s_item_count = 0;
    shot(&ctx, "follow_none");
    s_follow_count = 4;
    for (int i = 0; i < 4; i++) {
        snprintf(s_follows[i].id, sizeof(s_follows[i].id), "%d", i);
        snprintf(s_follows[i].name, sizeof(s_follows[i].name), "%s", i % 2 ? "湿乎乎的话题" : "湖人专区");
    }
    hr_allow_fit();   // 专区名清单是内容 / The zone-name list is content
    shot(&ctx, "follow_empty");
    fill_items(9, false);
    hr_allow_fit();
    shot(&ctx, "follow");

    // 比分 / Scores
    home(SEG_SCORE);
    s_day_count = 0;
    shot(&ctx, "scores_empty");
    fill_schedule();
    hr_allow_fit();
    shot(&ctx, "scores");
    s_day = 0;
    shot(&ctx, "scores_no_match");
    s_day = 1;
    s_day_page = 1;
    hr_allow_fit();
    shot(&ctx, "scores_page2");

    // 评分 / Ratings
    s_view = V_RATE;
    fill_rating();
    hr_no_bar_check();
    hr_allow_fit();
    shot(&ctx, "rate");
    s_rate_team = 0;
    s_rate_page = 1;
    hr_no_bar_check();
    hr_allow_fit();
    shot(&ctx, "rate_all");
    s_player_count = 0;
    hr_no_bar_check();
    hr_allow_fit();
    shot(&ctx, "rate_empty");

    // 专区 / Zones
    s_view = V_ZONES;
    s_msg[0] = 0;
    s_zone_count = 0;
    s_cat_count = 0;
    shot(&ctx, "zones_empty");
    static const char* cats[] = {"热门", "篮球", "足球", "电竞游戏", "数码", "影视娱乐", "生活", "体育综合", "汽车", "其他"};
    s_cat_count = 10;
    s_zone_count = 0;
    for (int c = 0; c < 10; c++) {
        snprintf(s_cats[c].name, sizeof(s_cats[c].name), "%s", cats[c]);
        s_cats[c].first = (int16_t)s_zone_count;
        const int n = c == 0 ? 40 : 6;
        for (int z = 0; z < n; z++) {
            snprintf(s_zones[s_zone_count].id, sizeof(s_zones[0].id), "%d", s_zone_count);
            snprintf(s_zones[s_zone_count].name, sizeof(s_zones[0].name), "%s", z % 4 == 1 ? "湿乎乎的话题专区" : z % 4 == 2 ? "NBA" : "湖人专区");
            s_zone_count++;
        }
        s_cats[c].count = (int16_t)n;
    }
    snprintf(s_follows[0].id, sizeof(s_follows[0].id), "1");
    s_cat = 0;
    s_zone_page = 0;
    hr_allow_fit();
    shot(&ctx, "zones");

    // 专区帖子 / Zone posts
    s_view = V_ZONE;
    snprintf(s_zone_id, sizeof(s_zone_id), "1");
    snprintf(s_zone_name, sizeof(s_zone_name), "湿乎乎的话题专区名字很长很长很长");
    s_zone_sort = 0;
    s_list_end = false;
    fill_items(10, false);
    s_list_page = 0;
    snprintf(s_msg, sizeof(s_msg), "加载失败，稍后再试");
    hr_allow_fit();
    shot(&ctx, "zone");
    s_msg[0] = 0;
    snprintf(s_zone_id, sizeof(s_zone_id), "99");
    s_zone_sort = 1;
    s_list_page = 2;
    hr_allow_fit();
    shot(&ctx, "zone_more");

    // 帖子 / Post
    s_view = V_POST;
    s_post_loaded = false;
    s_msg[0] = 0;
    hr_no_bar_check();
    shot(&ctx, "post_empty");
    fill_post();
    s_tab = T_BODY;
    s_page = 0;
    hr_no_bar_check();
    hr_allow_fit();
    shot(&ctx, "post_body");
    s_page = 1;
    hr_no_bar_check();
    hr_allow_fit();
    shot(&ctx, "post_body2");
    s_tab = T_LIGHTS;
    s_page = 0;
    hr_no_bar_check();
    hr_allow_fit();
    shot(&ctx, "post_lights");
    s_tab = T_LIST;
    s_page = s_reply_page_count[1] - 1;
    hr_no_bar_check();
    hr_allow_fit();
    shot(&ctx, "post_list_last");
    s_view = V_REPLY;
    s_reply_open = 1;
    hr_no_bar_check();
    hr_allow_fit();
    shot(&ctx, "reply_full");
    s_view = V_IMAGE;
    s_image = 0;
    hr_no_bar_check();
    shot(&ctx, "image");
    s_image = 1;
    hr_no_bar_check();
    shot(&ctx, "image_missing");

    // 内存不足 / Out of memory
    s_view = V_HOME;
    item_t* keep_items = s_items;
    s_items = NULL;
    shot(&ctx, "oom");
    s_items = keep_items;
}
