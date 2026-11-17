/*
 * KNEIL page: live view of the implant monitor (BLE central in main/kneil_central.c).
 * Shown instead of the watch face when Settings -> Mode is "KNEIL device".
 *
 * Everything here comes from kneil_model_t (main/kneil_state.h), which the central updates from the
 * LIVE / INFO / SUMMARY notifications. Nothing on this page touches the K+/pH pipeline.
 */

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"

#include "app_state.h"
#include "kneil_central.h"
#include "kneil_state.h"
#include "ui_internal.h"

static lv_obj_t *link_lbl;
static lv_obj_t *meta_lbl;
static lv_obj_t *class_box;
static lv_obj_t *class_lbl;
static lv_obj_t *age_lbl;
static lv_obj_t *raw_lbl;
static lv_obj_t *posture_lbl;
static lv_obj_t *ev_sts_lbl;
static lv_obj_t *ev_s2s_lbl;
static lv_obj_t *ev_bed_lbl;
static lv_obj_t *hist_cells[KNEIL_HISTORY];
static lv_obj_t *summary_lbl;
static lv_obj_t *info_lbl;
static lv_obj_t *confirm_box;

static int64_t last_refresh_us;

/* ------------------------------------------------------------------ */

bool ui_kneil_active(void)
{
    app_settings_t s;

    app_state_get_settings(&s);

    return s.ble_mode == BLE_MODE_KNEIL;
}

static void summary_now_cb(lv_event_t *e)
{
    (void)e;
    kneil_central_send_ctrl(KNEIL_CTRL_SUMMARY);
}

static void selftest_cb(lv_event_t *e)
{
    (void)e;
    kneil_central_send_ctrl(KNEIL_CTRL_SELFTEST);
}

static void confirm_close(void)
{
    if(confirm_box != NULL)
    {
        lv_obj_delete(confirm_box);
        confirm_box = NULL;
    }
}

static void confirm_cancel_cb(lv_event_t *e)
{
    (void)e;
    confirm_close();
}

static void confirm_reset_cb(lv_event_t *e)
{
    (void)e;
    kneil_central_send_ctrl(KNEIL_CTRL_RESET);
    confirm_close();
}

