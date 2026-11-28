/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 锁屏书卡：画最近在读的书和进度；设了壁纸就画壁纸，底部只留一条唤醒提示和电量（不显示书名和进度）。开了今日塔罗
 * 时，壁纸上不画白条，今日塔罗和唤醒提示悬浮在壁纸下半部，每行字按底下壁纸的亮度选黑或白并加反色描边。
 * 数据（包括壁纸缓存和今天的牌）在屏幕上电前准备好，上电后只画图，缩短屏幕供电时间。
 * Lock-screen book card: the book being read and its progress; with a wallpaper set, the wallpaper plus a bottom
 * strip with only the wake hint and battery (no book or progress). With the daily tarot on, there is no strip: the
 * tarot and the wake hint float over the lower wallpaper, each line black or white by the brightness beneath it, with a
 * contrasting outline.
 * Data (including the wallpaper cache and today's card) is prepared before the panel powers on, so the rails are up
 * only for drawing.
 */

#include "tr_lock.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "book_cover.h"
#include "book_progress.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mem_reclaim.h"
#include "nvs.h"
#include "read_pico_pmu.h"
#include "settings.h"
#include "tomato.h"
#include "tr_tarot.h"
#include "tr_ui.h"
#include "ttf_font.h"
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

#define TAG "tr_lock"

// 当前壁纸的原图路径，有新鲜的全屏缓存才非空；画的时候从卡上边读边画，不占一整块 PSRAM。/ The current wallpaper's
// image path, set only with a fresh full-screen cache; drawing reads it from the card strip by strip without a whole
// PSRAM block.
static char s_wall[288];
// 今天的牌（开了今日塔罗且设了壁纸时才算）。/ Today's card (drawn only with the tarot on and a wallpaper set).
static bool s_tarot_ok;
static tr_tarot_pick_t s_tarot;
// 画壁纸时顺便算的亮度表，悬浮文字按它选黑白。/ The luminance grid taken while painting the wallpaper; floating text
// picks black or white from it.
EXT_RAM_BSS_ATTR static uint8_t s_lum[BOOK_SCREEN_LUM_H][BOOK_SCREEN_LUM_W];

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
    // 帖子大图在缓存包里，不是卡上的文件，按缓存接口读（普通文件也走这里）。/ Post images live in the cache pack, not as
    // card files, so read through the cache API (plain files go through it too).
    size_t src_len = 0;
    if (!src || !tomato_cache_size(src, &src_len) || src_len == 0 || src_len > WALL_SRC_MAX) return TR_WALL_FAILED;
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
        if ((size_t)old.st_size == src_len) {
            snprintf(out, cap, "%s", path);
            return TR_WALL_EXISTS;
        }
    }
    if (k >= 100) return TR_WALL_FAILED;
    uint8_t* buf = NULL;
    size_t got = 0;
    bool ok = tomato_cache_load(src, WALL_SRC_MAX, &buf, &got) == ESP_OK && got == src_len;
    // 先写 .part 再改名，断电不会留下半张图。/ Write .part then rename so a power cut never leaves half an image.
    char tmp[192];
    snprintf(tmp, sizeof(tmp), "%s.part", path);
    if (ok) {
        FILE* f = fopen(tmp, "wb");
        ok = f && fwrite(buf, 1, src_len, f) == src_len;
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
    s_wall[0] = 0;
    char path[sizeof(s_wall)];
    struct stat st;
    if (!tr_lock_wallpaper_get(path, sizeof(path))) return;
    if (stat(path, &st) != 0) {
        ESP_LOGW(TAG, "wallpaper %s missing", path);
        return;
    }
    // 重建要解码，内存不够就放掉能重建的缓存再试一次。/ Rebuilding decodes; when memory is short, free the rebuildable
    // caches and try once more.
    bool ok = book_screen_build(path);
    if (!ok) {
        mem_reclaim("lock wallpaper");
        ok = book_screen_build(path);
    }
    if (ok) snprintf(s_wall, sizeof(s_wall), "%s", path);
    else ESP_LOGW(TAG, "wallpaper %s: no screen cache, showing the book card", path);
    s_tarot_ok = ok && app_settings_tarot() && tr_tarot_today(&s_tarot);
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

// 底部提示按睡眠方式说怎么醒：关机要长按开机，短按只亮灯，原来一律写“按电源键”会让人以为睡死了。
// The bottom hint says how to wake for the sleep mode: power-off needs a long press and a short press only lights the
// LED, so the old one-size "press the power key" made the device look hung.
static void wake_hint(char* line, size_t cap) {
    const app_sleep_mode_t mode = app_settings_sleep_mode();
    const char* how = mode == APP_SLEEP_OFF ? "长按电源键开机"
                    : mode == APP_SLEEP_DEEP ? "按电源键开机"
                    : app_settings_pickup_wake() ? "按电源键或拿起设备继续" : "按电源键继续";
    if (s_card.battery >= 0) snprintf(line, cap, "%s · 电量 %d%%", how, s_card.battery);
    else snprintf(line, cap, "%s", how);
}

// 壁纸铺满，底部白条只写唤醒提示和电量：不写读书进度（1.2.7）也不写书名（1.2.9），都是用户要求。/ Full
// wallpaper with a white bottom strip holding only the wake hint and battery: no reading progress (1.2.7) and no book
// title (1.2.9), both by user request.
static void draw_tarot(uint8_t* fb);

static bool draw_wallpaper(uint8_t* fb) {
    if (!book_screen_draw_file(fb, s_wall, &s_lum[0][0])) {
        ESP_LOGW(TAG, "wallpaper %s: cache read failed, showing the book card", s_wall);
        return false;
    }
    if (s_tarot_ok) {
        draw_tarot(fb);
        return true;
    }
    // 白条高度和字号随显示大小放大，从屏幕底往上量。/ The strip height and text scale with the display size, measured
    // up from the screen bottom.
    const int top = TR_H - TR_S(96);
    epd_fill_rect((EpdRect){0, top, TR_W, TR_H - top}, UI_GRAY_WHITE, fb);
    tr_hline(fb, top, 0, TR_W, false);
    char line[160];
    wake_hint(line, sizeof(line));
    tr_text_dim(fb, TR_W / 2, TR_H - TR_S(66), TR_S(26), line, EPD_DRAW_ALIGN_CENTER);
    return true;
}

/* ---- 今日塔罗 / Daily tarot ---- */

// 悬浮文字的两种颜色：字色和描边色，每样东西按自己底下的壁纸选。/ The two colors of floating text, ink and outline,
// picked for each item from the wallpaper beneath it.
static uint8_t s_ink, s_halo;

// 区域偏暗用白字黑边，否则黑字白边。/ A darkish region takes white with a black outline, else black with white.
static void pick_colors(int x0, int y0, int x1, int y1) {
    const int c0 = x0 < 0 ? 0 : x0 / BOOK_SCREEN_LUM_CELL, r0 = y0 < 0 ? 0 : y0 / BOOK_SCREEN_LUM_CELL;
    const int c1 = (x1 - 1) / BOOK_SCREEN_LUM_CELL, r1 = (y1 - 1) / BOOK_SCREEN_LUM_CELL;
    uint32_t sum = 0, n = 0;
    for (int r = r0; r <= r1 && r < BOOK_SCREEN_LUM_H; r++)
        for (int c = c0; c <= c1 && c < BOOK_SCREEN_LUM_W; c++) { sum += s_lum[r][c]; n++; }
    const bool dark = n && sum / n < 128;
    s_ink = dark ? UI_GRAY_WHITE : UI_GRAY_BLACK;
    s_halo = dark ? UI_GRAY_BLACK : UI_GRAY_WHITE;
}

// 带反色描边的文字：先在八个方向用描边色画，再画字。/ Text with a contrasting outline: the outline color in eight
// directions first, then the text.
static void float_text(uint8_t* fb, int x, int y, int px, const char* text, enum EpdFontFlags align, bool bold) {
    const int w = ttf_text_width_px(px, text);
    const int x0 = align & EPD_DRAW_ALIGN_CENTER ? x - w / 2 : align & EPD_DRAW_ALIGN_RIGHT ? x - w : x;
    pick_colors(x0, y, x0 + w, y + px);
    const int r = px >= TR_S(36) ? 3 : 2;
    for (int dy = -r; dy <= r; dy += r)
        for (int dx = -r; dx <= r; dx += r)
            if (dx || dy) tr_text_on(fb, x + dx, y + dy, px, text, align, bold, s_halo, s_halo);
    tr_text_on(fb, x, y, px, text, align, bold, s_ink, s_halo);
}

// 带描边的分隔线。/ A divider with an outline.
static void float_line(uint8_t* fb, int y) {
    pick_colors(TR_MARGIN, y - 2, TR_W - TR_MARGIN, y + 4);
    epd_fill_rect((EpdRect){TR_MARGIN - 2, y - 2, TR_W - 2 * TR_MARGIN + 4, 6}, s_halo, fb);
    epd_fill_rect((EpdRect){TR_MARGIN, y, TR_W - 2 * TR_MARGIN, 2}, s_ink, fb);
}

// 自动换行到至多 max 行，返回行数；fb 为 NULL 时只数行。/ Wrap into at most max lines and return the count; with fb
// NULL only count.
static int float_wrap(uint8_t* fb, int x, int y, int w, int px, int lh, const char* text, int max) {
    char line[160];
    int n = 0;
    for (const char* p = text; *p && n < max; n++) {
        size_t take = tr_line_bytes(p, px, w);
        if (take >= sizeof(line)) take = sizeof(line) - 1;
        memcpy(line, p, take);
        line[take] = 0;
        p += take;
        while (*p == ' ' || *p == '\n') p++;
        if (fb) float_text(fb, x, y + n * lh, px, line, EPD_DRAW_ALIGN_LEFT, false);
    }
    return n;
}

// 四角星（标题前的小装饰），带描边。/ A four-point star (the heading mark), outlined.
static void float_star(uint8_t* fb, int cx, int cy, int r) {
    pick_colors(cx - r, cy - r, cx + r, cy + r);
    for (int pass = 0; pass < 2; pass++) {
        const uint8_t c = pass ? s_ink : s_halo;
        const int R = pass ? r : r + 2, w = pass ? r / 4 + 1 : r / 4 + 3;
        epd_fill_triangle(cx - w, cy, cx + w, cy, cx, cy - R, c, fb);
        epd_fill_triangle(cx - w, cy, cx + w, cy, cx, cy + R, c, fb);
        epd_fill_triangle(cx, cy - w, cx, cy + w, cx - R, cy, c, fb);
        epd_fill_triangle(cx, cy - w, cx, cy + w, cx + R, cy, c, fb);
    }
}

/* ---- 牌面 / Card face ---- */

// 逆位时牌面图形绕牌心转 180 度。/ For a reversed card the figure turns 180 degrees about the card center.
static bool s_flip;
static int s_fx, s_fy;

static void card_pt(int* x, int* y) {
    if (s_flip) { *x = 2 * s_fx - *x; *y = 2 * s_fy - *y; }
}

static void card_line(uint8_t* fb, int x0, int y0, int x1, int y1, int t) {
    card_pt(&x0, &y0);
    card_pt(&x1, &y1);
    for (int i = -t / 2; i <= t / 2; i++) {
        if (x0 == x1) epd_draw_line(x0 + i, y0, x1 + i, y1, UI_GRAY_BLACK, fb);
        else epd_draw_line(x0, y0 + i, x1, y1 + i, UI_GRAY_BLACK, fb);
    }
}

static void card_tri(uint8_t* fb, int x0, int y0, int x1, int y1, int x2, int y2) {
    card_pt(&x0, &y0);
    card_pt(&x1, &y1);
    card_pt(&x2, &y2);
    epd_fill_triangle(x0, y0, x1, y1, x2, y2, UI_GRAY_BLACK, fb);
}

static void card_ring(uint8_t* fb, int x, int y, int r) {
    card_pt(&x, &y);
    epd_draw_circle(x, y, r, UI_GRAY_BLACK, fb);
    epd_draw_circle(x, y, r - 1, UI_GRAY_BLACK, fb);
}

// 牌组图案：大阿卡那画八角星，权杖、圣杯、宝剑、星币各画一个简笔图。/ The suit figure: an eight-point star for the
// major arcana, and a simple sketch each for Wands, Cups, Swords and Pentacles.
static void card_emblem(uint8_t* fb, int cx, int cy, int s, int suit) {
    const int t = s / 12 + 1;
    switch (suit) {
    case 0:   // 权杖 / Wands
        card_line(fb, cx, cy - s, cx, cy + s, t + 1);
        card_tri(fb, cx, cy - s / 2, cx - s / 3, cy - s / 2 - s / 4, cx, cy - s / 2 - s / 8);
        card_tri(fb, cx, cy - s / 6, cx + s / 3, cy - s / 6 - s / 4, cx, cy - s / 6 - s / 8);
        card_tri(fb, cx, cy + s / 6, cx - s / 3, cy + s / 6 - s / 4, cx, cy + s / 6 - s / 8);
        break;
    case 1:   // 圣杯 / Cups
        card_tri(fb, cx - s * 2 / 3, cy - s / 2, cx + s * 2 / 3, cy - s / 2, cx, cy + s / 4);
        card_line(fb, cx, cy + s / 4, cx, cy + s * 3 / 4, t + 1);
        card_line(fb, cx - s / 3, cy + s * 3 / 4, cx + s / 3, cy + s * 3 / 4, t + 1);
        break;
    case 2:   // 宝剑 / Swords
        card_line(fb, cx, cy - s, cx, cy + s / 2, t + 1);
        card_line(fb, cx - s / 2, cy + s / 2, cx + s / 2, cy + s / 2, t + 1);
        card_line(fb, cx, cy + s / 2, cx, cy + s * 4 / 5, t + 2);
        card_tri(fb, cx - t, cy - s, cx + t, cy - s, cx, cy - s - s / 5);
        break;
    case 3: {  // 星币 / Pentacles
        card_ring(fb, cx, cy, s * 2 / 3);
        static const int P[5][2] = {{0, -100}, {95, -31}, {59, 81}, {-59, 81}, {-95, -31}};
        const int r = s / 2;
        for (int i = 0; i < 5; i++) {
            const int* a = P[i];
            const int* b = P[(i + 2) % 5];
            card_line(fb, cx + a[0] * r / 100, cy + a[1] * r / 100, cx + b[0] * r / 100, cy + b[1] * r / 100, t);
        }
        break;
    }
    default:  // 大阿卡那 / Major arcana
        for (int i = 0; i < 2; i++) {
            const int d = i ? s * 7 / 10 : s;
            const int w = s / 6 + 1;
            if (!i) {
                card_tri(fb, cx - w, cy, cx + w, cy, cx, cy - d);
                card_tri(fb, cx - w, cy, cx + w, cy, cx, cy + d);
                card_tri(fb, cx, cy - w, cx, cy + w, cx - d, cy);
                card_tri(fb, cx, cy - w, cx, cy + w, cx + d, cy);
            } else {
                card_tri(fb, cx - w, cy - w, cx + w, cy + w, cx - d, cy - d);
                card_tri(fb, cx - w, cy - w, cx + w, cy + w, cx + d, cy + d);
                card_tri(fb, cx - w, cy + w, cx + w, cy - w, cx - d, cy + d);
                card_tri(fb, cx - w, cy + w, cx + w, cy - w, cx + d, cy - d);
            }
        }
        break;
    }
}

// 牌号：大阿卡那用罗马数字，小阿卡那用 ACE、2～10、PAGE、KNIGHT、QUEEN、KING。/ The card number: Roman numerals
// for the major arcana; ACE, 2-10, PAGE, KNIGHT, QUEEN, KING for the minor.
static const char* card_number(int index) {
    static const char* const ROMAN[22] = {"0", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X", "XI",
                                          "XII", "XIII", "XIV", "XV", "XVI", "XVII", "XVIII", "XIX", "XX", "XXI"};
    static const char* const RANK[14] = {"ACE", "2", "3", "4", "5", "6", "7", "8", "9", "10",
                                         "PAGE", "KNIGHT", "QUEEN", "KING"};
    return index < 22 ? ROMAN[index] : RANK[(index - 22) % 14];
}

// 白底黑框的牌，牌号在上（逆位在下），中间是图案。/ A white card with a black frame, the number on top (at the
// bottom when reversed) and the figure in the middle.
static void draw_card(uint8_t* fb, EpdRect r) {
    pick_colors(r.x, r.y, r.x + r.width, r.y + r.height);
    ui_fill_round_rect(fb, (EpdRect){r.x - 3, r.y - 3, r.width + 6, r.height + 6}, TR_S(14), s_halo);
    ui_fill_round_rect(fb, r, TR_S(12), UI_GRAY_WHITE);
    ui_draw_round_rect(fb, r, TR_S(12), UI_GRAY_BLACK);
    ui_draw_round_rect(fb, ui_inset_rect(r, TR_S(7)), TR_S(7), UI_GRAY_BLACK);
    const int px = TR_S(20);
    const int num_y = s_tarot.reversed ? r.y + r.height - TR_S(16) - px : r.y + TR_S(16);
    tr_text_on(fb, r.x + r.width / 2, num_y, px, card_number(s_tarot.index), EPD_DRAW_ALIGN_CENTER, true,
               UI_GRAY_BLACK, UI_GRAY_WHITE);
    s_flip = s_tarot.reversed;
    s_fx = r.x + r.width / 2;
    s_fy = r.y + r.height / 2;
    const int suit = s_tarot.index < 22 ? -1 : (s_tarot.index - 22) / 14;
    card_emblem(fb, s_fx, s_fy + (s_tarot.reversed ? -TR_S(8) : TR_S(8)), r.width * 3 / 10, suit);
    s_flip = false;
}

// 今日塔罗悬浮在壁纸下半部：先排版算出总高，贴着底部的唤醒提示往上放。
// The daily tarot floats over the lower wallpaper: lay out first for the total height, then stack it up from the
// wake hint at the bottom.
static void draw_tarot(uint8_t* fb) {
    const tr_tarot_card_t* c = s_tarot.card;
    const int side = s_tarot.reversed ? 1 : 0;
    const int text_w = TR_W - 2 * TR_MARGIN;
    const int body_px = TR_S(30), body_lh = TR_S(42), label_px = TR_S(24);
    const int msg_lines = float_wrap(NULL, 0, 0, text_w, body_px, body_lh, c->message[side], 2);
    const int adv_lines = float_wrap(NULL, 0, 0, text_w, body_px, body_lh, c->advice[side], 2);
    const int card_w = TR_S(124), card_h = TR_S(200);
    const int hint_y = TR_H - TR_S(66), bottom_line = TR_H - TR_S(96);
    const int height = TR_S(72) + card_h + TR_S(34) + TR_S(36) + msg_lines * body_lh + TR_S(18) + TR_S(36) +
                       adv_lines * body_lh + TR_S(26);
    const int top = bottom_line - height;

    float_line(fb, top);
    int y = top + TR_S(26);
    float_star(fb, TR_MARGIN + TR_S(10), y + TR_S(14), TR_S(11));
    float_text(fb, TR_MARGIN + TR_S(30), y, TR_S(26), "今日塔罗", EPD_DRAW_ALIGN_LEFT, true);
    y = top + TR_S(72);

    const EpdRect card = {TR_MARGIN, y, card_w, card_h};
    draw_card(fb, card);
    const int tx = TR_MARGIN + card_w + TR_S(28), tw = TR_W - TR_MARGIN - tx;
    char line[96];
    snprintf(line, sizeof(line), "%s", c->name_en);
    for (char* p = line; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
    tr_fit(line, TR_S(36), tw);
    float_text(fb, tx, y + TR_S(30), TR_S(36), line, EPD_DRAW_ALIGN_LEFT, true);
    // 英文版里中文名就是英文名，只写正逆位。/ In the English build the name is the English one, so only the side.
    const char* pos = s_tarot.reversed ? "逆位" : "正位";
    if (strcasecmp(c->name, c->name_en)) snprintf(line, sizeof(line), "%s · %s", c->name, pos);
    else snprintf(line, sizeof(line), "%s", pos);
    tr_fit(line, TR_S(30), tw);
    float_text(fb, tx, y + TR_S(88), TR_S(30), line, EPD_DRAW_ALIGN_LEFT, false);
    snprintf(line, sizeof(line), "%s", c->keywords[side]);
    tr_fit(line, TR_S(22), tw);
    float_text(fb, tx, y + card_h - TR_S(34), TR_S(22), line, EPD_DRAW_ALIGN_LEFT, false);
    y += card_h + TR_S(34);

    float_text(fb, TR_MARGIN, y, label_px, "今日讯息", EPD_DRAW_ALIGN_LEFT, true);
    y += TR_S(36);
    float_wrap(fb, TR_MARGIN, y, text_w, body_px, body_lh, c->message[side], 2);
    y += msg_lines * body_lh + TR_S(18);
    float_text(fb, TR_MARGIN, y, label_px, "今日建议", EPD_DRAW_ALIGN_LEFT, true);
    y += TR_S(36);
    float_wrap(fb, TR_MARGIN, y, text_w, body_px, body_lh, c->advice[side], 2);

    float_line(fb, bottom_line);
    wake_hint(line, sizeof(line));
    float_text(fb, TR_W / 2, hint_y, TR_S(26), line, EPD_DRAW_ALIGN_CENTER, false);
}

bool tr_lock_draw(uint8_t* fb) {
    if (s_wall[0] && draw_wallpaper(fb)) return true;
    if (!s_card.ok) return false;
    ui_clear_page(fb);
    // Material 3 填充卡片：封面、书名、作者和进度都在浅灰底的大卡片里；锁屏整屏刷新，灰底不受 DU 影响。
    // A Material 3 filled card: cover, title, author and progress on one light gray card; the lock screen refreshes
    // as a whole page, so DU never touches the gray.
    // 文字和间距随显示大小放大，封面是缓存的图片，大小不变；卡片下面的标语放不下原位置时，挪到卡片和底部提示的正中。
    // Text and gaps scale with the display size; the cover is a cached image and keeps its size. When the motto no
    // longer fits at its usual offset, it moves halfway between the card and the bottom hint.
    tr_text_dim(fb, TR_MARGIN, TR_S(120), TR_S(30), "正在读", EPD_DRAW_ALIGN_LEFT);
    const int cover_y = TR_S(164) + TR_S(16), title_y = cover_y + BOOK_COVER_H + TR_S(50);
    const EpdRect card = {TR_MARGIN - 16, TR_S(164), TR_W - 2 * TR_MARGIN + 32, title_y + TR_S(214) - TR_S(164)};
    ui_fill_round_rect(fb, card, TR_S(28), TR_GRAY_SURFACE);
    tr_book_cover(fb, (EpdRect){TR_MARGIN, cover_y, BOOK_COVER_W, BOOK_COVER_H}, s_card.path, s_card.title,
                  s_card.author, s_card.format, false);
    char line[128];
    snprintf(line, sizeof(line), "%s", s_card.title);
    tr_fit(line, TR_S(54), TR_W - 2 * TR_MARGIN);
    tr_text_on(fb, TR_MARGIN, title_y, TR_S(54), line, EPD_DRAW_ALIGN_LEFT, true, UI_GRAY_BLACK, TR_GRAY_SURFACE);
    if (s_card.has_progress) {
        const char* sep = s_card.author[0] ? " · " : "";
        char tail[32];
        if (s_card.pct >= 100) snprintf(tail, sizeof(tail), "%s读完", sep);
        else snprintf(tail, sizeof(tail), "%s已读 %u%%", sep, s_card.pct);
        // 作者名太长时只截作者，进度一定完整显示。/ A long author name is cut; the progress always shows in full.
        char author[64];
        snprintf(author, sizeof(author), "%s", s_card.author);
        tr_fit(author, TR_S(32), TR_W - 2 * TR_MARGIN - ttf_text_width_px(TR_S(32), tail));
        snprintf(line, sizeof(line), "%s%s", author, tail);
        tr_text_on(fb, TR_MARGIN, title_y + TR_S(74), TR_S(32), line, EPD_DRAW_ALIGN_LEFT, false, TR_GRAY_SECONDARY,
                   TR_GRAY_SURFACE);
        tr_progress(fb, TR_MARGIN, title_y + TR_S(142), TR_W - 2 * TR_MARGIN, s_card.pct);
    }
    const int card_bottom = card.y + card.height, hint_y = TR_H - TR_S(96);
    int motto_y = card_bottom + TR_S(116);
    const int mid = (card_bottom + hint_y - TR_S(34)) / 2;
    if (motto_y > mid) motto_y = mid;
    tr_text(fb, TR_W / 2, motto_y, TR_S(34), "慢一点，读几页。", EPD_DRAW_ALIGN_CENTER, false);
    wake_hint(line, sizeof(line));
    tr_text_dim(fb, TR_W / 2, hint_y, TR_S(26), line, EPD_DRAW_ALIGN_CENTER);

    return true;
}
