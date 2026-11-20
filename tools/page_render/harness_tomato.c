/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：番茄书架、搜索键盘、搜索结果、书籍详情和下载进度的渲染测试。
 * English: Render bench for the Tomato shelf, search keyboard, results, book detail and download progress.
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "app_tr_tomato.c"   // 按 -I 找：当前源码或 SRC_ROOT / Found via -I: current sources or SRC_ROOT
#include "page_render.h"

const char* hr_page = "tomato";

/* ---- 后台任务的替身 / Worker stand-ins ---- */

static bool s_hr_busy;
bool tomato_sync_busy(void) { return s_hr_busy; }
uint32_t tomato_sync_seq(void) { return 0; }

static tomato_book_t s_hr_shelf[TOMATO_SHELF_MAX];

static void hr_shelf(int n, bool pending) {
    static const char* const T[] = {"诡秘之主：一个特别特别长的书名用来测试截断和徽章的位置", "大奉打更人", "道诡异仙",
                                    "我在精神病院学斩神", "十日终焉", "宿命之环", "夜的命名术", "深海余烬"};
    static const char* const A[] = {"爱潜水的乌贼", "卖报小郎君", "狐尾的笔", "三九音域", "杀虫队队员", "爱潜水的乌贼",
                                    "会说话的肘子", "远瞳"};
    memset(s_hr_shelf, 0, sizeof(s_hr_shelf));
    for (int i = 0; i < n; i++) {
        tomato_book_t* b = &s_hr_shelf[i];
        snprintf(b->book_id, sizeof(b->book_id), "%d", 1000 + i);
        snprintf(b->title, sizeof(b->title), "%s", T[i % 8]);
        snprintf(b->author, sizeof(b->author), "%s", A[i % 8]);
        b->chapters = (uint16_t)(300 + i * 37);
        b->seen = b->chapters;
        b->remote = b->chapters;
        if (pending && i == 0) b->remote = b->chapters + 12;
        if (pending && i == 1) b->remote = b->chapters + 1;
        if (i == 2) b->seen = b->chapters - 3;
        snprintf(b->file, sizeof(b->file), "%d.txt", 1000 + i);
        s_order[i] = (uint8_t)i;
        s_pct[i] = (int8_t)(i == 3 ? -1 : i == 4 ? 100 : i * 9);
    }
    s_shelf = s_hr_shelf;
    s_count = n;
    s_pending = 0;
    for (int i = 0; i < n; i++) s_pending += tomato_book_pending(&s_hr_shelf[i]) > 0;
    s_has_config = true;
    s_page = 0;
}

static void hr_hits(int n) {
    static const char* const T[] = {"十日终焉", "十日终焉之后：一个很长很长的同人书名测试截断", "终焉", "十日", "日终焉",
                                    "终焉之后", "十日谈"};
    memset(s_hits, 0, sizeof(s_hits));
    for (int i = 0; i < n; i++) {
        tomato_hit_t* h = &s_hits[i];
        snprintf(h->book_id, sizeof(h->book_id), "%d", i == 0 ? 1004 : 2000 + i);
        snprintf(h->title, sizeof(h->title), "%s", T[i % 7]);
        snprintf(h->author, sizeof(h->author), "%s", i == 1 ? "名字特别长的作者用来测试截断" : "杀虫队队员");
        snprintf(h->category, sizeof(h->category), "%s", i % 3 ? "悬疑脑洞" : "");
        h->chapters = (uint16_t)(120 + i * 50);
        h->words = (uint32_t)(i % 2 ? 2540000 : 8000);
        h->finished = i % 2;
        snprintf(h->abstract, sizeof(h->abstract), "%s",
                 "陈陈陈陈陈醒来发现自己身处一座诡异的城市，十天之后所有人都会死去。为了活下去，他必须在一场场生死游戏里找到"
                 "真相，而每一次死亡都会让记忆重新开始。这是一段很长的简介，用来看四行以后有没有被正确截断。");
    }
    s_hit_count = n;
    s_result_page = 0;
}

static void hr_reset(void) {
    s_view = V_LIST;
    s_op = s_last = OP_NONE;
    s_hr_busy = false;
#ifndef HR_BASELINE
    s_dl_id[0] = 0;
#endif
    memset(&s_status, 0, sizeof(s_status));
    memset(s_recent, 0, sizeof(s_recent));
    s_query[0] = 0;
}

static void hr_render(const char* scene) {
    app_ctx_t ctx = {.fb = hr_fb};
    render(&ctx, hr_fb);
    hr_shot(scene);
}

