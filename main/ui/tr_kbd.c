/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 屏幕键盘实现。按键几何在绘制和命中之间共用，避免两边对不上。
 * Keyboard implementation. Key geometry is shared by drawing and hit testing so they never disagree.
 */

#include "tr_kbd.h"

#include <stdio.h>
#include <string.h>

#include "tr_ui.h"
#include "ttf_font.h"
#include "ui_kit.h"

#define COMP_Y (TR_KBD_TOP + 18)
#define CAND_Y (TR_KBD_TOP + 66)
#define CAND_H 70
#define CAND_PX 36
#define KEYS_Y (TR_KBD_TOP + 172)
#define KEY_W 56
#define KEY_H 92
#define KEY_GAP 6
#define ROW_GAP 12
#define KEY_GRAY 0xE0

enum {
    K_FN_LEFT = 100, K_BACK, K_MODE, K_LANG, K_SPACE, K_GO, K_CAND_MORE,
    K_CAND0 = 200,
};

static const char* const ROWS_LETTERS[3] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
// 数字符号布局；每个按键一个 ASCII 字符。/ Symbol layout; one ASCII character per key.
static const char* const ROWS_SYMBOLS[3] = {"1234567890", "-/:;()@&\"", ".,?!'_="};

typedef struct {
    EpdRect r;
    int id;        // 字符键为字符本身，功能键为 K_* / Character keys use the char, function keys K_*
} kb_key_t;

EpdRect tr_kbd_area(void) { return (EpdRect){0, TR_KBD_TOP, TR_W, TR_H - TR_KBD_TOP}; }

static int row_y(int row) { return KEYS_Y + row * (KEY_H + ROW_GAP); }

// 生成当前布局的全部按键。/ Build every key for the current layout.
static int layout(const tr_kbd_t* kbd, kb_key_t* keys) {
    const char* const* rows = kbd->mode == TR_KBD_SYMBOL ? ROWS_SYMBOLS : ROWS_LETTERS;
    int n = 0;
    for (int r = 0; r < 3; r++) {
        int count = (int)strlen(rows[r]);
        int total = count * KEY_W + (count - 1) * KEY_GAP;
        int x0 = (TR_W - total) / 2;
        for (int i = 0; i < count; i++) {
            keys[n++] = (kb_key_t){{x0 + i * (KEY_W + KEY_GAP), row_y(r), KEY_W, KEY_H}, rows[r][i]};
        }
    }
    keys[n++] = (kb_key_t){{24, row_y(2), 92, KEY_H}, K_FN_LEFT};
    keys[n++] = (kb_key_t){{TR_W - 24 - 92, row_y(2), 92, KEY_H}, K_BACK};
    keys[n++] = (kb_key_t){{24, row_y(3), 120, KEY_H}, K_MODE};
    keys[n++] = (kb_key_t){{156, row_y(3), 120, KEY_H}, K_LANG};
    keys[n++] = (kb_key_t){{288, row_y(3), 200, KEY_H}, K_SPACE};
    keys[n++] = (kb_key_t){{500, row_y(3), TR_W - 24 - 500, KEY_H}, K_GO};
    return n;
}

static const char* key_label(const tr_kbd_t* kbd, int id, char buf[4]) {
    switch (id) {
    case K_FN_LEFT: return kbd->mode == TR_KBD_PINYIN ? "分词" : ".";
    case K_BACK: return "删除";
    case K_MODE: return kbd->mode == TR_KBD_SYMBOL ? (kbd->letters == TR_KBD_PINYIN ? "拼音" : "abc") : "123";
    case K_LANG: return kbd->mode == TR_KBD_PINYIN ? "中 / 英" : "英 / 中";
    case K_SPACE: return "空格";
    case K_GO: return kbd->go_label ? kbd->go_label : "确定";
    default: buf[0] = (char)id; buf[1] = 0; return buf;
    }
}

/* ---- 候选 / Candidates ---- */

static void refresh_cands(tr_kbd_t* kbd) {
    kbd->cand_count = 0;
    kbd->cand_first = 0;
    if (kbd->mode == TR_KBD_PINYIN && kbd->ime_ok && kbd->comp[0]) {
        kbd->cand_count = pinyin_ime_query(kbd->comp, PINYIN_IME_QWERTY, kbd->cands, TR_KBD_CANDS);
    }
}

// 从 cand_first 开始能放下几个；more 表示后面还有。/ How many fit from cand_first; more if any remain.
static int cand_visible(const tr_kbd_t* kbd, int xs[], int ws[], bool* more) {
    int x = TR_MARGIN, n = 0;
    const int limit = TR_W - TR_MARGIN - 60;
    for (int i = kbd->cand_first; i < kbd->cand_count; i++) {
        int w = ttf_text_width_px(CAND_PX, kbd->cands[i].text) + 48;
        if (x + w > limit && n) break;
        xs[n] = x;
        ws[n] = w;
        x += w;
        n++;
    }
    *more = kbd->cand_first + n < kbd->cand_count || kbd->cand_first > 0;
    return n;
}

