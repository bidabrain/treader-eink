/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：书库页（书架、管理、弹窗、搜索、阅读工具条、排版面板、目录）的渲染测试。
 * English: Render bench for the library page (shelf, manage mode, dialogs, search, reader toolbar, layout panel, TOC).
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "app_book.c"   // 按 -I 找：当前源码或 SRC_ROOT / Found via -I: current sources or SRC_ROOT
#include "page_render.h"
#include "nvs.h"

// 书库的列表 / 网格设置要写 NVS；这里没有存储，打开失败就只改内存里的值。/ The list / grid setting writes NVS; there is
// no storage here, so a failed open keeps the value in memory only.
esp_err_t nvs_open(const char* ns, nvs_open_mode_t m, nvs_handle_t* h) { (void)ns; (void)m; (void)h; return ESP_FAIL; }

const char* hr_page = "book";

/* ---- 书和排版的替身 / Book and layout stand-ins ---- */

static size_t s_hr_chapters = 1;
size_t book_chapter_count(void) { return s_hr_chapters; }
esp_err_t book_chapter_title(size_t i, char* out, size_t cap) {
    if (i % 3 == 2)
        snprintf(out, cap, "第%u章 一个非常非常长的章节标题，用来看目录里的截断效果如何", (unsigned)i + 1);
    else snprintf(out, cap, "第%u章 归来", (unsigned)i + 1);
    return ESP_OK;
}
uint32_t book_total_bytes(void) { return 1000000; }
uint32_t book_chapter_byte_offset(size_t i) { return (uint32_t)(i * 1000000 / (s_hr_chapters ? s_hr_chapters : 1)); }
bool book_layout_complete(void) { return true; }
size_t book_layout_page_count(void) { return 12; }
size_t book_layout_page_start_offset(size_t page) { return page * 100; }
// 正文画成浅灰块：正文不随显示大小变。/ The body is a light gray block: it does not follow the display size.
void book_layout_draw_page(uint8_t* fb, size_t page, EpdRect rect, int px) {
    (void)page; (void)px;
    epd_fill_rect(rect, 0xE0, fb);
}
// 键盘画成浅灰块（键盘自己有测试）。/ The keyboard is a light gray block (it has its own bench).
void tr_kbd_draw(uint8_t* fb, const tr_kbd_t* kbd) {
    (void)kbd;
    epd_fill_rect((EpdRect){0, TR_KBD_TOP, TR_W, TR_H - TR_KBD_TOP}, 0xE0, fb);
}

/* ---- 书架数据 / Shelf data ---- */

static shelf_entry_t s_hr_books[9];
static void hr_book(int i, const char* name, const char* title, const char* author, bool tomato, uint8_t pct,
                    bool progress, uint16_t new_ch, uint16_t fresh, bool removed, bool selected) {
    shelf_entry_t* e = &s_hr_books[i];
    memset(e, 0, sizeof(*e));
    snprintf(e->name, sizeof(e->name), "%s", name);
    snprintf(e->path, sizeof(e->path), "/sdcard/books/%s", name);
    snprintf(e->title, sizeof(e->title), "%s", title);
    snprintf(e->author, sizeof(e->author), "%s", author);
    e->tomato = tomato;
    e->pct = pct;
    e->has_progress = progress;
    e->new_chapters = new_ch;
    e->fresh_chapters = fresh;
    e->removed = removed;
    e->selected = selected;
    e->size = 1234567;
    e->search_match = true;
}
static void hr_fill_shelf(int n) {
    hr_book(0, "三体.txt", "", "", false, 42, true, 0, 0, false, true);
    hr_book(1, "x.txt", "诡秘之主：一个特别特别长的书名用来测试截断和徽章的位置", "爱潜水的乌贼", true, 17, true, 12, 0,
            false, false);
    hr_book(2, "y.txt", "大奉打更人", "卖报小郎君", true, 100, true, 0, 3, false, true);
    hr_book(3, "百年孤独.epub", "", "", false, 0, false, 0, 0, false, false);
    hr_book(4, "已删除的书.txt", "", "", false, 30, true, 0, 0, true, false);
    hr_book(5, "活着.txt", "", "", false, 55, true, 0, 0, false, false);
    hr_book(6, "围城.epub", "", "", false, 0, false, 0, 0, false, false);
    hr_book(7, "红楼梦.txt", "", "", false, 88, true, 0, 0, false, false);
    hr_book(8, "平凡的世界.txt", "", "", false, 9, true, 0, 0, false, false);
    s_shelf = s_hr_books;
    s_count = s_visible_count = n;
}
static void hr_reset(void) {
    s_view = SHELF;
    s_query[0] = s_message[0] = s_shelf_warning[0] = s_batch_message[0] = s_manage_message[0] = 0;
    s_resume_path[0] = s_continue[0] = 0;
    s_save_failed = s_batch_confirm = s_clear_confirm = s_delete_confirm = s_file_removed = false;
    s_toolbar = s_typeset = s_image_open = false;
    s_filter = 0;
    s_recent_sort = false;
    s_pressed_control = -1;
    s_text = NULL;
    hr_fill_shelf(9);
}

