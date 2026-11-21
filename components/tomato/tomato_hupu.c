/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 虎扑只读浏览的数据层：取虎扑移动版页面，从页面里的 __NEXT_DATA__ 取出推荐列表或帖子，整理成精简 JSON 存 TF 卡，
 * 再把要显示的图片按需要的宽度取成 JPG。接口取法照安卓版 hupuX 的 doc/api/mobile-api.md。
 *
 * English: data layer for read-only Hupu browsing. Fetch Hupu mobile pages, pull the home feed or a post out of
 * the embedded __NEXT_DATA__, store a compact JSON on the TF card, and fetch the images to show as JPEG at the
 * needed width. Field paths follow hupuX's doc/api/mobile-api.md.
 *
 * 冻结：只读，不登录、不带 Cookie；只在后台同步任务里联网，由页面在用户点“刷新”或打开没缓存的帖子时启动。
 * 缓存有上限：帖子最多留 POST_KEEP 个，图片目录超过 IMG_FILES_MAX 个文件就只留当前要用的。
 * Frozen: read only, no login and no cookies; network happens only on the sync task, started by the page when the
 * user taps Refresh or opens an uncached post. Caches are bounded: at most POST_KEEP posts, and past
 * IMG_FILES_MAX image files only the ones in use stay.
 */

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "tomato_internal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TAG "tomato_hupu"
#define PAGE_MAX (1024 * 1024)       // 移动版页面约 100–200 KB / Mobile pages are ~100-200 KB
#define IMG_MAX (1024 * 1024)
#define HOME_ITEMS_MAX 40
#define BODY_IMAGES_MAX 16           // 一帖最多取这么多张图 / Most images fetched for one post
#define REPLIES_MAX 60
#define TEXT_MAX (16 * 1024)         // 一段文字的上限 / Cap on one text run
#define POST_KEEP 30
#define IMG_FILES_MAX 400            // 图片和缩略图缓存一共的文件数上限 / Cap on image and thumbnail cache files
#define THUMB_W 300
#define BODY_W 600

static const char HOME_URL[] = "https://m.hupu.com/";

/* ---- 路径 / Paths ---- */

// 虎扑缓存锁：后台清理删文件时，界面任务不能正开着同一批文件（FatFs 没开文件锁）。
// Hupu cache lock: the worker must not delete files the UI task has open (FatFs file locking is off).
static SemaphoreHandle_t s_cache_lock;
static StaticSemaphore_t s_cache_lock_buf;
static portMUX_TYPE s_cache_lock_init = portMUX_INITIALIZER_UNLOCKED;

void tomato_hupu_cache_lock(void) {
    portENTER_CRITICAL(&s_cache_lock_init);
    if (!s_cache_lock) s_cache_lock = xSemaphoreCreateMutexStatic(&s_cache_lock_buf);
    portEXIT_CRITICAL(&s_cache_lock_init);
    xSemaphoreTake(s_cache_lock, portMAX_DELAY);
}

void tomato_hupu_cache_unlock(void) {
    if (s_cache_lock) xSemaphoreGive(s_cache_lock);
}

// 服务器给的编号只留字母、数字、- 和 _，免得拼进路径时出现 ../。/ Keep only letters, digits, - and _ from server
// ids so they can never add ../ to a path.
static void safe_id(const char* in, char* out, size_t cap) {
    size_t n = 0;
    for (; in && *in && n + 1 < cap; in++)
        if (isalnum((unsigned char)*in) || *in == '-' || *in == '_') out[n++] = *in;
    out[n] = 0;
}

static uint32_t fnv(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h;
}

void tomato_hupu_post_path(const char* tid, char* out, size_t cap) {
    char id[24];
    safe_id(tid, id, sizeof(id));
    snprintf(out, cap, TOMATO_HUPU_DIR "/post/%.20s.json", id);
}

void tomato_hupu_img_path(const char* url, char* out, size_t cap) {
    snprintf(out, cap, TOMATO_HUPU_DIR "/img/%08lx.jpg", (unsigned long)fnv(url));
}

bool tomato_hupu_img_size(const char* url, int* w, int* h) {
    // 虎扑图床的文件名里带原图尺寸：…_w_1920_h_967_.jpeg。/ Hupu image names carry the size: ..._w_1920_h_967_.jpeg.
    const char* p = strstr(url, "_w_");
    if (!p) return false;
    int ww = atoi(p + 3);
    const char* q = strstr(p, "_h_");
    if (!q) return false;
    int hh = atoi(q + 3);
    if (ww <= 0 || hh <= 0) return false;
    *w = ww;
    *h = hh;
    return true;
}

// 虎扑图床支持阿里云 OSS 处理参数：统一要指定宽度的 JPG（设备解不了 WEBP）。别的图床原样用。
// Hupu's image host takes Aliyun OSS parameters: always ask for a JPEG at a set width (the device cannot decode
// WEBP). Other hosts are used as is.
static void image_url(const char* in, int width, char* out, size_t cap) {
    char base[400];
    if (!strncmp(in, "//", 2)) snprintf(base, sizeof(base), "https:%.390s", in);
    else snprintf(base, sizeof(base), "%.399s", in);
    if (!strstr(base, "hoopchina.com.cn") && !strstr(base, "hupu.com")) {
        snprintf(out, cap, "%.399s", base);
        return;
    }
    char* q = strchr(base, '?');
    if (q) *q = 0;
    snprintf(out, cap, "%.360s?x-oss-process=image/resize,w_%d/format,jpg", base, width);
}

/* ---- 页面里的 JSON / The JSON inside a page ---- */

// 找到 __NEXT_DATA__ 并就地截断，返回 JSON 开头。/ Locate __NEXT_DATA__, terminate it in place, return its start.
static char* next_data(char* html) {
    char* p = strstr(html, "id=\"__NEXT_DATA__\"");
    if (!p) return NULL;
    p = strchr(p, '>');
    if (!p) return NULL;
    p++;
    char* end = strstr(p, "</script>");
    if (!end) return NULL;
    *end = 0;
    return p;
}

static const cJSON* path(const cJSON* o, const char* const* keys, int n) {
    for (int i = 0; i < n && o; i++) o = cJSON_GetObjectItemCaseSensitive(o, keys[i]);
    return o;
}

// 字符串或数字都转成文字。/ Read a string or a number as text.
static void str_of(const cJSON* o, const char* key, char* out, size_t cap) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsString(v) && v->valuestring) tomato_copy_utf8(out, cap, v->valuestring);
    else if (cJSON_IsNumber(v)) snprintf(out, cap, "%.0f", v->valuedouble);
    else if (cap) out[0] = 0;
}

static esp_err_t write_json(const char* dest, cJSON* root) {
    char* text = cJSON_PrintUnformatted(root);
    if (!text) return ESP_ERR_NO_MEM;
    char tmp[96];
    snprintf(tmp, sizeof(tmp), "%s.tmp", dest);
    FILE* f = fopen(tmp, "wb");
    const size_t len = strlen(text);
    bool ok = f && fwrite(text, 1, len, f) == len;
    if (f) ok = fclose(f) == 0 && ok;
    cJSON_free(text);
    if (!ok) { remove(tmp); return ESP_FAIL; }
    remove(dest);
    if (rename(tmp, dest) != 0) { remove(tmp); return ESP_FAIL; }
    return ESP_OK;
}

/* ---- HTML 转文字 / HTML to text ---- */

