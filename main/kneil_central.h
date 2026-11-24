#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "kneil_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * KNEIL central: the watch connects TO a KNEIL implant monitor and receives its LIVE / INFO / SUMMARY
 * notifications (protocol v2; v1 packets are decoded too). Only active in BLE mode "KNEIL device".
 *
 * The device must advertise the KNEIL service a8f50001-3e2c-4c6e-9b1a-6b4e45494c00 (in the scan
 * response) or a name starting with "KNEIL-", and offer
 *   LIVE    a8f50002 (notify, 20 or 22 bytes),  INFO a8f50005 (notify, text),  SUMMARY a8f50006 (notify),
 *   CONTROL a8f50004 (write: 0x12 self-test, 0x13 summary now, 0x14 reset the daily metrics).
 * INFO and SUMMARY are optional (a v1 firmware has no SUMMARY).
 *
 * Everything the page shows is in kneil_model_t (main/kneil_state.h); this file owns one instance and
 * serialises access to it. KNEIL data never enters the K+/pH pipeline, the logger or the alerts.
 */

/* Called by ble_service from the NimBLE host when the stack is ready / reset. */
void kneil_central_on_sync(uint8_t own_addr_type);
void kneil_central_on_reset(void);

/* Start scanning (and keep reconnecting) or stop and disconnect. */
void kneil_central_set_active(bool active);

/* Copy of the page model (thread-safe). */
void kneil_central_snapshot(kneil_model_t *out);

/* Ask the device: KNEIL_CTRL_SELFTEST (0x12), KNEIL_CTRL_SUMMARY (0x13), KNEIL_CTRL_RESET (0x14),
   KNEIL_CTRL_RAW_START (0x10), KNEIL_CTRL_RAW_STOP (0x11).
   Returns false if there is no link. */
bool kneil_central_send_ctrl(uint8_t ctrl);

/* RAW notifications (50 Hz int16 x/y/z, n samples per packet) for the therapy module. The callback runs on
   the NimBLE host task; it must not block. Copy the packet, do not keep the pointer. */
typedef void (*kneil_raw_listener_t)(const kneil_raw_t *raw);
void kneil_central_set_raw_listener(kneil_raw_listener_t cb);

/* Console mode: print every received LIVE / INFO / SUMMARY as the protocol's text lines (L, I, D). */
void kneil_central_set_lines(bool on);
bool kneil_central_lines(void);

#ifdef __cplusplus
}
#endif
