/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 后台同步任务：检查更新、搜索、下载。同一时间只跑一个操作。
 * Background sync task: update check, search, download. One operation at a time.
 *
 * 更新判断沿用安卓版 v1.7.3 的规则：服务器章节数取 max(remote_total, local_total)，
 * 大于本地 TXT 的章节数才重新下载；不用服务器的 has_update 标志。
 * Update rule follows the Android v1.7.3 fix: server chapters = max(remote_total, local_total);
 * re-download only when that exceeds the local TXT; never trust the server has_update flag.
 *
 * 冻结：不在主循环任务里做网络；状态用临界区保护，页面只读快照。
 * Frozen: no networking on the main loop task; status is guarded by a critical section and pages read snapshots.
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tomato_internal.h"

#define TAG "tomato_sync"
// HTTPS 握手和 cJSON 都吃栈，留足余量。/ TLS handshakes and cJSON need stack; leave headroom.
#define TASK_STACK 12288
#define NET_TIMEOUT_MS 20000
#define POLL_MS 2000
#define SCAN_WAIT_POLLS 30   // 更新扫描最多等 60 秒 / Update scan waits at most 60 s
#define JOB_WAIT_POLLS 300   // 生成任务最多等 10 分钟 / Server job waits at most 10 min
#define BUSY_RETRIES 6

typedef enum { OP_CHECK, OP_UPDATE, OP_SEARCH, OP_DOWNLOAD, OP_WEBDAV } op_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static tomato_status_t s_status;
static tomato_hit_t s_hits[TOMATO_SEARCH_MAX];
static volatile bool s_cancel;
static volatile bool s_busy;
static volatile uint32_t s_bytes;
static op_t s_op;
static char s_query[96];
static tomato_hit_t s_target;
static tomato_file_hook_t s_file_hook;
static tomato_progress_export_t s_export;
static tomato_progress_import_t s_import;

void tomato_sync_set_progress_io(tomato_progress_export_t export_cb, tomato_progress_import_t import_cb) {
    s_export = export_cb;
    s_import = import_cb;
}

void tomato_sync_set_file_hook(tomato_file_hook_t hook) { s_file_hook = hook; }

/* ---- 状态 / Status ---- */

// 按 UTF-8 字符边界截断复制。/ Copy with truncation on a UTF-8 character boundary.
static void copy_utf8(char* out, size_t cap, const char* in) {
    size_t n = strlen(in);
    if (n >= cap) {
        n = cap - 1;
        while (n && ((unsigned char)in[n] & 0xC0) == 0x80) n--;
    }
    memcpy(out, in, n);
    out[n] = 0;
}

static void set_phase(tomato_phase_t phase, const char* message) {
    portENTER_CRITICAL(&s_lock);
    s_status.phase = phase;
    if (message) copy_utf8(s_status.message, sizeof(s_status.message), message);
    s_status.revision++;
    portEXIT_CRITICAL(&s_lock);
}

static void set_progress(int current, int total) {
    portENTER_CRITICAL(&s_lock);
    s_status.current = current;
    s_status.total = total;
    s_status.revision++;
    portEXIT_CRITICAL(&s_lock);
}

static void fail(esp_err_t err, const char* message) {
    portENTER_CRITICAL(&s_lock);
    s_status.error = err;
    portEXIT_CRITICAL(&s_lock);
    set_phase(TOMATO_FAILED, message);
}

void tomato_sync_get_status(tomato_status_t* out) {
    portENTER_CRITICAL(&s_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_lock);
    out->bytes = s_bytes;
}

int tomato_sync_hits(tomato_hit_t* out, int max) {
    portENTER_CRITICAL(&s_lock);
    int n = s_status.hit_count < max ? s_status.hit_count : max;
    memcpy(out, s_hits, sizeof(out[0]) * (size_t)n);
    portEXIT_CRITICAL(&s_lock);
    return n;
}

bool tomato_sync_busy(void) { return s_busy; }
void tomato_sync_cancel(void) { s_cancel = true; }

