/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * WEBTOON 漫画的数据层：只连用户自己部署的 webtoon 服务器（仓库 webtoon/ 目录），不直接访问 webtoons.com。发现页列表、
 * 搜索结果、作品详情和封面缓存在 TF 卡 webtoon/；订阅存在我的漫画里每部作品文件夹里的 .webtoon 标记文件，下载的话
 * 存成同一个文件夹里的 CBZ，我的漫画照常显示和阅读。服务器不保存订阅：检查更新时把所有标记里的作品和已下载到第几话
 * 发过去，服务器回哪些话是新的。
 *
 * English: WEBTOON comic data layer. It talks only to the user's own webtoon server (the repo's webtoon/ directory),
 * never to webtoons.com itself. Store lists, search results, title details and covers are cached under webtoon/ on
 * the TF card; subscriptions live in a .webtoon marker file in each title's folder on the comic shelf, and downloaded
 * episodes are CBZ files in that same folder, shown and read by the comic shelf as usual. The server keeps no
 * subscriptions: an update check sends every marked title and its last downloaded episode, and the server answers
 * which episodes are new.
 *
 * 冻结：只在后台同步任务里联网，由页面按用户操作启动；地址和令牌存在 NVS 的 webtoon 命名空间，不写 TF 卡、不打日志。
 * 只写 comics/ 下带标记的文件夹和 webtoon/ 缓存，不动本地导入的漫画。下载先写 .part，完整了才改名。
 * Frozen: network only on the sync task, started by pages on user actions; the address and token live in NVS
 * namespace webtoon, never on the TF card or in logs. Only marked folders under comics/ and the webtoon/ cache are
 * written; locally imported comics are never touched. Downloads write .part and rename only when complete.
 */

#include <dirent.h>
#include <limits.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "tomato_internal.h"

#define NVS_NS "webtoon"
#define HEADER "x-webtoon-token"
#define TIMEOUT_MS 20000
#define CBZ_MAX (60u * 1024u * 1024u)
// 一话转换最多等这么久（服务器下载原图、切页）。/ The longest wait for one episode's conversion on the server.
#define CONVERT_MAX_MS (6 * 60 * 1000)
#define POLL_GAP_MS 2500
#define RETRY_WAIT_MS 10000
#define TITLES_MAX 200
#define PATH_LEN 320
// 一次检查最多记这么多条服务器回的话号，用来推进 last。/ Episode numbers from one check kept for moving last forward.
#define SEEN_MAX 4096

/* ---- 设置 / Settings ---- */

static uint32_t s_revision;

esp_err_t tomato_wt_config_load(tomato_wt_config_t* out) { return tomato_srv_config_load(NVS_NS, out); }

esp_err_t tomato_wt_config_save(const tomato_wt_config_t* cfg) {
    const esp_err_t err = tomato_srv_config_save(NVS_NS, cfg);
    if (err == ESP_OK) s_revision++;
    return err;
}

uint32_t tomato_wt_config_revision(void) { return s_revision; }

static int get_u8(const char* key, int def) {
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return def;
    const esp_err_t err = nvs_get_u8(h, key, &v);
    nvs_close(h);
    return err == ESP_OK ? v : def;
}

static esp_err_t set_u8(const char* key, int v) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, key, (uint8_t)v);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static const char* const LANG_CODES[TOMATO_WT_LANGS] = {"zh-hant", "en"};
static const char* const LANG_NAMES[TOMATO_WT_LANGS] = {"繁体中文", "English"};

int tomato_wt_lang(void) {
    const int v = get_u8("lang", 0);
    return v >= 0 && v < TOMATO_WT_LANGS ? v : 0;
}
esp_err_t tomato_wt_lang_set(int lang) { return lang >= 0 && lang < TOMATO_WT_LANGS ? set_u8("lang", lang) : ESP_ERR_INVALID_ARG; }
const char* tomato_wt_lang_code(int lang) { return LANG_CODES[lang >= 0 && lang < TOMATO_WT_LANGS ? lang : 0]; }
const char* tomato_wt_lang_name(int lang) { return LANG_NAMES[lang >= 0 && lang < TOMATO_WT_LANGS ? lang : 0]; }
bool tomato_wt_simp(void) { return get_u8("simp", 1) != 0; }
esp_err_t tomato_wt_simp_set(bool on) { return set_u8("simp", on ? 1 : 0); }

int tomato_wt_max(void) {
    const int v = get_u8("max", 10);
    return v == 5 || v == 20 || v == 50 ? v : 10;
}
esp_err_t tomato_wt_max_set(int n) {
    return n == 5 || n == 10 || n == 20 || n == 50 ? set_u8("max", n) : ESP_ERR_INVALID_ARG;
}

#define SRC_MAX 24

