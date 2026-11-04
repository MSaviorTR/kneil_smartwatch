/*
 * UI simulator: runs the real watch UI (main/ui/*.c) on the host with
 * LVGL and fake sensor data, and writes screenshots - no watch needed.
 *
 *   ./test/sim/run.sh            -> test/sim/out/*.png
 *
 * Every hardware-facing service the UI calls (app_state, BLE, logger,
 * RTC, haptics, BSP) is replaced by the stubs below, driven by a
 * scenario name.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"

#include "app_state.h"
#include "ble_service.h"
#include "data_logger.h"
#include "haptics.h"
#include "rtc_service.h"
#include "bsp/esp-bsp.h"
#include "esp_timer.h"

#include "ui.h"
#include "ui_internal.h"

/* ------------------------------------------------------------------
   Scenario
   ------------------------------------------------------------------ */

typedef enum { SC_LIVE, SC_ALERT, SC_DEMO, SC_UNCAL, SC_NODATA } scenario_t;

static scenario_t sc;
static uint32_t   sim_ms;
static app_settings_t settings = {
    .ble_enabled = true, .vibration_enabled = true, .demo_mode = false,
    .k_alert_mM = 6.0f, .brightness = 80,
};
static cal_fit_t cal[CAL_CHANNEL_COUNT] = {
    [CAL_CHANNEL_K]  = { .slope = 57.9f, .intercept = 114.6f, .r2 = 0.9996f, .n_points = 3, .valid = true, .fitted_at = 1792488000 },
    [CAL_CHANNEL_PH] = { .slope = -56.8f, .intercept = 404.1f, .r2 = 0.9991f, .n_points = 3, .valid = true, .fitted_at = 1792488000 },
};

static float k_at(int i, int n)
{
    /* i = sample index, n = total; newest is n-1. */
    float t = (float)i;
    float base = 4.2f + 0.15f * sinf(t / 140.0f) + 0.04f * sinf(t * 1.7f);

    if(sc == SC_ALERT)
    {
        int from_end = n - 1 - i;

        if(from_end < 480)
        {
            base += 3.6f * (1.0f - (float)from_end / 480.0f);
        }
    }

    return base;
}

static float ph_at(int i, int n)
{
    float base = 7.40f + 0.02f * sinf((float)i / 200.0f);

    if(sc == SC_ALERT && n - 1 - i < 480)
    {
        base -= 0.22f * (1.0f - (float)(n - 1 - i) / 480.0f);
    }

    return base;
}

/* ------------------------------------------------------------------
   Stubs: app_state
   ------------------------------------------------------------------ */

bool app_state_latest(reading_t *out)
{
    if(sc == SC_NODATA)
    {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->epoch_ms = (int64_t)time(NULL) * 1000;
    out->source = (sc == SC_DEMO) ? DATA_SRC_DEMO : DATA_SRC_SENSOR;
    out->seq = 5321;
    out->k_mM = k_at(1799, 1800);
    out->ph = ph_at(1799, 1800);
    out->k_mv = cal[0].intercept + cal[0].slope * log10f(out->k_mM);
    out->ph_mv = cal[1].intercept + cal[1].slope * out->ph;
    out->temp_c = 36.7f;
    out->accel_mag_g = 1.01f;
    out->battery_pct = 82;
    out->k_alert = (sc == SC_ALERT);

    if(sc == SC_UNCAL)
    {
        out->k_mM = NAN;
        out->ph = NAN;
    }

    return true;
}

data_source_t app_state_active_source(void)
{
    return sc == SC_NODATA ? DATA_SRC_NONE : sc == SC_DEMO ? DATA_SRC_DEMO : DATA_SRC_SENSOR;
}

void app_state_link_stats(link_stats_t *out)
{
    memset(out, 0, sizeof(*out));

    if(sc != SC_NODATA && sc != SC_DEMO)
    {
        out->frames_ok = 5321;
        out->frames_dropped = 2;
        out->frames_bad_checksum = 1;
        out->last_frame_us = esp_timer_get_time() - 400000;
    }
}

bool app_state_link_alive(void) { return sc != SC_NODATA && sc != SC_DEMO; }

void app_state_motion(motion_state_t *out)
{
    *out = (motion_state_t){ .accel_g = { 0.02f, -0.11f, 0.99f }, .gyro_dps = { 1, -2, 0 },
                             .temperature_c = 31.5f, .accel_mag_g = 1.0f, .motion = false, .valid = true };
}

bool app_state_power(power_status_t *out)
{
    *out = (power_status_t){ .present = true, .percent = 82, .voltage_mv = 3986,
                             .charging = false, .usb_connected = false };
    return true;
}

int app_state_history(float *k_mM, float *ph, int max)
{
    if(sc == SC_NODATA)
    {
        return 0;
    }

    int n = max < 1800 ? max : 1800;

    for(int i = 0; i < n; i++)
    {
        int idx = 1800 - n + i;
        if(k_mM) k_mM[i] = (sc == SC_UNCAL) ? NAN : k_at(idx, 1800);
        if(ph)   ph[i]   = (sc == SC_UNCAL) ? NAN : ph_at(idx, 1800);
    }

    return n;
}

bool app_state_window_stats(cal_channel_t ch, int window_s, window_stats_t *out)
{
    (void)window_s;
    *out = (window_stats_t){ .mean_mv = ch == CAL_CHANNEL_K ? 151.24f : -16.08f,
                             .sd_mv = 0.21f, .drift_mv = 0.32f, .n = 30 };
    return sc != SC_NODATA;
}

void app_state_get_settings(app_settings_t *out) { *out = settings; }
esp_err_t app_state_set_settings(const app_settings_t *in) { settings = *in; return ESP_OK; }
void app_state_get_calibration(cal_channel_t ch, cal_fit_t *out) { *out = cal[ch]; }
esp_err_t app_state_set_calibration(cal_channel_t ch, const cal_fit_t *f) { cal[ch] = *f; return ESP_OK; }

const char *app_state_source_name(data_source_t s)
{
    return s == DATA_SRC_SENSOR ? "sensor" : s == DATA_SRC_DEMO ? "demo" : "none";
}

/* ------------------------------------------------------------------
   Stubs: other services
   ------------------------------------------------------------------ */

bool ble_service_connected(void) { return sc == SC_LIVE || sc == SC_ALERT; }
bool ble_service_advertising(void) { return true; }
void ble_service_set_enabled(bool on) { (void)on; }

void data_logger_status(logger_status_t *out)
{
    *out = (logger_status_t){ .store = LOG_STORE_SD, .rows = 5300,
                              .total_bytes = 31ull << 30, .free_bytes = 30ull << 30 };
    strcpy(out->file, "gw_20261020.csv");
}

void data_logger_clear(void) {}
void data_logger_log_calibration(cal_channel_t ch, const cal_fit_t *f, const cal_point_t *p, int n)
{
    (void)ch; (void)f; (void)p; (void)n;
}

bool rtc_service_time_valid(void) { return true; }
time_source_t rtc_service_time_source(void) { return TIME_SRC_RTC; }
esp_err_t rtc_service_set_epoch(time_t t) { (void)t; return ESP_OK; }

void haptics_tap(void) {}
esp_err_t bsp_display_brightness_set(int pct) { (void)pct; return ESP_OK; }

int64_t esp_timer_get_time(void) { return (int64_t)sim_ms * 1000; }

/* ------------------------------------------------------------------
   Rendering
   ------------------------------------------------------------------ */

static uint8_t *fb;

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a; (void)px;
    lv_display_flush_ready(d);
}

