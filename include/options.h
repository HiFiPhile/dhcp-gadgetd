#ifndef DG_OPTIONS_H
#define DG_OPTIONS_H

#include "dg_types.h"

size_t dg_options_encode(uint8_t *dst, size_t cap, uint8_t msg_type,
                         const DgConfig *cfg, int include_lease);

#endif
