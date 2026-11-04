#include <math.h>
#include <stdio.h>

#include "ui.h"
#include "ui_internal.h"

#define REFRESH_MS 500

lv_obj_t *ui_scr_home;
lv_obj_t *ui_scr_menu;
lv_obj_t *ui_scr_trend;
lv_obj_t *ui_scr_diag;
lv_obj_t *ui_scr_calib;
lv_obj_t *ui_scr_settings;
lv_obj_t *ui_scr_time;

/* ------------------------------------------------------------------
   Helpers
   ------------------------------------------------------------------ */

lv_obj_t *ui_screen_create(const char *title)
{
    lv_obj_t *scr = lv_obj_create(NULL);

    lv_obj_set_style_bg_color(scr, UI_COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, UI_COL_TEXT, 0);
    lv_obj_set_style_pad_all(scr, UI_PAD, 0);
    lv_obj_set_style_pad_row(scr, 12, 0);

    /* Column layout; content scrolls if it outgrows the panel. */
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(scr, LV_DIR_VER);

    if(title != NULL)
    {
        ui_label(scr, title, &lv_font_montserrat_22, UI_COL_TEXT);
    }

    return scr;
}

lv_obj_t *ui_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);

    lv_obj_set_size(btn, 300, 58);
    lv_obj_set_style_radius(btn, 16, 0);

    if(cb != NULL)
    {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_center(label);

    return btn;
}

lv_obj_t *ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);

    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);

    return l;
}

lv_obj_t *ui_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);

    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    return row;
}

void ui_go(lv_obj_t *screen)
{
    lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_IN, 150, 0, false);
}

void ui_go_menu_cb(lv_event_t *e)
{
    (void)e;
    ui_go(ui_scr_menu);
}

void ui_fmt(char *buf, size_t len, float v, int decimals)
{
    if(!isfinite(v))
    {
        snprintf(buf, len, "--");
        return;
    }

    snprintf(buf, len, "%.*f", decimals, (double)v);
}

/* ------------------------------------------------------------------
   Menu
   ------------------------------------------------------------------ */

static void go_cb(lv_event_t *e)
{
    ui_go((lv_obj_t *)lv_event_get_user_data(e));
}

static void create_menu(void)
{
    ui_scr_menu = ui_screen_create("MENU");

    ui_button(ui_scr_menu, LV_SYMBOL_IMAGE "  Trends",     go_cb, ui_scr_trend);
    ui_button(ui_scr_menu, LV_SYMBOL_EDIT "  Calibrate",   go_cb, ui_scr_calib);
    ui_button(ui_scr_menu, LV_SYMBOL_LIST "  Diagnostics", go_cb, ui_scr_diag);
    ui_button(ui_scr_menu, LV_SYMBOL_SETTINGS "  Settings", go_cb, ui_scr_settings);
    ui_button(ui_scr_menu, LV_SYMBOL_HOME "  Watch face",  go_cb, ui_scr_home);
}

/* ------------------------------------------------------------------
   Refresh timer
   ------------------------------------------------------------------ */

static void refresh_cb(lv_timer_t *t)
{
    (void)t;

    static unsigned tick;
    lv_obj_t *active = lv_screen_active();

    tick++;

    if(active == ui_scr_home)          ui_home_refresh();
    else if(active == ui_scr_diag)     ui_diag_refresh();
    else if(active == ui_scr_calib)    ui_calib_refresh();
    else if(active == ui_scr_settings) ui_settings_refresh();
    else if(active == ui_scr_trend && (tick % 2) == 0) ui_trend_refresh();
}

void ui_init(void)
{
    /* Content screens first: the menu holds pointers to them. */
    ui_home_create();
    ui_trend_create();
    ui_diag_create();
    ui_calib_create();
    ui_settings_create();
    ui_time_create();
    create_menu();

    lv_screen_load(ui_scr_home);
    ui_home_refresh();

    lv_timer_create(refresh_cb, REFRESH_MS, NULL);
}
