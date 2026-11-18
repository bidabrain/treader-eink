/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 虎扑标签页（只读，移植自安卓版 hupuX）：顶部分段 推荐 / 关注 / 比分；全部专区和专区帖子；帖子正文、亮了、回复、
 * 看大图；比赛评分。数据由 components/tomato 的后台任务取到 TF 卡 hupu/，本页只读缓存；图片缩放与抖动在 hupu_img。
 *
 * English: the Hupu tab (read only, ported from hupuX): Feed / Following / Scores segments; all zones and zone posts;
 * post body, lit replies, replies and image view; match ratings. The components/tomato task fetches data into hupu/ on
 * the TF card and this page reads that cache; hupu_img scales and dithers images.
 *
 * 冻结：只在用户点“刷新”、翻到没取过的一页、或打开没缓存的内容（帖子、专区、关注流、赛程、评分）时联网，
 * 进页不自动刷新；不联网时照常显示缓存，没缓存就提示联网后刷新。列表和帖子都按页显示，不滚动。
 * 大块数据（列表、专区、赛程、帖子）进页分配、离页全部释放，cJSON 树读完即删。不登录、不点亮、不回复。
 * Frozen: network only on Refresh, turning to a page not fetched yet, or opening uncached content (post, zone,
 * following feed, schedule, ratings); entering never refreshes. Offline, caches show as usual and missing caches ask
 * for a refresh once online. Lists and posts page, never scroll. Large buffers are allocated on entry and all freed
 * on exit; cJSON trees are deleted right after reading. No login, lighting or replying.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "app.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "hupu_img.h"
#include "tomato.h"
#include "tr_nav.h"
#include "tr_time.h"
#include "tr_ui.h"
#include "ttf_font.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"

#define TAG "app_hupu"
#define POLL_MS 400
#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define CW (TR_W - 2 * TR_MARGIN)

// 列表（推荐、关注、专区帖子共用）/ Lists (feed, following and zone posts share them)
#define LIST_MAX 200
#define LIST_TOP 284
#define ZONE_LIST_TOP 248
#define ROW_H 148
#define ROWS 5
#define PAGER_Y 1040
#define THUMB_W 150
#define THUMB_H 100
#define THUMB_X (TR_W - TR_MARGIN - THUMB_W)
#define STATUS_Y 240
#define REFRESH_RECT (EpdRect){TR_W - TR_MARGIN - 200, 70, 200, 80}

// 专区 / Zones
#define ZONES_MAX 320
#define CATS_MAX 20
#define FOLLOWS_MAX 12   // 和关注流一次取的专区数一致 / Matches the zones the follow feed reads
#define CELL_W 192
#define CELL_H 88
#define CELL_STEP_X 206
#define CELL_STEP_Y 102

// 帖子 / Post
#define POST_SEG (EpdRect){TR_MARGIN, 150, CW, 64}
#define TITLE_Y 240
#define TITLE_PX 38
#define TITLE_LH 50
#define BODY_TOP 240
#define BODY_BOTTOM 1100
#define BODY_PX 32
#define BODY_LH 52
#define PARA_GAP 18
#define IMAGE_MAX_H 680         // 第一页标题占两行时也放得下 / Fits page one even with a two-line title
#define POST_PAGER_Y 1124
#define BLOCKS_MAX 240
#define REPLIES_MAX 60
#define LINES_MAX 3000
#define PAGES_MAX 400
#define REPLY_LINES 4
#define REPLY_PX 30
#define REPLY_LH 44
#define IMAGE_BAND 96           // 看大图底部的白条 / White strip under the full-screen image

// 比分 / Scores
#define DAYS_MAX 90
#define MATCHES_MAX 420
#define CHIP_Y 244
#define DATE_Y 320
#define MATCH_TOP 396
#define MATCH_H 124
#define PLAYERS_MAX 70
#define RATE_TOP 300
#define RATE_H 112
#define RATE_ROWS 7

typedef enum { V_HOME, V_ZONES, V_ZONE, V_POST, V_REPLY, V_IMAGE, V_RATE } view_t;
typedef enum { SEG_FEED, SEG_FOLLOW, SEG_SCORE } seg_t;
typedef enum { T_BODY, T_LIGHTS, T_LIST } post_tab_t;

typedef struct {
    char tid[24];
    char title[200];
    char meta[96];       // 列表第二行，读入时拼好 / Second line, built when loading
    char jpg[64];        // 缩略图本地文件，空表示没有 / Local thumbnail; empty when none
} item_t;

typedef struct {
    char id[16];
    char name[48];
} zone_t;

typedef struct {
    char name[32];
    int16_t first, count;
} cat_t;

typedef struct {
    bool image;
    const char* text;
    char jpg[64];
    int16_t w, h;
} block_t;

typedef struct {
    const char* user;
    const char* quote;
    const char* text;
    int lights;
} reply_t;

typedef struct {
    uint16_t block;
    uint32_t off;
    uint16_t len;
    int16_t h;
    uint8_t kind;        // 0 文字行 1 图片 2 段间距 / 0 text line, 1 image, 2 paragraph gap
} line_t;

typedef struct {
    char label[32];
    int16_t first, count;
} day_t;

typedef struct {
    char a[24], b[24], as[8], bs[8], ai[24], bi[24];
    char desc[24], count[24];
    char k[32], n[20];   // 评分条目键 / Rating item key
    char ts[16];
    uint8_t st;          // 0 未开始 1 进行中 2 已结束 / 0 upcoming, 1 live, 2 finished
} match_t;

typedef struct {
    char name[40], team[24], type[12], min[10], stats[40];
    float score;
    int count;
} player_t;

static const struct { const char* name; const char* id; } LEAGUES[] = {
    {"NBA", "nba"}, {"CBA", "cba"}, {"英超", "epl"}, {"LOL", "lol"}, {"网球", "tennis"},
};
#define LEAGUE_COUNT ((int)(sizeof(LEAGUES) / sizeof(LEAGUES[0])))

/* ---- 状态 / State ---- */

static view_t s_view;
static seg_t s_seg;
static char s_msg[96];
static int64_t s_poll_ms;

// 后台任务：s_job 有值表示本页发起的任务还没处理完。/ Background job: set while a job this page started is pending.
static bool s_job_active;
static uint32_t s_job_seq;            // 本页任务的序号，用来认出“是不是自己的”/ Our job's sequence number
static tomato_hupu_req_t s_job;
static tomato_status_t s_status;
static bool s_step_loaded;

// 列表 / List
static item_t* s_items;
static int s_item_count;
static int s_list_page;
static bool s_list_end;              // 专区没有更多了 / Zone has no more pages
static char s_zone_id[16], s_zone_name[48];
static int s_zone_sort;
static view_t s_zone_back;           // 专区帖子返回到哪 / Where zone posts return to

// 专区与关注 / Zones and follows
static zone_t* s_zones;
static int s_zone_count;
static cat_t s_cats[CATS_MAX];
static int s_cat_count, s_cat, s_zone_page;
static zone_t* s_follows;
static int s_follow_count;

// 帖子 / Post
static view_t s_post_back;
static char* s_arena;
static size_t s_arena_len, s_arena_cap;
static char s_tid[24];
static bool s_post_loaded;
static const char *s_title, *s_author, *s_time, *s_zone;
static int s_reply_total;
static block_t* s_blocks;
static int s_block_count;
static reply_t* s_lights;
static int s_light_count;
static reply_t* s_list;
static int s_list_count;
static line_t* s_lines;
static int s_line_count;
static uint16_t* s_body_pages;
static int s_body_page_count;
static int s_title_lines;
static uint8_t s_reply_pages[2][REPLIES_MAX + 1];
static int s_reply_page_count[2];
static post_tab_t s_tab;
static int s_page;
static int s_reply_open;
static int s_image;

// 比分 / Scores
static int s_league;
static day_t* s_days;
static int s_day_count, s_day, s_day_page;
static match_t* s_matches;
static int s_match_count;
static int s_rate_match;              // s_matches 里的下标 / Index into s_matches
static player_t* s_players;
static int s_player_count;
static char s_rate_title[96];
static int s_rate_team;               // 0 全部（分不出队时）1 主 2 客 / 0 all, 1 first team, 2 second team
static int s_rate_page;

/* ---- 内存：进页分配，离页释放 / Memory: allocated on entry, freed on exit ---- */

static void* alloc_zero(size_t n, size_t size) { return heap_caps_calloc(n, size, PSRAM_CAPS); }

#define FREE(p) do { heap_caps_free(p); (p) = NULL; } while (0)

static void free_post(void) {
    FREE(s_arena);
    s_arena_len = s_arena_cap = 0;
    s_post_loaded = false;
    s_block_count = s_light_count = s_list_count = s_line_count = 0;
    s_body_page_count = 0;
}

static void free_all(void) {
    free_post();
    FREE(s_items);
    FREE(s_zones);
    FREE(s_follows);
    FREE(s_blocks);
    FREE(s_lights);
    FREE(s_list);
    FREE(s_lines);
    FREE(s_body_pages);
    FREE(s_days);
    FREE(s_matches);
    FREE(s_players);
    s_item_count = s_zone_count = s_follow_count = s_cat_count = 0;
    s_day_count = s_match_count = s_player_count = 0;
    hupu_img_release();
}

static bool alloc_all(void) {
    if (!s_items) s_items = alloc_zero(LIST_MAX, sizeof(item_t));
    if (!s_zones) s_zones = alloc_zero(ZONES_MAX, sizeof(zone_t));
    if (!s_follows) s_follows = alloc_zero(FOLLOWS_MAX, sizeof(zone_t));
    if (!s_blocks) s_blocks = alloc_zero(BLOCKS_MAX, sizeof(block_t));
    if (!s_lights) s_lights = alloc_zero(REPLIES_MAX, sizeof(reply_t));
    if (!s_list) s_list = alloc_zero(REPLIES_MAX, sizeof(reply_t));
    if (!s_lines) s_lines = alloc_zero(LINES_MAX, sizeof(line_t));
    if (!s_body_pages) s_body_pages = alloc_zero(PAGES_MAX, sizeof(uint16_t));
    if (!s_days) s_days = alloc_zero(DAYS_MAX, sizeof(day_t));
    if (!s_matches) s_matches = alloc_zero(MATCHES_MAX, sizeof(match_t));
    if (!s_players) s_players = alloc_zero(PLAYERS_MAX, sizeof(player_t));
    const bool ok = s_items && s_zones && s_follows && s_blocks && s_lights && s_list && s_lines && s_body_pages &&
                    s_days && s_matches && s_players;
    if (!ok) { ESP_LOGE(TAG, "out of memory"); free_all(); }
    return ok;
}

