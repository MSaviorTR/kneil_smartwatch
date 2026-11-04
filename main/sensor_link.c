#include <math.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "app_state.h"
#include "sensor_link.h"
#include "sensor_protocol.h"

static const char *TAG = "link";

#define UART_RX_BUF   2048
#define READ_CHUNK    128

/* ------------------------------------------------------------------
   UART receiver
   ------------------------------------------------------------------ */

static void link_task(void *arg)
{
    (void)arg;

    sensor_line_buf_t lb = {0};
    uint8_t chunk[READ_CHUNK];

    for(;;)
    {
        int n = uart_read_bytes(CONFIG_GW_LINK_UART_NUM, chunk, sizeof(chunk),
                                pdMS_TO_TICKS(100));

        for(int i = 0; i < n; i++)
        {
            if(!sensor_line_feed(&lb, (char)chunk[i]))
            {
                continue;
            }

            sensor_frame_t frame;
            sensor_parse_result_t res = sensor_protocol_parse(lb.buf, &frame);

            switch(res)
            {
                case SENSOR_PARSE_OK:
                    app_state_push_frame(&frame, DATA_SRC_SENSOR);
                    break;

                case SENSOR_PARSE_ERR_CHECKSUM:
                    app_state_count_bad_frame(true);
                    ESP_LOGD(TAG, "checksum error: %s", lb.buf);
                    break;

                case SENSOR_PARSE_ERR_FORMAT:
                    /* Lines without '$' are boot noise, not frames. */
                    if(lb.buf[0] == '$')
                    {
                        app_state_count_bad_frame(false);
                        ESP_LOGD(TAG, "bad frame: %s", lb.buf);
                    }
                    break;

                case SENSOR_PARSE_IGNORED:
                    break;
            }
        }
    }
}

esp_err_t sensor_link_start(void)
{
    const uart_config_t cfg = {
        .baud_rate  = CONFIG_GW_LINK_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(CONFIG_GW_LINK_UART_NUM, UART_RX_BUF, 0, 0, NULL, 0);

    if(err == ESP_OK)
    {
        err = uart_param_config(CONFIG_GW_LINK_UART_NUM, &cfg);
    }

    if(err == ESP_OK)
    {
        err = uart_set_pin(CONFIG_GW_LINK_UART_NUM, CONFIG_GW_LINK_TX_GPIO,
                           CONFIG_GW_LINK_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }

    if(err != ESP_OK)
    {
        ESP_LOGE(TAG, "UART%d setup failed: %s", CONFIG_GW_LINK_UART_NUM, esp_err_to_name(err));
        return err;
    }

    /* Idle-high pull-up so a disconnected RX pin reads as idle, not noise. */
    gpio_pullup_en(CONFIG_GW_LINK_RX_GPIO);

    if(xTaskCreate(link_task, "sensor_link", 6144, NULL, 6, NULL) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "listening on UART%d RX=GPIO%d TX=GPIO%d @ %d baud",
             CONFIG_GW_LINK_UART_NUM, CONFIG_GW_LINK_RX_GPIO,
             CONFIG_GW_LINK_TX_GPIO, CONFIG_GW_LINK_BAUD);

    return ESP_OK;
}

/* ------------------------------------------------------------------
   Demo generator

   Produces potentials, not concentrations, so the whole pipeline
   (calibration -> display -> alert -> log -> BLE) is exercised. The
   underlying K+ follows a slow baseline with an occasional "lysis
   event" that rises above the default alert threshold and decays.
   ------------------------------------------------------------------ */

/* Must match DEMO_CAL in app_state.c */
#define DEMO_K_SLOPE      58.0f
#define DEMO_K_INTERCEPT  120.0f
#define DEMO_PH_SLOPE    -57.5f
#define DEMO_PH_INTERCEPT 410.0f

#define DEMO_PERIOD_MS    1000
#define DEMO_EVENT_EVERY  240   /* s between simulated K+ spikes */

static float noise(float amplitude)
{
    /* Uniform in [-a, a]; plenty for a demo trace. */
    return amplitude * ((float)(esp_random() % 20001) / 10000.0f - 1.0f);
}

static void demo_task(void *arg)
{
    (void)arg;

    uint32_t seq = 0;
    TickType_t wake = xTaskGetTickCount();

    for(;;)
    {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(DEMO_PERIOD_MS));

        app_settings_t s;
        app_state_get_settings(&s);

        if(!s.demo_mode || app_state_link_alive())
        {
            continue;
        }

        float t = (float)(esp_timer_get_time() / 1000000);

        /* Baseline ~4.2 mM drifting slowly, plus a spike to ~8 mM that
           decays over ~1 min every DEMO_EVENT_EVERY seconds. */
        float phase = fmodf(t, (float)DEMO_EVENT_EVERY);
        float k_mM = 4.2f + 0.3f * sinf(t / 90.0f);

        if(phase > DEMO_EVENT_EVERY - 90)
        {
            float since = phase - (DEMO_EVENT_EVERY - 90);
            k_mM += 4.0f * (since / 15.0f < 1.0f ? since / 15.0f : expf(-(since - 15.0f) / 25.0f));
        }

        float ph = 7.38f - 0.05f * sinf(t / 120.0f) - (k_mM - 4.2f) * 0.02f;

        sensor_frame_t f = {
            .seq    = seq++,
            .t_ms   = (uint32_t)(esp_timer_get_time() / 1000),
            .k_mv   = DEMO_K_INTERCEPT + DEMO_K_SLOPE * log10f(k_mM) + noise(0.4f),
            .ph_mv  = DEMO_PH_INTERCEPT + DEMO_PH_SLOPE * ph + noise(0.4f),
            .temp_c = 36.8f + noise(0.1f),
            .flags  = 0,
        };

        app_state_push_frame(&f, DATA_SRC_DEMO);
    }
}

esp_err_t sensor_demo_start(void)
{
    if(xTaskCreate(demo_task, "sensor_demo", 6144, NULL, 4, NULL) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
