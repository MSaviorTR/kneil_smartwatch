#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "host/ble_hs.h"
#include "host/util/util.h"

#include "kneil_central.h"
#include "kneil_ctrl_queue.h"
#include "kneil_proto.h"
#include "kneil_state.h"

static const char *TAG = "kneil-central";

/* The KNEIL UUIDs (a8f5000X-3e2c-4c6e-9b1a-6b4e45494c00) come from main/kneil_state.c, where the host test
   checks them against kneil_firmware/protocol/README.md. NimBLE wants a struct, so they are built once from
   that byte array; no UUID byte is written by hand here any more. */
static ble_uuid128_t SVC_UUID;
static ble_uuid128_t LIVE_UUID;
static ble_uuid128_t CONTROL_UUID;
static ble_uuid128_t INFO_UUID;
static ble_uuid128_t SUMMARY_UUID;
static const ble_uuid16_t CCCD_UUID = BLE_UUID16_INIT(0x2902);

#define CONNECT_TIMEOUT_MS     10000
#define RESCAN_DELAY_MS        2000
#define SCAN_WINDOW_MS         3000    /* collect candidates, then connect to the strongest KNEIL device */
#define MAX_CANDIDATES         8
#define KNEIL_NAME_PREFIX      "KNEIL-"

static void uuid_init(ble_uuid128_t *uuid, uint16_t id)
{
    uint8_t bytes[16];

    kneil_uuid_le(bytes, id);
    ble_uuid_init_from_buf((ble_uuid_any_t *)uuid, bytes, sizeof(bytes));
}

static void init_uuids(void)
{
    static bool ready;

    if(ready)
    {
        return;
    }

    uuid_init(&SVC_UUID, KNEIL_UUID_SERVICE);
    uuid_init(&LIVE_UUID, KNEIL_UUID_LIVE);
    uuid_init(&CONTROL_UUID, KNEIL_UUID_CONTROL);
    uuid_init(&INFO_UUID, KNEIL_UUID_INFO);
    uuid_init(&SUMMARY_UUID, KNEIL_UUID_SUMMARY);
    ready = true;
}

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

static struct candidate cand[MAX_CANDIDATES];
static int n_cand;
static char dev_name[KNEIL_NAME_MAX];
static int  dev_rssi;

static esp_timer_handle_t scan_timer;       /* ends the scan window and connects */
static esp_timer_handle_t rescan_timer;

static SemaphoreHandle_t lock;
static kneil_model_t     model;

/* CONTROL writes: the UI task queues, the NimBLE task writes one at a time. All fields below are
   protected by `lock`; the pure queue logic lives in kneil_ctrl_queue.c and is host-tested. */
static kneil_ctrl_queue_t ctrl_queue;
static uint32_t           ctrl_gen;      /* bumped when a connection is lost: stale completions are ignored */

static int  gap_event(struct ble_gap_event *event, void *arg);
static void start_scan(void);

/* ------------------------------------------------------------------ model */

static void set_link(kneil_link_t link, const char *name, int rssi, uint16_t mtu)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    kneil_model_set_link(&model, link, name, rssi, mtu);
    xSemaphoreGive(lock);
}

/* Same, with the device the scan chose; dev_name/dev_rssi are written under the lock (scan window) and
   read here under the lock too. */
static void set_link_dev(kneil_link_t link, uint16_t mtu)
{
    char name[KNEIL_NAME_MAX];
    int  rssi;

    xSemaphoreTake(lock, portMAX_DELAY);
    memcpy(name, dev_name, sizeof(name));
    rssi = dev_rssi;
    kneil_model_set_link(&model, link, name, rssi, mtu);
    xSemaphoreGive(lock);
}

static bool conn_is(uint16_t c)
{
    bool same;

    xSemaphoreTake(lock, portMAX_DELAY);
    same = (conn == c);
    xSemaphoreGive(lock);
    return same;
}

