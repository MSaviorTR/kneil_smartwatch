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

/* History is kept per second, whatever rate the front end samples at:
   one slot per second for 30 min, direct-mapped by (second % HISTORY_S).
   Lives in PSRAM. */
#define HISTORY_S          1800
#define MAX_LISTENERS      4
#define DEMO_STALE_MS      3000
#define SEQ_RESTART_GAP    1000   /* bigger jumps = sensor MCU rebooted */

#define NVS_NAMESPACE      "gw"
#define NVS_KEY_SETTINGS   "settings"
#define SETTINGS_VERSION   2   /* 2: added ble_mode */
#define CAL_VERSION        1

static const char *CAL_KEYS[CAL_CHANNEL_COUNT] = { "cal_k", "cal_ph" };

/* Demo data is generated against this, so the demo never depends on
   whatever calibration happens to be stored from the bench. */
static const cal_fit_t DEMO_CAL[CAL_CHANNEL_COUNT] = {
    [CAL_CHANNEL_K]  = { .slope =  58.0f, .intercept = 120.0f, .r2 = 1.0f, .n_points = 2, .valid = true },
    [CAL_CHANNEL_PH] = { .slope = -57.5f, .intercept = 410.0f, .r2 = 1.0f, .n_points = 2, .valid = true },
};

/* Raw millivolts only. Concentrations are worked out on read with the
   calibration in force at that moment, so calibrating after a run
   immediately re-renders the past 30 min correctly. */
typedef struct
{
    int32_t  sec;          /* uptime second this slot holds; -1 = empty */
    float    k_mv_sum;
    float    ph_mv_sum;
    uint16_t k_n;
    uint16_t ph_n;
} hist_slot_t;

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
static hist_slot_t   *history;

static link_stats_t   link;
static bool           have_seq;
static uint32_t       last_seq;

static motion_state_t motion;
static power_status_t power;
static bool           have_power;

static bool           alert_active;
static int64_t        above_since_ms = -1;   /* K+ continuously >= threshold since */

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

static int32_t uptime_s(void)
{
    return (int32_t)(esp_timer_get_time() / 1000000);
}

static void history_clear(void)
{
    for(int i = 0; i < HISTORY_S; i++)
    {
        history[i].sec = -1;
    }
}

/* Validity of one channel in a frame: present, finite, not faulted, and
   the front end not clipping (a saturated ADC gives a meaningless value). */
