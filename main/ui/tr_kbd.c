/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 屏幕键盘实现。按键几何在绘制和命中之间共用，避免两边对不上。
 * 语言键在 全拼 → 九键 → 英文 之间循环；“123”切到数字符号再切回。
 *
 * Keyboard implementation. Key geometry is shared by drawing and hit testing so they never disagree.
 * The language key cycles full pinyin, T9 and English; "123" toggles symbols and back.
 */

#include "tr_kbd.h"

#include <stdio.h>
#include <string.h>

#include "tr_ui.h"
#include "ttf_font.h"
#include "ui_kit.h"

// 高度和字号随显示大小放大；横向受屏宽限制，见 key_w() 和 layout()。/ Heights and font sizes scale with the
// display size; widths are capped by the screen, see key_w() and layout().
#define COMP_Y (TR_KBD_TOP + TR_S(18))
#define CAND_Y (TR_KBD_TOP + TR_S(66))
#define CAND_H TR_S(70)
#define CAND_PX TR_S(36)
#define KEYS_Y (TR_KBD_TOP + TR_S(172))
#define KEY_H TR_S(92)
#define KEY_GAP TR_S(6)
#define ROW_GAP TR_S(12)
#define FN_GAP TR_S(12)   // 字母行两侧功能键和字母之间 / Between the side function keys and the letters
#define KB_EDGE 24        // 键盘左右边距，不放大 / Keyboard side margin; does not scale
#define KEY_GRAY 0xE0

enum {
    // 功能键编号从 256 起，不能和字符键（ASCII）撞上：原来从 100 起，正好是字母 d～j。
    // Function key ids start at 256 so they never collide with character keys; 100 used to equal letters d..j.
    K_FN_LEFT = 256, K_BACK, K_MODE, K_LANG, K_SPACE, K_GO, K_CLEAR,
};

static const char* const ROWS_LETTERS[3] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
// 数字符号布局；每个按键一个 ASCII 字符。/ Symbol layout; one ASCII character per key.
static const char* const ROWS_SYMBOLS[3] = {"1234567890", "-/:;()@&\"", ".,?!'_="};
static const char* const T9_LETTERS[10] = {"", "", "ABC", "DEF", "GHI", "JKL", "MNO", "PQRS", "TUV", "WXYZ"};

typedef struct {
    EpdRect r;
    int id;        // 字符键为字符本身，功能键为 K_* / Character keys use the char, function keys K_*
} kb_key_t;

EpdRect tr_kbd_area(void) { return (EpdRect){0, TR_KBD_TOP, TR_W, TR_H - TR_KBD_TOP}; }

static int row_y(int row) { return KEYS_Y + row * (KEY_H + ROW_GAP); }

// 字母键宽：放大后一行十个键放不下，就按屏宽均分（100% 时是 56）。
// Letter key width: when ten scaled keys no longer fit, share the screen width instead (56 at 100%).
static int key_w(void) {
    const int fit = (TR_W - 16 - 9 * KEY_GAP) / 10;
    return TR_S(56) < fit ? TR_S(56) : fit;
}

static bool composing_mode(const tr_kbd_t* kbd) { return kbd->mode == TR_KBD_PINYIN || kbd->mode == TR_KBD_T9; }