typedef struct {
    char* buf;           // PSRAM，TEXT_MAX / PSRAM, TEXT_MAX
    size_t len;
    bool space;          // 上一个字符是空白 / Last char was whitespace
    cJSON* blocks;       // 正文：{"t":…} / {"i":…}；回复：NULL，图片写成“[图]” / Body blocks; NULL for replies
    int images;
} html_out_t;

static void put(html_out_t* o, const char* s, size_t n) {
    if (o->len + n >= TEXT_MAX) n = TEXT_MAX - 1 - o->len;
    memcpy(o->buf + o->len, s, n);
    o->len += n;
    o->buf[o->len] = 0;
}

static void put_char(html_out_t* o, char c) {
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
        if (o->space || !o->len) return;
        o->space = true;
        put(o, " ", 1);
        return;
    }
    o->space = false;
    put(o, &c, 1);
}

// 段落结束：正文模式下存成一块，回复模式下换行。/ End a paragraph: a block for bodies, a newline for replies.
static void paragraph(html_out_t* o) {
    while (o->len && o->buf[o->len - 1] == ' ') o->buf[--o->len] = 0;
    if (!o->len) return;
    if (o->blocks) {
        cJSON* b = cJSON_CreateObject();
        if (b) { cJSON_AddStringToObject(b, "t", o->buf); cJSON_AddItemToArray(o->blocks, b); }
        o->len = 0;
        o->buf[0] = 0;
    } else if (o->buf[o->len - 1] != '\n') {
        put(o, "\n", 1);
    }
    o->space = true;
}

static void image(html_out_t* o, const char* url) {
    if (!url[0]) return;
    if (!o->blocks) { put(o, "[图]", strlen("[图]")); o->space = false; return; }
    paragraph(o);
    if (o->images >= BODY_IMAGES_MAX) return;
    char u[440];
    image_url(url, BODY_W, u, sizeof(u));
    cJSON* b = cJSON_CreateObject();
    if (b) { cJSON_AddStringToObject(b, "i", u); cJSON_AddItemToArray(o->blocks, b); o->images++; }
}

// 取标签里某个属性的值。/ Read an attribute value from a tag.
static bool attr(const char* tag, size_t tag_len, const char* name, char* out, size_t cap) {
    const size_t n = strlen(name);
    for (size_t i = 0; i + n + 2 < tag_len; i++) {
        if (strncasecmp(tag + i, name, n) || tag[i + n] != '=' || (i && !isspace((unsigned char)tag[i - 1]))) continue;
        const char* v = tag + i + n + 1;
        const char quote = (*v == '"' || *v == '\'') ? *v++ : 0;
        size_t k = 0;
        while (v + k < tag + tag_len && (quote ? v[k] != quote : !isspace((unsigned char)v[k]) && v[k] != '>')) k++;
        if (k >= cap) k = cap - 1;
        memcpy(out, v, k);
        out[k] = 0;
        // 地址里的 &amp; 还原。/ Restore &amp; inside URLs.
        for (char* a = strstr(out, "&amp;"); a; a = strstr(a + 1, "&amp;")) memmove(a + 1, a + 5, strlen(a + 5) + 1);
        return k > 0;
    }
    return false;
}

