/*
 * Contract test between tools/sensor_mcu_emulator.py and the firmware
 * parser: feeds stdin through the same line assembler + parser the
 * watch uses and prints a tally.
 */

#include <stdio.h>

#include "sensor_protocol.h"

int main(void)
{
    sensor_line_buf_t lb = {0};
    int ok = 0, bad_cs = 0, bad_fmt = 0, ignored = 0;
    int c;

    while((c = getchar()) != EOF)
    {
        if(!sensor_line_feed(&lb, (char)c))
        {
            continue;
        }

        sensor_frame_t f;

        switch(sensor_protocol_parse(lb.buf, &f))
        {
            case SENSOR_PARSE_OK:           ok++;      break;
            case SENSOR_PARSE_ERR_CHECKSUM: bad_cs++;  break;
            case SENSOR_PARSE_ERR_FORMAT:   bad_fmt++; break;
            case SENSOR_PARSE_IGNORED:      ignored++; break;
        }
    }

    printf("ok=%d checksum_err=%d format_err=%d ignored=%d\n", ok, bad_cs, bad_fmt, ignored);
    return bad_fmt == 0 ? 0 : 1;
}
