#ifndef ALLREDUCE_WORKLOAD_H
#define ALLREDUCE_WORKLOAD_H

#include "app_config.h"

int lab_run_host_allreduce(const lab_config_t *config, const char *host_name,
                           const char *config_path);

#endif
