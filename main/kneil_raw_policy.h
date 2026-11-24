#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Should a new KNEIL connection stop the implant's RAW stream? The implant keeps its RAW flag over a link
   loss, and the watch subscribes RAW on every connection, so an idle watch must send CONTROL 0x11 once per
   connection. A running therapy session or calibration owns the stream and must not be stopped. */
bool kneil_raw_stop_on_connect(bool connected, bool raw_owner_active);

#ifdef __cplusplus
}
#endif
