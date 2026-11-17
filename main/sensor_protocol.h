#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Sensor MCU -> ESP32 wire format (one ASCII line per sample, NMEA style):
 *
 *   $GKS,<seq>,<t_ms>,<k_mV>,<ph_mV>,<temp_C>,<flags>*<CS>\r\n
 *
 *   seq     uint32, incremented per frame (gaps = dropped frames)
 *   t_ms    uint32, sensor MCU uptime in ms
 *   k_mV    K+ ISE potential vs reference, mV      (empty = channel absent)
 *   ph_mV   pH electrode potential vs reference, mV (empty = channel absent)
 *   temp_C  solution / board temperature, deg C    (empty = not measured)
 *   flags   hex bitfield, see SENSOR_FLAG_*
 *   CS      two hex digits: XOR of every byte between '$' and '*'
 *
 * Raw millivolts are sent on purpose: calibration lives on the watch so
 * the sensor MCU firmware never has to change when electrodes do.
 */

#define SENSOR_PROTOCOL_TALKER   "GKS"
#define SENSOR_PROTOCOL_MAX_LINE 120

#define SENSOR_FLAG_K_FAULT      0x01   /* K+ channel open / out of range */
#define SENSOR_FLAG_PH_FAULT     0x02   /* pH channel open / out of range */
#define SENSOR_FLAG_SATURATED    0x04   /* front-end ADC clipped          */
#define SENSOR_FLAG_LOW_SUPPLY   0x08   /* sensor MCU supply below spec   */

typedef struct
{
    uint32_t seq;
    uint32_t t_ms;
    float    k_mv;      /* NAN when absent */
    float    ph_mv;     /* NAN when absent */
    float    temp_c;    /* NAN when absent */
    uint32_t flags;
} sensor_frame_t;

typedef enum
{
    SENSOR_PARSE_OK = 0,
    SENSOR_PARSE_ERR_FORMAT,
    SENSOR_PARSE_ERR_CHECKSUM,
    SENSOR_PARSE_IGNORED,       /* valid NMEA-ish line for another talker */
} sensor_parse_result_t;

/* Parses one line (with or without trailing CR/LF). */
sensor_parse_result_t sensor_protocol_parse(const char *line, sensor_frame_t *out);

/* Formats a frame including checksum and CRLF. Returns bytes written
   (excluding NUL), or 0 if the buffer is too small. */
size_t sensor_protocol_format(char *buf, size_t len, const sensor_frame_t *frame);

/* Byte-wise line assembler for a UART stream. Over-long lines are
   discarded up to the next newline instead of being split. */
typedef struct
{
    char   buf[SENSOR_PROTOCOL_MAX_LINE + 1];
    size_t len;
    bool   overflow;
} sensor_line_buf_t;

/* Feeds one byte. Returns true when a complete line is in lb->buf. */
bool sensor_line_feed(sensor_line_buf_t *lb, char c);

/* Feeds one packet (e.g. a BLE notification) and calls on_line for every
   complete line. Packets may split or join frames freely. A frame that
   ends a packet without a newline is completed as soon as its "*HH"
   checksum has arrived, so senders may omit the newline when each
   notification carries exactly one frame. Returns the lines delivered. */
typedef void (*sensor_line_cb_t)(const char *line, void *ctx);

int sensor_feed_chunk(sensor_line_buf_t *lb, const uint8_t *data, size_t len,
                      sensor_line_cb_t on_line, void *ctx);

#ifdef __cplusplus
}
#endif
