/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：番茄搜索键盘、搜索结果和书籍详情的演示截图（README / 网站预览图和设计文档用），内容像真实用户的设备。
 * English: Demo shots of the Tomato search keyboard, results and book detail (README / site previews and the design
 * doc), filled like a real user's device.
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "app_tr_tomato.c"   // 按 -I 找 / Found via -I
#include "page_render.h"

const char* hr_page = "demo_tomato";

/* ---- 后台任务的替身 / Worker stand-ins ---- */

bool tomato_sync_busy(void) { return false; }
uint32_t tomato_sync_seq(void) { return 0; }

/* ---- 书库里的番茄书 / Tomato books in the library ---- */

// 第一个搜索结果在书库里（“开始阅读”）。/ The first hit is already in the library ("开始阅读").
static tomato_book_t s_demo_shelf[1];

static void demo_shelf(void) {
    memset(s_demo_shelf, 0, sizeof(s_demo_shelf));
    snprintf(s_demo_shelf[0].book_id, sizeof(s_demo_shelf[0].book_id), "7%09d", 213450);
    snprintf(s_demo_shelf[0].title, sizeof(s_demo_shelf[0].title), "%s", "宿命之环");
    snprintf(s_demo_shelf[0].file, sizeof(s_demo_shelf[0].file), "%s", "宿命之环.txt");
    s_shelf = s_demo_shelf;
    s_count = 1;
}

/* ---- 搜索 / Search ---- */

typedef struct {
    const char* title;
    const char* author;
    const char* category;
    int chapters;
    int words_wan;   // 万字 / Ten-thousand characters
    bool finished;
    const char* intro;
} demo_hit_t;

static const demo_hit_t HITS[] = {
    {"宿命之环", "爱潜水的乌贼", "西方奇幻", 1186, 312, false,
     "一场意外让卢米安失去了一切，他在混乱的特里尔街头挣扎求生，追查那场诡异仪式背后的真相。"},
    {"宿命回廊", "青山不改", "悬疑脑洞", 640, 186, true,
     "一座没有出口的回廊，每扇门后都是另一个人的命运。他必须在第七次轮回之前找到写下这一切的人，"
     "否则所有走进回廊的人都会被永远困在同一天。线索藏在每一段陌生的人生里，而他也渐渐发现，"
     "自己并不是第一个闯进来的人。"},
    {"宿命之子", "北风辞", "东方仙侠", 302, 88, false, "山村少年背着一柄锈剑出门求学，一路斩妖除魔。"},
    {"宿命轮回之都市归来", "南山有雪", "都市高武", 856, 241, true, "百世轮回之后，他回到了一切开始的那座小城。"},
    {"宿命交织的夏天", "林间晚风", "青春甜宠", 128, 36, true, "那个夏天，她在图书馆遇见了总是借同一本书的少年。"},
    {"宿命星图", "长河落日", "科幻末世", 512, 152, false, "星图碎片散落在废土各处，拼出它的人将改写人类的命运。"},
    {"宿命之敌", "白鹭洲", "历史古代", 433, 119, true, "两个出身寒门的少年同年中举，从此一生互为对手。"},
};
#define HIT_N ((int)(sizeof(HITS) / sizeof(HITS[0])))

static void demo_hits(void) {
    memset(s_hits, 0, sizeof(s_hits));
    for (int i = 0; i < HIT_N; i++) {
        tomato_hit_t* h = &s_hits[i];
        const demo_hit_t* d = &HITS[i];
        // 第一本就是书库里的宿命之环。/ The first hit is 宿命之环 from the library.
        if (i == 0) snprintf(h->book_id, sizeof(h->book_id), "%s", s_demo_shelf[0].book_id);
        else snprintf(h->book_id, sizeof(h->book_id), "72%08d", 5531 + i * 20011);
        snprintf(h->title, sizeof(h->title), "%s", d->title);
        snprintf(h->author, sizeof(h->author), "%s", d->author);
        snprintf(h->category, sizeof(h->category), "%s", d->category);
        h->chapters = (uint16_t)d->chapters;
        h->words = (uint32_t)d->words_wan * 10000u;
        h->finished = d->finished;
        snprintf(h->abstract, sizeof(h->abstract), "%s", d->intro);
    }
    s_hit_count = HIT_N;
    s_result_page = 0;
}

static void demo_reset(void) {
    s_view = V_SEARCH;
    s_op = s_last = OP_NONE;
    memset(&s_status, 0, sizeof(s_status));
    s_detail_note[0] = 0;
    s_dl_id[0] = 0;
    memset(s_recent, 0, sizeof(s_recent));
    s_query[0] = 0;
    s_hit_count = 0;
    demo_shelf();
}

static void demo_shot(const char* scene) {
    app_ctx_t ctx = {.fb = hr_fb};
    render(&ctx, hr_fb);
    hr_shot(scene);
}

void hr_scenes(void) {
    // ⑲ 搜索：已经输入“宿命”，正在拼“zhihuan”。/ ⑲ Search: "宿命" typed, "zhihuan" being composed.
    demo_reset();
    snprintf(s_recent[0], sizeof(s_recent[0]), "%s", "十日终焉");
    snprintf(s_recent[1], sizeof(s_recent[1]), "%s", "诡秘之主");
    snprintf(s_recent[2], sizeof(s_recent[2]), "%s", "乌贼");
    memset(&s_kbd, 0, sizeof(s_kbd));
    s_kbd.mode = s_kbd.letters = TR_KBD_PINYIN;
    s_kbd.ime_ok = s_kbd.want_ime = true;
    s_kbd.go_label = "搜索";
    snprintf(s_kbd.text, sizeof(s_kbd.text), "%s", "宿命");
    {
        static const char* const W[] = {"之环", "指环", "纸环", "智慧", "之", "只", "知", "支", "指", "纸", "汁", "枝"};
        snprintf(s_kbd.comp, sizeof(s_kbd.comp), "%s", "zhihuan");
        s_kbd.cand_count = 12;
        for (int i = 0; i < 12; i++) {
            snprintf(s_kbd.cands[i].text, sizeof(s_kbd.cands[i].text), "%s", W[i]);
            s_kbd.cands[i].consumed = (uint8_t)(i < 4 ? 7 : 3);
        }
    }
    s_view = V_SEARCH;
    hr_no_bar_check();
    hr_allow_fit();
    demo_shot("ui23");

    // ⑳ 搜索结果。/ ⑳ Search results.
    demo_reset();
    demo_hits();
    snprintf(s_query, sizeof(s_query), "%s", "宿命");
    s_view = V_RESULTS;
    hr_allow_fit();
    demo_shot("ui24");

    // ㉑ 搜索详情：还没下载的书。/ ㉑ Search detail: a book not downloaded yet.
    demo_reset();
    demo_hits();
    snprintf(s_query, sizeof(s_query), "%s", "宿命");
    s_detail = 1;
    s_view = V_DETAIL;
    hr_allow_fit();
    demo_shot("ui25");
}
