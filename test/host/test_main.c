/*
 * Host-side unit tests for the hardware-independent modules.
 *
 *   ./test/host/run.sh
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "sensor_protocol.h"
#include "calibration.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do {                                              \
        checks++;                                                     \
        if(!(cond)) {                                                 \
            failures++;                                               \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
        }                                                             \
    } while(0)

#define CHECK_NEAR(a, b, tol) CHECK(fabs((double)(a) - (double)(b)) <= (tol))

/* Builds a frame line with a correct checksum around an arbitrary body. */
static void with_checksum(const char *body, char *out, size_t len)
{
    unsigned cs = 0;

    for(const char *p = body; *p; p++)
    {
        cs ^= (unsigned char)*p;
    }

    snprintf(out, len, "$%s*%02X\r\n", body, cs);
}

static void test_parse_valid(void)
{
    char line[160];
    sensor_frame_t f;

    with_checksum("GKS,42,123456,152.250,-21.5,24.8,0", line, sizeof(line));
    CHECK(sensor_protocol_parse(line, &f) == SENSOR_PARSE_OK);
    CHECK(f.seq == 42);
    CHECK(f.t_ms == 123456);
    CHECK_NEAR(f.k_mv, 152.25, 1e-4);
    CHECK_NEAR(f.ph_mv, -21.5, 1e-4);
    CHECK_NEAR(f.temp_c, 24.8, 1e-4);
    CHECK(f.flags == 0);

    /* Lower-case checksum digits and hex flags. */
    with_checksum("GKS,1,2,3,4,5,A", line, sizeof(line));
    for(char *p = strchr(line, '*'); *p; p++) if(*p >= 'A' && *p <= 'F') *p += 32;
    CHECK(sensor_protocol_parse(line, &f) == SENSOR_PARSE_OK);
    CHECK(f.flags == 0x0A);
}

static void test_parse_empty_fields(void)
{
    char line[160];
    sensor_frame_t f;

    with_checksum("GKS,7,99,,-10.0,,3", line, sizeof(line));
    CHECK(sensor_protocol_parse(line, &f) == SENSOR_PARSE_OK);
    CHECK(isnan(f.k_mv));
    CHECK_NEAR(f.ph_mv, -10.0, 1e-4);
    CHECK(isnan(f.temp_c));
    CHECK(f.flags == 3);
}

static void test_parse_rejects(void)
{
    char line[160];
    sensor_frame_t f;

    /* Wrong checksum. */
    CHECK(sensor_protocol_parse("$GKS,1,2,3,4,5,0*00\r\n", &f) == SENSOR_PARSE_ERR_CHECKSUM);

    /* Missing '$', missing '*', junk after checksum. */
    CHECK(sensor_protocol_parse("GKS,1,2,3,4,5,0*00", &f) == SENSOR_PARSE_ERR_FORMAT);
    CHECK(sensor_protocol_parse("$GKS,1,2,3,4,5,0", &f) == SENSOR_PARSE_ERR_FORMAT);
    with_checksum("GKS,1,2,3,4,5,0", line, sizeof(line));
    line[strlen(line) - 2] = 'x';
    CHECK(sensor_protocol_parse(line, &f) == SENSOR_PARSE_ERR_FORMAT);

    /* Too few / too many fields. */
    with_checksum("GKS,1,2,3,4,5", line, sizeof(line));
    CHECK(sensor_protocol_parse(line, &f) == SENSOR_PARSE_ERR_FORMAT);
    with_checksum("GKS,1,2,3,4,5,0,9", line, sizeof(line));
    CHECK(sensor_protocol_parse(line, &f) == SENSOR_PARSE_ERR_FORMAT);

    /* Non-numeric, negative seq, inf. */
    with_checksum("GKS,1,2,abc,4,5,0", line, sizeof(line));
    CHECK(sensor_protocol_parse(line, &f) == SENSOR_PARSE_ERR_FORMAT);
    with_checksum("GKS,-1,2,3,4,5,0", line, sizeof(line));
    CHECK(sensor_protocol_parse(line, &f) == SENSOR_PARSE_ERR_FORMAT);
    with_checksum("GKS,1,2,inf,4,5,0", line, sizeof(line));
    CHECK(sensor_protocol_parse(line, &f) == SENSOR_PARSE_ERR_FORMAT);

    /* Another talker is ignored, not an error. */
    with_checksum("XYZ,1,2,3", line, sizeof(line));
    CHECK(sensor_protocol_parse(line, &f) == SENSOR_PARSE_IGNORED);
}

