#include "kneil_raw_policy.h"

bool kneil_raw_stop_on_connect(bool connected, bool raw_owner_active)
{
    return connected && !raw_owner_active;
}
