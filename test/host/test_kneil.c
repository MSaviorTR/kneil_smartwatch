/*
 * Host tests for the KNEIL monitor: the protocol decode, the page model, and the pure CONTROL-queue and
 * role-order logic.
 * Uses the same example packets as the KNEIL firmware's protocol unit test
 * (kneil_firmware/protocol/test_kneil_proto.c) so all three implementations agree.
 * No ESP-IDF, no BLE: only components/kneil_proto, main/kneil_state.c, main/kneil_ctrl_queue.c and
 * main/ble_roles.c.
 */
#include <stdio.h>
#include <string.h>

#include "ble_roles.h"
#include "kneil_ctrl_queue.h"
#include "kneil_proto.h"
#include "kneil_state.h"

static int checks;
static int fails;

#define CHECK(cond) do { \
        checks++; \
        if(!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } \
    } while(0)

/* LIVE v2: seq 513, t 123456 ms, walking (0) after the rule, still (3) smoothed, 85 % of the raw class
   (stairs_up = 1), flags 3, mean (-12, 34, 987) mg, no battery, events 0x12 = stand-to-sit + posture sitting */
static const uint8_t LIVE_V2[KNEIL_LIVE_V2_LEN] = {
    0x01, 0x02, 0x01, 0x02, 0x40, 0xE2, 0x01, 0x00, 0x00, 0x03, 0x55,
    0x03, 0xF4, 0xFF, 0x22, 0x00, 0xDB, 0x03, 0x00, 0x00, 0x01, 0x12,
};

/* the same window as a v1 packet (20 bytes, version 1, 9 classes) */
static const uint8_t LIVE_V1[KNEIL_LIVE_V1_LEN] = {
    0x01, 0x01, 0x01, 0x02, 0x40, 0xE2, 0x01, 0x00, 0x00, 0x03, 0x55,
    0x03, 0xF4, 0xFF, 0x22, 0x00, 0xDB, 0x03, 0x00, 0x00,
};

/* SUMMARY: format 2, 8 classes, minutes 1..8, sitting 9, standing 10, unknown 11, events 12/13/2,
   mean sit-to-stand 2.5 s, 100 steps, cadence 123.4, stairs cadence 110.0, seated tilt -4.5 deg */
static const uint8_t SUMMARY[KNEIL_SUMMARY_LEN] = {
    0x04, 0x02, 0x08, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x04, 0x00, 0x05, 0x00, 0x06,
    0x00, 0x07, 0x00, 0x08, 0x00, 0x09, 0x00, 0x0A, 0x00, 0x0B, 0x00, 0x0C, 0x00, 0x0D,
    0x00, 0x02, 0x00, 0x19, 0x00, 0x64, 0x00, 0xD2, 0x04, 0x4C, 0x04, 0xD3, 0xFF,
};


static void test_decode_live_v2(void)
{
    kneil_live_t p;
    char line[KNEIL_LINE_MAX];

    CHECK(kneil_live_decode(LIVE_V2, sizeof(LIVE_V2), &p) == 0);
    CHECK(p.version == 2);
    CHECK(p.seq == 513 && p.t_ms == 123456);
    CHECK(p.cls == 0 && p.cls_smooth == 3 && p.vote_pct == 85 && p.flags == 3);
    CHECK(p.mean_mg[0] == -12 && p.mean_mg[1] == 34 && p.mean_mg[2] == 987);
    CHECK(p.battery_mv == 0);
    CHECK(p.raw_cls == 1);
    CHECK(p.events == 0x12);
    CHECK(kneil_live_to_line(&p, line, sizeof(line)) > 0);
    CHECK(strcmp(line, "L,513,123456,0,3,85,3,-12,34,987,0,1,18\n") == 0);
}

static void test_decode_live_v1(void)
{
    kneil_live_t p;
    char line[KNEIL_LINE_MAX];

    CHECK(kneil_live_decode(LIVE_V1, sizeof(LIVE_V1), &p) == 0);
    CHECK(p.version == 1);
    CHECK(p.cls == 0 && p.cls_smooth == 3);
    CHECK(p.raw_cls == KNEIL_CLASS_NONE && p.events == 0);
    CHECK(kneil_live_to_line(&p, line, sizeof(line)) > 0);
    CHECK(strcmp(line, "L,513,123456,0,3,85,3,-12,34,987,0\n") == 0);
}

