/* Host tests for the home-therapy modules: angle/calibration and the rep state machine.   therapy tests 1.0 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "therapy_angle.h"
#include "therapy_session.h"
#include "kneil_raw_policy.h"

static int checks;
static int fails;

#define CHECK(cond) do { \
        checks++; \
        if(!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } \
    } while(0)

#define DEG2RAD 0.017453292519943295f

static void sample_at(therapy_angle_t *a, float theta_deg)
{
    const float r = theta_deg * DEG2RAD;
    const int16_t x = (int16_t)lroundf(sinf(r) * 1000.0f);
    const int16_t z = (int16_t)lroundf(cosf(r) * 1000.0f);

    if(a->cal_active)
    {
        therapy_angle_cal_add(a, x, 0, z);
    }
    else
    {
        therapy_angle_push(a, x, 0, z);
    }
}

static void test_angle_basic(void)
{
    therapy_angle_t a;

    therapy_angle_init(&a);
    CHECK(!therapy_angle_calibrated(&a));

    for(int i = 0; i < 100; i++) sample_at(&a, 0.0f);
    CHECK(a.valid);
    CHECK(fabsf(a.phi_deg - 90.0f) < 2.0f);       /* uncalibrated: theta_cal = 0 */
    CHECK(fabsf(a.theta_deg) < 2.0f);

    for(int i = 0; i < 100; i++) sample_at(&a, 90.0f);
    CHECK(fabsf(a.phi_deg) < 2.0f);               /* straightening lowers phi */

    for(int i = 0; i < 100; i++) sample_at(&a, 45.0f);
    CHECK(fabsf(a.phi_deg - 45.0f) < 2.0f);
}

static void test_angle_calibration(void)
{
    therapy_angle_t a;

    therapy_angle_init(&a);
    therapy_angle_cal_begin(&a, THERAPY_CAL_SIT);

    for(int i = 0; i < THERAPY_CAL_SECONDS * THERAPY_FS_HZ; i++) sample_at(&a, 0.0f);
    CHECK(a.cal_done);
    CHECK(a.cal_ok);
    CHECK(therapy_angle_calibrated(&a));
    CHECK(fabsf(a.theta_cal) < 1.0f);

    for(int i = 0; i < 100; i++) sample_at(&a, 90.0f);
    CHECK(fabsf(a.phi_deg) < 2.0f);

    /* a thigh offset: scale still applies around theta_cal */
    therapy_angle_init(&a);
    therapy_angle_cal_begin(&a, THERAPY_CAL_SIT);
    for(int i = 0; i < THERAPY_CAL_SECONDS * THERAPY_FS_HZ; i++) sample_at(&a, 10.0f);
    CHECK(a.cal_ok);
    for(int i = 0; i < 100; i++) sample_at(&a, 100.0f);
    CHECK(fabsf(a.phi_deg) < 2.0f);

    /* optional straight check: 90 deg of travel keeps scale 1 */
    therapy_angle_t b;
    therapy_angle_init(&b);
    therapy_angle_cal_begin(&b, THERAPY_CAL_SIT);
    for(int i = 0; i < THERAPY_CAL_SECONDS * THERAPY_FS_HZ; i++) sample_at(&b, 0.0f);
    therapy_angle_cal_begin(&b, THERAPY_CAL_STRAIGHT);
    for(int i = 0; i < THERAPY_CAL_SECONDS * THERAPY_FS_HZ; i++) sample_at(&b, 90.0f);
    CHECK(b.cal_done && b.cal_ok && b.straight_checked);
    CHECK(b.orientation_ok);
    CHECK(fabsf(b.scale - 1.0f) < 0.02f);

    /* a patient who reaches only 20 deg is shown 20 deg, not rescaled to 0 */
    therapy_angle_t c;
    therapy_angle_init(&c);
    therapy_angle_cal_begin(&c, THERAPY_CAL_SIT);
    for(int i = 0; i < THERAPY_CAL_SECONDS * THERAPY_FS_HZ; i++) sample_at(&c, 0.0f);
    therapy_angle_cal_begin(&c, THERAPY_CAL_STRAIGHT);
    for(int i = 0; i < THERAPY_CAL_SECONDS * THERAPY_FS_HZ; i++) sample_at(&c, 70.0f);
    CHECK(c.cal_done && c.cal_ok && c.straight_checked && c.orientation_ok);
    CHECK(fabsf(c.straight_extension_deg - 20.0f) < 2.0f);
    CHECK(fabsf(c.phi_deg - 20.0f) < 2.0f);

    /* wrong sign: the straight check flags the orientation and must refuse a session */
    therapy_angle_t d;
    therapy_angle_init(&d);
    therapy_angle_cal_begin(&d, THERAPY_CAL_SIT);
    for(int i = 0; i < THERAPY_CAL_SECONDS * THERAPY_FS_HZ; i++) sample_at(&d, 0.0f);
    therapy_angle_cal_begin(&d, THERAPY_CAL_STRAIGHT);
    for(int i = 0; i < THERAPY_CAL_SECONDS * THERAPY_FS_HZ; i++) sample_at(&d, -30.0f);
    CHECK(d.cal_done && d.straight_checked);
    CHECK(!d.orientation_ok);

    /* a failed sitting calibration keeps the old one */
    const float old_cal = a.theta_cal;
    therapy_angle_cal_begin(&a, THERAPY_CAL_SIT);
    for(int i = 0; i < THERAPY_CAL_SECONDS * THERAPY_FS_HZ; i++)
    {
        therapy_angle_cal_add(&a, 0, 0, 1300);
    }
    CHECK(a.cal_done && !a.cal_ok);
    CHECK(a.theta_cal == old_cal);
}

