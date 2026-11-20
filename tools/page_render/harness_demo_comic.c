/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：漫画页的演示图：书架一整页封面，阅读器里一页画好的漫画。/ English: Demo shots of the comics page: a full shelf
 * of covers and one drawn manga page in the reader.
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "app_comic.c"   // 按 -I 找：当前源码 / Found via -I: current sources
#include "page_render.h"

const char* hr_page = "demo_comic";

/* ---- 灰度画布 / Gray canvas ---- */

typedef struct {
    uint8_t* p;
    int w, h;
} canvas_t;

static void put(canvas_t* c, int x, int y, int v) {
    if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;
    c->p[y * c->w + x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

static void fill(canvas_t* c, int x0, int y0, int w, int h, int v) {
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) put(c, x, y, v);
}

// 竖向渐变。/ Vertical gradient.
static void vgrad(canvas_t* c, int x0, int y0, int w, int h, int top, int bottom) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) put(c, x0 + x, y0 + y, top + (bottom - top) * y / (h > 1 ? h - 1 : 1));
}

// 网点：在已有灰度上叠圆点。/ Screentone: dots over the existing tone.
static void tone(canvas_t* c, int x0, int y0, int w, int h, int pitch, int r, int v) {
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) {
            const int ox = (x - x0 + ((y - y0) / pitch % 2) * pitch / 2) % pitch - pitch / 2;
            const int oy = (y - y0) % pitch - pitch / 2;
            if (ox * ox + oy * oy <= r * r) put(c, x, y, v);
        }
}

static void ellipse(canvas_t* c, int cx, int cy, int rx, int ry, int v) {
    for (int y = -ry; y <= ry; y++)
        for (int x = -rx; x <= rx; x++)
            if ((long)x * x * ry * ry + (long)y * y * rx * rx <= (long)rx * rx * ry * ry) put(c, cx + x, cy + y, v);
}

static void line(canvas_t* c, int x0, int y0, int x1, int y1, int t, int v) {
    const int dx = abs(x1 - x0), dy = abs(y1 - y0), n = dx > dy ? dx : dy;
    for (int i = 0; i <= n; i++) {
        const int x = x0 + (x1 - x0) * i / (n ? n : 1), y = y0 + (y1 - y0) * i / (n ? n : 1);
        fill(c, x - t / 2, y - t / 2, t, t, v);
    }
}

static void tri(canvas_t* c, int ax, int ay, int bx, int by, int qx, int qy, int v) {
    const int minx = ax < bx ? (ax < qx ? ax : qx) : (bx < qx ? bx : qx);
    const int maxx = ax > bx ? (ax > qx ? ax : qx) : (bx > qx ? bx : qx);
    const int miny = ay < by ? (ay < qy ? ay : qy) : (by < qy ? by : qy);
    const int maxy = ay > by ? (ay > qy ? ay : qy) : (by > qy ? by : qy);
    for (int y = miny; y <= maxy; y++)
        for (int x = minx; x <= maxx; x++) {
            const long d1 = (long)(x - bx) * (ay - by) - (long)(ax - bx) * (y - by);
            const long d2 = (long)(x - qx) * (by - qy) - (long)(bx - qx) * (y - qy);
            const long d3 = (long)(x - ax) * (qy - ay) - (long)(qx - ax) * (y - ay);
            const bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
            if (!(neg && pos)) put(c, x, y, v);
        }
}

static void frame(canvas_t* c, int x, int y, int w, int h, int t) {
    fill(c, x, y, w, t, 0);
    fill(c, x, y + h - t, w, t, 0);
    fill(c, x, y, t, h, 0);
    fill(c, x + w - t, y, t, h, 0);
}

