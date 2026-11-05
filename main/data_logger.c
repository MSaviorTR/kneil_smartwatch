#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"

#include "bsp/esp-bsp.h"

#include "app_state.h"
#include "data_logger.h"
#include "rtc_service.h"

static const char *TAG = "logger";

#define QUEUE_LEN          32
#define SPACE_CHECK_MS     60000
#define FSYNC_EVERY_MS     30000
#define CAL_FILE           "gw_cal.csv"

static const char *CSV_HEADER =
    "local_time,epoch_ms,uptime_ms,source,seq,n,"
    "k_mV,k_mM,ph_mV,pH,temp_C,flags,accel_g,motion,battery_pct,k_alert,time_valid\n";

static const char *CAL_HEADER =
    "local_time,epoch_ms,channel,n_points,slope_mV,intercept_mV,r2,slope_pct,points\n";

typedef enum { MSG_READING, MSG_CAL, MSG_CLEAR } msg_type_t;

typedef struct
{
    msg_type_t type;
    union
    {
        reading_t reading;
        struct
        {
            cal_channel_t ch;
            cal_fit_t     fit;
            cal_point_t   pts[CAL_MAX_POINTS];
            int           n;
        } cal;
    };
} log_msg_t;

/* Running average of everything that arrives within one period. */
typedef struct
{
    bool          open;
    data_source_t source;
    int64_t       first_uptime_ms;
    reading_t     last;
    int           n;
    double        sum[7];
    int           cnt[7];
    uint32_t      flags;
    bool          motion;
    bool          alert;
} bucket_t;

enum { F_K_MV, F_K_MM, F_PH_MV, F_PH, F_TEMP, F_ACCEL, F_COUNT };

static QueueHandle_t     queue;
static SemaphoreHandle_t status_lock;
static logger_status_t   status;
static const char       *root;

static FILE   *file;
static char    file_path[64];
static int     period_ms = 1000;
static bucket_t bucket;

/* ------------------------------------------------------------------ */

static void set_status_file(const char *name)
{
    xSemaphoreTake(status_lock, portMAX_DELAY);
    strlcpy(status.file, name, sizeof(status.file));
    xSemaphoreGive(status_lock);
}

static void refresh_space(void)
{
    uint64_t total = 0;
    uint64_t free_b = 0;

    if(status.store == LOG_STORE_SD)
    {
        esp_vfs_fat_info(root, &total, &free_b);
    }
    else if(status.store == LOG_STORE_FLASH)
    {
        size_t t = 0;
        size_t used = 0;

        if(esp_spiffs_info(CONFIG_BSP_SPIFFS_PARTITION_LABEL, &t, &used) == ESP_OK)
        {
            total = t;
            free_b = (t > used) ? t - used : 0;
        }
    }

    xSemaphoreTake(status_lock, portMAX_DELAY);
    status.total_bytes = total;
    status.free_bytes  = free_b;
    status.full        = total > 0 && free_b < (uint64_t)CONFIG_GW_LOG_MIN_FREE_KB * 1024;
    xSemaphoreGive(status_lock);
}

static bool mount_storage(void)
{
    if(bsp_sdcard_mount() == ESP_OK)
    {
        root = CONFIG_BSP_SD_MOUNT_POINT;
        status.store = LOG_STORE_SD;
        period_ms = 1000;
        ESP_LOGI(TAG, "logging to SD card (%s), every sample", root);
        return true;
    }

    ESP_LOGI(TAG, "no SD card - using internal flash (first boot formats it, may take a while)");

    if(bsp_spiffs_mount() == ESP_OK)
    {
        root = CONFIG_BSP_SPIFFS_MOUNT_POINT;
        status.store = LOG_STORE_FLASH;
        period_ms = CONFIG_GW_LOG_FLASH_PERIOD_S * 1000;
        ESP_LOGI(TAG, "logging to internal flash (%s), %d s averages", root,
                 CONFIG_GW_LOG_FLASH_PERIOD_S);
        return true;
    }

    ESP_LOGE(TAG, "no storage available - logging disabled");
    return false;
}

static void format_local_time(int64_t epoch_ms, char *buf, size_t len)
{
    time_t s = (time_t)(epoch_ms / 1000);
    struct tm t;

    localtime_r(&s, &t);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", &t);
}

static void wanted_file_name(char *buf, size_t len)
{
    if(!rtc_service_time_valid())
    {
        strlcpy(buf, "gw_nodate.csv", len);
        return;
    }

    time_t now = time(NULL);
    struct tm t;

    localtime_r(&now, &t);
    strftime(buf, len, "gw_%Y%m%d.csv", &t);
}

/* Opens (creating with a header if needed) the file for today. */
static FILE *open_with_header(const char *path, const char *header)
{
    struct stat st;
    bool exists = (stat(path, &st) == 0 && st.st_size > 0);

    FILE *f = fopen(path, "a");

    if(f != NULL && !exists)
    {
        fputs(header, f);
    }

    return f;
}

