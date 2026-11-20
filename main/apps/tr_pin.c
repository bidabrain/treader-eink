/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 4 位数字锁屏密码的键盘和密码关。键盘整屏自己画、自己读触摸，不经过主循环，所以过关之前任何页面和按键都碰不到。
 * 第一帧 GC16 整屏，之后每输一位只用 DU 刷上面的提示和四个格子。
 *
 * English: keypad and gate for the 4-digit lock PIN. The keypad draws the whole screen and reads touch itself,
 * outside the main loop, so no page or key can be reached before passing. The first frame is a full GC16; each
 * digit afterwards refreshes only the hint and the four boxes with DU.
 *
 * 冻结：密码只经 settings 的加盐哈希比对，这里不保存、不打印数字；设备上没有绕过密码的入口。
 * Frozen: the PIN is checked only through the salted hash in settings; digits are never kept or logged here; the
 * device offers no way around the PIN.
 */

#include "tr_pin.h"

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "display.h"
#include "e0470_epaper_waveform.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "read_pico_pmu.h"
#include "settings.h"
#include "sleep.h"
#include "tr_ui.h"
#include "ui_kit.h"

#define TAG "tr_pin"
#define POLL_MS 20
#define GATE_TIMEOUT_MS 30000
// 输错这么多次以后开始要等，每多错一次多等 30 秒，最多 5 分钟。/ After this many wrong tries a wait begins, 30 s
// longer per further miss, 5 minutes at most.
#define FREE_TRIES 5
#define WAIT_STEP_S 30
#define WAIT_MAX_S 300

// 版面（竖屏 684×1216）。/ Layout (portrait 684x1216).
#define TITLE_Y 190
#define HINT_Y 262
#define BOX_W 80
#define BOX_H 96
#define BOX_GAP 24
#define BOX_Y 330
#define BOX_X ((TR_W - 4 * BOX_W - 3 * BOX_GAP) / 2)
#define KEY_W 180
#define KEY_H 150
#define KEY_GAP 24
#define KEY_X ((TR_W - 3 * KEY_W - 2 * KEY_GAP) / 2)
#define KEY_Y 520
// 每输一位要重刷的一块：提示行加四个格子。/ The area redrawn per digit: the hint line and the four boxes.
#define TOP_AREA ((EpdRect){0, HINT_Y - 10, TR_W, BOX_Y + BOX_H + 20 - (HINT_Y - 10)})

// 键位：0～9 是数字，10 取消，11 删除，-1 空。/ Keys: 0-9 digits, 10 cancel, 11 delete, -1 none.
static const int8_t KEYS[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0, 11};

static int s_fails;   // 本次开机累计输错 / Wrong tries since boot

static EpdRect key_rect(int i) {
    return (EpdRect){KEY_X + (i % 3) * (KEY_W + KEY_GAP), KEY_Y + (i / 3) * (KEY_H + KEY_GAP), KEY_W, KEY_H};
}

static void draw_top(uint8_t* fb, const char* hint, int n) {
    epd_fill_rect(TOP_AREA, UI_GRAY_WHITE, fb);
    // 这一块每输一位按 DU 局部刷新，只用深灰和黑。/ This block is DU-refreshed per digit: dark gray and black only.
    if (hint && hint[0]) tr_text_dim(fb, TR_W / 2, HINT_Y, 26, hint, EPD_DRAW_ALIGN_CENTER);
    for (int i = 0; i < 4; i++) {
        const EpdRect r = {BOX_X + i * (BOX_W + BOX_GAP), BOX_Y, BOX_W, BOX_H};
        ui_draw_round_rect(fb, r, 10, TR_GRAY_OUTLINE);
        ui_draw_round_rect(fb, ui_inset_rect(r, 1), 9, TR_GRAY_OUTLINE);
        if (i < n) epd_fill_circle(r.x + BOX_W / 2, r.y + BOX_H / 2, 14, UI_GRAY_BLACK, fb);
    }
}

