#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "lvgl.h"

/* Display is 410 x 502. */
#define UI_W            410
#define UI_H            502
#define UI_PAD          16

#define UI_COL_BG       lv_color_hex(0x000000)
#define UI_COL_CARD     lv_color_hex(0x15171c)
#define UI_COL_TRACK    lv_color_hex(0x23262d)
#define UI_COL_TEXT     lv_color_hex(0xffffff)
#define UI_COL_DIM      lv_color_hex(0x8a8f98)
#define UI_COL_FAINT    lv_color_hex(0x3a3d44)
#define UI_COL_K        lv_color_hex(0xffa040)   /* K+ series / accents */
#define UI_COL_PH       lv_color_hex(0x40c8ff)   /* pH series / accents */
#define UI_COL_OK       lv_color_hex(0x3ddc84)
#define UI_COL_WARN     lv_color_hex(0xffc107)
#define UI_COL_ALERT    lv_color_hex(0xff4d4d)
#define UI_COL_BLE      lv_color_hex(0x4d8dff)

/* Upper end of the "normal" K+ band on the gauge. Between this and the
   alert threshold the gauge shows amber. Placeholder like the threshold. */
#define UI_K_NORMAL_MAX 5.0f

/* Swipeable pages, left to right. */
typedef enum
{
    UI_PAGE_FACE = 0,
    UI_PAGE_TREND,
    UI_PAGE_CALIB,
    UI_PAGE_DIAG,
    UI_PAGE_SETTINGS,
    UI_PAGE_COUNT
} ui_page_t;

extern lv_obj_t *ui_scr_main;     /* holds the tileview */
extern lv_obj_t *ui_scr_time;     /* set-time screen, opened from Settings */
extern lv_obj_t *ui_tv;
extern lv_obj_t *ui_tiles[UI_PAGE_COUNT];

/* ---- shared helpers (ui.c) ------------------------------------------ */

/* Turns a tile into a vertically scrolling page with a header.
   Content is added to the returned tile. */
lv_obj_t *ui_page(ui_page_t page, const char *icon, const char *title, lv_color_t accent);
lv_obj_t *ui_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user_data);
lv_obj_t *ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color);
lv_obj_t *ui_row(lv_obj_t *parent);
lv_obj_t *ui_card(lv_obj_t *parent);
void      ui_goto_page(ui_page_t page);
void      ui_open_time(void);
void      ui_close_time(void);

/* "--" for NAN, otherwise fixed decimals. */
void ui_fmt(char *buf, size_t len, float v, int decimals);

/* ---- pages ------------------------------------------------------------
   Refreshers run from the UI timer in the LVGL task, only for the page
   that is visible. */

void ui_face_create(lv_obj_t *tile);
void ui_face_refresh(void);
void ui_face_refresh_slow(void);     /* sparkline + trend arrow, every 2 s */
void ui_trend_create(lv_obj_t *tile);
void ui_trend_refresh(void);
void ui_calib_create(lv_obj_t *tile);
void ui_calib_refresh(void);
void ui_diag_create(lv_obj_t *tile);
void ui_diag_refresh(void);
void ui_settings_create(lv_obj_t *tile);
void ui_settings_refresh(void);
void ui_time_create(void);

/* Full-screen K+ alert on the top layer, checked on every refresh. */
void ui_alert_create(void);
void ui_alert_update(void);

/* K+ change over the last 10 min in mM, NAN if not enough data.
   Shared by the face and the alert. */
float ui_k_rate_per_10min(void);
