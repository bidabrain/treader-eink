/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：触摸采样。主循环刷屏、排版、解码时不读触摸，这段时间里点下又抬起的一下会整个丢掉。这里起一个高优先级的小任务，
 * 主循环超过 20 ms 没来读时替它读，把按下、抬起、点数变化和抬起前最后的位置带着时间排进一个短队列；主循环下一次读时先按
 * 顺序拿队列里的，拿空了才读芯片。按下太久之前的一笔（主循环卡了很久才回来）整笔丢掉，不在用户已经不记得的时候突然动作。
 * 本文件不解释触摸含义，也不画屏。
 *
 * English: Touch sampling. The loop does not read touch while it presents, lays out or decodes, so a tap pressed and
 * lifted in that time is lost whole. A small high-priority task reads for the loop once it has not come for 20 ms and
 * queues presses, lifts, point-count changes and the last position before a lift, each with its time; the loop's
 * next read takes the queue in order first and reads the chip only once it is empty. A stroke pressed too long ago
 * (the loop was stuck for long) is dropped whole, so nothing fires after the user has moved on. This file does not
 * interpret touch and does not draw.
 *
 * 冻结：只补主循环看不到的那段，不合并、不改顺序；页面切换、锁屏、页面自己读触摸（密码键盘）之后排着的一律清掉。页面
 * 自己换了画面（比如点进正文）不清：刷屏时点的下一下就是给刷出来的新画面的，翻页连点靠的正是这个。
 * Frozen: only fills the stretch the loop cannot see, never merging or reordering; whatever is queued is discarded
 * after a page switch, a lock, or a page reading touch itself (the PIN pad). A page changing its own view (opening a
 * post, say) does not clear it: the next tap made during that refresh is meant for the view being drawn, which is
 * what quick page turns rely on.
 */
#pragma once

#include <stdint.h>

#include "cst836u.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/// 起采样任务，并把调用它的任务（主循环）提到下载任务之上。只调一次。/ Start the sampling task and raise the calling
/// task (the loop) above the download tasks. Call once.
void app_touch_start(cst836u_handle_t tp);

/// 主循环读一帧触摸：队列里有就按顺序拿最早的一帧，否则直接读芯片。*t_ms 是这一帧读到的时间（毫秒）。
/// The loop reads one touch frame: the earliest queued frame in order if any, else the chip. *t_ms receives when the
/// frame was read (ms).
esp_err_t app_touch_read(cst836u_handle_t tp, cst836u_touch_t* out, int64_t* t_ms);

/// 丢掉排着的帧（切页、重画整页之后）。/ Discard queued frames (after a page switch or a whole redraw).
void app_touch_flush(void);

/// 别处要自己读触摸时暂停采样（可嵌套）；恢复时丢掉排着的帧，恢复时还按着的那一笔整笔不算（等抬起）。/ Pause sampling
/// while someone else reads touch itself (nestable); resuming discards queued frames, and a stroke still down at
/// resume is ignored whole (until its lift).
void app_touch_pause(void);
void app_touch_resume(void);

#ifdef APP_TOUCH_HOST_TEST
/// 主机测试用：在 now_ms 跑一次采样任务的一步。/ Host tests: run one sampling-task step at now_ms.
void app_touch_test_sample(cst836u_handle_t tp, int64_t now_ms);
/// 主机测试用：回到初始状态。/ Host tests: return to the initial state.
void app_touch_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif
