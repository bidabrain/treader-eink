// 主机测试桩：读一律“没有”，写一律成功（设置只存在于模块自己的缓存里）。
// Host-test stub: reads always miss and writes always succeed (settings live only in the module's cache).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef int nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
static inline esp_err_t nvs_open(const char* ns, nvs_open_mode_t m, nvs_handle_t* h) { (void)ns; (void)h; return m == NVS_READWRITE ? ESP_OK : ESP_FAIL; }
static inline esp_err_t nvs_get_u8(nvs_handle_t h, const char* k, uint8_t* v) { (void)h; (void)k; (void)v; return ESP_FAIL; }
static inline esp_err_t nvs_get_u16(nvs_handle_t h, const char* k, uint16_t* v) { (void)h; (void)k; (void)v; return ESP_FAIL; }
static inline esp_err_t nvs_get_str(nvs_handle_t h, const char* k, char* v, size_t* n) { (void)h; (void)k; (void)v; (void)n; return ESP_FAIL; }
static inline esp_err_t nvs_set_u8(nvs_handle_t h, const char* k, uint8_t v) { (void)h; (void)k; (void)v; return ESP_OK; }
static inline esp_err_t nvs_set_u16(nvs_handle_t h, const char* k, uint16_t v) { (void)h; (void)k; (void)v; return ESP_OK; }
static inline esp_err_t nvs_set_str(nvs_handle_t h, const char* k, const char* v) { (void)h; (void)k; (void)v; return ESP_OK; }
static inline esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
static inline void nvs_close(nvs_handle_t h) { (void)h; }
