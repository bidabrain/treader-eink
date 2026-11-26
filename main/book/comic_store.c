/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 漫画书架数据：扫描、自然排序、进度表（文本文件，一行一条）、删除。
 *
 * Comics shelf data: scanning, natural order, a progress table (a text file, one record per line), deletion.
 *
 * 冻结：只认 CBZ，只扫一层文件夹，不碰 comics/ 以外的文件。
 * Frozen: CBZ only, one folder level, never touch files outside comics/.
 */
#include "comic_store.h"
#include "tomato.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#define TAG "comic_store"
#define PROGRESS_FILE COMIC_META_DIR "/progress.txt"
#define RECORD_MAX 2000
#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

/* ---- 自然排序 / Natural order ---- */

int comic_name_cmp(const char* a, const char* b) {
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            // 跳过前导零后先比位数再逐位比。/ Skip leading zeros, compare length, then digits.
            while (*a == '0' && isdigit((unsigned char)a[1])) a++;
            while (*b == '0' && isdigit((unsigned char)b[1])) b++;
            size_t na = 0, nb = 0;
            while (isdigit((unsigned char)a[na])) na++;
            while (isdigit((unsigned char)b[nb])) nb++;
            if (na != nb) return na < nb ? -1 : 1;
            int c = strncmp(a, b, na);
            if (c) return c;
            a += na;
            b += nb;
            continue;
        }
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb) return ca < cb ? -1 : 1;
        a++;
        b++;
    }
    return *a ? 1 : *b ? -1 : 0;
}

static bool is_cbz(const char* name) {
    const size_t n = strlen(name);
    return n > 4 && name[0] != '.' && !strcasecmp(name + n - 4, ".cbz");
}

/* ---- 进度表 / Progress table ---- */

typedef struct {
    char path[COMIC_PATH_MAX];
    uint16_t page, pages;
    uint32_t stamp;
    uint8_t flags;   // bit0 看完，bit1 方向记录，bit2 从右往左 / bit0 done, bit1 direction record, bit2 RTL
} record_t;

#define F_DONE 1
#define F_DIR 2
#define F_RTL 4

static record_t* s_rec;
static int s_rec_count;
static uint32_t s_stamp;
static bool s_loaded, s_dirty;

static void load(void) {
    if (s_loaded) return;
    if (!s_rec) s_rec = heap_caps_calloc(RECORD_MAX, sizeof(record_t), PSRAM_CAPS);
    if (!s_rec) return;
    s_rec_count = 0;
    s_stamp = 0;
    FILE* f = fopen(PROGRESS_FILE, "r");
    if (!f) {
        // 上次写到一半断电：主文件已删、临时文件还在，就用临时文件。/ Power cut mid-flush: the main file is gone but
        // the temporary one survived, so use it.
        if (rename(PROGRESS_FILE ".tmp", PROGRESS_FILE) == 0) f = fopen(PROGRESS_FILE, "r");
    }
    if (!f) {
        // 只有卡在、comics/ 在、文件确实不存在时才当成空表；没卡时下次再读，免得空表盖掉卡上的进度。
        // Treat it as empty only when the card and comics/ exist and the file is truly absent; without a card retry
        // later so an empty table never overwrites the progress on the card.
        struct stat st;
        s_loaded = errno == ENOENT && stat(COMIC_ROOT, &st) == 0 && S_ISDIR(st.st_mode);
        return;
    }
    s_loaded = true;
    char line[COMIC_PATH_MAX + 64];
    while (s_rec_count < RECORD_MAX && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        unsigned page = 0, pages = 0, stamp = 0, flags = 0;
        int used = 0;
        if (sscanf(line, "%u\t%u\t%u\t%u\t%n", &page, &pages, &stamp, &flags, &used) != 4 || !used || !line[used]) continue;
        record_t* r = &s_rec[s_rec_count++];
        snprintf(r->path, sizeof(r->path), "%s", line + used);
        r->page = (uint16_t)page;
        r->pages = (uint16_t)pages;
        r->stamp = stamp;
        r->flags = (uint8_t)flags;
        if (stamp > s_stamp) s_stamp = stamp;
    }
    fclose(f);
}

static record_t* find(const char* path, bool create) {
    load();
    if (!s_rec || (create && !s_loaded)) return NULL;
    for (int i = 0; i < s_rec_count; i++)
        if (!strcmp(s_rec[i].path, path)) return &s_rec[i];
    if (!create) return NULL;
    if (s_rec_count >= RECORD_MAX) {
        // 满了就挤掉最久没看的一条。/ When full, evict the least recently read record.
        int old = 0;
        for (int i = 1; i < s_rec_count; i++)
            if (s_rec[i].stamp < s_rec[old].stamp) old = i;
        s_rec[old] = s_rec[--s_rec_count];
    }
    record_t* r = &s_rec[s_rec_count++];
    memset(r, 0, sizeof(*r));
    snprintf(r->path, sizeof(r->path), "%s", path);
    return r;
}

