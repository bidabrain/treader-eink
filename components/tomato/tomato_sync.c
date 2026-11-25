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

#include <stdarg.h>
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

typedef enum {
    OP_CHECK, OP_UPDATE, OP_SEARCH, OP_DOWNLOAD, OP_WEBDAV, OP_RANK, OP_RANK_COVER, OP_HUPU, OP_RED, OP_WB, OP_X, OP_WT,
    OP_OTA_CHECK, OP_OTA_DOWNLOAD, OP_TIME, OP_CHECK_ONE
} op_t;

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
static tomato_rank_book_t* s_rank_books;  // PSRAM，封面任务要取的书 / PSRAM, books for the cover op
static int s_rank_book_n;
static tomato_hupu_req_t s_hupu;
static tomato_red_req_t s_red;
static tomato_wb_req_t s_wb;
static tomato_x_req_t s_x;
static tomato_wt_req_t s_wt;
static tomato_ota_info_t s_ota;        // 检查结果，也是下载目标 / Check result, also the download target
static bool s_ota_valid;
// 每启动一次任务加一；页面据此认出“这是不是我发起的那个任务”。/ Bumped per started job so a page can tell its own job.
static uint32_t s_seq;
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

void tomato_sync_step(const char* fmt, ...) {
    char text[sizeof(s_status.step)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    portENTER_CRITICAL(&s_lock);
    memcpy(s_status.step, text, sizeof(text));
    s_status.revision++;
    portEXIT_CRITICAL(&s_lock);
}

static void fail(esp_err_t err, const char* message) {
    portENTER_CRITICAL(&s_lock);
    s_status.error = err;
    portEXIT_CRITICAL(&s_lock);
    // 有具体原因就接在后面（截断按字符边界）。/ Append the specific cause when there is one (cut on a char boundary).
    const char* detail = tomato_http_detail();
    if (message && detail[0] && err != ESP_ERR_INVALID_STATE) {
        char full[104];
        snprintf(full, sizeof(full), "%.56s（%.36s）", message, detail);
        set_phase(TOMATO_FAILED, full);
        return;
    }
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

// 浏览类任务（虎扑、小红书、微博、X、书城、WEBTOON 发现、搜索）可以让给前台；下载（包括 WEBTOON 更新）、检查更新
// 和 WebDAV 同步不打断。
// Browsing jobs (Hupu, Xiaohongshu, Weibo, X, bookstores, search) yield to the foreground; downloads (WEBTOON updates
// included), update checks and WebDAV sync are never interrupted.
static bool browsing(void) {
    return s_op == OP_HUPU || s_op == OP_RED || s_op == OP_WB || s_op == OP_X ||
           (s_op == OP_WT && s_wt.job != TOMATO_WT_UPDATE) || s_op == OP_RANK || s_op == OP_RANK_COVER ||
           s_op == OP_SEARCH;
}

bool tomato_sync_preemptible(void) {
    portENTER_CRITICAL(&s_lock);
    const bool yes = s_busy && browsing();
    portEXIT_CRITICAL(&s_lock);
    return yes;
}

tomato_preempt_t tomato_sync_preempt(void) {
    portENTER_CRITICAL(&s_lock);
    const bool busy = s_busy;
    const bool browse = browsing();
    if (busy && browse) s_cancel = true;
    portEXIT_CRITICAL(&s_lock);
    if (!busy) return TOMATO_PREEMPT_FREE;
    if (!browse) return TOMATO_PREEMPT_BLOCKED;
    // 只等一小会儿：连接或握手中的任务可能要十几秒才停，界面不能卡住。/ Wait only briefly: a job stuck in a
    // connect or handshake may take seconds to stop, and the UI must not freeze.
    for (int waited = 0; s_busy && waited < 300; waited += 20) vTaskDelay(pdMS_TO_TICKS(20));
    return s_busy ? TOMATO_PREEMPT_STOPPING : TOMATO_PREEMPT_FREE;
}

bool tomato_sync_stop(int timeout_ms) {
    if (s_busy) {
        s_cancel = true;
        for (int waited = 0; s_busy && waited < timeout_ms; waited += 50) vTaskDelay(pdMS_TO_TICKS(50));
    }
    // 任务做完还留着的 Wi-Fi 也一起断掉，锁屏浅睡前不能开着。/ Also drop Wi-Fi lingering after a job; it must not stay
    // up into light sleep.
    if (!s_busy) tomato_net_idle_off();
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
    // 本机第一次下载（新书，或 WebDAV 同步来、还没下载的）：下完算看过，书架显示“未读”而不是“+全部章”。
    // First download on this device (a new book, or one synced over WebDAV): count it as seen so it shows unread, not +all.
    const bool first = !book->file[0] || !book->chapters;
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
    if (first) book->seen = book->chapters;
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
static void fill_covers(const tomato_config_t* cfg, const tomato_book_t* shelf, int count, const char* only);

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
    // 顺带补缺的封面（之前下载时没取到的）。/ Fill missing covers on the way (ones a download failed to get).
    fill_covers(cfg, shelf, count, NULL);
    if (s_cancel) { fail(ESP_ERR_INVALID_STATE, "已取消"); return; }
    char msg[48];
    if (pending) snprintf(msg, sizeof(msg), "%d 本有新章节", pending);
    else snprintf(msg, sizeof(msg), "都是最新的");
    set_phase(TOMATO_DONE, msg);
}

// 只查一本：用番茄目录接口数章节，有新章节就直接下载。/ One book only: count chapters from the Tomato directory API
// and download at once when there are new ones.
static char s_one_id[TOMATO_ID_LEN];
static void run_check_one(const tomato_config_t* cfg, tomato_book_t* shelf, int count) {
    int i = 0;
    while (i < count && strcmp(shelf[i].book_id, s_one_id)) i++;
    if (i == count) { set_phase(TOMATO_DONE, "这本不在番茄书架上"); return; }
    tomato_book_t* b = &shelf[i];
    set_phase(TOMATO_CHECKING, "检查更新");
    snprintf(s_work->url, sizeof(s_work->url), "https://fanqienovel.com/api/reader/directory/detail?bookId=%s",
             b->book_id);
    int chapters = 0;
    if (tomato_http_count_keys(s_work->url, "\"itemId\"", "\"item_id\"", &s_cancel, &chapters) != ESP_OK ||
        chapters <= 0) {
        fail(s_cancel ? ESP_ERR_INVALID_STATE : ESP_FAIL, s_cancel ? "已取消" : "检查更新失败");
        return;
    }
    if (chapters > b->remote) b->remote = (uint16_t)chapters;
    tomato_shelf_save(shelf, count);
    set_pending(count_pending(shelf, count));
    fill_covers(cfg, shelf, count, b->book_id);
    if (s_cancel) { fail(ESP_ERR_INVALID_STATE, "已取消"); return; }
    set_phase(TOMATO_CHECKING, "检查更新");
    if (b->remote <= b->chapters) { set_phase(TOMATO_DONE, "这本已是最新"); return; }
    if (book_open_in_reader(b)) { set_phase(TOMATO_DONE, "有新章节，正在读，稍后再更新"); return; }
    const int before = b->chapters;
    esp_err_t err = fetch_book(cfg, b);
    if (err != ESP_OK) {
        fail(s_cancel ? ESP_ERR_INVALID_STATE : err, s_cancel ? "已取消" : "下载更新失败");
        return;
    }
    tomato_shelf_save(shelf, count);
    set_pending(count_pending(shelf, count));
    portENTER_CRITICAL(&s_lock);
    s_status.updated = 1;
    portEXIT_CRITICAL(&s_lock);
    char msg[48];
    snprintf(msg, sizeof(msg), "更新了 %d 章", b->chapters - before);
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

// 复制一个小文件（封面）：先写 .part 再改名，中途拔卡不会留下半张图（封面存在就不再下载，半张会一直坏着）。
// Copy a small file (a cover): write .part then rename, so a pull midway never leaves half an image (an existing cover
// is never fetched again, so half a cover would stay broken).
static bool copy_file(const char* from, const char* to) {
    FILE* in = fopen(from, "rb");
    if (!in) return false;
    char part[216];
    if (snprintf(part, sizeof(part), "%s.part", to) >= (int)sizeof(part)) { fclose(in); return false; }
    FILE* out = fopen(part, "wb");
    bool ok = out != NULL;
    char buf[1024];
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof(buf), in)) > 0) ok = fwrite(buf, 1, n, out) == n;
    fclose(in);
    if (out) ok = fclose(out) == 0 && ok;
    if (ok) {
        remove(to);
        ok = rename(part, to) == 0;
    }
    if (!ok) remove(part);
    return ok;
}

// 番茄封面带签名，搜索给的还是 1200 宽的 HEIC（设备解不了），去掉参数又会被拒（403）。同一张图换成不带签名的
// img 路径并要 300×400 的 JPEG：约 30 KB 的基线 JPEG，正好够封面用。返回是否是番茄图床；受限的图没有这种地址，
// out 留空。
// Tomato cover links are signed, and search hands out a 1200-wide HEIC the device cannot decode, while stripping the
// parameters is refused (403). The same image under the unsigned img path, asked for as a 300x400 JPEG, is a baseline
// JPEG of about 30 KB, just right for a cover. Returns whether this is Tomato's host; restricted images have no such
// address and leave out empty.
static bool cover_jpeg_url(const char* url, char* out, size_t cap) {
    // novel-pic-r 是受限的桶，不带签名的地址一律 403，直接交给网页那一步。/ novel-pic-r is a restricted bucket whose
    // unsigned addresses all answer 403; hand it straight to the web page step.
    if (strstr(url, "/novel-pic-r/")) { out[0] = 0; return true; }
    const char* key = strstr(url, "/novel-pic/");
    if (!key) return false;
    key += strlen("/novel-pic/");
    const size_t n = strcspn(key, "~?/");
    if (!n) return false;
    return snprintf(out, cap, "https://p6-novel.byteimg.com/img/novel-pic/%.*s~300x400.jpeg", (int)n, key) < (int)cap;
}

