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
/// 虎扑缓存目录：home.json、post/<tid>.json、img/。/ Hupu cache: home.json, post/<tid>.json, img/.
#define TOMATO_HUPU_DIR "/sdcard/hupu"
#define TOMATO_HUPU_HOME_FILE TOMATO_HUPU_DIR "/home.json"
#define TOMATO_RED_DIR "/sdcard/rednote"
#define TOMATO_RED_FEED_FILE TOMATO_RED_DIR "/feed.json"
#define TOMATO_RED_REC_FILE TOMATO_RED_DIR "/rec.json"
/// 微博缓存：follow.json、hot.json、p/<id>/。/ Weibo cache: follow.json, hot.json, p/<id>/.
#define TOMATO_WB_DIR "/sdcard/weibo"
#define TOMATO_WB_FOLLOW_FILE TOMATO_WB_DIR "/follow.json"
#define TOMATO_WB_HOT_FILE TOMATO_WB_DIR "/hot.json"
/// X 缓存：foryou.json、following.json、state.json（服务器上哪些流开着）、p/<id>/。/ X cache: foryou.json,
/// following.json, state.json (which feeds the server has on), p/<id>/.
#define TOMATO_X_DIR "/sdcard/x"
#define TOMATO_X_FORYOU_FILE TOMATO_X_DIR "/foryou.json"
#define TOMATO_X_FOLLOWING_FILE TOMATO_X_DIR "/following.json"
#define TOMATO_X_STATE_FILE TOMATO_X_DIR "/state.json"
/// WEBTOON 发现缓存：store.json（上次的列表）、search.json、t/<id>.json（作品详情）、c/<id>.jpg（封面）。下载的漫画
/// 不在这里，在我的漫画的 comics/<作品名>/。/ WEBTOON store cache: store.json (the last list), search.json,
/// t/<id>.json (title details), c/<id>.jpg (covers). Downloaded comics live on the comic shelf under comics/<title>/.
#define TOMATO_WT_DIR "/sdcard/webtoon"
#define TOMATO_WT_STORE_FILE TOMATO_WT_DIR "/store.json"
#define TOMATO_WT_SEARCH_FILE TOMATO_WT_DIR "/search.json"
/// 服务器上可选的来源（GET /sources 原样存下；旧服务器没有这个接口时不存在）。/ The server's sources (GET /sources
/// as is; absent with an older server that lacks it).
#define TOMATO_WT_SOURCES_FILE TOMATO_WT_DIR "/sources.json"
/// WEBTOON 自己的来源 key；其余来源是服务器上的 Mihon 插件来源，作品编号以 m 开头。/ WEBTOON's own source key; the
/// others are Mihon extension sources on the server, whose title ids start with m.
#define TOMATO_WT_SRC_WEBTOON "webtoon"
#define TOMATO_WT_COMICS "/sdcard/comics"
/// 订阅标记：comics/<作品名>/.webtoon。/ Subscription marker: comics/<title>/.webtoon.
#define TOMATO_WT_MARK ".webtoon"
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
    uint16_t seen;           ///< 上次打开时的章节数；旧文件缺省为 chapters / Chapters when last opened; defaults to chapters
    char file[TOMATO_TITLE_LEN + 8]; ///< books 目录下的文件名 / File name under books dir
} tomato_book_t;

/// 检查到、还没下载的新章节数。/ New chapters found by a check but not downloaded yet.
static inline int tomato_book_pending(const tomato_book_t* b) {
    return b->remote > b->chapters ? b->remote - b->chapters : 0;
}
/// 已下载、打开后还没看的新章节数。/ New chapters downloaded but not opened since.
static inline int tomato_book_fresh(const tomato_book_t* b) {
    return b->chapters > b->seen ? b->chapters - b->seen : 0;
}

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
/// 写入一条进度的结果。/ Result of storing one record.
typedef enum {
    TOMATO_IMPORT_OK,         ///< 写好了 / Stored
    TOMATO_IMPORT_MISSING,    ///< 本机没有这本书或文件比记录时小 / The book is missing here or smaller than recorded
    TOMATO_IMPORT_MISMATCH,   ///< 章节或位置超出本机这本书（文件和备份时不一样）/ Chapter or offset past this book (a different file)
} tomato_import_result_t;
/// 写入一条进度。/ Store one record.
typedef tomato_import_result_t (*tomato_progress_import_t)(const tomato_progress_t* in);

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

/* ---- 局域网代理 / LAN proxy ---- */

typedef enum {
    TOMATO_PROXY_SOCKS5 = 0,   ///< SOCKS5，域名交给代理解析 / SOCKS5, names resolved by the proxy
    TOMATO_PROXY_HTTP = 1,     ///< HTTP CONNECT 隧道 / HTTP CONNECT tunnel
} tomato_proxy_type_t;

typedef struct {
    bool on;            ///< 开关 / Enabled
    uint8_t type;       ///< tomato_proxy_type_t
    uint16_t port;      ///< 代理端口 / Proxy port
    char host[64];      ///< 代理地址（IPv4 或主机名）/ Proxy address (IPv4 or host name)
    char user[40];      ///< 用户名，空表示不认证 / User name; empty means no authentication
    char pass[64];      ///< 密码 / Password
} tomato_proxy_t;

