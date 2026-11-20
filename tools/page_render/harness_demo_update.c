/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 * 中文：系统升级页的演示截图（设计文档用）：TF 卡列表、写入中、在线发现新版本、在线下载中。
 * English: Demo shots of the system upgrade page for the design doc: TF card list, writing, online update found,
 * online download in progress.
 * 冻结：只在电脑上跑。/ Frozen: host-only.
 */
#include "app_tr_update.c"   // 按 -I 找：当前源码或 SRC_ROOT / Found via -I: current sources or SRC_ROOT
#include "page_render.h"

const char* hr_page = "demo_update";

static const esp_app_desc_t s_desc = {.version = "0.9.0", .date = "Nov 21 2026", .time = "15:43:00"};
const esp_app_desc_t* esp_app_get_description(void) { return &s_desc; }
static const esp_partition_t s_run = {.label = "ota_0", .size = 0x300000};
static const esp_partition_t s_next = {.label = "ota_1", .size = 0x300000};
const esp_partition_t* esp_ota_get_running_partition(void) { return &s_run; }
bool tomato_config_has_builtin(void) { return false; }
static fw_file_t s_mock[FILE_MAX];

static void shot(const char* name) {
    app_ctx_t ctx = {.fb = hr_fb};
    hr_no_bar_check();
    render(&ctx, hr_fb);
    hr_shot(name);
}

static void file(int i, const char* name, const char* version, const char* date, uint32_t size, const char* problem) {
    memset(&s_mock[i], 0, sizeof(s_mock[i]));
    snprintf(s_mock[i].name, sizeof(s_mock[i].name), "%s", name);
    snprintf(s_mock[i].version, sizeof(s_mock[i].version), "%s", version);
    snprintf(s_mock[i].date, sizeof(s_mock[i].date), "%s", date);
    snprintf(s_mock[i].time, sizeof(s_mock[i].time), "20:18:42");
    s_mock[i].app_size = size;
    s_mock[i].problem = problem;
}

void hr_scenes(void) {
    s_files = s_mock;
    s_target = &s_next;
    s_view = V_LIST;
    s_msg[0] = 0;
    s_on_msg[0] = 0;
    // 卡上两个固件：一个能用，一个不是 Treader 的。/ Two images on the card: one usable, one not Treader.
    file(0, "Read_Pico.bin", "0.9.1", "Nov 20 2026", 3355400, NULL);
    file(1, "micropython.bin", "", "", 0, "不是 Treader 固件");
    s_count = 2;
    s_on = O_IDLE;
    shot("ui60");
    // 写入中，45%。/ Writing, 45%.
    s_fw = &s_mock[0];
    s_view = V_WRITING;
    s_done = s_mock[0].app_size * 45 / 100;
    shot("ui61");
    s_view = V_LIST;
    // 在线：发现新版本；卡上那个和当前一样。/ Online: an update found; the card's image matches the current one.
    file(0, "Read_Pico.bin", "0.9.0", "Nov 21 2026", 3250586, NULL);
    s_count = 1;
    snprintf(s_info.version, sizeof(s_info.version), "0.9.1");
    s_info.size = 3355400;
    s_has_info = true;
    s_part = 0;
    s_on = O_AVAILABLE;
    shot("ui68");
    // 在线：下载中 38%。/ Online: downloading, 38%.
    s_on = O_DOWNLOADING;
    s_want_dl = true;
    s_dl_phase = TOMATO_DOWNLOADING;
    s_dl_total = 3355400;
    s_dl_done = s_dl_total * 38 / 100;
    shot("ui69");
    s_on = O_IDLE;
}
