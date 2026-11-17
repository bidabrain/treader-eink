/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 番茄小说同步：连接自建 Tomato-Novel-Downloader 服务器，搜索、下载 TXT、检查更新。
 * 检查和下载是两个独立操作，都只在用户点击时运行，不自动检查。
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
/// 番茄书封面原图：covers/<books 下的文件名>.img。/ Tomato cover image: covers/<file name under books>.img.
#define TOMATO_COVER_DIR TOMATO_DIR "/covers"
/// 书城榜单缓存目录。/ Bookstore rank cache.
#define TOMATO_RANK_DIR TOMATO_DIR "/store"
#define TOMATO_RANK_CAT_MAX 48
#define TOMATO_RANK_BOOK_MAX 20
#define TOMATO_RANK_INTRO_LEN 900

#define TOMATO_ID_LEN 24
#define TOMATO_TITLE_LEN 96
#define TOMATO_AUTHOR_LEN 48
#define TOMATO_SHELF_MAX 64
#define TOMATO_SEARCH_MAX 12
#define TOMATO_ABSTRACT_LEN 320
#define TOMATO_THUMB_LEN 400

typedef struct {
    char url[128];      ///< 服务器地址，不带末尾斜杠 / Server base URL, no trailing slash
    char password[64];  ///< x-tomato-password / Access password
    bool builtin;       ///< 用的是编译时内置的默认服务器，界面不得显示地址和密码 / Built-in default; UI must not show it
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
    char abstract[TOMATO_ABSTRACT_LEN]; ///< 简介，按字符边界截断 / Synopsis, cut on a character boundary
    char thumb[TOMATO_THUMB_LEN];       ///< 封面图地址，太长则为空 / Cover image URL; empty when too long
} tomato_hit_t;

/// 书城的一个分类。/ One bookstore category.
typedef struct {
    char name[32];       ///< 分类名，如“都市日常” / Category name
    char url[200];       ///< 榜单站内的相对地址（已转义）/ URL relative to the rank site (escaped)
    bool male;           ///< 男频 / Male channel
} tomato_rank_cat_t;

/// 榜单上的一本书。/ One book on a rank list.
typedef struct {
    char book_id[TOMATO_ID_LEN];
    char title[TOMATO_TITLE_LEN];
    char author[TOMATO_AUTHOR_LEN];
    char reads[16];                      ///< 在读人数，如“25.4万” / Readers, e.g. "25.4万"
    char intro[TOMATO_RANK_INTRO_LEN];   ///< 简介，按字符边界截断 / Synopsis, cut on a character boundary
    char cover[TOMATO_THUMB_LEN];        ///< 封面地址，太长则为空 / Cover URL; empty when too long
} tomato_rank_book_t;

/// WebDAV 同步配置，和番茄配置存在同一个 config.json。/ WebDAV settings, stored in the same config.json.
typedef struct {
    char url[160];        ///< 目录地址，同步文件放在其下 / Directory URL; the sync file lives under it
    char user[64];
    char password[64];
} tomato_webdav_config_t;

/// 一本书的阅读进度，供 WebDAV 同步读写。/ One book's reading progress for WebDAV sync.
typedef struct {
    char path[288];
    uint32_t size;        ///< 记录时的文件大小 / File size when recorded
    uint32_t off;         ///< 章内字节偏移 / Byte offset within the chapter
    uint16_t chapter;
    uint8_t px;
    uint8_t pct;
} tomato_progress_t;

/// 列出本机全部阅读进度，返回条数。/ List all local progress records; returns the count.
typedef int (*tomato_progress_export_t)(tomato_progress_t* out, int max);
/// 写入一条进度；本机没有这本书或文件对不上时返回 false。/ Store one record; false when the book is missing or does not match.
typedef bool (*tomato_progress_import_t)(const tomato_progress_t* in);

/* ---- 配置与书架（同步，读写 TF 卡）/ Config and shelf (synchronous, TF I/O) ---- */

/// 读 config.json；没填服务器时用编译时内置的默认服务器（builtin 置位），都没有返回 ESP_ERR_NOT_FOUND。
/// Read config.json; without a server there, fall back to the built-in default (builtin set); ESP_ERR_NOT_FOUND if neither.
esp_err_t tomato_config_load(tomato_config_t* out);
/// 固件里有没有内置默认服务器。/ Whether the firmware carries a built-in default server.
bool tomato_config_has_builtin(void);
esp_err_t tomato_config_save(const tomato_config_t* cfg);
/// WebDAV 配置读写；保存时保留 config.json 里的其他字段。/ WebDAV config I/O; saving keeps other config.json fields.
esp_err_t tomato_webdav_config_load(tomato_webdav_config_t* out);
esp_err_t tomato_webdav_config_save(const tomato_webdav_config_t* cfg);

/// 书城榜单地址：config.json 的 store_url，没填用编译时默认（local.properties 的 STORE_URL）。
/// Rank site URL: store_url in config.json, else the build default (STORE_URL in local.properties).
esp_err_t tomato_store_url_load(char* out, size_t cap);
/// 保存自定义榜单地址；空串表示恢复默认。/ Save a custom rank URL; empty restores the default.
esp_err_t tomato_store_url_save(const char* url);
/// 编译时的默认榜单地址。/ The build's default rank URL.
const char* tomato_store_url_default(void);

/* ---- 书城（读 TF 卡缓存，不联网）/ Bookstore (reads the TF cache, no network) ---- */

