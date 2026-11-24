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
/// 已联网时跑一次 SNTP（不管自动校时开关），成功返回真。/ Run SNTP once while online (regardless of the auto-time
/// switch); true on success.
bool tomato_net_sync_time(int wait_ms);
/// 改用公共 DNS；已经换过或没联网返回 false。/ Switch to public DNS; false if already switched or offline.
bool tomato_net_use_public_dns(void);
/// 打开 HTTP 连接；域名解析失败时换公共 DNS 再试一次，失败原因留给提示用。
/// Open an HTTP connection, retrying once with public DNS on a lookup failure; the cause is kept for messages.
esp_err_t tomato_http_open(esp_http_client_handle_t c, int len);
/// 取一个连到 url 的 client：和上一个请求同一来源（协议、主机、端口）且连接还开着就复用，省掉一次握手；否则新建。
/// 只在同步任务里用。头只能用 tomato_http_header 设，复用时会清掉上一个请求的头。
/// Take a client for url: reuse the kept one when the previous request had the same origin (scheme, host, port) and
/// its connection is still open, saving a handshake; otherwise create one. Sync task only. Set headers only through
/// tomato_http_header so a reused client drops the previous request's headers.
esp_http_client_handle_t tomato_http_take(const char* url, int timeout_ms, esp_http_client_method_t method,
                                          int tx_size);
/// 给 take 来的 client 设一个请求头。/ Set a request header on a client from take.
void tomato_http_header(esp_http_client_handle_t c, const char* name, const char* value);
/// 发请求并取响应头；复用的连接已被服务器关掉时重连一次。返回响应长度（不知道为 -1），状态码写进 *status。
/// Send the request and fetch the headers, reconnecting once when a reused connection was closed by the server.
/// Returns the content length (-1 if unknown) and writes the status to *status.
esp_err_t tomato_http_begin(esp_http_client_handle_t c, const char* body, int body_len, int64_t* length, int* status);
/// 用完交回：响应完整收完且服务器允许保持连接时留着给下一个请求，否则关掉。/ Give a client back: kept for the next
/// request when the reply was read in full and the server allows keep-alive, otherwise closed.
void tomato_http_give(esp_http_client_handle_t c, bool done);
/// 关掉留着的连接；断网前调用。/ Close the kept connection; call before going offline.
void tomato_http_drop(void);

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

/* ---- 在线升级 / Online upgrade ---- */

/// 读 GitHub 上 treader-latest 的升级包信息。/ Read the treader-latest package info from GitHub.
esp_err_t tomato_ota_check(tomato_ota_info_t* out, const volatile bool* cancel, char* msg, size_t cap);
/// 下载并校验升级包，规则见 tomato_sync_start_ota_download。/ Download and verify; see tomato_sync_start_ota_download.
esp_err_t tomato_ota_download(const tomato_ota_info_t* info, const volatile bool* cancel,
                              void (*progress)(int done, int total), char* msg, size_t cap);

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
/// 小红书（rednote 服务器）任务，在同步任务里调用。/ Xiaohongshu (rednote server) job, run on the sync task.
esp_err_t tomato_red_run(const tomato_red_req_t* r, const volatile bool* cancel, tomato_hupu_progress_t progress,
                         char* msg, size_t cap);
/// 微博（weibo 服务器）任务，在同步任务里调用。/ Weibo (weibo server) job, run on the sync task.
esp_err_t tomato_wb_run(const tomato_wb_req_t* r, const volatile bool* cancel, tomato_hupu_progress_t progress,
                        char* msg, size_t cap);

/// WEBTOON（webtoon 服务器）任务，在同步任务里调用。/ WEBTOON (webtoon server) job, run on the sync task.
esp_err_t tomato_wt_run(const tomato_wt_req_t* r, const volatile bool* cancel, tomato_hupu_progress_t progress,
                        char* msg, size_t cap);

/// X（x 服务器）任务，在同步任务里调用。/ X (x server) job, run on the sync task.
esp_err_t tomato_x_run(const tomato_x_req_t* r, const volatile bool* cancel, tomato_hupu_progress_t progress,
                       char* msg, size_t cap);

/* ---- 自建内容服务器共用 / Shared by self-hosted content servers ---- */

#define TOMATO_SRV_JSON_MAX (1024 * 1024)
#define TOMATO_SRV_IMG_MAX (2 * 1024 * 1024)

