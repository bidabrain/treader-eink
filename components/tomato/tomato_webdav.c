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
 * - 番茄书架按 book_id 取并集；只在服务器上的书加进本机书架，章节数记 0，等用户点“下载全部更新”。
 *
 * Merge: tomato/webdav_state.json on TF records each book's progress signature and the last sync time.
 * - Local differs from its signature: changed here, local wins.
 * - Local unchanged and the remote entry is newer than the last sync: remote wins and is written back.
 * - The Tomato shelf is a union by book_id; remote-only books join locally with 0 chapters, pending download.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    esp_err_t err = esp_http_client_open(c, len);
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
    if (ok) { remove(STATE_PATH); ok = rename(tmp, STATE_PATH) == 0; }
    if (!ok) remove(tmp);
    return ok ? ESP_OK : ESP_FAIL;
}

/* ---- 番茄书架合并 / Tomato shelf merge ---- */

static int merge_shelf(cJSON* remote_list, tomato_book_t* shelf, int* count) {
    int added = 0;
    const cJSON* it;
    cJSON_ArrayForEach(it, remote_list) {
        char id[TOMATO_ID_LEN];
        tomato_json_str(it, "book_id", id, sizeof(id));
        if (!id[0]) continue;
        int remote_total = tomato_json_int(it, "chapters");
        int r2 = tomato_json_int(it, "remote");
        if (r2 > remote_total) remote_total = r2;
        int at = -1;
        for (int i = 0; i < *count; i++) if (!strcmp(shelf[i].book_id, id)) at = i;
        if (at < 0) {
            if (*count >= TOMATO_SHELF_MAX) continue;
            tomato_book_t* b = &shelf[(*count)++];
            memset(b, 0, sizeof(*b));
            snprintf(b->book_id, sizeof(b->book_id), "%s", id);
            tomato_json_str(it, "title", b->title, sizeof(b->title));
            tomato_json_str(it, "author", b->author, sizeof(b->author));
            tomato_json_str(it, "file", b->file, sizeof(b->file));
            b->chapters = 0;
            b->remote = (uint16_t)(remote_total > 0 ? remote_total : 1);
            added++;
        } else if (remote_total > shelf[at].remote) {
            shelf[at].remote = (uint16_t)remote_total;
        }
    }
    return added;
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

    int status = 0;
    err = dav_request(&cfg, false, NULL, &body, &status);
    if (err != ESP_OK || (status != 200 && status != 404)) {
        snprintf(msg, cap, status == 401 ? "WebDAV 用户名或密码不对" : "连不上 WebDAV 服务器");
        if (err == ESP_OK) err = ESP_FAIL;
        goto done;
    }
    remote = body ? cJSON_Parse(body) : NULL;
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
        bool changed = !cJSON_IsString(old) || strcmp(old->valuestring, sig);
        cJSON* r = cJSON_GetObjectItemCaseSensitive(rp, local[i].path);
        if (r && !changed && json_num(r, "time") > last) {
            tomato_progress_t in;
            progress_from_json(r, local[i].path, &in);
            if (import_cb(&in)) downloaded++;
            continue;
        }
        if (!r || changed) {
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
    int count = 0;
    tomato_shelf_load(shelf, TOMATO_SHELF_MAX, &count);
    added = merge_shelf(cJSON_GetObjectItemCaseSensitive(remote, "tomato"), shelf, &count);
    if (added) tomato_shelf_save(shelf, count);
    cJSON_DeleteItemFromObjectCaseSensitive(remote, "tomato");
    cJSON_AddItemToObject(remote, "tomato", shelf_to_json(shelf, count));
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
