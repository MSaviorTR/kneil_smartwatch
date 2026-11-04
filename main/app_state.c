#include <math.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "app_state.h"

static const char *TAG = "state";

/* 30 min at 1 Hz. Lives in PSRAM. */
#define HISTORY_LEN        1800
#define MAX_LISTENERS      4
#define DEMO_STALE_MS      3000
#define SEQ_RESTART_GAP    1000   /* bigger jumps = sensor MCU rebooted */

#define NVS_NAMESPACE      "gw"
#define NVS_KEY_SETTINGS   "settings"
#define SETTINGS_VERSION   1
#define CAL_VERSION        1

static const char *CAL_KEYS[CAL_CHANNEL_COUNT] = { "cal_k", "cal_ph" };

/* Demo data is generated against this, so the demo never depends on
   whatever calibration happens to be stored from the bench. */
static const cal_fit_t DEMO_CAL[CAL_CHANNEL_COUNT] = {
    [CAL_CHANNEL_K]  = { .slope =  58.0f, .intercept = 120.0f, .r2 = 1.0f, .n_points = 2, .valid = true },
    [CAL_CHANNEL_PH] = { .slope = -57.5f, .intercept = 410.0f, .r2 = 1.0f, .n_points = 2, .valid = true },
};

typedef struct
{
    int64_t uptime_ms;
    float   k_mv;
    float   ph_mv;
    float   k_mM;
    float   ph;
} hist_entry_t;

typedef struct
{
    uint8_t        version;
    app_settings_t s;
} settings_blob_t;

typedef struct
{
    uint8_t   version;
    cal_fit_t fit;
} cal_blob_t;

static SemaphoreHandle_t lock;

static app_settings_t settings;
static cal_fit_t      cal[CAL_CHANNEL_COUNT];

static reading_t      latest;
static bool           have_latest;
static data_source_t  history_source = DATA_SRC_NONE;
static hist_entry_t  *history;
static int            hist_head;   /* next write index */
static int            hist_count;

static link_stats_t   link;
static bool           have_seq;
static uint32_t       last_seq;

static motion_state_t motion;
static power_status_t power;
static bool           have_power;

static bool           alert_active;

static reading_listener_t listeners[MAX_LISTENERS];
static int                listener_count;
static alert_listener_t   alert_listeners[MAX_LISTENERS];
static int                alert_listener_count;

/* ------------------------------------------------------------------ */

static int64_t now_epoch_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static int64_t uptime_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool link_alive_locked(void)
{
    if(link.last_frame_us == 0)
    {
        return false;
    }

    return (esp_timer_get_time() - link.last_frame_us) <
           (int64_t)CONFIG_GW_LINK_TIMEOUT_MS * 1000;
}

static data_source_t active_source_locked(void)
{
    if(link_alive_locked())
    {
        return DATA_SRC_SENSOR;
    }

    if(settings.demo_mode && have_latest && latest.source == DATA_SRC_DEMO &&
       uptime_ms() - latest.uptime_ms < DEMO_STALE_MS)
    {
        return DATA_SRC_DEMO;
    }

    return DATA_SRC_NONE;
}

static void settings_defaults(app_settings_t *s)
{
    s->ble_enabled       = true;
    s->vibration_enabled = true;
    s->demo_mode         = false;
    s->k_alert_mM        = CONFIG_GW_K_ALERT_DEFAULT_CENTI_MM / 100.0f;
    s->brightness        = 80;
}

static void load_nvs(void)
{
    settings_defaults(&settings);

    for(int ch = 0; ch < CAL_CHANNEL_COUNT; ch++)
    {
        memset(&cal[ch], 0, sizeof(cal[ch]));
    }

    nvs_handle_t h;

    if(nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
    {
        ESP_LOGI(TAG, "no stored settings yet, using defaults");
        return;
    }

    settings_blob_t sb;
    size_t len = sizeof(sb);

    if(nvs_get_blob(h, NVS_KEY_SETTINGS, &sb, &len) == ESP_OK &&
       len == sizeof(sb) && sb.version == SETTINGS_VERSION)
    {
        settings = sb.s;
    }

    for(int ch = 0; ch < CAL_CHANNEL_COUNT; ch++)
    {
        cal_blob_t cb;
        len = sizeof(cb);

        if(nvs_get_blob(h, CAL_KEYS[ch], &cb, &len) == ESP_OK &&
           len == sizeof(cb) && cb.version == CAL_VERSION)
        {
            cal[ch] = cb.fit;
        }
    }

    nvs_close(h);
}

static esp_err_t save_blob(const char *key, const void *data, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);

    if(err != ESP_OK)
    {
        return err;
    }

    err = nvs_set_blob(h, key, data, len);

    if(err == ESP_OK)
    {
        err = nvs_commit(h);
    }

    nvs_close(h);
    return err;
}

/* ------------------------------------------------------------------ */

