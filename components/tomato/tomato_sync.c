/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 后台同步任务：检查更新、搜索、下载、WebDAV。同一时间只跑一个操作。
 * Background sync task: update check, search, download, WebDAV. One operation at a time.
 *
 * 更新判断沿用安卓版 v1.7.3 的规则：服务器章节数取 max(remote_total, local_total)，
 * 大于本地 TXT 的章节数才重新下载；不用服务器的 has_update 标志。
 * Update rule follows the Android v1.7.3 fix: server chapters = max(remote_total, local_total);
 * re-download only when that exceeds the local TXT; never trust the server has_update flag.
 *
 * 冻结：不在主循环任务里做网络；状态用临界区保护，页面只读快照。正在阅读的书不替换文件（阅读器还开着它）。
 * 大缓冲放 PSRAM 的工作区，不占任务栈。
 * Frozen: no networking on the main loop task; status is guarded by a critical section and pages read snapshots.
 * The book open in the reader is never replaced. Large buffers live in a PSRAM workspace, off the task stack.
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tomato_internal.h"

#define TAG "tomato_sync"
// HTTPS 握手吃栈；大缓冲已移到工作区。/ TLS handshakes need stack; big buffers moved to the workspace.
#define TASK_STACK 14336
#define NET_TIMEOUT_MS 20000
#define POLL_MS 2000
#define SCAN_WAIT_POLLS 30   // 更新扫描最多等 60 秒 / Update scan waits at most 60 s
#define JOB_WAIT_POLLS 300   // 生成任务最多等 10 分钟 / Server job waits at most 10 min
#define BUSY_RETRIES 6
#define DEFER_MAX 8

typedef enum { OP_CHECK, OP_UPDATE, OP_SEARCH, OP_DOWNLOAD, OP_WEBDAV, OP_RANK, OP_RANK_COVER } op_t;

// 同步任务的大缓冲，任务开始时在 PSRAM 分配。/ The task's large buffers, allocated in PSRAM when it starts.
typedef struct {
    char esc[3 * TOMATO_TITLE_LEN + 4];
    char path[1200];
    char dir[256];
    char dir_esc[3 * 256 + 4];
    char rel[256];
    char rel_esc[3 * 256 + 4];
    char url[3 * 256 + 32];
} work_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static tomato_status_t s_status;
static tomato_hit_t* s_hits;           // PSRAM，首次搜索时分配 / PSRAM, allocated on first search
static SemaphoreHandle_t s_hits_mutex;
static volatile bool s_cancel;
static volatile bool s_busy;
static volatile uint32_t s_bytes;
static op_t s_op;
static char s_query[96];
static tomato_hit_t s_target;
static tomato_rank_cat_t s_rank_cat;
static bool s_rank_has_cat;
static tomato_rank_book_t* s_rank_book;   // PSRAM，封面任务用 / PSRAM, for the cover op
static work_t* s_work;
static tomato_file_hook_t s_file_hook;
static tomato_open_check_t s_open_check;
static tomato_progress_export_t s_export;
static tomato_progress_import_t s_import;
static char s_deferred[DEFER_MAX][TOMATO_TITLE_LEN + 8];
static int s_deferred_count;

void tomato_sync_set_progress_io(tomato_progress_export_t export_cb, tomato_progress_import_t import_cb) {
    s_export = export_cb;
    s_import = import_cb;
}

void tomato_sync_set_file_hook(tomato_file_hook_t hook) { s_file_hook = hook; }
void tomato_sync_set_open_check(tomato_open_check_t check) { s_open_check = check; }

/* ---- 状态 / Status ---- */

static void set_phase(tomato_phase_t phase, const char* message) {
    portENTER_CRITICAL(&s_lock);
    s_status.phase = phase;
    if (message) tomato_copy_utf8(s_status.message, sizeof(s_status.message), message);
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
    if (!s_hits_mutex || !s_hits) return 0;
    xSemaphoreTake(s_hits_mutex, portMAX_DELAY);
    int n = s_status.hit_count < max ? s_status.hit_count : max;
    memcpy(out, s_hits, sizeof(out[0]) * (size_t)n);
    xSemaphoreGive(s_hits_mutex);
    return n;
}

