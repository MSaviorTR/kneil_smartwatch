#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "haptics.h"
#include "kneil_central.h"
#include "kneil_state.h"
#include "therapy.h"
#include "therapy_audio.h"
#include "therapy_log.h"

static const char *TAG = "therapy";

static SemaphoreHandle_t lock;
static therapy_angle_t angle;
static therapy_session_t session;
static therapy_state_t state;
static bool initialized;

static bool     raw_on;
static bool     need_stop;          /* lost the link while RAW was on: send 0x11 after reconnect */
static bool     have_seq;
static uint16_t expected_seq;
static bool     have_raw;
static uint32_t last_raw_ms;

static therapy_cal_mode_t cal_mode;
static float best_extension;
static float best_flexion;
static bool summary_logged;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static const char *exercise_text(therapy_exercise_t ex)
{
    switch(ex)
    {
        case THERAPY_EX_EXTENSION: return "extension";
        case THERAPY_EX_BEND:      return "bend";
        default:                   return "standing_curl";
    }
}

static bool link_connected_locked(void)
{
    kneil_model_t m;

    kneil_central_snapshot(&m);

    return m.link == KNEIL_LINK_CONNECTED;
}

static void stop_raw_locked(void)
{
    if(raw_on)
    {
        if(!kneil_central_send_ctrl(KNEIL_CTRL_RAW_STOP))
        {
            need_stop = true;
        }

        raw_on = false;
    }

    kneil_central_set_raw_owner(false);
}

static void play_clip(therapy_clip_t clip)
{
    if(!therapy_audio_play(clip))
    {
        haptics_tap();
    }
}

static void play_rep(int rep)
{
    if(!therapy_audio_play_rep(rep))
    {
        haptics_tap();
    }
}

static void log_summary_locked(const char *reason)
{
    if(summary_logged || session.reps_total == 0)
    {
        return;
    }

    const int sets_done = (session.phase == THERAPY_PHASE_DONE) ? session.set : (session.set > 0 ? session.set - 1 : 0);

    therapy_log_session(exercise_text(session.exercise), sets_done,
                        session.full_total, session.partial_total,
                        (best_extension < 900.0f) ? best_extension : 0.0f,
                        (best_flexion > -900.0f) ? best_flexion : 0.0f,
                        session.exercise == THERAPY_EX_CURL, reason);
    summary_logged = true;
}

static void emit_events_locked(unsigned ev)
{
    if(ev & THERAPY_EV_PROMPT_EXT) play_clip(THERAPY_CLIP_STRAIGHTEN);
    if(ev & THERAPY_EV_PROMPT_BEND) play_clip(THERAPY_CLIP_BEND);
    if(ev & THERAPY_EV_PROMPT_CURL) play_clip(THERAPY_CLIP_CURL_BEND);
    if(ev & THERAPY_EV_HOLD) play_clip(THERAPY_CLIP_HOLD);
    if(ev & THERAPY_EV_HOLD_TICK) play_clip(THERAPY_CLIP_BEEP);
    if(ev & THERAPY_EV_SLOW) play_clip(THERAPY_CLIP_SLOW);

    if(ev & THERAPY_EV_RETURN)
    {
        play_clip(session.exercise == THERAPY_EX_BEND ? THERAPY_CLIP_STRAIGHTEN : THERAPY_CLIP_LOWER);
    }

    if(ev & (THERAPY_EV_REP_FULL | THERAPY_EV_REP_PARTIAL))
    {
        play_rep(session.last_rep);
        therapy_log_rep(exercise_text(session.exercise), session.set, session.last_rep,
                        session.last_peak, session.last_hold_s, session.last_full);
    }

    if(ev & THERAPY_EV_SET_DONE) play_clip(THERAPY_CLIP_SET_DONE);

    if(ev & THERAPY_EV_SESSION_DONE)
    {
        play_clip(THERAPY_CLIP_SESSION_DONE);
    }
}