static void test_angle_trust(void)
{
    therapy_angle_t a;

    therapy_angle_init(&a);
    therapy_angle_push(&a, 0, 0, 1300);           /* 1.3 g: not trusted */
    CHECK(!a.valid && !a.mag_ok);
    for(int i = 0; i < 30; i++) therapy_angle_push(&a, 0, 400, 900);   /* lateral settles above the limit */
    CHECK(!a.valid && !a.y_ok);
    for(int i = 0; i < 100; i++) sample_at(&a, 0.0f);
    CHECK(a.valid && a.mag_ok && a.y_ok);

    /* the 0.05 g gate is on the RAW magnitude: one kick is rejected at once, while the filtered magnitude
       is still near 1 g (L1 A.1) */
    therapy_angle_push(&a, 0, 0, 1100);
    CHECK(!a.mag_ok && !a.valid);
    CHECK(fabsf(a.magnitude_g - 1.0f) < 0.1f);
    for(int i = 0; i < 5; i++) sample_at(&a, 0.0f);
    CHECK(a.valid && a.mag_ok);
}

static void test_angle_standing(void)
{
    therapy_angle_t a;
    const float angles[] = {0.0f, 45.0f, 90.0f, 110.0f};

    /* Stand tall (shank vertical) is the same mount pose as the seated foot-flat calibration. */
    therapy_angle_init(&a);
    therapy_angle_set_mode(&a, true);
    therapy_angle_cal_begin(&a, THERAPY_CAL_STAND);
    for(int i = 0; i < THERAPY_CAL_SECONDS * THERAPY_FS_HZ; i++) sample_at(&a, 0.0f);
    CHECK(a.cal_done && a.cal_ok && a.calibrated);
    CHECK(fabsf(a.theta_cal) < 1.0f);

    /* Heel behind -> shank tilts back -> theta negative -> standing flexion positive. */
    for(int k = 0; k < 4; k++)
    {
        for(int i = 0; i < 100; i++) sample_at(&a, -angles[k]);
        CHECK(fabsf(a.phi_deg - angles[k]) < 2.0f);
    }

    for(int i = 0; i < 100; i++) sample_at(&a, -45.0f);
    CHECK(a.theta_deg < 0.0f);
    CHECK(a.phi_deg > 0.0f);

    /* Thigh-tilt error: with the knee unchanged, a 10 deg thigh lean shows up 1:1 in the standing angle. */
    for(int i = 0; i < 100; i++) sample_at(&a, -10.0f);
    CHECK(fabsf(a.phi_deg - 10.0f) < 2.0f);

    /* The seated formula is still theta 0 at the calibration pose and 90 at a straight leg. */
    therapy_angle_init(&a);
    therapy_angle_cal_begin(&a, THERAPY_CAL_SIT);
    for(int i = 0; i < THERAPY_CAL_SECONDS * THERAPY_FS_HZ; i++) sample_at(&a, 0.0f);
    for(int i = 0; i < 100; i++) sample_at(&a, 90.0f);
    CHECK(fabsf(a.phi_deg) < 2.0f);
}

