/**
 * arbor-switch-node.h — 带 Arbor 聚合加速的交换机节点。
 *
 * 继承 HPCC 谱系的 SwitchNode（PFC/ECN/共享 buffer 照常工作），对
 * l3Prot == ARBOR_PROTO 的包执行 V3 聚合数据面（implementation.md §2.5）：
 *   - 下行 normal credit：allocator 无条件分配、压栈（本级 slot 绑定
 *     owner=(udp_dst, credit_offset)）；repair credit 直通。
 *   - 上行 normal request/master：f = fanin[depth-1]；f==1 弹栈直转
 *     （聚合规则的恒等式退化情形，不触碰 slot）；owner 失配/已凑满/
 *     贡献溢出/形态不一致 → 改写 header-only AGG_MISS；否则计数聚合，
 *     凑满弹栈发出聚合结果（贡献计数写 offset_b）。
 *   - REGISTER/END/END_ACK/REGISTER_ACK/repair/AGG_MISS 一律旁路。
 *   - 组播：response/END 的目的 IP 命中组播表时按 PRE 复制到出端口
 *     集合（per-tree 端口列表由 helper 配置）。
 *
 * 仿真口径：payload 数值不建模（数值正确性由 testbed 验证），聚合 =
 * 计数 + 以首贡献包为模板发出聚合产物；事件级 slot 记账（G1a）与
 * 每类型字节计数（G-ML 每级削减）内建。
 */
#ifndef ARBOR_SWITCH_NODE_H
#define ARBOR_SWITCH_NODE_H

#include <map>
#include <unordered_map>
#include <vector>

#include "ns3/switch-node.h"
#include "arbor-wire.h"

namespace ns3 {

static constexpr uint8_t ARBOR_PROTO = 0x12;  ///< 仿真内 Arbor 的 IP 协议号

class ArborSwitchNode : public SwitchNode {
 public:
  static TypeId GetTypeId(void);
  ArborSwitchNode();

  /// 聚合 slot 池深度（allocator 轮转深度 A）。
  void SetSlotCount(uint32_t n);
  void SetPfcEnabled(bool enabled) { m_pfcEnabled = enabled; }
  void SetControlAggregation(bool enabled) { m_controlAggregation = enabled; }
  void SetHashAddressing(bool enabled) { m_hashAddressing = enabled; }
  /// 组播表：dip（组播 IP，主机序）→ 出端口列表。credit/END 下行复制。
  void AddMulticastGroup(uint32_t dip, const std::vector<uint32_t>& ports);

  // ── L2 多级树配置（implementation.md §2.5 树构造；helper 装配）──
  // 单交换机（calib-4/star-64）无需配置：无表项时保持 L1 行为（恒压栈、
  // 上行按 dip 路由，与既有 calib-4 逐事件一致）。多交换机拓扑中，
  // (channel, subchannel) 树上每台途经交换机都必须配置角色 + 父端口
  // （父 = 通往树根方向的端口；根自身的父 = 通往 responder 方向）：
  //  - LEVEL（聚合级）：normal credit 仅当从父端口进入（= 沿树下行）时
  //    分配压栈；从其他端口进入（= 根以下 responder→根 的上行段，§2.5
  //    “根以下的下行路径只转发不聚合”）只转发不压栈。上行 request 照常
  //    弹栈/聚合，输出 agg_depth>0 → 从父端口发出（沿树上行经根），
  //    ==0 → 按 dip 路由（根后 master 直达 responder）。
  //  - RELAY（纯中继：根以下段、asym 类拓扑的无汇合深链）：不压栈不
  //    弹栈不记账；下行方向（从父端口进入）按组播表复制，上行方向
  //    （其余端口进入）原样从父端口转发。
  enum class TreeRole : uint8_t { LEVEL = 0, RELAY = 1 };
  void SetTreeNode(uint16_t dport, TreeRole role, uint32_t parentPort);

  bool SwitchReceiveFromDevice(Ptr<NetDevice> device, Ptr<Packet> packet,
                               CustomHeader& ch);

  /// 对数驻留直方图桶数（1µs 起 ×2；桶 23 ≈ 8.4s 上溢）。
  static constexpr uint32_t kResidenceBuckets = 24;

  /// per-channel（按 slot owner dport）归属统计。G5.1 per-job 时间线数据源；
  /// dport→job 映射由 harness 层做。签名保持稳定（下一波 stats agent 消费）。
  struct ChanSlotStats {
    uint64_t allocs = 0;     ///< 该 dport 上的 slot 分配次数
    uint64_t masters = 0;    ///< 该 dport 上发出的聚合产物数
    uint64_t aggMiss = 0;    ///< 该 dport 上改写为 AGG_MISS 的失配数
    uint64_t liveSlots = 0;  ///< 该 dport 当前 live（分配未完成）
    uint64_t peakLive = 0;   ///< 该 dport live 峰值
  };

