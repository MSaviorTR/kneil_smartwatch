#include <math.h>
#include <stdio.h>

#include "app_state.h"

#include "ui_internal.h"

/*
 * Full-screen K+ alert on the top layer, so it covers whichever page is
 * open. Appears when the alert starts, pulses, and can be dismissed; it
 * comes back only on the next new alert (after the level has dropped
 * below the threshold minus hysteresis and risen again).
 */

#define PULSE_MS 900

static lv_obj_t *overlay;
static lv_obj_t *value_lbl;
static lv_obj_t *rate_lbl;
static lv_obj_t *thr_lbl;

static bool shown;
static bool dismissed;
static bool was_active;

#define RED_BRIGHT 0xb81c1c
#define RED_DEEP   0x5c0a0a

/* Pulses the colour, not the opacity: a translucent overlay would let
   the watch face show through and look like a rendering fault. */
static void pulse_cb(void *obj, int32_t v)
{
    lv_obj_set_style_bg_color(obj, lv_color_mix(lv_color_hex(RED_BRIGHT), lv_color_hex(RED_DEEP), (uint8_t)v), 0);
}

static void show(void)
{
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(overlay);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, overlay);
    lv_anim_set_exec_cb(&a, pulse_cb);
    lv_anim_set_values(&a, 255, 120);
    lv_anim_set_duration(&a, PULSE_MS);
    lv_anim_set_reverse_duration(&a, PULSE_MS);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);

    shown = true;
}

static void hide(void)
{
    lv_anim_delete(overlay, pulse_cb);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    shown = false;
}

static void dismiss_cb(lv_event_t *e)
{
    (void)e;
    dismissed = true;
    hide();
}

static void see_trend_cb(lv_event_t *e)
{
    (void)e;
    dismissed = true;
    hide();
    ui_goto_page(UI_PAGE_TREND);
}

static lv_obj_t *overlay_button(const char *text, lv_event_cb_t cb, bool filled)
{
    lv_obj_t *b = lv_button_create(overlay);

    lv_obj_set_size(b, 300, 58);
    lv_obj_set_style_radius(b, 29, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, filled ? UI_COL_TEXT : lv_color_hex(0x5a0d0d), 0);
    lv_obj_set_style_border_color(b, UI_COL_TEXT, 0);
    lv_obj_set_style_border_width(b, filled ? 0 : 2, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *l = ui_label(b, text, &lv_font_montserrat_20,
                           filled ? lv_color_hex(0x7a1010) : UI_COL_TEXT);
    lv_obj_center(l);

    return b;
}

void ui_alert_create(void)
{
    overlay = lv_obj_create(lv_layer_top());

    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay, lv_color_hex(RED_BRIGHT), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(overlay, UI_COL_TEXT, 0);
    lv_obj_set_style_pad_all(overlay, 24, 0);
    lv_obj_set_style_pad_row(overlay, 6, 0);
    lv_obj_set_flex_flow(overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);     /* swallow taps behind it */
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    ui_label(overlay, LV_SYMBOL_WARNING, &lv_font_montserrat_48, UI_COL_TEXT);
    ui_label(overlay, "K+ HIGH", &lv_font_montserrat_26, UI_COL_TEXT);

    lv_obj_t *gap = lv_obj_create(overlay);
    lv_obj_remove_style_all(gap);
    lv_obj_set_size(gap, 1, 6);

    value_lbl = ui_label(overlay, "--", &lv_font_montserrat_48, UI_COL_TEXT);
    ui_label(overlay, "mM", &lv_font_montserrat_18, lv_color_hex(0xffd0d0));

    gap = lv_obj_create(overlay);
    lv_obj_remove_style_all(gap);
    lv_obj_set_size(gap, 1, 10);

    rate_lbl = ui_label(overlay, "", &lv_font_montserrat_18, UI_COL_TEXT);
    thr_lbl  = ui_label(overlay, "", &lv_font_montserrat_16, lv_color_hex(0xffd0d0));

    gap = lv_obj_create(overlay);
    lv_obj_remove_style_all(gap);
    lv_obj_set_size(gap, 1, 18);

    overlay_button(LV_SYMBOL_IMAGE "  See trend", see_trend_cb, true);
    overlay_button("Dismiss", dismiss_cb, false);

    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
}

void ui_alert_update(void)
{
    reading_t r = {0};
    bool have = app_state_latest(&r);
    bool active = have && app_state_active_source() != DATA_SRC_NONE && r.k_alert;

    /* A new alert episode re-arms the overlay even if the last one was
       dismissed. */
    if(active && !was_active)
    {
        dismissed = false;
    }

    was_active = active;

    if(!active)
    {
        if(shown)
        {
            hide();
        }

        return;
    }

    if(!shown && !dismissed)
    {
        show();
    }

    if(!shown)
    {
        return;
    }

    char buf[48];

    ui_fmt(buf, sizeof(buf), r.k_mM, 2);
    lv_label_set_text(value_lbl, buf);

    /* The rate needs a history copy; once a second is plenty. */
    static unsigned n;

    if(n++ % 4 == 0)
    {
        float rate = ui_k_rate_per_10min();

        if(isfinite(rate))
        {
            snprintf(buf, sizeof(buf), "%s %+.1f mM in 10 min",
                     rate >= 0 ? LV_SYMBOL_UP : LV_SYMBOL_DOWN, (double)rate);
            lv_label_set_text(rate_lbl, buf);
        }
        else
        {
            lv_label_set_text(rate_lbl, "");
        }

        app_settings_t s;
        app_state_get_settings(&s);
        snprintf(buf, sizeof(buf), "alert threshold %.1f mM%s", (double)s.k_alert_mM,
                 r.source == DATA_SRC_DEMO ? "  -  DEMO" : "");
        lv_label_set_text(thr_lbl, buf);
    }
}
