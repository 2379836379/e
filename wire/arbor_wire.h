#ifndef TEST_LAB_WIRE_ARBOR_WIRE_H
#define TEST_LAB_WIRE_ARBOR_WIRE_H

#include <arpa/inet.h>
#include <stdint.h>
#include <string.h>

#define PAYLOAD_LEN    1024
#define ARBOR_IP_PROTO 0x12
#define ETH_TYPE_IP    0x0800
#define MTP_UDP_PORT_BASE 10000
#define MTP_MAX_RANKS_PER_GROUP 128
#define MTP_MAX_GROUPS 64
#define MTP_MAX_SUBCHANNELS 4

#define MAX_GROUP_SIZE 8
#define MAX_CONNS      32
#define MAX_CHANNELS   8
#define SUBCHANNEL_COUNT 2
#define ARBOR_MAX_STACK_DEPTH 3

#define WINDOW        32
#define RTO_US        50000
#define AGTR_ARRAY_SIZE (2 * WINDOW)

#define OP_ALLREDUCE    2

#define ARBOR_FLAG_ECN               0x01
#define ARBOR_FLAG_VALID             0x02
#define ARBOR_FLAG_CREDIT_VALID      0x04
#define ARBOR_FLAG_COMPLETION_VALID  0x08

typedef struct {
    uint8_t dst_mac[6];
    uint8_t src_mac[6];
    uint16_t ether_type;
} __attribute__((packed)) eth_header_t;

typedef struct {
    uint8_t version_ihl;
    uint8_t tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
} __attribute__((packed)) ip_header_t;

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t len;
    uint16_t checksum;
} __attribute__((packed)) udp_header_t;

typedef enum {
    ARBOR_PKT_REGISTER = 0,
    ARBOR_PKT_DATA_REQUEST = 1,
    ARBOR_PKT_RESPONSE = 2,
    ARBOR_PKT_END = 3,
    ARBOR_PKT_END_ACK = 4,
    ARBOR_PKT_REGISTER_ACK = 5,
    ARBOR_PKT_AGG_MISS = 6,
} arbor_packet_type_t;

typedef enum {
    ARBOR_PAYLOAD_DATA = 0,
    ARBOR_PAYLOAD_COMPLETION = 1,
    ARBOR_PAYLOAD_REPLAY = 2,
} arbor_payload_kind_t;

typedef enum {
    ARBOR_OP_NOP = 0,
    ARBOR_OP_SUM = 1,
    ARBOR_OP_MAX = 2,
    ARBOR_OP_MIN = 3,
} arbor_op_t;

typedef enum {
    ARBOR_DTYPE_FLOAT16 = 0,
    ARBOR_DTYPE_BFLOAT16 = 1,
    ARBOR_DTYPE_FLOAT32 = 2,
    ARBOR_DTYPE_FLOAT64 = 3,
    ARBOR_DTYPE_INT32 = 4,
    ARBOR_DTYPE_INT64 = 5,
} arbor_dtype_t;

typedef enum {
    ARBOR_MSG_REGISTER = 0,
    ARBOR_MSG_REQUEST = 1,
    ARBOR_MSG_RESPONSE = 2,
    ARBOR_MSG_END = 3,
    ARBOR_MSG_END_ACK = 4,
    ARBOR_MSG_REGISTER_ACK = 5,
    ARBOR_MSG_AGG_MISS = 6,
    ARBOR_MSG_REPAIR_TRIGGER = 7,
    ARBOR_MSG_REPAIR_REQUEST = 8,
} arbor_msg_type_t;

typedef enum {
    ARBOR_REQ_NONE = 0,
    ARBOR_REQ_AGGREGATE_CONTROL_ACK = 1,
    ARBOR_REQ_AGGREGATE_PAYLOAD = 2,
} arbor_request_kind_t;

typedef enum {
    ARBOR_ROUTER_ACT_BYPASS = 0,
    ARBOR_ROUTER_ACT_AGGREGATE_CONTROL_ACK = 1,
    ARBOR_ROUTER_ACT_AGGREGATE_PAYLOAD = 2,
    ARBOR_ROUTER_ACT_AGGREGATE = 3,
} arbor_router_action_t;

typedef struct {
    uint8_t byte0;
    uint8_t message_id;
    uint8_t ctrl;
    uint8_t offset_a[3];
    uint8_t offset_b[3];
    uint8_t agg_loc0[2];
    uint8_t fanin0;
    uint8_t fanin1;
    uint8_t fanin2;
    uint8_t reserved0;
    uint8_t dtype;
    uint8_t agg_loc1[2];
    uint8_t agg_loc2[2];
} __attribute__((packed)) arbor_header_t;