/* ---- 文件 / Files ---- */

static cJSON* read_json(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = n > 0 && n < 2 * 1024 * 1024 ? heap_caps_malloc((size_t)n + 1, PSRAM_CAPS) : NULL;
    bool ok = buf && fread(buf, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    cJSON* root = NULL;
    if (ok) {
        buf[n] = 0;
        tomato_json_use_psram();
        root = cJSON_Parse(buf);
    }
    heap_caps_free(buf);
    return root;
}

static bool write_json(const char* path, cJSON* root) {
    char* text = root ? cJSON_PrintUnformatted(root) : NULL;
    if (!text) return false;
    char tmp[96];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = fopen(tmp, "wb");
    const size_t len = strlen(text);
    bool ok = f && fwrite(text, 1, len, f) == len;
    if (f) ok = fclose(f) == 0 && ok;
    cJSON_free(text);
    if (!ok) { remove(tmp); return false; }   // 写失败就保留旧文件 / Keep the old file when the write failed
    remove(path);
    if (rename(tmp, path) != 0) { remove(tmp); return false; }
    return true;
}

static void str(char* out, size_t cap, const cJSON* o, const char* key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsString(v) && v->valuestring) snprintf(out, cap, "%s", v->valuestring);
    else if (cJSON_IsNumber(v)) snprintf(out, cap, "%.0f", v->valuedouble);
    else if (cap) out[0] = 0;
}

static bool exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && st.st_size > 0;
}

/* ---- 后台任务 / Background jobs ---- */

// 还没开始的最新请求：后台在忙时先记下，忙完马上执行；再来新请求就替换掉它。
// The latest request not yet started: kept while the worker is busy and run as soon as it is free; a newer one
// replaces it.
static tomato_hupu_req_t s_pending;
static bool s_has_pending;

static bool start_req(const tomato_hupu_req_t* r) {
    esp_err_t err = tomato_sync_start_hupu(r);
    if (err != ESP_OK) {
        snprintf(s_msg, sizeof(s_msg), "%s", err == ESP_ERR_INVALID_STATE ? "后台正在同步，稍后再试" : "无法开始加载");
        return false;
    }
    s_job = *r;
    s_job_seq = tomato_sync_seq();
    s_job_active = true;
    s_step_loaded = false;
    s_msg[0] = 0;
    tomato_sync_get_status(&s_status);
    return true;
}

// 后台正在跑的是不是本页发起的任务。/ Whether the running job is the one this page started.
static bool ours_running(void) {
    return s_job_active && tomato_sync_busy() && tomato_sync_seq() == s_job_seq;
}

static bool start_job(tomato_hupu_job_t job, const char* id, const char* key, int sort, bool more) {
    tomato_hupu_req_t req = {.job = job, .sort = sort, .more = more};
    if (id) snprintf(req.id, sizeof(req.id), "%s", id);
    if (key) snprintf(req.key, sizeof(req.key), "%s", key);
    if (tomato_sync_busy()) {
        // 前台优先：自己的旧任务和别的页留下的浏览任务立刻取消；下载和同步不打断，排在它后面。
        // Foreground first: cancel our old job and browsing jobs left by other pages at once; downloads and sync are
        // left to finish and this request waits behind them.
        const bool yields = ours_running() || tomato_sync_preemptible();
        if (yields) tomato_sync_cancel();
        s_pending = req;
        s_has_pending = true;
        if (yields) s_msg[0] = 0;
        else snprintf(s_msg, sizeof(s_msg), "后台正在下载或同步，完成后自动开始");
        return true;
    }
    s_has_pending = false;
    return start_req(&req);
}

// 不再需要某类请求（离开那一层）：取消正在跑的，丢掉还没开始的。/ A level is left: cancel its running job and drop a
// pending one.
static void drop_job(tomato_hupu_job_t job) {
    if (s_has_pending && s_pending.job == job) s_has_pending = false;
    if (s_job.job == job && ours_running()) tomato_sync_cancel();
}

static bool job_running(tomato_hupu_job_t job) {
    return (s_has_pending && s_pending.job == job) || (s_job.job == job && ours_running());
}

static const char* loading_text(const char* what) {
    static char buf[48];
    snprintf(buf, sizeof(buf), "%s", s_status.phase == TOMATO_CONNECTING ? "正在连接 Wi-Fi…" : what);
    return buf;
}

/* ---- 列表数据 / List data ---- */

static int list_pages(void) { return s_item_count ? (s_item_count + ROWS - 1) / ROWS : 1; }

// 读推荐（home.json）或关注流（feed.json）。/ Load the feed (home.json) or following feed (feed.json).
static void load_list_file(const char* path, bool with_thumbs) {
    s_item_count = 0;
    s_list_end = true;
    cJSON* root = read_json(path);
    const cJSON* it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "items")) {
        if (s_item_count >= LIST_MAX) break;
        item_t* x = &s_items[s_item_count];
        str(x->tid, sizeof(x->tid), it, "tid");
        str(x->title, sizeof(x->title), it, "title");
        if (!x->tid[0] || !x->title[0]) continue;
        char zone[48], replies[16];
        str(zone, sizeof(zone), it, "zone");
        str(replies, sizeof(replies), it, "replies");
        snprintf(x->meta, sizeof(x->meta), "%s%s%s 回复", zone, zone[0] ? " · " : "", replies[0] ? replies : "0");
        x->jpg[0] = 0;
        const cJSON* img = cJSON_GetObjectItemCaseSensitive(it, "img");
        if (with_thumbs && cJSON_IsString(img) && img->valuestring[0])
            tomato_hupu_img_path(img->valuestring, x->jpg, sizeof(x->jpg));
        s_item_count++;
    }
    cJSON_Delete(root);
    if (s_list_page >= list_pages()) s_list_page = list_pages() - 1;
}

static void load_zone_list(void) {
    s_item_count = 0;
    s_list_end = false;
    char path[96];
    tomato_hupu_zone_path(s_zone_id, s_zone_sort, path, sizeof(path));
    cJSON* root = read_json(path);
    s_list_end = !root || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "end"));
    const cJSON* it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "items")) {
        if (s_item_count >= LIST_MAX) { s_list_end = true; break; }
        item_t* x = &s_items[s_item_count];
        str(x->tid, sizeof(x->tid), it, "tid");
        str(x->title, sizeof(x->title), it, "title");
        if (!x->tid[0] || !x->title[0]) continue;
        char user[48], time[24], replies[16];
        str(user, sizeof(user), it, "user");
        str(time, sizeof(time), it, "time");
        str(replies, sizeof(replies), it, "replies");
        snprintf(x->meta, sizeof(x->meta), "%.40s · %.20s · %.12s 回复", user[0] ? user : "虎扑JR", time, replies[0] ? replies : "0");
        x->jpg[0] = 0;
        s_item_count++;
    }
    cJSON_Delete(root);
    if (s_list_page >= list_pages()) s_list_page = list_pages() - 1;
}

static void load_current_list(void) {
    if (s_view == V_ZONE) load_zone_list();
    else if (s_seg == SEG_FEED) load_list_file(TOMATO_HUPU_HOME_FILE, true);
    else if (s_seg == SEG_FOLLOW) load_list_file(TOMATO_HUPU_DIR "/feed.json", false);
}

/* ---- 专区与关注 / Zones and follows ---- */

static void load_zones(void) {
    s_zone_count = s_cat_count = 0;
    cJSON* root = read_json(TOMATO_HUPU_DIR "/zones.json");
    const cJSON* c;
    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(root, "cats")) {
        if (s_cat_count >= CATS_MAX) break;
        cat_t* cat = &s_cats[s_cat_count];
        str(cat->name, sizeof(cat->name), c, "name");
        cat->first = (int16_t)s_zone_count;
        const cJSON* z;
        cJSON_ArrayForEach(z, cJSON_GetObjectItemCaseSensitive(c, "zones")) {
            if (s_zone_count >= ZONES_MAX) break;
            str(s_zones[s_zone_count].id, sizeof(s_zones[0].id), z, "id");
            str(s_zones[s_zone_count].name, sizeof(s_zones[0].name), z, "name");
            if (s_zones[s_zone_count].id[0]) s_zone_count++;
        }
        cat->count = (int16_t)(s_zone_count - cat->first);
        if (cat->count) s_cat_count++;
    }
    cJSON_Delete(root);
    if (s_cat >= s_cat_count) s_cat = 0;
}

static void load_follows(void) {
    s_follow_count = 0;
    cJSON* root = read_json(TOMATO_HUPU_DIR "/follow.json");
    const cJSON* z;
    cJSON_ArrayForEach(z, cJSON_GetObjectItemCaseSensitive(root, "zones")) {
        if (s_follow_count >= FOLLOWS_MAX) break;
        str(s_follows[s_follow_count].id, sizeof(s_follows[0].id), z, "id");
        str(s_follows[s_follow_count].name, sizeof(s_follows[0].name), z, "name");
        if (s_follows[s_follow_count].id[0]) s_follow_count++;
    }
    cJSON_Delete(root);
}

static int follow_index(const char* id) {
    for (int i = 0; i < s_follow_count; i++) if (!strcmp(s_follows[i].id, id)) return i;
    return -1;
}

// 关注或取消关注，存 follow.json；关注流作废，下次进“关注”重新取。/ Toggle a follow and save; the old feed is dropped.
static void toggle_follow(const char* id, const char* name) {
    int at = follow_index(id);
    if (at >= 0) {
        memmove(&s_follows[at], &s_follows[at + 1], sizeof(zone_t) * (size_t)(s_follow_count - at - 1));
        s_follow_count--;
    } else if (s_follow_count < FOLLOWS_MAX) {
        snprintf(s_follows[s_follow_count].id, sizeof(s_follows[0].id), "%s", id);
        snprintf(s_follows[s_follow_count].name, sizeof(s_follows[0].name), "%s", name);
        s_follow_count++;
    } else {
        snprintf(s_msg, sizeof(s_msg), "最多关注 %d 个专区", FOLLOWS_MAX);
        return;
    }
    mkdir(TOMATO_HUPU_DIR, 0775);
    cJSON* root = cJSON_CreateObject();
    cJSON* arr = root ? cJSON_AddArrayToObject(root, "zones") : NULL;
    for (int i = 0; arr && i < s_follow_count; i++) {
        cJSON* z = cJSON_CreateObject();
        if (!z) break;
        cJSON_AddStringToObject(z, "id", s_follows[i].id);
        cJSON_AddStringToObject(z, "name", s_follows[i].name);
        cJSON_AddItemToArray(arr, z);
    }
    if (!arr || !write_json(TOMATO_HUPU_DIR "/follow.json", root)) snprintf(s_msg, sizeof(s_msg), "保存关注失败，检查 TF 卡");
    cJSON_Delete(root);
    remove(TOMATO_HUPU_DIR "/feed.json");
}

