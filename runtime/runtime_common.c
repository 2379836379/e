#include "runtime/runtime_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static int g_n = 0;
static config_entry_t g_cfg[MAX_GROUP_SIZE];
static uint32_t g_neighbor_masks[MAX_GROUP_SIZE];

uint64_t now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + tv.tv_usec;
}

static uint32_t default_neighbor_mask(int rank) {
    uint32_t mask = 0;
    for (int r = 0; r < g_n; r++)
        if (r != rank) mask |= (1u << r);
    return mask;
}

int count_bits32(uint32_t x) {
    int n = 0;
    while (x) {
        x &= (x - 1);
        n++;
    }
    return n;
}

int rank_of_ip(uint32_t ip) {
    for (int i = 0; i < g_n; i++)
        if (g_cfg[i].host_ip == ip) return g_cfg[i].rank;
    return -1;
}

uint32_t neighbor_mask_of(uint32_t vertex_id) {
    if (vertex_id >= (uint32_t)g_n) return 0;
    return g_neighbor_masks[vertex_id];
}

uint8_t arbor_plan_request_fanin_stack(uint32_t requester_rank,
                                       uint32_t responder_rank,
                                       uint8_t *fanin_vec_out,
                                       uint8_t max_depth) {
    uint8_t depth = 0;
    uint8_t fanins[ARBOR_MAX_STACK_DEPTH] = {0};
    const uint32_t total_requesters = (uint32_t)count_bits32(neighbor_mask_of(responder_rank));
    const uint32_t leaf_base = (requester_rank / 2U) * 2U;
    const uint32_t leaf_end = leaf_base + 2U > (uint32_t)g_n ? (uint32_t)g_n : leaf_base + 2U;
    const uint32_t mid_base = (requester_rank / 4U) * 4U;
    const uint32_t mid_end = mid_base + 4U > (uint32_t)g_n ? (uint32_t)g_n : mid_base + 4U;
    uint32_t leaf_requesters = leaf_end - leaf_base;
    uint32_t mid_requesters = mid_end - mid_base;

    if (!fanin_vec_out || max_depth == 0) return 0;
    if (requester_rank >= (uint32_t)g_n || responder_rank >= (uint32_t)g_n) return 0;

    if (responder_rank >= leaf_base && responder_rank < leaf_end && leaf_requesters > 0) {
        leaf_requesters--;
    }
    if (responder_rank >= mid_base && responder_rank < mid_end && mid_requesters > 0) {
        mid_requesters--;
    }
    if (g_n > 4 && depth < ARBOR_MAX_STACK_DEPTH) {
        fanins[depth++] = (uint8_t)(total_requesters > 0 ? total_requesters : 1U);
    }
    if (g_n > 2 && depth < ARBOR_MAX_STACK_DEPTH) {
        fanins[depth++] = (uint8_t)(mid_requesters > 0 ? mid_requesters : 1U);
    }
    if (depth < ARBOR_MAX_STACK_DEPTH) {
        fanins[depth++] = (uint8_t)(leaf_requesters > 0 ? leaf_requesters : 1U);
    }

    if (depth > max_depth) {
        const uint8_t keep = max_depth;
        const uint8_t start = (uint8_t)(depth - keep);
        for (uint8_t i = 0; i < keep; ++i) fanin_vec_out[i] = fanins[start + i];
        for (uint8_t i = keep; i < max_depth; ++i) fanin_vec_out[i] = 0;
        return keep;
    }
    for (uint8_t i = 0; i < depth; ++i) fanin_vec_out[i] = fanins[i];
    for (uint8_t i = depth; i < max_depth; ++i) fanin_vec_out[i] = 0;
    return depth;
}

static void init_default_neighbor_masks(void) {
    for (int r = 0; r < g_n; r++)
        g_neighbor_masks[r] = default_neighbor_mask(r);
}

static void try_load_neighbor_masks(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return;

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char *save = NULL;
        char *tok = strtok_r(line, ", \t\r\n", &save);
        if (!tok) continue;
        int vertex = atoi(tok);
        if (vertex < 0 || vertex >= g_n) continue;

        uint32_t mask = 0;
        while ((tok = strtok_r(NULL, ", \t\r\n", &save)) != NULL) {
            int nbr = atoi(tok);
            if (nbr >= 0 && nbr < g_n && nbr != vertex)
                mask |= (1u << nbr);
        }
        g_neighbor_masks[vertex] = mask;
    }
    fclose(fp);
}

void common_set_group(config_entry_t *cfgs, int n, const char *graph_path) {
    g_n = n;
    memcpy(g_cfg, cfgs, sizeof(config_entry_t) * n);
    init_default_neighbor_masks();
    try_load_neighbor_masks(graph_path);
}