// 生成当前布局的全部按键。/ Build every key for the current layout.
static int layout(const tr_kbd_t* kbd, kb_key_t* keys) {
    int n = 0;
    if (kbd->mode == TR_KBD_T9) {
        // 3×3 宫格：左上是分词，其余是 2-9；右侧一列删除、清空。/ 3x3 grid: separator top-left, digits 2-9; delete and clear on the right.
        // 九键横向不放大（已占满屏宽），只变高。/ T9 widths stay as they are (already full width); only heights grow.
        const int w = 170, gap = 12, x0 = KB_EDGE;
        for (int i = 0; i < 9; i++) {
            EpdRect r = {x0 + (i % 3) * (w + gap), row_y(i / 3), w, KEY_H};
            keys[n++] = (kb_key_t){r, i == 0 ? K_FN_LEFT : '1' + i};
        }
        const int rx = x0 + 3 * (w + gap);
        keys[n++] = (kb_key_t){{rx, row_y(0), TR_W - KB_EDGE - rx, KEY_H}, K_BACK};
        keys[n++] = (kb_key_t){{rx, row_y(1), TR_W - KB_EDGE - rx, KEY_H}, K_CLEAR};
    } else {
        const char* const* rows = kbd->mode == TR_KBD_SYMBOL ? ROWS_SYMBOLS : ROWS_LETTERS;
        const int kw = key_w();
        int x3 = 0;   // 第三行第一个字母键的 x / x of the third row's first letter key
        for (int r = 0; r < 3; r++) {
            int count = (int)strlen(rows[r]);
            int total = count * kw + (count - 1) * KEY_GAP;
            int x0 = (TR_W - total) / 2;
            if (r == 2) x3 = x0;
            for (int i = 0; i < count; i++) {
                keys[n++] = (kb_key_t){{x0 + i * (kw + KEY_GAP), row_y(r), kw, KEY_H}, rows[r][i]};
            }
        }
        // 两侧功能键占满字母行留下的空（100% 时 92 宽）。/ The side keys fill what the letter row leaves (92 at 100%).
        const int fw = x3 - FN_GAP - KB_EDGE;
        keys[n++] = (kb_key_t){{KB_EDGE, row_y(2), fw, KEY_H}, K_FN_LEFT};
        keys[n++] = (kb_key_t){{TR_W - KB_EDGE - fw, row_y(2), fw, KEY_H}, K_BACK};
    }
    // 底行四个键按 120 : 120 : 200 : 160 分屏宽，间隙放大（100% 时正好是原来的宽度）。
    // The bottom row splits the width 120 : 120 : 200 : 160 with scaled gaps (exactly the old widths at 100%).
    const int gap = TR_S(12), avail = TR_W - 2 * KB_EDGE - 3 * gap;
    const int mode_w = avail * 120 / 600, go_w = avail * 160 / 600, space_w = avail - 2 * mode_w - go_w;
    const int x_lang = KB_EDGE + mode_w + gap, x_space = x_lang + mode_w + gap, x_go = x_space + space_w + gap;
    keys[n++] = (kb_key_t){{KB_EDGE, row_y(3), mode_w, KEY_H}, K_MODE};
    keys[n++] = (kb_key_t){{x_lang, row_y(3), mode_w, KEY_H}, K_LANG};
    keys[n++] = (kb_key_t){{x_space, row_y(3), space_w, KEY_H}, K_SPACE};
    keys[n++] = (kb_key_t){{x_go, row_y(3), TR_W - KB_EDGE - x_go, KEY_H}, K_GO};
    return n;
}

static const char* key_label(const tr_kbd_t* kbd, int id, char buf[4]) {
    switch (id) {
    case K_FN_LEFT: return composing_mode(kbd) ? "分词" : ".";
    case K_BACK: return "删除";
    case K_CLEAR: return "清空";
    case K_MODE:
        if (kbd->mode != TR_KBD_SYMBOL) return "123";
        return kbd->letters == TR_KBD_ENGLISH ? "abc" : "拼音";
    case K_LANG: {
        // 数字符号下显示切回后的语言。/ In symbol mode, show the language it returns to.
        tr_kbd_mode_t m = kbd->mode == TR_KBD_SYMBOL ? kbd->letters : kbd->mode;
        return m == TR_KBD_PINYIN ? "全拼" : m == TR_KBD_T9 ? "九键" : "英文";
    }
    case K_SPACE: return "空格";
    case K_GO: return kbd->go_label ? kbd->go_label : "确定";
    default:
        if (kbd->mode == TR_KBD_T9 && id >= '2' && id <= '9') return T9_LETTERS[id - '0'];
        buf[0] = (char)id;
        buf[1] = 0;
        return buf;
    }
}

/* ---- 候选 / Candidates ---- */