/* ---- 赛程与评分数据 / Schedule and rating data ---- */

static void load_schedule(void) {
    s_day_count = s_match_count = 0;
    char path[96];
    tomato_hupu_schedule_path(LEAGUES[s_league].id, path, sizeof(path));
    cJSON* root = read_json(path);
    const cJSON* d;
    cJSON_ArrayForEach(d, cJSON_GetObjectItemCaseSensitive(root, "days")) {
        if (s_day_count >= DAYS_MAX) break;
        day_t* day = &s_days[s_day_count];
        str(day->label, sizeof(day->label), d, "d");
        day->first = (int16_t)s_match_count;
        const cJSON* m;
        cJSON_ArrayForEach(m, cJSON_GetObjectItemCaseSensitive(d, "m")) {
            if (s_match_count >= MATCHES_MAX) break;
            match_t* x = &s_matches[s_match_count++];
            str(x->a, sizeof(x->a), m, "a");
            str(x->as, sizeof(x->as), m, "as");
            str(x->ai, sizeof(x->ai), m, "ai");
            str(x->b, sizeof(x->b), m, "b");
            str(x->bs, sizeof(x->bs), m, "bs");
            str(x->bi, sizeof(x->bi), m, "bi");
            str(x->desc, sizeof(x->desc), m, "sd");
            str(x->count, sizeof(x->count), m, "c");
            str(x->k, sizeof(x->k), m, "k");
            str(x->n, sizeof(x->n), m, "n");
            str(x->ts, sizeof(x->ts), m, "t");
            const cJSON* st = cJSON_GetObjectItemCaseSensitive(m, "st");
            x->st = (uint8_t)(cJSON_IsNumber(st) ? st->valueint : 0);
        }
        day->count = (int16_t)(s_match_count - day->first);
        s_day_count++;
    }
    const cJSON* today = cJSON_GetObjectItemCaseSensitive(root, "today");
    s_day = cJSON_IsNumber(today) && today->valueint < s_day_count ? today->valueint : (s_day_count ? s_day_count - 1 : 0);
    s_day_page = 0;
    cJSON_Delete(root);
}

static int cmp_player(const void* a, const void* b) {
    const float x = ((const player_t*)a)->score, y = ((const player_t*)b)->score;
    return x < y ? 1 : x > y ? -1 : 0;
}

static bool load_rating(void) {
    s_player_count = 0;
    const match_t* m = &s_matches[s_rate_match];
    char path[96];
    tomato_hupu_rating_path(m->k, m->n, path, sizeof(path));
    cJSON* root = read_json(path);
    if (!root) return false;
    str(s_rate_title, sizeof(s_rate_title), root, "title");
    const cJSON* p;
    cJSON_ArrayForEach(p, cJSON_GetObjectItemCaseSensitive(root, "p")) {
        if (s_player_count >= PLAYERS_MAX) break;
        player_t* x = &s_players[s_player_count++];
        str(x->name, sizeof(x->name), p, "n");
        str(x->team, sizeof(x->team), p, "team");
        str(x->type, sizeof(x->type), p, "type");
        str(x->min, sizeof(x->min), p, "min");
        str(x->stats, sizeof(x->stats), p, "st");
        const cJSON* s = cJSON_GetObjectItemCaseSensitive(p, "s");
        const cJSON* c = cJSON_GetObjectItemCaseSensitive(p, "c");
        x->score = cJSON_IsNumber(s) ? (float)s->valuedouble : 0;
        x->count = cJSON_IsNumber(c) ? c->valueint : 0;
    }
    cJSON_Delete(root);
    qsort(s_players, (size_t)s_player_count, sizeof(player_t), cmp_player);
    // 球员能按两队分开就默认看第一队。/ Default to the first team when players split by team.
    bool a = false, b = false;
    for (int i = 0; i < s_player_count; i++) {
        a |= m->ai[0] && !strcmp(s_players[i].team, m->ai);
        b |= m->bi[0] && !strcmp(s_players[i].team, m->bi);
    }
    s_rate_team = a && b ? 1 : 0;
    s_rate_page = 0;
    return s_player_count > 0;
}

// 当前队（或全部）里第 k 个条目。/ The k-th entry of the current team (or all).
static int rate_count(void) {
    if (!s_rate_team) return s_player_count;
    const char* team = s_rate_team == 1 ? s_matches[s_rate_match].ai : s_matches[s_rate_match].bi;
    int n = 0;
    for (int i = 0; i < s_player_count; i++) n += !strcmp(s_players[i].team, team);
    return n;
}

static const player_t* rate_at(int k) {
    const char* team = s_rate_team == 1 ? s_matches[s_rate_match].ai : s_matches[s_rate_match].bi;
    for (int i = 0; i < s_player_count; i++) {
        if (s_rate_team && strcmp(s_players[i].team, team)) continue;
        if (k-- == 0) return &s_players[i];
    }
    return NULL;
}

static int rate_pages(void) {
    const int n = rate_count();
    return n ? (n + RATE_ROWS - 1) / RATE_ROWS : 1;
}

/* ---- 帖子数据与排版 / Post data and layout ---- */

static const char* keep(const cJSON* o, const char* key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
    const char* s = cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
    const size_t n = strlen(s) + 1;
    if (!s_arena || s_arena_len + n > s_arena_cap) return "";
    char* out = s_arena + s_arena_len;
    memcpy(out, s, n);
    s_arena_len += n;
    return out;
}

static int title_lines(void) {
    const char* p = s_title;
    int n = 0;
    while (*p && n < 2) { p += tr_line_bytes(p, TITLE_PX, CW); if (*p == '\n') p++; n++; }
    return n ? n : 1;
}

static int body_top(int page) {
    return page ? BODY_TOP : TITLE_Y + s_title_lines * TITLE_LH + 10 + 44 + 18;
}

static void add_line(int block, uint32_t off, int len, int h, int kind) {
    if (s_line_count >= LINES_MAX) return;
    s_lines[s_line_count++] = (line_t){.block = (uint16_t)block, .off = off, .len = (uint16_t)len, .h = (int16_t)h,
                                       .kind = (uint8_t)kind};
}

static void layout_body(void) {
    s_line_count = 0;
    for (int b = 0; b < s_block_count; b++) {
        const block_t* blk = &s_blocks[b];
        if (blk->image) {
            add_line(b, 0, 0, blk->h, 1);
        } else {
            const char* p = blk->text;
            while (*p) {
                size_t n = tr_line_bytes(p, BODY_PX, CW);
                if (n > 0xFFFF) n = 0xFFFF;
                add_line(b, (uint32_t)(p - blk->text), (int)n, BODY_LH, 0);
                p += n;
                if (*p == '\n') p++;
            }
        }
        add_line(b, 0, 0, PARA_GAP, 2);
    }
    // 每页第一行记进 s_body_pages；页首的段间距不占高度（画的时候同样跳过）。
    // Record each page's first line; a gap at the top of a page takes no height (drawing skips it the same way).
    s_body_pages[0] = 0;
    s_body_page_count = 1;
    int y = body_top(0);
    for (int i = 0; i < s_line_count; i++) {
        const line_t* l = &s_lines[i];
        const int top = body_top(s_body_page_count - 1);
        if (y + l->h > BODY_BOTTOM && y != top) {
            if (s_body_page_count >= PAGES_MAX) break;
            s_body_pages[s_body_page_count++] = (uint16_t)i;
            y = body_top(s_body_page_count - 1);
            if (l->kind == 2) continue;
        } else if (l->kind == 2 && i == s_body_pages[s_body_page_count - 1]) {
            continue;
        }
        y += l->h;
    }
}

static int reply_lines(const reply_t* r, int max) {
    const char* p = r->text;
    int n = 0;
    while (*p && n < max) { p += tr_line_bytes(p, REPLY_PX, CW); if (*p == '\n') p++; n++; }
    return n ? n : 1;
}

static int reply_height(const reply_t* r) {
    return 40 + (r->quote ? 40 : 0) + reply_lines(r, REPLY_LINES) * REPLY_LH + 32;
}

static void layout_replies(int which) {
    const reply_t* list = which ? s_list : s_lights;
    const int count = which ? s_list_count : s_light_count;
    int pages = 0, y = BODY_BOTTOM;
    for (int i = 0; i < count && pages < REPLIES_MAX; i++) {
        const int h = reply_height(&list[i]);
        if (y + h > BODY_BOTTOM && y != BODY_TOP) { s_reply_pages[which][pages++] = (uint8_t)i; y = BODY_TOP; }
        y += h;
    }
    if (!pages) s_reply_pages[which][pages++] = 0;
    s_reply_pages[which][pages] = (uint8_t)count;
    s_reply_page_count[which] = pages;
}

static void read_replies(const cJSON* arr, reply_t* out, int* count) {
    *count = 0;
    const cJSON* r;
    cJSON_ArrayForEach(r, arr) {
        if (*count >= REPLIES_MAX) break;
        reply_t* x = &out[(*count)++];
        x->user = keep(r, "u");
        x->text = keep(r, "c");
        x->quote = cJSON_IsString(cJSON_GetObjectItemCaseSensitive(r, "q")) ? keep(r, "q") : NULL;
        const cJSON* n = cJSON_GetObjectItemCaseSensitive(r, "n");
        x->lights = cJSON_IsNumber(n) ? n->valueint : 0;
    }
}

