#include "protocol/host_priv.h"
#include "arbor_fabric.h"

#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    uint8_t owner_valid;
    uint8_t forwarded;
    uint8_t fanin;
    uint8_t agg_count;
    uint8_t op;
    uint8_t dtype;
    uint16_t owner_udp_port;
    uint32_t owner_offset;
    uint16_t agg_loc;
    uint16_t payload_len;
    uint32_t responder_ip;
    uint8_t master_frame[HDR_LEN + PAYLOAD_LEN];
    uint32_t master_len;
} router_slot_t;

static net_device_t g_devs[16];
static int g_dev_count = 0;
static int g_dev_rr = 0;
static agtr_t *g_agtr = NULL;
static router_slot_t g_slots[AGTR_ARRAY_SIZE];
static const ArborRouterNodeConfig *g_router_topology = NULL;

static uint64_t g_router_register_bypass = 0;
static uint64_t g_router_response_bind = 0;
static uint64_t g_router_request_aggregate = 0;
static uint64_t g_router_request_bypass = 0;
static uint64_t g_router_request_complete = 0;
static uint64_t g_router_other_bypass = 0;
static uint64_t g_router_next_progress_log = 0;
static uint32_t g_dbg_router_a_rank3_to_0_tx = 0;
static uint32_t g_dbg_router_a0_rank3_to_0_rx = 0;

config_entry_t g_cfg[MAX_GROUP_SIZE] __attribute__((weak));
static uint32_t g_router_alloc_ptr = 0;

static pcap_t *router_open_pcap(const char *dev_name, char *errbuf, int direction) {
    pcap_t *handle = pcap_create(dev_name, errbuf);
    int rc;
    if (!handle) return NULL;
    if ((rc = pcap_set_snaplen(handle, DEV_BUF_SIZE)) != 0 ||
        (rc = pcap_set_promisc(handle, 1)) != 0 ||
        (rc = pcap_set_timeout(handle, 10)) != 0 ||
        (rc = pcap_set_buffer_size(handle, PCAP_BUFFER_SIZE)) != 0) {
        fprintf(stderr, "[router-pcap-config-error] dev=%s rc=%d err=%s\n", dev_name, rc, pcap_geterr(handle));
        pcap_close(handle);
        return NULL;
    }
    rc = pcap_activate(handle);
    if (rc < 0) {
        fprintf(stderr, "[router-pcap-activate-error] dev=%s rc=%d err=%s\n", dev_name, rc, pcap_geterr(handle));
        pcap_close(handle);
        return NULL;
    }
    if (direction != 0 && pcap_setdirection(handle, direction) != 0) {
        fprintf(stderr, "[router-pcap-direction-warning] dev=%s err=%s\n", dev_name, pcap_geterr(handle));
    }
    return handle;
}

static int router_port_matches_subchannel(const char *, uint32_t);
static void log_register_wire(const char *, const char *, const char *, const uint8_t *, int);
static int router_tree_is_level(const ArborRouterNodeConfig *, int, uint32_t);
static void slot_reset_for_credit(router_slot_t *, uint16_t, uint32_t, uint16_t, uint32_t);
static uint16_t find_slot_for_credit(uint16_t, uint32_t, uint32_t);
static void credit_push_agg_loc(uint8_t *, uint16_t);
static int router_port_is_child_uplink(const char *);

static void dump_router_stats(void) {
    fprintf(stderr,
            "[router-summary] register_bypass=%llu response_bind=%llu request_aggregate=%llu request_bypass=%llu request_complete=%llu other_bypass=%llu\n",
            (unsigned long long)g_router_register_bypass,
            (unsigned long long)g_router_response_bind,
            (unsigned long long)g_router_request_aggregate,
            (unsigned long long)g_router_request_bypass,
            (unsigned long long)g_router_request_complete,
            (unsigned long long)g_router_other_bypass);
    uint64_t total_drops = 0;
    for (int i = 0; i < g_dev_count; ++i) {
        struct pcap_stat ps;
        memset(&ps, 0, sizeof(ps));
        total_drops += g_devs[i].rx_ring_drops;
        (void)pcap_stats(g_devs[i].handle, &ps);
        fprintf(stderr,
                "[router-rx-port-summary] dev=%s rx=%llu tx=%llu ring_drops=%llu pcap_recv=%u pcap_drop=%u pcap_ifdrop=%u\n",
                g_devs[i].name,
                (unsigned long long)g_devs[i].rx_packets,
                (unsigned long long)g_devs[i].tx_packets,
                (unsigned long long)g_devs[i].rx_ring_drops,
                ps.ps_recv, ps.ps_drop, ps.ps_ifdrop);
    }
    fprintf(stderr, "[router-rx-summary] ring_drops=%llu\n",
            (unsigned long long)total_drops);
}

static void inject_on_port(const char *out_port, const uint8_t *frame, int len, int rank, uint32_t subchannel_id) {
    int i;
    if (!out_port) return;
    for (i = 0; i < g_dev_count; i++) {
        if (strcmp(g_devs[i].name, out_port) == 0) {
            pcap_t *tx = g_devs[i].tx_handle ? g_devs[i].tx_handle : g_devs[i].handle;
            if (g_router_topology && strcmp(ArborRouterNodeName(g_router_topology), "router-a") == 0 &&
                strcmp(out_port, "ra-a0-down") == 0 && rank == 0 && g_dbg_router_a_rank3_to_0_tx < 8) {
                const ip_header_t *dbg_ip = (const ip_header_t *)(frame + sizeof(eth_header_t));
                if (rank_of_ip(dbg_ip->src_ip) == 3) {
                    ++g_dbg_router_a_rank3_to_0_tx;
                    fprintf(stderr, "[dbg-ra-tx] count=%u out=%s src_rank=%d dst_rank=%d sub=%u len=%d\n",
                            g_dbg_router_a_rank3_to_0_tx, out_port, rank_of_ip(dbg_ip->src_ip),
                            rank_of_ip(dbg_ip->dst_ip), subchannel_id, len);
                }
            }
            int rc = pcap_inject(tx, frame, len);
            if (rc == len) ++g_devs[i].tx_packets;
            if (rc != len) {
                fprintf(stderr,
                        "[router-inject-error] router=%s out=%s dst_rank=%d sub=%u len=%d rc=%d err=%s\n",
                        g_router_topology ? ArborRouterNodeName(g_router_topology) : "?", out_port,
                        rank, subchannel_id, len, rc, pcap_geterr(tx));
            }
            return;
        }
    }
}

