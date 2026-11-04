#pragma once
/* Host stand-in for ESP-IDF's esp_timer.h (UI simulator only). */
#include <stdint.h>
int64_t esp_timer_get_time(void);