static bool wait_poll(void) {
    for (int i = 0; i < POLL_MS / 100; i++) {
        if (s_cancel) return false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return !s_cancel;
}

/* ---- 服务器操作 / Server operations ---- */

// FAT 文件名里不能出现的字符换成下划线。/ Replace characters FAT filenames cannot hold.
static void safe_name(const char* title, char* out, size_t cap) {
    size_t o = 0;
    for (const char* p = title; *p && o + 5 < cap; p++) {
        out[o++] = strchr("/\\:*?\"<>|", *p) ? '_' : *p;
    }
    out[o] = 0;
    if (!o) snprintf(out, cap, "book");
    strncat(out, ".txt", cap - strlen(out) - 1);
}

// 字段存在且不是 null。/ Field present and not null.
static bool has_value(const cJSON* obj, const char* key) {
    const cJSON* v = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : NULL;
    return v && !cJSON_IsNull(v);
}

// 在 library 里找书名对应的 TXT 相对路径。/ Find the TXT relative path for a title in the library.
static esp_err_t find_library_file(const tomato_config_t* cfg, const char* title, char* rel, size_t cap) {
    char esc[3 * TOMATO_TITLE_LEN + 1], path[1152];
    tomato_url_escape(title, esc, sizeof(esc), false);
    for (int attempt = 0; attempt < SCAN_WAIT_POLLS; attempt++) {
        if (attempt && !wait_poll()) return ESP_ERR_INVALID_STATE;
        snprintf(path, sizeof(path), "/api/library?name=%s&start=%s", esc, attempt ? "false" : "true");
        cJSON* root; int status;
        if (tomato_http_json(cfg, "GET", path, NULL, &root, &status) != ESP_OK) continue;
        bool running = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "running"));
        if (running) { cJSON_Delete(root); continue; }
        const cJSON* it;
        char dir[256] = "";
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "items")) {
            char kind[8], ext[8];
            tomato_json_str(it, "kind", kind, sizeof(kind));
            tomato_json_str(it, "ext", ext, sizeof(ext));
            if (!strcmp(kind, "file") && !strcmp(ext, "txt")) {
                tomato_json_str(it, "rel_path", rel, cap);
                cJSON_Delete(root);
                return ESP_OK;
            }
            if (!strcmp(kind, "dir") && !dir[0]) tomato_json_str(it, "rel_path", dir, sizeof(dir));
        }
        cJSON_Delete(root);
        if (!dir[0]) return ESP_ERR_NOT_FOUND;
        // 书名子目录：再进去找一次。/ Title subdirectory: look inside once.
        char dir_esc[3 * sizeof(dir)];
        tomato_url_escape(dir, dir_esc, sizeof(dir_esc), false);
        snprintf(path, sizeof(path), "/api/library?path=%s&name=%s", dir_esc, esc);
        if (tomato_http_json(cfg, "GET", path, NULL, &root, &status) != ESP_OK) return ESP_FAIL;
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "items")) {
            char kind[8], ext[8];
            tomato_json_str(it, "kind", kind, sizeof(kind));
            tomato_json_str(it, "ext", ext, sizeof(ext));
            if (!strcmp(kind, "file") && !strcmp(ext, "txt")) {
                tomato_json_str(it, "rel_path", rel, cap);
                cJSON_Delete(root);
                return ESP_OK;
            }
        }
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_ERR_TIMEOUT;
}

