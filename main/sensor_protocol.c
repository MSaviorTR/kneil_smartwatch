#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sensor_protocol.h"

#define FIELD_COUNT 7   /* talker + 6 values */

static int hex_value(char c)
{
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static uint8_t checksum(const char *start, const char *end)
{
    uint8_t cs = 0;

    for(const char *p = start; p < end; p++)
    {
        cs ^= (uint8_t)*p;
    }

    return cs;
}

/* Empty field -> NAN. Anything that is not a complete finite number fails. */
static bool parse_float_field(const char *s, float *out)
{
    if(*s == '\0')
    {
        *out = NAN;
        return true;
    }

    char *end = NULL;
    float v = strtof(s, &end);

    if(end == s || *end != '\0' || !isfinite(v))
    {
        return false;
    }

    *out = v;
    return true;
}

static bool parse_uint_field(const char *s, int base, uint32_t *out)
{
    if(*s == '\0' || *s == '-' || *s == '+')
    {
        return false;
    }

    char *end = NULL;
    unsigned long v = strtoul(s, &end, base);

    if(*end != '\0' || v > UINT32_MAX)
    {
        return false;
    }

    *out = (uint32_t)v;
    return true;
}

sensor_parse_result_t sensor_protocol_parse(const char *line, sensor_frame_t *out)
{
    if(line == NULL || out == NULL || line[0] != '$')
    {
        return SENSOR_PARSE_ERR_FORMAT;
    }

    size_t len = strlen(line);

    while(len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n'))
    {
        len--;
    }

    if(len > SENSOR_PROTOCOL_MAX_LINE)
    {
        return SENSOR_PARSE_ERR_FORMAT;
    }

    const char *star = memchr(line, '*', len);

    if(star == NULL || (size_t)(star - line) + 3 != len)
    {
        return SENSOR_PARSE_ERR_FORMAT;
    }

    int hi = hex_value(star[1]);
    int lo = hex_value(star[2]);

    if(hi < 0 || lo < 0)
    {
        return SENSOR_PARSE_ERR_FORMAT;
    }

    if(checksum(line + 1, star) != (uint8_t)((hi << 4) | lo))
    {
        return SENSOR_PARSE_ERR_CHECKSUM;
    }

    /* Copy the body so it can be split in place. Empty fields are kept,
       which is why strtok is not used here. */

    char body[SENSOR_PROTOCOL_MAX_LINE + 1];
    size_t body_len = (size_t)(star - (line + 1));

    memcpy(body, line + 1, body_len);
    body[body_len] = '\0';

    char *fields[FIELD_COUNT];
    int count = 0;
    char *p = body;

    while(count < FIELD_COUNT)
    {
        fields[count++] = p;

        char *comma = strchr(p, ',');

        /* Leave the last field unsplit so extra fields are detectable. */
        if(comma == NULL || count == FIELD_COUNT)
        {
            break;
        }

        *comma = '\0';
        p = comma + 1;
    }

    if(strcmp(fields[0], SENSOR_PROTOCOL_TALKER) != 0)
    {
        return SENSOR_PARSE_IGNORED;
    }

    /* Too few fields, or a trailing comma beyond the last one. */
    if(count != FIELD_COUNT || strchr(fields[FIELD_COUNT - 1], ',') != NULL)
    {
        return SENSOR_PARSE_ERR_FORMAT;
    }

    sensor_frame_t f;

    if(!parse_uint_field(fields[1], 10, &f.seq)    ||
       !parse_uint_field(fields[2], 10, &f.t_ms)   ||
       !parse_float_field(fields[3], &f.k_mv)      ||
       !parse_float_field(fields[4], &f.ph_mv)     ||
       !parse_float_field(fields[5], &f.temp_c)    ||
       !parse_uint_field(fields[6], 16, &f.flags))
    {
        return SENSOR_PARSE_ERR_FORMAT;
    }

    *out = f;
    return SENSOR_PARSE_OK;
}

static int append_float(char *dst, size_t len, float v)
{
    if(isnan(v))
    {
        return snprintf(dst, len, ",");
    }

    return snprintf(dst, len, ",%.3f", (double)v);
}

size_t sensor_protocol_format(char *buf, size_t len, const sensor_frame_t *frame)
{
    char body[SENSOR_PROTOCOL_MAX_LINE];
    int n = snprintf(body, sizeof(body), "%s,%lu,%lu", SENSOR_PROTOCOL_TALKER,
                     (unsigned long)frame->seq, (unsigned long)frame->t_ms);

    const float vals[3] = { frame->k_mv, frame->ph_mv, frame->temp_c };

    for(int i = 0; i < 3 && n > 0 && (size_t)n < sizeof(body); i++)
    {
        n += append_float(body + n, sizeof(body) - (size_t)n, vals[i]);
    }

    if(n > 0 && (size_t)n < sizeof(body))
    {
        n += snprintf(body + n, sizeof(body) - (size_t)n, ",%lX", (unsigned long)frame->flags);
    }

    if(n <= 0 || (size_t)n >= sizeof(body))
    {
        return 0;
    }

    int total = snprintf(buf, len, "$%s*%02X\r\n", body,
                         checksum(body, body + n));

    if(total <= 0 || (size_t)total >= len)
    {
        return 0;
    }

    return (size_t)total;
}

bool sensor_line_feed(sensor_line_buf_t *lb, char c)
{
    if(c == '\n')
    {
        bool complete = !lb->overflow && lb->len > 0;

        lb->buf[lb->len] = '\0';
        lb->overflow = false;

        if(!complete)
        {
            lb->len = 0;
            return false;
        }

        /* Caller reads lb->buf, then the next byte starts a fresh line. */
        lb->len = 0;
        return true;
    }

    if(c == '\r')
    {
        return false;
    }

    /* '$' always starts a new frame, which resyncs after line noise
       (e.g. the ROM bootloader banner on the shared UART0 pins). */
    if(c == '$')
    {
        lb->len = 0;
        lb->overflow = false;
    }

    if(lb->overflow)
    {
        return false;
    }

    if(lb->len >= SENSOR_PROTOCOL_MAX_LINE)
    {
        lb->overflow = true;
        lb->len = 0;
        return false;
    }

    lb->buf[lb->len++] = c;
    return false;
}