static void entity(html_out_t* o, const char** pp) {
    const char* p = *pp;
    static const struct { const char* name; const char* text; } MAP[] = {
        {"&nbsp;", " "}, {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&apos;", "'"},
    };
    for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++) {
        const size_t n = strlen(MAP[i].name);
        if (!strncmp(p, MAP[i].name, n)) {
            put_char(o, MAP[i].text[0]);
            *pp = p + n;
            return;
        }
    }
    if (p[1] == '#') {
        char* end;
        long cp = p[2] == 'x' || p[2] == 'X' ? strtol(p + 3, &end, 16) : strtol(p + 2, &end, 10);
        if (*end == ';' && cp > 0 && cp < 0x110000) {
            char u[5];
            int n = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
            if (n == 1) u[0] = (char)cp;
            else if (n == 2) { u[0] = (char)(0xC0 | (cp >> 6)); u[1] = (char)(0x80 | (cp & 0x3F)); }
            else if (n == 3) { u[0] = (char)(0xE0 | (cp >> 12)); u[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[2] = (char)(0x80 | (cp & 0x3F)); }
            else { u[0] = (char)(0xF0 | (cp >> 18)); u[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); u[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[3] = (char)(0x80 | (cp & 0x3F)); }
            o->space = false;
            put(o, u, (size_t)n);
            *pp = end + 1;
            return;
        }
    }
    put_char(o, '&');
    *pp = p + 1;
}

// 标签的结尾：跳过引号里的“>”（虎扑有 data-name="…>" 这样的属性）。/ End of a tag, skipping ">" inside quotes
// (Hupu has attributes such as data-name="...>").
static const char* tag_end(const char* p) {
    char quote = 0;
    for (p++; *p; p++) {
        if (quote) { if (*p == quote) quote = 0; }
        else if (*p == '"' || *p == '\'') quote = *p;
        else if (*p == '>') return p;
    }
    return NULL;
}

// 把一段 HTML 转成文字；正文模式（o->blocks 非空）同时拆出图片块。
// Convert HTML to text; in body mode (o->blocks set) images become their own blocks.
static void html_to_text(html_out_t* o, const char* html) {
    const char* p = html;
    while (*p) {
        if (*p == '&') { entity(o, &p); continue; }
        if (*p != '<') { put_char(o, *p++); continue; }
        const char* end = tag_end(p);
        if (!end) break;
        const char* tag = p + 1;
        const size_t tag_len = (size_t)(end - tag);
        const bool closing = *tag == '/';
        const char* name = closing ? tag + 1 : tag;
        size_t nl = 0;
        while (name + nl < end && isalnum((unsigned char)name[nl])) nl++;
        p = end + 1;
        if ((nl == 6 && !strncasecmp(name, "script", 6)) || (nl == 5 && !strncasecmp(name, "style", 5))) {
            if (closing) continue;
            const char* skip = strcasestr(p, nl == 6 ? "</script>" : "</style>");
            p = skip ? skip + (nl == 6 ? 9 : 8) : p + strlen(p);
            continue;
        }
        if ((nl == 1 && (*name == 'p' || *name == 'P')) || (nl == 2 && !strncasecmp(name, "br", 2)) ||
            (nl == 3 && !strncasecmp(name, "div", 3)) || (nl == 2 && !strncasecmp(name, "li", 2)) ||
            (nl == 2 && tolower((unsigned char)name[0]) == 'h' && isdigit((unsigned char)name[1]))) {
            paragraph(o);
            continue;
        }
        if (closing) continue;
        char url[400] = "";
        if (nl == 3 && !strncasecmp(name, "img", 3)) {
            if (!attr(tag, tag_len, "data-src", url, sizeof(url))) attr(tag, tag_len, "src", url, sizeof(url));
            if (!strncmp(url, "data:", 5)) url[0] = 0;
            image(o, url);
        } else if (nl == 6 && !strncasecmp(name, "center", 6)) {
            // 虎扑图片节点：src 是静态 JPG，动图取它也只要第一帧。/ Hupu image node: src is a static JPEG, fine for GIFs too.
            if (memmem(tag, tag_len, "hupu-img", 8) && (attr(tag, tag_len, "src", url, sizeof(url)) ||
                                            attr(tag, tag_len, "data_url", url, sizeof(url))))
                image(o, url);
        } else if (nl == 5 && !strncasecmp(name, "video", 5)) {
            paragraph(o);
            put(o, "[视频，请在虎扑 App 观看]", strlen("[视频，请在虎扑 App 观看]"));
            paragraph(o);
        }
    }
    paragraph(o);
}

/* ---- 推荐 / Home feed ---- */

static esp_err_t fetch_page(const char* url, const volatile bool* cancel, cJSON** out) {
    uint8_t* html;
    size_t len;
    esp_err_t err = tomato_http_get(url, PAGE_MAX, false, &html, &len, cancel);
    if (err != ESP_OK) return err;
    char* json = next_data((char*)html);
    *out = json ? cJSON_Parse(json) : NULL;
    heap_caps_free(html);
    return *out ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static volatile bool s_images;   // 设置里的“显示图片” / The "show images" setting

void tomato_hupu_set_images(bool on) { s_images = on; }

// 下载缺的图；返回成功张数。失败的原因留在 tomato_http_detail()。/ Fetch missing images; returns how many
// succeeded. The failure reason stays in tomato_http_detail().
static int fetch_images(const cJSON* urls, const volatile bool* cancel, tomato_hupu_progress_t progress, int done) {
    int ok = 0, total = done + cJSON_GetArraySize(urls), misses = 0;
    const cJSON* u;
    cJSON_ArrayForEach(u, urls) {
        if (cancel && *cancel) break;
        // 网断了就别一张张等超时。/ When the network is gone, stop instead of timing out on every image.
        if (misses >= 3) break;
        progress(++done, total);
        if (!cJSON_IsString(u)) continue;
        char dest[96];
        struct stat st;
        tomato_hupu_img_path(u->valuestring, dest, sizeof(dest));
        if (stat(dest, &st) == 0 && st.st_size > 0) { ok++; continue; }
        if (tomato_http_fetch(u->valuestring, dest, IMG_MAX, true, cancel) == ESP_OK) { ok++; misses = 0; }
        else misses++;
    }
    return ok;
}

// 图片目录太大时，只留 keep 里用到的图（和它们的缩略图缓存）。/ Past the cap, keep only images in use and their caches.
static void prune_images_locked(const cJSON* keep) {
    DIR* d = opendir(TOMATO_HUPU_DIR "/img");
    if (!d) return;
    int files = 0;
    struct dirent* e;
    while ((e = readdir(d))) if (e->d_name[0] != '.') files++;
    if (files <= IMG_FILES_MAX) { closedir(d); return; }
    rewinddir(d);
    char path_buf[300];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        bool kept = false;
        const cJSON* u;
        cJSON_ArrayForEach(u, keep) {
            char mine[96];
            tomato_hupu_img_path(u->valuestring, mine, sizeof(mine));
            // 缓存文件名是图片名去掉 .jpg 再加尺寸。/ Cache files share the image name minus .jpg.
            const char* base = strrchr(mine, '/') + 1;
            if (!strncmp(e->d_name, base, 8)) { kept = true; break; }
        }
        if (kept) continue;
        snprintf(path_buf, sizeof(path_buf), TOMATO_HUPU_DIR "/img/%.255s", e->d_name);
        remove(path_buf);
    }
    closedir(d);
}

static void prune_images(const cJSON* keep) {
    tomato_hupu_cache_lock();
    prune_images_locked(keep);
    tomato_hupu_cache_unlock();
}

static void make_dirs(void) {
    mkdir(TOMATO_HUPU_DIR, 0775);
    mkdir(TOMATO_HUPU_DIR "/post", 0775);
    mkdir(TOMATO_HUPU_DIR "/img", 0775);
}

esp_err_t tomato_hupu_home(const volatile bool* cancel, tomato_hupu_progress_t progress, char* msg, size_t cap) {
    make_dirs();
    cJSON* page = NULL;
    esp_err_t err = fetch_page(HOME_URL, cancel, &page);
    if (err != ESP_OK) {
        snprintf(msg, cap, "%s", err == ESP_ERR_INVALID_RESPONSE ? "虎扑页面结构变了，暂时取不到" : "加载失败，检查网络");
        return err;
    }
    static const char* const RES[] = {"props", "pageProps", "res"};
    const cJSON* res = path(page, RES, 3);
    cJSON* out = cJSON_CreateObject();
    cJSON* items = out ? cJSON_AddArrayToObject(out, "items") : NULL;
    cJSON* thumbs = cJSON_CreateArray();
    const cJSON* it;
    int n = 0;
    cJSON_ArrayForEach(it, res) {
        if (!items || !thumbs || n >= HOME_ITEMS_MAX) break;
        char tid[24], title[200], zone[48], replies[16];
        str_of(it, "tid", tid, sizeof(tid));
        str_of(it, "title", title, sizeof(title));
        if (!tid[0] || !title[0]) continue;
        str_of(it, "label", zone, sizeof(zone));
        str_of(it, "replies", replies, sizeof(replies));
        cJSON* o = cJSON_CreateObject();
        if (!o) break;
        cJSON_AddStringToObject(o, "tid", tid);
        cJSON_AddStringToObject(o, "title", title);
        cJSON_AddStringToObject(o, "zone", zone);
        cJSON_AddStringToObject(o, "replies", replies);
        const cJSON* src = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(it, "source"), 0);
        if (cJSON_IsString(src) && src->valuestring[0]) {
            char u[440];
            image_url(src->valuestring, THUMB_W, u, sizeof(u));
            cJSON_AddStringToObject(o, "img", u);
            cJSON_AddItemToArray(thumbs, cJSON_CreateString(u));
        }
        cJSON_AddItemToArray(items, o);
        n++;
    }
    cJSON_Delete(page);
    if (!n) {
        cJSON_Delete(out);
        cJSON_Delete(thumbs);
        snprintf(msg, cap, "虎扑页面结构变了，暂时取不到");
        return ESP_ERR_INVALID_RESPONSE;
    }
    err = write_json(TOMATO_HUPU_HOME_FILE, out);
    cJSON_Delete(out);
    if (err != ESP_OK) {
        cJSON_Delete(thumbs);
        snprintf(msg, cap, "写入 TF 卡失败");
        return err;
    }
    // 第 1 步完成：列表已可显示，页面据此先刷出文字。/ Step 1 done: the list can show; the page draws text first.
    // 不显示图片时只取文字，旧图留着（打开开关后还能用）。/ With images off, text only; old images stay for later.
    if (s_images) {
        progress(1, 1 + cJSON_GetArraySize(thumbs));
        prune_images(thumbs);
        fetch_images(thumbs, cancel, progress, 1);
    } else {
        progress(1, 1);
    }
    cJSON_Delete(thumbs);
    snprintf(msg, cap, "已更新");
    return cancel && *cancel ? ESP_ERR_INVALID_STATE : ESP_OK;
}

/* ---- 帖子 / Post ---- */

// 一条回复：用户、亮了数、引用、正文（图片写成“[图]”）。/ One reply: user, lights, quote, text ("[图]" for images).
static cJSON* reply_json(const cJSON* r, html_out_t* o) {
    cJSON* x = cJSON_CreateObject();
    if (!x) return NULL;
    char name[64], light[16];
    str_of(cJSON_GetObjectItemCaseSensitive(r, "user"), "username", name, sizeof(name));
    str_of(r, "light", light, sizeof(light));
    cJSON_AddStringToObject(x, "u", name);
    cJSON_AddNumberToObject(x, "n", atoi(light));
    const cJSON* q = cJSON_GetObjectItemCaseSensitive(r, "quote_info");
    if (cJSON_IsObject(q)) {
        char qn[64];
        str_of(q, "username", qn, sizeof(qn));
        const cJSON* qc = cJSON_GetObjectItemCaseSensitive(q, "content");
        o->len = 0; o->buf[0] = 0; o->space = false;
        if (cJSON_IsString(qc)) html_to_text(o, qc->valuestring);
        for (char* c = o->buf; *c; c++) if (*c == '\n') *c = ' ';
        char line[400];
        snprintf(line, sizeof(line), "%s：%.320s", qn, o->buf);
        cJSON_AddStringToObject(x, "q", line);
    }
    const cJSON* c = cJSON_GetObjectItemCaseSensitive(r, "content");
    o->len = 0; o->buf[0] = 0; o->space = false;
    if (cJSON_IsString(c)) html_to_text(o, c->valuestring);
    while (o->len && o->buf[o->len - 1] == '\n') o->buf[--o->len] = 0;
    cJSON_AddStringToObject(x, "c", o->buf);
    return x;
}

static void add_replies(cJSON* out, const char* key, const cJSON* list, html_out_t* o) {
    cJSON* arr = cJSON_AddArrayToObject(out, key);
    const cJSON* r;
    int n = 0;
    cJSON_ArrayForEach(r, list) {
        if (!arr || n >= REPLIES_MAX) break;
        cJSON* x = reply_json(r, o);
        if (x) { cJSON_AddItemToArray(arr, x); n++; }
    }
}

// 帖子缓存超过 POST_KEEP 个就全删，只留这一帖。/ Past POST_KEEP posts, delete all but this one.
static void prune_posts_locked(const char* keep) {
    DIR* d = opendir(TOMATO_HUPU_DIR "/post");
    if (!d) return;
    int files = 0;
    struct dirent* e;
    while ((e = readdir(d))) if (e->d_name[0] != '.') files++;
    if (files <= POST_KEEP) { closedir(d); return; }
    rewinddir(d);
    const char* keep_name = strrchr(keep, '/') + 1;
    char path_buf[300];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, keep_name)) continue;
        snprintf(path_buf, sizeof(path_buf), TOMATO_HUPU_DIR "/post/%.255s", e->d_name);
        remove(path_buf);
    }
    closedir(d);
}

