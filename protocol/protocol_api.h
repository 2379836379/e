#ifndef TEST_LAB_PROTOCOL_API_H
#define TEST_LAB_PROTOCOL_API_H

#include "runtime/runtime_common.h"

void init_host(config_entry_t *cfgs, int n, const char *host_name);
void start_host_rx(void);
int init_channel(uint32_t channel_id, uint32_t local_ip, uint32_t responder_ip);
int request(uint32_t channel_id, const void *buf, uint32_t size, uint8_t op);
int respond(uint32_t channel_id, void *buf, uint32_t size, uint8_t op);
void register_local_source(uint32_t channel_id, const void *buf, uint32_t size, uint8_t op);
void clear_local_source(uint32_t channel_id);
void register_request_result(uint32_t channel_id, void *buf, uint32_t size);
void clear_request_result(uint32_t channel_id);

void init_router(config_entry_t *cfgs, int n, const char *router_name);
void INC(void);

#endif
