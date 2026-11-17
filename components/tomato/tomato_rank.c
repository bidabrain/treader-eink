/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 书城榜单：地址、缓存文件和解析。数据来自 FanqieRankTracker 网站（index.json 列分类，每个分类一个文件）。
 * 联网下载由 tomato_sync 的后台任务做，这里只读写 TF 卡上的缓存。
 *
 * Bookstore ranks: URLs, cache files and parsing. Data comes from a FanqieRankTracker site (index.json lists
 * categories, one file per category). The sync task downloads; this file only reads and writes the TF cache.
 *
 * 冻结：页面只读缓存，不在主任务联网；榜单书只取书名、作者、在读、简介、封面和 book_id。
 * Frozen: pages read the cache only and never network on the main task; rank books keep only title, author,
 * reads, intro, cover and book_id.
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "tomato_builtin.h"
#include "tomato_internal.h"

#define TAG "tomato_rank"
#define INDEX_PATH TOMATO_RANK_DIR "/index.json"
#define DEFAULT_STORE_URL "https://bidabrain.github.io/FanqieRankTracker"
#define FILE_MAX (256 * 1024)

/* ---- 地址 / URLs ---- */

// 去掉末尾斜杠；填的是 .../api/... 的 JSON 地址就截到站点根。/ Strip slashes; cut a .../api/... JSON URL to the site root.
static void normalize(char* url) {
    char* api = strstr(url, "/api/");
    if (api) *api = 0;
    size_t n = strlen(url);
    while (n && url[n - 1] == '/') url[--n] = 0;
}

void tomato_rank_base(char* out, size_t cap) {
    out[0] = 0;
    tomato_store_url_load(out, cap);
    if (!out[0]) snprintf(out, cap, "%s", tomato_store_url_default());
    normalize(out);
}

const char* tomato_store_url_default(void) {
#ifdef TOMATO_STORE_DEFAULT
    if (TOMATO_STORE_DEFAULT[0]) return TOMATO_STORE_DEFAULT;
#endif
    return DEFAULT_STORE_URL;
}

bool tomato_rank_index_url(char* out, size_t cap) {
    char base[200];
    tomato_rank_base(base, sizeof(base));
    return snprintf(out, cap, "%s/api/lastest/index.json", base) < (int)cap;
}

bool tomato_rank_cat_url(const tomato_rank_cat_t* cat, char* out, size_t cap) {
    if (!strncmp(cat->url, "http://", 7) || !strncmp(cat->url, "https://", 8))
        return snprintf(out, cap, "%s", cat->url) < (int)cap;
    char base[200];
    tomato_rank_base(base, sizeof(base));
    return snprintf(out, cap, "%s/%s", base, cat->url[0] == '/' ? cat->url + 1 : cat->url) < (int)cap;
}

/* ---- 缓存文件 / Cache files ---- */

static uint32_t fnv(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

const char* tomato_rank_index_path(void) { return INDEX_PATH; }

void tomato_rank_cat_path(const tomato_rank_cat_t* cat, char* out, size_t cap) {
    snprintf(out, cap, TOMATO_RANK_DIR "/c_%08lx.json", (unsigned long)fnv(cat->url));
}

void tomato_rank_cover_path(const char* book_id, char* out, size_t cap) {
    snprintf(out, cap, TOMATO_RANK_DIR "/covers/%.23s.jpg", book_id);
}

bool tomato_rank_cat_cached(const tomato_rank_cat_t* cat) {
    char path[96];
    struct stat st;
    tomato_rank_cat_path(cat, path, sizeof(path));
    return stat(path, &st) == 0 && st.st_size > 0;
}

// 删掉 dir 里名字以 prefix 开头的文件。/ Delete files in dir whose names start with prefix.
static void remove_matching(const char* dir, const char* prefix) {
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    char path[320];
    const size_t n = strlen(prefix);
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || strncmp(e->d_name, prefix, n)) continue;
        snprintf(path, sizeof(path), "%.60s/%.255s", dir, e->d_name);
        remove(path);
    }
    closedir(d);
}

// 榜单换了日期就删掉旧的分类缓存和详情页封面，书城缓存不会一直变大。
// When the rank date changes, delete old category caches and detail covers so the store cache stays bounded.
void tomato_rank_drop_categories(void) {
    remove_matching(TOMATO_RANK_DIR, "c_");
    remove_matching(TOMATO_RANK_DIR "/covers", "");
}