#define HDR_LEN ((int)(sizeof(eth_header_t) + sizeof(ip_header_t) + sizeof(udp_header_t) + sizeof(arbor_header_t)))

#define ARBOR_WIRE_VERSION 2
#define ARBOR_SEQUENCE_MASK 0xFFFFFFU
#define ARBOR_IPV4_ECN_MASK 0x03
#define ARBOR_IPV4_ECN_ECT0 0x02
#define ARBOR_IPV4_ECN_CE 0x03
#define AGG_INDEX_UNUSED 0xFFFF

#define kArborByte0Offset 0
#define kArborMessageIdOffset 1
#define kArborCtrlOffset 2
#define kArborOffsetAOffset 3
#define kArborOffsetBOffset 6
#define kArborAggLocOffset 9
#define kArborFaninOffset 11
#define kArborReservedOffset 14
#define kArborDtypeOffset 15
#define kArborAggLoc1Offset 16
#define kArborAggLoc2Offset 18

static inline uint8_t arbor_get_version(uint8_t byte0) { return (byte0 >> 6U) & 0x3U; }
static inline arbor_packet_type_t arbor_get_packet_type(uint8_t byte0) { return (arbor_packet_type_t)((byte0 >> 3U) & 0x7U); }
static inline void arbor_set_packet_type(uint8_t *byte0, arbor_packet_type_t type) { *byte0 = (uint8_t)((*byte0 & 0xC7U) | (((uint8_t)type & 0x7U) << 3U)); }
static inline int arbor_get_repair(uint8_t byte0) { return ((byte0 >> 2U) & 0x1U) != 0; }
static inline void arbor_set_repair(uint8_t *byte0, int repair) { *byte0 = (uint8_t)((*byte0 & 0xFBU) | ((repair ? 1U : 0U) << 2U)); }
static inline uint8_t arbor_get_operation(uint8_t byte0) { return byte0 & 0x3U; }
static inline void arbor_set_operation(uint8_t *byte0, uint8_t op) { *byte0 = (uint8_t)((*byte0 & 0xFCU) | (op & 0x3U)); }
static inline uint8_t arbor_make_byte0(arbor_packet_type_t type, int repair, uint8_t op) { return (uint8_t)(((ARBOR_WIRE_VERSION & 0x3U) << 6U) | (((uint8_t)type & 0x7U) << 3U) | ((repair ? 1U : 0U) << 2U) | (op & 0x3U)); }
static inline int arbor_packet_type_is_downstream(arbor_packet_type_t type) { return type == ARBOR_PKT_RESPONSE || type == ARBOR_PKT_REGISTER_ACK || type == ARBOR_PKT_END; }
static inline int arbor_get_payload_valid(uint8_t ctrl) { return ((ctrl >> 7U) & 0x1U) != 0; }
static inline void arbor_set_payload_valid(uint8_t *ctrl, int valid) { *ctrl = (uint8_t)((*ctrl & 0x7FU) | ((valid ? 1U : 0U) << 7U)); }
static inline arbor_payload_kind_t arbor_get_payload_kind(uint8_t ctrl) { return (arbor_payload_kind_t)((ctrl >> 5U) & 0x3U); }
static inline void arbor_set_payload_kind(uint8_t *ctrl, arbor_payload_kind_t kind) { *ctrl = (uint8_t)((*ctrl & 0x9FU) | (((uint8_t)kind & 0x3U) << 5U)); }
static inline int arbor_get_credit_valid(uint8_t ctrl) { return ((ctrl >> 4U) & 0x1U) != 0; }
static inline void arbor_set_credit_valid(uint8_t *ctrl, int valid) { *ctrl = (uint8_t)((*ctrl & 0xEFU) | ((valid ? 1U : 0U) << 4U)); }
static inline uint8_t arbor_get_agg_depth(uint8_t ctrl) { return (ctrl >> 1U) & 0x3U; }
static inline void arbor_set_agg_depth(uint8_t *ctrl, uint8_t depth) { *ctrl = (uint8_t)((*ctrl & 0xF9U) | ((depth & 0x3U) << 1U)); }
static inline int arbor_get_aggregated(uint8_t ctrl) { return (ctrl & 0x1U) != 0; }
static inline void arbor_set_aggregated(uint8_t *ctrl, int aggregated) { *ctrl = (uint8_t)((*ctrl & 0xFEU) | (aggregated ? 1U : 0U)); }
static inline uint8_t arbor_make_ctrl(int payload_valid, arbor_payload_kind_t payload_kind, int credit_valid, uint8_t agg_depth, int aggregated) { return (uint8_t)(((payload_valid ? 1U : 0U) << 7U) | (((uint8_t)payload_kind & 0x3U) << 5U) | ((credit_valid ? 1U : 0U) << 4U) | ((agg_depth & 0x3U) << 1U) | (aggregated ? 1U : 0U)); }
static inline uint32_t arbor_load_offset24(const uint8_t *wire, uint32_t field_offset) { return ((uint32_t)wire[field_offset] << 16U) | ((uint32_t)wire[field_offset + 1] << 8U) | (uint32_t)wire[field_offset + 2]; }
static inline void arbor_store_offset24(uint8_t *wire, uint32_t field_offset, uint32_t value) { wire[field_offset] = (uint8_t)((value >> 16U) & 0xFFU); wire[field_offset + 1] = (uint8_t)((value >> 8U) & 0xFFU); wire[field_offset + 2] = (uint8_t)(value & 0xFFU); }
static inline uint8_t arbor_load_byte0(const uint8_t *wire) { return wire[kArborByte0Offset]; }
static inline void arbor_store_byte0(uint8_t *wire, uint8_t byte0) { wire[kArborByte0Offset] = byte0; }
static inline uint8_t arbor_load_message_id(const uint8_t *wire) { return wire[kArborMessageIdOffset]; }
static inline void arbor_store_message_id(uint8_t *wire, uint8_t message_id) { wire[kArborMessageIdOffset] = message_id; }
static inline uint8_t arbor_load_ctrl(const uint8_t *wire) { return wire[kArborCtrlOffset]; }
static inline void arbor_store_ctrl(uint8_t *wire, uint8_t ctrl) { wire[kArborCtrlOffset] = ctrl; }
static inline uint32_t arbor_load_offset_a(const uint8_t *wire) { return arbor_load_offset24(wire, kArborOffsetAOffset); }
static inline void arbor_store_offset_a(uint8_t *wire, uint32_t value) { arbor_store_offset24(wire, kArborOffsetAOffset, value); }
static inline uint32_t arbor_load_offset_b(const uint8_t *wire) { return arbor_load_offset24(wire, kArborOffsetBOffset); }
static inline void arbor_store_offset_b(uint8_t *wire, uint32_t value) { arbor_store_offset24(wire, kArborOffsetBOffset, value); }
static inline uint32_t arbor_load_request_offset(const uint8_t *wire) { return arbor_load_offset_a(wire); }
static inline void arbor_store_request_offset(uint8_t *wire, uint32_t value) { arbor_store_offset_a(wire, value); }
static inline uint32_t arbor_load_payload_offset(const uint8_t *wire) { return arbor_load_offset_a(wire); }
static inline void arbor_store_payload_offset(uint8_t *wire, uint32_t value) { arbor_store_offset_a(wire, value); }
static inline uint32_t arbor_load_credit_offset(const uint8_t *wire) { return arbor_load_offset_b(wire); }
static inline void arbor_store_credit_offset(uint8_t *wire, uint32_t value) { arbor_store_offset_b(wire, value); }
static inline uint16_t arbor_load_agg_loc_at(const uint8_t *wire, uint8_t level) {
    uint16_t encoded = 0;
    uint32_t off = kArborAggLocOffset;
    if (level == 1) off = kArborAggLoc1Offset;
    else if (level >= 2) off = kArborAggLoc2Offset;
    memcpy(&encoded, wire + off, sizeof(encoded));
    return ntohs(encoded);
}
static inline void arbor_store_agg_loc_at(uint8_t *wire, uint8_t level, uint16_t agg_loc_index) {
    const uint16_t encoded = htons(agg_loc_index);
    uint32_t off = kArborAggLocOffset;
    if (level == 1) off = kArborAggLoc1Offset;
    else if (level >= 2) off = kArborAggLoc2Offset;
    memcpy(wire + off, &encoded, sizeof(encoded));
}
static inline uint16_t arbor_load_agg_loc(const uint8_t *wire) { return arbor_load_agg_loc_at(wire, 0); }
static inline void arbor_store_agg_loc(uint8_t *wire, uint16_t agg_loc_index) { arbor_store_agg_loc_at(wire, 0, agg_loc_index); }
static inline uint8_t arbor_load_fanin_at(const uint8_t *wire, uint8_t level) {
    if (level >= ARBOR_MAX_STACK_DEPTH) return 0;
    return wire[kArborFaninOffset + level];
}
static inline void arbor_store_fanin_at(uint8_t *wire, uint8_t level, uint8_t fanin) {
    if (level >= ARBOR_MAX_STACK_DEPTH) return;
    wire[kArborFaninOffset + level] = fanin;
}
static inline uint8_t arbor_load_fanin(const uint8_t *wire) { return arbor_load_fanin_at(wire, 0); }
static inline void arbor_store_fanin(uint8_t *wire, uint8_t fanin) { arbor_store_fanin_at(wire, 0, fanin); }
static inline uint8_t arbor_load_dtype(const uint8_t *wire) { return wire[kArborDtypeOffset]; }
static inline void arbor_store_dtype(uint8_t *wire, uint8_t dtype) { wire[kArborDtypeOffset] = dtype; }
static inline uint32_t arbor_protocol_sequence_add(uint32_t base, uint32_t delta) { return (base + delta) & ARBOR_SEQUENCE_MASK; }
static inline uint32_t arbor_protocol_sequence_distance(uint32_t lhs, uint32_t rhs) { return (lhs - rhs) & ARBOR_SEQUENCE_MASK; }
static inline int arbor_protocol_sequence_contains(uint32_t start, uint32_t length, uint32_t packet_sequence) { return arbor_protocol_sequence_distance(packet_sequence, start) < length; }