static void router_forward(const uint8_t *frame, int len, uint32_t dst_ip, uint32_t subchannel_id) {
    const char *out_port = NULL;
    int rank = rank_of_ip(dst_ip);
    if (!g_router_topology || rank < 0 || rank >= MAX_GROUP_SIZE) return;
    if (subchannel_id >= SUBCHANNEL_COUNT) subchannel_id = 0;
    out_port = ArborRouterNodeRoutePort(g_router_topology, rank, subchannel_id);
    inject_on_port(out_port, frame, len, rank, subchannel_id);
}

static void router_flood_except(const uint8_t *frame, int len, const char *ingress_port, uint32_t subchannel_id) {
    if (!g_router_topology) return;
    if (subchannel_id >= SUBCHANNEL_COUNT) subchannel_id = 0;
    for (int i = 0; i < g_dev_count; ++i) {
        const char *out_port = g_devs[i].name;
        if (!router_port_matches_subchannel(out_port, subchannel_id)) continue;
        inject_on_port(out_port, frame, len, -1, subchannel_id);
    }
}

static void log_register_flood(const char *router_name, const char *ingress_port,
                               const char *egress_port, arbor_packet_type_t packet_type,
                               uint32_t src_ip, uint32_t dst_ip, uint32_t subchannel_id,
                               uint8_t message_id) {
    if (!router_name || !ingress_port || !egress_port) return;
    if (strcmp(router_name, "router-root") != 0) return;
    if (packet_type != ARBOR_PKT_REGISTER && packet_type != ARBOR_PKT_REGISTER_ACK) return;
    fprintf(stderr,
            "[router-register-flood] router=%s pkt=%u in=%s out=%s src_rank=%d dst_rank=%d sub=%u msg=%u\n",
            router_name, (unsigned)packet_type, ingress_port, egress_port,
            rank_of_ip(src_ip), rank_of_ip(dst_ip), subchannel_id, (unsigned)message_id);
}

static void log_register_trace(const char *phase, const char *router_name, const char *port_name,
                               arbor_packet_type_t packet_type, uint32_t src_ip, uint32_t dst_ip,
                               uint32_t subchannel_id, uint8_t message_id, const char *mode) {
    if (!router_name || !port_name) return;
    if (packet_type != ARBOR_PKT_REGISTER && packet_type != ARBOR_PKT_REGISTER_ACK) return;
    fprintf(stderr,
            "[router-register-%s] router=%s port=%s mode=%s src_rank=%d dst_rank=%d sub=%u msg=%u\n",
            phase, router_name, port_name, mode ? mode : "?",
            rank_of_ip(src_ip), rank_of_ip(dst_ip), subchannel_id, (unsigned)message_id);
}

static void log_register_wire(const char *tag, const char *router_name, const char *port_name,
                              const uint8_t *frame, int len) {
    const eth_header_t *eth;
    const ip_header_t *ip;
    const udp_header_t *udp;
    const uint8_t *arbor_hdr;
    arbor_header_view_t hdr;
    int ip_ihl;

    if (!tag || !router_name || !port_name || !frame || len < HDR_LEN) return;
    eth = (const eth_header_t *)frame;
    if (ntohs(eth->ether_type) != ETH_TYPE_IP) return;
    ip = (const ip_header_t *)(frame + sizeof(eth_header_t));
    if (ip->protocol != ARBOR_IP_PROTO) return;
    ip_ihl = (ip->version_ihl & 0x0f) * 4;
    if (len < (int)(sizeof(eth_header_t) + ip_ihl + sizeof(udp_header_t) + sizeof(arbor_header_t))) return;
    udp = (const udp_header_t *)(frame + sizeof(eth_header_t) + ip_ihl);
    arbor_hdr = frame + sizeof(eth_header_t) + ip_ihl + sizeof(udp_header_t);
    hdr = arbor_parse_header(arbor_hdr);
    if (hdr.packet_type != ARBOR_PKT_REGISTER && hdr.packet_type != ARBOR_PKT_REGISTER_ACK) return;
    fprintf(stderr,
            "[router-wire-%s] router=%s port=%s src_rank=%d dst_rank=%d udp=%u sub=%u type=%u msg=%u byte0=0x%02x ctrl=0x%02x\n",
            tag, router_name, port_name,
            rank_of_ip(ip->src_ip), rank_of_ip(ip->dst_ip), ntohs(udp->dst_port),
            mtp_port_to_subchannel(ntohs(udp->dst_port)), (unsigned)hdr.packet_type,
            (unsigned)hdr.message_id, arbor_hdr[0], arbor_hdr[2]);
}

static int router_port_matches_subchannel(const char *port_name, uint32_t subchannel_id) {
    size_t len;
    if (!port_name) return 0;
    len = strlen(port_name);
    if (len >= 2 && port_name[len - 2] == 's' && (port_name[len - 1] == '0' || port_name[len - 1] == '1')) {
        return (uint32_t)(port_name[len - 1] - '0') == subchannel_id;
    }
    return 1;
}

static int router_tree_is_level(const ArborRouterNodeConfig *topo, int responder_rank, uint32_t subchannel_id) {
    return ArborRouterNodeTreeIsLevel(topo, responder_rank, subchannel_id);
}

static const char *router_tree_parent_up_port(const ArborRouterNodeConfig *topo, int responder_rank, uint32_t subchannel_id) {
    if (subchannel_id >= SUBCHANNEL_COUNT) subchannel_id = 0;
    return ArborRouterNodeParentUpPort(topo, responder_rank, subchannel_id);
}

