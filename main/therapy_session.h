#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "therapy_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Knee-exercise repetition state machine. Pure C, no ESP-IDF, host-tested.
 *
 * One session is one exercise (seated extension, seated bend or standing knee curl), THERAPY_SETS sets
 * of THERAPY_REPS repetitions.
 * A repetition is:
 *   rest -> moving -> at target (hold the exercise's hold time, beep each second) -> return -> counted.
 * A return that reaches >= 50 % of the range but not the target counts as partial, with its peak angle.
 * A target reached but the hold cut short also counts as partial (the peak is logged).
 * A gap in the RAW stream resets the current repetition; nothing is counted from incomplete data.
 */

typedef enum
{
    THERAPY_EX_EXTENSION = 0,   /* seated: straighten the knee, phi goes from 90 towards 10 */
    THERAPY_EX_BEND,            /* seated: slide the foot back, phi goes from 90 towards 110 */
    THERAPY_EX_CURL,            /* standing: heel behind, phi goes from 10 towards 90 */
} therapy_exercise_t;

typedef enum
{
    THERAPY_PHASE_IDLE = 0,
    THERAPY_PHASE_REST,
    THERAPY_PHASE_MOVING,
    THERAPY_PHASE_HOLD,
    THERAPY_PHASE_RETURN,
    THERAPY_PHASE_SET_REST,
    THERAPY_PHASE_DONE,
    THERAPY_PHASE_PAUSED,
    THERAPY_PHASE_LOST,
} therapy_phase_t;

/* Events returned by one push; the glue maps them to voice/beep/log. */
enum
{
    THERAPY_EV_PROMPT_EXT  = 1u << 0,
    THERAPY_EV_PROMPT_BEND = 1u << 1,
    THERAPY_EV_HOLD        = 1u << 2,
    THERAPY_EV_HOLD_TICK   = 1u << 3,
    THERAPY_EV_RETURN      = 1u << 4,
    THERAPY_EV_REP_FULL    = 1u << 5,
    THERAPY_EV_REP_PARTIAL = 1u << 6,
    THERAPY_EV_SET_DONE    = 1u << 7,
    THERAPY_EV_SESSION_DONE= 1u << 8,
    THERAPY_EV_SLOW        = 1u << 9,
    THERAPY_EV_LOST        = 1u << 10,
    THERAPY_EV_PROMPT_CURL = 1u << 11,
};

typedef struct
{
    therapy_exercise_t exercise;
    therapy_phase_t    phase;
    int                set;          /* 1..THERAPY_SETS */
    int                rep;          /* current repetition in this set, 1..THERAPY_REPS */
    int                reps_done;    /* counted repetitions in this set (full + partial) */
    int                reps_total;   /* counted repetitions in the session */
    int                full_total;
    int                partial_total;

    float phi;                       /* last valid angle */
    float peak;                      /* peak of the current repetition */
    float hold_s;                    /* hold reached in the current repetition */
    uint32_t phase_start_ms;
    uint32_t last_warn_ms;

    /* Last counted repetition, for the log/voice. */
    int   last_rep;
    float last_peak;
    float last_hold_s;
    bool  last_full;
    bool  last_partial;

    bool  have_phi;
    float last_phi;
    uint32_t last_t_ms;
} therapy_session_t;

/* Read-only view for the UI. */
typedef struct
{
    bool               active;
    therapy_exercise_t exercise;
    therapy_phase_t    phase;
    int                set;
    int                rep;
    int                reps_done;
    int                reps_total;
    int                full_total;
    int                partial_total;
    float              phi;
    float              rest;
    float              target;
    float              peak;
    float              hold_s;
    int                hold_goal;
    float              last_peak;
    float              last_hold_s;
    bool               last_full;
    bool               last_partial;
    int                rest_remaining_s;
} therapy_progress_t;

void therapy_session_init(therapy_session_t *s);

/* Starts a session and returns the initial prompt event. */
unsigned therapy_session_begin(therapy_session_t *s, therapy_exercise_t ex, uint32_t now_ms);

/* Feed one filtered angle (phi). Returns a bit mask of THERAPY_EV_*. */
unsigned therapy_session_push(therapy_session_t *s, float phi, bool valid, uint32_t now_ms);

/* RAW gap / link loss: reset the current repetition; nothing is counted. */
void therapy_session_gap(therapy_session_t *s);
void therapy_session_lost(therapy_session_t *s, uint32_t now_ms);

void therapy_session_pause(therapy_session_t *s);
void therapy_session_resume(therapy_session_t *s, uint32_t now_ms);
void therapy_session_stop(therapy_session_t *s);

void therapy_session_snapshot(const therapy_session_t *s, therapy_progress_t *out);

#ifdef __cplusplus
}
#endif
