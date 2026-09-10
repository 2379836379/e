#include "protocol/host_priv.h"
#include "arbor_fabric.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pcap_t *host_open_pcap(const char *dev_name, char *errbuf, int direction) {
    pcap_t *handle = pcap_create(dev_name, errbuf);
    int rc;
    if (!handle) return NULL;
    if ((rc = pcap_set_snaplen(handle, DEV_BUF_SIZE)) != 0 ||
        (rc = pcap_set_promisc(handle, 1)) != 0 ||
        (rc = pcap_set_timeout(handle, 10)) != 0 ||
        (rc = pcap_set_buffer_size(handle, PCAP_BUFFER_SIZE)) != 0) {
        fprintf(stderr, "[host-pcap-config-error] dev=%s rc=%d err=%s\n", dev_name, rc, pcap_geterr(handle));
        pcap_close(handle);
        return NULL;
    }
    rc = pcap_activate(handle);
    if (rc < 0) {
        fprintf(stderr, "[host-pcap-activate-error] dev=%s rc=%d err=%s\n", dev_name, rc, pcap_geterr(handle));
        pcap_close(handle);
        return NULL;
    }
    if (direction != 0 && pcap_setdirection(handle, direction) != 0) {
        fprintf(stderr, "[host-pcap-direction-warning] dev=%s err=%s\n", dev_name, pcap_geterr(handle));
    }
    return handle;
}
#include <sys/ioctl.h>
#include <net/if.h>
#include <sys/socket.h>

typedef struct {
    int iface_index;
} host_rx_arg_t;

static pthread_t g_host_rx_tids[SUBCHANNEL_COUNT];
static host_rx_arg_t g_host_rx_args[SUBCHANNEL_COUNT];
static int g_host_rx_started = 0;

config_entry_t g_cfg[MAX_GROUP_SIZE];
int g_n = 0;
int g_rank = -1;
uint32_t g_my_ip = 0;
pcap_t *g_host_handles[SUBCHANNEL_COUNT] = {0};
pcap_t *g_host_tx_handles[SUBCHANNEL_COUNT] = {0};
pthread_mutex_t g_tx_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t g_host_iface_macs[SUBCHANNEL_COUNT][6] = {{0}};

static void load_iface_mac(const char *iface, uint8_t mac[6]) {
    int fd;
    struct ifreq ifr;

    if (!iface || !mac) return;
    memset(mac, 0, 6);
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFHWADDR, &ifr) == 0) {
        memcpy(mac, (uint8_t *)ifr.ifr_hwaddr.sa_data, 6);
    }
    close(fd);
}
conn_t g_conns[MAX_CONNS];
int g_conn_count = 0;
static host_channel_state_t g_channel_states[MAX_CHANNELS];
static int g_channel_state_count = 0;

host_channel_state_t *find_channel_state(uint32_t channel_id) {
    for (int i = 0; i < g_channel_state_count; i++) {
        if (g_channel_states[i].channel.active && g_channel_states[i].channel.channel_id == channel_id) {
            return &g_channel_states[i];
        }
    }
    return NULL;
}

channel_ctx_t *find_channel(uint32_t channel_id) {
    host_channel_state_t *state = find_channel_state(channel_id);
    return state ? &state->channel : NULL;
}

subchannel_ctx_t *find_subchannel(uint32_t channel_id, uint32_t subchannel_id) {
    host_channel_state_t *state;
    if (subchannel_id >= SUBCHANNEL_COUNT) return NULL;
    state = find_channel_state(channel_id);
    if (!state) return NULL;
    return state->subchannels[subchannel_id].active ? &state->subchannels[subchannel_id] : NULL;
}

subchannel_ctx_t *find_subchannel_by_port(uint16_t udp_port) {
    for (int c = 0; c < g_channel_state_count; ++c) {
        for (uint32_t s = 0; s < SUBCHANNEL_COUNT; ++s) {
            subchannel_ctx_t *sc = &g_channel_states[c].subchannels[s];
            if (sc->active && sc->udp_port == udp_port) return sc;
        }
    }
    return NULL;
}

