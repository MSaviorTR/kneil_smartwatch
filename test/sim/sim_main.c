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
#include "kneil_central.h"
#include "therapy.h"

void therapy_sim_init(void);
void therapy_sim_set(const therapy_snapshot_t *s);

/* ------------------------------------------------------------------
   Scenario
   ------------------------------------------------------------------ */

typedef enum { SC_LIVE, SC_ALERT, SC_DEMO, SC_UNCAL, SC_NODATA, SC_KNEIL, SC_THERAPY } scenario_t;

static scenario_t sc;
static uint32_t   sim_ms;
static app_settings_t settings = {
    .ble_enabled = true, .ble_mode = BLE_MODE_BOTH, .vibration_enabled = true, .demo_mode = false,
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
void ble_service_set_mode(uint8_t mode) { settings.ble_mode = mode; }

/* Stubs: KNEIL central (ui_kneil.c reads this page model) */
static kneil_model_t kneil_demo;

void kneil_central_snapshot(kneil_model_t *out) { *out = kneil_demo; }
bool kneil_central_send_ctrl(uint8_t ctrl) { (void)ctrl; return true; }
void kneil_central_set_lines(bool on) { (void)on; }
bool kneil_central_lines(void) { return false; }

/* A walking session with two transitions, a gap and a daily summary, to render the KNEIL page. */
static void kneil_demo_fill(void)
{
    kneil_live_t p;

    kneil_model_reset(&kneil_demo);
    kneil_model_set_active(&kneil_demo, true);
    kneil_model_set_link(&kneil_demo, KNEIL_LINK_CONNECTED, "KNEIL-7AD1", -48, 247);

    for(int i = 0; i < 40; i++)
    {
        memset(&p, 0, sizeof(p));
        p.version = 2;
        p.seq = (uint16_t)(100 + i + (i > 20 ? 2 : 0));      /* one gap after the 20th packet */
        p.t_ms = (uint32_t)(i * 1000);
        p.cls = 0;
        p.cls_smooth = (uint8_t)(i < 3 ? 3 : 0);
        p.raw_cls = (uint8_t)(i % 17 == 0 ? 1 : 0);
        p.vote_pct = (uint8_t)(70 + i % 20);
        p.events = (uint8_t)((i == 10 ? KNEIL_EV_STAND_TO_SIT : 0) |
                            (i == 14 ? KNEIL_EV_SIT_TO_STAND : 0) |
                            (KNEIL_POSTURE_STANDING << KNEIL_EV_POSTURE_SHIFT));
        kneil_model_on_live(&kneil_demo, &p, (int64_t)i * 1000000);
    }

    kneil_summary_t s;

    memset(&s, 0, sizeof(s));
    s.format = 2;
    s.n_classes = 8;
    s.minutes_by_class[0] = 12; s.minutes_by_class[1] = 2; s.minutes_by_class[2] = 1;
    s.minutes_by_class[3] = 34; s.minutes_by_class[4] = 90;
    s.minutes_sitting = 20; s.minutes_standing = 12; s.minutes_still_unknown = 2;
    s.n_sit_to_stand = 3; s.n_stand_to_sit = 4; s.n_bed_transfer = 0;
    s.sts_seconds_x10 = 25; s.steps = 412; s.cadence_x10 = 1010; s.sit_tilt_x10 = 145;
    kneil_model_on_summary(&kneil_demo, &s);
    kneil_model_on_info(&kneil_demo, "library 2.0; model V4half 4s; 8 classes; window 4 s");
    kneil_demo.last_live_us = esp_timer_get_time() - 1000000;
}

void ble_service_status(ble_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->enabled = settings.ble_enabled;
    out->mode = settings.ble_mode;
    out->phone_connected = (sc == SC_ALERT);
    out->advertising = true;
    out->sensor_connected = out->sensor_receiving = (sc == SC_LIVE);
    out->sensor_scanning = (sc != SC_LIVE);
    out->sensor_frames = 5321;
    strcpy(out->sensor_name, "GuderSensor-01");
}

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

/* Click a button by its label, as a finger would (used by the KNEIL scenario). */
static lv_obj_t *find_button(lv_obj_t *root, const char *text)
{
    const uint32_t n = lv_obj_get_child_count(root);

    for(uint32_t i = 0; i < n; i++)
    {
        lv_obj_t *c = lv_obj_get_child(root, i);

        if(lv_obj_check_type(c, &lv_button_class))
        {
            lv_obj_t *lbl = lv_obj_get_child(c, 0);

            if(lbl != NULL && lv_obj_check_type(lbl, &lv_label_class) &&
               strcmp(lv_label_get_text(lbl), text) == 0)
            {
                return c;
            }
        }

        lv_obj_t *r = find_button(c, text);

        if(r != NULL)
        {
            return r;
        }
    }

    return NULL;
}

static void click_button(lv_obj_t *root, const char *text)
{
    lv_obj_t *b = find_button(root, text);

    if(b != NULL)
    {
        lv_obj_send_event(b, LV_EVENT_CLICKED, NULL);
    }
    else
    {
        fprintf(stderr, "button '%s' not found\n", text);
    }
}

/* 32-bit top-down BMP; LVGL's XRGB8888 is B,G,R,X in memory like BMP. */static void save(const char *dir, const char *name)
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
        fprintf(stderr, "usage: %s live|alert|demo|uncal|nodata|kneil|therapy OUTDIR\n", argv[0]);
        return 2;
    }

    const char *names[] = { "live", "alert", "demo", "uncal", "nodata", "kneil", "therapy" };

    for(int i = 0; i < 7; i++)
    {
        if(strcmp(argv[1], names[i]) == 0) sc = (scenario_t)i;
    }

    /* mingw (the Windows host build) has no setenv(); _putenv_s() does the same here. */
