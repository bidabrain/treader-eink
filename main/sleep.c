/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 锁屏进睡、浅睡等待按键或拿起、软睡/关机拉掉 EN。
 *
 * Enter lock and sleep, light-sleep wait for key or pickup, and drop EN
 * for soft sleep / power-off.
 */

#include "tr_lock.h"
#include "sleep.h"

#include "app.h"
#include "display.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_private/sleep_cpu.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pmu_selftest.h"
#include "read_pico_board.h"
#include "read_pico_buzzer.h"
#include "read_pico_pmu.h"
#include "read_pico_pmu_protocol.h"
#include "sc7a20h_lab.h"
#include "settings.h"
#include "tomato.h"
#include "tr_auth.h"
#include "ui_kit.h"

static const char* TAG = "read_pico";

// 主机下电后等 EN 落下的上限；PMU 确认后约 100 ms 拉低，超时视为失败。
// Upper bound for EN to fall after power-down; the PMU drops it about 100 ms after acknowledging.
#define HOST_EN_DROP_WAIT_MS 5000

extern const uint8_t lock_4bpp_bin_start[] asm("_binary_lock_4bpp_bin_start");

static cst836u_handle_t s_tp;
static sc7a20h_handle_t s_acc;
static bool s_acc_was_on;

void app_sleep_bind(cst836u_handle_t tp, sc7a20h_handle_t acc) {
    s_tp = tp;
    s_acc = acc;
    // 没开 PM 时 IDF 不分配 CPU 保存区，浅睡里 CPU 不会掉电；这里手动申请一次。
    // Without PM, IDF never allocates CPU retention, so light sleep keeps the CPU powered; request it once here.
    esp_err_t err = sleep_cpu_configure(true);
    if (err != ESP_OK) ESP_LOGW(TAG, "CPU light-sleep power-down off: %s", esp_err_to_name(err));
}

bool app_lock_powers_down(void) {
    return app_settings_sleep_mode() != APP_SLEEP_LIGHT;
}

// 触摸、加速度计都在常开 3V3 上，ESP 浅睡或 EN 拉低都不会断它们的电，必须主动送进最低功耗态。
// Touch and accelerometer sit on the always-on 3V3; neither light sleep nor EN low cuts them, so park them explicitly.
static void periph_suspend(bool host_off) {
    if (read_pico_buzzer_score_busy()) (void)read_pico_buzzer_score_stop();
    esp_err_t ioe = read_pico_ioe_sleep(host_off);
    if (ioe != ESP_OK) ESP_LOGW(TAG, "ioe sleep %s", esp_err_to_name(ioe));
    s_acc_was_on = sc7a20h_powered(s_acc);
    if (s_acc_was_on) (void)sc7a20h_power_down(s_acc);
    if (s_tp != NULL && cst836u_get_mode(s_tp) != CST836U_MODE_DEEPSLEEP) {
        esp_err_t err = cst836u_set_mode(s_tp, CST836U_MODE_DEEPSLEEP);
        if (err != ESP_OK) ESP_LOGW(TAG, "touch deepsleep %s", esp_err_to_name(err));
    }
}

// 深睡的触摸只认 TP_RST 复位；加速度计按睡前的配置重新上电。
// Deep-sleeping touch only wakes via TP_RST; the accelerometer comes back with its pre-sleep config.
static void periph_resume(void) {
    (void)read_pico_ioe_wake();
    if (s_tp != NULL && cst836u_get_mode(s_tp) == CST836U_MODE_DEEPSLEEP) {
        esp_err_t err = cst836u_wake(s_tp);
        if (err != ESP_OK) ESP_LOGW(TAG, "touch wake %s", esp_err_to_name(err));
    }
    if (s_acc_was_on) (void)sc7a20h_power_up(s_acc);
    s_acc_was_on = false;
}