// 让服务器生成 TXT 并下载到书库；book 的 title/file/chapters 在成功后更新。
// Have the server build the TXT and fetch it into the library; updates title/file/chapters on success.
static esp_err_t fetch_book(const tomato_config_t* cfg, tomato_book_t* book) {
    char body[64], path[128];
    cJSON* root; int status = 0;
    int job = -1;
    snprintf(body, sizeof(body), "{\"book_id\":\"%s\"}", book->book_id);
    set_phase(TOMATO_QUEUED, "服务器准备中");
    for (int i = 0; i < BUSY_RETRIES && job < 0; i++) {
        esp_err_t err = tomato_http_json(cfg, "POST", "/api/jobs", body, &root, &status);
        if (err == ESP_OK && root) { job = tomato_json_int(root, "id"); cJSON_Delete(root); break; }
        if (status != 429 || !wait_poll() || !wait_poll()) return status == 429 ? ESP_ERR_INVALID_STATE : ESP_FAIL;
    }
    if (job < 0) return ESP_ERR_TIMEOUT;

    char title[TOMATO_TITLE_LEN] = "";
    int chapter_total = 0;
    bool done = false;
    for (int poll = 0; poll < JOB_WAIT_POLLS && !done; poll++) {
        if (!wait_poll()) return ESP_ERR_INVALID_STATE;
        snprintf(path, sizeof(path), "/api/jobs?id=%d", job);
        if (tomato_http_json(cfg, "GET", path, NULL, &root, &status) != ESP_OK) continue;
        const cJSON* item = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "items"), 0);
        char state[12] = "";
        tomato_json_str(item, "state", state, sizeof(state));
        tomato_json_str(item, "title", title, sizeof(title));
        chapter_total = tomato_json_int(cJSON_GetObjectItemCaseSensitive(item, "progress"), "chapter_total");
        bool need_format = has_value(item, "format_options");
        bool need_name = has_value(item, "book_name_options");
        cJSON_Delete(root);
        if (!strcmp(state, "failed")) return ESP_FAIL;
        done = !strcmp(state, "done");
        if (need_format) {
            snprintf(path, sizeof(path), "/api/jobs/%d/format", job);
            if (tomato_http_json(cfg, "POST", path, "{\"value\":\"txt\"}", &root, &status) == ESP_OK) cJSON_Delete(root);
        }
        if (need_name) {
            snprintf(path, sizeof(path), "/api/jobs/%d/book_name", job);
            if (tomato_http_json(cfg, "POST", path, "{\"value\":null}", &root, &status) == ESP_OK) cJSON_Delete(root);
        }
    }
    if (!done) return ESP_ERR_TIMEOUT;
    if (!title[0]) snprintf(title, sizeof(title), "%s", book->title);

    char rel[256];
    esp_err_t err = find_library_file(cfg, title, rel, sizeof(rel));
    if (err != ESP_OK) return err;

    char file[sizeof(book->file)], dest[192], url_path[3 * sizeof(rel) + 16];
    // 更新时沿用原文件名，阅读进度跟着文件走。/ Updates keep the old file name so reading progress follows it.
    if (book->file[0]) snprintf(file, sizeof(file), "%s", book->file);
    else safe_name(title, file, sizeof(file));
    snprintf(dest, sizeof(dest), TOMATO_BOOKS_DIR "/%s", file);
    char esc[3 * sizeof(rel)];
    tomato_url_escape(rel, esc, sizeof(esc), true);
    snprintf(url_path, sizeof(url_path), "/download/%s", esc);
    set_phase(TOMATO_DOWNLOADING, title);
    struct stat st;
    uint32_t old_size = stat(dest, &st) == 0 ? (uint32_t)st.st_size : 0;
    err = tomato_http_download(cfg, url_path, dest, &s_cancel, &s_bytes);
    if (err != ESP_OK) return err;
    if (s_file_hook && stat(dest, &st) == 0) s_file_hook(dest, old_size, (uint32_t)st.st_size);

    snprintf(book->title, sizeof(book->title), "%s", title);
    snprintf(book->file, sizeof(book->file), "%s", file);
    if (chapter_total > book->chapters) book->chapters = (uint16_t)chapter_total;
    if (book->remote > book->chapters) book->chapters = book->remote;
    return ESP_OK;
}