static bool load_post(const char* tid) {
    free_post();
    char path[96];
    tomato_hupu_post_path(tid, path, sizeof(path));
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) return false;
    cJSON* root = read_json(path);
    if (!root) return false;
    // 内存池按文件大小开：里面的字符串加起来不会比文件长。/ Size the arena by the file: its strings never exceed it.
    s_arena_cap = (size_t)st.st_size + 1024;
    s_arena = heap_caps_malloc(s_arena_cap, PSRAM_CAPS);
    if (!s_arena) { cJSON_Delete(root); s_arena_cap = 0; return false; }
    s_title = keep(root, "title");
    s_author = keep(root, "author");
    s_time = keep(root, "time");
    s_zone = keep(root, "zone");
    const cJSON* rt = cJSON_GetObjectItemCaseSensitive(root, "replies");
    s_reply_total = cJSON_IsNumber(rt) ? rt->valueint : 0;
    const cJSON* b;
    cJSON_ArrayForEach(b, cJSON_GetObjectItemCaseSensitive(root, "body")) {
        if (s_block_count >= BLOCKS_MAX) break;
        block_t* x = &s_blocks[s_block_count];
        memset(x, 0, sizeof(*x));
        const cJSON* img = cJSON_GetObjectItemCaseSensitive(b, "i");
        if (cJSON_IsString(img)) {
            x->image = true;
            tomato_hupu_img_path(img->valuestring, x->jpg, sizeof(x->jpg));
            int iw = 4, ih = 3;
            tomato_hupu_img_size(img->valuestring, &iw, &ih);
            int w = CW, h = (int)((int64_t)CW * ih / iw);
            if (h > IMAGE_MAX_H) { h = IMAGE_MAX_H; w = (int)((int64_t)IMAGE_MAX_H * iw / ih); }
            x->w = (int16_t)(w < 40 ? 40 : w);
            x->h = (int16_t)(h < 40 ? 40 : h);
        } else {
            x->text = keep(b, "t");
            if (!x->text[0]) continue;
        }
        s_block_count++;
    }
    read_replies(cJSON_GetObjectItemCaseSensitive(root, "lights"), s_lights, &s_light_count);
    read_replies(cJSON_GetObjectItemCaseSensitive(root, "list"), s_list, &s_list_count);
    cJSON_Delete(root);
    snprintf(s_tid, sizeof(s_tid), "%s", tid);
    s_title_lines = title_lines();
    layout_body();
    layout_replies(0);
    layout_replies(1);
    s_post_loaded = true;
    return true;
}

// 帖子里还有没下好的图吗。/ Whether the loaded post still has images not downloaded.
static bool post_missing_images(void) {
    for (int i = 0; i < s_block_count; i++)
        if (s_blocks[i].image && !exists(s_blocks[i].jpg)) return true;
    return false;
}

static int tab_pages(void) {
    if (s_tab == T_BODY) return s_body_page_count;
    return s_reply_page_count[s_tab == T_LIST];
}

static int image_count(void) {
    int n = 0;
    for (int i = 0; i < s_block_count; i++) n += s_blocks[i].image;
    return n;
}

static const block_t* image_at(int k) {
    for (int i = 0; i < s_block_count; i++)
        if (s_blocks[i].image && k-- == 0) return &s_blocks[i];
    return NULL;
}

/* ---- 绘制：列表 / Drawing: lists ---- */

static void draw_rows(uint8_t* fb, int top) {
    char line[160];
    for (int row = 0; row < ROWS; row++) {
        const int i = s_list_page * ROWS + row;
        if (i >= s_item_count) break;
        const item_t* x = &s_items[i];
        const int y = top + row * ROW_H;
        const int tw = x->jpg[0] ? THUMB_X - TR_MARGIN - 20 : CW;
        tr_wrap(fb, TR_MARGIN, y + 10, tw, 30, 40, x->title, 2);
        snprintf(line, sizeof(line), "%s", x->meta);
        tr_fit(line, 24, tw);
        tr_text(fb, TR_MARGIN, y + 100, 24, line, EPD_DRAW_ALIGN_LEFT, false);
        if (x->jpg[0]) {
            const uint8_t* px = hupu_img_get(x->jpg, THUMB_W, THUMB_H);
            if (px) hupu_img_draw(fb, THUMB_X, y + 18, THUMB_W, THUMB_H, px, true);
            else epd_draw_rect((EpdRect){THUMB_X, y + 18, THUMB_W, THUMB_H}, TR_GRAY_LINE, fb);
        }
        tr_hline(fb, y + ROW_H - 6, TR_MARGIN, CW, true);
    }
}

// 列表最后一页之后还能取更多：显示“加载更多”。/ More can be fetched after the last page.
static int list_pages_shown(void) {
    const int pages = list_pages();
    return s_view == V_ZONE && !s_list_end && s_item_count ? pages + 1 : pages;
}

static void draw_header(uint8_t* fb) {
    tr_title(fb, "虎扑");
    if (!tomato_sync_busy()) {
        tr_icon(fb, TR_ICON_SYNC, TR_W - TR_MARGIN - 86, 116, false);
        tr_text(fb, TR_W - TR_MARGIN, 98, 30, "刷新", EPD_DRAW_ALIGN_RIGHT, false);
    }
    static const char* const SEG[3] = {"推荐", "关注", "比分"};
    tr_segment(fb, TR_SEGMENT_RECT, SEG, 3, (int)s_seg);
}

static void draw_empty(uint8_t* fb, int y, const char* text) {
    tr_wrap(fb, TR_MARGIN, y, CW, 30, 46, text, 4);
}

static void draw_feed(uint8_t* fb) {
    char line[160];
    const bool loading = job_running(TOMATO_HUPU_HOME);
    if (loading) snprintf(line, sizeof(line), "%s", loading_text(s_step_loaded ? "正在下载缩略图…" : "正在加载虎扑推荐…"));
    else if (s_msg[0]) snprintf(line, sizeof(line), "%s", s_msg);
    else snprintf(line, sizeof(line), "首页推荐 · %d 条", s_item_count);
    tr_message(fb, TR_MARGIN, STATUS_Y, STATUS_Y - 12, CW, 26, line, EPD_DRAW_ALIGN_LEFT, false);
    if (!s_item_count) {
        draw_empty(fb, LIST_TOP + 20, loading ? "加载中，请稍候。" :
                   "还没有缓存。联网后点右上角“刷新”加载虎扑推荐；之后不联网也能看上次的内容。");
        return;
    }
    draw_rows(fb, LIST_TOP);
    tr_pager(fb, PAGER_Y, s_list_page, list_pages());
}

static void draw_follow(uint8_t* fb) {
    char line[200];
    if (!s_follow_count) {
        draw_empty(fb, STATUS_Y + 20, "还没有关注的专区。关注后，这里按回复时间混排各专区的最新帖子。关注只存在设备上，不用登录。");
        tr_pill(fb, (EpdRect){TR_MARGIN, 440, CW, 84}, "去选专区", true, 32);
        return;
    }
    const bool loading = job_running(TOMATO_HUPU_FOLLOW);
    if (loading) {
        snprintf(line, sizeof(line), "%s", loading_text("正在加载关注的专区…"));
    } else if (s_msg[0]) {
        snprintf(line, sizeof(line), "%s", s_msg);
    } else {
        int n = snprintf(line, sizeof(line), "来自 %d 个专区：", s_follow_count);
        for (int i = 0; i < s_follow_count && i < 3 && n > 0 && (size_t)n < sizeof(line); i++)
            n += snprintf(line + n, sizeof(line) - n, "%s%s", i ? "、" : "", s_follows[i].name);
    }
    const int manage_w = ttf_text_width_px(26, "管理 ›") + 20;
    if (loading || !s_msg[0]) {
        tr_fit(line, 26, CW - manage_w);
        tr_text(fb, TR_MARGIN, STATUS_Y, 26, line, EPD_DRAW_ALIGN_LEFT, false);
    } else {
        tr_message(fb, TR_MARGIN, STATUS_Y, STATUS_Y - 12, CW - manage_w, 26, line, EPD_DRAW_ALIGN_LEFT, false);
    }
    tr_text(fb, TR_W - TR_MARGIN, STATUS_Y, 26, "管理 ›", EPD_DRAW_ALIGN_RIGHT, true);
    if (!s_item_count) {
        draw_empty(fb, LIST_TOP + 20, loading ? "加载中，请稍候。" : "关注流还没有缓存。联网后点右上角“刷新”。");
        return;
    }
    draw_rows(fb, LIST_TOP);
    tr_pager(fb, PAGER_Y, s_list_page, list_pages());
}

/* ---- 绘制：比分 / Drawing: scores ---- */

static EpdRect league_rect(int i) {
    int x = TR_MARGIN;
    for (int k = 0; k < i; k++) x += ttf_text_width_px(28, LEAGUES[k].name) + 48 + 12;
    return (EpdRect){x, CHIP_Y, ttf_text_width_px(28, LEAGUES[i].name) + 48, 54};
}

static int day_pages(void) {
    const int n = s_day_count ? s_days[s_day].count : 0;
    return n ? (n + ROWS - 1) / ROWS : 1;
}

// 开赛时间：时间戳是毫秒，按北京时间显示，不依赖时区设置。/ Start time from ms, shown in Beijing time without TZ.
static void start_time(const char* ts, char* out, size_t cap) {
    const long long ms = atoll(ts);
    if (ms <= 0) { snprintf(out, cap, "未开始"); return; }
    const long long s = ms / 1000 + 8 * 3600;
    snprintf(out, cap, "%02d:%02d 开赛", (int)(s % 86400 / 3600), (int)(s % 3600 / 60));
}

static void draw_scores(uint8_t* fb) {
    for (int i = 0; i < LEAGUE_COUNT; i++) tr_pill(fb, league_rect(i), LEAGUES[i].name, i == s_league, 28);
    char line[96];
    const bool loading = job_running(TOMATO_HUPU_SCHEDULE);
    if (loading) snprintf(line, sizeof(line), "%s", loading_text("正在加载赛程…"));
    else if (s_msg[0]) snprintf(line, sizeof(line), "%s", s_msg);
    else if (s_day_count) snprintf(line, sizeof(line), "%s", s_days[s_day].label);
    else line[0] = 0;
    tr_message(fb, TR_W / 2, DATE_Y, DATE_Y - 4, CW - 160, 30, line, EPD_DRAW_ALIGN_CENTER, true);
    if (s_day_count && !loading) {
        if (s_day > 0) tr_text(fb, TR_MARGIN + 20, DATE_Y, 30, "‹", EPD_DRAW_ALIGN_CENTER, true);
        if (s_day + 1 < s_day_count) tr_text(fb, TR_W - TR_MARGIN - 20, DATE_Y, 30, "›", EPD_DRAW_ALIGN_CENTER, true);
    }
    tr_hline(fb, DATE_Y + 58, TR_MARGIN, CW, false);
    if (!s_day_count) {
        draw_empty(fb, MATCH_TOP + 20, loading ? "加载中，请稍候。" : "这个赛事还没有缓存。联网后点右上角“刷新”。");
        return;
    }
    const day_t* d = &s_days[s_day];
    if (!d->count) { draw_empty(fb, MATCH_TOP + 20, "这一天没有比赛"); return; }
    for (int row = 0; row < ROWS; row++) {
        const int k = s_day_page * ROWS + row;
        if (k >= d->count) break;
        const match_t* m = &s_matches[d->first + k];
        const int y = MATCH_TOP + row * MATCH_H;
        char a[32], b[32];
        snprintf(a, sizeof(a), "%s", m->a);
        snprintf(b, sizeof(b), "%s", m->b);
        tr_fit(a, 32, 186);
        tr_fit(b, 32, 186);
        tr_text(fb, TR_MARGIN + 95, y + 16, 32, a, EPD_DRAW_ALIGN_CENTER, true);
        tr_text(fb, TR_W - TR_MARGIN - 95, y + 16, 32, b, EPD_DRAW_ALIGN_CENTER, true);
        if (m->st && m->as[0]) snprintf(line, sizeof(line), "%s : %s", m->as, m->bs[0] ? m->bs : "0");
        else snprintf(line, sizeof(line), "VS");
        tr_text(fb, TR_W / 2, y + 8, 40, line, EPD_DRAW_ALIGN_CENTER, true);
        if (m->st == 2) snprintf(line, sizeof(line), "%s%s%s%s", m->desc[0] ? m->desc : "已结束",
                                 m->count[0] ? " · " : "", m->count, m->k[0] ? " ›" : "");
        else if (m->st == 1) snprintf(line, sizeof(line), "%s", m->desc[0] ? m->desc : "进行中");
        else start_time(m->ts, line, sizeof(line));
        tr_fit(line, 24, CW);
        tr_text(fb, TR_W / 2, y + 70, 24, line, EPD_DRAW_ALIGN_CENTER, false);
        tr_hline(fb, y + MATCH_H - 8, TR_MARGIN, CW, true);
    }
    tr_pager(fb, PAGER_Y, s_day_page, day_pages());
}

