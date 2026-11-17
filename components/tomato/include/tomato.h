/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 番茄小说同步：连接自建 Tomato-Novel-Downloader 服务器，搜索、下载 TXT、检查更新。
 * 网络只在一次操作期间打开，做完即断；所有阻塞操作在后台任务里跑，页面只轮询状态。
 *
 * Tomato novel sync: talks to a self-hosted Tomato-Novel-Downloader server to search,
 * download TXT and check updates. The network is up only for one operation; blocking
 * work runs on a background task and pages poll the status.
 *
 * 冻结：TXT 写入 /sdcard/books，交给系统书架阅读；番茄元数据只存 /sdcard/tomato/。
 * Frozen: TXT goes to /sdcard/books for the system shelf; Tomato metadata lives only in /sdcard/tomato/.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TOMATO_DIR "/sdcard/tomato"
#define TOMATO_CONFIG_PATH TOMATO_DIR "/config.json"
#define TOMATO_SHELF_PATH TOMATO_DIR "/shelf.json"
#define TOMATO_BOOKS_DIR "/sdcard/books"

#define TOMATO_ID_LEN 24
#define TOMATO_TITLE_LEN 96
#define TOMATO_AUTHOR_LEN 48
#define TOMATO_SHELF_MAX 64
#define TOMATO_SEARCH_MAX 12

typedef struct {
    char url[128];      ///< 服务器地址，不带末尾斜杠 / Server base URL, no trailing slash
    char password[64];  ///< x-tomato-password / Access password
} tomato_config_t;

/// 书架上的一本番茄书。/ One Tomato book on the shelf.
typedef struct {
    char book_id[TOMATO_ID_LEN];
    char title[TOMATO_TITLE_LEN];
    char author[TOMATO_AUTHOR_LEN];
    uint16_t chapters;       ///< 本地 TXT 对应的章节数 / Chapters in the local TXT
    uint16_t remote;         ///< 最近一次检查到的服务器章节数 / Server chapters at last check
    char file[TOMATO_TITLE_LEN + 8]; ///< books 目录下的文件名 / File name under books dir
} tomato_book_t;

/// 搜索结果。/ Search result.
typedef struct {
    char book_id[TOMATO_ID_LEN];
    char title[TOMATO_TITLE_LEN];
    char author[TOMATO_AUTHOR_LEN];
    char category[16];
    uint16_t chapters;
    uint32_t words;
    bool finished;           ///< 已完结 / Completed
} tomato_hit_t;

/* ---- 配置与书架（同步，读写 TF 卡）/ Config and shelf (synchronous, TF I/O) ---- */

/// 读 config.json；文件缺失或字段为空返回 ESP_ERR_NOT_FOUND。
/// Read config.json; ESP_ERR_NOT_FOUND when missing or empty.
esp_err_t tomato_config_load(tomato_config_t* out);
esp_err_t tomato_config_save(const tomato_config_t* cfg);

/// 书架读写。load 失败时 count 置 0。/ Shelf I/O. On load failure count is 0.
esp_err_t tomato_shelf_load(tomato_book_t* books, int max, int* count);
esp_err_t tomato_shelf_save(const tomato_book_t* books, int count);

/* ---- 后台同步 / Background sync ---- */

typedef enum {
    TOMATO_IDLE = 0,
    TOMATO_CONNECTING,   ///< 连 Wi-Fi / Joining Wi-Fi
    TOMATO_CHECKING,     ///< 等服务器更新扫描 / Waiting for the server update scan
    TOMATO_SEARCHING,
    TOMATO_QUEUED,       ///< 服务器排队或生成中 / Server job queued or running
    TOMATO_DOWNLOADING,  ///< 下载 TXT / Fetching TXT
    TOMATO_DONE,
    TOMATO_FAILED,
} tomato_phase_t;

typedef struct {
    tomato_phase_t phase;
    esp_err_t error;          ///< FAILED 时的原因 / Reason when FAILED
    char message[64];         ///< 给用户看的一句话 / One-line user message
    int current;              ///< 第几本 / Book index in progress (1-based)
    int total;                ///< 共几本 / Books in this run
    uint32_t bytes;           ///< 当前文件已收字节 / Bytes received for the current file
    int updated;              ///< 本轮更新了几本 / Books updated this run
    int hit_count;            ///< 搜索结果数 / Search result count
    uint32_t revision;        ///< 每次变化加一 / Bumped on every change
} tomato_status_t;

/// 检查书架所有书的更新并下载有新章节的。/ Check every shelf book and download those with new chapters.
esp_err_t tomato_sync_start_update(void);
/// 搜索；结果用 tomato_sync_hits() 取。/ Search; read results with tomato_sync_hits().
esp_err_t tomato_sync_start_search(const char* query);
/// 下载一本并加入书架（已在书架则覆盖更新）。/ Download one book and add it to the shelf (refresh if present).
esp_err_t tomato_sync_start_download(const tomato_hit_t* hit);
/// 请求取消；任务在下一个安全点退出。/ Request cancel; the task exits at its next safe point.
void tomato_sync_cancel(void);
/// 后台任务是否在跑。/ Whether the task is running.
bool tomato_sync_busy(void);
void tomato_sync_get_status(tomato_status_t* out);
/// 复制最近一次搜索结果，返回条数。/ Copy the latest search results; returns the count.
int tomato_sync_hits(tomato_hit_t* out, int max);

#ifdef __cplusplus
}
#endif