static void prune_posts(const char* keep) {
    tomato_hupu_cache_lock();
    prune_posts_locked(keep);
    tomato_hupu_cache_unlock();
}

static cJSON* read_json(const char* file);

// 只补图：读缓存里的帖子，下还没下的图。/ Images only: read the cached post and fetch the images still missing.
// 图没下好的提示，带上原因。/ The missing-images note with its reason.
static void image_failure(char* msg, size_t cap, int missing) {
    const char* why = tomato_http_detail();
    if (why[0]) snprintf(msg, cap, "有 %d 张图没下好（%s），点图重试", missing, why);
    else snprintf(msg, cap, "有 %d 张图没下好，点图重试", missing);
}

static esp_err_t post_images_only(const char* tid, const volatile bool* cancel, tomato_hupu_progress_t progress,
                                  char* msg, size_t cap) {
    char file[96];
    tomato_hupu_post_path(tid, file, sizeof(file));
    cJSON* post = read_json(file);
    cJSON* images = cJSON_CreateArray();
    const cJSON* blk;
    cJSON_ArrayForEach(blk, cJSON_GetObjectItemCaseSensitive(post, "body")) {
        const cJSON* i = cJSON_GetObjectItemCaseSensitive(blk, "i");
        if (images && cJSON_IsString(i)) cJSON_AddItemToArray(images, cJSON_CreateString(i->valuestring));
    }
    cJSON_Delete(post);
    if (!images) { snprintf(msg, cap, "内存不足"); return ESP_ERR_NO_MEM; }
    progress(1, 1 + cJSON_GetArraySize(images));
    tomato_http_clear_detail();
    const int got = fetch_images(images, cancel, progress, 1), want = cJSON_GetArraySize(images);
    cJSON_Delete(images);
    if (cancel && *cancel) return ESP_ERR_INVALID_STATE;
    if (got < want) { image_failure(msg, cap, want - got); return ESP_FAIL; }
    snprintf(msg, cap, "已加载");
    return ESP_OK;
}

esp_err_t tomato_hupu_post(const char* tid, bool images_only, const volatile bool* cancel,
                           tomato_hupu_progress_t progress, char* msg, size_t cap) {
    make_dirs();
    if (images_only) {
        if (!s_images) { snprintf(msg, cap, "已加载"); return ESP_OK; }
        return post_images_only(tid, cancel, progress, msg, cap);
    }
    char url[96];
    snprintf(url, sizeof(url), "https://m.hupu.com/bbs/%.20s.html", tid);
    cJSON* page = NULL;
    esp_err_t err = fetch_page(url, cancel, &page);
    if (err != ESP_OK) {
        snprintf(msg, cap, "%s", err == ESP_ERR_INVALID_RESPONSE ? "虎扑页面结构变了，暂时取不到" : "加载失败，检查网络");
        return err;
    }
    static const char* const PP[] = {"props", "pageProps"};
    const cJSON* pp = path(page, PP, 2);
    static const char* const DATA[] = {"threadData", "data"};
    const cJSON* data = path(pp, DATA, 2);
    const cJSON* mods = cJSON_GetObjectItemCaseSensitive(data, "moduleConfigList");
    static const char* const TITLE[] = {"title", "moduleContent"};
    static const char* const USER[] = {"user", "moduleContent"};
    static const char* const BODY[] = {"content", "moduleContent", "content"};
    const cJSON* body = path(mods, BODY, 3);
    html_out_t o = {.buf = heap_caps_malloc(TEXT_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)};
    cJSON* out = cJSON_CreateObject();
    if (!o.buf || !out || !mods) {
        heap_caps_free(o.buf);
        cJSON_Delete(out);
        cJSON_Delete(page);
        snprintf(msg, cap, "%s", mods ? "内存不足" : "帖子不存在或页面结构变了");
        return mods ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_RESPONSE;
    }
    o.buf[0] = 0;
    char text[200];
    cJSON_AddStringToObject(out, "tid", tid);
    str_of(path(mods, TITLE, 2), "title", text, sizeof(text));
    cJSON_AddStringToObject(out, "title", text);
    str_of(path(mods, USER, 2), "name", text, sizeof(text));
    cJSON_AddStringToObject(out, "author", text);
    str_of(path(mods, USER, 2), "time", text, sizeof(text));
    cJSON_AddStringToObject(out, "time", text);
    str_of(cJSON_GetObjectItemCaseSensitive(data, "basicInfo"), "topicName", text, sizeof(text));
    cJSON_AddStringToObject(out, "zone", text);
    str_of(data, "replies", text, sizeof(text));
    cJSON_AddNumberToObject(out, "replies", atoi(text));

    o.blocks = cJSON_AddArrayToObject(out, "body");
    if (cJSON_IsString(body) && body->valuestring[0] == '{') {
        // 正文是嵌入卡片（比赛战报之类），设备上显示不了。/ The body is an embedded card the device cannot show.
        cJSON* b = cJSON_CreateObject();
        if (b) { cJSON_AddStringToObject(b, "t", "[这是一张嵌入卡片，请在虎扑 App 查看]"); cJSON_AddItemToArray(o.blocks, b); }
    } else if (cJSON_IsString(body)) {
        html_to_text(&o, body->valuestring);
    }
    cJSON* images = cJSON_CreateArray();
    const cJSON* blk;
    cJSON_ArrayForEach(blk, o.blocks) {
        const cJSON* i = cJSON_GetObjectItemCaseSensitive(blk, "i");
        if (images && cJSON_IsString(i)) cJSON_AddItemToArray(images, cJSON_CreateString(i->valuestring));
    }
    o.blocks = NULL;   // 回复里的图写成“[图]” / Reply images become "[图]"
    static const char* const LIGHT[] = {"initialRepliesData", "lightReplies"};
    static const char* const INIT[] = {"initialRepliesData", "initialReplies"};
    add_replies(out, "lights", path(pp, LIGHT, 2), &o);
    add_replies(out, "list", path(pp, INIT, 2), &o);
    heap_caps_free(o.buf);
    cJSON_Delete(page);

    char dest[96];
    tomato_hupu_post_path(tid, dest, sizeof(dest));
    err = write_json(dest, out);
    cJSON_Delete(out);
    if (err != ESP_OK) {
        cJSON_Delete(images);
        snprintf(msg, cap, "写入 TF 卡失败");
        return err;
    }
    prune_posts(dest);
    progress(1, 1 + (s_images ? cJSON_GetArraySize(images) : 0));
    int missing = 0;
    if (images && s_images) {
        prune_images(images);
        tomato_http_clear_detail();
        missing = cJSON_GetArraySize(images) - fetch_images(images, cancel, progress, 1);
    }
    cJSON_Delete(images);
    if (cancel && *cancel) return ESP_ERR_INVALID_STATE;
    // 正文已经存好；图没下全就报失败并说明原因（原来不吭声，只显示“图片未下载”）。/ The text is saved; missing
    // images report a failure with the reason (it used to stay silent with only "image not downloaded").
    if (missing > 0) { image_failure(msg, cap, missing); return ESP_FAIL; }
    snprintf(msg, cap, "已加载");
    return ESP_OK;
}