// 人物剪影：头、肩、身体。/ Figure silhouette: head, shoulders, body.
static void figure(canvas_t* c, int cx, int foot, int hgt, int v) {
    const int head = hgt / 7;
    ellipse(c, cx, foot - hgt + head, head, head, v);
    tri(c, cx - hgt / 6, foot - hgt / 8, cx + hgt / 6, foot - hgt / 8, cx, foot - hgt + 2 * head, v);
    fill(c, cx - hgt / 6, foot - hgt / 8, hgt / 3, hgt / 8, v);
}

/* ---- 漫画页 / Manga page ---- */

#define PAGE_W 684
#define PAGE_H 1140

typedef struct {
    int cx, cy, rx, ry;   // 气泡 / Bubble
    int tx, ty;           // 尾巴尖 / Tail tip
    const char* lines[2];
} bubble_t;

static const bubble_t BUBBLES[] = {
    {178, 418, 128, 58, 170, 520, {"今天的浪，", "好像特别大。"}},
    {528, 812, 118, 52, 470, 900, {"明天也来吗？", ""}},
    {196, 1000, 150, 56, 272, 1090, {"嗯，约好了，", "下午四点见！"}},
};

static uint8_t s_page_px[PAGE_W * PAGE_H];

static void draw_bubble(canvas_t* c, const bubble_t* b) {
    tri(c, b->cx - 22, b->cy, b->cx + 22, b->cy, b->tx, b->ty, 0);
    ellipse(c, b->cx, b->cy, b->rx + 4, b->ry + 4, 0);
    tri(c, b->cx - 15, b->cy, b->cx + 15, b->cy, b->tx + (b->cx - b->tx) / 12, b->ty - 7, 255);
    ellipse(c, b->cx, b->cy, b->rx, b->ry, 255);
}

static void paint_page(void) {
    canvas_t c = {s_page_px, PAGE_W, PAGE_H};
    fill(&c, 0, 0, PAGE_W, PAGE_H, 255);
    // 第一格：海边远景，天空渐变、太阳、海浪、海鸥。/ Panel 1: seaside vista with sky, sun, waves and gulls.
    vgrad(&c, 18, 18, 648, 300, 236, 168);
    ellipse(&c, 520, 150, 46, 46, 252);
    for (int y = 218; y < 318; y++) {
        const int base = 120 - (y - 218) / 2;
        for (int x = 18; x < 666; x++) put(&c, x, y, base + ((x / 9 + y / 3) % 7 == 0 ? 60 : 0));
    }
    for (int k = 0; k < 8; k++) line(&c, 40 + k * 76, 226 + k % 3 * 26, 92 + k * 76, 226 + k % 3 * 26, 2, 245);
    for (int k = 0; k < 3; k++) {
        const int gx = 120 + k * 70, gy = 90 + (k % 2) * 30;
        line(&c, gx - 18, gy - 8, gx, gy, 3, 40);
        line(&c, gx, gy, gx + 18, gy - 8, 3, 40);
    }
    fill(&c, 26, 26, 210, 52, 255);
    frame(&c, 26, 26, 210, 52, 2);
    frame(&c, 18, 18, 648, 300, 4);
    // 第二格：少年的近景，网点背景。/ Panel 2: close-up of the boy over screentone.
    fill(&c, 18, 334, 300, 384, 238);
    tone(&c, 18, 334, 300, 384, 10, 2, 150);
    ellipse(&c, 168, 600, 92, 104, 0);
    ellipse(&c, 168, 606, 86, 96, 250);
    ellipse(&c, 168, 548, 94, 56, 30);
    fill(&c, 74, 548, 188, 18, 238);
    tone(&c, 74, 548, 188, 18, 10, 2, 150);
    ellipse(&c, 168, 560, 90, 30, 30);
    ellipse(&c, 134, 616, 10, 15, 0);
    ellipse(&c, 202, 616, 10, 15, 0);
    ellipse(&c, 131, 610, 3, 4, 255);
    ellipse(&c, 199, 610, 3, 4, 255);
    line(&c, 152, 664, 184, 664, 3, 60);
    fill(&c, 120, 700, 96, 18, 40);
    frame(&c, 18, 334, 300, 384, 4);
    // 第三格：奔跑，速度线。/ Panel 3: running with speed lines.
    fill(&c, 332, 334, 334, 384, 255);
    for (int k = 0; k < 40; k++) {
        const int ex = k < 20 ? 332 + k * 17 : 666, ey = k < 20 ? 334 : 334 + (k - 20) * 19;
        line(&c, 500 + (ex - 500) / 3, 530 + (ey - 530) / 3, ex, ey, k % 3 ? 2 : 3, 30);
    }
    for (int k = 0; k < 20; k++) {
        const int ex = 332 + k * 17;
        line(&c, 500 + (ex - 500) / 3, 530 + (718 - 530) / 3, ex, 718, 2, 30);
    }
    ellipse(&c, 500, 560, 90, 110, 255);
    figure(&c, 500, 680, 220, 20);
    line(&c, 470, 640, 420, 690, 14, 20);
    line(&c, 530, 640, 580, 676, 14, 20);
    frame(&c, 332, 334, 334, 384, 4);
    // 第四格：夕阳下两个人。/ Panel 4: two figures at sunset.
    vgrad(&c, 18, 732, 648, 390, 214, 92);
    ellipse(&c, 340, 980, 120, 120, 236);
    fill(&c, 18, 980, 648, 142, 70);
    for (int k = 0; k < 6; k++) line(&c, 60 + k * 100, 1010 + k % 2 * 30, 130 + k * 100, 1010 + k % 2 * 30, 3, 160);
    figure(&c, 420, 1100, 250, 12);
    figure(&c, 560, 1100, 220, 12);
    frame(&c, 18, 732, 648, 390, 4);
    for (size_t i = 0; i < sizeof(BUBBLES) / sizeof(BUBBLES[0]); i++) draw_bubble(&c, &BUBBLES[i]);
}

