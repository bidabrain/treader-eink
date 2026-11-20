// 主机测试桩：递归锁记深度，测试结束时应为 0。/ Host test stub: the recursive lock tracks depth, 0 at the end.
#pragma once
typedef struct { int depth; } StaticSemaphore_t;
typedef StaticSemaphore_t* SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t* b) { b->depth = 0; return b; }
static inline int xSemaphoreTakeRecursive(SemaphoreHandle_t s, int wait) { (void)wait; s->depth++; return 1; }
static inline int xSemaphoreGiveRecursive(SemaphoreHandle_t s) { s->depth--; return 1; }
