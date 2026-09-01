#include "protocol/host_priv.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define INVALID_OFFSET 0xffffffffu
#define FIXED_RS_CREDITS_PER_SEC 2000ULL
#define FIXED_STARTUP_WINDOW 4U
#define CC_MIN_WINDOW 1U
#define CC_MAX_WINDOW WINDOW
#define CC_MAX_GAP_US (RTO_US / 2U)
#define REPAIR_BUDGET_BPS 5000000000ULL
#define REPAIR_BURST_BYTES (1024.0 * 1024.0)

static uint64_t fixed_credit_gap_us(void) {
    return FIXED_RS_CREDITS_PER_SEC > 0 ? (1000000ULL / FIXED_RS_CREDITS_PER_SEC) : 0;
}

static uint64_t channel_credit_gap_us(void) {
    return fixed_credit_gap_us();
}

static double refill_repair_tokens(host_channel_state_t *state, uint64_t now_us) {
    double burst = REPAIR_BURST_BYTES;
    if (!state) return 0.0;
    if (state->response_repair_refill_at == 0) {
        state->response_repair_refill_at = now_us;
        if (state->response_repair_tokens < burst) state->response_repair_tokens = burst;
        return state->response_repair_tokens;
    }
    if (now_us > state->response_repair_refill_at) {
        state->response_repair_tokens += (double)(now_us - state->response_repair_refill_at) * (double)REPAIR_BUDGET_BPS / 8e6;
        if (state->response_repair_tokens > burst) state->response_repair_tokens = burst;
        state->response_repair_refill_at = now_us;
    }
    return state->response_repair_tokens;
}

static int charge_repair_tokens(host_channel_state_t *state, uint64_t now_us, double bytes) {
    if (!state) return 0;
    refill_repair_tokens(state, now_us);
    if (state->response_repair_tokens < bytes) return 0;
    state->response_repair_tokens -= bytes;
    return 1;
}

static uint32_t clamp_credit_window(uint32_t window) {
    if (window < CC_MIN_WINDOW) return CC_MIN_WINDOW;
    if (window > CC_MAX_WINDOW) return CC_MAX_WINDOW;
    return window;
}

static uint64_t clamp_credit_gap_us(uint64_t gap_us) {
    const uint64_t base = fixed_credit_gap_us();
    if (gap_us < base) return base;
    if (gap_us > CC_MAX_GAP_US) return CC_MAX_GAP_US;
    return gap_us;
}

typedef enum {
    COMPLETION_KIND_NONE = 0,
    COMPLETION_KIND_NORMAL = 1,
    COMPLETION_KIND_REPAIR = 2,
    COMPLETION_KIND_REPLAY = 3,
    COMPLETION_KIND_DIRECT = 4,
} completion_kind_t;

typedef struct {
    uint32_t channel_id;
    uint32_t subchannel_id;
    uint32_t credit_offset;
    uint64_t rtt_us;
    uint32_t ref_inflight;
    uint64_t ref_credit_gap_us;
    uint32_t ref_window;
    uint8_t completion_kind;
    uint8_t ce;
} responder_rate_sample_t;

typedef struct {
    uint8_t valid;
    uint8_t message_id;
    uint8_t _pad0;
    uint8_t _pad1[2];
    uint32_t channel_id;
    uint32_t subchannel_id;
    uint32_t credit_offset;
    uint32_t payload_offset;
    uint32_t agg_loc;
    uint16_t agg_level;
    uint8_t agg_valid;
    uint16_t fanin;
    uint16_t payload_len;
    uint8_t payload[PAYLOAD_LEN];
} primary_response_t;

typedef struct {
    uint8_t valid;
    uint8_t committed;
    uint8_t inflight_accounted;
    uint8_t credit_sent;
    uint8_t completion_sent;
    uint8_t done;
    uint8_t bound_payload_valid;
    uint8_t retry_count;
    uint8_t tail_response_valid;
    uint8_t tail_response_acked;
    uint32_t tail_successor_credit_offset;
    uint8_t sample_eligible;
    uint8_t sample_recorded;
    uint8_t completion_kind;
    uint8_t ce_seen;
    uint8_t repair_mode;

    uint32_t credit_offset;
    uint32_t local_credit_offset;
    uint8_t owner_message_id;
    uint8_t _pad0[3];
    uint32_t subchannel_id;
    uint32_t ref_inflight;
    uint64_t ref_credit_gap_us;
    uint32_t ref_window;
    uint32_t payload_offset;
    uint32_t agg_loc;
    uint64_t sent_at;
    uint32_t requester_bitmap;
    uint32_t repair_bitmap;
    uint32_t direct_contrib_count;
    uint32_t repair_contrib_count;
    uint32_t bound_payload_offset;
    uint8_t result[PAYLOAD_LEN];
    uint8_t bound_payload[PAYLOAD_LEN];
    primary_response_t primary_response;
    int32_t direct_accum[PAYLOAD_LEN / sizeof(int32_t)];
    int32_t repair_accum[PAYLOAD_LEN / sizeof(int32_t)];
} credit_entry_t;

typedef struct {
    uint64_t register_recv;
    uint64_t register_ack_sent;
    uint64_t credit_sent;
    uint64_t completion_sent;
    uint64_t request_commit;
    uint64_t repair_trigger_sent;
    uint64_t repair_request_recv;
    uint64_t repair_commit;
} responder_stats_t;

static responder_stats_t g_responder_stats[SUBCHANNEL_COUNT];

#define RESPONDER_SENDQ_SIZE 32768

typedef struct {
    uint8_t valid;
    uint32_t subchannel_id;
    uint32_t frame_len;
    uint64_t due_at;
    uint8_t frame[HDR_LEN + PAYLOAD_LEN];
} responder_sendq_entry_t;

static responder_sendq_entry_t g_responder_sendq[RESPONDER_SENDQ_SIZE];
static uint32_t g_responder_sendq_head = 0;
static uint32_t g_responder_sendq_tail = 0;
static pthread_mutex_t g_responder_sendq_lock = PTHREAD_MUTEX_INITIALIZER;

static int enqueue_send_frame(uint32_t subchannel_id, const uint8_t *frame, uint32_t frame_len, uint64_t due_at) {
    uint32_t next;
    responder_sendq_entry_t *e;
    if (subchannel_id >= SUBCHANNEL_COUNT) return 0;
    if (!frame || frame_len == 0 || frame_len > sizeof(g_responder_sendq[0].frame)) return 0;

    pthread_mutex_lock(&g_responder_sendq_lock);
    next = (g_responder_sendq_head + 1) % RESPONDER_SENDQ_SIZE;
    if (next == g_responder_sendq_tail) {
        fprintf(stderr,
                "[responder-sendq-full] ch? sub=%u len=%u head=%u tail=%u due=%llu\n",
                subchannel_id, frame_len, g_responder_sendq_head,
                g_responder_sendq_tail, (unsigned long long)due_at);
        pthread_mutex_unlock(&g_responder_sendq_lock);
        return 0;
    }

    e = &g_responder_sendq[g_responder_sendq_head];
    e->valid = 1;
    e->subchannel_id = subchannel_id;
    e->frame_len = frame_len;
    e->due_at = due_at;
    memcpy(e->frame, frame, frame_len);
    g_responder_sendq_head = next;
    pthread_mutex_unlock(&g_responder_sendq_lock);
    return 1;
}