esp_err_t app_state_init(void)
{
    esp_err_t err = nvs_flash_init();

    if(err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS partition reformatted (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    if(err != ESP_OK)
    {
        return err;
    }

    lock = xSemaphoreCreateMutex();

    history = heap_caps_calloc(HISTORY_LEN, sizeof(hist_entry_t),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if(lock == NULL || history == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    load_nvs();

    motion.valid = false;
    motion.accel_mag_g = NAN;

    ESP_LOGI(TAG, "settings: ble=%d vib=%d demo=%d alert=%.2f mM, cal K=%s pH=%s",
             settings.ble_enabled, settings.vibration_enabled, settings.demo_mode,
             (double)settings.k_alert_mM,
             cal[CAL_CHANNEL_K].valid ? "yes" : "no",
             cal[CAL_CHANNEL_PH].valid ? "yes" : "no");

    return ESP_OK;
}

/* ---- producers ----------------------------------------------------- */

void app_state_push_frame(const sensor_frame_t *frame, data_source_t source)
{
    reading_t r;
    bool alert_edge = false;

    xSemaphoreTake(lock, portMAX_DELAY);

    /* Real data always wins; drop demo frames while the link is up. */
    if(source == DATA_SRC_DEMO && (link_alive_locked() || !settings.demo_mode))
    {
        xSemaphoreGive(lock);
        return;
    }

    if(source == DATA_SRC_SENSOR)
    {
        if(have_seq)
        {
            uint32_t gap = frame->seq - last_seq;   /* wraps correctly */

            if(gap > 1 && gap < SEQ_RESTART_GAP)
            {
                link.frames_dropped += gap - 1;
            }
        }

        have_seq = true;
        last_seq = frame->seq;
        link.frames_ok++;
        link.last_frame_us = esp_timer_get_time();
    }

    /* Never mix demo and real samples in one chart / calibration window,
       and never let a demo alert carry over onto real (maybe uncalibrated,
       so never re-evaluated) data. */
    if(source != history_source)
    {
        hist_head = 0;
        hist_count = 0;
        history_source = source;
        alert_active = false;
    }

    const cal_fit_t *fits = (source == DATA_SRC_DEMO) ? DEMO_CAL : cal;

    memset(&r, 0, sizeof(r));
    r.epoch_ms  = now_epoch_ms();
    r.uptime_ms = uptime_ms();
    r.source    = source;
    r.seq       = frame->seq;
    r.k_mv      = frame->k_mv;
    r.ph_mv     = frame->ph_mv;
    r.temp_c    = frame->temp_c;
    r.flags     = frame->flags;

    r.k_mM = (frame->flags & SENSOR_FLAG_K_FAULT)
           ? NAN : cal_k_mv_to_mM(&fits[CAL_CHANNEL_K], frame->k_mv);

    r.ph = (frame->flags & SENSOR_FLAG_PH_FAULT)
         ? NAN : cal_mv_to_x(&fits[CAL_CHANNEL_PH], frame->ph_mv);

    r.accel_mag_g = motion.valid ? motion.accel_mag_g : NAN;
    r.motion      = motion.valid && motion.motion;
    r.battery_pct = have_power ? power.percent : -1;

    /* Alert with hysteresis so noise around the threshold does not buzz
       the wrist every second. Unknown concentration never changes state. */
    if(isfinite(r.k_mM))
    {
        float hyst = CONFIG_GW_K_ALERT_HYSTERESIS_CENTI_MM / 100.0f;

        if(!alert_active && r.k_mM >= settings.k_alert_mM)
        {
            alert_active = true;
            alert_edge = true;
        }
        else if(alert_active && r.k_mM < settings.k_alert_mM - hyst)
        {
            alert_active = false;
            alert_edge = true;
        }
    }

    r.k_alert = alert_active;

    latest = r;
    have_latest = true;

    hist_entry_t *e = &history[hist_head];
    e->uptime_ms = r.uptime_ms;
    e->k_mv      = r.k_mv;
    e->ph_mv     = r.ph_mv;
    e->k_mM      = r.k_mM;
    e->ph        = r.ph;

    hist_head = (hist_head + 1) % HISTORY_LEN;

    if(hist_count < HISTORY_LEN)
    {
        hist_count++;
    }

    reading_listener_t ls[MAX_LISTENERS];
    alert_listener_t   als[MAX_LISTENERS];
    int nl = listener_count;
    int na = alert_listener_count;

    memcpy(ls, listeners, sizeof(ls));
    memcpy(als, alert_listeners, sizeof(als));

    xSemaphoreGive(lock);

    /* Callbacks run outside the lock so they may call back in. */
    for(int i = 0; i < nl; i++)
    {
        ls[i](&r);
    }

    if(alert_edge)
    {
        for(int i = 0; i < na; i++)
        {
            als[i](r.k_alert, r.k_mM);
        }
    }
}

void app_state_count_bad_frame(bool checksum_error)
{
    xSemaphoreTake(lock, portMAX_DELAY);

    if(checksum_error)
    {
        link.frames_bad_checksum++;
    }
    else
    {
        link.frames_bad_format++;
    }

    xSemaphoreGive(lock);
}

void app_state_set_motion(const motion_state_t *m)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    motion = *m;
    xSemaphoreGive(lock);
}

void app_state_set_power(const power_status_t *p)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    power = *p;
    have_power = true;
    xSemaphoreGive(lock);
}

/* ---- consumers ----------------------------------------------------- */

bool app_state_latest(reading_t *out)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = have_latest;

    if(ok)
    {
        *out = latest;
    }

    xSemaphoreGive(lock);
    return ok;
}

data_source_t app_state_active_source(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    data_source_t s = active_source_locked();
    xSemaphoreGive(lock);
    return s;
}

void app_state_link_stats(link_stats_t *out)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    *out = link;
    xSemaphoreGive(lock);
}

