#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_log.h"

#include "power_service.h"
#include "imu_service.h"

static const char *TAG = "watch_ui";

/* Never go fully dark: bsp_display_brightness_set(0) blanks the AMOLED. */
#define BRIGHTNESS_MIN 10

/* =========================
   SCREENS
   ========================= */

static lv_obj_t *home_screen;
static lv_obj_t *apps_screen;
static lv_obj_t *sensors_screen;
static lv_obj_t *settings_screen;

/* =========================
   HOME OBJECTS
   ========================= */

static lv_obj_t *time_label;
static lv_obj_t *date_label;
static lv_obj_t *home_battery_label;

/* =========================
   SENSOR OBJECTS
   ========================= */

static lv_obj_t *accel_x_label;
static lv_obj_t *accel_y_label;
static lv_obj_t *accel_z_label;

static lv_obj_t *gyro_x_label;
static lv_obj_t *gyro_y_label;
static lv_obj_t *gyro_z_label;

static lv_obj_t *battery_label;

/* Fake clock values for UI testing */
static int hour = 14;
static int minute = 15;
static int second = 0;



/* ============================================================
   COMMON UI
   ============================================================ */

static void style_screen(lv_obj_t *screen)
{
    lv_obj_set_style_bg_color(
        screen,
        lv_color_hex(0x000000),
        LV_PART_MAIN
    );

    lv_obj_set_style_bg_opa(
        screen,
        LV_OPA_COVER,
        LV_PART_MAIN
    );
}


static lv_obj_t *create_button(
    lv_obj_t *parent,
    const char *text,
    int y,
    lv_event_cb_t callback
)
{
    lv_obj_t *button = lv_button_create(parent);

    lv_obj_set_size(button, 300, 65);

    lv_obj_align(
        button,
        LV_ALIGN_TOP_MID,
        0,
        y
    );

    lv_obj_add_event_cb(
        button,
        callback,
        LV_EVENT_CLICKED,
        NULL
    );

    lv_obj_t *label = lv_label_create(button);

    lv_label_set_text(label, text);

    lv_obj_center(label);

    return button;
}


/* ============================================================
   BRIGHTNESS
   ============================================================ */

static void brightness_changed_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target_obj(e);

    int percent = lv_slider_get_value(slider);

    if(percent < BRIGHTNESS_MIN)
    {
        percent = BRIGHTNESS_MIN;
    }

    esp_err_t err = bsp_display_brightness_set(percent);

    if(err != ESP_OK)
    {
        ESP_LOGE(TAG, "brightness_set(%d) failed: %s", percent, esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "brightness -> %d%%", percent);
}


/* ============================================================
   NAVIGATION CALLBACKS
   ============================================================ */

static void open_home(lv_event_t *e)
{
    (void)e;
    lv_screen_load(home_screen);
}


static void open_apps(lv_event_t *e)
{
    (void)e;
    lv_screen_load(apps_screen);
}


static void open_sensors(lv_event_t *e)
{
    (void)e;
    lv_screen_load(sensors_screen);
}


static void open_settings(lv_event_t *e)
{
    (void)e;
    lv_screen_load(settings_screen);
}


/* ============================================================
   CLOCK
   ============================================================ */

static void clock_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    second++;

    if(second >= 60)
    {
        second = 0;
        minute++;
    }

    if(minute >= 60)
    {
        minute = 0;
        hour++;
    }

    if(hour >= 24)
    {
        hour = 0;
    }

    lv_label_set_text_fmt(
        time_label,
        "%02d:%02d:%02d",
        hour,
        minute,
        second
    );
}


/* ============================================================
   BATTERY (real hardware, via AXP2101)
   ============================================================ */