void tomato_wt_src(char* out, size_t cap) {
    nvs_handle_t h;
    size_t n = cap;
    if (!cap) return;
    out[0] = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "src", out, &n) != ESP_OK) out[0] = 0;
        nvs_close(h);
    }
    if (!out[0]) snprintf(out, cap, "%s", TOMATO_WT_SRC_WEBTOON);
}

esp_err_t tomato_wt_src_set(const char* key) {
    if (!key || !key[0] || strlen(key) >= SRC_MAX) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "src", key);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

bool tomato_wt_is_ext(const char* id) { return id && id[0] == 'm'; }

// 请求里共用的 lang 和 zh 参数。/ The lang and zh query parameters shared by requests.
static void query(char* out, size_t cap) {
    snprintf(out, cap, "lang=%s&zh=%s", tomato_wt_lang_code(tomato_wt_lang()), tomato_wt_simp() ? "s" : "t");
}

/* ---- 路径与文件名 / Paths and file names ---- */

void tomato_wt_title_path(const char* id, char* out, size_t cap) {
    char sid[16];
    tomato_safe_id(id, sid, sizeof(sid));
    snprintf(out, cap, TOMATO_WT_DIR "/t/%s.json", sid);
}

void tomato_wt_cover_path(const char* id, char* out, size_t cap) {
    char sid[16];
    tomato_safe_id(id, sid, sizeof(sid));
    snprintf(out, cap, TOMATO_WT_DIR "/c/%s.jpg", sid);
}

static void mkdirs(void) {
    mkdir(TOMATO_WT_DIR, 0775);
    mkdir(TOMATO_WT_DIR "/t", 0775);
    mkdir(TOMATO_WT_DIR "/c", 0775);
}

// 文件名里不能用的字符换成“_”，去掉首尾空格，按字符边界截到 cap。/ Replace characters not allowed in file names with
// "_", trim spaces, and cut to cap on a character boundary.
static void safe_name(const char* in, char* out, size_t cap) {
    size_t n = 0;
    for (const unsigned char* p = (const unsigned char*)in; *p && n + 1 < cap; p++) {
        unsigned char c = *p;
        if (c < 0x20 || strchr("/\\:*?\"<>|", c)) c = '_';
        out[n++] = (char)c;
    }
    out[n] = 0;
    while (n && ((unsigned char)out[n - 1] & 0xC0) == 0x80) n--;   // 截在多字节字符中间 / Cut inside a character
    if (n && ((unsigned char)out[n - 1] & 0xC0) == 0xC0) n--;
    out[n] = 0;
    while (n && out[n - 1] == ' ') out[--n] = 0;
    size_t lead = 0;
    while (out[lead] == ' ' || out[lead] == '.') lead++;
    if (lead) memmove(out, out + lead, strlen(out + lead) + 1);
}

void tomato_wt_episode_name(int no, const char* title, char* out, size_t cap) {
    char t[80], digits[16];
    safe_name(title ? title : "", t, sizeof(t));
    snprintf(digits, sizeof(digits), "%d", no);
    // 标题就是“第12话”“第12話”“Episode 12”这一类时不再重复。/ Drop a title that only restates the number.
    const bool plain = !t[0] || ((!strncmp(t, "第", 3) || !strncmp(t, "Ep", 2) || !strncmp(t, "ep", 2)) &&
                                 strstr(t, digits) && strlen(t) <= strlen(digits) + 9);
    if (plain) snprintf(out, cap, "第%04d话.cbz", no);
    else snprintf(out, cap, "第%04d话 %s.cbz", no, t);
}

// 文件名开头“第NNNN话”的话号；不是这种名字返回 0。/ The episode number from a "第NNNN话" prefix; 0 otherwise.
static int episode_of(const char* name) {
    if (strncmp(name, "第", 3)) return 0;
    const char* p = name + 3;
    int no = 0, digits = 0;
    while (*p >= '0' && *p <= '9' && digits < 6) { no = no * 10 + (*p++ - '0'); digits++; }
    return digits && !strncmp(p, "话", 3) ? no : 0;
}

bool tomato_wt_has_episode(const char* folder, int no) {
    DIR* d = opendir(folder);
    if (!d) return false;
    bool found = false;
    struct dirent* e;
    while (!found && (e = readdir(d))) {
        const size_t n = strlen(e->d_name);
        found = n > 4 && !strcasecmp(e->d_name + n - 4, ".cbz") && episode_of(e->d_name) == no;
    }
    closedir(d);
    return found;
}

/* ---- 订阅标记 / Subscription markers ---- */

