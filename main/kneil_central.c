#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "host/ble_hs.h"
#include "host/util/util.h"

#include "kneil_central.h"
#include "kneil_proto.h"
#include "kneil_state.h"

static const char *TAG = "kneil-central";

/* KNEIL live service a8f5000X-3e2c-4c6e-9b1a-6b4e45494c00 (same UUIDs as the KNEIL firmware). */
#define KNEIL_UUID128(id) BLE_UUID128_INIT(0x00, 0x4c, 0x94, 0x45, 0x4e, 0x6b, 0x1a, 0x9b, 0x6e, 0x4c, 0x2c, 0x3e, \
                                           (uint8_t)((id) & 0xff), (uint8_t)(((id) >> 8) & 0xff), 0xf5, 0xa8)

static const ble_uuid128_t SVC_UUID     = KNEIL_UUID128(0x0001);
static const ble_uuid128_t LIVE_UUID    = KNEIL_UUID128(0x0002);
static const ble_uuid128_t CONTROL_UUID = KNEIL_UUID128(0x0004);
static const ble_uuid128_t INFO_UUID    = KNEIL_UUID128(0x0005);
static const ble_uuid128_t SUMMARY_UUID = KNEIL_UUID128(0x0006);
static const ble_uuid16_t  CCCD_UUID    = BLE_UUID16_DECLARE(0x2902);

#define CONNECT_TIMEOUT_MS     10000
#define RESCAN_DELAY_MS        2000
#define SCAN_WINDOW_MS         3000    /* collect candidates, then connect to the strongest KNEIL device */
#define MAX_CANDIDATES         8
#define KNEIL_NAME_PREFIX      "KNEIL-"

struct candidate
{
    ble_addr_t addr;
    int8_t     rssi;
    char       name[KNEIL_NAME_MAX];
};

static volatile bool active;
static volatile bool synced;
static volatile bool lines_on;
static uint8_t       own_addr_type;

static uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t svc_start, svc_end;
static uint16_t h_live, h_info, h_summary, h_ctrl;
static uint16_t cccd_live, cccd_info, cccd_summary;

static struct candidate cand[MAX_CANDIDATES];
static int n_cand;
static char dev_name[KNEIL_NAME_MAX];
static int  dev_rssi;

static esp_timer_handle_t scan_timer;       /* ends the scan window and connects */
static esp_timer_handle_t rescan_timer;

static SemaphoreHandle_t lock;
static kneil_model_t     model;

static int  gap_event(struct ble_gap_event *event, void *arg);
static void start_scan(void);

/* ------------------------------------------------------------------ model */

static void set_link(kneil_link_t link, const char *name, int rssi, uint16_t mtu)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    kneil_model_set_link(&model, link, name, rssi, mtu);
    xSemaphoreGive(lock);
}

static void on_live_decoded(const kneil_live_t *p)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    kneil_model_on_live(&model, p, esp_timer_get_time());
    xSemaphoreGive(lock);
}

static void on_summary_decoded(const kneil_summary_t *s)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    kneil_model_on_summary(&model, s);
    xSemaphoreGive(lock);
}

static void on_info_text(const char *text)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    kneil_model_on_info(&model, text);
    xSemaphoreGive(lock);
}

/* ------------------------------------------------------------------ text lines for the console */

static void emit_line(const char *line, int n)
{
    if(lines_on && n > 0)
    {
        fputs(line, stdout);
        fflush(stdout);
    }
}

/* ------------------------------------------------------------------ helpers */

