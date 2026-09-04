#include "protocol/host_priv.h"
#include "arbor_fabric.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CREDITQ_SIZE 4096
#define REGISTER_MAX_ATTEMPTS (1u << 20)
#define OFFSET_RING_SIZE (2u * WINDOW)
#define ARBOR_TEST_LINE_RATE_BPS 100000000000ULL

static pthread_mutex_t g_register_schedule_locks[MAX_CHANNELS];
static pthread_once_t g_register_schedule_once = PTHREAD_ONCE_INIT;
static uint64_t g_register_next_at[MAX_CHANNELS] = {0};

static void init_register_schedule_state(void) {
    for (uint32_t i = 0; i < MAX_CHANNELS; ++i) {
        pthread_mutex_init(&g_register_schedule_locks[i], NULL);
    }
}

typedef struct {
    uint8_t occupied;
    uint8_t normal_credit_pending;
    uint8_t request_sent;
    uint8_t delivered;
    uint32_t offset;
} requester_offset_state_t;

typedef struct {
    credit entries[SUBCHANNEL_COUNT][CREDITQ_SIZE];
    int head[SUBCHANNEL_COUNT];
    int tail[SUBCHANNEL_COUNT];
    uint32_t rr_cursor;
} requester_credit_queue_t;

typedef struct {
    uint64_t register_sent;
    uint64_t register_ack_recv;
    uint64_t request_sent;
    uint64_t repair_sent;
    uint64_t credit_recv;
    uint64_t response_recv;
    uint64_t payload_recv;
    uint64_t repair_trigger_recv;
    uint64_t protocol_violations;
} requester_stats_t;

static uint64_t requester_register_gap_us(uint32_t channel_id) {
    uint32_t total_requesters = (uint32_t)count_bits32(neighbor_mask_of(channel_id));
    uint64_t interval_ns;
    if (total_requesters < 2) total_requesters = 2;
    interval_ns = ((uint64_t)total_requesters * (uint64_t)PAYLOAD_LEN * 8ULL * 1000000000ULL) /
                  ARBOR_TEST_LINE_RATE_BPS;
    if (interval_ns < 100000000ULL) interval_ns = 100000000ULL;
    return (interval_ns + 999ULL) / 1000ULL;
}

static int requester_sequence_before(uint32_t lhs, uint32_t rhs) {
    const uint32_t dist = arbor_protocol_sequence_distance(rhs, lhs);
    return dist != 0 && dist < (ARBOR_SEQUENCE_MASK >> 1);
}

static void note_protocol_violation(requester_stats_t *stats, uint32_t subchannel_id,
                                    const char *reason, uint32_t channel_id,
                                    uint32_t seq, int src_rank) {
    if (stats && subchannel_id < SUBCHANNEL_COUNT) {
        stats[subchannel_id].protocol_violations++;
    }
    fprintf(stderr,
            "[requester-protocol-violation] ch=%u sub=%u reason=%s seq=%u src_rank=%d\n",
            channel_id, subchannel_id, reason ? reason : "?", seq, src_rank);
}

static void release_request_message(protocol_message_t *msg) {
    if (!msg) return;
    msg->in_use = 0;
    msg->error = 0;
    msg->register_acked_mask = 0;
    msg->register_failed_mask = 0;
    msg->end_ack_mask = 0;
    msg->end_pending = 0;
    msg->reuse_ready = 0;
    msg->end_ack_epoch = 0;
    msg->end_ack_epoch_valid = 0;
    msg->sequence_reserved = 0;
    memset(msg->register_attempts, 0, sizeof(msg->register_attempts));
}

static requester_offset_state_t *offset_state_for(requester_offset_state_t *ring, uint32_t offset) {
    return &ring[offset & (OFFSET_RING_SIZE - 1u)];
}

static void offset_state_init(requester_offset_state_t *st, uint32_t offset) {
    if (!st) return;
    st->occupied = 1;
    st->normal_credit_pending = 0;
    st->request_sent = 0;
    st->delivered = 0;
    st->offset = offset;
}