static void battery_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    power_status_t status;

    if(power_service_read(&status) != ESP_OK || status.percent < 0)
    {
        lv_label_set_text(home_battery_label, "BAT --");
        lv_label_set_text(battery_label, "Battery: no cell");
        return;
    }

    /* Charging bolt only if the font has it; plain text is always safe. */

    lv_label_set_text_fmt(
        home_battery_label,
        "BAT %d%%%s",
        status.percent,
        status.charging ? " +" : ""
    );

    lv_label_set_text_fmt(
        battery_label,
        "Battery: %d%%  %u mV  %s",
        status.percent,
        (unsigned)status.voltage_mv,
        status.charging      ? "charging" :
        status.usb_connected ? "usb"      : "discharging"
    );
}


/* ============================================================
   SENSOR DATA (real hardware, via QMI8658)
   ============================================================ */

/* LVGL's printf has float support compiled out by default, so every
   value is converted to integer parts before formatting. */

static void set_axis_label(lv_obj_t *label, const char *name,
                           const char *unit, float value, int decimals)
{
    bool negative = value < 0.0f;

    if(negative)
    {
        value = -value;
    }

    int scale = (decimals == 2) ? 100 : 1;
    int fixed = (int)(value * scale + 0.5f);

    if(decimals == 2)
    {
        lv_label_set_text_fmt(label, "%s: %s%d.%02d %s",
                              name,
                              negative ? "-" : "",
                              fixed / 100,
                              fixed % 100,
                              unit);
    }
    else
    {
        lv_label_set_text_fmt(label, "%s: %s%d %s",
                              name,
                              negative ? "-" : "",
                              fixed,
                              unit);
    }
}


static void sensor_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    imu_sample_t sample;

    if(imu_service_read(&sample) != ESP_OK)
    {
        lv_label_set_text(accel_x_label, "Accel X: --");
        lv_label_set_text(accel_y_label, "Accel Y: --");
        lv_label_set_text(accel_z_label, "Accel Z: --");
        lv_label_set_text(gyro_x_label,  "Gyro X: --");
        lv_label_set_text(gyro_y_label,  "Gyro Y: --");
        lv_label_set_text(gyro_z_label,  "Gyro Z: --");
        return;
    }

    set_axis_label(accel_x_label, "Accel X", "g", sample.accel_g[0], 2);
    set_axis_label(accel_y_label, "Accel Y", "g", sample.accel_g[1], 2);
    set_axis_label(accel_z_label, "Accel Z", "g", sample.accel_g[2], 2);

    set_axis_label(gyro_x_label, "Gyro X", "dps", sample.gyro_dps[0], 0);
    set_axis_label(gyro_y_label, "Gyro Y", "dps", sample.gyro_dps[1], 0);
    set_axis_label(gyro_z_label, "Gyro Z", "dps", sample.gyro_dps[2], 0);
}


/* ============================================================
   HOME SCREEN
   ============================================================ */

static void create_home_screen(void)
{
    home_screen = lv_obj_create(NULL);

    style_screen(home_screen);


    /* WATCH TITLE */

    lv_obj_t *title =
        lv_label_create(home_screen);

    lv_label_set_text(
        title,
        "ESP WATCH"
    );

    lv_obj_align(
        title,
        LV_ALIGN_TOP_MID,
        0,
        25
    );


    /* TIME */

    time_label =
        lv_label_create(home_screen);

    lv_label_set_text(
        time_label,
        "14:15:00"
    );

    lv_obj_set_style_text_font(
        time_label,
        &lv_font_montserrat_18,
        LV_PART_MAIN
    );

    lv_obj_align(
        time_label,
        LV_ALIGN_TOP_MID,
        0,
        80
    );


    /* DATE */

    date_label =
        lv_label_create(home_screen);

    lv_label_set_text(
        date_label,
        "20 AUG 2026"
    );

    lv_obj_align(
        date_label,
        LV_ALIGN_TOP_MID,
        0,
        145
    );


    /* BATTERY */

    home_battery_label =
        lv_label_create(home_screen);

    lv_label_set_text(
        home_battery_label,
        "BAT --"
    );

    lv_obj_align(
        home_battery_label,
        LV_ALIGN_TOP_RIGHT,
        -20,
        20
    );


    create_button(
        home_screen,
        "APPS",
        230,
        open_apps
    );

    create_button(
        home_screen,
        "SENSORS",
        310,
        open_sensors
    );
}


