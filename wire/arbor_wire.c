#include "wire/arbor_wire.h"

static uint16_t ip_checksum(const void *data, int len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    int i;
    for (i = 0; i + 1 < len; i += 2)
        sum += (uint16_t)((p[i] << 8) | p[i + 1]);
    if (len & 1)
        sum += (uint16_t)(p[len - 1] << 8);
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)(~sum & 0xffff);
}

static arbor_packet_type_t legacy_msg_type_to_packet_type(uint8_t msg_type) {
    switch (msg_type) {
        case ARBOR_MSG_REGISTER: return ARBOR_PKT_REGISTER;
        case ARBOR_MSG_REQUEST: return ARBOR_PKT_DATA_REQUEST;
        case ARBOR_MSG_RESPONSE: return ARBOR_PKT_RESPONSE;
        case ARBOR_MSG_END: return ARBOR_PKT_END;
        case ARBOR_MSG_END_ACK: return ARBOR_PKT_END_ACK;
        case ARBOR_MSG_REGISTER_ACK: return ARBOR_PKT_REGISTER_ACK;
        case ARBOR_MSG_AGG_MISS: return ARBOR_PKT_AGG_MISS;
        case ARBOR_MSG_REPAIR_TRIGGER:
        case ARBOR_MSG_REPAIR_REQUEST:
        default:
            return ARBOR_PKT_DATA_REQUEST;
    }
}

uint16_t mtp_udp_port(uint32_t channel_id, uint32_t subchannel_id) {
    const uint32_t group_id = 0;
    const uint32_t encoded_subchannel = subchannel_id % MTP_MAX_SUBCHANNELS;
    return (uint16_t)(MTP_UDP_PORT_BASE +
                      ((group_id * MTP_MAX_RANKS_PER_GROUP) +
                       (channel_id % MTP_MAX_RANKS_PER_GROUP)) *
                          MTP_MAX_SUBCHANNELS +
                      encoded_subchannel);
}

int mtp_udp_port_in_range(uint16_t udp_port) {
    const uint32_t limit = MTP_UDP_PORT_BASE +
                           (MTP_MAX_GROUPS * MTP_MAX_RANKS_PER_GROUP * MTP_MAX_SUBCHANNELS);
    return udp_port >= MTP_UDP_PORT_BASE && (uint32_t)udp_port < limit;
}

uint32_t mtp_port_to_channel(uint16_t udp_port) {
    return ((uint32_t)(udp_port - MTP_UDP_PORT_BASE) / MTP_MAX_SUBCHANNELS) % MTP_MAX_RANKS_PER_GROUP;
}

uint32_t mtp_port_to_group(uint16_t udp_port) {
    return (uint32_t)(udp_port - MTP_UDP_PORT_BASE) /
           (MTP_MAX_SUBCHANNELS * MTP_MAX_RANKS_PER_GROUP);
}

uint32_t mtp_port_to_subchannel(uint16_t udp_port) {
    return (uint32_t)(udp_port - MTP_UDP_PORT_BASE) % MTP_MAX_SUBCHANNELS;
}

