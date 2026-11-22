/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 系统升级：从 TF 卡的 update 文件夹（或根目录）读固件，写进另一个程序槽，校验通过后重启。
 * 在线升级先由后台任务把 GitHub 上 treader-latest 的升级包下载到 update 文件夹并校验，再走同一个写入流程；
 * 新固件正常启动后删掉这个升级包。
 * 新固件启动失败时引导程序自动退回旧版本（见 app_main 的 esp_ota_mark_app_valid_cancel_rollback）。
 * 从“关于 Treader”进入，不在菜单里。
 *
 * System upgrade: read firmware from the TF card's update folder (or root), write it to the other app slot,
 * verify and reboot. The online upgrade first has the background task download and verify the treader-latest
 * package from GitHub into the update folder, then uses the same write flow; the package is deleted once the new
 * firmware boots. If the new app fails to boot, the bootloader rolls back (see app_main). Opened from
 * "About Treader"; not in the menu.
 *
 * 冻结：TF 卡里的文件只在用户确认后写；在线升级在用户点“下载并升级”后下载完自动写，带内置服务器的本地版本和
 * 同版本重装要多确认一次；只有线上版本号更大才提示升级；电量低于 30% 且没充电时拒绝；只收 ESP32-S3 的 Read_Pico
 * 镜像；写入在 on_tick 里分块做，本页停留期间不锁屏、不自动睡眠；离开本页就停下下载。
 * Frozen: write a TF card file only after confirmation; the online upgrade writes once its download finishes after
 * the user taps "Download and upgrade", with an extra confirmation for a local build carrying a built-in server or
 * for a same-version reinstall; offer an upgrade only for a higher online version; refuse below 30% battery unless
 * charging; accept only ESP32-S3 Read_Pico images; write in on_tick chunks; no lock or idle sleep while this page is
 * open; leaving the page stops a download.
 */

#include <ctype.h>
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
#include "nvs.h"
#include "read_pico_pmu.h"
#include "read_pico_sd.h"
#include "tomato.h"
#include "tr_time.h"
#include "tr_ui.h"
#include "tr_update.h"
#include "ui_kit.h"
#include "ui_menu.h"

#define TAG "tr_update"
#define FILE_MAX 8
#define NAME_LEN 96
#define CHUNK 4096
#define CHUNKS_PER_TICK 8
#define MERGED_APP_OFFSET 0x10000
#define MIN_BATTERY 30
// 长度随显示大小放大。按钮行不低于屏幕下沿；放大后 TF 卡文件行收紧一点，多放一行。/ Lengths scale with the display
// size. The button row never goes past the screen bottom; at larger sizes TF file rows are a little tighter so one
// more fits.
#define ONLINE_Y TR_S(466)
#define TF_TITLE_Y TR_S(646)
#define LIST_TOP TR_S(700)
#define ROW_H (ui_scale_pct == 100 ? 120 : TR_S(108))
#define BTN_H TR_S(84)
#define BTN_Y (TR_S(980) < TR_H - BTN_H - TR_S(40) ? TR_S(980) : TR_H - BTN_H - TR_S(40))
#define SHA_KEEP 8   // NVS 里记升级包 ELF 摘要的前几个字节 / Leading ELF digest bytes kept in NVS
#define OTA_NVS_NS "tr_ota"

extern const app_desc_t app_tr_settings;

typedef enum { V_LIST, V_CONFIRM, V_OTA_CONFIRM, V_WRITING, V_DONE } view_t;
typedef enum { O_IDLE, O_CHECKING, O_LATEST, O_LOCAL_NEWER, O_AVAILABLE, O_DOWNLOADING, O_FAILED } online_t;

typedef struct {
    char path[NAME_LEN + 24];
    char name[NAME_LEN];
    char version[32];
    char date[16];
    char time[16];
    uint8_t elf_sha[SHA_KEEP];
    uint32_t app_offset;
    uint32_t app_size;
    const char* problem;   // 非空就不能用 / Unusable when set
} fw_file_t;

#define ONLINE_BTN ((EpdRect){TR_W - TR_MARGIN - TR_S(250), ONLINE_Y - TR_S(6), TR_S(250), TR_S(72)})

// 两个并排按钮的宽：放大后不超过半行。/ Width of the two side-by-side buttons: at larger sizes at most half a row.
static int btn_w(void) {
    const int half = (TR_W - 2 * TR_MARGIN - TR_S(24)) / 2;
    return TR_S(290) < half ? TR_S(290) : half;
}

static void draw_buttons(uint8_t* fb, const char* right) {
    const int w = btn_w();
    tr_pill(fb, (EpdRect){TR_MARGIN, BTN_Y, w, BTN_H}, "取消", false, TR_S(32));
    tr_pill(fb, (EpdRect){TR_W - TR_MARGIN - w, BTN_Y, w, BTN_H}, right, true, TR_S(32));
}

