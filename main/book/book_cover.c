/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：封面提取、缩略图缓存和绘制。缓存文件头记下源文件大小和修改时间，源文件变了就重建。
 *
 * English: Cover extraction, thumbnail cache and drawing. The cache header records the source size and
 * mtime, so a changed source is rebuilt.
 *
 * 冻结：只在主任务调用；解码走 book_image_decode，大小上限沿用它的约束。
 * Frozen: main task only; decoding goes through book_image_decode and keeps its size limits.
 */

#include "esp_attr.h"
#include "book_cover.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "book_image.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "tomato.h"
#include "zip_reader.h"

#define TAG "book_cover"
#define COVER_ROOT "/sdcard/.treader"
#define COVER_DIR COVER_ROOT "/covers"
#define SCREEN_DIR COVER_ROOT "/wall"
#define SLOT_MAX 12
#define QUEUE_MAX 12   // 书城一页 9 本 / A store page shows 9
#define PATH_MAX_LEN 320
#define PX_BYTES ((size_t)BOOK_COVER_W * BOOK_COVER_H)
#define IMAGE_MAX (2u * 1024u * 1024u)
#define OPF_MAX (1024u * 1024u)
#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

// 缓存文件头；w 为 0 表示“确认没有封面”，不再重试。/ Cache header; w == 0 records "no cover" so it is not retried.
typedef struct {
    char magic[4];
    uint32_t src_size;
    uint32_t src_mtime;
    uint16_t w, h;
} cover_head_t;

typedef struct {
    uint32_t key;
    uint32_t used;
    book_cover_state_t state;   // READY 或 NONE / READY or NONE
    uint8_t* px;                // READY 时 300×400 灰度 / 300x400 gray when READY
} slot_t;

static slot_t s_slots[SLOT_MAX];
static uint32_t s_clock;
EXT_RAM_BSS_ATTR static char s_queue[QUEUE_MAX][PATH_MAX_LEN];
static int s_queue_count;
static bool s_built_new;

/* ---- 工具 / Helpers ---- */

static uint32_t path_key(const char* s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h ? h : 1;
}

static bool has_ext(const char* path, const char* ext) {
    const char* dot = strrchr(path, '.');
    return dot && !strcasecmp(dot, ext);
}

// 找封面源文件：EPUB 是书本身，番茄 TXT 是下载时存的图。/ Cover source: the EPUB itself, or the image saved for a Tomato TXT.
static bool cover_source(const char* book, char* src, size_t cap, bool* epub, struct stat* st) {
    *epub = has_ext(book, ".epub");
    if (*epub || book_image_name(book)) {
        // EPUB 和图片本身就是源。/ An EPUB or an image is its own source.
        snprintf(src, cap, "%s", book);
    } else {
        const size_t dir_len = strlen(TOMATO_BOOKS_DIR);
        if (!has_ext(book, ".txt") || strncmp(book, TOMATO_BOOKS_DIR "/", dir_len + 1) || strchr(book + dir_len + 1, '/'))
            return false;
        if (snprintf(src, cap, "%s/%s.img", TOMATO_COVER_DIR, book + dir_len + 1) >= (int)cap) return false;
    }
    return stat(src, st) == 0 && st->st_size > 0;
}

// 书城封面的缩略图放在封面旁边，跟着书城缓存一起清理；其余放 .treader/covers。
// Store-cover thumbnails sit beside the cover so they are cleaned with the store cache; others go under .treader/covers.
static void cache_path(const char* book, char* out, size_t cap) {
    static const char STORE_COVERS[] = TOMATO_RANK_DIR "/covers/";
    const char* dot = strrchr(book, '.');
    if (!strncmp(book, STORE_COVERS, sizeof(STORE_COVERS) - 1) && dot && !strchr(dot, '/') &&
        snprintf(out, cap, "%.*s.tcv", (int)(dot - book), book) < (int)cap)
        return;
    snprintf(out, cap, COVER_DIR "/%08lx.tcv", (unsigned long)path_key(book));
}

static slot_t* find_slot(uint32_t key) {
    for (int i = 0; i < SLOT_MAX; i++)
        if (s_slots[i].key == key) { s_slots[i].used = ++s_clock; return &s_slots[i]; }
    return NULL;
}