static void test_angle_display(void)
{
    therapy_angle_t a;
    float phi_lo = 1e9f, phi_hi = -1e9f, disp_lo = 1e9f, disp_hi = -1e9f;

    therapy_angle_init(&a);
    for(int i = 0; i < 300; i++)
    {
        const float theta = 45.0f + 3.0f * sinf((float)i * 0.7f);

        sample_at(&a, theta);

        if(i >= 60 && a.valid)                    /* both filters settled */
        {
            if(a.phi_deg < phi_lo) phi_lo = a.phi_deg;
            if(a.phi_deg > phi_hi) phi_hi = a.phi_deg;
            if(a.phi_display < disp_lo) disp_lo = a.phi_display;
            if(a.phi_display > disp_hi) disp_hi = a.phi_display;
        }
    }

    CHECK(disp_hi - disp_lo < phi_hi - phi_lo);
}

static void test_angle_noise(void)
{
    therapy_angle_t a;
    float lo = 1e9f, hi = -1e9f;

    therapy_angle_init(&a);
    for(int i = 0; i < 300; i++)
    {
        const float theta = 0.0f + 2.0f * sinf((float)i * 0.7f);
        const float r = theta * DEG2RAD;

        therapy_angle_push(&a, (int16_t)lroundf((sinf(r) * 1000.0f) + (i % 3 - 1) * 15.0f), 0,
                           (int16_t)lroundf((cosf(r) * 1000.0f) + ((i + 1) % 3 - 1) * 15.0f));
        if(a.valid)
        {
            if(a.phi_deg < lo) lo = a.phi_deg;
            if(a.phi_deg > hi) hi = a.phi_deg;
        }
    }
    CHECK(lo > 80.0f && hi < 100.0f);
}

static void push_rest(therapy_session_t *s, int n, uint32_t *t, unsigned *events)
{
    for(int i = 0; i < n; i++)
    {
        *events |= therapy_session_push(s, THERAPY_REST_DEG, true, *t);
        *t += 20;
    }
}

static void push_ramp(therapy_session_t *s, float from, float to, int n, uint32_t *t, unsigned *events)
{
    for(int i = 1; i <= n; i++)
    {
        const float phi = from + (to - from) * (float)i / (float)n;

        *events |= therapy_session_push(s, phi, true, *t);
        *t += 20;
    }
}

static void push_hold(therapy_session_t *s, float phi, int ms, uint32_t *t, unsigned *events)
{
    for(int i = 0; i < ms / 20; i++)
    {
        *events |= therapy_session_push(s, phi, true, *t);
        *t += 20;
    }
}

static void test_session_clean_rep(void)
{
    therapy_session_t s;
    therapy_progress_t p;
    uint32_t t = 1000;
    unsigned ev = therapy_session_begin(&s, THERAPY_EX_EXTENSION, t);

    CHECK(ev & THERAPY_EV_PROMPT_EXT);
    push_rest(&s, 50, &t, &ev);
    push_ramp(&s, 90, 10, 50, &t, &ev);
    CHECK(ev & THERAPY_EV_HOLD);
    push_hold(&s, 10, 5200, &t, &ev);
    CHECK(ev & THERAPY_EV_RETURN);
    push_ramp(&s, 10, 90, 50, &t, &ev);
    CHECK(ev & THERAPY_EV_REP_FULL);
    CHECK(!(ev & THERAPY_EV_REP_PARTIAL));

    therapy_session_snapshot(&s, &p);
    CHECK(p.reps_done == 1 && p.full_total == 1 && p.partial_total == 0);
    CHECK(s.last_full && !s.last_partial);
    CHECK(fabsf(s.last_peak - 10.0f) < 6.0f);
    CHECK(s.last_hold_s >= (float)THERAPY_HOLD_SECONDS);
}