static void test_decode_summary(void)
{
    kneil_summary_t s;
    char line[KNEIL_LINE_MAX];

    CHECK(kneil_summary_decode(SUMMARY, sizeof(SUMMARY), &s) == 0);
    CHECK(s.format == 2 && s.n_classes == 8);
    CHECK(s.minutes_by_class[0] == 1 && s.minutes_by_class[7] == 8);
    CHECK(s.minutes_sitting == 9 && s.minutes_standing == 10 && s.minutes_still_unknown == 11);
    CHECK(s.n_sit_to_stand == 12 && s.n_stand_to_sit == 13 && s.n_bed_transfer == 2);
    CHECK(s.sts_seconds_x10 == 25 && s.steps == 100 && s.cadence_x10 == 1234);
    CHECK(s.stairs_cadence_x10 == 1100 && s.sit_tilt_x10 == -45);
    CHECK(kneil_summary_to_line(&s, line, sizeof(line)) > 0);
    CHECK(strcmp(line, "D,2,8,1,2,3,4,5,6,7,8,9,10,11,12,13,2,25,100,1234,1100,-45\n") == 0);
}

static void test_model_sequence(void)
{
    kneil_model_t m;
    kneil_live_t p;

    kneil_model_reset(&m);
    CHECK(m.cls == KNEIL_CLASS_NONE && m.version == 0 && m.link == KNEIL_LINK_OFF);
    CHECK(kneil_model_live_age_s(&m, 5000000) == -1);

    kneil_model_set_link(&m, KNEIL_LINK_CONNECTED, "KNEIL-7AD1", -45, 247);
    CHECK(strcmp(m.name, "KNEIL-7AD1") == 0 && m.rssi == -45 && m.mtu == 247);

    /* two v2 packets, the second one two sequence numbers later (one packet lost) */
    CHECK(kneil_live_decode(LIVE_V2, sizeof(LIVE_V2), &p) == 0);
    kneil_model_on_live(&m, &p, 10000000);
    p.seq = 516;
    kneil_model_on_live(&m, &p, 11000000);

    CHECK(m.packets == 2);
    CHECK(m.gaps == 2);
    CHECK(m.version == 2);
    CHECK(m.cls == 0 && m.cls_raw == 1 && m.cls_smooth == 3 && m.vote_pct == 85);
    CHECK(m.posture == KNEIL_POSTURE_SITTING);
    CHECK(m.n_stand_to_sit == 2 && m.n_sit_to_stand == 0 && m.n_bed_transfer == 0);
    CHECK(kneil_model_live_age_s(&m, 12000000) == 1);
    CHECK(m.hist_count == 2);
    CHECK(kneil_model_history_at(&m, 0) == 0 && kneil_model_history_at(&m, 1) == 0);
    CHECK(kneil_model_history_at(&m, 2) == KNEIL_CLASS_NONE);

    /* a v1 device: 9 classes, no posture, no event flags */
    CHECK(kneil_live_decode(LIVE_V1, sizeof(LIVE_V1), &p) == 0);
    p.cls = 3;
    p.cls_smooth = 3;
    kneil_model_on_live(&m, &p, 13000000);
    CHECK(m.version == 1);
    CHECK(m.posture == KNEIL_POSTURE_UNKNOWN);
    CHECK(m.cls_raw == 3);
    CHECK(m.n_stand_to_sit == 2);                 /* v1 has no event flags: unchanged */
    CHECK(kneil_model_history_at(&m, 0) == 3);

    /* summary and INFO */
    kneil_summary_t s;
    CHECK(kneil_summary_decode(SUMMARY, sizeof(SUMMARY), &s) == 0);
    kneil_model_on_summary(&m, &s);
    CHECK(m.have_summary && m.summary.minutes_sitting == 9 && m.summary.steps == 100);
    kneil_model_on_info(&m, "library 2.0; model V4half 4s");
    CHECK(m.have_info && strstr(m.info, "V4half") != NULL);

    /* restart: counters start again, the sequence tracking does not report a fake gap */
    kneil_model_reset(&m);
    CHECK(kneil_live_decode(LIVE_V2, sizeof(LIVE_V2), &p) == 0);
    p.seq = 0;
    kneil_model_on_live(&m, &p, 1000000);
    CHECK(m.packets == 1 && m.gaps == 0 && m.have_seq && m.last_seq == 0);
}