static void test_format_roundtrip(void)
{
    sensor_frame_t in = {
        .seq = 4000000000u, .t_ms = 17, .k_mv = 201.125f,
        .ph_mv = NAN, .temp_c = 37.0f, .flags = 0x5,
    };
    char line[160];
    sensor_frame_t out;

    size_t n = sensor_protocol_format(line, sizeof(line), &in);
    CHECK(n > 0 && n == strlen(line));
    CHECK(sensor_protocol_parse(line, &out) == SENSOR_PARSE_OK);
    CHECK(out.seq == in.seq);
    CHECK(out.t_ms == in.t_ms);
    CHECK_NEAR(out.k_mv, in.k_mv, 1e-3);
    CHECK(isnan(out.ph_mv));
    CHECK_NEAR(out.temp_c, 37.0, 1e-3);
    CHECK(out.flags == 0x5);

    /* Too-small buffer reports failure instead of truncating. */
    CHECK(sensor_protocol_format(line, 10, &in) == 0);
}

static int feed_string(sensor_line_buf_t *lb, const char *s, char lines[][160], int max)
{
    int got = 0;

    for(; *s; s++)
    {
        if(sensor_line_feed(lb, *s) && got < max)
        {
            strcpy(lines[got++], lb->buf);
        }
    }

    return got;
}

static void test_line_assembler(void)
{
    sensor_line_buf_t lb = {0};
    char lines[4][160];

    /* Boot noise, then two frames split across arbitrary boundaries. */
    int n = feed_string(&lb, "ets Jun  8 2016\r\nrst:0x1$GKS,1*00\r\n$GKS,2*00\n", lines, 4);
    CHECK(n == 3);
    CHECK(strcmp(lines[1], "$GKS,1*00") == 0);
    CHECK(strcmp(lines[2], "$GKS,2*00") == 0);

    /* An over-long line is dropped whole; the next line still arrives. */
    char longline[400];
    memset(longline, 'A', sizeof(longline) - 2);
    longline[sizeof(longline) - 2] = '\n';
    longline[sizeof(longline) - 1] = '\0';

    n = feed_string(&lb, longline, lines, 4);
    CHECK(n == 0);
    n = feed_string(&lb, "$OK*00\n", lines, 4);
    CHECK(n == 1);
    CHECK(strcmp(lines[0], "$OK*00") == 0);

    /* Blank lines produce nothing. */
    CHECK(feed_string(&lb, "\r\n\n", lines, 4) == 0);
}

/* ---- BLE-style packet assembly ---------------------------------- */

static char got_lines[8][160];
static int got_n;

static void collect(const char *line, void *ctx)
{
    (void)ctx;
    if(got_n < 8) strcpy(got_lines[got_n++], line);
}

static int feed_packets(sensor_line_buf_t *lb, const char *const *pkts, int n)
{
    got_n = 0;
    for(int i = 0; i < n; i++)
    {
        sensor_feed_chunk(lb, (const uint8_t *)pkts[i], strlen(pkts[i]), collect, NULL);
    }
    return got_n;
}

static void test_chunk_assembly(void)
{
    char f1[160], f2[160];
    with_checksum("GKS,1,10,150.000,-20.000,36.0,0", f1, sizeof(f1));
    with_checksum("GKS,2,20,151.000,-21.000,36.0,0", f2, sizeof(f2));

    /* 1. One frame per notification, no newline (common for BLE sensors). */
    sensor_line_buf_t lb = {0};
    char a[160], b[160];
    snprintf(a, sizeof(a), "%.*s", (int)(strlen(f1) - 2), f1);   /* strip CRLF */
    snprintf(b, sizeof(b), "%.*s", (int)(strlen(f2) - 2), f2);
    const char *p1[] = { a, b };
    CHECK(feed_packets(&lb, p1, 2) == 2);
    sensor_frame_t fr;
    CHECK(sensor_protocol_parse(got_lines[0], &fr) == SENSOR_PARSE_OK && fr.seq == 1);
    CHECK(sensor_protocol_parse(got_lines[1], &fr) == SENSOR_PARSE_OK && fr.seq == 2);

    /* 2. A frame split across 20-byte packets (default BLE MTU), with CRLF. */
    sensor_line_buf_t lb2 = {0};
    char parts[8][32];
    const char *pp[8];
    int np = 0;
    for(size_t off = 0; off < strlen(f1); off += 20, np++)
    {
        snprintf(parts[np], sizeof(parts[np]), "%.20s", f1 + off);
        pp[np] = parts[np];
    }
    CHECK(feed_packets(&lb2, pp, np) == 1);
    CHECK(sensor_protocol_parse(got_lines[0], &fr) == SENSOR_PARSE_OK && fr.seq == 1);

    /* 3. Split right after the checksum, newline in the next packet:
          delivered once, not twice. */
    sensor_line_buf_t lb3 = {0};
    const char *p3[] = { a, "\r\n", b, "\n" };
    CHECK(feed_packets(&lb3, p3, 4) == 2);

    /* 4. Split inside the checksum: must wait for the second hex digit. */
    sensor_line_buf_t lb4 = {0};
    char head[160];
    snprintf(head, sizeof(head), "%.*s", (int)(strlen(a) - 1), a);
    const char *p4[] = { head, a + strlen(a) - 1 };
    CHECK(feed_packets(&lb4, p4, 2) == 1);
    CHECK(sensor_protocol_parse(got_lines[0], &fr) == SENSOR_PARSE_OK);

    /* 5. Two frames in one packet, junk before them. */
    sensor_line_buf_t lb5 = {0};
    char both[400];
    snprintf(both, sizeof(both), "noise%s%s", f1, f2);
    const char *p5[] = { both };
    CHECK(feed_packets(&lb5, p5, 1) == 2);
}

