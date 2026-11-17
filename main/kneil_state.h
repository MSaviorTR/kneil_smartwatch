#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "kneil_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * KNEIL monitor model: everything the KNEIL page and the `kneil` console command show.
 *
 * Pure C on purpose (no ESP-IDF, no LVGL, no NimBLE): the protocol packets are decoded by
 * components/kneil_proto and pushed in here, and the host tests drive it directly. The BLE central
 * (main/kneil_central.c) owns one instance and serialises access to it.
 *
 * Nothing from the KNEIL device enters the K+/pH pipeline, the logger or the alerts.
 */

#define KNEIL_HISTORY     60
#define KNEIL_NAME_MAX    24

typedef enum
{
    KNEIL_LINK_OFF = 0,
    KNEIL_LINK_SCANNING,
    KNEIL_LINK_CONNECTING,
    KNEIL_LINK_CONNECTED,
    KNEIL_LINK_LOST,
} kneil_link_t;

typedef struct
{
    bool         active;          /* the KNEIL mode is selected */
    kneil_link_t link;
    char         name[KNEIL_NAME_MAX];   /* advertised name, "" if unknown */
    int          rssi;            /* dBm at the last sighting, 0 if unknown */
    uint16_t     mtu;             /* ATT MTU, 0 if not connected */
    uint32_t     packets;         /* LIVE packets decoded since the last reset */
    uint32_t     gaps;            /* missed LIVE sequence numbers since the last reset */

    /* last LIVE packet */
    uint8_t      version;         /* 1 or 2, 0 = no packet yet */
    int          cls;             /* class after the stairs rule (v2) / raw class (v1); KNEIL_CLASS_NONE */
    int          cls_raw;         /* raw class before the stairs rule (v2 only; = cls on v1) */
    int          cls_smooth;      /* majority-of-3 smoothed class */
    int          vote_pct;        /* vote share of the raw class, KNEIL_VOTE_NONE if unknown */
    int          posture;         /* KNEIL_POSTURE_* (v2; 0 = unknown) */
    int64_t      last_live_us;    /* esp_timer time of the last LIVE, 0 = never */

    /* event counters since connect, from the LIVE event flags (v2) */
    uint32_t     n_sit_to_stand;
    uint32_t     n_stand_to_sit;
    uint32_t     n_bed_transfer;

    /* last 60 classes, oldest first in `history` order: read with kneil_history_at() */
    uint8_t      history[KNEIL_HISTORY];
    int          hist_count;      /* 0..KNEIL_HISTORY */
    int          hist_pos;        /* next write position */

    bool         have_summary;
    kneil_summary_t summary;
    bool         have_info;
    char         info[KNEIL_INFO_TEXT_MAX + 1];

    /* sequence tracking (kept here so the model is self-contained) */
    uint16_t     last_seq;
    bool         have_seq;
} kneil_model_t;

void kneil_model_reset(kneil_model_t *m);

/* ---- writers (BLE central task) --------------------------------------- */
void kneil_model_set_active(kneil_model_t *m, bool active);
void kneil_model_set_link(kneil_model_t *m, kneil_link_t link, const char *name, int rssi, uint16_t mtu);
void kneil_model_on_live(kneil_model_t *m, const kneil_live_t *p, int64_t now_us);
void kneil_model_on_summary(kneil_model_t *m, const kneil_summary_t *s);
void kneil_model_on_info(kneil_model_t *m, const char *text);

/* ---- readers (UI, console) -------------------------------------------- */
/* Which page page 1 holds. The UI builds page 1 once (face or KNEIL page) and must rebuild it when the
   BLE mode enters or leaves KNEIL - otherwise the refresh would touch widgets that were never created.
   Pure integer logic so the host tests can cover it. */
bool kneil_page_needs_rebuild(bool built_kneil, int ble_mode, int kneil_mode);

/* Class at `i` back in time (0 = newest), KNEIL_CLASS_NONE where there is no packet. */
int  kneil_model_history_at(const kneil_model_t *m, int i);
/* Age of the last LIVE packet in seconds, -1 if none yet. */
int  kneil_model_live_age_s(const kneil_model_t *m, int64_t now_us);

/* ---- names and colours (the PC app's tables) -------------------------- */
int         kneil_version_classes(uint8_t version);
const char *kneil_link_text(kneil_link_t link);
const char *kneil_class_text(int cls, uint8_t version);
uint32_t    kneil_class_colour(int cls, uint8_t version);   /* 0xRRGGBB */
const char *kneil_posture_text(int posture);

/* ---- 128-bit UUIDs ---------------------------------------------------- */
/*
 * One concrete copy of the UUID base, in the byte order NimBLE takes (little-endian: least significant
 * byte first). The 16-bit part sits at bytes 12-13 and is filled in by kneil_uuid_le().
 * Text form (kneil_firmware/protocol/README.md): a8f5000X-3e2c-4c6e-9b1a-6b4e45494c00
 *   0x0001 service, 0x0002 LIVE, 0x0004 CONTROL, 0x0005 INFO, 0x0006 SUMMARY.
 * The host test formats all five and compares them with those strings, so a wrong byte cannot come back.
 */
extern const uint8_t KNEIL_UUID_BASE_LE[16];

/* 16 bytes of UUID a8f5000<id>-3e2c-4c6e-9b1a-6b4e45494c00, in NimBLE order. */
void kneil_uuid_le(uint8_t out[16], uint16_t id);
/* The same UUID as text "a8f5000x-3e2c-4c6e-9b1a-6b4e45494c00": out must hold 37 bytes. */
void kneil_uuid_text(char out[37], uint16_t id);

/* ---- descriptor (CCCD) ranges ---------------------------------------- */
/*
 * A characteristic's descriptors live between its value handle + 1 and the definition handle of the next
 * characteristic - 1, or the end of the service for the last characteristic. Searching to the service end
 * and keeping the last CCCD gave LIVE and INFO the SUMMARY descriptor (found on hardware, 17 Nov 2026).
 * `next_def` is 0 when there is no next characteristic. Pure, so the host test replays the layout from
 * the owner's log.
 */
void kneil_chr_dsc_range(uint16_t value, uint16_t next_def, uint16_t svc_end,
                         uint16_t *first, uint16_t *last);

/* First (lowest) CCCD inside [first, last]: the index into `handles`/`is_cccd` where is_cccd is non-zero,
   0 when the range holds none. */
uint16_t kneil_first_cccd(const uint16_t *handles, const uint8_t *is_cccd, int n,
                          uint16_t first, uint16_t last);

/* Arguments for NimBLE's ble_gattc_disc_all_dscs(): `start_arg` is the characteristic VALUE handle (NimBLE
   searches value + 1 .. end itself; host/ble_gatt.h "The handle of the characteristic value attribute"),
   `end_arg` the last handle of the characteristic definition. */
void kneil_dsc_disc_args(uint16_t value, uint16_t next_def, uint16_t svc_end,
                         uint16_t *start_arg, uint16_t *end_arg);

/* The CCCD of the characteristic with value handle `value`: its own range only, first CCCD, 0 if none. */
uint16_t kneil_cccd_for(uint16_t value, uint16_t next_def, uint16_t svc_end,
                        const uint16_t *handles, const uint8_t *is_cccd, int n);

#ifdef __cplusplus
}
#endif
