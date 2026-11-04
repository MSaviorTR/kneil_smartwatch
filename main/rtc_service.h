#pragma once

#include <stdbool.h>
#include <time.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    TIME_SRC_UNSET = 0,   /* RTC lost power and nobody has set it yet */
    TIME_SRC_RTC,         /* restored from the PCF85063 at boot */
    TIME_SRC_USER,        /* set on the watch, over BLE, or over USB */
} time_source_t;

/* Reads the PCF85063 and sets the system clock from it. If the RTC lost
   power the clock is seeded from the firmware build time so it is at
   least roughly right, and the source reports TIME_SRC_UNSET. */
esp_err_t rtc_service_init(void);

/* Sets system time and writes it back to the RTC (UTC). */
esp_err_t rtc_service_set_epoch(time_t utc);

time_source_t rtc_service_time_source(void);

/* True once the clock is trustworthy enough to timestamp data. */
bool rtc_service_time_valid(void);

#ifdef __cplusplus
}
#endif