void hr_scenes(void) {
    app_ctx_t ctx = {.fb = hr_fb};

    // 书架：继续阅读卡片、番茄更新提示、各种标记。/ Shelf: continue card, Tomato hint, every kind of mark.
    hr_reset();
    snprintf(s_continue, sizeof(s_continue), "%s", s_hr_books[1].path);
    ctx.leaf = 0;
    render(&ctx, hr_fb);
    hr_allow_fit();
    hr_shot("shelf_card");
    ctx.leaf = 1;
    render(&ctx, hr_fb);
    hr_allow_fit();
    hr_shot("shelf_card_page2");

    // 书架：搜索中（没有卡片），按最近。/ Shelf while searching (no card), sorted by recent.
    hr_reset();
    snprintf(s_query, sizeof(s_query), "santi");
    s_recent_sort = true;
    s_filter = 2;
    ctx.leaf = 0;
    render(&ctx, hr_fb);
    hr_allow_fit();
    hr_shot("shelf_search");

    // 书架：进度未保存，按下第一行。/ Shelf: progress save failed, first row pressed.
    hr_reset();
    s_save_failed = true;
    s_pressed_control = 0;
    render(&ctx, hr_fb);
    hr_allow_fit();
    hr_shot("shelf_save_failed");

    // 网格：有卡片、搜索中（没有卡片）、按下一格、管理里选中。/ Grid: with the card, while searching (no card), a
    // pressed cell, and selections in manage mode.
    hr_reset();
    app_settings_set_shelf_grid(true);
    snprintf(s_continue, sizeof(s_continue), "%s", s_hr_books[1].path);
    ctx.leaf = 0;
    render(&ctx, hr_fb);
    hr_allow_fit();
    hr_shot("grid_card");
    hr_reset();
    snprintf(s_query, sizeof(s_query), "santi");
    s_pressed_control = 1;
    render(&ctx, hr_fb);
    hr_allow_fit();
    hr_shot("grid_search");
    hr_reset();
    s_view = BULK;
    s_shelf[0].selected = s_shelf[2].selected = true;
    render(&ctx, hr_fb);
    hr_allow_fit();
    hr_no_bar_check();
    hr_shot("grid_bulk");
    s_shelf[0].selected = s_shelf[2].selected = false;
    app_settings_set_shelf_grid(false);

    // 空书架。/ Empty shelf.
    hr_reset();
    s_count = s_visible_count = 0;
    snprintf(s_message, sizeof(s_message), "暂无图书，请传入 TXT 或 EPUB");
    render(&ctx, hr_fb);
    hr_shot("shelf_empty");
    // 有书但筛选没结果。/ Books exist but the filter matches none.
    hr_reset();
    s_visible_count = 0;
    s_filter = 1;
    render(&ctx, hr_fb);
    hr_shot("shelf_filter_none");

    // 管理书库（批量）。/ Bulk manage mode.
    hr_reset();
    s_view = BULK;
    ctx.leaf = 0;
    render(&ctx, hr_fb);
    hr_allow_fit();
    hr_no_bar_check();
    hr_shot("bulk");
    s_batch_confirm = true;
    s_batch_delete = true;
    render(&ctx, hr_fb);
    hr_allow_fit();
    hr_no_bar_check();
    hr_shot("bulk_confirm");
    s_batch_confirm = false;
    snprintf(s_batch_message, sizeof(s_batch_message), "成功 3 本，失败 1 本；所选可重试");
    render(&ctx, hr_fb);
    hr_allow_fit();
    hr_no_bar_check();
    hr_shot("bulk_message");

    // 单本详情弹窗。/ Single-book detail dialog.
    hr_reset();
    s_view = MANAGE;
    s_managed = s_hr_books[1];
    snprintf(s_managed.name, sizeof(s_managed.name),
             "诡秘之主：一个特别特别长的文件名用来测试换行，看看三行四行的时候弹窗怎么排（番茄下载版本）.txt");
    render(&ctx, hr_fb);
    hr_allow_fit();
    hr_shot("manage");
    s_clear_confirm = s_delete_confirm = true;
    snprintf(s_manage_message, sizeof(s_manage_message), "文件删除失败，请检查存储后重试");
    render(&ctx, hr_fb);
    hr_allow_fit();
    hr_shot("manage_confirm");
    s_clear_confirm = false;
    s_file_removed = true;
    snprintf(s_managed.name, sizeof(s_managed.name), "短.txt");
    render(&ctx, hr_fb);
    hr_allow_fit();
    hr_shot("manage_removed");

    // 续读弹窗。/ Resume dialog.
    hr_reset();
    snprintf(s_resume_path, sizeof(s_resume_path), "/sdcard/books/三体.txt");
    snprintf(s_resume_name, sizeof(s_resume_name), "三体.txt");
    render(&ctx, hr_fb);
    hr_allow_fit();
    hr_shot("resume");

    // 搜索页。/ Search page.
    hr_reset();
    s_view = SEARCH;
    snprintf(s_search_kbd.text, sizeof(s_search_kbd.text), "guimizhizhu");
    render(&ctx, hr_fb);
    hr_no_bar_check();
    hr_shot("search");

    // 阅读页：正文、工具条、排版面板。/ Reader: body, toolbar, layout panel.
    hr_reset();
    static char text[] = "x";
    s_text = text;
    s_text_len = 1;
    s_view = READING;
    s_hr_chapters = 30;
    s_chapter = 5;
    s_page = 3;
    s_px = 48;
    snprintf(s_title, sizeof(s_title), "第6章 一个非常长的章节名字，看看页脚怎么截断");
    render(&ctx, hr_fb);
    hr_no_bar_check();
    hr_shot("reader");
    s_toolbar = true;
    s_auto_level = 3;
    render(&ctx, hr_fb);
    hr_allow_fit();
    hr_no_bar_check();
    hr_shot("reader_toolbar");
    s_pressed_control = 204;
    render(&ctx, hr_fb);
    hr_allow_fit();
    hr_no_bar_check();
    hr_shot("reader_toolbar_pressed");
#ifndef HR_BASELINE   // 改动前没有上一章 / 下一章 / No previous / next chapter before this change
    s_pressed_control = 210;
    render(&ctx, hr_fb);
    hr_allow_fit();
    hr_no_bar_check();
    hr_shot("reader_toolbar_next_pressed");
    s_pressed_control = -1;
    s_chapter = 0;
    render(&ctx, hr_fb);
    hr_allow_fit();
    hr_no_bar_check();
    hr_shot("reader_toolbar_first");
    s_chapter = 29;
    s_save_failed = true;
    render(&ctx, hr_fb);
    hr_allow_fit();
    hr_no_bar_check();
    hr_shot("reader_toolbar_last_unsaved");
    s_save_failed = false;
    s_chapter = 5;
#endif
    s_pressed_control = -1;
    s_typeset = true;
    render(&ctx, hr_fb);
    hr_no_bar_check();
    hr_shot("reader_typeset");
    s_typeset = false;
    s_toolbar = false;
    s_save_failed = true;
    render(&ctx, hr_fb);
    hr_no_bar_check();
    hr_shot("reader_save_failed");
    s_save_failed = false;

    // 目录。/ TOC.
    s_view = TOC;
    ctx.leaf = 0;
    render(&ctx, hr_fb);
    hr_allow_fit();
    hr_no_bar_check();
    hr_shot("toc");
    ctx.leaf = 1;
    s_chapter = 12;
    render(&ctx, hr_fb);
    hr_allow_fit();
    hr_no_bar_check();
    hr_shot("toc_page2");

    // 图片预览（没加载成功）。/ Image preview (load failed).
    s_view = READING;
    s_image_open = true;
    s_image_error = "图片超出大小限制或文件损坏";
    snprintf(s_image_origin, sizeof(s_image_origin), "已读最早：第 3 节 · 第3章 一个非常非常长的章节标题");
    render(&ctx, hr_fb);
    hr_allow_fit();
    hr_no_bar_check();
    hr_shot("image_error");
    s_image_open = false;
    s_image_error = NULL;
    s_image_origin[0] = 0;
    s_text = NULL;
    s_hr_chapters = 1;
    ctx.leaf = 0;
}