static void test_session_partial(void)
{
    therapy_session_t s;
    uint32_t t = 0;
    unsigned ev = therapy_session_begin(&s, THERAPY_EX_EXTENSION, t);

    push_rest(&s, 20, &t, &ev);
    push_ramp(&s, 90, 50, 40, &t, &ev);          /* exactly 50 % of the 80 deg range */
    push_ramp(&s, 50, 90, 40, &t, &ev);
    CHECK(ev & THERAPY_EV_REP_PARTIAL);
    CHECK(s.last_partial && !s.last_full);
    CHECK(fabsf(s.last_peak - 50.0f) < 5.0f);
}

static void test_session_short_hold(void)
{
    therapy_session_t s;
    uint32_t t = 0;
    unsigned ev = therapy_session_begin(&s, THERAPY_EX_EXTENSION, t);

    push_rest(&s, 20, &t, &ev);
    push_ramp(&s, 90, 10, 40, &t, &ev);
    push_hold(&s, 10, 2000, &t, &ev);            /* target reached, hold too short */
    push_ramp(&s, 10, 90, 40, &t, &ev);
    CHECK(ev & THERAPY_EV_REP_PARTIAL);
    CHECK(s.last_partial && !s.last_full);
    CHECK(s.last_hold_s > 1.5f && s.last_hold_s < 3.0f);
}

static void test_session_jitter(void)
{
    therapy_session_t s;
    uint32_t t = 0;
    unsigned ev = therapy_session_begin(&s, THERAPY_EX_EXTENSION, t);

    push_rest(&s, 20, &t, &ev);
    push_ramp(&s, 90, 10, 40, &t, &ev);

    for(int i = 0; i < 260; i++)                 /* 5.2 s at 10 +- 2 deg: inside the target window */
    {
        ev |= therapy_session_push(&s, 10.0f + ((i % 2) ? 2.0f : -2.0f), true, t);
        t += 20;
    }

    push_ramp(&s, 10, 90, 40, &t, &ev);
    CHECK(ev & THERAPY_EV_REP_FULL);
    CHECK(!(ev & THERAPY_EV_REP_PARTIAL));
}

static void test_session_gap_and_lost(void)
{
    therapy_session_t s;
    uint32_t t = 0;
    unsigned ev = therapy_session_begin(&s, THERAPY_EX_EXTENSION, t);

    push_rest(&s, 20, &t, &ev);
    push_ramp(&s, 90, 50, 30, &t, &ev);
    therapy_session_gap(&s);
    CHECK(s.phase == THERAPY_PHASE_REST);
    push_ramp(&s, 50, 90, 30, &t, &ev);
    CHECK(!(ev & THERAPY_EV_REP_FULL) && !(ev & THERAPY_EV_REP_PARTIAL));

    push_ramp(&s, 90, 60, 20, &t, &ev);
    therapy_session_lost(&s, t);
    CHECK(s.phase == THERAPY_PHASE_LOST);
    CHECK(!(ev & THERAPY_EV_REP_FULL));
}

static void test_session_bend_and_slow(void)
{
    therapy_session_t s;
    uint32_t t = 0;
    unsigned ev = therapy_session_begin(&s, THERAPY_EX_BEND, t);

    CHECK(ev & THERAPY_EV_PROMPT_BEND);
    push_rest(&s, 20, &t, &ev);
    push_ramp(&s, 90, 110, 30, &t, &ev);
    push_hold(&s, 110, 5200, &t, &ev);
    push_ramp(&s, 110, 90, 30, &t, &ev);
    CHECK(ev & THERAPY_EV_REP_FULL);

    therapy_session_t f;
    uint32_t u = 0;
    ev = therapy_session_begin(&f, THERAPY_EX_EXTENSION, u);
    push_rest(&f, 20, &u, &ev);
    push_ramp(&f, 90, 10, 5, &u, &ev);          /* 80 deg in 100 ms: much too fast */
    CHECK(ev & THERAPY_EV_SLOW);
}

