# Arbor 

## 3. 构建与运行接口

### 3.1 构建

~~~
make                 # 构建 build/inc
make clean           # 清理 build/ 和测试输出
make setup_env       # 安装 libpcap-dev 并构建 node 镜像
~~~

Makefile 的默认编译单元包括：

~~~
protocol/main.c config/app_config.c protocol/allreduce_workload.c
wire/arbor_wire.c runtime/runtime_common.c
protocol/host.c protocol/requester.c protocol/responder.c protocol/router.c
config/arbor_fabric.c
~~~

编译选项使用 -Wall -Wextra -O2，链接 libpcap 和 pthread。

### 3.2 进程命令行

~~~
./build/inc <name> <ranks.cfg> allreduce
~~~

- name 是 host1、host2 或 router-root 等节点名。
- 名称以 router 开头时进入 init_router() 并运行 INC()；否则进入 host AllReduce workload。
- 目前只支持 allreduce 模式。

典型启动顺序是先启动所有 router，再启动所有 host。测试脚本负责生成输入、编译、创建 veth/container、启动进程、等待输出并调用结果校验。

### 3.3 C 层公共接口

protocol/protocol_api.h 提供 host/router API：

~~~c
void init_host(config_entry_t *cfgs, int n, const char *host_name);
void start_host_rx(void);
int init_channel(uint32_t channel_id, uint32_t local_ip, uint32_t responder_ip);
int init_channel_group(uint32_t group_id, uint32_t channel_id,
                       uint32_t local_ip, uint32_t responder_ip);
int request(uint32_t channel_id, const void *buf, uint32_t size, uint8_t op);
int respond(uint32_t channel_id, void *buf, uint32_t size, uint8_t op);
int request_push(uint32_t channel_id, const void *buf, uint32_t size, uint8_t op);
int respond_push(uint32_t channel_id, void *buf, uint32_t size, uint8_t op);
void register_local_source(uint32_t channel_id, const void *buf, uint32_t size, uint8_t op);
void register_request_result(uint32_t channel_id, void *buf, uint32_t size);
void clear_local_source(uint32_t channel_id);
void clear_request_result(uint32_t channel_id);
void init_router(config_entry_t *cfgs, int n, const char *router_name);
void INC(void);
~~~

request()/respond() 是 pull 语义：requester 在 request 中携带输入 payload，responder 返回聚合结果。request_push()/respond_push() 是 push 变体，控制请求和结果 payload 的方向由 responder 状态机决定。

config/config_api.h 提供配置入口：

~~~c
int lab_config_load(lab_config_t *config, const char *path);
int lab_config_prepare(const lab_config_t *config, const char *path);
uint32_t lab_config_ip_of_rank(const lab_config_t *config, int rank);
int lab_config_rank_of_name(const lab_config_t *config, const char *name);
int lab_node_is_router(const char *name);
~~~

lab_config_prepare() 会根据 ranks.cfg 所在目录自动寻找同目录的 graph.cfg 和 tree.cfg，先初始化邻居图，再加载 Fabric、tree 和 requester plan。

## 4. 配置系统

一个 topology 目录至少包含：

~~~
ranks.cfg   # rank 与容器接口/IP
graph.cfg   # responder 的 requester 邻居集合
tree.cfg    # router 设备、单播路由、组播路由、树角色、聚合计划
setup.sh    # 创建/删除容器和 veth；部分拓扑负责生成 tree.cfg
~~~

### 4.1 ranks.cfg

双 subchannel 配置格式为：

~~~
rank,host_name,host_iface0,router_iface0,host_iface1,router_iface1,host_ip
~~~

例如：

~~~
0,host1,host1-eth0,ra0-h1s0,host1-eth1,ra0-h1s1,10.0.0.1
~~~

config_entry_t 保存这些字段。SUBCHANNEL_COUNT=1 时使用单接口格式；当前默认值为 2。