bool tomato_sync_busy(void) { return s_busy; }
void tomato_sync_cancel(void) { s_cancel = true; }

bool tomato_sync_stop(int timeout_ms) {
    if (!s_busy) return true;
    s_cancel = true;
    for (int waited = 0; s_busy && waited < timeout_ms; waited += 50) vTaskDelay(pdMS_TO_TICKS(50));
    return !s_busy;
}

bool tomato_sync_defer_remove(const char* file) {
    bool queued = false;
    portENTER_CRITICAL(&s_lock);
    if (s_busy && s_deferred_count < DEFER_MAX) {
        tomato_copy_utf8(s_deferred[s_deferred_count++], sizeof(s_deferred[0]), file);
        queued = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return queued;
}

static bool wait_poll(void) {
    for (int i = 0; i < POLL_MS / 100; i++) {
        if (s_cancel) return false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return !s_cancel;
}

/* ---- 文件名 / File names ---- */

bool tomato_file_name_safe(const char* file) {
    if (!file[0] || strstr(file, "..")) return false;
    for (const char* p = file; *p; p++) if (strchr("/\\:*?\"<>|", *p) || (unsigned char)*p < 32) return false;
    return true;
}

// FAT 文件名里不能出现的字符换成下划线。/ Replace characters FAT filenames cannot hold.
static void safe_name(const char* title, const char* suffix, char* out, size_t cap) {
    size_t o = 0;
    const size_t room = cap - strlen(suffix) - 5;
    for (const char* p = title; *p && o < room; p++) {
        unsigned char c = (unsigned char)*p;
        out[o++] = strchr("/\\:*?\"<>|", c) || c < 32 ? '_' : (char)c;
    }
    // 停在字符中间就退回字符开头，不留半个汉字。/ If stopped mid-character, back up to its start.
    while (o && ((unsigned char)title[o] & 0xC0) == 0x80) o--;
    out[o] = 0;
    if (!o) snprintf(out, cap, "book");
    strncat(out, suffix, cap - strlen(out) - 1);
    strncat(out, ".txt", cap - strlen(out) - 1);
}

static bool path_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static bool book_open_in_reader(const tomato_book_t* book) {
    if (!s_open_check || !book->file[0]) return false;
    char path[TOMATO_TITLE_LEN + 32];
    snprintf(path, sizeof(path), TOMATO_BOOKS_DIR "/%s", book->file);
    return s_open_check(path);
}

/* ---- 服务器操作 / Server operations ---- */

// 字段存在且不是 null。/ Field present and not null.
static bool has_value(const cJSON* obj, const char* key) {
    const cJSON* v = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : NULL;
    return v && !cJSON_IsNull(v);
}

static bool hard_http_error(int status) { return status == 401 || status == 403 || (status >= 400 && status != 429 && status < 500); }

// 在 library 里找书名对应的 TXT 相对路径，写进 s_work->rel。/ Find the TXT path for a title into s_work->rel.
static esp_err_t find_library_file(const tomato_config_t* cfg, const char* title) {
    work_t* w = s_work;
    tomato_url_escape(title, w->esc, sizeof(w->esc), false);
    for (int attempt = 0; attempt < SCAN_WAIT_POLLS; attempt++) {
        if (attempt && !wait_poll()) return ESP_ERR_INVALID_STATE;
        snprintf(w->path, sizeof(w->path), "/api/library?name=%s&start=%s", w->esc, attempt ? "false" : "true");
        cJSON* root; int status;
        if (tomato_http_json(cfg, "GET", w->path, NULL, &root, &status) != ESP_OK) {
            if (hard_http_error(status)) return ESP_FAIL;
            continue;
        }
        bool running = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "running"));
        if (running) { cJSON_Delete(root); continue; }
        const cJSON* it;
        w->dir[0] = 0;
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "items")) {
            char kind[8], ext[8];
            tomato_json_str(it, "kind", kind, sizeof(kind));
            tomato_json_str(it, "ext", ext, sizeof(ext));
            if (!strcmp(kind, "file") && !strcmp(ext, "txt")) {
                tomato_json_str(it, "rel_path", w->rel, sizeof(w->rel));
                cJSON_Delete(root);
                return ESP_OK;
            }
            if (!strcmp(kind, "dir") && !w->dir[0]) tomato_json_str(it, "rel_path", w->dir, sizeof(w->dir));
        }
        cJSON_Delete(root);
        if (!w->dir[0]) return ESP_ERR_NOT_FOUND;
        // 书名子目录：再进去找一次。/ Title subdirectory: look inside once.
        tomato_url_escape(w->dir, w->dir_esc, sizeof(w->dir_esc), false);
        snprintf(w->path, sizeof(w->path), "/api/library?path=%s&name=%s", w->dir_esc, w->esc);
        if (tomato_http_json(cfg, "GET", w->path, NULL, &root, &status) != ESP_OK) return ESP_FAIL;
        cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "items")) {
            char kind[8], ext[8];
            tomato_json_str(it, "kind", kind, sizeof(kind));
            tomato_json_str(it, "ext", ext, sizeof(ext));
            if (!strcmp(kind, "file") && !strcmp(ext, "txt")) {
                tomato_json_str(it, "rel_path", w->rel, sizeof(w->rel));
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
    work_t* w = s_work;
    char body[64];
    cJSON* root; int status = 0;
    int job = -1;
    snprintf(body, sizeof(body), "{\"book_id\":\"%s\"}", book->book_id);
    set_phase(TOMATO_QUEUED, "服务器准备中");
    for (int i = 0; i < BUSY_RETRIES; i++) {
        esp_err_t err = tomato_http_json(cfg, "POST", "/api/jobs", body, &root, &status);
        if (err == ESP_OK) {
            const cJSON* id = cJSON_GetObjectItemCaseSensitive(root, "id");
            job = cJSON_IsNumber(id) ? id->valueint : -1;
            cJSON_Delete(root);
            break;
        }
        if (status != 429) return ESP_FAIL;
        if (!wait_poll() || !wait_poll()) return ESP_ERR_INVALID_STATE;
    }
    if (job < 0) return ESP_ERR_TIMEOUT;

    char title[TOMATO_TITLE_LEN] = "";
    int chapter_total = 0;
    bool done = false;
    for (int poll = 0; poll < JOB_WAIT_POLLS && !done; poll++) {
        if (!wait_poll()) return ESP_ERR_INVALID_STATE;
        snprintf(w->path, sizeof(w->path), "/api/jobs?id=%d", job);
        if (tomato_http_json(cfg, "GET", w->path, NULL, &root, &status) != ESP_OK) {
            if (hard_http_error(status)) return ESP_FAIL;   // 任务不存在或没权限，别空等 10 分钟 / Job gone or forbidden; don't wait 10 min
            continue;
        }
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
            snprintf(w->path, sizeof(w->path), "/api/jobs/%d/format", job);
            if (tomato_http_json(cfg, "POST", w->path, "{\"value\":\"txt\"}", &root, &status) == ESP_OK) cJSON_Delete(root);
        }
        if (need_name) {
            snprintf(w->path, sizeof(w->path), "/api/jobs/%d/book_name", job);
            if (tomato_http_json(cfg, "POST", w->path, "{\"value\":null}", &root, &status) == ESP_OK) cJSON_Delete(root);
        }
    }
    if (!done) return ESP_ERR_TIMEOUT;
    if (!title[0]) tomato_copy_utf8(title, sizeof(title), book->title);

    esp_err_t err = find_library_file(cfg, title);
    if (err != ESP_OK) return err;

    char file[sizeof(book->file)], dest[TOMATO_TITLE_LEN + 32];
    if (book->file[0] && tomato_file_name_safe(book->file)) {
        // 更新时沿用原文件名，阅读进度跟着文件走。/ Updates keep the old file name so reading progress follows it.
        tomato_copy_utf8(file, sizeof(file), book->file);
    } else {
        // 新书：同名文件已存在就加上 book_id 尾号，不覆盖别的书。/ New book: add a book_id suffix rather than overwrite another file.
        safe_name(title, "", file, sizeof(file));
        snprintf(dest, sizeof(dest), TOMATO_BOOKS_DIR "/%s", file);
        if (path_exists(dest)) {
            char suffix[12];
            size_t idl = strlen(book->book_id);
            snprintf(suffix, sizeof(suffix), "_%s", book->book_id + (idl > 6 ? idl - 6 : 0));
            safe_name(title, suffix, file, sizeof(file));
        }
    }
    snprintf(dest, sizeof(dest), TOMATO_BOOKS_DIR "/%s", file);
    tomato_url_escape(w->rel, w->rel_esc, sizeof(w->rel_esc), true);
    snprintf(w->url, sizeof(w->url), "/download/%s", w->rel_esc);
    set_phase(TOMATO_DOWNLOADING, title);
    struct stat st;
    uint32_t old_size = stat(dest, &st) == 0 ? (uint32_t)st.st_size : 0;
    err = tomato_http_download(cfg, w->url, dest, &s_cancel, &s_bytes);
    if (err != ESP_OK) return err;
    if (s_file_hook && stat(dest, &st) == 0) s_file_hook(dest, old_size, (uint32_t)st.st_size);

    tomato_copy_utf8(book->title, sizeof(book->title), title);
    tomato_copy_utf8(book->file, sizeof(book->file), file);
    if (chapter_total > book->chapters) book->chapters = (uint16_t)chapter_total;
    if (book->remote > book->chapters) book->chapters = book->remote;
    return ESP_OK;
}

