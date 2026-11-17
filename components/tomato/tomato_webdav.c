/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * WebDAV 同步：服务器目录下放一个 treader-sync.json，GET 读、PUT 写，HTTP Basic 认证。
 * 同步阅读进度和番茄书架，不传书籍文件。
 *
 * WebDAV sync: one treader-sync.json under the server directory, read with GET and written
 * with PUT using HTTP Basic auth. Syncs reading progress and the Tomato shelf, never book files.
 *
 * 合并规则：TF 卡 tomato/webdav_state.json 记着上次同步时每本书的进度签名和同步时间。
 * - 本机进度和签名不同：本机改过，以本机为准。
 * - 本机没改、服务器那条在上次同步之后更新过：以服务器为准写回本机。
 * - 本机这本书从没同步过、服务器上已有：以服务器为准（新设备或重置后不会把别处的进度冲掉）。
 * - 番茄书架按 book_id 取并集；只在服务器上的书加进本机书架，章节数记 0，等用户点“下载全部更新”。
 *   书库删掉的书记在“已删除”清单里，两边都不再加回；远端的文件名不可信，新加的书由下载时重新命名。
 * - 没对过时不同步；服务器文件读得到却解析不了时不覆盖它。
 *
 * Merge: tomato/webdav_state.json on TF records each book's progress signature and the last sync time.
 * - Local differs from its signature: changed here, local wins.
 * - Local unchanged and the remote entry is newer than the last sync: remote wins and is written back.
 * - A book never synced here but present remotely takes the remote entry (new or reset devices don't clobber others).
 * - The Tomato shelf is a union by book_id; remote-only books join locally with 0 chapters, pending download.
 *   Books deleted from the library stay on a removed list and are never re-added; remote file names are not
 *   trusted, so new books get named at download time.
 * - No sync without a valid clock; a remote file that downloads but fails to parse is never overwritten.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "tomato_internal.h"

#define TAG "tomato_dav"
#define SYNC_FILE "treader-sync.json"
#define STATE_PATH TOMATO_DIR "/webdav_state.json"
#define BODY_MAX (256 * 1024)
#define PROGRESS_MAX 128

/* ---- HTTP ---- */

// GET 时把响应读进 *body（调用方释放）；PUT 时发送 body。/ GET reads into *body (caller frees); PUT sends body.
static esp_err_t dav_request(const tomato_webdav_config_t* cfg, bool put, const char* out_body,
                             char** in_body, int* status) {
    char url[224];
    snprintf(url, sizeof(url), "%s/" SYNC_FILE, cfg->url);
    esp_http_client_config_t hc = {
        .url = url,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .method = put ? HTTP_METHOD_PUT : HTTP_METHOD_GET,
        .username = cfg->user[0] ? cfg->user : NULL,
        .password = cfg->user[0] ? cfg->password : NULL,
        .auth_type = cfg->user[0] ? HTTP_AUTH_TYPE_BASIC : HTTP_AUTH_TYPE_NONE,
    };
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) return ESP_ERR_NO_MEM;
    *status = 0;
    if (in_body) *in_body = NULL;
    int len = out_body ? (int)strlen(out_body) : 0;
    if (put) esp_http_client_set_header(c, "Content-Type", "application/json");
    char* buf = NULL;
    esp_err_t err = tomato_http_open(c, len);
    if (err == ESP_OK && len && esp_http_client_write(c, out_body, len) != len) err = ESP_FAIL;
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(c);
        *status = esp_http_client_get_status_code(c);
    }
    if (err == ESP_OK && in_body && *status == 200) {
        buf = heap_caps_malloc(BODY_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        int got = 0;
        while (buf) {
            int n = esp_http_client_read(c, buf + got, BODY_MAX - got);
            if (n < 0) { err = ESP_FAIL; break; }
            if (n == 0) break;
            got += n;
            if (got >= BODY_MAX) { err = ESP_ERR_INVALID_SIZE; break; }
        }
        if (!buf) err = ESP_ERR_NO_MEM;
        else { buf[got] = 0; *in_body = buf; buf = NULL; }
    }
    heap_caps_free(buf);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (err != ESP_OK) ESP_LOGW(TAG, "%s: %s http=%d", put ? "PUT" : "GET", esp_err_to_name(err), *status);
    return err;
}

/* ---- 小工具 / Helpers ---- */

static void signature(const tomato_progress_t* p, char* out, size_t cap) {
    snprintf(out, cap, "%lu:%u:%lu", (unsigned long)p->size, p->chapter, (unsigned long)p->off);
}

