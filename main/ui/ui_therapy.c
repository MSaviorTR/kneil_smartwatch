#include <stdio.h>
#include <string.h>

#include "therapy.h"
#include "ui_internal.h"

/*
 * Home therapy screen. Only reachable from the KNEIL page (the only mode in which the implant RAW stream is
 * available). The screen is a separate full-screen page, like the set-time screen, so the watch face and the
 * other pages are untouched.
 */

lv_obj_t *ui_scr_therapy;

static lv_obj_t *angle_lbl;
static lv_obj_t *target_lbl;
static lv_obj_t *rep_lbl;
static lv_obj_t *counts_lbl;
static lv_obj_t *status_lbl;
static lv_obj_t *warn_lbl;
static lv_obj_t *arc;
static lv_obj_t *start_btn;
static lv_obj_t *start_lbl;
static lv_obj_t *ext_btn;
static lv_obj_t *bend_btn;
static lv_obj_t *cal_btn;
static lv_obj_t *straight_btn;

static therapy_exercise_t selected_exercise = THERAPY_EX_EXTENSION;

static void small_button_style(lv_obj_t *b)
{
    lv_obj_set_style_bg_color(b, UI_COL_CARD, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, UI_COL_FAINT, 0);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_text_color(b, UI_COL_TEXT, 0);
}

static lv_obj_t *small_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);

    lv_obj_set_size(b, LV_PCT(48), 44);
    small_button_style(b);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_center(l);
    return b;
}

static void back_cb(lv_event_t *e)
{
    (void)e;
    ui_therapy_close();
}

static void start_cb(lv_event_t *e)
{
    therapy_snapshot_t s;

    (void)e;
    therapy_snapshot(&s);

    if(s.state == THERAPY_STATE_RUNNING)
    {
        therapy_pause();
    }
    else if(s.state == THERAPY_STATE_PAUSED)
    {
        therapy_resume();
    }
    else if(!therapy_start(selected_exercise))
    {
        lv_label_set_text(status_lbl, "Calibrate first, and connect to the implant.");
    }
}

static void stop_cb(lv_event_t *e)
{
    (void)e;
    therapy_stop();
}

static void calibrate_cb(lv_event_t *e)
{
    (void)e;

    if(!therapy_calibrate_start(THERAPY_CAL_SIT))
    {
        lv_label_set_text(status_lbl, "Not connected. Check the implant link.");
    }
}

static void straight_cb(lv_event_t *e)
{
    (void)e;

    if(!therapy_calibrate_start(THERAPY_CAL_STRAIGHT))
    {
        lv_label_set_text(status_lbl, "Calibrate sitting first, and check the link.");
    }
}

static void exercise_cb(lv_event_t *e)
{
    therapy_snapshot_t s;

    therapy_snapshot(&s);

    if(s.state == THERAPY_STATE_RUNNING || s.state == THERAPY_STATE_CALIBRATING)
    {
        return;
    }

    selected_exercise = (lv_event_get_target_obj(e) == bend_btn) ? THERAPY_EX_BEND : THERAPY_EX_EXTENSION;
    lv_obj_set_style_border_color(ext_btn, selected_exercise == THERAPY_EX_EXTENSION ? UI_COL_BLE : UI_COL_FAINT, 0);
    lv_obj_set_style_border_width(ext_btn, selected_exercise == THERAPY_EX_EXTENSION ? 2 : 1, 0);
    lv_obj_set_style_border_color(bend_btn, selected_exercise == THERAPY_EX_BEND ? UI_COL_BLE : UI_COL_FAINT, 0);
    lv_obj_set_style_border_width(bend_btn, selected_exercise == THERAPY_EX_BEND ? 2 : 1, 0);
}