static void test_names_and_colours(void)
{
    CHECK(kneil_version_classes(1) == 9 && kneil_version_classes(2) == 8);
    CHECK(strcmp(kneil_class_text(3, 2), "still") == 0);
    CHECK(strcmp(kneil_class_text(3, 1), "sitting") == 0);
    CHECK(strcmp(kneil_class_text(4, 1), "standing") == 0);
    CHECK(strcmp(kneil_class_text(7, 2), "bed transfer") == 0);
    CHECK(strcmp(kneil_class_text(8, 2), "-") == 0);            /* v2 has 8 classes */
    CHECK(kneil_class_colour(3, 2) == 0x2166d6);
    CHECK(kneil_class_colour(2, 1) == 0xc65102);
    CHECK(kneil_class_colour(KNEIL_CLASS_NONE, 2) == 0x3a3d44);
    CHECK(strcmp(kneil_posture_text(0), "unknown") == 0);
    CHECK(strcmp(kneil_posture_text(2), "standing") == 0);
    CHECK(strcmp(kneil_link_text(KNEIL_LINK_CONNECTING), "connecting") == 0);
}

/* The mode can change at run time (Settings -> Mode cycles Phone -> Sensor -> Both -> KNEIL). The UI
   rebuilds page 1 only when the wanted page differs from the one built, so a refresh never touches
   widgets of the other page. */
static void test_page_rebuild_logic(void)
{
    enum { MODE_PHONE = 0, MODE_SENSOR = 1, MODE_BOTH = 2, MODE_KNEIL = 3 };   /* app_state.h ble_mode_t */

    CHECK(!kneil_page_needs_rebuild(false, MODE_PHONE, MODE_KNEIL));
    CHECK(!kneil_page_needs_rebuild(false, MODE_SENSOR, MODE_KNEIL));
    CHECK(!kneil_page_needs_rebuild(false, MODE_BOTH, MODE_KNEIL));
    CHECK(!kneil_page_needs_rebuild(true, MODE_KNEIL, MODE_KNEIL));
    CHECK(kneil_page_needs_rebuild(false, MODE_KNEIL, MODE_KNEIL));   /* entered KNEIL */
    CHECK(kneil_page_needs_rebuild(true, MODE_PHONE, MODE_KNEIL));    /* left KNEIL */
    CHECK(kneil_page_needs_rebuild(true, MODE_BOTH, MODE_KNEIL));

    /* a full cycle with the flag updated on each rebuild: one rebuild per mode change, correct page after */
    bool built = false;
    const int cycle[] = { MODE_PHONE, MODE_SENSOR, MODE_BOTH, MODE_KNEIL, MODE_KNEIL, MODE_PHONE, MODE_SENSOR };

    for(int i = 0; i < (int)(sizeof(cycle) / sizeof(cycle[0])); i++)
    {
        if(kneil_page_needs_rebuild(built, cycle[i], MODE_KNEIL))
        {
            built = (cycle[i] == MODE_KNEIL);
        }

        CHECK(built == (cycle[i] == MODE_KNEIL));
    }
}

/* The five KNEIL UUIDs the watch uses, formatted from KNEIL_UUID_BASE_LE. The expected text is copied from
   kneil_firmware/protocol/README.md, "A. Bluetooth LE (binary)" (the service, LIVE, RAW, CONTROL, INFO table)
   and the v2 notes above it, which add the SUMMARY characteristic a8f50006. */
