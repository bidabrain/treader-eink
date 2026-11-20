// 主机测试桩：单线程，锁只计数。/ Host test stub: single thread, locks only count.
#pragma once
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
#define portMAX_DELAY 0
