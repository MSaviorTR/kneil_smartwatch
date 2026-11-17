/*
 * KNEIL live protocol, version 2: binary BLE packets (section A) and USB text lines (section B).
 * Shared by the KNEIL firmware (src/kneil_live.c) and the XIAO bridge (bridge/), so both sides
 * produce exactly the same lines. The PC app parses these lines: do not change one side alone.
 *
 * All multi-byte values are little-endian. Acceleration in mg (int16), device frame
 * x anterior, y subject's left, z superior. t_ms = milliseconds since boot.
 * Plain C99, no Zephyr dependency (except KNEIL_UUID_ENCODE, which needs <zephyr/bluetooth/uuid.h>).
 *
 * Version history
 *  v1 (Nov 2026): 9 classes (2 s window), LIVE 20 bytes, no daily summary characteristic.
 *  v2 (17 Nov 2026, step E): 8 classes (4 s window), stairs rule, transition events. LIVE is 22 bytes:
 *     bytes 0-19 as v1 but byte 8 = class after the stairs rule and byte 10 = vote share of the RAW class;
 *     byte 20 = raw class (argmax, 255 = none); byte 21 = event flags (bit0 sit-to-stand counted in this
 *     window, bit1 stand-to-sit, bit2 bed transfer, bits 4-5 posture: 0 unknown, 1 sitting, 2 standing).
 *     New SUMMARY packet (type 4, 41 bytes) with the 40-byte daily summary (format 2).
 *     The decoder still accepts LIVE v1 packets, so old firmware keeps working.
 */
#ifndef KNEIL_PROTO_H
#define KNEIL_PROTO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KNEIL_PROTO_VERSION 2

/* BLE: 128-bit UUIDs a8f5000X-3e2c-4c6e-9b1a-6b4e45494c00 */
#define KNEIL_UUID_SERVICE 0x0001   /* KNEIL live service */
#define KNEIL_UUID_LIVE    0x0002   /* notify */
#define KNEIL_UUID_RAW     0x0003   /* notify */
#define KNEIL_UUID_CONTROL 0x0004   /* write */
#define KNEIL_UUID_INFO    0x0005   /* notify: text information (I lines); optional, added 16 Nov 2026 */
#define KNEIL_UUID_SUMMARY 0x0006   /* notify: 40-byte daily summary, format 2 (v2); optional */
#define KNEIL_UUID_ENCODE(id) BT_UUID_128_ENCODE(0xa8f50000U | (id), 0x3e2c, 0x4c6e, 0x9b1a, 0x6b4e45494c00ULL)

#define KNEIL_NAME_PREFIX "KNEIL-"  /* advertised name KNEIL-xxxx (last 4 hex digits of the address) */

/* Packet types and sizes */
#define KNEIL_PKT_LIVE     0x01
#define KNEIL_PKT_RAW      0x02
#define KNEIL_PKT_INFO     0x03
#define KNEIL_PKT_SUMMARY  0x04
/* INFO packet: uint8 type = 0x03, then 1..KNEIL_INFO_TEXT_MAX characters of printable ASCII text
 * (no line end; needs ATT MTU >= 104 for the full length). The bridge writes it as "I,<text>". */
#define KNEIL_INFO_TEXT_MAX 100
#define KNEIL_LIVE_V1_LEN  20
#define KNEIL_LIVE_V2_LEN  22
#define KNEIL_LIVE_LEN     KNEIL_LIVE_V2_LEN
#define KNEIL_RAW_HDR_LEN  8
#define KNEIL_RAW_N_NORMAL 25       /* 0.5 s at 50 Hz: 158-byte packet (needs ATT MTU >= 161) */
#define KNEIL_RAW_N_MAX    39       /* floor((247 - 3 - 8) / 6) */
#define KNEIL_RAW_N_FOR_MTU(mtu) (((mtu) - 3 - KNEIL_RAW_HDR_LEN) / 6)
/* SUMMARY packet: uint8 type = 0x04 + the 40-byte daily summary (needs ATT MTU >= 44) */
#define KNEIL_SUMMARY_LEN  41

/* CONTROL bytes (BLE) and command letters (USB) */
#define KNEIL_CTRL_RAW_START 0x10   /* 'S' */
#define KNEIL_CTRL_RAW_STOP  0x11   /* 'P' */
#define KNEIL_CTRL_SELFTEST  0x12   /* 'T' */
#define KNEIL_CTRL_SUMMARY   0x13   /* 'Q': send the daily summary now */
#define KNEIL_CTRL_RESET     0x14   /* 'Z': reset the daily metrics */

#define KNEIL_CLASS_NONE 255        /* no result yet */
#define KNEIL_VOTE_NONE  255        /* vote share not available */

/* LIVE flags */
#define KNEIL_FLAG_SELFTEST_OK 0x01
#define KNEIL_FLAG_LOWPASS     0x02
#define KNEIL_FLAG_SMALL_MODEL 0x04