bool tomato_wt_mark_read(const char* folder, tomato_wt_mark_t* out) {
    char path[PATH_LEN];
    snprintf(path, sizeof(path), "%s/" TOMATO_WT_MARK, folder);
    cJSON* root = tomato_cache_read_json(path);
    if (!root) return false;
    memset(out, 0, sizeof(*out));
    tomato_json_str(root, "id", out->id, sizeof(out->id));
    tomato_json_str(root, "lang", out->lang, sizeof(out->lang));
    tomato_json_str(root, "title", out->title, sizeof(out->title));
    tomato_json_str(root, "author", out->author, sizeof(out->author));
    out->last = tomato_json_int(root, "last");
    out->fresh = tomato_json_int(root, "fresh");
    out->added = (uint32_t)tomato_json_int(root, "added");
    cJSON_Delete(root);
    if (!out->lang[0]) snprintf(out->lang, sizeof(out->lang), "%s", tomato_wt_lang_code(tomato_wt_lang()));
    return out->id[0] != 0;
}

esp_err_t tomato_wt_mark_write(const char* folder, const tomato_wt_mark_t* m) {
    char path[PATH_LEN];
    snprintf(path, sizeof(path), "%s/" TOMATO_WT_MARK, folder);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", m->id);
    cJSON_AddStringToObject(root, "lang", m->lang);
    cJSON_AddStringToObject(root, "title", m->title);
    cJSON_AddStringToObject(root, "author", m->author);
    cJSON_AddNumberToObject(root, "last", m->last);
    cJSON_AddNumberToObject(root, "fresh", m->fresh);
    cJSON_AddNumberToObject(root, "added", m->added);
    const esp_err_t err = tomato_cache_write_json(path, root);
    cJSON_Delete(root);
    return err;
}

// 逐个看 comics/ 下的文件夹，对每个有标记的调用 fn；fn 返回 false 停下。/ Visit each folder under comics/ and call
// fn for those with a marker; stop when fn returns false.
typedef bool (*mark_fn_t)(const char* folder, const tomato_wt_mark_t* m, void* arg);

static void each_mark(mark_fn_t fn, void* arg) {
    DIR* d = opendir(TOMATO_WT_COMICS);
    if (!d) return;
    char folder[PATH_LEN];
    tomato_wt_mark_t m;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(folder, sizeof(folder), TOMATO_WT_COMICS "/%s", e->d_name);
        struct stat st;
        if (stat(folder, &st) != 0 || !S_ISDIR(st.st_mode) || !tomato_wt_mark_read(folder, &m)) continue;
        if (!fn(folder, &m, arg)) break;
    }
    closedir(d);
}

typedef struct {
    const char* id;
    char* out;
    size_t cap;
    bool found;
} find_t;

static bool find_one(const char* folder, const tomato_wt_mark_t* m, void* arg) {
    find_t* f = arg;
    if (strcmp(m->id, f->id)) return true;
    snprintf(f->out, f->cap, "%s", folder);
    f->found = true;
    return false;
}

bool tomato_wt_find(const char* id, char* folder, size_t cap) {
    find_t f = {.id = id, .out = folder, .cap = cap};
    if (id && id[0]) each_mark(find_one, &f);
    return f.found;
}

esp_err_t tomato_wt_subscribe(const char* id, const char* title, const char* author, int last, char* folder,
                              size_t cap) {
    char sid[16];
    tomato_safe_id(id, sid, sizeof(sid));
    if (!sid[0]) return ESP_ERR_INVALID_ARG;
    // WEBTOON 是数字或 c 开头，插件来源是 m 开头。/ WEBTOON ids are digits or start with c; extension ids start with m.
    if (sid[0] != 'c' && !tomato_wt_is_ext(sid) && (sid[0] < '0' || sid[0] > '9')) return ESP_ERR_INVALID_ARG;
    tomato_wt_mark_t m = {0};
    // 已经订阅过：只改起点。/ Already subscribed: only move the starting point.
    if (!tomato_wt_find(sid, folder, cap)) {
        char name[72];
        safe_name(title && title[0] ? title : sid, name, sizeof(name));
        if (!name[0]) snprintf(name, sizeof(name), "%s", sid);
        mkdir(TOMATO_WT_COMICS, 0775);
        // 同名的本地文件夹（没有标记）不碰，另起一个名字。/ Leave a same-named local folder (no marker) alone and use
        // another name.
        for (int k = 0; k < 20; k++) {
            if (k == 0) snprintf(folder, cap, TOMATO_WT_COMICS "/%s", name);
            else if (k == 1) snprintf(folder, cap, TOMATO_WT_COMICS "/%s (WEBTOON)", name);
            else snprintf(folder, cap, TOMATO_WT_COMICS "/%s (WEBTOON %d)", name, k);
            struct stat st;
            if (stat(folder, &st) != 0) break;
        }
        if (mkdir(folder, 0775) != 0) return ESP_FAIL;
        m.added = (uint32_t)time(NULL);
    } else {
        tomato_wt_mark_read(folder, &m);
    }
    snprintf(m.id, sizeof(m.id), "%s", sid);
    snprintf(m.lang, sizeof(m.lang), "%s", tomato_wt_is_ext(sid) ? "mihon" : tomato_wt_lang_code(tomato_wt_lang()));
    if (title && title[0]) tomato_copy_utf8(m.title, sizeof(m.title), title);
    if (author && author[0]) tomato_copy_utf8(m.author, sizeof(m.author), author);
    m.last = last < 0 ? 0 : last;
    return tomato_wt_mark_write(folder, &m);
}