conn_t *find_conn_by_remote(uint32_t remote_ip) {
    for (int i = 0; i < g_conn_count; i++) {
        if (g_conns[i].in_use && g_conns[i].local_ip == g_my_ip && g_conns[i].remote_ip == remote_ip) {
            return &g_conns[i];
        }
    }
    return NULL;
}

void register_local_source(uint32_t channel_id, const void *buf, uint32_t size, uint8_t op) {
    host_channel_state_t *state = find_channel_state(channel_id);
    if (!state) return;
    state->local_src_buf = (const uint8_t *)buf;
    state->local_src_npkts = size / PAYLOAD_LEN;
    state->local_src_op = op;
}

void clear_local_source(uint32_t channel_id) {
    host_channel_state_t *state = find_channel_state(channel_id);
    if (!state) return;
    state->local_src_buf = NULL;
    state->local_src_npkts = 0;
    state->local_src_op = 0;
}

void register_request_result(uint32_t channel_id, void *buf, uint32_t size) {
    host_channel_state_t *state = find_channel_state(channel_id);
    if (!state) return;
    state->request_result_buf = (uint8_t *)buf;
    state->request_result_npkts = size / PAYLOAD_LEN;
}

void clear_request_result(uint32_t channel_id) {
    host_channel_state_t *state = find_channel_state(channel_id);
    if (!state) return;
    state->request_result_buf = NULL;
    state->request_result_npkts = 0;
}

static protocol_message_t *message_by_id(protocol_message_t queue[MAX_ACTIVE_MESSAGES], uint8_t message_id) {
    return &queue[message_id];
}

static int protocol_sequence_ranges_overlap(uint32_t lhs_start, uint32_t lhs_length,
                                         uint32_t rhs_start, uint32_t rhs_length) {
    return arbor_protocol_sequence_contains(lhs_start, lhs_length, rhs_start) ||
           arbor_protocol_sequence_contains(rhs_start, rhs_length, lhs_start);
}

int reserve_protocol_sequence(uint32_t channel_id, protocol_message_t *candidate,
                              uint32_t packet_count, uint32_t *start_sequence_out) {
    host_channel_state_t *state = find_channel_state(channel_id);
    if (!state || !candidate || !start_sequence_out ||
        packet_count == 0 || packet_count > (ARBOR_SEQUENCE_MASK >> 1)) {
        return -1;
    }
    uint32_t current = state->request_next_sequence & ARBOR_SEQUENCE_MASK;
    for (uint16_t id = 0; id < MAX_ACTIVE_MESSAGES; ++id) {
        protocol_message_t *live = &state->request_messages[id];
        if (live == candidate) continue;
        if (!live->in_use || !live->sequence_reserved) continue;
        const uint32_t live_age = arbor_protocol_sequence_distance(current, live->start_sequence);
        if (live_age >= (ARBOR_SEQUENCE_MASK >> 1)) return -1;
        if (protocol_sequence_ranges_overlap(current, packet_count, live->start_sequence, live->total_packets)) {
            return -1;
        }
    }
    *start_sequence_out = current;
    state->request_next_sequence = arbor_protocol_sequence_add(current, packet_count);
    candidate->start_sequence = current;
    candidate->epoch = current & ARBOR_SEQUENCE_MASK;
    candidate->sequence_reserved = 1;
    return 0;
}

int rollback_protocol_sequence(uint32_t channel_id, uint32_t start_sequence, uint32_t packet_count) {
    host_channel_state_t *state = find_channel_state(channel_id);
    uint32_t expected;
    if (!state || packet_count == 0) return 0;
    expected = arbor_protocol_sequence_add(start_sequence, packet_count);
    if ((state->request_next_sequence & ARBOR_SEQUENCE_MASK) != expected) return 0;
    state->request_next_sequence = start_sequence & ARBOR_SEQUENCE_MASK;
    return 1;
}

