#pragma once
/* Host stand-in for esp_log.h: logging compiled out in unit tests. */
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGD(tag, ...) ((void)(tag))
#define ESP_ERROR_CHECK(x) ((void)(x))
