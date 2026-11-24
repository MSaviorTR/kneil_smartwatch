#include <math.h>
#include <string.h>

#include "therapy_session.h"

#define SET_REST_MS ((uint32_t)THERAPY_SET_REST_SECONDS * 1000u)
#define HOLD_MS     ((uint32_t)THERAPY_HOLD_SECONDS * 1000u)

static uint32_t elapsed_ms(uint32_t since, uint32_t now)
{
    return (uint32_t)(now - since);
}

static float target_phi(therapy_exercise_t ex)
{
    return (ex == THERAPY_EX_EXTENSION) ? THERAPY_EXT_TARGET_DEG : THERAPY_BEND_TARGET_DEG;
}

static float progress(const therapy_session_t *s, float phi)
{
    const float span = (s->exercise == THERAPY_EX_EXTENSION)
                       ? (THERAPY_REST_DEG - THERAPY_EXT_TARGET_DEG)
                       : (THERAPY_BEND_TARGET_DEG - THERAPY_REST_DEG);

    const float moved = (s->exercise == THERAPY_EX_EXTENSION) ? (THERAPY_REST_DEG - phi) : (phi - THERAPY_REST_DEG);

    if(span <= 0.0f)
    {
        return 0.0f;
    }

    return moved / span;
}

static bool at_target(const therapy_session_t *s, float phi)
{
    return (s->exercise == THERAPY_EX_EXTENSION)
           ? (phi <= THERAPY_EXT_TARGET_DEG + THERAPY_TARGET_TOL_DEG)
           : (phi >= THERAPY_BEND_TARGET_DEG - THERAPY_TARGET_TOL_DEG);
}

static bool near_rest(const therapy_session_t *s, float phi)
{
    return (s->exercise == THERAPY_EX_EXTENSION)
           ? (phi >= THERAPY_REST_DEG - THERAPY_REST_TOL_DEG)
           : (phi <= THERAPY_REST_DEG + THERAPY_REST_TOL_DEG);
}

static unsigned prompt_event(therapy_exercise_t ex)
{
    return (ex == THERAPY_EX_EXTENSION) ? THERAPY_EV_PROMPT_EXT : THERAPY_EV_PROMPT_BEND;
}

void therapy_session_init(therapy_session_t *s)
{
    memset(s, 0, sizeof(*s));
    s->phase = THERAPY_PHASE_IDLE;
    s->exercise = THERAPY_EX_EXTENSION;
}

unsigned therapy_session_begin(therapy_session_t *s, therapy_exercise_t ex, uint32_t now_ms)
{
    therapy_session_init(s);
    s->exercise = ex;
    s->phase = THERAPY_PHASE_REST;
    s->set = 1;
    s->rep = 1;
    s->phase_start_ms = now_ms;
    s->last_t_ms = now_ms;
    s->last_warn_ms = now_ms - THERAPY_WARN_INTERVAL_MS;

    return prompt_event(ex);
}

static unsigned record_rep(therapy_session_t *s, bool full, bool partial, float peak, float hold_s, uint32_t now_ms)
{
    unsigned ev = 0;

    s->last_rep = s->rep;
    s->last_peak = peak;
    s->last_hold_s = hold_s;
    s->last_full = full;
    s->last_partial = partial;
    s->reps_done++;
    s->reps_total++;

    if(full)
    {
        s->full_total++;
        ev |= THERAPY_EV_REP_FULL;
    }
    else
    {
        s->partial_total++;
        ev |= THERAPY_EV_REP_PARTIAL;
    }

    s->rep++;

    if(s->reps_done >= THERAPY_REPS)
    {
        if(s->set >= THERAPY_SETS)
        {
            s->phase = THERAPY_PHASE_DONE;
            ev |= THERAPY_EV_SESSION_DONE;
        }
        else
        {
            s->phase = THERAPY_PHASE_SET_REST;
            s->phase_start_ms = now_ms;
            ev |= THERAPY_EV_SET_DONE;
        }
    }
    else
    {
        s->phase = THERAPY_PHASE_REST;
        s->phase_start_ms = now_ms;
    }

    s->peak = 0.0f;
    s->hold_s = 0.0f;
    return ev;
}

