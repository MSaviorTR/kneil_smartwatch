#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bsp/esp-bsp.h"

#include "app_state.h"
#include "ble_service.h"
#include "data_logger.h"
#include "rtc_service.h"

#include "ui_internal.h"

/* Never fully dark: brightness 0 blanks the AMOLED. */
#define BRIGHTNESS_MIN      10

/* K+ alert slider works in tenths of mM. */
#define ALERT_MIN_TENTHS    30
#define ALERT_MAX_TENTHS    150

#define YEAR_FIRST          2025
#define YEAR_COUNT          16

static lv_obj_t *ble_sw;
static lv_obj_t *ble_mode_lbl;
static lv_obj_t *vib_sw;
static lv_obj_t *demo_sw;
static lv_obj_t *alert_lbl;
static lv_obj_t *alert_slider;
static lv_obj_t *storage_lbl;
static lv_obj_t *confirm_box;

/* Time screen */
static lv_obj_t *r_day;
static lv_obj_t *r_mon;
static lv_obj_t *r_year;
static lv_obj_t *r_hour;
static lv_obj_t *r_min;
static lv_obj_t *time_msg;

/* ------------------------------------------------------------------
   Settings
   ------------------------------------------------------------------ */

static void apply_switch(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target_obj(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    app_settings_t s;

    app_state_get_settings(&s);

    if(sw == ble_sw)
    {
        s.ble_enabled = on;
        ble_service_set_enabled(on);
    }
    else if(sw == vib_sw)
    {
        s.vibration_enabled = on;
    }
    else if(sw == demo_sw)
    {
        s.demo_mode = on;
    }

    app_state_set_settings(&s);
}

static const char *BLE_MODE_TXT[BLE_MODE_COUNT] = {
    [BLE_MODE_PHONE]  = "Phone only",
    [BLE_MODE_SENSOR] = "Sensor only",
    [BLE_MODE_BOTH]   = "Phone + sensor",
};

static void ble_mode_text(uint8_t mode)
{
    lv_label_set_text_fmt(ble_mode_lbl, LV_SYMBOL_BLUETOOTH "  Mode: %s",
                          BLE_MODE_TXT[mode % BLE_MODE_COUNT]);
}

/* Cycles Phone -> Sensor -> Both. Phone = the watch is a peripheral a
   phone connects to (data out); Sensor = the watch is a central that
   connects to the wireless sensor (data in). */
static void ble_mode_cb(lv_event_t *e)
{
    (void)e;

    app_settings_t s;
    app_state_get_settings(&s);
    s.ble_mode = (uint8_t)((s.ble_mode + 1) % BLE_MODE_COUNT);
    app_state_set_settings(&s);

    ble_service_set_mode(s.ble_mode);
    ble_mode_text(s.ble_mode);
}

static void alert_text(int tenths)
{
    lv_label_set_text_fmt(alert_lbl, "K+ alert above %d.%d mM", tenths / 10, tenths % 10);
}

static void alert_changed(lv_event_t *e)
{
    alert_text(lv_slider_get_value(lv_event_get_target_obj(e)));
}

/* Saved on release only, so dragging does not hammer flash. */
static void alert_released(lv_event_t *e)
{
    app_settings_t s;
    app_state_get_settings(&s);
    s.k_alert_mM = lv_slider_get_value(lv_event_get_target_obj(e)) / 10.0f;
    app_state_set_settings(&s);
}

static void brightness_changed(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target_obj(e));
    bsp_display_brightness_set(v < BRIGHTNESS_MIN ? BRIGHTNESS_MIN : v);
}

static void brightness_released(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target_obj(e));
    app_settings_t s;

    app_state_get_settings(&s);
    s.brightness = (uint8_t)(v < BRIGHTNESS_MIN ? BRIGHTNESS_MIN : v);
    app_state_set_settings(&s);
}

static void confirm_cb(lv_event_t *e)
{
    if((bool)(intptr_t)lv_event_get_user_data(e))
    {
        data_logger_clear();
    }

    lv_msgbox_close_async(confirm_box);
    confirm_box = NULL;
}

static void clear_logs_cb(lv_event_t *e)
{
    (void)e;

    confirm_box = lv_msgbox_create(NULL);
    lv_msgbox_add_title(confirm_box, "Delete all logs?");
    lv_msgbox_add_text(confirm_box, "Every gw_*.csv file on the watch storage will be "
                                    "erased. Copy them off first (tools/pull_logs.py).");

    lv_obj_t *yes = lv_msgbox_add_footer_button(confirm_box, "Delete");
    lv_obj_add_event_cb(yes, confirm_cb, LV_EVENT_CLICKED, (void *)(intptr_t)true);

    lv_obj_t *no = lv_msgbox_add_footer_button(confirm_box, "Cancel");
    lv_obj_add_event_cb(no, confirm_cb, LV_EVENT_CLICKED, (void *)(intptr_t)false);

    lv_obj_set_width(confirm_box, UI_W - 40);
}