/// 读缓存的分类列表；没有缓存返回 -1。date 是榜单日期。/ Read cached categories; -1 without a cache.
int tomato_rank_load_index(tomato_rank_cat_t* out, int max, char* date, size_t date_cap);
/// 读缓存的分类榜单；没有缓存返回 -1。/ Read a cached category list; -1 without a cache.
int tomato_rank_load_books(const tomato_rank_cat_t* cat, tomato_rank_book_t* out, int max);
/// 这个分类有没有缓存。/ Whether this category is cached.
bool tomato_rank_cat_cached(const tomato_rank_cat_t* cat);
/// 详情页封面的本地路径（.jpg 后缀，内容可能是 PNG）。/ Local path of a detail cover (.jpg name; may hold PNG).
void tomato_rank_cover_path(const char* book_id, char* out, size_t cap);

/// 书架读写。load 失败时 count 置 0。/ Shelf I/O. On load failure count is 0.
esp_err_t tomato_shelf_load(tomato_book_t* books, int max, int* count);
esp_err_t tomato_shelf_save(const tomato_book_t* books, int count);
/// 书库删了文件后，从番茄书架去掉对应记录（file 是 books 目录下的文件名）。
/// After the library deletes a file, drop its Tomato shelf entry (file is the name under the books dir).
esp_err_t tomato_shelf_remove_file(const char* file);

/* ---- 后台同步 / Background sync ---- */

typedef enum {
    TOMATO_IDLE = 0,
    TOMATO_CONNECTING,   ///< 连 Wi-Fi / Joining Wi-Fi
    TOMATO_CHECKING,     ///< 等服务器更新扫描 / Waiting for the server update scan
    TOMATO_SEARCHING,
    TOMATO_QUEUED,       ///< 服务器排队或生成中 / Server job queued or running
    TOMATO_DOWNLOADING,  ///< 下载 TXT / Fetching TXT
    TOMATO_WEBDAV,       ///< WebDAV 同步中 / WebDAV sync in progress
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
    int pending;              ///< 检查后有新章节的本数 / Books with new chapters after a check
    int hit_count;            ///< 搜索结果数 / Search result count
    uint32_t revision;        ///< 每次变化加一 / Bumped on every change
} tomato_status_t;

/// 只检查不下载：向服务器取最新章节数，写进书架的 remote 字段。
/// Check only: fetch the latest chapter counts into each shelf book's remote field; downloads nothing.
esp_err_t tomato_sync_start_check(void);
/// 下载书架上 remote 大于 chapters 的书；用上次检查的结果，不重新检查。
/// Download shelf books whose remote exceeds chapters, using the last check; does not re-check.
esp_err_t tomato_sync_start_update(void);
/// 搜索；结果用 tomato_sync_hits() 取。/ Search; read results with tomato_sync_hits().
esp_err_t tomato_sync_start_search(const char* query);
/// 下载一本并加入书架（已在书架则覆盖更新）。/ Download one book and add it to the shelf (refresh if present).
esp_err_t tomato_sync_start_download(const tomato_hit_t* hit);
/// 取书城榜单：cat 为 NULL 只取分类列表，否则顺带取这个分类。不需要番茄服务器。
/// Fetch ranks: NULL fetches the category index only, else also this category. Needs no Tomato server.
esp_err_t tomato_sync_start_rank(const tomato_rank_cat_t* cat);
/// 下载一本榜单书的封面到 tomato_rank_cover_path()。/ Fetch one rank book's cover to tomato_rank_cover_path().
esp_err_t tomato_sync_start_rank_cover(const tomato_rank_book_t* book);
/// WebDAV 同步阅读进度和番茄书架。/ Sync reading progress and the Tomato shelf over WebDAV.
esp_err_t tomato_sync_start_webdav(void);
/// 注册阅读进度的读写回调（进度存储在主程序里）。/ Register progress I/O callbacks (progress lives in the main app).
void tomato_sync_set_progress_io(tomato_progress_export_t export_cb, tomato_progress_import_t import_cb);
/// 下载替换 TXT 后回调（在同步任务里调用），用来迁移阅读进度。old_size 为 0 表示新书。
/// Called on the sync task after a TXT is replaced, to migrate reading progress. old_size 0 means a new book.
typedef void (*tomato_file_hook_t)(const char* path, uint32_t old_size, uint32_t new_size);
void tomato_sync_set_file_hook(tomato_file_hook_t hook);
/// 系统时间是否已经对过（联网时用 SNTP 对时）。/ Whether system time has been set (SNTP while online).
bool tomato_time_valid(void);
/// 上次取走之后是否又用 SNTP 对过时（取走即清零）。主程序据此把时间写进 PMU 的 RTC。
/// Whether SNTP set the time since the last call (cleared on read); the app then writes it to the PMU RTC.
bool tomato_take_time_synced(void);
/// 判断一个路径是不是阅读器正开着的书；是的话更新会跳过它（阅读器还持有旧文件）。
/// Whether a path is the book open in the reader; updates skip it because the reader still holds the old file.
typedef bool (*tomato_open_check_t)(const char* path);
void tomato_sync_set_open_check(tomato_open_check_t check);
/// 取消并等待后台任务结束，最多 timeout_ms；锁屏断电前调用。返回是否已停。
/// Cancel and wait up to timeout_ms for the task to end; call before a power-down lock. Returns whether it stopped.
bool tomato_sync_stop(int timeout_ms);
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