typedef struct {
    arbor_packet_type_t packet_type;
    int repair;
    uint8_t op;
    uint8_t message_id;
    int payload_valid;
    arbor_payload_kind_t payload_kind;
    int credit_valid;
    uint8_t agg_depth;
    int aggregated;
    uint32_t offset_a;
    uint32_t offset_b;
    uint16_t agg_locs[ARBOR_MAX_STACK_DEPTH];
    uint8_t fanins[ARBOR_MAX_STACK_DEPTH];
    uint8_t dtype;
} arbor_header_view_t;

static inline arbor_header_view_t arbor_parse_header(const uint8_t *wire) {
    arbor_header_view_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    const uint8_t byte0 = arbor_load_byte0(wire);
    hdr.packet_type = arbor_get_packet_type(byte0);
    hdr.repair = arbor_get_repair(byte0);
    hdr.op = arbor_get_operation(byte0);
    hdr.message_id = arbor_load_message_id(wire);
    const uint8_t ctrl = arbor_load_ctrl(wire);
    hdr.payload_valid = arbor_get_payload_valid(ctrl);
    hdr.payload_kind = arbor_get_payload_kind(ctrl);
    hdr.credit_valid = arbor_get_credit_valid(ctrl);
    hdr.agg_depth = arbor_get_agg_depth(ctrl);
    hdr.aggregated = arbor_get_aggregated(ctrl);
    hdr.offset_a = arbor_load_offset_a(wire);
    hdr.offset_b = arbor_load_offset_b(wire);
    for (uint8_t i = 0; i < ARBOR_MAX_STACK_DEPTH; ++i) {
        hdr.agg_locs[i] = arbor_load_agg_loc_at(wire, i);
        hdr.fanins[i] = arbor_load_fanin_at(wire, i);
    }
    hdr.dtype = arbor_load_dtype(wire);
    return hdr;
}