static const char *router_tree_parent_down_port(const ArborRouterNodeConfig *topo, int responder_rank, uint32_t subchannel_id) {
    if (subchannel_id >= SUBCHANNEL_COUNT) subchannel_id = 0;
    return ArborRouterNodeParentDownPort(topo, responder_rank, subchannel_id);
}

static int router_ingress_from_parent(const ArborRouterNodeConfig *topo, int responder_rank,
                                      uint32_t subchannel_id, const char *ingress_port) {
    const char *down_port;
    const char *up_port;

    if (!ingress_port) return 0;
    down_port = router_tree_parent_down_port(topo, responder_rank, subchannel_id);
    if (down_port && down_port[0]) return strcmp(ingress_port, down_port) == 0;
    up_port = router_tree_parent_up_port(topo, responder_rank, subchannel_id);
    return up_port && strcmp(ingress_port, up_port) == 0;
}

static int router_port_is_child_uplink(const char *port_name) {
    size_t len;
    if (!port_name) return 0;
    len = strlen(port_name);
    if (len >= 2 && port_name[len - 2] == 's' &&
        (port_name[len - 1] == '0' || port_name[len - 1] == '1')) {
        return 0;
    }
    return len >= 3 && strcmp(port_name + len - 3, "-up") == 0;
}

static void router_forward_bypass_tree(const uint8_t *frame, int len,
                                      const char *ingress_port, uint32_t subchannel_id,
                                      uint32_t dst_ip) {
    int responder_rank;
    const char *out_port;

    if (!g_router_topology) return;
    if (subchannel_id >= SUBCHANNEL_COUNT) subchannel_id = 0;

    responder_rank = rank_of_ip(dst_ip);
    if (responder_rank < 0) {
        router_flood_except(frame, len, ingress_port, subchannel_id);
        return;
    }

    out_port = ArborRouterNodeRoutePort(g_router_topology, responder_rank, subchannel_id);
    if (!out_port) return;

    inject_on_port(out_port, frame, len, responder_rank, subchannel_id);
}

static void router_multicast_response(const uint8_t *frame, int len, const char *ingress_port,
                                      uint32_t subchannel_id, uint32_t responder_ip, int credit_valid) {
    int responder_rank = rank_of_ip(responder_ip);
    const char *parent_up_port;
    uint16_t slot_index = AGG_INDEX_UNUSED;
    uint16_t udp_port = 0;
    uint8_t tmp_frame[HDR_LEN + PAYLOAD_LEN];
    uint8_t *tmp_arbor_hdr;
    arbor_header_view_t hdrv;
    int repair = 0;

    if (!g_router_topology || responder_rank < 0) return;
    if (subchannel_id >= SUBCHANNEL_COUNT) subchannel_id = 0;
    parent_up_port = router_tree_parent_up_port(g_router_topology, responder_rank, subchannel_id);
    if (!parent_up_port) return;

    if (len > (int)sizeof(tmp_frame)) return;
    memcpy(tmp_frame, frame, (size_t)len);
    udp_port = ntohs(((udp_header_t *)(tmp_frame + sizeof(eth_header_t) + sizeof(ip_header_t)))->dst_port);
    tmp_arbor_hdr = tmp_frame + sizeof(eth_header_t) + sizeof(ip_header_t) + sizeof(udp_header_t);
    hdrv = arbor_parse_header(tmp_arbor_hdr);
    repair = arbor_get_repair(arbor_load_byte0(tmp_arbor_hdr));

    if (!router_ingress_from_parent(g_router_topology, responder_rank, subchannel_id, ingress_port)) {
        fprintf(stderr,
                "[router-response-up] router=%s in=%s parent=%s responder_rank=%d sub=%u msg=%u credit_valid=%d completion_valid=%u credit_off=%u payload_off=%u depth=%u\n",
                g_router_topology ? ArborRouterNodeName(g_router_topology) : "?",
                ingress_port ? ingress_port : "?",
                parent_up_port, responder_rank, subchannel_id, (unsigned)hdrv.message_id, credit_valid,
                hdrv.payload_valid ? 1u : 0u, hdrv.offset_b, hdrv.offset_a, hdrv.agg_depth);
        inject_on_port(parent_up_port, tmp_frame, len, responder_rank, subchannel_id);
        return;
    }

    if (credit_valid && !repair && g_router_topology &&
        router_tree_is_level(g_router_topology, responder_rank, subchannel_id)) {
        const uint32_t credit_offset = arbor_load_offset_b(tmp_arbor_hdr);
        slot_index = find_slot_for_credit(udp_port, credit_offset, responder_ip);
        if (slot_index == AGG_INDEX_UNUSED) {
            slot_index = (uint16_t)g_router_alloc_ptr;
            g_router_alloc_ptr = (g_router_alloc_ptr + 1U) % AGTR_ARRAY_SIZE;
            slot_reset_for_credit(&g_slots[slot_index], udp_port, credit_offset, slot_index, responder_ip);
        } else {
            fprintf(stderr,
                    "[router-credit-reuse] router=%s slot=%u udp=%u credit_off=%u responder_rank=%d\n",
                    g_router_topology ? ArborRouterNodeName(g_router_topology) : "?", slot_index, udp_port,
                    credit_offset, rank_of_ip(responder_ip));
        }
        credit_push_agg_loc(tmp_arbor_hdr, slot_index);
        hdrv = arbor_parse_header(tmp_arbor_hdr);
        fprintf(stderr,
                "[router-credit-push] router=%s in=%s slot=%u responder_rank=%d sub=%u msg=%u credit_off=%u new_depth=%u agg0=%u agg1=%u\n",
                g_router_topology ? ArborRouterNodeName(g_router_topology) : "?",
                ingress_port ? ingress_port : "?", slot_index, responder_rank, subchannel_id,
                (unsigned)hdrv.message_id, hdrv.offset_b, hdrv.agg_depth, hdrv.agg_locs[0], hdrv.agg_locs[1]);
    }

    if (ArborRouterNodeMcastCount(g_router_topology, responder_rank, subchannel_id) == 0) {
        fprintf(stderr,
                "[router-config-error] router=%s missing mcast rank=%d sub=%u\n",
                ArborRouterNodeName(g_router_topology), responder_rank, subchannel_id);
        return;
    }
    for (uint8_t i = 0; i < ArborRouterNodeMcastCount(g_router_topology, responder_rank, subchannel_id); ++i) {
        const char *out_port = ArborRouterNodeMcastPort(g_router_topology, responder_rank, subchannel_id, i);
        uint8_t local_frame[HDR_LEN + PAYLOAD_LEN];
        arbor_header_view_t out_hdr;
        if (len > (int)sizeof(local_frame)) continue;
        memcpy(local_frame, tmp_frame, (size_t)len);
        out_hdr = arbor_parse_header(local_frame + sizeof(eth_header_t) + sizeof(ip_header_t) + sizeof(udp_header_t));
        fprintf(stderr,
                "[router-response-down] router=%s out=%s responder_rank=%d sub=%u msg=%u credit_valid=%u completion_valid=%u credit_off=%u payload_off=%u depth=%u\n",
                g_router_topology ? ArborRouterNodeName(g_router_topology) : "?",
                out_port, responder_rank, subchannel_id, (unsigned)out_hdr.message_id,
                out_hdr.credit_valid ? 1u : 0u, out_hdr.payload_valid ? 1u : 0u,
                out_hdr.offset_b, out_hdr.offset_a, out_hdr.agg_depth);
        inject_on_port(out_port, local_frame, len, -1, subchannel_id);
    }
}

