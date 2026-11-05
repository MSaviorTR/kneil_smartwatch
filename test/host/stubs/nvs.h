#pragma once
/* Host stand-in for NVS: a tiny in-memory key/value store. */
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
#define ESP_ERR_NVS_NOT_FOUND          0x1102
#define ESP_ERR_NVS_NO_FREE_PAGES      0x110d
#define ESP_ERR_NVS_NEW_VERSION_FOUND  0x1110
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *len);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *val, size_t len);
esp_err_t nvs_commit(nvs_handle_t h);
void      nvs_close(nvs_handle_t h);
