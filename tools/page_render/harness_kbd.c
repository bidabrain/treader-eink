/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：屏幕键盘单独的渲染测试：全拼、九键、英文、数字符号、候选条和缺词库提示，上面画一条输入框。
 * English: Render bench for the on-screen keyboard alone: full pinyin, T9, English, symbols, the candidate bar and
 * the missing-lexicon hint, under a plain input line.
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "tr_kbd.c"   // 按 -I 找：当前源码或 SRC_ROOT / Found via -I: current sources or SRC_ROOT
#include "page_render.h"

const char* hr_page = "kbd";

static tr_kbd_t s_k;

// 输入框用固定坐标：测的是键盘，不是页面。/ The field uses fixed coordinates: this benches the keyboard, not a page.
static void hr_field(const char* text) {
    ui_clear_page(hr_fb);
    tr_status_bar(hr_fb);
    ui_draw_round_rect(hr_fb, (EpdRect){TR_MARGIN, 120, TR_W - 2 * TR_MARGIN, 84}, 14, TR_GRAY_OUTLINE);
    tr_text(hr_fb, TR_MARGIN + 20, 142, 32, text, EPD_DRAW_ALIGN_LEFT, false);
}

static void hr_open(tr_kbd_mode_t mode, bool ime_ok, const char* go) {
    memset(&s_k, 0, sizeof(s_k));
    s_k.mode = s_k.letters = mode;
    s_k.ime_ok = ime_ok;
    s_k.want_ime = true;
    s_k.go_label = go;
}

static void hr_cands(const char* comp, int n) {
    static const char* const WORDS[] = {"你好", "拟好", "你", "尼", "泥", "逆", "腻", "倪", "妮", "霓", "匿", "溺"};
    snprintf(s_k.comp, sizeof(s_k.comp), "%s", comp);
    s_k.cand_count = n;
    for (int i = 0; i < n; i++) {
        snprintf(s_k.cands[i].text, sizeof(s_k.cands[i].text), "%s", WORDS[i % 12]);
        s_k.cands[i].consumed = (uint8_t)strlen(comp);
    }
}

void hr_scenes(void) {
    hr_open(TR_KBD_PINYIN, true, "搜索");
    hr_field("三体");
    tr_kbd_draw(hr_fb, &s_k);
    hr_no_bar_check();
    hr_shot("pinyin_empty");

    hr_open(TR_KBD_PINYIN, true, "搜索");
    hr_cands("nihao", 12);
    hr_field("三体");
    tr_kbd_draw(hr_fb, &s_k);
    hr_no_bar_check();
    hr_shot("pinyin_cands");

    hr_open(TR_KBD_PINYIN, true, "搜索");
    hr_cands("nihao", 12);
    s_k.cand_first = 6;
    hr_field("三体");
    tr_kbd_draw(hr_fb, &s_k);
    hr_no_bar_check();
    hr_shot("pinyin_cands_more");

    hr_open(TR_KBD_T9, true, "搜索");
    hr_field("");
    tr_kbd_draw(hr_fb, &s_k);
    hr_no_bar_check();
    hr_shot("t9_empty");

    hr_open(TR_KBD_T9, true, "搜索");
    hr_cands("64426", 5);
    hr_field("");
    tr_kbd_draw(hr_fb, &s_k);
    hr_no_bar_check();
    hr_shot("t9_cands");

    hr_open(TR_KBD_ENGLISH, true, "下一步");
    hr_field("http://192.168.1.10");
    tr_kbd_draw(hr_fb, &s_k);
    hr_no_bar_check();
    hr_shot("english");

    hr_open(TR_KBD_ENGLISH, false, "保存");
    hr_field("abc");
    tr_kbd_draw(hr_fb, &s_k);
    hr_no_bar_check();
    hr_shot("english_no_lexicon");

    hr_open(TR_KBD_SYMBOL, true, "确定");
    s_k.letters = TR_KBD_PINYIN;
    hr_field("123");
    tr_kbd_draw(hr_fb, &s_k);
    hr_no_bar_check();
    hr_shot("symbols_pinyin");

    hr_open(TR_KBD_SYMBOL, true, NULL);
    s_k.letters = TR_KBD_ENGLISH;
    hr_field("123");
    tr_kbd_draw(hr_fb, &s_k);
    hr_no_bar_check();
    hr_shot("symbols_english");
}
