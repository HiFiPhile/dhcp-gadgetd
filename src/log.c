#include "log.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static enum DgLogLevel g_log_level = DG_LOG_INFO;

static int dg_str_equal_nocase(const char *a, const char *b) {
    size_t i;

    if (a == NULL || b == NULL) {
        return 0;
    }

    for (i = 0; a[i] != '\0' && b[i] != '\0'; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) {
            return 0;
        }
    }

    return a[i] == '\0' && b[i] == '\0';
}

void dg_log_init(enum DgLogLevel level) {
    g_log_level = level;
}

enum DgLogLevel dg_log_parse_level(const char *text) {
    if (dg_str_equal_nocase(text, "debug")) {
        return DG_LOG_DEBUG;
    }
    if (dg_str_equal_nocase(text, "warn")) {
        return DG_LOG_WARN;
    }
    if (dg_str_equal_nocase(text, "error")) {
        return DG_LOG_ERROR;
    }
    return DG_LOG_INFO;
}

const char *dg_log_level_name(enum DgLogLevel level) {
    switch (level) {
        case DG_LOG_DEBUG:
            return "DEBUG";
        case DG_LOG_INFO:
            return "INFO";
        case DG_LOG_WARN:
            return "WARN";
        case DG_LOG_ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}

void dg_log(enum DgLogLevel level, const char *fmt, ...) {
    time_t now;
    struct tm tm_now;
    char ts_buf[32];
    va_list args;

    if (level < g_log_level) {
        return;
    }

    now = time(NULL);
    if (localtime_r(&now, &tm_now) == NULL) {
        return;
    }

    if (strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%d %H:%M:%S", &tm_now) == 0) {
        return;
    }

    fprintf(stderr, "%s %-5s ", ts_buf, dg_log_level_name(level));
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}