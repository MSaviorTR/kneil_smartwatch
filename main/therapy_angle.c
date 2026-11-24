#include <math.h>

#include "therapy_angle.h"

#define RAD2DEG 57.29577951308232f

static float lowpass_alpha(void)
{
    return 1.0f - expf(-2.0f * 3.14159265358979f * THERAPY_LOWPASS_HZ / (float)THERAPY_FS_HZ);
}

void therapy_angle_init(therapy_angle_t *a)
{
    *a = (therapy_angle_t){0};
    a->scale = 1.0f;
}

void therapy_angle_reset_filter(therapy_angle_t *a)
{
    a->primed = false;
    a->lp_x = a->lp_y = a->lp_z = 0.0f;
    a->valid = false;
    a->mag_ok = a->y_ok = false;
}

void therapy_angle_set_calibration(therapy_angle_t *a, float theta_cal, float scale)
{
    a->theta_cal = theta_cal;
    a->scale = (scale > 0.1f) ? scale : 1.0f;
    a->calibrated = true;
    a->straight_checked = fabsf(a->scale - 1.0f) > 1e-4f;
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

    if(!a->primed)
    {
        a->lp_x = x; a->lp_y = y; a->lp_z = z;
        a->primed = true;
    }
    else
    {
        const float alpha = lowpass_alpha();

        a->lp_x += alpha * (x - a->lp_x);
        a->lp_y += alpha * (y - a->lp_y);
        a->lp_z += alpha * (z - a->lp_z);
    }

    a->magnitude_g = sqrtf(a->lp_x * a->lp_x + a->lp_y * a->lp_y + a->lp_z * a->lp_z);
    a->theta_deg   = atan2f(a->lp_x, a->lp_z) * RAD2DEG;
    a->phi_deg     = 90.0f - a->scale * (a->theta_deg - a->theta_cal);
    a->mag_ok      = fabsf(a->magnitude_g - 1.0f) < THERAPY_TRUST_DY_G;
    a->y_ok        = fabsf(a->lp_y) < THERAPY_TRUST_Y_G;
    a->valid       = a->mag_ok && a->y_ok;

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

    if(a->cal_mode == THERAPY_CAL_SIT)
    {
        a->theta_cal = mean;
        a->scale = 1.0f;
        a->calibrated = true;
        a->straight_checked = false;
        a->cal_ok = true;
    }
    else if(a->cal_mode == THERAPY_CAL_STRAIGHT && a->calibrated)
    {
        const float travel = mean - a->theta_cal;

        /* 90 deg of shank rotation should be 90 deg of knee extension; accept a plausible range only. */
        if(travel >= 45.0f && travel <= 135.0f)
        {
            a->scale = 90.0f / travel;
            a->straight_checked = true;
            a->cal_ok = true;
        }
    }

    return true;
}
