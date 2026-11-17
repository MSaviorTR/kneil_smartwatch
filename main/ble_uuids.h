#pragma once

/*
 * Guder Bluetooth UUIDs, shared by the phone side (ble_service.c) and the
 * sensor side (ble_central.c):  6e7a00XX-3c1f-4b8e-9d2a-5f4c1b2a7e90
 *
 *   0x01  watch service          (phone reads from the watch)
 *   0x02    live reading         read, notify
 *   0x03    time                 read, write
 *   0x10  Guder Sensor service   (watch reads from a wireless sensor)
 *   0x11    $GKS frames          notify
 *
 * NimBLE wants the 128-bit value little-endian, hence the reversed bytes.
 */

#include "host/ble_uuid.h"

#define GW_UUID(x) BLE_UUID128_INIT(0x90, 0x7e, 0x2a, 0x1b, 0x4c, 0x5f, 0x2a, 0x9d, \
                                    0x8e, 0x4b, 0x1f, 0x3c, (x) & 0xff, 0x00, 0x7a, 0x6e)
