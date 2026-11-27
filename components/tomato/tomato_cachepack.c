/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：把小红书、微博、X 的帖子缓存（每个帖子目录里的 JSON、原图、缩略图和灰度缓存）放进缓存包，并按列表判断
 * 回收时留什么，规则和原来的目录清理一样：列表里有的留正文和缩略图、有空间再留大图，正在看的全留，不在列表里的
 * 先扔。列表文件（feed.json 这类）照旧是普通文件。缓存文件的读写入口也在这里：归缓存包管的路径进包，其余照旧读写
 * 文件。
 *
 * English: Puts the Xiaohongshu, Weibo and X post caches (each post folder's JSON, full images, thumbnails and gray
 * caches) into the cache pack and judges what collection keeps, with the old folder pruning's rules: listed items
 * keep their text and thumbnails and their full images while there is room, the open item keeps everything, and
 * unlisted items go first. List files (feed.json and the like) stay ordinary files. The cache file entry points live
 * here too: paths the pack owns go into the pack, everything else is still read and written as files.
 *
 * 冻结：只放能重新下载的缓存；列表读坏了这一轮不回收（宁可多占也不误删）。
 * Frozen: only re-downloadable caches go in; a list that fails to read stops this round's collection (better to hold
 * more than delete by mistake).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cache_pack.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "tomato.h"
#include "tomato_internal.h"

#define TAG "tomato_pack"
#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
// 每个来源的缓存上限。原来目录清理是 200 MB；索引在 PSRAM 里按条数算，所以少一点。/ Cache cap per source. The old
// folder pruning allowed 200 MB; the index sits in PSRAM per record, so a little less.
#define POOL_CAP (128ull * 1024 * 1024)
#define RULES_MAX 2
#define LISTS_MAX 2

typedef struct {
    const char* sub;                    // key 里这一类的开头，例如 "n/" / This kind's key start, e.g. "n/"
    const char* lists[LISTS_MAX];       // 判断“在不在列表里”的列表文件 / List files deciding "listed"
    bool dirs;                          // true：sub 后面是“编号/文件名”；false：文件名本身以编号开头
                                        // true: sub is followed by "id/file"; false: the file name starts with the id
} rule_t;

typedef struct {
    cpack_pool_cfg_t cfg;
    const char* prune_root;             // tomato_cache_prune 传进来的目录 / The folder tomato_cache_prune is given
    rule_t rules[RULES_MAX];
    char keep[40];                      // 正在看的那条（缓存锁保护）/ The open item (under the cache lock)
} glue_t;

typedef struct {
    const glue_t* g;
    cJSON* roots[RULES_MAX][LISTS_MAX];
    char keep[40];
} gc_ctx_t;

static void* gc_begin(void* user);
static cpack_live_t gc_live(void* ctx, const char* key);
static void gc_end(void* ctx);

static const char* const RED_OLD[] = {TOMATO_RED_DIR "/n", TOMATO_RED_DIR "/rc", NULL};
static const char* const WB_OLD[] = {TOMATO_WB_DIR "/p", NULL};
static const char* const X_OLD[] = {TOMATO_X_DIR "/p", NULL};

static glue_t s_glue[] = {
    {
        .cfg = {.name = "red", .prefixes = {TOMATO_RED_DIR "/n/", TOMATO_RED_DIR "/rc/", NULL}, .root = TOMATO_RED_DIR "/",
                .cap_bytes = POOL_CAP, .old_dirs = RED_OLD},
        .prune_root = TOMATO_RED_DIR "/n",
        .rules = {{.sub = "n/", .lists = {TOMATO_RED_FEED_FILE, TOMATO_RED_REC_FILE}, .dirs = true},
                  {.sub = "rc/", .lists = {TOMATO_RED_REC_FILE, NULL}, .dirs = false}},
    },
    {
        .cfg = {.name = "wb", .prefixes = {TOMATO_WB_DIR "/p/", NULL}, .root = TOMATO_WB_DIR "/", .cap_bytes = POOL_CAP,
                .old_dirs = WB_OLD},
        .prune_root = TOMATO_WB_DIR "/p",
        .rules = {{.sub = "p/", .lists = {TOMATO_WB_FOLLOW_FILE, TOMATO_WB_HOT_FILE}, .dirs = true}},
    },
    {
        .cfg = {.name = "x", .prefixes = {TOMATO_X_DIR "/p/", NULL}, .root = TOMATO_X_DIR "/", .cap_bytes = POOL_CAP,
                .old_dirs = X_OLD},
        .prune_root = TOMATO_X_DIR "/p",
        .rules = {{.sub = "p/", .lists = {TOMATO_X_FORYOU_FILE, TOMATO_X_FOLLOWING_FILE}, .dirs = true}},
    },
};

#define GLUE_COUNT (sizeof(s_glue) / sizeof(s_glue[0]))

static bool s_ready;

