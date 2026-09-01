#ifndef TEST_LAB_CONFIG_API_H
#define TEST_LAB_CONFIG_API_H

#include "runtime/runtime_common.h"

typedef struct {
    int count;
    config_entry_t entries[MAX_GROUP_SIZE];
} lab_config_t;

int lab_config_load(lab_config_t *config, const char *path);
int lab_config_prepare(const lab_config_t *config, const char *path);
uint32_t lab_config_ip_of_rank(const lab_config_t *config, int rank);
int lab_config_rank_of_name(const lab_config_t *config, const char *name);
int lab_node_is_router(const char *name);

#endif