// 第 i 个文件行是否完整在屏幕里。/ Whether file row i fits on screen.
static bool row_fits(int i) { return LIST_TOP + (i + 1) * ROW_H <= TR_H - 20; }

static view_t s_view;
static fw_file_t* s_files;   // PSRAM
static int s_count;
static fw_file_t* s_fw;      // 要写的那个：列表里的一项或在线下载的 / The one to write: a list entry or the download
static fw_file_t s_online_fw;
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

// 在线升级的状态。/ Online upgrade state.
static online_t s_on;
static tomato_ota_info_t s_info;
static bool s_has_info;
static bool s_reinstall;        // 线上与当前版本号相同 / Online and current versions match
static uint32_t s_part;         // 卡上已下载的字节 / Bytes already on the card
static char s_on_msg[96];
static uint32_t s_job;          // 我们发起的后台任务序号 / Sequence of our background job
static bool s_job_dl;           // 那个任务是下载（否则是检查）/ That job downloads (else checks)
static bool s_want_dl;          // 用户最后要做的是下载（失败后“重试”据此）/ The user last asked to download (drives Retry)
static int64_t s_last_try_ms;   // 上次尝试开始的时间 / When a pending start was last tried
static bool s_start_pending;    // 等浏览任务停下再开始 / Waiting for a browsing job to stop
static bool s_pending_dl;
static uint32_t s_dl_done, s_dl_total;
static tomato_phase_t s_dl_phase;
static int s_dl_drawn = -1;

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
    // date 在 0x80、ELF 摘要在 0xB0，超出前面读的 0x80 字节，单独再读。
    // date sits at 0x80 and the ELF digest at 0xB0, past the first read.
    uint8_t d[0x38];
    if (fseek(f, (long)base + 0x80, SEEK_SET) == 0 && fread(d, 1, sizeof(d), f) == sizeof(d)) {
        snprintf(out->date, sizeof(out->date), "%.15s", (const char*)d);
        memcpy(out->elf_sha, d + 0x30, SHA_KEEP);
    }
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
    scan_dir(TOMATO_OTA_DIR);
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

static bool card_mounted(void) {
    read_pico_sd_info_t info = {0};
    return read_pico_sd_get_info(&info) == ESP_OK && info.mounted;
}

/* ---- 写入 / Writing ---- */

static void stop_write(void) {
    if (s_ota_open) { esp_ota_abort(s_ota); s_ota_open = false; }
    if (s_fp) { fclose(s_fp); s_fp = NULL; }
    heap_caps_free(s_buf);
    s_buf = NULL;
}

static bool start_write(void) {
    const fw_file_t* fw = s_fw;
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

// 在线下载的升级包写好后记下名字和 ELF 摘要，下次开机确认新固件起来了再删。
// After writing a downloaded package, note its name and ELF digest; the next boot deletes it once the new app is up.
static void remember_package(const fw_file_t* fw) {
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_str(h, "pkg", fw->name) == ESP_OK && nvs_set_blob(h, "sha", fw->elf_sha, SHA_KEEP) == ESP_OK)
        nvs_commit(h);
    nvs_close(h);
}

// 写一批；返回 1 写完，0 继续，-1 出错。/ Write one batch; 1 done, 0 more, -1 error.
static int write_some(void) {
    const fw_file_t* fw = s_fw;
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
    if (fw == &s_online_fw) remember_package(fw);
    err = esp_ota_set_boot_partition(s_target);
    if (err != ESP_OK) { snprintf(s_msg, sizeof(s_msg), "无法切换启动分区（%s）", esp_err_to_name(err)); return -1; }
    return 1;
}

/* ---- 在线升级 / Online upgrade ---- */

// 比较 a.b.c 形式的版本号，后缀不计。/ Compare a.b.c versions, ignoring any suffix.
static int ver_cmp(const char* a, const char* b) {
    for (int i = 0; i < 4; i++) {
        unsigned long x = 0, y = 0;
        while (isdigit((unsigned char)*a) && x < 100000000UL) x = x * 10 + (unsigned long)(*a++ - '0');
        while (isdigit((unsigned char)*b) && y < 100000000UL) y = y * 10 + (unsigned long)(*b++ - '0');
        if (x != y) return x < y ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}

static void part_path(char* out, size_t cap) { snprintf(out, cap, "%s/%s.part", TOMATO_OTA_DIR, s_info.name); }

static void refresh_part(void) {
    s_part = 0;
    if (!s_has_info) return;
    char path[sizeof(TOMATO_OTA_DIR) + sizeof(s_info.name) + 8];
    part_path(path, sizeof(path));
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 0 && (uint64_t)st.st_size <= s_info.size) s_part = (uint32_t)st.st_size;
}

