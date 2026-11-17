/*
 * SPDX-FileCopyrightText: 2026 Treader contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * 组件内部接口：联网、HTTP、JSON 小工具。只在 components/tomato 内使用。
 * Component-private helpers: network, HTTP and JSON. Used only inside components/tomato.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "tomato.h"

/// 用系统保存的 Wi-Fi 连网，等到拿到 IP 或超时。/ Join the saved Wi-Fi and wait for an IP or time out.
esp_err_t tomato_net_up(int timeout_ms);
/// 断网并释放全部 Wi-Fi 资源；可重复调用。/ Disconnect and release all Wi-Fi resources; repeatable.
void tomato_net_down(void);

/// 百分号编码。keep_slash 用于 /download/ 路径。/ Percent-encode; keep_slash for /download/ paths.
void tomato_url_escape(const char* in, char* out, size_t cap, bool keep_slash);

/// 发请求并把 JSON 响应解析到 *out（调用方 cJSON_Delete）。HTTP 状态写入 *status。
/// Send a request and parse the JSON reply into *out (caller deletes). HTTP status goes to *status.
esp_err_t tomato_http_json(const tomato_config_t* cfg, const char* method, const char* path,
                           const char* body, cJSON** out, int* status);

/// 下载到 dest（先写 .part 再改名）。cancel 置位时中止。
/// Download to dest via a .part file and rename. Aborts when *cancel is set.
esp_err_t tomato_http_download(const tomato_config_t* cfg, const char* path, const char* dest,
                               const volatile bool* cancel, volatile uint32_t* bytes);

/// 运行一次 WebDAV 同步，结果写进 msg。/ Run one WebDAV sync; the outcome goes to msg.
esp_err_t tomato_webdav_run(tomato_progress_export_t export_cb, tomato_progress_import_t import_cb,
                            char* msg, size_t cap);

/// GET 一个完整 URL，边收边数 key_a / key_b 出现次数，返回较大者。/ GET a full URL and count key occurrences while streaming.
esp_err_t tomato_http_count_keys(const char* url, const char* key_a, const char* key_b, int* count);

/// 数字字段可能是数字也可能是字符串。/ Numeric fields may arrive as numbers or strings.
int tomato_json_int(const cJSON* obj, const char* key);
/// 复制字符串字段，缺失时写空串。/ Copy a string field, empty when missing.
void tomato_json_str(const cJSON* obj, const char* key, char* out, size_t cap);