bool app_state_link_alive(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    bool alive = link_alive_locked();
    xSemaphoreGive(lock);
    return alive;
}

void app_state_motion(motion_state_t *out)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    *out = motion;
    xSemaphoreGive(lock);
}

bool app_state_power(power_status_t *out)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = have_power;

    if(ok)
    {
        *out = power;
    }

    xSemaphoreGive(lock);
    return ok;
}

int app_state_history(float *k_mM, float *ph, int max)
{
    xSemaphoreTake(lock, portMAX_DELAY);

    int n = (hist_count < max) ? hist_count : max;
    int start = (hist_head - n + HISTORY_LEN) % HISTORY_LEN;

    for(int i = 0; i < n; i++)
    {
        const hist_entry_t *e = &history[(start + i) % HISTORY_LEN];

        if(k_mM) k_mM[i] = e->k_mM;
        if(ph)   ph[i]   = e->ph;
    }

    xSemaphoreGive(lock);
    return n;
}

bool app_state_window_stats(cal_channel_t ch, int window_s, window_stats_t *out)
{
    xSemaphoreTake(lock, portMAX_DELAY);

    int64_t cutoff = uptime_ms() - (int64_t)window_s * 1000;
    double sum = 0.0;
    double sum2 = 0.0;
    int n = 0;
    float newest = NAN;
    float oldest = NAN;

    for(int i = 0; i < hist_count; i++)
    {
        const hist_entry_t *e = &history[(hist_head - 1 - i + HISTORY_LEN) % HISTORY_LEN];

        if(e->uptime_ms < cutoff)
        {
            break;
        }

        float v = (ch == CAL_CHANNEL_K) ? e->k_mv : e->ph_mv;

        if(!isfinite(v))
        {
            continue;
        }

        if(n == 0)
        {
            newest = v;
        }

        oldest = v;
        sum  += v;
        sum2 += (double)v * v;
        n++;
    }

    xSemaphoreGive(lock);

    if(n < 2)
    {
        return false;
    }

    double mean = sum / n;
    double var  = (sum2 - n * mean * mean) / (n - 1);

    out->mean_mv  = (float)mean;
    out->sd_mv    = (float)sqrt(var > 0.0 ? var : 0.0);
    out->drift_mv = newest - oldest;
    out->n        = n;

    return true;
}

void app_state_add_listener(reading_listener_t cb)
{
    xSemaphoreTake(lock, portMAX_DELAY);

    if(listener_count < MAX_LISTENERS)
    {
        listeners[listener_count++] = cb;
    }

    xSemaphoreGive(lock);
}

void app_state_add_alert_listener(alert_listener_t cb)
{
    xSemaphoreTake(lock, portMAX_DELAY);

    if(alert_listener_count < MAX_LISTENERS)
    {
        alert_listeners[alert_listener_count++] = cb;
    }

    xSemaphoreGive(lock);
}

/* ---- settings & calibration --------------------------------------- */

void app_state_get_settings(app_settings_t *out)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    *out = settings;
    xSemaphoreGive(lock);
}

esp_err_t app_state_set_settings(const app_settings_t *in)
{
    settings_blob_t sb = { .version = SETTINGS_VERSION, .s = *in };

    xSemaphoreTake(lock, portMAX_DELAY);
    settings = *in;
    xSemaphoreGive(lock);

    esp_err_t err = save_blob(NVS_KEY_SETTINGS, &sb, sizeof(sb));

    if(err != ESP_OK)
    {
        ESP_LOGE(TAG, "saving settings failed: %s", esp_err_to_name(err));
    }

    return err;
}

void app_state_get_calibration(cal_channel_t ch, cal_fit_t *out)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    *out = cal[ch];
    xSemaphoreGive(lock);
}

esp_err_t app_state_set_calibration(cal_channel_t ch, const cal_fit_t *fit)
{
    cal_blob_t cb = { .version = CAL_VERSION, .fit = *fit };

    esp_err_t err = save_blob(CAL_KEYS[ch], &cb, sizeof(cb));

    if(err != ESP_OK)
    {
        ESP_LOGE(TAG, "saving calibration failed: %s", esp_err_to_name(err));
        return err;
    }

    xSemaphoreTake(lock, portMAX_DELAY);
    cal[ch] = *fit;
    xSemaphoreGive(lock);

    return ESP_OK;
}

const char *app_state_source_name(data_source_t s)
{
    switch(s)
    {
        case DATA_SRC_SENSOR: return "sensor";
        case DATA_SRC_DEMO:   return "demo";
        default:              return "none";
    }
}