/// 读代理设置（NVS 命名空间 proxy）；没设过就是关。/ Read the proxy settings (NVS namespace proxy); off when never set.
esp_err_t tomato_proxy_load(tomato_proxy_t* out);
/// 保存并立即生效，对之后新建的连接有效。/ Save and apply at once, for connections opened afterwards.
esp_err_t tomato_proxy_save(const tomato_proxy_t* cfg);
/// 把“192.168.1.5:7890”或带 socks5:// / http:// 头的文字解析进 cfg 的 host、port、type；格式不对返回 false。
/// Parse "192.168.1.5:7890", optionally with a socks5:// or http:// prefix, into host, port and type; false if malformed.
bool tomato_proxy_parse(const char* text, tomato_proxy_t* cfg);

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
/// 保存时和磁盘上的书架合并 seen（取大），后台任务手里的旧副本不会冲掉“已看”。
/// Saving merges seen with the shelf on disk (larger wins), so a stale copy held by the worker cannot undo "seen".
esp_err_t tomato_shelf_save(const tomato_book_t* books, int count);
/// 打开了这本书：seen 记成当前章节数（file 是 books 目录下的文件名）；不是番茄书时什么也不做。
/// The book was opened: set seen to its chapters (file is the name under the books dir); no-op for other books.
esp_err_t tomato_shelf_mark_seen(const char* file);
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
    char message[96];         ///< 给用户看的一句话（可能带失败原因）/ One-line user message (may carry the cause)
    int current;              ///< 第几本 / Book index in progress (1-based)
    int total;                ///< 共几本 / Books in this run
    uint32_t bytes;           ///< 当前文件已收字节 / Bytes received for the current file
    uint32_t expect;          ///< 当前文件预计大小，0 是不知道 / Expected size of the current file; 0 when unknown
    int updated;              ///< 本轮更新了几本 / Books updated this run
    int pending;              ///< 检查后有新章节的本数 / Books with new chapters after a check
    int hit_count;            ///< 搜索结果数 / Search result count
    uint32_t revision;        ///< 每次变化加一 / Bumped on every change
    char step[96];            ///< 正在做的那一步（漫画下载用），可能为空 / The step under way (comic downloads); may be empty
} tomato_status_t;

/// 只检查不下载：向服务器取最新章节数，写进书架的 remote 字段。
/// Check only: fetch the latest chapter counts into each shelf book's remote field; downloads nothing.
esp_err_t tomato_sync_start_check(void);
/// 只检查书架上的这一本，有新章节就直接下载（正在读就只记下）。/ Check just this shelf book and download new chapters
/// at once (only noted when it is open in the reader).
esp_err_t tomato_sync_start_check_one(const char* book_id);
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
/// 一次联网下载这些榜单书缺的封面到 tomato_rank_cover_path()；目录太大先清理。
/// Fetch these rank books' missing covers to tomato_rank_cover_path() in one session; prunes an oversized dir first.
esp_err_t tomato_sync_start_rank_covers(const tomato_rank_book_t* books, int n);
/// cJSON 节点改放 PSRAM（全局生效，可重复调用）。解析大 JSON 前调用。/ Put cJSON nodes in PSRAM (global,
/// idempotent). Call before parsing large JSON.
void tomato_json_use_psram(void);

/// 虎扑后台任务种类。/ Hupu background jobs.
typedef enum {
    TOMATO_HUPU_HOME,       ///< 推荐和缩略图；列表写好时进度到 1 / Feed and thumbnails; progress 1 once the list is written
    TOMATO_HUPU_POST,       ///< 一帖和正文图片；文字写好时进度到 1 / One post and its images; progress 1 once text is written
    TOMATO_HUPU_ZONES,      ///< 全部专区 / All zones
    TOMATO_HUPU_THREADS,    ///< 专区帖子：id 专区，sort 0 最新回复 1 最新发布，more 追加下一页 / Zone posts; more appends a page
    TOMATO_HUPU_FOLLOW,     ///< 关注的专区混排 / Interleaved feed of followed zones
    TOMATO_HUPU_SCHEDULE,   ///< 赛程：id 赛事（nba、cba…）/ Schedule; id is the league (nba, cba...)
    TOMATO_HUPU_RATING,     ///< 比赛评分：id 条目类型，key 条目号 / Match ratings; id is the item type, key the item number
} tomato_hupu_job_t;

/// 虎扑请求。/ Hupu request.
typedef struct {
    tomato_hupu_job_t job;
    char id[40];
    char key[24];
    int sort;
    bool more;
    uint8_t pages;      ///< 关注：每个专区取几页，0 当 1 / Following: pages per zone, 0 counts as 1
} tomato_hupu_req_t;

/// 在后台任务里跑一件虎扑的事；只读，不需要番茄服务器。/ Run one Hupu job on the sync task; read only, no Tomato server.
esp_err_t tomato_sync_start_hupu(const tomato_hupu_req_t* req);
/// 帖子缓存文件。/ Cached post file.
/// 虎扑缓存文件锁：界面读写缓存和后台清理缓存互斥。/ Hupu cache file lock shared by UI cache I/O and worker pruning.
void tomato_hupu_cache_lock(void);
void tomato_hupu_cache_unlock(void);

/* ---- 缓存文件 / Cache files ---- */