static void test_calibration_two_point(void)
{
    /* Ideal K+ electrode: 1 mM -> 100 mV, 10 mM -> 159.16 mV */
    cal_point_t pts[2] = { { 0.0f, 100.0f }, { 1.0f, 159.16f } };
    cal_fit_t fit = {0};

    CHECK(cal_fit_linear(pts, 2, &fit));
    CHECK(fit.valid);
    CHECK_NEAR(fit.slope, 59.16, 1e-3);
    CHECK_NEAR(fit.intercept, 100.0, 1e-3);
    CHECK_NEAR(fit.r2, 1.0, 1e-6);
    CHECK_NEAR(cal_slope_percent(&fit), 100.0, 0.05);

    CHECK_NEAR(cal_k_mv_to_mM(&fit, 100.0f), 1.0, 1e-4);
    CHECK_NEAR(cal_k_mv_to_mM(&fit, 159.16f), 10.0, 1e-3);
    /* 4 mM: E = 100 + 59.16*log10(4) */
    CHECK_NEAR(cal_k_mv_to_mM(&fit, 100.0f + 59.16f * log10f(4.0f)), 4.0, 1e-3);
}

static void test_calibration_ph(void)
{
    /* pH buffers 4 / 7 / 10 with a slightly sub-Nernstian glass electrode */
    cal_point_t pts[3] = { { 4.0f, 171.0f }, { 7.0f, 0.5f }, { 10.0f, -170.0f } };
    cal_fit_t fit = {0};

    CHECK(cal_fit_linear(pts, 3, &fit));
    CHECK(fit.slope < 0.0f);
    CHECK(fit.r2 > 0.999f);
    CHECK_NEAR(cal_mv_to_x(&fit, 0.5f), 7.0, 0.02);
    CHECK(cal_slope_percent(&fit) > 95.0f && cal_slope_percent(&fit) < 97.0f);
}

static void test_calibration_degenerate(void)
{
    cal_fit_t fit = { .valid = false };
    cal_point_t same_x[2] = { { 1.0f, 10.0f }, { 1.0f, 20.0f } };
    cal_point_t flat[2]   = { { 0.0f, 10.0f }, { 1.0f, 10.0f } };
    cal_point_t nan_pt[2] = { { 0.0f, NAN },   { 1.0f, 10.0f } };

    CHECK(!cal_fit_linear(same_x, 2, &fit));
    CHECK(!cal_fit_linear(flat, 2, &fit));
    CHECK(!cal_fit_linear(nan_pt, 2, &fit));
    CHECK(!cal_fit_linear(same_x, 1, &fit));
    CHECK(!cal_fit_linear(same_x, CAL_MAX_POINTS + 1, &fit));
    CHECK(!fit.valid);

    CHECK(isnan(cal_mv_to_x(&fit, 10.0f)));
    CHECK(isnan(cal_k_mv_to_mM(&fit, 10.0f)));
    CHECK(isnan(cal_slope_percent(&fit)));
}

static void test_nernst(void)
{
    CHECK_NEAR(cal_nernst_slope(25.0f, 1), 59.16, 0.01);
    CHECK_NEAR(cal_nernst_slope(37.0f, 1), 61.54, 0.01);
    CHECK_NEAR(cal_nernst_slope(25.0f, 2), 29.58, 0.01);
}

int main(void)
{
    test_parse_valid();
    test_parse_empty_fields();
    test_parse_rejects();
    test_format_roundtrip();
    test_line_assembler();
    test_chunk_assembly();
    test_calibration_two_point();
    test_calibration_ph();
    test_calibration_degenerate();
    test_nernst();

    printf("%d/%d checks passed\n", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
