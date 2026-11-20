#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_app_desc.h"
typedef uint32_t esp_ota_handle_t;
typedef struct { int type, subtype; uint32_t address, size; char label[17]; } esp_partition_t;
const esp_partition_t* esp_ota_get_running_partition(void);
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t* p);
#define OTA_WITH_SEQUENTIAL_WRITES 0xFFFFFFFE
#define OTA_SIZE_UNKNOWN 0xFFFFFFFF
#define ESP_ERR_OTA_VALIDATE_FAILED 0x1503
esp_err_t esp_ota_begin(const esp_partition_t* p, size_t n, esp_ota_handle_t* h);
esp_err_t esp_ota_write(esp_ota_handle_t h, const void* d, size_t n);
esp_err_t esp_ota_end(esp_ota_handle_t h);
esp_err_t esp_ota_abort(esp_ota_handle_t h);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t* p);