// 拉服务器更新扫描，把章节数写进 shelf[i].remote。/ Run the server update scan and fill shelf[i].remote.
static esp_err_t fetch_remote_totals(const tomato_config_t* cfg, tomato_book_t* shelf, int count, uint8_t* found) {
    set_phase(TOMATO_CHECKING, "检查更新");
    cJSON* root = NULL; int status;
    esp_err_t err = tomato_http_json(cfg, "GET", "/api/updates?start=true", NULL, &root, &status);
    for (int i = 0; err == ESP_OK && i < SCAN_WAIT_POLLS; i++) {
        if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "running"))) break;
        cJSON_Delete(root); root = NULL;
        if (!wait_poll()) return ESP_ERR_INVALID_STATE;
        err = tomato_http_json(cfg, "GET", "/api/updates?start=false", NULL, &root, &status);
    }
    if (err != ESP_OK) return err;
    static const char* const lists[] = { "updates", "no_updates" };
    for (int l = 0; l < 2; l++) {
        const cJSON* row;
        cJSON_ArrayForEach(row, cJSON_GetObjectItemCaseSensitive(root, lists[l])) {
            char id[TOMATO_ID_LEN];
            tomato_json_str(row, "book_id", id, sizeof(id));
            int remote = tomato_json_int(row, "remote_total");
            int local = tomato_json_int(row, "local_total");
            int total = remote > local ? remote : local;
            for (int i = 0; i < count; i++) {
                if (!strcmp(shelf[i].book_id, id) && total > 0) { shelf[i].remote = (uint16_t)total; found[i] = 1; }
            }
        }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

static int count_pending(const tomato_book_t* shelf, int count) {
    int pending = 0;
    for (int i = 0; i < count; i++) pending += shelf[i].remote > shelf[i].chapters;
    return pending;
}

static void set_pending(int pending) {
    portENTER_CRITICAL(&s_lock);
    s_status.pending = pending;
    portEXIT_CRITICAL(&s_lock);
}

// 只检查：更新每本书的 remote 并保存，不下载。/ Check only: refresh each book's remote and save; no downloads.
static void run_check(const tomato_config_t* cfg, tomato_book_t* shelf, int count) {
    if (!count) { set_phase(TOMATO_DONE, "书架上还没有番茄小说"); return; }
    uint8_t found[TOMATO_SHELF_MAX] = {0};
    esp_err_t err = fetch_remote_totals(cfg, shelf, count, found);
    if (err != ESP_OK) { fail(err, s_cancel ? "已取消" : "检查更新失败"); return; }
    // 服务器扫描里没有的书（缓存被清或新加），直接查番茄目录接口数章节。
    // Books missing from the server scan (cleared cache or new): count chapters from the Tomato directory API.
    for (int i = 0; i < count && !s_cancel; i++) {
        if (found[i]) continue;
        char url[160];
        snprintf(url, sizeof(url), "https://fanqienovel.com/api/reader/directory/detail?bookId=%s", shelf[i].book_id);
        int chapters = 0;
        set_progress(i + 1, count);
        if (tomato_http_count_keys(url, "\"itemId\"", "\"item_id\"", &chapters) == ESP_OK && chapters > 0)
            shelf[i].remote = (uint16_t)chapters;
    }
    tomato_shelf_save(shelf, count);
    int pending = count_pending(shelf, count);
    set_pending(pending);
    char msg[48];
    if (pending) snprintf(msg, sizeof(msg), "%d 本有新章节", pending);
    else snprintf(msg, sizeof(msg), "都是最新的");
    set_phase(TOMATO_DONE, msg);
}

// 下载上次检查标出的更新。/ Download the updates marked by the last check.
static void run_update(const tomato_config_t* cfg, tomato_book_t* shelf, int count) {
    int pending = count_pending(shelf, count);
    if (!pending) { set_phase(TOMATO_DONE, "没有待下载的更新"); return; }
    int done = 0, updated = 0;
    for (int i = 0; i < count && !s_cancel; i++) {
        if (shelf[i].remote <= shelf[i].chapters) continue;
        set_progress(++done, pending);
        if (fetch_book(cfg, &shelf[i]) == ESP_OK) {
            updated++;
            tomato_shelf_save(shelf, count);
            portENTER_CRITICAL(&s_lock);
            s_status.updated = updated;
            portEXIT_CRITICAL(&s_lock);
        }
    }
    set_pending(count_pending(shelf, count));
    char msg[48];
    if (s_cancel) snprintf(msg, sizeof(msg), "已取消，更新了 %d 本", updated);
    else if (updated == pending) snprintf(msg, sizeof(msg), "更新了 %d 本", updated);
    else snprintf(msg, sizeof(msg), "更新了 %d 本，%d 本失败", updated, pending - updated);
    set_phase(TOMATO_DONE, msg);
}

static void run_search(const tomato_config_t* cfg) {
    set_phase(TOMATO_SEARCHING, s_query);
    char esc[3 * sizeof(s_query)], path[sizeof(esc) + 16];
    tomato_url_escape(s_query, esc, sizeof(esc), false);
    snprintf(path, sizeof(path), "/api/search?q=%s", esc);
    cJSON* root; int status;
    esp_err_t err = tomato_http_json(cfg, "GET", path, NULL, &root, &status);
    if (err != ESP_OK) { fail(err, "搜索失败"); return; }
    // 带简介后约 6 KB，放 PSRAM，不占任务栈。/ ~6 KB with synopses; keep it in PSRAM, off the task stack.
    tomato_hit_t* hits = heap_caps_calloc(TOMATO_SEARCH_MAX, sizeof(tomato_hit_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!hits) { cJSON_Delete(root); fail(ESP_ERR_NO_MEM, "内存不足"); return; }
    int n = 0;
    const cJSON* it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "items")) {
        if (n >= TOMATO_SEARCH_MAX) break;
        const cJSON* raw = cJSON_GetObjectItemCaseSensitive(it, "raw");
        tomato_hit_t* h = &hits[n];
        memset(h, 0, sizeof(*h));
        tomato_json_str(it, "book_id", h->book_id, sizeof(h->book_id));
        tomato_json_str(it, "author", h->author, sizeof(h->author));
        tomato_json_str(raw, "book_name", h->title, sizeof(h->title));
        tomato_json_str(raw, "category", h->category, sizeof(h->category));
        h->chapters = (uint16_t)tomato_json_int(raw, "serial_count");
        h->words = (uint32_t)tomato_json_int(raw, "word_number");
        // update_status 为 "0" 表示已完结。/ update_status "0" means completed.
        char st[4];
        tomato_json_str(raw, "update_status", st, sizeof(st));
        h->finished = !strcmp(st, "0");
        const cJSON* ab = cJSON_GetObjectItemCaseSensitive(raw, "abstract");
        if (cJSON_IsString(ab) && ab->valuestring) copy_utf8(h->abstract, sizeof(h->abstract), ab->valuestring);
        if (h->book_id[0] && h->title[0]) n++;
    }
    cJSON_Delete(root);
    portENTER_CRITICAL(&s_lock);
    memcpy(s_hits, hits, sizeof(hits[0]) * (size_t)n);
    s_status.hit_count = n;
    portEXIT_CRITICAL(&s_lock);
    heap_caps_free(hits);
    set_phase(TOMATO_DONE, n ? NULL : "没有找到");
}