/* ============================================================
   APPS SCREEN
   ============================================================ */

static void create_apps_screen(void)
{
    apps_screen = lv_obj_create(NULL);

    style_screen(apps_screen);


    lv_obj_t *title =
        lv_label_create(apps_screen);

    lv_label_set_text(
        title,
        "APPLICATIONS"
    );

    lv_obj_align(
        title,
        LV_ALIGN_TOP_MID,
        0,
        30
    );


    create_button(
        apps_screen,
        "SENSORS",
        100,
        open_sensors
    );

    create_button(
        apps_screen,
        "SETTINGS",
        180,
        open_settings
    );

    create_button(
        apps_screen,
        "HOME",
        340,
        open_home
    );
}


/* ============================================================
   SENSOR SCREEN
   ============================================================ */

static void create_sensors_screen(void)
{
    sensors_screen = lv_obj_create(NULL);

    style_screen(sensors_screen);


    lv_obj_t *title =
        lv_label_create(sensors_screen);

    lv_label_set_text(
        title,
        "SENSOR DATA"
    );

    lv_obj_align(
        title,
        LV_ALIGN_TOP_MID,
        0,
        20
    );


    accel_x_label =
        lv_label_create(sensors_screen);

    lv_label_set_text(
        accel_x_label,
        "Accel X: 0.00 g"
    );

    lv_obj_align(
        accel_x_label,
        LV_ALIGN_TOP_LEFT,
        35,
        80
    );


    accel_y_label =
        lv_label_create(sensors_screen);

    lv_label_set_text(
        accel_y_label,
        "Accel Y: 0.00 g"
    );

    lv_obj_align(
        accel_y_label,
        LV_ALIGN_TOP_LEFT,
        35,
        115
    );


    accel_z_label =
        lv_label_create(sensors_screen);

    lv_label_set_text(
        accel_z_label,
        "Accel Z: 1.00 g"
    );

    lv_obj_align(
        accel_z_label,
        LV_ALIGN_TOP_LEFT,
        35,
        150
    );


    gyro_x_label =
        lv_label_create(sensors_screen);

    lv_label_set_text(
        gyro_x_label,
        "Gyro X: 0 dps"
    );

    lv_obj_align(
        gyro_x_label,
        LV_ALIGN_TOP_LEFT,
        35,
        205
    );


    gyro_y_label =
        lv_label_create(sensors_screen);

    lv_label_set_text(
        gyro_y_label,
        "Gyro Y: 0 dps"
    );

    lv_obj_align(
        gyro_y_label,
        LV_ALIGN_TOP_LEFT,
        35,
        240
    );


    gyro_z_label =
        lv_label_create(sensors_screen);

    lv_label_set_text(
        gyro_z_label,
        "Gyro Z: 0 dps"
    );

    lv_obj_align(
        gyro_z_label,
        LV_ALIGN_TOP_LEFT,
        35,
        275
    );


    battery_label =
        lv_label_create(sensors_screen);

    lv_label_set_text(
        battery_label,
        "Battery: --"
    );

    lv_obj_align(
        battery_label,
        LV_ALIGN_TOP_LEFT,
        35,
        330
    );


    create_button(
        sensors_screen,
        "BACK",
        400,
        open_apps
    );
}


/* ============================================================
   SETTINGS SCREEN
   ============================================================ */