static void test_uuids(void)
{
    static const struct
    {
        uint16_t    id;
        const char *text;
    } TABLE[] = {
        { 0x0001, "a8f50001-3e2c-4c6e-9b1a-6b4e45494c00" },   /* service */
        { 0x0002, "a8f50002-3e2c-4c6e-9b1a-6b4e45494c00" },   /* LIVE */
        { 0x0004, "a8f50004-3e2c-4c6e-9b1a-6b4e45494c00" },   /* CONTROL */
        { 0x0005, "a8f50005-3e2c-4c6e-9b1a-6b4e45494c00" },   /* INFO */
        { 0x0006, "a8f50006-3e2c-4c6e-9b1a-6b4e45494c00" },   /* SUMMARY */
    };

    for(int i = 0; i < (int)(sizeof(TABLE) / sizeof(TABLE[0])); i++)
    {
        char text[37];

        kneil_uuid_text(text, TABLE[i].id);

        if(strcmp(text, TABLE[i].text) != 0)
        {
            printf("FAIL uuid %04x: %s (protocol: %s)\n", TABLE[i].id, text, TABLE[i].text);
        }

        CHECK(strcmp(text, TABLE[i].text) == 0);
    }

    /* the concrete bytes NimBLE gets, for the service: the text bytes of ...6b4e45494c00 come first,
       least significant byte first, then the 16-bit part at 12-13 */
    static const uint8_t BASE[16] = {
        0x00, 0x4c, 0x49, 0x45, 0x4e, 0x6b, 0x1a, 0x9b,
        0x6e, 0x4c, 0x2c, 0x3e, 0x00, 0x00, 0xf5, 0xa8,
    };
    uint8_t b[16];

    CHECK(memcmp(KNEIL_UUID_BASE_LE, BASE, sizeof(BASE)) == 0);
    kneil_uuid_le(b, KNEIL_UUID_SERVICE);
    CHECK(memcmp(b, BASE, 12) == 0);
    CHECK(b[12] == 0x01 && b[13] == 0x00);
    CHECK(b[14] == 0xf5 && b[15] == 0xa8);
}

/* The descriptor layout from the owner's hardware log (service 19..33): LIVE value 21 CCCD 22, RAW value 24
   CCCD 25, CONTROL value 27 (no CCCD), INFO value 29 CCCD 30, SUMMARY value 32 CCCD 33. The old watch code
   searched from the value handle to the service end and kept the LAST CCCD, so every subscription wrote
   handle 33 (the SUMMARY CCCD) and LIVE/INFO were never enabled. */
static uint16_t old_rule_last_cccd(const uint16_t *handles, int n, uint16_t value, uint16_t svc_end)
{
    uint16_t last = 0;

    for(int i = 0; i < n; i++)
    {
        if(handles[i] > value && handles[i] <= svc_end)
        {
            last = handles[i];
        }
    }

    return last;
}

static void test_cccd_ranges(void)
{
    enum { SVC_END = 33 };
    static const uint16_t DSC[]     = { 22, 25, 30, 33 };
    static const uint8_t  IS_CCCD[] = { 1, 1, 1, 1 };
    uint16_t first, last;

    /* the bug, on the record: with the old rule all three subscriptions get one and the same handle */
    CHECK(old_rule_last_cccd(DSC, 4, 21, SVC_END) == 33);
    CHECK(old_rule_last_cccd(DSC, 4, 29, SVC_END) == 33);
    CHECK(old_rule_last_cccd(DSC, 4, 32, SVC_END) == 33);

    /* the fix: each characteristic's own range, first CCCD */
    CHECK(kneil_cccd_for(21, 23, SVC_END, DSC, IS_CCCD, 4) == 22);   /* LIVE */
    CHECK(kneil_cccd_for(24, 26, SVC_END, DSC, IS_CCCD, 4) == 25);   /* RAW */
    CHECK(kneil_cccd_for(27, 28, SVC_END, DSC, IS_CCCD, 4) == 0);    /* CONTROL: no CCCD */
    CHECK(kneil_cccd_for(29, 31, SVC_END, DSC, IS_CCCD, 4) == 30);   /* INFO */
    CHECK(kneil_cccd_for(32, 0, SVC_END, DSC, IS_CCCD, 4) == 33);    /* SUMMARY, last in the service */

    /* ranges: value + 1 .. next definition - 1, service end for the last characteristic */
    kneil_chr_dsc_range(21, 23, SVC_END, &first, &last);
    CHECK(first == 22 && last == 22);
    kneil_chr_dsc_range(27, 28, SVC_END, &first, &last);
    CHECK(first == 28 && last == 27);                               /* empty: nothing between 27 and 28 */
    kneil_chr_dsc_range(32, 0, SVC_END, &first, &last);
    CHECK(first == 33 && last == 33);

    /* the SUMMARY CCCD (33) is never taken for LIVE, whatever the callback order */
    CHECK(kneil_first_cccd(DSC, IS_CCCD, 4, 22, 22) == 22);
    CHECK(kneil_first_cccd(DSC, IS_CCCD, 4, 30, 30) == 30);
    CHECK(kneil_first_cccd(DSC, IS_CCCD, 4, 22, 29) == 22);          /* lowest handle in the range */

    /* a v1 device without SUMMARY: INFO is the last characteristic of the service */
    CHECK(kneil_cccd_for(29, 0, 30, DSC, IS_CCCD, 4) == 30);
}

