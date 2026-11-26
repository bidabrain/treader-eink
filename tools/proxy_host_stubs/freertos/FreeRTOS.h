// 主机测试桩：临界区在测试里不需要，当空操作。/ Host-test stub: critical sections are no-ops in the test.
#pragma once
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
