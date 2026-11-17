/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 组件内部接口：联网、HTTP、JSON 小工具、书架内部操作。只在 components/tomato 内使用。
 * Component-private helpers: network, HTTP, JSON and internal shelf operations. Used only inside components/tomato.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "tomato.h"

#define TOMATO_REMOVED_MAX 64

/// 用系统保存的 Wi-Fi 连网，等到拿到 IP 或超时。/ Join the saved Wi-Fi and wait for an IP or time out.
esp_err_t tomato_net_up(int timeout_ms, const volatile bool* cancel);
/// 断网并释放全部 Wi-Fi 资源；可重复调用。/ Disconnect and release all Wi-Fi resources; repeatable.
void tomato_net_down(void);
/// 改用公共 DNS；已经换过或没联网返回 false。/ Switch to public DNS; false if already switched or offline.
bool tomato_net_use_public_dns(void);
/// 打开 HTTP 连接；域名解析失败时换公共 DNS 再试一次，失败原因留给提示用。
/// Open an HTTP connection, retrying once with public DNS on a lookup failure; the cause is kept for messages.
esp_err_t tomato_http_open(esp_http_client_handle_t c, int len);

/// 百分号编码。keep_slash 用于 /download/ 路径。/ Percent-encode; keep_slash for /download/ paths.
void tomato_url_escape(const char* in, char* out, size_t cap, bool keep_slash);
/// 按 UTF-8 字符边界截断复制。/ Copy with truncation on a UTF-8 character boundary.
void tomato_copy_utf8(char* out, size_t cap, const char* in);
/// 让 cJSON 用 PSRAM（只需调用一次）。/ Route cJSON allocations to PSRAM (call once).
void tomato_json_use_psram(void);
/// 联网失败原因：同步开始时清空，失败时接在提示后面。/ Network failure cause: cleared per sync, appended on failure.
void tomato_http_clear_detail(void);
const char* tomato_http_detail(void);
void tomato_http_set_detail(const char* text);

/// 发请求并把 JSON 响应解析到 *out（调用方 cJSON_Delete）。非 2xx 返回错误，HTTP 状态写入 *status。
/// Send a request and parse the JSON reply into *out (caller deletes). Non-2xx is an error; status goes to *status.
esp_err_t tomato_http_json(const tomato_config_t* cfg, const char* method, const char* path,
                           const char* body, cJSON** out, int* status);

/// 下载到 dest（先写 .part 再改名）。cancel 置位时中止。
/// Download to dest via a .part file and rename. Aborts when *cancel is set.
esp_err_t tomato_http_download(const tomato_config_t* cfg, const char* path, const char* dest,
                               const volatile bool* cancel, volatile uint32_t* bytes);

/// GET 一个完整 URL 存到 dest（不超过 max 字节）。image 为真时只收 JPEG/PNG，否则不写文件。
/// GET a full URL into dest (at most max bytes). With image set, only JPEG/PNG content is written.
esp_err_t tomato_http_fetch(const char* url, const char* dest, size_t max, bool image, const volatile bool* cancel);
/// GET 到 PSRAM 缓冲（调用方 heap_caps_free），末尾补 0；image 时只收 JPEG/PNG。
/// GET into a PSRAM buffer (caller frees) with a trailing NUL; image accepts JPEG/PNG only.
esp_err_t tomato_http_get(const char* url, size_t max, bool image, uint8_t** out, size_t* out_len,
                         const volatile bool* cancel);

/* ---- 书城 / Bookstore ---- */