static void lose_locked(const char *reason)
{
    if(state == THERAPY_STATE_RUNNING || state == THERAPY_STATE_CALIBRATING)
    {
        ESP_LOGW(TAG, "session lost: %s", reason);
        stop_raw_locked();
        need_stop = true;                 /* raw may still run on the implant; stop it when the link is back */
        therapy_session_lost(&session, now_ms());
        state = THERAPY_STATE_LOST;
        log_summary_locked(reason);
        if(!therapy_audio_play(THERAPY_CLIP_CONNECTION_LOST))
        {
            haptics_alert();
        }
    }
}

static void finish_calibration_locked(void)
{
    const therapy_cal_mode_t mode = cal_mode;

    stop_raw_locked();
    state = THERAPY_STATE_IDLE;

    if(!angle.cal_ok)
    {
        ESP_LOGW(TAG, "calibration failed (%d/%d valid samples)", angle.cal_valid, angle.cal_n);
        play_clip(THERAPY_CLIP_CALIBRATE);
        return;
    }

    ESP_LOGI(TAG, "calibration ok: theta_cal %.1f, orientation %s, straight reach %.1f deg",
             (double)angle.theta_cal, angle.orientation_ok ? "ok" : "NOT AS EXPECTED",
             (double)angle.straight_extension_deg);

    if(mode == THERAPY_CAL_STRAIGHT)
    {
        therapy_log_straight(angle.straight_extension_deg, angle.orientation_ok);
        play_clip(angle.orientation_ok ? THERAPY_CLIP_CAL_DONE : THERAPY_CLIP_ORIENTATION_BAD);
    }
    else
    {
        play_clip(THERAPY_CLIP_CAL_DONE);
    }
}

static void on_raw(const kneil_raw_t *raw)
{
    unsigned events = 0;
    bool cal_done = false;

    if(!initialized)
    {
        return;
    }

    xSemaphoreTake(lock, portMAX_DELAY);

    if(state != THERAPY_STATE_RUNNING && state != THERAPY_STATE_CALIBRATING)
    {
        xSemaphoreGive(lock);
        return;
    }

    if(have_seq && raw->seq != expected_seq)
    {
        if((int16_t)(raw->seq - expected_seq) > 0)
        {
            ESP_LOGW(TAG, "RAW sequence gap (expected %u, got %u)", (unsigned)expected_seq,
                     (unsigned)raw->seq);

            if(state == THERAPY_STATE_CALIBRATING)
            {
                angle.cal_active = false;
                stop_raw_locked();
                state = THERAPY_STATE_IDLE;
                xSemaphoreGive(lock);
                play_clip(THERAPY_CLIP_CALIBRATE);
                return;
            }

            therapy_session_gap(&session);
        }
        else
        {
            xSemaphoreGive(lock);         /* duplicate / out-of-order packet */
            return;
        }
    }

    expected_seq = (uint16_t)(raw->seq + 1);
    have_seq = true;
    have_raw = true;
    last_raw_ms = now_ms();

    for(int i = 0; i < raw->n; i++)
    {
        const uint32_t t = raw->t_ms_first + (uint32_t)i * 20u;

        if(state == THERAPY_STATE_CALIBRATING)
        {
            if(therapy_angle_cal_add(&angle, raw->xyz_mg[i][0], raw->xyz_mg[i][1], raw->xyz_mg[i][2]))
            {
                cal_done = true;
                break;
            }
        }
        else
        {
            const bool valid = therapy_angle_push(&angle, raw->xyz_mg[i][0], raw->xyz_mg[i][1],
                                                  raw->xyz_mg[i][2]);

            if(valid)
            {
                if(angle.phi_deg < best_extension) best_extension = angle.phi_deg;
                if(angle.phi_deg > best_flexion) best_flexion = angle.phi_deg;
            }

            events |= therapy_session_push(&session, angle.phi_deg, valid, t);
        }
    }

    if(cal_done)
    {
        finish_calibration_locked();
    }

    if(events & THERAPY_EV_SESSION_DONE)
    {
        stop_raw_locked();
        state = THERAPY_STATE_DONE;
        log_summary_locked("complete");
    }

    emit_events_locked(events);
    xSemaphoreGive(lock);
}