/* ---- 封面 / Covers ---- */

typedef struct {
    const char* name;
    bool series;
    uint16_t volumes, done;
    uint32_t stamp;
    uint8_t pct;
} demo_comic_t;

static const demo_comic_t COMICS[] = {
    {"海边的少年", true, 24, 7, 9, 0},
    {"星辰旅馆", false, 1, 0, 8, 46},
    {"山间食堂", true, 36, 36, 7, 0},
    {"纸飞机日记", false, 1, 1, 6, 100},
    {"猫咪侦探社", true, 18, 3, 5, 0},
    {"雪国列车员", false, 1, 0, 0, 0},
    {"灯塔守望者", true, 12, 0, 0, 0},
    {"夏日回声", false, 1, 0, 3, 72},
    {"银杏街的书店", true, 30, 12, 2, 0},
};
#define DEMO_COUNT ((int)(sizeof(COMICS) / sizeof(COMICS[0])))

static uint8_t s_cover_px[DEMO_COUNT][CELL_W * COVER_H];
static uint8_t s_mini_px[DEMO_COUNT][MINI_W * MINI_H];

// 一张封面：渐变底、大月亮或太阳、人物剪影、地面，按序号换花样。/ One cover: gradient, a big sun or moon, a figure and
// ground, varied by index.
static void paint_cover(canvas_t* c, int i) {
    const int s = c->w;   // 按宽度缩放 / Scale by width
    const int top = 70 + (i * 47) % 120, bottom = 200 + (i * 13) % 50;
    vgrad(c, 0, 0, c->w, c->h, i % 2 ? bottom : top, i % 2 ? top : bottom);
    ellipse(c, c->w * (30 + (i * 23) % 40) / 100, c->h * 38 / 100, s * 22 / 100, s * 22 / 100, i % 2 ? 60 : 248);
    if (i % 3 == 0) tone(c, 0, c->h * 70 / 100, c->w, c->h * 30 / 100, s / 15 + 3, s / 60 + 1, 90);
    else fill(c, 0, c->h * 74 / 100, c->w, c->h * 26 / 100, 50 + (i * 31) % 60);
    figure(c, c->w * (55 + (i * 17) % 25) / 100, c->h * 88 / 100, c->h * 46 / 100, 15);
    if (i % 2 == 0) figure(c, c->w * 28 / 100, c->h * 90 / 100, c->h * 36 / 100, 40);
    frame(c, 0, 0, c->w, c->h, 1);
}

