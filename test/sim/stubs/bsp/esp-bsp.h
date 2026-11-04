#pragma once
/* Host stand-in for the Waveshare BSP (UI simulator only). */
#include "esp_err.h"
esp_err_t bsp_display_brightness_set(int brightness_percent);
