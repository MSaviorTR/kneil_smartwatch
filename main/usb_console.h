#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Command line on the USB port (the same one `idf.py monitor` uses):
 *
 *   status              link, latest reading, time, storage
 *   cal                 stored calibrations
 *   settime <unix_s>    set the clock (UTC seconds), e.g. settime $(date +%s)
 *   ls                  list log files
 *   cat <file>          dump a log file between BEGIN/END markers
 *   rm <file> | rm all  delete one / every log file
 *
 * tools/pull_logs.py uses `ls` and `cat` to copy logs to the laptop.
 */
esp_err_t usb_console_start(void);

#ifdef __cplusplus
}
#endif
