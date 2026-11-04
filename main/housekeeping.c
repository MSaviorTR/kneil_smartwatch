#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "app_state.h"
#include "ble_service.h"
#include "housekeeping.h"
#include "imu_service.h"
#include "power_service.h"

#define IMU_PERIOD_MS      100
#define POWER_EVERY_N      50     /* 50 x 100 ms = 5 s */

/* Thresholds for "the wearer is moving enough to disturb a potentiometric
   reading". Starting points only - tune against real recordings. */
#define MOTION_ACCEL_G     0.15f  /* deviation of |a| from 1 g */
#define MOTION_GYRO_DPS    40.0f

static void housekeeping_task(void *arg)
{
    (void)arg;

    TickType_t wake = xTaskGetTickCount();
    int tick = 0;

    for(;;)
    {
        motion_state_t m = { .accel_mag_g = NAN };
        imu_sample_t s;

        if(imu_service_ready() && imu_service_read(&s) == ESP_OK)
        {
            float a = sqrtf(s.accel_g[0] * s.accel_g[0] +
                            s.accel_g[1] * s.accel_g[1] +
                            s.accel_g[2] * s.accel_g[2]);
            float g = sqrtf(s.gyro_dps[0] * s.gyro_dps[0] +
                            s.gyro_dps[1] * s.gyro_dps[1] +
                            s.gyro_dps[2] * s.gyro_dps[2]);

            for(int i = 0; i < 3; i++)
            {
                m.accel_g[i]  = s.accel_g[i];
                m.gyro_dps[i] = s.gyro_dps[i];
            }

            m.temperature_c = s.temperature_c;
            m.accel_mag_g   = a;
            m.motion        = fabsf(a - 1.0f) > MOTION_ACCEL_G || g > MOTION_GYRO_DPS;
            m.valid         = true;
        }

        app_state_set_motion(&m);

        if(tick % POWER_EVERY_N == 0)
        {
            power_status_t p;

            if(power_service_read(&p) == ESP_OK)
            {
                app_state_set_power(&p);
                ble_service_update_battery(p.percent);
            }
        }

        tick++;
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(IMU_PERIOD_MS));
    }
}

esp_err_t housekeeping_start(void)
{
    if(xTaskCreate(housekeeping_task, "housekeeping", 4096, NULL, 3, NULL) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