/* ble_gattc_disc_all_dscs() takes the characteristic VALUE handle as its start handle and searches
   value + 1 .. end itself (host/ble_gatt.h, ble_gattc.c). Passing the descriptor start (value + 1, the W4
   code) made NimBLE send find-info 23..22 and return BLE_HS_EINVAL. */
static void test_dsc_disc_args(void)
{
    enum { SVC_END = 33 };
    uint16_t start, end;

    /* Layout: LIVE 20/21 CCCD 22, RAW 23/24 CCCD 25, CONTROL 26/27, INFO 28/29 CCCD 30, SUMMARY 31/32
       CCCD 33. The end argument is the last handle of the characteristic's own definition (its CCCD), so
       INFO is 30, not 31: 31 is the SUMMARY definition handle and belongs to the next characteristic
       (host/ble_gatt.h: "The last handle in the characteristic definition"). */
    kneil_dsc_disc_args(21, 23, SVC_END, &start, &end);
    CHECK(start == 21 && end == 22);

    kneil_dsc_disc_args(24, 26, SVC_END, &start, &end);
    CHECK(start == 24 && end == 25);          /* RAW */

    kneil_dsc_disc_args(29, 31, SVC_END, &start, &end);
    CHECK(start == 29 && end == 30);          /* INFO */

    kneil_dsc_disc_args(32, 0, SVC_END, &start, &end);
    CHECK(start == 32 && end == 33);          /* SUMMARY: last in the service */

    /* CONTROL: the next definition follows directly, so the range is empty and the call is skipped */
    kneil_dsc_disc_args(27, 28, SVC_END, &start, &end);
    CHECK(start == 27 && end == 27);
    CHECK(end < (uint16_t)(start + 1));

    /* the start handle must never be the descriptor start: that is what broke on hardware */
    kneil_dsc_disc_args(21, 23, SVC_END, &start, &end);
    CHECK(start != 22);
}

/* CONTROL bytes: four waiting, one in flight. A full queue must say so (the UI used to be told `true`
   and the byte was dropped), and a completion that arrives after the connection is gone must not
   release a write that belongs to the next connection (F2). */