static void *dev_capture_thread(void *arg) {
    net_device_t *dev = (net_device_t *)arg;
    struct pcap_pkthdr *hdr;
    const u_char *pkt;
    int rc;
    while ((rc = pcap_next_ex(dev->handle, &hdr, &pkt)) >= 0) {
        if (g_router_topology && ((strcmp(ArborRouterNodeName(g_router_topology), "router-b") == 0 && strcmp(dev->name, "rb-u") == 0) || (strcmp(ArborRouterNodeName(g_router_topology), "router-b0") == 0 && strcmp(dev->name, "rb0-u") == 0))) {
            fprintf(stderr, "[router-capture] router=%s dev=%s rc=%d caplen=%u\n", ArborRouterNodeName(g_router_topology), dev->name, rc, hdr ? (unsigned)hdr->caplen : 0u);
        }
        if (rc == 0) continue;
        if (g_router_topology && strcmp(ArborRouterNodeName(g_router_topology), "router-a0") == 0 &&
            strcmp(dev->name, "ra0-up") == 0 && g_dbg_router_a0_rank3_to_0_rx < 8) {
            const eth_header_t *dbg_eth = (const eth_header_t *)pkt;
            if (hdr && hdr->caplen >= (unsigned)HDR_LEN && ntohs(dbg_eth->ether_type) == ETH_TYPE_IP) {
                const ip_header_t *dbg_ip = (const ip_header_t *)(pkt + sizeof(eth_header_t));
                const udp_header_t *dbg_udp = (const udp_header_t *)(pkt + sizeof(eth_header_t) + sizeof(ip_header_t));
                if (dbg_ip->protocol == ARBOR_IP_PROTO && rank_of_ip(dbg_ip->src_ip) == 3 && rank_of_ip(dbg_ip->dst_ip) == 0) {
                    ++g_dbg_router_a0_rank3_to_0_rx;
                    fprintf(stderr, "[dbg-ra0-rx] count=%u src_rank=%d dst_rank=%d udp=%u len=%u\n",
                            g_dbg_router_a0_rank3_to_0_rx, rank_of_ip(dbg_ip->src_ip), rank_of_ip(dbg_ip->dst_ip),
                            (unsigned)ntohs(dbg_udp->dst_port), hdr->caplen);
                }
            }
        }
        ++dev->rx_packets;
        dev_buffer_t *rx_buf = dev->rx_buf;
        pthread_mutex_lock(&rx_buf->lock);
        int nh = (rx_buf->head + 1) % DEV_RING_SIZE;
        if (nh != rx_buf->tail) {
            dev_pkt_t *e = &rx_buf->packets[rx_buf->head];
            e->device = dev;
            e->len = hdr->caplen > DEV_BUF_SIZE ? DEV_BUF_SIZE : hdr->caplen;
            memcpy(e->data, pkt, e->len);
            rx_buf->head = nh;
        } else {
            ++dev->rx_ring_drops;
            if ((dev->rx_ring_drops & (dev->rx_ring_drops - 1)) == 0) {
                fprintf(stderr, "[router-rx-drop] router=%s dev=%s drops=%llu\n",
                        g_router_topology ? ArborRouterNodeName(g_router_topology) : "?",
                        dev->name,
                        (unsigned long long)dev->rx_ring_drops);
            }
        }
        pthread_mutex_unlock(&rx_buf->lock);
    }
    return NULL;
}

static int dev_pop(dev_pkt_t *out) {
    if (g_dev_count <= 0) return 0;
    for (int attempt = 0; attempt < g_dev_count; ++attempt) {
        int idx = (g_dev_rr + attempt) % g_dev_count;
        net_device_t *dev = &g_devs[idx];
        dev_buffer_t *rx_buf = dev->rx_buf;
        int ok = 0;

        pthread_mutex_lock(&rx_buf->lock);
        if (rx_buf->tail != rx_buf->head) {
            *out = rx_buf->packets[rx_buf->tail];
            rx_buf->tail = (rx_buf->tail + 1) % DEV_RING_SIZE;
            ok = 1;
        }
        pthread_mutex_unlock(&rx_buf->lock);
        if (ok) {
            g_dev_rr = (idx + 1) % g_dev_count;
            return 1;
        }
    }
    return 0;
}