static void draw_all(uint8_t* fb, const char* title, const char* hint, int n, bool cancel) {
    epd_fill_rect((EpdRect){0, 0, TR_W, TR_H}, UI_GRAY_WHITE, fb);
    tr_text(fb, TR_W / 2, TITLE_Y, 40, title, EPD_DRAW_ALIGN_CENTER, true);
    draw_top(fb, hint, n);
    for (int i = 0; i < 12; i++) {
        const int k = KEYS[i];
        if (k == 10 && !cancel) continue;
        // Material 3 数字键盘：数字键是浅灰圆角块（键盘只在整屏刷新时画），取消和删除是深灰描边。
        // Material 3 keypad: digits sit on light gray rounded keys (the pad is only drawn on full refreshes); cancel
        // and delete are dark gray outlines.
        const EpdRect r = key_rect(i);
        if (k <= 9) {
            ui_fill_round_rect(fb, r, 24, TR_GRAY_SURFACE);
        } else {
            ui_draw_round_rect(fb, r, 24, TR_GRAY_OUTLINE);
            ui_draw_round_rect(fb, ui_inset_rect(r, 1), 23, TR_GRAY_OUTLINE);
        }
        char label[8];
        if (k <= 9) snprintf(label, sizeof(label), "%d", k);
        else snprintf(label, sizeof(label), "%s", k == 10 ? "取消" : "删除");
        const int px = k <= 9 ? 56 : 34;
        tr_text_on(fb, r.x + KEY_W / 2, r.y + (KEY_H - px) / 2 - 4, px, label, EPD_DRAW_ALIGN_CENTER, k <= 9,
                   UI_GRAY_BLACK, k <= 9 ? TR_GRAY_SURFACE : UI_GRAY_WHITE);
    }
}

static void push_top(EpdiyHighlevelState* hl) {
    guard_draw_result(hl, update_display_area_with(hl, &E0470_WAVEFORM, MODE_DU, TOP_AREA));
}

static int key_at(uint16_t x, uint16_t y, bool cancel) {
    for (int i = 0; i < 12; i++) {
        const EpdRect r = key_rect(i);
        if (x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height) {
            return KEYS[i] == 10 && !cancel ? -1 : KEYS[i];
        }
    }
    return -1;
}

tr_pin_result_t tr_pin_pad(EpdiyHighlevelState* hl, cst836u_handle_t tp, const char* title, const char* hint,
                           bool cancel, int timeout_ms, int64_t ignore_power_until_ms, char out[5]) {
    uint8_t* fb = epd_hl_get_framebuffer(hl);
    int n = 0;
    memset(out, 0, 5);
    draw_all(fb, title, hint, 0, cancel);
    guard_draw_result(hl, update_display_full(hl));
    bool was = false;
    int64_t last = esp_timer_get_time() / 1000;
    int64_t last_key_poll = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        const int64_t now = esp_timer_get_time() / 1000;
        rails_idle_check(now);
        if (read_pico_pmu_ready() && now - last_key_poll >= APP_LOCK_POLL_MS) {
            last_key_poll = now;
            if (read_pico_pmu_take_key_short() && now >= ignore_power_until_ms) {
                memset(out, 0, 5);
                return TR_PIN_POWER;
            }
        }
        if (timeout_ms > 0 && now - last >= timeout_ms) {
            memset(out, 0, 5);
            return TR_PIN_TIMEOUT;
        }
        cst836u_touch_t t = {0};
        if (cst836u_read(tp, &t) != ESP_OK) continue;
        const bool pressed = t.touched && !was;
        was = t.touched;
        if (!pressed) continue;
        last = now;
        const int k = key_at(t.x, t.y, cancel);
        if (k < 0) continue;
        if (k == 10) {
            memset(out, 0, 5);
            return TR_PIN_CANCELLED;
        }
        if (k == 11) {
            if (n == 0) continue;
            out[--n] = 0;
        } else {
            out[n++] = (char)('0' + k);
        }
        draw_top(fb, hint, n);
        push_top(hl);
        if (n == 4) return TR_PIN_ENTERED;
    }
}

