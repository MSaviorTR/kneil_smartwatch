#include <math.h>
#include <stdio.h>
#include <time.h>

#include "app_state.h"
#include "ble_service.h"
#include "data_logger.h"
#include "rtc_service.h"
#include "sensor_protocol.h"

#include "ui_internal.h"

#define TILE_W 182
#define TILE_H 178
#define TILE_Y 158

typedef struct
{
    lv_obj_t *card;
    lv_obj_t *title;
    lv_obj_t *value;
    lv_obj_t *unit;
    lv_obj_t *sub;
} tile_t;

static lv_obj_t *link_lbl;
static lv_obj_t *ble_lbl;
static lv_obj_t *store_lbl;
static lv_obj_t *batt_lbl;
static lv_obj_t *time_lbl;
static lv_obj_t *date_lbl;
static lv_obj_t *clock_warn_lbl;
static lv_obj_t *alert_box;
static lv_obj_t *alert_lbl;
static lv_obj_t *motion_lbl;

static tile_t k_tile;
static tile_t ph_tile;

static void open_trends(lv_event_t *e)
{
    (void)e;
    ui_go(ui_scr_trend);
}

static void tile_create(tile_t *t, lv_obj_t *parent, int x, const char *title, lv_color_t accent)
{
    t->card = lv_obj_create(parent);
    lv_obj_set_size(t->card, TILE_W, TILE_H);
    lv_obj_set_pos(t->card, x, TILE_Y);
    lv_obj_set_style_bg_color(t->card, UI_COL_CARD, 0);
    lv_obj_set_style_border_color(t->card, accent, 0);
    lv_obj_set_style_border_width(t->card, 2, 0);
    lv_obj_set_style_radius(t->card, 20, 0);
    lv_obj_set_style_pad_all(t->card, 10, 0);
    lv_obj_remove_flag(t->card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(t->card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(t->card, open_trends, LV_EVENT_CLICKED, NULL);

    t->title = ui_label(t->card, title, &lv_font_montserrat_20, accent);
    lv_obj_align(t->title, LV_ALIGN_TOP_LEFT, 0, 0);

    t->value = ui_label(t->card, "--", &lv_font_montserrat_48, UI_COL_TEXT);
    lv_obj_align(t->value, LV_ALIGN_CENTER, 0, -4);

    t->unit = ui_label(t->card, "", &lv_font_montserrat_16, UI_COL_DIM);
    lv_obj_align(t->unit, LV_ALIGN_TOP_RIGHT, 0, 4);

    t->sub = ui_label(t->card, "", &lv_font_montserrat_14, UI_COL_DIM);
    lv_obj_set_width(t->sub, TILE_W - 20);
    lv_obj_set_style_text_align(t->sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t->sub, LV_ALIGN_BOTTOM_MID, 0, 0);
}

void ui_home_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    ui_scr_home = scr;

    lv_obj_set_style_bg_color(scr, UI_COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, UI_COL_TEXT, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Status bar */
    link_lbl = ui_label(scr, "", &lv_font_montserrat_16, UI_COL_DIM);
    lv_obj_set_pos(link_lbl, 22, 16);

    ble_lbl = ui_label(scr, LV_SYMBOL_BLUETOOTH, &lv_font_montserrat_16, UI_COL_DIM);
    lv_obj_align(ble_lbl, LV_ALIGN_TOP_MID, -10, 16);

    store_lbl = ui_label(scr, LV_SYMBOL_SD_CARD, &lv_font_montserrat_16, UI_COL_DIM);
    lv_obj_align(store_lbl, LV_ALIGN_TOP_MID, 18, 16);

    batt_lbl = ui_label(scr, "", &lv_font_montserrat_16, UI_COL_TEXT);
    lv_obj_align(batt_lbl, LV_ALIGN_TOP_RIGHT, -22, 16);

    /* Clock */
    time_lbl = ui_label(scr, "--:--", &lv_font_montserrat_48, UI_COL_TEXT);
    lv_obj_align(time_lbl, LV_ALIGN_TOP_MID, 0, 46);

    date_lbl = ui_label(scr, "", &lv_font_montserrat_18, UI_COL_DIM);
    lv_obj_align(date_lbl, LV_ALIGN_TOP_MID, 0, 104);

    clock_warn_lbl = ui_label(scr, LV_SYMBOL_WARNING " clock not set", &lv_font_montserrat_14, UI_COL_WARN);
    lv_obj_align(clock_warn_lbl, LV_ALIGN_TOP_MID, 0, 130);

    /* Sensor tiles */
    tile_create(&k_tile, scr, 15, "K+", UI_COL_K);
    tile_create(&ph_tile, scr, UI_W - 15 - TILE_W, "pH", UI_COL_PH);
    lv_label_set_text(k_tile.unit, "mM");

    /* Alert banner */
    alert_box = lv_obj_create(scr);
    lv_obj_set_size(alert_box, UI_W - 30, 52);
    lv_obj_align(alert_box, LV_ALIGN_TOP_MID, 0, TILE_Y + TILE_H + 10);
    lv_obj_set_style_bg_color(alert_box, UI_COL_ALERT, 0);
    lv_obj_set_style_border_width(alert_box, 0, 0);
    lv_obj_set_style_radius(alert_box, 14, 0);
    lv_obj_remove_flag(alert_box, LV_OBJ_FLAG_SCROLLABLE);

    alert_lbl = ui_label(alert_box, "", &lv_font_montserrat_20, UI_COL_TEXT);
    lv_obj_center(alert_lbl);
    lv_obj_add_flag(alert_box, LV_OBJ_FLAG_HIDDEN);

    motion_lbl = ui_label(scr, "", &lv_font_montserrat_14, UI_COL_WARN);
    lv_obj_align(motion_lbl, LV_ALIGN_TOP_MID, 0, TILE_Y + TILE_H + 68);

    /* Menu */
    lv_obj_t *menu = ui_button(scr, LV_SYMBOL_LIST "  MENU", ui_go_menu_cb, NULL);
    lv_obj_set_size(menu, 200, 54);
    lv_obj_align(menu, LV_ALIGN_BOTTOM_MID, 0, -16);
}

/* Fills one tile from a reading. */
static void tile_update(tile_t *t, bool have, data_source_t active, const reading_t *r,
                        float value, float mv, uint32_t fault_flag, int decimals)
{
    char v[16];
    char sub[48];
    char mv_txt[16];

    ui_fmt(mv_txt, sizeof(mv_txt), mv, 1);

    if(!have || active == DATA_SRC_NONE)
    {
        lv_label_set_text(t->value, "--");
        lv_obj_set_style_text_color(t->value, UI_COL_DIM, 0);
        lv_label_set_text(t->sub, have ? "last data stale" : "waiting for data");
        lv_obj_set_style_text_color(t->sub, UI_COL_DIM, 0);
        return;
    }

    if(r->flags & fault_flag)
    {
        lv_label_set_text(t->value, "ERR");
        lv_obj_set_style_text_color(t->value, UI_COL_ALERT, 0);
        lv_label_set_text(t->sub, "electrode fault");
        lv_obj_set_style_text_color(t->sub, UI_COL_ALERT, 0);
        return;
    }

    if(!isfinite(mv))
    {
        lv_label_set_text(t->value, "--");
        lv_obj_set_style_text_color(t->value, UI_COL_DIM, 0);
        lv_label_set_text(t->sub, "channel not sent");
        lv_obj_set_style_text_color(t->sub, UI_COL_DIM, 0);
        return;
    }

    if(!isfinite(value))
    {
        /* Real data but no calibration stored for this channel. */
        lv_label_set_text(t->value, "--");
        lv_obj_set_style_text_color(t->value, UI_COL_DIM, 0);
        snprintf(sub, sizeof(sub), "UNCAL  %s mV", mv_txt);
        lv_label_set_text(t->sub, sub);
        lv_obj_set_style_text_color(t->sub, UI_COL_WARN, 0);
        return;
    }

    ui_fmt(v, sizeof(v), value, decimals);
    lv_label_set_text(t->value, v);
    lv_obj_set_style_text_color(t->value, UI_COL_TEXT, 0);

    snprintf(sub, sizeof(sub), "%s%s mV", active == DATA_SRC_DEMO ? "DEMO  " : "", mv_txt);
    lv_label_set_text(t->sub, sub);
    lv_obj_set_style_text_color(t->sub, active == DATA_SRC_DEMO ? UI_COL_WARN : UI_COL_DIM, 0);
}

void ui_home_refresh(void)
{
    /* Clock */
    time_t now = time(NULL);
    struct tm t;
    char buf[48];

    localtime_r(&now, &t);
    strftime(buf, sizeof(buf), "%H:%M", &t);
    lv_label_set_text(time_lbl, buf);
    strftime(buf, sizeof(buf), "%a %d %b %Y", &t);
    lv_label_set_text(date_lbl, buf);

    if(rtc_service_time_valid())
    {
        lv_obj_add_flag(clock_warn_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_remove_flag(clock_warn_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    /* Link status */
    data_source_t active = app_state_active_source();
    link_stats_t ls;
    app_state_link_stats(&ls);

    switch(active)
    {
        case DATA_SRC_SENSOR:
            lv_label_set_text(link_lbl, LV_SYMBOL_OK " LIVE");
            lv_obj_set_style_text_color(link_lbl, UI_COL_OK, 0);
            break;
        case DATA_SRC_DEMO:
            lv_label_set_text(link_lbl, LV_SYMBOL_PLAY " DEMO");
            lv_obj_set_style_text_color(link_lbl, UI_COL_WARN, 0);
            break;
        default:
            lv_label_set_text(link_lbl, ls.frames_ok ? LV_SYMBOL_CLOSE " LINK LOST" : "NO SENSOR");
            lv_obj_set_style_text_color(link_lbl, ls.frames_ok ? UI_COL_ALERT : UI_COL_DIM, 0);
            break;
    }

    lv_obj_set_style_text_color(ble_lbl,
        ble_service_connected() ? UI_COL_BLE :
        ble_service_advertising() ? UI_COL_DIM : lv_color_hex(0x333333), 0);

    logger_status_t lg;
    data_logger_status(&lg);
    lv_label_set_text(store_lbl, lg.store == LOG_STORE_SD ? LV_SYMBOL_SD_CARD : LV_SYMBOL_SAVE);
    lv_obj_set_style_text_color(store_lbl,
        (lg.full || lg.error) ? UI_COL_ALERT :
        lg.store == LOG_STORE_NONE ? lv_color_hex(0x333333) : UI_COL_DIM, 0);

    /* Battery */
    power_status_t p = {0};

    if(app_state_power(&p) && p.percent >= 0)
    {
        snprintf(buf, sizeof(buf), "%s%d%%", p.charging ? LV_SYMBOL_CHARGE " " : "", p.percent);
        lv_label_set_text(batt_lbl, buf);
        lv_obj_set_style_text_color(batt_lbl, p.percent <= 15 && !p.charging ? UI_COL_ALERT : UI_COL_TEXT, 0);
    }
    else
    {
        lv_label_set_text(batt_lbl, p.usb_connected ? LV_SYMBOL_USB : "--%");
    }

    lv_obj_align(batt_lbl, LV_ALIGN_TOP_RIGHT, -22, 16);

    /* Readings */
    reading_t r = {0};
    bool have = app_state_latest(&r);

    tile_update(&k_tile, have, active, &r, r.k_mM, r.k_mv, SENSOR_FLAG_K_FAULT, 2);
    tile_update(&ph_tile, have, active, &r, r.ph, r.ph_mv, SENSOR_FLAG_PH_FAULT, 2);

    bool show_alert = have && active != DATA_SRC_NONE && r.k_alert;

    if(show_alert)
    {
        char v[16];
        ui_fmt(v, sizeof(v), r.k_mM, 2);
        snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING "  K+ HIGH  %s mM", v);
        lv_label_set_text(alert_lbl, buf);
        lv_obj_remove_flag(alert_box, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(alert_box, LV_OBJ_FLAG_HIDDEN);
    }

    lv_label_set_text(motion_lbl, (have && active != DATA_SRC_NONE && r.motion)
                                  ? "movement - readings may be noisy" : "");
}