static void test_ctrl_queue(void)
{
    kneil_ctrl_queue_t q;
    uint8_t b = 0;

    kneil_ctrl_queue_reset(&q);
    CHECK(kneil_ctrl_queue_count(&q) == 0);
    CHECK(!kneil_ctrl_queue_busy(&q));
    CHECK(!kneil_ctrl_queue_take(&q, &b));                 /* nothing queued */

    CHECK(kneil_ctrl_queue_push(&q, KNEIL_CTRL_SUMMARY));
    CHECK(kneil_ctrl_queue_push(&q, KNEIL_CTRL_SELFTEST));
    CHECK(kneil_ctrl_queue_push(&q, KNEIL_CTRL_RESET));
    CHECK(kneil_ctrl_queue_push(&q, 0x7f));
    CHECK(!kneil_ctrl_queue_push(&q, 0x55));               /* full: false and not queued */
    CHECK(kneil_ctrl_queue_count(&q) == KNEIL_CTRL_QUEUE_SIZE);

    CHECK(kneil_ctrl_queue_take(&q, &b) && b == KNEIL_CTRL_SUMMARY);   /* oldest first */
    CHECK(kneil_ctrl_queue_busy(&q));
    CHECK(!kneil_ctrl_queue_take(&q, &b));                 /* one write in flight */
    CHECK(kneil_ctrl_queue_count(&q) == KNEIL_CTRL_QUEUE_SIZE - 1);

    /* the write could not be started: the byte goes back to the front, then busy is cleared */
    kneil_ctrl_queue_requeue(&q, KNEIL_CTRL_SUMMARY);
    CHECK(kneil_ctrl_queue_count(&q) == KNEIL_CTRL_QUEUE_SIZE);
    kneil_ctrl_queue_done(&q);
    CHECK(!kneil_ctrl_queue_busy(&q));

    CHECK(kneil_ctrl_queue_take(&q, &b) && b == KNEIL_CTRL_SUMMARY);
    kneil_ctrl_queue_done(&q);
    CHECK(kneil_ctrl_queue_take(&q, &b) && b == KNEIL_CTRL_SELFTEST);
    kneil_ctrl_queue_done(&q);

    /* disconnect / host reset with a write in flight: empty and idle again */
    kneil_ctrl_queue_reset(&q);
    CHECK(kneil_ctrl_queue_count(&q) == 0);
    CHECK(!kneil_ctrl_queue_busy(&q));
    CHECK(!kneil_ctrl_queue_take(&q, &b));

    /* the ring wraps: four queued, one taken, one pushed, then all four drained in order */
    for(int i = 1; i <= 4; i++)
    {
        CHECK(kneil_ctrl_queue_push(&q, (uint8_t)i));
    }

    CHECK(kneil_ctrl_queue_take(&q, &b) && b == 1);
    kneil_ctrl_queue_done(&q);
    CHECK(kneil_ctrl_queue_push(&q, 5));
    CHECK(kneil_ctrl_queue_count(&q) == 4);

    for(int i = 2; i <= 5; i++)
    {
        CHECK(kneil_ctrl_queue_take(&q, &b) && b == (uint8_t)i);
        kneil_ctrl_queue_done(&q);
    }

    CHECK(!kneil_ctrl_queue_take(&q, &b));
}

/* F3: the central that becomes inactive is switched off before the new one is switched on. The shadow
   of the old apply_roles() (sensor central first, KNEIL second) left the sensor central idle after
   KNEIL -> Sensor only, because its start_scan() found KNEIL's discovery still active. */
static void test_role_plan(void)
{
    ble_role_step_t s[BLE_ROLES_MAX_STEPS];

    /* Sensor only (or Phone + sensor) leaving KNEIL: KNEIL off, then the sensor central on */
    CHECK(ble_roles_plan(true, false, s) == 2);
    CHECK(s[0].role == BLE_ROLE_KNEIL && !s[0].on);
    CHECK(s[1].role == BLE_ROLE_CENTRAL && s[1].on);

    /* entering KNEIL: the sensor central off first, then KNEIL on */
    CHECK(ble_roles_plan(false, true, s) == 2);
    CHECK(s[0].role == BLE_ROLE_CENTRAL && !s[0].on);
    CHECK(s[1].role == BLE_ROLE_KNEIL && s[1].on);

    /* Phone + sensor: both on (no off call in between) */
    CHECK(ble_roles_plan(true, true, s) == 2);
    CHECK(s[0].role == BLE_ROLE_CENTRAL && s[0].on);
    CHECK(s[1].role == BLE_ROLE_KNEIL && s[1].on);

    /* Bluetooth off: both off */
    CHECK(ble_roles_plan(false, false, s) == 2);
    CHECK(s[0].role == BLE_ROLE_CENTRAL && !s[0].on);
    CHECK(s[1].role == BLE_ROLE_KNEIL && !s[1].on);

    /* in every combination, no "on" step comes before an "off" step */
    for(int m = 0; m < 4; m++)
    {
        const bool central = (m & 1) != 0;
        const bool kneil   = (m & 2) != 0;
        const int  n = ble_roles_plan(central, kneil, s);
        bool seen_on = false;

        for(int i = 0; i < n; i++)
        {
            if(s[i].on)
            {
                seen_on = true;
            }
            else
            {
                CHECK(!seen_on);
            }
        }
    }
}

int main(void)
{
    test_decode_live_v2();
    test_decode_live_v1();
    test_decode_summary();
    test_model_sequence();
    test_names_and_colours();
    test_page_rebuild_logic();
    test_uuids();
    test_cccd_ranges();
    test_dsc_disc_args();
    test_ctrl_queue();
    test_role_plan();

    printf("kneil tests: %d checks, %d failed\n", checks, fails);
    printf("KNEIL TEST: %s\n", fails ? "FAILED" : "PASS");
    return fails ? 1 : 0;
}