/// 开机挂卡后调用一次：小红书、微博、X 的帖子缓存放进 TF 卡上的缓存包（/sdcard/.treader/cache），旧的帖子目录第一次
/// 用到时挪进回收站后台删掉。generation 是卡的挂载代数，free_bytes 是卡上剩余字节，都可以为 NULL。
/// Call once after the card is mounted at boot: Xiaohongshu, Weibo and X post caches go into the cache pack on the TF
/// card (/sdcard/.treader/cache), and the old post folders move to the trash on first use and are deleted in the
/// background. generation is the card's mount generation and free_bytes its free byte count; either may be NULL.
void tomato_cache_pack_setup(uint32_t (*generation)(void), uint64_t (*free_bytes)(void));
/// 缓存文件有没有、多大（缓存包里的或普通文件）。/ Whether a cache file exists and its size (in the pack or a file).
bool tomato_cache_size(const char* path, size_t* len);
/// 整个读进 PSRAM（末尾补 0），用 heap_caps_free 放掉；比 max 大返回 ESP_ERR_INVALID_SIZE。/ Read it whole into PSRAM
/// (with a trailing 0), freed with heap_caps_free; larger than max returns ESP_ERR_INVALID_SIZE.
esp_err_t tomato_cache_load(const char* path, size_t max, uint8_t** out, size_t* len);
/// 写一个缓存文件，数据分两段（a 在前 b 在后）；断电不会留下半个。/ Write a cache file from two parts (a then b); a power
/// cut never leaves half of one.
esp_err_t tomato_cache_store2(const char* path, const void* a, size_t alen, const void* b, size_t blen);
/// 读 JSON 缓存文件，读不出或解析不了返回 NULL。/ Read a JSON cache file; NULL when unreadable or unparsable.
struct cJSON* tomato_cache_read_json(const char* path);
/// 从 JPEG 缓存文件头读宽高。/ Read width and height from a cached JPEG's header.
bool tomato_cache_jpeg_size(const char* path, int* w, int* h);
void tomato_hupu_post_path(const char* tid, char* out, size_t cap);
/// 图片地址对应的本地 JPG（缩略图缓存同名加尺寸）。/ Local JPEG for an image URL (thumbnail caches share the name).
void tomato_hupu_img_path(const char* url, char* out, size_t cap);
/// 虎扑要不要下图（缩略图和正文图）；关着时只取文字。主程序按设置调用，默认关。/ Whether Hupu downloads images
/// (thumbnails and body images); off fetches text only. The app sets it from settings; off by default.
void tomato_hupu_set_images(bool on);
/// 从虎扑图片文件名里读原图尺寸（…_w_1920_h_967_…）。/ Read the original size from a Hupu image name.
bool tomato_hupu_img_size(const char* url, int* w, int* h);
/// 专区帖子、赛程、评分的缓存文件。/ Cache files for zone posts, schedules and ratings.
void tomato_hupu_zone_path(const char* zone, int sort, char* out, size_t cap);
void tomato_hupu_schedule_path(const char* league, char* out, size_t cap);
void tomato_hupu_rating_path(const char* type, const char* no, char* out, size_t cap);
/* ---- 小红书（经自建 rednote 服务器）/ Xiaohongshu via the self-hosted rednote server ---- */

/// 自建服务器（rednote、weibo）的地址和设备令牌，存在 NVS。/ Self-hosted server (rednote, weibo) address and
/// device token, kept in NVS.
typedef struct {
    char url[160];
    char token[96];
} tomato_srv_config_t;

typedef tomato_srv_config_t tomato_red_config_t;

/// 关注、热门列表一次取几条：20 / 30 / 50，默认 20（1.2.3 起，原来 100 / 200 / 300）。列表数组按 TOMATO_LIST_CAP 分配，
/// 推荐列表的条数由服务器定，也装得下。
/// Posts per following or hot list: 20 / 30 / 50, 20 by default (from 1.2.3; was 100 / 200 / 300). List arrays are sized
/// by TOMATO_LIST_CAP, which also fits recommendation lists whose length the server decides.
#define TOMATO_LIST_LIMIT_MIN 20
#define TOMATO_LIST_LIMIT_MID 30
#define TOMATO_LIST_LIMIT_MAX 50
#define TOMATO_LIST_CAP 300
int tomato_red_list_limit(void);
esp_err_t tomato_red_list_limit_set(int n);

/// “往后翻接着拉服务器”（小红书关注、微博关注、X 两段共用，默认关）：开着时翻完已经拉到的，就向服务器要更早的一批，
/// 直到和卡上旧缓存接上；列表最多 TOMATO_LIST_CAP 条。关着时刷新后直接接卡上旧缓存，最多 100 条。
/// "Keep pulling from the server" (shared by Xiaohongshu Following, Weibo Following and both X segments; off by
/// default): when on, paging past what was fetched asks the server for the next older batch until it joins the old
/// cache on the card, with lists up to TOMATO_LIST_CAP; when off, a refresh is followed directly by the old cache, up to
/// 100.
bool tomato_feed_more_on(void);
esp_err_t tomato_feed_more_set(bool on);

/// 没设置地址返回 ESP_ERR_NOT_FOUND（结构体清零）。/ ESP_ERR_NOT_FOUND (zeroed struct) when no address is set.
esp_err_t tomato_red_config_load(tomato_red_config_t* out);
esp_err_t tomato_red_config_save(const tomato_red_config_t* cfg);
/// 每保存一次加一（设置页据此发现网页填写已提交）。/ Bumped on every save (settings notices a web submission).
uint32_t tomato_red_config_revision(void);

/// 小红书后台任务种类。/ Xiaohongshu background jobs.
typedef enum {
    TOMATO_RED_FEED,   ///< 关注列表和缩略图；列表写好时进度到 1 / Following list and thumbnails; progress 1 once written
    TOMATO_RED_REC,    ///< 推荐列表和封面；refresh 时服务器才读小红书 / Recommendations; the server reads Xiaohongshu on refresh
    TOMATO_RED_NOTE,   ///< 一篇笔记和图片；正文写好时进度到 1 / One note and its images; progress 1 once text is written
    TOMATO_RED_TEST,   ///< 测试连接 / Connection test
} tomato_red_job_t;