static void draw_rate(uint8_t* fb) {
    tr_text(fb, TR_MARGIN, 92, 30, "‹ 比分", EPD_DRAW_ALIGN_LEFT, false);
    const match_t* m = &s_matches[s_rate_match];
    char line[96];
    if (m->as[0]) snprintf(line, sizeof(line), "%s %s : %s %s", m->a, m->as, m->bs, m->b);
    else snprintf(line, sizeof(line), "%s", s_rate_title);
    tr_fit(line, 30, 400);
    tr_text(fb, TR_W - TR_MARGIN, 92, 30, line, EPD_DRAW_ALIGN_RIGHT, true);
    if (!s_player_count) {
        const char* msg = job_running(TOMATO_HUPU_RATING) ? loading_text("正在加载评分…") : s_msg[0] ? s_msg : "没有评分数据";
        draw_empty(fb, 200, msg);
        return;
    }
    if (s_rate_team) {
        const char* labels[2] = {m->a, m->b};
        tr_segment(fb, (EpdRect){TR_MARGIN, 150, CW, 64}, labels, 2, s_rate_team - 1);
    }
    tr_text(fb, TR_MARGIN, 250, 24, "球员", EPD_DRAW_ALIGN_LEFT, false);
    tr_text(fb, 370, 250, 24, "评分", EPD_DRAW_ALIGN_CENTER, false);
    tr_text(fb, TR_W - TR_MARGIN, 250, 24, "技术统计", EPD_DRAW_ALIGN_RIGHT, false);
    tr_hline(fb, 288, TR_MARGIN, CW, false);
    for (int row = 0; row < RATE_ROWS; row++) {
        const player_t* p = rate_at(s_rate_page * RATE_ROWS + row);
        if (!p) break;
        const int y = RATE_TOP + row * RATE_H;
        snprintf(line, sizeof(line), "%s", p->name);
        tr_fit(line, 30, 270);
        tr_text(fb, TR_MARGIN, y + 10, 30, line, EPD_DRAW_ALIGN_LEFT, true);
        if (!strcmp(p->type, "coach")) snprintf(line, sizeof(line), "教练");
        else if (!strcmp(p->type, "referee")) snprintf(line, sizeof(line), "裁判");
        else if (p->min[0]) snprintf(line, sizeof(line), "出场 %s", p->min);
        else line[0] = 0;
        tr_fit(line, 24, 270);
        tr_text(fb, TR_MARGIN, y + 56, 24, line, EPD_DRAW_ALIGN_LEFT, false);
        snprintf(line, sizeof(line), "%.1f", (double)p->score);
        tr_text(fb, 370, y + 16, 40, line, EPD_DRAW_ALIGN_CENTER, true);
        snprintf(line, sizeof(line), "%s", p->stats);
        tr_fit(line, 24, 214);
        tr_text(fb, TR_W - TR_MARGIN, y + 14, 24, line, EPD_DRAW_ALIGN_RIGHT, false);
        if (p->count >= 10000) snprintf(line, sizeof(line), "%.1f 万人评", p->count / 10000.0);
        else snprintf(line, sizeof(line), "%d 人评", p->count);
        tr_text(fb, TR_W - TR_MARGIN, y + 56, 24, line, EPD_DRAW_ALIGN_RIGHT, false);
        tr_hline(fb, y + RATE_H - 6, TR_MARGIN, CW, true);
    }
    tr_pager(fb, POST_PAGER_Y, s_rate_page, rate_pages());
}

/* ---- 绘制：专区 / Drawing: zones ---- */

// 分类胶囊按顺序排，返回第 i 个的位置。/ Lay category chips out in order; returns chip i.
static EpdRect cat_rect(int i, int* bottom) {
    int x = TR_MARGIN, y = 170;
    EpdRect r = {0};
    for (int k = 0; k < s_cat_count; k++) {
        const int w = ttf_text_width_px(26, s_cats[k].name) + 32;
        if (x + w > TR_W - TR_MARGIN) { x = TR_MARGIN; y += 60; }
        if (k == i) r = (EpdRect){x, y, w, 50};
        x += w + 10;
    }
    if (bottom) *bottom = y + 50;
    return r;
}

static int grid_top(void) {
    int bottom = 170;
    if (s_cat_count) cat_rect(-1, &bottom);
    return bottom + 40;
}

static int grid_rows(void) { return (PAGER_Y - 20 - grid_top() + (CELL_STEP_Y - CELL_H)) / CELL_STEP_Y; }

static int zone_pages(void) {
    const int per = grid_rows() * 3;
    const int n = s_cat_count ? s_cats[s_cat].count : 0;
    return n && per > 0 ? (n + per - 1) / per : 1;
}

static void draw_zones(uint8_t* fb) {
    tr_text(fb, TR_MARGIN, 92, 44, "‹", EPD_DRAW_ALIGN_LEFT, false);
    tr_text(fb, 96, 96, 40, "全部专区", EPD_DRAW_ALIGN_LEFT, true);
    char line[64];
    snprintf(line, sizeof(line), "已关注 %d", s_follow_count);
    tr_text(fb, TR_W - TR_MARGIN, 104, 28, line, EPD_DRAW_ALIGN_RIGHT, false);
    if (!s_zone_count) {
        const char* msg = job_running(TOMATO_HUPU_ZONES) ? loading_text("正在加载专区…") :
                          s_msg[0] ? s_msg : "专区列表还没有缓存，联网后再进来。";
        draw_empty(fb, 200, msg);
        return;
    }
    for (int i = 0; i < s_cat_count; i++) {
        tr_pill(fb, cat_rect(i, NULL), s_cats[i].name, i == s_cat, 26);
    }
    const int top = grid_top(), rows = grid_rows(), per = rows * 3;
    tr_hline(fb, top - 22, TR_MARGIN, CW, false);
    const cat_t* c = &s_cats[s_cat];
    for (int k = 0; k < per; k++) {
        const int i = s_zone_page * per + k;
        if (i >= c->count) break;
        const zone_t* z = &s_zones[c->first + i];
        const bool on = follow_index(z->id) >= 0;
        char name[64];
        snprintf(name, sizeof(name), "%s%s", on ? "✓ " : "", z->name);
        tr_fit(name, 28, CELL_W - 16);
        tr_pill(fb, (EpdRect){TR_MARGIN + (k % 3) * CELL_STEP_X, top + (k / 3) * CELL_STEP_Y, CELL_W, CELL_H}, name, on, 28);
    }
    tr_pager(fb, PAGER_Y, s_zone_page, zone_pages());
}

static void draw_zone(uint8_t* fb) {
    tr_text(fb, TR_MARGIN, 92, 44, "‹", EPD_DRAW_ALIGN_LEFT, false);
    char line[96];
    snprintf(line, sizeof(line), "%s", s_zone_name);
    tr_fit(line, 40, 360);
    tr_text(fb, 96, 96, 40, line, EPD_DRAW_ALIGN_LEFT, true);
    const bool on = follow_index(s_zone_id) >= 0;
    tr_pill(fb, (EpdRect){TR_W - TR_MARGIN - 160, 92, 160, 56}, on ? "✓ 已关注" : "+ 关注", !on, 28);
    tr_pill(fb, (EpdRect){TR_MARGIN, 176, 170, 54}, "最新回复", s_zone_sort == 0, 28);
    tr_pill(fb, (EpdRect){TR_MARGIN + 182, 176, 170, 54}, "最新发布", s_zone_sort == 1, 28);
    const bool loading = job_running(TOMATO_HUPU_THREADS);
    if (!loading) tr_text(fb, TR_W - TR_MARGIN, 186, 30, "刷新", EPD_DRAW_ALIGN_RIGHT, false);
    const int pages = list_pages();
    if (s_list_page >= pages || !s_item_count) {
        // 翻到了还没取的一页，或者整个专区还没缓存。/ A page not fetched yet, or the zone has no cache.
        const char* msg = loading ? loading_text(s_item_count ? "正在加载更多…" : "正在加载专区帖子…")
                        : s_msg[0] ? s_msg : s_item_count ? "没有更多了" : "这个专区还没有缓存，联网后点“刷新”。";
        draw_empty(fb, ZONE_LIST_TOP + 20, msg);
    } else {
        draw_rows(fb, ZONE_LIST_TOP);
        if (s_msg[0]) tr_message(fb, TR_W / 2, 996, 988, CW, 24, s_msg, EPD_DRAW_ALIGN_CENTER, false);
    }
    tr_pager(fb, PAGER_Y, s_list_page, list_pages_shown());
}

/* ---- 绘制：帖子 / Drawing: post ---- */

static void draw_post_header(uint8_t* fb) {
    char back[80];
    snprintf(back, sizeof(back), "‹ %s", s_post_loaded && s_zone[0] ? s_zone : "返回");
    tr_fit(back, 30, 360);
    tr_text(fb, TR_MARGIN, 92, 30, back, EPD_DRAW_ALIGN_LEFT, false);
    if (!s_post_loaded) return;
    char l1[24], l2[24];
    snprintf(l1, sizeof(l1), "亮了 %d", s_light_count);
    snprintf(l2, sizeof(l2), "回复 %d", s_reply_total);
    const char* labels[3] = {"正文", l1, l2};
    tr_segment(fb, POST_SEG, labels, 3, (int)s_tab);
}