static void therapy_task(void *arg)
{
    (void)arg;

    while(1)
    {
        therapy_tick();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

esp_err_t therapy_init(void)
{
    if(initialized)
    {
        return ESP_OK;
    }

    lock = xSemaphoreCreateMutex();

    if(lock == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    therapy_angle_init(&angle);
    therapy_session_init(&session);
    state = THERAPY_STATE_IDLE;
    best_extension = 999.0f;
    best_flexion = -999.0f;

    therapy_log_init();
    therapy_audio_init();
    kneil_central_set_raw_listener(on_raw);

    if(xTaskCreate(therapy_task, "therapy", 3072, NULL, 3, NULL) != pdPASS)
    {
        ESP_LOGW(TAG, "background tick not started");
    }

    initialized = true;
    return ESP_OK;
}

static bool send_start_locked(void)
{
    if(need_stop)
    {
        kneil_central_send_ctrl(KNEIL_CTRL_RAW_STOP);
        need_stop = false;
    }

    return kneil_central_send_ctrl(KNEIL_CTRL_RAW_START);
}

bool therapy_start(therapy_exercise_t exercise)
{
    bool ok = false;
    unsigned ev = 0;

    if(!initialized) return false;

    xSemaphoreTake(lock, portMAX_DELAY);

    if(!angle.calibrated || !angle.orientation_ok || !link_connected_locked() ||
       state == THERAPY_STATE_RUNNING || state == THERAPY_STATE_CALIBRATING)
    {
        xSemaphoreGive(lock);
        return false;
    }

    if(!send_start_locked())
    {
        xSemaphoreGive(lock);
        return false;
    }

    session = (therapy_session_t){0};
    therapy_angle_set_mode(&angle, exercise == THERAPY_EX_CURL);
    therapy_angle_reset_filter(&angle);
    ev = therapy_session_begin(&session, exercise, now_ms());
    state = THERAPY_STATE_RUNNING;
    raw_on = true;
    kneil_central_set_raw_owner(true);
    have_seq = false;
    have_raw = false;
    last_raw_ms = now_ms();
    best_extension = 999.0f;
    best_flexion = -999.0f;
    summary_logged = false;
    ok = true;

    play_clip((exercise == THERAPY_EX_CURL) ? THERAPY_CLIP_CURL_SETUP : THERAPY_CLIP_SETUP);
    play_clip(THERAPY_CLIP_PAIN);
    emit_events_locked(ev);
    xSemaphoreGive(lock);
    return ok;
}

void therapy_pause(void)
{
    if(!initialized) return;

    xSemaphoreTake(lock, portMAX_DELAY);

    if(state == THERAPY_STATE_RUNNING)
    {
        stop_raw_locked();
        therapy_session_pause(&session);
        state = THERAPY_STATE_PAUSED;
    }

    xSemaphoreGive(lock);
}

bool therapy_resume(void)
{
    bool ok = false;

    if(!initialized) return false;

    xSemaphoreTake(lock, portMAX_DELAY);

    if(state == THERAPY_STATE_PAUSED && link_connected_locked() && send_start_locked())
    {
        therapy_session_resume(&session, now_ms());
        state = THERAPY_STATE_RUNNING;
        raw_on = true;
        kneil_central_set_raw_owner(true);
        therapy_angle_reset_filter(&angle);
        have_seq = false;
        have_raw = false;
        last_raw_ms = now_ms();
        emit_events_locked(session.exercise == THERAPY_EX_EXTENSION ? THERAPY_EV_PROMPT_EXT
                                                                   : THERAPY_EV_PROMPT_BEND);
        ok = true;
    }

    xSemaphoreGive(lock);
    return ok;
}

void therapy_stop(void)
{
    if(!initialized) return;

    xSemaphoreTake(lock, portMAX_DELAY);

    if(state != THERAPY_STATE_IDLE)
    {
        stop_raw_locked();
        log_summary_locked("stopped");
        therapy_session_stop(&session);
        state = THERAPY_STATE_IDLE;
    }

    xSemaphoreGive(lock);
}

bool therapy_calibrate_start(therapy_cal_mode_t mode)
{
    bool ok = false;

    if(!initialized) return false;

    xSemaphoreTake(lock, portMAX_DELAY);

    if((state == THERAPY_STATE_IDLE || state == THERAPY_STATE_PAUSED || state == THERAPY_STATE_DONE ||
        state == THERAPY_STATE_LOST) && link_connected_locked())
    {
        if(send_start_locked())
        {
            therapy_angle_cal_begin(&angle, mode);
            cal_mode = mode;
            state = THERAPY_STATE_CALIBRATING;
            raw_on = true;
            kneil_central_set_raw_owner(true);
            have_seq = false;
            have_raw = false;
            last_raw_ms = now_ms();

            if(mode == THERAPY_CAL_STAND)
            {
                play_clip(THERAPY_CLIP_CALIBRATE_STAND);
            }
            else if(mode == THERAPY_CAL_SIT)
            {
                play_clip(THERAPY_CLIP_CALIBRATE);
            }
            else
            {
                play_clip(THERAPY_CLIP_CALIBRATE_STRAIGHT);
            }

            ok = true;
        }
    }

    xSemaphoreGive(lock);
    return ok;
}

void therapy_tick(void)
{
    if(!initialized) return;

    xSemaphoreTake(lock, portMAX_DELAY);

    if(state == THERAPY_STATE_RUNNING || state == THERAPY_STATE_CALIBRATING)
    {
        if(!link_connected_locked())
        {
            lose_locked("link lost");
        }
        else if(have_raw && (uint32_t)(now_ms() - last_raw_ms) > THERAPY_RAW_TIMEOUT_MS)
        {
            lose_locked("RAW stream stopped");
        }
    }

    if(need_stop && link_connected_locked())
    {
        if(kneil_central_send_ctrl(KNEIL_CTRL_RAW_STOP))
        {
            need_stop = false;
        }
    }

    xSemaphoreGive(lock);
}

void therapy_snapshot(therapy_snapshot_t *out)
{
    therapy_progress_t p;
    kneil_model_t m;

    memset(out, 0, sizeof(*out));

    if(!initialized) return;

    xSemaphoreTake(lock, portMAX_DELAY);
    kneil_central_snapshot(&m);
    therapy_session_snapshot(&session, &p);

    out->state = state;
    out->exercise = session.exercise;
    out->phase = p.phase;
    out->set = p.set;
    out->rep = p.rep;
    out->reps_done = p.reps_done;
    out->reps_total = p.reps_total;
    out->full_total = p.full_total;
    out->partial_total = p.partial_total;
    out->phi = angle.phi_display;
    out->rest = p.rest;
    out->target = p.target;
    out->hold_goal = p.hold_goal;
    out->peak = p.peak;
    out->hold_s = p.hold_s;
    out->best_extension = (best_extension < 900.0f) ? best_extension : 0.0f;
    out->best_flexion = (best_flexion > -900.0f) ? best_flexion : 0.0f;
    out->angle_valid = angle.valid;
    out->calibrated = angle.calibrated;
    out->straight_checked = angle.straight_checked;
    out->orientation_ok = angle.orientation_ok;
    out->straight_extension_deg = angle.straight_extension_deg;
    out->cal_progress_pct = (angle.cal_active && angle.cal_n > 0)
                            ? (int)(100.0f * (float)angle.cal_n /
                                    (float)(THERAPY_CAL_SECONDS * THERAPY_FS_HZ))
                            : (angle.cal_done ? 100 : 0);
    out->link_ok = (m.link == KNEIL_LINK_CONNECTED);
    out->audio_ready = therapy_audio_ready();
    out->log_ready = therapy_log_available();
    out->cal_mode = cal_mode;
    out->last_full = p.last_full;
    out->last_partial = p.last_partial;
    out->last_peak = p.last_peak;
    out->last_hold_s = p.last_hold_s;
    out->rest_remaining_s = p.rest_remaining_s;

    xSemaphoreGive(lock);
}
