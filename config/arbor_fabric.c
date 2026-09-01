#include "arbor_fabric.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROUTER_MAX_COUNT 32
#define ROUTER_MAX_PORTS (MAX_GROUP_SIZE * SUBCHANNEL_COUNT * 4)

struct ArborRouterNodeConfig {
    char name[32];
    char devs[ROUTER_MAX_PORTS][32];
    int dev_count;
    char route_ports[MAX_GROUP_SIZE][SUBCHANNEL_COUNT][32];
    char parent_up_ports[MAX_GROUP_SIZE][SUBCHANNEL_COUNT][32];
    char parent_down_ports[MAX_GROUP_SIZE][SUBCHANNEL_COUNT][32];
    uint8_t tree_role[MAX_GROUP_SIZE][SUBCHANNEL_COUNT];
    uint8_t route_valid[MAX_GROUP_SIZE][SUBCHANNEL_COUNT];
    uint8_t tree_valid[MAX_GROUP_SIZE][SUBCHANNEL_COUNT];
    char mcast_ports[MAX_GROUP_SIZE][SUBCHANNEL_COUNT][ROUTER_MAX_PORTS][32];
    uint8_t mcast_count[MAX_GROUP_SIZE][SUBCHANNEL_COUNT];
};

typedef struct {
    uint8_t valid;
    ArborRankPlan plan;
} requester_plan_entry_t;

static ArborFabric g_fabric;
static ArborRouterNodeConfig g_router_configs[ROUTER_MAX_COUNT];
static size_t g_router_count = 0;
static requester_plan_entry_t g_requester_plans[MAX_GROUP_SIZE][MAX_GROUP_SIZE][SUBCHANNEL_COUNT];
static int g_loaded = 0;

static void reset_state(void) {
    memset(&g_fabric, 0, sizeof(g_fabric));
    memset(g_router_configs, 0, sizeof(g_router_configs));
    memset(g_requester_plans, 0, sizeof(g_requester_plans));
    g_router_count = 0;
}

static ArborRouterNodeConfig *router_for(const char *name, int create) {
    size_t i;
    if (!name || !name[0]) return NULL;
    for (i = 0; i < g_router_count; ++i) {
        if (strcmp(g_router_configs[i].name, name) == 0) return &g_router_configs[i];
    }
    if (!create || g_router_count >= ROUTER_MAX_COUNT) return NULL;
    memset(&g_router_configs[g_router_count], 0, sizeof(g_router_configs[g_router_count]));
    strncpy(g_router_configs[g_router_count].name, name, sizeof(g_router_configs[g_router_count].name) - 1);
    return &g_router_configs[g_router_count++];
}

static int add_dev(ArborRouterNodeConfig *cfg, const char *port) {
    int i;
    if (!cfg || !port || !port[0] || cfg->dev_count >= ROUTER_MAX_PORTS) return 0;
    for (i = 0; i < cfg->dev_count; ++i) {
        if (strcmp(cfg->devs[i], port) == 0) return 1;
    }
    strncpy(cfg->devs[cfg->dev_count++], port, sizeof(cfg->devs[0]) - 1);
    return 1;
}

static void set_route(ArborRouterNodeConfig *cfg, int responder_rank, int sub, const char *port) {
    if (!cfg || !port || responder_rank < 0 || responder_rank >= MAX_GROUP_SIZE || sub < 0 || sub >= SUBCHANNEL_COUNT) return;
    strncpy(cfg->route_ports[responder_rank][sub], port, sizeof(cfg->route_ports[0][0]) - 1);
    cfg->route_valid[responder_rank][sub] = 1;
    add_dev(cfg, port);
}

static void set_tree(ArborRouterNodeConfig *cfg, int responder_rank, int sub, ArborTreeRole role,
                     const char *parent_up_port, const char *parent_down_port) {
    if (!cfg || !parent_up_port || responder_rank < 0 || responder_rank >= MAX_GROUP_SIZE || sub < 0 || sub >= SUBCHANNEL_COUNT) return;
    if (!parent_down_port || !parent_down_port[0]) parent_down_port = parent_up_port;
    cfg->tree_role[responder_rank][sub] = (uint8_t)role;
    strncpy(cfg->parent_up_ports[responder_rank][sub], parent_up_port, sizeof(cfg->parent_up_ports[0][0]) - 1);
    strncpy(cfg->parent_down_ports[responder_rank][sub], parent_down_port, sizeof(cfg->parent_down_ports[0][0]) - 1);
    cfg->tree_valid[responder_rank][sub] = 1;
    add_dev(cfg, parent_up_port);
    add_dev(cfg, parent_down_port);
}