static bool ensure_data_file(void)
{
    char name[32];
    wanted_file_name(name, sizeof(name));

    char path[64];
    snprintf(path, sizeof(path), "%s/%s", root, name);

    if(file != NULL && strcmp(path, file_path) == 0)
    {
        return true;
    }

    if(file != NULL)
    {
        fclose(file);
        file = NULL;
    }

    file = open_with_header(path, CSV_HEADER);

    if(file == NULL)
    {
        ESP_LOGE(TAG, "cannot open %s", path);
        return false;
    }

    strlcpy(file_path, path, sizeof(file_path));
    set_status_file(name);
    ESP_LOGI(TAG, "writing %s", path);

    return true;
}

static void put_float(char **p, char *end, double sum, int cnt, const char *fmt)
{
    int n = (cnt > 0) ? snprintf(*p, (size_t)(end - *p), fmt, sum / cnt)
                      : snprintf(*p, (size_t)(end - *p), ",");

    if(n > 0 && *p + n < end)
    {
        *p += n;
    }
}

static void flush_bucket(void)
{
    if(!bucket.open || bucket.n == 0)
    {
        bucket.open = false;
        return;
    }

    bucket.open = false;

    xSemaphoreTake(status_lock, portMAX_DELAY);
    bool full = status.full;
    xSemaphoreGive(status_lock);

    if(full || !ensure_data_file())
    {
        return;
    }

    const reading_t *r = &bucket.last;
    char ts[24];
    char line[256];
    char *p = line;
    char *end = line + sizeof(line);

    format_local_time(r->epoch_ms, ts, sizeof(ts));

    p += snprintf(p, (size_t)(end - p), "%s,%lld,%lld,%s,%lu,%d",
                  ts, (long long)r->epoch_ms, (long long)r->uptime_ms,
                  app_state_source_name(bucket.source),
                  (unsigned long)r->seq, bucket.n);

    put_float(&p, end, bucket.sum[F_K_MV],  bucket.cnt[F_K_MV],  ",%.2f");
    put_float(&p, end, bucket.sum[F_K_MM],  bucket.cnt[F_K_MM],  ",%.4f");
    put_float(&p, end, bucket.sum[F_PH_MV], bucket.cnt[F_PH_MV], ",%.2f");
    put_float(&p, end, bucket.sum[F_PH],    bucket.cnt[F_PH],    ",%.3f");
    put_float(&p, end, bucket.sum[F_TEMP],  bucket.cnt[F_TEMP],  ",%.2f");

    p += snprintf(p, (size_t)(end - p), ",%lX", (unsigned long)bucket.flags);

    put_float(&p, end, bucket.sum[F_ACCEL], bucket.cnt[F_ACCEL], ",%.3f");

    snprintf(p, (size_t)(end - p), ",%d,%d,%d,%d\n",
             bucket.motion, r->battery_pct, bucket.alert, rtc_service_time_valid());

    bool ok = fputs(line, file) >= 0 && fflush(file) == 0;

    xSemaphoreTake(status_lock, portMAX_DELAY);
    status.error = !ok;

    if(ok)
    {
        status.rows++;
    }

    xSemaphoreGive(status_lock);

    if(!ok)
    {
        ESP_LOGE(TAG, "write failed on %s", file_path);
        fclose(file);
        file = NULL;   /* reopened on the next row */
    }
}

static void add_to_bucket(const reading_t *r)
{
    bool new_period = bucket.open &&
                      (r->source != bucket.source ||
                       r->uptime_ms - bucket.first_uptime_ms >= period_ms);

    if(new_period)
    {
        flush_bucket();
    }

    if(!bucket.open)
    {
        memset(&bucket, 0, sizeof(bucket));
        bucket.open = true;
        bucket.source = r->source;
        bucket.first_uptime_ms = r->uptime_ms;
    }

    const float v[F_COUNT] = { r->k_mv, r->k_mM, r->ph_mv, r->ph, r->temp_c, r->accel_mag_g };

    for(int i = 0; i < F_COUNT; i++)
    {
        if(isfinite(v[i]))
        {
            bucket.sum[i] += v[i];
            bucket.cnt[i]++;
        }
    }

    bucket.n++;
    bucket.flags  |= r->flags;
    bucket.motion |= r->motion;
    bucket.alert  |= r->k_alert;
    bucket.last    = *r;
}

