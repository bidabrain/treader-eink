/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB 传书的实现：esp_tinyusb 的 MSC 存储接 SD 卡；用链接器 --wrap 包住 sdmmc 读写数出读写量。电脑“弹出”时
 * esp_tinyusb 会把卡挂回本机（挂到不用的 /usbmsc），存储事件据此认出弹出。连接状态来自 TinyUSB 事件。
 *
 * English: USB transfer. esp_tinyusb MSC storage on the SD card; linker --wrap around sdmmc reads/writes counts
 * bytes. On an eject esp_tinyusb mounts the card back locally (at the unused /usbmsc), and its storage event marks the
 * eject. Connection state comes from TinyUSB events.
 */

#include "read_pico_usb.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "sdmmc_cmd.h"
#include "class/msc/msc_device.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"

#define TAG "usb_msc"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static read_pico_usb_status_t s_status;
static bool s_started, s_ever_connected;
static tinyusb_msc_storage_handle_t s_storage;
static bool s_usb_up;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void set_state(read_pico_usb_state_t st) {
    portENTER_CRITICAL(&s_lock);
    if (st == READ_PICO_USB_GONE && s_status.state != READ_PICO_USB_GONE) s_status.gone_ms = now_ms();
    s_status.state = st;
    portEXIT_CRITICAL(&s_lock);
}

// 存储事件：电脑弹出（或拔线）时 esp_tinyusb 试着把卡挂回本机，成功、失败、要格式化都算“弹出了”；只有重新挂给
// USB（电脑又接管）才清掉。弹出是单独的标志，连接状态的变化（挂起、恢复）不会把它冲掉。
// Storage events: on an eject (or unplug) esp_tinyusb tries to mount the card back locally; success, failure or
// "format required" all mean ejected. Only mounting back to USB (the computer takes it again) clears it. Ejected is
// its own flag, so link changes (suspend, resume) never override it.
static void storage_event(tinyusb_msc_storage_handle_t handle, tinyusb_msc_event_t* event, void* arg) {
    (void)handle;
    (void)arg;
    const bool to_app = event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP;
    bool ejected;
    switch (event->id) {
    case TINYUSB_MSC_EVENT_MOUNT_COMPLETE: ejected = to_app; break;
    case TINYUSB_MSC_EVENT_MOUNT_FAILED:
    case TINYUSB_MSC_EVENT_FORMAT_REQUIRED: if (!to_app) return; ejected = true; break;
    default: return;
    }
    portENTER_CRITICAL(&s_lock);
    s_status.ejected = ejected;
    portEXIT_CRITICAL(&s_lock);
}

// 电脑上显示的盘名（SCSI INQUIRY），esp_tinyusb 没管，这里填上。/ The drive name the computer shows (SCSI INQUIRY);
// esp_tinyusb leaves it alone, so fill it here.
uint32_t tud_msc_inquiry2_cb(uint8_t lun, scsi_inquiry_resp_t* resp, uint32_t bufsize) {
    (void)lun;
    if (bufsize < sizeof(*resp)) return 0;
    memcpy(resp->vendor_id, "ReadPico", 8);
    memcpy(resp->product_id, "Read Pico TF    ", 16);
    memcpy(resp->product_rev, "1.0 ", 4);
    return sizeof(*resp);
}

static void usb_event(tinyusb_event_t* event, void* arg) {
    (void)arg;
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
    case TINYUSB_EVENT_RESUMED:
        s_ever_connected = true;
        set_state(READ_PICO_USB_CONNECTED);
        break;
    case TINYUSB_EVENT_DETACHED:
    case TINYUSB_EVENT_SUSPENDED:
        if (s_ever_connected) set_state(READ_PICO_USB_GONE);
        break;
    default:
        break;
    }
}

/* ---- 链接器包装 / Linker wraps ---- */

esp_err_t __real_sdmmc_read_sectors(sdmmc_card_t* card, void* dst, size_t start_sector, size_t sector_count);
esp_err_t __real_sdmmc_write_sectors(sdmmc_card_t* card, const void* src, size_t start_sector, size_t sector_count);

esp_err_t __wrap_sdmmc_read_sectors(sdmmc_card_t* card, void* dst, size_t start_sector, size_t sector_count) {
    const esp_err_t err = __real_sdmmc_read_sectors(card, dst, start_sector, sector_count);
    if (s_started && err == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_status.read_bytes += (uint64_t)sector_count * card->csd.sector_size;
        s_status.io_ms = now_ms();
        portEXIT_CRITICAL(&s_lock);
    }
    return err;
}

esp_err_t __wrap_sdmmc_write_sectors(sdmmc_card_t* card, const void* src, size_t start_sector, size_t sector_count) {
    const esp_err_t err = __real_sdmmc_write_sectors(card, src, start_sector, sector_count);
    if (s_started && err == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_status.write_bytes += (uint64_t)sector_count * card->csd.sector_size;
        s_status.io_ms = now_ms();
        portEXIT_CRITICAL(&s_lock);
    }
    return err;
}

/* ---- 接口 / API ---- */

esp_err_t read_pico_usb_start(void* card) {
    if (!card || s_started) return ESP_ERR_INVALID_STATE;
    memset(&s_status, 0, sizeof(s_status));
    s_ever_connected = false;
    // 保留自动挂回：弹出时 esp_tinyusb 把卡挂到 /usbmsc（设备不用它），我们借这个事件认出弹出；结束总是重启。
    // Keep auto-remount: on eject esp_tinyusb mounts the card at /usbmsc (unused by the device) and that event
    // marks the eject; the end is always a restart.
    const tinyusb_msc_driver_config_t drv = {.callback = storage_event};
    esp_err_t err = tinyusb_msc_install_driver(&drv);
    if (err != ESP_OK) return err;
    static char base_path[] = "/usbmsc";   // 结构体成员不是 const / The struct field is not const
    const tinyusb_msc_storage_config_t cfg = {
        .medium.card = (sdmmc_card_t*)card,
        .fat_fs = {.base_path = base_path, .do_not_format = true},
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
    };
    err = tinyusb_msc_new_storage_sdmmc(&cfg, &s_storage);
    if (err != ESP_OK) {
        tinyusb_msc_uninstall_driver();
        return err;
    }
    s_started = true;
    const tinyusb_config_t usb = TINYUSB_DEFAULT_CONFIG(usb_event);
    err = tinyusb_driver_install(&usb);
    s_usb_up = err == ESP_OK;
    if (err != ESP_OK) {
        s_started = false;
        tinyusb_msc_delete_storage(s_storage);
        tinyusb_msc_uninstall_driver();
        s_storage = NULL;
        return err;
    }
    ESP_LOGI(TAG, "usb drive started");
    return ESP_OK;
}

void read_pico_usb_get_status(read_pico_usb_status_t* out) {
    portENTER_CRITICAL(&s_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_lock);
}

void read_pico_usb_stop(void) {
    if (!s_started) return;
    // 什么都不拆：卸驱动或删存储在弹出挂载失败后会断言；重启会复位 USB 外设，电脑同样马上看到设备断开。
    // Tear nothing down: uninstalling or deleting storage asserts after a failed eject mount; the restart resets the
    // USB peripheral and the computer sees the device leave just the same.
    s_usb_up = false;
}