/* ---- 第二期：专区、专区帖子、关注 / Phase two: zones, zone posts, follows ---- */

#define ZONE_ITEMS_MAX 200          // 一个专区最多缓存的帖子 / Most cached posts per zone
#define FOLLOW_MAX 12               // 关注流最多取这么多个专区 / Most zones read for the follow feed
#define ZONE_FILES_MAX 40
#define RATE_FILES_MAX 20
#define SCHEDULE_MAX (1024 * 1024)  // NBA 一季赛程约 420 KB / A season of NBA games is ~420 KB
#define JSON_GET_MAX (512 * 1024)

void tomato_hupu_zone_path(const char* zone, int sort, char* out, size_t cap) {
    char id[16];
    safe_id(zone, id, sizeof(id));
    snprintf(out, cap, TOMATO_HUPU_DIR "/zone/%.12s_%d.json", id, sort ? 1 : 0);
}

void tomato_hupu_schedule_path(const char* league, char* out, size_t cap) {
    char id[16];
    safe_id(league, id, sizeof(id));
    snprintf(out, cap, TOMATO_HUPU_DIR "/sched_%.12s.json", id);
}

void tomato_hupu_rating_path(const char* type, const char* no, char* out, size_t cap) {
    char a[28], b[20];
    safe_id(type, a, sizeof(a));
    safe_id(no, b, sizeof(b));
    snprintf(out, cap, TOMATO_HUPU_DIR "/rate/%.24s_%.16s.json", a, b);
}

static void make_more_dirs(void) {
    make_dirs();
    mkdir(TOMATO_HUPU_DIR "/zone", 0775);
    mkdir(TOMATO_HUPU_DIR "/rate", 0775);
}

// 读 TF 卡上的 JSON；没有或坏了返回 NULL。/ Read a JSON file from the card; NULL when missing or broken.
static cJSON* read_json(const char* file) {
    FILE* f = fopen(file, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = n > 0 && n < 2 * 1024 * 1024 ? heap_caps_malloc((size_t)n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    bool ok = buf && fread(buf, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    cJSON* root = NULL;
    if (ok) { buf[n] = 0; root = cJSON_Parse(buf); }
    heap_caps_free(buf);
    return root;
}

// GET 一个 JSON 接口并解析；原始响应解析完立刻释放。/ GET a JSON API and parse it; the raw reply is freed right away.
static esp_err_t fetch_json(const char* url, size_t max, const volatile bool* cancel, cJSON** out) {
    uint8_t* body;
    size_t len;
    *out = NULL;
    esp_err_t err = tomato_http_get(url, max, false, &body, &len, cancel);
    if (err != ESP_OK) return err;
    // 解析出来的树约是原文的 2.5 倍：PSRAM 不够就不解析，免得半路失败拖垮别的功能。
    // The parsed tree is ~2.5x the text: skip parsing when PSRAM is short rather than fail halfway.
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < len * 3) { heap_caps_free(body); return ESP_ERR_NO_MEM; }
    *out = cJSON_Parse((const char*)body);
    heap_caps_free(body);
    return *out ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

// 目录里文件超过 max 个就全删，只留 keep。/ Past max files, delete everything in dir except keep.
static void prune_dir_locked(const char* dir, int max, const char* keep) {
    DIR* d = opendir(dir);
    if (!d) return;
    int files = 0;
    struct dirent* e;
    while ((e = readdir(d))) if (e->d_name[0] != '.') files++;
    if (files <= max) { closedir(d); return; }
    rewinddir(d);
    const char* keep_name = keep ? strrchr(keep, '/') + 1 : "";
    char path_buf[300];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, keep_name)) continue;
        snprintf(path_buf, sizeof(path_buf), "%.40s/%.255s", dir, e->d_name);
        remove(path_buf);
    }
    closedir(d);
}

static void prune_dir(const char* dir, int max, const char* keep) {
    tomato_hupu_cache_lock();
    prune_dir_locked(dir, max, keep);
    tomato_hupu_cache_unlock();
}

static void fail_msg(esp_err_t err, char* msg, size_t cap) {
    snprintf(msg, cap, "%s", err == ESP_ERR_INVALID_RESPONSE ? "虎扑页面结构变了，暂时取不到" :
                             err == ESP_ERR_INVALID_STATE ? "已取消" :
                             err == ESP_ERR_NO_MEM ? "内存不足，先退出正在读的书再试" : "加载失败，检查网络");
}