static void flush_send_queue(void) {
    uint64_t now = now_us();
    pthread_mutex_lock(&g_responder_sendq_lock);
    while (g_responder_sendq_tail != g_responder_sendq_head) {
        responder_sendq_entry_t *e = &g_responder_sendq[g_responder_sendq_tail];
        if (!e->valid) {
            g_responder_sendq_tail = (g_responder_sendq_tail + 1) % RESPONDER_SENDQ_SIZE;
            continue;
        }
        if (e->due_at > now) break;
        if (e->subchannel_id < SUBCHANNEL_COUNT && e->frame_len > 0) {
            host_inject_on_subchannel(e->subchannel_id, e->frame, (int)e->frame_len);
        }
        e->valid = 0;
        e->frame_len = 0;
        e->due_at = 0;
        g_responder_sendq_tail = (g_responder_sendq_tail + 1) % RESPONDER_SENDQ_SIZE;
    }
    pthread_mutex_unlock(&g_responder_sendq_lock);
}

static void dump_responder_stats(uint32_t channel_id) {
    for (uint32_t s = 0; s < SUBCHANNEL_COUNT; s++) {
        fprintf(stderr, "[responder-summary] ch=%u sub=%u register_recv=%llu register_ack_sent=%llu credit_sent=%llu completion_sent=%llu request_commit=%llu repair_trigger_sent=%llu repair_request_recv=%llu repair_commit=%llu\n",
                channel_id, s,
                (unsigned long long)g_responder_stats[s].register_recv,
                (unsigned long long)g_responder_stats[s].register_ack_sent,
                (unsigned long long)g_responder_stats[s].credit_sent,
                (unsigned long long)g_responder_stats[s].completion_sent,
                (unsigned long long)g_responder_stats[s].request_commit,
                (unsigned long long)g_responder_stats[s].repair_trigger_sent,
                (unsigned long long)g_responder_stats[s].repair_request_recv,
                (unsigned long long)g_responder_stats[s].repair_commit);
    }
}

static uint32_t responder_local_to_wire(uint32_t epoch, uint32_t local_offset) {
    return arbor_protocol_sequence_add(epoch, local_offset);
}

static int responder_wire_to_local(uint32_t epoch, uint32_t total_npkts,
                                   uint32_t wire_offset, uint32_t *local_offset_out) {
    if (!local_offset_out) return 0;
    if (!arbor_protocol_sequence_contains(epoch, total_npkts, wire_offset)) return 0;
    *local_offset_out = arbor_protocol_sequence_distance(wire_offset, epoch);
    return 1;
}

static void send_response_frame(uint32_t dst_ip, uint8_t message_id,
                                uint32_t channel_id, uint32_t subchannel_id,
                                uint32_t credit_offset, uint32_t payload_offset, uint32_t agg_loc,
                                uint16_t agg_level, uint8_t agg_valid, uint16_t fanin,
                                const void *payload, uint16_t plen, uint64_t due_at,
                                arbor_payload_kind_t payload_kind, uint8_t repair) {
    static const uint32_t zero_stack[ARBOR_MAX_STACK_DEPTH] = {0};
    static const uint8_t zero_fanin[ARBOR_MAX_STACK_DEPTH] = {0};
    uint8_t frame[HDR_LEN + PAYLOAD_LEN];
    uint8_t flags = ARBOR_FLAG_VALID;
    uint8_t *arbor_hdr;
    arbor_header_view_t hdr;
    if (repair) flags |= 0x1U;
    if (credit_offset != INVALID_OFFSET) flags |= ARBOR_FLAG_CREDIT_VALID;
    if (payload_offset != INVALID_OFFSET) flags |= ARBOR_FLAG_COMPLETION_VALID;
    (void)agg_loc;
    (void)agg_level;
    (void)agg_valid;
    (void)fanin;
    int len = build_frame_ex(frame,
                             g_my_ip, dst_ip,
                             ARBOR_MSG_RESPONSE, flags,
                             channel_id, subchannel_id,
                             credit_offset, payload_offset, 0, zero_stack, zero_fanin,
                             ARBOR_REQ_NONE, payload, plen);
    arbor_hdr = frame + sizeof(eth_header_t) + sizeof(ip_header_t) + sizeof(udp_header_t);
    hdr = arbor_parse_header(arbor_hdr);
    hdr.message_id = message_id;
    hdr.repair = repair ? 1 : 0;
    if (hdr.payload_valid) hdr.payload_kind = payload_kind;
    arbor_write_header(arbor_hdr, &hdr);
    (void)enqueue_send_frame(subchannel_id, frame, (uint32_t)len, due_at);
}

static void broadcast_response(uint8_t message_id, uint32_t channel_id, uint32_t subchannel_id,
                               uint32_t credit_offset, uint32_t payload_offset,
                               uint32_t agg_loc, uint16_t agg_level, uint8_t agg_valid,
                               uint16_t fanin, const void *payload, uint16_t plen, uint64_t due_at,
                               arbor_payload_kind_t payload_kind, uint8_t repair) {
    for (int i = 0; i < g_n; i++) {
        uint32_t dst_ip = g_cfg[i].host_ip;
        if (dst_ip == g_my_ip) continue;
        send_response_frame(dst_ip, message_id, channel_id, subchannel_id,
                            credit_offset, payload_offset, agg_loc,
                            agg_level, agg_valid, fanin, payload, plen, due_at,
                            payload_kind, repair);
    }
}

static void cache_primary_response(credit_entry_t *e, uint8_t message_id, uint32_t channel_id,
                                  uint32_t subchannel_id, uint32_t credit_offset,
                                  uint32_t payload_offset, uint32_t agg_loc,
                                  uint16_t agg_level, uint8_t agg_valid,
                                  uint16_t fanin, const void *payload, uint16_t plen) {
    primary_response_t *pr = &e->primary_response;
    memset(pr, 0, sizeof(*pr));
    pr->valid = 1;
    pr->message_id = message_id;
    pr->channel_id = channel_id;
    pr->subchannel_id = subchannel_id;
    pr->credit_offset = credit_offset;
    pr->payload_offset = payload_offset;
    pr->agg_loc = agg_loc;
    pr->agg_level = agg_level;
    pr->agg_valid = agg_valid;
    pr->fanin = fanin;
    pr->payload_len = plen;
    if (plen > 0 && payload) memcpy(pr->payload, payload, plen);
}

static void emit_primary_response(const credit_entry_t *e, arbor_payload_kind_t payload_kind) {
    const primary_response_t *pr = &e->primary_response;
    if (!pr->valid) return;
    broadcast_response(pr->message_id, pr->channel_id, pr->subchannel_id,
                       pr->credit_offset, pr->payload_offset, pr->agg_loc,
                       pr->agg_level, pr->agg_valid, pr->fanin,
                       pr->payload_len > 0 ? pr->payload : NULL, pr->payload_len, 0,
                       payload_kind, 0);
}

static void replay_primary_response(const credit_entry_t *e) {
    emit_primary_response(e, ARBOR_PAYLOAD_REPLAY);
}

static void send_repair_trigger(uint32_t channel_id, uint32_t subchannel_id,
                                uint32_t credit_offset, uint32_t agg_loc) {
    for (int i = 0; i < g_n; i++) {
        uint32_t dst_ip = g_cfg[i].host_ip;
        if (dst_ip == g_my_ip) continue;
        (void)agg_loc;
        send_response_frame(dst_ip, 0, channel_id, subchannel_id,
                            credit_offset, INVALID_OFFSET, agg_loc,
                            0, 0, 0, NULL, 0, 0,
                            ARBOR_PAYLOAD_COMPLETION, 1);
    }
}