// 删掉其他版本没下完的 .part：线上已经换了版本，续不上了。/ Delete unfinished .part files of other versions.
static void drop_stale_parts(void) {
    DIR* d = opendir(TOMATO_OTA_DIR);
    if (!d) return;
    char keep[sizeof(s_info.name) + 8];
    snprintf(keep, sizeof(keep), "%s.part", s_info.name);
    struct dirent* e;
    while ((e = readdir(d))) {
        const size_t len = strlen(e->d_name);
        static const char part[] = TOMATO_OTA_SUFFIX ".part";
        const size_t plen = sizeof(part) - 1;
        if (len <= 8 + plen || strncmp(e->d_name, "treader-", 8) || strcmp(e->d_name + len - plen, part) ||
            !strcmp(e->d_name, keep)) continue;
        char path[sizeof(TOMATO_OTA_DIR) + 280];
        snprintf(path, sizeof(path), "%s/%s", TOMATO_OTA_DIR, e->d_name);
        ESP_LOGI(TAG, "drop stale %s", path);
        remove(path);
    }
    closedir(d);
}

static void evaluate_check(void) {
    s_has_info = true;
    const int c = ver_cmp(s_info.version, esp_app_get_description()->version);
    s_reinstall = c == 0;
    s_on = c > 0 ? O_AVAILABLE : c == 0 ? O_LATEST : O_LOCAL_NEWER;
    s_on_msg[0] = 0;
    if (card_mounted()) {
        drop_stale_parts();
        refresh_part();
    }
}

static void fail_online(const char* msg) {
    s_on = O_FAILED;
    snprintf(s_on_msg, sizeof(s_on_msg), "%s", msg);
}

// 开始检查或下载；浏览类后台任务先让路，下载和同步不打断。返回状态是否变了。
// Start a check or download; a browsing job yields, downloads and sync are not interrupted. Returns whether state
// changed.
static bool try_start(void) {
    // 我们自己刚暂停的任务还没停下：等它，不算“后台忙”。/ Our own paused job is still stopping: wait, not "busy".
    if (tomato_sync_busy() && tomato_sync_seq() == s_job) return false;
    const tomato_preempt_t p = tomato_sync_preempt();
    if (p == TOMATO_PREEMPT_STOPPING) return false;
    if (p == TOMATO_PREEMPT_BLOCKED) {
        s_start_pending = false;
        fail_online("后台在下载书或同步，等它结束再试");
        return true;
    }
    const esp_err_t err = s_pending_dl ? tomato_sync_start_ota_download(&s_info) : tomato_sync_start_ota_check();
    if (err == ESP_ERR_INVALID_STATE) return false;   // 刚好又有任务，下个 tick 再试 / Raced; retry next tick
    s_start_pending = false;
    if (err != ESP_OK) { fail_online("无法开始，内存不足"); return true; }
    s_job = tomato_sync_seq();
    s_job_dl = s_pending_dl;
    s_on = s_pending_dl ? O_DOWNLOADING : O_CHECKING;
    s_dl_done = s_part;
    s_dl_total = s_info.size;
    s_dl_phase = TOMATO_CONNECTING;
    s_dl_drawn = -1;
    return true;
}

static void begin_job(bool download) {
    s_want_dl = download;
    s_pending_dl = download;
    s_start_pending = true;
    s_on = download ? O_DOWNLOADING : O_CHECKING;
    s_dl_phase = TOMATO_CONNECTING;
    try_start();
}

// 下载前的检查；不通过就把原因写进状态。/ Pre-download checks; on failure the reason goes into the state.
static bool can_download(void) {
    int pct;
    if (!s_target) { fail_online("当前分区表不支持升级，请先用网页刷机做一次“完整安装”"); return false; }
    if (s_info.size > s_target->size) { fail_online("固件超过分区大小，需要用网页刷机做“完整安装”"); return false; }
    if (!card_mounted()) { fail_online("没有 TF 卡：在线升级要先把升级包下载到卡上"); return false; }
    if (!battery_ok(&pct)) { fail_online("电量低于 30%，请先充电"); return false; }
    return true;
}

static void start_download_or_confirm(void) {
    s_want_dl = true;
    if (!can_download()) return;
    // 本地版本带内置服务器，或者同版本重装，先确认一次。/ Confirm first for a built-in server build or a reinstall.
    if (tomato_config_has_builtin() || s_reinstall) { s_view = V_OTA_CONFIRM; return; }
    begin_job(true);
}

// 下载完成：检查文件，走 TF 卡升级的写入流程。/ Download done: inspect the file and use the TF write flow.
static void flash_download(void) {
    fw_file_t* fw = &s_online_fw;
    memset(fw, 0, sizeof(*fw));
    snprintf(fw->name, sizeof(fw->name), "%s", s_info.name);
    snprintf(fw->path, sizeof(fw->path), "%s/%s", TOMATO_OTA_DIR, s_info.name);
    inspect(fw);
    int pct;
    if (fw->problem) { fail_online(fw->problem); return; }
    if (fw->app_offset || strcmp(fw->version, s_info.version)) { fail_online("下载的固件和线上版本对不上"); return; }
    if (!battery_ok(&pct)) { fail_online("已下载好，但电量低于 30%，充电后再点升级"); return; }
    s_fw = fw;
    s_msg[0] = 0;
    if (!start_write()) { fail_online(s_msg); return; }
    s_view = V_WRITING;
}