esp_err_t tomato_hupu_zones(const volatile bool* cancel, char* msg, size_t cap) {
    make_more_dirs();
    cJSON* page = NULL;
    esp_err_t err = fetch_page("https://m.hupu.com/zone", cancel, &page);
    if (err != ESP_OK) { fail_msg(err, msg, cap); return err; }
    static const char* const DATA[] = {"props", "pageProps", "data"};
    const cJSON* cats = path(page, DATA, 3);
    cJSON* out = cJSON_CreateObject();
    cJSON* arr = out ? cJSON_AddArrayToObject(out, "cats") : NULL;
    int total = 0;
    bool lol = false;
    const cJSON* c;
    cJSON_ArrayForEach(c, cats) {
        if (!arr) break;
        char name[32];
        str_of(c, "name", name, sizeof(name));
        // “热门”和其他分类重复，“站务管理”用不上。/ "Hot" repeats other categories; "site admin" is not useful.
        if (!name[0] || !strcmp(name, "热门") || !strcmp(name, "站务管理")) continue;
        cJSON* cat = cJSON_CreateObject();
        cJSON* zones = cat ? cJSON_AddArrayToObject(cat, "zones") : NULL;
        if (!zones) { cJSON_Delete(cat); break; }
        cJSON_AddStringToObject(cat, "name", name);
        // 移动版漏了英雄联盟主区（85），补到“游戏”第一个（同 hupuX）。/ The mobile list misses LoL zone 85; add it to
        // "Games" first, as hupuX does.
        if (!strcmp(name, "游戏")) {
            cJSON* z = cJSON_CreateObject();
            if (z) { cJSON_AddStringToObject(z, "id", "85"); cJSON_AddStringToObject(z, "name", "英雄联盟"); cJSON_AddItemToArray(zones, z); lol = true; total++; }
        }
        const cJSON* zz;
        cJSON_ArrayForEach(zz, cJSON_GetObjectItemCaseSensitive(c, "topicList")) {
            char id[16], zname[48];
            str_of(zz, "topicId", id, sizeof(id));
            str_of(zz, "topicName", zname, sizeof(zname));
            if (!id[0] || !zname[0] || (lol && !strcmp(id, "85"))) continue;
            cJSON* z = cJSON_CreateObject();
            if (!z) break;
            cJSON_AddStringToObject(z, "id", id);
            cJSON_AddStringToObject(z, "name", zname);
            cJSON_AddItemToArray(zones, z);
            total++;
        }
        cJSON_AddItemToArray(arr, cat);
    }
    cJSON_Delete(page);
    if (!total) {
        cJSON_Delete(out);
        snprintf(msg, cap, "虎扑页面结构变了，暂时取不到");
        return ESP_ERR_INVALID_RESPONSE;
    }
    err = write_json(TOMATO_HUPU_DIR "/zones.json", out);
    cJSON_Delete(out);
    snprintf(msg, cap, "%s", err == ESP_OK ? "已更新" : "写入 TF 卡失败");
    return err;
}

// 一条帖子精简成列表行。/ Reduce one post to a list row.
static cJSON* thread_row(const char* tid, const char* title, const char* user, const char* time, const char* replies) {
    cJSON* o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddStringToObject(o, "tid", tid);
    cJSON_AddStringToObject(o, "title", title);
    cJSON_AddStringToObject(o, "user", user);
    cJSON_AddStringToObject(o, "time", time);
    cJSON_AddStringToObject(o, "replies", replies);
    return o;
}

// 移动版接口一页 10 条（按最新回复）；items 追加，返回下一页游标。/ Mobile API, 10 per page by latest reply.
static esp_err_t threads_by_reply(const char* zone, int page, const char* cursor, const volatile bool* cancel,
                                  cJSON* items, char* next, size_t next_cap, int* added) {
    char url[200];
    if (page <= 1) snprintf(url, sizeof(url), "https://m.hupu.com/api/v2/bbs/topicThreads?topicId=%.12s&page=1", zone);
    else {
        char esc[128];
        tomato_url_escape(cursor, esc, sizeof(esc), false);
        snprintf(url, sizeof(url), "https://m.hupu.com/api/v2/bbs/topicThreads?topicId=%.12s&page=%d&cursor=%.60s", zone, page, esc);
    }
    cJSON* root = NULL;
    *added = 0;
    esp_err_t err = fetch_json(url, JSON_GET_MAX, cancel, &root);
    if (err != ESP_OK) return err;
    const cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    str_of(data, "nextCursor", next, next_cap);
    const cJSON* t;
    cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(data, "topicThreads")) {
        char tid[24], title[200], user[48], time[24], replies[16];
        str_of(t, "tid", tid, sizeof(tid));
        str_of(t, "title", title, sizeof(title));
        str_of(t, "username", user, sizeof(user));
        str_of(t, "time", time, sizeof(time));
        str_of(t, "replies", replies, sizeof(replies));
        if (!tid[0] || !title[0]) continue;
        cJSON* row = thread_row(tid, title, user, time, replies);
        if (row) { cJSON_AddItemToArray(items, row); (*added)++; }
    }
    cJSON_Delete(root);
    return ESP_OK;
}

// 桌面版“最新发布”一页 50 条，数据在 window.$$data。/ Desktop "newest posts", 50 per page in window.$$data.
static esp_err_t threads_by_post(const char* zone, int page, const volatile bool* cancel, cJSON* items,
                                 bool* end, int* added) {
    char url[96];
    if (page <= 1) snprintf(url, sizeof(url), "https://bbs.hupu.com/%.12s-postdate", zone);
    else snprintf(url, sizeof(url), "https://bbs.hupu.com/%.12s-postdate-%d", zone, page);
    uint8_t* html;
    size_t len;
    *added = 0;
    esp_err_t err = tomato_http_get(url, PAGE_MAX, false, &html, &len, cancel);
    if (err != ESP_OK) return err;
    static const char MARK[] = "window.$$data=";
    const char* p = strstr((const char*)html, MARK);
    // 只解析第一个 JSON 值，后面的脚本忽略。/ Parse only the first JSON value; the script after it is ignored.
    cJSON* root = p ? cJSON_ParseWithOpts(p + strlen(MARK), NULL, false) : NULL;
    heap_caps_free(html);
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    static const char* const THREADS[] = {"topic", "threads"};
    const cJSON* threads = path(root, THREADS, 2);
    const cJSON* t;
    cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(threads, "list")) {
        char tid[24], title[200], user[48], time[24], replies[16];
        str_of(t, "tid", tid, sizeof(tid));
        str_of(t, "title", title, sizeof(title));
        str_of(cJSON_GetObjectItemCaseSensitive(t, "author"), "puname", user, sizeof(user));
        str_of(t, "createdAtFormat", time, sizeof(time));
        str_of(t, "replies", replies, sizeof(replies));
        if (!tid[0] || !title[0]) continue;
        cJSON* row = thread_row(tid, title, user, time, replies);
        if (row) { cJSON_AddItemToArray(items, row); (*added)++; }
    }
    const cJSON* cur = cJSON_GetObjectItemCaseSensitive(threads, "current");
    const cJSON* tot = cJSON_GetObjectItemCaseSensitive(threads, "total");
    *end = !*added || (cJSON_IsNumber(cur) && cJSON_IsNumber(tot) && cur->valueint >= tot->valueint);
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t tomato_hupu_threads(const char* zone, int sort, bool more, const volatile bool* cancel, char* msg, size_t cap) {
    make_more_dirs();
    char file[96];
    tomato_hupu_zone_path(zone, sort, file, sizeof(file));
    cJSON* out = more ? read_json(file) : NULL;
    if (!out) { out = cJSON_CreateObject(); more = false; }
    if (!out) { snprintf(msg, cap, "内存不足"); return ESP_ERR_NO_MEM; }
    cJSON* items = cJSON_GetObjectItemCaseSensitive(out, "items");
    if (!cJSON_IsArray(items)) {
        cJSON_DeleteItemFromObjectCaseSensitive(out, "items");
        items = cJSON_AddArrayToObject(out, "items");
    }
    const cJSON* pg = cJSON_GetObjectItemCaseSensitive(out, "page");
    const int page = more && cJSON_IsNumber(pg) ? pg->valueint + 1 : 1;
    char cursor[48] = "";
    str_of(out, "next", cursor, sizeof(cursor));
    if (more && (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(out, "end")) || cJSON_GetArraySize(items) >= ZONE_ITEMS_MAX)) {
        cJSON_Delete(out);
        snprintf(msg, cap, "没有更多了");
        return ESP_OK;
    }
    bool end = false;
    int added = 0;
    char next[48] = "";
    esp_err_t err = !items ? ESP_ERR_NO_MEM
                  : sort ? threads_by_post(zone, page, cancel, items, &end, &added)
                         : threads_by_reply(zone, page, cursor, cancel, items, next, sizeof(next), &added);
    if (err != ESP_OK) { cJSON_Delete(out); fail_msg(err, msg, cap); return err; }
    if (!sort) end = !next[0] || !added;
    if (cJSON_GetArraySize(items) >= ZONE_ITEMS_MAX) end = true;
    cJSON_DeleteItemFromObjectCaseSensitive(out, "page");
    cJSON_DeleteItemFromObjectCaseSensitive(out, "next");
    cJSON_DeleteItemFromObjectCaseSensitive(out, "end");
    cJSON_AddNumberToObject(out, "page", page);
    cJSON_AddStringToObject(out, "next", next);
    cJSON_AddBoolToObject(out, "end", end);
    err = write_json(file, out);
    cJSON_Delete(out);
    prune_dir(TOMATO_HUPU_DIR "/zone", ZONE_FILES_MAX, file);
    snprintf(msg, cap, "%s", err == ESP_OK ? "已更新" : "写入 TF 卡失败");
    return err;
}

