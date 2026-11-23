#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "app_state.h"
#include "kneil_central.h"
#include "kneil_state.h"
#include "ble_service.h"
#include "data_logger.h"
#include "rtc_service.h"
#include "sensor_protocol.h"
#include "usb_console.h"

static const char *TAG = "console";

static bool safe_name(const char *name)
{
    return name[0] != '\0' && strchr(name, '/') == NULL && strstr(name, "..") == NULL;
}

static const char *root_or_complain(void)
{
    const char *root = data_logger_root();

    if(root == NULL)
    {
        printf("storage not mounted\n");
    }

    return root;
}

static void print_fit(const char *name, const cal_fit_t *f)
{
    if(!f->valid)
    {
        printf("%-3s not calibrated\n", name);
        return;
    }

    printf("%-3s slope %.2f mV (%.1f %% Nernst)  intercept %.2f mV  R2 %.5f  n=%d  at %lld\n",
           name, (double)f->slope, (double)cal_slope_percent(f), (double)f->intercept,
           (double)f->r2, f->n_points, (long long)f->fitted_at);
}

/* ------------------------------------------------------------------ */

static int cmd_status(int argc, char **argv)
{
    (void)argc; (void)argv;

    time_t now = time(NULL);
    struct tm t;
    char ts[32];

    localtime_r(&now, &t);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S %Z", &t);

    static const char *time_src[] = { "NOT SET", "rtc", "user" };
    printf("time     %s (%s)\n", ts, time_src[rtc_service_time_source()]);

    link_stats_t ls;
    app_state_link_stats(&ls);

    double age = ls.last_frame_us ? (esp_timer_get_time() - ls.last_frame_us) / 1e6 : -1.0;
    printf("link     %s  ok=%lu bad_cs=%lu bad_fmt=%lu dropped=%lu  last=%.1fs ago\n",
           app_state_link_alive() ? "UP" : "DOWN",
           (unsigned long)ls.frames_ok, (unsigned long)ls.frames_bad_checksum,
           (unsigned long)ls.frames_bad_format, (unsigned long)ls.frames_dropped, age);

    reading_t r;

    printf("source   %s\n", app_state_source_name(app_state_active_source()));

    if(app_state_latest(&r))
    {
        printf("latest   [%s] K %.2f mV = %.3f mM | pH %.2f mV = %.3f | %.1f C | flags %lX%s\n",
               app_state_source_name(r.source), (double)r.k_mv, (double)r.k_mM,
               (double)r.ph_mv, (double)r.ph, (double)r.temp_c,
               (unsigned long)r.flags, r.k_alert ? " | K+ ALERT" : "");
    }

    logger_status_t lg;
    data_logger_status(&lg);

    static const char *store[] = { "none", "sd", "flash" };
    printf("logging  %s  file=%s rows=%lu dropped=%lu free=%llu/%llu KB%s%s\n",
           store[lg.store], lg.file[0] ? lg.file : "-", (unsigned long)lg.rows,
           (unsigned long)lg.dropped,
           (unsigned long long)(lg.free_bytes / 1024),
           (unsigned long long)(lg.total_bytes / 1024),
           lg.full ? " FULL" : "", lg.error ? " ERROR" : "");

    ble_status_t b;
    ble_service_status(&b);
    static const char *const bmode[BLE_MODE_COUNT] = { "phone", "sensor", "phone+sensor", "kneil" };
    _Static_assert(sizeof(bmode) / sizeof(bmode[0]) == BLE_MODE_COUNT,
                   "bmode must have one name per ble_mode_t");
    printf("ble      %s  phone=%s  sensor=%s%s%s frames=%lu\n",
           b.enabled ? bmode[b.mode % BLE_MODE_COUNT] : "OFF",
           b.phone_connected ? "connected" : b.advertising ? "advertising" : "-",
           b.sensor_receiving ? "receiving" : b.sensor_connected ? "connected" :
           b.sensor_scanning ? "scanning" : "-",
           b.sensor_name[0] ? " " : "", b.sensor_name, (unsigned long)b.sensor_frames);

    /* Internal DMA RAM is what the display and SPI need; watch this one. */
    printf("memory   internal DMA free %u KB (largest block %u KB), PSRAM free %u KB\n",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    return 0;
}

static int cmd_cal(int argc, char **argv)
{
    (void)argc; (void)argv;

    cal_fit_t f;

    app_state_get_calibration(CAL_CHANNEL_K, &f);
    print_fit("K", &f);
    app_state_get_calibration(CAL_CHANNEL_PH, &f);
    print_fit("pH", &f);

    return 0;
}

static int cmd_settime(int argc, char **argv)
{
    if(argc != 2)
    {
        printf("usage: settime <unix seconds>\n");
        return 1;
    }

    char *end = NULL;
    long long v = strtoll(argv[1], &end, 10);

    if(*end != '\0' || v < 1735689600LL)
    {
        printf("expected unix seconds after 2025-01-01\n");
        return 1;
    }

    esp_err_t err = rtc_service_set_epoch((time_t)v);
    printf("%s\n", err == ESP_OK ? "ok" : "system time set, RTC write failed");

    return err == ESP_OK ? 0 : 1;
}

static int cmd_ls(int argc, char **argv)
{
    (void)argc; (void)argv;

    const char *root = root_or_complain();

    if(root == NULL)
    {
        return 1;
    }

    DIR *d = opendir(root);

    if(d == NULL)
    {
        printf("cannot open %s\n", root);
        return 1;
    }

    struct dirent *e;
    char path[300];

    while((e = readdir(d)) != NULL)
    {
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", root, e->d_name);

        if(stat(path, &st) == 0 && S_ISREG(st.st_mode))
        {
            printf("%10ld  %s\n", (long)st.st_size, e->d_name);
        }
    }

    closedir(d);
    return 0;
}

static int cmd_cat(int argc, char **argv)
{
    const char *root = root_or_complain();

    if(root == NULL)
    {
        return 1;
    }

    if(argc != 2 || !safe_name(argv[1]))
    {
        printf("usage: cat <file>\n");
        return 1;
    }

    char path[300];
    snprintf(path, sizeof(path), "%s/%s", root, argv[1]);

    FILE *f = fopen(path, "r");

    if(f == NULL)
    {
        printf("no such file: %s\n", argv[1]);
        return 1;
    }

    struct stat st;
    long size = (stat(path, &st) == 0) ? (long)st.st_size : -1;

    /* Keep log lines out of the middle of the dump. */
    esp_log_level_t prev = esp_log_level_get("*");
    esp_log_level_set("*", ESP_LOG_NONE);

    printf("-----BEGIN %s %ld-----\n", argv[1], size);

    char buf[256];
    size_t n;

    while((n = fread(buf, 1, sizeof(buf), f)) > 0)
    {
        fwrite(buf, 1, n, stdout);
    }

    printf("-----END-----\n");
    fflush(stdout);
    fclose(f);

    esp_log_level_set("*", prev);
    return 0;
}

static int cmd_rm(int argc, char **argv)
{
    const char *root = root_or_complain();

    if(root == NULL)
    {
        return 1;
    }

    if(argc != 2 || !safe_name(argv[1]))
    {
        printf("usage: rm <file> | rm all\n");
        return 1;
    }

    if(strcmp(argv[1], "all") == 0)
    {
        data_logger_clear();
        printf("clearing all gw_*.csv files\n");
        return 0;
    }

    char path[300];
    snprintf(path, sizeof(path), "%s/%s", root, argv[1]);

    if(unlink(path) != 0)
    {
        printf("could not delete %s\n", argv[1]);
        return 1;
    }

    printf("deleted %s\n", argv[1]);
    return 0;
}

/* Same frames as the UART link, delivered over the USB cable, so a laptop
   can stream live data (potentiostat export, bridge script, emulator)
   without a USB-UART adapter. Silent on success to keep streaming fast. */
static int cmd_frame(int argc, char **argv)
{
    if(argc != 2)
    {
        printf("usage: frame $GKS,...*CS\n");
        return 1;
    }

    sensor_frame_t f;

    switch(sensor_protocol_parse(argv[1], &f))
    {
        case SENSOR_PARSE_OK:
            app_state_push_frame(&f, DATA_SRC_SENSOR);
            return 0;

        case SENSOR_PARSE_ERR_CHECKSUM:
            app_state_count_bad_frame(true);
            printf("bad checksum\n");
            return 1;

        default:
            app_state_count_bad_frame(false);
            printf("bad frame\n");
            return 1;
    }
}

/* KNEIL monitor: one status line, and an optional text-line stream for the PC.
   `kneil`                  status: link, device, RSSI, MTU, version, class, counters
   `kneil lines on|off`     print every received LIVE / INFO / SUMMARY as the KNEIL protocol lines
                            (L,..., I,..., D,...) so the PC can watch the same stream. Raw streaming is
                            not forwarded: this is not a replacement for the XIAO bridge. */
static int cmd_kneil(int argc, char **argv)
{
    if(argc >= 2 && strcmp(argv[1], "lines") == 0)
    {
        if(argc != 3 || (strcmp(argv[2], "on") != 0 && strcmp(argv[2], "off") != 0))
        {
            printf("usage: kneil lines on|off\n");
            return 1;
        }

        kneil_central_set_lines(strcmp(argv[2], "on") == 0);
        printf("kneil lines %s\n", kneil_central_lines() ? "on" : "off");
        return 0;
    }

    if(argc != 1)
    {
        printf("usage: kneil | kneil lines on|off\n");
        return 1;
    }

    kneil_model_t m;
    kneil_central_snapshot(&m);

    printf("kneil: %s%s%s%s rssi=%d mtu=%u v%u pkts=%lu gaps=%lu class=%s raw=%s vote=%d posture=%s "
           "sts=%lu s2s=%lu bed=%lu summary=%s lines=%s\n",
           kneil_link_text(m.link), m.name[0] ? " " : "", m.name[0] ? m.name : "", "",
           m.rssi, (unsigned)m.mtu, (unsigned)m.version,
           (unsigned long)m.packets, (unsigned long)m.gaps,
           kneil_class_text(m.cls, m.version), kneil_class_text(m.cls_raw, m.version),
           (m.vote_pct == KNEIL_VOTE_NONE) ? -1 : m.vote_pct,
           kneil_posture_text(m.posture),
           (unsigned long)m.n_sit_to_stand, (unsigned long)m.n_stand_to_sit, (unsigned long)m.n_bed_transfer,
           m.have_summary ? "yes" : "no", kneil_central_lines() ? "on" : "off");
    return 0;
}

/* ------------------------------------------------------------------ */

esp_err_t usb_console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();

    cfg.prompt = "gw>";
    cfg.max_cmdline_length = 128;
    cfg.task_stack_size = 6144;

    esp_console_dev_usb_serial_jtag_config_t hw = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&hw, &cfg, &repl);

    if(err != ESP_OK)
    {
        ESP_LOGE(TAG, "console init failed: %s", esp_err_to_name(err));
        return err;
    }

    const esp_console_cmd_t cmds[] = {
        { .command = "status",  .help = "link, latest reading, time, storage", .func = cmd_status },
        { .command = "cal",     .help = "show stored calibrations",            .func = cmd_cal },
        { .command = "settime", .help = "settime <unix seconds UTC>",          .func = cmd_settime },
        { .command = "ls",      .help = "list log files",                      .func = cmd_ls },
        { .command = "cat",     .help = "cat <file> - dump a log file",        .func = cmd_cat },
        { .command = "rm",      .help = "rm <file> | rm all",                  .func = cmd_rm },
        { .command = "frame",   .help = "frame <$GKS line> - live data over USB", .func = cmd_frame },
        { .command = "kneil",   .help = "kneil | kneil lines on|off - KNEIL monitor status / text lines", .func = cmd_kneil },
    };

    for(size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
    {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }

    esp_console_register_help_command();

    return esp_console_start_repl(repl);
}
