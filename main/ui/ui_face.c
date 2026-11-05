#include <math.h>
#include <stdio.h>
#include <time.h>

#include "esp_heap_caps.h"

#include "app_state.h"
#include "ble_service.h"
#include "data_logger.h"
#include "rtc_service.h"
#include "sensor_protocol.h"

#include "ui_internal.h"

/*
 * Watch face:
 *
 *   status row      link . BLE . storage . battery
 *   clock           HH:MM, date (or a warning in its place)
 *   K+ gauge        240-degree arc, green / amber / red zones, animated
 *                   marker, big animated value, state text, raw mV
 *   bottom row      pH ring  |  30-min K+ sparkline + trend arrow
 */

#define GAUGE_SIZE      280
#define GAUGE_Y         96
#define GAUGE_ROT       150     /* arc starts bottom-left ...           */
#define GAUGE_SWEEP     240     /* ... and sweeps clockwise to bottom-right */
#define GAUGE_WIDTH     16
#define ZONE_GAP_DEG    2
#define MARKER_RANGE    1000

#define PH_SIZE         108
#define PH_MIN          600     /* pH * 100 */
#define PH_MAX          850

#define SPARK_POINTS    60
#define HISTORY_LEN     1800    /* 30 min, one value per second */

#define ANIM_MS         600

/* Trend thresholds in mM per 10 min. Placeholders. */
#define RATE_NOTABLE    0.3f
#define RATE_FAST       1.0f

static lv_obj_t *link_lbl;
static lv_obj_t *ble_lbl;
static lv_obj_t *store_lbl;
static lv_obj_t *batt_lbl;
static lv_obj_t *time_lbl;
static lv_obj_t *date_lbl;

static lv_obj_t *zones[3];
static lv_obj_t *marker;
static lv_obj_t *k_value_lbl;
static lv_obj_t *k_state_lbl;
static lv_obj_t *k_mv_lbl;
static lv_obj_t *scale_max_lbl;

static lv_obj_t *ph_arc;
static lv_obj_t *ph_value_lbl;
static lv_obj_t *ph_note_lbl;

static lv_obj_t *spark;
static lv_chart_series_t *spark_ser;
static lv_obj_t *trend_lbl;

static float  gauge_max = -1.0f;
static float  zone_threshold = -1.0f;
static int32_t k_shown = INT32_MIN;      /* displayed value * 100 */
static int32_t ph_shown = INT32_MIN;

static float *hist;                      /* PSRAM, HISTORY_LEN floats */

/* ------------------------------------------------------------------
   Gauge geometry
   ------------------------------------------------------------------ */

static int32_t deg_for(float mM)
{
    float f = mM / gauge_max;

    if(f < 0.0f) f = 0.0f;
    if(f > 1.0f) f = 1.0f;

    return (int32_t)lroundf(f * GAUGE_SWEEP);
}