// 取一个空位，满了就挤掉最久没用的。/ Take a free slot, evicting the least recently used.
static slot_t* new_slot(uint32_t key) {
    slot_t* best = &s_slots[0];
    for (int i = 0; i < SLOT_MAX; i++) {
        if (!s_slots[i].key) { best = &s_slots[i]; break; }
        if (s_slots[i].used < best->used) best = &s_slots[i];
    }
    heap_caps_free(best->px);
    memset(best, 0, sizeof(*best));
    best->key = key;
    best->used = ++s_clock;
    return best;
}

static void enqueue(const char* book) {
    for (int i = 0; i < s_queue_count; i++) if (!strcmp(s_queue[i], book)) return;
    if (s_queue_count >= QUEUE_MAX || strlen(book) >= PATH_MAX_LEN) return;
    snprintf(s_queue[s_queue_count++], PATH_MAX_LEN, "%s", book);
}

/* ---- 读缓存 / Cache read ---- */

book_cover_state_t book_cover_load(const char* book) {
    if (!book || !book[0]) return BOOK_COVER_NONE;
    const uint32_t key = path_key(book);
    slot_t* slot = find_slot(key);
    if (slot) return slot->state;
    char src[PATH_MAX_LEN];
    bool epub;
    struct stat st;
    if (!cover_source(book, src, sizeof(src), &epub, &st)) return BOOK_COVER_NONE;

    char path[PATH_MAX_LEN];
    cache_path(book, path, sizeof(path));
    FILE* f = fopen(path, "rb");
    cover_head_t head;
    bool fresh = f && fread(&head, sizeof(head), 1, f) == 1 && !memcmp(head.magic, "TRC1", 4) &&
                 head.src_size == (uint32_t)st.st_size && head.src_mtime == (uint32_t)st.st_mtime;
    if (!fresh) {
        if (f) fclose(f);
        enqueue(book);
        return BOOK_COVER_PENDING;
    }
    book_cover_state_t state = BOOK_COVER_NONE;
    uint8_t* px = NULL;
    if (head.w == BOOK_COVER_W && head.h == BOOK_COVER_H) {
        px = heap_caps_malloc(PX_BYTES, PSRAM_CAPS);
        if (px && fread(px, 1, PX_BYTES, f) == PX_BYTES) state = BOOK_COVER_READY;
        else { heap_caps_free(px); px = NULL; }
    }
    fclose(f);
    // 读失败（内存不足或文件坏了）不记成“没有封面”，下次再试。/ A failed read is not remembered as "no cover".
    if (head.w && state != BOOK_COVER_READY) return BOOK_COVER_NONE;
    slot = new_slot(key);
    slot->state = state;
    slot->px = px;
    return state;
}

/* ---- 从 EPUB 找封面 / Find the EPUB cover ---- */

// 在 [p, end) 的标签里取属性值。/ Read an attribute value inside the tag [p, end).
static bool tag_attr(const char* p, const char* end, const char* name, char* out, size_t cap) {
    const size_t n = strlen(name);
    for (const char* q = p; q + n + 2 < end; q++) {
        if (strncmp(q, name, n) || (q > p && !isspace((unsigned char)q[-1]))) continue;
        const char* v = q + n;
        while (v < end && isspace((unsigned char)*v)) v++;
        if (v >= end || *v != '=') continue;
        v++;
        while (v < end && isspace((unsigned char)*v)) v++;
        if (v >= end || (*v != '"' && *v != '\'')) continue;
        const char quote = *v++;
        const char* e = memchr(v, quote, (size_t)(end - v));
        if (!e || (size_t)(e - v) >= cap) return false;
        memcpy(out, v, (size_t)(e - v));
        out[e - v] = 0;
        return true;
    }
    return false;
}