static void add_mcast(ArborRouterNodeConfig *cfg, int responder_rank, int sub, const char *port) {
    uint8_t i;
    if (!cfg || !port || responder_rank < 0 || responder_rank >= MAX_GROUP_SIZE || sub < 0 || sub >= SUBCHANNEL_COUNT) return;
    if (cfg->mcast_count[responder_rank][sub] >= ROUTER_MAX_PORTS) return;
    for (i = 0; i < cfg->mcast_count[responder_rank][sub]; ++i) {
        if (strcmp(cfg->mcast_ports[responder_rank][sub][i], port) == 0) return;
    }
    strncpy(cfg->mcast_ports[responder_rank][sub][cfg->mcast_count[responder_rank][sub]++], port,
            sizeof(cfg->mcast_ports[0][0][0]) - 1);
    add_dev(cfg, port);
}

static int load_ranks(const char *path) {
    FILE *fp;
    char line[512];

    fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "router config %s not found\n", path ? path : "(null)");
        return 0;
    }
    while (fgets(line, sizeof(line), fp)) {
        char *save = NULL;
        char *tok;
        config_entry_t *rc;
        char ip[32];
        if (line[0] == '#' || line[0] == '\n') continue;
        if (g_fabric.rank_count >= MAX_GROUP_SIZE) break;
        tok = strtok_r(line, ", \t\r\n", &save);
        if (!tok) continue;
        rc = &g_fabric.ranks[g_fabric.rank_count];
        memset(rc, 0, sizeof(*rc));
        if (sscanf(tok, "%d", &rc->rank) != 1) continue;
        tok = strtok_r(NULL, ", \t\r\n", &save); if (!tok) continue; strncpy(rc->host_name, tok, sizeof(rc->host_name) - 1);
        tok = strtok_r(NULL, ", \t\r\n", &save); if (!tok) continue; strncpy(rc->host_ifaces[0], tok, sizeof(rc->host_ifaces[0]) - 1);
        tok = strtok_r(NULL, ", \t\r\n", &save); if (!tok) continue; strncpy(rc->router_ifaces[0], tok, sizeof(rc->router_ifaces[0]) - 1);
        tok = strtok_r(NULL, ", \t\r\n", &save); if (!tok) continue; strncpy(rc->host_ifaces[1], tok, sizeof(rc->host_ifaces[1]) - 1);
        tok = strtok_r(NULL, ", \t\r\n", &save); if (!tok) continue; strncpy(rc->router_ifaces[1], tok, sizeof(rc->router_ifaces[1]) - 1);
        tok = strtok_r(NULL, ", \t\r\n", &save); if (!tok) continue; strncpy(ip, tok, sizeof(ip) - 1); ip[sizeof(ip) - 1] = '\0';
        if (inet_pton(AF_INET, ip, &rc->host_ip) != 1) continue;
        ++g_fabric.rank_count;
    }
    fclose(fp);
    return g_fabric.rank_count > 0;
}

static void build_channels(void) {
    int responder;
    for (responder = 0; responder < g_fabric.rank_count && g_fabric.channel_count < MAX_CHANNELS; ++responder) {
        uint32_t requester_mask = neighbor_mask_of((uint32_t)responder);
        ArborChannelSpec *channel = &g_fabric.channels[g_fabric.channel_count++];
        int rank;
        memset(channel, 0, sizeof(*channel));
        channel->group_id = 0;
        channel->channel_id = (uint32_t)responder;
        channel->responder_rank = (uint32_t)responder;
        channel->num_subchannels = SUBCHANNEL_COUNT;
        for (rank = 0; rank < g_fabric.rank_count; ++rank) {
            if (((requester_mask >> rank) & 1u) == 0) continue;
            channel->requesters[channel->num_requesters++] = (uint32_t)rank;
        }
    }
}

static int is_requester_for(int responder_rank, int requester_rank) {
    uint32_t requester_mask;
    if (responder_rank < 0 || responder_rank >= g_fabric.rank_count) return 0;
    requester_mask = neighbor_mask_of((uint32_t)responder_rank);
    return requester_rank >= 0 && requester_rank < MAX_GROUP_SIZE && ((requester_mask >> requester_rank) & 1u) != 0;
}