// 后台任务结束后收结果。/ Collect the result once the background job ends.
static void finish_job(const tomato_status_t* st) {
    if (!s_job_dl) {
        if (st->phase == TOMATO_DONE && tomato_sync_ota_info(&s_info)) evaluate_check();
        else fail_online(st->message[0] ? st->message : "检查失败");
        return;
    }
    if (card_mounted()) refresh_part();
    if (st->phase == TOMATO_DONE) { s_part = 0; flash_download(); return; }
    if (st->error == ESP_ERR_INVALID_STATE) {
        // 暂停：回到可续传状态，提示已下载多少。/ Paused: back to resumable, noting what is on the card.
        s_on = s_reinstall ? O_LATEST : O_AVAILABLE;
        snprintf(s_on_msg, sizeof(s_on_msg), "%s", st->message);
        return;
    }
    fail_online(st->message[0] ? st->message : "下载失败");
}

// 离开页面或丢卡时停下我们的任务。/ Stop our job when leaving the page or losing the card.
static void stop_online(void) {
    s_start_pending = false;
    if ((s_on == O_CHECKING || s_on == O_DOWNLOADING) && tomato_sync_busy() && tomato_sync_seq() == s_job)
        tomato_sync_cancel();
    if (s_on == O_CHECKING) s_on = s_has_info ? O_FAILED : O_IDLE;
    if (s_on == O_DOWNLOADING) {
        // 卡上已经有的字节，暂停后照样显示“继续下载”。/ Bytes already on the card still show as resumable.
        if (s_dl_phase != TOMATO_CONNECTING && s_dl_done > s_part && s_dl_done < s_info.size) s_part = s_dl_done;
        s_on = s_reinstall ? O_LATEST : O_AVAILABLE;
    }
}

void tr_update_boot_cleanup(void) {
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    char name[NAME_LEN];
    size_t name_len = sizeof(name);
    uint8_t sha[SHA_KEEP];
    size_t sha_len = sizeof(sha);
    if (nvs_get_str(h, "pkg", name, &name_len) == ESP_OK) {
        const bool have_sha = nvs_get_blob(h, "sha", sha, &sha_len) == ESP_OK && sha_len == SHA_KEEP;
        const bool booted = have_sha && !memcmp(esp_app_get_description()->app_elf_sha256, sha, SHA_KEEP);
        // 只删在线升级自己起的名字，不碰别的文件。/ Delete only names the online upgrade made, nothing else.
        bool done = true;
        if (booted && !strchr(name, '/') && !strncmp(name, "treader-", 8)) {
            char path[sizeof(TOMATO_OTA_DIR) + NAME_LEN + 2];
            snprintf(path, sizeof(path), "%s/%s", TOMATO_OTA_DIR, name);
            struct stat st;
            if (stat(path, &st) == 0) {
                ESP_LOGI(TAG, "new firmware is up; removing %s", path);
                remove(path);
            } else {
                // 这张卡上没有，可能换了卡：记录留着，下次开机见到原来的卡再删。
                // Not on this card, maybe a swap: keep the record and delete on a later boot with the original card.
                done = false;
            }
        } else if (!booted) {
            ESP_LOGW(TAG, "running app is not the downloaded one (rolled back?); keeping %s", name);
        }
        if (done) {
            nvs_erase_key(h, "pkg");
            nvs_erase_key(h, "sha");
            nvs_commit(h);
        }
    }
    nvs_close(h);
}

/* ---- 绘制 / Drawing ---- */