static protocol_message_t *find_message_for_sequence(protocol_message_t queue[MAX_ACTIVE_MESSAGES],
                                                     uint8_t *lookup_hint,
                                                     uint32_t packet_sequence,
                                                     uint32_t *local_offset_out) {
    if (!queue || !lookup_hint) return NULL;
    protocol_message_t *hint = &queue[*lookup_hint];
    if (hint->in_use && hint->sequence_reserved &&
        arbor_protocol_sequence_contains(hint->start_sequence, hint->total_packets, packet_sequence)) {
        if (local_offset_out) {
            *local_offset_out = arbor_protocol_sequence_distance(packet_sequence, hint->start_sequence);
        }
        return hint;
    }
    uint8_t next_id = (uint8_t)(*lookup_hint + 1u);
    protocol_message_t *next = &queue[next_id];
    if (next->in_use && next->sequence_reserved &&
        arbor_protocol_sequence_contains(next->start_sequence, next->total_packets, packet_sequence)) {
        *lookup_hint = next_id;
        if (local_offset_out) {
            *local_offset_out = arbor_protocol_sequence_distance(packet_sequence, next->start_sequence);
        }
        return next;
    }
    for (uint16_t id = 0; id < MAX_ACTIVE_MESSAGES; ++id) {
        protocol_message_t *meta = &queue[id];
        if (!meta->in_use || !meta->sequence_reserved) continue;
        if (!arbor_protocol_sequence_contains(meta->start_sequence, meta->total_packets, packet_sequence)) continue;
        *lookup_hint = (uint8_t)id;
        if (local_offset_out) {
            *local_offset_out = arbor_protocol_sequence_distance(packet_sequence, meta->start_sequence);
        }
        return meta;
    }
    return NULL;
}

protocol_message_t *request_message_by_id(uint32_t channel_id, uint8_t message_id) {
    host_channel_state_t *state = find_channel_state(channel_id);
    if (!state) return NULL;
    return message_by_id(state->request_messages, message_id);
}

protocol_message_t *response_message_by_id(uint32_t channel_id, uint8_t message_id) {
    host_channel_state_t *state = find_channel_state(channel_id);
    if (!state) return NULL;
    return message_by_id(state->response_messages, message_id);
}

protocol_message_t *find_request_message_for_sequence(uint32_t channel_id, uint32_t packet_sequence,
                                                      uint32_t *local_offset_out) {
    host_channel_state_t *state = find_channel_state(channel_id);
    if (!state) return NULL;
    return find_message_for_sequence(state->request_messages,
                                     &state->request_message_lookup_hint,
                                     packet_sequence, local_offset_out);
}

protocol_message_t *find_response_message_for_sequence(uint32_t channel_id, uint32_t packet_sequence,
                                                       uint32_t *local_offset_out) {
    host_channel_state_t *state = find_channel_state(channel_id);
    if (!state) return NULL;
    return find_message_for_sequence(state->response_messages,
                                     &state->response_message_lookup_hint,
                                     packet_sequence, local_offset_out);
}

protocol_message_t *acquire_request_message(uint32_t channel_id, uint32_t total_packets) {
    host_channel_state_t *state = find_channel_state(channel_id);
    if (!state || total_packets == 0 || total_packets > ARBOR_SEQUENCE_MASK) return NULL;
    uint8_t start = state->request_next_message_id;
    for (uint16_t i = 0; i < MAX_ACTIVE_MESSAGES; ++i) {
        uint8_t id = (uint8_t)(start + i);
        uint32_t start_sequence = 0;
        protocol_message_t *meta = &state->request_messages[id];
        if (meta->in_use || meta->sequence_reserved) continue;
        memset(meta, 0, sizeof(*meta));
        meta->in_use = 1;
        meta->message_id = id;
        meta->total_packets = total_packets;
        if (reserve_protocol_sequence(channel_id, meta, total_packets, &start_sequence) != 0) {
            memset(meta, 0, sizeof(*meta));
            return NULL;
        }
        meta->start_sequence = start_sequence;
        meta->epoch = start_sequence & ARBOR_SEQUENCE_MASK;
        state->request_next_message_id = (uint8_t)(id + 1u);
        state->request_message_lookup_hint = id;
        return meta;
    }
    return NULL;
}

