/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 锁屏书卡：画最近在读的书和进度。数据在屏幕上电前准备好，上电后只画图，缩短屏幕供电时间。
 * Lock-screen book card: the book being read and its progress. Data is prepared before the panel powers on,
 * so the rails are up only for drawing.
 */

#include "tr_lock.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "book_progress.h"
#include "esp_heap_caps.h"
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
} s_card;

void tr_lock_prepare(void) {
    memset(&s_card, 0, sizeof(s_card));
    s_card.battery = -1;
    char path[BOOK_STORE_PATH_MAX];
    struct stat st;
    if (!book_progress_last_path(path, sizeof(path)) || stat(path, &st) != 0) return;
    book_progress_t p = {0};
    s_card.has_progress = book_progress_load(path, (uint32_t)st.st_size, &p);
    s_card.pct = p.pct;
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
    if (read_pico_pmu_ready()) {
        const pmu_snapshot_t* pmu = read_pico_pmu_get();
        if (pmu) s_card.battery = (pmu->soc_permille + 5) / 10 > 100 ? 100 : (pmu->soc_permille + 5) / 10;
    }
    s_card.ok = true;
}

bool tr_lock_draw(uint8_t* fb) {
    if (!s_card.ok) return false;
    ui_clear_page(fb);
    tr_text(fb, TR_MARGIN, 120, 30, "正在读", EPD_DRAW_ALIGN_LEFT, false);
    tr_cover(fb, (EpdRect){TR_MARGIN, 180, 300, 400}, s_card.title, s_card.author, s_card.format, false);
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