static void run_download(const tomato_config_t* cfg, tomato_book_t* shelf, int count) {
    int at = -1;
    for (int i = 0; i < count; i++) if (!strcmp(shelf[i].book_id, s_target.book_id)) at = i;
    if (at < 0) {
        if (count >= TOMATO_SHELF_MAX) { fail(ESP_ERR_NO_MEM, "书架已满"); return; }
        at = count++;
        tomato_book_t* b = &shelf[at];
        memset(b, 0, sizeof(*b));
        snprintf(b->book_id, sizeof(b->book_id), "%s", s_target.book_id);
        snprintf(b->title, sizeof(b->title), "%s", s_target.title);
        snprintf(b->author, sizeof(b->author), "%s", s_target.author);
        b->remote = s_target.chapters;
    }
    set_progress(1, 1);
    esp_err_t err = fetch_book(cfg, &shelf[at]);
    if (err != ESP_OK) { fail(err, s_cancel ? "已取消" : "下载失败"); return; }
    tomato_shelf_save(shelf, count);
    set_phase(TOMATO_DONE, "已加入书架");
}

static void sync_task(void* arg) {
    (void)arg;
    tomato_config_t cfg;
    tomato_book_t* shelf = heap_caps_calloc(TOMATO_SHELF_MAX, sizeof(tomato_book_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int count = 0;
    if (!shelf) { fail(ESP_ERR_NO_MEM, "内存不足"); goto out; }
    // WebDAV 同步不需要番茄服务器。/ WebDAV sync does not need the Tomato server.
    if (s_op != OP_WEBDAV && tomato_config_load(&cfg) != ESP_OK) { fail(ESP_ERR_NOT_FOUND, "未设置服务器"); goto out; }
    tomato_shelf_load(shelf, TOMATO_SHELF_MAX, &count);
    // 没事可做就不连网。/ Skip networking when there is nothing to do.
    if ((s_op == OP_CHECK && !count) || (s_op == OP_UPDATE && !count_pending(shelf, count))) {
        set_phase(TOMATO_DONE, count ? "没有待下载的更新" : "书架上还没有番茄小说");
        goto out;
    }
    set_phase(TOMATO_CONNECTING, "连接 Wi-Fi");
    esp_err_t err = tomato_net_up(NET_TIMEOUT_MS);
    if (err != ESP_OK) { fail(err, err == ESP_ERR_NOT_FOUND ? "未设置 Wi-Fi" : "Wi-Fi 连接失败"); goto out; }
    switch (s_op) {
    case OP_CHECK: run_check(&cfg, shelf, count); break;
    case OP_UPDATE: run_update(&cfg, shelf, count); break;
    case OP_SEARCH: run_search(&cfg); break;
    case OP_DOWNLOAD: run_download(&cfg, shelf, count); break;
    case OP_WEBDAV: {
        set_phase(TOMATO_WEBDAV, "正在同步");
        char msg[64];
        err = tomato_webdav_run(s_export, s_import, msg, sizeof(msg));
        if (err == ESP_OK) set_phase(TOMATO_DONE, msg);
        else fail(err, msg);
        break;
    }
    }
    tomato_net_down();
out:
    heap_caps_free(shelf);
    memset(&cfg, 0, sizeof(cfg));
    s_busy = false;
    vTaskDelete(NULL);
}

static esp_err_t start(op_t op) {
    if (s_busy) return ESP_ERR_INVALID_STATE;
    s_busy = true;
    s_cancel = false;
    s_bytes = 0;
    s_op = op;
    portENTER_CRITICAL(&s_lock);
    int hits = s_status.hit_count;
    memset(&s_status, 0, sizeof(s_status));
    // 下载时保留搜索结果，返回结果页还能看到。/ Keep search hits across a download so the result page survives.
    if (op == OP_DOWNLOAD) s_status.hit_count = hits;
    portEXIT_CRITICAL(&s_lock);
    if (xTaskCreate(sync_task, "tomato_sync", TASK_STACK, NULL, 4, NULL) != pdPASS) {
        s_busy = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t tomato_sync_start_check(void) { return start(OP_CHECK); }
esp_err_t tomato_sync_start_webdav(void) { return start(OP_WEBDAV); }
esp_err_t tomato_sync_start_update(void) { return start(OP_UPDATE); }

esp_err_t tomato_sync_start_search(const char* query) {
    if (!query || !query[0] || s_busy) return ESP_ERR_INVALID_ARG;
    snprintf(s_query, sizeof(s_query), "%s", query);
    return start(OP_SEARCH);
}

esp_err_t tomato_sync_start_download(const tomato_hit_t* hit) {
    if (!hit || !hit->book_id[0] || s_busy) return ESP_ERR_INVALID_ARG;
    s_target = *hit;
    return start(OP_DOWNLOAD);
}
