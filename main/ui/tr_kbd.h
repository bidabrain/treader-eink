/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 屏幕键盘：26 键拼音、九键拼音、英文、数字符号四种布局，带候选条。占屏幕下半部分（TR_KBD_TOP 以下）。
 * 输入框由页面自己画；键盘只管按键、编码串和候选。
 *
 * On-screen keyboard: full pinyin, T9 pinyin, English and symbol layouts with a candidate bar, occupying the
 * lower screen from TR_KBD_TOP. The page draws its own field; the keyboard owns keys,
 * composition and candidates.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "epdiy.h"
#include "pinyin_ime.h"
#include "ui_scale.h"

#ifdef __cplusplus
extern "C" {
#endif

/// 键盘顶边：按键随显示大小变高，键盘往上长；底下 60 像素留白不放大（100% 时 580）。
/// Keyboard top edge: keys grow taller with the display size, so the keyboard grows upward; the 60 px bottom margin
/// does not scale (580 at 100%).
static inline int tr_kbd_top(void) { return 1216 - 60 - UI_S(576); }
#define TR_KBD_TOP tr_kbd_top()
#define TR_KBD_TEXT_MAX 128
#define TR_KBD_CANDS 24

typedef enum { TR_KBD_PINYIN = 0, TR_KBD_T9, TR_KBD_ENGLISH, TR_KBD_SYMBOL } tr_kbd_mode_t;

typedef enum {
    TR_KBD_NONE = 0,  ///< 没点中 / Nothing hit
    TR_KBD_CHANGED,   ///< 内容变了，重画键盘和输入框 / Content changed; redraw keyboard and field
    TR_KBD_SUBMIT,    ///< 按了确认键 / Go key pressed
} tr_kbd_result_t;

typedef struct {
    char text[TR_KBD_TEXT_MAX];            ///< 已上屏的文字 / Committed text
    char comp[PINYIN_IME_MAX_INPUT + 8];   ///< 拼音编码串 / Pinyin composition
    tr_kbd_mode_t mode;
    tr_kbd_mode_t letters;                 ///< 从数字符号切回时用的布局 / Layout to return to from symbols
    bool ime_ok;                           ///< 拼音词库可用 / Pinyin lexicon loaded
    bool want_ime;                         ///< 调用方要中文输入 / Caller asked for Chinese input
    pinyin_ime_cand_t cands[TR_KBD_CANDS];
    int cand_count;
    int cand_first;                        ///< 候选条第一个显示的下标 / First visible candidate
    const char* go_label;                  ///< 确认键文字 / Go key label
} tr_kbd_t;

/// 打开键盘；chinese 为真时先尝试载入拼音词库，失败退到英文。
/// Open the keyboard; with chinese, try to load the pinyin lexicon and fall back to English.
void tr_kbd_open(tr_kbd_t* kbd, const char* initial, bool chinese, const char* go_label);
void tr_kbd_draw(uint8_t* fb, const tr_kbd_t* kbd);
tr_kbd_result_t tr_kbd_touch(tr_kbd_t* kbd, uint16_t x, uint16_t y);
/// 键盘区域（局部刷新用）。/ Keyboard area for partial refresh.
EpdRect tr_kbd_area(void);

#ifdef __cplusplus
}
#endif