static void send_register_ack_all(uint32_t channel_id, uint32_t subchannel_id, uint8_t message_id, uint32_t epoch) {
    for (int i = 0; i < g_n; i++) {
        uint32_t dst_ip = g_cfg[i].host_ip;
        if (dst_ip == g_my_ip) continue;
        uint8_t frame[HDR_LEN];
        static const uint32_t zero_stack[ARBOR_MAX_STACK_DEPTH] = {0};
        static const uint8_t zero_fanin[ARBOR_MAX_STACK_DEPTH] = {0};
        int len = build_frame_ex(frame,
                                 g_my_ip, dst_ip,
                                 ARBOR_MSG_REGISTER_ACK, ARBOR_FLAG_VALID,
                                 channel_id, subchannel_id,
                                 0, epoch, 0, zero_stack, zero_fanin, ARBOR_REQ_NONE, NULL, 0);
        arbor_store_message_id(frame + sizeof(eth_header_t) + sizeof(ip_header_t) + sizeof(udp_header_t), message_id);
        fprintf(stderr,
                "[register-ack-tx] ch=%u sub=%u dst_rank=%d msg=%u epoch=%u\n",
                channel_id, subchannel_id, rank_of_ip(dst_ip), message_id, epoch);
        (void)enqueue_send_frame(subchannel_id, frame, (uint32_t)len, 0);
    }
    if (subchannel_id < SUBCHANNEL_COUNT) {
        g_responder_stats[subchannel_id].register_ack_sent++;
    }
}

static void refresh_register_ack_pending(protocol_message_t *meta, uint32_t expected_requesters) {
    if (!meta || !meta->in_use) return;
    for (uint32_t s = 0; s < SUBCHANNEL_COUNT; ++s) {
        const uint8_t sub_bit = (uint8_t)(1u << s);
        if ((meta->registered_acknowledged_mask & sub_bit) != 0) continue;
        if ((meta->registered_bitmap[s] & expected_requesters) != expected_requesters) continue;
        meta->register_ack_pending |= sub_bit;
    }
}

static void flush_register_acks(uint32_t channel_id,
                                const uint8_t *register_message_id,
                                const uint8_t *register_message_id_valid,
                                const uint32_t *register_epoch,
                                const uint8_t *register_epoch_valid) {
    for (uint16_t msg_id = 0; msg_id < MAX_ACTIVE_MESSAGES; ++msg_id) {
        protocol_message_t *meta = response_message_by_id(channel_id, (uint8_t)msg_id);
        if (!meta || !meta->in_use || meta->register_ack_pending == 0) continue;
        for (uint32_t s = 0; s < SUBCHANNEL_COUNT; ++s) {
            const uint8_t sub_bit = (uint8_t)(1u << s);
            if ((meta->register_ack_pending & sub_bit) == 0) continue;
            if (register_message_id_valid && !register_message_id_valid[s]) continue;
            if (register_epoch_valid && !register_epoch_valid[s]) continue;
            send_register_ack_all(channel_id, s,
                                  register_message_id ? register_message_id[s] : meta->message_id,
                                  register_epoch ? register_epoch[s] : meta->epoch);
            meta->registered_acknowledged_mask |= sub_bit;
            meta->register_ack_pending &= (uint8_t)~sub_bit;
            refresh_register_ack_pending(meta, neighbor_mask_of(channel_id));
        }
    }
}

static void send_end_all(uint32_t channel_id, const uint8_t *register_message_id, const uint8_t *register_message_id_valid, const uint32_t *register_epoch, const uint8_t *register_epoch_valid) {
    for (uint32_t s = 0; s < SUBCHANNEL_COUNT; s++) {
        if (register_message_id_valid && !register_message_id_valid[s]) continue;
        for (int i = 0; i < g_n; i++) {
            uint32_t dst_ip = g_cfg[i].host_ip;
            if (dst_ip == g_my_ip) continue;
            uint8_t frame[HDR_LEN];
            static const uint32_t zero_stack[ARBOR_MAX_STACK_DEPTH] = {0};
            static const uint8_t zero_fanin[ARBOR_MAX_STACK_DEPTH] = {0};
            int len = build_frame_ex(frame,
                                     g_my_ip, dst_ip,
                                     ARBOR_MSG_END, ARBOR_FLAG_VALID,
                                     channel_id, s,
                                     0, (register_epoch_valid && register_epoch_valid[s]) ? register_epoch[s] : 0, 0, zero_stack, zero_fanin, ARBOR_REQ_NONE, NULL, 0);
            if (register_message_id_valid && register_message_id_valid[s]) {
                arbor_store_message_id(frame + sizeof(eth_header_t) + sizeof(ip_header_t) + sizeof(udp_header_t), register_message_id[s]);
            }
            (void)enqueue_send_frame(s, frame, (uint32_t)len, 0);
        }
    }
}

static int all_registered_subchannels_acked(uint32_t expected_requesters,
                                            const uint32_t *end_ack_mask,
                                            const uint8_t *register_message_id_valid) {
    for (uint32_t s = 0; s < SUBCHANNEL_COUNT; s++) {
        if (register_message_id_valid && !register_message_id_valid[s]) continue;
        if (end_ack_mask[s] != expected_requesters) return 0;
    }
    return 1;
}

static uint32_t requester_mask_from_ip(uint32_t ip) {
    int r = rank_of_ip(ip);
    if (r < 0 || r >= 32) return 0;
    return 1u << (uint32_t)r;
}

static uint32_t inflight_credit_count_subchannel(const credit_entry_t *entries,
                                                 uint32_t subchannel_id) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < AGTR_ARRAY_SIZE; i++) {
        const credit_entry_t *e = &entries[i];
        if (!e->valid || !e->inflight_accounted) continue;
        if (e->subchannel_id != subchannel_id) continue;
        count++;
    }
    return count;
}

static void note_credit_sent(uint32_t channel_id, credit_entry_t *e, credit_entry_t *entries,
                             uint64_t credit_gap_us, uint32_t credit_window) {
    host_channel_state_t *state = find_channel_state(channel_id);
    e->sent_at = now_us();
    e->ref_inflight = state ? state->response_credits_outstanding : inflight_credit_count_subchannel(entries, e->subchannel_id);
    e->ref_credit_gap_us = clamp_credit_gap_us(credit_gap_us);
    e->ref_window = clamp_credit_window(credit_window);
    e->ce_seen = 0;
    e->inflight_accounted = 1;
    e->sample_eligible = 1;
    e->sample_recorded = 0;
    e->completion_kind = COMPLETION_KIND_NONE;
    {
        host_channel_state_t *state = find_channel_state(channel_id);
        if (state) state->response_credits_outstanding++;
    }
}

static int note_normal_path_sample(credit_entry_t *e) {
    if (!e->sample_eligible || e->sample_recorded) return 0;
    e->sample_recorded = 1;
    e->sample_eligible = 0;
    e->completion_kind = COMPLETION_KIND_NORMAL;
    e->inflight_accounted = 0;
    return 1;
}

static int build_normal_rate_sample(uint32_t channel_id, uint32_t subchannel_id,
                                    const credit_entry_t *e, uint64_t completed_at,
                                    responder_rate_sample_t *out) {
    if (!e || !out) return 0;
    if (!e->sample_recorded || e->completion_kind != COMPLETION_KIND_NORMAL) return 0;
    if (e->sent_at == 0 || completed_at < e->sent_at) return 0;

    out->channel_id = channel_id;
    out->subchannel_id = subchannel_id;
    out->credit_offset = e->credit_offset;
    out->rtt_us = completed_at - e->sent_at;
    out->ref_inflight = e->ref_inflight;
    out->ref_credit_gap_us = e->ref_credit_gap_us;
    out->ref_window = e->ref_window;
    out->completion_kind = e->completion_kind;
    out->ce = e->ce_seen ? 1u : 0u;
    return 1;
}