// 遍历某种标签，回调返回 true 停止。/ Walk tags of one name; stop when the callback returns true.
typedef bool (*tag_fn)(const char* p, const char* end, void* arg, char* out, size_t cap);
static bool each_tag(const char* xml, const char* tag, tag_fn fn, void* arg, char* out, size_t cap) {
    const size_t n = strlen(tag);
    for (const char* p = strstr(xml, tag); p; p = strstr(p + n, tag)) {
        const char c = p[n];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '/' && c != '>') continue;
        const char* end = strchr(p, '>');
        if (!end) return false;
        if (fn(p, end, arg, out, cap)) return true;
    }
    return false;
}

// EPUB 3：manifest 里 properties 含 cover-image 的项。/ EPUB 3: the manifest item whose properties include cover-image.
static bool item_cover_image(const char* p, const char* end, void* arg, char* out, size_t cap) {
    (void)arg;
    char props[96];
    return tag_attr(p, end, "properties", props, sizeof(props)) && strstr(props, "cover-image") &&
           tag_attr(p, end, "href", out, cap);
}

// EPUB 2：<meta name="cover" content="id"/>。/ EPUB 2: <meta name="cover" content="id"/>.
static bool meta_cover(const char* p, const char* end, void* arg, char* out, size_t cap) {
    (void)arg;
    char name[16];
    return tag_attr(p, end, "name", name, sizeof(name)) && !strcmp(name, "cover") && tag_attr(p, end, "content", out, cap);
}

static bool item_by_id(const char* p, const char* end, void* arg, char* out, size_t cap) {
    char id[128];
    return tag_attr(p, end, "id", id, sizeof(id)) && !strcmp(id, (const char*)arg) && tag_attr(p, end, "href", out, cap);
}

