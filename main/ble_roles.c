#include "ble_roles.h"

int ble_roles_plan(bool central_on, bool kneil_on, ble_role_step_t steps[BLE_ROLES_MAX_STEPS])
{
    int n = 0;

    /* off first: free the radio before the other central starts */
    if(!central_on)
    {
        steps[n++] = (ble_role_step_t){ .role = BLE_ROLE_CENTRAL, .on = false };
    }

    if(!kneil_on)
    {
        steps[n++] = (ble_role_step_t){ .role = BLE_ROLE_KNEIL, .on = false };
    }

    if(central_on)
    {
        steps[n++] = (ble_role_step_t){ .role = BLE_ROLE_CENTRAL, .on = true };
    }

    if(kneil_on)
    {
        steps[n++] = (ble_role_step_t){ .role = BLE_ROLE_KNEIL, .on = true };
    }

    return n;
}