protocol_message_t *upsert_response_message(uint32_t channel_id, uint8_t message_id,
                                            uint32_t start_sequence, uint32_t total_packets) {
    host_channel_state_t *state = find_channel_state(channel_id);
    if (!state || total_packets == 0) return NULL;
    protocol_message_t *meta = &state->response_messages[message_id];
    if (!meta->in_use || meta->epoch != (start_sequence & ARBOR_SEQUENCE_MASK)) {
        memset(meta, 0, sizeof(*meta));
    }
    meta->in_use = 1;
    meta->message_id = message_id;
    meta->start_sequence = start_sequence & ARBOR_SEQUENCE_MASK;
    meta->epoch = start_sequence & ARBOR_SEQUENCE_MASK;
    meta->total_packets = total_packets;
    meta->sequence_reserved = 1;
    meta->reuse_ready = 0;
    state->response_message_lookup_hint = message_id;
    return meta;
}

void host_inject(uint8_t *frame, int len) {
    host_inject_on_subchannel(0, frame, len);
}

void host_inject_on_subchannel(uint32_t subchannel_id, uint8_t *frame, int len) {
    uint32_t idx = subchannel_id < SUBCHANNEL_COUNT ? subchannel_id : 0;
    pcap_t *handle = g_host_tx_handles[idx] ? g_host_tx_handles[idx] :
                     (g_host_tx_handles[0] ? g_host_tx_handles[0] :
                      (g_host_handles[idx] ? g_host_handles[idx] : g_host_handles[0]));
    if (!handle) return;
    if (len >= (int)sizeof(eth_header_t)) {
        eth_header_t *eth = (eth_header_t *)frame;
        memcpy(eth->src_mac, g_host_iface_macs[idx], 6);
    }

    pthread_mutex_lock(&g_tx_lock);
    int rc = pcap_inject(handle, frame, len);
    if (rc < 0) {
        fprintf(stderr, "[host-inject-error] rank=%d sub=%u len=%d err=%s\n",
                g_rank, idx, len, pcap_geterr(handle));
    }
    pthread_mutex_unlock(&g_tx_lock);
}

