#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Linear electrode calibration:  E(mV) = intercept + slope * x
 *
 *   K+ ISE : x = log10([K+] in mM)   slope ~ +59.16 mV/decade at 25 C
 *   pH     : x = pH                  slope ~ -59.16 mV/pH     at 25 C
 *
 * Concentration is used in place of activity. That is fine as long as
 * standards and samples share a similar ionic-strength background
 * (e.g. standards made up in artificial synovial fluid / PBS).
 */

#define CAL_MAX_POINTS 5

typedef enum
{
    CAL_CHANNEL_K = 0,
    CAL_CHANNEL_PH,
    CAL_CHANNEL_COUNT
} cal_channel_t;

typedef struct
{
    float x;    /* log10(mM) or pH */
    float mv;   /* mean electrode potential at that standard */
} cal_point_t;

typedef struct
{
    float   slope;       /* mV per unit x */
    float   intercept;   /* mV at x = 0   */
    float   r2;          /* 1.0 for exactly two points */
    uint8_t n_points;
    bool    valid;
    int64_t fitted_at;   /* unix seconds, 0 if unknown */
} cal_fit_t;

/* Least-squares line through n >= 2 points with at least two distinct x.
   Returns false (and leaves *out untouched) if the fit is degenerate. */
bool cal_fit_linear(const cal_point_t *pts, int n, cal_fit_t *out);

/* Inverse: potential -> x. NAN if the fit is invalid or mv is NAN. */
float cal_mv_to_x(const cal_fit_t *fit, float mv);

/* K+ helper: potential -> concentration in mM. NAN on failure. */
float cal_k_mv_to_mM(const cal_fit_t *fit, float mv);

/* Ideal Nernst slope in mV per decade for an ion of charge z at temp_c. */
float cal_nernst_slope(float temp_c, int z);

/* |slope| as a percentage of the ideal 25 C Nernstian slope (z = 1).
   ~95-102 % is a healthy electrode; < 90 % usually means a tired membrane. */
float cal_slope_percent(const cal_fit_t *fit);

#ifdef __cplusplus
}
#endif
