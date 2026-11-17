#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "host/ble_hs.h"
#include "host/util/util.h"

#include "app_state.h"
#include "ble_central.h"
#include "ble_uuids.h"
#include "sensor_protocol.h"

static const char *TAG = "ble-central";


static const ble_uuid128_t SENSOR_SVC_UUID    = GW_UUID(0x10);
static const ble_uuid128_t SENSOR_FRAMES_UUID = GW_UUID(0x11);
static const ble_uuid16_t  CCCD_UUID          = BLE_UUID16_INIT(0x2902);

#define CONNECT_TIMEOUT_MS  10000
#define RESCAN_DELAY_MS     1000

/* NimBLE calls everything below from its host task. The status block is
   also read from the UI and console, hence the small lock. */
static volatile bool active;
static volatile bool synced;
static uint8_t       own_addr_type;

static uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t svc_start;
static uint16_t svc_end;
static uint16_t frames_handle;
static uint16_t cccd_handle;

static sensor_line_buf_t  rx;              /* reassembles $GKS frames */
static esp_timer_handle_t rescan_timer;

static SemaphoreHandle_t    status_lock;
static ble_central_status_t status;

static int  gap_event(struct ble_gap_event *event, void *arg);
static void start_scan(void);

/* ------------------------------------------------------------------ */

static void set_status(bool scanning, bool connected, bool subscribed)
{
    xSemaphoreTake(status_lock, portMAX_DELAY);
    status.active     = active;
    status.scanning   = scanning;
    status.connected  = connected;
    status.subscribed = subscribed;
    xSemaphoreGive(status_lock);
}

