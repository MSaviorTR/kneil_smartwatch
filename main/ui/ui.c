#include <math.h>
#include <stdio.h>

#include "ui.h"
#include "ui_internal.h"

#define REFRESH_MS      250
#define SPLASH_MS       1800

lv_obj_t *ui_scr_main;
lv_obj_t *ui_scr_time;
lv_obj_t *ui_tv;
lv_obj_t *ui_tiles[UI_PAGE_COUNT];

static lv_obj_t *splash;

static void splash_deleted(lv_event_t *e)
{
    (void)e;
    splash = NULL;
}

/* ------------------------------------------------------------------
   Helpers
   ------------------------------------------------------------------ */

lv_obj_t *ui_page(ui_page_t page, const char *icon, const char *title, lv_color_t accent)
{
    lv_obj_t *tile = ui_tiles[page];

    lv_obj_set_style_text_color(tile, UI_COL_TEXT, 0);
    lv_obj_set_style_pad_hor(tile, UI_PAD, 0);
    lv_obj_set_style_pad_top(tile, 18, 0);
    lv_obj_set_style_pad_bottom(tile, 44, 0);     /* room for the page dots */
    lv_obj_set_style_pad_row(tile, 12, 0);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(tile, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(tile, LV_SCROLLBAR_MODE_OFF);

    /* Header: coloured icon + title, thin accent underline. */
    lv_obj_t *head = ui_row(tile);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui_label(head, icon, &lv_font_montserrat_22, accent);
    ui_label(head, title, &lv_font_montserrat_22, UI_COL_TEXT);

    lv_obj_t *line = lv_obj_create(tile);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, 48, 3);
    lv_obj_set_style_bg_color(line, accent, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(line, 2, 0);

    return tile;
}

lv_obj_t *ui_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);

    lv_obj_set_size(btn, LV_PCT(100), 56);
    lv_obj_set_style_radius(btn, 18, 0);
    lv_obj_set_style_bg_color(btn, UI_COL_CARD, 0);
    lv_obj_set_style_bg_color(btn, UI_COL_TRACK, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, UI_COL_FAINT, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_text_color(btn, UI_COL_TEXT, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_50, LV_STATE_DISABLED);
    lv_obj_set_style_text_opa(btn, LV_OPA_40, LV_STATE_DISABLED);

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

lv_obj_t *ui_card(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);

    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, UI_COL_CARD, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    return card;
}

/* One row of page dots on the main screen, above the tileview. */
static lv_obj_t *dots[UI_PAGE_COUNT];

static void create_dots(void)
{
    lv_obj_t *row = lv_obj_create(ui_scr_main);

    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, 10);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 7, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -10);

    /* Black pill behind the dots keeps them legible over scrolled content. */
    lv_obj_set_size(row, LV_SIZE_CONTENT, 22);
    lv_obj_set_style_pad_hor(row, 10, 0);
    lv_obj_set_style_radius(row, 11, 0);
    lv_obj_set_style_bg_color(row, UI_COL_BG, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_80, 0);

    for(int i = 0; i < UI_PAGE_COUNT; i++)
    {
        dots[i] = lv_obj_create(row);
        lv_obj_remove_style_all(dots[i]);
        lv_obj_set_style_radius(dots[i], 4, 0);
        lv_obj_set_style_bg_opa(dots[i], LV_OPA_COVER, 0);
        lv_obj_remove_flag(dots[i], LV_OBJ_FLAG_CLICKABLE);
    }
}

static void update_dots(void)
{
    lv_obj_t *tile = lv_tileview_get_tile_active(ui_tv);

    /* Before the first scroll there is no active tile yet: that's the face. */
    if(tile == NULL)
    {
        tile = ui_tiles[UI_PAGE_FACE];
    }

    for(int i = 0; i < UI_PAGE_COUNT; i++)
    {
        bool on = (tile == ui_tiles[i]);

        lv_obj_set_size(dots[i], on ? 22 : 8, 8);
        lv_obj_set_style_bg_color(dots[i], on ? UI_COL_TEXT : UI_COL_FAINT, 0);
    }
}

void ui_goto_page(ui_page_t page)
{
    if(lv_screen_active() != ui_scr_main)
    {
        lv_screen_load_anim(ui_scr_main, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, false);
    }

    lv_tileview_set_tile_by_index(ui_tv, page, 0, LV_ANIM_ON);
}

