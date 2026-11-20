/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 定时自动翻页的档位、调节行和全局标记。/ Auto page turn levels, adjust row and global flag.
 */
#include "auto_turn.h"

#include <stdio.h>

#include "tr_ui.h"
#include "ui_kit.h"

// 按钮、字号随显示大小放大；“−”的左边不动，标签留在两个按钮中间。/ Buttons and text follow the display size; the
// minus button's left edge stays put and the label sits between the buttons.
#define MINUS_X 300
#define BTN_W AUTO_TURN_BTN_W
#define PLUS_X (TR_W - TR_MARGIN - BTN_W)

static volatile bool s_active;

int32_t auto_turn_ms(int level) {
    if (level <= 0) return 0;
    if (level > AUTO_TURN_LEVELS) level = AUTO_TURN_LEVELS;
    return level * AUTO_TURN_STEP_S * 1000;
}

int auto_turn_step(int level, int dir) {
    level += dir;
    if (level < 0) level = 0;
    if (level > AUTO_TURN_LEVELS) level = AUTO_TURN_LEVELS;
    return level;
}

void auto_turn_draw_row(uint8_t* fb, int y, int level) {
    tr_text(fb, TR_MARGIN, y + TR_S(14), TR_S(28), "自动翻页", EPD_DRAW_ALIGN_LEFT, false);
    tr_pill(fb, (EpdRect){MINUS_X, y, BTN_W, AUTO_TURN_ROW_H}, "−", false, TR_S(34));
    tr_pill(fb, (EpdRect){PLUS_X, y, BTN_W, AUTO_TURN_ROW_H}, "+", false, TR_S(34));
    char label[16];
    if (level > 0) snprintf(label, sizeof(label), "%d 秒", level * AUTO_TURN_STEP_S);
    else snprintf(label, sizeof(label), "关");
    tr_text(fb, (MINUS_X + BTN_W + PLUS_X) / 2, y + TR_S(10), TR_S(34), label, EPD_DRAW_ALIGN_CENTER, level > 0);
}

int auto_turn_row_hit(int y, uint16_t tx, uint16_t ty) {
    if (ty < y - TR_S(8) || ty >= y + AUTO_TURN_ROW_H + TR_S(8)) return 0;
    if (tx >= MINUS_X - 10 && tx < MINUS_X + BTN_W + 10) return -1;
    if (tx >= PLUS_X - 10 && tx < PLUS_X + BTN_W + 10) return 1;
    return 0;
}

void auto_turn_set_active(bool active) { s_active = active; }
bool auto_turn_active(void) { return s_active; }
