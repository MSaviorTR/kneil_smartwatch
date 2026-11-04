#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * QMI8658 6-axis IMU (3-axis accelerometer + 3-axis gyroscope).
 *
 * Talks to the chip over the same I2C bus the BSP created for the touch
 * controller. The UI never sees a register — it only sees this struct.
 */

typedef struct
{
    float accel_g[3];    /* X, Y, Z in g          */
    float gyro_dps[3];   /* X, Y, Z in degrees/s  */
    float temperature_c; /* die temperature       */
} imu_sample_t;

/* Probes the chip and configures it. ESP_FAIL if it does not answer. */
esp_err_t imu_service_init(void);

/* True once init has succeeded. */
bool imu_service_ready(void);

/* Latest conversion. ESP_ERR_INVALID_STATE if the IMU is not initialised. */
esp_err_t imu_service_read(imu_sample_t *out);

#ifdef __cplusplus
}
#endif
