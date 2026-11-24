#include <math.h>

#include "therapy_angle.h"

#define RAD2DEG 57.29577951308232f

static float lowpass_alpha(float hz)
{
    return 1.0f - expf(-2.0f * 3.14159265358979f * hz / (float)THERAPY_FS_HZ);
}

void therapy_angle_init(therapy_angle_t *a)
{
    *a = (therapy_angle_t){0};
    a->scale = 1.0f;
    a->orientation_ok = true;
}

void therapy_angle_set_mode(therapy_angle_t *a, bool standing)
{
    if(a->standing != standing)
    {
        a->standing = standing;
        therapy_angle_reset_filter(a);
    }
}

void therapy_angle_reset_filter(therapy_angle_t *a)
{
    a->primed = false;
    a->lp_x = a->lp_y = a->lp_z = 0.0f;
    a->display_primed = false;
    a->valid = false;
    a->mag_ok = a->y_ok = false;
}

void therapy_angle_set_calibration(therapy_angle_t *a, float theta_cal, float scale)
{
    (void)scale;
    a->theta_cal = theta_cal;
    a->scale = 1.0f;
    a->calibrated = true;
    a->orientation_ok = true;
}

bool therapy_angle_calibrated(const therapy_angle_t *a)
{
    return a->calibrated;
}

void therapy_angle_cal_begin(therapy_angle_t *a, therapy_cal_mode_t mode)
{
    therapy_angle_reset_filter(a);
    a->cal_active = true;
    a->cal_mode = mode;
    a->cal_n = 0;
    a->cal_valid = 0;
    a->cal_sum = 0.0;
    a->cal_done = false;
    a->cal_ok = false;
}

bool therapy_angle_push(therapy_angle_t *a, int16_t x_mg, int16_t y_mg, int16_t z_mg)
{
    const float x = (float)x_mg / 1000.0f;
    const float y = (float)y_mg / 1000.0f;
    const float z = (float)z_mg / 1000.0f;

    /* The gate uses the raw magnitude (L1 A.1): a sudden kick is rejected immediately, before the filter
       smears it into the angle. */
    a->raw_magnitude_g = sqrtf(x * x + y * y + z * z);

    if(!a->primed)
    {
        a->lp_x = x; a->lp_y = y; a->lp_z = z;
        a->primed = true;
    }
    else
    {
        const float alpha = lowpass_alpha(THERAPY_LOWPASS_HZ);

        a->lp_x += alpha * (x - a->lp_x);
        a->lp_y += alpha * (y - a->lp_y);
        a->lp_z += alpha * (z - a->lp_z);
    }

    a->magnitude_g = sqrtf(a->lp_x * a->lp_x + a->lp_y * a->lp_y + a->lp_z * a->lp_z);
    a->theta_deg   = atan2f(a->lp_x, a->lp_z) * RAD2DEG;

    /* Seated: straightening the knee raises theta, so phi falls. Standing: the thigh is vertical and the heel
       goes back, which makes theta negative, so phi rises. */
    a->phi_deg = a->standing ? -(a->theta_deg - a->theta_cal)
                             : 90.0f - (a->theta_deg - a->theta_cal);

    a->mag_ok = fabsf(a->raw_magnitude_g - 1.0f) < THERAPY_TRUST_DY_G;
    a->y_ok   = fabsf(a->lp_y) < THERAPY_TRUST_Y_G;
    a->valid  = a->mag_ok && a->y_ok;

    if(a->valid)
    {
        /* Display-only smoothing: the rep logic always uses the 6 Hz phi. */
        if(!a->display_primed)
        {
            a->phi_display = a->phi_deg;
            a->display_primed = true;
        }
        else
        {
            const float alpha = lowpass_alpha(THERAPY_DISPLAY_HZ);

            a->phi_display += alpha * (a->phi_deg - a->phi_display);
        }
    }

    return a->valid;
}

bool therapy_angle_cal_add(therapy_angle_t *a, int16_t x_mg, int16_t y_mg, int16_t z_mg)
{
    if(!a->cal_active)
    {
        return false;
    }

    const bool valid = therapy_angle_push(a, x_mg, y_mg, z_mg);

    a->cal_n++;

    if(valid)
    {
        a->cal_sum += a->theta_deg;
        a->cal_valid++;
    }

    const int need = THERAPY_CAL_SECONDS * THERAPY_FS_HZ;

    if(a->cal_n < need)
    {
        return false;
    }

    a->cal_active = false;
    a->cal_done = true;
    a->cal_ok = false;

    if(a->cal_valid < THERAPY_CAL_MIN_VALID)
    {
        return true;
    }

    const float mean = (float)(a->cal_sum / a->cal_valid);

    if(a->cal_mode == THERAPY_CAL_SIT || a->cal_mode == THERAPY_CAL_STAND)
    {
        /* Both poses have a vertical shank, so either gives the same mount calibration. */
        a->theta_cal = mean;
        a->scale = 1.0f;
        a->calibrated = true;
        a->straight_checked = false;
        a->orientation_checked = false;
        a->orientation_ok = true;
        a->straight_extension_deg = 0.0f;
        a->cal_ok = true;
    }
    else if(a->cal_mode == THERAPY_CAL_STRAIGHT && a->calibrated)
    {
        const float travel = mean - a->theta_cal;

        /* No scale correction: an accelerometer tilt is absolute. The sign tells us whether the documented
           device frame is right; the reached angle is the patient's extension (10-20 deg lag is common). */
        a->straight_checked = true;
        a->orientation_checked = true;
        a->orientation_ok = (travel > THERAPY_STRAIGHT_MIN_TRAVEL_DEG);
        a->straight_extension_deg = 90.0f - travel;
        a->cal_ok = true;
    }

    return true;
}
