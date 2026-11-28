/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：触摸采样任务和它的短队列，约定见 app_touch.h。
 *
 * English: The touch sampling task and its short queue; see app_touch.h for the contract.
 *
 * 冻结：见 app_touch.h。/ Frozen: see app_touch.h.
 */

#include "app_touch.h"

#include <stdbool.h>
#include <string.h>

#include "esp_timer.h"

#ifndef APP_TOUCH_HOST_TEST
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

// 主循环这么久没来读，采样任务才替它读。/ The task reads for the loop only once the loop has been away this long.
#define AWAY_MS 20
// 采样任务多久醒一次。/ How often the sampling task wakes.
#define SAMPLE_MS 8
// 按下超过这么久还没轮到的一笔整笔丢掉（翻一页漫画加刷屏约 1 秒，留出余量）。/ A stroke pressed longer ago than this
// when its turn comes is dropped whole (a comic page plus its refresh takes about 1 s; this leaves a margin).
#define EXPIRE_MS 1500
// 队列能排几帧：一下点击最多 3 帧（按下、抬起前位置、抬起），够排 5 下。/ Queue depth: a tap takes at most 3 frames
// (press, last position, lift), enough for 5 taps.
#define QUEUE_N 16
// 主循环提到的优先级（下载任务是 4），采样任务再高一级。/ The loop's raised priority (download tasks run at 4); the
// sampling task sits one above it.
#define LOOP_PRIORITY 5
#define SAMPLE_PRIORITY 6

typedef struct {
    cst836u_touch_t t;
    int64_t ms;          // 读到的时间 / When it was read
    uint32_t stroke;     // 属于第几笔 / Which stroke it belongs to
    int64_t stroke_ms;   // 这一笔按下的时间 / When that stroke was pressed
} frame_t;

static frame_t s_q[QUEUE_N];
static int s_head, s_n;
// 流里最后一帧（排进队列或直接给了主循环）的状态，判断按下、抬起用。/ State of the stream's last frame (queued or
// handed straight to the loop), for telling presses and lifts.
static bool s_tail_touched;
static uint8_t s_tail_count;
// 主循环最后拿到的一帧是不是按着。/ Whether the last frame the loop got was touched.
static bool s_given_touched;
static uint8_t s_given_count;
// 按着移动时最新的一帧，还没进流；抬起时先补上它，手势才看得到滑到了哪。/ The newest frame while moving, not yet in
// the stream; it goes in before a lift so the gesture sees where the finger went.
static frame_t s_recent;
static bool s_recent_pending;
static uint32_t s_stroke;
static int64_t s_stroke_ms;
static int64_t s_loop_ms = INT64_MIN / 2;   // 主循环最后来读的时间 / When the loop last came to read
static int s_paused;
// 恢复时手指可能还按着（密码键盘在按下第 4 位时就返回）：这一笔一直不算，等抬起再说。/ The finger may still be down on
// resume (the PIN pad returns on the 4th digit's press): that stroke never counts; wait for the lift.
static bool s_until_lift;

#ifdef APP_TOUCH_HOST_TEST
static void lock(void) {}
static void unlock(void) {}
#else
static SemaphoreHandle_t s_lock;
static void lock(void) { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }
#endif

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void flush_locked(void) {
    s_head = s_n = 0;
    s_recent_pending = false;
    // 排着的都扔了，流的末尾回到主循环手上那一帧。/ With the queue gone, the stream ends at the loop's last frame.
    s_tail_touched = s_given_touched;
    s_tail_count = s_given_count;
}

// 把一帧接到流尾（press 时开新的一笔）；frame 填好了 stroke。/ Append a frame to the stream (a press starts a new
// stroke); fills in the frame's stroke.
static void note_tail(frame_t* f) {
    if (f->t.touched && !s_tail_touched) {
        s_stroke++;
        s_stroke_ms = f->ms;
    }
    f->stroke = s_stroke;
    f->stroke_ms = s_stroke_ms;
    s_tail_touched = f->t.touched;
    s_tail_count = f->t.touched ? f->t.count : 0;
    s_recent_pending = false;
}

static bool push(frame_t f) {
    if (s_n >= QUEUE_N) return false;   // 满了：这帧丢掉，流尾不动，下一帧再比 / Full: drop it, keep the tail
    note_tail(&f);
    s_q[(s_head + s_n) % QUEUE_N] = f;
    s_n++;
    return true;
}

static void sample_locked(cst836u_handle_t tp, int64_t now) {
    if (s_paused || tp == NULL || now - s_loop_ms < AWAY_MS) return;
    frame_t f = { .ms = now };
    if (cst836u_read(tp, &f.t) != ESP_OK) return;
    if (s_until_lift) {
        if (f.t.touched) return;
        s_until_lift = false;
    }
    if (f.t.touched && !s_tail_touched) {
        (void)push(f);
    } else if (f.t.touched) {
        if (f.t.count != s_tail_count) {
            (void)push(f);
        } else {
            s_recent = f;
            s_recent_pending = true;
        }
    } else if (s_tail_touched) {
        if (s_recent_pending && !push(s_recent)) return;
        (void)push(f);
    }
}

