#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * BLE peripheral (NimBLE). Works with any generic BLE app such as
 * nRF Connect - no custom phone app needed.
 *
 *  Battery Service 0x180F / Battery Level 0x2A19       read, notify
 *
 *  Guder Watch service  6e7a0001-3c1f-4b8e-9d2a-5f4c1b2a7e90
 *    Live reading       6e7a0002-...  read, notify   UTF-8 CSV line:
 *        epoch_ms,source,k_mV,k_mM,ph_mV,pH,temp_C,flags,k_alert
 *    Time (UTC)         6e7a0003-...  read, write    uint32 LE unix seconds
 *
 * There is no pairing: anyone in range can read data and set the clock.
 * Fine for a bench prototype; add LE Secure Connections before any
 * study involving real subjects.
 */

esp_err_t ble_service_start(void);

/* Advertising on/off. Turning it off also drops an active connection. */
void ble_service_set_enabled(bool enabled);

bool ble_service_connected(void);
bool ble_service_advertising(void);

/* Pushes a new battery percentage to subscribed clients. */
void ble_service_update_battery(int percent);

#ifdef __cplusplus
}
#endif