typedef struct {
    tomato_red_job_t job;
    char id[28];
    bool rec;           ///< 笔记来自推荐（服务器可能要现拉）/ Note comes from recommendations (server may fetch live)
    bool refresh;       ///< 推荐：让服务器重新读首页 / Recommendations: ask the server to re-read the home feed
    bool images_only;   ///< 笔记已缓存，只补缺的图 / Note cached; fetch missing images only
    bool more_comments; ///< 笔记：让服务器现拉更多评论（慢）/ Note: have the server fetch more comments now (slow)
    bool thumbs_only;      ///< 列表：不重新取，只补卡上这份列表的缩略图 / List: no refetch, fill thumbnails of the list on the card
    uint16_t thumbs_from;  ///< 列表：从第几条（从 0 起）开始下缩略图 / List: first item (from 0) to fetch thumbnails for
    /// 列表：往后拉一批，接在 id 那条后面（它带着服务器给的游标）/ List: pull the next older batch after entry id (which
    /// holds the server's cursor)
    bool older;
    uint16_t thumbs_count; ///< 列表：下几条的缩略图，0 是全部 / List: how many items get thumbnails; 0 means all
} tomato_red_req_t;

esp_err_t tomato_sync_start_red(const tomato_red_req_t* req);
/// 最近一次启动的任务序号（启动成功后马上读就是自己的）。忙且序号相同，才是自己的任务在跑；不忙且序号相同，状态就是它的结果。
/// Sequence number of the most recently started job (read right after a successful start to get your own). Busy with
/// the same number means your job runs; idle with the same number means the status is its result.
uint32_t tomato_sync_seq(void);
/// 笔记目录、第 n 张图（n 为 0 是缩略图）、推荐封面。/ Note folder, image n (0 = thumbnail), recommendation cover.
void tomato_red_note_dir(const char* id, char* out, size_t cap);
void tomato_red_img_path(const char* id, int n, char* out, size_t cap);
void tomato_red_cover_path(const char* id, char* out, size_t cap);
/// 关注列表里标记看过；调用方保证此时没有关注列表任务在写。/ Mark a following note as seen; the caller makes sure no
/// following-list job is writing at the time.
void tomato_red_mark_seen(const char* id);

/* ---- 微博（经自建 weibo 服务器）/ Weibo via the self-hosted weibo server ---- */

typedef tomato_srv_config_t tomato_wb_config_t;

/// 见 tomato_red_list_limit；关注和热门用同一个值。/ See tomato_red_list_limit; following and hot share it.
int tomato_wb_list_limit(void);
esp_err_t tomato_wb_list_limit_set(int n);

/// 没设置地址返回 ESP_ERR_NOT_FOUND（结构体清零）。/ ESP_ERR_NOT_FOUND (zeroed struct) when no address is set.
esp_err_t tomato_wb_config_load(tomato_wb_config_t* out);
esp_err_t tomato_wb_config_save(const tomato_wb_config_t* cfg);
/// 每保存一次加一（设置页据此发现网页填写已提交）。/ Bumped on every save (settings notices a web submission).
uint32_t tomato_wb_config_revision(void);

/// 微博后台任务种类。/ Weibo background jobs.
typedef enum {
    TOMATO_WB_FEED,       ///< 关注或热门列表和缩略图；列表写好时进度到 1 / Following or hot list and thumbnails; progress 1 once written
    TOMATO_WB_POST,       ///< 一条微博和图片；正文写好时进度到 1 / One post and its images; progress 1 once text is written
    TOMATO_WB_COMMENTS,   ///< 一条微博的评论（服务器第一次会去微博取）/ One post's comments (the server fetches them once)
    TOMATO_WB_TEST,       ///< 测试连接 / Connection test
} tomato_wb_job_t;

typedef struct {
    tomato_wb_job_t job;
    char id[24];
    bool hot;           ///< 列表：热门；否则关注 / List: hot; otherwise following
    bool images_only;   ///< 微博已缓存，只补缺的图 / Post cached; fetch missing images only
    bool thumbs_only;      ///< 列表：不重新取，只补卡上这份列表的缩略图 / List: no refetch, fill thumbnails of the list on the card
    uint16_t thumbs_from;  ///< 列表：从第几条（从 0 起）开始下缩略图 / List: first item (from 0) to fetch thumbnails for
    /// 列表：往后拉一批，接在 id 那条后面（它带着服务器给的游标）/ List: pull the next older batch after entry id (which
    /// holds the server's cursor)
    bool older;
    uint16_t thumbs_count; ///< 列表：下几条的缩略图，0 是全部 / List: how many items get thumbnails; 0 means all
} tomato_wb_req_t;

esp_err_t tomato_sync_start_wb(const tomato_wb_req_t* req);
/// 微博目录、第 n 张图（n 为 0 是缩略图）、评论文件。/ Post folder, image n (0 = thumbnail), comments file.
void tomato_wb_post_dir(const char* id, char* out, size_t cap);
void tomato_wb_img_path(const char* id, int n, char* out, size_t cap);
void tomato_wb_comments_path(const char* id, char* out, size_t cap);
/// 第 k 张原图的方形小图（k 从 1 起；服务器 0.1.3 起才有）。/ Square small image of source image k (from 1; servers
/// from 0.1.3 on).
void tomato_wb_square_path(const char* id, int k, char* out, size_t cap);
/// 列表里每条最多取几张方形小图；正文页最多显示几张。/ Most square images fetched per list post; most shown on a post.
#define TOMATO_WB_LIST_SQUARES 3
#define TOMATO_WB_POST_SQUARES 4
/// 关注列表里标记看过；调用方保证此时没有关注列表任务在写。/ Mark a following post as seen; the caller makes sure no
/// following-list job is writing at the time.
void tomato_wb_mark_seen(const char* id);