/* ---- 发现页 / Store ---- */

// 列表里缺的封面一张张取（封面是服务器转好的 150×200 灰度小图）。/ Fetch missing list covers one by one (150x200
// gray images made by the server).
static void fetch_covers(const tomato_srv_t* srv, const cJSON* items, const volatile bool* cancel,
                         tomato_hupu_progress_t progress, int from, int count) {
    char q[48], path[96], dest[64], id[16];
    query(q, sizeof(q));
    const int total = cJSON_GetArraySize(items);
    int i = 0;
    const cJSON* it;
    cJSON_ArrayForEach(it, items) {
        if (*cancel) return;
        i++;
        // 只取要的那几部（count 为 0 是全部）；进度仍按整个列表的序号报，页面据此知道这一页轮过了。
        // Only the requested titles (count 0 means all); progress still counts by position in the whole list, which is
        // how the page knows its page has had its turn.
        if (i - 1 < from || (count && i - 1 >= from + count)) continue;
        tomato_json_str(it, "id", id, sizeof(id));
        tomato_wt_cover_path(id, dest, sizeof(dest));
        if (id[0] && !tomato_file_exists(dest)) {
            char sid[16];
            tomato_safe_id(id, sid, sizeof(sid));
            snprintf(path, sizeof(path), "/api/device/v1/cover/%s.jpg?%s", sid, q);
            tomato_srv_image(srv, path, dest, TIMEOUT_MS, cancel);
        }
        progress(1 + i, 1 + total);
    }
}

static esp_err_t run_list(const tomato_srv_t* srv, const tomato_wt_req_t* r, const char* path, const char* file,
                          const volatile bool* cancel, tomato_hupu_progress_t progress, char* msg, size_t cap) {
    cJSON* root = NULL;
    if (r->thumbs_only) {
        // 翻到的页补封面：不重新取列表，用卡上这份。/ Covers for the pages being viewed: no refetch, use the list
        // already on the card.
        root = tomato_cache_read_json(file);
        if (!root) { snprintf(msg, cap, "列表还没取过"); return ESP_ERR_NOT_FOUND; }
    } else {
        esp_err_t err = tomato_srv_json(srv, path, TIMEOUT_MS, cancel, &root, msg, cap);
        if (err != ESP_OK) return err;
        mkdirs();
        err = tomato_cache_write_json(file, root);
        if (err != ESP_OK) { cJSON_Delete(root); snprintf(msg, cap, "写不进 TF 卡"); return err; }
        progress(1, 2);
    }
    const cJSON* items = cJSON_GetObjectItemCaseSensitive(root, "items");
    const int n = cJSON_GetArraySize(items);
    fetch_covers(srv, items, cancel, progress, r->thumbs_from, r->thumbs_count);
    cJSON_Delete(root);
    if (*cancel) { snprintf(msg, cap, "已取消"); return ESP_ERR_INVALID_STATE; }
    snprintf(msg, cap, "%d 部", n);
    return ESP_OK;
}

// 来源参数：WEBTOON 不带（旧服务器也认），插件来源带 &src=。/ The source parameter: none for WEBTOON (old servers
// accept that too), &src= for an extension source.
static bool ext_src(const tomato_wt_req_t* r, char* out, size_t cap) {
    out[0] = 0;
    if (!r->src[0] || !strcmp(r->src, TOMATO_WT_SRC_WEBTOON)) return false;
    char esc[64];
    tomato_url_escape(r->src, esc, sizeof(esc), false);
    snprintf(out, cap, "&src=%s", esc);
    return true;
}

// 刷新发现页时顺带取一次来源列表；旧服务器没有这个接口（404）就删掉缓存，发现页不显示来源按钮。
// A store refresh also fetches the source list; an older server without it (404) clears the cache and the store
// shows no source button.
static void fetch_sources(const tomato_srv_t* srv, const volatile bool* cancel) {
    cJSON* root = NULL;
    char m[96];
    const esp_err_t err = tomato_srv_json(srv, "/api/device/v1/sources", TIMEOUT_MS, cancel, &root, m, sizeof(m));
    if (err == ESP_OK) {
        mkdirs();
        tomato_cache_write_json(TOMATO_WT_SOURCES_FILE, root);
        cJSON_Delete(root);
    } else if (err == ESP_ERR_NOT_FOUND) {
        remove(TOMATO_WT_SOURCES_FILE);
    }
}