static void append_text(tr_kbd_t* kbd, const char* s) {
    size_t have = strlen(kbd->text), add = strlen(s);
    if (have + add < sizeof(kbd->text)) memcpy(kbd->text + have, s, add + 1);
}

static void pick(tr_kbd_t* kbd, int index) {
    if (index < 0 || index >= kbd->cand_count) return;
    append_text(kbd, kbd->cands[index].text);
    size_t len = strlen(kbd->comp), used = kbd->cands[index].consumed;
    if (used > len) used = len;
    memmove(kbd->comp, kbd->comp + used, len - used + 1);
    refresh_cands(kbd);
}

// 有编码串时按空格或确认：选第一个候选，没有候选就把字母原样上屏。
// Space or Go with a composition: take the first candidate, or commit the letters as typed.
static void commit_comp(tr_kbd_t* kbd) {
    if (!kbd->comp[0]) return;
    if (kbd->cand_count) { pick(kbd, 0); return; }
    for (char* p = kbd->comp; *p; p++) if (*p != '\'') { char c[2] = {*p, 0}; append_text(kbd, c); }
    kbd->comp[0] = 0;
}

static void backspace_text(tr_kbd_t* kbd) {
    size_t n = strlen(kbd->text);
    while (n && ((unsigned char)kbd->text[n - 1] & 0xC0) == 0x80) n--;
    if (n) n--;
    kbd->text[n] = 0;
}

void tr_kbd_open(tr_kbd_t* kbd, const char* initial, bool chinese, const char* go_label) {
    memset(kbd, 0, sizeof(*kbd));
    snprintf(kbd->text, sizeof(kbd->text), "%s", initial ? initial : "");
    kbd->go_label = go_label;
    kbd->ime_ok = chinese && pinyin_ime_load(NULL);
    kbd->mode = kbd->ime_ok ? TR_KBD_PINYIN : TR_KBD_ENGLISH;
    kbd->letters = kbd->mode;
}

/* ---- 绘制 / Drawing ---- */

void tr_kbd_draw(uint8_t* fb, const tr_kbd_t* kbd) {
    epd_fill_rect(tr_kbd_area(), UI_GRAY_WHITE, fb);
    tr_hline(fb, TR_KBD_TOP, 0, TR_W, false);
    if (kbd->mode == TR_KBD_PINYIN) {
        tr_text(fb, TR_MARGIN, COMP_Y, 30, kbd->comp, EPD_DRAW_ALIGN_LEFT, true);
        int xs[TR_KBD_CANDS], ws[TR_KBD_CANDS];
        bool more = false;
        int n = cand_visible(kbd, xs, ws, &more);
        for (int i = 0; i < n; i++) {
            const pinyin_ime_cand_t* c = &kbd->cands[kbd->cand_first + i];
            if (i) epd_fill_rect((EpdRect){xs[i], CAND_Y + 16, 2, CAND_H - 32}, TR_GRAY_LINE, fb);
            tr_text(fb, xs[i] + ws[i] / 2, CAND_Y + (CAND_H - CAND_PX) / 2 - 2, CAND_PX, c->text,
                    EPD_DRAW_ALIGN_CENTER, i == 0 && kbd->cand_first == 0);
        }
        if (more) tr_text(fb, TR_W - TR_MARGIN, CAND_Y + 14, CAND_PX, "›", EPD_DRAW_ALIGN_RIGHT, false);
        if (!kbd->comp[0]) {
            tr_text(fb, TR_MARGIN, CAND_Y + 18, 28, "输入拼音，点候选上屏", EPD_DRAW_ALIGN_LEFT, false);
        }
    } else if (!kbd->ime_ok && kbd->mode == TR_KBD_ENGLISH) {
        tr_text(fb, TR_MARGIN, CAND_Y + 18, 26, "没有拼音词库：把 ime/pinyin-v1.bin 放进 TF 卡", EPD_DRAW_ALIGN_LEFT, false);
    }
    tr_hline(fb, CAND_Y + CAND_H + 10, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);

    kb_key_t keys[48];
    int n = layout(kbd, keys);
    char buf[4];
    for (int i = 0; i < n; i++) {
        const kb_key_t* k = &keys[i];
        const char* label = key_label(kbd, k->id, buf);
        if (k->id == K_GO) {
            ui_fill_round_rect(fb, k->r, 12, UI_GRAY_BLACK);
            tr_text_inv(fb, k->r.x + k->r.width / 2, k->r.y + (k->r.height - 30) / 2 - 2, 30, label, EPD_DRAW_ALIGN_CENTER, true);
        } else if (k->id >= K_FN_LEFT && k->id != K_SPACE) {
            ui_draw_round_rect(fb, k->r, 12, UI_GRAY_BLACK);
            ui_draw_round_rect(fb, ui_inset_rect(k->r, 1), 11, UI_GRAY_BLACK);
            tr_text(fb, k->r.x + k->r.width / 2, k->r.y + (k->r.height - 28) / 2 - 2, 28, label, EPD_DRAW_ALIGN_CENTER, false);
        } else {
            ui_fill_round_rect(fb, k->r, 12, KEY_GRAY);
            int px = k->id == K_SPACE ? 28 : 36;
            tr_text(fb, k->r.x + k->r.width / 2, k->r.y + (k->r.height - px) / 2 - 2, px, label, EPD_DRAW_ALIGN_CENTER, false);
        }
    }
}

