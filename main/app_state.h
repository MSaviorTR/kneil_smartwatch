#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "calibration.h"
#include "sensor_protocol.h"
#include "power_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Central data hub. Producers (sensor link, demo generator, housekeeping
 * task) push into it; consumers (UI timers, logger, BLE) read snapshots or
 * get called back. Nothing in here touches LVGL.
 */

typedef enum
{
    DATA_SRC_NONE = 0,
    DATA_SRC_SENSOR,    /* real frames from the sensor MCU */
    DATA_SRC_DEMO,      /* synthetic data, clearly labelled everywhere */
} data_source_t;

typedef struct
{
    int64_t       epoch_ms;     /* wall clock when received */
    int64_t       uptime_ms;
    data_source_t source;
    uint32_t      seq;
    float         k_mv;         /* NAN when channel absent */
    float         ph_mv;
    float         temp_c;
    float         k_mM;         /* NAN when uncalibrated / absent / faulted */
    float         ph;
    uint32_t      flags;        /* SENSOR_FLAG_* */
    float         accel_mag_g;  /* |a| from the watch IMU, NAN if no IMU */
    bool          motion;       /* movement big enough to disturb readings */
    int           battery_pct;  /* -1 unknown */
    bool          k_alert;
} reading_t;

typedef struct
{
    uint32_t frames_ok;
    uint32_t frames_bad_checksum;
    uint32_t frames_bad_format;
    uint32_t frames_dropped;    /* inferred from seq gaps */
    int64_t  last_frame_us;     /* esp_timer time, 0 = never */
} link_stats_t;

/* Which Bluetooth roles run. Both can run at once: central towards a
   wireless sensor (data in), peripheral towards a phone (data out). */
typedef enum
{
    BLE_MODE_PHONE = 0,    /* peripheral only: phone connects to the watch */
    BLE_MODE_SENSOR,       /* central only: watch connects to the sensor   */
    BLE_MODE_BOTH,         /* both at once                                 */
    BLE_MODE_COUNT
} ble_mode_t;

typedef struct
{
    bool    ble_enabled;
    uint8_t ble_mode;           /* ble_mode_t */
    bool    vibration_enabled;
    bool    demo_mode;
    float   k_alert_mM;
    uint8_t brightness;         /* 10..100 */
} app_settings_t;

typedef struct
{
    float accel_g[3];
    float gyro_dps[3];
    float temperature_c;
    float accel_mag_g;
    bool  motion;
    bool  valid;
} motion_state_t;

typedef void (*reading_listener_t)(const reading_t *reading);
typedef void (*alert_listener_t)(bool active, float k_mM);

esp_err_t app_state_init(void);

/* ---- producers -------------------------------------------------------- */

void app_state_push_frame(const sensor_frame_t *frame, data_source_t source);
void app_state_count_bad_frame(bool checksum_error);
void app_state_set_motion(const motion_state_t *m);
void app_state_set_power(const power_status_t *p);

/* ---- consumers -------------------------------------------------------- */

/* Latest reading; false if nothing has arrived from the active source. */
bool app_state_latest(reading_t *out);

/* Which source currently owns the display: real data always wins. */
data_source_t app_state_active_source(void);

void app_state_link_stats(link_stats_t *out);
bool app_state_link_alive(void);
void app_state_motion(motion_state_t *out);
bool app_state_power(power_status_t *out);

/* Fills `seconds` values per array, one per second, oldest first, ending
   with the current second. NAN = no valid data that second (gap, fault,
   uncalibrated). Values use the calibration in force now. Independent of
   the front end's sample rate. Returns how many seconds held data. */
int app_state_history(float *k_mM, float *ph, int seconds);

/* Mean / SD / drift of raw mV over the last window_s seconds, computed
   on per-second means, for calibration capture. drift = last - first. */
typedef struct
{
    float mean_mv;
    float sd_mv;
    float drift_mv;
    int   n;
} window_stats_t;

bool app_state_window_stats(cal_channel_t ch, int window_s, window_stats_t *out);

void app_state_add_listener(reading_listener_t cb);
void app_state_add_alert_listener(alert_listener_t cb);

/* ---- settings & calibration ------------------------------------------ */

void      app_state_get_settings(app_settings_t *out);
esp_err_t app_state_set_settings(const app_settings_t *in);

/* Calibration in use for real sensor data (demo data uses a fixed
   nominal calibration so the demo never depends on bench results). */
void      app_state_get_calibration(cal_channel_t ch, cal_fit_t *out);
esp_err_t app_state_set_calibration(cal_channel_t ch, const cal_fit_t *fit);

const char *app_state_source_name(data_source_t s);

#ifdef __cplusplus
}
#endif