/* ---- X（经自建 x 服务器）/ X via the self-hosted x server ---- */

typedef tomato_srv_config_t tomato_x_config_t;

/// 见 tomato_red_list_limit；为你推荐和正在关注用同一个值。/ See tomato_red_list_limit; For You and Following share it.
int tomato_x_list_limit(void);
esp_err_t tomato_x_list_limit_set(int n);
/// 没设置地址返回 ESP_ERR_NOT_FOUND（结构体清零）。/ ESP_ERR_NOT_FOUND (zeroed struct) when no address is set.
esp_err_t tomato_x_config_load(tomato_x_config_t* out);
esp_err_t tomato_x_config_save(const tomato_x_config_t* cfg);
/// 每保存一次加一（设置页据此发现网页填写已提交）。/ Bumped on every save (settings notices a web submission).
uint32_t tomato_x_config_revision(void);

/// X 后台任务种类。/ X background jobs.
typedef enum {
    TOMATO_X_FEED,       ///< 为你推荐或正在关注列表和缩略图；列表写好时进度到 1 / A list and thumbnails; progress 1 once written
    TOMATO_X_POST,       ///< 一条推文和图片；正文写好时进度到 1 / One post and its images; progress 1 once text is written
    TOMATO_X_COMMENTS,   ///< 一条推文的回复（服务器第一次会去 X 取）/ One post's replies (the server fetches them once)
    TOMATO_X_STATE,      ///< 只问服务器哪些流开着 / Only ask the server which feeds are on
    TOMATO_X_TEST,       ///< 测试连接 / Connection test
} tomato_x_job_t;

typedef struct {
    tomato_x_job_t job;
    char id[28];
    bool following;     ///< 列表：正在关注；否则为你推荐 / List: Following; otherwise For You
    bool images_only;   ///< 推文已缓存，只补缺的图 / Post cached; fetch missing images only
    bool thumbs_only;      ///< 列表：不重新取，只补卡上这份列表的缩略图 / List: no refetch, fill thumbnails of the list on the card
    uint16_t thumbs_from;  ///< 列表：从第几条（从 0 起）开始下缩略图 / List: first item (from 0) to fetch thumbnails for
    /// 列表：往后拉一批，接在 id 那条后面（它带着服务器给的游标）/ List: pull the next older batch after entry id (which
    /// holds the server's cursor)
    bool older;
    uint16_t thumbs_count; ///< 列表：下几条的缩略图，0 是全部 / List: how many items get thumbnails; 0 means all
} tomato_x_req_t;

esp_err_t tomato_sync_start_x(const tomato_x_req_t* req);

/* ---- WEBTOON 漫画（经自建 webtoon 服务器）/ WEBTOON comics via the self-hosted webtoon server ---- */

typedef tomato_srv_config_t tomato_wt_config_t;

/// 没设置地址返回 ESP_ERR_NOT_FOUND（结构体清零）。/ ESP_ERR_NOT_FOUND (zeroed struct) when no address is set.
esp_err_t tomato_wt_config_load(tomato_wt_config_t* out);
esp_err_t tomato_wt_config_save(const tomato_wt_config_t* cfg);
/// 每保存一次加一（设置页据此发现网页填写已提交）。/ Bumped on every save (settings notices a web submission).
uint32_t tomato_wt_config_revision(void);

/// 语言版本：0 繁体中文（默认），1 English。/ Language version: 0 traditional Chinese (default), 1 English.
#define TOMATO_WT_LANGS 2
int tomato_wt_lang(void);
esp_err_t tomato_wt_lang_set(int lang);
const char* tomato_wt_lang_code(int lang);
const char* tomato_wt_lang_name(int lang);
/// 中文版文字转成简体显示（默认开）。/ Show the Chinese edition's text in simplified characters (on by default).
bool tomato_wt_simp(void);
esp_err_t tomato_wt_simp_set(bool on);
/// 每次检查更新最多下载几话：5 / 10 / 20 / 50，默认 10。/ Most episodes downloaded per update check: 5 / 10 / 20 /
/// 50, 10 by default.
int tomato_wt_max(void);
esp_err_t tomato_wt_max_set(int n);

/// 发现页选中的来源（NVS），默认 TOMATO_WT_SRC_WEBTOON。/ The store's chosen source (NVS), TOMATO_WT_SRC_WEBTOON by
/// default.
void tomato_wt_src(char* out, size_t cap);
esp_err_t tomato_wt_src_set(const char* key);
/// 编号是插件来源的作品（m 开头）。/ Whether an id belongs to an extension source (starts with m).
bool tomato_wt_is_ext(const char* id);
/// 订阅标记的内容（comics/<作品名>/.webtoon，JSON）。/ The subscription marker (comics/<title>/.webtoon, JSON).
typedef struct {
    char id[16];          ///< 作品编号，Canvas 作品前面带 c，插件来源带 m / Title id; Canvas ids start with c, extension ids with m
    char lang[12];        ///< 语言版本 / Language version
    char title[96];
    char author[64];
    int last;             ///< 检查更新从这话之后问；只推进到第一个没下好的话前面 / Checks ask for episodes after this; it only moves up to just before the first episode not yet downloaded
    int fresh;            ///< 上次打开这部以后新下载的话数 / Episodes downloaded since the series was last opened
    uint32_t added;       ///< 订阅时间（秒）/ Subscription time (seconds)
} tomato_wt_mark_t;

