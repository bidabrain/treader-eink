/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 系统升级：从 TF 卡的 update 文件夹（或根目录）读固件，写进另一个程序槽，校验通过后重启。
 * 新固件启动失败时引导程序自动退回旧版本（见 app_main 的 esp_ota_mark_app_valid_cancel_rollback）。
 * 从“关于 Treader”进入，不在菜单里。
 *
 * System upgrade: read firmware from the TF card's update folder (or root), write it to the other app slot,
 * verify and reboot. If the new app fails to boot, the bootloader rolls back (see app_main). Opened from
 * "About Treader"; not in the menu.
 *
 * 冻结：只在用户确认后写；电量低于 30% 且没充电时拒绝；只收 ESP32-S3 的 Read_Pico 镜像；写入在 on_tick 里分块做，
 * 本页停留期间不锁屏、不自动睡眠。
 * Frozen: write only after confirmation; refuse below 30% battery unless charging; accept only ESP32-S3
 * Read_Pico images; write in on_tick chunks; no lock or idle sleep while this page is open.
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "app.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "read_pico_pmu.h"
#include "tomato.h"
#include "tr_time.h"
#include "tr_ui.h"
#include "ui_kit.h"
#include "ui_menu.h"

#define TAG "tr_update"
#define FILE_MAX 8
#define NAME_LEN 96
#define CHUNK 4096
#define CHUNKS_PER_TICK 8
#define MERGED_APP_OFFSET 0x10000
#define MIN_BATTERY 30
#define LIST_TOP 470
#define ROW_H 120
#define BTN_Y 980
#define BTN_H 84

extern const app_desc_t app_tr_settings;

typedef enum { V_LIST, V_CONFIRM, V_WRITING, V_DONE } view_t;

typedef struct {
    char path[NAME_LEN + 24];
    char name[NAME_LEN];
    char version[32];
    char date[16];
    char time[16];
    uint32_t app_offset;
    uint32_t app_size;
    const char* problem;   // 非空就不能用 / Unusable when set
} fw_file_t;

static view_t s_view;
static fw_file_t* s_files;   // PSRAM
static int s_count;
static int s_sel = -1;
static char s_msg[96];
static const esp_partition_t* s_target;

// 写入中的状态。/ In-flight write state.
static FILE* s_fp;
static esp_ota_handle_t s_ota;
static bool s_ota_open;
static uint8_t* s_buf;
static uint32_t s_done;
static int s_drawn_pct = -1;
static int64_t s_done_ms;

/* ---- 文件检查 / File checks ---- */

