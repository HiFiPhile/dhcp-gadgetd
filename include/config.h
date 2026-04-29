#ifndef DG_CONFIG_H
#define DG_CONFIG_H

#include "dg_types.h"

void dg_config_set_defaults(DgConfig *out_cfg);
int dg_config_load(const char *path, DgConfig *out_cfg);

#endif