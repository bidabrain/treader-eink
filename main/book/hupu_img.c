/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 虎扑图片缓存：JPG → 指定尺寸灰度 → TF 卡缓存文件（与 JPG 同名加尺寸）和内存 LRU。
 *
 * English: Hupu image cache: JPEG -> gray at a set size -> a TF cache file (the JPEG name plus the size) and an
 * in-memory LRU.
 *
 * 冻结：内存里的图总量不超过 MEM_MAX（2 MB）；解码失败的图记下来，不反复重试。
 * Frozen: in-memory images stay under MEM_MAX in total; failed decodes are remembered and not retried.
 */

#include "esp_attr.h"
#include "hupu_img.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "book_cover.h"
#include "book_image.h"
#include "epdiy.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "tomato.h"

#define TAG "hupu_img"
#define SLOT_MAX 16
#define MEM_MAX (2u * 1024u * 1024u)   // 一张全屏图约 770 KB / A full-screen image is ~770 KB
#define QUEUE_MAX 20
#define FAILED_MAX 32
#define FILE_MAX (2u * 1024u * 1024u)
#define PATH_LEN 112
#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

typedef struct {
    uint32_t key;
    uint32_t used;
    uint16_t w, h;
    uint8_t* px;
} slot_t;

typedef struct {
    char jpg[PATH_LEN];
    uint16_t w, h;
} job_t;

static slot_t s_slots[SLOT_MAX];
static uint32_t s_clock;
EXT_RAM_BSS_ATTR static job_t s_queue[QUEUE_MAX];
static int s_queue_count;
static bool s_built;
static uint32_t s_failed[FAILED_MAX];
static int s_failed_count;

// 4×4 有序抖动到 16 级灰度。/ 4x4 ordered dither to 16 gray levels.
static const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

static uint32_t key_of(const char* jpg, int w, int h) {
    uint32_t k = 2166136261u;
    for (const char* s = jpg; *s; s++) { k ^= (uint8_t)*s; k *= 16777619u; }
    k ^= (uint32_t)w * 2654435761u ^ (uint32_t)h * 40503u;
    return k ? k : 1;
}

// 缓存文件：去掉 .jpg 再加“_宽x高.g”。/ Cache file: drop .jpg and append "_WxH.g".
static void cache_path(const char* jpg, int w, int h, char* out, size_t cap) {
    const char* dot = strrchr(jpg, '.');
    const int base = dot ? (int)(dot - jpg) : (int)strlen(jpg);
    snprintf(out, cap, "%.*s_%dx%d.g", base, jpg, w, h);
}

static size_t mem_used(void) {
    size_t n = 0;
    for (int i = 0; i < SLOT_MAX; i++) if (s_slots[i].px) n += (size_t)s_slots[i].w * s_slots[i].h;
    return n;
}

static void free_slot(slot_t* s) {
    heap_caps_free(s->px);
    memset(s, 0, sizeof(*s));
}

// 放进内存：位置满了或总量超了就挤掉最久没用的。/ Insert, evicting least recently used slots by count or size.
static void insert(uint32_t key, int w, int h, uint8_t* px) {
    const size_t need = (size_t)w * h;
    for (;;) {
        int empty = -1, oldest = -1;
        for (int i = 0; i < SLOT_MAX; i++) {
            if (!s_slots[i].px) { if (empty < 0) empty = i; continue; }
            if (oldest < 0 || s_slots[i].used < s_slots[oldest].used) oldest = i;
        }
        if (empty >= 0 && mem_used() + need <= MEM_MAX) {
            s_slots[empty] = (slot_t){.key = key, .used = ++s_clock, .w = (uint16_t)w, .h = (uint16_t)h, .px = px};
            return;
        }
        if (oldest < 0) { heap_caps_free(px); return; }
        free_slot(&s_slots[oldest]);
    }
}

static bool failed(uint32_t key) {
    for (int i = 0; i < s_failed_count; i++) if (s_failed[i] == key) return true;
    return false;
}

bool hupu_img_failed(const char* jpg, int w, int h) {
    return jpg && jpg[0] && w > 0 && h > 0 && failed(key_of(jpg, w, h));
}