static bool conn_is_none(void)
{
    return conn_is(BLE_HS_CONN_HANDLE_NONE);
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
    uint16_t c;

    xSemaphoreTake(lock, portMAX_DELAY);
    c = conn;
    xSemaphoreGive(lock);

    if(c != BLE_HS_CONN_HANDLE_NONE)
    {
        ESP_LOGW(TAG, "disconnecting: %s", why);
        ble_gap_terminate(c, BLE_ERR_REM_USER_CONN_TERM);
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

/* The characteristic table of this connect: definition and value handles in discovery order, so each
   characteristic's descriptors can be searched in its own range (see kneil_chr_dsc_range()). */
#define MAX_CHRS 12
static uint16_t chr_def[MAX_CHRS];
static uint16_t chr_val[MAX_CHRS];
static int      n_chrs;
static int      idx_live = -1;
static int      idx_info = -1;
static int      idx_summary = -1;

/* Descriptors of the characteristic being subscribed (one short discovery). */
#define MAX_DSCS 12
static uint16_t dsc_handle[MAX_DSCS];
static uint8_t  dsc_is_cccd[MAX_DSCS];
static int      n_dscs;

/* The subscription in progress: name for the log, value handle, its range and the CCCD found. */
static const char *sub_name;
static uint16_t    sub_value;
static uint16_t    sub_next_def;
static uint16_t    sub_start;        /* argument for ble_gattc_disc_all_dscs(): the value handle */
static uint16_t    sub_first, sub_last;
static uint16_t    sub_cccd;
static int         sub_phase;        /* 0 live, 1 info, 2 summary */

/* One line per subscription, after the CCCD write succeeded (the owner's log then shows three handles). */
static void log_subscribed(void)
{
    ESP_LOGI(TAG, "subscribed %s: value %u cccd %u", sub_name ? sub_name : "?",
             (unsigned)sub_value, (unsigned)sub_cccd);
}

static int on_sub_dsc(uint16_t c, const struct ble_gatt_error *err, uint16_t chr_val_handle,
                      const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)chr_val_handle; (void)arg;

    if(err->status == 0 && dsc != NULL)
    {
        if(n_dscs < MAX_DSCS)
        {
            dsc_handle[n_dscs]  = dsc->handle;
            dsc_is_cccd[n_dscs] = ble_uuid_cmp(&dsc->uuid.u, &CCCD_UUID.u) == 0;
            n_dscs++;
        }

        return 0;
    }

    if(err->status != BLE_HS_EDONE)
    {
        drop_connection("descriptor discovery failed");
        return 0;
    }

    /* its own range only, first CCCD: kneil_cccd_for() also filters by the range, so a device that
       ignores the discovery range cannot hand over a later characteristic's descriptor */
    const uint16_t cccd = kneil_cccd_for(sub_value, sub_next_def, svc_end,
                                         dsc_handle, dsc_is_cccd, n_dscs);

    if(cccd == 0)
    {
        if(sub_phase == 0)
        {
            /* LIVE is the reason for this connection: no CCCD means no stream (F5) */
            ESP_LOGW(TAG, "no LIVE CCCD (value %u)", (unsigned)sub_value);
            drop_connection("no LIVE CCCD");
            return 0;
        }

        /* no notify descriptor: ignore this optional characteristic and go on */
        ESP_LOGW(TAG, "no CCCD for %s (value %u)", sub_name ? sub_name : "?", (unsigned)sub_value);
        sub_phase++;
        subscribe_next();
        return 0;
    }

    sub_cccd = cccd;

    static const uint8_t enable[2] = { 0x01, 0x00 };

    if(ble_gattc_write_flat(c, cccd, enable, sizeof(enable), on_cccd_written, NULL) != 0)
    {
        drop_connection("subscribe write failed");
    }

    return 0;
}

static int on_cccd_written(uint16_t c, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    (void)c; (void)attr; (void)arg;

    if(err->status != 0)
    {
        ESP_LOGW(TAG, "CCCD write failed for %s (cccd %u): %d", sub_name ? sub_name : "?",
                 (unsigned)sub_cccd, err->status);
        drop_connection("subscribe failed");
        return 0;
    }

    log_subscribed();

    sub_phase++;
    subscribe_next();
    return 0;
}

static void subscribe_next(void)
{
    /* The mode may have been switched off while MTU exchange or a CCCD write was in flight (F7): do not
       continue subscribing and do not report `connected` on a connection that is being dropped. */
    if(!active || conn_is_none())
    {
        return;
    }

    /* live is required; info and summary only when the device has them */
    while(sub_phase < 3)
    {
        const int idx = (sub_phase == 0) ? idx_live : (sub_phase == 1) ? idx_info : idx_summary;

        if(idx < 0)
        {
            sub_phase++;         /* not offered by this device */
            continue;
        }

        sub_name  = (sub_phase == 0) ? "LIVE" : (sub_phase == 1) ? "INFO" : "SUMMARY";
        sub_value = chr_val[idx];
        sub_next_def = (idx + 1 < n_chrs) ? chr_def[idx + 1] : 0;

        /* start = the value handle (NimBLE searches value + 1 .. end itself), end = next definition - 1 */
        kneil_dsc_disc_args(sub_value, sub_next_def, svc_end, &sub_start, &sub_last);
        sub_first = (uint16_t)(sub_value + 1);
        n_dscs = 0;

        if(sub_last < sub_first)
        {
            /* the range would be empty: no descriptors of its own */
            if(sub_phase == 0)
            {
                ESP_LOGW(TAG, "no LIVE CCCD (value %u)", (unsigned)sub_value);
                drop_connection("no LIVE CCCD");
                return;
            }

            ESP_LOGW(TAG, "no CCCD for %s (value %u)", sub_name, (unsigned)sub_value);
            sub_phase++;
            continue;
        }

        if(ble_gattc_disc_all_dscs(conn, sub_start, sub_last, on_sub_dsc, NULL) != 0)
        {
            drop_connection("descriptor discovery failed");
        }

        return;
    }

    if(!active || conn_is_none())
    {
        return;         /* switched off while the last subscription completed */
    }

    const uint16_t mtu = (uint16_t)ble_att_mtu(conn);

    set_link_dev(KNEIL_LINK_CONNECTED, mtu);
    ESP_LOGI(TAG, "subscriptions done, MTU %u", (unsigned)mtu);
}

/* ------------------------------------------------------------------ notifications */

/* One INFO line per connection, on the first LIVE packet that decodes. */
static bool first_live_logged;

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
            if(!first_live_logged)
            {
                first_live_logged = true;
                ESP_LOGI(TAG, "first LIVE packet, v%u, class %s", (unsigned)p.version,
                         kneil_class_text(p.cls, p.version));
            }

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
    else
    {
        /* routing is by value handle: this is a notification from a characteristic we did not subscribe to */
        ESP_LOGD(TAG, "notification on unknown handle %u", (unsigned)attr);
    }
}

