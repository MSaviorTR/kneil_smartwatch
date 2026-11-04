#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Polls the on-board IMU (10 Hz) and power IC (every 5 s) and publishes
   the results to app_state, so the UI never does I2C itself. */
esp_err_t housekeeping_start(void);

#ifdef __cplusplus
}
#endif