static void seed_tree_specs(void) {
    uint32_t channel_idx;
    for (channel_idx = 0; channel_idx < g_fabric.channel_count; ++channel_idx) {
        const ArborChannelSpec *channel = &g_fabric.channels[channel_idx];
        uint32_t sub;
        for (sub = 0; sub < channel->num_subchannels; ++sub) {
            ArborTreeSpec *tree = &g_fabric.tree_specs[channel->channel_id][sub];
            uint32_t req_idx;
            memset(tree, 0, sizeof(*tree));
            tree->requester_count = (uint8_t)channel->num_requesters;
            for (req_idx = 0; req_idx < channel->num_requesters && req_idx < MAX_GROUP_SIZE; ++req_idx) {
                tree->requesters[req_idx].requester_rank = channel->requesters[req_idx];
            }
        }
    }
}

static void infer_tree_summary_from_routers(void) {
    size_t router_idx;
    for (router_idx = 0; router_idx < g_router_count; ++router_idx) {
        ArborRouterNodeConfig *cfg = &g_router_configs[router_idx];
        int responder;
        for (responder = 0; responder < g_fabric.rank_count; ++responder) {
            uint32_t sub;
            for (sub = 0; sub < SUBCHANNEL_COUNT; ++sub) {
                ArborTreeSpec *tree;
                if (!cfg->tree_valid[responder][sub]) continue;
                tree = &g_fabric.tree_specs[responder][sub];
                if (cfg->tree_role[responder][sub] == ARBOR_TREE_RELAY) {
                    if (!tree->root_router[0]) {
                        strncpy(tree->root_router, cfg->name, sizeof(tree->root_router) - 1);
                    }
                } else if (tree->level_count < ARBOR_MAX_STACK_DEPTH) {
                    strncpy(tree->level_routers[tree->level_count], cfg->name,
                            sizeof(tree->level_routers[tree->level_count]) - 1);
                    ++tree->level_count;
                }
            }
        }
    }
}