void ui_open_time(void)
{
    lv_screen_load_anim(ui_scr_time, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

void ui_close_time(void)
{
    lv_screen_load_anim(ui_scr_main, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, false);
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
   Splash
   ------------------------------------------------------------------ */

static void create_splash(void)
{
    splash = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(splash, UI_COL_BG, 0);
    lv_obj_set_style_bg_opa(splash, LV_OPA_COVER, 0);
    lv_obj_remove_flag(splash, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(splash, splash_deleted, LV_EVENT_DELETE, NULL);

    lv_obj_t *spin = lv_spinner_create(splash);
    lv_spinner_set_anim_params(spin, 1200, 90);
    lv_obj_set_size(spin, 120, 120);
    lv_obj_align(spin, LV_ALIGN_CENTER, 0, -70);
    lv_obj_set_style_arc_width(spin, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spin, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spin, UI_COL_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_color(spin, UI_COL_K, LV_PART_INDICATOR);

    lv_obj_t *k = ui_label(splash, "K+", &lv_font_montserrat_26, UI_COL_TEXT);
    lv_obj_align_to(k, spin, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *title = ui_label(splash, "GUDER WATCH", &lv_font_montserrat_26, UI_COL_TEXT);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 40);

    lv_obj_t *sub = ui_label(splash, "K+ / pH biosensor readout", &lv_font_montserrat_16, UI_COL_DIM);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 76);

    lv_obj_t *lab = ui_label(splash, "Guder Lab  -  Imperial College London", &lv_font_montserrat_14, UI_COL_FAINT);
    lv_obj_align(lab, LV_ALIGN_BOTTOM_MID, 0, -28);
}

/* ------------------------------------------------------------------
   Refresh timer
   ------------------------------------------------------------------ */

static void refresh_cb(lv_timer_t *t)
{
    (void)t;

    static unsigned tick;
    tick++;

    lv_obj_t *scr = lv_screen_active();

    if(scr == splash)
    {
        return;
    }

    ui_alert_update();

    if(scr != ui_scr_main)
    {
        return;
    }

    /* 250 ms tick: the face animates every tick, slower pages less often. */
    lv_obj_t *tile = lv_tileview_get_tile_active(ui_tv);

    if(tile == ui_tiles[UI_PAGE_FACE])
    {
        /* In KNEIL mode the first page is the KNEIL monitor instead of the watch face. */
        if(ui_kneil_active())
        {
            ui_kneil_refresh();
        }
        else
        {
            ui_face_refresh();

            if(tick % 8 == 1)
            {
                ui_face_refresh_slow();
            }
        }
    }
    else if(tile == ui_tiles[UI_PAGE_TREND] && tick % 8 == 1) ui_trend_refresh();
    else if(tile == ui_tiles[UI_PAGE_CALIB] && tick % 2 == 0) ui_calib_refresh();
    else if(tile == ui_tiles[UI_PAGE_DIAG] && tick % 2 == 0)  ui_diag_refresh();
    else if(tile == ui_tiles[UI_PAGE_SETTINGS] && tick % 4 == 0) ui_settings_refresh();
}

/* Refresh a page the moment it is swiped in, not up to 2 s later. */
static void tile_changed(lv_event_t *e)
{
    (void)e;

    lv_obj_t *tile = lv_tileview_get_tile_active(ui_tv);

    update_dots();

    if(tile == ui_tiles[UI_PAGE_FACE])          { if(ui_kneil_active()) ui_kneil_refresh(); else ui_face_refresh_slow(); }
    else if(tile == ui_tiles[UI_PAGE_TREND])    ui_trend_refresh();
    else if(tile == ui_tiles[UI_PAGE_CALIB])    ui_calib_refresh();
    else if(tile == ui_tiles[UI_PAGE_DIAG])     ui_diag_refresh();
    else if(tile == ui_tiles[UI_PAGE_SETTINGS]) ui_settings_refresh();
}

void ui_init(void)
{
    create_splash();
    lv_screen_load(splash);

    ui_scr_main = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_scr_main, UI_COL_BG, 0);
    lv_obj_set_style_bg_opa(ui_scr_main, LV_OPA_COVER, 0);

    ui_tv = lv_tileview_create(ui_scr_main);
    lv_obj_set_size(ui_tv, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ui_tv, UI_COL_BG, 0);
    lv_obj_set_scrollbar_mode(ui_tv, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(ui_tv, tile_changed, LV_EVENT_VALUE_CHANGED, NULL);

    for(int i = 0; i < UI_PAGE_COUNT; i++)
    {
        lv_dir_t dir = (i == 0) ? LV_DIR_RIGHT :
                       (i == UI_PAGE_COUNT - 1) ? LV_DIR_LEFT : LV_DIR_HOR;

        ui_tiles[i] = lv_tileview_add_tile(ui_tv, (uint8_t)i, 0, dir);
        lv_obj_set_style_bg_color(ui_tiles[i], UI_COL_BG, 0);
        lv_obj_set_style_bg_opa(ui_tiles[i], LV_OPA_COVER, 0);
    }

    create_dots();
    update_dots();

    if(ui_kneil_active())
    {
        ui_kneil_create(ui_tiles[UI_PAGE_FACE]);   /* KNEIL mode: page 1 is the KNEIL monitor */
    }
    else
    {
        ui_face_create(ui_tiles[UI_PAGE_FACE]);
    }
    ui_trend_create(ui_tiles[UI_PAGE_TREND]);
    ui_calib_create(ui_tiles[UI_PAGE_CALIB]);
    ui_diag_create(ui_tiles[UI_PAGE_DIAG]);
    ui_settings_create(ui_tiles[UI_PAGE_SETTINGS]);
    ui_time_create();
    ui_alert_create();

    if(ui_kneil_active())
    {
        ui_kneil_refresh();
    }
    else
    {
        ui_face_refresh();
        ui_face_refresh_slow();
    }

    /* Splash fades into the watch face and is freed afterwards. */
    lv_screen_load_anim(ui_scr_main, LV_SCR_LOAD_ANIM_FADE_IN, 500, SPLASH_MS, true);

    lv_timer_create(refresh_cb, REFRESH_MS, NULL);
}