static bool channel_ok(float mv, uint32_t flags, uint32_t fault_flag)
{
    return isfinite(mv) && !(flags & (fault_flag | SENSOR_FLAG_SATURATED));
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
    s->ble_mode          = BLE_MODE_BOTH;
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

        if(settings.ble_mode >= BLE_MODE_COUNT)
        {
            settings.ble_mode = BLE_MODE_BOTH;
        }
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

    history = heap_caps_calloc(HISTORY_S, sizeof(hist_slot_t),
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if(lock == NULL || history == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    history_clear();

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
        history_clear();
        history_source = source;
        alert_active = false;
        above_since_ms = -1;
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

    bool k_ok  = channel_ok(frame->k_mv, frame->flags, SENSOR_FLAG_K_FAULT);
    bool ph_ok = channel_ok(frame->ph_mv, frame->flags, SENSOR_FLAG_PH_FAULT);

    r.k_mM = k_ok  ? cal_k_mv_to_mM(&fits[CAL_CHANNEL_K], frame->k_mv) : NAN;
    r.ph   = ph_ok ? cal_mv_to_x(&fits[CAL_CHANNEL_PH], frame->ph_mv) : NAN;

    r.accel_mag_g = motion.valid ? motion.accel_mag_g : NAN;
    r.motion      = motion.valid && motion.motion;
    r.battery_pct = have_power ? power.percent : -1;

    /* Alert rules, tuned for real electrodes rather than clean demo data:
       - it fires only after K+ has stayed at/above the threshold for
         GW_K_ALERT_HOLD_S, so a single spike (cable movement, ADC glitch)
         does not buzz the wrist;
       - it clears below threshold minus hysteresis, so noise around the
         threshold does not toggle it;
       - an unknown concentration never changes the state. */
    if(isfinite(r.k_mM))
    {
        float hyst = CONFIG_GW_K_ALERT_HYSTERESIS_CENTI_MM / 100.0f;

        if(r.k_mM >= settings.k_alert_mM)
        {
            if(above_since_ms < 0)
            {
                above_since_ms = r.uptime_ms;
            }

            if(!alert_active &&
               r.uptime_ms - above_since_ms >= (int64_t)CONFIG_GW_K_ALERT_HOLD_S * 1000)
            {
                alert_active = true;
                alert_edge = true;
            }
        }
        else
        {
            above_since_ms = -1;

            if(alert_active && r.k_mM < settings.k_alert_mM - hyst)
            {
                alert_active = false;
                alert_edge = true;
            }
        }
    }

    r.k_alert = alert_active;

    latest = r;
    have_latest = true;

    /* Accumulate into this second's slot (reset if it held an older second). */
    int32_t sec = (int32_t)(r.uptime_ms / 1000);
    hist_slot_t *slot = &history[sec % HISTORY_S];

    if(slot->sec != sec)
    {
        *slot = (hist_slot_t){ .sec = sec };
    }

    if(k_ok && slot->k_n < UINT16_MAX)
    {
        slot->k_mv_sum += frame->k_mv;
        slot->k_n++;
    }

    if(ph_ok && slot->ph_n < UINT16_MAX)
    {
        slot->ph_mv_sum += frame->ph_mv;
        slot->ph_n++;
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

int app_state_history(float *k_mM, float *ph, int seconds)
{
    if(seconds > HISTORY_S)
    {
        seconds = HISTORY_S;
    }

    xSemaphoreTake(lock, portMAX_DELAY);

    const cal_fit_t *fits = (history_source == DATA_SRC_DEMO) ? DEMO_CAL : cal;
    int32_t now = uptime_s();
    int with_data = 0;

    for(int i = 0; i < seconds; i++)
    {
        int32_t sec = now - (seconds - 1) + i;
        const hist_slot_t *slot = (sec >= 0) ? &history[sec % HISTORY_S] : NULL;
        bool here = slot != NULL && slot->sec == sec;

        float k = (here && slot->k_n) ? cal_k_mv_to_mM(&fits[CAL_CHANNEL_K], slot->k_mv_sum / slot->k_n) : NAN;
        float p = (here && slot->ph_n) ? cal_mv_to_x(&fits[CAL_CHANNEL_PH], slot->ph_mv_sum / slot->ph_n) : NAN;

        if(k_mM) k_mM[i] = k;
        if(ph)   ph[i]   = p;

        if(here && (slot->k_n || slot->ph_n))
        {
            with_data++;
        }
    }

    xSemaphoreGive(lock);
    return with_data;
}

bool app_state_window_stats(cal_channel_t ch, int window_s, window_stats_t *out)
{
    if(window_s > HISTORY_S)
    {
        window_s = HISTORY_S;
    }

    xSemaphoreTake(lock, portMAX_DELAY);

    int32_t now = uptime_s();
    double sum = 0.0;
    double sum2 = 0.0;
    int n = 0;
    float newest = NAN;
    float oldest = NAN;

    /* Oldest to newest over the window, one value per second (the mean of
       that second), so stability does not depend on the sample rate. */
    for(int32_t sec = now - window_s + 1; sec <= now; sec++)
    {
        if(sec < 0)
        {
            continue;
        }

        const hist_slot_t *slot = &history[sec % HISTORY_S];
        uint16_t cnt = (ch == CAL_CHANNEL_K) ? slot->k_n : slot->ph_n;

        if(slot->sec != sec || cnt == 0)
        {
            continue;
        }

        float v = ((ch == CAL_CHANNEL_K) ? slot->k_mv_sum : slot->ph_mv_sum) / cnt;

        if(n == 0)
        {
            oldest = v;
        }

        newest = v;
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
