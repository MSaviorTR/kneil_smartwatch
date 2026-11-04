#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Thin C interface over the AXP2101 power-management IC.
 *
 * XPowersLib is C++, so the implementation lives in power_service.cpp
 * and only this plain-C surface is exposed to the UI code.
 */

typedef struct
{
    bool     present;       /* battery detected by the PMU        */
    int      percent;       /* 0..100, or -1 when unknown         */
    uint16_t voltage_mv;    /* battery terminal voltage in mV     */
    bool     charging;      /* actively charging                  */
    bool     usb_connected; /* VBUS present                       */
} power_status_t;

/* Safe to call once, after the BSP is up. Returns ESP_FAIL if no PMU answers. */
esp_err_t power_service_init(void);

/* Fills *out with a fresh reading. Returns ESP_ERR_INVALID_STATE if init failed. */
esp_err_t power_service_read(power_status_t *out);

#ifdef __cplusplus
}
#endif