/// 一台自建服务器：地址和令牌、令牌请求头名、出错时的称呼。/ One self-hosted server: address and token, the token
/// header name, and its name in error messages.
typedef struct {
    const tomato_srv_config_t* cfg;
    const char* header;
    const char* name;
} tomato_srv_t;

esp_err_t tomato_srv_config_load(const char* ns, tomato_srv_config_t* out);
esp_err_t tomato_srv_config_save(const char* ns, const tomato_srv_config_t* cfg);
int tomato_srv_list_limit(const char* ns);
esp_err_t tomato_srv_list_limit_set(const char* ns, int n);
/// 带令牌 GET 一个接口到 PSRAM（调用方 heap_caps_free，末尾补 0）。非 200 时把服务器给的原因写进 msg。
/// GET an endpoint with the token into PSRAM (caller frees; NUL-terminated). On a non-200 status msg gets the reason.
esp_err_t tomato_srv_get(const tomato_srv_t* srv, const char* path, size_t max, int timeout_ms,
                         const volatile bool* cancel, uint8_t** out, size_t* out_len, char* msg, size_t cap);
esp_err_t tomato_srv_json(const tomato_srv_t* srv, const char* path, int timeout_ms, const volatile bool* cancel,
                          cJSON** out, char* msg, size_t cap);
/// 带令牌 POST 一段 JSON，返回解析好的 JSON。出错时同 tomato_srv_get。/ POST a JSON body with the token and parse the
/// reply. Errors as tomato_srv_get.
esp_err_t tomato_srv_post(const tomato_srv_t* srv, const char* path, const char* body, int timeout_ms,
                          const volatile bool* cancel, cJSON** out, char* msg, size_t cap);
/// 带令牌把一个大文件边收边写到 dest（先写 .part，完整了再改名），最多 max 字节。/ Stream a large file with the token
/// into dest (write .part, rename once complete), at most max bytes.
esp_err_t tomato_srv_download(const tomato_srv_t* srv, const char* path, const char* dest, size_t max, int timeout_ms,
                              const volatile bool* cancel, char* msg, size_t cap);
/// 慢请求的轮询：先 GET first，返回里 key 为 true 就隔 POLL 秒 GET again（again 为 NULL 时重复 first），直到 key 不再为
/// true，或者等满 max_ms。每次请求服务器都马上回话，等待时每 100 ms 看一次取消，所以前台随时能插队。超时返回
/// ESP_ERR_TIMEOUT。
/// Polling for slow server work: GET first; while the reply has key true, GET again (first when again is NULL) every
/// few seconds until key is no longer true or max_ms passes. Each request answers at once and the wait checks for
/// cancellation every 100 ms, so the foreground can always cut in. Returns ESP_ERR_TIMEOUT when time runs out.
esp_err_t tomato_srv_poll(const tomato_srv_t* srv, const char* first, const char* again, const char* key,
                          int timeout_ms, int max_ms, const volatile bool* cancel, cJSON** out, char* msg, size_t cap);
/// 取一张 JPEG 存到 dest；不是 JPEG 不写。/ Fetch one JPEG into dest; anything else is not written.
esp_err_t tomato_srv_image(const tomato_srv_t* srv, const char* path, const char* dest, int timeout_ms,
                           const volatile bool* cancel);
/// 编号只留字母和数字，免得拼进路径时出现 ../。/ Keep only letters and digits from ids used in paths.
void tomato_safe_id(const char* in, char* out, size_t cap);
bool tomato_file_exists(const char* path);
/// 先写 .part 再改名，断电不会留下半个文件。/ Write .part then rename so a power cut never leaves half a file.
esp_err_t tomato_cache_write(const char* dest, const void* data, size_t len);
esp_err_t tomato_cache_write_json(const char* dest, const cJSON* root);
cJSON* tomato_cache_read_json(const char* path);
/// root 下的条目目录（名字就是编号）两道上限：超过 keep_dirs 个时先删不在 lists 任何一个里的旧条目；总大小超过
/// max_bytes 时从最旧的开始只删大图（留 .json 和缩略图）。keep 这一条两样都不动。
/// Two caps on item folders under root (named by id): past keep_dirs, delete the oldest items absent from every list
/// file; past max_bytes in total, strip full images from the oldest (keeping .json and thumbnails). keep is untouched.
void tomato_cache_prune(const char* root, const char* const* lists, int list_count, const char* keep, int keep_dirs,
                        uint64_t max_bytes);
/// 列表文件里把 id 标成看过。/ Mark id as seen in a list file.
void tomato_cache_mark_seen(const char* list_file, const char* id);
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