static int load_tree(const char *path) {
    FILE *fp;
    char line[512];

    fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "tree config %s not found\n", path ? path : "(null)");
        return 0;
    }

    while (fgets(line, sizeof(line), fp)) {
        char original[512];
        char *save = NULL;
        char *kind;
        strncpy(original, line, sizeof(original) - 1);
        original[sizeof(original) - 1] = '\0';
        if (line[0] == '#' || line[0] == '\n') continue;

        kind = strtok_r(line, ", \t\r\n", &save);
        if (!kind) continue;

        if (strcmp(kind, "dev") == 0) {
            char *router_name = strtok_r(NULL, ", \t\r\n", &save);
            char *port = strtok_r(NULL, ", \t\r\n", &save);
            ArborRouterNodeConfig *cfg = router_for(router_name, 1);
            if (!cfg || !port) {
                fprintf(stderr, "bad tree dev line: %s", original);
                fclose(fp);
                return 0;
            }
            add_dev(cfg, port);
            continue;
        }

        if (strcmp(kind, "route") == 0) {
            char *router_name = strtok_r(NULL, ", \t\r\n", &save);
            char *rank_s = strtok_r(NULL, ", \t\r\n", &save);
            char *sub_s = strtok_r(NULL, ", \t\r\n", &save);
            char *port = strtok_r(NULL, ", \t\r\n", &save);
            int rank, sub;
            ArborRouterNodeConfig *cfg = router_for(router_name, 1);
            if (!cfg || !rank_s || !sub_s || !port || sscanf(rank_s, "%d", &rank) != 1 || sscanf(sub_s, "%d", &sub) != 1) {
                fprintf(stderr, "bad tree route line: %s", original);
                fclose(fp);
                return 0;
            }
            set_route(cfg, rank, sub, port);
            continue;
        }

        if (strcmp(kind, "mcast") == 0) {
            char *router_name = strtok_r(NULL, ", \t\r\n", &save);
            char *rank_s = strtok_r(NULL, ", \t\r\n", &save);
            char *sub_s = strtok_r(NULL, ", \t\r\n", &save);
            char *port = strtok_r(NULL, ", \t\r\n", &save);
            int rank, sub;
            ArborRouterNodeConfig *cfg = router_for(router_name, 1);
            if (!cfg || !rank_s || !sub_s || !port || sscanf(rank_s, "%d", &rank) != 1 || sscanf(sub_s, "%d", &sub) != 1) {
                fprintf(stderr, "bad tree mcast line: %s", original);
                fclose(fp);
                return 0;
            }
            add_mcast(cfg, rank, sub, port);
            continue;
        }

        if (strcmp(kind, "tree") == 0) {
            char *router_name = strtok_r(NULL, ", \t\r\n", &save);
            char *rank_s = strtok_r(NULL, ", \t\r\n", &save);
            char *sub_s = strtok_r(NULL, ", \t\r\n", &save);
            char *role_s = strtok_r(NULL, ", \t\r\n", &save);
            char *parent_up_port = strtok_r(NULL, ", \t\r\n", &save);
            char *parent_down_port = strtok_r(NULL, ", \t\r\n", &save);
            int rank, sub;
            ArborTreeRole role;
            ArborRouterNodeConfig *cfg = router_for(router_name, 1);
            if (!cfg || !rank_s || !sub_s || !role_s || !parent_up_port ||
                sscanf(rank_s, "%d", &rank) != 1 || sscanf(sub_s, "%d", &sub) != 1) {
                fprintf(stderr, "bad tree tree line: %s", original);
                fclose(fp);
                return 0;
            }
            role = strcmp(role_s, "RELAY") == 0 ? ARBOR_TREE_RELAY : ARBOR_TREE_LEVEL;
            set_tree(cfg, rank, sub, role, parent_up_port, parent_down_port);
            continue;
        }

        if (strcmp(kind, "req") == 0) {
            char *requester_s = strtok_r(NULL, ", \t\r\n", &save);
            char *responder_s = strtok_r(NULL, ", \t\r\n", &save);
            char *sub_s = strtok_r(NULL, ", \t\r\n", &save);
            char *depth_s = strtok_r(NULL, ", \t\r\n", &save);
            int requester_rank, responder_rank, sub, depth;
            int i;
            requester_plan_entry_t *entry;
            ArborTreeSpec *tree;
            if (!requester_s || !responder_s || !sub_s || !depth_s ||
                sscanf(requester_s, "%d", &requester_rank) != 1 ||
                sscanf(responder_s, "%d", &responder_rank) != 1 ||
                sscanf(sub_s, "%d", &sub) != 1 ||
                sscanf(depth_s, "%d", &depth) != 1 ||
                requester_rank < 0 || requester_rank >= MAX_GROUP_SIZE ||
                responder_rank < 0 || responder_rank >= MAX_GROUP_SIZE ||
                sub < 0 || sub >= SUBCHANNEL_COUNT ||
                depth < 0 || depth > ARBOR_MAX_STACK_DEPTH) {
                fprintf(stderr, "bad tree req line: %s", original);
                fclose(fp);
                return 0;
            }
            entry = &g_requester_plans[requester_rank][responder_rank][sub];
            memset(entry, 0, sizeof(*entry));
            entry->valid = 1;
            entry->plan.stack_depth = (uint8_t)depth;
            for (i = 0; i < depth; ++i) {
                char *fanin_s = strtok_r(NULL, ", \t\r\n", &save);
                if (!fanin_s || sscanf(fanin_s, "%hhu", &entry->plan.fanin[i]) != 1) {
                    fprintf(stderr, "bad tree req line: %s", original);
                    fclose(fp);
                    return 0;
                }
            }
            tree = &g_fabric.tree_specs[responder_rank][sub];
            if (tree->level_count < entry->plan.stack_depth) tree->level_count = entry->plan.stack_depth;
            continue;
        }

        fprintf(stderr, "unknown tree config line: %s", original);
        fclose(fp);
        return 0;
    }

    fclose(fp);
    return 1;
}

static int validate_loaded_state(void) {
    uint32_t channel_idx;
    if (g_fabric.rank_count <= 0 || g_fabric.channel_count <= 0) return 0;
    for (channel_idx = 0; channel_idx < g_fabric.channel_count; ++channel_idx) {
        const ArborChannelSpec *channel = &g_fabric.channels[channel_idx];
        uint32_t sub;
        for (sub = 0; sub < channel->num_subchannels; ++sub) {
            uint32_t req_idx;
            for (req_idx = 0; req_idx < channel->num_requesters; ++req_idx) {
                uint32_t requester_rank = channel->requesters[req_idx];
                if (!is_requester_for((int)channel->responder_rank, (int)requester_rank)) return 0;
                if (!g_requester_plans[requester_rank][channel->responder_rank][sub].valid) return 0;
            }
        }
    }
    return 1;
}

int ArborFabricLoadFromFiles(const char *ranks_path, const char *tree_path) {
    if (g_loaded) return 1;
    reset_state();
    if (!load_ranks(ranks_path)) return 0;
    build_channels();
    seed_tree_specs();
    if (!load_tree(tree_path)) return 0;
    infer_tree_summary_from_routers();
    if (!validate_loaded_state()) return 0;
    g_loaded = 1;
    return 1;
}

const ArborFabric *ArborFabricGet(void) {
    return g_loaded ? &g_fabric : NULL;
}

