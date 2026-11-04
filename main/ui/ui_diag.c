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

static lv_obj_t *section(lv_obj_t *parent, const char *title)
{
    lv_obj_t *card = lv_obj_create(parent);

    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, UI_COL_CARD, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 4, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    ui_label(card, title, &lv_font_montserrat_16, UI_COL_DIM);

    lv_obj_t *body = ui_label(card, "", &lv_font_montserrat_16, UI_COL_TEXT);
    lv_obj_set_width(body, LV_PCT(100));

    return body;
}

void ui_diag_create(void)
{
    ui_scr_diag = ui_screen_create("DIAGNOSTICS");

    link_lbl = section(ui_scr_diag, "SENSOR LINK");
    raw_lbl  = section(ui_scr_diag, "RAW SIGNALS");
    imu_lbl  = section(ui_scr_diag, "WATCH IMU");
    sys_lbl  = section(ui_scr_diag, "SYSTEM");

    ui_button(ui_scr_diag, LV_SYMBOL_LEFT "  Back", ui_go_menu_cb, NULL);
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

    snprintf(buf, sizeof(buf), "battery %s\nlog %s  %s\nrows %lu  free %llu KB%s\nclock %s   BLE %s",
             batt, store[lg.store], lg.file[0] ? lg.file : "-",
             (unsigned long)lg.rows, (unsigned long long)(lg.free_bytes / 1024),
             lg.full ? "  FULL" : lg.error ? "  ERROR" : "",
             tsrc[rtc_service_time_source()],
             ble_service_connected() ? "connected" :
             ble_service_advertising() ? "advertising" : "off");

    lv_label_set_text(sys_lbl, buf);
}
