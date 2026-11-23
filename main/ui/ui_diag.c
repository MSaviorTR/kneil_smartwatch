#include <math.h>
#include <stdio.h>

#include "esp_timer.h"

#include "app_state.h"
#include "ble_service.h"
#include "data_logger.h"
#include "rtc_service.h"

#include "ui_internal.h"

/* One multi-line label per section keeps this screen cheap to refresh. */
static lv_obj_t *link_lbl;
static lv_obj_t *raw_lbl;
static lv_obj_t *imu_lbl;
static lv_obj_t *sys_lbl;
static lv_obj_t *ble_lbl;

static lv_obj_t *section(lv_obj_t *parent, const char *icon, const char *title)
{
    lv_obj_t *card = ui_card(parent);

    lv_obj_t *head = ui_row(card);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui_label(head, icon, &lv_font_montserrat_16, UI_COL_DIM);
    ui_label(head, title, &lv_font_montserrat_16, UI_COL_DIM);

    lv_obj_t *body = ui_label(card, "", &lv_font_montserrat_16, UI_COL_TEXT);
    lv_obj_set_width(body, LV_PCT(100));

    return body;
}

void ui_diag_create(lv_obj_t *tile)
{
    lv_obj_t *page = ui_page(UI_PAGE_DIAG, LV_SYMBOL_LIST, "DIAGNOSTICS", UI_COL_DIM);
    (void)tile;

    link_lbl = section(page, LV_SYMBOL_SHUFFLE, "SENSOR LINK");
    raw_lbl  = section(page, LV_SYMBOL_CHARGE, "RAW SIGNALS");
    imu_lbl  = section(page, LV_SYMBOL_GPS, "WATCH IMU");
    ble_lbl  = section(page, LV_SYMBOL_BLUETOOTH, "BLUETOOTH");
    sys_lbl  = section(page, LV_SYMBOL_SETTINGS, "SYSTEM");
}

void ui_diag_refresh(void)
{
    char buf[256];
    char a[16], b[16], c[16];

    /* Link */
    link_stats_t ls;
    app_state_link_stats(&ls);

    if(ls.last_frame_us == 0)
    {
        snprintf(buf, sizeof(buf), "%s - no frame received yet\nUART%d  RX GPIO%d  %d baud",
                 app_state_link_alive() ? "UP" : "DOWN",
                 CONFIG_GW_LINK_UART_NUM, CONFIG_GW_LINK_RX_GPIO, CONFIG_GW_LINK_BAUD);
    }
    else
    {
        double age = (esp_timer_get_time() - ls.last_frame_us) / 1e6;
        snprintf(buf, sizeof(buf), "%s - last frame %.1f s ago\nok %lu   dropped %lu\nchecksum err %lu   format err %lu",
                 app_state_link_alive() ? "UP" : "DOWN", age,
                 (unsigned long)ls.frames_ok, (unsigned long)ls.frames_dropped,
                 (unsigned long)ls.frames_bad_checksum, (unsigned long)ls.frames_bad_format);
    }

    lv_label_set_text(link_lbl, buf);

    /* Raw */
    reading_t r;

    if(app_state_latest(&r))
    {
        ui_fmt(a, sizeof(a), r.k_mv, 2);
        ui_fmt(b, sizeof(b), r.ph_mv, 2);
        ui_fmt(c, sizeof(c), r.temp_c, 1);
        snprintf(buf, sizeof(buf), "[%s]  seq %lu\nK+  %s mV\npH  %s mV\ntemp %s C   flags 0x%lX",
                 app_state_source_name(r.source), (unsigned long)r.seq,
                 a, b, c, (unsigned long)r.flags);
    }
    else
    {
        snprintf(buf, sizeof(buf), "no data yet");
    }

    lv_label_set_text(raw_lbl, buf);

    /* IMU */
    motion_state_t m;
    app_state_motion(&m);

    if(m.valid)
    {
        snprintf(buf, sizeof(buf), "acc  %+.2f %+.2f %+.2f g\ngyro %+.0f %+.0f %+.0f dps\n|a| %.2f g   %s",
                 (double)m.accel_g[0], (double)m.accel_g[1], (double)m.accel_g[2],
                 (double)m.gyro_dps[0], (double)m.gyro_dps[1], (double)m.gyro_dps[2],
                 (double)m.accel_mag_g, m.motion ? "MOVING" : "still");
    }
    else
    {
        snprintf(buf, sizeof(buf), "IMU not available");
    }

    lv_label_set_text(imu_lbl, buf);

    /* System */
    power_status_t p = {0};
    bool have_p = app_state_power(&p);

    logger_status_t lg;
    data_logger_status(&lg);

    static const char *store[] = { "none", "SD card", "flash" };
    static const char *tsrc[]  = { "NOT SET", "RTC", "user" };

    char batt[48];

    if(have_p && p.percent >= 0)
    {
        snprintf(batt, sizeof(batt), "%d%%  %u mV  %s", p.percent, (unsigned)p.voltage_mv,
                 p.charging ? "charging" : p.usb_connected ? "USB" : "on battery");
    }
    else
    {
        snprintf(batt, sizeof(batt), have_p && p.usb_connected ? "no cell, USB" : "unknown");
    }

    snprintf(buf, sizeof(buf), "battery %s\nlog %s  %s\nrows %lu  free %llu KB%s\nclock %s",
             batt, store[lg.store], lg.file[0] ? lg.file : "-",
             (unsigned long)lg.rows, (unsigned long long)(lg.free_bytes / 1024),
             lg.full ? "  FULL" : lg.error ? "  ERROR" : "",
             tsrc[rtc_service_time_source()]);

    lv_label_set_text(sys_lbl, buf);

    /* Bluetooth: both roles */
    ble_status_t bs;
    ble_service_status(&bs);

    static const char *const mode_txt[BLE_MODE_COUNT] = { "phone only", "sensor only", "phone + sensor",
                                                          "KNEIL device" };
    _Static_assert(sizeof(mode_txt) / sizeof(mode_txt[0]) == BLE_MODE_COUNT,
                   "mode_txt must have one name per ble_mode_t");
    bool phone_role  = bs.enabled && bs.mode != BLE_MODE_SENSOR;
    bool sensor_role = bs.enabled && bs.mode != BLE_MODE_PHONE;
    char sensor[64];

    if(!sensor_role)               snprintf(sensor, sizeof(sensor), "off");
    else if(bs.sensor_receiving)    snprintf(sensor, sizeof(sensor), "%s, %lu frames",
                                            bs.sensor_name[0] ? bs.sensor_name : "receiving",
                                            (unsigned long)bs.sensor_frames);
    else if(bs.sensor_connected)    snprintf(sensor, sizeof(sensor), "connected, setting up");
    else if(bs.sensor_scanning)     snprintf(sensor, sizeof(sensor), "searching...");
    else                           snprintf(sensor, sizeof(sensor), "waiting to retry");

    snprintf(buf, sizeof(buf), "mode %s\nphone (out): %s\nsensor (in): %s",
             bs.enabled ? mode_txt[bs.mode % BLE_MODE_COUNT] : "Bluetooth off",
             !phone_role ? "off" : bs.phone_connected ? "connected" :
             bs.advertising ? "advertising" : "idle",
             sensor);

    lv_label_set_text(ble_lbl, buf);
}