static void enqueue_credit_local(requester_credit_queue_t *q, uint32_t credit_offset,
                                 uint8_t agg_depth, const uint32_t *agg_stack,
                                 const uint8_t *fanin_vec, uint32_t subchannel_id,
                                 uint32_t channel_id, uint8_t repair, uint8_t ecn_ce) {
    if (!q || subchannel_id >= SUBCHANNEL_COUNT) return;
    const int next_head = (q->head[subchannel_id] + 1) % CREDITQ_SIZE;
    if (next_head == q->tail[subchannel_id]) return;
    credit *e = &q->entries[subchannel_id][q->head[subchannel_id]];
    e->credit_offset = credit_offset;
    e->agg_depth = repair ? 0 : agg_depth;
    e->repair = repair ? 1 : 0;
    e->ecn_ce = ecn_ce ? 1 : 0;
    for (int i = 0; i < ARBOR_MAX_STACK_DEPTH; i++) {
        e->agg_stack[i] = (repair || !agg_stack) ? 0 : agg_stack[i];
        e->fanin[i] = (repair || !fanin_vec) ? 0 : fanin_vec[i];
    }
    e->channel_id = channel_id;
    e->subchannel_id = subchannel_id;
    q->head[subchannel_id] = next_head;
}

static int dequeue_credit_local(requester_credit_queue_t *q, credit *out) {
    if (!q || !out) return 0;
    for (uint32_t i = 0; i < SUBCHANNEL_COUNT; i++) {
        const uint32_t subchannel_id = (q->rr_cursor + i) % SUBCHANNEL_COUNT;
        if (q->tail[subchannel_id] == q->head[subchannel_id]) continue;
        *out = q->entries[subchannel_id][q->tail[subchannel_id]];
        q->tail[subchannel_id] = (q->tail[subchannel_id] + 1) % CREDITQ_SIZE;
        q->rr_cursor = (subchannel_id + 1) % SUBCHANNEL_COUNT;
        return 1;
    }
    return 0;
}

static int all_end_seen(const uint8_t *end_seen) {
    for (uint32_t s = 0; s < SUBCHANNEL_COUNT; s++) {
        if (!end_seen[s]) return 0;
    }
    return 1;
}

static void dump_requester_stats(uint32_t channel_id, const requester_stats_t *stats) {
    for (uint32_t s = 0; s < SUBCHANNEL_COUNT; s++) {
        fprintf(stderr,
                "[requester-summary] ch=%u sub=%u register_sent=%llu register_ack_recv=%llu request_sent=%llu repair_sent=%llu credit_recv=%llu response_recv=%llu payload_recv=%llu repair_trigger_recv=%llu protocol_violations=%llu\n",
                channel_id, s,
                (unsigned long long)stats[s].register_sent,
                (unsigned long long)stats[s].register_ack_recv,
                (unsigned long long)stats[s].request_sent,
                (unsigned long long)stats[s].repair_sent,
                (unsigned long long)stats[s].credit_recv,
                (unsigned long long)stats[s].response_recv,
                (unsigned long long)stats[s].payload_recv,
                (unsigned long long)stats[s].repair_trigger_recv,
                (unsigned long long)stats[s].protocol_violations);
    }
}

