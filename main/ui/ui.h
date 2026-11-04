#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Builds every screen and starts the refresh timer.
   Call with the display lock held (bsp_display_lock). */
void ui_init(void);

#ifdef __cplusplus
}
#endif