// 拉服务器更新扫描，把章节数写进 shelf[i].remote。/ Run the server update scan and fill shelf[i].remote.
static esp_err_t fetch_remote_totals(const tomato_config_t* cfg, tomato_book_t* shelf, int count, uint8_t* found) {
    set_phase(TOMATO_CHECKING, "检查更新");
    cJSON* root = NULL; int status;
    esp_err_t err = tomato_http_json(cfg, "GET", "/api/updates?start=true", NULL, &root, &status);
    bool running = err == ESP_OK && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "running"));
    for (int i = 0; running && i < SCAN_WAIT_POLLS; i++) {
        cJSON_Delete(root); root = NULL;
        if (!wait_poll()) return ESP_ERR_INVALID_STATE;
        err = tomato_http_json(cfg, "GET", "/api/updates?start=false", NULL, &root, &status);
        running = err == ESP_OK && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "running"));
    }
    if (err != ESP_OK) return err;
    // 等满 60 秒还没扫完：别拿半截结果判断。/ Still scanning after 60 s: don't decide from partial results.
    if (running) { cJSON_Delete(root); return ESP_ERR_TIMEOUT; }
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

static const char* server_error_text(int status, const char* fallback) {
    if (status == 401 || status == 403) return "服务器密码不对";
    if (status == 429) return "服务器忙，稍后再试";
    return fallback;
}