esp_err_t tomato_hupu_follow(const volatile bool* cancel, tomato_hupu_progress_t progress, char* msg, size_t cap) {
    make_more_dirs();
    cJSON* follow = read_json(TOMATO_HUPU_DIR "/follow.json");
    const cJSON* zones = cJSON_GetObjectItemCaseSensitive(follow, "zones");
    int n = cJSON_GetArraySize(zones);
    if (n > FOLLOW_MAX) n = FOLLOW_MAX;
    if (n <= 0) {
        cJSON_Delete(follow);
        snprintf(msg, cap, "还没有关注的专区");
        return ESP_ERR_NOT_FOUND;
    }
    // 每个专区先取第一页，再轮流各取一条混排。/ Take each zone's first page, then interleave one row at a time.
    cJSON* lists = cJSON_CreateArray();
    int ok = 0;
    for (int i = 0; i < n && lists && !(cancel && *cancel); i++) {
        const cJSON* z = cJSON_GetArrayItem(zones, i);
        char id[16], name[48], next[48];
        str_of(z, "id", id, sizeof(id));
        str_of(z, "name", name, sizeof(name));
        progress(i + 1, n);
        cJSON* items = cJSON_CreateArray();
        int added = 0;
        if (!items) break;
        if (threads_by_reply(id, 1, "", cancel, items, next, sizeof(next), &added) == ESP_OK && added) {
            cJSON* row;
            cJSON_ArrayForEach(row, items) cJSON_AddStringToObject(row, "zone", name);
            cJSON_AddItemToArray(lists, items);
            ok++;
        } else {
            cJSON_Delete(items);
        }
    }
    cJSON_Delete(follow);
    if (!ok) {
        cJSON_Delete(lists);
        const bool cancelled = cancel && *cancel;
        snprintf(msg, cap, "%s", cancelled ? "已取消" : "加载失败，检查网络");
        return cancelled ? ESP_ERR_INVALID_STATE : ESP_FAIL;
    }
    cJSON* out = cJSON_CreateObject();
    cJSON* items = out ? cJSON_AddArrayToObject(out, "items") : NULL;
    for (int k = 0; items; k++) {
        bool any = false;
        const cJSON* list;
        cJSON_ArrayForEach(list, lists) {
            const cJSON* row = cJSON_GetArrayItem(list, k);
            if (!row) continue;
            any = true;
            cJSON_AddItemToArray(items, cJSON_Duplicate(row, true));
        }
        if (!any) break;
    }
    cJSON_Delete(lists);
    esp_err_t err = items ? write_json(TOMATO_HUPU_DIR "/feed.json", out) : ESP_ERR_NO_MEM;
    cJSON_Delete(out);
    snprintf(msg, cap, "%s", err == ESP_OK ? "已更新" : "写入 TF 卡失败");
    return err;
}

/* ---- 第三期：赛程与评分 / Phase three: schedule and ratings ---- */

static void member(const cJSON* m, const char* prefix, cJSON* out) {
    static const char* const FIELDS[4][2] = {
        {"memberName", ""}, {"memberBaseScore", "s"}, {"memberBigScore", "b"}, {"memberId", "i"},
    };
    for (int i = 0; i < 4; i++) {
        char v[48], key[8];
        str_of(m, FIELDS[i][0], v, sizeof(v));
        snprintf(key, sizeof(key), "%s%s", prefix, FIELDS[i][1]);
        cJSON_AddStringToObject(out, key, v);
    }
}

esp_err_t tomato_hupu_schedule(const char* league, const volatile bool* cancel, char* msg, size_t cap) {
    make_more_dirs();
    char url[240];
    snprintf(url, sizeof(url), "https://match-api.hupu.com/1/8.2.10/matchallapi/bff/standard/getScheduleListByTagForH5"
             "?businessType=common&datasource=navigation&businessId=%.12s", league);
    cJSON* root = NULL;
    esp_err_t err = fetch_json(url, SCHEDULE_MAX, cancel, &root);
    if (err != ESP_OK) { fail_msg(err, msg, cap); return err; }
    const cJSON* result = cJSON_GetObjectItemCaseSensitive(root, "result");
    char anchor[32];
    str_of(result, "anchorMatchId", anchor, sizeof(anchor));
    cJSON* out = cJSON_CreateObject();
    cJSON* days = out ? cJSON_AddArrayToObject(out, "days") : NULL;
    int today = -1, anchor_day = -1, nd = 0;
    const cJSON* day;
    cJSON_ArrayForEach(day, cJSON_GetObjectItemCaseSensitive(result, "dayGameData")) {
        if (!days) break;
        char block[32];
        str_of(day, "dateBlock", block, sizeof(block));
        cJSON* d = cJSON_CreateObject();
        cJSON* ms = d ? cJSON_AddArrayToObject(d, "m") : NULL;
        if (!ms) { cJSON_Delete(d); break; }
        cJSON_AddStringToObject(d, "d", block);
        if (!strncmp(block, "今天", strlen("今天"))) today = nd;
        const cJSON* m;
        cJSON_ArrayForEach(m, cJSON_GetObjectItemCaseSensitive(day, "matchData")) {
            cJSON* x = cJSON_CreateObject();
            if (!x) break;
            char v[64];
            str_of(m, "matchId", v, sizeof(v));
            if (anchor[0] && !strcmp(v, anchor)) anchor_day = nd;
            str_of(m, "matchStatus", v, sizeof(v));
            cJSON_AddNumberToObject(x, "st", !strcmp(v, "COMPLETED") ? 2 : !strcmp(v, "NOTSTARTED") ? 0 : 1);
            str_of(m, "matchStatusDesc", v, sizeof(v));
            cJSON_AddStringToObject(x, "sd", v);
            str_of(m, "matchIntroduction", v, sizeof(v));
            cJSON_AddStringToObject(x, "in", v);
            str_of(m, "scoreCountText", v, sizeof(v));
            cJSON_AddStringToObject(x, "c", v);
            str_of(m, "matchStartTimeStamp", v, sizeof(v));
            cJSON_AddStringToObject(x, "t", v);
            const cJSON* mem = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(m, "againstInfo"), "memberInfos");
            member(cJSON_GetArrayItem(mem, 0), "a", x);
            member(cJSON_GetArrayItem(mem, 1), "b", x);
            const cJSON* si = cJSON_GetObjectItemCaseSensitive(m, "scoreItemInfo");
            str_of(si, "scoreOutBizType", v, sizeof(v));
            cJSON_AddStringToObject(x, "k", v);
            str_of(si, "scoreOutBizNo", v, sizeof(v));
            cJSON_AddStringToObject(x, "n", v);
            cJSON_AddItemToArray(ms, x);
        }
        cJSON_AddItemToArray(days, d);
        nd++;
    }
    cJSON_Delete(root);
    if (!nd) {
        cJSON_Delete(out);
        snprintf(msg, cap, "这个赛事暂时没有赛程");
        return ESP_ERR_NOT_FOUND;
    }
    // 定位到今天；没有“今天”就用虎扑给的锚点那天。/ Locate today; fall back to Hupu's anchor day.
    cJSON_AddNumberToObject(out, "today", today >= 0 ? today : anchor_day >= 0 ? anchor_day : nd - 1);
    char file[96];
    tomato_hupu_schedule_path(league, file, sizeof(file));
    err = write_json(file, out);
    cJSON_Delete(out);
    snprintf(msg, cap, "%s", err == ESP_OK ? "已更新" : "写入 TF 卡失败");
    return err;
}

