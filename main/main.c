/*
 * Guder Watch - wearable readout for the Guder Lab K+ / pH biosensor.
 *
 *   pH + K+ electrodes -> sensor MCU (high-Z front end, ADC)
 *        -- UART frames (sensor_protocol.h) -->
 *   ESP32-S3 watch: calibration, display, alerts, logging, BLE
 *
 * Start-up order matters: NVS/settings first (everything reads them),
 * then the shared I2C bus users, then the display, then background tasks,
 * and the UI last so it only ever sees initialised services.
 */

#include "esp_log.h"

#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#include "app_state.h"
#include "ble_service.h"
#include "data_logger.h"
#include "haptics.h"
#include "housekeeping.h"
#include "imu_service.h"
#include "power_service.h"
#include "rtc_service.h"
#include "sensor_link.h"
#include "therapy.h"
#include "usb_console.h"
#include "ui.h"

static const char *TAG = "main";

/* Logs and carries on: every service degrades gracefully on its own. */
static void check(const char *what, esp_err_t err)
{
    if(err != ESP_OK)
    {
        ESP_LOGW(TAG, "%s: %s - continuing without it", what, esp_err_to_name(err));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(app_state_init());

    check("rtc", rtc_service_init());

    if(bsp_display_start() == NULL)
    {
        ESP_LOGE(TAG, "display failed to start");
    }

    app_settings_t s;
    app_state_get_settings(&s);
    bsp_display_brightness_set(s.brightness);

    check("power (AXP2101)", power_service_init());
    check("imu (QMI8658)", imu_service_init());
    check("haptics", haptics_init());
    check("logger", data_logger_start());
    check("sensor link", sensor_link_start());
    check("demo generator", sensor_demo_start());
    check("therapy", therapy_init());
    check("ble", ble_service_start());
    check("housekeeping", housekeeping_start());

    bsp_display_lock(0);
    ui_init();
    bsp_display_unlock();

    check("usb console", usb_console_start());

    ESP_LOGI(TAG, "up - type 'help' on this console");
}
