#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_state.h"
#include "data_logger.h"
#include "haptics.h"
#include "rtc_service.h"

#include "ui_internal.h"

/*
 * Workflow: pick channel -> put electrode in a standard -> wait for
 * STABLE -> pick that standard in the roller -> Capture. Repeat for 2-5
 * standards, then Fit & save. Each capture is the mean potential over
 * the last WINDOW_S seconds.
 */

#define WINDOW_S        30
#define MIN_SAMPLES     5

/* Stability: thresholds over the capture window. Starting points; the
   right values depend on the membrane and the front end's noise. */
#define STABLE_SD_MV    0.5f
#define STABLE_DRIFT_MV 1.0f

typedef struct
{
    const char  *roller;      /* options shown */
    const float *values;      /* standard values (mM for K+, pH for pH) */
    int          count;
    const char  *unit;        /* for point list */
    const char  *slope_unit;
} standards_t;

static const float K_STD[]  = { 0.1f, 1.0f, 10.0f, 100.0f };
static const float PH_STD[] = { 4.01f, 6.86f, 7.00f, 9.18f, 10.01f };

static const standards_t STD[CAL_CHANNEL_COUNT] = {
    [CAL_CHANNEL_K]  = { "0.1 mM\n1 mM\n10 mM\n100 mM", K_STD, 4, "mM", "mV/decade" },
    [CAL_CHANNEL_PH] = { "pH 4.01\npH 6.86\npH 7.00\npH 9.18\npH 10.01", PH_STD, 5, "", "mV/pH" },
};

static cal_channel_t channel = CAL_CHANNEL_K;
static cal_point_t   points[CAL_CHANNEL_COUNT][CAL_MAX_POINTS];
static float         point_std[CAL_CHANNEL_COUNT][CAL_MAX_POINTS];
static bool          point_unstable[CAL_CHANNEL_COUNT][CAL_MAX_POINTS];
static int           npoints[CAL_CHANNEL_COUNT];

static cal_fit_t     pending_fit;

static lv_obj_t *k_btn;
static lv_obj_t *ph_btn;
static lv_obj_t *fit_lbl;
static lv_obj_t *live_lbl;
static lv_obj_t *blocked_lbl;
static lv_obj_t *roller;
static lv_obj_t *capture_btn;
static lv_obj_t *points_lbl;
static lv_obj_t *fit_btn;
static lv_obj_t *mbox;

/* ------------------------------------------------------------------ */

static float std_to_x(cal_channel_t ch, float v)
{
    return (ch == CAL_CHANNEL_K) ? log10f(v) : v;
}

static void set_enabled(lv_obj_t *obj, bool on)
{
    if(on) lv_obj_remove_state(obj, LV_STATE_DISABLED);
    else   lv_obj_add_state(obj, LV_STATE_DISABLED);
}

static void describe_fit(char *buf, size_t len, cal_channel_t ch, const cal_fit_t *f)
{
    if(!f->valid)
    {
        snprintf(buf, len, "Not calibrated");
        return;
    }

    char when[24] = "";

    if(f->fitted_at > 0)
    {
        time_t t = (time_t)f->fitted_at;
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(when, sizeof(when), "  %d %b %H:%M", &tm);
    }

    snprintf(buf, len, "Saved: %.1f %s (%.0f %%)  R2 %.4f%s",
             (double)f->slope, STD[ch].slope_unit, (double)cal_slope_percent(f),
             (double)f->r2, when);
}

static void update_points_label(void)
{
    char buf[256];
    size_t used = 0;

    buf[0] = '\0';

    for(int i = 0; i < npoints[channel] && used < sizeof(buf); i++)
    {
        int n = snprintf(buf + used, sizeof(buf) - used, "%s%g %s  ->  %.2f mV%s",
                         i ? "\n" : "",
                         (double)point_std[channel][i], STD[channel].unit,
                         (double)points[channel][i].mv,
                         point_unstable[channel][i] ? "  (unstable)" : "");

        if(n > 0) used += (size_t)n;
    }

    lv_label_set_text(points_lbl, npoints[channel] ? buf : "No points captured yet");

    /* Need two different standards for a line. */
    bool distinct = false;

    for(int i = 1; i < npoints[channel]; i++)
    {
        if(points[channel][i].x != points[channel][0].x)
        {
            distinct = true;
        }
    }

    set_enabled(fit_btn, distinct);
}