const uint8_t* hupu_img_get(const char* jpg, int w, int h) {
    if (!jpg || !jpg[0] || w <= 0 || h <= 0) return NULL;
    const uint32_t key = key_of(jpg, w, h);
    for (int i = 0; i < SLOT_MAX; i++)
        if (s_slots[i].px && s_slots[i].key == key) { s_slots[i].used = ++s_clock; return s_slots[i].px; }
    if (failed(key)) return NULL;
    char path[PATH_LEN + 24];
    cache_path(jpg, w, h, path, sizeof(path));
    tomato_hupu_cache_lock();
    FILE* f = fopen(path, "rb");
    if (f) {
        char magic[4];
        uint16_t dims[2];
        uint8_t* px = heap_caps_malloc((size_t)w * h, PSRAM_CAPS);
        bool ok = px && fread(magic, 1, 4, f) == 4 && !memcmp(magic, "HPG1", 4) && fread(dims, sizeof(dims), 1, f) == 1 &&
                  dims[0] == w && dims[1] == h && fread(px, 1, (size_t)w * h, f) == (size_t)w * h;
        fclose(f);
        if (ok) { tomato_hupu_cache_unlock(); insert(key, w, h, px); return px; }
        heap_caps_free(px);
    }
    struct stat st;
    const bool have = stat(jpg, &st) == 0 && st.st_size > 0;
    tomato_hupu_cache_unlock();
    if (!have) return NULL;   // 还没下载 / Not downloaded yet
    for (int i = 0; i < s_queue_count; i++)
        if (s_queue[i].w == w && s_queue[i].h == h && !strcmp(s_queue[i].jpg, jpg)) return NULL;
    if (s_queue_count < QUEUE_MAX && strlen(jpg) < PATH_LEN) {
        job_t* j = &s_queue[s_queue_count++];
        snprintf(j->jpg, sizeof(j->jpg), "%s", jpg);
        j->w = (uint16_t)w;
        j->h = (uint16_t)h;
    }
    return NULL;
}

static uint8_t* read_file(const char* path, size_t* len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = n > 0 && (size_t)n <= FILE_MAX ? heap_caps_malloc((size_t)n, PSRAM_CAPS) : NULL;
    bool ok = buf && fread(buf, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!ok) { heap_caps_free(buf); return NULL; }
    *len = (size_t)n;
    return buf;
}

static void write_cache(const char* jpg, int w, int h, const uint8_t* px) {
    char path[PATH_LEN + 24], tmp[PATH_LEN + 32];
    cache_path(jpg, w, h, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = fopen(tmp, "wb");
    if (!f) return;
    const uint16_t dims[2] = {(uint16_t)w, (uint16_t)h};
    bool ok = fwrite("HPG1", 1, 4, f) == 4 && fwrite(dims, sizeof(dims), 1, f) == 1 &&
              fwrite(px, 1, (size_t)w * h, f) == (size_t)w * h;
    ok = fclose(f) == 0 && ok;
    remove(path);
    if (!ok || rename(tmp, path) != 0) remove(tmp);
}

bool hupu_img_tick(void) {
    if (!s_queue_count) return false;
    job_t job = s_queue[0];
    memmove(&s_queue[0], &s_queue[1], sizeof(s_queue[0]) * (size_t)(--s_queue_count));
    const uint32_t key = key_of(job.jpg, job.w, job.h);
    size_t len = 0;
    tomato_hupu_cache_lock();
    uint8_t* data = read_file(job.jpg, &len);
    uint8_t* gray = NULL;
    uint16_t gw = 0, gh = 0;
    const bool decoded = data && book_image_decode(data, len, 648u * 1000u, &gray, &gw, &gh);
    heap_caps_free(data);
    uint8_t* px = decoded ? heap_caps_malloc((size_t)job.w * job.h, PSRAM_CAPS) : NULL;
    if (px) {
        book_image_fill(gray, gw, gh, px, job.w, job.h);
        write_cache(job.jpg, job.w, job.h, px);
        insert(key, job.w, job.h, px);
        s_built = true;
    } else if (!decoded && s_failed_count < FAILED_MAX) {
        // 解不了（比如图床给了 WEBP）就记下，免得每次重画都重试。/ Remember undecodable images so redraws do not retry.
        s_failed[s_failed_count++] = key;
        ESP_LOGW(TAG, "decode failed: %s", job.jpg);
    }
    tomato_hupu_cache_unlock();
    free(gray);
    if (s_queue_count || !s_built) return false;
    s_built = false;
    return true;
}

bool hupu_img_pending(void) { return s_queue_count > 0; }

void hupu_img_drop_pending(void) {
    s_queue_count = 0;
    s_built = false;
}

void hupu_img_release(void) {
    for (int i = 0; i < SLOT_MAX; i++) free_slot(&s_slots[i]);
    hupu_img_drop_pending();
    // 失败记录只在这次停留期间有效，下次进来再试（可能是临时缺内存）。/ Failures last for this visit only; retry next
    // time in case it was a transient shortage.
    s_failed_count = 0;
}

void hupu_img_draw(uint8_t* fb, int x, int y, int w, int h, const uint8_t* px, bool border) {
    if (!px) return;
    for (int yy = 0; yy < h; yy++) {
        const uint8_t* row = px + (size_t)yy * w;
        for (int xx = 0; xx < w; xx++) {
            uint32_t q = (row[xx] * 15u + (BAYER[yy & 3][xx & 3] * 255u + 128u) / 16u) / 255u;
            if (q > 15) q = 15;
            epd_draw_pixel(x + xx, y + yy, (uint8_t)(q * 17), fb);
        }
    }
    if (border) {
        epd_draw_rect((EpdRect){x, y, w, h}, 0, fb);
        epd_draw_rect((EpdRect){x + 1, y + 1, w - 2, h - 2}, 0, fb);
    }
}