void hr_scenes(void) {
    hr_reset();
    s_count = 0;
    s_has_config = false;
    hr_render("list_no_config");

    hr_reset();
    s_count = 0;
    s_has_config = true;
    hr_render("list_empty");

    hr_reset();
    hr_shelf(20, true);
    hr_allow_fit();
    hr_render("list_pending");

    hr_reset();
    hr_shelf(20, false);
    hr_allow_fit();
    hr_render("list_plain");

    hr_reset();
    hr_shelf(20, false);
    s_page = 2;
    hr_allow_fit();
    hr_render("list_last_page");

    hr_reset();
    hr_shelf(20, true);
    s_last = OP_CHECK;   // 只显示本页任务的结果 / Only our own op's result is shown
    s_status.phase = TOMATO_FAILED;
    snprintf(s_status.message, sizeof(s_status.message), "%s",
             "检查失败：连接服务器超时，请确认服务器已启动且在同一网络里");
    hr_allow_fit();
    hr_render("list_failed");

    hr_reset();
    hr_shelf(20, true);
    s_hr_busy = true;
    s_op = OP_UPDATE;
    s_status.phase = TOMATO_DOWNLOADING;
    s_status.current = 2;
    s_status.total = 5;
    s_status.bytes = 3456789;
    snprintf(s_status.message, sizeof(s_status.message), "%s", "诡秘之主：一个特别特别长的书名用来测试截断");
    hr_allow_fit();
    hr_render("list_updating");

    hr_reset();
    hr_shelf(20, false);
    s_hr_busy = true;
    s_op = OP_CHECK;
    s_status.phase = TOMATO_QUEUED;
    snprintf(s_status.message, sizeof(s_status.message), "%s", "正在检查");
    hr_allow_fit();
    hr_render("list_checking");

    // 搜索：键盘有自己的测试，这里看输入框和最近搜索。/ Search: the keyboard has its own bench; this checks the field
    // and recent searches.
    hr_reset();
    snprintf(s_recent[0], sizeof(s_recent[0]), "%s", "十日终焉");
    snprintf(s_recent[1], sizeof(s_recent[1]), "%s", "诡秘之主");
    snprintf(s_recent[2], sizeof(s_recent[2]), "%s", "我在精神病院学斩神");
    memset(&s_kbd, 0, sizeof(s_kbd));
    s_kbd.mode = s_kbd.letters = TR_KBD_PINYIN;
    s_kbd.ime_ok = s_kbd.want_ime = true;
    s_kbd.go_label = "搜索";
    snprintf(s_kbd.text, sizeof(s_kbd.text), "%s", "十日");
    s_view = V_SEARCH;
    hr_no_bar_check();
    hr_render("search");

    hr_reset();
    memset(&s_kbd, 0, sizeof(s_kbd));
    s_kbd.mode = s_kbd.letters = TR_KBD_ENGLISH;
    s_kbd.go_label = "搜索";
    snprintf(s_kbd.text, sizeof(s_kbd.text), "%s", "一个特别特别长的搜索词用来测试输入框只显示末尾的效果abcdefg");
    s_view = V_SEARCH;
    hr_no_bar_check();
    hr_render("search_long");

    hr_reset();
    hr_shelf(8, false);
    hr_hits(12);
    snprintf(s_query, sizeof(s_query), "%s", "十日终焉");
    s_view = V_RESULTS;
    hr_allow_fit();
    hr_render("results");

    hr_reset();
    hr_shelf(8, false);
    hr_hits(12);
    s_result_page = 2;
    hr_allow_fit();
    snprintf(s_query, sizeof(s_query), "%s", "十日终焉");
    s_view = V_RESULTS;
    hr_render("results_last_page");

    hr_reset();
    s_hit_count = 0;
    s_hr_busy = true;
    s_op = OP_SEARCH;
    s_status.phase = TOMATO_CONNECTING;
    snprintf(s_query, sizeof(s_query), "%s", "十日终焉");
    s_view = V_RESULTS;
    hr_render("results_searching");

    hr_reset();
    s_hit_count = 0;
    s_last = OP_SEARCH;
    s_status.phase = TOMATO_FAILED;
    snprintf(s_status.message, sizeof(s_status.message), "%s",
             "搜索失败：服务器返回 502，番茄接口暂时不可用，请稍后再试");
    snprintf(s_query, sizeof(s_query), "%s", "十日终焉");
    s_view = V_RESULTS;
    hr_render("results_failed");

    hr_reset();
    hr_shelf(8, false);
    hr_hits(12);
    s_detail = 1;
    s_view = V_DETAIL;
    hr_allow_fit();
    hr_render("detail");

    hr_reset();
    hr_shelf(8, false);
    hr_hits(12);
    s_detail = 0;
    s_view = V_DETAIL;
    hr_render("detail_owned");

    hr_reset();
    hr_shelf(8, false);
    hr_hits(12);
    s_detail = 2;
    s_view = V_DETAIL;
    s_hr_busy = true;
    s_op = OP_DOWNLOAD;
#ifndef HR_BASELINE
    snprintf(s_dl_id, sizeof(s_dl_id), "%s", s_hits[s_detail].book_id);   // 下载的就是这本 / This is the downloading book
#endif
    s_status.phase = TOMATO_DOWNLOADING;
    s_status.bytes = 1234567;
    hr_render("detail_downloading");

    hr_reset();
    hr_shelf(8, false);
    hr_hits(12);
    s_detail = 2;
    s_view = V_DETAIL;
    s_last = OP_DOWNLOAD;
#ifndef HR_BASELINE
    snprintf(s_dl_id, sizeof(s_dl_id), "%s", s_hits[s_detail].book_id);   // 下载的就是这本 / This is the downloading book
#endif
    s_status.phase = TOMATO_FAILED;
    snprintf(s_status.message, sizeof(s_status.message), "%s",
             "下载失败：服务器生成 TXT 超时，可能被限流，过几分钟再试");
    hr_render("detail_failed");
#ifndef HR_BASELINE
    // 另一本在下时点“下载到书库”：说原因。/ Tapping "下载到书库" while another book downloads explains why.
    hr_reset();
    hr_shelf(8, false);
    hr_hits(12);
    s_detail = 3;
    s_view = V_DETAIL;
    snprintf(s_detail_note, sizeof(s_detail_note), "%s", "正在下载另一本，下完再点");
    hr_render("detail_busy_note");
#endif
}