// infoJson 的值都是字符串数组，取第 0 个。/ infoJson values are string arrays; take item 0.
static void info_str(const cJSON* info, const char* key, char* out, size_t cap) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(info, key);
    if (cJSON_IsArray(v)) v = cJSON_GetArrayItem(v, 0);
    if (cJSON_IsString(v) && v->valuestring) tomato_copy_utf8(out, cap, v->valuestring);
    else if (cJSON_IsNumber(v)) snprintf(out, cap, "%.0f", v->valuedouble);
    else if (cap) out[0] = 0;
}

esp_err_t tomato_hupu_rating(const char* type, const char* no, const volatile bool* cancel, char* msg, size_t cap) {
    make_more_dirs();
    static const char BASE[] = "https://games.mobileapi.hupu.com/1/8.0.99/bplcommentapi/bpl/score_tree";
    char url[260], mtype[40], mno[24];
    snprintf(mtype, sizeof(mtype), "%s", type);
    snprintf(mno, sizeof(mno), "%s", no);
    cJSON* root = NULL;
    esp_err_t err;
    // 赛程给的是条目（当场最高分的人），先找它所属的比赛。/ The schedule gives an item; find its parent match first.
    if (!strstr(type, "_match")) {
        snprintf(url, sizeof(url), "%s/getSelfByBizKey?outBizType=%.32s&outBizNo=%.16s", BASE, type, no);
        err = fetch_json(url, JSON_GET_MAX, cancel, &root);
        if (err != ESP_OK) { fail_msg(err, msg, cap); return err; }
        const cJSON* l;
        bool found = false;
        cJSON_ArrayForEach(l, cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "data"), "linkNodes")) {
            char target[16];
            str_of(l, "target", target, sizeof(target));
            if (strcmp(target, "PARENT")) continue;
            const cJSON* key = cJSON_GetObjectItemCaseSensitive(l, "outBizKey");
            str_of(key, "outBizType", mtype, sizeof(mtype));
            str_of(key, "outBizNo", mno, sizeof(mno));
            found = mtype[0] && mno[0];
            break;
        }
        cJSON_Delete(root);
        root = NULL;
        if (!found) { snprintf(msg, cap, "这场比赛还没有评分"); return ESP_ERR_NOT_FOUND; }
    }
    snprintf(url, sizeof(url), "%s/getCurAndSubNodeByBizKey?outBizType=%.32s&outBizNo=%.16s&relation=CHILD&page=1&pageSize=60",
             BASE, mtype, mno);
    err = fetch_json(url, JSON_GET_MAX, cancel, &root);
    if (err != ESP_OK) { fail_msg(err, msg, cap); return err; }
    const cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    const cJSON* self = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(data, "self"), "node");
    cJSON* out = cJSON_CreateObject();
    cJSON* players = out ? cJSON_AddArrayToObject(out, "p") : NULL;
    char v[96];
    if (players) {
        str_of(self, "name", v, sizeof(v));
        cJSON_AddStringToObject(out, "title", v);
        str_of(self, "summedScorePersonCount", v, sizeof(v));
        cJSON_AddStringToObject(out, "persons", v);
    }
    const cJSON* it;
    int n = 0;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(data, "pageResult"), "data")) {
        if (!players) break;
        const cJSON* node = cJSON_GetObjectItemCaseSensitive(it, "node");
        const cJSON* info = cJSON_GetObjectItemCaseSensitive(node, "infoJson");
        cJSON* x = cJSON_CreateObject();
        if (!x) break;
        str_of(node, "name", v, sizeof(v));
        cJSON_AddStringToObject(x, "n", v);
        const cJSON* avg = cJSON_GetObjectItemCaseSensitive(node, "scoreAvg");
        cJSON_AddNumberToObject(x, "s", cJSON_IsNumber(avg) ? avg->valuedouble : atof(cJSON_IsString(avg) ? avg->valuestring : "0"));
        str_of(node, "scorePersonCount", v, sizeof(v));
        cJSON_AddNumberToObject(x, "c", atoi(v));
        info_str(info, "teamId", v, sizeof(v));
        cJSON_AddStringToObject(x, "team", v);
        info_str(info, "type", v, sizeof(v));
        cJSON_AddStringToObject(x, "type", v);
        info_str(info, "minutes", v, sizeof(v));
        cJSON_AddStringToObject(x, "min", v);
        // 技术统计拼成一行：得分、篮板、助攻，有抢断盖帽再加。/ One stats line: points, rebounds, assists, then steals/blocks.
        char pts[8], reb[8], ast[8], stl[8], blk[8], line[64] = "";
        info_str(info, "pts", pts, sizeof(pts));
        info_str(info, "reb", reb, sizeof(reb));
        info_str(info, "ast", ast, sizeof(ast));
        info_str(info, "stl", stl, sizeof(stl));
        info_str(info, "blk", blk, sizeof(blk));
        if (pts[0]) {
            int k = snprintf(line, sizeof(line), "%s分 %s板 %s助", pts, reb[0] ? reb : "0", ast[0] ? ast : "0");
            if (atoi(stl) > 0 && k > 0 && k < (int)sizeof(line)) k += snprintf(line + k, sizeof(line) - k, " %s断", stl);
            if (atoi(blk) > 0 && k > 0 && k < (int)sizeof(line)) snprintf(line + k, sizeof(line) - k, " %s帽", blk);
        }
        cJSON_AddStringToObject(x, "st", line);
        cJSON_AddItemToArray(players, x);
        n++;
    }
    cJSON_Delete(root);
    if (!n) {
        cJSON_Delete(out);
        snprintf(msg, cap, "这场比赛还没有评分");
        return ESP_ERR_NOT_FOUND;
    }
    char file[96];
    tomato_hupu_rating_path(type, no, file, sizeof(file));
    err = write_json(file, out);
    cJSON_Delete(out);
    prune_dir(TOMATO_HUPU_DIR "/rate", RATE_FILES_MAX, file);
    snprintf(msg, cap, "%s", err == ESP_OK ? "已更新" : "写入 TF 卡失败");
    return err;
}