static uint16_t ipv4_checksum(const uint8_t *hdr, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < len; i += 2) {
        sum += ((uint32_t)hdr[i] << 8U) | (uint32_t)hdr[i + 1];
    }
    while ((sum >> 16U) != 0) {
        sum = (sum & 0xFFFFu) + (sum >> 16U);
    }
    return (uint16_t)(~sum & 0xFFFFu);
}

static void router_rewrite_to_agg_miss(uint8_t *frame) {
    uint8_t *ip = frame + sizeof(eth_header_t);
    uint8_t *udp = ip + sizeof(ip_header_t);
    uint8_t *arbor_hdr = udp + sizeof(udp_header_t);

    arbor_rewrite_to_agg_miss(arbor_hdr);

    const uint16_t ip_total_len = (uint16_t)(sizeof(ip_header_t) + sizeof(udp_header_t) + sizeof(arbor_header_t));
    const uint16_t udp_len = (uint16_t)(sizeof(udp_header_t) + sizeof(arbor_header_t));
    ip[2] = (uint8_t)(ip_total_len >> 8U);
    ip[3] = (uint8_t)(ip_total_len & 0xFFu);
    udp[4] = (uint8_t)(udp_len >> 8U);
    udp[5] = (uint8_t)(udp_len & 0xFFu);
    udp[6] = 0;
    udp[7] = 0;
    ip[1] = (uint8_t)((ip[1] & ~ARBOR_IPV4_ECN_MASK) | ARBOR_IPV4_ECN_CE);
    ip[10] = 0;
    ip[11] = 0;
    {
        const uint16_t cksum = ipv4_checksum(ip, sizeof(ip_header_t));
        ip[10] = (uint8_t)(cksum >> 8U);
        ip[11] = (uint8_t)(cksum & 0xFFu);
    }
}

static int32_t *slot_payload(router_slot_t *slot) {
    if (!slot || !g_agtr) return NULL;
    return g_agtr[slot->agg_loc % AGTR_ARRAY_SIZE].payload;
}

static void slot_reset_for_credit(router_slot_t *slot, uint16_t udp_port, uint32_t credit_offset,
                                  uint16_t slot_index, uint32_t responder_ip) {
    if (!slot) return;
    memset(slot, 0, sizeof(*slot));
    slot->owner_valid = 1;
    slot->owner_udp_port = udp_port;
    slot->owner_offset = credit_offset;
    slot->agg_loc = slot_index;
    slot->responder_ip = responder_ip;
    fprintf(stderr,
            "[router-credit-bind] router=%s slot=%u udp=%u credit_off=%u responder_rank=%d\n",
            g_router_topology ? ArborRouterNodeName(g_router_topology) : "?", slot_index, udp_port,
            credit_offset, rank_of_ip(responder_ip));
    if (g_agtr && slot_index < AGTR_ARRAY_SIZE) memset(&g_agtr[slot_index], 0, sizeof(g_agtr[slot_index]));
}

static uint16_t find_slot_for_credit(uint16_t udp_port, uint32_t credit_offset, uint32_t responder_ip) {
    for (uint16_t i = 0; i < AGTR_ARRAY_SIZE; ++i) {
        const router_slot_t *slot = &g_slots[i];
        if (!slot->owner_valid || slot->forwarded) continue;
        if (slot->owner_udp_port != udp_port) continue;
        if (slot->owner_offset != credit_offset) continue;
        if (slot->responder_ip != responder_ip) continue;
        return i;
    }
    return AGG_INDEX_UNUSED;
}

static void credit_push_agg_loc(uint8_t *arbor_hdr, uint16_t slot_index) {
    arbor_store_agg_loc_at(arbor_hdr, arbor_get_agg_depth(arbor_load_ctrl(arbor_hdr)), slot_index);
    uint8_t ctrl = arbor_load_ctrl(arbor_hdr);
    arbor_set_agg_depth(&ctrl, (uint8_t)(arbor_get_agg_depth(ctrl) + 1U));
    arbor_set_aggregated(&ctrl, 0);
    arbor_store_ctrl(arbor_hdr, ctrl);
}

static void request_pop_one_level(uint8_t *arbor_hdr) {
    uint8_t ctrl = arbor_load_ctrl(arbor_hdr);
    uint8_t depth = arbor_get_agg_depth(ctrl);
    if (depth == 0) return;
    arbor_set_agg_depth(&ctrl, (uint8_t)(depth - 1U));
    arbor_store_ctrl(arbor_hdr, ctrl);
}

static void master_finalize_header(uint8_t *arbor_hdr, uint16_t contribution_count) {
    uint8_t ctrl = arbor_load_ctrl(arbor_hdr);
    arbor_set_aggregated(&ctrl, 1);
    arbor_set_agg_depth(&ctrl, (uint8_t)(arbor_get_agg_depth(ctrl) - 1U));
    arbor_store_ctrl(arbor_hdr, ctrl);
    arbor_store_offset_b(arbor_hdr, contribution_count);
}

typedef enum {
    SLOT_FAIL_NONE = 0,
    SLOT_FAIL_OWNER = 1,
    SLOT_FAIL_FORWARDED = 2,
    SLOT_FAIL_FANIN_ZERO = 3,
    SLOT_FAIL_CONTRIBUTION = 4,
    SLOT_FAIL_FANIN_MISMATCH = 5,
    SLOT_FAIL_SHAPE = 6,
    SLOT_FAIL_OVERFLOW = 7,
} slot_fail_reason_t;

