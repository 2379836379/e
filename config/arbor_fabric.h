#ifndef TEST_LAB_ARBOR_FABRIC_H
#define TEST_LAB_ARBOR_FABRIC_H

#include "runtime/runtime_common.h"

typedef enum {
    ARBOR_TREE_LEVEL = 0,
    ARBOR_TREE_RELAY = 1,
} ArborTreeRole;

typedef struct ArborRouterNodeConfig ArborRouterNodeConfig;

typedef struct {
    uint32_t requester_rank;
    uint8_t ascend_len;
    char ascend_routers[ARBOR_MAX_STACK_DEPTH][32];
} ArborAscendPath;

typedef struct {
    char root_router[32];
    uint8_t responder_leg_len;
    char responder_leg[ARBOR_MAX_STACK_DEPTH][32];
    uint8_t level_count;
    char level_routers[ARBOR_MAX_STACK_DEPTH][32];
    uint8_t requester_count;
    ArborAscendPath requesters[MAX_GROUP_SIZE];
} ArborTreeSpec;

typedef struct {
    uint32_t group_id;
    uint32_t channel_id;
    uint32_t responder_rank;
    uint32_t num_requesters;
    uint32_t requesters[MAX_GROUP_SIZE];
    uint32_t num_subchannels;
} ArborChannelSpec;

typedef struct {
    uint8_t stack_depth;
    uint8_t fanin[ARBOR_MAX_STACK_DEPTH];
    char stack_routers[ARBOR_MAX_STACK_DEPTH][32];
} ArborRankPlan;

typedef struct {
    int rank_count;
    config_entry_t ranks[MAX_GROUP_SIZE];
    uint32_t channel_count;
    ArborChannelSpec channels[MAX_CHANNELS];
    ArborTreeSpec tree_specs[MAX_CHANNELS][SUBCHANNEL_COUNT];
} ArborFabric;

int ArborFabricLoadFromFiles(const char *ranks_path, const char *tree_path);
const ArborFabric *ArborFabricGet(void);
const ArborRouterNodeConfig *ArborFabricFindRouter(const char *name);
const ArborChannelSpec *ArborFabricGetChannelSpec(uint32_t channel_id);
const ArborTreeSpec *ArborFabricGetTreeSpec(uint32_t channel_id, uint32_t subchannel_id);
int ArborFabricGetRequesterPlan(int requester_rank, int responder_rank, uint32_t subchannel_id,
                                ArborRankPlan *plan_out);

const char *ArborRouterNodeName(const ArborRouterNodeConfig *cfg);
int ArborRouterNodeDevCount(const ArborRouterNodeConfig *cfg);
const char *ArborRouterNodeDevName(const ArborRouterNodeConfig *cfg, int index);
const char *ArborRouterNodeRoutePort(const ArborRouterNodeConfig *cfg, int responder_rank, uint32_t subchannel_id);
const char *ArborRouterNodeParentUpPort(const ArborRouterNodeConfig *cfg, int responder_rank, uint32_t subchannel_id);
const char *ArborRouterNodeParentDownPort(const ArborRouterNodeConfig *cfg, int responder_rank, uint32_t subchannel_id);
int ArborRouterNodeTreeIsLevel(const ArborRouterNodeConfig *cfg, int responder_rank, uint32_t subchannel_id);
int ArborRouterNodeMcastCount(const ArborRouterNodeConfig *cfg, int responder_rank, uint32_t subchannel_id);
const char *ArborRouterNodeMcastPort(const ArborRouterNodeConfig *cfg, int responder_rank, uint32_t subchannel_id, int index);

#endif