static void select_channel(cal_channel_t ch)
{
    channel = ch;

    if(ch == CAL_CHANNEL_K)
    {
        lv_obj_add_state(k_btn, LV_STATE_CHECKED);
        lv_obj_remove_state(ph_btn, LV_STATE_CHECKED);
    }
    else
    {
        lv_obj_add_state(ph_btn, LV_STATE_CHECKED);
        lv_obj_remove_state(k_btn, LV_STATE_CHECKED);
    }

    lv_roller_set_options(roller, STD[ch].roller, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_selected(roller, 1, LV_ANIM_OFF);

    update_points_label();
    ui_calib_refresh();
}

/* ------------------------------------------------------------------
   Events
   ------------------------------------------------------------------ */

static void channel_cb(lv_event_t *e)
{
    select_channel((cal_channel_t)(intptr_t)lv_event_get_user_data(e));
}

static void capture_cb(lv_event_t *e)
{
    (void)e;

    window_stats_t ws;

    if(app_state_active_source() != DATA_SRC_SENSOR ||
       !app_state_window_stats(channel, WINDOW_S, &ws) || ws.n < MIN_SAMPLES)
    {
        return;
    }

    uint32_t sel = lv_roller_get_selected(roller);
    float std_value = STD[channel].values[sel];
    float x = std_to_x(channel, std_value);
    bool unstable = ws.sd_mv > STABLE_SD_MV || fabsf(ws.drift_mv) > STABLE_DRIFT_MV;

    /* Re-capturing the same standard replaces the earlier point. */
    int idx = -1;

    for(int i = 0; i < npoints[channel]; i++)
    {
        if(points[channel][i].x == x)
        {
            idx = i;
        }
    }

    if(idx < 0)
    {
        if(npoints[channel] >= CAL_MAX_POINTS)
        {
            return;
        }

        idx = npoints[channel]++;
    }

    points[channel][idx].x       = x;
    points[channel][idx].mv      = ws.mean_mv;
    point_std[channel][idx]      = std_value;
    point_unstable[channel][idx] = unstable;

    haptics_tap();
    update_points_label();
}

static void clear_cb(lv_event_t *e)
{
    (void)e;
    npoints[channel] = 0;
    update_points_label();
}

static void mbox_cb(lv_event_t *e)
{
    bool save = (bool)(intptr_t)lv_event_get_user_data(e);

    if(save)
    {
        pending_fit.fitted_at = rtc_service_time_valid() ? (int64_t)time(NULL) : 0;

        if(app_state_set_calibration(channel, &pending_fit) == ESP_OK)
        {
            data_logger_log_calibration(channel, &pending_fit, points[channel], npoints[channel]);
            npoints[channel] = 0;
            haptics_tap();
        }
    }

    lv_msgbox_close_async(mbox);   /* deleting inside its own event is unsafe */
    mbox = NULL;

    update_points_label();
    ui_calib_refresh();
}

static const char *verdict(cal_channel_t ch, const cal_fit_t *f)
{
    float pct = cal_slope_percent(f);
    bool sign_ok = (ch == CAL_CHANNEL_K) ? f->slope > 0 : f->slope < 0;

    if(!sign_ok)
    {
        return "Slope has the wrong sign. Check the standards were picked "
               "correctly and the electrode leads are not swapped.";
    }

    if(f->r2 < 0.995f)
    {
        return "Poor linearity. Re-measure the point that looks off, "
               "or check for a drifting reference.";
    }

    if(pct < 90.0f)
    {
        return "Sub-Nernstian. Membrane may need more conditioning, or "
               "the low standard is below the detection limit.";
    }

    if(pct > 110.0f)
    {
        return "Super-Nernstian. Check standard concentrations and for "
               "leakage / interferents.";
    }

    return "Looks healthy.";
}

static void fit_cb(lv_event_t *e)
{
    (void)e;

    if(!cal_fit_linear(points[channel], npoints[channel], &pending_fit))
    {
        return;
    }

    char text[320];
    snprintf(text, sizeof(text),
             "Slope  %.2f %s\n(%.1f %% of Nernstian)\nIntercept  %.2f mV\nR2  %.5f   (%d points)\n\n%s",
             (double)pending_fit.slope, STD[channel].slope_unit,
             (double)cal_slope_percent(&pending_fit),
             (double)pending_fit.intercept, (double)pending_fit.r2,
             npoints[channel], verdict(channel, &pending_fit));

    mbox = lv_msgbox_create(NULL);
    lv_msgbox_add_title(mbox, channel == CAL_CHANNEL_K ? "K+ calibration" : "pH calibration");
    lv_msgbox_add_text(mbox, text);

    lv_obj_t *save = lv_msgbox_add_footer_button(mbox, "Save");
    lv_obj_add_event_cb(save, mbox_cb, LV_EVENT_CLICKED, (void *)(intptr_t)true);

    lv_obj_t *discard = lv_msgbox_add_footer_button(mbox, "Discard");
    lv_obj_add_event_cb(discard, mbox_cb, LV_EVENT_CLICKED, (void *)(intptr_t)false);

    lv_obj_set_width(mbox, UI_W - 40);
}

/* ------------------------------------------------------------------ */

static lv_obj_t *channel_button(lv_obj_t *parent, const char *text, cal_channel_t ch, lv_color_t accent)
{
    lv_obj_t *b = ui_button(parent, text, channel_cb, (void *)(intptr_t)ch);

    /* Not LV_OBJ_FLAG_CHECKABLE: the CHECKED state is set by
       select_channel(), so tapping the active one cannot uncheck it. */
    lv_obj_set_size(b, 170, 50);
    lv_obj_set_style_bg_color(b, UI_COL_CARD, 0);
    lv_obj_set_style_bg_color(b, accent, LV_STATE_CHECKED);

    return b;
}

void ui_calib_create(void)
{
    ui_scr_calib = ui_screen_create("CALIBRATE");
    lv_obj_set_style_pad_row(ui_scr_calib, 10, 0);

    lv_obj_t *row = ui_row(ui_scr_calib);
    k_btn  = channel_button(row, "K+", CAL_CHANNEL_K, UI_COL_K);
    ph_btn = channel_button(row, "pH", CAL_CHANNEL_PH, UI_COL_PH);

    fit_lbl = ui_label(ui_scr_calib, "", &lv_font_montserrat_14, UI_COL_DIM);
    lv_obj_set_width(fit_lbl, LV_PCT(100));
    lv_label_set_long_mode(fit_lbl, LV_LABEL_LONG_WRAP);

    live_lbl = ui_label(ui_scr_calib, "", &lv_font_montserrat_18, UI_COL_TEXT);
    lv_obj_set_width(live_lbl, LV_PCT(100));
    lv_obj_set_style_text_align(live_lbl, LV_TEXT_ALIGN_CENTER, 0);

    blocked_lbl = ui_label(ui_scr_calib,
                           "Needs live sensor data.\nConnect the sensor MCU and turn demo mode off.",
                           &lv_font_montserrat_16, UI_COL_ALERT);
    lv_obj_set_width(blocked_lbl, LV_PCT(100));
    lv_obj_set_style_text_align(blocked_lbl, LV_TEXT_ALIGN_CENTER, 0);

    roller = lv_roller_create(ui_scr_calib);
    lv_roller_set_visible_row_count(roller, 3);
    lv_obj_set_width(roller, 240);
    lv_obj_set_style_text_font(roller, &lv_font_montserrat_22, 0);
    lv_obj_set_style_bg_color(roller, UI_COL_CARD, 0);
    lv_obj_set_style_text_color(roller, UI_COL_TEXT, 0);

    capture_btn = ui_button(ui_scr_calib, LV_SYMBOL_DOWNLOAD "  Capture standard", capture_cb, NULL);

    points_lbl = ui_label(ui_scr_calib, "", &lv_font_montserrat_16, UI_COL_TEXT);
    lv_obj_set_width(points_lbl, LV_PCT(100));

    row = ui_row(ui_scr_calib);
    fit_btn = ui_button(row, LV_SYMBOL_OK "  Fit & save", fit_cb, NULL);
    lv_obj_set_size(fit_btn, 180, 54);

    lv_obj_t *clear = ui_button(row, LV_SYMBOL_TRASH "  Clear", clear_cb, NULL);
    lv_obj_set_size(clear, 180, 54);

    ui_button(ui_scr_calib, LV_SYMBOL_LEFT "  Back", ui_go_menu_cb, NULL);

    select_channel(CAL_CHANNEL_K);
}

void ui_calib_refresh(void)
{
    char buf[160];
    cal_fit_t f;

    app_state_get_calibration(channel, &f);
    describe_fit(buf, sizeof(buf), channel, &f);
    lv_label_set_text(fit_lbl, buf);

    bool live = app_state_active_source() == DATA_SRC_SENSOR;

    if(live)
    {
        lv_obj_add_flag(blocked_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_remove_flag(blocked_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    window_stats_t ws;
    bool have = live && app_state_window_stats(channel, WINDOW_S, &ws) && ws.n >= MIN_SAMPLES;

    if(have)
    {
        bool stable = ws.sd_mv <= STABLE_SD_MV && fabsf(ws.drift_mv) <= STABLE_DRIFT_MV;

        snprintf(buf, sizeof(buf), "%.2f mV   sd %.2f   drift %+.2f\n%s",
                 (double)ws.mean_mv, (double)ws.sd_mv, (double)ws.drift_mv,
                 stable ? LV_SYMBOL_OK " STABLE" : "settling...");
        lv_label_set_text(live_lbl, buf);
        lv_obj_set_style_text_color(live_lbl, stable ? UI_COL_OK : UI_COL_WARN, 0);
    }
    else
    {
        lv_label_set_text(live_lbl, live ? "collecting samples..." : "");
        lv_obj_set_style_text_color(live_lbl, UI_COL_DIM, 0);
    }

    set_enabled(capture_btn, have);
}