static esp_err_t run_store(const tomato_srv_t* srv, const tomato_wt_req_t* r, const volatile bool* cancel,
                           tomato_hupu_progress_t progress, char* msg, size_t cap) {
    static const char* const SEGS[] = {"rank", "genre", "day", "done"};
    if (r->thumbs_only) return run_list(srv, r, NULL, TOMATO_WT_STORE_FILE, cancel, progress, msg, cap);
    char src[80];
    const bool ext = ext_src(r, src, sizeof(src));
    bool ok = false;
    if (ext) ok = !strcmp(r->seg, "pop") || !strcmp(r->seg, "latest");
    else for (int i = 0; i < 4; i++) ok |= !strcmp(r->seg, SEGS[i]);
    fetch_sources(srv, cancel);
    if (*cancel) { snprintf(msg, cap, "已取消"); return ESP_ERR_INVALID_STATE; }
    char key[48], q[48], path[300];
    tomato_url_escape(r->key, key, sizeof(key), false);
    query(q, sizeof(q));
    snprintf(path, sizeof(path), "/api/device/v1/store?seg=%s&key=%s&limit=60&%s%s",
             ok ? r->seg : ext ? "pop" : "rank", key, q, src);
    return run_list(srv, r, path, TOMATO_WT_STORE_FILE, cancel, progress, msg, cap);
}

static esp_err_t run_search(const tomato_srv_t* srv, const tomato_wt_req_t* r, const volatile bool* cancel,
                            tomato_hupu_progress_t progress, char* msg, size_t cap) {
    if (r->thumbs_only) return run_list(srv, r, NULL, TOMATO_WT_SEARCH_FILE, cancel, progress, msg, cap);
    if (!r->q[0]) { snprintf(msg, cap, "没有关键词"); return ESP_ERR_INVALID_ARG; }
    char q[200], lq[48], src[80], path[400];
    tomato_url_escape(r->q, q, sizeof(q), false);
    query(lq, sizeof(lq));
    ext_src(r, src, sizeof(src));
    snprintf(path, sizeof(path), "/api/device/v1/search?q=%s&%s%s", q, lq, src);
    return run_list(srv, r, path, TOMATO_WT_SEARCH_FILE, cancel, progress, msg, cap);
}

static esp_err_t run_title(const tomato_srv_t* srv, const char* id, const volatile bool* cancel, char* msg,
                           size_t cap) {
    char q[48], path[128], file[64], cover[64];
    query(q, sizeof(q));
    snprintf(path, sizeof(path), "/api/device/v1/title/%s?%s", id, q);
    cJSON* root = NULL;
    esp_err_t err = tomato_srv_json(srv, path, TIMEOUT_MS, cancel, &root, msg, cap);
    if (err != ESP_OK) return err;
    mkdirs();
    tomato_wt_title_path(id, file, sizeof(file));
    err = tomato_cache_write_json(file, root);
    cJSON_Delete(root);
    if (err != ESP_OK) { snprintf(msg, cap, "写不进 TF 卡"); return err; }
    tomato_wt_cover_path(id, cover, sizeof(cover));
    if (!tomato_file_exists(cover)) {
        snprintf(path, sizeof(path), "/api/device/v1/cover/%s.jpg?%s", id, q);
        tomato_srv_image(srv, path, cover, TIMEOUT_MS, cancel);
    }
    msg[0] = 0;
    return ESP_OK;
}

/* ---- 检查更新与下载 / Update check and download ---- */

typedef struct {
    char folder[PATH_LEN];
    tomato_wt_mark_t mark;
    int stop;              // 这次没下好的最小话号（失败、没轮到、超上限）/ Lowest episode not done this run
} target_t;

// 服务器这次回的一话（新话，或补全时比 last 新的话），下完后用来推进 last。/ One episode from the server this run (new,
// or newer than last when filling), used afterwards to move last forward.
typedef struct {
    int target;
    int no;
} seen_t;

typedef struct {
    seen_t* list;
    int n;
} seen_list_t;

static void see(seen_list_t* s, target_t* list, int target, int no) {
    if (s->n < SEEN_MAX) s->list[s->n++] = (seen_t){.target = target, .no = no};
    // 记不下就当没下好，last 停在它前面。/ Not recorded: treat it as not done so last stops before it.
    else if (no < list[target].stop) list[target].stop = no;
}

typedef struct {
    target_t* list;
    int n, max;
    const char* only;      // 只检查这一部 / Only this title
} collect_t;

static bool collect_one(const char* folder, const tomato_wt_mark_t* m, void* arg) {
    collect_t* c = arg;
    if (c->only && c->only[0] && strcmp(c->only, m->id)) return true;
    if (c->n >= c->max) return false;
    snprintf(c->list[c->n].folder, sizeof(c->list[c->n].folder), "%s", folder);
    c->list[c->n].stop = INT_MAX;
    c->list[c->n++].mark = *m;
    return true;
}