static void send_register_frame(subchannel_ctx_t *sc, uint32_t src_ip, uint32_t dst_ip,
                                uint8_t message_id, uint32_t epoch, requester_stats_t *stats) {
    static const uint32_t zero_stack[ARBOR_MAX_STACK_DEPTH] = {0};
    static const uint8_t zero_fanin[ARBOR_MAX_STACK_DEPTH] = {0};
    uint8_t frame[HDR_LEN];
    int len = build_frame_ex(frame, src_ip, dst_ip,
                             ARBOR_MSG_REGISTER, (uint8_t)(ARBOR_FLAG_VALID | 0x1U),
                             sc->channel_id, sc->subchannel_id,
                             0, epoch, 0, zero_stack, zero_fanin,
                             ARBOR_REQ_NONE, NULL, 0);
    arbor_store_message_id(frame + sizeof(eth_header_t) + sizeof(ip_header_t) + sizeof(udp_header_t), message_id);
    fprintf(stderr, "[register-tx] ch=%u sub=%u dst_rank=%d msg=%u epoch=%u\n",
            sc->channel_id, sc->subchannel_id, rank_of_ip(dst_ip), message_id, epoch);
    stats[sc->subchannel_id].register_sent++;
    host_inject_on_subchannel(sc->subchannel_id, frame, len);
}

static void send_request_frame(subchannel_ctx_t *sc, uint32_t src_ip, uint32_t dst_ip,
                               uint8_t message_id, uint32_t start_sequence,
                               const credit *c, const void *payload, uint16_t plen,
                               requester_stats_t *stats) {
    uint8_t frame[HDR_LEN + PAYLOAD_LEN];
    const uint8_t msg_type = c->repair ? ARBOR_MSG_REPAIR_REQUEST : ARBOR_MSG_REQUEST;
    const uint8_t flags = (uint8_t)(ARBOR_FLAG_VALID | (c->repair ? 0x1U : 0x0U));
    uint8_t local_fanin_vec[ARBOR_MAX_STACK_DEPTH] = {0};
    uint32_t local_agg_stack[ARBOR_MAX_STACK_DEPTH] = {0};
    uint8_t agg_depth = 0;
    const uint32_t *agg_stack = NULL;
    const uint8_t *fanin = NULL;

    if (!c->repair) {
        const int requester_rank = rank_of_ip(src_ip);
        const int responder_rank = rank_of_ip(dst_ip);
        ArborRankPlan plan;
        memset(&plan, 0, sizeof(plan));
        agg_depth = c->agg_depth;
        for (uint8_t i = 0; i < ARBOR_MAX_STACK_DEPTH; ++i) {
            local_agg_stack[i] = c->agg_stack[i];
            local_fanin_vec[i] = c->fanin[i];
        }
        agg_stack = local_agg_stack;
        fanin = local_fanin_vec;
        if (requester_rank >= 0 && responder_rank >= 0) {
            if (!ArborFabricGetRequesterPlan(requester_rank, responder_rank, c->subchannel_id, &plan)) {
                fprintf(stderr,
                        "[request-config-miss] ch=%u sub=%u requester=%d responder=%d\n",
                        c->channel_id, c->subchannel_id, requester_rank, responder_rank);
                return;
            }
            if (plan.stack_depth != c->agg_depth) {
                fprintf(stderr,
                        "[request-config-depth-mismatch] ch=%u sub=%u requester=%d responder=%d credit_depth=%u cfg_depth=%u\n",
                        c->channel_id, c->subchannel_id, requester_rank, responder_rank,
                        c->agg_depth, plan.stack_depth);
                return;
            }
            agg_depth = plan.stack_depth;
            for (uint8_t i = 0; i < agg_depth && i < ARBOR_MAX_STACK_DEPTH; ++i) {
                local_fanin_vec[i] = plan.fanin[i];
            }
        }
    }

    const uint32_t wire_offset = arbor_protocol_sequence_add(start_sequence, c->credit_offset);
    int len = build_frame_ex(frame, src_ip, dst_ip,
                             msg_type, flags,
                             sc->channel_id, sc->subchannel_id,
                             0, wire_offset,
                             agg_depth, agg_stack, fanin,
                             ARBOR_REQ_AGGREGATE_PAYLOAD, payload, plen);
    if (c->ecn_ce) {
        ip_header_t *ip = (ip_header_t *)(frame + sizeof(eth_header_t));
        ip->tos = (uint8_t)((ip->tos & ~ARBOR_IPV4_ECN_MASK) | ARBOR_IPV4_ECN_CE);
    }
    arbor_store_message_id(frame + sizeof(eth_header_t) + sizeof(ip_header_t) + sizeof(udp_header_t), message_id);
    if (c->repair) { stats[sc->subchannel_id].repair_sent++; fprintf(stderr, "[repair-request-tx] ch=%u sub=%u offset=%u msg=%u\n", c->channel_id, c->subchannel_id, c->credit_offset, (unsigned)message_id); }
    else stats[sc->subchannel_id].request_sent++;
    host_inject_on_subchannel(sc->subchannel_id, frame, len);
}