static void write_calibration(const log_msg_t *m)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", root, CAL_FILE);

    FILE *f = open_with_header(path, CAL_HEADER);

    if(f == NULL)
    {
        ESP_LOGE(TAG, "cannot open %s", path);
        return;
    }

    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t now_ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;

    char ts[24];
    format_local_time(now_ms, ts, sizeof(ts));

    fprintf(f, "%s,%lld,%s,%d,%.3f,%.3f,%.5f,%.1f,",
            ts, (long long)now_ms, m->cal.ch == CAL_CHANNEL_K ? "K" : "pH",
            m->cal.n, (double)m->cal.fit.slope, (double)m->cal.fit.intercept,
            (double)m->cal.fit.r2, (double)cal_slope_percent(&m->cal.fit));

    /* x is log10(mM) for K+, pH for pH; written as "x:mV" pairs. */
    for(int i = 0; i < m->cal.n; i++)
    {
        fprintf(f, "%s%.4f:%.2f", i ? ";" : "", (double)m->cal.pts[i].x, (double)m->cal.pts[i].mv);
    }

    fputs("\n", f);
    fclose(f);
}

static void clear_logs(void)
{
    flush_bucket();

    if(file != NULL)
    {
        fclose(file);
        file = NULL;
        file_path[0] = '\0';
    }

    DIR *d = opendir(root);

    if(d == NULL)
    {
        return;
    }

    int removed = 0;
    struct dirent *e;
    char path[300];

    while((e = readdir(d)) != NULL)
    {
        size_t len = strlen(e->d_name);

        if(strncmp(e->d_name, "gw_", 3) == 0 && len > 4 &&
           strcasecmp(e->d_name + len - 4, ".csv") == 0)
        {
            snprintf(path, sizeof(path), "%s/%s", root, e->d_name);

            if(unlink(path) == 0)
            {
                removed++;
            }
        }
    }

    closedir(d);

    xSemaphoreTake(status_lock, portMAX_DELAY);
    status.rows = 0;
    status.file[0] = '\0';
    xSemaphoreGive(status_lock);

    ESP_LOGI(TAG, "cleared %d log file(s)", removed);
}

static void logger_task(void *arg)
{
    (void)arg;

    if(!mount_storage())
    {
        /* Drain and drop so producers never block. */
        log_msg_t m;
        for(;;) xQueueReceive(queue, &m, portMAX_DELAY);
    }

    refresh_space();

    int64_t last_space = esp_timer_get_time() / 1000;
    int64_t last_sync = last_space;
    log_msg_t m;

    for(;;)
    {
        bool got = xQueueReceive(queue, &m, pdMS_TO_TICKS(1000)) == pdTRUE;
        int64_t now = esp_timer_get_time() / 1000;

        if(got)
        {
            switch(m.type)
            {
                case MSG_READING: add_to_bucket(&m.reading); break;
                case MSG_CAL:     write_calibration(&m);    break;
                case MSG_CLEAR:   clear_logs(); refresh_space(); break;
            }
        }

        /* Close out a period when data stops arriving, so the final
           average before a link drop is not held back indefinitely. */
        if(bucket.open && now - bucket.first_uptime_ms >= 2 * period_ms)
        {
            flush_bucket();
        }

        if(file != NULL && now - last_sync >= FSYNC_EVERY_MS)
        {
            fsync(fileno(file));
            last_sync = now;
        }

        if(now - last_space >= SPACE_CHECK_MS)
        {
            refresh_space();
            last_space = now;
        }
    }
}

static void on_reading(const reading_t *r)
{
    log_msg_t m = { .type = MSG_READING, .reading = *r };

    /* Never block the sensor path; a full queue drops the reading, and
       that is counted so data loss is visible in `status`. */
    if(xQueueSend(queue, &m, 0) != pdTRUE)
    {
        xSemaphoreTake(status_lock, portMAX_DELAY);
        status.dropped++;
        xSemaphoreGive(status_lock);
    }
}

/* ------------------------------------------------------------------ */

esp_err_t data_logger_start(void)
{
    status_lock = xSemaphoreCreateMutex();
    queue = xQueueCreate(QUEUE_LEN, sizeof(log_msg_t));

    if(status_lock == NULL || queue == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    /* SD/FAT and SPIFFS calls need a fair amount of stack. */
    if(xTaskCreate(logger_task, "logger", 6144, NULL, 3, NULL) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }

    app_state_add_listener(on_reading);
    return ESP_OK;
}

void data_logger_status(logger_status_t *out)
{
    xSemaphoreTake(status_lock, portMAX_DELAY);
    *out = status;
    xSemaphoreGive(status_lock);
}

const char *data_logger_root(void)
{
    return root;
}

void data_logger_log_calibration(cal_channel_t ch, const cal_fit_t *fit,
                                 const cal_point_t *pts, int n)
{
    log_msg_t m = { .type = MSG_CAL };

    m.cal.ch  = ch;
    m.cal.fit = *fit;
    m.cal.n   = (n > CAL_MAX_POINTS) ? CAL_MAX_POINTS : n;
    memcpy(m.cal.pts, pts, sizeof(cal_point_t) * (size_t)m.cal.n);

    xQueueSend(queue, &m, pdMS_TO_TICKS(100));
}

void data_logger_clear(void)
{
    log_msg_t m = { .type = MSG_CLEAR };
    xQueueSend(queue, &m, pdMS_TO_TICKS(100));
}