// 只检查：更新每本书的 remote 并保存，不下载。/ Check only: refresh each book's remote and save; no downloads.
static void run_check(const tomato_config_t* cfg, tomato_book_t* shelf, int count) {
    if (!count) { set_phase(TOMATO_DONE, "书架上还没有番茄小说"); return; }
    uint8_t found[TOMATO_SHELF_MAX] = {0};
    esp_err_t err = fetch_remote_totals(cfg, shelf, count, found);
    if (err != ESP_OK) {
        fail(err, s_cancel ? "已取消" : err == ESP_ERR_TIMEOUT ? "服务器扫描超时，稍后再试" : "检查更新失败");
        return;
    }
    // 服务器扫描里没有的书（缓存被清或新加），直接查番茄目录接口数章节。
    // Books missing from the server scan (cleared cache or new): count chapters from the Tomato directory API.
    for (int i = 0; i < count && !s_cancel; i++) {
        if (found[i]) continue;
        snprintf(s_work->url, sizeof(s_work->url),
                 "https://fanqienovel.com/api/reader/directory/detail?bookId=%s", shelf[i].book_id);
        int chapters = 0;
        set_progress(i + 1, count);
        if (tomato_http_count_keys(s_work->url, "\"itemId\"", "\"item_id\"", &s_cancel, &chapters) == ESP_OK && chapters > 0)
            shelf[i].remote = (uint16_t)chapters;
    }
    if (s_cancel) { fail(ESP_ERR_INVALID_STATE, "已取消"); return; }
    tomato_shelf_save(shelf, count);
    int pending = count_pending(shelf, count);
    set_pending(pending);
    char msg[48];
    if (pending) snprintf(msg, sizeof(msg), "%d 本有新章节", pending);
    else snprintf(msg, sizeof(msg), "都是最新的");
    set_phase(TOMATO_DONE, msg);
}