static void apply_rate_sample(uint32_t channel_id, const responder_rate_sample_t *sample,
                              uint64_t *credit_gap_us, uint32_t *credit_window) {
    uint64_t base_gap;
    uint64_t next_gap;
    uint32_t next_window;
    if (!sample || !credit_gap_us || !credit_window) return;
    base_gap = fixed_credit_gap_us();
    next_gap = *credit_gap_us ? *credit_gap_us : base_gap;
    next_window = *credit_window ? *credit_window : FIXED_STARTUP_WINDOW;
    if (sample->ce) {
        next_window = clamp_credit_window((next_window + 1U) / 2U);
        next_gap = clamp_credit_gap_us(next_gap >= CC_MAX_GAP_US ? CC_MAX_GAP_US : next_gap * 2U);
    } else {
        if (next_window < CC_MAX_WINDOW) next_window++;
        if (next_gap > base_gap) {
            uint64_t dec = (next_gap - base_gap) / 2U;
            if (dec == 0) dec = 1;
            next_gap = clamp_credit_gap_us(next_gap - dec);
        } else {
            next_gap = base_gap;
        }
    }
    *credit_gap_us = next_gap;
    *credit_window = next_window;
    fprintf(stderr,
            "[responder-cc] ch=%u sub=%u credit_off=%u rtt_us=%llu ce=%u gap_us=%llu window=%u\n",
            channel_id, sample->subchannel_id, sample->credit_offset,
            (unsigned long long)sample->rtt_us, (unsigned)sample->ce,
            (unsigned long long)*credit_gap_us, *credit_window);
}

static void note_non_sample_completion(credit_entry_t *e, completion_kind_t kind) {
    if (e->completion_kind == COMPLETION_KIND_NONE) e->completion_kind = (uint8_t)kind;
    e->sample_eligible = 0;
    if (e->inflight_accounted) e->inflight_accounted = 0;
}

static int mark_entry_done(uint32_t channel_id, credit_entry_t *e, uint32_t expected_requesters,
                           uint32_t *done_count) {
    if (!e || e->done || !e->committed) return 0;
    if ((e->requester_bitmap & expected_requesters) != expected_requesters) return 0;
    e->done = 1;
    if (done_count) (*done_count)++;
    {
        host_channel_state_t *state = find_channel_state(channel_id);
        if (state && state->response_credits_outstanding > 0) state->response_credits_outstanding--;
    }
    return 1;
}

static credit_entry_t *find_entry(credit_entry_t *entries, uint32_t credit_offset) {
    credit_entry_t *free_slot = NULL;
    for (uint32_t i = 0; i < AGTR_ARRAY_SIZE; i++) {
        credit_entry_t *e = &entries[i];
        if (e->valid && e->credit_offset == credit_offset) return e;
        if (!e->valid && !free_slot) free_slot = e;
    }
    if (!free_slot) return NULL;
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->valid = 1;
    free_slot->credit_offset = credit_offset;
    free_slot->payload_offset = 0;
    free_slot->agg_loc = 0;
    return free_slot;
}

static void build_result_payload(uint32_t channel_id, uint8_t *result_buf, uint32_t credit_offset,
                                 const uint8_t *remote_payload, uint16_t payload_len) {
    int32_t result[PAYLOAD_LEN / sizeof(int32_t)];
    memset(result, 0, sizeof(result));

    host_channel_state_t *state = find_channel_state(channel_id);
    if (state && state->local_src_buf && credit_offset < state->local_src_npkts) {
        memcpy(result, state->local_src_buf + credit_offset * PAYLOAD_LEN, PAYLOAD_LEN);
    }
    if (remote_payload && payload_len > 0) {
        const int32_t *remote = (const int32_t *)remote_payload;
        uint16_t words = payload_len / (uint16_t)sizeof(int32_t);
        for (uint16_t i = 0; i < words; i++) result[i] += remote[i];
    }

    if (payload_len > 0 && channel_id == 0 && credit_offset < 4) {
        const int32_t local0 = (state && state->local_src_buf && credit_offset < state->local_src_npkts)
            ? ((const int32_t *)(state->local_src_buf + credit_offset * PAYLOAD_LEN))[0]
            : 0;
        const int32_t remote0 = remote_payload ? ((const int32_t *)remote_payload)[0] : 0;
        fprintf(stderr,
                "[responder-build-result] ch=%u off=%u local0=%d remote0=%d result0=%d payload_len=%u\n",
                channel_id, credit_offset, local0, remote0, result[0], payload_len);
    }

    memcpy(result_buf, result, PAYLOAD_LEN);
}

static void commit_primary_response(credit_entry_t *e, void *buf, uint32_t credit_offset,
                                    const uint8_t *remote_payload, uint16_t payload_len,
                                    uint8_t message_id,
                                    uint32_t channel_id, uint32_t subchannel_id,
                                    uint32_t response_credit_offset, uint32_t payload_offset,
                                    uint32_t agg_loc, uint16_t agg_level,
                                    uint8_t agg_valid, uint16_t fanin) {
    uint8_t result_buf[PAYLOAD_LEN];
    if (e && e->subchannel_id < SUBCHANNEL_COUNT) {
        subchannel_id = e->subchannel_id;
    }
    build_result_payload(channel_id, result_buf, credit_offset, remote_payload, payload_len);
    memcpy((uint8_t *)buf + credit_offset * PAYLOAD_LEN, result_buf, PAYLOAD_LEN);
    memcpy(e->result, result_buf, PAYLOAD_LEN);
    cache_primary_response(e, message_id, channel_id, subchannel_id,
                           response_credit_offset, payload_offset, agg_loc,
                           agg_level, agg_valid, fanin, result_buf, PAYLOAD_LEN);
    e->committed = 1;
    e->tail_response_valid = 1;
    e->tail_response_acked = 0;
    e->tail_successor_credit_offset = response_credit_offset;
}

static int try_enter_repair(uint32_t channel_id, uint32_t subchannel_id,
                            uint32_t expected_requester_count, credit_entry_t *e) {
    host_channel_state_t *state;
    uint64_t now;
    double replay_bytes;
    double request_bytes;
    double trigger_bytes;
    if (!e || e->committed || e->done) return 0;
    if (e->subchannel_id >= SUBCHANNEL_COUNT) e->subchannel_id = subchannel_id;
    if (!e->repair_mode) e->repair_mode = 1;
    now = now_us();
    state = find_channel_state(channel_id);
    replay_bytes = e->bound_payload_valid ? (double)(PAYLOAD_LEN + HDR_LEN) : 0.0;
    request_bytes = (double)expected_requester_count * (double)(PAYLOAD_LEN + HDR_LEN);
    trigger_bytes = (double)HDR_LEN + replay_bytes + request_bytes;
    if (!charge_repair_tokens(state, now, trigger_bytes)) return 0;
    g_responder_stats[e->subchannel_id].repair_trigger_sent++;
    send_repair_trigger(channel_id, e->subchannel_id, e->credit_offset, e->agg_loc);
    if (e->retry_count < 0xff) e->retry_count++;
    e->sent_at = now;
    return 1;
}

