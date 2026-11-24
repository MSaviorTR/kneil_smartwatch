#pragma once

/*
 * Home knee-exercise therapy: parameters.   therapy_config 1.0, 24 Nov 2026
 *
 * Every value below is a first guess and MUST be confirmed by the physiotherapist before the watch is
 * used by a patient. Seated angles are knee flexion in degrees: 90 = knee at a right angle (foot flat),
 * 0 = leg straight, >90 = heel slid back under the chair. Standing flexion is 0 = leg straight, 90 = heel
 * lifted behind the knee.
 */

#define THERAPY_FS_HZ              50
#define THERAPY_LOWPASS_HZ         6.0f    /* L1 A.1: 6 Hz keeps the 95th-percentile error < 4 deg at 60 deg/s */
#define THERAPY_DISPLAY_HZ         2.0f    /* display-only smoothing of the shown number (rep logic uses 6 Hz) */
#define THERAPY_TRUST_DY_G         0.05f   /* TBC: | |a_raw| - 1 g | below this, on the RAW magnitude */
#define THERAPY_TRUST_Y_G          0.25f   /* TBC: |a_y| below this */

#define THERAPY_CAL_SECONDS        3       /* TBC: sit/stand still with a vertical shank */
#define THERAPY_CAL_MIN_VALID      120     /* of 150 samples at 50 Hz */
#define THERAPY_REST_DEG           90.0f   /* TBC: seated rest (foot flat, knee at a right angle) */
#define THERAPY_EXT_TARGET_DEG     10.0f   /* TBC: knee extension target */
#define THERAPY_BEND_TARGET_DEG    110.0f  /* TBC: seated knee flexion target */
#define THERAPY_CURL_REST_DEG      10.0f   /* TBC: standing, heel just off the floor */
#define THERAPY_CURL_TARGET_DEG    90.0f   /* TBC: standing knee flexion target */
#define THERAPY_TARGET_TOL_DEG     5.0f    /* TBC: "at target" window */
#define THERAPY_REST_TOL_DEG       12.0f   /* TBC: back near the start */
#define THERAPY_MOVE_START_DEG     8.0f    /* TBC: leave rest */
#define THERAPY_STRAIGHT_MIN_TRAVEL_DEG 10.0f  /* TBC: smallest believable straightening travel for the sign check */
#define THERAPY_HOLD_SECONDS       5       /* TBC: seated exercises */
#define THERAPY_CURL_HOLD_SECONDS  3       /* TBC: standing knee curl */
#define THERAPY_REPS               10      /* TBC: per set */
#define THERAPY_SETS               3       /* TBC */
#define THERAPY_SET_REST_SECONDS   30      /* TBC */
#define THERAPY_PARTIAL_FRACTION   0.50f   /* TBC: >= 50 % of the range counts as partial */
#define THERAPY_MAX_SPEED_DPS      30.0f   /* TBC: warn above this angular speed */
#define THERAPY_WARN_INTERVAL_MS   5000    /* TBC: at most one slow-movement warning */
#define THERAPY_RAW_TIMEOUT_MS     2000    /* TBC: no RAW packet for this long = lost */