esp_err_t app_touch_read(cst836u_handle_t tp, cst836u_touch_t* out, int64_t* t_ms) {
    lock();
    const int64_t now = now_ms();
    s_loop_ms = now;
    while (s_n > 0) {
        const frame_t* f = &s_q[s_head];
        if (!s_given_touched && now - f->stroke_ms > EXPIRE_MS) {
            // 主循环还没见过这一笔的按下，而它按下太久了：整笔丢掉。/ The loop never saw this stroke's press and it is
            // too old: drop the whole stroke.
            const uint32_t stroke = f->stroke;
            while (s_n > 0 && s_q[s_head].stroke == stroke) {
                s_head = (s_head + 1) % QUEUE_N;
                s_n--;
            }
            continue;
        }
        *out = f->t;
        *t_ms = f->ms;
        s_given_touched = f->t.touched;
        s_given_count = f->t.touched ? f->t.count : 0;
        s_head = (s_head + 1) % QUEUE_N;
        s_n--;
        unlock();
        return ESP_OK;
    }
    frame_t f = { .ms = now };
    esp_err_t err = cst836u_read(tp, &f.t);
    if (err == ESP_OK && s_until_lift) {
        // 还按着暂停前那一笔：给主循环一帧“没按”，抬起之后照常。/ Still on the stroke from before the pause: hand the loop
        // an untouched frame, and carry on as usual after the lift.
        if (f.t.touched) {
            const cst836u_touch_t held = f.t;
            memset(&f.t, 0, sizeof(f.t));
            memcpy(f.t.raw, held.raw, sizeof(f.t.raw));
        } else {
            s_until_lift = false;
        }
    }
    if (err == ESP_OK) {
        // 队列空了，这一帧直接接在流尾。/ The queue is empty, so this frame joins the stream's tail directly.
        note_tail(&f);
        s_given_touched = f.t.touched;
        s_given_count = f.t.touched ? f.t.count : 0;
        *out = f.t;
    } else {
        memset(out, 0, sizeof(*out));
    }
    *t_ms = now;
    unlock();
    return err;
}

void app_touch_flush(void) {
    lock();
    flush_locked();
    unlock();
}

void app_touch_pause(void) {
    lock();
    s_paused++;
    unlock();
}

void app_touch_resume(void) {
    lock();
    if (s_paused > 0) s_paused--;
    flush_locked();
    // 还按着的那一笔等抬起；抬起照常接在流尾，主循环就看得到这一下结束了。/ A stroke still down waits for its lift; the
    // lift joins the stream as usual so the loop sees that stroke end.
    s_until_lift = true;
    // 暂停期间别人读过触摸，主循环手上的状态已经旧了；从下一次主循环自己读开始接。/ Someone else read touch while
    // paused, so the loop's state is stale; continue from the loop's next own read.
    s_loop_ms = now_ms();
    unlock();
}

#ifdef APP_TOUCH_HOST_TEST
void app_touch_test_sample(cst836u_handle_t tp, int64_t now) {
    sample_locked(tp, now);
}

void app_touch_test_reset(void) {
    s_head = s_n = 0;
    s_tail_touched = s_given_touched = s_recent_pending = false;
    s_tail_count = s_given_count = 0;
    s_stroke = 0;
    s_stroke_ms = 0;
    s_loop_ms = INT64_MIN / 2;
    s_paused = 0;
    s_until_lift = false;
}

void app_touch_start(cst836u_handle_t tp) { (void)tp; }
#else
static const char* TAG = "app_touch";

static void sample_task(void* arg) {
    cst836u_handle_t tp = (cst836u_handle_t)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
        lock();
        sample_locked(tp, now_ms());
        unlock();
    }
}

void app_touch_start(cst836u_handle_t tp) {
    if (s_lock != NULL || tp == NULL) return;
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        ESP_LOGW(TAG, "no mutex; touch sampling off");
        return;
    }
    lock();
    s_loop_ms = now_ms();
    unlock();
    // I2C 驱动在栈上用信号量，栈放内部 RAM。/ The I2C driver blocks on semaphores from this stack; keep it internal.
    if (xTaskCreate(sample_task, "touch_sample", 4096, tp, SAMPLE_PRIORITY, NULL) != pdPASS) {
        ESP_LOGW(TAG, "touch sampling task not started");
    }
    // 主循环比下载任务高：后台解码、写卡时点一下也马上有反应。/ The loop outranks the download tasks, so a tap responds
    // at once while the background decodes or writes.
    vTaskPrioritySet(NULL, LOOP_PRIORITY);
}
#endif