/* ------------------------------------------------------------------ discovery */

static int on_chrs(uint16_t c, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;

    if(err->status == 0 && chr != NULL)
    {
        const int slot = n_chrs;

        if(n_chrs < MAX_CHRS)
        {
            chr_def[n_chrs] = chr->def_handle;
            chr_val[n_chrs] = chr->val_handle;
            n_chrs++;
        }

        /* the index lets the subscription search only this characteristic's descriptor range */
        if(ble_uuid_cmp(&chr->uuid.u, &LIVE_UUID.u) == 0)          { h_live = chr->val_handle;    if(slot < MAX_CHRS) idx_live = slot; }
        else if(ble_uuid_cmp(&chr->uuid.u, &INFO_UUID.u) == 0)     { h_info = chr->val_handle;    if(slot < MAX_CHRS) idx_info = slot; }
        else if(ble_uuid_cmp(&chr->uuid.u, &SUMMARY_UUID.u) == 0)  { h_summary = chr->val_handle; if(slot < MAX_CHRS) idx_summary = slot; }
        else if(ble_uuid_cmp(&chr->uuid.u, &CONTROL_UUID.u) == 0)
        {
            /* read by the UI task through kneil_central_send_ctrl() */
            xSemaphoreTake(lock, portMAX_DELAY);
            h_ctrl = chr->val_handle;
            xSemaphoreGive(lock);
        }

        return 0;
    }

    if(err->status != BLE_HS_EDONE)
    {
        drop_connection("characteristic discovery failed");
        return 0;
    }

    ESP_LOGI(TAG, "%d characteristics: LIVE %u, INFO %u, SUMMARY %u, CONTROL %u", n_chrs,
             (unsigned)h_live, (unsigned)h_info, (unsigned)h_summary, (unsigned)h_ctrl);

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
    (void)arg;

    if(err->status != 0)
    {
        ESP_LOGW(TAG, "MTU exchange failed: %d (subscriptions continue at the default MTU)", err->status);
        return 0;
    }

    ESP_LOGI(TAG, "MTU %u", mtu);

    /* the mode may have been switched off while the exchange was in flight (F7) */
    if(active && conn_is(c))
    {
        set_link_dev(KNEIL_LINK_CONNECTED, mtu);
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

static void remember_name(char *dst, size_t size, const struct ble_hs_adv_fields *f)
{
    if(f->name == NULL)
    {
        return;
    }

    size_t n = f->name_len < size - 1 ? f->name_len : size - 1;

    memcpy(dst, f->name, n);
    dst[n] = '\0';
}

/* Called from the NimBLE task; cand[]/n_cand are read by scan_window_cb() on the esp_timer task. */
static void remember_locked(const struct ble_hs_adv_fields *f, const ble_addr_t *addr, int8_t rssi)
{
    for(int i = 0; i < n_cand; i++)
    {
        if(memcmp(&cand[i].addr, addr, sizeof(*addr)) == 0)
        {
            cand[i].rssi = rssi;

            /* the scan response can arrive first, without the name: fill it in from a later packet (F8) */
            if(cand[i].name[0] == '\0')
            {
                remember_name(cand[i].name, sizeof(cand[i].name), f);
            }

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
    remember_name(cand[n_cand].name, sizeof(cand[n_cand].name), f);
    n_cand++;
}

static void remember(const struct ble_hs_adv_fields *f, const ble_addr_t *addr, int8_t rssi)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    remember_locked(f, addr, rssi);
    xSemaphoreGive(lock);
}

/* Scan window over: connect to the strongest KNEIL device seen. Runs on the esp_timer task and may race
   with a mode change and with gap_event(), so it checks the lifecycle first and copies the candidate out
   under the lock before connecting (F4). */
static void scan_window_cb(void *arg)
{
    (void)arg;

    if(!active || !synced || !conn_is_none())
    {
        return;
    }

    if(ble_gap_disc_active())
    {
        ble_gap_disc_cancel();
    }

    struct candidate chosen;
    bool have_candidate = false;

    xSemaphoreTake(lock, portMAX_DELAY);

    if(n_cand > 0)
    {
        int best = 0;

        for(int i = 1; i < n_cand; i++)
        {
            if(cand[i].rssi > cand[best].rssi)
            {
                best = i;
            }
        }

        chosen = cand[best];
        memcpy(dev_name, chosen.name, sizeof(dev_name));
        dev_rssi = chosen.rssi;
        have_candidate = true;
    }

    xSemaphoreGive(lock);

    if(!have_candidate)
    {
        rescan_later();
        return;
    }

    /* the mode may have been switched off between the check above and the scan cancel */
    if(!active || !synced)
    {
        return;
    }

    set_link(KNEIL_LINK_CONNECTING, chosen.name, chosen.rssi, 0);
    ESP_LOGI(TAG, "connecting to %s (%d dBm)", chosen.name[0] ? chosen.name : "KNEIL", chosen.rssi);

    /* the connect call has to run outside the timer callback's stack constraints: NimBLE queues it */
    if(ble_gap_connect(own_addr_type, &chosen.addr, CONNECT_TIMEOUT_MS, NULL, gap_event, NULL) != 0)
    {
        ESP_LOGW(TAG, "connect failed to start");
        rescan_later();
    }
}

static void start_scan(void)
{
    if(!active || !synced || !conn_is_none() || ble_gap_disc_active())
    {
        return;
    }

    xSemaphoreTake(lock, portMAX_DELAY);
    n_cand = 0;
    xSemaphoreGive(lock);

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
        {
            if(event->connect.status != 0)
            {
                ESP_LOGW(TAG, "connection failed: %d", event->connect.status);
                rescan_later();
                return 0;
            }

            xSemaphoreTake(lock, portMAX_DELAY);
            conn = event->connect.conn_handle;
            h_live = h_info = h_summary = h_ctrl = 0;
            kneil_ctrl_queue_reset(&ctrl_queue);   /* a new connection never inherits queued CONTROL bytes */
            xSemaphoreGive(lock);

            svc_start = svc_end = 0;
            n_chrs = 0;
            idx_live = idx_info = idx_summary = -1;
            first_live_logged = false;

            if(!active)
            {
                /* the mode was switched off while connecting: never show `connected` (F7) */
                set_link_dev(KNEIL_LINK_OFF, 0);
                drop_connection("KNEIL mode switched off");
                return 0;
            }

            set_link_dev(KNEIL_LINK_CONNECTED, 0);

            int rc = ble_gattc_exchange_mtu(conn, on_mtu, NULL);

            if(rc != 0)
            {
                ESP_LOGW(TAG, "MTU exchange failed to start: %d", rc);
            }

            if(ble_gattc_disc_svc_by_uuid(conn, &SVC_UUID.u, on_svc, NULL) != 0)
            {
                drop_connection("service discovery failed");
            }

            return 0;
        }

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "KNEIL device disconnected (reason 0x%x)", event->disconnect.reason);

            xSemaphoreTake(lock, portMAX_DELAY);
            conn = BLE_HS_CONN_HANDLE_NONE;
            h_live = h_info = h_summary = h_ctrl = 0;
            kneil_ctrl_queue_reset(&ctrl_queue);   /* no queued CONTROL byte may reach the next connection */
            ctrl_gen++;                            /* a completion still in flight is stale now */
            xSemaphoreGive(lock);

            n_chrs = 0;
            idx_live = idx_info = idx_summary = -1;
            set_link_dev(active ? KNEIL_LINK_LOST : KNEIL_LINK_OFF, 0);
            rescan_later();
            return 0;

        case BLE_GAP_EVENT_NOTIFY_RX:
            if(event->notify_rx.conn_handle == conn)
            {
                handle_notify(event->notify_rx.attr_handle, event->notify_rx.om);
            }

            return 0;

        case BLE_GAP_EVENT_DISC_COMPLETE:
            if(conn_is_none())
            {
                rescan_later();
            }

            return 0;

        default:
            return 0;
    }
}

/* ------------------------------------------------------------------ control writes */

static void ctrl_kick(void);

static int on_ctrl_written(uint16_t c, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    (void)c; (void)attr;

    const uint32_t gen = (uint32_t)(uintptr_t)arg;
    bool current;

    xSemaphoreTake(lock, portMAX_DELAY);
    current = (gen == ctrl_gen);
    if(current)
    {
        kneil_ctrl_queue_done(&ctrl_queue);
    }
    xSemaphoreGive(lock);

    if(!current)
    {
        return 0;      /* completion of a write from a connection that is already gone */
    }

    if(err->status != 0)
    {
        ESP_LOGW(TAG, "CONTROL write failed: %d", err->status);
    }

    ctrl_kick();
    return 0;
}

static void ctrl_kick(void)
{
    uint8_t  byte;
    uint16_t c, h;
    uint32_t gen;

    xSemaphoreTake(lock, portMAX_DELAY);

    if(conn == BLE_HS_CONN_HANDLE_NONE || h_ctrl == 0 || !kneil_ctrl_queue_take(&ctrl_queue, &byte))
    {
        xSemaphoreGive(lock);
        return;
    }

    /* busy is already set (take() does it): set before the write, or a fast completion would leave it
       stuck true and every CONTROL button dead until reboot (F2) */
    c   = conn;
    h   = h_ctrl;
    gen = ++ctrl_gen;

    xSemaphoreGive(lock);

    if(ble_gattc_write_flat(c, h, &byte, 1, on_ctrl_written, (void *)(uintptr_t)gen) != 0)
    {
        ESP_LOGW(TAG, "CONTROL write failed to start");

        xSemaphoreTake(lock, portMAX_DELAY);
        kneil_ctrl_queue_done(&ctrl_queue);
        kneil_ctrl_queue_requeue(&ctrl_queue, byte);   /* keep the byte for the next attempt */
        xSemaphoreGive(lock);
    }
}

bool kneil_central_send_ctrl(uint8_t ctrl)
{
    bool accepted;

    xSemaphoreTake(lock, portMAX_DELAY);
    accepted = (conn != BLE_HS_CONN_HANDLE_NONE && h_ctrl != 0) && kneil_ctrl_queue_push(&ctrl_queue, ctrl);
    xSemaphoreGive(lock);

    if(accepted)
    {
        ctrl_kick();
    }

    return accepted;
}

/* ------------------------------------------------------------------ public */

static void ensure_init(void)
{
    init_uuids();

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

    if(lock == NULL)
    {
        conn = BLE_HS_CONN_HANDLE_NONE;
        return;
    }

    xSemaphoreTake(lock, portMAX_DELAY);
    conn = BLE_HS_CONN_HANDLE_NONE;
    h_live = h_info = h_summary = h_ctrl = 0;
    kneil_ctrl_queue_reset(&ctrl_queue);   /* the host reset drops the connection and every CONTROL byte */
    ctrl_gen++;
    xSemaphoreGive(lock);

    n_chrs = 0;
    idx_live = idx_info = idx_summary = -1;

    set_link_dev(active ? KNEIL_LINK_LOST : KNEIL_LINK_OFF, 0);
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

        /* a 10 s connect in progress would otherwise keep the controller busy for the next mode (F4) */
        if(ble_gap_conn_active())
        {
            int rc = ble_gap_conn_cancel();

            if(rc != 0)
            {
                ESP_LOGW(TAG, "connect cancel failed: %d", rc);
            }
        }

        drop_connection("KNEIL mode switched off");

        xSemaphoreTake(lock, portMAX_DELAY);
        kneil_ctrl_queue_reset(&ctrl_queue);
        ctrl_gen++;
        xSemaphoreGive(lock);

        set_link_dev(KNEIL_LINK_OFF, 0);
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