// 下载上次检查标出的更新；正在阅读的那本跳过。/ Download updates marked by the last check; skip the book being read.
static void run_update(const tomato_config_t* cfg, tomato_book_t* shelf, int count) {
    int pending = count_pending(shelf, count);
    if (!pending) { set_phase(TOMATO_DONE, "没有待下载的更新"); return; }
    int done = 0, updated = 0, skipped = 0;
    for (int i = 0; i < count && !s_cancel; i++) {
        if (shelf[i].remote <= shelf[i].chapters) continue;
        set_progress(++done, pending);
        if (book_open_in_reader(&shelf[i])) { skipped++; continue; }
        if (fetch_book(cfg, &shelf[i]) == ESP_OK) {
            updated++;
            tomato_shelf_save(shelf, count);
            portENTER_CRITICAL(&s_lock);
            s_status.updated = updated;
            portEXIT_CRITICAL(&s_lock);
        }
    }
    set_pending(count_pending(shelf, count));
    char msg[64];
    int failed = pending - updated - skipped;
    if (s_cancel) snprintf(msg, sizeof(msg), "已取消，更新了 %d 本", updated);
    else if (skipped && !failed) snprintf(msg, sizeof(msg), "更新了 %d 本，正在读的那本稍后再更新", updated);
    else if (!failed) snprintf(msg, sizeof(msg), "更新了 %d 本", updated);
    else snprintf(msg, sizeof(msg), "更新了 %d 本，%d 本失败", updated, failed);
    set_phase(TOMATO_DONE, msg);
}

static void run_search(const tomato_config_t* cfg) {
    set_phase(TOMATO_SEARCHING, s_query);
    tomato_url_escape(s_query, s_work->esc, sizeof(s_work->esc), false);
    snprintf(s_work->path, sizeof(s_work->path), "/api/search?q=%s", s_work->esc);
    cJSON* root; int status;
    esp_err_t err = tomato_http_json(cfg, "GET", s_work->path, NULL, &root, &status);
    if (err != ESP_OK) { fail(err, server_error_text(status, "搜索失败")); return; }
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
        tomato_json_str(raw, "abstract", h->abstract, sizeof(h->abstract));
        // 地址截断就没法用，太长宁可不要。/ A truncated URL is useless; drop it when too long.
        const cJSON* thumb = cJSON_GetObjectItemCaseSensitive(raw, "thumb_url");
        if (cJSON_IsString(thumb) && thumb->valuestring && strlen(thumb->valuestring) < sizeof(h->thumb))
            strcpy(h->thumb, thumb->valuestring);
        if (h->book_id[0] && h->title[0]) n++;
    }
    cJSON_Delete(root);
    xSemaphoreTake(s_hits_mutex, portMAX_DELAY);
    memcpy(s_hits, hits, sizeof(hits[0]) * (size_t)n);
    portENTER_CRITICAL(&s_lock);
    s_status.hit_count = n;
    portEXIT_CRITICAL(&s_lock);
    xSemaphoreGive(s_hits_mutex);
    heap_caps_free(hits);
    set_phase(TOMATO_DONE, n ? NULL : "没有找到");
}