void tomato_cache_pack_setup(uint32_t (*generation)(void), uint64_t (*free_bytes)(void)) {
    if (s_ready) return;
    if (cpack_init("/sdcard/.treader/cache", "/sdcard/.treader/trash") != ESP_OK) return;
    cpack_set_generation_fn(generation);
    cpack_set_free_fn(free_bytes);
    for (size_t i = 0; i < GLUE_COUNT; i++) {
        glue_t* g = &s_glue[i];
        g->cfg.gc_begin = gc_begin;
        g->cfg.live = gc_live;
        g->cfg.gc_end = gc_end;
        g->cfg.user = g;
        if (cpack_register(&g->cfg) != ESP_OK) ESP_LOGW(TAG, "register %s failed", g->cfg.name);
    }
    s_ready = true;
}

/* ---- 回收判断 / Collection judgement ---- */

static void* gc_begin(void* user) {
    const glue_t* g = user;
    gc_ctx_t* c = heap_caps_calloc(1, sizeof(*c), PSRAM_CAPS);
    if (!c) return NULL;
    c->g = g;
    bool ok = true;
    // 列表在缓存锁里读：写列表的 remove 和 rename 之间读到“没有”会把所有条目当成不在列表里。
    // Read the lists under the cache lock: a read between a writer's remove and rename would see no list at all.
    tomato_hupu_cache_lock();
    for (int r = 0; ok && r < RULES_MAX && g->rules[r].sub; r++) {
        for (int k = 0; ok && k < LISTS_MAX && g->rules[r].lists[k]; k++) {
            struct stat st;
            if (stat(g->rules[r].lists[k], &st) != 0) continue;   // 还没有这个列表 / No such list yet
            c->roots[r][k] = tomato_cache_read_json(g->rules[r].lists[k]);
            ok = c->roots[r][k] != NULL;
        }
    }
    snprintf(c->keep, sizeof(c->keep), "%s", g->keep);
    tomato_hupu_cache_unlock();
    if (!ok) {
        ESP_LOGW(TAG, "%s: a list failed to read, skip collection", g->cfg.name);
        gc_end(c);
        return NULL;
    }
    return c;
}

static void gc_end(void* ctx) {
    gc_ctx_t* c = ctx;
    if (!c) return;
    for (int r = 0; r < RULES_MAX; r++)
        for (int k = 0; k < LISTS_MAX; k++) cJSON_Delete(c->roots[r][k]);
    heap_caps_free(c);
}

static bool listed(const gc_ctx_t* c, int r, const char* id) {
    for (int k = 0; k < LISTS_MAX; k++) {
        const cJSON* it;
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(c->roots[r][k], "items")) {
            const cJSON* v = cJSON_GetObjectItemCaseSensitive(it, "id");
            if (cJSON_IsString(v) && !strcmp(v->valuestring, id)) return true;
        }
    }
    return false;
}

// 原来 strip_images 留下的那些：正文 .json、封面缩略图 t.* / t_*、列表卡片的小方图 s1.jpg / s1_宽x高.g。
// What strip_images used to keep: text .json, cover thumbnails t.* / t_*, list-card squares s1.jpg / s1_WxH.g.
static bool small_file(const char* name) {
    const size_t n = strlen(name);
    const bool square = name[0] == 's' && name[1] >= '0' && name[1] <= '9' && (name[2] == '.' || name[2] == '_');
    return (n > 5 && !strcmp(name + n - 5, ".json")) || !strncmp(name, "t.", 2) || !strncmp(name, "t_", 2) || square;
}

static cpack_live_t gc_live(void* ctx, const char* key) {
    const gc_ctx_t* c = ctx;
    for (int r = 0; r < RULES_MAX && c->g->rules[r].sub; r++) {
        const rule_t* rule = &c->g->rules[r];
        const size_t n = strlen(rule->sub);
        if (strncmp(key, rule->sub, n)) continue;
        const char* rest = key + n;
        char id[48];
        if (rule->dirs) {
            const char* slash = strchr(rest, '/');
            if (!slash || slash == rest || (size_t)(slash - rest) >= sizeof(id)) return CPACK_LIVE_DROP;
            memcpy(id, rest, (size_t)(slash - rest));
            id[slash - rest] = 0;
            if (c->keep[0] && !strcmp(id, c->keep)) return CPACK_LIVE_KEEP;
            if (!listed(c, r, id)) return CPACK_LIVE_DROP;
            return small_file(slash + 1) ? CPACK_LIVE_KEEP : CPACK_LIVE_ROOM;
        }
        // 封面 id.jpg 和它的灰度缓存 id_120x160.g 都按 id 判断。/ Both id.jpg and its gray cache id_120x160.g go by id.
        snprintf(id, sizeof(id), "%.40s", rest);
        id[strcspn(id, "._")] = 0;
        return id[0] && listed(c, r, id) ? CPACK_LIVE_KEEP : CPACK_LIVE_DROP;
    }
    return CPACK_LIVE_DROP;
}

