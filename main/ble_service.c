#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "esp_log.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "app_state.h"
#include "ble_central.h"
#include "ble_roles.h"
#include "ble_service.h"
#include "ble_uuids.h"
#include "kneil_central.h"
#include "rtc_service.h"

static const char *TAG = "ble";


static const ble_uuid128_t SVC_UUID  = GW_UUID(0x01);
static const ble_uuid128_t LIVE_UUID = GW_UUID(0x02);
static const ble_uuid128_t TIME_UUID = GW_UUID(0x03);

#define BATTERY_SVC_UUID   0x180F
#define BATTERY_LEVEL_UUID 0x2A19

static uint16_t live_handle;
static uint16_t battery_handle;

static volatile bool     enabled = true;
static volatile uint8_t  mode = BLE_MODE_BOTH;
static volatile bool     synced;
static volatile uint16_t conn_handle = BLE_HS_CONN_HANDLE_NONE;
static volatile bool     live_subscribed;
static volatile bool     battery_subscribed;
static volatile int      battery_pct = -1;
static uint8_t           own_addr_type;

static void start_advertising(void);

/* Role switches. Peripheral = phone connects to us (data out). Central =
   we connect to the wireless sensor (data in). Both may run together. */
static bool peripheral_on(void) { return enabled && mode != BLE_MODE_SENSOR && mode != BLE_MODE_KNEIL; }
static bool central_on(void)    { return enabled && (mode == BLE_MODE_SENSOR || mode == BLE_MODE_BOTH); }
/* KNEIL mode: neither the phone peripheral nor the Guder-sensor central; the KNEIL central instead. */
static bool kneil_on(void)      { return enabled && mode == BLE_MODE_KNEIL; }

/* ------------------------------------------------------------------
   GATT
   ------------------------------------------------------------------ */

static int format_reading(char *buf, size_t len)
{
    reading_t r;

    if(!app_state_latest(&r) || app_state_active_source() == DATA_SRC_NONE)
    {
        return snprintf(buf, len, "0,none,,,,,,0,0");
    }

    /* NAN prints as "nan", which most CSV tools read as missing. */
    return snprintf(buf, len, "%lld,%s,%.2f,%.4f,%.2f,%.3f,%.2f,%lX,%d",
                    (long long)r.epoch_ms, app_state_source_name(r.source),
                    (double)r.k_mv, (double)r.k_mM, (double)r.ph_mv, (double)r.ph,
                    (double)r.temp_c, (unsigned long)r.flags, r.k_alert);
}