/* "Reset day" asks first: the daily summary on the device is erased. */
static void reset_day_cb(lv_event_t *e)
{
    (void)e;

    if(confirm_box != NULL)
    {
        return;
    }

    confirm_box = lv_obj_create(lv_layer_top());
    lv_obj_set_size(confirm_box, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(confirm_box, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(confirm_box, LV_OPA_70, 0);
    lv_obj_set_style_border_width(confirm_box, 0, 0);
    lv_obj_set_style_radius(confirm_box, 0, 0);
    lv_obj_remove_flag(confirm_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = ui_card(confirm_box);
    lv_obj_set_width(card, LV_PCT(88));
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    ui_label(card, "Reset the daily summary", &lv_font_montserrat_20, UI_COL_TEXT);
    ui_label(card, "The KNEIL device clears its minutes and\nevent counters. This cannot be undone.",
             &lv_font_montserrat_14, UI_COL_DIM);
    ui_button(card, "Reset", confirm_reset_cb, NULL);
    ui_button(card, "Cancel", confirm_cancel_cb, NULL);
}

/* ------------------------------------------------------------------ */

void ui_kneil_create(lv_obj_t *tile)
{
    ui_page(UI_PAGE_FACE, LV_SYMBOL_BLUETOOTH, "KNEIL", UI_COL_BLE);

    /* link */
    lv_obj_t *card = ui_card(tile);
    link_lbl = ui_label(card, "not started", &lv_font_montserrat_20, UI_COL_TEXT);
    meta_lbl = ui_label(card, "-", &lv_font_montserrat_14, UI_COL_DIM);
    lv_label_set_long_mode(meta_lbl, LV_LABEL_LONG_WRAP);   /* two lines at 410 px */
    lv_obj_set_width(meta_lbl, LV_PCT(100));

    /* the decision */
    class_box = lv_obj_create(tile);
    lv_obj_set_size(class_box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(class_box, UI_COL_TRACK, 0);
    lv_obj_set_style_radius(class_box, 20, 0);
    lv_obj_set_style_border_width(class_box, 0, 0);
    lv_obj_set_style_pad_all(class_box, 16, 0);
    lv_obj_set_flex_flow(class_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(class_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(class_box, LV_OBJ_FLAG_SCROLLABLE);
    class_lbl = ui_label(class_box, "no data", &lv_font_montserrat_26, UI_COL_TEXT);
    age_lbl = ui_label(class_box, "-", &lv_font_montserrat_14, UI_COL_DIM);
    raw_lbl = ui_label(class_box, "-", &lv_font_montserrat_14, UI_COL_DIM);
    posture_lbl = ui_label(class_box, "-", &lv_font_montserrat_14, UI_COL_DIM);

    /* events */
    lv_obj_t *ev = ui_card(tile);
    lv_obj_t *r = ui_row(ev);
    ui_label(r, "sit-to-stand", &lv_font_montserrat_14, UI_COL_DIM);
    ev_sts_lbl = ui_label(r, "0", &lv_font_montserrat_20, UI_COL_TEXT);
    r = ui_row(ev);
    ui_label(r, "stand-to-sit", &lv_font_montserrat_14, UI_COL_DIM);
    ev_s2s_lbl = ui_label(r, "0", &lv_font_montserrat_20, UI_COL_TEXT);
    r = ui_row(ev);
    ui_label(r, "bed transfer", &lv_font_montserrat_14, UI_COL_DIM);
    ev_bed_lbl = ui_label(r, "0", &lv_font_montserrat_20, UI_COL_TEXT);

    /* history strip: 60 LIVE packets, newest on the right */
    lv_obj_t *strip = lv_obj_create(tile);
    lv_obj_remove_style_all(strip);
    lv_obj_set_size(strip, LV_PCT(100), 22);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(strip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(strip, 1, 0);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_SCROLLABLE);

    for(int i = 0; i < KNEIL_HISTORY; i++)
    {
        hist_cells[i] = lv_obj_create(strip);
        lv_obj_remove_style_all(hist_cells[i]);
        lv_obj_set_size(hist_cells[i], 4, 18);
        lv_obj_set_style_radius(hist_cells[i], 1, 0);
        lv_obj_set_style_bg_opa(hist_cells[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(hist_cells[i], UI_COL_TRACK, 0);
    }

    /* daily summary and last INFO line */
    lv_obj_t *sum = ui_card(tile);
    summary_lbl = ui_label(sum, "summary: not received", &lv_font_montserrat_14, UI_COL_TEXT);
    lv_label_set_long_mode(summary_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(summary_lbl, LV_PCT(100));
    info_lbl = ui_label(sum, "info: -", &lv_font_montserrat_14, UI_COL_DIM);
    lv_label_set_long_mode(info_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(info_lbl, LV_PCT(100));

    /* actions */
    ui_button(tile, "Summary now", summary_now_cb, NULL);
    ui_button(tile, "Self-test", selftest_cb, NULL);
    ui_button(tile, "Reset day", reset_day_cb, NULL);
}

/* ------------------------------------------------------------------ */

void ui_kneil_refresh(void)
{
    kneil_model_t m;

    kneil_central_snapshot(&m);

    /* link line */
    char buf[64];

    if(m.link == KNEIL_LINK_CONNECTED)
    {
        snprintf(buf, sizeof(buf), "connected  %s", m.name[0] ? m.name : "KNEIL");
    }
    else if(m.link == KNEIL_LINK_LOST)
    {
        snprintf(buf, sizeof(buf), "lost - reconnecting");
    }
    else
    {
        snprintf(buf, sizeof(buf), "%s", kneil_link_text(m.link));
    }

    lv_label_set_text(link_lbl, buf);
    lv_obj_set_style_text_color(link_lbl,
                                (m.link == KNEIL_LINK_CONNECTED) ? UI_COL_OK :
                                (m.link == KNEIL_LINK_LOST) ? UI_COL_ALERT : UI_COL_TEXT, 0);

    if(m.version == 0)
    {
        snprintf(buf, sizeof(buf), "RSSI %d dBm   MTU %u   packets %lu   gaps %lu",
                 m.rssi, (unsigned)m.mtu, (unsigned long)m.packets, (unsigned long)m.gaps);
    }
    else
    {
        snprintf(buf, sizeof(buf), "RSSI %d dBm   MTU %u   v%u (%d classes)   packets %lu   gaps %lu",
                 m.rssi, (unsigned)m.mtu, (unsigned)m.version, kneil_version_classes(m.version),
                 (unsigned long)m.packets, (unsigned long)m.gaps);
    }

    lv_label_set_text(meta_lbl, buf);

    /* the decision */
    const int64_t now = esp_timer_get_time();
    const int age = kneil_model_live_age_s(&m, now);

    if(m.cls >= 0 && m.cls != KNEIL_CLASS_NONE)
    {
        lv_label_set_text(class_lbl, kneil_class_text(m.cls, m.version));
        lv_obj_set_style_bg_color(class_box, lv_color_hex(kneil_class_colour(m.cls, m.version)), 0);
        lv_obj_set_style_text_color(class_lbl, lv_color_hex(0x000000), 0);
        lv_obj_set_style_text_color(age_lbl, lv_color_hex(0x000000), 0);
        lv_obj_set_style_text_color(raw_lbl, lv_color_hex(0x000000), 0);
        lv_obj_set_style_text_color(posture_lbl, lv_color_hex(0x000000), 0);
        lv_obj_set_style_text_opa(age_lbl, LV_OPA_70, 0);
        lv_obj_set_style_text_opa(raw_lbl, LV_OPA_70, 0);
        lv_obj_set_style_text_opa(posture_lbl, LV_OPA_70, 0);
    }
    else
    {
        lv_label_set_text(class_lbl, "no data");
        lv_obj_set_style_bg_color(class_box, UI_COL_TRACK, 0);
        lv_obj_set_style_text_color(class_lbl, UI_COL_TEXT, 0);
        lv_obj_set_style_text_color(age_lbl, UI_COL_DIM, 0);
        lv_obj_set_style_text_color(raw_lbl, UI_COL_DIM, 0);
        lv_obj_set_style_text_color(posture_lbl, UI_COL_DIM, 0);
        lv_obj_set_style_text_opa(age_lbl, LV_OPA_COVER, 0);
        lv_obj_set_style_text_opa(raw_lbl, LV_OPA_COVER, 0);
        lv_obj_set_style_text_opa(posture_lbl, LV_OPA_COVER, 0);
    }

    if(age < 0)
    {
        lv_label_set_text(age_lbl, "waiting for the first LIVE packet");
    }
    else
    {
        snprintf(buf, sizeof(buf), "%d s old", age);
        lv_label_set_text(age_lbl, buf);
    }

    if(m.vote_pct == KNEIL_VOTE_NONE)
    {
        snprintf(buf, sizeof(buf), "raw: %s   vote: -", kneil_class_text(m.cls_raw, m.version));
    }
    else
    {
        snprintf(buf, sizeof(buf), "raw: %s   vote: %d %%", kneil_class_text(m.cls_raw, m.version), m.vote_pct);
    }

    lv_label_set_text(raw_lbl, buf);

    if(m.version >= 2)
    {
        snprintf(buf, sizeof(buf), "just before: %s   posture: %s", kneil_class_text(m.cls_smooth, m.version),
                 kneil_posture_text(m.posture));
    }
    else
    {
        snprintf(buf, sizeof(buf), "just before: %s   posture: - (v1)",
                 kneil_class_text(m.cls_smooth, m.version));
    }

    lv_label_set_text(posture_lbl, buf);

    /* events */
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)m.n_sit_to_stand);
    lv_label_set_text(ev_sts_lbl, buf);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)m.n_stand_to_sit);
    lv_label_set_text(ev_s2s_lbl, buf);
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)m.n_bed_transfer);
    lv_label_set_text(ev_bed_lbl, buf);

    /* history strip: oldest on the left, newest on the right */
    for(int i = 0; i < KNEIL_HISTORY; i++)
    {
        const int cls = kneil_model_history_at(&m, KNEIL_HISTORY - 1 - i);

        lv_obj_set_style_bg_color(hist_cells[i],
                                  (cls == KNEIL_CLASS_NONE) ? UI_COL_TRACK
                                                            : lv_color_hex(kneil_class_colour(cls, m.version)), 0);
    }

    /* summary / info every 2 s: they change rarely */
    if(now - last_refresh_us < 2000000)
    {
        return;
    }

    last_refresh_us = now;

    if(m.have_summary)
    {
        const kneil_summary_t *s = &m.summary;
        char sum[192];

        snprintf(sum, sizeof(sum),
                 "walk %u, stairs %u/%u, still %u (sit %u / stand %u / ? %u), lying %u min\n"
                 "sit-to-stand %u, stand-to-sit %u, bed %u, steps %u, cadence %.1f/min",
                 s->minutes_by_class[0], s->minutes_by_class[1], s->minutes_by_class[2],
                 s->minutes_by_class[3], s->minutes_sitting, s->minutes_standing,
                 s->minutes_still_unknown, s->minutes_by_class[4],
                 s->n_sit_to_stand, s->n_stand_to_sit, s->n_bed_transfer, s->steps, s->cadence_x10 / 10.0);
        lv_label_set_text(summary_lbl, sum);
    }
    else
    {
        lv_label_set_text(summary_lbl, "summary: not received (firmware v1 has none)");
    }

    lv_label_set_text(info_lbl, m.have_info ? m.info : "info: -");
}
