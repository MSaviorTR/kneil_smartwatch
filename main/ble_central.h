#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * BLE central role: the watch connects TO a wireless sensor and receives
 * its measurements. Internal to the BLE module; the rest of the firmware
 * uses ble_service.h.
 *
 * What the sensor must do (see README "Wireless sensor"):
 *   - advertise the Guder Sensor service 6e7a0010-3c1f-4b8e-9d2a-5f4c1b2a7e90
 *     (or use a name starting with CONFIG_GW_BLE_SENSOR_NAME_PREFIX)
 *   - offer characteristic 6e7a0011-... with NOTIFY
 *   - notify $GKS frames (same text format as the UART link). One frame per
 *     notification without newline, or a stream split freely with newlines.
 */

typedef struct
{
    bool     active;          /* central role switched on */
    bool     scanning;
    bool     connected;
    bool     subscribed;      /* receiving notifications */
    char     name[24];        /* sensor's advertised name, "" if unknown */
    uint32_t frames;          /* valid frames received since boot */
} ble_central_status_t;

/* Called by ble_service from the NimBLE host when the stack is ready / reset. */
void ble_central_on_sync(uint8_t own_addr_type);
void ble_central_on_reset(void);

/* Start scanning (and keep reconnecting) or stop and disconnect. */
void ble_central_set_active(bool active);

void ble_central_status(ble_central_status_t *out);

#ifdef __cplusplus
}
#endif
