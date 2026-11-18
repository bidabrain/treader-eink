/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 锁屏书卡：画最近在读的书和进度；设了壁纸就画壁纸，底部留一条书名、进度和电量。
 * 数据（包括壁纸缓存）在屏幕上电前准备好，上电后只画图，缩短屏幕供电时间。
 * Lock-screen book card: the book being read and its progress; with a wallpaper set, the wallpaper plus a bottom
 * strip with the book, progress and battery. Data (including the wallpaper cache) is prepared before the panel
 * powers on, so the rails are up only for drawing.
 */

#include "tr_lock.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "book_cover.h"
#include "book_progress.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "read_pico_pmu.h"
#include "tomato.h"
#include "tr_ui.h"
#include "ui_kit.h"

static struct {
    bool ok;
    bool has_progress;
    uint8_t pct;
    int battery;          // -1 未知 / -1 unknown
    char title[96];
    char author[48];
    char format[8];
    char path[BOOK_STORE_PATH_MAX];
} s_card;

// 壁纸的全屏缓冲，画完就释放。/ Wallpaper buffer; freed right after drawing.
static uint8_t* s_wall;

bool tr_lock_wallpaper_get(char* out, size_t cap) {
    nvs_handle_t h;
    if (nvs_open("treader", NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = cap;
    bool ok = nvs_get_str(h, "wallpaper", out, &len) == ESP_OK && out[0];
    nvs_close(h);
    return ok;
}

void tr_lock_wallpaper_set(const char* path) {
    nvs_handle_t h;
    if (nvs_open("treader", NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t err = path && path[0] ? nvs_set_str(h, "wallpaper", path) : nvs_erase_key(h, "wallpaper");
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) nvs_commit(h);
    nvs_close(h);
}

/* ---- 存为壁纸 / Save as wallpaper ---- */

#define WALL_DIR "/sdcard/wallpapers"
#define WALL_SRC_MAX (2 * 1024 * 1024)

// 文件名里不能有的字符换成“-”，按 UTF-8 边界截到 max 字节。/ Replace characters not allowed in file names with "-"
// and cut to max bytes on a UTF-8 boundary.
static void name_part(const char* in, char* out, size_t max) {
    const char* src = in ? in : "";
    size_t n = 0;
    for (; src[n] && n + 1 < max; n++) {
        const unsigned char c = (unsigned char)src[n];
        out[n] = c < 0x20 || c == 0x7F || strchr("/\\:*?\"<>|", c) ? '-' : (char)c;
    }
    // 停在一个字符中间就退到它开头。/ If we stopped inside a character, back up to its start.
    while (n && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
    out[n] = 0;
}

tr_wall_save_t tr_lock_wallpaper_save(const char* src, const char* source, const char* author, int n, char* out,
                                      size_t cap) {
    out[0] = 0;
    struct stat st;
    if (!src || stat(src, &st) != 0 || st.st_size <= 0 || st.st_size > WALL_SRC_MAX) return TR_WALL_FAILED;
    const char* ext = strrchr(src, '.');
    if (!ext || (strcasecmp(ext, ".jpg") && strcasecmp(ext, ".jpeg") && strcasecmp(ext, ".png"))) ext = ".jpg";
    char who[64], from[24], base[112];
    name_part(author && author[0] ? author : "未知作者", who, sizeof(who));
    name_part(source, from, sizeof(from));
    snprintf(base, sizeof(base), "%s_%s_%d", from, who, n);
    mkdir(WALL_DIR, 0775);
    // 找空位：同名同大小就是存过了；同名不同大小加序号。/ Find a free name: same name and size means already saved;
    // same name but different size gets a number.
    char path[180];
    int k = 1;
    for (; k < 100; k++) {
        if (k == 1) snprintf(path, sizeof(path), WALL_DIR "/%s%s", base, ext);
        else snprintf(path, sizeof(path), WALL_DIR "/%s(%d)%s", base, k, ext);
        struct stat old;
        if (stat(path, &old) != 0) break;
        if (old.st_size == st.st_size) {
            snprintf(out, cap, "%s", path);
            return TR_WALL_EXISTS;
        }
    }
    if (k >= 100) return TR_WALL_FAILED;
    FILE* in = fopen(src, "rb");
    uint8_t* buf = in ? heap_caps_malloc((size_t)st.st_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    bool ok = buf && fread(buf, 1, (size_t)st.st_size, in) == (size_t)st.st_size;
    if (in) fclose(in);
    // 先写 .part 再改名，断电不会留下半张图。/ Write .part then rename so a power cut never leaves half an image.
    char tmp[192];
    snprintf(tmp, sizeof(tmp), "%s.part", path);
    if (ok) {
        FILE* f = fopen(tmp, "wb");
        ok = f && fwrite(buf, 1, (size_t)st.st_size, f) == (size_t)st.st_size;
        if (f) ok = fclose(f) == 0 && ok;
        ok = ok && rename(tmp, path) == 0;
        if (!ok) remove(tmp);
    }
    heap_caps_free(buf);
    if (!ok) return TR_WALL_FAILED;
    snprintf(out, cap, "%s", path);
    return TR_WALL_SAVED;
}

static void prepare_battery(void) {
    if (read_pico_pmu_ready()) {
        const pmu_snapshot_t* pmu = read_pico_pmu_get();
        if (pmu) s_card.battery = (pmu->soc_permille + 5) / 10 > 100 ? 100 : (pmu->soc_permille + 5) / 10;
    }
}

// 壁纸缓存丢了（比如换了卡）就在这里重建，此时屏幕还没上电。/ Rebuild a missing cache here, before the panel powers on.
static void prepare_wallpaper(void) {
    heap_caps_free(s_wall);
    s_wall = NULL;
    char path[288];
    struct stat st;
    if (!tr_lock_wallpaper_get(path, sizeof(path)) || stat(path, &st) != 0) return;
    s_wall = book_screen_load(path);
    if (!s_wall && book_screen_build(path)) s_wall = book_screen_load(path);
}

void tr_lock_prepare(void) {
    memset(&s_card, 0, sizeof(s_card));
    s_card.battery = -1;
    prepare_wallpaper();
    prepare_battery();
    char path[BOOK_STORE_PATH_MAX];
    struct stat st;
    if (!book_progress_last_path(path, sizeof(path)) || stat(path, &st) != 0) return;
    book_progress_t p = {0};
    s_card.has_progress = book_progress_load(path, (uint32_t)st.st_size, &p);
    s_card.pct = p.pct;
    snprintf(s_card.path, sizeof(s_card.path), "%s", path);
    // 封面缓存在上电前读进内存；没生成过就用文字封面，锁屏不解码。
    // Load the cached cover before power-up; if none was built, use the text cover. The lock never decodes.
    book_cover_load(path);
    book_cover_drop_pending();
    tr_title_from_path(path, s_card.title, sizeof(s_card.title));
    const char* dot = strrchr(path, '.');
    snprintf(s_card.format, sizeof(s_card.format), "%s", dot && !strcasecmp(dot, ".epub") ? "EPUB" : "TXT");
    // 番茄书用书架里的书名和作者。/ Tomato books take title and author from the shelf.
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    tomato_book_t* t = heap_caps_calloc(TOMATO_SHELF_MAX, sizeof(*t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int n = 0;
    if (t && tomato_shelf_load(t, TOMATO_SHELF_MAX, &n) == ESP_OK) {
        for (int i = 0; i < n; i++) {
            if (strcmp(t[i].file, base)) continue;
            snprintf(s_card.title, sizeof(s_card.title), "%s", t[i].title);
            snprintf(s_card.author, sizeof(s_card.author), "%s", t[i].author);
            snprintf(s_card.format, sizeof(s_card.format), "番茄");
            break;
        }
    }
    heap_caps_free(t);
    s_card.ok = true;
}

// 壁纸铺满，底部白条写在读的书和电量。/ Full wallpaper with a white bottom strip for the book and battery.
static void draw_wallpaper(uint8_t* fb) {
    book_screen_draw(fb, s_wall);
    heap_caps_free(s_wall);
    s_wall = NULL;
    const int top = s_card.ok ? 1056 : 1120;
    epd_fill_rect((EpdRect){0, top, TR_W, TR_H - top}, UI_GRAY_WHITE, fb);
    tr_hline(fb, top, 0, TR_W, false);
    char line[160];
    if (s_card.ok) {
        snprintf(line, sizeof(line), "%s", s_card.title);
        tr_fit(line, 32, TR_W - 2 * TR_MARGIN - 150);
        tr_text(fb, TR_MARGIN, top + 20, 32, line, EPD_DRAW_ALIGN_LEFT, true);
        if (s_card.has_progress) {
            if (s_card.pct >= 100) snprintf(line, sizeof(line), "读完");
            else snprintf(line, sizeof(line), "已读 %u%%", s_card.pct);
            tr_text(fb, TR_W - TR_MARGIN, top + 22, 28, line, EPD_DRAW_ALIGN_RIGHT, false);
        }
    }
    if (s_card.battery >= 0) snprintf(line, sizeof(line), "按电源键或拿起设备继续 · 电量 %d%%", s_card.battery);
    else snprintf(line, sizeof(line), "按电源键或拿起设备继续");
    tr_text(fb, TR_W / 2, TR_H - 66, 26, line, EPD_DRAW_ALIGN_CENTER, false);
}

bool tr_lock_draw(uint8_t* fb) {
    if (s_wall) { draw_wallpaper(fb); return true; }
    if (!s_card.ok) return false;
    ui_clear_page(fb);
    tr_text(fb, TR_MARGIN, 120, 30, "正在读", EPD_DRAW_ALIGN_LEFT, false);
    tr_book_cover(fb, (EpdRect){TR_MARGIN, 180, BOOK_COVER_W, BOOK_COVER_H}, s_card.path, s_card.title, s_card.author,
                  s_card.format, false);
    char line[128];
    snprintf(line, sizeof(line), "%s", s_card.title);
    tr_fit(line, 54, TR_W - 2 * TR_MARGIN);
    tr_text(fb, TR_MARGIN, 630, 54, line, EPD_DRAW_ALIGN_LEFT, true);
    if (s_card.has_progress) {
        const char* sep = s_card.author[0] ? " · " : "";
        if (s_card.pct >= 100) snprintf(line, sizeof(line), "%s%s读完", s_card.author, sep);
        else snprintf(line, sizeof(line), "%s%s已读 %u%%", s_card.author, sep, s_card.pct);
        tr_text(fb, TR_MARGIN, 704, 32, line, EPD_DRAW_ALIGN_LEFT, false);
        tr_progress(fb, TR_MARGIN, 772, TR_W - 2 * TR_MARGIN, s_card.pct);
    }
    tr_text(fb, TR_W / 2, 960, 34, "慢一点，读几页。", EPD_DRAW_ALIGN_CENTER, false);
    if (s_card.battery >= 0) snprintf(line, sizeof(line), "按电源键或拿起设备继续 · 电量 %d%%", s_card.battery);
    else snprintf(line, sizeof(line), "按电源键或拿起设备继续");
    tr_text(fb, TR_W / 2, 1120, 26, line, EPD_DRAW_ALIGN_CENTER, false);
    return true;
}