bool tomato_pack_prune(const char* root, const char* keep) {
    if (!s_ready) return false;
    for (size_t i = 0; i < GLUE_COUNT; i++) {
        glue_t* g = &s_glue[i];
        if (strcmp(root, g->prune_root)) continue;
        tomato_hupu_cache_lock();
        snprintf(g->keep, sizeof(g->keep), "%s", keep ? keep : "");
        tomato_hupu_cache_unlock();
        cpack_gc_kick();
        return true;
    }
    // 包里的其他目录（推荐封面）：不用记正在看的，只提醒回收。/ Other packed folders (recommendation covers): nothing
    // open to note, just nudge collection.
    char probe[160];
    snprintf(probe, sizeof(probe), "%s/x", root);
    if (!cpack_owns(probe)) return false;
    cpack_gc_kick();
    return true;
}

/* ---- 读写入口 / Access entry points ---- */

void tomato_cache_mkdir(const char* dir) {
    char probe[160];
    snprintf(probe, sizeof(probe), "%s/x", dir);
    if (!cpack_owns(probe)) mkdir(dir, 0775);
}

bool tomato_cache_size(const char* path, size_t* len) {
    if (cpack_owns(path)) return cpack_size(path, len);
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size < 0) return false;
    if (len) *len = (size_t)st.st_size;
    return true;
}

esp_err_t tomato_cache_load(const char* path, size_t max, uint8_t** out, size_t* len) {
    *out = NULL;
    *len = 0;
    if (cpack_owns(path)) return cpack_get(path, max, out, len);
    FILE* f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    esp_err_t err = ESP_OK;
    uint8_t* buf = NULL;
    if (n < 0 || (size_t)n > max) err = ESP_ERR_INVALID_SIZE;
    else if (!(buf = heap_caps_malloc((size_t)n + 1, PSRAM_CAPS))) err = ESP_ERR_NO_MEM;
    else if (fread(buf, 1, (size_t)n, f) != (size_t)n) err = ESP_FAIL;
    fclose(f);
    if (err != ESP_OK) { heap_caps_free(buf); return err; }
    buf[n] = 0;
    *out = buf;
    *len = (size_t)n;
    return ESP_OK;
}

esp_err_t tomato_cache_store2(const char* path, const void* a, size_t alen, const void* b, size_t blen) {
    if (cpack_owns(path)) return cpack_put2(path, a, alen, b, blen);
    char tmp[160];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = fopen(tmp, "wb");
    if (!f) return ESP_FAIL;
    bool ok = (!alen || fwrite(a, 1, alen, f) == alen) && (!blen || fwrite(b, 1, blen, f) == blen);
    ok = fclose(f) == 0 && ok;
    remove(path);
    if (!ok || rename(tmp, path) != 0) { remove(tmp); return ESP_FAIL; }
    return ESP_OK;
}

// 从 JPEG 开头的一段里找 SOF 读宽高；buf 不够长返回 false。/ Find the SOF in a JPEG's leading bytes and read the size;
// false when buf is too short.
static bool sof_in(const uint8_t* b, size_t n, int* w, int* h) {
    if (n < 4 || b[0] != 0xFF || b[1] != 0xD8) return false;
    size_t at = 2;
    for (int guard = 0; guard < 64 && at + 4 <= n; guard++) {
        if (b[at] != 0xFF) return false;
        while (at < n && b[at] == 0xFF) at++;
        if (at + 3 > n) return false;
        const uint8_t c = b[at++];
        const size_t len = (size_t)b[at] << 8 | b[at + 1];
        if ((c >= 0xC0 && c <= 0xC3) || (c >= 0xC5 && c <= 0xC7) || (c >= 0xC9 && c <= 0xCB)) {
            if (at + 7 > n) return false;
            *h = b[at + 3] << 8 | b[at + 4];
            *w = b[at + 5] << 8 | b[at + 6];
            return *w > 0 && *h > 0;
        }
        if (len < 2) return false;
        at += len;
    }
    return false;
}

bool tomato_cache_jpeg_size(const char* path, int* w, int* h) {
    // 服务器出的是基线 JPEG，SOF 在开头不远；开头一段找不到再整张读。/ The server writes baseline JPEGs with the SOF
    // near the start; read the whole file only when the head does not have it.
    enum { HEAD = 4096 };
    uint8_t* head = heap_caps_malloc(HEAD, PSRAM_CAPS);
    if (!head) return false;
    size_t got = 0;
    bool ok = false;
    if (cpack_owns(path)) {
        ok = cpack_read_head(path, head, HEAD, &got) == ESP_OK && sof_in(head, got, w, h);
    } else {
        FILE* f = fopen(path, "rb");
        if (f) { got = fread(head, 1, HEAD, f); fclose(f); ok = sof_in(head, got, w, h); }
    }
    heap_caps_free(head);
    if (ok || got < HEAD) return ok;
    uint8_t* all = NULL;
    size_t len = 0;
    if (tomato_cache_load(path, TOMATO_SRV_IMG_MAX, &all, &len) == ESP_OK) ok = sof_in(all, len, w, h);
    heap_caps_free(all);
    return ok;
}
