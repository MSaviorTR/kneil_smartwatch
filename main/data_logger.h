#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "calibration.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * CSV logger. Prefers the microSD card (every sample); falls back to the
 * internal 7 MB SPIFFS partition (averaged rows, see GW_LOG_FLASH_PERIOD_S).
 *
 *   <root>/gw_YYYYMMDD.csv   one file per local day
 *   <root>/gw_nodate.csv     rows logged before the clock was set
 *   <root>/gw_cal.csv        every calibration that was saved
 */

typedef enum
{
    LOG_STORE_NONE = 0,     /* still mounting, or nothing mounted */
    LOG_STORE_SD,
    LOG_STORE_FLASH,
} log_store_t;

typedef struct
{
    log_store_t store;
    char        file[40];       /* current data file name, "" if none */
    uint32_t    rows;           /* rows written since boot */
    uint32_t    dropped;        /* readings lost because the queue was full */
    uint64_t    total_bytes;
    uint64_t    free_bytes;
    bool        full;           /* stopped: below GW_LOG_MIN_FREE_KB */
    bool        error;          /* last write failed */
} logger_status_t;

/* Mounts storage in the background and starts logging every reading. */
esp_err_t data_logger_start(void);

void data_logger_status(logger_status_t *out);

/* Mount point ("/sdcard" or "/spiffs"), or NULL while nothing is mounted. */
const char *data_logger_root(void);

/* Appends a calibration record to gw_cal.csv (asynchronous). */
void data_logger_log_calibration(cal_channel_t ch, const cal_fit_t *fit,
                                 const cal_point_t *pts, int n);

/* Deletes every gw_*.csv file (asynchronous). */
void data_logger_clear(void);

#ifdef __cplusplus
}
#endif
