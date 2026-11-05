#pragma once
/* Host stand-in for FreeRTOS (single-threaded unit tests). */
#include <stdint.h>
typedef void *SemaphoreHandle_t;
typedef uint32_t TickType_t;
#define portMAX_DELAY 0xFFFFFFFFu
#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