static void refresh_cands(tr_kbd_t* kbd) {
    kbd->cand_count = 0;
    kbd->cand_first = 0;
    if (composing_mode(kbd) && kbd->ime_ok && kbd->comp[0]) {
        // 内存紧张时词库可能被 mem_reclaim 放掉了，用时再读回来。/ mem_reclaim may have dropped the lexicon when
        // memory ran short; read it back when needed.
        if (!pinyin_ime_ready()) pinyin_ime_load(NULL);
        kbd->cand_count = pinyin_ime_query(kbd->comp, kbd->mode == TR_KBD_T9 ? PINYIN_IME_T9 : PINYIN_IME_QWERTY,
                                           kbd->cands, TR_KBD_CANDS);
    }
}

// 从 cand_first 开始能放下几个；more 表示前后还有。/ How many fit from cand_first; more if others exist.
static int cand_visible(const tr_kbd_t* kbd, int xs[], int ws[], bool* more) {
    int x = TR_MARGIN, n = 0;
    const int limit = TR_W - TR_MARGIN - TR_S(60);
    for (int i = kbd->cand_first; i < kbd->cand_count; i++) {
        int w = ttf_text_width_px(CAND_PX, kbd->cands[i].text) + TR_S(48);
        if (x + w > limit && n) break;
        if (x + w > limit) w = limit - x;   // 一个就太宽：截到可用宽度 / A single over-wide one is clipped
        xs[n] = x;
        ws[n] = w;
        x += w;
        n++;
    }
    *more = kbd->cand_first + n < kbd->cand_count || kbd->cand_first > 0;
    return n;
}

static bool append_text(tr_kbd_t* kbd, const char* s) {
    size_t have = strlen(kbd->text), add = strlen(s);
    if (have + add >= sizeof(kbd->text)) return false;
    memcpy(kbd->text + have, s, add + 1);
    return true;
}

// 输入框满了就不消耗编码串，免得字丢掉。/ When the field is full, keep the composition so nothing is lost.
static void pick(tr_kbd_t* kbd, int index) {
    if (index < 0 || index >= kbd->cand_count) return;
    if (!append_text(kbd, kbd->cands[index].text)) return;
    size_t len = strlen(kbd->comp), used = kbd->cands[index].consumed;
    if (used > len) used = len;
    memmove(kbd->comp, kbd->comp + used, len - used + 1);
    refresh_cands(kbd);
}

// 有编码串时按空格或确认：选第一个候选；全拼没有候选就把字母原样上屏，九键直接丢弃数字。
// Space or Go with a composition: take the first candidate; with none, full pinyin commits the letters and T9 drops its digits.
static void commit_comp(tr_kbd_t* kbd) {
    // 一个候选可能只吃掉一部分编码，连续选到用完。/ One candidate may consume only part; keep picking until done.
    for (int guard = 0; kbd->comp[0] && kbd->cand_count && guard < PINYIN_IME_MAX_INPUT; guard++) {
        size_t before = strlen(kbd->comp);
        pick(kbd, 0);
        if (strlen(kbd->comp) == before) break;   // 输入框满了 / Field is full
    }
    if (!kbd->comp[0]) return;
    if (kbd->mode == TR_KBD_PINYIN) {
        for (char* p = kbd->comp; *p; p++) if (*p != '\'') { char c[2] = {*p, 0}; append_text(kbd, c); }
    }
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
    kbd->want_ime = chinese;
    kbd->ime_ok = chinese && pinyin_ime_load(NULL);
    kbd->mode = kbd->ime_ok ? TR_KBD_PINYIN : TR_KBD_ENGLISH;
    kbd->letters = kbd->mode;
}

/* ---- 绘制 / Drawing ---- */

// 键宽受屏宽限制：放大后字比键宽就缩一点字号，不截断（100% 时都放得下，不变）。
// Key widths are capped by the screen: a scaled label wider than its key steps its size down instead of being cut
// (everything fits at 100%, so nothing changes there).
static int label_px(const char* label, int px, int key_w) {
    while (px > 20 && ttf_text_width_px(px, label) > key_w - TR_S(8)) px -= 2;
    return px;
}

