#ifndef DG_SERVER_H
#define DG_SERVER_H

#include <stdint.h>

#include "allocator.h"
#include "bus.h"
#include "dg_types.h"

typedef struct {
    DgConfig config;
    DgAllocator *allocator;
    DgBus *bus;
    int running;
    uint64_t tick_count;
} DgServer;

int dg_server_init(DgServer *server, const DgConfig *cfg);
int dg_server_tick(DgServer *server, int timeout_ms);
void dg_server_request_stop(DgServer *server);
int dg_server_is_running(const DgServer *server);
void dg_server_shutdown(DgServer *server);

#endif