#ifndef DG_LOG_H
#define DG_LOG_H

enum DgLogLevel {
    DG_LOG_DEBUG = 0,
    DG_LOG_INFO = 1,
    DG_LOG_WARN = 2,
    DG_LOG_ERROR = 3,
};

void dg_log_init(enum DgLogLevel level);
enum DgLogLevel dg_log_parse_level(const char *text);
const char *dg_log_level_name(enum DgLogLevel level);
void dg_log(enum DgLogLevel level, const char *fmt, ...);

#endif