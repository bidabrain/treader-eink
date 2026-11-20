#pragma once
#include <stdint.h>
typedef void* SemaphoreHandle_t;
typedef struct { int x; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
#define portMAX_DELAY 0xFFFFFFFFu
#define xSemaphoreCreateRecursiveMutex() ((void*)1)
#define xSemaphoreTakeRecursive(s, w) 1
#define xSemaphoreGiveRecursive(s) 1
#define xSemaphoreCreateMutex() ((void*)1)
#define xSemaphoreCreateBinary() ((void*)1)
#define xSemaphoreTake(s, w) 1
#define xSemaphoreGive(s) 1
#define vSemaphoreDelete(s) ((void)(s))