// 只和这一话有关的失败（服务器说不行、转换出错或太久、文件太大）：跳过这一话接着下。其余的（连不上、下载中断、
// 写不进卡、内存不足）后面的话也会一样失败：停下，下次检查更新接着下。
// Failures that only concern this episode (the server refused, conversion failed or took too long, the file is too
// big): skip it and go on. Anything else (unreachable, dropped mid-download, card write, out of memory) would fail
// the later episodes too: stop, and the next check resumes.
static bool episode_only(esp_err_t err) {
    return err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_RESPONSE || err == ESP_ERR_INVALID_SIZE ||
           err == ESP_ERR_TIMEOUT;
}

typedef struct {
    int target;            // list 里的下标 / Index into the target list
    int no;
    char title[80];
} job_t;

// 等服务器转好一话再下载到系列文件夹；成功后记新话数。last 不在这里动，全部下完再推进。/ Wait for the server to convert
// an episode, then download it into the series folder; count it as new on success. last moves only after the run.
static esp_err_t fetch_episode(const tomato_srv_t* srv, target_t* t, const job_t* j, const volatile bool* cancel,
                               char* msg, size_t cap) {
    char path[128], q[48];
    snprintf(q, sizeof(q), "lang=%s", t->mark.lang[0] ? t->mark.lang : tomato_wt_lang_code(tomato_wt_lang()));
    snprintf(path, sizeof(path), "/api/device/v1/episode/%s/%d?%s", t->mark.id, j->no, q);
    // 状态里写“哪部第几话 · 在做什么”，书架上的状态卡片照着显示。/ The status reads "which title, episode, what now",
    // shown as is by the shelf's status card.
    const char* series = strrchr(t->folder, '/');
    series = series ? series + 1 : t->folder;
    tomato_sync_step("%.40s 第 %d 话 · 问服务器", series, j->no);
    const int64_t start = (int64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
    for (;;) {
        cJSON* st = NULL;
        esp_err_t err = tomato_srv_json(srv, path, TIMEOUT_MS, cancel, &st, msg, cap);
        if (err != ESP_OK) return err;
        char state[12];
        tomato_json_str(st, "state", state, sizeof(state));
        if (!strcmp(state, "error")) tomato_json_str(st, "error", msg, cap);
        if (!strcmp(state, "pending")) {
            const cJSON* d = cJSON_GetObjectItemCaseSensitive(st, "done");
            const cJSON* n = cJSON_GetObjectItemCaseSensitive(st, "total");
            if (cJSON_IsNumber(n) && n->valueint > 0)
                tomato_sync_step("%.40s 第 %d 话 · 服务器转换中 %d/%d", series, j->no,
                                 cJSON_IsNumber(d) ? d->valueint : 0, n->valueint);
            else tomato_sync_step("%.40s 第 %d 话 · 排队等服务器转换", series, j->no);
        } else if (strcmp(state, "ready") && strcmp(state, "error")) {
            tomato_sync_step("%.40s 第 %d 话 · 等服务器开始转换", series, j->no);
        }
        cJSON_Delete(st);
        if (!strcmp(state, "ready")) break;
        if (!strcmp(state, "error")) return ESP_ERR_INVALID_RESPONSE;
        if ((int64_t)xTaskGetTickCount() * portTICK_PERIOD_MS - start > CONVERT_MAX_MS) {
            snprintf(msg, cap, "服务器转换太久了");
            return ESP_ERR_TIMEOUT;
        }
        for (int w = 0; w < POLL_GAP_MS; w += 100) {
            if (*cancel) { snprintf(msg, cap, "已取消"); return ESP_ERR_INVALID_STATE; }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    char name[128], dest[PATH_LEN + 128];
    tomato_wt_episode_name(j->no, j->title, name, sizeof(name));
    snprintf(dest, sizeof(dest), "%s/%s", t->folder, name);
    snprintf(path, sizeof(path), "/api/device/v1/episode/%s/%d.cbz?%s", t->mark.id, j->no, q);
    tomato_sync_step("%.40s 第 %d 话 · 下载中", series, j->no);
    esp_err_t err = tomato_srv_download(srv, path, dest, CBZ_MAX, TIMEOUT_MS * 3, cancel, msg, cap);
    if (err != ESP_OK) return err;
    t->mark.fresh++;
    tomato_wt_mark_write(t->folder, &t->mark);
    return ESP_OK;
}

// 普通更新：一次问服务器所有订阅里比 last 新的话。/ Normal update: ask the server once for episodes newer than last
// across all subscriptions.
static int plan_new(const tomato_srv_t* srv, target_t* list, int n, job_t* jobs, int max, seen_list_t* seen,
                    const volatile bool* cancel, char* msg, size_t cap, esp_err_t* err) {
    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "lang", tomato_wt_lang_code(tomato_wt_lang()));
    cJSON_AddStringToObject(body, "zh", tomato_wt_simp() ? "s" : "t");
    cJSON* titles = cJSON_AddArrayToObject(body, "titles");
    for (int i = 0; i < n; i++) {
        cJSON* t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "id", list[i].mark.id);
        cJSON_AddNumberToObject(t, "last", list[i].mark.last);
        cJSON_AddItemToArray(titles, t);
    }
    char* text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    cJSON* root = NULL;
    *err = text ? tomato_srv_post(srv, "/api/device/v1/check", text, TIMEOUT_MS * 3, cancel, &root, msg, cap)
                : ESP_ERR_NO_MEM;
    cJSON_free(text);
    if (*err != ESP_OK) return 0;
    int count = 0;
    const cJSON* t;
    cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(root, "titles")) {
        char id[16];
        tomato_json_str(t, "id", id, sizeof(id));
        int idx = -1;
        for (int i = 0; i < n && idx < 0; i++) if (!strcmp(list[i].mark.id, id)) idx = i;
        if (idx < 0) continue;
        const cJSON* e;
        cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(t, "new")) {
            const int no = tomato_json_int(e, "no");
            if (no <= 0) continue;
            see(seen, list, idx, no);
            if (tomato_wt_has_episode(list[idx].folder, no)) continue;
            // 超过每次上限的留到下次。/ Episodes past the per-run cap wait for the next check.
            if (count >= max) {
                if (no < list[idx].stop) list[idx].stop = no;
                continue;
            }
            jobs[count].target = idx;
            jobs[count].no = no;
            tomato_json_str(e, "title", jobs[count].title, sizeof(jobs[count].title));
            count++;
        }
    }
    cJSON_Delete(root);
    return count;
}

