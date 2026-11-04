#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "driver/i2c_master.h"
#include "esp_log.h"

#include "bsp/esp-bsp.h"

#include "rtc_service.h"

static const char *TAG = "rtc";

/* PCF85063A on the shared I2C bus (schematic U6). */
#define PCF85063_ADDR       0x51
#define REG_CONTROL_1       0x00
#define REG_SECONDS         0x04   /* bit 7 = OS: oscillator stopped */
#define SECONDS_OS          0x80
#define CONTROL_1_12_24     0x02   /* 0 = 24 h mode */
#define CONTROL_1_STOP      0x20   /* 1 = clock halted */

#define RTC_TIMEOUT_MS      500
#define MIN_VALID_YEAR      2025

static i2c_master_dev_handle_t rtc_dev;
static time_source_t           source = TIME_SRC_UNSET;

static uint8_t bcd_to_bin(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t bin_to_bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/* timegm() is not in newlib; do the UTC conversion by hand. */
static time_t utc_mktime(const struct tm *t)
{
    static const int cum_days[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };

    int year = t->tm_year + 1900;
    int64_t days = (int64_t)(year - 1970) * 365
                 + ((year - 1969) / 4) - ((year - 1901) / 100) + ((year - 1601) / 400)
                 + cum_days[t->tm_mon] + (t->tm_mday - 1);

    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;

    if(leap && t->tm_mon > 1)
    {
        days++;
    }

    return (time_t)(days * 86400 + t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec);
}

static esp_err_t rtc_read(struct tm *out, bool *osc_stopped)
{
    uint8_t reg = REG_SECONDS;
    uint8_t d[7];

    esp_err_t err = i2c_master_transmit_receive(rtc_dev, &reg, 1, d, sizeof(d), RTC_TIMEOUT_MS);

    if(err != ESP_OK)
    {
        return err;
    }

    *osc_stopped = (d[0] & SECONDS_OS) != 0;

    memset(out, 0, sizeof(*out));
    out->tm_sec  = bcd_to_bin(d[0] & 0x7F);
    out->tm_min  = bcd_to_bin(d[1] & 0x7F);
    out->tm_hour = bcd_to_bin(d[2] & 0x3F);
    out->tm_mday = bcd_to_bin(d[3] & 0x3F);
    out->tm_wday = d[4] & 0x07;
    out->tm_mon  = bcd_to_bin(d[5] & 0x1F) - 1;
    out->tm_year = bcd_to_bin(d[6]) + 100;      /* RTC holds 2000..2099 */

    return ESP_OK;
}

static esp_err_t rtc_write(const struct tm *t)
{
    uint8_t buf[8] = {
        REG_SECONDS,
        bin_to_bcd((uint8_t)t->tm_sec),            /* also clears OS */
        bin_to_bcd((uint8_t)t->tm_min),
        bin_to_bcd((uint8_t)t->tm_hour),
        bin_to_bcd((uint8_t)t->tm_mday),
        (uint8_t)t->tm_wday,
        bin_to_bcd((uint8_t)(t->tm_mon + 1)),
        bin_to_bcd((uint8_t)(t->tm_year - 100)),
    };

    return i2c_master_transmit(rtc_dev, buf, sizeof(buf), RTC_TIMEOUT_MS);
}

static void set_system_time(time_t utc)
{
    struct timeval tv = { .tv_sec = utc, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

/* __DATE__ = "Oct 20 2026", __TIME__ = "14:05:09" (build machine local) */
static time_t build_time(void)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *date = __DATE__;
    const char *tim  = __TIME__;
    char mon[4] = { date[0], date[1], date[2], 0 };

    struct tm t = {0};
    const char *m = strstr(months, mon);

    t.tm_mon  = m ? (int)(m - months) / 3 : 0;
    t.tm_mday = atoi(date + 4);
    t.tm_year = atoi(date + 7) - 1900;
    t.tm_hour = atoi(tim);
    t.tm_min  = atoi(tim + 3);
    t.tm_sec  = atoi(tim + 6);

    return utc_mktime(&t);
}

esp_err_t rtc_service_init(void)
{
    setenv("TZ", CONFIG_GW_TIMEZONE, 1);
    tzset();

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();

    if(bus == NULL)
    {
        set_system_time(build_time());
        return ESP_FAIL;
    }

    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = PCF85063_ADDR,
        .scl_speed_hz    = 400000,
    };

    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &rtc_dev);

    if(err != ESP_OK)
    {
        set_system_time(build_time());
        return err;
    }

    /* Force 24 h mode and make sure the clock is running. Read-modify-write
       so the crystal load-cap selection (CAP_SEL) is left untouched. */
    uint8_t reg = REG_CONTROL_1;
    uint8_t ctrl1 = 0;

    if(i2c_master_transmit_receive(rtc_dev, &reg, 1, &ctrl1, 1, RTC_TIMEOUT_MS) == ESP_OK &&
       (ctrl1 & (CONTROL_1_12_24 | CONTROL_1_STOP)))
    {
        uint8_t ctrl[2] = { REG_CONTROL_1, (uint8_t)(ctrl1 & ~(CONTROL_1_12_24 | CONTROL_1_STOP)) };
        i2c_master_transmit(rtc_dev, ctrl, sizeof(ctrl), RTC_TIMEOUT_MS);
    }

    struct tm t;
    bool stopped = true;

    err = rtc_read(&t, &stopped);

    if(err == ESP_OK && !stopped && t.tm_year + 1900 >= MIN_VALID_YEAR)
    {
        set_system_time(utc_mktime(&t));
        source = TIME_SRC_RTC;

        ESP_LOGI(TAG, "time restored from RTC: %04d-%02d-%02d %02d:%02d:%02d UTC",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);

        return ESP_OK;
    }

    set_system_time(build_time());
    source = TIME_SRC_UNSET;

    ESP_LOGW(TAG, "RTC %s - clock seeded from build time, please set the time",
             err != ESP_OK ? "not responding" : "lost power");

    return err == ESP_OK ? ESP_OK : err;
}

esp_err_t rtc_service_set_epoch(time_t utc)
{
    set_system_time(utc);
    source = TIME_SRC_USER;

    if(rtc_dev == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    struct tm t;
    gmtime_r(&utc, &t);

    esp_err_t err = rtc_write(&t);

    if(err != ESP_OK)
    {
        ESP_LOGE(TAG, "RTC write failed: %s", esp_err_to_name(err));
    }
    else
    {
        ESP_LOGI(TAG, "time set to %lld (UTC)", (long long)utc);
    }

    return err;
}

time_source_t rtc_service_time_source(void)
{
    return source;
}

bool rtc_service_time_valid(void)
{
    return source != TIME_SRC_UNSET;
}
