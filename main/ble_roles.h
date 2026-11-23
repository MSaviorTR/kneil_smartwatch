#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Order in which the two BLE centrals are switched, as pure C so the host tests can cover it.
 *
 * Both centrals share the radio, and a central that becomes inactive may still hold it (a discovery or
 * a connect in progress): that is why it must be switched off before the other one is switched on. This
 * is the F3 finding: KNEIL scan -> Sensor only used to start the sensor central first, which found the
 * radio busy and stayed idle.
 */

typedef enum
{
    BLE_ROLE_CENTRAL = 0,   /* the Guder-sensor central, main/ble_central.c */
    BLE_ROLE_KNEIL,         /* the KNEIL central, main/kneil_central.c */
} ble_role_t;

typedef struct
{
    ble_role_t role;
    bool       on;
} ble_role_step_t;

#define BLE_ROLES_MAX_STEPS 2   /* one step per role: its new state */

/* Writes one step per role, in the order apply_roles() must run them: every role that becomes inactive
   (off) first, then every role that becomes active (on). Returns the number of steps (always 2). */
int ble_roles_plan(bool central_on, bool kneil_on, ble_role_step_t steps[BLE_ROLES_MAX_STEPS]);

#ifdef __cplusplus
}
#endif