// 解开 %XX，处理 ./ 和 ../，拼成 ZIP 内路径。/ Decode %XX, resolve ./ and ../, and join into a ZIP path.
static bool resolve(const char* base_dir, const char* href, char* out, size_t cap) {
    char joined[PATH_MAX_LEN];
    if (snprintf(joined, sizeof(joined), "%s%s", base_dir, href) >= (int)sizeof(joined)) return false;
    char* q = strchr(joined, '#');
    if (q) *q = 0;
    size_t o = 0;
    for (const char* p = joined; *p && o + 1 < cap;) {
        if (p[0] == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = {p[1], p[2], 0};
            out[o++] = (char)strtol(hex, NULL, 16);
            p += 3;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = 0;
    // 逐段处理 . 和 ..。/ Resolve . and .. segment by segment.
    char tmp[PATH_MAX_LEN];
    size_t t = 0;
    char* save = NULL;
    for (char* seg = strtok_r(out, "/", &save); seg; seg = strtok_r(NULL, "/", &save)) {
        if (!strcmp(seg, ".")) continue;
        if (!strcmp(seg, "..")) {
            while (t && tmp[t - 1] != '/') t--;
            if (t) t--;
            continue;
        }
        size_t n = strlen(seg);
        if (t + n + 2 >= sizeof(tmp)) return false;
        if (t) tmp[t++] = '/';
        memcpy(tmp + t, seg, n);
        t += n;
    }
    tmp[t] = 0;
    snprintf(out, cap, "%s", tmp);
    return t > 0;
}

static char* extract(zip_reader_t* zip, int index, size_t max, size_t* len) {
    const size_t n = zip_entry_size(zip, index);
    if (!n || n > max) return NULL;
    char* buf = heap_caps_malloc(n + 1, PSRAM_CAPS);
    if (!buf) return NULL;
    if (zip_extract(zip, index, buf, n) != ESP_OK) { heap_caps_free(buf); return NULL; }
    buf[n] = 0;
    *len = n;
    return buf;
}

// 不区分大小写地查子串。/ Case-insensitive substring search.
static bool contains_nocase(const char* s, const char* needle) {
    const size_t n = strlen(needle);
    for (; *s; s++) if (!strncasecmp(s, needle, n)) return true;
    return false;
}

bool book_image_name(const char* name) {
    return has_ext(name, ".jpg") || has_ext(name, ".jpeg") || has_ext(name, ".png");
}

// 读出 EPUB 封面图的原始字节。/ Read the raw bytes of the EPUB cover image.
static uint8_t* epub_cover_bytes(const char* path, size_t* len) {
    zip_reader_t* zip = NULL;
    if (zip_open(path, &zip) != ESP_OK) return NULL;
    char opf_path[PATH_MAX_LEN] = "", href[PATH_MAX_LEN] = "", target[PATH_MAX_LEN] = "";
    size_t n = 0;
    char* xml = NULL;
    int index = zip_find(zip, "META-INF/container.xml");
    if (index >= 0 && (xml = extract(zip, index, 64 * 1024, &n))) {
        const char* root = strstr(xml, "<rootfile");
        const char* end = root ? strchr(root, '>') : NULL;
        if (end) tag_attr(root, end, "full-path", opf_path, sizeof(opf_path));
        heap_caps_free(xml);
        xml = NULL;
    }
    index = opf_path[0] ? zip_find(zip, opf_path) : -1;
    if (index >= 0 && (xml = extract(zip, index, OPF_MAX, &n))) {
        char id[128];
        bool found = each_tag(xml, "<item", item_cover_image, NULL, href, sizeof(href));
        if (!found && each_tag(xml, "<meta", meta_cover, NULL, id, sizeof(id)))
            found = each_tag(xml, "<item", item_by_id, id, href, sizeof(href));
        if (found && book_image_name(href)) {
            char dir[PATH_MAX_LEN];
            snprintf(dir, sizeof(dir), "%s", opf_path);
            char* slash = strrchr(dir, '/');
            if (slash) slash[1] = 0;
            else dir[0] = 0;
            resolve(dir, href, target, sizeof(target));
        }
        heap_caps_free(xml);
    }
    index = target[0] ? zip_find(zip, target) : -1;
    // 元数据没写封面时，找文件名带 cover 的图片。/ Without cover metadata, take an image named like "cover".
    for (int i = 0; index < 0 && i < (int)zip_entry_count(zip); i++) {
        const char* name = zip_entry_name(zip, i);
        if (!name || !book_image_name(name)) continue;
        const char* base = strrchr(name, '/');
        base = base ? base + 1 : name;
        if (contains_nocase(base, "cover")) index = i;
    }
    uint8_t* data = index >= 0 ? (uint8_t*)extract(zip, index, IMAGE_MAX, len) : NULL;
    zip_close(zip);
    return data;
}

static uint8_t* file_bytes(const char* path, size_t* len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t* data = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long n = ftell(f);
        if (n > 0 && (unsigned long)n <= IMAGE_MAX && fseek(f, 0, SEEK_SET) == 0 &&
            (data = heap_caps_malloc((size_t)n, PSRAM_CAPS))) {
            if (fread(data, 1, (size_t)n, f) == (size_t)n) *len = (size_t)n;
            else { heap_caps_free(data); data = NULL; }
        }
    }
    fclose(f);
    return data;
}

/* ---- 生成 / Build ---- */

// 按比例铺满 dw×dh 并居中裁切；缩小取区域平均，放大用双线性。
// Fill dw x dh keeping aspect, crop centered; average when shrinking, bilinear when enlarging.
static void fit_image(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dh) {
    const float sx = (float)dw / sw, sy = (float)dh / sh;
    const float s = sx > sy ? sx : sy;
    const float vis_w = dw / s, vis_h = dh / s;
    const float ox = (sw - vis_w) / 2, oy = (sh - vis_h) / 2;
    for (int y = 0; y < dh; y++) {
        if (s > 1.0f) {
            float fy = oy + (y + 0.5f) / s - 0.5f;
            if (fy < 0) fy = 0;
            int y0 = (int)fy, y1 = y0 + 1 < sh ? y0 + 1 : y0;
            const float wy = fy - y0;
            for (int x = 0; x < dw; x++) {
                float fx = ox + (x + 0.5f) / s - 0.5f;
                if (fx < 0) fx = 0;
                int x0 = (int)fx, x1 = x0 + 1 < sw ? x0 + 1 : x0;
                const float wx = fx - x0;
                const float top = src[(size_t)y0 * sw + x0] * (1 - wx) + src[(size_t)y0 * sw + x1] * wx;
                const float bot = src[(size_t)y1 * sw + x0] * (1 - wx) + src[(size_t)y1 * sw + x1] * wx;
                dst[(size_t)y * dw + x] = (uint8_t)(top * (1 - wy) + bot * wy + 0.5f);
            }
            continue;
        }
        int y0 = (int)(oy + y / s), y1 = (int)(oy + (y + 1) / s);
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > sh) y1 = sh;
        if (y0 >= sh) y0 = sh - 1;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)(ox + x / s), x1 = (int)(ox + (x + 1) / s);
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > sw) x1 = sw;
            if (x0 >= sw) x0 = sw - 1;
            uint32_t sum = 0, cnt = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) { sum += src[(size_t)yy * sw + xx]; cnt++; }
            dst[(size_t)y * dw + x] = (uint8_t)(cnt ? sum / cnt : 255);
        }
    }
}