static inline void arbor_write_header(uint8_t *wire, const arbor_header_view_t *hdr) {
    arbor_store_byte0(wire, arbor_make_byte0(hdr->packet_type, hdr->repair, hdr->op));
    arbor_store_message_id(wire, hdr->message_id);
    arbor_store_ctrl(wire, arbor_make_ctrl(hdr->payload_valid, hdr->payload_kind, hdr->credit_valid, hdr->agg_depth, hdr->aggregated));
    arbor_store_offset_a(wire, hdr->offset_a);
    arbor_store_offset_b(wire, hdr->offset_b);
    for (uint8_t i = 0; i < ARBOR_MAX_STACK_DEPTH; ++i) {
        arbor_store_agg_loc_at(wire, i, hdr->agg_locs[i]);
        arbor_store_fanin_at(wire, i, hdr->fanins[i]);
    }
    wire[kArborReservedOffset] = 0;
    arbor_store_dtype(wire, hdr->dtype);
}

static inline uint8_t arbor_legacy_msg_from_wire(const arbor_header_view_t *hdr) {
    if (!hdr) return ARBOR_MSG_REQUEST;
    switch (hdr->packet_type) {
        case ARBOR_PKT_REGISTER: return ARBOR_MSG_REGISTER;
        case ARBOR_PKT_DATA_REQUEST: return hdr->repair ? ARBOR_MSG_REPAIR_REQUEST : ARBOR_MSG_REQUEST;
        case ARBOR_PKT_RESPONSE: return ARBOR_MSG_RESPONSE;
        case ARBOR_PKT_END: return ARBOR_MSG_END;
        case ARBOR_PKT_END_ACK: return ARBOR_MSG_END_ACK;
        case ARBOR_PKT_REGISTER_ACK: return ARBOR_MSG_REGISTER_ACK;
        case ARBOR_PKT_AGG_MISS: return ARBOR_MSG_AGG_MISS;
        default: return ARBOR_MSG_REQUEST;
    }
}