### 4.2 graph.cfg

每行格式为：

~~~
vertex_rank,neighbor_rank0,neighbor_rank1,...
~~~

一行描述一个 responder 对应的 requester 集合。common_set_group() 先建立默认的“除自己外所有 rank”邻居掩码，再用 graph.cfg 覆盖。

config/arbor_fabric.c 依据该邻居图为每个 responder 建立一个 channel：

~~~
channel_id == responder_rank
requesters == graph.cfg 中该 rank 的邻居
~~~

因此完整 N-rank AllReduce 通常有 N 个 channel，每个 channel 的 responder 是对应 rank，其他 rank 是 requester。

### 4.3 tree.cfg

文件语法如下：

~~~
dev,router,port
route,router,responder_rank,subchannel,egress_port
mcast,router,responder_rank,subchannel,egress_port
tree,router,responder_rank,subchannel,LEVEL|RELAY,parent_up_port,parent_down_ingress_port
req,requester_rank,responder_rank,subchannel,stack_depth,fanin0,fanin1,fanin2
~~~

字段语义：

- dev：router 要打开的接口名。
- route：normal request/单播控制包到指定 responder 的出口。
- mcast：responder 发出的 response、REGISTER_ACK、END 等下行控制包的复制出口。
- tree：该 router 对指定 responder/subchannel 的树角色和父方向端口。
- req：requester 发包时使用的聚合栈深度和每层 fanin。只读取 stack_depth 个 fanin。

LEVEL 表示该 router 在下行 credit 路径上分配聚合 slot、压入 agg_loc，在上行 request 路径上按栈顶 fanin 聚合并弹栈。RELAY 只转发和复制，不分配聚合 slot，也不消耗聚合栈。

聚合栈按 root-to-leaf 存储，但 request 从 requester leaf 向 responder 方向转发，因此 router 总是使用最后一个有效元素：

~~~
level = agg_depth - 1
fanin = fanin[level]
agg_loc = agg_loc[level]
~~~

当前 tree3 是 8-rank 完全二叉树，根节点连接 router-l 和 router-r，三层都参与规约：

~~~
root LEVEL:   fanin = 7
middle LEVEL: fanin = 3（同一半）或 4（跨半）
leaf LEVEL:   fanin = 1（同一叶组）或 2（跨叶组）
~~~

因此 requester plan 形如：

~~~
req,4,0,0,3,7,4,2
~~~

表示 rank 4 到 responder rank 0 的 request 依次经过 root、middle、leaf 三个聚合层。topology/tree3/setup.sh generate 会按照 rank 所在叶组和半树自动生成这些 fanin。

### 4.4 四种内置拓扑

| 拓扑 | rank 数 | 结构 | 聚合层 |
| --- | ---: | --- | --- |
| star1 | 4 | 一个 router、一个 subchannel | 1 个 LEVEL |
| star | 4 | 两个并行 subchannel router | 每个 subchannel 1 个 LEVEL |
| tree | 4 | root + middle + 两个 leaf | root RELAY，middle/leaf LEVEL，深度 2 |
| tree3 | 8 | root + 两个 middle + 四个 leaf | root/middle/leaf 均 LEVEL，深度 3 |

topology/tree3/setup.sh 会生成 7 个 router、8 个 host、双 subchannel veth；setup 创建并启动环境，clean 删除容器，generate 只重新生成 tree.cfg。

## 5. 协议语义与状态机

### 5.1 Channel、subchannel 和 offset

- 一个 channel 对应一个 responder rank；channel id 默认就是 responder rank。
- 一个 channel 有 SUBCHANNEL_COUNT 条独立的 UDP 逻辑通道，默认是 2。
- 数据按 PAYLOAD_LEN=8192 bytes 分片，每个分片有 payload offset。
- credit offset 标识 responder 预留的接收窗口位置；requester 只能使用已经收到的 credit。
- 24-bit offset 和 ARBOR_SEQUENCE_MASK=0xFFFFFF 用于有限空间的序列计算。