static void drop_connection(const char *why)
{
    if(conn != BLE_HS_CONN_HANDLE_NONE)
    {
        ESP_LOGW(TAG, "disconnecting: %s", why);
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static void rescan_later(void)
{
    if(active && rescan_timer != NULL)
    {
        esp_timer_stop(rescan_timer);
        esp_timer_start_once(rescan_timer, (uint64_t)RESCAN_DELAY_MS * 1000);
    }
}

static void rescan_cb(void *arg)
{
    (void)arg;
    start_scan();
}

/* ------------------------------------------------------------------
   Incoming frames
   ------------------------------------------------------------------ */

static void on_line(const char *line, void *ctx)
{
    (void)ctx;

    sensor_frame_t f;

    switch(sensor_protocol_parse(line, &f))
    {
        case SENSOR_PARSE_OK:
            app_state_push_frame(&f, DATA_SRC_SENSOR);
            xSemaphoreTake(status_lock, portMAX_DELAY);
            status.frames++;
            xSemaphoreGive(status_lock);
            break;

        case SENSOR_PARSE_ERR_CHECKSUM:
            app_state_count_bad_frame(true);
            break;

        case SENSOR_PARSE_ERR_FORMAT:
            if(line[0] == '$')
            {
                app_state_count_bad_frame(false);
            }
            break;

        case SENSOR_PARSE_IGNORED:
            break;
    }
}

static void handle_notification(struct os_mbuf *om)
{
    uint8_t buf[256];
    uint16_t len = OS_MBUF_PKTLEN(om);

    if(len > sizeof(buf))
    {
        len = sizeof(buf);
    }

    if(ble_hs_mbuf_to_flat(om, buf, len, &len) == 0)
    {
        sensor_feed_chunk(&rx, buf, len, on_line, NULL);
    }
}

/* ------------------------------------------------------------------
   GATT discovery: service -> characteristic -> CCCD -> subscribe
   ------------------------------------------------------------------ */

static int on_subscribed(uint16_t c, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    (void)c; (void)attr; (void)arg;

    if(err->status != 0)
    {
        drop_connection("subscribe failed");
        return 0;
    }

    ESP_LOGI(TAG, "subscribed to sensor frames");
    set_status(false, true, true);
    return 0;
}

static int on_dsc(uint16_t c, const struct ble_gatt_error *err, uint16_t chr_val_handle,
                  const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)chr_val_handle; (void)arg;

    if(err->status == 0 && dsc != NULL)
    {
        if(ble_uuid_cmp(&dsc->uuid.u, &CCCD_UUID.u) == 0)
        {
            cccd_handle = dsc->handle;
        }

        return 0;
    }

    if(err->status != BLE_HS_EDONE || cccd_handle == 0)
    {
        drop_connection("no notify descriptor");
        return 0;
    }

    static const uint8_t enable_notify[2] = { 0x01, 0x00 };

    if(ble_gattc_write_flat(c, cccd_handle, enable_notify, sizeof(enable_notify), on_subscribed, NULL) != 0)
    {
        drop_connection("subscribe write failed");
    }

    return 0;
}

static int on_chr(uint16_t c, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;

    if(err->status == 0 && chr != NULL)
    {
        frames_handle = chr->val_handle;
        return 0;
    }

    if(err->status != BLE_HS_EDONE || frames_handle == 0)
    {
        drop_connection("frames characteristic not found");
        return 0;
    }

    if(ble_gattc_disc_all_dscs(c, frames_handle, svc_end, on_dsc, NULL) != 0)
    {
        drop_connection("descriptor discovery failed");
    }

    return 0;
}

static int on_svc(uint16_t c, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg)
{
    (void)arg;

    if(err->status == 0 && svc != NULL)
    {
        svc_start = svc->start_handle;
        svc_end   = svc->end_handle;
        return 0;
    }

    if(err->status != BLE_HS_EDONE || svc_start == 0)
    {
        drop_connection("Guder Sensor service not found");
        return 0;
    }

    if(ble_gattc_disc_chrs_by_uuid(c, svc_start, svc_end, &SENSOR_FRAMES_UUID.u, on_chr, NULL) != 0)
    {
        drop_connection("characteristic discovery failed");
    }

    return 0;
}

static int on_mtu(uint16_t c, const struct ble_gatt_error *err, uint16_t mtu, void *arg)
{
    (void)c; (void)arg;

    if(err->status == 0)
    {
        ESP_LOGI(TAG, "MTU %u", mtu);
    }

    return 0;
}

/* ------------------------------------------------------------------
   GAP: scan, match, connect
   ------------------------------------------------------------------ */

/* A device is "our sensor" if it advertises the Guder Sensor service, or
   (fallback for simple sensors) its name starts with the configured prefix. */
static bool is_sensor(const struct ble_hs_adv_fields *f)
{
    for(int i = 0; i < f->num_uuids128; i++)
    {
        if(ble_uuid_cmp(&f->uuids128[i].u, &SENSOR_SVC_UUID.u) == 0)
        {
            return true;
        }
    }

    size_t plen = strlen(CONFIG_GW_BLE_SENSOR_NAME_PREFIX);

    return plen > 0 && f->name != NULL && f->name_len >= plen &&
           memcmp(f->name, CONFIG_GW_BLE_SENSOR_NAME_PREFIX, plen) == 0;
}

static void start_scan(void)
{
    if(!active || !synced || conn != BLE_HS_CONN_HANDLE_NONE || ble_gap_disc_active())
    {
        return;
    }

    /* Active scan so names/UUIDs carried in scan responses are seen too. */
    struct ble_gap_disc_params p = {
        .passive = 0,
        .filter_duplicates = 1,
    };

    int rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &p, gap_event, NULL);

    if(rc == 0)
    {
        ESP_LOGI(TAG, "scanning for a Guder sensor");
        set_status(true, false, false);
    }
    else
    {
        ESP_LOGE(TAG, "scan start failed: %d", rc);
        rescan_later();
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch(event->type)
    {
        case BLE_GAP_EVENT_DISC:
        {
            struct ble_hs_adv_fields f;

            if(ble_hs_adv_parse_fields(&f, event->disc.data, event->disc.length_data) != 0 ||
               !is_sensor(&f))
            {
                return 0;
            }

            xSemaphoreTake(status_lock, portMAX_DELAY);
            size_t n = f.name_len < sizeof(status.name) - 1 ? f.name_len : sizeof(status.name) - 1;
            if(f.name != NULL)
            {
                memcpy(status.name, f.name, n);
                status.name[n] = '\0';
            }
            xSemaphoreGive(status_lock);

            ble_gap_disc_cancel();

            int rc = ble_gap_connect(own_addr_type, &event->disc.addr, CONNECT_TIMEOUT_MS,
                                     NULL, gap_event, NULL);

            if(rc != 0)
            {
                ESP_LOGW(TAG, "connect failed to start: %d", rc);
                rescan_later();
            }

            return 0;
        }

        case BLE_GAP_EVENT_CONNECT:
            if(event->connect.status != 0)
            {
                ESP_LOGW(TAG, "connection failed: %d", event->connect.status);
                rescan_later();
                return 0;
            }

            conn = event->connect.conn_handle;
            svc_start = svc_end = frames_handle = cccd_handle = 0;
            memset(&rx, 0, sizeof(rx));
            set_status(false, true, false);
            ESP_LOGI(TAG, "connected to sensor");

            if(!active)
            {
                drop_connection("central switched off");
                return 0;
            }

            ble_gattc_exchange_mtu(conn, on_mtu, NULL);

            if(ble_gattc_disc_svc_by_uuid(conn, &SENSOR_SVC_UUID.u, on_svc, NULL) != 0)
            {
                drop_connection("service discovery failed");
            }

            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "sensor disconnected (reason 0x%x)", event->disconnect.reason);
            conn = BLE_HS_CONN_HANDLE_NONE;
            set_status(false, false, false);
            rescan_later();
            return 0;

        case BLE_GAP_EVENT_NOTIFY_RX:
            if(event->notify_rx.conn_handle == conn && event->notify_rx.attr_handle == frames_handle)
            {
                handle_notification(event->notify_rx.om);   /* the stack frees om */
            }
            return 0;

        case BLE_GAP_EVENT_DISC_COMPLETE:
            set_status(false, conn != BLE_HS_CONN_HANDLE_NONE, false);
            if(conn == BLE_HS_CONN_HANDLE_NONE)
            {
                rescan_later();
            }
            return 0;

        default:
            return 0;
    }
}

