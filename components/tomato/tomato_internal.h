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
#include "tomato.h"

#define TOMATO_REMOVED_MAX 64

/// 用系统保存的 Wi-Fi 连网，等到拿到 IP 或超时。/ Join the saved Wi-Fi and wait for an IP or time out.
esp_err_t tomato_net_up(int timeout_ms, const volatile bool* cancel);
/// 断网并释放全部 Wi-Fi 资源；可重复调用。/ Disconnect and release all Wi-Fi resources; repeatable.
void tomato_net_down(void);

/// 百分号编码。keep_slash 用于 /download/ 路径。/ Percent-encode; keep_slash for /download/ paths.
void tomato_url_escape(const char* in, char* out, size_t cap, bool keep_slash);
/// 按 UTF-8 字符边界截断复制。/ Copy with truncation on a UTF-8 character boundary.
void tomato_copy_utf8(char* out, size_t cap, const char* in);
/// 让 cJSON 用 PSRAM（只需调用一次）。/ Route cJSON allocations to PSRAM (call once).
void tomato_json_use_psram(void);

/// 发请求并把 JSON 响应解析到 *out（调用方 cJSON_Delete）。非 2xx 返回错误，HTTP 状态写入 *status。
/// Send a request and parse the JSON reply into *out (caller deletes). Non-2xx is an error; status goes to *status.
esp_err_t tomato_http_json(const tomato_config_t* cfg, const char* method, const char* path,
                           const char* body, cJSON** out, int* status);

/// 下载到 dest（先写 .part 再改名）。cancel 置位时中止。
/// Download to dest via a .part file and rename. Aborts when *cancel is set.
esp_err_t tomato_http_download(const tomato_config_t* cfg, const char* path, const char* dest,
                               const volatile bool* cancel, volatile uint32_t* bytes);

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