static void send_end_ack(uint32_t src_ip, uint32_t dst_ip, uint32_t channel_id,
                         uint32_t subchannel_id, uint8_t message_id, uint32_t epoch) {
    static const uint32_t zero_stack[ARBOR_MAX_STACK_DEPTH] = {0};
    static const uint8_t zero_fanin[ARBOR_MAX_STACK_DEPTH] = {0};
    uint8_t frame[HDR_LEN];
    int len = build_frame_ex(frame, src_ip, dst_ip,
                             ARBOR_MSG_END_ACK, ARBOR_FLAG_VALID,
                             channel_id, subchannel_id,
                             0, epoch, 0, zero_stack, zero_fanin,
                             ARBOR_REQ_NONE, NULL, 0);
    arbor_store_message_id(frame + sizeof(eth_header_t) + sizeof(ip_header_t) + sizeof(udp_header_t), message_id);
    host_inject_on_subchannel(subchannel_id, frame, len);
}

static void store_response_payload(uint32_t channel_id, uint32_t local_offset,
                                   const uint8_t *payload, uint16_t payload_len) {
    host_channel_state_t *state = find_channel_state(channel_id);
    if (!state || !state->request_result_buf || payload_len == 0) return;
    if (local_offset >= state->request_result_npkts) return;
    memcpy(state->request_result_buf + local_offset * PAYLOAD_LEN,
           payload, payload_len > PAYLOAD_LEN ? PAYLOAD_LEN : payload_len);
}

