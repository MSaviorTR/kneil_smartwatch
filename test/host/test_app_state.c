/*
 * Unit tests for app_state.c, the data hub every reading passes through.
 * Focus: behaviour that matters once REAL sensor data arrives - sample-rate
 * independence, gaps, saturation, alert debouncing, re-calibration.
 *
 * FreeRTOS, NVS and esp_timer are replaced by host stubs (test/host/stubs);
 * time is driven by the test.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app_state.h"
#include "nvs_flash.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do {                                              \
        checks++;                                                     \
        if(!(cond)) {                                                 \
            failures++;                                               \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
        }                                                             \
    } while(0)

#define CHECK_NEAR(a, b, tol) CHECK(fabs((double)(a) - (double)(b)) <= (tol))

/* ---- stubs -------------------------------------------------------- */

static int64_t fake_us = 1000000;   /* start at t = 1 s */

int64_t esp_timer_get_time(void) { return fake_us; }

static void advance_ms(int64_t ms) { fake_us += ms * 1000; }

#define NVS_SLOTS 8
static struct { char key[16]; unsigned char data[128]; size_t len; } nvs_mem[NVS_SLOTS];

esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { memset(nvs_mem, 0, sizeof(nvs_mem)); return ESP_OK; }
esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h) { (void)ns; (void)m; *h = 1; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len)
{
    (void)h;
    for(int i = 0; i < NVS_SLOTS; i++)
    {
        if(strcmp(nvs_mem[i].key, key) == 0)
        {
            if(*len < nvs_mem[i].len) return ESP_FAIL;
            memcpy(out, nvs_mem[i].data, nvs_mem[i].len);
            *len = nvs_mem[i].len;
            return ESP_OK;
        }
    }
    return ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *val, size_t len)
{
    (void)h;
    for(int i = 0; i < NVS_SLOTS; i++)
    {
        if(nvs_mem[i].key[0] == 0 || strcmp(nvs_mem[i].key, key) == 0)
        {
            snprintf(nvs_mem[i].key, sizeof(nvs_mem[i].key), "%s", key);
            memcpy(nvs_mem[i].data, val, len);
            nvs_mem[i].len = len;
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

/* ---- helpers ------------------------------------------------------ */

/* Ideal K+ electrode used throughout: E = 100 + 59 * log10(mM). */
static const cal_fit_t K_CAL = { .slope = 59.0f, .intercept = 100.0f, .r2 = 1.0f, .n_points = 2, .valid = true };

static float k_mv_for(float mM) { return 100.0f + 59.0f * log10f(mM); }

static uint32_t seq;

static void push(float k_mv, uint32_t flags)
{
    sensor_frame_t f = { .seq = seq++, .t_ms = 0, .k_mv = k_mv, .ph_mv = NAN, .temp_c = NAN, .flags = flags };
    app_state_push_frame(&f, DATA_SRC_SENSOR);
}

/* Feeds `seconds` of data at `hz`, value from fn(t). */
static void feed(int seconds, int hz, float k_mM)
{
    for(int i = 0; i < seconds * hz; i++)
    {
        push(k_mv_for(k_mM), 0);
        advance_ms(1000 / hz);
    }
}

static int alert_on, alert_off;
static void on_alert(bool active, float k) { (void)k; if(active) alert_on++; else alert_off++; }

static float hist[1800];

/* ---- tests -------------------------------------------------------- */

static void test_rate_independent_history(void)
{
    /* 10 Hz for 30 min must still be 30 min of history, not 3 min. */
    feed(1800, 10, 4.0f);

    int have = app_state_history(hist, NULL, 1800);
    CHECK(have >= 1798);                     /* every second has data */
    CHECK_NEAR(hist[0], 4.0, 0.01);          /* oldest = 30 min ago */
    /* hist[1799] is the second in progress (may still be empty);
       hist[1798] is the last completed one. */
    CHECK_NEAR(hist[1798], 4.0, 0.01);

    /* The 30-s calibration window sees one value per second. */
    window_stats_t ws;
    CHECK(app_state_window_stats(CAL_CHANNEL_K, 30, &ws));
    CHECK(ws.n >= 29 && ws.n <= 30);
    CHECK_NEAR(ws.mean_mv, k_mv_for(4.0f), 0.01);
    CHECK_NEAR(ws.sd_mv, 0.0, 0.001);
}

static void test_gap_shows_as_gap(void)
{
    /* Link drops for 60 s: those seconds must be empty, not squashed out. */
    advance_ms(60000);
    feed(10, 1, 4.0f);

    app_state_history(hist, NULL, 120);
    int empty = 0;
    for(int i = 0; i < 120; i++) if(!isfinite(hist[i])) empty++;
    CHECK(empty >= 59 && empty <= 61);
    CHECK(isfinite(hist[118]));              /* last completed second */
}

static void test_saturation_is_not_a_reading(void)
{
    push(500.0f, SENSOR_FLAG_SATURATED);     /* clipped ADC, absurd value */

    reading_t r;
    CHECK(app_state_latest(&r));
    CHECK(isnan(r.k_mM));                    /* never converted */
    CHECK(r.flags & SENSOR_FLAG_SATURATED);

    /* ...and it must not pollute this second's history average. */
    app_state_history(hist, NULL, 1);
    CHECK(!isfinite(hist[0]) || fabs(hist[0] - 4.0) < 0.01);

    push(150.0f, SENSOR_FLAG_K_FAULT);
    CHECK(app_state_latest(&r));
    CHECK(isnan(r.k_mM));
    advance_ms(1000);
}

static void test_alert_debounce_and_hysteresis(void)
{
    /* Threshold 6.0 mM (default), hold 5 s, hysteresis 0.3 mM. */
    alert_on = alert_off = 0;

    /* A 2-second spike to 9 mM: must NOT alert. */
    feed(2, 1, 9.0f);
    feed(5, 1, 4.0f);
    CHECK(alert_on == 0);

    /* Sustained 7 mM: alerts once, after ~5 s. */
    feed(4, 1, 7.0f);
    CHECK(alert_on == 0);
    feed(3, 1, 7.0f);
    CHECK(alert_on == 1);

    /* Dips just below threshold but inside hysteresis: stays on. */
    feed(3, 1, 5.85f);
    CHECK(alert_off == 0);

    reading_t r;
    app_state_latest(&r);
    CHECK(r.k_alert);

    /* Clear below threshold - hysteresis. */
    feed(1, 1, 5.5f);
    CHECK(alert_off == 1);
    app_state_latest(&r);
    CHECK(!r.k_alert);
}

static void test_calibrating_later_fixes_history(void)
{
    /* Wipe calibration (invalid fit) and record "uncalibrated" data. */
    cal_fit_t none = { .valid = false };
    app_state_set_calibration(CAL_CHANNEL_K, &none);
    feed(20, 2, 5.0f);

    app_state_history(hist, NULL, 10);
    CHECK(isnan(hist[5]));                   /* no calibration yet */

    /* Calibrate afterwards: the same past seconds now read correctly. */
    app_state_set_calibration(CAL_CHANNEL_K, &K_CAL);
    app_state_history(hist, NULL, 10);
    CHECK_NEAR(hist[5], 5.0, 0.01);
}

static void test_demo_never_mixes_with_sensor(void)
{
    app_settings_t s;
    app_state_get_settings(&s);
    s.demo_mode = true;
    app_state_set_settings(&s);

    /* While real frames are arriving, demo frames are ignored. */
    sensor_frame_t demo = { .seq = 1, .k_mv = k_mv_for(9.0f), .ph_mv = NAN, .temp_c = NAN };
    app_state_push_frame(&demo, DATA_SRC_DEMO);

    reading_t r;
    app_state_latest(&r);
    CHECK(r.source == DATA_SRC_SENSOR);

    /* After the link times out, demo takes over and history restarts. */
    advance_ms(CONFIG_GW_LINK_TIMEOUT_MS + 1000);
    app_state_push_frame(&demo, DATA_SRC_DEMO);
    app_state_latest(&r);
    CHECK(r.source == DATA_SRC_DEMO);

    app_state_history(hist, NULL, 60);
    int with = 0;
    for(int i = 0; i < 60; i++) if(isfinite(hist[i])) with++;
    CHECK(with == 1);                        /* only the demo second */
}

static void test_dropped_frames_counted(void)
{
    link_stats_t before, after;
    app_state_link_stats(&before);

    sensor_frame_t f = { .seq = 1000, .k_mv = 150.0f, .ph_mv = NAN, .temp_c = NAN };
    app_state_push_frame(&f, DATA_SRC_SENSOR);
    f.seq = 1004;                            /* 3 frames lost */
    app_state_push_frame(&f, DATA_SRC_SENSOR);
    f.seq = 0;                               /* sensor rebooted: not "drops" */
    app_state_push_frame(&f, DATA_SRC_SENSOR);

    app_state_link_stats(&after);
    CHECK(after.frames_dropped - before.frames_dropped == 3);
}

/* Settings saved by firmware version 1 (no ble_mode) must survive an
   update instead of resetting to defaults. Same layout as in app_state.c. */
static void store_v1_settings(void)
{
    struct
    {
        uint8_t version;
        struct { bool ble_enabled, vibration_enabled, demo_mode; float k_alert_mM; uint8_t brightness; } s;
    } v1 = { .version = 1, .s = { false, false, false, 6.0f, 55 } };

    nvs_set_blob(1, "settings", &v1, sizeof(v1));
}

static void test_settings_migrated(void)
{
    app_settings_t s;
    app_state_get_settings(&s);
    CHECK(!s.ble_enabled);                 /* user's old values kept... */
    CHECK(!s.vibration_enabled);
    CHECK(s.brightness == 55);
    CHECK_NEAR(s.k_alert_mM, 6.0, 1e-6);
    CHECK(s.ble_mode == BLE_MODE_BOTH);    /* ...new field gets its default */
}

int main(void)
{
    store_v1_settings();
    CHECK(app_state_init() == ESP_OK);
    test_settings_migrated();
    CHECK(app_state_set_calibration(CAL_CHANNEL_K, &K_CAL) == ESP_OK);
    app_state_add_alert_listener(on_alert);

    test_rate_independent_history();
    test_gap_shows_as_gap();
    test_saturation_is_not_a_reading();
    test_alert_debounce_and_hysteresis();
    test_calibrating_later_fixes_history();
    test_demo_never_mixes_with_sensor();
    test_dropped_frames_counted();

    printf("app_state: %d/%d checks passed\n", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