static void test_session_curl(void)
{
    therapy_session_t s;
    therapy_progress_t p;
    uint32_t t = 0;
    unsigned ev = therapy_session_begin(&s, THERAPY_EX_CURL, t);

    CHECK(ev & THERAPY_EV_PROMPT_CURL);
    push_hold(&s, THERAPY_CURL_REST_DEG, 40, &t, &ev);
    push_ramp(&s, THERAPY_CURL_REST_DEG, THERAPY_CURL_TARGET_DEG, 60, &t, &ev);
    CHECK(ev & THERAPY_EV_HOLD);
    push_hold(&s, THERAPY_CURL_TARGET_DEG, 3200, &t, &ev);
    CHECK(ev & THERAPY_EV_RETURN);
    push_ramp(&s, THERAPY_CURL_TARGET_DEG, THERAPY_CURL_REST_DEG, 60, &t, &ev);
    CHECK(ev & THERAPY_EV_REP_FULL);
    CHECK(!(ev & THERAPY_EV_REP_PARTIAL));

    therapy_session_snapshot(&s, &p);
    CHECK(fabsf(p.rest - THERAPY_CURL_REST_DEG) < 0.1f);
    CHECK(fabsf(p.target - THERAPY_CURL_TARGET_DEG) < 0.1f);
    CHECK(p.hold_goal == THERAPY_CURL_HOLD_SECONDS);
    CHECK(p.full_total == 1 && p.partial_total == 0);
}

static void test_session_sets(void)
{    therapy_session_t s;
    uint32_t t = 0;
    unsigned ev = therapy_session_begin(&s, THERAPY_EX_EXTENSION, t);
    int set_done = 0;

    for(int set = 0; set < THERAPY_SETS; set++)
    {
        for(int r = 0; r < THERAPY_REPS; r++)
        {
            push_rest(&s, 10, &t, &ev);
            push_ramp(&s, 90, 10, 20, &t, &ev);
            push_hold(&s, 10, 5200, &t, &ev);
            push_ramp(&s, 10, 90, 20, &t, &ev);
        }

        if(set < THERAPY_SETS - 1)
        {
            CHECK(ev & THERAPY_EV_SET_DONE);
            set_done++;
            push_hold(&s, 90, THERAPY_SET_REST_SECONDS * 1000 + 100, &t, &ev);
        }
    }

    CHECK(set_done == THERAPY_SETS - 1);
    CHECK(s.phase == THERAPY_PHASE_DONE);
    CHECK(ev & THERAPY_EV_SESSION_DONE);
    CHECK(s.reps_total == THERAPY_SETS * THERAPY_REPS);
}

static void test_raw_policy(void)
{
    CHECK(kneil_raw_stop_on_connect(true, false));    /* connected, idle -> stop once */
    CHECK(!kneil_raw_stop_on_connect(true, true));    /* session/calibration owns RAW */
    CHECK(!kneil_raw_stop_on_connect(false, false));  /* not connected */
}

int main(void)
{
    test_angle_basic();
    test_angle_calibration();
    test_angle_trust();
    test_angle_standing();
    test_angle_display();
    test_angle_noise();
    test_session_clean_rep();
    test_session_partial();
    test_session_short_hold();
    test_session_jitter();
    test_session_gap_and_lost();
    test_session_bend_and_slow();
    test_session_curl();
    test_session_sets();
    test_raw_policy();

    printf("therapy tests: %d checks, %d failed\n", checks, fails);
    printf("THERAPY TEST: %s\n", fails ? "FAILED" : "PASS");
    return fails ? 1 : 0;
}