// 复制一个小文件（封面）。/ Copy a small file (a cover).
static bool copy_file(const char* from, const char* to) {
    FILE* in = fopen(from, "rb");
    if (!in) return false;
    FILE* out = fopen(to, "wb");
    bool ok = out != NULL;
    char buf[1024];
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0) ok = fwrite(buf, 1, n, out) == n;
    fclose(in);
    if (out) ok = fclose(out) == 0 && ok;
    if (!ok) remove(to);
    return ok;
}

// 封面尽力而为：失败只是显示文字封面，不影响下载结果。
// Covers are best effort: a failure only means a text cover; the download still succeeds.
static void fetch_cover(const tomato_book_t* book) {
    if (!s_target.thumb[0] || !book->file[0] || s_cancel) return;
    char dest[200];
    if (snprintf(dest, sizeof(dest), "%s/%s.img", TOMATO_COVER_DIR, book->file) >= (int)sizeof(dest)) return;
    struct stat st;
    if (stat(dest, &st) == 0) return;
    mkdir(TOMATO_COVER_DIR, 0775);
    set_phase(TOMATO_DOWNLOADING, "下载封面");
    // 书城详情页已经下过这张封面就直接复制。/ Reuse the cover the bookstore detail already fetched.
    char cached[96];
    tomato_rank_cover_path(book->book_id, cached, sizeof(cached));
    if (copy_file(cached, dest)) return;
    if (tomato_http_fetch(s_target.thumb, dest, 1024 * 1024, true, &s_cancel) == ESP_OK) return;
    // 图床有时按格式协商给 WEBP；去掉处理参数再要一次原图。/ Hosts may hand back WEBP; retry the bare image without processing params.
    char* bare = s_work->url;
    snprintf(bare, sizeof(s_work->url), "%s", s_target.thumb);
    char* cut = strchr(bare, '~');
    if (!cut) cut = strchr(bare, '?');
    if (!cut || s_cancel) return;
    *cut = 0;
    tomato_http_fetch(bare, dest, 1024 * 1024, true, &s_cancel);
}

/* ---- 书城 / Bookstore ---- */

// 取分类列表；榜单换了日期就清掉旧分类缓存。/ Fetch the index; drop old category caches when the date changes.
static esp_err_t fetch_rank_index(void) {
    char old_date[16], new_date[16];
    tomato_rank_index_date(old_date, sizeof(old_date));
    if (!tomato_rank_index_url(s_work->url, sizeof(s_work->url))) return ESP_ERR_INVALID_SIZE;
    esp_err_t err = tomato_http_fetch(s_work->url, tomato_rank_index_path(), 64 * 1024, false, &s_cancel);
    if (err != ESP_OK) return err;
    tomato_rank_index_date(new_date, sizeof(new_date));
    if (strcmp(old_date, new_date)) tomato_rank_drop_categories();
    return ESP_OK;
}

static void run_rank(void) {
    mkdir(TOMATO_DIR, 0775);
    mkdir(TOMATO_RANK_DIR, 0775);
    set_phase(TOMATO_SEARCHING, "正在获取榜单");
    struct stat st;
    // 只取分类时总是刷新；取某个分类时，没有分类列表才顺带取。/ Index-only always refreshes; a category fetch adds it if missing.
    if (!s_rank_has_cat || stat(tomato_rank_index_path(), &st) != 0) {
        esp_err_t err = fetch_rank_index();
        if (err != ESP_OK) { fail(err, s_cancel ? "已取消" : "榜单获取失败，检查网络和榜单地址"); return; }
    }
    if (s_rank_has_cat) {
        char path[96];
        tomato_rank_cat_path(&s_rank_cat, path, sizeof(path));
        if (!tomato_rank_cat_url(&s_rank_cat, s_work->url, sizeof(s_work->url))) { fail(ESP_ERR_INVALID_SIZE, "榜单地址太长"); return; }
        esp_err_t err = tomato_http_fetch(s_work->url, path, 256 * 1024, false, &s_cancel);
        if (err != ESP_OK) { fail(err, s_cancel ? "已取消" : "榜单获取失败，检查网络和榜单地址"); return; }
    }
    set_phase(TOMATO_DONE, "榜单已更新");
}