void ui_therapy_create(void)
{
    ui_scr_therapy = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_scr_therapy, UI_COL_BG, 0);
    lv_obj_set_style_bg_opa(ui_scr_therapy, LV_OPA_COVER, 0);
    lv_obj_remove_flag(ui_scr_therapy, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *page = lv_obj_create(ui_scr_therapy);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(page, UI_COL_BG, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(page, 8, 0);
    lv_obj_set_style_pad_row(page, 4, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *head = ui_row(page);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *back = lv_button_create(head);
    lv_obj_set_size(back, 74, 36);
    small_button_style(back);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_16, 0);
    lv_obj_center(bl);
    ui_label(head, "KNEE THERAPY", &lv_font_montserrat_22, UI_COL_BLE);

    lv_obj_t *angle_card = ui_card(page);
    lv_obj_set_style_pad_all(angle_card, 8, 0);
    lv_obj_set_style_pad_row(angle_card, 2, 0);
    angle_lbl = ui_label(angle_card, "--", &lv_font_montserrat_36, UI_COL_TEXT);
    lv_obj_set_style_text_align(angle_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(angle_lbl, LV_PCT(100));
    arc = lv_arc_create(angle_card);
    lv_obj_set_size(arc, 112, 112);
    lv_obj_set_style_margin_all(arc, 0, 0);
    lv_arc_set_range(arc, 0, 140);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, UI_COL_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, UI_COL_BLE, LV_PART_INDICATOR);
    target_lbl = ui_label(angle_card, "target --", &lv_font_montserrat_16, UI_COL_DIM);
    rep_lbl = ui_label(angle_card, "set 1/3   rep 1/10", &lv_font_montserrat_18, UI_COL_TEXT);
    counts_lbl = ui_label(angle_card, "full 0   partial 0", &lv_font_montserrat_14, UI_COL_DIM);
    lv_label_set_long_mode(counts_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(counts_lbl, LV_PCT(100));

    status_lbl = ui_label(page, "Calibrate first.", &lv_font_montserrat_16, UI_COL_TEXT);
    lv_label_set_long_mode(status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(status_lbl, LV_PCT(100));
    lv_obj_set_style_text_align(status_lbl, LV_TEXT_ALIGN_CENTER, 0);

    warn_lbl = ui_label(page, "", &lv_font_montserrat_14, UI_COL_WARN);
    lv_label_set_long_mode(warn_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(warn_lbl, LV_PCT(100));
    lv_obj_set_style_text_align(warn_lbl, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *ex_row = ui_row(page);
    ext_btn = small_button(ex_row, "Extension", exercise_cb);
    bend_btn = small_button(ex_row, "Bend", exercise_cb);
    lv_obj_set_style_border_color(ext_btn, UI_COL_BLE, 0);
    lv_obj_set_style_border_width(ext_btn, 2, 0);

    lv_obj_t *cal_row = ui_row(page);
    cal_btn = small_button(cal_row, "Calibrate", calibrate_cb);
    straight_btn = small_button(cal_row, "Straight check", straight_cb);

    lv_obj_t *run_row = ui_row(page);
    start_btn = small_button(run_row, "Start", start_cb);
    start_lbl = lv_obj_get_child(start_btn, 0);
    small_button(run_row, "Stop", stop_cb);

    lv_obj_t *safety = ui_label(page, "The watch guides only. Stop at sharp pain. Targets come from your "
                                      "physiotherapist.", &lv_font_montserrat_14, UI_COL_WARN);
    lv_label_set_long_mode(safety, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(safety, LV_PCT(100));
}

void ui_therapy_open(void)
{
    if(ui_scr_therapy != NULL)
    {
        lv_screen_load_anim(ui_scr_therapy, LV_SCR_LOAD_ANIM_MOVE_LEFT, 220, 0, false);
    }
}

void ui_therapy_close(void)
{
    therapy_stop();
    lv_screen_load_anim(ui_scr_main, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 220, 0, false);
}

static const char *phase_text(const therapy_snapshot_t *s, char *buf, size_t size)
{
    switch(s->state)
    {
        case THERAPY_STATE_CALIBRATING:
            snprintf(buf, size, "Sit still, foot flat, knee at 90 deg. %d%%", s->cal_progress_pct);
            return buf;
        case THERAPY_STATE_PAUSED:
            return "Paused.";
        case THERAPY_STATE_LOST:
            return "Connection lost. Rest for a moment; the watch will reconnect.";
        case THERAPY_STATE_DONE:
            return "Session complete. Well done.";
        default:
            break;
    }

    if(s->straight_checked && !s->orientation_ok)
    {
        return "Sensor orientation not as expected. Check the watch.";
    }

    switch(s->phase)
    {
        case THERAPY_PHASE_SET_REST:
            snprintf(buf, size, "Set complete. Rest %d s.", s->rest_remaining_s);
            return buf;
        case THERAPY_PHASE_HOLD:
            snprintf(buf, size, "Hold. %.0f / %d s.", (double)s->hold_s, THERAPY_HOLD_SECONDS);
            return buf;
        case THERAPY_PHASE_RETURN:
            return (s->exercise == THERAPY_EX_EXTENSION) ? "Lower slowly." : "Straighten slowly.";
        case THERAPY_PHASE_MOVING:
            return (s->exercise == THERAPY_EX_EXTENSION) ? "Straighten slowly to the target." :
                                                           "Slide the foot back slowly.";
        case THERAPY_PHASE_REST:
            return (s->exercise == THERAPY_EX_EXTENSION) ? "Straighten your knee slowly." :
                                                           "Bend your knee, slide the foot back.";
        default:
            break;
    }

    if(!s->calibrated) return "Calibrate first: sit with the foot flat, knee at 90 deg.";
    if(!s->link_ok) return "No implant link.";
    return "Ready. Press Start.";
}

void ui_therapy_refresh(void)
{
    therapy_snapshot_t s;
    char buf[96];
    char phase[96];

    therapy_tick();
    therapy_snapshot(&s);

    lv_label_set_text(start_lbl, (s.state == THERAPY_STATE_RUNNING) ? "Pause" :
                                 (s.state == THERAPY_STATE_PAUSED) ? "Resume" : "Start");

    if(s.angle_valid)
    {
        snprintf(buf, sizeof(buf), "%.0f deg", (double)s.phi);
    }
    else
    {
        snprintf(buf, sizeof(buf), "--");
    }

    lv_label_set_text(angle_lbl, buf);
    lv_obj_set_style_text_color(angle_lbl, s.angle_valid ? UI_COL_TEXT : UI_COL_WARN, 0);

    snprintf(buf, sizeof(buf), "target %.0f deg", (double)s.target);
    lv_label_set_text(target_lbl, buf);

    snprintf(buf, sizeof(buf), "set %d/%d   rep %d/%d", s.set, THERAPY_SETS,
             (s.reps_done < THERAPY_REPS) ? s.reps_done + 1 : THERAPY_REPS, THERAPY_REPS);
    lv_label_set_text(rep_lbl, buf);

    if(s.straight_checked)
    {
        snprintf(buf, sizeof(buf), "full %d partial %d  best ext %.0f / flex %.0f  straight %.0f",
                 s.full_total, s.partial_total, (double)s.best_extension, (double)s.best_flexion,
                 (double)s.straight_extension_deg);
    }
    else
    {
        snprintf(buf, sizeof(buf), "full %d   partial %d   best ext %.0f / flex %.0f",
                 s.full_total, s.partial_total, (double)s.best_extension, (double)s.best_flexion);
    }
    lv_label_set_text(counts_lbl, buf);

    lv_arc_set_value(arc, (int)((s.phi < 0) ? 0 : (s.phi > 140 ? 140 : s.phi)));

    const char *txt = phase_text(&s, phase, sizeof(phase));

    if(!s.angle_valid && s.state == THERAPY_STATE_RUNNING)
    {
        txt = "Keep the leg straight ahead.";
    }

    lv_label_set_text(status_lbl, txt);

    const bool can_start = (s.state == THERAPY_STATE_RUNNING) || (s.state == THERAPY_STATE_PAUSED) ||
                           (s.calibrated && s.orientation_ok && s.link_ok &&
                            s.state != THERAPY_STATE_CALIBRATING);

    if(can_start)
    {
        lv_obj_remove_state(start_btn, LV_STATE_DISABLED);
    }
    else
    {
        lv_obj_add_state(start_btn, LV_STATE_DISABLED);
    }

    char warn[96] = "";

    if(!s.audio_ready && !s.log_ready)
    {
        snprintf(warn, sizeof(warn), "Speaker off: vibration cues. Log not saved.");
    }
    else if(!s.audio_ready)
    {
        snprintf(warn, sizeof(warn), "Speaker off: vibration cues.");
    }
    else if(!s.log_ready)
    {
        snprintf(warn, sizeof(warn), "Log not saved.");
    }

    if(warn[0])
    {
        lv_label_set_text(warn_lbl, warn);
        lv_obj_remove_flag(warn_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_label_set_text(warn_lbl, "");
        lv_obj_add_flag(warn_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    if(s.state == THERAPY_STATE_RUNNING || s.state == THERAPY_STATE_CALIBRATING)
    {
        lv_obj_add_state(ext_btn, LV_STATE_DISABLED);
        lv_obj_add_state(bend_btn, LV_STATE_DISABLED);
        lv_obj_add_state(cal_btn, LV_STATE_DISABLED);
        lv_obj_add_state(straight_btn, LV_STATE_DISABLED);
    }
    else
    {
        lv_obj_remove_state(ext_btn, LV_STATE_DISABLED);
        lv_obj_remove_state(bend_btn, LV_STATE_DISABLED);
        lv_obj_remove_state(cal_btn, LV_STATE_DISABLED);

        if(s.calibrated)
        {
            lv_obj_remove_state(straight_btn, LV_STATE_DISABLED);
        }
        else
        {
            lv_obj_add_state(straight_btn, LV_STATE_DISABLED);
        }
    }
}
