/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB 传书：把 TF 卡整张交给电脑当 U 盘（TinyUSB MSC），并报告连接、弹出和读写量。
 *
 * English: USB transfer. Hand the whole TF card to a computer as a USB drive (TinyUSB MSC) and report connection,
 * eject and bytes moved.
 *
 * 冻结：开始后 USB 口不再是调试串口，卡也不再归设备；没有“原地恢复”，结束一律由调用方重启。
 * Frozen: once started the USB port is no longer the debug console and the card no longer belongs to the device;
 * there is no in-place recovery, so the caller always restarts to finish.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    READ_PICO_USB_WAITING = 0,   ///< 等电脑连上 / Waiting for a computer
    READ_PICO_USB_CONNECTED,     ///< 电脑已连上 / Connected to a computer
    READ_PICO_USB_GONE,          ///< 连过又没信号了：拔线或总线挂起（分不出来，看供电）/ Was connected, now detached or
                                 ///< suspended (indistinguishable here; check external power)
} read_pico_usb_state_t;

typedef struct {
    read_pico_usb_state_t state;
    bool ejected;            ///< 电脑弹出过，且没再挂回 / The computer ejected and has not taken the drive back
    uint64_t read_bytes;     ///< 电脑读走的字节 / Bytes the computer read
    uint64_t write_bytes;    ///< 电脑写入的字节 / Bytes the computer wrote
    int64_t gone_ms;         ///< 进入 GONE 的时间（esp_timer 毫秒）/ When GONE began (esp_timer ms)
    int64_t io_ms;           ///< 最近一次读写的时间 / Time of the last read or write
} read_pico_usb_status_t;

/// 把已初始化的 SD 卡（sdmmc_card_t*，不挂文件系统）交给 USB。/ Expose an initialized SD card (sdmmc_card_t*, no
/// filesystem mounted) over USB.
esp_err_t read_pico_usb_start(void* card);
void read_pico_usb_get_status(read_pico_usb_status_t* out);
/// 结束前调用，之后调用方立刻重启。先让电脑看到 U 盘拔出，再把内部 USB PHY 交还 USB 串口/JTAG（重启后的刷机和日志
/// 口）。不卸驱动、不删存储：弹出后挂载失败时那样做会断言。会阻塞约 100 ms。
/// Call before ending; the caller restarts right after. The computer first sees the drive unplugged, then the
/// internal USB PHY goes back to USB Serial/JTAG (the flashing and log port after the restart). No driver uninstall or
/// storage delete: after a failed eject mount either would assert. Blocks for about 100 ms.
void read_pico_usb_stop(void);

#ifdef __cplusplus
}
#endif