// 番茄网页（/page/书号）开头 2 KB 内的结构化数据里有带签名的 JPEG 封面地址（"image":["…"]）。novel-pic-r 这类
// 受限的图没有不带签名的地址，靠它取。返回是否找到。
// The Tomato web page (/page/<book id>) carries a signed JPEG cover address in the structured data within its first
// 2 KB ("image":["..."]). Restricted images such as novel-pic-r have no unsigned address, so this is how they come.
// Returns whether one was found.
#define PAGE_HEAD_MAX (16 * 1024)
static bool page_cover_url(const char* book_id, char* out, size_t cap) {
    if (!book_id[0]) return false;
    char* head = heap_caps_malloc(PAGE_HEAD_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!head) return false;
    snprintf(out, cap, "https://fanqienovel.com/page/%s", book_id);
    bool ok = false;
    if (tomato_http_head(out, head, PAGE_HEAD_MAX, &s_cancel) == ESP_OK) {
        static const char KEY[] = "\"image\":[\"";
        const char* p = strstr(head, KEY);
        if (p) {
            p += sizeof(KEY) - 1;
            const size_t n = strcspn(p, "\"");
            ok = n && p[n] == '"' && n < cap && !strncmp(p, "https://", 8);
            if (ok) snprintf(out, cap, "%.*s", (int)n, p);
        }
    }
    heap_caps_free(head);
    if (!ok) out[0] = 0;
    return ok;
}

// 封面尽力而为：失败只是显示文字封面，不影响下载结果。
// Covers are best effort: a failure only means a text cover; the download still succeeds.
// 这本书的封面文件路径；路径太长返回 false。/ This book's cover file path; false when too long.
static bool cover_dest(const tomato_book_t* book, char* dest, size_t cap) {
    return book->file[0] && snprintf(dest, cap, "%s/%s.img", TOMATO_COVER_DIR, book->file) < (int)cap;
}

static bool cover_missing(const tomato_book_t* book) {
    char dest[200];
    struct stat st;
    return cover_dest(book, dest, sizeof(dest)) && stat(dest, &st) != 0;
}

static void fetch_cover(const tomato_book_t* book, const char* thumb) {
    if (s_cancel) return;
    char dest[200];
    if (!cover_dest(book, dest, sizeof(dest))) return;
    struct stat st;
    if (stat(dest, &st) == 0) return;
    mkdir(TOMATO_COVER_DIR, 0775);
    set_phase(TOMATO_DOWNLOADING, "下载封面");
    // 书城详情页已经下过这张封面就直接复制。/ Reuse the cover the bookstore detail already fetched.
    char cached[96];
    tomato_rank_cover_path(book->book_id, cached, sizeof(cached));
    if (copy_file(cached, dest)) return;
    if (!thumb || !thumb[0]) return;
    // 番茄图床先要 300×400 的 JPEG，不行再用原地址；别的图床直接用原地址，拿到 WEBP 这类就去掉处理参数再要一次。
    // Tomato's host: ask for the 300x400 JPEG first, then the original address; other hosts: the original address,
    // retrying without processing params when it hands back WEBP and the like.
    char* alt = s_work->url;
    if (cover_jpeg_url(thumb, alt, sizeof(s_work->url))) {
        if (alt[0] && tomato_http_fetch(alt, dest, 1024 * 1024, true, &s_cancel) == ESP_OK) return;
        if (s_cancel) return;
        ESP_LOGW(TAG, "cover %.*s", (int)strcspn(thumb, "?"), thumb);
        // 受限的图（novel-pic-r）没有不带签名的地址：从番茄网页取带签名的 JPEG 地址。/ Restricted images
        // (novel-pic-r) have no unsigned address: take the signed JPEG address from the Tomato web page.
        if (page_cover_url(book->book_id, alt, sizeof(s_work->url)) &&
            (tomato_http_fetch(alt, dest, 1024 * 1024, true, &s_cancel) == ESP_OK || s_cancel))
            return;
        tomato_http_fetch(thumb, dest, 1024 * 1024, true, &s_cancel);
        return;
    }
    if (tomato_http_fetch(thumb, dest, 1024 * 1024, true, &s_cancel) == ESP_OK || s_cancel) return;
    ESP_LOGW(TAG, "cover %.*s", (int)strcspn(thumb, "?"), thumb);
    snprintf(alt, sizeof(s_work->url), "%s", thumb);
    char* cut = strchr(alt, '~');
    if (!cut) cut = strchr(alt, '?');
    if (!cut) return;
    *cut = 0;
    tomato_http_fetch(alt, dest, 1024 * 1024, true, &s_cancel);
}

