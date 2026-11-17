#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bluetooth LE (NimBLE), two roles that can run at the same time:
 *
 *  PERIPHERAL (data out): the watch advertises as GuderWatch and a phone
 *  connects to it. Works with any generic BLE app such as nRF Connect.
 *
 *  CENTRAL (data in): the watch scans for a wireless Guder sensor, connects
 *  and subscribes to its $GKS frames, which enter the same pipeline as the
 *  wired link. See ble_central.h for what the sensor must offer.
 *
 * Settings -> Bluetooth mode picks Phone / Sensor / Both.
 *
 * Peripheral services:
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

/* Master switch for Bluetooth. Off stops advertising and scanning and drops
   any connections. */
void ble_service_set_enabled(bool enabled);

/* BLE_MODE_PHONE / BLE_MODE_SENSOR / BLE_MODE_BOTH (app_state.h). */
void ble_service_set_mode(uint8_t mode);

typedef struct
{
    bool     enabled;
    uint8_t  mode;
    bool     phone_connected;
    bool     advertising;
    bool     sensor_scanning;
    bool     sensor_connected;
    bool     sensor_receiving;
    uint32_t sensor_frames;
    char     sensor_name[24];
} ble_status_t;

void ble_service_status(ble_status_t *out);

bool ble_service_connected(void);
bool ble_service_advertising(void);

/* Pushes a new battery percentage to subscribed clients. */
void ble_service_update_battery(int percent);

#ifdef __cplusplus
}
#endif
