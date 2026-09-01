#include "protocol/allreduce_workload.h"

#include "protocol_api.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MSG_SIZE (16 * PAYLOAD_LEN)

typedef struct {
    uint32_t channel_id;
    int rank;
    uint8_t *src_slice;
    uint8_t *dst_slice;
    uint32_t slice_bytes;
} allreduce_task_t;

static int init_host_channel_for_allreduce(const lab_config_t *config, int rank,
                                           uint32_t channel_id) {
    return init_channel(channel_id,
                        lab_config_ip_of_rank(config, rank),
                        lab_config_ip_of_rank(config, (int)channel_id));
}

static void dirname_copy(const char *path, char *out, size_t out_sz) {
    const char *slash;
    size_t len;

    if (!out || out_sz == 0) return;
    if (!path || !path[0]) {
        snprintf(out, out_sz, ".");
        return;
    }
    slash = strrchr(path, '/');
    if (!slash) {
        snprintf(out, out_sz, ".");
        return;
    }
    len = (size_t)(slash - path);
    if (len == 0) len = 1;
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, path, len);
    out[len] = '\0';
}

static void tests_root_from_config(const char *config_path, char *out, size_t out_sz) {
    char config_dir[512];

    dirname_copy(config_path, config_dir, sizeof(config_dir));
    dirname_copy(config_dir, out, out_sz);
}

static void read_input(const char *config_path, int rank, int32_t *buf, uint32_t maxints) {
    char tests_root[512];
    char fn[768];
    FILE *fp;
    uint32_t c = 0;
    long v;

    tests_root_from_config(config_path, tests_root, sizeof(tests_root));
    snprintf(fn, sizeof(fn), "%s/data/current/input-%d.data", tests_root, rank);
    fp = fopen(fn, "r");
    if (!fp) {
        perror(fn);
        exit(1);
    }
    while (c < maxints && fscanf(fp, "%ld", &v) == 1) buf[c++] = (int32_t)v;
    for (; c < maxints; ++c) buf[c] = 0;
    fclose(fp);
}

static void write_output(const char *config_path, int rank, const int32_t *buf, uint32_t nints) {
    char tests_root[512];
    char fn[768];
    FILE *fp;
    uint32_t i;

    tests_root_from_config(config_path, tests_root, sizeof(tests_root));
    snprintf(fn, sizeof(fn), "/app/tests/out/output-%d.data", rank);
    fp = fopen(fn, "w");
    if (!fp) {
        perror(fn);
        return;
    }
    for (i = 0; i < nints; ++i) fprintf(fp, "%d\n", buf[i]);
    fclose(fp);
}

static void *allreduce_worker(void *arg) {
    allreduce_task_t *task = (allreduce_task_t *)arg;
    register_local_source(task->channel_id, task->src_slice, task->slice_bytes, OP_ALLREDUCE);
    register_request_result(task->channel_id, task->dst_slice, task->slice_bytes);

    if (task->rank == (int)task->channel_id) {
        fprintf(stderr, "[host] rank%d acts as responder for channel %u (%u bytes)\n",
                task->rank, task->channel_id, task->slice_bytes);
        respond(task->channel_id, task->dst_slice, task->slice_bytes, OP_ALLREDUCE);
    } else {
        fprintf(stderr, "[host] rank%d acts as requester for channel %u -> responder rank %u (%u bytes)\n",
                task->rank, task->channel_id, task->channel_id, task->slice_bytes);
        request(task->channel_id, task->src_slice, task->slice_bytes, OP_ALLREDUCE);
    }

    clear_request_result(task->channel_id);
    clear_local_source(task->channel_id);
    return NULL;
}

int lab_run_host_allreduce(const lab_config_t *config, const char *host_name,
                           const char *config_path) {
    int rank;
    uint32_t nints;
    uint32_t total_npkts;
    int32_t *src;
    int32_t *dst;
    pthread_t tids[MAX_GROUP_SIZE] = {0};
    allreduce_task_t tasks[MAX_GROUP_SIZE];
    uint32_t base_npkts;
    uint32_t rem_npkts;
    uint32_t pkt_cursor;
    uint32_t channel_id;

    if (!config || !host_name || !config_path) return 1;
    init_host((config_entry_t *)config->entries, config->count, host_name);
    rank = lab_config_rank_of_name(config, host_name);
    if (rank < 0) {
        fprintf(stderr, "unknown host %s\n", host_name);
        return 1;
    }

    nints = MSG_SIZE / sizeof(int32_t);
    total_npkts = MSG_SIZE / PAYLOAD_LEN;
    src = malloc(MSG_SIZE);
    dst = calloc(1, MSG_SIZE);
    if (!src || !dst) {
        fprintf(stderr, "alloc failed\n");
        free(src);
        free(dst);
        return 1;
    }
    read_input(config_path, rank, src, nints);

    if (MSG_SIZE % PAYLOAD_LEN != 0) {
        fprintf(stderr, "MSG_SIZE=%u is not packet aligned to PAYLOAD_LEN=%u\n", MSG_SIZE, PAYLOAD_LEN);
        free(src);
        free(dst);
        return 1;
    }

    for (channel_id = 0; channel_id < (uint32_t)config->count; ++channel_id) {
        if (init_host_channel_for_allreduce(config, rank, channel_id) < 0) {
            fprintf(stderr, "init_channel(%u) failed\n", channel_id);
            free(src);
            free(dst);
            return 1;
        }
    }
    start_host_rx();

    base_npkts = total_npkts / (uint32_t)config->count;
    rem_npkts = total_npkts % (uint32_t)config->count;
    pkt_cursor = 0;
    for (channel_id = 0; channel_id < (uint32_t)config->count; ++channel_id) {
        uint32_t slice_npkts = base_npkts + (channel_id < rem_npkts ? 1u : 0u);
        uint32_t slice_bytes = slice_npkts * PAYLOAD_LEN;
        tasks[channel_id].channel_id = channel_id;
        tasks[channel_id].rank = rank;
        tasks[channel_id].src_slice = (uint8_t *)src + pkt_cursor * PAYLOAD_LEN;
        tasks[channel_id].dst_slice = (uint8_t *)dst + pkt_cursor * PAYLOAD_LEN;
        tasks[channel_id].slice_bytes = slice_bytes;
        pkt_cursor += slice_npkts;
        pthread_create(&tids[channel_id], NULL, allreduce_worker, &tasks[channel_id]);
    }

    for (channel_id = 0; channel_id < (uint32_t)config->count; ++channel_id) {
        pthread_join(tids[channel_id], NULL);
    }

    write_output(config_path, rank, dst, nints);
    printf("[host] rank%d allreduce done (%u channels, %u packets total, concurrent)\n",
           rank, config->count, total_npkts);
    fflush(stdout);

    free(src);
    free(dst);
    return 0;
}
