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
#define UI_COL_TEXT     lv_color_hex(0xffffff)
#define UI_COL_DIM      lv_color_hex(0x8a8f98)
#define UI_COL_K        lv_color_hex(0xffa040)   /* K+ series / accents */
#define UI_COL_PH       lv_color_hex(0x40c8ff)   /* pH series / accents */
#define UI_COL_OK       lv_color_hex(0x3ddc84)
#define UI_COL_WARN     lv_color_hex(0xffc107)
#define UI_COL_ALERT    lv_color_hex(0xff4d4d)
#define UI_COL_BLE      lv_color_hex(0x4d8dff)

/* Screens */
extern lv_obj_t *ui_scr_home;
extern lv_obj_t *ui_scr_menu;
extern lv_obj_t *ui_scr_trend;
extern lv_obj_t *ui_scr_diag;
extern lv_obj_t *ui_scr_calib;
extern lv_obj_t *ui_scr_settings;
extern lv_obj_t *ui_scr_time;

/* Shared helpers (ui.c) */
lv_obj_t *ui_screen_create(const char *title);
lv_obj_t *ui_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user_data);
lv_obj_t *ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color);
lv_obj_t *ui_row(lv_obj_t *parent);
void      ui_go(lv_obj_t *screen);
void      ui_go_menu_cb(lv_event_t *e);

/* "--" for NAN, otherwise fixed decimals. */
void ui_fmt(char *buf, size_t len, float v, int decimals);

/* Per-screen builders and refreshers. Refreshers run from the UI timer
   in the LVGL task, only for the screen that is visible. */
void ui_home_create(void);
void ui_home_refresh(void);
void ui_trend_create(void);
void ui_trend_refresh(void);
void ui_diag_create(void);
void ui_diag_refresh(void);
void ui_calib_create(void);
void ui_calib_refresh(void);
void ui_settings_create(void);
void ui_settings_refresh(void);
void ui_time_create(void);