// 补全：取作品的全部免费话，下卡上缺的（从旧到新）。/ Fill: fetch the title's free episodes and download the missing
// ones, oldest first.
static int plan_fill(const tomato_srv_t* srv, target_t* t, job_t* jobs, int max, seen_list_t* seen,
                     const volatile bool* cancel, char* msg, size_t cap, esp_err_t* err) {
    char q[48], path[128];
    query(q, sizeof(q));
    snprintf(path, sizeof(path), "/api/device/v1/title/%s?%s", t->mark.id, q);
    cJSON* root = NULL;
    *err = tomato_srv_json(srv, path, TIMEOUT_MS, cancel, &root, msg, cap);
    if (*err != ESP_OK) return 0;
    int count = 0;
    const cJSON* e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "episodes")) {
        const int no = tomato_json_int(e, "no");
        if (no <= 0) continue;
        if (no > t->mark.last) see(seen, t, 0, no);
        if (tomato_wt_has_episode(t->folder, no)) continue;
        if (count >= max) {
            if (no < t->stop) t->stop = no;
            continue;
        }
        jobs[count].target = 0;
        jobs[count].no = no;
        tomato_json_str(e, "title", jobs[count].title, sizeof(jobs[count].title));
        count++;
    }
    cJSON_Delete(root);
    return count;
}