static void draw_body(uint8_t* fb) {
    int y = body_top(s_page);
    if (s_page == 0) {
        tr_wrap(fb, TR_MARGIN, TITLE_Y, CW, TITLE_PX, TITLE_LH, s_title, 2);
        char meta[160];
        const int my = TITLE_Y + s_title_lines * TITLE_LH + 10;
        snprintf(meta, sizeof(meta), "%s · %s · %d 回复", s_author, s_time, s_reply_total);
        tr_fit(meta, 24, CW);
        tr_text(fb, TR_MARGIN, my, 24, meta, EPD_DRAW_ALIGN_LEFT, false);
        tr_hline(fb, my + 40, TR_MARGIN, CW, true);
    }
    const int start = s_body_pages[s_page];
    const int end = s_page + 1 < s_body_page_count ? s_body_pages[s_page + 1] : s_line_count;
    char buf[512];
    for (int i = start; i < end; i++) {
        const line_t* l = &s_lines[i];
        const block_t* b = &s_blocks[l->block];
        if (l->kind == 0) {
            size_t n = l->len < sizeof(buf) - 1 ? l->len : sizeof(buf) - 1;
            memcpy(buf, b->text + l->off, n);
            buf[n] = 0;
            tr_text(fb, TR_MARGIN, y, BODY_PX, buf, EPD_DRAW_ALIGN_LEFT, false);
        } else if (l->kind == 1) {
            const int x = TR_MARGIN + (CW - b->w) / 2;
            const uint8_t* px = hupu_img_get(b->jpg, b->w, b->h);
            if (px) {
                hupu_img_draw(fb, x, y, b->w, b->h, px, true);
            } else {
                epd_draw_rect((EpdRect){x, y, b->w, b->h}, UI_GRAY_BLACK, fb);
                tr_text(fb, TR_W / 2, y + b->h / 2 - 14, 26, exists(b->jpg) ? "图片处理中" : job_running(TOMATO_HUPU_POST) ? "图片下载中…" : "图片未下载",
                        EPD_DRAW_ALIGN_CENTER, false);
            }
        }
        if (l->kind != 2 || i != start) y += l->h;
    }
}

static void draw_reply(uint8_t* fb, const reply_t* r, int y, int max_lines) {
    char line[200];
    snprintf(line, sizeof(line), "%s", r->user[0] ? r->user : "虎扑JR");
    tr_fit(line, 26, CW - 160);
    tr_text(fb, TR_MARGIN, y, 26, line, EPD_DRAW_ALIGN_LEFT, true);
    if (r->lights > 0) {
        snprintf(line, sizeof(line), "亮 %d", r->lights);
        tr_text(fb, TR_W - TR_MARGIN, y, 24, line, EPD_DRAW_ALIGN_RIGHT, true);
    }
    y += 40;
    if (r->quote) {
        epd_fill_rect((EpdRect){TR_MARGIN, y, 6, 30}, 0x90, fb);
        snprintf(line, sizeof(line), "引用 %s", r->quote);
        tr_fit(line, 24, CW - 22);
        tr_text(fb, TR_MARGIN + 22, y, 24, line, EPD_DRAW_ALIGN_LEFT, false);
        y += 40;
    }
    tr_wrap(fb, TR_MARGIN, y, CW, REPLY_PX, REPLY_LH, r->text[0] ? r->text : "（空）", max_lines);
}

static void draw_replies(uint8_t* fb) {
    const int which = s_tab == T_LIST;
    const reply_t* list = which ? s_list : s_lights;
    const int count = which ? s_list_count : s_light_count;
    if (!count) {
        tr_text(fb, TR_MARGIN, BODY_TOP + 20, 30, which ? "还没有回复" : "还没有亮了的回复", EPD_DRAW_ALIGN_LEFT, false);
        return;
    }
    int y = BODY_TOP;
    for (int i = s_reply_pages[which][s_page]; i < s_reply_pages[which][s_page + 1]; i++) {
        draw_reply(fb, &list[i], y, REPLY_LINES);
        y += reply_height(&list[i]);
        tr_hline(fb, y - 16, TR_MARGIN, CW, true);
    }
    // 最后一页有空位才写提示，不压到回复上。/ Only add the note when the last page has room, never over a reply.
    if (which && s_page + 1 == s_reply_page_count[1] && s_reply_total > s_list_count && y + 34 <= BODY_BOTTOM)
        tr_text(fb, TR_W / 2, y, 24, "设备上只显示前面的回复", EPD_DRAW_ALIGN_CENTER, false);
}

static void draw_post(uint8_t* fb) {
    draw_post_header(fb);
    if (!s_post_loaded) {
        const char* msg = job_running(TOMATO_HUPU_POST) ? loading_text("正在加载帖子…") :
                          s_msg[0] ? s_msg : "帖子还没有缓存，联网后再打开。";
        draw_empty(fb, 260, msg);
        return;
    }
    if (s_tab == T_BODY) draw_body(fb);
    else draw_replies(fb);
    tr_pager(fb, POST_PAGER_Y, s_page, tab_pages());
}

static void draw_reply_full(uint8_t* fb) {
    tr_text(fb, TR_MARGIN, 92, 30, "‹ 返回", EPD_DRAW_ALIGN_LEFT, false);
    const reply_t* r = &(s_tab == T_LIST ? s_list : s_lights)[s_reply_open];
    draw_reply(fb, r, 170, (BODY_BOTTOM - 170 - 80) / REPLY_LH);
}

static void image_fit(const block_t* b, int* w, int* h) {
    *w = TR_W;
    *h = (int)((int64_t)TR_W * b->h / b->w);
    if (*h > TR_H - IMAGE_BAND) { *h = TR_H - IMAGE_BAND; *w = (int)((int64_t)*h * b->w / b->h); }
}

static void draw_image(uint8_t* fb) {
    const block_t* b = image_at(s_image);
    if (b) {
        int w, h;
        image_fit(b, &w, &h);
        const uint8_t* px = hupu_img_get(b->jpg, w, h);
        if (px) hupu_img_draw(fb, (TR_W - w) / 2, (TR_H - IMAGE_BAND - h) / 2, w, h, px, false);
        else tr_text(fb, TR_W / 2, (TR_H - IMAGE_BAND) / 2, 30, exists(b->jpg) ? "正在处理图片…" : job_running(TOMATO_HUPU_POST) ? "图片下载中…" : "图片还没下载",
                     EPD_DRAW_ALIGN_CENTER, false);
    }
    char label[48];
    snprintf(label, sizeof(label), "图 %d / %d", s_image + 1, image_count());
    epd_fill_rect((EpdRect){0, TR_H - IMAGE_BAND, TR_W, IMAGE_BAND}, UI_GRAY_WHITE, fb);
    tr_hline(fb, TR_H - IMAGE_BAND, 0, TR_W, false);
    tr_text(fb, TR_MARGIN, TR_H - IMAGE_BAND + 26, 30, label, EPD_DRAW_ALIGN_LEFT, true);
    tr_text(fb, TR_W - TR_MARGIN, TR_H - IMAGE_BAND + 28, 28, "左右翻图 · 点中间返回", EPD_DRAW_ALIGN_RIGHT, false);
}

static void render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    ui_clear_page(fb);
    if (!s_items) {
        tr_status_bar(fb);
        tr_title(fb, "虎扑");
        draw_empty(fb, 240, "内存不足，打不开虎扑页。");
        tr_tab_bar(fb, TR_TAB_HUPU);
        return;
    }
    if (s_view != V_IMAGE) tr_status_bar(fb);
    switch (s_view) {
    case V_HOME:
        draw_header(fb);
        if (s_seg == SEG_FEED) draw_feed(fb);
        else if (s_seg == SEG_FOLLOW) draw_follow(fb);
        else draw_scores(fb);
        tr_tab_bar(fb, TR_TAB_HUPU);
        break;
    case V_ZONES: draw_zones(fb); tr_tab_bar(fb, TR_TAB_HUPU); break;
    case V_ZONE: draw_zone(fb); tr_tab_bar(fb, TR_TAB_HUPU); break;
    case V_POST: draw_post(fb); break;
    case V_REPLY: draw_reply_full(fb); break;
    case V_IMAGE: draw_image(fb); break;
    case V_RATE: draw_rate(fb); break;
    }
}

/* ---- 动作 / Actions ---- */

// 切到某个分段：读缓存；关注流或赛程没缓存就取一次。/ Switch segment: read caches; fetch once when missing.
static app_redraw_t show_seg(seg_t seg) {
    s_seg = seg;
    s_list_page = 0;
    s_msg[0] = 0;
    hupu_img_drop_pending();
    if (seg == SEG_SCORE) {
        load_schedule();
        if (!s_day_count) start_job(TOMATO_HUPU_SCHEDULE, LEAGUES[s_league].id, NULL, 0, false);
    } else {
        load_current_list();
        if (seg == SEG_FOLLOW && s_follow_count && !s_item_count)
            start_job(TOMATO_HUPU_FOLLOW, NULL, NULL, 0, false);
    }
    return APP_REDRAW_PAGE;
}

static app_redraw_t refresh(void) {
    if (s_seg == SEG_FEED) start_job(TOMATO_HUPU_HOME, NULL, NULL, 0, false);
    else if (s_seg == SEG_FOLLOW) {
        if (s_follow_count) start_job(TOMATO_HUPU_FOLLOW, NULL, NULL, 0, false);
    } else start_job(TOMATO_HUPU_SCHEDULE, LEAGUES[s_league].id, NULL, 0, false);
    return APP_REDRAW_PAGE;
}

static app_redraw_t open_post(const char* tid, view_t back) {
    hupu_img_drop_pending();
    s_post_back = back;
    s_view = V_POST;
    s_tab = T_BODY;
    s_page = 0;
    s_msg[0] = 0;
    // 缓存里有就直接看；有图还没下好就在后台补。/ A cached post opens at once; missing images are filled in behind it.
    if (load_post(tid)) {
        snprintf(s_tid, sizeof(s_tid), "%s", tid);
        if (post_missing_images()) start_job(TOMATO_HUPU_POST, tid, NULL, 0, true);
        return APP_REDRAW_PAGE;
    }
    snprintf(s_tid, sizeof(s_tid), "%s", tid);
    start_job(TOMATO_HUPU_POST, tid, NULL, 0, false);
    return APP_REDRAW_PAGE;
}

static app_redraw_t open_zones(void) {
    s_view = V_ZONES;
    s_msg[0] = 0;
    s_zone_page = 0;
    load_zones();
    if (!s_zone_count) start_job(TOMATO_HUPU_ZONES, NULL, NULL, 0, false);
    return APP_REDRAW_PAGE;
}

