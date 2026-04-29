#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "log.h"
#include "server.h"

static volatile sig_atomic_t g_stop_requested = 0;

typedef struct {
    const char *config_path;
    enum DgLogLevel log_level;
} DgArgs;

static void dg_print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s --config <path> [--log-level <debug|info|warn|error>]\n", prog);
}

static void dg_on_signal(int signum) {
    (void)signum;
    g_stop_requested = 1;
}

static int dg_install_signal_handlers(void) {
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = dg_on_signal;

    if (sigaction(SIGINT, &action, NULL) != 0) {
        return -1;
    }
    if (sigaction(SIGTERM, &action, NULL) != 0) {
        return -1;
    }
    return 0;
}

static int dg_parse_args(int argc, char **argv, DgArgs *out_args) {
    int i;

    out_args->config_path = NULL;
    out_args->log_level = DG_LOG_INFO;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0) {
            if (i + 1 >= argc) {
                return -1;
            }
            out_args->config_path = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--log-level") == 0) {
            if (i + 1 >= argc) {
                return -1;
            }
            out_args->log_level = dg_log_parse_level(argv[++i]);
            continue;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            dg_print_usage(argv[0]);
            return 1;
        }

        return -1;
    }

    if (out_args->config_path == NULL) {
        return -1;
    }

    return 0;
}

int main(int argc, char **argv) {
    DgArgs args;
    DgConfig cfg;
    DgServer server;
    int parse_rc;
    int exit_code = 0;

    parse_rc = dg_parse_args(argc, argv, &args);
    if (parse_rc != 0) {
        if (parse_rc < 0) {
            dg_print_usage(argv[0]);
            return 2;
        }
        return 0;
    }

    dg_log_init(args.log_level);

    if (dg_config_load(args.config_path, &cfg) != 0) {
        return 1;
    }
    if (dg_server_init(&server, &cfg) != 0) {
        return 1;
    }
    if (dg_install_signal_handlers() != 0) {
        dg_log(DG_LOG_ERROR, "failed to install signal handlers");
        dg_server_shutdown(&server);
        return 1;
    }

    dg_log(DG_LOG_INFO, "dhcp-gadgetd started");

    while (!g_stop_requested && dg_server_is_running(&server)) {
        if (dg_server_tick(&server, 250) != 0) {
            dg_log(DG_LOG_ERROR, "server loop aborted due to runtime error");
            exit_code = 1;
            break;
        }
    }

    if (g_stop_requested) {
        dg_server_request_stop(&server);
    }
    dg_server_shutdown(&server);
    dg_log(DG_LOG_INFO, "dhcp-gadgetd stopped");

    return exit_code;
}