static esp_err_t run_update(const tomato_srv_t* srv, const tomato_wt_req_t* r, const volatile bool* cancel,
                            tomato_hupu_progress_t progress, char* msg, size_t cap) {
    target_t* list = heap_caps_calloc(TITLES_MAX, sizeof(target_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const int max = tomato_wt_max();
    job_t* jobs = heap_caps_calloc((size_t)max, sizeof(job_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    seen_list_t seen = {.list = heap_caps_calloc(SEEN_MAX, sizeof(seen_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)};
    if (!list || !jobs || !seen.list) {
        heap_caps_free(list);
        heap_caps_free(jobs);
        heap_caps_free(seen.list);
        snprintf(msg, cap, "内存不足");
        return ESP_ERR_NO_MEM;
    }
    collect_t c = {.list = list, .max = TITLES_MAX, .only = r->id};
    each_mark(collect_one, &c);
    esp_err_t err = ESP_OK;
    int planned = 0;
    if (!c.n) {
        snprintf(msg, cap, r->id[0] ? "没找到这部的订阅" : "还没有订阅");
    } else if (r->fill && c.n == 1) {
        tomato_sync_step("问服务器缺哪几话");
        planned = plan_fill(srv, &list[0], jobs, max, &seen, cancel, msg, cap, &err);
    } else {
        tomato_sync_step("问服务器 %d 部订阅有没有新话", c.n);
        planned = plan_new(srv, list, c.n, jobs, max, &seen, cancel, msg, cap, &err);
    }
    int done = 0, failed = 0;
    char why[96] = "";
    bool stopped = false;
    int k = 0;
    if (err == ESP_OK && c.n) {
        progress(0, planned);
        for (; k < planned && !*cancel; k++) {
            char m[96] = "";
            esp_err_t e = fetch_episode(srv, &list[jobs[k].target], &jobs[k], cancel, m, sizeof(m));
            // 网络抖一下不算断：等 10 秒再试一次。/ A brief blip is not an outage: wait 10 s and try once more.
            if (e != ESP_OK && !*cancel && !episode_only(e)) {
                tomato_sync_step("连不上服务器，%d 秒后再试", RETRY_WAIT_MS / 1000);
                for (int w = 0; w < RETRY_WAIT_MS && !*cancel; w += 100) vTaskDelay(pdMS_TO_TICKS(100));
                if (!*cancel) { m[0] = 0; e = fetch_episode(srv, &list[jobs[k].target], &jobs[k], cancel, m, sizeof(m)); }
            }
            if (e == ESP_OK) done++;
            else {
                target_t* t = &list[jobs[k].target];
                if (jobs[k].no < t->stop) t->stop = jobs[k].no;
                if (*cancel) break;
                failed++;
                if (!why[0]) snprintf(why, sizeof(why), "%s", m);
                if (!episode_only(e)) { snprintf(why, sizeof(why), "%s", m); stopped = true; break; }
            }
            progress(k + 1, planned);
        }
        // 没轮到的话（取消或停下）也算没下好。/ Episodes never reached (cancelled or stopped) are not done either.
        for (int r2 = k; r2 < planned; r2++) {
            target_t* t = &list[jobs[r2].target];
            if (jobs[r2].no < t->stop) t->stop = jobs[r2].no;
        }
        // 推进 last：服务器回的话里，比第一个没下好的小的都已在卡上，last 推到其中最大的；没下好的下次检查更新再下。
        // Move last forward: every listed episode below the first one not done is on the card, so last becomes the
        // largest of them; the ones not done come back on the next check.
        for (int i = 0; i < c.n; i++) {
            int last = list[i].mark.last;
            for (int s = 0; s < seen.n; s++)
                if (seen.list[s].target == i && seen.list[s].no < list[i].stop && seen.list[s].no > last)
                    last = seen.list[s].no;
            if (last != list[i].mark.last) {
                list[i].mark.last = last;
                tomato_wt_mark_write(list[i].folder, &list[i].mark);
            }
        }
        if (*cancel) { snprintf(msg, cap, "已取消，下载了 %d 话", done); err = ESP_ERR_INVALID_STATE; }
        else if (stopped)
            snprintf(msg, cap, "下载了 %d 话后停下：%.60s；再%s一次接着下", done, why, r->fill ? "补全" : "检查更新");
        else if (!planned) snprintf(msg, cap, r->fill ? "没有缺的话" : "没有新的话");
        else if (failed)
            snprintf(msg, cap, "下载了 %d 话，%d 话失败，再%s一次会重下：%.40s", done, failed, r->fill ? "补全" : "检查更新", why);
        else snprintf(msg, cap, "下载了 %d 话", done);
    }
    heap_caps_free(list);
    heap_caps_free(jobs);
    heap_caps_free(seen.list);
    return err;
}

static esp_err_t run_test(const tomato_srv_t* srv, const volatile bool* cancel, char* msg, size_t cap) {
    char q[48], path[96];
    query(q, sizeof(q));
    snprintf(path, sizeof(path), "/api/device/v1/store?seg=rank&limit=1&%s", q);
    cJSON* root = NULL;
    const esp_err_t err = tomato_srv_json(srv, path, TIMEOUT_MS, cancel, &root, msg, cap);
    if (err != ESP_OK) return err;
    cJSON_Delete(root);
    snprintf(msg, cap, "正常 · %s", tomato_wt_lang_name(tomato_wt_lang()));
    return ESP_OK;
}

esp_err_t tomato_wt_run(const tomato_wt_req_t* r, const volatile bool* cancel, tomato_hupu_progress_t progress,
                        char* msg, size_t cap) {
    tomato_wt_config_t cfg;
    if (tomato_wt_config_load(&cfg) != ESP_OK) { snprintf(msg, cap, "还没设置漫画服务器"); return ESP_ERR_NOT_FOUND; }
    const tomato_srv_t srv = {.cfg = &cfg, .header = HEADER, .name = "webtoon"};
    char id[16];
    tomato_safe_id(r->id, id, sizeof(id));
    esp_err_t err;
    switch (r->job) {
    case TOMATO_WT_STORE: err = run_store(&srv, r, cancel, progress, msg, cap); break;
    case TOMATO_WT_SEARCH: err = run_search(&srv, r, cancel, progress, msg, cap); break;
    case TOMATO_WT_TITLE:
        if (!id[0]) { snprintf(msg, cap, "作品编号不对"); err = ESP_ERR_INVALID_ARG; break; }
        err = run_title(&srv, id, cancel, msg, cap);
        break;
    case TOMATO_WT_UPDATE: err = run_update(&srv, r, cancel, progress, msg, cap); break;
    case TOMATO_WT_TEST: err = run_test(&srv, cancel, msg, cap); break;
    default: err = ESP_ERR_INVALID_ARG; break;
    }
    memset(&cfg, 0, sizeof(cfg));
    return err;
}
