#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "nvs.h"

#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"

#include "therapy_audio.h"
#include "therapy_voice_data.h"

static const char *TAG = "therapy-audio";

/* the binary is embedded by main/CMakeLists.txt (EMBED_FILES therapy_voice.bin) */
extern const uint8_t therapy_voice_pcm[] asm("_binary_therapy_voice_bin_start");
extern const uint8_t therapy_voice_pcm_end[] asm("_binary_therapy_voice_bin_end");

#define AUDIO_QUEUE_LEN 8
#define AUDIO_VOLUME_DEFAULT 70
#define NVS_NAMESPACE "therapy"
#define NVS_VOLUME "vol"
#define NVS_MUTED "mute"

static esp_codec_dev_handle_t speaker;
static QueueHandle_t queue;
static int volume = AUDIO_VOLUME_DEFAULT;
static bool muted;
static nvs_handle_t nvs;
static bool nvs_ok;

static void nvs_load(void)
{
    if(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK)
    {
        return;
    }

    nvs_ok = true;

    uint8_t v = AUDIO_VOLUME_DEFAULT;
    uint8_t m = 0;

    if(nvs_get_u8(nvs, NVS_VOLUME, &v) == ESP_OK && v <= 100)
    {
        volume = v;
    }

    if(nvs_get_u8(nvs, NVS_MUTED, &m) == ESP_OK)
    {
        muted = (m != 0);
    }
}

void therapy_audio_save(void)
{
    if(!nvs_ok)
    {
        return;
    }

    nvs_set_u8(nvs, NVS_VOLUME, (uint8_t)volume);
    nvs_set_u8(nvs, NVS_MUTED, muted ? 1 : 0);
    nvs_commit(nvs);
}

int therapy_audio_volume(void)
{
    return volume;
}

void therapy_audio_set_volume(int percent)
{
    if(percent < 0) percent = 0;
    if(percent > 100) percent = 100;

    volume = percent;
}

bool therapy_audio_muted(void)
{
    return muted;
}

void therapy_audio_set_muted(bool mute)
{
    muted = mute;
}

bool therapy_audio_ready(void)
{
    return speaker != NULL;
}

static void audio_task(void *arg)
{
    (void)arg;

    while(1)
    {
        int clip;

        if(xQueueReceive(queue, &clip, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        if(clip < 0 || clip >= THERAPY_VOICE_COUNT || muted || volume == 0)
        {
            continue;
        }

        const uint8_t *p = therapy_voice_pcm + therapy_voice_offset[clip];
        const int len = (int)therapy_voice_len[clip];

        if(p + len > therapy_voice_pcm_end)
        {
            ESP_LOGW(TAG, "clip %d outside the embedded data", clip);
            continue;
        }

        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 1,
            .channel_mask = 0,
            .sample_rate = THERAPY_VOICE_SAMPLE_RATE,
            .mclk_multiple = 0,
        };

        if(esp_codec_dev_open(speaker, &fs) != ESP_CODEC_DEV_OK)
        {
            ESP_LOGW(TAG, "codec open failed");
            continue;
        }

        esp_codec_dev_set_out_vol(speaker, volume);
        const int rc = esp_codec_dev_write(speaker, (void *)p, len);
        esp_codec_dev_close(speaker);

        if(rc != ESP_CODEC_DEV_OK)
        {
            ESP_LOGW(TAG, "speaker write failed: %d", rc);
        }
    }
}

void therapy_audio_init(void)
{
    nvs_load();

    speaker = bsp_audio_codec_speaker_init();

    if(speaker == NULL)
    {
        ESP_LOGW(TAG, "speaker codec unavailable: therapy cues fall back to vibration");
        return;
    }

    queue = xQueueCreate(AUDIO_QUEUE_LEN, sizeof(int));

    if(queue == NULL || xTaskCreate(audio_task, "therapy_audio", 4096, NULL, 4, NULL) != pdPASS)
    {
        ESP_LOGW(TAG, "audio task not started");
        speaker = NULL;
        return;
    }

    ESP_LOGI(TAG, "speaker ready, volume %d%s", volume, muted ? " (muted)" : "");
}

bool therapy_audio_play(therapy_clip_t clip)
{
    if(speaker == NULL || queue == NULL || clip < 0 || clip >= THERAPY_VOICE_COUNT)
    {
        return false;
    }

    const int id = (int)clip;

    return xQueueSend(queue, &id, 0) == pdTRUE;
}

bool therapy_audio_play_rep(int rep)
{
    if(rep < 1 || rep > 20)
    {
        return false;
    }

    return therapy_audio_play((therapy_clip_t)(THERAPY_CLIP_REP_01 + rep - 1));
}