/* LIVE v2 event flags (byte 21) */
#define KNEIL_EV_SIT_TO_STAND 0x01   /* a sit-to-stand event was counted in this window */
#define KNEIL_EV_STAND_TO_SIT 0x02
#define KNEIL_EV_BED_TRANSFER 0x04
#define KNEIL_EV_POSTURE_SHIFT 4     /* bits 4-5: posture after this window */
#define KNEIL_EV_POSTURE_MASK 0x30
#define KNEIL_POSTURE_UNKNOWN 0
#define KNEIL_POSTURE_SITTING 1
#define KNEIL_POSTURE_STANDING 2

/* Longest text line: R with KNEIL_RAW_N_MAX samples (about 850 characters) */
#define KNEIL_LINE_MAX 1024

typedef struct {
	uint8_t version;       /* 1 or 2 (set by the decoder; the encoder always writes 2) */
	uint16_t seq;          /* wraps */
	uint32_t t_ms;         /* end of the classified window */
	uint8_t cls;           /* v2: class after the stairs rule 0-7 (v1: raw class 0-8); KNEIL_CLASS_NONE = none */
	uint8_t cls_smooth;    /* after the majority-of-3 smoothing */
	uint8_t vote_pct;      /* v2: vote share of the raw class (%); v1: of the winning class */
	uint8_t flags;         /* KNEIL_FLAG_* */
	int16_t mean_mg[3];    /* mean x, y, z of the window */
	uint16_t battery_mv;   /* 0 = not measured */
	uint8_t raw_cls;       /* v2 only: argmax before the stairs rule; KNEIL_CLASS_NONE in v1 */
	uint8_t events;        /* v2 only: KNEIL_EV_* + posture bits; 0 in v1 */
} kneil_live_t;

typedef struct {
	uint16_t seq;          /* wraps */
	uint32_t t_ms_first;   /* time of the first sample */
	uint8_t n;             /* samples in this packet (1..KNEIL_RAW_N_MAX) */
	int16_t xyz_mg[KNEIL_RAW_N_MAX][3];
} kneil_raw_t;

/* Daily summary format 2 (same layout as kneil_daily_summary_t in kneil_tinyml.h, copied field by field) */
#define KNEIL_SUMMARY_FORMAT  2
#define KNEIL_SUMMARY_CLASSES 8

typedef struct __attribute__((packed)) {
	uint8_t  format;                 /* 2 */
	uint8_t  n_classes;              /* 8 */
	uint16_t minutes_by_class[KNEIL_SUMMARY_CLASSES];
	uint16_t minutes_sitting;
	uint16_t minutes_standing;
	uint16_t minutes_still_unknown;
	uint16_t n_sit_to_stand;
	uint16_t n_stand_to_sit;
	uint16_t n_bed_transfer;
	uint16_t sts_seconds_x10;
	uint16_t steps;
	uint16_t cadence_x10;
	uint16_t stairs_cadence_x10;
	int16_t  sit_tilt_x10;
} kneil_summary_t;

/* Binary packets (BLE). encode: returns the packet length, or -1 if buf is too small (or n is invalid).
 * decode: returns 0, or -1 if the packet is not a valid packet of that type (LIVE v1 and v2 are accepted). */
int kneil_live_encode(const kneil_live_t *p, uint8_t *buf, size_t size);
int kneil_live_decode(const uint8_t *buf, size_t len, kneil_live_t *p);
int kneil_raw_encode(const kneil_raw_t *p, uint8_t *buf, size_t size);
int kneil_raw_decode(const uint8_t *buf, size_t len, kneil_raw_t *p);
int kneil_summary_encode(const kneil_summary_t *s, uint8_t *buf, size_t size);
int kneil_summary_decode(const uint8_t *buf, size_t len, kneil_summary_t *s);

/* Text lines (USB): "L,..." and "R,..." ending with '\n' (and a terminating NUL).
 * Return the line length without the NUL, or -1 if out is too small. A LIVE v2 message writes two more
 * values at the end: ",<raw_class>,<event_flags>". */
int kneil_live_to_line(const kneil_live_t *p, char *out, size_t size);
int kneil_raw_to_line(const kneil_raw_t *p, char *out, size_t size);
/* "D,<format>,<n_classes>,<min_0..min_7>,<min_sitting>,<min_standing>,<min_still_unknown>,<n_sts>,<n_s2s>,<n_bed>,<sts_x10>,<steps>,<cadence_x10>,<stairs_cadence_x10>,<sit_tilt_x10>" */
int kneil_summary_to_line(const kneil_summary_t *s, char *out, size_t size);

/* INFO: encode text (characters outside 0x20-0x7E become '?', text longer than KNEIL_INFO_TEXT_MAX or
 * than the buffer is cut). Returns the packet length (>= 2), or -1 if the text is empty or buf too small. */
int kneil_info_encode(const char *text, uint8_t *buf, size_t size);
/* INFO packet -> "I,<text>\n". Returns the line length, or -1 if it is not a valid INFO packet or out is too small. */
int kneil_info_to_line(const uint8_t *buf, size_t len, char *out, size_t size);

/* Command line from the PC ("S", "P", "T", "Q", "Z") -> CONTROL byte, or -1 if it is not one of them */
int kneil_cmd_to_ctrl(const char *line);

#ifdef __cplusplus
}
#endif
#endif /* KNEIL_PROTO_H */
