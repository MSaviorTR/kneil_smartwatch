#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the UART receiver task for frames from the sensor MCU
   (see sensor_protocol.h for the wire format). */
esp_err_t sensor_link_start(void);

/* Starts the demo generator. It only produces data while demo mode is
   on in Settings and no real sensor frames are arriving. */
esp_err_t sensor_demo_start(void);

#ifdef __cplusplus
}
#endif