### 5.2 正常 pull 流程

一次正常请求按以下顺序运行：

1. requester 在每个 subchannel 发送 REGISTER。
2. responder 检查注册 epoch/位图，发送 REGISTER_ACK；注册门关闭前不会发放正常 credit。
3. responder 为可用 payload offset 发送带 credit_valid 的 RESPONSE。下行经过每个 LEVEL router 时，router 分配 slot，并把 slot index 压入 agg_loc 栈。
4. requester 收到 credit 后建立本地 credit 队列，校验配置的 stack_depth 与 credit 实际深度一致，再发送 normal REQUEST。
5. request 沿 requester 到 responder 的路径向上走。每个 LEVEL router 取栈顶 agg_loc/fanin：
   - fanin == 1：不需要真正合并，弹栈后直接转发；
   - 收集未完成：slot 记录贡献并等待；
   - 收齐 fanin：把 payload 做 SUM/MAX/MIN，保留 master header，写入贡献计数并弹栈转发；
   - slot owner、fanin、payload shape 或聚合状态不匹配：改写为 header-only AGG_MISS。
6. responder 收到最终 master 或合法 control request 后，将远端贡献与本地 source 合并，缓存 primary response，并在对应 credit 上提交结果。
7. requester 接收 response payload，按 offset 去重写入目标 buffer；全部分片完成后发送 END_ACK。
8. responder 等待各 requester 的 END_ACK，完成 END 握手后回收 message 状态。

交换机只对 normal REQUEST 做聚合。REGISTER、REGISTER_ACK、RESPONSE、REPAIR_TRIGGER、REPAIR_REQUEST、END、END_ACK 和 AGG_MISS 都走旁路或树组播路径。

### 5.3 Push、control aggregation 与数据类型

请求头中的 request_kind 区分：

- ARBOR_REQ_NONE：普通控制/非聚合请求；
- ARBOR_REQ_AGGREGATE_CONTROL_ACK：允许聚合的控制确认；
- ARBOR_REQ_AGGREGATE_PAYLOAD：带 payload 的数据聚合。

操作码包括 SUM、MAX、MIN，数据类型包括 float16、bfloat16、float32、float64、int32、int64。当前 testbed AllReduce workload 使用 SUM + INT32；router 的 payload 聚合按 op 对定长 payload 做逐元素计算，responder 会拒绝不支持的 dtype 或形态不一致的请求。

### 5.4 丢包、AGG_MISS 与 repair

正常 credit 或 normal request 丢失时，协议不把 repair request 放入聚合栈：

1. requester 发现 credit/response 超时，或 router 产生 AGG_MISS。
2. responder 可发送 REPAIR_TRIGGER，要求 requester 进入 repair 状态。
3. requester 发送带 repair 标志的 REPAIR_REQUEST；router 对 repair 包旁路，不消费 normal agg stack。
4. responder 用 requester 位图收齐 repair 贡献，重放或提交 primary response。
5. repair 使用独立 token budget，避免恢复流量无限占用正常 credit 预算。

normal path 和 repair path 的提交结果都必须计入同一 payload offset；重复 response 通过 message/offset 状态去重。

### 5.5 拥塞、ECN 和收尾

responder 为每个 channel/subchannel 维护 credit gap、window、RTT 和 ECN 样本。只有正常路径的首次完整位图收齐事件用于 CC 采样；repair、重放和 AGG_MISS 不作为正常样本。

数据包 IPv4 ECN CE 位会随聚合结果向 master 传播。响应和 END 使用树组播表下发，END/END_ACK 完成后才释放 message 和 credit 状态。

## 6. 线上格式

### 6.1 传输层

~~~
Ethernet: 0x0800 IPv4
IPv4 protocol: 0x12 (ARBOR_IP_PROTO)
UDP destination: 10000 + channel_id * 4 + subchannel_id
~~~