void book_image_fill(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dh) {
    fit_image(src, sw, sh, dst, dw, dh);
}

static void write_cache(const char* book, const struct stat* st, const uint8_t* px) {
    mkdir(COVER_ROOT, 0775);
    mkdir(COVER_DIR, 0775);
    char path[PATH_MAX_LEN], tmp[PATH_MAX_LEN + 8];
    cache_path(book, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    cover_head_t head = {.magic = {'T', 'R', 'C', '1'}, .src_size = (uint32_t)st->st_size,
                         .src_mtime = (uint32_t)st->st_mtime, .w = px ? BOOK_COVER_W : 0, .h = px ? BOOK_COVER_H : 0};
    FILE* f = fopen(tmp, "wb");
    if (!f) return;
    bool ok = fwrite(&head, sizeof(head), 1, f) == 1 && (!px || fwrite(px, 1, PX_BYTES, f) == PX_BYTES);
    ok = fclose(f) == 0 && ok;
    remove(path);
    if (!ok || rename(tmp, path) != 0) remove(tmp);
}

// 生成一本书的封面；返回是否得到了图。/ Build one book's cover; returns whether an image resulted.
static bool build(const char* book) {
    const int64_t t0 = esp_timer_get_time();  // 临时诊断 / Temporary diagnostics
    char src[PATH_MAX_LEN];
    bool epub;
    struct stat st;
    if (!cover_source(book, src, sizeof(src), &epub, &st)) return false;
    size_t len = 0;
    uint8_t* data = epub ? epub_cover_bytes(src, &len) : file_bytes(src, &len);
    uint8_t* gray = NULL;
    uint16_t w = 0, h = 0;
    bool decoded = data && book_image_decode(data, len, 648u * 1000u, &gray, &w, &h);
    heap_caps_free(data);
    uint8_t* px = decoded ? heap_caps_malloc(PX_BYTES, PSRAM_CAPS) : NULL;
    if (px) fit_image(gray, w, h, px, BOOK_COVER_W, BOOK_COVER_H);
    free(gray);
    if (decoded && !px) return false;   // 只是内存不够，下次再试 / Only out of memory; retry later
    write_cache(book, &st, px);
    slot_t* slot = new_slot(path_key(book));
    slot->state = px ? BOOK_COVER_READY : BOOK_COVER_NONE;
    slot->px = px;
    ESP_LOGI(TAG, "%s: %s %lld ms", book, px ? "cover" : "none", (long long)((esp_timer_get_time() - t0) / 1000));
    return px != NULL;
}

bool book_cover_tick(void) {
    if (!s_queue_count) return false;
    char book[PATH_MAX_LEN];
    memcpy(book, s_queue[0], sizeof(book));
    memmove(s_queue[0], s_queue[1], sizeof(s_queue[0]) * (size_t)(--s_queue_count));
    if (!find_slot(path_key(book)) && build(book)) s_built_new = true;
    if (s_queue_count || !s_built_new) return false;
    s_built_new = false;
    return true;
}

void book_cover_drop_pending(void) {
    s_queue_count = 0;
    s_built_new = false;
}

void book_cover_forget(const char* book) {
    if (!book || !book[0]) return;
    const uint32_t key = path_key(book);
    slot_t* slot = find_slot(key);
    if (slot) {
        heap_caps_free(slot->px);
        memset(slot, 0, sizeof(*slot));
    }
    char path[PATH_MAX_LEN];
    cache_path(book, path, sizeof(path));
    remove(path);
}

void book_cover_release(void) {
    for (int i = 0; i < SLOT_MAX; i++) heap_caps_free(s_slots[i].px);
    memset(s_slots, 0, sizeof(s_slots));
    book_cover_drop_pending();
}

/* ---- 绘制 / Drawing ---- */

// 4×4 有序抖动到 16 级灰度。/ 4x4 ordered dither to 16 gray levels.
static const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

bool book_cover_draw(uint8_t* fb, EpdRect r, const char* book) {
    if (r.width < 8 || r.height < 8 || book_cover_load(book) != BOOK_COVER_READY) return false;
    const slot_t* slot = find_slot(path_key(book));
    if (!slot || !slot->px) return false;
    const uint8_t* px = slot->px;
    for (int y = 0; y < r.height; y++) {
        int y0 = y * BOOK_COVER_H / r.height, y1 = (y + 1) * BOOK_COVER_H / r.height;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < r.width; x++) {
            int x0 = x * BOOK_COVER_W / r.width, x1 = (x + 1) * BOOK_COVER_W / r.width;
            if (x1 <= x0) x1 = x0 + 1;
            uint32_t sum = 0, cnt = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) { sum += px[yy * BOOK_COVER_W + xx]; cnt++; }
            const uint32_t v = sum / cnt;
            uint32_t q = (v * 15 + (BAYER[y & 3][x & 3] * 255u + 128u) / 16u) / 255u;
            if (q > 15) q = 15;
            epd_draw_pixel(r.x + x, r.y + y, (uint8_t)(q * 17), fb);
        }
    }
    epd_draw_rect(r, 0, fb);
    epd_draw_rect((EpdRect){r.x + 1, r.y + 1, r.width - 2, r.height - 2}, 0, fb);
    return true;
}