static void create_settings_screen(void)
{
    settings_screen =
        lv_obj_create(NULL);

    style_screen(settings_screen);


    lv_obj_t *title =
        lv_label_create(settings_screen);

    lv_label_set_text(
        title,
        "SETTINGS"
    );

    lv_obj_align(
        title,
        LV_ALIGN_TOP_MID,
        0,
        20
    );


    /* WIFI */

    lv_obj_t *wifi_text =
        lv_label_create(settings_screen);

    lv_label_set_text(
        wifi_text,
        "Wi-Fi"
    );

    lv_obj_align(
        wifi_text,
        LV_ALIGN_TOP_LEFT,
        30,
        90
    );

    lv_obj_t *wifi_switch =
        lv_switch_create(settings_screen);

    lv_obj_align(
        wifi_switch,
        LV_ALIGN_TOP_RIGHT,
        -30,
        80
    );


    /* BLUETOOTH */

    lv_obj_t *bluetooth_text =
        lv_label_create(settings_screen);

    lv_label_set_text(
        bluetooth_text,
        "Bluetooth"
    );

    lv_obj_align(
        bluetooth_text,
        LV_ALIGN_TOP_LEFT,
        30,
        150
    );

    lv_obj_t *bluetooth_switch =
        lv_switch_create(settings_screen);

    lv_obj_align(
        bluetooth_switch,
        LV_ALIGN_TOP_RIGHT,
        -30,
        140
    );


    /* VIBRATION */

    lv_obj_t *vibration_text =
        lv_label_create(settings_screen);

    lv_label_set_text(
        vibration_text,
        "Vibration"
    );

    lv_obj_align(
        vibration_text,
        LV_ALIGN_TOP_LEFT,
        30,
        210
    );

    lv_obj_t *vibration_switch =
        lv_switch_create(settings_screen);

    lv_obj_align(
        vibration_switch,
        LV_ALIGN_TOP_RIGHT,
        -30,
        200
    );


    /* BRIGHTNESS */

    lv_obj_t *brightness_text =
        lv_label_create(settings_screen);

    lv_label_set_text(
        brightness_text,
        "Brightness"
    );

    lv_obj_align(
        brightness_text,
        LV_ALIGN_TOP_LEFT,
        30,
        275
    );


    lv_obj_t *brightness =
        lv_slider_create(settings_screen);

    lv_obj_set_width(
        brightness,
        340
    );

    lv_slider_set_range(
        brightness,
        0,
        100
    );

    /* Seed from the panel instead of a hard-coded 70:
       the BSP sets 100% during bsp_display_new(). */

    int current_brightness = bsp_display_brightness_get();

    if(current_brightness < 0)
    {
        current_brightness = 100;
    }

    lv_slider_set_value(
        brightness,
        current_brightness,
        LV_ANIM_OFF
    );

    lv_obj_add_event_cb(
        brightness,
        brightness_changed_cb,
        LV_EVENT_VALUE_CHANGED,
        NULL
    );

    lv_obj_align(
        brightness,
        LV_ALIGN_TOP_MID,
        0,
        320
    );


    create_button(
        settings_screen,
        "BACK",
        390,
        open_apps
    );
}


/* ============================================================
   MAIN
   ============================================================ */

void app_main(void)
{
    /*
     * Waveshare BSP starts:
     *
     * AMOLED display
     * Touch controller
     * LVGL
     */

    bsp_display_start();


    /* Power IC shares the BSP's I2C bus. Non-fatal if absent: the UI
       just shows "--" instead of a percentage. */

    if(power_service_init() != ESP_OK)
    {
        ESP_LOGW(TAG, "AXP2101 unavailable - battery readings disabled");
    }

    if(imu_service_init() != ESP_OK)
    {
        ESP_LOGW(TAG, "QMI8658 unavailable - sensor screen will show --");
    }


    bsp_display_lock(0);


    /* Build smartwatch interface */

    create_home_screen();

    create_apps_screen();

    create_sensors_screen();

    create_settings_screen();


    /* Start on watch face */

    lv_screen_load(home_screen);


    /* Clock update every 1 second */

    lv_timer_create(
        clock_timer_cb,
        1000,
        NULL
    );


    /* Sensor UI update every 500 ms */

    lv_timer_create(
        sensor_timer_cb,
        500,
        NULL
    );


    /* Battery is slow-moving: 5 s is plenty and keeps I2C quiet */

    lv_timer_t *battery_timer = lv_timer_create(
        battery_timer_cb,
        5000,
        NULL
    );

    lv_timer_ready(battery_timer);   /* fire once immediately */


    bsp_display_unlock();
}