const ArborRouterNodeConfig *ArborFabricFindRouter(const char *name) {
    return router_for(name, 0);
}

const ArborChannelSpec *ArborFabricGetChannelSpec(uint32_t channel_id) {
    if (!g_loaded || channel_id >= MAX_CHANNELS || channel_id >= g_fabric.channel_count) return NULL;
    return &g_fabric.channels[channel_id];
}

const ArborTreeSpec *ArborFabricGetTreeSpec(uint32_t channel_id, uint32_t subchannel_id) {
    if (!g_loaded || channel_id >= MAX_CHANNELS || subchannel_id >= SUBCHANNEL_COUNT) return NULL;
    return &g_fabric.tree_specs[channel_id][subchannel_id];
}

int ArborFabricGetRequesterPlan(int requester_rank, int responder_rank, uint32_t subchannel_id,
                                ArborRankPlan *plan_out) {
    requester_plan_entry_t *entry;
    if (!g_loaded || !plan_out || requester_rank < 0 || responder_rank < 0 ||
        requester_rank >= MAX_GROUP_SIZE || responder_rank >= MAX_GROUP_SIZE || subchannel_id >= SUBCHANNEL_COUNT) return 0;
    entry = &g_requester_plans[requester_rank][responder_rank][subchannel_id];
    if (!entry->valid) return 0;
    *plan_out = entry->plan;
    return 1;
}

const char *ArborRouterNodeName(const ArborRouterNodeConfig *cfg) { return cfg ? cfg->name : NULL; }
int ArborRouterNodeDevCount(const ArborRouterNodeConfig *cfg) { return cfg ? cfg->dev_count : 0; }
const char *ArborRouterNodeDevName(const ArborRouterNodeConfig *cfg, int index) {
    if (!cfg || index < 0 || index >= cfg->dev_count) return NULL;
    return cfg->devs[index];
}
const char *ArborRouterNodeRoutePort(const ArborRouterNodeConfig *cfg, int responder_rank, uint32_t subchannel_id) {
    if (!cfg || responder_rank < 0 || responder_rank >= MAX_GROUP_SIZE || subchannel_id >= SUBCHANNEL_COUNT || !cfg->route_valid[responder_rank][subchannel_id]) return NULL;
    return cfg->route_ports[responder_rank][subchannel_id];
}
const char *ArborRouterNodeParentUpPort(const ArborRouterNodeConfig *cfg, int responder_rank, uint32_t subchannel_id) {
    if (!cfg || responder_rank < 0 || responder_rank >= MAX_GROUP_SIZE || subchannel_id >= SUBCHANNEL_COUNT || !cfg->tree_valid[responder_rank][subchannel_id]) return NULL;
    return cfg->parent_up_ports[responder_rank][subchannel_id];
}
const char *ArborRouterNodeParentDownPort(const ArborRouterNodeConfig *cfg, int responder_rank, uint32_t subchannel_id) {
    if (!cfg || responder_rank < 0 || responder_rank >= MAX_GROUP_SIZE || subchannel_id >= SUBCHANNEL_COUNT || !cfg->tree_valid[responder_rank][subchannel_id]) return NULL;
    return cfg->parent_down_ports[responder_rank][subchannel_id];
}
int ArborRouterNodeTreeIsLevel(const ArborRouterNodeConfig *cfg, int responder_rank, uint32_t subchannel_id) {
    if (!cfg || responder_rank < 0 || responder_rank >= MAX_GROUP_SIZE || subchannel_id >= SUBCHANNEL_COUNT || !cfg->tree_valid[responder_rank][subchannel_id]) return 0;
    return cfg->tree_role[responder_rank][subchannel_id] == ARBOR_TREE_LEVEL;
}
int ArborRouterNodeMcastCount(const ArborRouterNodeConfig *cfg, int responder_rank, uint32_t subchannel_id) {
    if (!cfg || responder_rank < 0 || responder_rank >= MAX_GROUP_SIZE || subchannel_id >= SUBCHANNEL_COUNT) return 0;
    return cfg->mcast_count[responder_rank][subchannel_id];
}
const char *ArborRouterNodeMcastPort(const ArborRouterNodeConfig *cfg, int responder_rank, uint32_t subchannel_id, int index) {
    if (!cfg || responder_rank < 0 || responder_rank >= MAX_GROUP_SIZE || subchannel_id >= SUBCHANNEL_COUNT || index < 0 || index >= cfg->mcast_count[responder_rank][subchannel_id]) return NULL;
    return cfg->mcast_ports[responder_rank][subchannel_id][index];
}
