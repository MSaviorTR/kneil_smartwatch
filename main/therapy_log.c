#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "data_logger.h"
#include "therapy_log.h"

static const char *TAG = "therapy-log";

#define LOG_QUEUE_LEN 16
#define LOG_LINE_MAX  200

static QueueHandle_t queue;
static SemaphoreHandle_t lock;

static void write_line(const char *line)
{
    const char *root = data_logger_root();

    if(root == NULL)
    {
        return;
    }

    char path[80];
    snprintf(path, sizeof(path), "%s/gw_therapy.csv", root);

    if(lock != NULL) xSemaphoreTake(lock, portMAX_DELAY);

    FILE *f = fopen(path, "a");

    if(f != NULL)
    {
        if(ftell(f) == 0)
        {
            fputs("date,time,event,exercise,set,rep,peak_deg,hold_s,full,note\n", f);
        }

        fputs(line, f);
        fputc('\n', f);
        fclose(f);
    }

    if(lock != NULL) xSemaphoreGive(lock);
}

static void log_task(void *arg)
{
    (void)arg;

    char line[LOG_LINE_MAX];

    while(1)
    {
        if(xQueueReceive(queue, line, portMAX_DELAY) == pdTRUE)
        {
            write_line(line);
        }
    }
}

void therapy_log_init(void)
{
    if(lock == NULL)
    {
        lock = xSemaphoreCreateMutex();
    }

    if(queue == NULL)
    {
        queue = xQueueCreate(LOG_QUEUE_LEN, LOG_LINE_MAX);
    }

    if(queue != NULL && xTaskCreate(log_task, "therapy_log", 4096, NULL, 2, NULL) != pdPASS)
    {
        ESP_LOGW(TAG, "log task not started");
    }
}

static void stamp(char *out, size_t size)
{
    time_t now = time(NULL);
    struct tm tmv;

    localtime_r(&now, &tmv);
    strftime(out, size, "%Y-%m-%d,%H:%M:%S", &tmv);
}

static void enqueue(const char *line)
{
    if(queue == NULL || xQueueSend(queue, line, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "therapy log queue full, line dropped");
    }
}

void therapy_log_rep(const char *exercise, int set, int rep, float peak_deg, float hold_s, bool full)
{
    char when[24];
    char line[LOG_LINE_MAX];

    stamp(when, sizeof(when));
    snprintf(line, sizeof(line), "%s,rep,%s,%d,%d,%.1f,%.1f,%d,",
             when, exercise ? exercise : "?", set, rep, (double)peak_deg, (double)hold_s, full ? 1 : 0);
    enqueue(line);
}

void therapy_log_straight(float extension_deg, bool orientation_ok)
{
    char when[24];
    char line[LOG_LINE_MAX];

    stamp(when, sizeof(when));
    snprintf(line, sizeof(line), "%s,straight_check,-,0,0,%.1f,0,0,orientation %s",
             when, (double)extension_deg, orientation_ok ? "ok" : "NOT AS EXPECTED");
    enqueue(line);
}

bool therapy_log_available(void)
{
    const char *root = data_logger_root();

    return root != NULL && root[0] != '\0';
}

void therapy_log_session(const char *exercise, int sets_done, int full, int partial,
                         float best_extension_deg, float best_flexion_deg, const char *reason)
{
    char when[24];
    char line[LOG_LINE_MAX];

    stamp(when, sizeof(when));
    snprintf(line, sizeof(line), "%s,session,%s,0,0,0,0,0,%s; sets %d full %d partial %d; best extension %.1f; "
             "best flexion %.1f",
             when, exercise ? exercise : "?", reason ? reason : "",
             sets_done, full, partial, (double)best_extension_deg, (double)best_flexion_deg);
    enqueue(line);
}