static int cover_index(const uint8_t* px) {
    for (int i = 0; i < DEMO_COUNT; i++)
        if (px == s_cover_px[i]) return i;
    return -1;
}

// 图片替身：把灰度缓冲原样画上；大封面顶部压一条深色标题带，漫画页在气泡里写字。/ Image stand-in: blit the gray buffer;
// large covers get a dark title band, the manga page gets text in its bubbles.
void hupu_img_draw(uint8_t* fb, int x, int y, int w, int h, const uint8_t* px, bool border) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) epd_draw_pixel(x + i, y + j, px[j * w + i], fb);
    if (border) epd_draw_rect((EpdRect){x, y, w, h}, 0x60, fb);
    const int ci = cover_index(px);
    if (ci >= 0) {
        epd_fill_rect((EpdRect){x + 1, y + 12, w - 2, 36}, 0x20, fb);
        tr_text_on(fb, x + w / 2, y + 16, 22, COMICS[ci].name, EPD_DRAW_ALIGN_CENTER, true, 0xFF, 0x20);
    }
    if (px == s_page_px) {
        tr_text_on(fb, x + 26 + 105, y + 38, 22, "七月 · 海边", EPD_DRAW_ALIGN_CENTER, true, 0x00, 0xFF);
        for (size_t k = 0; k < sizeof(BUBBLES) / sizeof(BUBBLES[0]); k++) {
            const bubble_t* b = &BUBBLES[k];
            const int n = b->lines[1][0] ? 2 : 1;
            for (int l = 0; l < n; l++)
                tr_text_on(fb, x + b->cx, y + b->cy - n * 17 + l * 34 - 2, 26, b->lines[l], EPD_DRAW_ALIGN_CENTER, true,
                           0x00, 0xFF);
        }
    }
}

// 话名：系列页和工具条用。/ Episode names for the series page and the toolbar.
static const char* const EPISODES[24] = {
    "初夏", "转学生", "防波堤", "旧相机", "雨天的约定", "夜市", "灯塔下", "潮声", "海风信", "退潮以后", "萤火",
    "台风前夜", "停电", "晴空", "渡船", "运动会", "秋祭", "第一封信", "海雾", "冬日海岸", "归港", "初雪", "新年参拜",
    "春汛"};

// 进度替身：海边的少年前 7 话看完、第 8 话看到第 23 页，其余没看。/ Progress stand-in: the first 7 episodes of the
// seaside series are finished, episode 8 is at page 23, the rest unread.
bool comic_progress_get(const char* path, comic_progress_t* out) {
    memset(out, 0, sizeof(*out));
    const char* p = strstr(path, "海边的少年/第");
    if (!p) return false;
    const int n = atoi(p + strlen("海边的少年/第"));
    if (n < 1 || n > 8) return false;
    out->pages = n == 8 ? 52 : 48 + n % 5;
    out->page = n == 8 ? 22 : out->pages - 1;
    out->done = n < 8;
    out->stamp = (uint32_t)n;
    return true;
}

static comic_entry_t s_demo_shelf[DEMO_COUNT];
static char s_demo_vols[24][COMIC_NAME_MAX];