static inline arbor_packet_type_t arbor_packet_type_from_legacy_msg(uint8_t msg_type) {
    switch (msg_type) {
        case ARBOR_MSG_REGISTER: return ARBOR_PKT_REGISTER;
        case ARBOR_MSG_REQUEST: return ARBOR_PKT_DATA_REQUEST;
        case ARBOR_MSG_REPAIR_REQUEST: return ARBOR_PKT_DATA_REQUEST;
        case ARBOR_MSG_RESPONSE: return ARBOR_PKT_RESPONSE;
        case ARBOR_MSG_END: return ARBOR_PKT_END;
        case ARBOR_MSG_END_ACK: return ARBOR_PKT_END_ACK;
        case ARBOR_MSG_REGISTER_ACK: return ARBOR_PKT_REGISTER_ACK;
        case ARBOR_MSG_AGG_MISS:
        case ARBOR_MSG_REPAIR_TRIGGER: return ARBOR_PKT_AGG_MISS;
        default: return ARBOR_PKT_DATA_REQUEST;
    }
}
static inline void arbor_rewrite_to_agg_miss(uint8_t *arbor_hdr) {
    uint8_t byte0 = arbor_load_byte0(arbor_hdr);
    arbor_set_packet_type(&byte0, ARBOR_PKT_AGG_MISS);
    arbor_set_repair(&byte0, 0);
    arbor_store_byte0(arbor_hdr, byte0);
    arbor_store_ctrl(arbor_hdr, arbor_make_ctrl(0, ARBOR_PAYLOAD_DATA, 0, 0, 0));
    arbor_store_offset_b(arbor_hdr, 0);
    for (uint8_t i = 0; i < ARBOR_MAX_STACK_DEPTH; ++i) {
        arbor_store_fanin_at(arbor_hdr, i, 0);
        arbor_store_agg_loc_at(arbor_hdr, i, AGG_INDEX_UNUSED);
    }
}

int build_frame_ex(uint8_t *buf,
                   uint32_t src_ip, uint32_t dst_ip,
                   uint8_t msg_type, uint8_t flags,
                   uint32_t channel_id, uint32_t subchannel_id,
                   uint32_t credit_offset, uint32_t payload_offset,
                   uint8_t agg_depth, const uint32_t *agg_stack,
                   const uint8_t *fanin_vec, uint8_t request_kind,
                   const void *payload, uint16_t plen);
arbor_router_action_t classify_router_request(uint8_t msg_type, uint8_t agg_depth,
                                              uint8_t request_kind, int slot_match,
                                              int slot_valid, uint32_t credit_offset,
                                              uint32_t slot_credit_offset);
int router_encode_aggregated_request_for_test(uint8_t *frame,
                                              uint32_t responder_ip,
                                              uint32_t channel_id,
                                              uint32_t subchannel_id,
                                              uint32_t credit_offset,
                                              uint32_t payload_offset,
                                              uint8_t agg_depth,
                                              const uint32_t *agg_stack,
                                              const uint8_t *fanin_vec,
                                              uint8_t request_kind,
                                              const int32_t *payload_words);
uint16_t mtp_udp_port(uint32_t channel_id, uint32_t subchannel_id);
int mtp_udp_port_in_range(uint16_t udp_port);
uint32_t mtp_port_to_channel(uint16_t udp_port);
uint32_t mtp_port_to_group(uint16_t udp_port);
uint32_t mtp_port_to_subchannel(uint16_t udp_port);

#endif
