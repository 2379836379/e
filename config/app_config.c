#include "app_config.h"
#include "arbor_fabric.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

int lab_config_load(lab_config_t *config, const char *path) {
    FILE *fp;
    char line[256];

    if (!config || !path) return -1;
    memset(config, 0, sizeof(*config));
    fp = fopen(path, "r");
    if (!fp) {
        perror("open config");
        return -1;
    }
    while (fgets(line, sizeof(line), fp) && config->count < MAX_GROUP_SIZE) {
        config_entry_t *e;
        char ip[32];
        if (line[0] == '#' || line[0] == '\n') continue;
        e = &config->entries[config->count];
        if (sscanf(line, "%d,%31[^,],%31[^,],%31[^,],%31[^,],%31[^,],%31s",
                   &e->rank, e->host_name,
                   e->host_ifaces[0], e->router_ifaces[0],
                   e->host_ifaces[1], e->router_ifaces[1], ip) != 7) {
            fprintf(stderr, "bad config line: %s", line);
            continue;
        }
        if (inet_pton(AF_INET, ip, &e->host_ip) != 1) {
            fprintf(stderr, "bad ip: %s\n", ip);
            continue;
        }
        config->count++;
    }
    fclose(fp);
    return config->count;
}

int lab_config_prepare(const lab_config_t *config, const char *path) {
    char graph_path[512];
    char tree_path[512];
    const char *slash;

    if (!config || !path) return -1;
    slash = strrchr(path, '/');
    if (slash) {
        snprintf(graph_path, sizeof(graph_path), "%.*s/graph.cfg", (int)(slash - path), path);
        snprintf(tree_path, sizeof(tree_path), "%.*s/tree.cfg", (int)(slash - path), path);
    } else {
        snprintf(graph_path, sizeof(graph_path), "graph.cfg");
        snprintf(tree_path, sizeof(tree_path), "tree.cfg");
    }
    common_set_group((config_entry_t *)config->entries, config->count, graph_path);
    if (!ArborFabricLoadFromFiles(path, tree_path)) return -1;
    return 0;
}

uint32_t lab_config_ip_of_rank(const lab_config_t *config, int rank) {
    int i;
    if (!config) return 0;
    for (i = 0; i < config->count; ++i) {
        if (config->entries[i].rank == rank) return config->entries[i].host_ip;
    }
    return 0;
}

int lab_config_rank_of_name(const lab_config_t *config, const char *name) {
    int i;
    if (!config || !name) return -1;
    for (i = 0; i < config->count; ++i) {
        if (strcmp(config->entries[i].host_name, name) == 0) return config->entries[i].rank;
    }
    return -1;
}

int lab_node_is_router(const char *name) {
    return name && strncmp(name, "router", 6) == 0;
}
