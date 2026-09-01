#ifndef HOST_PRIV_H
#define HOST_PRIV_H

#include "runtime/runtime_common.h"

#define MAX_ACTIVE_MESSAGES 256

typedef struct {
    uint32_t credit_offset;
    uint8_t agg_depth;
    uint8_t repair;
    uint8_t ecn_ce;
    uint32_t agg_stack[ARBOR_MAX_STACK_DEPTH];
    uint8_t fanin[ARBOR_MAX_STACK_DEPTH];
    uint32_t channel_id;
    uint32_t subchannel_id;
} credit;

typedef struct {
    uint32_t repair_offset;
    uint32_t channel_id;
    uint32_t subchannel_id;
} repair;

typedef struct {
    int active;
    uint32_t channel_id;
    uint32_t subchannel_id;
    uint16_t udp_port;
    int iface_index;
} subchannel_ctx_t;

typedef struct {
    int active;
    uint32_t channel_id;
    int uplink_conn;
    int recv_conn;
    uint32_t local_ip;
    uint32_t responder_ip;
} channel_ctx_t;

typedef struct {
    uint8_t in_use;
    uint8_t error;
    uint8_t message_id;
    uint8_t sequence_reserved;
    uint32_t start_sequence;
    uint32_t epoch;
    uint32_t total_packets;
    const void *src_buffer;
    void *dst_buffer;
    uint8_t register_acked_mask;
    uint8_t register_failed_mask;
    uint8_t end_ack_mask;
    uint8_t end_pending;
    uint8_t reuse_ready;
    uint8_t complete;
    uint8_t registered_ready_mask;
    uint8_t registered_acknowledged_mask;
    uint8_t next_credit_subchannel;
    uint8_t end_sent;
    uint8_t register_ack_pending;
    uint32_t next_credit_offset;
    uint32_t registered_bitmap[SUBCHANNEL_COUNT];
    uint32_t responder_end_ack_mask[SUBCHANNEL_COUNT];
    uint32_t end_ack_epoch;
    uint8_t end_ack_epoch_valid;
    uint32_t register_attempts[SUBCHANNEL_COUNT];
    uint64_t end_sent_at;
} protocol_message_t;

typedef struct {
    channel_ctx_t channel;
    subchannel_ctx_t subchannels[SUBCHANNEL_COUNT];
    const uint8_t *local_src_buf;
    uint32_t local_src_npkts;
    uint8_t local_src_op;
    uint8_t *request_result_buf;
    uint32_t request_result_npkts;
    uint32_t request_next_sequence;
    protocol_message_t request_messages[MAX_ACTIVE_MESSAGES];
    protocol_message_t response_messages[MAX_ACTIVE_MESSAGES];
    uint8_t request_message_lookup_hint;
    uint8_t response_message_lookup_hint;
    uint8_t request_next_message_id;
    uint8_t response_message_cursor;
    uint32_t response_credits_outstanding;
    uint8_t response_sub_rr;
    uint64_t response_next_channel_credit_at;
    double response_repair_tokens;
    uint64_t response_repair_refill_at;
    uint32_t request_end_tombstone_seq[MAX_ACTIVE_MESSAGES];
    uint8_t request_end_tombstone_valid[MAX_ACTIVE_MESSAGES];
} host_channel_state_t;

extern config_entry_t g_cfg[MAX_GROUP_SIZE];
extern int g_n;
extern int g_rank;
extern uint32_t g_my_ip;
extern pcap_t *g_host_handles[SUBCHANNEL_COUNT];
extern pcap_t *g_host_tx_handles[SUBCHANNEL_COUNT];
extern pthread_mutex_t g_tx_lock;
extern conn_t g_conns[MAX_CONNS];
extern int g_conn_count;

conn_t *find_conn_by_remote(uint32_t remote_ip);
host_channel_state_t *find_channel_state(uint32_t channel_id);
channel_ctx_t *find_channel(uint32_t channel_id);
subchannel_ctx_t *find_subchannel(uint32_t channel_id, uint32_t subchannel_id);
subchannel_ctx_t *find_subchannel_by_port(uint16_t udp_port);
void host_inject(uint8_t *frame, int len);
void host_inject_on_subchannel(uint32_t subchannel_id, uint8_t *frame, int len);
int conn_pop(conn_t *cn, rx_msg_t *out);
void register_request_result(uint32_t channel_id, void *buf, uint32_t size);
void clear_request_result(uint32_t channel_id);
protocol_message_t *request_message_by_id(uint32_t channel_id, uint8_t message_id);
protocol_message_t *response_message_by_id(uint32_t channel_id, uint8_t message_id);
protocol_message_t *find_request_message_for_sequence(uint32_t channel_id, uint32_t packet_sequence,
                                                      uint32_t *local_offset_out);
protocol_message_t *find_response_message_for_sequence(uint32_t channel_id, uint32_t packet_sequence,
                                                       uint32_t *local_offset_out);
int reserve_protocol_sequence(uint32_t channel_id, protocol_message_t *candidate,
                              uint32_t packet_count, uint32_t *start_sequence_out);
int rollback_protocol_sequence(uint32_t channel_id, uint32_t start_sequence, uint32_t packet_count);
protocol_message_t *acquire_request_message(uint32_t channel_id, uint32_t total_packets);
protocol_message_t *upsert_response_message(uint32_t channel_id, uint8_t message_id,
                                            uint32_t start_sequence, uint32_t total_packets);

#endif