static void progress_to_json(cJSON* obj, const tomato_progress_t* p, long long when) {
    cJSON_AddNumberToObject(obj, "size", p->size);
    cJSON_AddNumberToObject(obj, "chapter", p->chapter);
    cJSON_AddNumberToObject(obj, "off", p->off);
    cJSON_AddNumberToObject(obj, "px", p->px);
    cJSON_AddNumberToObject(obj, "pct", p->pct);
    cJSON_AddNumberToObject(obj, "time", (double)when);
}

static void progress_from_json(const cJSON* obj, const char* path, tomato_progress_t* p) {
    memset(p, 0, sizeof(*p));
    snprintf(p->path, sizeof(p->path), "%s", path);
    p->size = (uint32_t)tomato_json_int(obj, "size");
    p->chapter = (uint16_t)tomato_json_int(obj, "chapter");
    p->off = (uint32_t)tomato_json_int(obj, "off");
    p->px = (uint8_t)tomato_json_int(obj, "px");
    p->pct = (uint8_t)tomato_json_int(obj, "pct");
}

static double json_num(const cJSON* obj, const char* key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

static cJSON* read_state(void) {
    FILE* f = fopen(STATE_PATH, "rb");
    if (!f) return NULL;
    char* buf = heap_caps_malloc(BODY_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t n = buf ? fread(buf, 1, BODY_MAX, f) : 0;
    fclose(f);
    if (!buf) return NULL;
    buf[n] = 0;
    cJSON* root = cJSON_ParseWithLength(buf, n);
    heap_caps_free(buf);
    return root;
}

static esp_err_t write_state(const cJSON* root) {
    char* text = cJSON_PrintUnformatted(root);
    if (!text) return ESP_ERR_NO_MEM;
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%s.tmp", STATE_PATH);
    FILE* f = fopen(tmp, "wb");
    size_t len = strlen(text);
    bool ok = f && fwrite(text, 1, len, f) == len;
    if (f && fclose(f) != 0) ok = false;
    cJSON_free(text);
    if (!ok) { remove(tmp); return ESP_FAIL; }
    remove(STATE_PATH);
    return rename(tmp, STATE_PATH) == 0 ? ESP_OK : ESP_FAIL;
}

/* ---- 番茄书架合并 / Tomato shelf merge ---- */

static bool id_in(const char (*ids)[TOMATO_ID_LEN], int n, const char* id) {
    for (int i = 0; i < n; i++) if (!strcmp(ids[i], id)) return true;
    return false;
}

// 合并远端书架；返回本机书架是否变了，added 记新增本数。/ Merge the remote shelf; returns whether the local shelf changed.
static bool merge_shelf(cJSON* remote_list, tomato_book_t* shelf, int* count,
                        const char (*removed)[TOMATO_ID_LEN], int removed_n, int* added) {
    bool changed = false;
    *added = 0;
    const cJSON* it;
    cJSON_ArrayForEach(it, remote_list) {
        char id[TOMATO_ID_LEN];
        tomato_json_str(it, "book_id", id, sizeof(id));
        if (!id[0] || id_in(removed, removed_n, id)) continue;
        int remote_total = tomato_json_int(it, "chapters");
        int r2 = tomato_json_int(it, "remote");
        if (r2 > remote_total) remote_total = r2;
        int at = -1;
        for (int i = 0; i < *count; i++) if (!strcmp(shelf[i].book_id, id)) at = i;
        if (at < 0) {
            if (*count >= TOMATO_SHELF_MAX) continue;
            tomato_book_t* b = &shelf[(*count)++];
            memset(b, 0, sizeof(*b));
            tomato_copy_utf8(b->book_id, sizeof(b->book_id), id);
            tomato_json_str(it, "title", b->title, sizeof(b->title));
            tomato_json_str(it, "author", b->author, sizeof(b->author));
            // 文件名留空：本机还没有这本，下载时再起名。/ Leave the file empty: not downloaded here yet; named at download.
            b->chapters = 0;
            b->remote = (uint16_t)(remote_total > 0 ? remote_total : 1);
            (*added)++;
            changed = true;
        } else if (remote_total > shelf[at].remote) {
            shelf[at].remote = (uint16_t)remote_total;
            changed = true;
        }
    }
    // 另一台设备删掉的书，本机书架也去掉记录（文件不删）。/ Drop shelf records another device removed (files stay).
    int write = 0;
    for (int i = 0; i < *count; i++) {
        if (id_in(removed, removed_n, shelf[i].book_id)) { changed = true; continue; }
        shelf[write++] = shelf[i];
    }
    *count = write;
    return changed;
}

static cJSON* shelf_to_json(const tomato_book_t* shelf, int count) {
    cJSON* arr = cJSON_CreateArray();
    for (int i = 0; arr && i < count; i++) {
        cJSON* o = cJSON_CreateObject();
        if (!o) break;
        cJSON_AddStringToObject(o, "book_id", shelf[i].book_id);
        cJSON_AddStringToObject(o, "title", shelf[i].title);
        cJSON_AddStringToObject(o, "author", shelf[i].author);
        cJSON_AddStringToObject(o, "file", shelf[i].file);
        cJSON_AddNumberToObject(o, "chapters", shelf[i].chapters);
        cJSON_AddNumberToObject(o, "remote", shelf[i].remote);
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

/* ---- 主流程 / Main flow ---- */

esp_err_t tomato_webdav_run(tomato_progress_export_t export_cb, tomato_progress_import_t import_cb,
                            char* msg, size_t cap) {
    tomato_webdav_config_t cfg;
    if (tomato_webdav_config_load(&cfg) != ESP_OK) { snprintf(msg, cap, "未设置 WebDAV"); return ESP_ERR_NOT_FOUND; }
    if (!export_cb || !import_cb) { snprintf(msg, cap, "同步未就绪"); return ESP_ERR_INVALID_STATE; }

    tomato_progress_t* local = heap_caps_calloc(PROGRESS_MAX, sizeof(*local), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    tomato_book_t* shelf = heap_caps_calloc(TOMATO_SHELF_MAX, sizeof(*shelf), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    cJSON *remote = NULL, *state = NULL;
    char* body = NULL;
    esp_err_t err = ESP_OK;
    int uploaded = 0, downloaded = 0, added = 0;
    if (!local || !shelf) { err = ESP_ERR_NO_MEM; snprintf(msg, cap, "内存不足"); goto done; }

    if (!tomato_time_valid()) {
        // 没有可信时间就判断不了新旧。/ Without a trusted clock, newer and older cannot be told apart.
        err = ESP_ERR_INVALID_STATE;
        snprintf(msg, cap, "对时失败，暂不能同步");
        goto done;
    }
    int status = 0;
    err = dav_request(&cfg, false, NULL, &body, &status);
    if (err != ESP_OK || (status != 200 && status != 404)) {
        snprintf(msg, cap, status == 401 ? "WebDAV 用户名或密码不对" : "连不上 WebDAV 服务器");
        if (err == ESP_OK) err = ESP_FAIL;
        goto done;
    }
    remote = body ? cJSON_Parse(body) : NULL;
    if (!remote && status == 200) {
        // 文件在但读不懂（被截断或是登录页）：别用空内容覆盖别的设备的数据。
        // The file exists but does not parse (truncated or a login page): never overwrite other devices' data.
        err = ESP_ERR_INVALID_RESPONSE;
        snprintf(msg, cap, "服务器上的同步文件损坏，未覆盖");
        goto done;
    }
    if (!remote) remote = cJSON_CreateObject();
    cJSON* rp = cJSON_GetObjectItemCaseSensitive(remote, "progress");
    if (!cJSON_IsObject(rp)) {
        cJSON_DeleteItemFromObjectCaseSensitive(remote, "progress");
        rp = cJSON_AddObjectToObject(remote, "progress");
    }
    state = read_state();
    if (!state) state = cJSON_CreateObject();
    double last = json_num(state, "last");
    cJSON* sigs = cJSON_GetObjectItemCaseSensitive(state, "sig");
    const long long now = (long long)time(NULL);

    // 本机 → 服务器，或服务器 → 本机。/ Local to remote, or remote to local.
    int n = export_cb(local, PROGRESS_MAX);
    for (int i = 0; i < n; i++) {
        char sig[48];
        signature(&local[i], sig, sizeof(sig));
        const cJSON* old = cJSON_GetObjectItemCaseSensitive(sigs, local[i].path);
        bool never_synced = !cJSON_IsString(old);
        bool changed = !never_synced && strcmp(old->valuestring, sig);
        cJSON* r = cJSON_GetObjectItemCaseSensitive(rp, local[i].path);
        // 从没同步过且服务器有：用服务器的；没改过且服务器更新过：也用服务器的。
        // Never synced and present remotely: take remote; unchanged here and newer remotely: take remote too.
        if (r && (never_synced || (!changed && json_num(r, "time") > last))) {
            tomato_progress_t in;
            progress_from_json(r, local[i].path, &in);
            if (import_cb(&in)) downloaded++;
            continue;
        }
        if (!r || changed) {
            // 本机新改的或服务器没有的，上传。/ Upload what changed here or is missing remotely.
            cJSON_DeleteItemFromObjectCaseSensitive(rp, local[i].path);
            cJSON* o = cJSON_AddObjectToObject(rp, local[i].path);
            if (o) { progress_to_json(o, &local[i], now); uploaded++; }
        }
    }
    // 只在服务器上、比上次同步新的进度：本机有这本书就写回。/ Remote-only newer entries: import if the book exists here.
    const cJSON* r;
    cJSON_ArrayForEach(r, rp) {
        bool known = false;
        for (int i = 0; i < n && !known; i++) known = !strcmp(local[i].path, r->string);
        if (known || json_num(r, "time") <= last) continue;
        tomato_progress_t in;
        progress_from_json(r, r->string, &in);
        if (import_cb(&in)) downloaded++;
    }

    // 番茄书架。/ Tomato shelf.
    // 已删除清单两边取并集。/ The removed list is a union of both sides.
    EXT_RAM_BSS_ATTR static char removed[TOMATO_REMOVED_MAX][TOMATO_ID_LEN];
    int removed_n = tomato_removed_load(removed, TOMATO_REMOVED_MAX);
    bool removed_changed = false;
    const cJSON* rid;
    cJSON_ArrayForEach(rid, cJSON_GetObjectItemCaseSensitive(remote, "removed")) {
        if (!cJSON_IsString(rid) || !rid->valuestring || !rid->valuestring[0]) continue;
        if (id_in((const char (*)[TOMATO_ID_LEN])removed, removed_n, rid->valuestring) || removed_n >= TOMATO_REMOVED_MAX) continue;
        tomato_copy_utf8(removed[removed_n++], TOMATO_ID_LEN, rid->valuestring);
        removed_changed = true;
    }
    if (removed_changed) tomato_removed_save((const char (*)[TOMATO_ID_LEN])removed, removed_n);
    int count = 0;
    tomato_shelf_load(shelf, TOMATO_SHELF_MAX, &count);
    if (merge_shelf(cJSON_GetObjectItemCaseSensitive(remote, "tomato"), shelf, &count,
                    (const char (*)[TOMATO_ID_LEN])removed, removed_n, &added)) {
        tomato_shelf_save(shelf, count);
    }
    cJSON_DeleteItemFromObjectCaseSensitive(remote, "tomato");
    cJSON_AddItemToObject(remote, "tomato", shelf_to_json(shelf, count));
    cJSON_DeleteItemFromObjectCaseSensitive(remote, "removed");
    cJSON* rarr = cJSON_AddArrayToObject(remote, "removed");
    for (int i = 0; rarr && i < removed_n; i++) cJSON_AddItemToArray(rarr, cJSON_CreateString(removed[i]));
    cJSON_DeleteItemFromObjectCaseSensitive(remote, "version");
    cJSON_AddNumberToObject(remote, "version", 1);
    cJSON_DeleteItemFromObjectCaseSensitive(remote, "updated");
    cJSON_AddNumberToObject(remote, "updated", (double)now);

    char* out = cJSON_PrintUnformatted(remote);
    if (!out) { err = ESP_ERR_NO_MEM; snprintf(msg, cap, "内存不足"); goto done; }
    err = dav_request(&cfg, true, out, NULL, &status);
    cJSON_free(out);
    if (err != ESP_OK || status < 200 || status >= 300) {
        snprintf(msg, cap, "上传到 WebDAV 失败");
        if (err == ESP_OK) err = ESP_FAIL;
        goto done;
    }

    // 记下这次同步后的签名。/ Record signatures after this sync.
    n = export_cb(local, PROGRESS_MAX);
    cJSON_Delete(state);
    state = cJSON_CreateObject();
    cJSON_AddNumberToObject(state, "last", (double)now);
    cJSON* ns = cJSON_AddObjectToObject(state, "sig");
    for (int i = 0; ns && i < n; i++) {
        char sig[48];
        signature(&local[i], sig, sizeof(sig));
        cJSON_AddStringToObject(ns, local[i].path, sig);
    }
    write_state(state);
    snprintf(msg, cap, "已同步：上传 %d 条，下载 %d 条%s", uploaded, downloaded, added ? "，书架有新书" : "");

done:
    cJSON_Delete(remote);
    cJSON_Delete(state);
    heap_caps_free(body);
    heap_caps_free(local);
    heap_caps_free(shelf);
    memset(&cfg, 0, sizeof(cfg));
    return err;
}
