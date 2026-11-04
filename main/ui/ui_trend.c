#include <math.h>
#include <stdio.h>

#include "esp_heap_caps.h"

#include "app_state.h"

#include "ui_internal.h"

/* Chart resolution. Longer spans are averaged down to this many points. */
#define CHART_POINTS   240
#define MAX_SAMPLES    1800      /* app_state history length (30 min @ 1 Hz) */
#define CHART_W        (UI_W - 2 * UI_PAD)
#define CHART_H        130
#define SCALE          100       /* charts hold value * 100 as int32 */

static const int SPANS_S[] = { 300, 1800 };
static const char *SPAN_TXT[] = { "5 min", "30 min" };

typedef struct
{
    lv_obj_t           *chart;
    lv_chart_series_t  *series;
    lv_chart_series_t  *limit;     /* alert threshold line (K+ only) */
    lv_obj_t           *header;
    lv_obj_t           *range;
} trend_t;

static trend_t k_trend;
static trend_t ph_trend;
static lv_obj_t *span_btn_lbl;
static lv_obj_t *note_lbl;
static int span_idx;

static float *buf_k;
static float *buf_ph;

static void span_cb(lv_event_t *e)
{
    (void)e;
    span_idx = (span_idx + 1) % (int)(sizeof(SPANS_S) / sizeof(SPANS_S[0]));
    lv_label_set_text_fmt(span_btn_lbl, LV_SYMBOL_LOOP "  %s", SPAN_TXT[span_idx]);
    ui_trend_refresh();
}

