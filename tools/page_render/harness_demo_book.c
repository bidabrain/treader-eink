/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：书库页的演示图（README / 网站预览用）：书架、阅读页（真实排版）、阅读工具条、特大显示的书架。
 * English: Demo shots of the library page (README / site): shelf, reader (real layout), reader toolbar, and the shelf
 * at the largest display size.
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "app_book.c"   // 按 -I 找 / Found via -I
#include "page_render.h"

const char* hr_page = "demo_book";

/* ---- 书的替身：一本一千多章的长篇 / Book stand-ins: a long novel of over a thousand chapters ---- */

#define HR_CHAPTERS 1180
#define HR_TOTAL 9600000u
size_t book_chapter_count(void) { return HR_CHAPTERS; }
esp_err_t book_chapter_title(size_t i, char* out, size_t cap) {
    (void)i;
    snprintf(out, cap, "第四百九十一章 雨夜来信");
    return ESP_OK;
}
uint32_t book_total_bytes(void) { return HR_TOTAL; }
uint32_t book_chapter_byte_offset(size_t i) { return (uint32_t)((uint64_t)i * HR_TOTAL / HR_CHAPTERS); }

// 正文：一段原创的小说文字，由真实排版代码分页绘制。/ Body: original novel prose paginated and drawn by the real
// layout code.
static char s_hr_text[] =
    "　　雨是从傍晚开始下的。\n"
    "　　林舟撑着一把旧伞，站在钟楼对面的屋檐下，看着街灯一盏接一盏亮起来。石板路被雨水洗得发亮，偶尔有马车驶过，"
    "车轮碾开积水，溅起一片细碎的光。\n"
    "　　他已经在这里等了快一个小时。\n"
    "　　按照信里的说法，那位自称“引路人”的客人会在钟声敲响第七下的时候出现。可现在钟声早已过去，街上除了几个匆匆"
    "赶路的行人，什么也没有。\n"
    "　　“也许对方根本不会来。”他低声对自己说，指尖却仍然紧紧捏着那封没有署名的信。\n"
    "　　信纸很薄，边缘有被火燎过的痕迹，上面只有短短三行字：\n"
    "　　“你丢失的那一天，我知道在哪里。\n"
    "　　七点，钟楼对面。\n"
    "　　别带任何人。”\n"
    "　　林舟丢失过一天，这件事他从没对任何人说过。\n"
    "　　三个月前的某个清晨，他在自己的书桌前醒来，桌上的日历停在十四号，可报纸上的日期却已经是十六号。中间那一天"
    "去了哪里，他做过什么、见过谁，脑子里一片空白，就像有人用一把干净的刀，把那一页从他的人生里整整齐齐地裁了下去。\n"
    "　　他去问过医生，医生说是过度疲劳；他去问过房东，房东说那天看见他一早出门，直到深夜才回来，脸色很差，一句话"
    "也没说。\n"
    "　　从那以后，他开始做同一个梦。梦里有一扇灰色的门，门后是没有尽头的雾，雾里有人在叫他的名字，声音很近，却永远"
    "看不清是谁。\n"
    "　　雨渐渐小了。\n"
    "　　就在林舟准备转身离开的时候，钟楼的阴影里忽然走出一个人。那人穿着深色的长外套，帽檐压得很低，手里提着一盏"
    "老式的煤油灯，灯光在雨雾中晕开，像一小团不肯熄灭的火。\n"
    "　　“抱歉，来晚了。”来人在他面前停下，声音出乎意料地年轻，“路上遇到了一点麻烦。”\n"
    "　　林舟下意识地后退半步：“你就是写信的人？”\n"
    "　　“是我。”对方抬起头，露出一张平静而苍白的脸，“不过在回答你的问题之前，我想先确认一件事——最近，你是不是"
    "总梦见一扇灰色的门？”\n"
    "　　林舟的心猛地一沉。\n"
    "　　他没有回答，可对方似乎已经从他的沉默里得到了答案，轻轻叹了口气，把煤油灯举高了一些。\n"
    "　　“果然如此。”那人说，“跟我来吧，有些东西，你得亲眼看看。”\n";

/* ---- 书架数据 / Shelf data ---- */