static void run_rank_cover(void) {
    if (!s_rank_book || !s_rank_book->cover[0]) { set_phase(TOMATO_DONE, NULL); return; }
    mkdir(TOMATO_RANK_DIR, 0775);
    mkdir(TOMATO_RANK_DIR "/covers", 0775);
    char dest[96];
    tomato_rank_cover_path(s_rank_book->book_id, dest, sizeof(dest));
    set_phase(TOMATO_DOWNLOADING, "下载封面");
    esp_err_t err = tomato_http_fetch(s_rank_book->cover, dest, 1024 * 1024, true, &s_cancel);
    if (err != ESP_OK && !s_cancel) {
        // 和番茄下载一样，拿到 WEBP 就去掉处理参数再要一次。/ As with downloads, retry without processing params on WEBP.
        snprintf(s_work->url, sizeof(s_work->url), "%s", s_rank_book->cover);
        char* cut = strchr(s_work->url, '~');
        if (!cut) cut = strchr(s_work->url, '?');
        if (cut) { *cut = 0; err = tomato_http_fetch(s_work->url, dest, 1024 * 1024, true, &s_cancel); }
    }
    if (err == ESP_OK) set_phase(TOMATO_DONE, NULL);
    else fail(err, "封面下载失败");
}

static void run_download(const tomato_config_t* cfg, tomato_book_t* shelf, int count) {
    int at = -1;
    for (int i = 0; i < count; i++) if (!strcmp(shelf[i].book_id, s_target.book_id)) at = i;
    if (at < 0) {
        if (count >= TOMATO_SHELF_MAX) { fail(ESP_ERR_NO_MEM, "书架已满"); return; }
        at = count++;
        tomato_book_t* b = &shelf[at];
        memset(b, 0, sizeof(*b));
        tomato_copy_utf8(b->book_id, sizeof(b->book_id), s_target.book_id);
        tomato_copy_utf8(b->title, sizeof(b->title), s_target.title);
        tomato_copy_utf8(b->author, sizeof(b->author), s_target.author);
        b->remote = s_target.chapters;
    }
    if (book_open_in_reader(&shelf[at])) { fail(ESP_ERR_INVALID_STATE, "这本书正在阅读，先退出再更新"); return; }
    set_progress(1, 1);
    esp_err_t err = fetch_book(cfg, &shelf[at]);
    if (err != ESP_OK) { fail(err, s_cancel ? "已取消" : "下载失败"); return; }
    tomato_shelf_save(shelf, count);
    // 重新下载的书不再算“已删除”。/ A re-downloaded book is no longer "removed".
    tomato_removed_mark(shelf[at].book_id, false);
    fetch_cover(&shelf[at]);
    set_phase(TOMATO_DONE, "已加入书架");
}

// 结束前处理同步期间书库删掉的书，并在同一把锁里清掉忙标志，保证不漏。
// Before ending, apply removals queued during the sync and clear busy under the same lock so none slip through.
static void finish_and_drain(void) {
    for (;;) {
        char file[TOMATO_TITLE_LEN + 8];
        portENTER_CRITICAL(&s_lock);
        if (!s_deferred_count) {
            s_busy = false;
            portEXIT_CRITICAL(&s_lock);
            return;
        }
        memcpy(file, s_deferred[--s_deferred_count], sizeof(file));
        portEXIT_CRITICAL(&s_lock);
        tomato_shelf_remove_now(file);
    }
}