unsigned therapy_session_push(therapy_session_t *s, float phi, bool valid, uint32_t now_ms)
{
    unsigned ev = 0;

    if(s->phase == THERAPY_PHASE_IDLE || s->phase == THERAPY_PHASE_DONE ||
       s->phase == THERAPY_PHASE_PAUSED || s->phase == THERAPY_PHASE_LOST)
    {
        return 0;
    }

    float speed = 0.0f;
    const uint32_t dt = elapsed_ms(s->last_t_ms, now_ms);

    if(valid && s->have_phi && dt > 0)
    {
        speed = fabsf(phi - s->last_phi) * 1000.0f / (float)dt;
    }

    s->last_t_ms = now_ms;

    if(valid)
    {
        s->last_phi = phi;
        s->have_phi = true;
        s->phi = phi;
    }

    if(s->phase == THERAPY_PHASE_SET_REST)
    {
        if(elapsed_ms(s->phase_start_ms, now_ms) >= SET_REST_MS)
        {
            s->set++;
            s->rep = 1;
            s->reps_done = 0;
            s->phase = THERAPY_PHASE_REST;
            s->phase_start_ms = now_ms;
            s->have_phi = false;
            ev |= prompt_event(s->exercise);
        }

        return ev;
    }

    if(!valid)
    {
        return 0;
    }

    if(speed > THERAPY_MAX_SPEED_DPS && elapsed_ms(s->last_warn_ms, now_ms) >= THERAPY_WARN_INTERVAL_MS)
    {
        s->last_warn_ms = now_ms;
        ev |= THERAPY_EV_SLOW;
    }

    switch(s->phase)
    {
        case THERAPY_PHASE_REST:
        {
            const float moved = (s->exercise == THERAPY_EX_EXTENSION)
                                ? (THERAPY_REST_DEG - phi) : (phi - THERAPY_REST_DEG);

            if(moved >= THERAPY_MOVE_START_DEG)
            {
                s->phase = THERAPY_PHASE_MOVING;
                s->phase_start_ms = now_ms;
                s->peak = phi;
                s->hold_s = 0.0f;
            }

            break;
        }

        case THERAPY_PHASE_MOVING:
        {
            if(s->exercise == THERAPY_EX_EXTENSION)
            {
                if(phi < s->peak) s->peak = phi;
            }
            else if(phi > s->peak)
            {
                s->peak = phi;
            }

            if(at_target(s, phi))
            {
                s->phase = THERAPY_PHASE_HOLD;
                s->phase_start_ms = now_ms;
                s->hold_s = 0.0f;
                ev |= THERAPY_EV_HOLD;
                break;
            }

            if(near_rest(s, phi))
            {
                const float p = progress(s, s->peak);

                if(p >= THERAPY_PARTIAL_FRACTION)
                {
                    ev |= record_rep(s, false, true, s->peak, 0.0f, now_ms);
                }
                else
                {
                    s->phase = THERAPY_PHASE_REST;
                    s->phase_start_ms = now_ms;
                    s->peak = 0.0f;
                }
            }

            break;
        }

        case THERAPY_PHASE_HOLD:
        {
            if(!at_target(s, phi))
            {
                s->hold_s = (float)elapsed_ms(s->phase_start_ms, now_ms) / 1000.0f;
                s->phase = THERAPY_PHASE_RETURN;
                s->phase_start_ms = now_ms;
                ev |= THERAPY_EV_RETURN;
                break;
            }

            const float held = (float)elapsed_ms(s->phase_start_ms, now_ms) / 1000.0f;
            const int whole = (int)floorf(held);
            const int before = (int)floorf(s->hold_s);

            s->hold_s = held;

            if(whole > before)
            {
                ev |= THERAPY_EV_HOLD_TICK;
            }

            if(held >= (float)THERAPY_HOLD_SECONDS)
            {
                s->phase = THERAPY_PHASE_RETURN;
                s->phase_start_ms = now_ms;
                ev |= THERAPY_EV_RETURN;
            }

            break;
        }

        case THERAPY_PHASE_RETURN:
        {
            if(near_rest(s, phi))
            {
                const bool full = (s->hold_s >= (float)THERAPY_HOLD_SECONDS);
                const bool partial = !full && (progress(s, s->peak) >= THERAPY_PARTIAL_FRACTION);

                ev |= record_rep(s, full, partial, s->peak, s->hold_s, now_ms);
            }

            break;
        }

        default:
            break;
    }

    return ev;
}

void therapy_session_gap(therapy_session_t *s)
{
    if(s->phase == THERAPY_PHASE_MOVING || s->phase == THERAPY_PHASE_HOLD || s->phase == THERAPY_PHASE_RETURN)
    {
        s->phase = THERAPY_PHASE_REST;
        s->phase_start_ms = s->last_t_ms;
        s->have_phi = false;
        s->peak = 0.0f;
        s->hold_s = 0.0f;
    }
}

void therapy_session_lost(therapy_session_t *s, uint32_t now_ms)
{
    if(s->phase != THERAPY_PHASE_IDLE && s->phase != THERAPY_PHASE_DONE)
    {
        s->phase = THERAPY_PHASE_LOST;
        s->phase_start_ms = now_ms;
        s->have_phi = false;
        s->peak = 0.0f;
    }
}

void therapy_session_pause(therapy_session_t *s)
{
    if(s->phase != THERAPY_PHASE_IDLE && s->phase != THERAPY_PHASE_DONE && s->phase != THERAPY_PHASE_LOST)
    {
        s->phase = THERAPY_PHASE_PAUSED;
        s->have_phi = false;
        s->peak = 0.0f;
        s->hold_s = 0.0f;
    }
}

void therapy_session_resume(therapy_session_t *s, uint32_t now_ms)
{
    if(s->phase == THERAPY_PHASE_PAUSED)
    {
        s->phase = THERAPY_PHASE_REST;
        s->phase_start_ms = now_ms;
        s->last_t_ms = now_ms;
        s->have_phi = false;
        s->peak = 0.0f;
        s->hold_s = 0.0f;
    }
}

void therapy_session_stop(therapy_session_t *s)
{
    s->phase = THERAPY_PHASE_IDLE;
    s->have_phi = false;
    s->peak = 0.0f;
    s->hold_s = 0.0f;
}

void therapy_session_snapshot(const therapy_session_t *s, therapy_progress_t *out)
{
    memset(out, 0, sizeof(*out));

    out->active = (s->phase != THERAPY_PHASE_IDLE);
    out->exercise = s->exercise;
    out->phase = s->phase;
    out->set = s->set;
    out->rep = s->rep;
    out->reps_done = s->reps_done;
    out->reps_total = s->reps_total;
    out->full_total = s->full_total;
    out->partial_total = s->partial_total;
    out->phi = s->phi;
    out->target = target_phi(s->exercise);
    out->peak = s->peak;
    out->hold_s = s->hold_s;
    out->last_peak = s->last_peak;
    out->last_hold_s = s->last_hold_s;
    out->last_full = s->last_full;
    out->last_partial = s->last_partial;

    if(s->phase == THERAPY_PHASE_SET_REST)
    {
        const uint32_t left = SET_REST_MS - elapsed_ms(s->phase_start_ms, s->last_t_ms);

        out->rest_remaining_s = (int)((left + 999u) / 1000u);
    }
}
