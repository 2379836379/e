#ifndef TEST_LAB_RUNTIME_COMMON_H
#define TEST_LAB_RUNTIME_COMMON_H

#include "wire/arbor_wire.h"

#include <pcap.h>
#include <pthread.h>
#include <stdint.h>

typedef struct {
    int rank;
    char host_name[32];
    char host_ifaces[SUBCHANNEL_COUNT][32];
    char router_ifaces[SUBCHANNEL_COUNT][32];
    uint32_t host_ip;
} config_entry_t;

typedef struct {
    uint8_t msg_type;
    uint8_t flags;
    uint8_t repair;
    uint8_t message_id;
    uint8_t reserved2;
    uint32_t src_ip;
    uint32_t channel_id;
    uint32_t subchannel_id;
    uint32_t credit_offset;
    uint32_t payload_offset;
    uint8_t agg_depth;
    uint8_t request_kind;
    uint8_t reserved0;
    uint8_t reserved1;
    uint32_t agg_stack[ARBOR_MAX_STACK_DEPTH];
    uint8_t fanin[ARBOR_MAX_STACK_DEPTH];
    uint8_t fanin_pad;
    uint32_t seq_num;
    uint16_t payload_len;
    uint8_t payload[PAYLOAD_LEN];
} __attribute__((packed)) rx_msg_t;

#define RXQ_SIZE 8192
typedef struct {
    int in_use;
    uint32_t local_ip;
    uint32_t remote_ip;
    rx_msg_t queue[RXQ_SIZE];
    volatile int head;
    volatile int tail;
    pthread_mutex_t lock;
} conn_t;

#define DEV_BUF_SIZE 4096
#define PCAP_BUFFER_SIZE (16 * 1024 * 1024)
typedef struct dev_buffer_t dev_buffer_t;
typedef struct {
    char name[32];
    pcap_t *handle;
    pcap_t *tx_handle;
    pthread_t thread_id;
    int index;
    dev_buffer_t *rx_buf;
    uint64_t rx_ring_drops;
    uint64_t rx_packets;
    uint64_t tx_packets;
} net_device_t;
typedef struct {
    net_device_t *device;
    uint8_t data[DEV_BUF_SIZE];
    uint32_t len;
} dev_pkt_t;

#define DEV_RING_SIZE 65536
struct dev_buffer_t {
    dev_pkt_t packets[DEV_RING_SIZE];
    volatile int head;
    volatile int tail;
    pthread_mutex_t lock;
};

typedef struct { uint32_t dst_ip; char out_port[32]; } route_entry_t;
typedef struct { int32_t payload[PAYLOAD_LEN / sizeof(int32_t)]; } agtr_t;

void common_set_group(config_entry_t *cfgs, int n, const char *graph_path);
int rank_of_ip(uint32_t ip);
int count_bits32(uint32_t x);
uint32_t neighbor_mask_of(uint32_t vertex_id);
uint8_t arbor_plan_request_fanin_stack(uint32_t requester_rank,
                                       uint32_t responder_rank,
                                       uint8_t *fanin_vec_out,
                                       uint8_t max_depth);
uint64_t now_us(void);

#endif