static cJSON* read_json_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    cJSON* root = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long n = ftell(f);
        char* buf = n > 0 && n <= FILE_MAX && fseek(f, 0, SEEK_SET) == 0
                        ? heap_caps_malloc((size_t)n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
        if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) {
            buf[n] = 0;
            tomato_json_use_psram();
            root = cJSON_ParseWithLength(buf, (size_t)n);
        }
        heap_caps_free(buf);
    }
    fclose(f);
    return root;
}

bool tomato_rank_index_date(char* out, size_t cap) {
    out[0] = 0;
    cJSON* root = read_json_file(INDEX_PATH);
    if (!root) return false;
    tomato_json_str(root, "date", out, cap);
    cJSON_Delete(root);
    return out[0] != 0;
}

/* ---- 解析 / Parsing ---- */

int tomato_rank_load_index(tomato_rank_cat_t* out, int max, char* date, size_t date_cap) {
    if (date && date_cap) date[0] = 0;
    cJSON* root = read_json_file(INDEX_PATH);
    if (!root) return -1;
    if (date) tomato_json_str(root, "date", date, date_cap);
    int n = 0;
    const cJSON* it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "types")) {
        if (n >= max) break;
        tomato_rank_cat_t* c = &out[n];
        memset(c, 0, sizeof(*c));
        char channel[12];
        tomato_json_str(it, "type", c->name, sizeof(c->name));
        tomato_json_str(it, "channel", channel, sizeof(channel));
        // 地址截断就不能用，跳过。/ A truncated URL is useless; skip it.
        const cJSON* url = cJSON_GetObjectItemCaseSensitive(it, "url");
        if (!cJSON_IsString(url) || !url->valuestring || strlen(url->valuestring) >= sizeof(c->url)) continue;
        strcpy(c->url, url->valuestring);
        // “all” 是全量文件（1 MB），设备不用。/ "all" is the 1 MB full file the device skips.
        if (!c->name[0] || !strcmp(c->name, "all") || !channel[0]) continue;
        c->male = !strcmp(channel, "male");
        n++;
    }
    cJSON_Delete(root);
    return n;
}

// 番茄链接 .../page/<book_id>。/ Tomato link .../page/<book_id>.
static void book_id_from_url(const char* url, char* out, size_t cap) {
    out[0] = 0;
    const char* p = strstr(url, "/page/");
    if (!p) return;
    p += 6;
    size_t n = 0;
    while (p[n] >= '0' && p[n] <= '9' && n + 1 < cap) { out[n] = p[n]; n++; }
    out[n] = 0;
}

int tomato_rank_load_books(const tomato_rank_cat_t* cat, tomato_rank_book_t* out, int max) {
    char path[96];
    tomato_rank_cat_path(cat, path, sizeof(path));
    cJSON* root = read_json_file(path);
    if (!root) return -1;
    // 分类文件可能把书放在 books，也可能放在 categories[0].books。/ Books may sit in books or categories[0].books.
    const cJSON* books = cJSON_GetObjectItemCaseSensitive(root, "books");
    if (!cJSON_IsArray(books)) {
        const cJSON* first = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "categories"), 0);
        books = first ? cJSON_GetObjectItemCaseSensitive(first, "books") : NULL;
    }
    int n = 0;
    const cJSON* it;
    cJSON_ArrayForEach(it, books) {
        if (n >= max) break;
        tomato_rank_book_t* b = &out[n];
        memset(b, 0, sizeof(*b));
        char url[160];
        tomato_json_str(it, "url", url, sizeof(url));
        book_id_from_url(url, b->book_id, sizeof(b->book_id));
        tomato_json_str(it, "title", b->title, sizeof(b->title));
        tomato_json_str(it, "author", b->author, sizeof(b->author));
        tomato_json_str(it, "reads", b->reads, sizeof(b->reads));
        tomato_json_str(it, "intro", b->intro, sizeof(b->intro));
        const cJSON* cover = cJSON_GetObjectItemCaseSensitive(it, "cover");
        if (cJSON_IsString(cover) && cover->valuestring && strlen(cover->valuestring) < sizeof(b->cover))
            strcpy(b->cover, cover->valuestring);
        if (b->book_id[0] && b->title[0]) n++;
    }
    cJSON_Delete(root);
    return n;
}
