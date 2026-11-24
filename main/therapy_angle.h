#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "therapy_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Seated knee angle from the implant accelerometer. Pure C, no ESP-IDF, host-tested.
 *
 * Device frame: x anterior, y subject's left, z superior. Sitting with the foot flat and the knee at a
 * right angle gives about (0, 0, +1 g): theta = atan2(a_x, a_z) = 0. Straightening the knee rotates the
 * shank forward; theta grows towards +90 deg. Seated knee flexion is therefore
 *
 *     phi = 90 - scale * (theta - theta_cal)
 *
 * so straightening lowers phi. The samples are low-passed at about 2 Hz. The angle is trusted only when
 * the acceleration magnitude is near 1 g and the lateral component is small.
 *
 * Calibration has two optional steps: sitting (sets theta_cal) and leg straight (checks the scale).
 * The second step is optional; without it scale stays 1.
 */

typedef enum
{
    THERAPY_CAL_NONE = 0,
    THERAPY_CAL_SIT,
    THERAPY_CAL_STRAIGHT,
} therapy_cal_mode_t;

typedef struct
{
    float lp_x, lp_y, lp_z;     /* low-passed acceleration, g */
    bool  primed;

    float theta_deg;            /* atan2(a_x, a_z), degrees */
    float phi_deg;              /* seated knee flexion, degrees (90 = right angle) */
    float magnitude_g;
    bool  mag_ok;
    bool  y_ok;
    bool  valid;                /* mag_ok && y_ok */

    float theta_cal;            /* theta at the seated calibration pose */
    float scale;                /* deg of phi per deg of theta; 1 unless a straight check ran */
    bool  calibrated;
    bool  straight_checked;

    /* Calibration collection */
    bool              cal_active;
    therapy_cal_mode_t cal_mode;
    int               cal_n;        /* samples collected so far */
    int               cal_valid;    /* valid samples among them */
    double            cal_sum;      /* sum of theta over the valid samples */
    bool              cal_done;     /* the 3 s collection finished */
    bool              cal_ok;       /* ... and the calibration was accepted */
} therapy_angle_t;

void therapy_angle_init(therapy_angle_t *a);

/* Drops the filter state (after a BLE sequence gap) but keeps the calibration. */
void therapy_angle_reset_filter(therapy_angle_t *a);

/* Use a known calibration (tests, or restoring it). */
void therapy_angle_set_calibration(therapy_angle_t *a, float theta_cal, float scale);

bool therapy_angle_calibrated(const therapy_angle_t *a);

/* Start a 3 s collection. SIT sets theta_cal; STRAIGHT checks the scale against 90 deg of travel. */
void therapy_angle_cal_begin(therapy_angle_t *a, therapy_cal_mode_t mode);

/* One RAW sample (mg). Returns true when this sample was valid (trusted angle). */
bool therapy_angle_push(therapy_angle_t *a, int16_t x_mg, int16_t y_mg, int16_t z_mg);

/* Same, plus calibration collection. Returns true when the 3 s collection is complete (check cal_ok). */
bool therapy_angle_cal_add(therapy_angle_t *a, int16_t x_mg, int16_t y_mg, int16_t z_mg);

#ifdef __cplusplus
}
#endif
