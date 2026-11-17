/*
 * Host tests for the KNEIL monitor: the protocol decode and the page model.
 * Uses the same example packets as the KNEIL firmware's protocol unit test
 * (kneil_firmware/protocol/test_kneil_proto.c) so all three implementations agree.
 * No ESP-IDF, no BLE: only components/kneil_proto and main/kneil_state.c.
 */
#include <stdio.h>
#include <string.h>

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

int main(void)
{
    test_decode_live_v2();
    test_decode_live_v1();
    test_decode_summary();
    test_model_sequence();
    test_names_and_colours();
    test_page_rebuild_logic();

    printf("kneil tests: %d checks, %d failed\n", checks, fails);
    printf("KNEIL TEST: %s\n", fails ? "FAILED" : "PASS");
    return fails ? 1 : 0;
}