static void sync_task(void* arg) {
    (void)arg;
    tomato_config_t cfg = {0};
    tomato_book_t* shelf = heap_caps_calloc(TOMATO_SHELF_MAX, sizeof(tomato_book_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_work = heap_caps_calloc(1, sizeof(work_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int count = 0;
    if (!shelf || !s_work) { fail(ESP_ERR_NO_MEM, "内存不足"); goto out; }
    // WebDAV 同步和书城不需要番茄服务器。/ WebDAV sync and the bookstore do not need the Tomato server.
    const bool needs_server = s_op != OP_WEBDAV && s_op != OP_RANK && s_op != OP_RANK_COVER;
    if (needs_server && tomato_config_load(&cfg) != ESP_OK) { fail(ESP_ERR_NOT_FOUND, "未设置服务器"); goto out; }
    tomato_shelf_load(shelf, TOMATO_SHELF_MAX, &count);
    // 没事可做就不连网。/ Skip networking when there is nothing to do.
    if ((s_op == OP_CHECK && !count) || (s_op == OP_UPDATE && !count_pending(shelf, count))) {
        set_phase(TOMATO_DONE, count ? "没有待下载的更新" : "书架上还没有番茄小说");
        goto out;
    }
    set_phase(TOMATO_CONNECTING, "连接 Wi-Fi");
    esp_err_t err = tomato_net_up(NET_TIMEOUT_MS, &s_cancel);
    if (err != ESP_OK) {
        fail(err, s_cancel ? "已取消" : err == ESP_ERR_NOT_FOUND ? "未设置 Wi-Fi" : "Wi-Fi 连接失败");
        goto out;
    }
    switch (s_op) {
    case OP_CHECK: run_check(&cfg, shelf, count); break;
    case OP_UPDATE: run_update(&cfg, shelf, count); break;
    case OP_SEARCH: run_search(&cfg); break;
    case OP_DOWNLOAD: run_download(&cfg, shelf, count); break;
    case OP_RANK: run_rank(); break;
    case OP_RANK_COVER: run_rank_cover(); break;
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
    heap_caps_free(s_work);
    s_work = NULL;
    memset(&cfg, 0, sizeof(cfg));
    finish_and_drain();
    vTaskDelete(NULL);
}

static esp_err_t start(op_t op) {
    tomato_json_use_psram();
    if (!s_hits_mutex) s_hits_mutex = xSemaphoreCreateMutex();
    if (!s_hits) s_hits = heap_caps_calloc(TOMATO_SEARCH_MAX, sizeof(tomato_hit_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_hits_mutex || !s_hits) return ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&s_lock);
    if (s_busy) { portEXIT_CRITICAL(&s_lock); return ESP_ERR_INVALID_STATE; }
    s_busy = true;
    s_cancel = false;
    s_bytes = 0;
    s_op = op;
    int hits = s_status.hit_count;
    memset(&s_status, 0, sizeof(s_status));
    // 只有新的搜索才清结果，其他操作保留，返回结果页还能看到。/ Only a new search clears hits; other ops keep them.
    if (op != OP_SEARCH) s_status.hit_count = hits;
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
    if (!query || !query[0]) return ESP_ERR_INVALID_ARG;
    if (s_busy) return ESP_ERR_INVALID_STATE;
    tomato_copy_utf8(s_query, sizeof(s_query), query);
    return start(OP_SEARCH);
}

esp_err_t tomato_sync_start_rank(const tomato_rank_cat_t* cat) {
    if (s_busy) return ESP_ERR_INVALID_STATE;
    s_rank_has_cat = cat != NULL;
    if (cat) s_rank_cat = *cat;
    return start(OP_RANK);
}

esp_err_t tomato_sync_start_rank_cover(const tomato_rank_book_t* book) {
    if (!book || !book->book_id[0] || !book->cover[0]) return ESP_ERR_INVALID_ARG;
    if (s_busy) return ESP_ERR_INVALID_STATE;
    if (!s_rank_book) s_rank_book = heap_caps_malloc(sizeof(*s_rank_book), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rank_book) return ESP_ERR_NO_MEM;
    *s_rank_book = *book;
    return start(OP_RANK_COVER);
}

esp_err_t tomato_sync_start_download(const tomato_hit_t* hit) {
    if (!hit || !hit->book_id[0]) return ESP_ERR_INVALID_ARG;
    if (s_busy) return ESP_ERR_INVALID_STATE;
    s_target = *hit;
    return start(OP_DOWNLOAD);
}
