/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 锁屏书卡：画最近在读的书和进度。只在锁屏那一刻画一次。
 * Lock-screen book card: the book being read and its progress, drawn once at lock time.
 */

#include "tr_lock.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "book_progress.h"
#include "esp_heap_caps.h"
#include "read_pico_pmu.h"
#include "tomato.h"
#include "tr_ui.h"
#include "ui_kit.h"

bool tr_lock_draw(uint8_t* fb) {
    char path[BOOK_STORE_PATH_MAX];
    struct stat st;
    if (!book_progress_last_path(path, sizeof(path)) || stat(path, &st) != 0) return false;
    book_progress_t p = {0};
    bool has = book_progress_load(path, (uint32_t)st.st_size, &p);

    char title[96], author[48] = "", format[8] = "TXT";
    tr_title_from_path(path, title, sizeof(title));
    const char* dot = strrchr(path, '.');
    if (dot && !strcmp(dot, ".epub")) snprintf(format, sizeof(format), "EPUB");
    // 番茄书用书架里的书名和作者。/ Tomato books take title and author from the shelf.
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    tomato_book_t* t = heap_caps_calloc(TOMATO_SHELF_MAX, sizeof(*t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int n = 0;
    if (t && tomato_shelf_load(t, TOMATO_SHELF_MAX, &n) == ESP_OK) {
        for (int i = 0; i < n; i++) {
            if (strcmp(t[i].file, base)) continue;
            snprintf(title, sizeof(title), "%s", t[i].title);
            snprintf(author, sizeof(author), "%s", t[i].author);
            snprintf(format, sizeof(format), "番茄");
            break;
        }
    }
    heap_caps_free(t);

    ui_clear_page(fb);
    tr_text(fb, TR_MARGIN, 120, 30, "正在读", EPD_DRAW_ALIGN_LEFT, false);
    tr_cover(fb, (EpdRect){TR_MARGIN, 180, 300, 400}, title, author, format, false);
    char line[96];
    snprintf(line, sizeof(line), "%s", title);
    tr_fit(line, 54, TR_W - 2 * TR_MARGIN);
    tr_text(fb, TR_MARGIN, 630, 54, line, EPD_DRAW_ALIGN_LEFT, true);
    if (has) {
        snprintf(line, sizeof(line), p.pct >= 100 ? "%s%s读完" : "%s%s已读 %u%%", author, author[0] ? " · " : "", p.pct);
        tr_text(fb, TR_MARGIN, 704, 32, line, EPD_DRAW_ALIGN_LEFT, false);
        tr_progress(fb, TR_MARGIN, 772, TR_W - 2 * TR_MARGIN, p.pct);
    }
    tr_text(fb, TR_W / 2, 960, 34, "慢一点，读几页。", EPD_DRAW_ALIGN_CENTER, false);
    int pct = -1;
    if (read_pico_pmu_ready()) {
        const pmu_snapshot_t* pmu = read_pico_pmu_get();
        if (pmu) pct = (pmu->soc_permille + 5) / 10;
    }
    if (pct > 100) pct = 100;
    if (pct >= 0) snprintf(line, sizeof(line), "按电源键或拿起设备继续 · 电量 %d%%", pct);
    else snprintf(line, sizeof(line), "按电源键或拿起设备继续");
    tr_text(fb, TR_W / 2, 1120, 26, line, EPD_DRAW_ALIGN_CENTER, false);
    return true;
}
