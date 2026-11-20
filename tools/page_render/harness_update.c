/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：系统升级页的渲染测试。/ English: Render bench for the system upgrade page.
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "app_tr_update.c"   // 按 -I 找：当前源码或 SRC_ROOT / Found via -I: current sources or SRC_ROOT
#include "page_render.h"

const char* hr_page = "update";

static const esp_app_desc_t s_desc = {.version = "1.42.0", .date = "Nov 21 2026", .time = "15:43:00"};
const esp_app_desc_t* esp_app_get_description(void) { return &s_desc; }
static const esp_partition_t s_run = {.label = "ota_0", .size = 0x300000};
static const esp_partition_t s_next = {.label = "ota_1", .size = 0x300000};
const esp_partition_t* esp_ota_get_running_partition(void) { return &s_run; }
static bool s_builtin;
bool tomato_config_has_builtin(void) { return s_builtin; }
static fw_file_t s_mock[FILE_MAX];

static void shot(const char* name) {
    app_ctx_t ctx = {.fb = hr_fb};
    hr_no_bar_check();
    render(&ctx, hr_fb);
    hr_shot(name);
}

static void file(int i, const char* name, const char* version, const char* problem) {
    memset(&s_mock[i], 0, sizeof(s_mock[i]));
    snprintf(s_mock[i].name, sizeof(s_mock[i].name), "%s", name);
    snprintf(s_mock[i].version, sizeof(s_mock[i].version), "%s", version);
    snprintf(s_mock[i].date, sizeof(s_mock[i].date), "Nov 20 2026");
    snprintf(s_mock[i].time, sizeof(s_mock[i].time), "10:11:12");
    s_mock[i].app_size = 2345678;
    s_mock[i].problem = problem;
}

void hr_scenes(void) {
    s_files = s_mock;
    s_target = &s_next;
    s_view = V_LIST;
    s_on = O_IDLE;
    s_count = 0;
    s_msg[0] = 0;
    shot("empty");
    file(0, "Read_Pico.bin", "1.43.0", NULL);
    file(1, "treader-full-with-a-very-long-file-name-v1.42.0.bin", "1.42.0", NULL);
    file(2, "notes.bin", "", "不是 ESP32 固件");
    s_count = 3;
    hr_allow_fit();
    shot("files");
    snprintf(s_info.version, sizeof(s_info.version), "1.44.0");
    s_info.size = 2456789;
    s_has_info = true;
    s_on = O_AVAILABLE;
    s_part = 1200000;
    snprintf(s_msg, sizeof(s_msg), "电量低于 30%%，请先充电");
    hr_allow_fit();
    shot("available");
    s_msg[0] = 0;
    s_on = O_DOWNLOADING;
    s_dl_phase = TOMATO_DOWNLOADING;
    s_dl_done = 1500000;
    s_dl_total = 2456789;
    hr_allow_fit();
    shot("downloading");
    s_on = O_LATEST;
    s_on_msg[0] = 0;
    hr_allow_fit();
    shot("latest");
    s_on = O_FAILED;
    s_want_dl = true;
    snprintf(s_on_msg, sizeof(s_on_msg), "当前分区表不支持升级，请先用网页刷机做一次“完整安装”");
    hr_allow_fit();
    shot("failed");
    s_on = O_CHECKING;
    s_dl_phase = TOMATO_CONNECTING;
    s_count = 8;
    for (int i = 3; i < 8; i++) file(i, "Read_Pico-old.bin", "1.30.0", NULL);
    hr_allow_fit();
    shot("checking_many");
    s_count = 3;
    s_target = NULL;
    s_on = O_IDLE;
    shot("no_target");
    s_target = &s_next;
    s_fw = &s_mock[0];
    s_view = V_CONFIRM;
    shot("confirm");
    s_view = V_OTA_CONFIRM;
    s_reinstall = true;
    s_builtin = true;
    shot("ota_confirm");
    s_view = V_WRITING;
    s_done = 1000000;
    s_fw = &s_mock[0];
    shot("writing");
    s_view = V_DONE;
    shot("done");
}
