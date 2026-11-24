#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "therapy_angle.h"
#include "therapy_session.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Home therapy glue: owns the RAW subscription (through the existing KNEIL central), the angle/rep state,
 * voice cues, the therapy log and the screen view. No chip or protocol change: RAW is started with the
 * existing CONTROL 0x10 and stopped with 0x11.
 */

typedef enum
{
    THERAPY_STATE_IDLE = 0,
    THERAPY_STATE_CALIBRATING,
    THERAPY_STATE_RUNNING,
    THERAPY_STATE_PAUSED,
    THERAPY_STATE_DONE,
    THERAPY_STATE_LOST,
} therapy_state_t;

typedef struct
{
    therapy_state_t    state;
    therapy_exercise_t exercise;
    therapy_phase_t    phase;
    int                set;
    int                rep;              /* current rep in the set */
    int                reps_done;        /* counted in this set */
    int                reps_total;
    int                full_total;
    int                partial_total;

    float              phi;
    float              rest;
    float              target;
    float              hold_goal;        /* seconds for this exercise */
    float              peak;
    float              hold_s;
    float              best_extension;   /* lowest phi this session */
    float              best_flexion;     /* highest phi this session */
    bool               angle_valid;

    bool               calibrated;
    bool               straight_checked;
    bool               orientation_ok;      /* straight check: false = sensor orientation not as expected */
    float              straight_extension_deg;
    int                cal_progress_pct;

    bool               link_ok;
    bool               audio_ready;
    bool               log_ready;
    therapy_cal_mode_t cal_mode;           /* the collection in progress (when state is CALIBRATING) */

    bool               last_full;
    bool               last_partial;
    float              last_peak;
    float              last_hold_s;
    int                rest_remaining_s;
} therapy_snapshot_t;

esp_err_t therapy_init(void);

/* Start a session (seated extension, seated bend or standing knee curl). Sends 0x10. Fails when not
   calibrated or not connected. */
bool therapy_start(therapy_exercise_t exercise);

/* Stop and send 0x11, pause/resume, or run one calibration collection. */
void therapy_stop(void);
void therapy_pause(void);
bool therapy_resume(void);
bool therapy_calibrate_start(therapy_cal_mode_t mode);

/* Called from the LVGL refresh timer while the therapy screen is open: link timeout and deferred STOP. */
void therapy_tick(void);

void therapy_snapshot(therapy_snapshot_t *out);

#ifdef __cplusplus
}
#endif