static lv_obj_t *gauge_arc(lv_obj_t *parent)
{
    lv_obj_t *a = lv_arc_create(parent);

    lv_obj_set_size(a, GAUGE_SIZE, GAUGE_SIZE);
    lv_obj_align(a, LV_ALIGN_TOP_MID, 0, GAUGE_Y);
    lv_arc_set_rotation(a, GAUGE_ROT);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(a, GAUGE_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, GAUGE_WIDTH, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, false, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(a, 0, LV_PART_KNOB);

    return a;
}

/* Zones: green up to the normal limit, amber up to the alert threshold,
   red beyond. Rebuilt only when the threshold changes. */
static void layout_zones(float threshold)
{
    if(threshold == zone_threshold)
    {
        return;
    }

    zone_threshold = threshold;
    gauge_max = fmaxf(10.0f, ceilf(threshold * 1.5f));

    float normal_hi = fminf(UI_K_NORMAL_MAX, threshold);
    int32_t a_normal = deg_for(normal_hi);
    int32_t a_thr = deg_for(threshold);

    lv_arc_set_bg_angles(zones[0], 0, a_normal - ZONE_GAP_DEG / 2);

    if(a_thr - a_normal > ZONE_GAP_DEG)
    {
        lv_obj_remove_flag(zones[1], LV_OBJ_FLAG_HIDDEN);
        lv_arc_set_bg_angles(zones[1], a_normal + ZONE_GAP_DEG / 2, a_thr - ZONE_GAP_DEG / 2);
    }
    else
    {
        lv_obj_add_flag(zones[1], LV_OBJ_FLAG_HIDDEN);
    }

    lv_arc_set_bg_angles(zones[2], a_thr + ZONE_GAP_DEG / 2, GAUGE_SWEEP);

    lv_label_set_text_fmt(scale_max_lbl, "%d", (int)gauge_max);
}

/* ------------------------------------------------------------------
   Animated values
   ------------------------------------------------------------------ */

static void k_anim_cb(void *var, int32_t v)
{
    *(int32_t *)var = v;

    lv_label_set_text_fmt(k_value_lbl, "%" LV_PRId32 ".%02" LV_PRId32, v / 100, v % 100);
    lv_arc_set_value(marker, (int32_t)lroundf((v / 100.0f) / gauge_max * MARKER_RANGE));
}

static void ph_anim_cb(void *var, int32_t v)
{
    *(int32_t *)var = v;

    lv_label_set_text_fmt(ph_value_lbl, "%" LV_PRId32 ".%02" LV_PRId32, v / 100, v % 100);
    lv_arc_set_value(ph_arc, v);
}

/* Glides from the shown value to the new one; jumps on first show. */
static void animate_to(int32_t *shown, int32_t target, lv_anim_exec_xcb_t cb)
{
    if(*shown == target)
    {
        return;
    }

    lv_anim_delete(shown, cb);

    if(*shown == INT32_MIN)
    {
        cb(shown, target);
        return;
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, shown);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, *shown, target);
    lv_anim_set_duration(&a, ANIM_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void stop_anim(int32_t *shown, lv_anim_exec_xcb_t cb)
{
    lv_anim_delete(shown, cb);
    *shown = INT32_MIN;
}

/* ------------------------------------------------------------------
   Build
   ------------------------------------------------------------------ */

static void open_trends(lv_event_t *e)
{
    (void)e;
    ui_goto_page(UI_PAGE_TREND);
}

/* Invisible hit area. The dials themselves are display-only arcs: if they
   were clickable a drag would change their value and block swiping. A
   plain object still passes horizontal drags on to the tileview. */
static void tap_zone(lv_obj_t *parent, int32_t w, int32_t h, lv_align_t align, int32_t x, int32_t y)
{
    lv_obj_t *z = lv_obj_create(parent);

    lv_obj_remove_style_all(z);
    lv_obj_set_size(z, w, h);
    lv_obj_align(z, align, x, y);
    lv_obj_add_flag(z, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(z, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(z, open_trends, LV_EVENT_SHORT_CLICKED, NULL);
}

static lv_obj_t *abs_label(lv_obj_t *parent, const char *txt, const lv_font_t *f, lv_color_t c,
                           lv_align_t align, int32_t x, int32_t y)
{
    lv_obj_t *l = ui_label(parent, txt, f, c);
    lv_obj_align(l, align, x, y);
    return l;
}

void ui_face_create(lv_obj_t *tile)
{
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_color(tile, UI_COL_TEXT, 0);
    lv_obj_set_style_pad_all(tile, 0, 0);

    hist = heap_caps_malloc(HISTORY_LEN * sizeof(float), MALLOC_CAP_SPIRAM);

    /* Status row */
    link_lbl  = abs_label(tile, "", &lv_font_montserrat_16, UI_COL_DIM, LV_ALIGN_TOP_LEFT, 24, 14);
    ble_lbl   = abs_label(tile, LV_SYMBOL_BLUETOOTH, &lv_font_montserrat_16, UI_COL_DIM, LV_ALIGN_TOP_MID, -14, 14);
    store_lbl = abs_label(tile, LV_SYMBOL_SD_CARD, &lv_font_montserrat_16, UI_COL_DIM, LV_ALIGN_TOP_MID, 14, 14);
    batt_lbl  = abs_label(tile, "", &lv_font_montserrat_16, UI_COL_TEXT, LV_ALIGN_TOP_RIGHT, -24, 14);

    /* Clock */
    time_lbl = abs_label(tile, "--:--", &lv_font_montserrat_36, UI_COL_TEXT, LV_ALIGN_TOP_MID, 0, 36);
    date_lbl = abs_label(tile, "", &lv_font_montserrat_14, UI_COL_DIM, LV_ALIGN_TOP_MID, 0, 78);

    /* K+ gauge: three zone arcs under one marker arc. */
    static const uint32_t zone_cols[3] = { 0x2fae6a, 0xd9a300, 0xd93b3b };

    for(int i = 0; i < 3; i++)
    {
        zones[i] = gauge_arc(tile);
        lv_obj_set_style_arc_color(zones[i], lv_color_hex(zone_cols[i]), LV_PART_MAIN);
    }

    marker = gauge_arc(tile);
    lv_arc_set_bg_angles(marker, 0, GAUGE_SWEEP);
    lv_arc_set_range(marker, 0, MARKER_RANGE);
    lv_obj_set_style_arc_opa(marker, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(marker, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_color(marker, UI_COL_TEXT, LV_PART_KNOB);
    lv_obj_set_style_border_width(marker, 4, LV_PART_KNOB);
    lv_obj_set_style_border_color(marker, UI_COL_BG, LV_PART_KNOB);
    lv_obj_set_style_pad_all(marker, 6, LV_PART_KNOB);

    int32_t cy = GAUGE_Y + GAUGE_SIZE / 2;   /* gauge centre, from the top */

    abs_label(tile, "K+", &lv_font_montserrat_18, UI_COL_K, LV_ALIGN_TOP_MID, 0, cy - 78);
    k_value_lbl = abs_label(tile, "--", &lv_font_montserrat_48, UI_COL_TEXT, LV_ALIGN_TOP_MID, 0, cy - 50);
    k_state_lbl = abs_label(tile, "", &lv_font_montserrat_16, UI_COL_DIM, LV_ALIGN_TOP_MID, 0, cy + 10);
    k_mv_lbl    = abs_label(tile, "", &lv_font_montserrat_14, UI_COL_DIM, LV_ALIGN_TOP_MID, 0, cy + 38);

    /* Scale ends sit just below the arc ends (+/-120 deg from the top). */
    int32_t end_dx = (int32_t)(GAUGE_SIZE / 2 * 0.866f);
    int32_t end_y  = cy + GAUGE_SIZE / 4 + 6;

    abs_label(tile, "0", &lv_font_montserrat_14, UI_COL_DIM, LV_ALIGN_TOP_MID, -end_dx, end_y);
    scale_max_lbl = abs_label(tile, "10", &lv_font_montserrat_14, UI_COL_DIM, LV_ALIGN_TOP_MID, end_dx, end_y);

    /* pH ring */
    ph_arc = lv_arc_create(tile);
    lv_obj_set_size(ph_arc, PH_SIZE, PH_SIZE);
    lv_obj_set_pos(ph_arc, 26, 336);
    lv_arc_set_rotation(ph_arc, 135);
    lv_arc_set_bg_angles(ph_arc, 0, 270);
    lv_arc_set_range(ph_arc, PH_MIN, PH_MAX);
    lv_arc_set_value(ph_arc, PH_MIN);
    lv_obj_set_style_arc_width(ph_arc, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ph_arc, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ph_arc, UI_COL_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_color(ph_arc, UI_COL_PH, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(ph_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(ph_arc, 0, LV_PART_KNOB);
    lv_obj_remove_flag(ph_arc, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    ph_value_lbl = ui_label(ph_arc, "--", &lv_font_montserrat_22, UI_COL_TEXT);
    lv_obj_align(ph_value_lbl, LV_ALIGN_CENTER, 0, -6);
    lv_obj_t *ph_unit = ui_label(ph_arc, "pH", &lv_font_montserrat_14, UI_COL_PH);
    lv_obj_align(ph_unit, LV_ALIGN_CENTER, 0, 18);


    ph_note_lbl = abs_label(tile, "", &lv_font_montserrat_14, UI_COL_DIM, LV_ALIGN_TOP_LEFT, 26, 448);
    lv_obj_set_width(ph_note_lbl, PH_SIZE);
    lv_obj_set_style_text_align(ph_note_lbl, LV_TEXT_ALIGN_CENTER, 0);

    /* Sparkline */
    abs_label(tile, "K+  last 30 min", &lv_font_montserrat_14, UI_COL_DIM, LV_ALIGN_TOP_LEFT, 158, 340);

    spark = lv_chart_create(tile);
    lv_obj_set_size(spark, 226, 62);
    lv_obj_set_pos(spark, 158, 364);
    lv_chart_set_type(spark, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(spark, SPARK_POINTS);
    lv_chart_set_div_line_count(spark, 0, 0);
    lv_obj_set_style_bg_opa(spark, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spark, 0, 0);
    lv_obj_set_style_pad_all(spark, 2, 0);
    lv_obj_set_style_line_width(spark, 3, LV_PART_ITEMS);
    lv_obj_set_style_size(spark, 0, 0, LV_PART_INDICATOR);
    lv_obj_remove_flag(spark, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    spark_ser = lv_chart_add_series(spark, UI_COL_K, LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_values(spark, spark_ser, LV_CHART_POINT_NONE);

    trend_lbl = abs_label(tile, "", &lv_font_montserrat_16, UI_COL_DIM, LV_ALIGN_TOP_LEFT, 158, 432);

    /* Created last so they sit above the labels and receive the taps. */
    tap_zone(tile, GAUGE_SIZE - 60, GAUGE_SIZE - 80, LV_ALIGN_TOP_MID, 0, GAUGE_Y + 40);
    tap_zone(tile, PH_SIZE, PH_SIZE, LV_ALIGN_TOP_LEFT, 26, 336);

    app_settings_t s;
    app_state_get_settings(&s);
    layout_zones(s.k_alert_mM);
}

/* ------------------------------------------------------------------
   Refresh (every 250 ms while visible)
   ------------------------------------------------------------------ */

static void refresh_status(data_source_t active)
{
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
            lv_label_set_text(link_lbl, ls.frames_ok ? LV_SYMBOL_CLOSE " LOST" : "NO SENSOR");
            lv_obj_set_style_text_color(link_lbl, ls.frames_ok ? UI_COL_ALERT : UI_COL_DIM, 0);
            break;
    }

    lv_obj_set_style_text_color(ble_lbl,
        ble_service_connected() ? UI_COL_BLE :
        ble_service_advertising() ? UI_COL_DIM : UI_COL_FAINT, 0);

    logger_status_t lg;
    data_logger_status(&lg);
    lv_label_set_text(store_lbl, lg.store == LOG_STORE_SD ? LV_SYMBOL_SD_CARD : LV_SYMBOL_SAVE);
    lv_obj_set_style_text_color(store_lbl,
        (lg.full || lg.error) ? UI_COL_ALERT :
        lg.store == LOG_STORE_NONE ? UI_COL_FAINT : UI_COL_DIM, 0);

    power_status_t p = {0};
    char buf[24];

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
}

static void refresh_clock(const reading_t *r, bool fresh)
{
    time_t now = time(NULL);
    struct tm t;
    char buf[40];

    localtime_r(&now, &t);
    strftime(buf, sizeof(buf), "%H:%M", &t);
    lv_label_set_text(time_lbl, buf);

    /* The date line doubles as the one-line warning slot. */
    if(!rtc_service_time_valid())
    {
        lv_label_set_text(date_lbl, LV_SYMBOL_WARNING " clock not set - see Settings");
        lv_obj_set_style_text_color(date_lbl, UI_COL_WARN, 0);
    }
    else if(fresh && r->motion)
    {
        lv_label_set_text(date_lbl, "movement - readings may be noisy");
        lv_obj_set_style_text_color(date_lbl, UI_COL_WARN, 0);
    }
    else
    {
        strftime(buf, sizeof(buf), "%A %d %B", &t);
        lv_label_set_text(date_lbl, buf);
        lv_obj_set_style_text_color(date_lbl, UI_COL_DIM, 0);
    }
}

static void gauge_inactive(const char *value, const char *state, lv_color_t state_col, const char *mv)
{
    stop_anim(&k_shown, k_anim_cb);
    lv_label_set_text(k_value_lbl, value);
    lv_obj_set_style_text_color(k_value_lbl, UI_COL_DIM, 0);
    lv_label_set_text(k_state_lbl, state);
    lv_obj_set_style_text_color(k_state_lbl, state_col, 0);
    lv_label_set_text(k_mv_lbl, mv);
    lv_obj_add_flag(marker, LV_OBJ_FLAG_HIDDEN);

    for(int i = 0; i < 3; i++)
    {
        lv_obj_set_style_arc_opa(zones[i], LV_OPA_30, LV_PART_MAIN);
    }
}

static void refresh_gauge(const reading_t *r, bool have, data_source_t active, float threshold)
{
    char mv[32];
    char tmp[16];

    layout_zones(threshold);

    ui_fmt(tmp, sizeof(tmp), r->k_mv, 1);
    snprintf(mv, sizeof(mv), "%s mV", tmp);

    if(!have || active == DATA_SRC_NONE)
    {
        gauge_inactive("--", have ? "link lost" : "waiting for sensor", UI_COL_DIM, "");
        return;
    }

    if(r->flags & SENSOR_FLAG_K_FAULT)
    {
        gauge_inactive("ERR", "electrode fault", UI_COL_ALERT, mv);
        return;
    }

    if(r->flags & SENSOR_FLAG_SATURATED)
    {
        gauge_inactive("ERR", "input out of range", UI_COL_ALERT, mv);
        return;
    }

    if(!isfinite(r->k_mv))
    {
        gauge_inactive("--", "K+ channel not sent", UI_COL_DIM, "");
        return;
    }

    if(!isfinite(r->k_mM))
    {
        gauge_inactive("--", "uncalibrated", UI_COL_WARN, mv);
        return;
    }

    for(int i = 0; i < 3; i++)
    {
        lv_obj_set_style_arc_opa(zones[i], LV_OPA_COVER, LV_PART_MAIN);
    }

    lv_obj_remove_flag(marker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_color(k_value_lbl, UI_COL_TEXT, 0);

    int32_t target = (int32_t)lroundf(fminf(r->k_mM, 999.0f) * 100.0f);
    animate_to(&k_shown, target, k_anim_cb);

    const char *state;
    lv_color_t col;

    if(r->k_mM >= threshold || r->k_alert)
    {
        state = "HIGH";
        col = UI_COL_ALERT;
    }
    else if(r->k_mM >= UI_K_NORMAL_MAX)
    {
        state = "elevated";
        col = UI_COL_WARN;
    }
    else
    {
        state = "normal";
        col = UI_COL_OK;
    }

    char line[48];
    snprintf(line, sizeof(line), "mM  -  %s%s", state, active == DATA_SRC_DEMO ? "  (demo)" : "");
    lv_label_set_text(k_state_lbl, line);
    lv_obj_set_style_text_color(k_state_lbl, col, 0);
    lv_obj_set_style_border_color(marker, col, LV_PART_KNOB);
    lv_label_set_text(k_mv_lbl, mv);
}

static void refresh_ph(const reading_t *r, bool have, data_source_t active)
{
    const uint32_t bad = SENSOR_FLAG_PH_FAULT | SENSOR_FLAG_SATURATED;
    bool ok = have && active != DATA_SRC_NONE && !(r->flags & bad) && isfinite(r->ph);

    if(!ok)
    {
        stop_anim(&ph_shown, ph_anim_cb);
        lv_arc_set_value(ph_arc, PH_MIN);

        const char *why = !have || active == DATA_SRC_NONE ? "" :
                          (r->flags & SENSOR_FLAG_PH_FAULT) ? "fault" :
                          (r->flags & SENSOR_FLAG_SATURATED) ? "out of range" :
                          isfinite(r->ph_mv) ? "uncalibrated" : "";

        lv_label_set_text(ph_value_lbl, (r->flags & bad) && have ? "ERR" : "--");
        lv_label_set_text(ph_note_lbl, why);
        lv_obj_set_style_text_color(ph_note_lbl, UI_COL_WARN, 0);
        return;
    }

    /* The ring is clamped to its 6.0-8.5 scale by lv_arc; the label is not. */
    int32_t target = (int32_t)lroundf(r->ph * 100.0f);

    animate_to(&ph_shown, target, ph_anim_cb);

    /* Physiological extracellular pH is ~7.35-7.45; local acidosis is
       one of the infection signs worth flagging. */
    bool acidic = r->ph < 7.2f;
    lv_label_set_text(ph_note_lbl, acidic ? "acidic" : "");
    lv_obj_set_style_text_color(ph_note_lbl, UI_COL_WARN, 0);
    lv_obj_set_style_arc_color(ph_arc, acidic ? UI_COL_WARN : UI_COL_PH, LV_PART_INDICATOR);
}

void ui_face_refresh(void)
{
    reading_t r = {0};
    bool have = app_state_latest(&r);
    data_source_t active = app_state_active_source();
    bool fresh = have && active != DATA_SRC_NONE;

    app_settings_t s;
    app_state_get_settings(&s);

    refresh_status(active);
    refresh_clock(&r, fresh);
    refresh_gauge(&r, have, active, s.k_alert_mM);
    refresh_ph(&r, have, active);
}

/* ------------------------------------------------------------------
   Slow refresh (every 2 s): sparkline + trend
   ------------------------------------------------------------------ */

float ui_k_rate_per_10min(void)
{
    /* Mean of the newest 30 s compared with the mean of a 30 s block 10 min
       earlier (or the oldest block available, if at least 90 s back),
       scaled to mM per 10 min. Works in seconds, not samples, so the
       front end's sample rate does not matter. */
    enum { WIN = 30, LAG = 600, SPAN = LAG + WIN, MIN_LAG = 90 };

    static float *buf;

    if(buf == NULL)
    {
        buf = heap_caps_malloc(SPAN * sizeof(float), MALLOC_CAP_SPIRAM);

        if(buf == NULL)
        {
            return NAN;
        }
    }

    if(app_state_history(buf, NULL, SPAN) == 0)
    {
        return NAN;
    }

    int first = 0;

    while(first < SPAN && !isfinite(buf[first]))
    {
        first++;
    }

    int lag = LAG - first;

    if(lag < MIN_LAG)
    {
        return NAN;
    }

    double a = 0.0, b = 0.0;
    int na = 0, nb = 0;

    for(int i = SPAN - WIN; i < SPAN; i++)
    {
        if(isfinite(buf[i])) { a += buf[i]; na++; }
    }

    for(int i = first; i < first + WIN; i++)
    {
        if(isfinite(buf[i])) { b += buf[i]; nb++; }
    }

    if(na < WIN / 2 || nb < WIN / 2)
    {
        return NAN;
    }

    return (float)((a / na - b / nb) * (double)LAG / lag);
}

void ui_face_refresh_slow(void)
{
    if(hist == NULL)
    {
        return;
    }

    int have = app_state_history(hist, NULL, HISTORY_LEN);
    const int n = HISTORY_LEN;   /* always a full, time-aligned window */
    int32_t *y = lv_chart_get_series_y_array(spark, spark_ser);
    int per = HISTORY_LEN / SPARK_POINTS;
    float lo = INFINITY, hi = -INFINITY;
    int finite = 0;

    /* Right-aligned: the newest data is always at the right edge. */
    for(int p = 0; p < SPARK_POINTS; p++)
    {
        int from = n - HISTORY_LEN + p * per;
        double sum = 0.0;
        int cnt = 0;

        for(int i = from; i < from + per; i++)
        {
            if(i >= 0 && i < n && isfinite(hist[i])) { sum += hist[i]; cnt++; }
        }

        if(cnt == 0)
        {
            y[p] = LV_CHART_POINT_NONE;
            continue;
        }

        float v = (float)(sum / cnt);
        y[p] = (int32_t)lroundf(v * 100.0f);
        finite++;
        lo = fminf(lo, v);
        hi = fmaxf(hi, v);
    }

    if(!isfinite(lo))
    {
        lo = 0.0f;
        hi = 1.0f;
    }

    /* Minimum 1 mM span so sensor noise does not look like a trend. */
    if(hi - lo < 1.0f)
    {
        float mid = (hi + lo) / 2.0f;
        lo = mid - 0.5f;
        hi = mid + 0.5f;
    }

    lv_chart_set_axis_range(spark, LV_CHART_AXIS_PRIMARY_Y,
                            (int32_t)floorf(lo * 100.0f), (int32_t)ceilf(hi * 100.0f));
    lv_chart_refresh(spark);

    float rate = ui_k_rate_per_10min();
    char buf[48];

    if(!isfinite(rate))
    {
        lv_label_set_text(trend_lbl, !have ? "" : finite ? "collecting trend..." : "no calibrated data");
        lv_obj_set_style_text_color(trend_lbl, UI_COL_DIM, 0);
    }
    else if(fabsf(rate) < RATE_NOTABLE)
    {
        lv_label_set_text(trend_lbl, LV_SYMBOL_MINUS "  stable");
        lv_obj_set_style_text_color(trend_lbl, UI_COL_DIM, 0);
    }
    else
    {
        snprintf(buf, sizeof(buf), "%s  %+.1f mM / 10 min",
                 rate > 0 ? LV_SYMBOL_UP : LV_SYMBOL_DOWN, (double)rate);
        lv_label_set_text(trend_lbl, buf);
        lv_obj_set_style_text_color(trend_lbl,
            rate >= RATE_FAST ? UI_COL_ALERT : rate > 0 ? UI_COL_WARN : UI_COL_OK, 0);
    }
}