int build_frame_ex(uint8_t *buf,
                   uint32_t src_ip, uint32_t dst_ip,
                   uint8_t msg_type, uint8_t flags,
                   uint32_t channel_id, uint32_t subchannel_id,
                   uint32_t credit_offset, uint32_t payload_offset,
                   uint8_t agg_depth, const uint32_t *agg_stack, const uint8_t *fanin_vec, uint8_t request_kind,
                   const void *payload, uint16_t plen) {
    eth_header_t *eth = (eth_header_t *)buf;
    (void)request_kind;
    memset(eth->dst_mac, 0xff, 6);
    memset(eth->src_mac, 0x00, 6);
    eth->ether_type = htons(ETH_TYPE_IP);

    ip_header_t *ip = (ip_header_t *)(buf + sizeof(eth_header_t));
    ip->version_ihl = 0x45;
    ip->tos = ARBOR_IPV4_ECN_ECT0;
    ip->total_len = htons(sizeof(ip_header_t) + sizeof(udp_header_t) + sizeof(arbor_header_t) + plen);
    ip->id = htons((uint16_t)credit_offset);
    ip->frag_off = 0;
    ip->ttl = 64;
    ip->protocol = ARBOR_IP_PROTO;
    ip->checksum = 0;
    ip->src_ip = src_ip;
    ip->dst_ip = dst_ip;
    ip->checksum = htons(ip_checksum(ip, sizeof(ip_header_t)));

    udp_header_t *udp = (udp_header_t *)(buf + sizeof(eth_header_t) + sizeof(ip_header_t));
    udp->src_port = htons(mtp_udp_port(channel_id, subchannel_id));
    udp->dst_port = htons(mtp_udp_port(channel_id, subchannel_id));
    udp->len = htons(sizeof(udp_header_t) + sizeof(arbor_header_t) + plen);
    udp->checksum = 0;

    arbor_header_t *mtp = (arbor_header_t *)(buf + sizeof(eth_header_t) + sizeof(ip_header_t) + sizeof(udp_header_t));
    memset(mtp, 0, sizeof(*mtp));
    const arbor_packet_type_t pkt_type = legacy_msg_type_to_packet_type(msg_type);
    const int repair = (flags & 0x1U) != 0;
    const uint8_t op = (uint8_t)(OP_ALLREDUCE & 0x3U);
    const uint8_t depth = agg_depth > ARBOR_MAX_STACK_DEPTH ? ARBOR_MAX_STACK_DEPTH : agg_depth;
    const int payload_valid = plen > 0;
    const arbor_payload_kind_t payload_kind = payload_valid ? ARBOR_PAYLOAD_DATA : ARBOR_PAYLOAD_COMPLETION;
    const int credit_valid = (flags & 0x4U) != 0;
    const int aggregated = 0;

    arbor_store_byte0((uint8_t *)mtp, arbor_make_byte0(pkt_type, repair, op));
    arbor_store_message_id((uint8_t *)mtp, 0);
    arbor_store_ctrl((uint8_t *)mtp, arbor_make_ctrl(payload_valid, payload_kind, credit_valid, depth, aggregated));
    arbor_store_request_offset((uint8_t *)mtp, payload_offset);
    arbor_store_credit_offset((uint8_t *)mtp, credit_valid ? credit_offset : 0);
    for (uint8_t i = 0; i < ARBOR_MAX_STACK_DEPTH; ++i) {
        arbor_store_agg_loc_at((uint8_t *)mtp, i, (agg_stack && i < depth) ? (uint16_t)(agg_stack[i] & 0xFFFFU) : AGG_INDEX_UNUSED);
        arbor_store_fanin_at((uint8_t *)mtp, i, (fanin_vec && i < depth) ? fanin_vec[i] : 0);
    }
    ((uint8_t *)mtp)[kArborReservedOffset] = 0;
    arbor_store_dtype((uint8_t *)mtp, payload_valid ? ARBOR_DTYPE_FLOAT32 : 0);

    if (plen > 0 && payload)
        memcpy(buf + HDR_LEN, payload, plen);

    return (int)(HDR_LEN + plen);
}

arbor_router_action_t classify_router_request(uint8_t msg_type, uint8_t agg_depth,
                                              uint8_t request_kind, int slot_match,
                                              int slot_valid, uint32_t credit_offset,
                                              uint32_t slot_credit_offset) {
    (void)request_kind;
    (void)slot_match;
    (void)slot_valid;
    (void)credit_offset;
    (void)slot_credit_offset;
    if (msg_type != ARBOR_MSG_REQUEST) return ARBOR_ROUTER_ACT_BYPASS;
    if (agg_depth == 0) return ARBOR_ROUTER_ACT_BYPASS;
    return ARBOR_ROUTER_ACT_AGGREGATE;
}

int router_encode_aggregated_request_for_test(uint8_t *frame, uint32_t responder_ip,
                                              uint32_t channel_id, uint32_t subchannel_id,
                                              uint32_t credit_offset, uint32_t payload_offset,
                                              uint8_t agg_depth, const uint32_t *agg_stack,
                                              const uint8_t *fanin_vec, uint8_t request_kind,
                                              const int32_t *payload_words) {
    const void *payload = NULL;
    uint16_t plen = 0;
    if (request_kind == ARBOR_REQ_AGGREGATE_PAYLOAD) {
        payload = payload_words;
        plen = PAYLOAD_LEN;
    }
    return build_frame_ex(frame, 0, responder_ip,
                          ARBOR_MSG_REQUEST, ARBOR_FLAG_VALID,
                          channel_id, subchannel_id,
                          credit_offset, payload_offset,
                          agg_depth, agg_stack, fanin_vec,
                          request_kind, payload, plen);
}