bool comic_progress_get(const char* path, comic_progress_t* out) {
    const record_t* r = find(path, false);
    if (!r || !(r->stamp || (r->flags & F_DONE))) return false;
    if (out) *out = (comic_progress_t){.page = r->page, .pages = r->pages, .stamp = r->stamp, .done = r->flags & F_DONE};
    return true;
}

void comic_progress_set(const char* path, const comic_progress_t* p) {
    record_t* r = find(path, true);
    if (!r) return;
    r->page = p->page;
    r->pages = p->pages;
    r->stamp = p->stamp ? p->stamp : ++s_stamp;
    if (r->stamp > s_stamp) s_stamp = r->stamp;
    r->flags = (uint8_t)((r->flags & ~F_DONE) | (p->done ? F_DONE : 0));
    s_dirty = true;
}

void comic_progress_forget(const char* path) {
    load();
    const size_t n = strlen(path);
    for (int i = 0; i < s_rec_count;) {
        const char* p = s_rec[i].path;
        if (!strncmp(p, "#dir:", 5)) p += 5;   // 方向记录也一起清 / Direction records go too
        if (!strncmp(p, path, n) && (p[n] == 0 || p[n] == '/')) {
            s_rec[i] = s_rec[--s_rec_count];
            s_dirty = true;
        } else {
            i++;
        }
    }
}

bool comic_progress_last(char* path, size_t cap) {
    load();
    const record_t* best = NULL;
    for (int i = 0; i < s_rec_count; i++)
        if (!(s_rec[i].flags & F_DIR) && s_rec[i].stamp && (!best || s_rec[i].stamp > best->stamp)) best = &s_rec[i];
    if (!best) return false;
    struct stat st;
    if (stat(best->path, &st) != 0) return false;
    snprintf(path, cap, "%s", best->path);
    return true;
}

// 方向记录和进度共用一张表，用 “#dir:” 前缀区分。/ Direction records share the table under a "#dir:" prefix.
static void direction_key(const char* key, char* out, size_t cap) { snprintf(out, cap, "#dir:%s", key); }

int comic_direction_get(const char* key) {
    char k[COMIC_PATH_MAX];
    direction_key(key, k, sizeof(k));
    const record_t* r = find(k, false);
    return r && (r->flags & F_DIR) ? (r->flags & F_RTL ? 1 : 0) : -1;
}

void comic_direction_set(const char* key, bool rtl) {
    char k[COMIC_PATH_MAX];
    direction_key(key, k, sizeof(k));
    record_t* r = find(k, true);
    if (!r) return;
    r->flags = (uint8_t)(F_DIR | (rtl ? F_RTL : 0));
    s_dirty = true;
}

esp_err_t comic_progress_flush(void) {
    if (!s_dirty || !s_rec || !s_loaded) return ESP_OK;
    mkdir(COMIC_ROOT, 0775);
    mkdir(COMIC_META_DIR, 0775);
    FILE* f = fopen(PROGRESS_FILE ".tmp", "w");
    if (!f) return ESP_FAIL;
    bool ok = true;
    for (int i = 0; i < s_rec_count && ok; i++) {
        const record_t* r = &s_rec[i];
        ok = fprintf(f, "%u\t%u\t%u\t%u\t%s\n", r->page, r->pages, (unsigned)r->stamp, r->flags, r->path) > 0;
    }
    ok = fclose(f) == 0 && ok;
    if (ok) {
        remove(PROGRESS_FILE);
        ok = rename(PROGRESS_FILE ".tmp", PROGRESS_FILE) == 0;
    }
    if (!ok) {
        remove(PROGRESS_FILE ".tmp");
        ESP_LOGW(TAG, "progress write failed");
        return ESP_FAIL;
    }
    s_dirty = false;
    return ESP_OK;
}

void comic_progress_drop(void) {
    s_loaded = false;
    s_dirty = false;
    s_rec_count = 0;
}

void comic_progress_release(void) {
    // 还有没写进卡的改动就先留着，不丢进度。/ Keep it while changes are not on the card yet, so no progress is lost.
    if (s_dirty) return;
    heap_caps_free(s_rec);
    s_rec = NULL;
    s_loaded = false;
    s_rec_count = 0;
}

/* ---- 扫描 / Scanning ---- */

