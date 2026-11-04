#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "app_state.h"
#include "haptics.h"

static const char *TAG = "haptics";

#define MOTOR_GPIO GPIO_NUM_18

/* Pattern: on/off durations in ms, played by a one-shot timer chain. */
static const uint16_t TAP[]   = { 60 };
static const uint16_t ALERT[] = { 400, 200, 400, 200, 400 };

static esp_timer_handle_t timer;
static const uint16_t    *pattern;
static int                pattern_len;
static int                step;

static void advance(void *arg)
{
    (void)arg;

    if(step >= pattern_len)
    {
        gpio_set_level(MOTOR_GPIO, 0);
        pattern = NULL;
        return;
    }

    /* Even steps are "on", odd steps are "off". */
    gpio_set_level(MOTOR_GPIO, (step % 2) == 0);
    esp_timer_start_once(timer, (uint64_t)pattern[step] * 1000);
    step++;
}

static void play(const uint16_t *p, int len)
{
    app_settings_t s;
    app_state_get_settings(&s);

    if(!s.vibration_enabled || timer == NULL)
    {
        return;
    }

    /* Kick the timer instead of calling advance() here, so every GPIO
       change happens on the esp_timer task, never from two tasks at once. */
    esp_timer_stop(timer);
    pattern = p;
    pattern_len = len;
    step = 0;
    esp_timer_start_once(timer, 1);
}

static void on_alert(bool active, float k_mM)
{
    if(active)
    {
        ESP_LOGW(TAG, "K+ alert: %.2f mM", (double)k_mM);
        haptics_alert();
    }
}

esp_err_t haptics_init(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << MOTOR_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
    };

    esp_err_t err = gpio_config(&io);

    if(err != ESP_OK)
    {
        return err;
    }

    gpio_set_level(MOTOR_GPIO, 0);

    const esp_timer_create_args_t args = {
        .callback = advance,
        .name     = "haptics",
    };

    err = esp_timer_create(&args, &timer);

    if(err == ESP_OK)
    {
        app_state_add_alert_listener(on_alert);
    }

    return err;
}

void haptics_tap(void)
{
    play(TAP, sizeof(TAP) / sizeof(TAP[0]));
}

void haptics_alert(void)
{
    play(ALERT, sizeof(ALERT) / sizeof(ALERT[0]));
}