  // ── 统计（事件级；G1a / G-ML / G5 直接读取） ──
  struct SlotStats {
    uint64_t allocs = 0;            ///< allocator 推进次数
    uint64_t overwriteLive = 0;     ///< 覆盖了未完成聚合的 slot（金丝雀；含零贡献）
    uint64_t partialOverwrites = 0; ///< 覆盖时 count>0 且未 forwarded 的子集
    uint64_t aggInputs = 0;         ///< 计入聚合的输入数
    uint64_t ctrlInputs = 0;        ///< 控制聚合输入数
    uint64_t masters = 0;           ///< 发出的聚合产物数
    uint64_t mastersCe = 0;         ///< 其中携带 CE（ceSeen）的聚合产物数
    uint64_t aggMiss = 0;           ///< 改写为 AGG_MISS 的失配包数
    uint64_t popForward = 0;        ///< f==1 弹栈直转数
    uint64_t liveSlots = 0;         ///< 当前 live（分配未完成）
    uint64_t peakLive = 0;          ///< live 峰值
    double liveTimeIntegralNs = 0;  ///< ∫live dt（稳态时间平均 = 积分/时长）
    uint64_t lastEventNs = 0;
    uint64_t partialSlots = 0;      ///< 当前已有输入但未凑满的 slot
    double partialTimeIntegralNs = 0;  ///< ∫partial dt
    uint64_t lastPartialEventNs = 0;
    uint64_t bytesInData = 0;       ///< 上行进入本级的数据字节
    uint64_t bytesOutData = 0;      ///< 本级向上发出的数据字节（削减量=in-out）
    // ── slot 驻留分布（emit/覆盖时按 now-allocNs 累进）──
    std::vector<uint64_t> residenceHistNs;  ///< 对数桶（kResidenceBuckets 个）
    uint64_t residenceCount = 0;    ///< 计入直方图的 slot 数
    uint64_t residenceSumNs = 0;    ///< 驻留时长总和（均值 = sum/count）
    uint64_t residenceMaxNs = 0;    ///< 最长观测驻留时间
    // ── per-channel 归属（owner dport → 统计）──
    std::map<uint16_t, ChanSlotStats> perChan;
  };
  const SlotStats& GetStats() const { return m_stats; }

  /// ns-3 原子 packet 模型中的 slot 状态。unbound/partial/completed 互斥；
  /// aged 是 unbound+partial 中驻留超过阈值的子集。硬件 emit 的 pinned
  /// 周期不在 packet-level 仿真中展开，由 testbed 单独测量。
  struct SlotStateSnapshot {
    uint32_t idle = 0;
    uint32_t unbound = 0;   ///< 已分配，尚无 contribution
    uint32_t partial = 0;   ///< 已有 contribution，尚未凑满
    uint32_t completed = 0; ///< 已凑满或 fanin=1 已直转，等待覆盖
    uint32_t aged = 0;      ///< unbound/partial 中超过 stale 阈值的项
  };
  SlotStateSnapshot SnapshotSlotStates() const;
  /// stale 判定阈值（默认 10ms）；harness 可覆盖。
  void SetStaleThresholdNs(uint64_t ns) { m_staleNs = ns; }

 private:
  struct Slot {
    bool valid = false;
    bool forwarded = false;
    bool isCtrl = false;
    bool ceSeen = false;
    uint16_t ownerPort = 0;
    uint32_t ownerOffset = 0;
    uint16_t count = 0;
    uint16_t fanin = 0;   ///< 0 = 尚未由首个输入绑定
    uint8_t op = 0;
    uint8_t depthAtSlot = 0;          ///< 分配时的栈深（emit 时弹到 depth-1）
    Ptr<Packet> master;               ///< 首贡献包（聚合产物模板）
    arbor::WireHeader masterHdr;      ///< 首贡献的头（emit 时改写）
    uint32_t masterPayloadLen = 0;
    uint64_t allocNs = 0;
  };

  struct TreeNodeCfg {
    TreeRole role = TreeRole::LEVEL;
    uint32_t parentPort = 0;
  };

  void HandleDownstream(Ptr<Packet> p, arbor::WireHeader& h, uint16_t udpDst,
                        CustomHeader& ch);
  void HandleUpstream(Ptr<Packet> p, arbor::WireHeader& h, uint16_t udpDst,
                      uint32_t payloadLen, CustomHeader& ch, int parentPort,
                      bool inCe);
  void ForwardArbor(Ptr<Packet> p, CustomHeader& ch);
  void MulticastArbor(Ptr<Packet> p, uint32_t dip, CustomHeader& ch);
  void SendArborVia(uint32_t idx, Ptr<Packet> p, CustomHeader& ch);
  void AccountLive(int delta);
  void AccountPartial(int delta);
  // ── 观测记账（fix 4）──
  void RecordResidence(uint64_t durNs);       ///< 驻留直方图 + 计数/和
  void ChanOnAlloc(uint16_t dport);           ///< perChan：alloc + live++
  void ChanOnMaster(uint16_t dport);          ///< perChan：masters++ + live--
  void ChanOnLeave(uint16_t dport);           ///< perChan：覆盖 live--
  void ChanOnAggMiss(uint16_t dport);         ///< perChan：aggMiss++

  std::vector<Slot> m_slots;
  uint32_t m_allocPtr = 0;
  bool m_pfcEnabled = true;
  bool m_controlAggregation = true;
  bool m_hashAddressing = false;
  std::unordered_map<uint32_t, std::vector<uint32_t>> m_mcast;
  std::unordered_map<uint16_t, TreeNodeCfg> m_tree;  ///< dport → 树角色
  uint64_t m_staleNs = 10ull * 1000 * 1000;          ///< stale 阈值（默认 10ms）
  SlotStats m_stats;
};

}  // namespace ns3

#endif  // ARBOR_SWITCH_NODE_H