int comic_store_volumes(const char* series_path, char (*names)[COMIC_NAME_MAX], int max) {
    DIR* dir = opendir(series_path);
    if (!dir) return 0;
    int n = 0;
    struct dirent* e;
    while ((e = readdir(dir)) && n < max) {
        if (!is_cbz(e->d_name) || strlen(e->d_name) >= COMIC_NAME_MAX) continue;
        snprintf(names[n++], COMIC_NAME_MAX, "%s", e->d_name);
    }
    closedir(dir);
    // 话数不多，插入排序就够了。/ Episode counts are small; insertion sort is enough.
    for (int i = 1; i < n; i++) {
        char key[COMIC_NAME_MAX];
        memcpy(key, names[i], COMIC_NAME_MAX);
        int j = i - 1;
        while (j >= 0 && comic_name_cmp(names[j], key) > 0) {
            memcpy(names[j + 1], names[j], COMIC_NAME_MAX);
            j--;
        }
        memcpy(names[j + 1], key, COMIC_NAME_MAX);
    }
    return n;
}

// 统计系列：话数、看完几话、最近顺序号。/ Tally a series: episodes, finished episodes, latest counter.
static void tally_series(comic_entry_t* e) {
    DIR* dir = opendir(e->path);
    if (!dir) return;
    struct dirent* d;
    char path[COMIC_PATH_MAX + COMIC_NAME_MAX];
    while ((d = readdir(dir))) {
        if (!is_cbz(d->d_name)) continue;
        e->volumes++;
        snprintf(path, sizeof(path), "%s/%s", e->path, d->d_name);
        comic_progress_t p;
        if (!comic_progress_get(path, &p)) continue;
        if (p.done) e->done++;
        if (p.stamp > e->stamp) e->stamp = p.stamp;
    }
    closedir(dir);
}

int comic_store_scan(comic_entry_t* out, int max) {
    DIR* dir = opendir(COMIC_ROOT);
    if (!dir) return 0;
    load();
    int n = 0;
    struct dirent* d;
    while ((d = readdir(dir)) && n < max) {
        if (d->d_name[0] == '.' || strlen(d->d_name) >= COMIC_NAME_MAX) continue;
        comic_entry_t* e = &out[n];
        memset(e, 0, sizeof(*e));
        if (snprintf(e->path, sizeof(e->path), "%s/%s", COMIC_ROOT, d->d_name) >= (int)sizeof(e->path)) continue;
        struct stat st;
        if (stat(e->path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            e->series = true;
            snprintf(e->name, sizeof(e->name), "%s", d->d_name);
            tally_series(e);
            tomato_wt_mark_t m;
            if (tomato_wt_mark_read(e->path, &m)) {
                e->webtoon = true;
                e->fresh = (uint16_t)(m.fresh > 0 ? m.fresh : 0);
                snprintf(e->wt_id, sizeof(e->wt_id), "%s", m.id);
            }
            if (!e->volumes && !e->webtoon) continue;
        } else if (is_cbz(d->d_name)) {
            snprintf(e->name, sizeof(e->name), "%.*s", (int)strlen(d->d_name) - 4, d->d_name);
            e->volumes = 1;
            comic_progress_t p;
            if (comic_progress_get(e->path, &p)) {
                e->stamp = p.stamp;
                e->done = p.done;
                e->pct = p.done ? 100 : p.pages ? (uint8_t)((p.page + 1) * 100 / p.pages) : 0;
            }
        } else {
            continue;
        }
        n++;
    }
    closedir(dir);
    return n;
}

/* ---- 删除 / Deletion ---- */

esp_err_t comic_store_delete(const char* path) {
    // 只允许 comics/ 下一层或两层的路径。/ Only paths one or two levels under comics/.
    const size_t root = strlen(COMIC_ROOT);
    if (strncmp(path, COMIC_ROOT "/", root + 1) || strstr(path, "..")) return ESP_ERR_INVALID_ARG;
    struct stat st;
    if (stat(path, &st) != 0) return ESP_ERR_NOT_FOUND;
    esp_err_t err = ESP_OK;
    if (S_ISDIR(st.st_mode)) {
        DIR* dir = opendir(path);
        if (!dir) return ESP_FAIL;
        struct dirent* d;
        char file[COMIC_PATH_MAX + COMIC_NAME_MAX];
        while ((d = readdir(dir))) {
            if (d->d_name[0] == '.' && (!d->d_name[1] || (d->d_name[1] == '.' && !d->d_name[2]))) continue;
            snprintf(file, sizeof(file), "%s/%s", path, d->d_name);
            struct stat fs;
            // 只删普通文件（CBZ 和上传残留），不递归。/ Remove plain files only (CBZ and upload leftovers); no recursion.
            if (stat(file, &fs) == 0 && S_ISREG(fs.st_mode) && remove(file) != 0) err = ESP_FAIL;
        }
        closedir(dir);
        if (err == ESP_OK && rmdir(path) != 0) err = ESP_FAIL;
    } else if (remove(path) != 0) {
        err = ESP_FAIL;
    }
    comic_progress_forget(path);
    comic_progress_flush();
    return err;
}