static void open_time(lv_event_t *e)
{
    (void)e;
    ui_open_time();
}

static lv_obj_t *switch_row(lv_obj_t *parent, const char *text, bool on)
{
    lv_obj_t *row = ui_row(parent);

    ui_label(row, text, &lv_font_montserrat_20, UI_COL_TEXT);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 70, 38);

    if(on)
    {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }

    lv_obj_add_event_cb(sw, apply_switch, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

static lv_obj_t *slider(lv_obj_t *parent, int min, int max, int value)
{
    lv_obj_t *s = lv_slider_create(parent);

    lv_obj_set_width(s, LV_PCT(90));
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, value, LV_ANIM_OFF);

    /* Give the knob room so the screen does not scroll instead. */
    lv_obj_set_style_margin_top(s, 8, 0);
    lv_obj_set_style_margin_bottom(s, 8, 0);

    return s;
}

void ui_settings_create(lv_obj_t *tile)
{
    app_settings_t s;
    app_state_get_settings(&s);

    lv_obj_t *page = ui_page(UI_PAGE_SETTINGS, LV_SYMBOL_SETTINGS, "SETTINGS", UI_COL_BLE);
    (void)tile;

    ble_sw  = switch_row(page, LV_SYMBOL_BLUETOOTH "  Bluetooth", s.ble_enabled);

    lv_obj_t *mode_btn = ui_button(page, "", ble_mode_cb, NULL);
    ble_mode_lbl = lv_obj_get_child(mode_btn, 0);
    ble_mode_text(s.ble_mode);

    vib_sw  = switch_row(page, "Vibration alerts", s.vibration_enabled);
    demo_sw = switch_row(page, "Demo data", s.demo_mode);

    int tenths = (int)(s.k_alert_mM * 10.0f + 0.5f);

    alert_lbl = ui_label(page, "", &lv_font_montserrat_18, UI_COL_K);
    alert_text(tenths);

    alert_slider = slider(page, ALERT_MIN_TENTHS, ALERT_MAX_TENTHS, tenths);
    lv_obj_add_event_cb(alert_slider, alert_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(alert_slider, alert_released, LV_EVENT_RELEASED, NULL);

    ui_label(page, LV_SYMBOL_EYE_OPEN "  Brightness", &lv_font_montserrat_18, UI_COL_TEXT);

    lv_obj_t *bright = slider(page, BRIGHTNESS_MIN, 100, s.brightness);
    lv_obj_add_event_cb(bright, brightness_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(bright, brightness_released, LV_EVENT_RELEASED, NULL);

    ui_button(page, LV_SYMBOL_REFRESH "  Set time", open_time, NULL);
    ui_button(page, LV_SYMBOL_TRASH "  Clear logs", clear_logs_cb, NULL);

    storage_lbl = ui_label(page, "", &lv_font_montserrat_14, UI_COL_DIM);
}

void ui_settings_refresh(void)
{
    logger_status_t lg;
    data_logger_status(&lg);

    static const char *store[] = { "no storage", "SD card", "internal flash" };

    lv_label_set_text_fmt(storage_lbl, "Logging to %s - %lu KB free%s",
                          store[lg.store], (unsigned long)(lg.free_bytes / 1024),
                          lg.full ? " (FULL)" : "");
}

/* ------------------------------------------------------------------
   Set time
   ------------------------------------------------------------------ */

/* The roller copies its options, so one scratch buffer can be reused. */
static const char *number_options(int first, int count, int width)
{
    static char opts[256];
    size_t used = 0;

    for(int i = 0; i < count && used < sizeof(opts); i++)
    {
        used += (size_t)snprintf(opts + used, sizeof(opts) - used, "%s%0*d",
                                 i ? "\n" : "", width, first + i);
    }

    return opts;
}

static lv_obj_t *time_roller(lv_obj_t *parent, const char *opts, int width)
{
    lv_obj_t *r = lv_roller_create(parent);

    lv_roller_set_options(r, opts, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(r, 3);
    lv_obj_set_width(r, width);
    lv_obj_set_style_text_font(r, &lv_font_montserrat_22, 0);
    lv_obj_set_style_bg_color(r, UI_COL_CARD, 0);
    lv_obj_set_style_text_color(r, UI_COL_TEXT, 0);

    return r;
}

static void time_prefill(lv_event_t *e)
{
    (void)e;

    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);

    int year_idx = t.tm_year + 1900 - YEAR_FIRST;

    if(year_idx < 0) year_idx = 0;
    if(year_idx >= YEAR_COUNT) year_idx = YEAR_COUNT - 1;

    lv_roller_set_selected(r_day, (uint32_t)(t.tm_mday - 1), LV_ANIM_OFF);
    lv_roller_set_selected(r_mon, (uint32_t)t.tm_mon, LV_ANIM_OFF);
    lv_roller_set_selected(r_year, (uint32_t)year_idx, LV_ANIM_OFF);
    lv_roller_set_selected(r_hour, (uint32_t)t.tm_hour, LV_ANIM_OFF);
    lv_roller_set_selected(r_min, (uint32_t)t.tm_min, LV_ANIM_OFF);

    lv_label_set_text(time_msg, "Local time (UK). Seconds are set to 00.");
    lv_obj_set_style_text_color(time_msg, UI_COL_DIM, 0);
}

static void time_save(lv_event_t *e)
{
    (void)e;

    struct tm t = {0};

    t.tm_mday  = (int)lv_roller_get_selected(r_day) + 1;
    t.tm_mon   = (int)lv_roller_get_selected(r_mon);
    t.tm_year  = (int)lv_roller_get_selected(r_year) + YEAR_FIRST - 1900;
    t.tm_hour  = (int)lv_roller_get_selected(r_hour);
    t.tm_min   = (int)lv_roller_get_selected(r_min);
    t.tm_isdst = -1;   /* let the TZ rules decide GMT vs BST */

    int want_mday = t.tm_mday;
    time_t utc = mktime(&t);

    /* mktime normalises 31 Feb into March; treat that as invalid input. */
    if(utc == (time_t)-1 || t.tm_mday != want_mday)
    {
        lv_label_set_text(time_msg, "That date does not exist.");
        lv_obj_set_style_text_color(time_msg, UI_COL_ALERT, 0);
        return;
    }

    if(rtc_service_set_epoch(utc) != ESP_OK)
    {
        lv_label_set_text(time_msg, "Clock set, but saving to the RTC failed.");
        lv_obj_set_style_text_color(time_msg, UI_COL_WARN, 0);
        return;
    }

    ui_close_time();
}

static void time_cancel(lv_event_t *e)
{
    (void)e;
    ui_close_time();
}

void ui_time_create(void)
{
    ui_scr_time = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_scr_time, UI_COL_BG, 0);
    lv_obj_set_style_bg_opa(ui_scr_time, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(ui_scr_time, UI_COL_TEXT, 0);
    lv_obj_set_style_pad_all(ui_scr_time, UI_PAD, 0);
    lv_obj_set_style_pad_row(ui_scr_time, 14, 0);
    lv_obj_set_flex_flow(ui_scr_time, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui_scr_time, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *head = ui_row(ui_scr_time);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui_label(head, LV_SYMBOL_REFRESH, &lv_font_montserrat_22, UI_COL_BLE);
    ui_label(head, "SET TIME", &lv_font_montserrat_22, UI_COL_TEXT);

    lv_obj_t *date_row = ui_row(ui_scr_time);
    lv_obj_set_flex_align(date_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    r_day  = time_roller(date_row, number_options(1, 31, 2), 80);
    r_mon  = time_roller(date_row, "Jan\nFeb\nMar\nApr\nMay\nJun\nJul\nAug\nSep\nOct\nNov\nDec", 100);
    r_year = time_roller(date_row, number_options(YEAR_FIRST, YEAR_COUNT, 4), 110);

    lv_obj_t *time_row = ui_row(ui_scr_time);
    lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    r_hour = time_roller(time_row, number_options(0, 24, 2), 90);
    ui_label(time_row, ":", &lv_font_montserrat_26, UI_COL_TEXT);
    r_min  = time_roller(time_row, number_options(0, 60, 2), 90);

    time_msg = ui_label(ui_scr_time, "", &lv_font_montserrat_14, UI_COL_DIM);

    ui_button(ui_scr_time, LV_SYMBOL_OK "  Save", time_save, NULL);
    ui_button(ui_scr_time, LV_SYMBOL_CLOSE "  Cancel", time_cancel, NULL);

    lv_obj_add_event_cb(ui_scr_time, time_prefill, LV_EVENT_SCREEN_LOAD_START, NULL);
}