static uint32_t u32le(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// 读 base 处的应用描述；不是 ESP 应用镜像返回 false。/ Read the app description at base; false if not an app image.
static bool read_desc(FILE* f, uint32_t base, fw_file_t* out, uint16_t* chip) {
    uint8_t h[0x80];
    if (fseek(f, (long)base, SEEK_SET) != 0 || fread(h, 1, sizeof(h), f) != sizeof(h)) return false;
    if (h[0] != 0xE9 || u32le(h + 0x20) != 0xABCD5432) return false;
    *chip = (uint16_t)(h[12] | h[13] << 8);
    snprintf(out->version, sizeof(out->version), "%.31s", (const char*)h + 0x30);
    char project[33] = "";
    memcpy(project, h + 0x50, 32);
    project[32] = 0;
    snprintf(out->time, sizeof(out->time), "%.15s", (const char*)h + 0x70);
    // date 在 0x80 处，超出前面读的 0x80 字节，单独再读。/ date sits at 0x80, past the first read.
    uint8_t d[16];
    if (fseek(f, (long)base + 0x80, SEEK_SET) == 0 && fread(d, 1, sizeof(d), f) == sizeof(d))
        snprintf(out->date, sizeof(out->date), "%.15s", (const char*)d);
    if (strcmp(project, "Read_Pico")) out->problem = "不是 Treader 固件";
    return true;
}

static void inspect(fw_file_t* fw) {
    struct stat st;
    FILE* f = fopen(fw->path, "rb");
    if (!f || stat(fw->path, &st) != 0) {
        fw->problem = "读不了这个文件";
        if (f) fclose(f);
        return;
    }
    uint16_t chip = 0;
    if (read_desc(f, 0, fw, &chip)) {
        fw->app_offset = 0;
    } else if ((uint32_t)st.st_size > MERGED_APP_OFFSET && read_desc(f, MERGED_APP_OFFSET, fw, &chip)) {
        // 合并镜像：只取 0x10000 之后的主程序。/ Merged image: take only the app after 0x10000.
        fw->app_offset = MERGED_APP_OFFSET;
    } else {
        fw->problem = "不是 ESP32 固件";
        fclose(f);
        return;
    }
    fclose(f);
    fw->app_size = (uint32_t)st.st_size - fw->app_offset;
    if (chip != 9) fw->problem = "不是给 ESP32-S3 的固件";
    else if (s_target && fw->app_size > s_target->size) fw->problem = "固件比程序槽还大";
}

static void scan_dir(const char* dir) {
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) && s_count < FILE_MAX) {
        const char* dot = strrchr(e->d_name, '.');
        if (e->d_name[0] == '.' || !dot || strcasecmp(dot, ".bin") || strlen(e->d_name) >= NAME_LEN) continue;
        fw_file_t* fw = &s_files[s_count];
        memset(fw, 0, sizeof(*fw));
        snprintf(fw->name, sizeof(fw->name), "%s", e->d_name);
        snprintf(fw->path, sizeof(fw->path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(fw->path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        inspect(fw);
        s_count++;
    }
    closedir(d);
}

static void scan(void) {
    s_count = 0;
    if (!s_files) return;
    scan_dir("/sdcard/update");
    scan_dir("/sdcard");
}

// 电量够不够：低于 30% 必须在充电。/ Enough battery: below 30% it must be charging.
static bool battery_ok(int* pct) {
    *pct = -1;
    if (!read_pico_pmu_ready()) return true;
    const pmu_snapshot_t* pmu = read_pico_pmu_get();
    if (!pmu) return true;
    *pct = (pmu->soc_permille + 5) / 10;
    const bool charging = pmu->charge_state == PMU_CHARGE_CHARGING || pmu->charge_state == PMU_CHARGE_FULL_INFERRED;
    return *pct >= MIN_BATTERY || charging;
}

/* ---- 写入 / Writing ---- */

static void stop_write(void) {
    if (s_ota_open) { esp_ota_abort(s_ota); s_ota_open = false; }
    if (s_fp) { fclose(s_fp); s_fp = NULL; }
    heap_caps_free(s_buf);
    s_buf = NULL;
}

static bool start_write(void) {
    const fw_file_t* fw = &s_files[s_sel];
    s_buf = heap_caps_malloc(CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_fp = fopen(fw->path, "rb");
    if (!s_buf || !s_fp || fseek(s_fp, (long)fw->app_offset, SEEK_SET) != 0) {
        snprintf(s_msg, sizeof(s_msg), "打不开固件文件");
        stop_write();
        return false;
    }
    // 边写边擦，不在一个 tick 里卡住几十秒。/ Erase as we write so no single tick blocks for seconds.
    esp_err_t err = esp_ota_begin(s_target, OTA_WITH_SEQUENTIAL_WRITES, &s_ota);
    if (err != ESP_OK) {
        snprintf(s_msg, sizeof(s_msg), "无法开始写入（%s）", esp_err_to_name(err));
        stop_write();
        return false;
    }
    s_ota_open = true;
    s_done = 0;
    s_drawn_pct = -1;
    ESP_LOGI(TAG, "write %s (%lu bytes) to %s", fw->path, (unsigned long)fw->app_size, s_target->label);
    return true;
}

// 写一批；返回 1 写完，0 继续，-1 出错。/ Write one batch; 1 done, 0 more, -1 error.
static int write_some(void) {
    const fw_file_t* fw = &s_files[s_sel];
    for (int i = 0; i < CHUNKS_PER_TICK && s_done < fw->app_size; i++) {
        size_t want = fw->app_size - s_done < CHUNK ? fw->app_size - s_done : CHUNK;
        if (fread(s_buf, 1, want, s_fp) != want) { snprintf(s_msg, sizeof(s_msg), "读固件文件出错，检查 TF 卡"); return -1; }
        esp_err_t err = esp_ota_write(s_ota, s_buf, want);
        if (err != ESP_OK) { snprintf(s_msg, sizeof(s_msg), "写入失败（%s）", esp_err_to_name(err)); return -1; }
        s_done += want;
    }
    if (s_done < fw->app_size) return 0;
    esp_err_t err = esp_ota_end(s_ota);
    s_ota_open = false;
    if (err != ESP_OK) {
        snprintf(s_msg, sizeof(s_msg), err == ESP_ERR_OTA_VALIDATE_FAILED ? "固件校验失败，文件可能损坏"
                                                                         : "写入收尾失败（%s）", esp_err_to_name(err));
        return -1;
    }
    err = esp_ota_set_boot_partition(s_target);
    if (err != ESP_OK) { snprintf(s_msg, sizeof(s_msg), "无法切换启动分区（%s）", esp_err_to_name(err)); return -1; }
    return 1;
}

/* ---- 绘制 / Drawing ---- */

static void info_row(uint8_t* fb, int y, const char* label, const char* value) {
    tr_text(fb, TR_MARGIN, y, 30, label, EPD_DRAW_ALIGN_LEFT, false);
    char v[96];
    snprintf(v, sizeof(v), "%s", value);
    tr_fit(v, 28, TR_W - 2 * TR_MARGIN - 170);
    tr_text(fb, TR_W - TR_MARGIN, y + 2, 28, v, EPD_DRAW_ALIGN_RIGHT, false);
    tr_hline(fb, y + 54, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
}

static void draw_header(uint8_t* fb, bool back) {
    tr_status_bar(fb);
    if (back) tr_text(fb, TR_MARGIN, 96, 44, "‹", EPD_DRAW_ALIGN_LEFT, false);
    tr_text(fb, 96, 100, 36, "系统升级", EPD_DRAW_ALIGN_LEFT, true);
}

static void draw_list(uint8_t* fb) {
    draw_header(fb, true);
    const esp_app_desc_t* app = esp_app_get_description();
    const esp_partition_t* running = esp_ota_get_running_partition();
    char line[96];
    snprintf(line, sizeof(line), "%s（%s %s）", app->version, app->date, app->time);
    info_row(fb, 190, "当前版本", line);
    info_row(fb, 266, "运行位置", running ? running->label : "—");
    int pct;
    bool ok = battery_ok(&pct);
    if (pct >= 0) snprintf(line, sizeof(line), "%d%%%s", pct, ok ? "" : " · 低于 30%，请先充电");
    else snprintf(line, sizeof(line), "未知");
    info_row(fb, 342, "电量", line);
    if (s_msg[0]) tr_text(fb, TR_MARGIN, 416, 28, s_msg, EPD_DRAW_ALIGN_LEFT, true);
    if (!s_target) {
        tr_wrap(fb, TR_MARGIN, LIST_TOP, TR_W - 2 * TR_MARGIN, 30, 46,
                "当前分区表不支持 TF 卡升级。请先用电脑或网页刷机做一次“完整安装”。", 3);
        return;
    }
    if (!s_count) {
        tr_wrap(fb, TR_MARGIN, LIST_TOP, TR_W - 2 * TR_MARGIN, 30, 46,
                "把编译出的 Read_Pico.bin（或 treader-full.bin）放进 TF 卡的 update 文件夹，再回到这里。", 4);
        return;
    }
    for (int i = 0; i < s_count; i++) {
        const fw_file_t* fw = &s_files[i];
        const int y = LIST_TOP + i * ROW_H;
        if (y + ROW_H > TR_H - 20) break;
        snprintf(line, sizeof(line), "%s", fw->name);
        tr_fit(line, 32, TR_W - 2 * TR_MARGIN);
        tr_text(fb, TR_MARGIN, y + 12, 32, line, EPD_DRAW_ALIGN_LEFT, !fw->problem);
        if (fw->problem) snprintf(line, sizeof(line), "%s", fw->problem);
        else snprintf(line, sizeof(line), "版本 %s · %s · %.1f MB%s", fw->version, fw->date, fw->app_size / 1048576.0,
                      strcmp(fw->version, app->version) ? "" : " · 与当前相同");
        tr_fit(line, 26, TR_W - 2 * TR_MARGIN);
        tr_text(fb, TR_MARGIN, y + 60, 26, line, EPD_DRAW_ALIGN_LEFT, false);
        tr_hline(fb, y + ROW_H - 4, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    }
}

static void draw_confirm(uint8_t* fb) {
    draw_header(fb, true);
    const fw_file_t* fw = &s_files[s_sel];
    char line[160];
    snprintf(line, sizeof(line), "升级到 %s？", fw->version);
    tr_text(fb, TR_MARGIN, 200, 44, line, EPD_DRAW_ALIGN_LEFT, true);
    snprintf(line, sizeof(line), "文件：%s（%s %s）", fw->name, fw->date, fw->time);
    tr_wrap(fb, TR_MARGIN, 284, TR_W - 2 * TR_MARGIN, 28, 42, line, 2);
    tr_wrap(fb, TR_MARGIN, 400, TR_W - 2 * TR_MARGIN, 30, 48,
            "写入约 1 分钟，期间不要拔 TF 卡。完成后自动重启。设置、阅读进度和 TF 卡上的书都会保留。"
            "新固件启动失败时会自动退回当前版本。", 6);
    tr_pill(fb, (EpdRect){TR_MARGIN, BTN_Y, 290, BTN_H}, "取消", false, 32);
    tr_pill(fb, (EpdRect){TR_W - TR_MARGIN - 290, BTN_Y, 290, BTN_H}, "开始升级", true, 32);
}

static void draw_writing(uint8_t* fb) {
    draw_header(fb, false);
    const fw_file_t* fw = &s_files[s_sel];
    const int pct = fw->app_size ? (int)((uint64_t)s_done * 100 / fw->app_size) : 0;
    char line[64];
    snprintf(line, sizeof(line), "正在写入 %s", fw->version);
    tr_text(fb, TR_MARGIN, 420, 40, line, EPD_DRAW_ALIGN_LEFT, true);
    tr_progress(fb, TR_MARGIN, 520, TR_W - 2 * TR_MARGIN, pct);
    snprintf(line, sizeof(line), "%d%% · %.1f / %.1f MB", pct, s_done / 1048576.0, fw->app_size / 1048576.0);
    tr_text(fb, TR_MARGIN, 560, 30, line, EPD_DRAW_ALIGN_LEFT, false);
    tr_text(fb, TR_MARGIN, 640, 30, "不要拔 TF 卡，也不要关机。", EPD_DRAW_ALIGN_LEFT, false);
}

static void draw_done(uint8_t* fb) {
    draw_header(fb, false);
    tr_text(fb, TR_W / 2, 520, 40, "升级完成，正在重启…", EPD_DRAW_ALIGN_CENTER, true);
}

static void render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    ui_clear_page(fb);
    switch (s_view) {
    case V_LIST: draw_list(fb); break;
    case V_CONFIRM: draw_confirm(fb); break;
    case V_WRITING: draw_writing(fb); break;
    case V_DONE: draw_done(fb); break;
    }
}

/* ---- 事件 / Events ---- */

static app_redraw_t on_touch(app_ctx_t* ctx, const cst836u_touch_t* touch) {
    const int x = touch->x, y = touch->y;
    if (s_view == V_WRITING || s_view == V_DONE) return APP_REDRAW_NONE;
    if (y < 160 && x < 120) {
        if (s_view == V_CONFIRM) { s_view = V_LIST; return APP_REDRAW_PAGE; }
        ctx->request_app = &app_tr_settings;
        return APP_REDRAW_NONE;
    }
    if (s_view == V_CONFIRM) {
        if (y < BTN_Y || y >= BTN_Y + BTN_H) return APP_REDRAW_NONE;
        if (x < TR_W / 2) { s_view = V_LIST; return APP_REDRAW_PAGE; }
        int pct;
        if (!battery_ok(&pct)) { snprintf(s_msg, sizeof(s_msg), "电量低于 30%%，请先充电"); s_view = V_LIST; return APP_REDRAW_PAGE; }
        if (tomato_sync_busy()) { snprintf(s_msg, sizeof(s_msg), "后台同步还在进行，等它结束再升级"); s_view = V_LIST; return APP_REDRAW_PAGE; }
        s_msg[0] = 0;
        if (!start_write()) { s_view = V_LIST; return APP_REDRAW_PAGE; }
        s_view = V_WRITING;
        return APP_REDRAW_PAGE;
    }
    if (!s_target || y < LIST_TOP) return APP_REDRAW_NONE;
    const int i = (y - LIST_TOP) / ROW_H;
    if (i >= s_count || s_files[i].problem) return APP_REDRAW_NONE;
    s_sel = i;
    s_msg[0] = 0;
    s_view = V_CONFIRM;
    return APP_REDRAW_PAGE;
}

static app_redraw_t on_key(app_ctx_t* ctx, int key) {
    if (key != UI_KEY_1 || s_view == V_WRITING || s_view == V_DONE) return APP_REDRAW_NONE;
    if (s_view == V_CONFIRM) { s_view = V_LIST; return APP_REDRAW_PAGE; }
    ctx->request_app = &app_tr_settings;
    return APP_REDRAW_NONE;
}

static EpdRect area_hint(app_ctx_t* ctx) {
    (void)ctx;
    return (EpdRect){0, 500, TR_W, 120};
}

static app_redraw_t on_tick(app_ctx_t* ctx) {
    if (s_view == V_DONE) {
        // 让“正在重启”先显示出来再重启。/ Let "restarting" reach the screen first.
        if (ctx->now_ms - s_done_ms >= 1500) esp_restart();
        return APP_REDRAW_NONE;
    }
    if (s_view != V_WRITING) { tr_time_poll(); return APP_REDRAW_NONE; }
    const int r = write_some();
    if (r < 0) {
        ESP_LOGW(TAG, "upgrade failed: %s", s_msg);
        stop_write();
        s_view = V_LIST;
        return APP_REDRAW_PAGE;
    }
    if (r > 0) {
        ESP_LOGI(TAG, "upgrade written; restarting into %s", s_target->label);
        stop_write();
        s_view = V_DONE;
        s_done_ms = ctx->now_ms;
        return APP_REDRAW_PAGE;
    }
    // 每 5% 局部刷一次进度条。/ Refresh the progress bar every 5%.
    const fw_file_t* fw = &s_files[s_sel];
    const int pct = (int)((uint64_t)s_done * 100 / fw->app_size);
    if (pct / 5 == s_drawn_pct / 5) return APP_REDRAW_NONE;
    s_drawn_pct = pct;
    render(ctx, ctx->fb);
    return APP_REDRAW_AREA;
}

static void on_enter(app_ctx_t* ctx) {
    (void)ctx;
    if (!s_files) s_files = heap_caps_calloc(FILE_MAX, sizeof(*s_files), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_view = V_LIST;
    s_sel = -1;
    s_msg[0] = 0;
    s_target = esp_ota_get_next_update_partition(NULL);
    scan();
}

static void page_on_exit(app_ctx_t* ctx) {
    (void)ctx;
    stop_write();
    s_view = V_LIST;
}

static void on_media_lost(app_ctx_t* ctx) {
    (void)ctx;
    if (s_view == V_WRITING) snprintf(s_msg, sizeof(s_msg), "TF 卡被拔出，升级已取消，当前版本不受影响");
    stop_write();
    s_view = V_LIST;
    s_count = 0;
}

const app_desc_t app_tr_update = {
    .title = "系统升级",
    .detail = "从 TF 卡升级固件",
    .enter_full = true,
    .tab_nav = true,
    .holds_pmu = true,           // 升级时电源键不锁屏 / The power key does not lock during an upgrade
    .blocks_idle_sleep = true,   // 也不自动睡眠 / Nor does idle sleep
    .render = render,
    .on_enter = on_enter,
    .on_exit = page_on_exit,
    .on_media_lost = on_media_lost,
    .on_touch = on_touch,
    .on_key = on_key,
    .on_tick = on_tick,
    .area_hint = area_hint,
};