static void *host_rx_thread(void *arg) {
    host_rx_arg_t *rx = (host_rx_arg_t *)arg;
    pcap_t *handle = g_host_handles[rx->iface_index];
    fprintf(stderr, "[host-rx-start] rank=%d iface=%d handle=%p\n", g_rank, rx->iface_index, (void *)handle);
    struct pcap_pkthdr *hdr;
    const u_char *pkt;
    int rc;

    while ((rc = pcap_next_ex(handle, &hdr, &pkt)) >= 0) {
        if (g_rank >= 4) {
            fprintf(stderr, "[host-rx-raw] rank=%d iface=%d rc=%d caplen=%u\n", g_rank, rx->iface_index, rc, hdr ? (unsigned)hdr->caplen : 0u);
        }
        if (rc == 0 || hdr->caplen < (unsigned)HDR_LEN) continue;
        const eth_header_t *eth = (const eth_header_t *)pkt;
        if (ntohs(eth->ether_type) != ETH_TYPE_IP) continue;

        const ip_header_t *ip = (const ip_header_t *)(pkt + sizeof(eth_header_t));
        if (ip->protocol != ARBOR_IP_PROTO) continue;
        if (g_rank >= 4) {
            fprintf(stderr,
                    "[host-rx-ingress] rank=%d iface=%d src_rank=%d dst_rank=%d len=%u\n",
                    g_rank, rx->iface_index, rank_of_ip(ip->src_ip), rank_of_ip(ip->dst_ip),
                    (unsigned)hdr->caplen);
        }
        if (ip->src_ip == g_my_ip) continue;
        if (ip->dst_ip != g_my_ip) continue;

        int ip_ihl = (ip->version_ihl & 0x0f) * 4;
        const udp_header_t *udp = (const udp_header_t *)(pkt + sizeof(eth_header_t) + ip_ihl);
        const arbor_header_t *wire = (const arbor_header_t *)(pkt + sizeof(eth_header_t) + ip_ihl + sizeof(udp_header_t));
        arbor_header_view_t hdrv = arbor_parse_header((const uint8_t *)wire);

        int plen = ntohs(udp->len) - (int)sizeof(udp_header_t) - (int)sizeof(arbor_header_t);
        if (plen < 0) plen = 0;
        if (plen > PAYLOAD_LEN) plen = PAYLOAD_LEN;

        uint32_t channel_id = MAX_CHANNELS;
        uint32_t subchannel_id = 0;
        uint8_t legacy_msg_type;
        channel_ctx_t *ctx;
        conn_t *cn;
        const uint16_t udp_port = ntohs(udp->dst_port);
        if (!mtp_udp_port_in_range(udp_port)) continue;
        channel_id = mtp_port_to_channel(udp_port);
        subchannel_id = mtp_port_to_subchannel(udp_port);
        if (subchannel_id >= SUBCHANNEL_COUNT) continue;
        legacy_msg_type = arbor_legacy_msg_from_wire(&hdrv);
        if ((legacy_msg_type == ARBOR_MSG_REGISTER || legacy_msg_type == ARBOR_MSG_REGISTER_ACK) &&
            g_rank >= 4) {
            fprintf(stderr,
                    "[host-rx-register] rank=%d iface=%d msg_type=%u src_rank=%d dst_rank=%d sub=%u msg=%u src_ip=%u dst_ip=%u\n",
                    g_rank, rx->iface_index, (unsigned)legacy_msg_type,
                    rank_of_ip(ip->src_ip), rank_of_ip(ip->dst_ip), subchannel_id,
                    (unsigned)hdrv.message_id, ntohl(ip->src_ip), ntohl(ip->dst_ip));
        }
        if (channel_id >= MAX_CHANNELS) continue;
        ctx = find_channel(channel_id);
        if (!ctx) continue;
        switch (legacy_msg_type) {
            case ARBOR_MSG_REGISTER:
            case ARBOR_MSG_REQUEST:
            case ARBOR_MSG_REPAIR_REQUEST:
            case ARBOR_MSG_END_ACK:
                cn = &g_conns[ctx->recv_conn];
                break;
            default:
                cn = &g_conns[ctx->uplink_conn];
                break;
        }

        pthread_mutex_lock(&cn->lock);
        int nh = (cn->head + 1) % RXQ_SIZE;
        if (nh != cn->tail) {
            rx_msg_t *m = &cn->queue[cn->head];
            memset(m, 0, sizeof(*m));
            m->msg_type = legacy_msg_type;
            m->flags = ARBOR_FLAG_VALID;
            m->repair = hdrv.repair ? 1u : 0u;
            m->message_id = hdrv.message_id;
            if ((ip->tos & ARBOR_IPV4_ECN_MASK) == ARBOR_IPV4_ECN_CE) m->flags |= ARBOR_FLAG_ECN;
            if (hdrv.credit_valid) m->flags |= ARBOR_FLAG_CREDIT_VALID;
            if (hdrv.payload_valid && hdrv.packet_type == ARBOR_PKT_RESPONSE) m->flags |= ARBOR_FLAG_COMPLETION_VALID;
            m->src_ip = ip->src_ip;
            m->channel_id = channel_id;
            m->subchannel_id = subchannel_id;
            m->credit_offset = hdrv.offset_b;
            m->payload_offset = hdrv.offset_a;
            m->agg_depth = hdrv.agg_depth;
            if (!hdrv.payload_valid && hdrv.packet_type == ARBOR_PKT_DATA_REQUEST) {
                m->request_kind = ARBOR_REQ_AGGREGATE_CONTROL_ACK;
            } else if (hdrv.packet_type == ARBOR_PKT_DATA_REQUEST && hdrv.payload_valid) {
                m->request_kind = ARBOR_REQ_AGGREGATE_PAYLOAD;
            } else {
                m->request_kind = ARBOR_REQ_NONE;
            }
            for (uint8_t i = 0; i < ARBOR_MAX_STACK_DEPTH; ++i) {
                m->agg_stack[i] = (i < hdrv.agg_depth) ? hdrv.agg_locs[i] : 0;
                m->fanin[i] = (i < hdrv.agg_depth) ? hdrv.fanins[i] : 0;
            }
            m->seq_num = ntohs(ip->id);
            m->payload_len = (uint16_t)plen;
            if (plen > 0) {
                memcpy(m->payload,
                       pkt + sizeof(eth_header_t) + ip_ihl + sizeof(udp_header_t) + sizeof(arbor_header_t),
                       (size_t)plen);
            }
            if (legacy_msg_type == ARBOR_MSG_REGISTER || legacy_msg_type == ARBOR_MSG_REGISTER_ACK) {
                fprintf(stderr,
                        "[host-rx-register] rank=%d iface=%d ch=%u sub=%u msg_type=%u head=%d tail=%d src_rank=%d dst_rank=%d\n",
                        g_rank, rx->iface_index, channel_id, subchannel_id, (unsigned)legacy_msg_type,
                        cn->head, cn->tail, rank_of_ip(ip->src_ip), rank_of_ip(ip->dst_ip));
            } else if (legacy_msg_type == ARBOR_MSG_RESPONSE) {
                fprintf(stderr,
                        "[host-rx-response] rank=%d iface=%d ch=%u sub=%u msg=%u credit_valid=%u completion_valid=%u credit_off=%u payload_off=%u src_rank=%d dst_rank=%d head=%d tail=%d\n",
                        g_rank, rx->iface_index, channel_id, subchannel_id, (unsigned)hdrv.message_id,
                        hdrv.credit_valid ? 1u : 0u,
                        (hdrv.payload_valid && hdrv.packet_type == ARBOR_PKT_RESPONSE) ? 1u : 0u,
                        hdrv.offset_b, hdrv.offset_a,
                        rank_of_ip(ip->src_ip), rank_of_ip(ip->dst_ip), cn->head, cn->tail);
            } else if (legacy_msg_type == ARBOR_MSG_REQUEST || legacy_msg_type == ARBOR_MSG_REPAIR_REQUEST) {
                fprintf(stderr,
                        "[host-rx-request] rank=%d iface=%d ch=%u sub=%u msg_type=%u msg=%u depth=%u aggregated=%u payload_valid=%u payload_len=%d payload_off=%u credit_off=%u src_rank=%d dst_rank=%d head=%d tail=%d\n",
                        g_rank, rx->iface_index, channel_id, subchannel_id, (unsigned)legacy_msg_type,
                        (unsigned)hdrv.message_id, hdrv.agg_depth, hdrv.aggregated ? 1u : 0u,
                        hdrv.payload_valid ? 1u : 0u, plen, hdrv.offset_a, hdrv.offset_b,
                        rank_of_ip(ip->src_ip), rank_of_ip(ip->dst_ip),
                        cn->head, cn->tail);
            }
            cn->head = nh;
        } else {
            fprintf(stderr,
                    "[host-rx-drop] rank=%d ch=%u msg_type=%u sub=%u src_rank=%d dst_rank=%d head=%d tail=%d\n",
                    g_rank, channel_id, (unsigned)legacy_msg_type, subchannel_id,
                    rank_of_ip(ip->src_ip), rank_of_ip(ip->dst_ip), cn->head, cn->tail);
        }
        pthread_mutex_unlock(&cn->lock);
    }
    return NULL;
}