/// 读写一个系列文件夹的订阅标记；没有标记返回 false。/ Read or write a series folder's marker; false without one.
bool tomato_wt_mark_read(const char* folder, tomato_wt_mark_t* out);
esp_err_t tomato_wt_mark_write(const char* folder, const tomato_wt_mark_t* m);
/// 找订阅了 id 的系列文件夹。/ Find the series folder subscribed to id.
bool tomato_wt_find(const char* id, char* folder, size_t cap);
/// 订阅：建 comics/<作品名>/（重名的本地文件夹另起名）并写标记，last 是“已下载到第几话”的起点。folder 返回文件夹。
/// Subscribe: create comics/<title>/ (renaming around a same-named local folder) and write the marker; last is the
/// starting "downloaded up to" episode. folder gets the folder path.
esp_err_t tomato_wt_subscribe(const char* id, const char* title, const char* author, int last, char* folder,
                              size_t cap);
/// 第 no 话的文件名：“第0012话 标题.cbz”（标题就是“第12话”时省掉）。/ Episode file name: "第0012话 title.cbz" (the
/// title is dropped when it is just "第12话").
void tomato_wt_episode_name(int no, const char* title, char* out, size_t cap);
/// 系列文件夹里已经有第 no 话。/ Whether the series folder already has episode no.
bool tomato_wt_has_episode(const char* folder, int no);
/// 作品详情缓存和封面的路径。/ Paths of a title's detail cache and cover.
void tomato_wt_title_path(const char* id, char* out, size_t cap);
void tomato_wt_cover_path(const char* id, char* out, size_t cap);

/// 书城后台任务种类。/ Store background jobs.
typedef enum {
    TOMATO_WT_STORE,    ///< 发现页列表和封面；列表写好时进度到 1 / A store list and covers; progress 1 once the list is written
    TOMATO_WT_SEARCH,   ///< 搜索和封面 / Search and covers
    TOMATO_WT_TITLE,    ///< 作品详情和封面 / Title details and cover
    TOMATO_WT_UPDATE,   ///< 检查更新并下载新话（id 为空时检查全部订阅）；进度按话 / Check and download new episodes (all subscriptions when id is empty); progress counts episodes
    TOMATO_WT_TEST,     ///< 测试连接 / Connection test
} tomato_wt_job_t;

typedef struct {
    tomato_wt_job_t job;
    char seg[8];          ///< 书城：rank / genre / day / done；插件来源是 pop / latest / Store: rank / genre / day / done; pop / latest for extension sources
    char src[24];         ///< 书城、搜索：来源 key，空为 WEBTOON / Store and search: source key, empty for WEBTOON
    char key[24];         ///< 书城：榜单、类型或星期，空为默认 / Store: ranking, genre or weekday; empty for the default
    char q[64];           ///< 搜索词 / Search query
    char id[16];          ///< 详情、更新：作品编号 / Detail and update: title id
    bool fill;            ///< 更新：补全缺的话（包括更早的），而不是只下比 last 新的 / Update: fill every missing episode (earlier ones too), not only those after last
    bool thumbs_only;     ///< 书城、搜索：不重新取列表，只补卡上这份列表的封面 / Store and search: no refetch, only fill covers of the list on the card
    uint16_t thumbs_from; ///< 书城、搜索：从第几部（从 0 起）开始下封面 / Store and search: first title (from 0) to fetch covers for
    uint16_t thumbs_count;///< 书城、搜索：下几部的封面，0 是全部 / Store and search: how many titles get covers; 0 means all
} tomato_wt_req_t;

esp_err_t tomato_sync_start_wt(const tomato_wt_req_t* req);
/// 后台正在检查或下载 WEBTOON 更新（不管是哪个页面发起的）。/ Whether the worker is checking or downloading WEBTOON
/// updates (whichever page started it).
bool tomato_wt_updating(void);

/* ---- 在线升级 / Online upgrade ---- */

/// 下载的升级包放在这里。/ Downloaded upgrade packages go here.
#define TOMATO_OTA_DIR "/sdcard/update"
/// 在线升级包文件名的结尾。中文版是 "-upgrade.bin"；英文版由 tools/i18n/en.py 换成 "-upgrade-en.bin"，两种语言各升
/// 各的（旧固件只认 "-upgrade.bin" 结尾，不会误下英文版）。/ The online package name's ending: "-upgrade.bin" for the
/// Chinese firmware; tools/i18n/en.py makes it "-upgrade-en.bin" for the English one, so each language updates to
/// itself (older firmware only accepts the "-upgrade.bin" ending and never picks the English package).
#define TOMATO_OTA_SUFFIX "-upgrade.bin"

/// GitHub 上 treader-latest 发布里的升级包。/ The upgrade package in the treader-latest GitHub release.
typedef struct {
    char version[24];      ///< 文件名里的版本号 / Version from the file name
    char name[64];         ///< treader-<版本>-upgrade.bin
    char url[200];         ///< 下载地址（会跳转）/ Download URL (redirects)
    uint32_t size;         ///< 字节数 / Bytes
    uint8_t sha256[32];
    bool has_sha256;       ///< GitHub 给了摘要 / GitHub supplied a digest
} tomato_ota_info_t;