void tr_kbd_draw(uint8_t* fb, const tr_kbd_t* kbd) {
    epd_fill_rect(tr_kbd_area(), UI_GRAY_WHITE, fb);
    tr_hline(fb, TR_KBD_TOP, 0, TR_W, false);
    if (composing_mode(kbd)) {
        tr_text(fb, TR_MARGIN, COMP_Y, TR_S(30), kbd->comp, EPD_DRAW_ALIGN_LEFT, true);
        int xs[TR_KBD_CANDS], ws[TR_KBD_CANDS];
        bool more = false;
        int n = cand_visible(kbd, xs, ws, &more);
        for (int i = 0; i < n; i++) {
            const pinyin_ime_cand_t* c = &kbd->cands[kbd->cand_first + i];
            if (i) epd_fill_rect((EpdRect){xs[i], CAND_Y + TR_S(16), 2, CAND_H - TR_S(32)}, TR_GRAY_LINE, fb);
            char shown[PINYIN_IME_CAND_TEXT];
            memcpy(shown, c->text, sizeof(shown));
            tr_fit(shown, CAND_PX, ws[i] - TR_S(24));
            tr_text(fb, xs[i] + ws[i] / 2, CAND_Y + (CAND_H - CAND_PX) / 2 - TR_S(2), CAND_PX, shown,
                    EPD_DRAW_ALIGN_CENTER, i == 0 && kbd->cand_first == 0);
        }
        if (more) tr_text(fb, TR_W - TR_MARGIN, CAND_Y + TR_S(14), CAND_PX, "›", EPD_DRAW_ALIGN_RIGHT, false);
        if (!kbd->comp[0]) {
            tr_text(fb, TR_MARGIN, CAND_Y + TR_S(18), TR_S(28),
                    kbd->mode == TR_KBD_T9 ? "按字母所在的键，点候选上屏" : "输入拼音，点候选上屏", EPD_DRAW_ALIGN_LEFT,
                    false);
        }
    } else if (kbd->want_ime && !kbd->ime_ok && kbd->mode == TR_KBD_ENGLISH) {
        // 放大后一行放不下就分两行，仍在候选条里。/ When too wide once scaled it takes two lines, still in the bar.
        tr_message(fb, TR_MARGIN, CAND_Y + TR_S(18), CAND_Y + TR_S(6), TR_W - 2 * TR_MARGIN, TR_S(26),
                   "没有拼音词库：把 ime/pinyin-v1.bin 放进 TF 卡", EPD_DRAW_ALIGN_LEFT, false);
    }
    tr_hline(fb, CAND_Y + CAND_H + TR_S(10), TR_MARGIN, TR_W - 2 * TR_MARGIN, true);

    kb_key_t keys[48];
    int n = layout(kbd, keys);
    char buf[4];
    for (int i = 0; i < n; i++) {
        const kb_key_t* k = &keys[i];
        const char* label = key_label(kbd, k->id, buf);
        const int cx = k->r.x + k->r.width / 2;
        if (k->id == K_GO) {
            ui_fill_round_rect(fb, k->r, TR_S(12), UI_GRAY_BLACK);
            const int px = label_px(label, TR_S(30), k->r.width);
            tr_text_inv(fb, cx, k->r.y + (k->r.height - px) / 2 - TR_S(2), px, label, EPD_DRAW_ALIGN_CENTER, true);
        } else if (k->id >= K_FN_LEFT && k->id != K_SPACE) {
            ui_draw_round_rect(fb, k->r, TR_S(12), UI_GRAY_BLACK);
            ui_draw_round_rect(fb, ui_inset_rect(k->r, 1), TR_S(12) - 1, UI_GRAY_BLACK);
            const int px = label_px(label, TR_S(28), k->r.width);
            tr_text(fb, cx, k->r.y + (k->r.height - px) / 2 - TR_S(2), px, label, EPD_DRAW_ALIGN_CENTER, false);
        } else {
            ui_fill_round_rect(fb, k->r, TR_S(12), KEY_GRAY);
            bool t9 = kbd->mode == TR_KBD_T9 && k->id != K_SPACE;
            const int px = label_px(label, k->id == K_SPACE || t9 ? TR_S(30) : TR_S(36), k->r.width);
            tr_text(fb, cx, k->r.y + (k->r.height - px) / 2 - TR_S(2), px, label, EPD_DRAW_ALIGN_CENTER, t9);
        }
    }
}