static int reserve_credit(uint32_t channel_id, protocol_message_t *meta, uint32_t subchannel_id,
                          credit_entry_t *entries,
                          uint64_t *next_credit_at,
                          uint64_t credit_gap_us, uint32_t credit_window,
                          credit_entry_t **reserved_entry_out,
                          uint32_t *credit_offset_out,
                          uint32_t *agg_loc_out,
                          uint16_t *agg_level_out,
                          uint8_t *agg_valid_out) {
    uint64_t now = now_us();
    (void)now;
    if (reserved_entry_out) *reserved_entry_out = NULL;
    if (!meta || subchannel_id >= SUBCHANNEL_COUNT) return 0;
    if (meta->next_credit_offset >= meta->total_packets) return 0;

    while (meta->next_credit_offset < meta->total_packets) {
        uint32_t local_credit_offset = meta->next_credit_offset;
        uint32_t credit_offset = responder_local_to_wire(meta->epoch, local_credit_offset);
        credit_entry_t *e = find_entry(entries, credit_offset);
        if (!e) return 0;
        if (e->credit_sent) {
            meta->next_credit_offset++;
            continue;
        }
        note_credit_sent(channel_id, e, entries, credit_gap_us, credit_window);
        e->credit_sent = 1;
        e->completion_sent = 0;
        e->bound_payload_valid = 0;
        e->retry_count = 0;
        e->tail_response_valid = 0;
        e->tail_response_acked = 0;
        e->tail_successor_credit_offset = INVALID_OFFSET;
        e->bound_payload_offset = INVALID_OFFSET;
        e->subchannel_id = subchannel_id;
        e->agg_loc = 0;
        e->local_credit_offset = local_credit_offset;
        e->owner_message_id = meta->message_id;
        meta->next_credit_offset = local_credit_offset + 1;
        if (reserved_entry_out) *reserved_entry_out = e;
        *credit_offset_out = credit_offset;
        *agg_loc_out = 0;
        *agg_level_out = 0;
        *agg_valid_out = 0;
        g_responder_stats[subchannel_id].credit_sent++;
        if (next_credit_at) *next_credit_at = now + clamp_credit_gap_us(credit_gap_us);
        return 1;
    }
    return 0;
}

static void replay_completion_response(const credit_entry_t *e, uint32_t channel_id,
                                      uint32_t subchannel_id, uint16_t fanin) {
    (void)channel_id;
    (void)fanin;
    if (e && e->subchannel_id < SUBCHANNEL_COUNT) {
        subchannel_id = e->subchannel_id;
    }
    (void)subchannel_id;
    replay_primary_response(e);
}

static void replay_bound_payload_response(const credit_entry_t *e, uint32_t channel_id,
                                         uint32_t subchannel_id, uint16_t fanin) {
    if (!e->bound_payload_valid) return;
    if (e->subchannel_id < SUBCHANNEL_COUNT) {
        subchannel_id = e->subchannel_id;
    }
    broadcast_response(e->owner_message_id, channel_id, subchannel_id,
                       INVALID_OFFSET, e->bound_payload_offset, e->agg_loc,
                       0, 0, fanin, e->bound_payload, PAYLOAD_LEN, 0,
                       ARBOR_PAYLOAD_REPLAY, 0);
}

static int message_subchannel_ready(const protocol_message_t *meta, uint32_t subchannel_id) {
    if (!meta || subchannel_id >= SUBCHANNEL_COUNT) return 0;
    return (meta->registered_ready_mask & (1u << subchannel_id)) != 0;
}

static void note_registration_credit_sent(protocol_message_t *meta, uint32_t subchannel_id) {
    if (!meta || subchannel_id >= SUBCHANNEL_COUNT) return;
    meta->registered_acknowledged_mask |= (uint8_t)(1u << subchannel_id);
    meta->register_ack_pending &= (uint8_t)~(1u << subchannel_id);
}

static int response_message_done(const protocol_message_t *meta) {
    return !meta || !meta->in_use || !meta->sequence_reserved || meta->reuse_ready;
}

static void replay_tail_responses(uint32_t channel_id, uint16_t fanin,
                                  credit_entry_t *entries) {
    for (uint32_t i = 0; i < AGTR_ARRAY_SIZE; i++) {
        credit_entry_t *e = &entries[i];
        if (!e->valid || !e->tail_response_valid || e->tail_response_acked || e->subchannel_id >= SUBCHANNEL_COUNT) continue;
        note_non_sample_completion(e, COMPLETION_KIND_REPLAY);
        replay_completion_response(e, channel_id, e->subchannel_id, fanin);
    }
}

static int maybe_issue_credits(uint32_t channel_id, protocol_message_t *meta,
                               uint32_t subchannel_id, uint16_t fanin,
                               credit_entry_t *entries,
                               uint64_t *next_credit_at,
                               uint64_t credit_gap_us, uint32_t credit_window) {
    uint32_t credit_offset;
    uint32_t agg_loc;
    uint16_t agg_level;
    uint8_t agg_valid;

    if (!reserve_credit(channel_id, meta, subchannel_id, entries, next_credit_at,
                        credit_gap_us, credit_window,
                        NULL, &credit_offset, &agg_loc, &agg_level, &agg_valid)) return 0;

    broadcast_response(meta->message_id, channel_id, subchannel_id,
                       credit_offset,
                       INVALID_OFFSET, agg_loc,
                       agg_level, agg_valid, fanin, NULL, 0, next_credit_at ? *next_credit_at : 0,
                       ARBOR_PAYLOAD_COMPLETION, 0);
    note_registration_credit_sent(meta, subchannel_id);
    return 1;
}

static void accumulate_requester_input(credit_entry_t *e, uint32_t req_mask,
                                     const uint8_t *payload, uint16_t payload_len,
                                     int is_repair, uint32_t contribution) {
    int32_t *accum = is_repair ? e->repair_accum : e->direct_accum;
    uint32_t *bitmap = is_repair ? &e->repair_bitmap : &e->requester_bitmap;
    uint32_t *count = is_repair ? &e->repair_contrib_count : &e->direct_contrib_count;

    if (contribution == 0) contribution = 1;
    if (req_mask && (*bitmap & req_mask)) return;
    if (req_mask) *bitmap |= req_mask;
    *count += contribution;
    if (!payload || payload_len == 0) return;

    const int32_t *remote = (const int32_t *)payload;
    uint16_t words = payload_len / (uint16_t)sizeof(int32_t);
    for (uint16_t i = 0; i < words; i++) accum[i] += remote[i];
}

static void maybe_timeout_repair(uint32_t channel_id, uint32_t subchannel_id,
                                 credit_entry_t *entries, uint16_t fanin,
                                 uint32_t expected_requester_count) {
    uint64_t now = now_us();
    for (uint32_t i = 0; i < AGTR_ARRAY_SIZE; i++) {
        credit_entry_t *e = &entries[i];
        if (!e->valid || e->sent_at == 0) continue;
        if (e->subchannel_id != subchannel_id) continue;
        if (now - e->sent_at < RTO_US) continue;
        if (e->bound_payload_valid) {
            note_non_sample_completion(e, COMPLETION_KIND_REPLAY);
            replay_bound_payload_response(e, channel_id, subchannel_id, fanin);
        }
        if (!e->committed) {
            (void)try_enter_repair(channel_id, subchannel_id, expected_requester_count, e);
        }
    }
}