int request(uint32_t channel_id, const void *buf, uint32_t size, uint8_t op) {
    pthread_once(&g_register_schedule_once, init_register_schedule_state);
    channel_ctx_t *ctx = find_channel(channel_id);
    host_channel_state_t *state = find_channel_state(channel_id);
    requester_credit_queue_t credit_q = {0};
    requester_offset_state_t offset_ring[OFFSET_RING_SIZE] = {0};
    requester_stats_t stats[SUBCHANNEL_COUNT] = {0};
    const uint64_t register_gap_us = requester_register_gap_us(channel_id);
    if (!ctx || !state) return -1;

    uint32_t total_npkts = size / PAYLOAD_LEN;
    if (total_npkts == 0) return 0;

    const uint8_t *src = (const uint8_t *)buf;
    uint8_t *register_acked = calloc(SUBCHANNEL_COUNT, sizeof(uint8_t));
    uint8_t *completed = calloc(total_npkts, sizeof(uint8_t));
    protocol_message_t *msg = acquire_request_message(channel_id, total_npkts);
    uint8_t fatal_register_failure = 0;
    uint8_t fatal_protocol_violation = 0;
    if (!register_acked || !completed || !msg) {
        free(register_acked);
        free(completed);
        return -1;
    }
    msg->src_buffer = buf;
    msg->dst_buffer = state->request_result_buf;

    uint32_t completed_count = 0;
    uint8_t end_seen[SUBCHANNEL_COUNT] = {0};
    uint8_t end_ack_pending[SUBCHANNEL_COUNT] = {0};
    uint64_t done_since = 0;
    uint64_t last_end_seen_at = 0;
    uint64_t next_progress_log = 0;
    (void)op;

    while (1) {
        credit c;
        uint8_t progressed = 0;

        conn_t *cn = &g_conns[ctx->uplink_conn];
        rx_msg_t m;
        while (conn_pop(cn, &m)) {
            if (m.channel_id != channel_id) continue;

            if (m.msg_type == ARBOR_MSG_REGISTER_ACK) {
                fprintf(stderr,
                        "[register-ack-rx] ch=%u sub=%u msg=%u expect_msg=%u epoch=%u expect_epoch=%u src_rank=%d\n",
                        channel_id, m.subchannel_id, m.message_id, msg->message_id,
                        m.payload_offset, msg->epoch, rank_of_ip(m.src_ip));
                if (m.subchannel_id < SUBCHANNEL_COUNT &&
                    m.message_id == msg->message_id &&
                    m.payload_offset == msg->epoch) {
                    register_acked[m.subchannel_id] = 1;
                    msg->register_acked_mask |= (uint8_t)(1u << m.subchannel_id);
                    stats[m.subchannel_id].register_ack_recv++;
                    progressed = 1;
                }
                continue;
            }

            if (m.msg_type == ARBOR_MSG_RESPONSE) {
                uint32_t local_payload_offset = 0;
                uint32_t local_credit_offset = 0;
                protocol_message_t *credit_meta = NULL;
                protocol_message_t *payload_meta = NULL;
                const uint8_t completion_valid = (m.flags & ARBOR_FLAG_COMPLETION_VALID) != 0;
                const uint8_t credit_valid = (m.flags & ARBOR_FLAG_CREDIT_VALID) != 0;
                const uint8_t repair_valid = m.repair != 0;
                fprintf(stderr,
                        "[response-rx] ch=%u sub=%u msg=%u credit_valid=%u repair=%u completion_valid=%u credit_off=%u payload_off=%u src_rank=%d\n",
                        channel_id, m.subchannel_id, m.message_id,
                        (unsigned)credit_valid, (unsigned)repair_valid, (unsigned)completion_valid,
                        m.credit_offset, m.payload_offset, rank_of_ip(m.src_ip));
                stats[m.subchannel_id].response_recv++;
                if (completion_valid && m.payload_len > 0) {
                    payload_meta = find_request_message_for_sequence(channel_id, m.payload_offset, &local_payload_offset);
                    if (payload_meta != msg && !requester_sequence_before(m.payload_offset, state->request_next_sequence)) {
                        note_protocol_violation(stats, m.subchannel_id, "future-payload", channel_id,
                                                m.payload_offset, rank_of_ip(m.src_ip));
                        fatal_protocol_violation = 1;
                    }
                }
                if (completion_valid && m.payload_len > 0 && payload_meta == msg) {
                    stats[m.subchannel_id].payload_recv++;
                    store_response_payload(channel_id, local_payload_offset, m.payload, m.payload_len);
                    if (!completed[local_payload_offset]) {
                        completed[local_payload_offset] = 1;
                        completed_count++;
                    }
                    progressed = 1;
                }
                if (credit_valid && m.subchannel_id < SUBCHANNEL_COUNT) {
                    credit_meta = find_request_message_for_sequence(channel_id, m.credit_offset, &local_credit_offset);
                    if (credit_meta != msg && !requester_sequence_before(m.credit_offset, state->request_next_sequence)) {
                        note_protocol_violation(stats, m.subchannel_id, "future-credit", channel_id,
                                                m.credit_offset, rank_of_ip(m.src_ip));
                        fatal_protocol_violation = 1;
                    }
                }
                if (credit_valid && m.subchannel_id < SUBCHANNEL_COUNT && credit_meta == msg) {
                    /* Arbor behavior: the first credit, including repair, confirms readiness. */
                    register_acked[m.subchannel_id] = 1;
                    msg->register_acked_mask |= (uint8_t)(1u << m.subchannel_id);
                    requester_offset_state_t *st = offset_state_for(offset_ring, local_credit_offset);
                    if (completed[local_credit_offset]) {
                        continue;
                    }
                    if (!repair_valid && st->occupied && st->offset == local_credit_offset &&
                        (st->normal_credit_pending || st->request_sent)) {
                        continue;
                    }
                    uint32_t agg_stack[ARBOR_MAX_STACK_DEPTH] = {0};
                    uint8_t fanin_vec[ARBOR_MAX_STACK_DEPTH] = {0};
                    for (int i = 0; i < ARBOR_MAX_STACK_DEPTH; i++) {
                        agg_stack[i] = m.agg_stack[i];
                        fanin_vec[i] = m.fanin[i];
                    }
                    offset_state_init(st, local_credit_offset);
                    fprintf(stderr,
                            "[credit-enqueue] ch=%u sub=%u msg=%u wire_credit=%u local_credit=%u agg_depth=%u repair=%u src_rank=%d\n",
                            channel_id, m.subchannel_id, m.message_id,
                            m.credit_offset, local_credit_offset, m.agg_depth,
                            (unsigned)repair_valid, rank_of_ip(m.src_ip));
                    stats[m.subchannel_id].credit_recv++;
                    if (repair_valid) {
                        stats[m.subchannel_id].repair_trigger_recv++;
                        fprintf(stderr, "[repair-trigger-rx] ch=%u sub=%u msg=%u offset=%u src_rank=%d\n", channel_id, m.subchannel_id, (unsigned)m.message_id, m.credit_offset, rank_of_ip(m.src_ip));
                    }
                    enqueue_credit_local(&credit_q, local_credit_offset,
                                         repair_valid ? 0 : m.agg_depth,
                                         repair_valid ? NULL : agg_stack,
                                         repair_valid ? NULL : fanin_vec,
                                         m.subchannel_id, m.channel_id,
                                         repair_valid ? 1 : 0,
                                         ((m.flags & ARBOR_FLAG_ECN) != 0) ? 1 : 0);
                    st->normal_credit_pending = repair_valid ? 0 : 1;
                    progressed = 1;
                }
                continue;
            }

            if (m.msg_type == ARBOR_MSG_END) {
                if (m.subchannel_id < SUBCHANNEL_COUNT &&
                    m.message_id == msg->message_id &&
                    m.payload_offset == msg->epoch) {
                    end_seen[m.subchannel_id] = 1;
                    end_ack_pending[m.subchannel_id] = 1;
                    msg->end_pending = 1;
                    last_end_seen_at = now_us();
                } else if (state->request_end_tombstone_valid[m.message_id] &&
                           state->request_end_tombstone_seq[m.message_id] == m.payload_offset) {
                    send_end_ack(ctx->local_ip, ctx->responder_ip, channel_id, m.subchannel_id,
                                 m.message_id, m.payload_offset);
                    progressed = 1;
                }
                progressed = 1;
            }
        }

        uint64_t now = now_us();
        if (now >= next_progress_log) {
            fprintf(stderr, "[requester-progress] ch=%u msg=%u reg_mask=0x%x failed=0x%x completed=%u/%u end_seen=0x%x end_ack=0x%x\n",
                    channel_id, msg->message_id, msg->register_acked_mask,
                    msg->register_failed_mask, completed_count, total_npkts,
#if SUBCHANNEL_COUNT > 1
                    end_seen[0] | (uint8_t)(end_seen[1] << 1),
#else
                    end_seen[0],
#endif
                    msg->end_ack_mask);
            next_progress_log = now + 1000000ULL;
        }
        for (uint32_t scan = 0; scan < SUBCHANNEL_COUNT; ++scan) {
            uint32_t sidx = (uint32_t)((msg->next_credit_subchannel + scan) % SUBCHANNEL_COUNT);
            if (register_acked[sidx]) continue;
            if (msg->register_attempts[sidx] >= REGISTER_MAX_ATTEMPTS) {
                msg->register_failed_mask |= (uint8_t)(1u << sidx);
                fatal_register_failure = 1;
                continue;
            }
            pthread_mutex_t *schedule_lock = &g_register_schedule_locks[channel_id];
            pthread_mutex_lock(schedule_lock);
            if (now < g_register_next_at[channel_id]) {
                pthread_mutex_unlock(schedule_lock);
                continue;
            }
            subchannel_ctx_t *sc = find_subchannel(channel_id, sidx);
            if (sc) {
                send_register_frame(sc, ctx->local_ip, ctx->responder_ip, msg->message_id, msg->epoch, stats);
                msg->register_attempts[sidx]++;
                g_register_next_at[channel_id] = now + register_gap_us;
                msg->next_credit_subchannel = (uint8_t)((sidx + 1u) % SUBCHANNEL_COUNT);
                progressed = 1;
            }
            pthread_mutex_unlock(schedule_lock);
            break;
        }

        while (dequeue_credit_local(&credit_q, &c)) {
            subchannel_ctx_t *sc = find_subchannel(c.channel_id, c.subchannel_id);
            if (sc && c.credit_offset < total_npkts) {
                requester_offset_state_t *st = offset_state_for(offset_ring, c.credit_offset);
                fprintf(stderr,
                        "[request-tx] ch=%u sub=%u msg=%u local_credit=%u start_seq=%u dst_rank=%d repair=%u agg_depth=%u\n",
                        c.channel_id, c.subchannel_id, msg->message_id, c.credit_offset,
                        msg->start_sequence, rank_of_ip(ctx->responder_ip), (unsigned)c.repair,
                        c.agg_depth);
                send_request_frame(sc, ctx->local_ip, ctx->responder_ip, msg->message_id,
                                   msg->start_sequence, &c,
                                   src + c.credit_offset * PAYLOAD_LEN,
                                   PAYLOAD_LEN, stats);
                if (st && st->occupied && st->offset == c.credit_offset && !c.repair) {
                    st->request_sent = 1;
                    st->normal_credit_pending = 0;
                }
                progressed = 1;
            }
        }

        if (completed_count >= total_npkts) {
            if (done_since == 0) done_since = now_us();
            for (uint32_t sidx = 0; sidx < SUBCHANNEL_COUNT; sidx++) {
                if (end_ack_pending[sidx]) {
                    send_end_ack(ctx->local_ip, ctx->responder_ip, channel_id, sidx, msg->message_id, msg->epoch);
                    end_ack_pending[sidx] = 0;
                    msg->end_ack_mask |= (uint8_t)(1u << sidx);
                    msg->end_ack_epoch = msg->epoch;
                    msg->end_ack_epoch_valid = 1;
                    msg->sequence_reserved = 0;
                    progressed = 1;
                }
            }
            if (all_end_seen(end_seen) && !progressed) {
                uint64_t now_done = now_us();
                if (last_end_seen_at != 0 &&
                    now_done - done_since >= (2 * RTO_US) &&
                    now_done - last_end_seen_at >= (3 * RTO_US) &&
                    msg->end_ack_mask == ((1u << SUBCHANNEL_COUNT) - 1u)) {
                    state->request_end_tombstone_seq[msg->message_id] = msg->epoch;
                    state->request_end_tombstone_valid[msg->message_id] = 1;
                    msg->end_pending = 0;
                    msg->reuse_ready = 1;
                    break;
                }
            }
        } else {
            done_since = 0;
        }

        if (fatal_register_failure || fatal_protocol_violation) {
            break;
        }

        if (!progressed) usleep(200);
    }

    dump_requester_stats(channel_id, stats);
    release_request_message(msg);
    if (fatal_register_failure || fatal_protocol_violation) {
        (void)rollback_protocol_sequence(channel_id, msg->start_sequence, total_npkts);
    }
    free(register_acked);
    free(completed);
    return (fatal_register_failure || fatal_protocol_violation) ? -1 : (int)size;
}