static void drop_connection(const char *why)
{
    if(conn != BLE_HS_CONN_HANDLE_NONE)
    {
        ESP_LOGW(TAG, "disconnecting: %s", why);
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

static void rescan_cb(void *arg)
{
    (void)arg;
    start_scan();
}

static void rescan_later(void)
{
    if(active && rescan_timer != NULL)
    {
        esp_timer_stop(rescan_timer);
        esp_timer_start_once(rescan_timer, (uint64_t)RESCAN_DELAY_MS * 1000);
    }
}

/* ------------------------------------------------------------------ subscriptions */


static void subscribe_next(void);

static int on_cccd_written(uint16_t c, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg);

/* Descriptor discovery for one characteristic: find its CCCD, then write notify=1. */
static uint16_t sub_cccd;            /* CCCD of the characteristic being subscribed */
static int      sub_phase;           /* 0 live, 1 info, 2 summary */

static int on_sub_dsc(uint16_t c, const struct ble_gatt_error *err, uint16_t chr_val_handle,
                      const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)chr_val_handle; (void)arg;

    if(err->status == 0 && dsc != NULL)
    {
        if(ble_uuid_cmp(&dsc->uuid.u, &CCCD_UUID.u) == 0)
        {
            sub_cccd = dsc->handle;
        }

        return 0;
    }

    if(err->status != BLE_HS_EDONE)
    {
        drop_connection("descriptor discovery failed");
        return 0;
    }

    if(sub_cccd == 0)
    {
        /* no notify descriptor: ignore this optional characteristic and go on */
        ESP_LOGW(TAG, "characteristic without a CCCD, skipped");
        sub_phase++;
        subscribe_next();
        return 0;
    }

    static const uint8_t enable[2] = { 0x01, 0x00 };

    if(sub_phase == 0)      cccd_live = sub_cccd;
    else if(sub_phase == 1) cccd_info = sub_cccd;
    else                    cccd_summary = sub_cccd;

    if(ble_gattc_write_flat(c, sub_cccd, enable, sizeof(enable), on_cccd_written, NULL) != 0)
    {
        drop_connection("subscribe write failed");
    }

    sub_cccd = 0;
    return 0;
}

static int on_cccd_written(uint16_t c, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    (void)c; (void)attr; (void)arg;

    if(err->status != 0)
    {
        drop_connection("subscribe failed");
        return 0;
    }

    sub_phase++;
    subscribe_next();
    return 0;
}

static void subscribe_next(void)
{
    /* live is required; info and summary only when the device has them */
    while(sub_phase < 3)
    {
        uint16_t h = (sub_phase == 0) ? h_live : (sub_phase == 1) ? h_info : h_summary;

        if(h == 0)
        {
            sub_phase++;         /* not offered by this device */
            continue;
        }

        if(ble_gattc_disc_all_dscs(conn, h, svc_end, on_sub_dsc, NULL) != 0)
        {
            drop_connection("descriptor discovery failed");
        }

        return;
    }

    const uint16_t mtu = (uint16_t)ble_att_mtu(conn);

    set_link(KNEIL_LINK_CONNECTED, dev_name, dev_rssi, mtu);
    ESP_LOGI(TAG, "subscribed (live%s%s), MTU %u", h_info ? " + info" : "", h_summary ? " + summary" : "",
             (unsigned)mtu);
}

/* ------------------------------------------------------------------ notifications */

static void handle_notify(uint16_t attr, struct os_mbuf *om)
{
    uint8_t buf[256];
    uint16_t len = OS_MBUF_PKTLEN(om);

    if(len > sizeof(buf))
    {
        len = sizeof(buf);
    }

    if(ble_hs_mbuf_to_flat(om, buf, len, &len) != 0)
    {
        return;
    }

    if(attr == h_live)
    {
        kneil_live_t p;

        if(kneil_live_decode(buf, len, &p) == 0)
        {
            on_live_decoded(&p);

            char line[KNEIL_LINE_MAX];
            int n = kneil_live_to_line(&p, line, sizeof(line));
            emit_line(line, n);
        }
        else
        {
            ESP_LOGW(TAG, "bad LIVE packet (%u bytes)", (unsigned)len);
        }
    }
    else if(attr == h_summary)
    {
        kneil_summary_t s;

        if(kneil_summary_decode(buf, len, &s) == 0)
        {
            on_summary_decoded(&s);

            char line[KNEIL_LINE_MAX];
            int n = kneil_summary_to_line(&s, line, sizeof(line));
            emit_line(line, n);
        }
    }
    else if(attr == h_info)
    {
        char line[KNEIL_LINE_MAX];
        int n = kneil_info_to_line(buf, len, line, sizeof(line));

        if(n > 0)
        {
            /* the line is "I,<text>\n": store the text without the prefix and the line end */
            char text[KNEIL_INFO_TEXT_MAX + 1];
            size_t t = (n > 2) ? (size_t)n - 3 : 0;   /* skip "I," and "\n" */

            if(t > KNEIL_INFO_TEXT_MAX)
            {
                t = KNEIL_INFO_TEXT_MAX;
            }

            memcpy(text, line + 2, t);
            text[t] = '\0';
            on_info_text(text);
            emit_line(line, n);
        }
    }
}

/* ------------------------------------------------------------------ discovery */

static int on_chrs(uint16_t c, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;

    if(err->status == 0 && chr != NULL)
    {
        if(ble_uuid_cmp(&chr->uuid.u, &LIVE_UUID.u) == 0)         h_live = chr->val_handle;
        else if(ble_uuid_cmp(&chr->uuid.u, &INFO_UUID.u) == 0)    h_info = chr->val_handle;
        else if(ble_uuid_cmp(&chr->uuid.u, &SUMMARY_UUID.u) == 0) h_summary = chr->val_handle;
        else if(ble_uuid_cmp(&chr->uuid.u, &CONTROL_UUID.u) == 0) h_ctrl = chr->val_handle;

        return 0;
    }

    if(err->status != BLE_HS_EDONE)
    {
        drop_connection("characteristic discovery failed");
        return 0;
    }

    if(h_live == 0)
    {
        drop_connection("no LIVE characteristic");
        return 0;
    }

    sub_phase = 0;
    subscribe_next();
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
        drop_connection("KNEIL service not found");
        return 0;
    }

    if(ble_gattc_disc_all_chrs(c, svc_start, svc_end, on_chrs, NULL) != 0)
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
        set_link(KNEIL_LINK_CONNECTED, dev_name, dev_rssi, mtu);
    }

    return 0;
}