static app_redraw_t open_zone(const zone_t* z, view_t back) {
    snprintf(s_zone_id, sizeof(s_zone_id), "%s", z->id);
    snprintf(s_zone_name, sizeof(s_zone_name), "%s", z->name);
    s_zone_back = back;
    s_view = V_ZONE;
    s_list_page = 0;
    s_msg[0] = 0;
    hupu_img_drop_pending();
    load_zone_list();
    if (!s_item_count) start_job(TOMATO_HUPU_THREADS, s_zone_id, NULL, s_zone_sort, false);
    return APP_REDRAW_PAGE;
}

static app_redraw_t open_rate(int match) {
    const match_t* m = &s_matches[match];
    if (m->st != 2 || !m->k[0] || !m->n[0]) {
        snprintf(s_msg, sizeof(s_msg), "%s", m->st == 0 ? "比赛还没开始，没有评分" : "比赛结束后才有评分");
        return APP_REDRAW_PAGE;
    }
    s_rate_match = match;
    s_view = V_RATE;
    s_msg[0] = 0;
    s_rate_title[0] = 0;
    if (!load_rating()) {
        s_player_count = 0;
        start_job(TOMATO_HUPU_RATING, m->k, m->n, 0, false);
    }
    return APP_REDRAW_PAGE;
}

// 返回上一层；正在为这一层取数据就取消。/ Go back one level, cancelling a fetch for the level we leave.
static app_redraw_t go_back(void) {
    hupu_img_drop_pending();
    s_msg[0] = 0;
    switch (s_view) {
    case V_IMAGE:
    case V_REPLY: s_view = V_POST; break;
    case V_POST:
        drop_job(TOMATO_HUPU_POST);
        free_post();
        s_view = s_post_back;
        if (s_view == V_ZONE) load_zone_list();
        else load_current_list();
        break;
    case V_ZONE:
        drop_job(TOMATO_HUPU_THREADS);
        s_view = s_zone_back;
        s_list_page = 0;
        if (s_view == V_HOME) load_current_list();
        break;
    case V_ZONES: s_view = V_HOME; load_current_list(); break;
    case V_RATE:
        drop_job(TOMATO_HUPU_RATING);
        s_view = V_HOME;
        break;
    default: return APP_REDRAW_NONE;
    }
    return APP_REDRAW_PAGE;
}

static app_redraw_t turn(int dir) {
    int *page, pages;
    switch (s_view) {
    case V_HOME:
        if (s_seg == SEG_SCORE) { page = &s_day_page; pages = day_pages(); }
        else { page = &s_list_page; pages = list_pages(); }
        break;
    case V_ZONE: page = &s_list_page; pages = list_pages_shown(); break;
    case V_ZONES: page = &s_zone_page; pages = zone_pages(); break;
    case V_POST: if (!s_post_loaded) return APP_REDRAW_NONE; page = &s_page; pages = tab_pages(); break;
    case V_IMAGE: page = &s_image; pages = image_count(); break;
    case V_RATE: page = &s_rate_page; pages = rate_pages(); break;
    default: return APP_REDRAW_NONE;
    }
    const int next = *page + dir;
    if (next < 0 || next >= pages) return APP_REDRAW_NONE;
    *page = next;
    hupu_img_drop_pending();
    s_msg[0] = 0;
    // 专区翻到还没取的一页：取下一批。/ A zone page not fetched yet: fetch the next batch.
    if (s_view == V_ZONE && *page >= list_pages() && !s_list_end && !job_running(TOMATO_HUPU_THREADS))
        start_job(TOMATO_HUPU_THREADS, s_zone_id, NULL, s_zone_sort, true);
    return APP_REDRAW_PAGE;
}

/* ---- 触摸 / Touch ---- */