static void trend_create(trend_t *t, lv_obj_t *parent, const char *name, lv_color_t color, bool with_limit)
{
    t->header = ui_label(parent, name, &lv_font_montserrat_18, color);
    lv_obj_set_width(t->header, LV_PCT(100));

    t->chart = lv_chart_create(parent);
    lv_obj_set_size(t->chart, CHART_W, CHART_H);
    lv_chart_set_type(t->chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(t->chart, CHART_POINTS);
    lv_chart_set_update_mode(t->chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_div_line_count(t->chart, 4, 0);

    lv_obj_set_style_bg_color(t->chart, UI_COL_CARD, 0);
    lv_obj_set_style_border_width(t->chart, 0, 0);
    lv_obj_set_style_line_color(t->chart, lv_color_hex(0x2a2e36), LV_PART_MAIN);
    lv_obj_set_style_line_width(t->chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(t->chart, 0, 0, LV_PART_INDICATOR);   /* no point dots */

    t->series = lv_chart_add_series(t->chart, color, LV_CHART_AXIS_PRIMARY_Y);
    t->limit  = with_limit ? lv_chart_add_series(t->chart, UI_COL_ALERT, LV_CHART_AXIS_PRIMARY_Y) : NULL;

    lv_chart_set_all_values(t->chart, t->series, LV_CHART_POINT_NONE);

    if(t->limit)
    {
        lv_chart_set_all_values(t->chart, t->limit, LV_CHART_POINT_NONE);
    }

    t->range = ui_label(parent, "", &lv_font_montserrat_14, UI_COL_DIM);
    lv_obj_set_width(t->range, LV_PCT(100));
}

void ui_trend_create(void)
{
    ui_scr_trend = ui_screen_create("TRENDS");
    lv_obj_set_style_pad_row(ui_scr_trend, 6, 0);

    buf_k  = heap_caps_malloc(MAX_SAMPLES * sizeof(float), MALLOC_CAP_SPIRAM);
    buf_ph = heap_caps_malloc(MAX_SAMPLES * sizeof(float), MALLOC_CAP_SPIRAM);

    trend_create(&k_trend, ui_scr_trend, "K+ (mM)", UI_COL_K, true);
    trend_create(&ph_trend, ui_scr_trend, "pH", UI_COL_PH, false);

    note_lbl = ui_label(ui_scr_trend, "", &lv_font_montserrat_14, UI_COL_WARN);

    lv_obj_t *row = ui_row(ui_scr_trend);

    lv_obj_t *span = ui_button(row, "", span_cb, NULL);
    lv_obj_set_size(span, 180, 54);
    span_btn_lbl = lv_obj_get_child(span, 0);
    lv_label_set_text_fmt(span_btn_lbl, LV_SYMBOL_LOOP "  %s", SPAN_TXT[span_idx]);

    lv_obj_t *back = ui_button(row, LV_SYMBOL_LEFT "  Back", ui_go_menu_cb, NULL);
    lv_obj_set_size(back, 180, 54);
}

/* Averages n_in samples (NAN-aware) into CHART_POINTS buckets,
   right-aligned so the newest data is always at the right edge. */
static void fill_chart(trend_t *t, const float *in, int n_in, int span_samples,
                       int decimals, float min_span, float threshold)
{
    int32_t *y = lv_chart_get_series_y_array(t->chart, t->series);
    float per_point = (float)span_samples / CHART_POINTS;
    float lo = INFINITY;
    float hi = -INFINITY;
    float last = NAN;

    for(int p = 0; p < CHART_POINTS; p++)
    {
        /* Sample range covered by this point, counted back from newest. */
        int from = n_in - span_samples + (int)(p * per_point);
        int to   = n_in - span_samples + (int)((p + 1) * per_point);

        if(to <= from) to = from + 1;

        double sum = 0.0;
        int cnt = 0;

        for(int i = from; i < to; i++)
        {
            if(i >= 0 && i < n_in && isfinite(in[i]))
            {
                sum += in[i];
                cnt++;
            }
        }

        if(cnt == 0)
        {
            y[p] = LV_CHART_POINT_NONE;
            continue;
        }

        float v = (float)(sum / cnt);
        y[p] = (int32_t)lroundf(v * SCALE);

        if(v < lo) lo = v;
        if(v > hi) hi = v;
        last = v;
    }

    if(!isfinite(lo))
    {
        lo = 0.0f;
        hi = min_span;
    }

    if(isfinite(threshold))
    {
        if(threshold > hi) hi = threshold;
        if(threshold < lo) lo = threshold;
    }

    /* Pad and enforce a minimum visible span so noise is not magnified. */
    float span = hi - lo;

    if(span < min_span)
    {
        float mid = (hi + lo) / 2.0f;
        lo = mid - min_span / 2.0f;
        hi = mid + min_span / 2.0f;
        span = min_span;
    }

    lo -= span * 0.1f;
    hi += span * 0.1f;

    lv_chart_set_axis_range(t->chart, LV_CHART_AXIS_PRIMARY_Y,
                            (int32_t)floorf(lo * SCALE), (int32_t)ceilf(hi * SCALE));

    if(t->limit != NULL)
    {
        lv_chart_set_all_values(t->chart, t->limit,
                                isfinite(threshold) ? (int32_t)lroundf(threshold * SCALE)
                                                    : LV_CHART_POINT_NONE);
    }

    lv_chart_refresh(t->chart);

    char a[16], b[16], c[16];
    ui_fmt(a, sizeof(a), last, decimals);
    ui_fmt(b, sizeof(b), lo + span * 0.1f, decimals);
    ui_fmt(c, sizeof(c), hi - span * 0.1f, decimals);

    if(isfinite(last))
    {
        lv_label_set_text_fmt(t->range, "now %s    axis %s - %s", a, b, c);
    }
    else
    {
        lv_label_set_text(t->range, "no calibrated data in this window");
    }
}

void ui_trend_refresh(void)
{
    if(buf_k == NULL || buf_ph == NULL)
    {
        return;
    }

    int span = SPANS_S[span_idx];
    int n = app_state_history(buf_k, buf_ph, span);

    app_settings_t s;
    app_state_get_settings(&s);

    fill_chart(&k_trend, buf_k, n, span, 2, 1.0f, s.k_alert_mM);
    fill_chart(&ph_trend, buf_ph, n, span, 2, 0.4f, NAN);

    data_source_t src = app_state_active_source();

    lv_label_set_text(note_lbl,
        src == DATA_SRC_DEMO ? "DEMO DATA - not a measurement" :
        src == DATA_SRC_NONE ? "no live data" : "");
}