int conn_pop(conn_t *cn, rx_msg_t *out) {
    int ok = 0;
    pthread_mutex_lock(&cn->lock);
    if (cn->tail != cn->head) {
        *out = cn->queue[cn->tail];
        cn->tail = (cn->tail + 1) % RXQ_SIZE;
        ok = 1;
    }
    pthread_mutex_unlock(&cn->lock);
    return ok;
}

static int init_conn(uint32_t local_ip, uint32_t remote_ip) {
    if (g_conn_count >= MAX_CONNS) return -1;
    conn_t *cn = &g_conns[g_conn_count];
    memset(cn, 0, sizeof(*cn));
    cn->in_use = 1;
    cn->local_ip = local_ip;
    cn->remote_ip = remote_ip;
    pthread_mutex_init(&cn->lock, NULL);
    return g_conn_count++;
}

void init_host(config_entry_t *cfgs, int n, const char *host_name) {
    g_n = n;
    memcpy(g_cfg, cfgs, sizeof(config_entry_t) * (size_t)n);

    for (int i = 0; i < n; i++) {
        if (strcmp(cfgs[i].host_name, host_name) == 0) {
            g_rank = cfgs[i].rank;
            g_my_ip = cfgs[i].host_ip;
            break;
        }
    }
    if (g_rank < 0) {
        fprintf(stderr, "init_host: host %s not found\n", host_name);
        exit(1);
    }

    char errbuf[PCAP_ERRBUF_SIZE];
    for (uint32_t s = 0; s < SUBCHANNEL_COUNT; s++) {
        const char *iface = NULL;
        for (int i = 0; i < n; i++) {
            if (cfgs[i].rank == g_rank) iface = cfgs[i].host_ifaces[s];
        }
        g_host_handles[s] = host_open_pcap(iface, errbuf, PCAP_D_IN);
        if (!g_host_handles[s]) {
            fprintf(stderr, "pcap_open_live(%s) failed: %s\n", iface, errbuf);
            exit(1);
        }
        g_host_tx_handles[s] = host_open_pcap(iface, errbuf, 0);
        if (!g_host_tx_handles[s]) {
            fprintf(stderr, "pcap_open_live(tx:%s) failed: %s\n", iface, errbuf);
            exit(1);
        }
        load_iface_mac(iface, g_host_iface_macs[s]);
        g_host_rx_args[s].iface_index = (int)s;
    }
    printf("[host] %s rank=%d ready on %u subchannels\n", host_name, g_rank, SUBCHANNEL_COUNT);
    fflush(stdout);
}