/// 榜单站点根地址（去掉末尾斜杠和 /api/...）。/ Rank site root (no trailing slash or /api/...).
void tomato_rank_base(char* out, size_t cap);
/// 榜单地址候选数（github.io 站点有 jsDelivr 镜像）、当前用哪个、切换。/ Rank site candidates (github.io gets jsDelivr
/// mirrors), the current one, and switching.
int tomato_rank_mirror_count(void);
int tomato_rank_mirror(void);
void tomato_rank_use_mirror(int index);
bool tomato_rank_index_url(char* out, size_t cap);
bool tomato_rank_cat_url(const tomato_rank_cat_t* cat, char* out, size_t cap);
const char* tomato_rank_index_path(void);
void tomato_rank_cat_path(const tomato_rank_cat_t* cat, char* out, size_t cap);
/// 读缓存里的榜单日期。/ Read the cached rank date.
bool tomato_rank_index_date(char* out, size_t cap);
/// 删掉所有分类缓存（榜单换日期时）。/ Delete all category caches (when the rank date changes).
void tomato_rank_drop_categories(void);
typedef void (*tomato_hupu_progress_t)(int current, int total);
/// 虎扑推荐和帖子，在同步任务里调用；msg 写给用户看的一句话。/ Hupu feed and post, run on the sync task.
esp_err_t tomato_hupu_home(const volatile bool* cancel, tomato_hupu_progress_t progress, char* msg, size_t cap);
/// images_only：不重取正文，只补缓存帖子里缺的图。/ images_only: skip the page and fetch missing images of the cached post.
esp_err_t tomato_hupu_post(const char* tid, bool images_only, const volatile bool* cancel, tomato_hupu_progress_t progress,
                           char* msg, size_t cap);
esp_err_t tomato_hupu_zones(const volatile bool* cancel, char* msg, size_t cap);
esp_err_t tomato_hupu_threads(const char* zone, int sort, bool more, const volatile bool* cancel, char* msg, size_t cap);
esp_err_t tomato_hupu_follow(const volatile bool* cancel, tomato_hupu_progress_t progress, char* msg, size_t cap);
esp_err_t tomato_hupu_schedule(const char* league, const volatile bool* cancel, char* msg, size_t cap);
esp_err_t tomato_hupu_rating(const char* type, const char* no, const volatile bool* cancel, char* msg, size_t cap);
/// 封面目录太大时，只留 keep 里这些书的封面和缩略图。/ When the cover dir grows too big, keep only these books' covers and thumbnails.
void tomato_rank_prune_covers(const tomato_rank_book_t* keep, int n);

/// GET 一个完整 URL，边收边数 key_a / key_b 出现次数，返回较大者。/ GET a full URL and count key occurrences while streaming.
esp_err_t tomato_http_count_keys(const char* url, const char* key_a, const char* key_b,
                                 const volatile bool* cancel, int* count);

/// 运行一次 WebDAV 同步，结果写进 msg。/ Run one WebDAV sync; the outcome goes to msg.
esp_err_t tomato_webdav_run(tomato_progress_export_t export_cb, tomato_progress_import_t import_cb,
                            char* msg, size_t cap);

/// 数字字段可能是数字也可能是字符串。/ Numeric fields may arrive as numbers or strings.
int tomato_json_int(const cJSON* obj, const char* key);
/// 复制字符串字段，按字符边界截断，缺失时写空串。/ Copy a string field, cut on a character boundary, empty when missing.
void tomato_json_str(const cJSON* obj, const char* key, char* out, size_t cap);

/// 已删除清单：书库删掉的番茄书，WebDAV 合并时不再加回来。/ Removed list: Tomato books deleted from the library; WebDAV never re-adds them.
int tomato_removed_load(char (*ids)[TOMATO_ID_LEN], int max);
esp_err_t tomato_removed_save(const char (*ids)[TOMATO_ID_LEN], int n);
esp_err_t tomato_removed_mark(const char* book_id, bool removed);

/// 立即从书架去掉一个文件。/ Drop a file from the shelf right away.
esp_err_t tomato_shelf_remove_now(const char* file);
/// 同步任务在跑时记下待删文件，任务结束前统一处理；没在跑返回 false。
/// While the sync task runs, queue a file for removal before it ends; returns false when idle.
bool tomato_sync_defer_remove(const char* file);

/// 书名里不能当文件名的字符和路径分隔都换掉；file 来自远端时用它清洗。
/// Make a safe file name: replace FAT-invalid characters and path separators; used to sanitize remote names.
bool tomato_file_name_safe(const char* file);
