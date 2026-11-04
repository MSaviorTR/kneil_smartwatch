#include <math.h>
#include <stddef.h>

#include "calibration.h"

/* R * ln(10) / F in mV per kelvin */
#define NERNST_MV_PER_K 0.198416f

bool cal_fit_linear(const cal_point_t *pts, int n, cal_fit_t *out)
{
    if(pts == NULL || out == NULL || n < 2 || n > CAL_MAX_POINTS)
    {
        return false;
    }

    double sx = 0.0;
    double sy = 0.0;

    for(int i = 0; i < n; i++)
    {
        if(!isfinite(pts[i].x) || !isfinite(pts[i].mv))
        {
            return false;
        }

        sx += pts[i].x;
        sy += pts[i].mv;
    }

    double mx = sx / n;
    double my = sy / n;
    double sxx = 0.0;
    double sxy = 0.0;
    double syy = 0.0;

    for(int i = 0; i < n; i++)
    {
        double dx = pts[i].x - mx;
        double dy = pts[i].mv - my;

        sxx += dx * dx;
        sxy += dx * dy;
        syy += dy * dy;
    }

    /* All standards at the same x: slope is undefined. */
    if(sxx < 1e-9)
    {
        return false;
    }

    double slope = sxy / sxx;

    /* A flat response cannot be inverted into a concentration. */
    if(fabs(slope) < 1e-3)
    {
        return false;
    }

    out->slope     = (float)slope;
    out->intercept = (float)(my - slope * mx);
    out->r2        = (syy > 0.0) ? (float)((sxy * sxy) / (sxx * syy)) : 1.0f;
    out->n_points  = (uint8_t)n;
    out->valid     = true;

    return true;
}

float cal_mv_to_x(const cal_fit_t *fit, float mv)
{
    if(fit == NULL || !fit->valid || !isfinite(mv) || fabsf(fit->slope) < 1e-3f)
    {
        return NAN;
    }

    return (mv - fit->intercept) / fit->slope;
}

float cal_k_mv_to_mM(const cal_fit_t *fit, float mv)
{
    float x = cal_mv_to_x(fit, mv);

    if(!isfinite(x))
    {
        return NAN;
    }

    float c = powf(10.0f, x);

    return isfinite(c) ? c : NAN;
}

float cal_nernst_slope(float temp_c, int z)
{
    if(z == 0)
    {
        return NAN;
    }

    return NERNST_MV_PER_K * (temp_c + 273.15f) / (float)z;
}

float cal_slope_percent(const cal_fit_t *fit)
{
    if(fit == NULL || !fit->valid)
    {
        return NAN;
    }

    return 100.0f * fabsf(fit->slope) / cal_nernst_slope(25.0f, 1);
}