/// 联网读 treader-latest 的升级包信息；结束后用 tomato_sync_ota_info 取结果。
/// Fetch the treader-latest package info; read it with tomato_sync_ota_info when the job ends.
esp_err_t tomato_sync_start_ota_check(void);
/// 最近一次成功检查的结果；还没有返回 false。/ The last successful check; false if none yet.
bool tomato_sync_ota_info(tomato_ota_info_t* out);
/// 把升级包下载到 TOMATO_OTA_DIR/<name>：先写 <name>.part，已有就断点续传；大小和 SHA-256 都对上才改名成正式文件，
/// 校验不对就删掉。进度在状态的 current / total（字节）。
/// Download the package to TOMATO_OTA_DIR/<name>: write <name>.part, resuming one that exists; only a matching size and
/// SHA-256 rename it into place, and a mismatch deletes it. Progress is the status current / total (bytes).
esp_err_t tomato_sync_start_ota_download(const tomato_ota_info_t* info);
/// 推文目录、第 n 张图（n 为 0 是缩略图）、回复文件、第 k 张方形小图。/ Post folder, image n (0 = thumbnail),
/// replies file, square image k.
void tomato_x_post_dir(const char* id, char* out, size_t cap);
void tomato_x_img_path(const char* id, int n, char* out, size_t cap);
void tomato_x_comments_path(const char* id, char* out, size_t cap);
void tomato_x_square_path(const char* id, int k, char* out, size_t cap);
/// 服务器上次说的“正在关注”开关；没问过算开。/ The server's last known Following switch; on when never asked.
bool tomato_x_following_on(void);
/// 正在关注列表里标记看过；调用方保证此时没有该列表任务在写。/ Mark a Following post as seen; the caller makes sure
/// no Following-list job is writing at the time.
void tomato_x_mark_seen(const char* id);