static int commit_entry_response(uint32_t channel_id, uint32_t subchannel_id,
                                 uint32_t expected_requesters, uint32_t local_credit_offset,
                                 uint16_t fanin, void *buf, protocol_message_t *msg_meta,
                                 credit_entry_t *entries, credit_entry_t *e,
                                 const uint8_t *payload, uint16_t payload_len,
                                 uint64_t *next_credit_at, uint64_t *credit_gap_us,
                                 uint32_t *credit_window, uint32_t *done_count,
                                 completion_kind_t completion_kind, int allow_sample) {
    uint32_t response_credit_offset = INVALID_OFFSET;
    uint32_t response_agg_loc = e->agg_loc;
    uint16_t response_agg_level = 0;
    uint8_t response_agg_valid = 0;
    credit_entry_t *reserved_entry = NULL;
    if (!e || !msg_meta || e->committed) return 0;
    if ((e->requester_bitmap & expected_requesters) != expected_requesters) return 0;
    if (!e->completion_sent && message_subchannel_ready(msg_meta, subchannel_id)) {
        (void)reserve_credit(channel_id, msg_meta, subchannel_id, entries,
                             next_credit_at,
                             credit_gap_us ? *credit_gap_us : fixed_credit_gap_us(),
                             credit_window ? *credit_window : FIXED_STARTUP_WINDOW, &reserved_entry,
                             &response_credit_offset, &response_agg_loc,
                             &response_agg_level, &response_agg_valid);
        if (reserved_entry) {
            reserved_entry->bound_payload_valid = 1;
            reserved_entry->bound_payload_offset =
                responder_local_to_wire(msg_meta->epoch, e->payload_offset);
        }
    }
    if (response_credit_offset != INVALID_OFFSET) {
        response_credit_offset = responder_local_to_wire(msg_meta->epoch,
                                                         response_credit_offset);
    }
    commit_primary_response(e, buf, local_credit_offset, payload, payload_len, msg_meta->message_id,
                            channel_id, subchannel_id, response_credit_offset,
                            responder_local_to_wire(msg_meta->epoch, e->payload_offset),
                            response_agg_loc, response_agg_level, response_agg_valid, fanin);
    if (reserved_entry) memcpy(reserved_entry->bound_payload, e->result, PAYLOAD_LEN);
    if (allow_sample) {
        if (note_normal_path_sample(e)) {
            responder_rate_sample_t sample;
            if (build_normal_rate_sample(channel_id, subchannel_id, e, now_us(), &sample)) {
                apply_rate_sample(channel_id, &sample, credit_gap_us, credit_window);
            }
        }
    } else {
        note_non_sample_completion(e, completion_kind);
    }
    if (!e->completion_sent) {
        g_responder_stats[subchannel_id].completion_sent++;
        emit_primary_response(e, ARBOR_PAYLOAD_COMPLETION);
        e->completion_sent = 1;
    }
    (void)mark_entry_done(channel_id, e, expected_requesters, done_count);
    return 1;
}

static void issue_ready_credits_round(uint32_t channel_id,
                                      uint16_t fanin,
                                      credit_entry_t *entries,
                                      uint64_t *next_credit_at,
                                      uint64_t *credit_gap_us,
                                      uint32_t *credit_window) {
    host_channel_state_t *state = find_channel_state(channel_id);
    uint8_t start_msg = state ? state->response_message_cursor : 0;
    uint64_t now = now_us();
    const uint64_t channel_gap = channel_credit_gap_us();
    if (state && state->response_credits_outstanding >= (1u << 20)) return;
    if (state && state->response_next_channel_credit_at != 0 && now < state->response_next_channel_credit_at) return;
    int made_progress;
    do {
        made_progress = 0;
        for (uint16_t mi = 0; mi < MAX_ACTIVE_MESSAGES; ++mi) {
            uint8_t msg_id = (uint8_t)(start_msg + mi);
            protocol_message_t *meta = response_message_by_id(channel_id, msg_id);
            if (!meta || response_message_done(meta) || meta->next_credit_offset >= meta->total_packets) continue;
            for (uint32_t i = 0; i < SUBCHANNEL_COUNT; i++) {
                uint32_t s = state ? (uint32_t)((state->response_sub_rr + i) % SUBCHANNEL_COUNT)
                                   : (meta->next_credit_subchannel + i) % SUBCHANNEL_COUNT;
                if (!message_subchannel_ready(meta, s)) continue;
                if (credit_window && inflight_credit_count_subchannel(entries, s) >= credit_window[s]) continue;
                if (next_credit_at && next_credit_at[s] != 0 && now < next_credit_at[s]) continue;
                if (!maybe_issue_credits(channel_id, meta, s, fanin, entries, &next_credit_at[s],
                                         credit_gap_us ? credit_gap_us[s] : fixed_credit_gap_us(),
                                         credit_window ? credit_window[s] : FIXED_STARTUP_WINDOW)) continue;
                meta->next_credit_subchannel = (uint8_t)((s + 1) % SUBCHANNEL_COUNT);
                if (state) {
                    state->response_message_cursor = msg_id;
                    state->response_sub_rr = (uint8_t)((s + 1) % SUBCHANNEL_COUNT);
                    state->response_next_channel_credit_at = now + channel_gap;
                }
                made_progress = 1;
                break;
            }
        }
    } while (made_progress);
}


