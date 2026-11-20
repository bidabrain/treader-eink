// 主机测试桩：直接用 libc。/ Host test stub over libc.
#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#define heap_caps_malloc(n, caps) malloc(n)
#define heap_caps_calloc(n, s, caps) calloc(n, s)
#define heap_caps_free(p) free(p)