/* ------------------------------------------------------------------ */

static void ensure_init(void)
{
    if(status_lock == NULL)
    {
        status_lock = xSemaphoreCreateMutex();
    }

    if(rescan_timer == NULL)
    {
        const esp_timer_create_args_t a = { .callback = rescan_cb, .name = "ble_rescan" };
        esp_timer_create(&a, &rescan_timer);
    }
}

void ble_central_on_sync(uint8_t addr_type)
{
    ensure_init();
    own_addr_type = addr_type;
    synced = true;
    start_scan();
}

void ble_central_on_reset(void)
{
    synced = false;
    conn = BLE_HS_CONN_HANDLE_NONE;

    if(status_lock != NULL)
    {
        set_status(false, false, false);
    }
}

void ble_central_set_active(bool on)
{
    ensure_init();
    active = on;
    set_status(false, conn != BLE_HS_CONN_HANDLE_NONE, false);

    if(!synced)
    {
        return;
    }

    if(on)
    {
        start_scan();
        return;
    }

    esp_timer_stop(rescan_timer);

    if(ble_gap_disc_active())
    {
        ble_gap_disc_cancel();
    }

    drop_connection("central switched off");
}

void ble_central_status(ble_central_status_t *out)
{
    ensure_init();
    xSemaphoreTake(status_lock, portMAX_DELAY);
    *out = status;
    out->active = active;
    xSemaphoreGive(status_lock);
}