// 输错太多：整屏显示还要等几秒，倒计时到了才回去。电源键照样能锁屏。返回是否被电源键打断。
// Too many misses: show how long to wait and count down before returning. The power key still locks. Returns whether
// the power key cut it short.
static bool wait_penalty(EpdiyHighlevelState* hl, int seconds, int64_t ignore_power_until_ms) {
    uint8_t* fb = epd_hl_get_framebuffer(hl);
    int shown = -1;
    const int64_t end = esp_timer_get_time() / 1000 + (int64_t)seconds * 1000;
    int64_t last_key_poll = 0;
    for (;;) {
        const int64_t now = esp_timer_get_time() / 1000;
        if (now >= end) return false;
        rails_idle_check(now);
        if (read_pico_pmu_ready() && now - last_key_poll >= APP_LOCK_POLL_MS) {
            last_key_poll = now;
            if (read_pico_pmu_take_key_short() && now >= ignore_power_until_ms) return true;
        }
        // 每 5 秒更新一次剩余时间。/ Update the remaining time every 5 seconds.
        const int left = (int)((end - now + 999) / 1000);
        const int bucket = (left + 4) / 5;
        if (bucket != shown) {
            shown = bucket;
            char line[64];
            snprintf(line, sizeof(line), "输错太多次，%d 秒后再试", left);
            const EpdRect area = {0, TITLE_Y - 20, TR_W, 140};
            if (shown * 5 >= seconds - 4) {
                epd_fill_rect((EpdRect){0, 0, TR_W, TR_H}, UI_GRAY_WHITE, fb);
                tr_text(fb, TR_W / 2, TITLE_Y, 40, "已锁定", EPD_DRAW_ALIGN_CENTER, true);
                tr_text(fb, TR_W / 2, TITLE_Y + 72, 28, line, EPD_DRAW_ALIGN_CENTER, false);
                guard_draw_result(hl, update_display_full(hl));
            } else {
                epd_fill_rect(area, UI_GRAY_WHITE, fb);
                tr_text(fb, TR_W / 2, TITLE_Y, 40, "已锁定", EPD_DRAW_ALIGN_CENTER, true);
                tr_text(fb, TR_W / 2, TITLE_Y + 72, 28, line, EPD_DRAW_ALIGN_CENTER, false);
                guard_draw_result(hl, update_display_area_with(hl, &E0470_WAVEFORM, MODE_DU, area));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void tr_pin_gate(EpdiyHighlevelState* hl, cst836u_handle_t tp, sc7a20h_handle_t acc, int64_t* ignore_until_ms) {
    if (!app_settings_pin_enabled()) return;
    char hint[64] = "";
    for (;;) {
        const int64_t ignore = ignore_until_ms ? *ignore_until_ms : 0;
        if (s_fails >= FREE_TRIES) {
            int wait = (s_fails - FREE_TRIES + 1) * WAIT_STEP_S;
            if (wait > WAIT_MAX_S) wait = WAIT_MAX_S;
            if (wait_penalty(hl, wait, ignore)) {
                enter_lock_and_sleep(hl, ignore_until_ms, acc);
                hint[0] = 0;
                continue;
            }
        }
        char pin[5];
        const tr_pin_result_t r = tr_pin_pad(hl, tp, "输入密码解锁", hint, false, GATE_TIMEOUT_MS, ignore, pin);
        if (r == TR_PIN_ENTERED) {
            const bool ok = app_settings_pin_check(pin);
            memset(pin, 0, sizeof(pin));
            if (ok) {
                s_fails = 0;
                ESP_LOGI(TAG, "unlocked");
                return;
            }
            s_fails++;
            ESP_LOGW(TAG, "wrong PIN (%d)", s_fails);
            if (s_fails < FREE_TRIES) snprintf(hint, sizeof(hint), "密码不对，还能试 %d 次", FREE_TRIES - s_fails);
            else snprintf(hint, sizeof(hint), "密码不对");
            continue;
        }
        // 太久没输或按了电源键：回到锁屏睡眠，醒来再要密码。/ No input for long, or the power key: back to the
        // locked sleep; waking asks again.
        hint[0] = 0;
        enter_lock_and_sleep(hl, ignore_until_ms, acc);
    }
}