// 已经下好的书没存封面地址：按书名到服务器搜一次，用书号对上那一条的封面地址。/ Downloaded books keep no cover
// address: search the server by title once and take the cover address of the hit with the same book id.
static bool find_thumb(const tomato_config_t* cfg, const tomato_book_t* book, char* out, size_t cap) {
    out[0] = 0;
    tomato_url_escape(book->title, s_work->esc, sizeof(s_work->esc), false);
    snprintf(s_work->path, sizeof(s_work->path), "/api/search?q=%s", s_work->esc);
    cJSON* root = NULL;
    int status;
    if (tomato_http_json(cfg, "GET", s_work->path, NULL, &root, &status) != ESP_OK) return false;
    const cJSON* it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "items")) {
        char id[TOMATO_ID_LEN];
        tomato_json_str(it, "book_id", id, sizeof(id));
        if (strcmp(id, book->book_id)) continue;
        const cJSON* thumb = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(it, "raw"), "thumb_url");
        if (cJSON_IsString(thumb) && thumb->valuestring && strlen(thumb->valuestring) < cap)
            strcpy(out, thumb->valuestring);
        break;
    }
    cJSON_Delete(root);
    return out[0] != 0;
}

// 补缺的封面：only 非空只补这一本，否则补书架上所有缺的（一次最多 COVER_FILL_MAX 本，下次检查接着补）。尽力而为，
// 失败不影响检查结果。/ Fill missing covers: only that book when only is set, else every shelf book missing one (at
// most COVER_FILL_MAX per run; the next check continues). Best effort; failures never change the check result.
#define COVER_FILL_MAX 10
static void fill_covers(const tomato_config_t* cfg, const tomato_book_t* shelf, int count, const char* only) {
    int need = 0;
    for (int i = 0; i < count; i++)
        need += (!only || !strcmp(shelf[i].book_id, only)) && shelf[i].title[0] && cover_missing(&shelf[i]);
    if (!need) return;
    if (need > COVER_FILL_MAX) need = COVER_FILL_MAX;
    set_phase(TOMATO_DOWNLOADING, "下载封面");
    mkdir(TOMATO_COVER_DIR, 0775);
    char* thumb = heap_caps_malloc(sizeof(s_target.thumb), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!thumb) return;
    int done = 0;
    for (int i = 0; i < count && done < need && !s_cancel; i++) {
        const tomato_book_t* b = &shelf[i];
        if ((only && strcmp(b->book_id, only)) || !b->title[0] || !cover_missing(b)) continue;
        set_progress(++done, need);
        // 书城下过就直接复制，不用搜。/ Copy the bookstore's cover when it has one; no search needed.
        char cached[96];
        tomato_rank_cover_path(b->book_id, cached, sizeof(cached));
        char dest[200];
        if (cover_dest(b, dest, sizeof(dest)) && copy_file(cached, dest)) continue;
        if (find_thumb(cfg, b, thumb, sizeof(s_target.thumb))) fetch_cover(b, thumb);
    }
    heap_caps_free(thumb);
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

// 按当前榜单地址取分类列表和（要的话）分类。/ Fetch the index and, if asked, a category from the current rank site.
static esp_err_t rank_fetch_once(void) {
    struct stat st;
    // 只取分类时总是刷新；取某个分类时，没有分类列表才顺带取。/ Index-only always refreshes; a category fetch adds it if missing.
    if (!s_rank_has_cat || stat(tomato_rank_index_path(), &st) != 0) {
        esp_err_t err = fetch_rank_index();
        if (err != ESP_OK) return err;
    }
    if (s_rank_has_cat) {
        char path[96];
        tomato_rank_cat_path(&s_rank_cat, path, sizeof(path));
        if (!tomato_rank_cat_url(&s_rank_cat, s_work->url, sizeof(s_work->url))) return ESP_ERR_INVALID_SIZE;
        return tomato_http_fetch(s_work->url, path, 256 * 1024, false, &s_cancel);
    }
    return ESP_OK;
}

static void run_rank(void) {
    mkdir(TOMATO_DIR, 0775);
    mkdir(TOMATO_RANK_DIR, 0775);
    set_phase(TOMATO_SEARCHING, "正在获取榜单");
    // 先用上次通的地址；失败就依次换 jsDelivr 镜像（只对 github.io 站点有），通了就记住。
    // Start with the site that worked last; on failure try the jsDelivr mirrors (github.io sites only) and remember a hit.
    const int n = tomato_rank_mirror_count(), first = tomato_rank_mirror();
    esp_err_t err = ESP_FAIL;
    for (int k = 0; k < n && !s_cancel; k++) {
        tomato_rank_use_mirror((first + k) % n);
        if (k) set_phase(TOMATO_SEARCHING, "换镜像获取榜单");
        tomato_http_clear_detail();
        err = rank_fetch_once();
        if (err == ESP_OK || err == ESP_ERR_INVALID_SIZE) break;
    }
    if (err == ESP_ERR_INVALID_SIZE) { fail(err, "榜单地址太长"); return; }
    if (err != ESP_OK) { fail(err, s_cancel ? "已取消" : "榜单获取失败，检查网络和榜单地址"); return; }
    set_phase(TOMATO_DONE, tomato_rank_mirror() ? "榜单已更新（经镜像）" : "榜单已更新");
}

// 取一张榜单封面。/ Fetch one rank cover.
static esp_err_t fetch_rank_cover(const tomato_rank_book_t* b, const char* dest) {
    esp_err_t err = tomato_http_fetch(b->cover, dest, 1024 * 1024, true, &s_cancel);
    if (err != ESP_OK && !s_cancel) {
        // 和番茄下载一样，拿到 WEBP 就换 300×400 的 JPEG 再要一次。/ As with downloads, retry the 300x400 JPEG on WEBP.
        if (cover_jpeg_url(b->cover, s_work->url, sizeof(s_work->url))) {
            if (s_work->url[0]) err = tomato_http_fetch(s_work->url, dest, 1024 * 1024, true, &s_cancel);
        } else {
            snprintf(s_work->url, sizeof(s_work->url), "%s", b->cover);
            char* cut = strchr(s_work->url, '~');
            if (!cut) cut = strchr(s_work->url, '?');
            if (cut) { *cut = 0; err = tomato_http_fetch(s_work->url, dest, 1024 * 1024, true, &s_cancel); }
        }
    }
    return err;
}

// 一次联网取完一个分类缺的封面；单张失败只跳过。/ Fetch a category's missing covers in one session; a failed one is skipped.
static void run_rank_cover(void) {
    mkdir(TOMATO_RANK_DIR, 0775);
    mkdir(TOMATO_RANK_DIR "/covers", 0775);
    tomato_rank_prune_covers(s_rank_books, s_rank_book_n);
    char dest[96];
    struct stat st;
    int need = 0;
    for (int i = 0; i < s_rank_book_n; i++) {
        tomato_rank_cover_path(s_rank_books[i].book_id, dest, sizeof(dest));
        if (s_rank_books[i].cover[0] && stat(dest, &st) != 0) need++;
    }
    if (!need) { set_phase(TOMATO_DONE, NULL); return; }
    set_phase(TOMATO_DOWNLOADING, "下载封面");
    int done = 0, got = 0;
    for (int i = 0; i < s_rank_book_n && !s_cancel; i++) {
        tomato_rank_cover_path(s_rank_books[i].book_id, dest, sizeof(dest));
        if (!s_rank_books[i].cover[0] || stat(dest, &st) == 0) continue;
        set_progress(++done, need);
        if (fetch_rank_cover(&s_rank_books[i], dest) == ESP_OK) got++;
    }
    if (s_cancel) fail(ESP_ERR_INVALID_STATE, "已取消");
    else if (!got) fail(ESP_FAIL, "封面下载失败");
    else set_phase(TOMATO_DONE, NULL);
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
    fetch_cover(&shelf[at], s_target.thumb);
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
    tomato_http_clear_detail();
    if (!shelf || !s_work) { fail(ESP_ERR_NO_MEM, "内存不足"); goto out; }
    // WebDAV 同步和书城不需要番茄服务器。/ WebDAV sync and the bookstore do not need the Tomato server.
    const bool needs_server =
        s_op != OP_WEBDAV && s_op != OP_RANK && s_op != OP_RANK_COVER && s_op != OP_HUPU && s_op != OP_RED &&
        s_op != OP_WB && s_op != OP_X && s_op != OP_WT && s_op != OP_OTA_CHECK && s_op != OP_OTA_DOWNLOAD && s_op != OP_TIME;
    if (needs_server && tomato_config_load(&cfg) != ESP_OK) { fail(ESP_ERR_NOT_FOUND, "未设置服务器"); goto out; }
    tomato_shelf_load(shelf, TOMATO_SHELF_MAX, &count);
    // 没事可做就不连网。/ Skip networking when there is nothing to do.
    if (((s_op == OP_CHECK || s_op == OP_CHECK_ONE) && !count) || (s_op == OP_UPDATE && !count_pending(shelf, count))) {
        set_phase(TOMATO_DONE, count ? "没有待下载的更新" : "书架上还没有番茄小说");
        goto out;
    }
    set_phase(TOMATO_CONNECTING, "连接 Wi-Fi");
    esp_err_t err = tomato_net_up(NET_TIMEOUT_MS, &s_cancel);
    if (err != ESP_OK) {
        // 只有真的取消才用 ESP_ERR_INVALID_STATE，页面据此不提示；其余失败一律 ESP_FAIL 带原因。
        // Only a real cancel reports ESP_ERR_INVALID_STATE (pages stay quiet); other failures are ESP_FAIL with a cause.
        fail(s_cancel ? ESP_ERR_INVALID_STATE : err == ESP_ERR_NOT_FOUND ? ESP_ERR_NOT_FOUND : ESP_FAIL,
             s_cancel ? "已取消" : err == ESP_ERR_NOT_FOUND ? "未设置 Wi-Fi" : "Wi-Fi 连接失败");
        goto out;
    }
    switch (s_op) {
    case OP_CHECK: run_check(&cfg, shelf, count); break;
    case OP_UPDATE: run_update(&cfg, shelf, count); break;
    case OP_CHECK_ONE: run_check_one(&cfg, shelf, count); break;
    case OP_SEARCH: run_search(&cfg); break;
    case OP_DOWNLOAD: run_download(&cfg, shelf, count); break;
    case OP_RANK: run_rank(); break;
    case OP_RANK_COVER: run_rank_cover(); break;
    case OP_HUPU: {
        set_phase(TOMATO_DOWNLOADING, "正在加载");
        char msg[64] = "";
        const tomato_hupu_req_t* r = &s_hupu;
        switch (r->job) {
        case TOMATO_HUPU_HOME: err = tomato_hupu_home(&s_cancel, set_progress, msg, sizeof(msg)); break;
        case TOMATO_HUPU_POST: err = tomato_hupu_post(r->id, r->more, &s_cancel, set_progress, msg, sizeof(msg)); break;
        case TOMATO_HUPU_ZONES: err = tomato_hupu_zones(&s_cancel, msg, sizeof(msg)); break;
        case TOMATO_HUPU_THREADS: err = tomato_hupu_threads(r->id, r->sort, r->more, &s_cancel, msg, sizeof(msg)); break;
        case TOMATO_HUPU_FOLLOW: err = tomato_hupu_follow(r->pages, &s_cancel, set_progress, msg, sizeof(msg)); break;
        case TOMATO_HUPU_SCHEDULE: err = tomato_hupu_schedule(r->id, &s_cancel, msg, sizeof(msg)); break;
        case TOMATO_HUPU_RATING: err = tomato_hupu_rating(r->id, r->key, &s_cancel, msg, sizeof(msg)); break;
        default: err = ESP_ERR_INVALID_ARG; break;
        }
        if (err == ESP_OK) set_phase(TOMATO_DONE, msg);
        else fail(err, s_cancel ? "已取消" : msg);
        break;
    }
    case OP_RED: {
        set_phase(TOMATO_DOWNLOADING, "正在加载");
        char msg[96] = "";
        err = tomato_red_run(&s_red, &s_cancel, set_progress, msg, sizeof(msg));
        if (err == ESP_OK) set_phase(TOMATO_DONE, msg);
        else fail(err, s_cancel ? "已取消" : msg);
        break;
    }
    case OP_WB: {
        set_phase(TOMATO_DOWNLOADING, "正在加载");
        char msg[96] = "";
        err = tomato_wb_run(&s_wb, &s_cancel, set_progress, msg, sizeof(msg));
        if (err == ESP_OK) set_phase(TOMATO_DONE, msg);
        else fail(err, s_cancel ? "已取消" : msg);
        break;
    }
    case OP_X: {
        set_phase(TOMATO_DOWNLOADING, "正在加载");
        char msg[96] = "";
        err = tomato_x_run(&s_x, &s_cancel, set_progress, msg, sizeof(msg));
        if (err == ESP_OK) set_phase(TOMATO_DONE, msg);
        else fail(err, s_cancel ? "已取消" : msg);
        break;
    }
    case OP_WT: {
        set_phase(TOMATO_DOWNLOADING, s_wt.job == TOMATO_WT_UPDATE ? "正在检查更新" : "正在加载");
        char msg[96] = "";
        err = tomato_wt_run(&s_wt, &s_cancel, set_progress, msg, sizeof(msg));
        if (err == ESP_OK) set_phase(TOMATO_DONE, msg);
        else fail(err, s_cancel ? "已取消" : msg);
        break;
    }
    case OP_OTA_CHECK: {
        set_phase(TOMATO_CHECKING, "正在检查");
        char msg[96] = "";
        tomato_ota_info_t info;
        err = tomato_ota_check(&info, &s_cancel, msg, sizeof(msg));
        if (err == ESP_OK) {
            portENTER_CRITICAL(&s_lock);
            s_ota = info;
            s_ota_valid = true;
            portEXIT_CRITICAL(&s_lock);
            set_phase(TOMATO_DONE, msg);
        } else {
            fail(err, s_cancel ? "已取消" : msg);
        }
        break;
    }
    case OP_OTA_DOWNLOAD: {
        set_phase(TOMATO_DOWNLOADING, "正在下载");
        char msg[96] = "";
        tomato_ota_info_t info;
        portENTER_CRITICAL(&s_lock);
        info = s_ota;
        portEXIT_CRITICAL(&s_lock);
        err = tomato_ota_download(&info, &s_cancel, set_progress, msg, sizeof(msg));
        if (err == ESP_OK) set_phase(TOMATO_DONE, msg);
        else fail(s_cancel ? ESP_ERR_INVALID_STATE : err, msg);
        break;
    }
    case OP_TIME:
        set_phase(TOMATO_CHECKING, "正在校时");
        if (tomato_net_sync_time(8000)) set_phase(TOMATO_DONE, "已校时");
        else fail(ESP_FAIL, "校时失败，连不上时间服务器");
        break;
    case OP_WEBDAV: {
        set_phase(TOMATO_WEBDAV, "正在同步");
        char msg[64];
        err = tomato_webdav_run(s_export, s_import, msg, sizeof(msg));
        if (err == ESP_OK) set_phase(TOMATO_DONE, msg);
        else fail(err, msg);
        break;
    }
    }
    // Wi-Fi 先留一会儿：切到下一篇帖子之类紧接着的任务不用重新连。/ Keep Wi-Fi a little: a job right after this one
    // (say, the next post) need not reconnect.
    tomato_net_linger();
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
    s_seq++;
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
esp_err_t tomato_sync_start_check_one(const char* book_id) {
    if (!book_id || !book_id[0]) return ESP_ERR_INVALID_ARG;
    if (s_busy) return ESP_ERR_INVALID_STATE;
    snprintf(s_one_id, sizeof(s_one_id), "%s", book_id);
    return start(OP_CHECK_ONE);
}
esp_err_t tomato_sync_start_hupu(const tomato_hupu_req_t* req) {
    if (!req) return ESP_ERR_INVALID_ARG;
    if (s_busy) return ESP_ERR_INVALID_STATE;
    s_hupu = *req;
    s_hupu.id[sizeof(s_hupu.id) - 1] = 0;
    s_hupu.key[sizeof(s_hupu.key) - 1] = 0;
    return start(OP_HUPU);
}
uint32_t tomato_sync_seq(void) {
    portENTER_CRITICAL(&s_lock);
    const uint32_t seq = s_seq;
    portEXIT_CRITICAL(&s_lock);
    return seq;
}

esp_err_t tomato_sync_start_red(const tomato_red_req_t* req) {
    if (!req) return ESP_ERR_INVALID_ARG;
    if (s_busy) return ESP_ERR_INVALID_STATE;
    s_red = *req;
    s_red.id[sizeof(s_red.id) - 1] = 0;
    return start(OP_RED);
}
esp_err_t tomato_sync_start_wb(const tomato_wb_req_t* req) {
    if (!req) return ESP_ERR_INVALID_ARG;
    if (s_busy) return ESP_ERR_INVALID_STATE;
    s_wb = *req;
    s_wb.id[sizeof(s_wb.id) - 1] = 0;
    return start(OP_WB);
}
esp_err_t tomato_sync_start_x(const tomato_x_req_t* req) {
    if (!req) return ESP_ERR_INVALID_ARG;
    if (s_busy) return ESP_ERR_INVALID_STATE;
    s_x = *req;
    s_x.id[sizeof(s_x.id) - 1] = 0;
    return start(OP_X);
}
bool tomato_wt_updating(void) {
    portENTER_CRITICAL(&s_lock);
    const bool yes = s_busy && s_op == OP_WT && s_wt.job == TOMATO_WT_UPDATE;
    portEXIT_CRITICAL(&s_lock);
    return yes;
}

bool tomato_sync_downloading(void) {
    portENTER_CRITICAL(&s_lock);
    const bool yes = s_busy && !s_cancel &&
                     (s_op == OP_DOWNLOAD || s_op == OP_UPDATE || (s_op == OP_WT && s_wt.job == TOMATO_WT_UPDATE));
    portEXIT_CRITICAL(&s_lock);
    return yes;
}

uint32_t tomato_sync_stamp(void) {
    portENTER_CRITICAL(&s_lock);
    const uint32_t revision = s_status.revision;
    portEXIT_CRITICAL(&s_lock);
    return revision * 2654435761u ^ s_bytes;
}

esp_err_t tomato_sync_start_wt(const tomato_wt_req_t* req) {
    if (!req) return ESP_ERR_INVALID_ARG;
    if (s_busy) return ESP_ERR_INVALID_STATE;
    s_wt = *req;
    s_wt.seg[sizeof(s_wt.seg) - 1] = 0;
    s_wt.key[sizeof(s_wt.key) - 1] = 0;
    s_wt.q[sizeof(s_wt.q) - 1] = 0;
    s_wt.id[sizeof(s_wt.id) - 1] = 0;
    return start(OP_WT);
}
esp_err_t tomato_sync_start_webdav(void) { return start(OP_WEBDAV); }
esp_err_t tomato_sync_start_time(void) { return start(OP_TIME); }

esp_err_t tomato_sync_start_ota_check(void) { return start(OP_OTA_CHECK); }

bool tomato_sync_ota_info(tomato_ota_info_t* out) {
    portENTER_CRITICAL(&s_lock);
    const bool ok = s_ota_valid;
    if (ok) *out = s_ota;
    portEXIT_CRITICAL(&s_lock);
    return ok;
}

esp_err_t tomato_sync_start_ota_download(const tomato_ota_info_t* info) {
    if (!info) return ESP_ERR_INVALID_ARG;
    if (s_busy) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_lock);
    s_ota = *info;
    s_ota.name[sizeof(s_ota.name) - 1] = 0;
    s_ota.url[sizeof(s_ota.url) - 1] = 0;
    s_ota.version[sizeof(s_ota.version) - 1] = 0;
    s_ota_valid = true;
    portEXIT_CRITICAL(&s_lock);
    return start(OP_OTA_DOWNLOAD);
}
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

esp_err_t tomato_sync_start_rank_covers(const tomato_rank_book_t* books, int n) {
    if (!books || n <= 0) return ESP_ERR_INVALID_ARG;
    if (s_busy) return ESP_ERR_INVALID_STATE;
    if (!s_rank_books)
        s_rank_books = heap_caps_malloc(TOMATO_RANK_BOOK_MAX * sizeof(*s_rank_books), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rank_books) return ESP_ERR_NO_MEM;
    s_rank_book_n = n > TOMATO_RANK_BOOK_MAX ? TOMATO_RANK_BOOK_MAX : n;
    memcpy(s_rank_books, books, (size_t)s_rank_book_n * sizeof(*s_rank_books));
    return start(OP_RANK_COVER);
}

esp_err_t tomato_sync_start_download(const tomato_hit_t* hit) {
    if (!hit || !hit->book_id[0]) return ESP_ERR_INVALID_ARG;
    if (s_busy) return ESP_ERR_INVALID_STATE;
    s_target = *hit;
    return start(OP_DOWNLOAD);
}