static int access_live(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;

    if(ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR)
    {
        return BLE_ATT_ERR_UNLIKELY;
    }

    char buf[128];
    int n = format_reading(buf, sizeof(buf));

    return os_mbuf_append(ctxt->om, buf, (uint16_t)n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int access_time(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;

    if(ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR)
    {
        uint32_t now = (uint32_t)time(NULL);
        uint8_t le[4] = { (uint8_t)now, (uint8_t)(now >> 8), (uint8_t)(now >> 16), (uint8_t)(now >> 24) };

        return os_mbuf_append(ctxt->om, le, sizeof(le)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if(ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR)
    {
        uint8_t le[4];
        uint16_t got = 0;

        if(OS_MBUF_PKTLEN(ctxt->om) != sizeof(le) ||
           ble_hs_mbuf_to_flat(ctxt->om, le, sizeof(le), &got) != 0 || got != sizeof(le))
        {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        uint32_t epoch = (uint32_t)le[0] | ((uint32_t)le[1] << 8) |
                         ((uint32_t)le[2] << 16) | ((uint32_t)le[3] << 24);

        /* Reject obviously wrong values (before 2025-01-01). */
        if(epoch < 1735689600u)
        {
            return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
        }

        rtc_service_set_epoch((time_t)epoch);
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static int access_battery(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;

    if(ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR)
    {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint8_t pct = (battery_pct < 0) ? 0 : (uint8_t)battery_pct;

    return os_mbuf_append(ctxt->om, &pct, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def gatt_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid       = &LIVE_UUID.u,
                .access_cb  = access_live,
                .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &live_handle,
            },
            {
                .uuid      = &TIME_UUID.u,
                .access_cb = access_time,
                .flags     = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
            },
            { 0 },
        },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(BATTERY_SVC_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid       = BLE_UUID16_DECLARE(BATTERY_LEVEL_UUID),
                .access_cb  = access_battery,
                .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &battery_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

/* ------------------------------------------------------------------
   GAP
   ------------------------------------------------------------------ */

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch(event->type)
    {
        case BLE_GAP_EVENT_CONNECT:
            if(event->connect.status == 0)
            {
                conn_handle = event->connect.conn_handle;
                ESP_LOGI(TAG, "phone connected");

                if(!peripheral_on())
                {
                    ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                }
            }
            else
            {
                start_advertising();
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "disconnected (reason 0x%x)", event->disconnect.reason);
            conn_handle = BLE_HS_CONN_HANDLE_NONE;
            live_subscribed = false;
            battery_subscribed = false;
            start_advertising();
            break;

        case BLE_GAP_EVENT_ADV_COMPLETE:
            start_advertising();
            break;

        case BLE_GAP_EVENT_SUBSCRIBE:
            if(event->subscribe.attr_handle == live_handle)
            {
                live_subscribed = event->subscribe.cur_notify;
            }
            else if(event->subscribe.attr_handle == battery_handle)
            {
                battery_subscribed = event->subscribe.cur_notify;
            }
            break;

        default:
            break;
    }

    return 0;
}

static void start_advertising(void)
{
    if(!peripheral_on() || !synced || conn_handle != BLE_HS_CONN_HANDLE_NONE || ble_gap_adv_active())
    {
        return;
    }

    /* Flags + name in the advert, the 128-bit UUID in the scan response:
       both together would not fit in 31 bytes. */
    struct ble_hs_adv_fields adv = {0};
    const char *name = ble_svc_gap_device_name();

    adv.flags            = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.name             = (const uint8_t *)name;
    adv.name_len         = (uint8_t)strlen(name);
    adv.name_is_complete = 1;

    struct ble_hs_adv_fields rsp = {0};
    rsp.uuids128             = &SVC_UUID;
    rsp.num_uuids128         = 1;
    rsp.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&adv);

    if(rc == 0)
    {
        rc = ble_gap_adv_rsp_set_fields(&rsp);
    }

    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };

    if(rc == 0)
    {
        rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    }

    if(rc != 0)
    {
        ESP_LOGE(TAG, "advertising failed: %d", rc);
    }
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &own_addr_type);
    synced = true;
    start_advertising();
    ble_central_on_sync(own_addr_type);
    kneil_central_on_sync(own_addr_type);
}

static void on_reset(int reason)
{
    synced = false;
    ble_central_on_reset();
    kneil_central_on_reset();
    ESP_LOGW(TAG, "host reset, reason %d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ------------------------------------------------------------------
   Notifications
   ------------------------------------------------------------------ */

static void notify(uint16_t handle, const void *data, uint16_t len)
{
    uint16_t conn = conn_handle;

    if(conn == BLE_HS_CONN_HANDLE_NONE)
    {
        return;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);

    if(om != NULL)
    {
        ble_gatts_notify_custom(conn, handle, om);   /* takes ownership of om */
    }
}

#define NOTIFY_MIN_INTERVAL_MS 1000

static void on_reading(const reading_t *r)
{
    /* At most one notification per second: a fast front end (10-100 Hz)
       would otherwise flood the link and the phone. The value sent is
       always the latest reading. */
    static int64_t last_ms = INT64_MIN / 2;

    if(!live_subscribed || r->uptime_ms - last_ms < NOTIFY_MIN_INTERVAL_MS)
    {
        return;
    }

    last_ms = r->uptime_ms;

    char buf[128];
    int n = format_reading(buf, sizeof(buf));

    notify(live_handle, buf, (uint16_t)n);
}

/* ------------------------------------------------------------------ */

esp_err_t ble_service_start(void)
{
    app_settings_t s;
    app_state_get_settings(&s);
    enabled = s.ble_enabled;
    mode    = s.ble_mode;

    esp_err_t err = nimble_port_init();

    if(err != ESP_OK)
    {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(gatt_services);

    if(rc == 0)
    {
        rc = ble_gatts_add_svcs(gatt_services);
    }

    if(rc == 0)
    {
        rc = ble_svc_gap_device_name_set(CONFIG_GW_BLE_DEVICE_NAME);
    }

    if(rc != 0)
    {
        ESP_LOGE(TAG, "GATT setup failed: %d", rc);
        return ESP_FAIL;
    }

    app_state_add_listener(on_reading);
    ble_central_set_active(central_on());     /* takes effect once synced */
    kneil_central_set_active(kneil_on());
    nimble_port_freertos_init(host_task);

    static const char *const MODE_TXT[BLE_MODE_COUNT] = { "phone", "sensor", "phone + sensor", "kneil device" };
    _Static_assert(sizeof(MODE_TXT) / sizeof(MODE_TXT[0]) == BLE_MODE_COUNT,
                   "MODE_TXT must have one name per ble_mode_t");
    ESP_LOGI(TAG, "started as \"%s\", mode %s%s", CONFIG_GW_BLE_DEVICE_NAME,
             MODE_TXT[mode % BLE_MODE_COUNT], enabled ? "" : " (Bluetooth off)");

    return ESP_OK;
}

/* Brings both roles in line with `enabled` and `mode`. */
static void apply_roles(void)
{
    ble_role_step_t steps[BLE_ROLES_MAX_STEPS];
    const int n = ble_roles_plan(central_on(), kneil_on(), steps);

    /* Inactive first, then active (F3): a central that is still scanning or connecting would keep the
       radio busy, and the new one's start_scan() would return on ble_gap_disc_active() and stay idle. */
    for(int i = 0; i < n; i++)
    {
        if(steps[i].role == BLE_ROLE_CENTRAL)
        {
            ble_central_set_active(steps[i].on);
        }
        else
        {
            kneil_central_set_active(steps[i].on);
        }
    }

    if(!synced)
    {
        return;
    }

    if(peripheral_on())
    {
        start_advertising();
        return;
    }

    if(ble_gap_adv_active())
    {
        ble_gap_adv_stop();
    }

    if(conn_handle != BLE_HS_CONN_HANDLE_NONE)
    {
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

void ble_service_set_enabled(bool on)
{
    enabled = on;
    apply_roles();
}

void ble_service_set_mode(uint8_t new_mode)
{
    mode = (new_mode < BLE_MODE_COUNT) ? new_mode : BLE_MODE_BOTH;
    apply_roles();
}

void ble_service_status(ble_status_t *out)
{
    ble_central_status_t c;
    ble_central_status(&c);

    out->enabled           = enabled;
    out->mode              = mode;
    out->phone_connected   = conn_handle != BLE_HS_CONN_HANDLE_NONE;
    out->advertising       = synced && ble_gap_adv_active();
    out->sensor_scanning   = c.scanning;
    out->sensor_connected  = c.connected;
    out->sensor_receiving  = c.subscribed;
    out->sensor_frames     = c.frames;
    memcpy(out->sensor_name, c.name, sizeof(out->sensor_name));
    out->sensor_name[sizeof(out->sensor_name) - 1] = '\0';
}

bool ble_service_connected(void)
{
    return conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

bool ble_service_advertising(void)
{
    return synced && ble_gap_adv_active();
}

void ble_service_update_battery(int percent)
{
    if(percent == battery_pct)
    {
        return;
    }

    battery_pct = percent;

    if(battery_subscribed && percent >= 0)
    {
        uint8_t pct = (uint8_t)percent;
        notify(battery_handle, &pct, 1);
    }
}