static void info_row(uint8_t* fb, int y, const char* label, const char* value) {
    tr_text(fb, TR_MARGIN, y, TR_S(30), label, EPD_DRAW_ALIGN_LEFT, false);
    char v[96];
    snprintf(v, sizeof(v), "%s", value);
    tr_fit(v, TR_S(28), TR_W - 2 * TR_MARGIN - TR_S(170));
    tr_text(fb, TR_W - TR_MARGIN, y + TR_S(2), TR_S(28), v, EPD_DRAW_ALIGN_RIGHT, false);
    tr_hline(fb, y + TR_S(54), TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
}

static void draw_header(uint8_t* fb, bool back) {
    tr_status_bar(fb);
    if (back) tr_text(fb, TR_MARGIN, TR_S(96), TR_S(44), "‹", EPD_DRAW_ALIGN_LEFT, false);
    tr_text(fb, TR_MARGIN + TR_S(56), TR_S(100), TR_S(36), "系统升级", EPD_DRAW_ALIGN_LEFT, true);
}

static const char* online_button(void) {
    switch (s_on) {
    case O_CHECKING: return NULL;
    case O_DOWNLOADING: return "暂停";
    case O_LATEST: return "重新安装";
    case O_AVAILABLE: return s_part ? "继续下载" : "下载并升级";
    case O_FAILED: return s_has_info && s_want_dl ? "重试" : "检查更新";
    default: return "检查更新";
    }
}

static void draw_online(uint8_t* fb) {
    tr_text(fb, TR_MARGIN, ONLINE_Y + TR_S(4), TR_S(32), "在线升级", EPD_DRAW_ALIGN_LEFT, true);
    const char* button = online_button();
    if (button) tr_pill(fb, ONLINE_BTN, button, s_on == O_AVAILABLE, TR_S(28));
    char line[128];
    const char* note = NULL;
    switch (s_on) {
    case O_IDLE: snprintf(line, sizeof(line), "从 GitHub 下载最新版本，先存到 TF 卡，校验后再写入"); break;
    case O_CHECKING: snprintf(line, sizeof(line), s_dl_phase == TOMATO_CONNECTING ? "连接 Wi-Fi…" : "正在检查线上版本…"); break;
    case O_LATEST:
        snprintf(line, sizeof(line), "已是最新（线上 %s）", s_info.version);
        note = s_on_msg[0] ? s_on_msg : "“重新安装”会装上 GitHub 上这个版本号的最新构建";
        break;
    case O_LOCAL_NEWER: snprintf(line, sizeof(line), "当前版本比线上新（线上 %s）", s_info.version); break;
    case O_AVAILABLE:
        if (s_part) snprintf(line, sizeof(line), "发现 %s · %.1f MB · 已下载 %.1f MB", s_info.version,
                             s_info.size / 1048576.0, s_part / 1048576.0);
        else snprintf(line, sizeof(line), "发现 %s · %.1f MB", s_info.version, s_info.size / 1048576.0);
        note = s_on_msg[0] ? s_on_msg : "先下载到 TF 卡 update 文件夹，校验通过后自动写入并重启";
        break;
    case O_DOWNLOADING: {
        if (s_dl_phase == TOMATO_CONNECTING) { snprintf(line, sizeof(line), "连接 Wi-Fi…"); break; }
        const int pct = s_dl_total ? (int)((uint64_t)s_dl_done * 100 / s_dl_total) : 0;
        snprintf(line, sizeof(line), "下载中 %d%% · %.1f / %.1f MB", pct, s_dl_done / 1048576.0, s_dl_total / 1048576.0);
        tr_progress(fb, TR_MARGIN, ONLINE_Y + TR_S(104), TR_W - 2 * TR_MARGIN, pct);
        break;
    }
    case O_FAILED: snprintf(line, sizeof(line), "%s", s_on_msg); break;
    }
    tr_fit(line, TR_S(26), TR_W - 2 * TR_MARGIN);
    tr_text(fb, TR_MARGIN, ONLINE_Y + TR_S(62), TR_S(26), line, EPD_DRAW_ALIGN_LEFT, false);
    if (note) {
        char n[128];
        snprintf(n, sizeof(n), "%s", note);
        tr_fit(n, TR_S(24), TR_W - 2 * TR_MARGIN);
        tr_text(fb, TR_MARGIN, ONLINE_Y + TR_S(106), TR_S(24), n, EPD_DRAW_ALIGN_LEFT, false);
    }
    tr_hline(fb, TF_TITLE_Y - TR_S(20), TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
}

static void draw_list(uint8_t* fb) {
    draw_header(fb, true);
    const esp_app_desc_t* app = esp_app_get_description();
    const esp_partition_t* running = esp_ota_get_running_partition();
    char line[96];
    snprintf(line, sizeof(line), "%s（%s %s）", app->version, app->date, app->time);
    info_row(fb, TR_S(190), "当前版本", line);
    info_row(fb, TR_S(266), "运行位置", running ? running->label : "—");
    int pct;
    bool ok = battery_ok(&pct);
    if (pct >= 0) snprintf(line, sizeof(line), "%d%%%s", pct, ok ? "" : " · 低于 30%，请先充电");
    else snprintf(line, sizeof(line), "未知");
    info_row(fb, TR_S(342), "电量", line);
    if (s_msg[0]) tr_text(fb, TR_MARGIN, TR_S(410), TR_S(28), s_msg, EPD_DRAW_ALIGN_LEFT, true);
    draw_online(fb);
    tr_text(fb, TR_MARGIN, TF_TITLE_Y, TR_S(32), "TF 卡升级", EPD_DRAW_ALIGN_LEFT, true);
    if (!s_target) {
        tr_wrap(fb, TR_MARGIN, LIST_TOP, TR_W - 2 * TR_MARGIN, TR_S(30), TR_S(46),
                "当前分区表不支持 TF 卡升级。请先用电脑或网页刷机做一次“完整安装”。", 3);
        return;
    }
    if (!s_count) {
        tr_wrap(fb, TR_MARGIN, LIST_TOP, TR_W - 2 * TR_MARGIN, TR_S(30), TR_S(46),
                "把编译出的 Read_Pico.bin（或 treader-full.bin）放进 TF 卡的 update 文件夹，再回到这里。", 4);
        return;
    }
    for (int i = 0; i < s_count; i++) {
        const fw_file_t* fw = &s_files[i];
        const int y = LIST_TOP + i * ROW_H;
        if (!row_fits(i)) break;
        snprintf(line, sizeof(line), "%s", fw->name);
        tr_fit(line, TR_S(32), TR_W - 2 * TR_MARGIN);
        tr_text(fb, TR_MARGIN, y + TR_S(12), TR_S(32), line, EPD_DRAW_ALIGN_LEFT, !fw->problem);
        if (fw->problem) snprintf(line, sizeof(line), "%s", fw->problem);
        else snprintf(line, sizeof(line), "版本 %s · %s · %.1f MB%s", fw->version, fw->date, fw->app_size / 1048576.0,
                      strcmp(fw->version, app->version) ? "" : " · 与当前相同");
        tr_fit(line, TR_S(26), TR_W - 2 * TR_MARGIN);
        if (fw->problem) tr_text(fb, TR_MARGIN, y + TR_S(60), TR_S(26), line, EPD_DRAW_ALIGN_LEFT, false);
        else tr_text_dim(fb, TR_MARGIN, y + TR_S(60), TR_S(26), line, EPD_DRAW_ALIGN_LEFT);
        tr_hline(fb, y + ROW_H - 4, TR_MARGIN, TR_W - 2 * TR_MARGIN, true);
    }
}

static void draw_confirm(uint8_t* fb) {
    draw_header(fb, true);
    const fw_file_t* fw = s_fw;
    char line[160];
    snprintf(line, sizeof(line), "升级到 %s？", fw->version);
    tr_text(fb, TR_MARGIN, TR_S(200), TR_S(44), line, EPD_DRAW_ALIGN_LEFT, true);
    snprintf(line, sizeof(line), "文件：%s（%s %s）", fw->name, fw->date, fw->time);
    tr_wrap(fb, TR_MARGIN, TR_S(284), TR_W - 2 * TR_MARGIN, TR_S(28), TR_S(42), line, 2);
    tr_wrap(fb, TR_MARGIN, TR_S(400), TR_W - 2 * TR_MARGIN, TR_S(30), TR_S(48),
            "写入约 1 分钟，期间不要拔 TF 卡。完成后自动重启。设置、阅读进度和 TF 卡上的书都会保留。"
            "新固件启动失败时会自动退回当前版本。", 6);
    draw_buttons(fb, "开始升级");
}

static void draw_ota_confirm(uint8_t* fb) {
    draw_header(fb, true);
    char line[160];
    if (s_reinstall) snprintf(line, sizeof(line), "重新安装 %s？", s_info.version);
    else snprintf(line, sizeof(line), "升级到 %s？", s_info.version);
    tr_text(fb, TR_MARGIN, TR_S(200), TR_S(44), line, EPD_DRAW_ALIGN_LEFT, true);
    int y = TR_S(290);
    if (s_reinstall)
        y += TR_S(48) * tr_wrap(fb, TR_MARGIN, y, TR_W - 2 * TR_MARGIN, TR_S(30), TR_S(48),
                                "会装上 GitHub 上这个版本号的最新构建，替换当前固件。", 3) + TR_S(24);
    if (tomato_config_has_builtin())
        y += TR_S(48) * tr_wrap(fb, TR_MARGIN, y, TR_W - 2 * TR_MARGIN, TR_S(30), TR_S(48),
                          "当前是带内置番茄服务器的本地版本。升级后换成公开版，公开版没有内置服务器；"
                                "设置里自己填的服务器不受影响。", 4) + TR_S(24);
    snprintf(line, sizeof(line), "先下载 %.1f MB 到 TF 卡 update 文件夹，校验通过后自动写入并重启。网络断了可以接着下。",
             s_info.size / 1048576.0);
    tr_wrap(fb, TR_MARGIN, y, TR_W - 2 * TR_MARGIN, TR_S(30), TR_S(48), line, 4);
    draw_buttons(fb, "开始");
}

static void draw_writing(uint8_t* fb) {
    draw_header(fb, false);
    const fw_file_t* fw = s_fw;
    const int pct = fw->app_size ? (int)((uint64_t)s_done * 100 / fw->app_size) : 0;
    char line[64];
    snprintf(line, sizeof(line), "正在写入 %s", fw->version);
    tr_text(fb, TR_MARGIN, TR_S(420), TR_S(40), line, EPD_DRAW_ALIGN_LEFT, true);
    tr_progress(fb, TR_MARGIN, TR_S(520), TR_W - 2 * TR_MARGIN, pct);
    snprintf(line, sizeof(line), "%d%% · %.1f / %.1f MB", pct, s_done / 1048576.0, fw->app_size / 1048576.0);
    tr_text(fb, TR_MARGIN, TR_S(560), TR_S(30), line, EPD_DRAW_ALIGN_LEFT, false);
    tr_text(fb, TR_MARGIN, TR_S(640), TR_S(30), "不要拔 TF 卡，也不要关机。", EPD_DRAW_ALIGN_LEFT, false);
}

static void draw_done(uint8_t* fb) {
    draw_header(fb, false);
    tr_text(fb, TR_W / 2, TR_S(520), TR_S(40), "升级完成，正在重启…", EPD_DRAW_ALIGN_CENTER, true);
}

static void render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    ui_clear_page(fb);
    switch (s_view) {
    case V_LIST: draw_list(fb); break;
    case V_CONFIRM: draw_confirm(fb); break;
    case V_OTA_CONFIRM: draw_ota_confirm(fb); break;
    case V_WRITING: draw_writing(fb); break;
    case V_DONE: draw_done(fb); break;
    }
}

/* ---- 事件 / Events ---- */

static app_redraw_t touch_online(void) {
    switch (s_on) {
    case O_CHECKING: return APP_REDRAW_NONE;
    case O_DOWNLOADING:
        stop_online();
        return APP_REDRAW_PAGE;
    case O_AVAILABLE:
    case O_LATEST:
        start_download_or_confirm();
        return APP_REDRAW_PAGE;
    case O_FAILED:
        if (s_has_info && s_want_dl) start_download_or_confirm();
        else begin_job(false);
        return APP_REDRAW_PAGE;
    default:
        begin_job(false);
        return APP_REDRAW_PAGE;
    }
}

static app_redraw_t on_touch(app_ctx_t* ctx, const cst836u_touch_t* touch) {
    const int x = touch->x, y = touch->y;
    if (s_view == V_WRITING || s_view == V_DONE) return APP_REDRAW_NONE;
    if (y < TR_S(160) && x < TR_S(120)) {
        if (s_view == V_CONFIRM || s_view == V_OTA_CONFIRM) { s_view = V_LIST; return APP_REDRAW_PAGE; }
        ctx->request_app = &app_tr_settings;
        return APP_REDRAW_NONE;
    }
    if (s_view == V_CONFIRM || s_view == V_OTA_CONFIRM) {
        if (y < BTN_Y || y >= BTN_Y + BTN_H) return APP_REDRAW_NONE;
        const bool online = s_view == V_OTA_CONFIRM;
        s_view = V_LIST;
        if (x < TR_W / 2) return APP_REDRAW_PAGE;
        if (online) {
            if (can_download()) begin_job(true);
            return APP_REDRAW_PAGE;
        }
        int pct;
        if (!battery_ok(&pct)) { snprintf(s_msg, sizeof(s_msg), "电量低于 30%%，请先充电"); return APP_REDRAW_PAGE; }
        if (tomato_sync_busy()) { snprintf(s_msg, sizeof(s_msg), "后台同步还在进行，等它结束再升级"); return APP_REDRAW_PAGE; }
        s_msg[0] = 0;
        if (!start_write()) return APP_REDRAW_PAGE;
        s_view = V_WRITING;
        return APP_REDRAW_PAGE;
    }
    // 按钮热区上下各放宽一点。/ Pad the button's hit area a little.
    const EpdRect btn = ONLINE_BTN;
    const EpdRect hit = {btn.x - TR_S(20), btn.y - TR_S(16), btn.width + TR_S(20), btn.height + TR_S(32)};
    if (ui_rect_hit(hit, (uint16_t)x, (uint16_t)y)) return touch_online();
    if (!s_target || y < LIST_TOP) return APP_REDRAW_NONE;
    // 在线升级在跑时不碰 TF 卡列表。/ Leave the TF list alone while the online upgrade runs.
    if (s_on == O_CHECKING || s_on == O_DOWNLOADING) return APP_REDRAW_NONE;
    const int i = (y - LIST_TOP) / ROW_H;
    if (i >= s_count || !row_fits(i) || s_files[i].problem) return APP_REDRAW_NONE;
    s_fw = &s_files[i];
    s_msg[0] = 0;
    s_view = V_CONFIRM;
    return APP_REDRAW_PAGE;
}

static app_redraw_t on_key(app_ctx_t* ctx, int key) {
    if (key != UI_KEY_1 || s_view == V_WRITING || s_view == V_DONE) return APP_REDRAW_NONE;
    if (s_view == V_CONFIRM || s_view == V_OTA_CONFIRM) { s_view = V_LIST; return APP_REDRAW_PAGE; }
    ctx->request_app = &app_tr_settings;
    return APP_REDRAW_NONE;
}

static EpdRect area_hint(app_ctx_t* ctx) {
    (void)ctx;
    // 列表里只刷在线升级的状态行和进度条；写入页刷进度条。
    // On the list refresh the online status line and bar; on the write view, its bar.
    if (s_view == V_LIST) return (EpdRect){0, ONLINE_Y + TR_S(56), TR_W, TR_S(84)};
    return (EpdRect){0, TR_S(500), TR_W, TR_S(120)};
}

static app_redraw_t tick_online(app_ctx_t* ctx) {
    if (s_start_pending) {
        // 抢占会阻塞最多 0.3 秒，隔半秒才试一次，别让界面一直卡着。
        // Preempting can block up to 0.3 s; try every half second so the UI stays responsive.
        if (ctx->now_ms - s_last_try_ms < 500) return APP_REDRAW_NONE;
        s_last_try_ms = ctx->now_ms;
        return try_start() ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
    }
    if (s_on != O_CHECKING && s_on != O_DOWNLOADING) return APP_REDRAW_NONE;
    tomato_status_t st;
    tomato_sync_get_status(&st);
    const bool ours = tomato_sync_seq() == s_job;
    if (ours && tomato_sync_busy()) {
        const bool phase_changed = st.phase != s_dl_phase;
        s_dl_phase = st.phase;
        if (s_on == O_CHECKING) return phase_changed ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
        if (st.total > 0) { s_dl_done = (uint32_t)st.current; s_dl_total = (uint32_t)st.total; }
        const int pct = s_dl_total ? (int)((uint64_t)s_dl_done * 100 / s_dl_total) : 0;
        // 每 2% 局部刷一次。/ Partial refresh every 2%.
        if (!phase_changed && pct / 2 == s_dl_drawn / 2) return APP_REDRAW_NONE;
        s_dl_drawn = pct;
        render(ctx, ctx->fb);
        return phase_changed ? APP_REDRAW_PAGE : APP_REDRAW_AREA;
    }
    if (ours) finish_job(&st);
    else fail_online("后台任务被打断，请重试");
    return APP_REDRAW_PAGE;
}

static app_redraw_t on_tick(app_ctx_t* ctx) {
    if (s_view == V_DONE) {
        // 让“正在重启”先显示出来再重启。/ Let "restarting" reach the screen first.
        if (ctx->now_ms - s_done_ms >= 1500) esp_restart();
        return APP_REDRAW_NONE;
    }
    if (s_view != V_WRITING) {
        tr_time_poll();
        app_redraw_t r = tick_online(ctx);
        // 下载完成后 flash_download 可能已经切到写入页。/ flash_download may have switched to the write view.
        return r;
    }
    const int r = write_some();
    if (r < 0) {
        ESP_LOGW(TAG, "upgrade failed: %s", s_msg);
        stop_write();
        if (s_fw == &s_online_fw) fail_online(s_msg);
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
    const int pct = (int)((uint64_t)s_done * 100 / s_fw->app_size);
    if (pct / 5 == s_drawn_pct / 5) return APP_REDRAW_NONE;
    s_drawn_pct = pct;
    render(ctx, ctx->fb);
    return APP_REDRAW_AREA;
}

static void on_enter(app_ctx_t* ctx) {
    (void)ctx;
    if (!s_files) s_files = heap_caps_calloc(FILE_MAX, sizeof(*s_files), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_view = V_LIST;
    s_fw = NULL;
    s_msg[0] = 0;
    s_target = esp_ota_get_next_update_partition(NULL);
    // 上次离开时停下的检查或下载，回来显示可以接着做的状态。/ A job stopped on the last exit shows as resumable.
    if (s_on == O_CHECKING || s_on == O_DOWNLOADING) stop_online();
    if (s_has_info && card_mounted()) refresh_part();
    scan();
}

static void page_on_exit(app_ctx_t* ctx) {
    (void)ctx;
    // 已经写完并切好启动分区：离页（比如按了 KEY3）也要马上重启，不能留到下次关机才悄悄换版本。
    // Written and the boot slot already switched: leaving (e.g. KEY3) must restart now rather than switch versions
    // silently at the next power-off.
    if (s_view == V_DONE) esp_restart();
    stop_write();
    stop_online();
    s_view = V_LIST;
}

static void on_media_lost(app_ctx_t* ctx) {
    (void)ctx;
    if (s_view == V_WRITING) snprintf(s_msg, sizeof(s_msg), "TF 卡被拔出，升级已取消，当前版本不受影响");
    if (s_on == O_DOWNLOADING) {
        stop_online();
        snprintf(s_on_msg, sizeof(s_on_msg), "TF 卡被拔出，下载已停下；插回后可以接着下");
    }
    stop_write();
    s_view = V_LIST;
    s_count = 0;
    s_part = 0;
}

const app_desc_t app_tr_update = {
    .title = "系统升级",
    .detail = "在线或从 TF 卡升级固件",
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