/* ---- 全屏壁纸 / Full-screen wallpaper ---- */

static void screen_path(const char* image, char* out, size_t cap) {
    snprintf(out, cap, SCREEN_DIR "/%08lx.tws", (unsigned long)path_key(image));
}

// 缓存头和源文件对得上就打开，文件指针停在像素数据处。/ Open the cache when it matches the source, positioned at the pixels.
static FILE* open_screen(const char* image) {
    struct stat st;
    if (!book_image_name(image) || stat(image, &st) != 0) return NULL;
    char path[64];
    screen_path(image, path, sizeof(path));
    FILE* f = fopen(path, "rb");
    cover_head_t head;
    if (f && fread(&head, sizeof(head), 1, f) == 1 && !memcmp(head.magic, "TRS1", 4) &&
        head.src_size == (uint32_t)st.st_size && head.src_mtime == (uint32_t)st.st_mtime &&
        head.w == BOOK_SCREEN_W && head.h == BOOK_SCREEN_H)
        return f;
    if (f) fclose(f);
    return NULL;
}

// Floyd-Steinberg 抖动到 16 级并打包成每字节两点。/ Floyd-Steinberg dither to 16 levels, packed two pixels per byte.
static bool dither_pack(const uint8_t* gray, uint8_t* packed) {
    int16_t* err = heap_caps_calloc(2 * (BOOK_SCREEN_W + 2), sizeof(int16_t), PSRAM_CAPS);
    if (!err) return false;
    int16_t* cur = err + 1;
    int16_t* next = err + BOOK_SCREEN_W + 3;
    for (int y = 0; y < BOOK_SCREEN_H; y++) {
        memset(next - 1, 0, (BOOK_SCREEN_W + 2) * sizeof(int16_t));
        for (int x = 0; x < BOOK_SCREEN_W; x++) {
            int v = gray[(size_t)y * BOOK_SCREEN_W + x] + cur[x] / 16;
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            const int q = (v * 15 + 127) / 255;
            const int e = v - q * 17;
            cur[x + 1] += (int16_t)(e * 7);
            next[x - 1] += (int16_t)(e * 3);
            next[x] += (int16_t)(e * 5);
            next[x + 1] += (int16_t)e;
            uint8_t* b = &packed[((size_t)y * BOOK_SCREEN_W + x) / 2];
            *b = (x & 1) ? (uint8_t)((*b & 0xF0) | q) : (uint8_t)((q << 4) | (*b & 0x0F));
        }
        int16_t* t = cur;
        cur = next;
        next = t;
    }
    heap_caps_free(err);
    return true;
}