static uint32_t tick_cb(void) { return sim_ms; }

static void run_ms(uint32_t ms)
{
    for(uint32_t t = 0; t < ms; t += 5)
    {
        sim_ms += 5;
        lv_timer_handler();
    }
}

/* 32-bit top-down BMP; LVGL's XRGB8888 is B,G,R,X in memory like BMP. */
static void save(const char *dir, const char *name)
{
    lv_refr_now(NULL);

    char path[512];
    snprintf(path, sizeof(path), "%s/%s.bmp", dir, name);

    FILE *f = fopen(path, "wb");
    if(!f) { perror(path); exit(1); }

    uint32_t w = UI_W, h = UI_H, img = w * h * 4;
    uint8_t hdr[54] = { 'B', 'M' };
    uint32_t v;

    v = 54 + img; memcpy(hdr + 2, &v, 4);
    v = 54;       memcpy(hdr + 10, &v, 4);
    v = 40;       memcpy(hdr + 14, &v, 4);
    memcpy(hdr + 18, &w, 4);
    int32_t neg_h = -(int32_t)h; memcpy(hdr + 22, &neg_h, 4);
    hdr[26] = 1; hdr[28] = 32;
    memcpy(hdr + 34, &img, 4);

    fwrite(hdr, 1, sizeof(hdr), f);

    /* Force alpha opaque so viewers don't show the frame as transparent. */
    for(uint32_t i = 0; i < w * h; i++)
    {
        uint8_t px[4] = { fb[i * 4], fb[i * 4 + 1], fb[i * 4 + 2], 0xFF };
        fwrite(px, 1, 4, f);
    }

    fclose(f);
    printf("  %s\n", path);
}

static void show_page(ui_page_t p)
{
    lv_tileview_set_tile_by_index(ui_tv, p, 0, LV_ANIM_ON);
    run_ms(2600);   /* scroll animation + at least one slow refresh */
}

int main(int argc, char **argv)
{
    if(argc != 3)
    {
        fprintf(stderr, "usage: %s live|alert|demo|uncal|nodata OUTDIR\n", argv[0]);
        return 2;
    }

    const char *names[] = { "live", "alert", "demo", "uncal", "nodata" };

    for(int i = 0; i < 5; i++)
    {
        if(strcmp(argv[1], names[i]) == 0) sc = (scenario_t)i;
    }

    setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0", 1);
    tzset();

    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *disp = lv_display_create(UI_W, UI_H);
    size_t fb_size = (size_t)UI_W * UI_H * 4;
    fb = calloc(1, fb_size);
    lv_display_set_buffers(disp, fb, NULL, fb_size, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);

    ui_init();

    const char *out = argv[2];
    char name[64];

    if(sc == SC_LIVE)
    {
        run_ms(400);
        save(out, "00_splash");
    }

    run_ms(3000);   /* splash -> face */
    snprintf(name, sizeof(name), "01_face_%s", names[sc]);
    save(out, name);

    if(sc == SC_LIVE)
    {
        show_page(UI_PAGE_TREND);    save(out, "02_trends");
        show_page(UI_PAGE_CALIB);    save(out, "03_calibrate");
        show_page(UI_PAGE_DIAG);     save(out, "04_diagnostics");
        show_page(UI_PAGE_SETTINGS); save(out, "05_settings");

        ui_open_time();
        run_ms(800);
        save(out, "06_set_time");
    }

    return 0;
}