static int slot_accept(router_slot_t *slot, uint16_t udp_port, uint32_t request_offset,
                       uint8_t fanin, int aggregated, uint32_t merged_count,
                       uint8_t op, uint8_t dtype, uint16_t payload_len,
                       const uint8_t *frame, uint32_t frame_len,
                       slot_fail_reason_t *fail_reason) {
    uint32_t contribution = aggregated ? merged_count : 1u;
    if (fail_reason) *fail_reason = SLOT_FAIL_NONE;
    if (!slot || !slot->owner_valid || slot->owner_udp_port != udp_port || slot->owner_offset != request_offset) {
        if (fail_reason) *fail_reason = SLOT_FAIL_OWNER;
        return -1;
    }
    if (slot->forwarded) {
        if (fail_reason) *fail_reason = SLOT_FAIL_FORWARDED;
        return -1;
    }
    if (fanin == 0) {
        if (fail_reason) *fail_reason = SLOT_FAIL_FANIN_ZERO;
        return -1;
    }
    if (contribution == 0 || contribution > fanin) {
        if (fail_reason) *fail_reason = SLOT_FAIL_CONTRIBUTION;
        return -1;
    }
    if (slot->agg_count == 0) {
        slot->fanin = fanin;
        slot->op = op;
        slot->dtype = dtype;
        slot->payload_len = payload_len;
        slot->master_len = frame_len > sizeof(slot->master_frame) ? sizeof(slot->master_frame) : frame_len;
        memcpy(slot->master_frame, frame, slot->master_len);
    } else {
        if (slot->fanin != fanin) {
            if (fail_reason) *fail_reason = SLOT_FAIL_FANIN_MISMATCH;
            return -1;
        }
        if (slot->op != op || slot->dtype != dtype || slot->payload_len != payload_len) {
            if (fail_reason) *fail_reason = SLOT_FAIL_SHAPE;
            return -1;
        }
        if (contribution > (uint32_t)slot->fanin - slot->agg_count) {
            if (fail_reason) *fail_reason = SLOT_FAIL_OVERFLOW;
            return -1;
        }
    }
    slot->agg_count = (uint8_t)(slot->agg_count + contribution);
    return slot->agg_count == slot->fanin ? 1 : 0;
}

static void slot_accumulate_payload(router_slot_t *slot, const uint8_t *payload, uint16_t payload_len) {
    int32_t *accum;
    const int32_t *src;
    uint16_t words;
    if (!slot || payload_len == 0 || !payload) return;
    accum = slot_payload(slot);
    if (!accum) return;
    src = (const int32_t *)payload;
    words = payload_len / (uint16_t)sizeof(int32_t);
    for (uint16_t i = 0; i < words; ++i) accum[i] += src[i];
}

static void slot_or_ecn(router_slot_t *slot, const uint8_t *frame) {
    const ip_header_t *ip;
    if (!slot || !frame || !slot->master_len) return;
    ip = (const ip_header_t *)(frame + sizeof(eth_header_t));
    if ((ip->tos & ARBOR_IPV4_ECN_MASK) == 0) return;
    {
        ip_header_t *master_ip = (ip_header_t *)(slot->master_frame + sizeof(eth_header_t));
        master_ip->tos = (uint8_t)((master_ip->tos & ~ARBOR_IPV4_ECN_MASK) |
                                   ((master_ip->tos & ARBOR_IPV4_ECN_MASK) | (ip->tos & ARBOR_IPV4_ECN_MASK)));
    }
}

void init_router(config_entry_t *cfgs, int n, const char *router_name) {
    char errbuf[PCAP_ERRBUF_SIZE];
    int i;
    g_router_topology = ArborFabricFindRouter(router_name);
    if (!g_router_topology) {
        fprintf(stderr, "unknown router topology: %s\n", router_name ? router_name : "(null)");
        exit(1);
    }
    memcpy(g_cfg, cfgs, sizeof(config_entry_t) * (size_t)n);
    g_dev_rr = 0;
    memset(g_slots, 0, sizeof(g_slots));
    for (i = 0; i < ArborRouterNodeDevCount(g_router_topology); i++) {
        const char *dev_name = ArborRouterNodeDevName(g_router_topology, i);
        net_device_t *dev;
        if (!dev_name || if_nametoindex(dev_name) == 0) {
            fprintf(stderr, "[router-init-skip] router=%s dev=%s absent\n",
                    router_name, dev_name ? dev_name : "(null)");
            continue;
        }
        dev = &g_devs[g_dev_count];
        memset(dev, 0, sizeof(*dev));
        strncpy(dev->name, dev_name, sizeof(dev->name) - 1);
        dev->rx_buf = calloc(1, sizeof(*dev->rx_buf));
        if (!dev->rx_buf) {
            fprintf(stderr, "router rx buffer alloc failed for %s\n", dev_name);
            exit(1);
        }
        pthread_mutex_init(&dev->rx_buf->lock, NULL);
        dev->handle = router_open_pcap(dev_name, errbuf, PCAP_D_IN);
        if (!dev->handle) {
            fprintf(stderr, "pcap_open_live(%s) failed: %s\n", dev_name, errbuf);
            exit(1);
        }
        dev->tx_handle = router_open_pcap(dev_name, errbuf, 0);
        if (!dev->tx_handle) {
            fprintf(stderr, "pcap_open_live(tx:%s) failed: %s\n", dev_name, errbuf);
            exit(1);
        }
        pthread_create(&dev->thread_id, NULL, dev_capture_thread, dev);
        g_dev_count++;
    }
    g_agtr = calloc(AGTR_ARRAY_SIZE, sizeof(agtr_t));
    if (!g_agtr) {
        fprintf(stderr, "router alloc failed\n");
        exit(1);
    }
    printf("[%s] opened %d ports, group_n=%d, agtr_slots=%d\n",
           router_name, g_dev_count, n, AGTR_ARRAY_SIZE);
    fflush(stdout);
}