reference/arbor-wire.h 还支持 (group, channel, subchannel) 的扩展端口空间；testbed 当前使用 group 0 的简单 channel 映射。

### 6.2 arbor_header_t

固定 20 字节，字段布局如下：

| 字节 | 字段 | 语义 |
| --- | --- | --- |
| 0 | byte0 | wire version、packet type、repair、operation |
| 1 | message_id | message 标识 |
| 2 | ctrl | payload valid/kind、credit valid、agg depth、aggregated |
| 3..5 | offset_a | request/payload offset，24 bit |
| 6..8 | offset_b | credit offset；聚合 master 时也承载贡献计数 |
| 9..10 | agg_loc[0] | root 级 slot index，网络字节序 |
| 11..13 | fanin[0..2] | 与聚合层对齐的 fanin |
| 14 | reserved | 必须为 0 |
| 15 | dtype | 数据类型 |
| 16..17 | agg_loc[1] | middle 级 slot index |
| 18..19 | agg_loc[2] | leaf 级 slot index |

agg_depth 最大为 ARBOR_MAX_STACK_DEPTH=3。wire/arbor_wire.h 提供 header view、24-bit offset、agg stack、fanin 和 sequence helper，wire/arbor_wire.c 负责构造完整 Ethernet/IP/UDP frame。

## 7. 核心数据结构

### 7.1 配置与 Fabric

config_entry_t（runtime/runtime_common.h）保存一个 rank 的主机名、双 subchannel 接口和 IPv4 地址。

ArborFabric（config/arbor_fabric.h）是加载后的全局拓扑：

~~~c
typedef struct {
    int rank_count;
    config_entry_t ranks[MAX_GROUP_SIZE];
    uint32_t channel_count;
    ArborChannelSpec channels[MAX_CHANNELS];
    ArborTreeSpec tree_specs[MAX_CHANNELS][SUBCHANNEL_COUNT];
} ArborFabric;
~~~

其中：

- ArborChannelSpec 保存 responder、requester 列表和 subchannel 数；
- ArborTreeSpec 保存 root、树层 router、requester ascend path；
- ArborRankPlan 保存某个 requester 到 responder 的 stack_depth、fanin 和可选 stack router 名称；
- ArborRouterNodeConfig 保存 dev、route、mcast、tree role、parent port 等 per-router 表项。

### 7.2 Wire 与 host 队列

arbor_header_t 是 packed 的线上头；arbor_header_view_t 是解析后的主机字节序视图。

rx_msg_t 是 host 接收队列的内部消息，除 wire 字段外还带完整 payload；conn_t 为每个远端连接维护 RXQ_SIZE=256 的环形队列。

credit 保存 requester 侧的一次 credit：

~~~c
typedef struct {
    uint32_t credit_offset;
    uint8_t agg_depth;
    uint8_t repair;
    uint32_t agg_stack[ARBOR_MAX_STACK_DEPTH];
    uint8_t fanin[ARBOR_MAX_STACK_DEPTH];
    uint32_t channel_id;
    uint32_t subchannel_id;
} credit;
~~~

protocol_message_t 描述一个 request/response message 的生命周期，包括 message id、epoch、起始 sequence、分片数、注册位图、END 状态、重试次数和完成标志。

host_channel_state_t 将一个 channel 的 requester/responder 状态集中保存：subchannel 上下文、source/result buffer、request/response message 表、pending response 队列、repair token 和 responder mutex。

### 7.3 Router slot 与聚合缓存

router 使用 AGTR_ARRAY_SIZE=2*WINDOW 个 slot，并以 (owner_udp_port, owner_offset, responder_ip) 绑定 credit：

~~~c
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
~~~

agtr_t 是逐元素聚合缓冲区。首个贡献包作为 master header 模板，payload 数值写入 agtr_t；收齐 fanin 后把聚合结果写回 master frame 并转发。