static app_redraw_t tap_home(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (tr_nav_touch(ctx, x, y, TR_TAB_HUPU)) return APP_REDRAW_NONE;
    if (ui_rect_hit(REFRESH_RECT, x, y) && !tomato_sync_busy()) return refresh();
    const int seg = tr_segment_hit(TR_SEGMENT_RECT, 3, x, y);
    if (seg >= 0) return seg == (int)s_seg ? APP_REDRAW_NONE : show_seg((seg_t)seg);
    const int pager = tr_pager_hit(PAGER_Y, x, y);
    if (pager) return turn(pager);
    if (s_seg == SEG_SCORE) {
        for (int i = 0; i < LEAGUE_COUNT; i++) {
            if (!ui_rect_hit(league_rect(i), x, y)) continue;
            if (i == s_league) return APP_REDRAW_NONE;
            s_league = i;
            return show_seg(SEG_SCORE);
        }
        if (y >= DATE_Y - 10 && y < DATE_Y + 50 && s_day_count) {
            const int next = s_day + (x < TR_W / 3 ? -1 : x >= TR_W * 2 / 3 ? 1 : 0);
            if (next == s_day || next < 0 || next >= s_day_count) return APP_REDRAW_NONE;
            s_day = next;
            s_day_page = 0;
            s_msg[0] = 0;
            return APP_REDRAW_PAGE;
        }
        if (s_day_count && y >= MATCH_TOP && y < MATCH_TOP + ROWS * MATCH_H) {
            const int k = s_day_page * ROWS + (y - MATCH_TOP) / MATCH_H;
            if (k < s_days[s_day].count) return open_rate(s_days[s_day].first + k);
        }
        return APP_REDRAW_NONE;
    }
    if (s_seg == SEG_FOLLOW) {
        if (!s_follow_count && ui_rect_hit((EpdRect){TR_MARGIN, 440, CW, 84}, x, y)) return open_zones();
        if (s_follow_count && y >= STATUS_Y - 10 && y < STATUS_Y + 40 && x > TR_W - TR_MARGIN - 140) return open_zones();
    }
    if (y >= LIST_TOP && y < LIST_TOP + ROWS * ROW_H) {
        const int i = s_list_page * ROWS + (y - LIST_TOP) / ROW_H;
        if (i < s_item_count) return open_post(s_items[i].tid, V_HOME);
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t tap_zones(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (tr_nav_touch(ctx, x, y, TR_TAB_HUPU)) return APP_REDRAW_NONE;
    if (y < 160 && x < 360) return go_back();
    const int pager = tr_pager_hit(PAGER_Y, x, y);
    if (pager) return turn(pager);
    for (int i = 0; i < s_cat_count; i++) {
        if (!ui_rect_hit(cat_rect(i, NULL), x, y)) continue;
        if (i == s_cat) return APP_REDRAW_NONE;
        s_cat = i;
        s_zone_page = 0;
        return APP_REDRAW_PAGE;
    }
    if (!s_cat_count) return APP_REDRAW_NONE;
    const int top = grid_top(), rows = grid_rows(), per = rows * 3;
    if (y < top || x < TR_MARGIN) return APP_REDRAW_NONE;
    const int col = (x - TR_MARGIN) / CELL_STEP_X, row = (y - top) / CELL_STEP_Y;
    if (col > 2 || row >= rows) return APP_REDRAW_NONE;
    const int i = s_zone_page * per + row * 3 + col;
    if (i >= s_cats[s_cat].count) return APP_REDRAW_NONE;
    return open_zone(&s_zones[s_cats[s_cat].first + i], V_ZONES);
}

static app_redraw_t tap_zone(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (tr_nav_touch(ctx, x, y, TR_TAB_HUPU)) return APP_REDRAW_NONE;
    if (y < 160 && x < 360) return go_back();
    if (ui_rect_hit((EpdRect){TR_W - TR_MARGIN - 160, 86, 160, 68}, x, y)) {
        toggle_follow(s_zone_id, s_zone_name);
        return APP_REDRAW_PAGE;
    }
    if (y >= 170 && y < 236) {
        if (x > TR_W - TR_MARGIN - 120) {
            { s_list_page = 0; start_job(TOMATO_HUPU_THREADS, s_zone_id, NULL, s_zone_sort, false); }
            return APP_REDRAW_PAGE;
        }
        const int sort = x < TR_MARGIN + 176 ? 0 : x < TR_MARGIN + 352 ? 1 : -1;
        if (sort < 0 || sort == s_zone_sort) return APP_REDRAW_NONE;
        s_zone_sort = sort;
        s_list_page = 0;
        s_msg[0] = 0;
        load_zone_list();
        if (!s_item_count) start_job(TOMATO_HUPU_THREADS, s_zone_id, NULL, s_zone_sort, false);
        return APP_REDRAW_PAGE;
    }
    const int pager = tr_pager_hit(PAGER_Y, x, y);
    if (pager) return turn(pager);
    if (s_list_page < list_pages() && y >= ZONE_LIST_TOP && y < ZONE_LIST_TOP + ROWS * ROW_H) {
        const int i = s_list_page * ROWS + (y - ZONE_LIST_TOP) / ROW_H;
        if (i < s_item_count) return open_post(s_items[i].tid, V_ZONE);
    }
    return APP_REDRAW_NONE;
}

// 正文页上点到的图片序号，没点到返回 -1。/ Index of the image tapped on a body page, or -1.
static int image_hit(uint16_t x, uint16_t y) {
    int yy = body_top(s_page), k = 0;
    const int start = s_body_pages[s_page];
    const int end = s_page + 1 < s_body_page_count ? s_body_pages[s_page + 1] : s_line_count;
    for (int b = 0; b < s_lines[start].block; b++) k += s_blocks[b].image;
    for (int i = start; i < end; i++) {
        const line_t* l = &s_lines[i];
        if (l->kind == 1) {
            const block_t* blk = &s_blocks[l->block];
            if (ui_rect_hit((EpdRect){TR_MARGIN + (CW - blk->w) / 2, yy, blk->w, blk->h}, x, y)) return k;
            k++;
        }
        if (l->kind != 2 || i != start) yy += l->h;
    }
    return -1;
}

static app_redraw_t tap_post(uint16_t x, uint16_t y) {
    if (y < 140 && x < 360) return go_back();
    if (!s_post_loaded) return APP_REDRAW_NONE;
    const int seg = tr_segment_hit(POST_SEG, 3, x, y);
    if (seg >= 0) {
        if (seg == (int)s_tab) return APP_REDRAW_NONE;
        s_tab = (post_tab_t)seg;
        s_page = 0;
        hupu_img_drop_pending();
        return APP_REDRAW_PAGE;
    }
    const int pager = tr_pager_hit(POST_PAGER_Y, x, y);
    if (pager) return turn(pager);
    if (y < BODY_TOP || y >= BODY_BOTTOM) return APP_REDRAW_NONE;
    if (s_tab == T_BODY) {
        if (s_line_count) {
            const int img = image_hit(x, y);
            if (img >= 0) { s_image = img; s_view = V_IMAGE; hupu_img_drop_pending(); return APP_REDRAW_PAGE; }
        }
        if (x < TR_W * 3 / 10) return turn(-1);
        if (x >= TR_W * 7 / 10) return turn(1);
        return APP_REDRAW_NONE;
    }
    const int which = s_tab == T_LIST;
    const reply_t* list = which ? s_list : s_lights;
    int yy = BODY_TOP;
    for (int i = s_reply_pages[which][s_page]; i < s_reply_pages[which][s_page + 1]; i++) {
        const int h = reply_height(&list[i]);
        if (y >= yy && y < yy + h) { s_reply_open = i; s_view = V_REPLY; return APP_REDRAW_PAGE; }
        yy += h;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t tap_rate(uint16_t x, uint16_t y) {
    if (y < 140 && x < 300) return go_back();
    if (s_rate_team && y >= 150 && y < 214) {
        const int team = tr_segment_hit((EpdRect){TR_MARGIN, 150, CW, 64}, 2, x, y) + 1;
        if (team <= 0 || team == s_rate_team) return APP_REDRAW_NONE;
        s_rate_team = team;
        s_rate_page = 0;
        return APP_REDRAW_PAGE;
    }
    const int pager = tr_pager_hit(POST_PAGER_Y, x, y);
    return pager ? turn(pager) : APP_REDRAW_NONE;
}

static app_redraw_t tap(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    switch (s_view) {
    case V_HOME: return tap_home(ctx, x, y);
    case V_ZONES: return tap_zones(ctx, x, y);
    case V_ZONE: return tap_zone(ctx, x, y);
    case V_POST: return tap_post(x, y);
    case V_REPLY: return go_back();
    case V_IMAGE:
        if (x < TR_W * 3 / 10) return turn(-1);
        if (x >= TR_W * 7 / 10) return turn(1);
        return go_back();
    case V_RATE: return tap_rate(x, y);
    }
    return APP_REDRAW_NONE;
}

// 轻点照旧；左滑 / 上滑下一页，右滑 / 下滑上一页。/ Taps as usual; swipes turn whole pages.
static app_redraw_t on_gesture(app_ctx_t* ctx, const ui_gesture_event_t* ev) {
    if (!s_items) {
        // 内存不足时只让标签栏能用，好离开这一页。/ Out of memory: only the tab bar works so the user can leave.
        if (ev->type == UI_GESTURE_TAP) tr_nav_touch(ctx, ev->x0, ev->y0, TR_TAB_HUPU);
        return APP_REDRAW_NONE;
    }
    if (ev->type == UI_GESTURE_TAP) return tap(ctx, ev->x0, ev->y0);
    if (ev->type == UI_GESTURE_SWIPE_L || ev->type == UI_GESTURE_SWIPE_U) return turn(1);
    if (ev->type == UI_GESTURE_SWIPE_R || ev->type == UI_GESTURE_SWIPE_D) return turn(-1);
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key(app_ctx_t* ctx, int key) {
    (void)ctx;
    if (key != UI_KEY_1 || !s_items) return APP_REDRAW_NONE;
    return go_back();
}

/* ---- 后台进度 / Background progress ---- */

static void queue_visible_images(void) {
    if (s_view == V_HOME && s_seg == SEG_FEED) {
        for (int row = 0; row < ROWS; row++) {
            const int i = s_list_page * ROWS + row;
            if (i < s_item_count && s_items[i].jpg[0]) hupu_img_get(s_items[i].jpg, THUMB_W, THUMB_H);
        }
    } else if (s_view == V_POST && s_post_loaded && s_tab == T_BODY) {
        const int start = s_body_pages[s_page];
        const int end = s_page + 1 < s_body_page_count ? s_body_pages[s_page + 1] : s_line_count;
        for (int i = start; i < end; i++)
            if (s_lines[i].kind == 1) {
                const block_t* b = &s_blocks[s_lines[i].block];
                hupu_img_get(b->jpg, b->w, b->h);
            }
    }
}

// 任务结束：把结果读进当前视图。返回要不要刷新。/ Job finished: load results into the view; returns the redraw.
static app_redraw_t job_done(bool ok) {
    const tomato_hupu_req_t* r = &s_job;
    switch (r->job) {
    case TOMATO_HUPU_HOME:
    case TOMATO_HUPU_FOLLOW:
        if (s_view == V_HOME && ((r->job == TOMATO_HUPU_HOME) == (s_seg == SEG_FEED)) && s_seg != SEG_SCORE) {
            load_current_list();
            if (ok) { queue_visible_images(); if (hupu_img_pending()) return APP_REDRAW_NONE; }
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    case TOMATO_HUPU_POST:
        if (s_view != V_POST || strcmp(r->id, s_tid)) return APP_REDRAW_NONE;
        if (!s_post_loaded) load_post(s_tid);
        if (ok) { queue_visible_images(); if (hupu_img_pending()) return APP_REDRAW_NONE; }
        return APP_REDRAW_PAGE;
    case TOMATO_HUPU_ZONES:
        load_zones();
        return s_view == V_ZONES ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
    case TOMATO_HUPU_THREADS:
        if (s_view != V_ZONE || strcmp(r->id, s_zone_id) || r->sort != s_zone_sort) return APP_REDRAW_NONE;
        load_zone_list();
        return APP_REDRAW_PAGE;
    case TOMATO_HUPU_SCHEDULE:
        // 只在比分页才重读：评分页正用着赛程里的那场比赛。/ Reload only on the score view; the rating view still uses a
        // match from the current schedule.
        if (strcmp(r->id, LEAGUES[s_league].id) || !(s_view == V_HOME && s_seg == SEG_SCORE)) return APP_REDRAW_NONE;
        load_schedule();
        return APP_REDRAW_PAGE;
    case TOMATO_HUPU_RATING:
        if (s_view != V_RATE) return APP_REDRAW_NONE;
        load_rating();
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_tick(app_ctx_t* ctx) {
    if (!s_items) return APP_REDRAW_NONE;
    if (!(ctx->touch && ctx->touch->touched) && hupu_img_tick()) return APP_REDRAW_PAGE;
    if (ctx->now_ms - s_poll_ms < POLL_MS) return APP_REDRAW_NONE;
    s_poll_ms = ctx->now_ms;
    tr_time_poll();
    // 记下的请求：后台一空就开始。/ A remembered request starts as soon as the worker is free.
    if (s_has_pending && !tomato_sync_busy() && !s_job_active) {
        s_has_pending = false;
        const tomato_hupu_req_t r = s_pending;
        start_req(&r);
        return APP_REDRAW_PAGE;
    }
    if (s_has_pending && !s_job_active && tomato_sync_preemptible()) tomato_sync_cancel();
    if (!s_job_active) return APP_REDRAW_NONE;
    // 序号变了：自己的任务早结束了，后台在跑别人的；不再跟踪。/ The sequence moved on: stop tracking.
    if (tomato_sync_seq() != s_job_seq) { s_job_active = false; return APP_REDRAW_NONE; }
    // 先看是否还在忙，再取状态，免得拿到失败前的旧状态。/ Read busy first, then the status, so a failure is never missed.
    const bool busy = tomato_sync_busy();
    tomato_sync_get_status(&s_status);
    // 推荐和帖子分两步：第 1 步（文字）写好就先显示。/ Feed and posts have two steps: show step 1 (text) as soon as ready.
    if (busy && !s_step_loaded && s_status.current >= 1 && s_status.phase != TOMATO_FAILED &&
        (s_job.job == TOMATO_HUPU_HOME || s_job.job == TOMATO_HUPU_POST)) {
        s_step_loaded = true;
        if (s_job.job == TOMATO_HUPU_HOME && s_view == V_HOME && s_seg == SEG_FEED) {
            s_list_page = 0;
            load_current_list();
            return APP_REDRAW_PAGE;
        }
        if (s_job.job == TOMATO_HUPU_POST && s_view == V_POST && !strcmp(s_job.id, s_tid) && load_post(s_tid)) {
            s_page = 0;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (busy) return APP_REDRAW_NONE;
    s_job_active = false;
    const bool ok = s_status.phase != TOMATO_FAILED;
    // 自己取消的（返回上一层）不提示；别的失败留一句话，缓存照常显示。
    // A cancel we caused (going back) needs no message; other failures leave a line while caches still show.
    if (!ok) {
        if (s_status.error == ESP_ERR_INVALID_STATE) s_msg[0] = 0;
        else snprintf(s_msg, sizeof(s_msg), "%s", s_status.message);
    }
    // 旧任务被新请求顶掉了：不提示，马上开始新的。/ The old job gave way to a newer request: no message, start it now.
    if (s_has_pending) {
        if (ok) job_done(ok);
        s_has_pending = false;
        const tomato_hupu_req_t next = s_pending;
        start_req(&next);
        return APP_REDRAW_PAGE;
    }
    const app_redraw_t r = job_done(ok);
    return !ok && r == APP_REDRAW_NONE && s_view != V_IMAGE ? APP_REDRAW_PAGE : r;
}

static void on_enter(app_ctx_t* ctx) {
    (void)ctx;
    hupu_img_drop_pending();
    if (!alloc_all()) return;
    load_follows();
    s_msg[0] = 0;
    // 每次进来都从首页当前分段开始（大块数据离页时已释放）。/ Always start at the home segment (buffers were freed).
    s_view = V_HOME;
    s_list_page = 0;
    if (s_seg == SEG_SCORE) load_schedule();
    else load_current_list();
    // 后台任务还在跑就继续跟踪，结束时把结果读进来。/ Keep tracking a running job and load its result when it ends.
    if (s_job_active && !ours_running()) s_job_active = false;
}

static void page_on_exit(app_ctx_t* ctx) {
    (void)ctx;
    s_has_pending = false;   // 回来时从首页开始，旧请求不再需要 / We come back at home; the old request is moot
    free_all();
}

static void on_media_lost(app_ctx_t* ctx) {
    (void)ctx;
    tomato_sync_cancel();
    s_job_active = false;
    s_has_pending = false;
    free_all();
    alloc_all();
    s_view = V_HOME;
}

const app_desc_t app_tr_hupu = {
    .title = "虎扑",
    .detail = "推荐、关注、比分（只读）",
    .enter_full = true,
    .tab_nav = true,
    .render = render,
    .on_enter = on_enter,
    .on_exit = page_on_exit,
    .on_media_lost = on_media_lost,
    .on_gesture = on_gesture,
    .on_key = on_key,
    .on_tick = on_tick,
};