static void setup(void) {
    paint_page();
    s_shelf = s_demo_shelf;
    s_vols = s_demo_vols;
    s_count = DEMO_COUNT;
    for (int i = 0; i < DEMO_COUNT; i++) {
        comic_entry_t* e = &s_shelf[i];
        const demo_comic_t* d = &COMICS[i];
        memset(e, 0, sizeof(*e));
        snprintf(e->name, sizeof(e->name), "%s", d->name);
        snprintf(e->path, sizeof(e->path), COMIC_ROOT "/%s%s", d->name, d->series ? "" : ".cbz");
        e->series = d->series;
        e->volumes = d->volumes;
        e->done = d->done;
        e->stamp = d->stamp;
        e->pct = d->pct;
        canvas_t big = {s_cover_px[i], CELL_W, COVER_H}, mini = {s_mini_px[i], MINI_W, MINI_H};
        paint_cover(&big, i);
        paint_cover(&mini, i);
        snprintf(s_covers[i].key, sizeof(s_covers[i].key), "%s", e->path);
        s_covers[i].px = s_cover_px[i];
        s_covers[i].mini = s_mini_px[i];
    }
    s_sort_name = false;
    s_page = 0;
    s_message[0] = 0;
    snprintf(s_last, sizeof(s_last), COMIC_ROOT "/海边的少年/第8话 潮声.cbz");
    snprintf(s_last_title, sizeof(s_last_title), "海边的少年 · 第 8 话");
    snprintf(s_last_owner, sizeof(s_last_owner), COMIC_ROOT "/海边的少年");
    s_last_page = 22;
    s_last_pages = 52;
    s_series = 0;
    s_vol_count = 24;
    for (int i = 0; i < 24; i++) snprintf(s_vols[i], COMIC_NAME_MAX, "第%02d话 %s.cbz", i + 1, EPISODES[i]);
}

// 阅读器停在海边的少年第 8 话第 23 页。/ The reader sits on page 23 of episode 8 of the seaside series.
static void setup_reader(void) {
    setup();
    s_view = V_READ;
    s_buf[0] = (pagebuf_t){.idx = 22, .w = PAGE_W, .h = PAGE_H, .px = s_page_px};
    s_buf[1].idx = -1;
    snprintf(s_book_title, sizeof(s_book_title), "第08话 潮声");
    s_cur = 22;
    s_page_count = 52;
    s_vol_index = 7;
    s_rtl = true;
    s_toolbar = false;
    s_message[0] = 0;
}

void hr_scenes(void) {
    app_ctx_t ctx = {.fb = hr_fb};
    setup();
    s_view = V_SHELF;
    render(&ctx, hr_fb);
    hr_shot("shelf");

    setup();
    s_view = V_READ;
    s_buf[0] = (pagebuf_t){.idx = 22, .w = PAGE_W, .h = PAGE_H, .px = s_page_px};
    s_buf[1].idx = -1;
    snprintf(s_book_title, sizeof(s_book_title), "海边的少年 · 第 8 话 潮声");
    s_cur = 22;
    s_page_count = 52;
    s_vol_index = 7;
    s_rtl = true;
    s_toolbar = false;
    hr_no_bar_check();
    render(&ctx, hr_fb);
    hr_shot("reader");

    // 设计文档 ui.html 的漫画图。/ Comics figures for the ui.html design doc.
    setup();
    s_view = V_SHELF;
    render(&ctx, hr_fb);
    hr_shot("ui6");

    // 系列页：默认翻到“在看”那一页（第 2 页，第 8～14 话）。/ Series page on the "reading" page (page 2, episodes 8-14).
    setup();
    s_view = V_SERIES;
    s_ser_done = 7;
    s_ser_resume = 7;
    s_ser_page = 22;
    s_ser_dir = 1;
    s_vol_page = s_ser_resume / ROWS_PER;
    render(&ctx, hr_fb);
    hr_shot("ui7");

    setup_reader();
    hr_no_bar_check();
    render(&ctx, hr_fb);
    hr_shot("ui8");

    setup_reader();
    s_toolbar = true;
    s_auto_level = 0;
    hr_no_bar_check();
    render(&ctx, hr_fb);
    hr_shot("ui9");
}