/* ------------------------------------------------------------------ GAP */

static bool is_kneil(const struct ble_hs_adv_fields *f)
{
    for(int i = 0; i < f->num_uuids128; i++)
    {
        if(ble_uuid_cmp(&f->uuids128[i].u, &SVC_UUID.u) == 0)
        {
            return true;
        }
    }

    size_t plen = strlen(KNEIL_NAME_PREFIX);

    return f->name != NULL && f->name_len >= plen && memcmp(f->name, KNEIL_NAME_PREFIX, plen) == 0;
}

static void remember(const struct ble_hs_adv_fields *f, const ble_addr_t *addr, int8_t rssi)
{
    for(int i = 0; i < n_cand; i++)
    {
        if(memcmp(&cand[i].addr, addr, sizeof(*addr)) == 0)
        {
            cand[i].rssi = rssi;
            return;
        }
    }

    if(n_cand >= MAX_CANDIDATES)
    {
        return;
    }

    cand[n_cand].addr = *addr;
    cand[n_cand].rssi = rssi;
    cand[n_cand].name[0] = '\0';

    if(f->name != NULL)
    {
        size_t n = f->name_len < sizeof(cand[n_cand].name) - 1 ? f->name_len : sizeof(cand[n_cand].name) - 1;
        memcpy(cand[n_cand].name, f->name, n);
        cand[n_cand].name[n] = '\0';
    }

    n_cand++;
}

/* Scan window over: connect to the strongest KNEIL device seen. */
static void scan_window_cb(void *arg)
{
    (void)arg;

    if(ble_gap_disc_active())
    {
        ble_gap_disc_cancel();
    }

    if(n_cand == 0)
    {
        rescan_later();
        return;
    }

    int best = 0;

    for(int i = 1; i < n_cand; i++)
    {
        if(cand[i].rssi > cand[best].rssi)
        {
            best = i;
        }
    }

    dev_rssi = cand[best].rssi;
    memcpy(dev_name, cand[best].name, sizeof(dev_name));
    set_link(KNEIL_LINK_CONNECTING, dev_name, dev_rssi, 0);
    ESP_LOGI(TAG, "connecting to %s (%d dBm)", cand[best].name[0] ? cand[best].name : "KNEIL", cand[best].rssi);

    /* the connect call has to run outside the timer callback's stack constraints: NimBLE queues it */
    if(ble_gap_connect(own_addr_type, &cand[best].addr, CONNECT_TIMEOUT_MS, NULL, gap_event, NULL) != 0)
    {
        ESP_LOGW(TAG, "connect failed to start");
        rescan_later();
    }
}

static void start_scan(void)
{
    if(!active || !synced || conn != BLE_HS_CONN_HANDLE_NONE || ble_gap_disc_active())
    {
        return;
    }

    n_cand = 0;
    set_link(KNEIL_LINK_SCANNING, "", 0, 0);

    struct ble_gap_disc_params p = {
        .passive = 0,               /* active scan: the service UUID is in the scan response */
        .filter_duplicates = 1,
    };

    int rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &p, gap_event, NULL);

    if(rc == 0)
    {
        ESP_LOGI(TAG, "scanning for a KNEIL device");
        esp_timer_stop(scan_timer);
        esp_timer_start_once(scan_timer, (uint64_t)SCAN_WINDOW_MS * 1000);
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

            if(ble_hs_adv_parse_fields(&f, event->disc.data, event->disc.length_data) == 0 && is_kneil(&f))
            {
                remember(&f, &event->disc.addr, event->disc.rssi);
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
            svc_start = svc_end = 0;
            h_live = h_info = h_summary = h_ctrl = 0;
            cccd_live = cccd_info = cccd_summary = 0;
            set_link(KNEIL_LINK_CONNECTED, dev_name, dev_rssi, 0);

            if(!active)
            {
                drop_connection("KNEIL mode switched off");
                return 0;
            }

            ble_gattc_exchange_mtu(conn, on_mtu, NULL);

            if(ble_gattc_disc_svc_by_uuid(conn, &SVC_UUID.u, on_svc, NULL) != 0)
            {
                drop_connection("service discovery failed");
            }

            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "KNEIL device disconnected (reason 0x%x)", event->disconnect.reason);
            conn = BLE_HS_CONN_HANDLE_NONE;
            h_live = h_info = h_summary = h_ctrl = 0;
            set_link(active ? KNEIL_LINK_LOST : KNEIL_LINK_OFF, dev_name, dev_rssi, 0);
            rescan_later();
            return 0;

        case BLE_GAP_EVENT_NOTIFY_RX:
            if(event->notify_rx.conn_handle == conn)
            {
                handle_notify(event->notify_rx.attr_handle, event->notify_rx.om);
            }

            return 0;

        case BLE_GAP_EVENT_DISC_COMPLETE:
            if(conn == BLE_HS_CONN_HANDLE_NONE)
            {
                rescan_later();
            }

            return 0;

        default:
            return 0;
    }
}