#ifdef _WIN32
    _putenv_s("TZ", "GMT0BST,M3.5.0/1,M10.5.0");
#else
    setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0", 1);
#endif
    tzset();

    if(sc == SC_KNEIL)
    {
        settings.ble_mode = BLE_MODE_KNEIL;   /* page 1 becomes the KNEIL monitor */
        kneil_demo_fill();
    }

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

    if(sc == SC_KNEIL)
    {
        /* the other link states of the KNEIL page */
        kneil_model_set_link(&kneil_demo, KNEIL_LINK_SCANNING, "", 0, 0);
        run_ms(600);
        save(out, "07_kneil_scanning");

        kneil_model_set_link(&kneil_demo, KNEIL_LINK_CONNECTED, "KNEIL-7AD1", -48, 247);
        kneil_demo.last_live_us = esp_timer_get_time() - 1000000;
        run_ms(600);
        save(out, "08_kneil_connected");

        /* Reset day -> confirmation overlay on the top layer */
        click_button(ui_tiles[UI_PAGE_FACE], "Reset day");
        run_ms(400);
        save(out, "09_kneil_reset_dialog");
        click_button(lv_layer_top(), "Cancel");
        run_ms(400);

        /* Settings -> Mode cycles at run time: page 1 is rebuilt as the watch face and back */
        settings.ble_mode = BLE_MODE_PHONE;
        run_ms(800);
        save(out, "10_mode_phone_face");

        settings.ble_mode = BLE_MODE_KNEIL;
        run_ms(800);
        save(out, "11_mode_kneil_again");
    }

    if(sc == SC_THERAPY)
    {
        therapy_snapshot_t t;

        therapy_sim_init();
        memset(&t, 0, sizeof(t));
        t.state = THERAPY_STATE_IDLE;
        t.exercise = THERAPY_EX_EXTENSION;
        t.phase = THERAPY_PHASE_IDLE;
        t.set = 1;
        t.rep = 1;
        t.target = THERAPY_EXT_TARGET_DEG;
        t.phi = 90.0f;
        t.angle_valid = true;
        t.calibrated = true;
        t.orientation_ok = true;
        t.link_ok = true;
        t.audio_ready = true;
        t.log_ready = true;
        t.best_extension = 90.0f;
        t.best_flexion = 90.0f;
        therapy_sim_set(&t);

        ui_therapy_open();
        run_ms(800);
        save(out, "12_therapy_start");

        t.state = THERAPY_STATE_RUNNING;
        t.phase = THERAPY_PHASE_HOLD;
        t.set = 1;
        t.rep = 3;
        t.reps_done = 2;
        t.full_total = 2;
        t.phi = 11.0f;
        t.target = THERAPY_EXT_TARGET_DEG;
        t.hold_s = 3.0f;
        t.best_extension = 10.0f;
        t.best_flexion = 92.0f;
        therapy_sim_set(&t);
        run_ms(600);
        save(out, "13_therapy_rep");

        t.phase = THERAPY_PHASE_SET_REST;
        t.reps_done = 10;
        t.rep = 10;
        t.full_total = 9;
        t.partial_total = 1;
        t.rest_remaining_s = 24;
        t.phi = 90.0f;
        therapy_sim_set(&t);
        run_ms(600);
        save(out, "14_therapy_set_complete");

        t.state = THERAPY_STATE_IDLE;
        t.phase = THERAPY_PHASE_IDLE;
        t.audio_ready = false;
        t.log_ready = false;
        t.straight_checked = true;
        t.straight_extension_deg = 18.0f;
        t.reps_done = 0;
        t.rep = 1;
        t.full_total = 0;
        t.partial_total = 0;
        t.phi = 90.0f;
        therapy_sim_set(&t);
        run_ms(600);
        save(out, "15_therapy_warnings");

        /* standing knee curl: third exercise, own target and 3 s hold */
        click_button(ui_scr_therapy, "Standing");
        run_ms(400);
        t.state = THERAPY_STATE_RUNNING;
        t.phase = THERAPY_PHASE_HOLD;
        t.exercise = THERAPY_EX_CURL;
        t.set = 1;
        t.rep = 4;
        t.reps_done = 3;
        t.full_total = 3;
        t.partial_total = 0;
        t.phi = 88.0f;
        t.rest = THERAPY_CURL_REST_DEG;
        t.target = THERAPY_CURL_TARGET_DEG;
        t.hold_goal = THERAPY_CURL_HOLD_SECONDS;
        t.hold_s = 2.0f;
        t.best_extension = 10.0f;
        t.best_flexion = 88.0f;
        t.audio_ready = true;
        t.log_ready = true;
        t.straight_checked = false;
        therapy_sim_set(&t);
        run_ms(600);
        save(out, "16_therapy_standing");
    }

    return 0;
}