void start_host_rx(void) {
    if (g_host_rx_started) return;
    for (uint32_t s = 0; s < SUBCHANNEL_COUNT; s++) {
        if (!g_host_handles[s]) continue;
        pthread_create(&g_host_rx_tids[s], NULL, host_rx_thread, &g_host_rx_args[s]);
    }
    g_host_rx_started = 1;
}

int init_channel(uint32_t channel_id, uint32_t local_ip, uint32_t responder_ip) {
    if (g_channel_state_count >= MAX_CHANNELS) return -1;
    if (find_channel(channel_id)) return -1;

    int uplink_conn = init_conn(local_ip, responder_ip);
    int recv_conn = init_conn(local_ip, responder_ip);
    if (uplink_conn < 0 || recv_conn < 0) return -1;

    host_channel_state_t *state = &g_channel_states[g_channel_state_count++];
    channel_ctx_t *ctx;
    memset(state, 0, sizeof(*state));
    ctx = &state->channel;
    ctx->active = 1;
    ctx->channel_id = channel_id;
    ctx->uplink_conn = uplink_conn;
    ctx->recv_conn = recv_conn;
    ctx->local_ip = local_ip;
    ctx->responder_ip = responder_ip;

    for (uint32_t s = 0; s < SUBCHANNEL_COUNT; s++) {
        subchannel_ctx_t *sc = &state->subchannels[s];
        memset(sc, 0, sizeof(*sc));
        sc->active = 1;
        sc->channel_id = channel_id;
        sc->subchannel_id = s;
        sc->udp_port = mtp_udp_port(channel_id, s);
        sc->iface_index = (int)s;
    }
    return 0;
}