void INC(void) {
    printf("[router] running as INC (shared tree)\n");
    fflush(stdout);

    while (1) {
        uint64_t now = now_us();
        if (now >= g_router_next_progress_log) {
            dump_router_stats();
            g_router_next_progress_log = now + 1000000ULL;
        }

        dev_pkt_t pk;
        if (!dev_pop(&pk)) {
            usleep(200);
            continue;
        }

        eth_header_t *eth = (eth_header_t *)pk.data;
        if (ntohs(eth->ether_type) != ETH_TYPE_IP) continue;
        ip_header_t *ip = (ip_header_t *)(pk.data + sizeof(eth_header_t));
        if (ip->protocol != ARBOR_IP_PROTO) continue;

        int ip_ihl = (ip->version_ihl & 0x0f) * 4;
        udp_header_t *udp = (udp_header_t *)(pk.data + sizeof(eth_header_t) + ip_ihl);
        uint8_t *arbor_hdr = pk.data + sizeof(eth_header_t) + ip_ihl + sizeof(udp_header_t);
        arbor_header_view_t hdr = arbor_parse_header(arbor_hdr);
        uint16_t udp_port = ntohs(udp->dst_port);
        uint32_t subchannel_id = 0;
        uint16_t payload_len = 0;
        if (!mtp_udp_port_in_range(udp_port)) continue;
        subchannel_id = mtp_port_to_subchannel(udp_port);
        if (subchannel_id >= SUBCHANNEL_COUNT) continue;
        if (pk.len >= (uint32_t)HDR_LEN) payload_len = (uint16_t)(pk.len - HDR_LEN);
        if (payload_len > PAYLOAD_LEN) payload_len = PAYLOAD_LEN;

        if (hdr.packet_type == ARBOR_PKT_REGISTER || hdr.packet_type == ARBOR_PKT_REGISTER_ACK ||
            hdr.packet_type == ARBOR_PKT_END || hdr.packet_type == ARBOR_PKT_END_ACK ||
            hdr.packet_type == ARBOR_PKT_AGG_MISS) {
            g_router_register_bypass++;
            if ((hdr.packet_type == ARBOR_PKT_REGISTER || hdr.packet_type == ARBOR_PKT_REGISTER_ACK) &&
                g_router_topology && pk.device) {
                int dst_rank = rank_of_ip(ip->dst_ip);
                const char *parent_up_port = router_tree_parent_up_port(g_router_topology, dst_rank, subchannel_id);
                log_register_trace("in", ArborRouterNodeName(g_router_topology), pk.device->name, hdr.packet_type,
                                   ip->src_ip, ip->dst_ip, subchannel_id, hdr.message_id, "bypass");
                if (parent_up_port && !router_ingress_from_parent(g_router_topology, dst_rank, subchannel_id, pk.device->name)) {
                    log_register_flood(ArborRouterNodeName(g_router_topology), pk.device->name, parent_up_port,
                                       hdr.packet_type, ip->src_ip, ip->dst_ip,
                                       subchannel_id, hdr.message_id);
                } else if (dst_rank >= 0) {
                    const char *out_port = ArborRouterNodeRoutePort(g_router_topology, dst_rank, subchannel_id);
                    if (out_port && strcmp(out_port, pk.device->name) != 0) {
                        log_register_flood(ArborRouterNodeName(g_router_topology), pk.device->name, out_port,
                                           hdr.packet_type, ip->src_ip, ip->dst_ip,
                                           subchannel_id, hdr.message_id);
                    }
                }
            }
            if ((hdr.packet_type == ARBOR_PKT_REGISTER || hdr.packet_type == ARBOR_PKT_REGISTER_ACK) &&
                g_router_topology && pk.device) {
                int dst_rank = rank_of_ip(ip->dst_ip);
                const char *parent_up_port = router_tree_parent_up_port(g_router_topology, dst_rank, subchannel_id);
                if (parent_up_port && !router_ingress_from_parent(g_router_topology, dst_rank, subchannel_id, pk.device->name)) {
                    log_register_trace("out", ArborRouterNodeName(g_router_topology), parent_up_port, hdr.packet_type,
                                       ip->src_ip, ip->dst_ip, subchannel_id, hdr.message_id, "up");
                } else if (dst_rank >= 0) {
                    const char *out_port = ArborRouterNodeRoutePort(g_router_topology, dst_rank, subchannel_id);
                    if (out_port && strcmp(out_port, pk.device->name) != 0) {
                        log_register_trace("out", ArborRouterNodeName(g_router_topology), out_port, hdr.packet_type,
                                           ip->src_ip, ip->dst_ip, subchannel_id, hdr.message_id, "down");
                    }
                }
            }
            router_forward_bypass_tree(pk.data, pk.len, pk.device ? pk.device->name : NULL,
                                       subchannel_id, ip->dst_ip);
            continue;
        }

        if (hdr.packet_type == ARBOR_PKT_RESPONSE) {
            if (hdr.credit_valid) {
                g_router_response_bind++;
            }
            router_multicast_response(pk.data, pk.len, pk.device ? pk.device->name : NULL,
                                      subchannel_id, ip->src_ip, hdr.credit_valid);
            continue;
        }

        if (hdr.packet_type != ARBOR_PKT_DATA_REQUEST) {
            g_router_other_bypass++;
            router_forward_bypass_tree(pk.data, pk.len, pk.device ? pk.device->name : NULL,
                                       subchannel_id, ip->dst_ip);
            continue;
        }

        fprintf(stderr,
                "[router-request-in] router=%s in=%s src_rank=%d dst_rank=%d sub=%u msg=%u depth=%u aggregated=%u req_off=%u merged=%u payload_valid=%u payload_kind=%u payload_len=%u\n",
                g_router_topology ? ArborRouterNodeName(g_router_topology) : "?",
                pk.device ? pk.device->name : "?",
                rank_of_ip(ip->src_ip), rank_of_ip(ip->dst_ip), subchannel_id,
                (unsigned)hdr.message_id, hdr.agg_depth, hdr.aggregated,
                hdr.offset_a, hdr.offset_b, hdr.payload_valid ? 1u : 0u,
                (unsigned)hdr.payload_kind, payload_len);

        if (hdr.agg_depth == 0) {
            g_router_request_bypass++;
            router_forward_bypass_tree(pk.data, pk.len,
                                       pk.device ? pk.device->name : NULL,
                                       subchannel_id, ip->dst_ip);
            continue;
        }
        {
            const int responder_rank = rank_of_ip(ip->dst_ip);
            if (responder_rank >= 0 && g_router_topology &&
                !router_tree_is_level(g_router_topology, responder_rank, subchannel_id)) {
                g_router_request_bypass++;
                router_forward_bypass_tree(pk.data, pk.len,
                                           pk.device ? pk.device->name : NULL,
                                           subchannel_id, ip->dst_ip);
                continue;
            }
            uint8_t level = (uint8_t)(hdr.agg_depth - 1);
            uint16_t level_agg_loc = hdr.agg_locs[level];
            uint8_t level_fanin = hdr.fanins[level];
            if (level_agg_loc == AGG_INDEX_UNUSED || level_agg_loc >= AGTR_ARRAY_SIZE) {
                router_rewrite_to_agg_miss(pk.data);
                g_router_request_bypass++;
                router_forward_bypass_tree(pk.data, HDR_LEN,
                                           pk.device ? pk.device->name : NULL,
                                           subchannel_id, ip->dst_ip);
                continue;
            }
            if (level_fanin == 1) {
                request_pop_one_level(arbor_hdr);
                g_router_request_bypass++;
                router_forward_bypass_tree(pk.data, pk.len,
                                           pk.device ? pk.device->name : NULL,
                                           subchannel_id, ip->dst_ip);
                continue;
            }
        }
        const int is_payload_request = hdr.payload_valid && hdr.payload_kind == ARBOR_PAYLOAD_DATA;
        const int is_control_request = !hdr.payload_valid;
        if (!is_payload_request && !is_control_request) {
            router_rewrite_to_agg_miss(pk.data);
            g_router_request_bypass++;
            router_forward_bypass_tree(pk.data, HDR_LEN,
                                       pk.device ? pk.device->name : NULL,
                                       subchannel_id, ip->dst_ip);
            continue;
        }

        g_router_request_aggregate++;
        {
            uint8_t level = (uint8_t)(hdr.agg_depth - 1);
            uint16_t level_agg_loc = hdr.agg_locs[level];
            uint8_t level_fanin = hdr.fanins[level];
            router_slot_t *slot = &g_slots[level_agg_loc];
            slot_fail_reason_t fail_reason = SLOT_FAIL_NONE;
            int final = slot_accept(slot, udp_port, hdr.offset_a, level_fanin,
                                    hdr.aggregated, hdr.aggregated ? hdr.offset_b : 0,
                                    hdr.op, hdr.dtype, payload_len,
                                    pk.data, pk.len, &fail_reason);
            if (final < 0) {
                fprintf(stderr,
                        "[router-slot-miss] router=%s sub=%u level=%u slot=%u reason=%d udp=%u req_off=%u owner_valid=%u owner_udp=%u owner_off=%u fanin=%u slot_fanin=%u agg_count=%u aggregated=%u merged=%u payload_len=%u op=%u dtype=%u ingress=%s\n",
                        g_router_topology ? ArborRouterNodeName(g_router_topology) : "?", subchannel_id, level, level_agg_loc,
                        (int)fail_reason, udp_port, hdr.offset_a,
                        slot->owner_valid, slot->owner_udp_port, slot->owner_offset,
                        level_fanin, slot->fanin, slot->agg_count,
                        hdr.aggregated, hdr.aggregated ? hdr.offset_b : 0,
                        payload_len, hdr.op, hdr.dtype,
                        pk.device ? pk.device->name : "?");
                router_rewrite_to_agg_miss(pk.data);
                g_router_request_bypass++;
                router_forward_bypass_tree(pk.data, HDR_LEN,
                                           pk.device ? pk.device->name : NULL,
                                           subchannel_id, ip->dst_ip);
                continue;
            }
            slot_accumulate_payload(slot,
                pk.data + sizeof(eth_header_t) + ip_ihl + sizeof(udp_header_t) + sizeof(arbor_header_t),
                payload_len);
            slot_or_ecn(slot, pk.data);
            if (!final) {
                fprintf(stderr,
                        "[router-slot-progress] router=%s sub=%u slot=%u req_off=%u count=%u fanin=%u aggregated=%u merged=%u payload_len=%u\n",
                        g_router_topology ? ArborRouterNodeName(g_router_topology) : "?", subchannel_id,
                        level_agg_loc, hdr.offset_a, slot->agg_count, slot->fanin,
                        hdr.aggregated ? 1u : 0u, hdr.aggregated ? hdr.offset_b : 0u,
                        payload_len);
                continue;
            }
            if (slot->payload_len > 0) {
                memcpy(slot->master_frame + HDR_LEN, slot_payload(slot), slot->payload_len);
            }
            master_finalize_header(slot->master_frame + sizeof(eth_header_t) + sizeof(ip_header_t) + sizeof(udp_header_t),
                                   slot->agg_count);
            slot->forwarded = 1;
            g_router_request_complete++;
            {
                arbor_header_view_t out_hdr = arbor_parse_header(slot->master_frame + sizeof(eth_header_t) + sizeof(ip_header_t) + sizeof(udp_header_t));
                fprintf(stderr,
                        "[router-slot-complete] router=%s sub=%u slot=%u msg=%u depth=%u aggregated=%u req_off=%u merged=%u payload_valid=%u payload_len=%u dst_rank=%d\n",
                        g_router_topology ? ArborRouterNodeName(g_router_topology) : "?", subchannel_id,
                        level_agg_loc, (unsigned)out_hdr.message_id, out_hdr.agg_depth,
                        out_hdr.aggregated ? 1u : 0u, out_hdr.offset_a, out_hdr.offset_b,
                        out_hdr.payload_valid ? 1u : 0u, slot->payload_len, rank_of_ip(ip->dst_ip));
            }
            router_forward(slot->master_frame, (int)slot->master_len, ip->dst_ip, subchannel_id);
        }
    }
}
