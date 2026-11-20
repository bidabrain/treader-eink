#pragma once
#include <stdint.h>
#include "FreeRTOS.h"
typedef void* TaskHandle_t;
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portTICK_PERIOD_MS 1
#define tskNO_AFFINITY (-1)
void vTaskDelay(TickType_t t);
TickType_t xTaskGetTickCount(void);
BaseType_t xTaskCreate(void (*f)(void*), const char* n, uint32_t s, void* a, UBaseType_t p, TaskHandle_t* h);
BaseType_t xTaskCreatePinnedToCore(void (*f)(void*), const char* n, uint32_t s, void* a, UBaseType_t p, TaskHandle_t* h, int core);
BaseType_t xTaskCreatePinnedToCoreWithCaps(void (*f)(void*), const char* n, uint32_t s, void* a, UBaseType_t p, TaskHandle_t* h, int core, uint32_t caps);
void vTaskDelete(TaskHandle_t h);
void vTaskDeleteWithCaps(TaskHandle_t h);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait);
void xTaskNotifyGive(TaskHandle_t h);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
