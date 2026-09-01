#include "protocol/allreduce_workload.h"
#include "app_config.h"
#include "protocol_api.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[]) {
    lab_config_t config;
    const char *name;
    const char *cfg;
    const char *mode;

    if (argc != 4) {
        fprintf(stderr, "Usage: %s <name> <config> allreduce\n", argv[0]);
        return 1;
    }
    name = argv[1];
    cfg = argv[2];
    mode = argv[3];

    if (strcmp(mode, "allreduce") != 0) {
        fprintf(stderr, "only allreduce mode is supported\n");
        return 1;
    }
    if (lab_config_load(&config, cfg) <= 0) {
        fprintf(stderr, "no config\n");
        return 1;
    }
    if (lab_config_prepare(&config, cfg) != 0) {
        fprintf(stderr, "failed to prepare config\n");
        return 1;
    }

    if (lab_node_is_router(name)) {
        init_router(config.entries, config.count, name);
        INC();
        return 0;
    }

    return lab_run_host_allreduce(&config, name, cfg);
}