/* ---- 触摸 / Touch ---- */

static void next_language(tr_kbd_t* kbd) {
    commit_comp(kbd);
    kbd->comp[0] = 0;
    // 用到拼音才加载词库（书库搜索默认英文）。/ Load the lexicon only when pinyin is needed (library search starts in English).
    if (!kbd->ime_ok && kbd->mode == TR_KBD_ENGLISH) {
        kbd->want_ime = true;
        kbd->ime_ok = pinyin_ime_load(NULL);
    }
    if (kbd->mode == TR_KBD_PINYIN) kbd->mode = TR_KBD_T9;
    else if (kbd->mode == TR_KBD_T9) kbd->mode = TR_KBD_ENGLISH;
    else kbd->mode = kbd->ime_ok ? TR_KBD_PINYIN : TR_KBD_ENGLISH;
    kbd->letters = kbd->mode;
    refresh_cands(kbd);
}

tr_kbd_result_t tr_kbd_touch(tr_kbd_t* kbd, uint16_t x, uint16_t y) {
    if (y < TR_KBD_TOP) return TR_KBD_NONE;
    // 候选条。/ Candidate bar.
    if (y >= CAND_Y && y < CAND_Y + CAND_H && composing_mode(kbd) && kbd->cand_count) {
        int xs[TR_KBD_CANDS], ws[TR_KBD_CANDS];
        bool more = false;
        int n = cand_visible(kbd, xs, ws, &more);
        if (more && x >= TR_W - TR_MARGIN - TR_S(60)) {
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
    if (best < 0 || best_d > TR_S(12) * TR_S(12)) return TR_KBD_NONE;
    int id = keys[best].id;
    switch (id) {
    case K_FN_LEFT:
        if (composing_mode(kbd)) {
            size_t len = strlen(kbd->comp);
            if (len && kbd->comp[len - 1] != '\'' && len + 1 < sizeof(kbd->comp)) { kbd->comp[len] = '\''; kbd->comp[len + 1] = 0; }
            refresh_cands(kbd);
        } else {
            append_text(kbd, ".");
        }
        break;
    case K_BACK:
        if (composing_mode(kbd) && kbd->comp[0]) {
            kbd->comp[strlen(kbd->comp) - 1] = 0;
            refresh_cands(kbd);
        } else {
            backspace_text(kbd);
        }
        break;
    case K_CLEAR:
        kbd->comp[0] = 0;
        refresh_cands(kbd);
        break;
    case K_MODE:
        commit_comp(kbd);
        kbd->comp[0] = 0;
        kbd->mode = kbd->mode == TR_KBD_SYMBOL ? kbd->letters : TR_KBD_SYMBOL;
        refresh_cands(kbd);
        break;
    case K_LANG:
        if (kbd->mode == TR_KBD_SYMBOL) kbd->mode = kbd->letters;
        next_language(kbd);
        break;
    case K_SPACE:
        if (composing_mode(kbd) && kbd->comp[0]) commit_comp(kbd);
        else append_text(kbd, " ");
        break;
    case K_GO:
        commit_comp(kbd);
        return TR_KBD_SUBMIT;
    default:
        if (composing_mode(kbd)) {
            // 只数字母，分词符不占名额。/ Count letters only; separators don't use up the limit.
            size_t len = strlen(kbd->comp), letters = 0;
            for (size_t i = 0; i < len; i++) letters += kbd->comp[i] != '\'';
            if (letters < PINYIN_IME_MAX_INPUT && len + 1 < sizeof(kbd->comp)) { kbd->comp[len] = (char)id; kbd->comp[len + 1] = 0; }
            refresh_cands(kbd);
        } else {
            char c[2] = {(char)id, 0};
            append_text(kbd, c);
        }
        break;
    }
    return TR_KBD_CHANGED;
}