/// WebDAV 同步阅读进度和番茄书架。/ Sync reading progress and the Tomato shelf over WebDAV.
esp_err_t tomato_sync_start_webdav(void);
/// 注册阅读进度的读写回调（进度存储在主程序里）。/ Register progress I/O callbacks (progress lives in the main app).
void tomato_sync_set_progress_io(tomato_progress_export_t export_cb, tomato_progress_import_t import_cb);
/// 下载替换 TXT 后回调（在同步任务里调用），用来迁移阅读进度。old_size 为 0 表示新书。
/// Called on the sync task after a TXT is replaced, to migrate reading progress. old_size 0 means a new book.
typedef void (*tomato_file_hook_t)(const char* path, uint32_t old_size, uint32_t new_size);
void tomato_sync_set_file_hook(tomato_file_hook_t hook);
/// 联网时要不要顺便用 SNTP 对时（主程序按“自动校时”设置调用，默认要）。/ Whether going online also syncs time over SNTP
/// (the app sets this from its "auto time" switch; on by default).
void tomato_net_set_sntp(bool on);
/// 后台任务做完后还留着的 Wi-Fi 立即断开；有任务正在跑时不动。传书页开热点或连网前调用。
/// Drop Wi-Fi still lingering after a background job right away; leaves a running job alone. Called before the
/// transfer page starts its own Wi-Fi.
void tomato_net_idle_off(void);
/// 系统时间是否已经对过（联网时用 SNTP 对时）。/ Whether system time has been set (SNTP while online).
bool tomato_time_valid(void);
/// 上次取走之后是否又用 SNTP 对过时（取走即清零）。主程序据此把时间写进 PMU 的 RTC。
/// Whether SNTP set the time since the last call (cleared on read); the app then writes it to the PMU RTC.
bool tomato_take_time_synced(void);
/// 最近一次 SNTP 对时把系统时间往前（正）或往后（负）挪了多少毫秒；对时前时间无效时为 INT32_MIN。
/// How many milliseconds the last SNTP sync moved the clock forward (positive) or back; INT32_MIN when the time was
/// invalid before.
int32_t tomato_last_time_fix_ms(void);
/// 只联网对一次时（不管“自动校时”开没开），结果看 tomato_sync_get_status。验证器的“校时”用。
/// Go online just to sync the time once (regardless of the auto-time switch); see tomato_sync_get_status. Used by the
/// authenticator's "sync time".
esp_err_t tomato_sync_start_time(void);
/// 用当前的 Wi-Fi（临时网络优先）试连一次，不换别的保存网络，连上就断开；会阻塞到连上或超时。后台在跑任务或传书
/// 占着 Wi-Fi 时直接失败。msg 写结果（连上了，或者连不上的原因）。设置里 Wi-Fi 页的“测试连接”用。
/// Try the current Wi-Fi (temporary first) once without switching to other saved networks, then disconnect; blocks
/// until connected or timed out. Fails at once while a background job runs or transfer holds Wi-Fi. msg gets the
/// outcome (connected, or why not). Used by "test connection" on the Settings Wi-Fi page.
esp_err_t tomato_net_test(int timeout_ms, char* msg, size_t cap);
/// 试连失败的原因。/ Why a connection attempt failed.
typedef enum {
    TOMATO_NET_FAIL_NONE = 0,  ///< 连上了 / Connected
    TOMATO_NET_FAIL_AUTH,      ///< 密码不对 / Wrong password
    TOMATO_NET_FAIL_NO_AP,     ///< 找不到网络 / Network not found
    TOMATO_NET_FAIL_OTHER,     ///< 其他（超时、拿不到 IP 等）/ Anything else (timeout, no IP, ...)
} tomato_net_fail_t;
/// 用给定的网络试连一次，连上就断开，不读也不写保存的网络；会阻塞到连上或超时。后台在跑任务或传书占着 Wi-Fi 时直接
/// 失败。设置里 Wi-Fi 页“先连后存”用。
/// Try the given network once and disconnect, reading and writing no saved network; blocks until connected or timed
/// out. Fails at once while a background job runs or transfer holds Wi-Fi. Used by the Settings Wi-Fi page to connect
/// before saving.
esp_err_t tomato_net_try(const char* ssid, const char* pass, int timeout_ms, tomato_net_fail_t* why);
/// 一个候选网络。/ One candidate network.
typedef struct {
    char ssid[33];
    char password[65];
} tomato_wifi_cred_t;
/// 多网络回调，由主程序注册（保存的网络列表在主程序里）。都在联网的后台任务里调用，可以阻塞几秒。
/// Multi-network hooks registered by the app, which owns the saved list. Both run on the networking task and may block
/// for a few seconds.
typedef struct {
    /// 当前网络连不上时调用：扫描后给出最多 max 个保存过、在范围内的别的网络，先试的在前，返回个数。failed 是没连上的
    /// 网络名（没有当前网络时为空串）。
    /// Called when the current network fails: scan and return up to max other saved networks in range, first to try
    /// first; returns the count. failed names the network that failed (empty with no current network).
    int (*candidates)(const char* failed, tomato_wifi_cred_t* out, int max);
    /// 连上一个保存的网络后调用；fallback 为真表示是候选网络（应记成当前网络）。临时网络不调。
    /// Called after joining a saved network; fallback means a candidate (to be made the current one). Not called for
    /// the temporary network.
    void (*joined)(const char* ssid, const char* password, bool fallback);
} tomato_wifi_hooks_t;
/// 注册多网络回调；hooks 须一直有效。/ Register the multi-network hooks; hooks must stay valid.
void tomato_net_set_wifi_hooks(const tomato_wifi_hooks_t* hooks);
/// 判断一个路径是不是阅读器正开着的书；是的话更新会跳过它（阅读器还持有旧文件）。
/// Whether a path is the book open in the reader; updates skip it because the reader still holds the old file.
typedef bool (*tomato_open_check_t)(const char* path);
void tomato_sync_set_open_check(tomato_open_check_t check);
/// 书城或书库里的番茄书下载、番茄书更新下载、漫画更新下载正在跑（还没被取消）。锁屏时据此留着继续下，不睡也不关机。
/// A bookstore download, a Tomato update download or a comic update download is running and not cancelled. The lock uses
/// it to keep downloading instead of sleeping or powering off.
bool tomato_sync_downloading(void);
/// 下载有没有往前走的戳：进度、步骤、已收字节变了它就变，一直不变就是卡住了。/ A stamp of download progress: it
/// changes whenever progress, step or bytes received change, so one that stays put means a stall.
uint32_t tomato_sync_stamp(void);
/// 取消并等待后台任务结束，最多 timeout_ms，停了再断开任务后还留着的 Wi-Fi；锁屏前调用。返回是否已停。
/// Cancel and wait up to timeout_ms for the task to end, then drop any Wi-Fi lingering after it; call before a lock.
/// Returns whether it stopped.
bool tomato_sync_stop(int timeout_ms);
/// 请求取消；任务在下一个安全点退出。/ Request cancel; the task exits at its next safe point.
void tomato_sync_cancel(void);
/// 前台优先：后台在跑的是浏览类任务（虎扑、小红书、书城、搜索）时为真，前台可以取消它。下载、检查更新、WebDAV 同步为假。
/// Foreground first: true while a browsing job (Hupu, Xiaohongshu, bookstore, search) runs, which the foreground may
/// cancel. False for downloads, update checks and WebDAV sync.
bool tomato_sync_preemptible(void);
typedef enum {
    TOMATO_PREEMPT_FREE,       ///< 后台空了，可以马上开始 / The worker is free now
    TOMATO_PREEMPT_STOPPING,   ///< 浏览任务已取消、还没停下，稍后再开始 / A browsing job was cancelled and is stopping
    TOMATO_PREEMPT_BLOCKED,    ///< 在下载或同步，不打断 / A download or sync runs and is not interrupted
} tomato_preempt_t;
/// 给前台腾出后台：浏览类任务就取消（最多等 0.3 秒），下载和同步不打断。不会长时间阻塞界面。
/// Make room for the foreground: cancel a browsing job (waiting at most 0.3 s); downloads and sync are left alone.
/// Never blocks the UI for long.
tomato_preempt_t tomato_sync_preempt(void);
/// 外部组件的后台任务（微信读书）：联网、校时之后在后台任务里调 run，返回值和 msg 作为结果。cancel 变真时尽快返回。
/// A background job from another component (WeRead): run is called on the task after the network is up and the time
/// is set; its return and msg become the result. Return soon after cancel turns true.
typedef esp_err_t (*tomato_ext_run_t)(void* ctx, const volatile bool* cancel, void (*progress)(int current, int total),
                                      char* msg, size_t cap);
typedef struct {
    tomato_ext_run_t run;
    void* ctx;
    bool download;      ///< 下载类：锁屏不睡、前台不打断；否则算浏览类 / Download: holds the lock and is not preempted;
                        ///< otherwise it counts as browsing
    const char* step;   ///< 开始时的提示 / Message shown at the start
} tomato_ext_job_t;
esp_err_t tomato_sync_start_ext(const tomato_ext_job_t* job);
/// 后台任务是否在跑。/ Whether the task is running.
bool tomato_sync_busy(void);
void tomato_sync_get_status(tomato_status_t* out);
/// 复制最近一次搜索结果，返回条数。/ Copy the latest search results; returns the count.
int tomato_sync_hits(tomato_hit_t* out, int max);

#ifdef __cplusplus
}
#endif