/* ---- 触摸 / Touch ---- */

tr_kbd_result_t tr_kbd_touch(tr_kbd_t* kbd, uint16_t x, uint16_t y) {
    if (y < TR_KBD_TOP) return TR_KBD_NONE;
    // 候选条。/ Candidate bar.
    if (y >= CAND_Y && y < CAND_Y + CAND_H && kbd->mode == TR_KBD_PINYIN && kbd->cand_count) {
        int xs[TR_KBD_CANDS], ws[TR_KBD_CANDS];
        bool more = false;
        int n = cand_visible(kbd, xs, ws, &more);
        if (more && x >= TR_W - TR_MARGIN - 60) {
            kbd->cand_first += n;
            if (kbd->cand_first >= kbd->cand_count) kbd->cand_first = 0;
            return TR_KBD_CHANGED;
        }
        for (int i = 0; i < n; i++) {
            if (x >= xs[i] && x < xs[i] + ws[i]) { pick(kbd, kbd->cand_first + i); return TR_KBD_CHANGED; }
        }
        return TR_KBD_NONE;
    }
    kb_key_t keys[48];
    int n = layout(kbd, keys);
    // 键之间的缝隙按最近的键算，手指偏一点也能按中。/ Gaps snap to the nearest key so a slight miss still lands.
    int best = -1, best_d = 1 << 30;
    for (int i = 0; i < n; i++) {
        const EpdRect* r = &keys[i].r;
        int dx = x < r->x ? r->x - x : x >= r->x + r->width ? x - (r->x + r->width) + 1 : 0;
        int dy = y < r->y ? r->y - y : y >= r->y + r->height ? y - (r->y + r->height) + 1 : 0;
        int d = dx * dx + dy * dy;
        if (d < best_d) { best_d = d; best = i; }
    }
    if (best < 0 || best_d > 12 * 12) return TR_KBD_NONE;
    int id = keys[best].id;
    switch (id) {
    case K_FN_LEFT:
        if (kbd->mode == TR_KBD_PINYIN) {
            size_t len = strlen(kbd->comp);
            if (len && kbd->comp[len - 1] != '\'' && len + 1 < sizeof(kbd->comp)) { kbd->comp[len] = '\''; kbd->comp[len + 1] = 0; }
        } else {
            append_text(kbd, ".");
        }
        break;
    case K_BACK:
        if (kbd->mode == TR_KBD_PINYIN && kbd->comp[0]) {
            kbd->comp[strlen(kbd->comp) - 1] = 0;
            refresh_cands(kbd);
        } else {
            backspace_text(kbd);
        }
        break;
    case K_MODE:
        commit_comp(kbd);
        kbd->mode = kbd->mode == TR_KBD_SYMBOL ? kbd->letters : TR_KBD_SYMBOL;
        break;
    case K_LANG:
        commit_comp(kbd);
        if (kbd->mode == TR_KBD_PINYIN) kbd->mode = TR_KBD_ENGLISH;
        else if (kbd->ime_ok) kbd->mode = TR_KBD_PINYIN;
        kbd->letters = kbd->mode == TR_KBD_SYMBOL ? kbd->letters : kbd->mode;
        refresh_cands(kbd);
        break;
    case K_SPACE:
        if (kbd->mode == TR_KBD_PINYIN && kbd->comp[0]) commit_comp(kbd);
        else append_text(kbd, " ");
        break;
    case K_GO:
        commit_comp(kbd);
        return TR_KBD_SUBMIT;
    default:
        if (kbd->mode == TR_KBD_PINYIN) {
            size_t len = strlen(kbd->comp);
            if (len < PINYIN_IME_MAX_INPUT) { kbd->comp[len] = (char)id; kbd->comp[len + 1] = 0; }
            refresh_cands(kbd);
        } else {
            char c[2] = {(char)id, 0};
            append_text(kbd, c);
        }
        break;
    }
    return TR_KBD_CHANGED;
}