bool book_screen_build(const char* image) {
    FILE* f = image ? open_screen(image) : NULL;
    if (f) { fclose(f); return true; }
    struct stat st;
    if (!image || !book_image_name(image) || stat(image, &st) != 0) return false;
    size_t len = 0;
    uint8_t* data = file_bytes(image, &len);
    uint8_t* gray = NULL;
    uint16_t w = 0, h = 0;
    bool ok = data && book_image_decode(data, len, 648u * 1000u, &gray, &w, &h);
    heap_caps_free(data);
    uint8_t* full = ok ? heap_caps_malloc((size_t)BOOK_SCREEN_W * BOOK_SCREEN_H, PSRAM_CAPS) : NULL;
    uint8_t* packed = full ? heap_caps_calloc(1, BOOK_SCREEN_BYTES, PSRAM_CAPS) : NULL;
    ok = packed != NULL;
    if (ok) {
        fit_image(gray, w, h, full, BOOK_SCREEN_W, BOOK_SCREEN_H);
        ok = dither_pack(full, packed);
    }
    free(gray);
    heap_caps_free(full);
    if (ok) {
        mkdir(COVER_ROOT, 0775);
        mkdir(SCREEN_DIR, 0775);
        char path[64], tmp[72];
        screen_path(image, path, sizeof(path));
        snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        cover_head_t head = {.magic = {'T', 'R', 'S', '1'}, .src_size = (uint32_t)st.st_size,
                             .src_mtime = (uint32_t)st.st_mtime, .w = BOOK_SCREEN_W, .h = BOOK_SCREEN_H};
        FILE* out = fopen(tmp, "wb");
        ok = out && fwrite(&head, sizeof(head), 1, out) == 1 &&
             fwrite(packed, 1, BOOK_SCREEN_BYTES, out) == BOOK_SCREEN_BYTES;
        if (out) ok = fclose(out) == 0 && ok;
        remove(path);
        if (!ok || rename(tmp, path) != 0) { remove(tmp); ok = false; }
    }
    heap_caps_free(packed);
    ESP_LOGI(TAG, "wallpaper %s: %s", image, ok ? "ok" : "failed");
    return ok;
}

uint8_t* book_screen_load(const char* image) {
    FILE* f = image ? open_screen(image) : NULL;
    if (!f) return NULL;
    uint8_t* packed = heap_caps_malloc(BOOK_SCREEN_BYTES, PSRAM_CAPS);
    if (packed && fread(packed, 1, BOOK_SCREEN_BYTES, f) != BOOK_SCREEN_BYTES) { heap_caps_free(packed); packed = NULL; }
    fclose(f);
    return packed;
}

void book_screen_draw(uint8_t* fb, const uint8_t* packed) {
    for (int y = 0; y < BOOK_SCREEN_H; y++) {
        const uint8_t* row = packed + (size_t)y * (BOOK_SCREEN_W / 2);
        for (int x = 0; x < BOOK_SCREEN_W; x += 2) {
            const uint8_t b = row[x / 2];
            epd_draw_pixel(x, y, (uint8_t)((b >> 4) * 17), fb);
            epd_draw_pixel(x + 1, y, (uint8_t)((b & 0x0F) * 17), fb);
        }
    }
}

void book_screen_prune(const char* keep) {
    char keep_name[64] = "";
    if (keep && keep[0]) {
        char full[64];
        screen_path(keep, full, sizeof(full));
        const char* base = strrchr(full, '/');
        snprintf(keep_name, sizeof(keep_name), "%s", base ? base + 1 : full);
    }
    DIR* d = opendir(SCREEN_DIR);
    if (!d) return;
    struct dirent* e;
    char path[300];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, keep_name)) continue;
        snprintf(path, sizeof(path), SCREEN_DIR "/%.255s", e->d_name);
        remove(path);
    }
    closedir(d);
}
