#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Vibration motor on GPIO18 (via Q1). All calls respect the
   Vibration switch in Settings and never block. */

esp_err_t haptics_init(void);

/* One short tap, e.g. to confirm a capture. */
void haptics_tap(void);

/* Three long pulses for a K+ alert. */
void haptics_alert(void);

#ifdef __cplusplus
}
#endif