/* ------------------------------------------------------------------ control writes */

static uint8_t  ctrl_q[4];
static int      ctrl_head, ctrl_count;
static bool     ctrl_busy;

static void ctrl_kick(void);

static int on_ctrl_written(uint16_t c, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    (void)c; (void)p;
    ctrl_busy = false;

    if(err->status != 0)
    {
        ESP_LOGW(TAG, "CONTROL write failed: %d", err->status);
    }

    ctrl_kick();
    return 0;
}

static void ctrl_kick(void)
{
    if(ctrl_busy || ctrl_count == 0 || conn == BLE_HS_CONN_HANDLE_NONE || h_ctrl == 0)
    {
        return;
    }

    static uint8_t byte;

    byte = ctrl_q[ctrl_head];
    ctrl_head = (ctrl_head + 1) % (int)sizeof(ctrl_q);
    ctrl_count--;

    if(ble_gattc_write_flat(conn, h_ctrl, &byte, 1, on_ctrl_written, NULL) == 0)
    {
        ctrl_busy = true;
    }
}

bool kneil_central_send_ctrl(uint8_t ctrl)
{
    if(conn == BLE_HS_CONN_HANDLE_NONE || h_ctrl == 0)
    {
        return false;
    }

    if(ctrl_count < (int)sizeof(ctrl_q))
    {
        ctrl_q[(ctrl_head + ctrl_count) % (int)sizeof(ctrl_q)] = ctrl;
        ctrl_count++;
    }

    ctrl_kick();
    return true;
}

/* ------------------------------------------------------------------ public */

static void ensure_init(void)
{
    if(lock == NULL)
    {
        lock = xSemaphoreCreateMutex();
        kneil_model_reset(&model);
    }

    if(scan_timer == NULL)
    {
        const esp_timer_create_args_t a = { .callback = scan_window_cb, .name = "kneil_scan" };
        esp_timer_create(&a, &scan_timer);
    }

    if(rescan_timer == NULL)
    {
        const esp_timer_create_args_t a = { .callback = rescan_cb, .name = "kneil_rescan" };
        esp_timer_create(&a, &rescan_timer);
    }
}

void kneil_central_on_sync(uint8_t addr_type)
{
    ensure_init();
    own_addr_type = addr_type;
    synced = true;
    start_scan();
}

void kneil_central_on_reset(void)
{
    synced = false;
    conn = BLE_HS_CONN_HANDLE_NONE;

    if(lock != NULL)
    {
        set_link(active ? KNEIL_LINK_LOST : KNEIL_LINK_OFF, dev_name, dev_rssi, 0);
    }
}

void kneil_central_set_active(bool on)
{
    ensure_init();
    active = on;

    xSemaphoreTake(lock, portMAX_DELAY);
    kneil_model_set_active(&model, on);
    xSemaphoreGive(lock);

    if(!on)
    {
        esp_timer_stop(scan_timer);
        esp_timer_stop(rescan_timer);

        if(ble_gap_disc_active())
        {
            ble_gap_disc_cancel();
        }

        drop_connection("KNEIL mode switched off");
        set_link(KNEIL_LINK_OFF, dev_name, dev_rssi, 0);
        return;
    }

    if(synced)
    {
        start_scan();
    }
}

void kneil_central_snapshot(kneil_model_t *out)
{
    ensure_init();
    xSemaphoreTake(lock, portMAX_DELAY);
    *out = model;
    out->active = active;
    xSemaphoreGive(lock);
}

void kneil_central_set_lines(bool on)
{
    lines_on = on;
}

bool kneil_central_lines(void)
{
    return lines_on;
}