static void lock_arm_ioe_wakeup(void) {
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << READ_PICO_IOE_INT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    read_pico_clear_ioe_int();
    gpio_wakeup_enable((gpio_num_t)READ_PICO_IOE_INT_GPIO, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
}

void app_lock_wait_key_idle(int timeout_ms) {
    int64_t start = esp_timer_get_time() / 1000;
    int64_t high_from = 0;
    while (esp_timer_get_time() / 1000 - start < timeout_ms) {
        read_pico_pmu_drain_events();
        read_pico_clear_ioe_int();
        bool held = false;
        if (read_pico_pmu_poll() == ESP_OK) {
            held = (read_pico_pmu_get()->key_state & 0x01) != 0;
        }
        bool int_high = gpio_get_level((gpio_num_t)READ_PICO_IOE_INT_GPIO) != 0;
        if (!held && int_high) {
            if (high_from == 0) high_from = esp_timer_get_time() / 1000;
            if (esp_timer_get_time() / 1000 - high_from >= 150) return;
        } else {
            high_from = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

app_wake_source_t app_last_wake_source(void) {
    return (app_wake_source_t)app_settings_last_wake();
}

/* ---- 浅睡按键唤醒 / Light-sleep key wake ---- */

// 按键唤醒要经 PMU：按键 → PMU 记事件并拉低 CW_INT → IO 扩展器报中断 → ESP 醒来再用 I2C 向 PMU 确认。
// CW_INT 是电平信号，只要还有没 ACK 的事件就一直低；醒来那一下 I2C 没读到，事件留在 PMU 里，CW_INT 不再变化，
// 扩展器也就不再报中断，之后怎么按都叫不醒（拿起唤醒走加速度计自己的引脚，不受影响）。
// Key wake goes through the PMU: key -> PMU queues an event and pulls CW_INT low -> the IO expander interrupts -> the
// ESP wakes and confirms with the PMU over I2C. CW_INT is a level: it stays low while any event is unacknowledged. If
// the I2C read right after waking misses, the event stays queued, CW_INT never changes again, the expander never
// interrupts again and no key press can wake the device (pickup uses the accelerometer's own pin and still works).

// 定时保底：疑似卡住时每隔这么久醒一次，直接问 PMU 有没有按键。/ Fallback: while a stall is suspected, wake this often
// and ask the PMU directly about the key.
#define KEY_FALLBACK_US (1500 * 1000)
// 睡前扩展器中断一直撤不掉，最多等这么多轮（每轮 20 ms）就改用定时保底入睡。/ If the expander interrupt will not
// clear before sleeping, give up after this many 20 ms rounds and sleep on the fallback timer.
#define IOE_STUCK_ROUNDS 50

static bool s_key_fallback;

// 醒来马上读 PMU 可能失败（I2C 偶发错误），隔一下重试几次。/ Reading the PMU right after waking can fail (transient
// I2C errors); retry a few times with a short gap.
static bool pmu_poll_retry(void) {
    for (int i = 0; i < 5; i++) {
        if (read_pico_pmu_poll() == ESP_OK) return true;
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    return false;
}

// PMU 还压着没 ACK 的事件（读不到它也按最坏情况算）。/ The PMU still holds unacknowledged events (an unreadable PMU
// counts as the worst case).
static bool pmu_events_stuck(void) {
    if (!pmu_poll_retry()) return true;
    const pmu_snapshot_t* snap = read_pico_pmu_get();
    return snap->pending_events > 0 || (snap->flags & PMU_STATUS_CW_INT_ASSERTED) != 0;
}

static void key_fallback_set(bool on) {
    if (on == s_key_fallback) return;
    s_key_fallback = on;
    if (on) esp_sleep_enable_timer_wakeup(KEY_FALLBACK_US);
    else esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    ESP_LOGW(TAG, "key wake fallback timer %s", on ? "on" : "off");
}

// 醒来后确认是不是按键：读得到 PMU 再取事件，键正按着也算。别的事件顺手 ACK 掉；还有剩的（或读不到）就打开
// 定时保底，清干净了就关掉。
// After waking, decide whether it was the key: read the PMU, then take events; a key held down counts too. Other
// events get acknowledged on the way; if any remain (or the PMU cannot be read) turn the fallback timer on, and off
// once the queue is clean.
static bool key_wake_check(const char* why) {
    const bool ok = pmu_poll_retry();
    const bool held = ok && (read_pico_pmu_get()->key_state & 0x01) != 0;
    const bool key = ok && read_pico_pmu_take_key_wakeup();
    if (key || held) {
        ESP_LOGI(TAG, "key wake (%s)%s", why, held ? " held" : "");
        return true;
    }
    if (ok) read_pico_pmu_drain_events();
    const bool stuck = pmu_events_stuck();
    const pmu_snapshot_t* snap = read_pico_pmu_get();
    ESP_LOGI(TAG, "no key (%s): pmu=%s pending=%u flags=0x%04x ioe=%d", why, ok ? "ok" : "fail",
             (unsigned)snap->pending_events, (unsigned)(snap->flags & 0xFFFF),
             gpio_get_level((gpio_num_t)READ_PICO_IOE_INT_GPIO));
    key_fallback_set(stuck);
    return false;
}

static void pickup_ack(sc7a20h_handle_t acc) {
    sc7a20h_events_t ev;
    sc7a20h_read_events(acc, &ev);
    sc7a20h_ack_int(acc);
}

static bool pickup_ia(sc7a20h_handle_t acc) {
    sc7a20h_events_t ev;
    sc7a20h_aoi_src_t aoi;
    if (sc7a20h_read_events(acc, &ev) != ESP_OK) return false;
    sc7a20h_aoi_decode(ev.aoi1_src, &aoi);
    return aoi.ia;
}

// 第一次低电平不够：高通余波还会再拉高。要连续安静一段时间才进浅睡。
// A first low is not enough: high-pass ringing can rise again. Wait for a quiet stretch before light sleep.
static bool pickup_wait_quiet(sc7a20h_handle_t acc, int quiet_ms, int timeout_ms) {
    int64_t start = esp_timer_get_time() / 1000;
    int64_t low_from = 0;
    while (esp_timer_get_time() / 1000 - start < timeout_ms) {
        if (sc7a20h_int1_level(acc) == 0) {
            if (low_from == 0) low_from = esp_timer_get_time() / 1000;
            if (esp_timer_get_time() / 1000 - low_from >= quiet_ms) return true;
        } else {
            pickup_ack(acc);
            low_from = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}

// 睡前确认面板真的断电了：读引脚，没断就再关一次（I2C 偶发失败时关电命令可能没生效），最多试三次。
// Confirm before sleeping that the panel is really off: read the pins and power it off again if not (a transient I2C
// failure can drop the power-off command), up to three tries.
static void ensure_panel_off(void) {
    for (int attempt = 0; attempt < 3; attempt++) {
        if (read_pico_epd_verify_poweroff() == ESP_OK) return;
        // 刚关完 PGOOD 可能还没落下，等一下再看。/ PGOOD may not have fallen yet right after power-off; look again.
        vTaskDelay(pdMS_TO_TICKS(20));
        if (read_pico_epd_verify_poweroff() == ESP_OK) return;
        epd_poweroff();
    }
    if (read_pico_epd_verify_poweroff() != ESP_OK) ESP_LOGE(TAG, "panel power-off not confirmed before sleep");
}

app_wake_source_t app_light_sleep_wait(sc7a20h_handle_t acc) {
    ensure_panel_off();
    bool pickup = acc != NULL && app_settings_pickup_wake();
    bool acc_armed = false;
    periph_suspend(false);
    lock_arm_ioe_wakeup();
    if (pickup) {
        sc7a20h_motion_cfg_t motion = SC7A20H_MOTION_DEFAULT();
        motion.ths_mg = 350;
        motion.duration = 3;
        sc7a20h_arm_pickup_wake(acc, &motion);
        acc_armed = true;
        pickup_ack(acc);
        if (!pickup_wait_quiet(acc, 400, 2000)) {
            pickup = false;
            ESP_LOGW(TAG, "pickup INT1 stuck high, key only");
        } else {
            sc7a20h_config_light_sleep_wakeup(acc);
            if (!pickup_wait_quiet(acc, 400, 1500)) {
                gpio_wakeup_disable(sc7a20h_int1_gpio(acc));
                pickup = false;
                ESP_LOGW(TAG, "pickup INT1 re-asserted, key only");
            } else {
                // 安静窗口里若 INT1 闪过，ESP 会留下高电平唤醒挂起，关掉再打开清掉。
                // If INT1 glitched in the quiet window, ESP keeps a high-level wake pending; disable then re-enable to clear it.
                gpio_wakeup_disable(sc7a20h_int1_gpio(acc));
                sc7a20h_config_light_sleep_wakeup(acc);
                if (sc7a20h_int1_level(acc) > 0) {
                    gpio_wakeup_disable(sc7a20h_int1_gpio(acc));
                    pickup = false;
                    ESP_LOGW(TAG, "pickup INT1 high at sleep, key only");
                } else {
                    ESP_LOGI(
                        TAG, "wait key GPIO%d or pickup GPIO%d",
                        READ_PICO_IOE_INT_GPIO, (int)sc7a20h_int1_gpio(acc)
                    );
                }
            }
        }
    } else {
        ESP_LOGI(TAG, "wait key on IOE_INT GPIO%d", READ_PICO_IOE_INT_GPIO);
    }

    app_wake_source_t wake = APP_WAKE_NONE;
    bool slept = false;
    // 睡前先把 PMU 里积压的事件清干净，清不掉就打开定时保底。/ Clear the PMU backlog before sleeping; if it will not
    // clear, turn on the fallback timer.
    read_pico_pmu_drain_events();
    key_fallback_set(pmu_events_stuck());
    bool ioe_stuck = false;
    int ioe_low_rounds = 0;
    for (;;) {
        read_pico_clear_ioe_int();
        if (!slept && !ioe_stuck && gpio_get_level((gpio_num_t)READ_PICO_IOE_INT_GPIO) == 0) {
            read_pico_pmu_drain_events();
            read_pico_clear_ioe_int();
            // 一直撤不掉就别空转：关掉扩展器唤醒，靠定时保底入睡。/ Do not spin forever: drop the expander wake and
            // sleep on the fallback timer.
            if (++ioe_low_rounds >= IOE_STUCK_ROUNDS) {
                ESP_LOGW(TAG, "IOE_INT stuck low before sleep, timer fallback");
                gpio_wakeup_disable((gpio_num_t)READ_PICO_IOE_INT_GPIO);
                ioe_stuck = true;
                key_fallback_set(true);
                continue;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!slept && pickup && sc7a20h_int1_level(acc) > 0) {
            pickup_ack(acc);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (slept && !ioe_stuck && gpio_get_level((gpio_num_t)READ_PICO_IOE_INT_GPIO) == 0) {
            if (key_wake_check("ioe low")) {
                wake = APP_WAKE_KEY;
                break;
            }
            read_pico_clear_ioe_int();
            // 清过还是低：扩展器读不动，改靠定时保底，免得刚睡下又被同一个电平叫醒。/ Still low after clearing: the
            // expander cannot be read, so rely on the fallback timer instead of waking on the same level again.
            if (gpio_get_level((gpio_num_t)READ_PICO_IOE_INT_GPIO) == 0) {
                ESP_LOGW(TAG, "IOE_INT stuck low, timer fallback");
                gpio_wakeup_disable((gpio_num_t)READ_PICO_IOE_INT_GPIO);
                ioe_stuck = true;
                key_fallback_set(true);
            }
            continue;
        }
        if (slept && pickup && sc7a20h_int1_level(acc) > 0 && pickup_ia(acc)) {
            wake = APP_WAKE_PICKUP;
            ESP_LOGI(TAG, "pickup wake");
            break;
        }
        if (slept && pickup && sc7a20h_int1_level(acc) > 0) {
            pickup_ack(acc);
            pickup_wait_quiet(acc, 400, 1500);
            continue;
        }
        int64_t t0 = esp_timer_get_time();
        esp_light_sleep_start();
        slept = true;
        int64_t dt_ms = (esp_timer_get_time() - t0) / 1000;
        const bool by_timer = (esp_sleep_get_wakeup_causes() & BIT(ESP_SLEEP_WAKEUP_TIMER)) != 0;
        // 定时醒来时扩展器若已恢复，重新打开它的唤醒。/ On a timer wake, re-arm the expander wake once it has recovered.
        if (by_timer && ioe_stuck) {
            read_pico_clear_ioe_int();
            if (gpio_get_level((gpio_num_t)READ_PICO_IOE_INT_GPIO) != 0) {
                gpio_wakeup_enable((gpio_num_t)READ_PICO_IOE_INT_GPIO, GPIO_INTR_LOW_LEVEL);
                ioe_stuck = false;
                ESP_LOGI(TAG, "IOE_INT recovered");
            }
        }
        read_pico_clear_ioe_int();
        if (key_wake_check(by_timer ? "timer" : "sleep")) {
            wake = APP_WAKE_KEY;
            break;
        }
        if (pickup && sc7a20h_int1_level(acc) > 0 && pickup_ia(acc)) {
            if (dt_ms < 80) {
                ESP_LOGW(TAG, "pickup ignored, sleep %lld ms", (long long)dt_ms);
                pickup_ack(acc);
                pickup_wait_quiet(acc, 400, 1500);
                continue;
            }
            wake = APP_WAKE_PICKUP;
            ESP_LOGI(TAG, "pickup wake");
            break;
        }
        if (pickup && sc7a20h_int1_level(acc) > 0) {
            pickup_ack(acc);
            pickup_wait_quiet(acc, 400, 1500);
        }
    }

    gpio_wakeup_disable((gpio_num_t)READ_PICO_IOE_INT_GPIO);
    key_fallback_set(false);
    if (acc_armed) {
        gpio_wakeup_disable(sc7a20h_int1_gpio(acc));
        sc7a20h_events_t ev;
        sc7a20h_read_events(acc, &ev);
        sc7a20h_power_down(acc);
        sc7a20h_int1_begin(acc);
    }
    periph_resume();
    app_settings_set_last_wake((uint8_t)wake);
    return wake;
}

esp_err_t app_enter_host_sleep(app_sleep_mode_t mode) {
    // 验证器先上锁（深睡和关机本来也会清掉内存，浅睡不会）。/ Lock the authenticator first (deep sleep and power-off
    // clear RAM anyway; light sleep does not).
    tr_auth_lock();
    // 先关屏轨和 VCOM 再让 PMU 断主机：SY_EN / VCOM_EN 在常开的 IO 扩展器上，主机断电后电平还保持着，
    // 刚画完锁屏的高压轨会一直开着，面板慢慢变黑（像渗墨）。关掉以后 IO 扩展器休眠时也会把 MODE 一起拉低。
    // Drop the panel rails and VCOM before the PMU cuts the host: SY_EN / VCOM_EN sit on the always-on IO expander and
    // keep their level after the host powers off, so the rails left up by the lock-face refresh would stay on and the
    // panel would slowly darken (like ink bleeding). With the rails off, the expander's sleep also parks MODE low.
    epd_poweroff();
    ensure_panel_off();
    periph_suspend(true);
    pmu_selftest_prepare_powerdown();
    esp_err_t err = mode == APP_SLEEP_OFF
        ? read_pico_pmu_power_off()
        : read_pico_pmu_report_sleep();
    ESP_LOGI(TAG, "%s %s", mode == APP_SLEEP_OFF ? "power off" : "SOFT_SLEEP", esp_err_to_name(err));
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(HOST_EN_DROP_WAIT_MS));
        err = ESP_ERR_TIMEOUT;
        ESP_LOGE(TAG, "EN still high after PMU ack");
    }
    // PMU 没把 EN 拉下来：恢复外设交还调用方，不能停在这里空转耗电。
    // The PMU did not drop EN: restore peripherals and return instead of idling at full power.
    periph_resume();
    return err;
}

// 锁屏画面已经画好（下载在跑时先画了一遍），enter_lock_and_sleep 就不再重画。/ The lock face is already up (drawn when
// a download kept the lock awake), so enter_lock_and_sleep does not redraw it.
static bool s_face_shown;

static void lock_face_show(EpdiyHighlevelState* hl) {
    // 乐曲的 32 kHz 采样中断和线圈电流会干扰锁屏整屏刷新，先停再上屏轨。
    // The score's 32 kHz sample ISR and coil current disturb the lock-face full refresh; stop it before the rails come up.
    if (read_pico_buzzer_score_busy()) (void)read_pico_buzzer_score_stop();
    // 每次锁屏都把验证器上锁，钥匙和账号表清零；锁屏画面整屏重画，验证码不会留在屏上。
    // Every lock locks the authenticator and zeroes its key and table; the lock face redraws the whole screen, so no
    // code stays visible.
    tr_auth_lock();
    uint8_t* framebuffer = epd_hl_get_framebuffer(hl);
    tr_lock_prepare();
    epd_poweron();
    epd_clear();
    epd_hl_set_all_white(hl);
    // Treader：有在读的书就画书卡，否则用官方锁屏图。/ Treader: draw the book card when reading, else the stock image.
    if (!tr_lock_draw(framebuffer)) ui_draw_full_image(framebuffer, lock_4bpp_bin_start);
    epd_hl_update_screen_from_white(hl, MODE_GC16, 25);

    app_lock_wait_key_idle(800);
    s_face_shown = true;
}

// 回到主循环前的收尾：参考帧和屏幕都归零，回到主循环后由当前页自己画一遍，不必知道是哪一页。
// Wrap-up before the main loop resumes: zero the reference frame and the panel; the current page redraws after the loop
// resumes, without knowing which page it is.
static void lock_face_leave(EpdiyHighlevelState* hl, int64_t* ignore_until_ms) {
    read_pico_pmu_drain_events();
    epd_poweron();
    epd_clear();
    epd_hl_set_all_white(hl);
    read_pico_pmu_drain_events();
    if (ignore_until_ms) {
        *ignore_until_ms = esp_timer_get_time() / 1000 + APP_LOCK_IGNORE_BOOT_MS;
    }
    s_face_shown = false;
    ESP_LOGI(TAG, "unlocked");
}

// 画面留着、屏轨断电，主机不睡；每 200 毫秒看一次电源键和下载有没有往前走。返回 true 是按了电源键。
// The picture stays with the panel rails off and the host awake; every 200 ms look at the power key and at whether the
// download moves. True means the power key was pressed.
static bool hold_for_download(EpdiyHighlevelState* hl, int64_t* ignore_until_ms) {
    epd_poweroff();
    const int64_t t0 = esp_timer_get_time() / 1000;
    int64_t moved = t0;
    uint32_t stamp = tomato_sync_stamp();
    const char* why = NULL;
    ESP_LOGI(TAG, "lock held while a download runs");
    while (tomato_sync_downloading()) {
        if (read_pico_pmu_take_key_short()) {
            lock_face_leave(hl, ignore_until_ms);
            return true;
        }
        const int64_t now = esp_timer_get_time() / 1000;
        const uint32_t s = tomato_sync_stamp();
        if (s != stamp) {
            stamp = s;
            moved = now;
        }
        const pmu_snapshot_t* p = read_pico_pmu_get();
        if (now - moved > APP_LOCK_HOLD_STALL_MS) why = "no progress";
        else if (now - t0 > APP_LOCK_HOLD_MAX_MS) why = "time limit";
        else if (p && p->battery_mv && p->battery_mv < APP_LOCK_HOLD_MIN_MV) why = "low battery";
        if (why) break;
        vTaskDelay(pdMS_TO_TICKS(APP_LOCK_POLL_MS));
    }
    if (why) {
        ESP_LOGW(TAG, "hold ended (%s), stopping the download", why);
        (void)tomato_sync_stop(8000);
    } else {
        ESP_LOGI(TAG, "hold ended, download finished");
    }
    // 屏轨回到“刚画完锁屏”的状态，后面的睡眠路径照常先断它。/ Put the rails back to "lock face just drawn"; the sleep
    // path that follows drops them as usual.
    epd_poweron();
    return false;
}

bool app_lock_hold(EpdiyHighlevelState* hl, int64_t* ignore_until_ms) {
    lock_face_show(hl);
    return hold_for_download(hl, ignore_until_ms);
}

void enter_lock_and_sleep(
    EpdiyHighlevelState* hl, int64_t* ignore_until_ms, sc7a20h_handle_t acc
) {
    if (!s_face_shown) lock_face_show(hl);
    s_face_shown = false;
    // 下载在跑（比如从密码关回到锁屏睡眠时）：先留着继续下，下完才睡；按了电源键就当醒来。
    // A download is running (say, returning from the PIN gate to the locked sleep): keep downloading first and sleep
    // afterwards; the power key counts as a wake.
    if (tomato_sync_downloading() && hold_for_download(hl, ignore_until_ms)) return;
    app_sleep_mode_t mode = app_settings_sleep_mode();
    ESP_LOGI(TAG, "lock %s", app_sleep_mode_name(mode));

    // 主机下电只在失败时返回，退到浅睡，至少外设已停、按键可唤醒。
    // Host power-down returns only on failure; fall back to light sleep with peripherals parked and the key as wake.
    if (mode == APP_SLEEP_OFF || mode == APP_SLEEP_DEEP) {
        ESP_LOGW(TAG, "host sleep failed: %s, light sleep", esp_err_to_name(app_enter_host_sleep(mode)));
    }
    epd_poweroff();
    app_light_sleep_wait(acc);

    lock_face_leave(hl, ignore_until_ms);
}