int respond(uint32_t channel_id, void *buf, uint32_t size, uint8_t op) {
    (void)op;

    channel_ctx_t *ctx = find_channel(channel_id);
    host_channel_state_t *state = find_channel_state(channel_id);
    if (!ctx || !state) return -1;

    uint32_t total_npkts = size / PAYLOAD_LEN;
    if (total_npkts == 0) return 0;

    credit_entry_t *entries = calloc(AGTR_ARRAY_SIZE, sizeof(credit_entry_t));
    if (!entries) return -1;

    uint16_t fanin = (uint16_t)count_bits32(neighbor_mask_of(channel_id));
    if (fanin == 0) fanin = 1;

    uint32_t expected_requesters = neighbor_mask_of(channel_id);
    uint32_t expected_requester_count = (uint32_t)count_bits32(expected_requesters);
    if (expected_requester_count == 0) expected_requester_count = 1;
    uint8_t register_message_id[SUBCHANNEL_COUNT] = {0};
    uint8_t register_message_id_valid[SUBCHANNEL_COUNT] = {0};
    uint32_t register_epoch[SUBCHANNEL_COUNT] = {0};
    uint8_t register_epoch_valid[SUBCHANNEL_COUNT] = {0};
    uint64_t next_credit_at[SUBCHANNEL_COUNT] = {0};
    state->response_credits_outstanding = 0;
    state->response_sub_rr = 0;
    state->response_next_channel_credit_at = 0;
    state->response_repair_tokens = REPAIR_BURST_BYTES;
    state->response_repair_refill_at = now_us();
    uint64_t credit_gap_us[SUBCHANNEL_COUNT];
    uint32_t credit_window[SUBCHANNEL_COUNT];
    for (uint32_t s = 0; s < SUBCHANNEL_COUNT; ++s) {
        credit_gap_us[s] = fixed_credit_gap_us();
        credit_window[s] = FIXED_STARTUP_WINDOW;
    }

    uint32_t done_count = 0;
    uint32_t end_ack_mask[SUBCHANNEL_COUNT] = {0};
    uint8_t end_sent = 0;
    uint64_t end_sent_at = 0;
    uint64_t next_progress_log = 0;

    while (done_count < total_npkts ||
           !all_registered_subchannels_acked(expected_requesters, end_ack_mask,
                                             register_message_id_valid)) {
        rx_msg_t m;
        conn_t *cn = &g_conns[ctx->recv_conn];
        int popped_any = 0;
        uint64_t loop_now = now_us();
        if (loop_now >= next_progress_log) {
            fprintf(stderr,
                    "[responder-progress] ch=%u registered=0x%x ready=0x%x done=%u/%u end_ack0=0x%x end_ack1=0x%x\n",
                    channel_id,
                    response_message_by_id(channel_id, register_message_id[0]) && register_message_id_valid[0]
                        ? response_message_by_id(channel_id, register_message_id[0])->registered_bitmap[0] : 0,
                    response_message_by_id(channel_id, register_message_id[0]) && register_message_id_valid[0]
                        ? response_message_by_id(channel_id, register_message_id[0])->registered_ready_mask : 0,
                    done_count, total_npkts, end_ack_mask[0], end_ack_mask[1]);
            next_progress_log = loop_now + 1000000ULL;
        }
        for (uint32_t s = 0; s < SUBCHANNEL_COUNT; s++) {
            maybe_timeout_repair(channel_id, s, entries, fanin, expected_requester_count);
        }
        issue_ready_credits_round(channel_id, fanin, entries, next_credit_at, credit_gap_us, credit_window);
        if (end_sent && !all_registered_subchannels_acked(expected_requesters, end_ack_mask,
                                                          register_message_id_valid) && end_sent_at != 0 &&
            loop_now - end_sent_at >= RTO_US) {
            if (charge_repair_tokens(state, loop_now, (double)HDR_LEN * SUBCHANNEL_COUNT)) {
                replay_tail_responses(channel_id, fanin, entries);
                send_end_all(channel_id, register_message_id, register_message_id_valid,
                             register_epoch, register_epoch_valid);
            }
            end_sent_at = loop_now;
        }
        flush_register_acks(channel_id, register_message_id, register_message_id_valid,
                            register_epoch, register_epoch_valid);
        flush_send_queue();

        while (conn_pop(cn, &m)) {
            popped_any = 1;
            uint64_t now = now_us();
            if (now >= next_progress_log) {
                fprintf(stderr,
                        "[responder-progress] ch=%u registered=0x%x ready=0x%x done=%u/%u end_ack0=0x%x end_ack1=0x%x\n",
                        channel_id,
                        response_message_by_id(channel_id, register_message_id[0]) && register_message_id_valid[0]
                            ? response_message_by_id(channel_id, register_message_id[0])->registered_bitmap[0] : 0,
                        response_message_by_id(channel_id, register_message_id[0]) && register_message_id_valid[0]
                            ? response_message_by_id(channel_id, register_message_id[0])->registered_ready_mask : 0,
                        done_count, total_npkts, end_ack_mask[0], end_ack_mask[1]);
                next_progress_log = now + 1000000ULL;
            }

            for (uint32_t s = 0; s < SUBCHANNEL_COUNT; s++) {
                maybe_timeout_repair(channel_id, s, entries, fanin, expected_requester_count);
            }
            issue_ready_credits_round(channel_id, fanin, entries, next_credit_at, credit_gap_us, credit_window);

            if (end_sent && !all_registered_subchannels_acked(expected_requesters, end_ack_mask,
                                                              register_message_id_valid) && end_sent_at != 0 &&
                now - end_sent_at >= RTO_US) {
                if (charge_repair_tokens(state, now, (double)HDR_LEN * SUBCHANNEL_COUNT)) {
                    replay_tail_responses(channel_id, fanin, entries);
                    send_end_all(channel_id, register_message_id, register_message_id_valid,
                                 register_epoch, register_epoch_valid);
                }
                end_sent_at = now;
            }
            flush_register_acks(channel_id, register_message_id, register_message_id_valid,
                                register_epoch, register_epoch_valid);
            flush_send_queue();

            if (channel_id >= 6) {
                fprintf(stderr,
                        "[responder-pop] ch=%u msg_type=%u msg=%u sub=%u src_rank=%d pkt_ch=%u\n",
                        channel_id, (unsigned)m.msg_type, (unsigned)m.message_id,
                        m.subchannel_id, rank_of_ip(m.src_ip), m.channel_id);
            }
            if (m.channel_id != channel_id) continue;
            if (m.subchannel_id >= SUBCHANNEL_COUNT) continue;

            if (m.msg_type == ARBOR_MSG_AGG_MISS) {
                uint32_t local_credit_offset;
                protocol_message_t *meta = find_response_message_for_sequence(channel_id, m.credit_offset,
                                                                              &local_credit_offset);
                if (!meta || meta->message_id != m.message_id) continue;
                credit_entry_t *e = find_entry(entries, m.payload_offset);
                if (!e || e->committed || e->done || e->repair_mode) continue;
                e->sample_eligible = 0;
                e->credit_offset = responder_local_to_wire(meta->epoch, local_credit_offset);
                e->subchannel_id = m.subchannel_id;
                (void)try_enter_repair(channel_id, m.subchannel_id, expected_requester_count, e);
                continue;
            }

            if (m.msg_type == ARBOR_MSG_END_ACK) {
                protocol_message_t *ack_meta = response_message_by_id(channel_id, m.message_id);
                if (m.subchannel_id < SUBCHANNEL_COUNT && ack_meta && ack_meta->in_use &&
                    (!register_message_id_valid[m.subchannel_id] ||
                     m.message_id == register_message_id[m.subchannel_id]) &&
                    m.payload_offset == ack_meta->epoch) {
                    ack_meta->responder_end_ack_mask[m.subchannel_id] |= requester_mask_from_ip(m.src_ip);
                    end_ack_mask[m.subchannel_id] = ack_meta->responder_end_ack_mask[m.subchannel_id];
                    if (ack_meta->responder_end_ack_mask[m.subchannel_id] == expected_requesters) {
                        ack_meta->sequence_reserved = 0;
                        ack_meta->reuse_ready = 1;
                    }
                }
                continue;
            }

            if (m.msg_type == ARBOR_MSG_REGISTER) {
                protocol_message_t *meta = upsert_response_message(channel_id, m.message_id,
                                                                  m.payload_offset, total_npkts);
                uint32_t req_mask;
                if (!meta) continue;
                register_message_id[m.subchannel_id] = m.message_id;
                register_message_id_valid[m.subchannel_id] = 1;
                g_responder_stats[m.subchannel_id].register_recv++;
                req_mask = requester_mask_from_ip(m.src_ip);
                meta->registered_bitmap[m.subchannel_id] |= req_mask;
                if ((req_mask & (1u << 7)) != 0 || channel_id >= 4) {
                    fprintf(stderr,
                            "[register-rx] ch=%u sub=%u src_rank=%d mask=0x%x bitmap=0x%x expect=0x%x msg=%u epoch=%u\n",
                            channel_id, m.subchannel_id, rank_of_ip(m.src_ip), req_mask,
                            meta->registered_bitmap[m.subchannel_id], expected_requesters,
                            m.message_id, meta->epoch);
                }
                if ((meta->registered_bitmap[m.subchannel_id] & expected_requesters) == expected_requesters) {
                    const uint8_t sub_bit = (uint8_t)(1u << m.subchannel_id);
                    if ((meta->registered_ready_mask & sub_bit) == 0) {
                        meta->registered_ready_mask |= sub_bit;
                        register_epoch[m.subchannel_id] = meta->epoch;
                        register_epoch_valid[m.subchannel_id] = 1;
                        fprintf(stderr,
                                "[register-ready] ch=%u sub=%u bitmap=0x%x expect=0x%x msg=%u epoch=%u\n",
                                channel_id, m.subchannel_id,
                                meta->registered_bitmap[m.subchannel_id], expected_requesters,
                                register_message_id[m.subchannel_id], register_epoch[m.subchannel_id]);
                        refresh_register_ack_pending(meta, expected_requesters);
                        issue_ready_credits_round(channel_id, fanin, entries, next_credit_at, credit_gap_us, credit_window);
                    } else {
                        refresh_register_ack_pending(meta, expected_requesters);
                    }
                }
                continue;
            }

            if (m.msg_type != ARBOR_MSG_REQUEST && m.msg_type != ARBOR_MSG_REPAIR_REQUEST) {
                continue;
            }
            fprintf(stderr,
                    "[responder-request-pop] ch=%u sub=%u msg_type=%u msg=%u payload_off=%u credit_off=%u payload_len=%u agg_depth=%u request_kind=%u src_rank=%d first_word=%d\n",
                    channel_id, m.subchannel_id, (unsigned)m.msg_type, (unsigned)m.message_id,
                    m.payload_offset, m.credit_offset, m.payload_len, m.agg_depth,
                    (unsigned)m.request_kind, rank_of_ip(m.src_ip),
                    (m.payload_len >= sizeof(int32_t)) ? ((const int32_t *)m.payload)[0] : 0);
            if (register_message_id_valid[m.subchannel_id] &&
                m.message_id != register_message_id[m.subchannel_id]) {
                fprintf(stderr,
                        "[responder-request-drop] ch=%u sub=%u reason=msgid expect=%u got=%u payload_off=%u src_rank=%d\n",
                        channel_id, m.subchannel_id, register_message_id[m.subchannel_id], m.message_id,
                        m.payload_offset, rank_of_ip(m.src_ip));
                continue;
            }

            uint32_t local_credit_offset;
            uint32_t local_payload_offset;
            protocol_message_t *msg_meta = find_response_message_for_sequence(channel_id,
                                                                             m.payload_offset,
                                                                             &local_credit_offset);
            if (!msg_meta || msg_meta->message_id != m.message_id) {
                fprintf(stderr,
                        "[responder-request-drop] ch=%u sub=%u reason=seq-lookup payload_off=%u msg=%u src_rank=%d meta=%p\n",
                        channel_id, m.subchannel_id, m.payload_offset, m.message_id, rank_of_ip(m.src_ip), (void *)msg_meta);
                continue;
            }
            if (!responder_wire_to_local(msg_meta->epoch, msg_meta->total_packets,
                                         m.payload_offset, &local_payload_offset)) {
                fprintf(stderr,
                        "[responder-request-drop] ch=%u sub=%u reason=wire-to-local payload_off=%u epoch=%u total=%u src_rank=%d\n",
                        channel_id, m.subchannel_id, m.payload_offset, msg_meta->epoch, msg_meta->total_packets, rank_of_ip(m.src_ip));
                continue;
            }

            credit_entry_t *e = find_entry(entries, m.payload_offset);
            if (!e) {
                fprintf(stderr,
                        "[responder-request-drop] ch=%u sub=%u reason=no-entry payload_off=%u src_rank=%d\n",
                        channel_id, m.subchannel_id, m.payload_offset, rank_of_ip(m.src_ip));
                continue;
            }
            e->payload_offset = local_payload_offset;
            e->agg_loc = (m.agg_depth > 0) ? m.agg_stack[m.agg_depth - 1] : 0;
            if ((m.flags & ARBOR_FLAG_ECN) != 0) e->ce_seen = 1;

            if (e->committed) {
                replay_completion_response(e, channel_id, m.subchannel_id, fanin);
                continue;
            }

            for (uint32_t i = 0; i < AGTR_ARRAY_SIZE; i++) {
                credit_entry_t *prev = &entries[i];
                if (!prev->valid || !prev->tail_response_valid || prev->tail_response_acked) continue;
                if (prev->tail_successor_credit_offset != m.credit_offset) continue;
                prev->tail_response_acked = 1;
            }

            uint32_t req_mask = requester_mask_from_ip(m.src_ip);
            const int allow_normal_single = expected_requester_count <= 1;
            const int from_repair = (m.msg_type == ARBOR_MSG_REPAIR_REQUEST);
            uint32_t ready_bitmap = 0;
            const uint8_t *accum_payload = NULL;
            completion_kind_t completion_kind = COMPLETION_KIND_DIRECT;


            if (m.request_kind == ARBOR_REQ_AGGREGATE_CONTROL_ACK && m.payload_len == 0) {
                fprintf(stderr,
                        "[responder-control-ack] ch=%u sub=%u msg=%u payload_off=%u credit_off=%u src_rank=%d\n",
                        channel_id, m.subchannel_id, (unsigned)m.message_id,
                        m.payload_offset, m.credit_offset, rank_of_ip(m.src_ip));
                continue;
            }
            if (from_repair) {
                g_responder_stats[m.subchannel_id].repair_request_recv++;
                accumulate_requester_input(e, req_mask, m.payload, m.payload_len, 1, 1);
                ready_bitmap = e->repair_bitmap;
                accum_payload = (const uint8_t *)e->repair_accum;
                completion_kind = COMPLETION_KIND_REPAIR;
            } else if (m.request_kind == ARBOR_REQ_AGGREGATE_PAYLOAD) {
                uint32_t contribution = m.credit_offset > 0 ? m.credit_offset : 1;
                accumulate_requester_input(e, req_mask, m.payload, m.payload_len, 0, contribution);
                ready_bitmap = (e->direct_contrib_count >= expected_requester_count) ? expected_requesters : e->requester_bitmap;
                accum_payload = (const uint8_t *)e->direct_accum;
                completion_kind = COMPLETION_KIND_DIRECT;
            } else if (allow_normal_single) {
                accumulate_requester_input(e, req_mask, m.payload, m.payload_len, 0, 1);
                ready_bitmap = e->requester_bitmap;
                accum_payload = (const uint8_t *)e->direct_accum;
                completion_kind = COMPLETION_KIND_DIRECT;
            } else {
                fprintf(stderr,
                        "[responder-protocol-violation] ch=%u sub=%u reason=raw-normal src_rank=%d payload_off=%u credit_off=%u expect=%u\n",
                        channel_id, m.subchannel_id, rank_of_ip(m.src_ip),
                        m.payload_offset, m.credit_offset, expected_requester_count);
                fflush(stderr);
                exit(2);
            }
            if ((ready_bitmap & expected_requesters) == expected_requesters) {
                if (!from_repair && e->direct_contrib_count >= expected_requester_count) e->requester_bitmap = expected_requesters;
                e->requester_bitmap = expected_requesters;
                if (from_repair) {
                    g_responder_stats[m.subchannel_id].repair_commit++;
                    e->repair_bitmap = expected_requesters;
                } else {
                    g_responder_stats[m.subchannel_id].request_commit++;
                }
                if (commit_entry_response(channel_id, m.subchannel_id, expected_requesters,
                                          local_credit_offset, fanin, buf, msg_meta,
                                          entries, e, accum_payload, PAYLOAD_LEN,
                                          &next_credit_at[m.subchannel_id],
                                          &credit_gap_us[m.subchannel_id],
                                          &credit_window[m.subchannel_id], &done_count,
                                          completion_kind, 0)) {
                    issue_ready_credits_round(channel_id, fanin, entries, next_credit_at, credit_gap_us, credit_window);
                }
            }

            if (!end_sent && done_count >= total_npkts) {
                for (uint32_t s = 0; s < SUBCHANNEL_COUNT; s++) {
                    if (!register_message_id_valid[s]) continue;
                    protocol_message_t *meta = response_message_by_id(channel_id, register_message_id[s]);
                    if (meta) meta->complete = 1;
                }
                send_end_all(channel_id, register_message_id, register_message_id_valid,
                             register_epoch, register_epoch_valid);
                end_sent = 1;
                end_sent_at = now_us();
            }
        }

        if (!popped_any) {
            usleep(200);
        }
    }

    flush_send_queue();
    dump_responder_stats(channel_id);
    free(entries);
    return (int)size;
}