static shelf_entry_t s_hr_books[7];
static void hr_book(int i, const char* name, const char* path, const char* title, const char* author, bool tomato,
                    uint8_t pct, bool progress, uint16_t new_ch, uint16_t fresh) {
    shelf_entry_t* e = &s_hr_books[i];
    memset(e, 0, sizeof(*e));
    snprintf(e->name, sizeof(e->name), "%s", name);
    snprintf(e->path, sizeof(e->path), "/sdcard/books/%s", path);
    snprintf(e->title, sizeof(e->title), "%s", title);
    snprintf(e->author, sizeof(e->author), "%s", author);
    e->tomato = tomato;
    e->pct = pct;
    e->has_progress = progress;
    e->new_chapters = new_ch;
    e->fresh_chapters = fresh;
    e->size = 2400000;
    e->search_match = true;
}
// 路径里带 "text:" 的书没有图片封面，画文字封面。/ Paths containing "text:" have no image cover and get a text cover.
static void hr_fill_shelf(void) {
    hr_book(0, "宿命之环.txt", "tomato/宿命之环.txt", "宿命之环", "爱潜水的乌贼", true, 41, true, 0, 3);
    hr_book(1, "大奉打更人.txt", "tomato/大奉打更人.txt", "大奉打更人", "卖报小郎君", true, 67, true, 5, 0);
    hr_book(2, "三体.epub", "三体.epub", "", "", false, 88, true, 0, 0);
    hr_book(3, "活着.txt", "text:活着.txt", "", "", false, 100, true, 0, 0);
    hr_book(4, "围城.epub", "围城.epub", "", "", false, 23, true, 0, 0);
    hr_book(5, "平凡的世界.txt", "text:平凡的世界.txt", "", "", false, 12, true, 0, 0);
    hr_book(6, "百年孤独.epub", "百年孤独.epub", "", "", false, 0, false, 0, 0);
    s_shelf = s_hr_books;
    s_count = s_visible_count = 7;
}
static void hr_reset(void) {
    s_view = SHELF;
    s_query[0] = s_message[0] = s_shelf_warning[0] = s_batch_message[0] = s_manage_message[0] = 0;
    s_resume_path[0] = 0;
    s_save_failed = s_batch_confirm = s_clear_confirm = s_delete_confirm = s_file_removed = false;
    s_toolbar = s_typeset = s_image_open = false;
    s_filter = 0;
    s_recent_sort = false;
    s_pressed_control = -1;
    s_text = NULL;
    hr_fill_shelf();
    snprintf(s_continue, sizeof(s_continue), "%s", s_hr_books[0].path);
}
static void hr_shelf(app_ctx_t* ctx, const char* scene) {
    hr_reset();
    ctx->leaf = 0;
    render(ctx, hr_fb);
    hr_allow_fit();
    hr_shot(scene);
}

void hr_scenes(void) {
    app_ctx_t ctx = {.fb = hr_fb};

    // 书架：继续阅读卡片、番茄更新、图片和文字封面。/ Shelf: continue card, Tomato updates, image and text covers.
    hr_shelf(&ctx, "shelf");

    // 阅读页：真实排版的正文，显示本章第 2 页。/ Reader: real layout body on page 2 of the chapter.
    hr_reset();
    s_view = READING;
    s_text = s_hr_text;
    s_text_len = strlen(s_hr_text);
    s_chapter = 490;
    s_px = 44;
    snprintf(s_path, sizeof(s_path), "%s", s_hr_books[0].path);
    book_chapter_title(s_chapter, s_title, sizeof(s_title));
    apply_style();
    if (!book_layout_build(s_text, s_text_len, body_rect(), s_px)) printf("layout failed\n");
    s_page = book_layout_page_count() > 1 ? 1 : 0;
    render(&ctx, hr_fb);
    hr_no_bar_check();
    hr_shot("reader");

    // 同一页打开工具条。/ The same page with the toolbar open.
    s_toolbar = true;
    s_auto_level = 0;
    render(&ctx, hr_fb);
    hr_allow_fit();
    hr_no_bar_check();
    hr_shot("toolbar");
    s_toolbar = false;
    book_layout_free();
    s_text = NULL;

    // 特大显示（130%）的书架。/ The shelf at the largest display size (130%).
    ui_set_scale(130);
    hr_shelf(&ctx, "shelf_large");
    ui_set_scale(100);
}
