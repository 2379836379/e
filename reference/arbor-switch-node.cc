#include "arbor-switch-node.h"

#include "ns3/ipv4-header.h"
#include "ns3/log.h"
#include "ns3/ppp-header.h"
#include "ns3/simulator.h"
#include "ns3/interface-tag.h"
#include "arbor-packet.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("ArborSwitchNode");
NS_OBJECT_ENSURE_REGISTERED(ArborSwitchNode);

TypeId ArborSwitchNode::GetTypeId(void) {
  static TypeId tid = TypeId("ns3::ArborSwitchNode")
                          .SetParent<SwitchNode>()
                          .AddConstructor<ArborSwitchNode>();
  return tid;
}

ArborSwitchNode::ArborSwitchNode() {
  m_stats.residenceHistNs.assign(kResidenceBuckets, 0);
  SetSlotCount(1024);
}

// ── 观测记账（fix 4）：slot 驻留分布与 per-channel 归属 ──
void ArborSwitchNode::RecordResidence(uint64_t durNs) {
  ++m_stats.residenceCount;
  m_stats.residenceSumNs += durNs;
  m_stats.residenceMaxNs = std::max(m_stats.residenceMaxNs, durNs);
  // 对数桶：1µs 起 ×2；<2µs 归桶 0，>= 1µs·2^23 归桶 23。
  uint64_t us = durNs / 1000;
  uint32_t b = 0;
  while (us >= 2 && b + 1 < kResidenceBuckets) {
    us >>= 1;
    ++b;
  }
  ++m_stats.residenceHistNs[b];
}

void ArborSwitchNode::ChanOnAlloc(uint16_t dport) {
  ChanSlotStats& c = m_stats.perChan[dport];
  ++c.allocs;
  ++c.liveSlots;
  c.peakLive = std::max(c.peakLive, c.liveSlots);
}
void ArborSwitchNode::ChanOnMaster(uint16_t dport) {
  ChanSlotStats& c = m_stats.perChan[dport];
  ++c.masters;
  if (c.liveSlots > 0) --c.liveSlots;
}
void ArborSwitchNode::ChanOnLeave(uint16_t dport) {
  ChanSlotStats& c = m_stats.perChan[dport];
  if (c.liveSlots > 0) --c.liveSlots;
}
void ArborSwitchNode::ChanOnAggMiss(uint16_t dport) {
  ++m_stats.perChan[dport].aggMiss;
}

ArborSwitchNode::SlotStateSnapshot ArborSwitchNode::SnapshotSlotStates() const {
  SlotStateSnapshot out;
  const uint64_t now = Simulator::Now().GetNanoSeconds();
  for (const Slot& s : m_slots) {
    if (!s.valid) {
      ++out.idle;
    } else if (s.forwarded) {
      ++out.completed;
    } else {
      if (s.count == 0) ++out.unbound;
      else ++out.partial;
      if (now - s.allocNs > m_staleNs) ++out.aged;
    }
  }
  return out;
}

void ArborSwitchNode::SetSlotCount(uint32_t n) {
  NS_ABORT_MSG_IF(n == 0, "Arbor slot 池深度必须 > 0");
  m_slots.assign(n, Slot{});
  m_allocPtr = 0;
}

void ArborSwitchNode::AddMulticastGroup(uint32_t dip,
                                        const std::vector<uint32_t>& ports) {
  m_mcast[dip] = ports;
}

// L2 模型扩展（逐条记录）：多级树角色表。依据 implementation.md §2.5
// 树构造——“根以下的下行路径只转发不聚合”“任一 requester 路径至多经过
// {本 leaf, 本 pod agg, 根} 三个聚合点，位置栈三项覆盖”。L1 的单交换机
// 拓扑（calib-4）中该表恒空、行为逐事件不变；多交换机拓扑若不区分
// 聚合级/中继段与上下行方向，途经交换机会为同一 credit 多压栈（>3 级
// 溢出 V3 位置栈）且上行部分聚合产物无法沿树经根，故该表是 §2.5 语义
// 在多级拓扑下的必要装配面，不改变任何聚合/失配/repair 规则。
void ArborSwitchNode::SetTreeNode(uint16_t dport, TreeRole role,
                                  uint32_t parentPort) {
  auto it = m_tree.find(dport);
  if (it != m_tree.end()) {
    NS_ABORT_MSG_IF(it->second.role != role ||
                        it->second.parentPort != parentPort,
                    "SetTreeNode 冲突：dport=" << dport
                        << " 已配置不同角色/父端口（helper 配置错误）");
    return;
  }
  m_tree[dport] = TreeNodeCfg{role, parentPort};
}

void ArborSwitchNode::AccountLive(int delta) {
  const uint64_t now = Simulator::Now().GetNanoSeconds();
  m_stats.liveTimeIntegralNs +=
      double(m_stats.liveSlots) * double(now - m_stats.lastEventNs);
  m_stats.lastEventNs = now;
  m_stats.liveSlots += delta;
  m_stats.peakLive = std::max(m_stats.peakLive, m_stats.liveSlots);
}

void ArborSwitchNode::AccountPartial(int delta) {
  const uint64_t now = Simulator::Now().GetNanoSeconds();
  m_stats.partialTimeIntegralNs +=
      double(m_stats.partialSlots) * double(now - m_stats.lastPartialEventNs);
  m_stats.lastPartialEventNs = now;
  NS_ABORT_MSG_IF(delta < 0 && m_stats.partialSlots == 0,
                  "部分填充 slot 记账下溢");
  m_stats.partialSlots += delta;
}

bool ArborSwitchNode::SwitchReceiveFromDevice(Ptr<NetDevice> device,
                                              Ptr<Packet> packet,
                                              CustomHeader& ch) {
  if (ch.l3Prot != ARBOR_PROTO) {
    return SwitchNode::SwitchReceiveFromDevice(device, packet, ch);
  }

  // 修补（编译/运行级，记录）：CustomHeader 不解析 0x12 的 L4，ch.udp.pg
  // 是未初始化内存；基类 SendToDev 用 ch.udp.pg 选 egress 队列。Arbor 流量
  // 统一钉在无损 PG=3（与 SendArborVia 的 qIndex 一致）。
  ch.udp.pg = 3;

  // 摘除 [PPP][IPv4][UDP+Arbor]，处理后回填再走基类转发。
  PppHeader ppp;
  Ipv4Header ip;
  ArborUdpHeader ah;
  packet->RemoveHeader(ppp);
  packet->RemoveHeader(ip);
  packet->RemoveHeader(ah);
  const uint32_t payloadLen = packet->GetSize();
  arbor::WireHeader& h = ah.h;

  // 入包 IP ECN（CE 回显/CE-OR 建模，implementation.md §2.1/§4）：normal
  // request 携带 requester 回显的 CE；聚合级 OR 进 slot.ceSeen，master 发出
  // 时按 ceSeen 置 CE 否则 ECT(0)。credit/控制/repair 直通路径由 restoreAndSend
  // 原样保留入头 ECN（ip2=ip），随后出口 RED 由基类 SwitchNotifyDequeue 处理。
  const bool inCe = ip.GetEcn() == Ipv4Header::ECN_CE;

  // L2 多级树：本包所属 (channel, subchannel) 在本交换机上的树角色。
  const uint32_t inPort = device->GetIfIndex();
  const TreeNodeCfg* tree = nullptr;
  {
    auto it = m_tree.find(ah.dport);
    if (it != m_tree.end()) tree = &it->second;
  }

  enum class Fw { UNICAST, MCAST, PORT };
  auto restoreAndSend = [&](Ptr<Packet> p, ArborUdpHeader& hdr, Fw mode,
                            uint32_t port) {
    hdr.payloadLen = p->GetSize();
    p->AddHeader(hdr);
    Ipv4Header ip2 = ip;
    ip2.SetPayloadSize(static_cast<uint16_t>(p->GetSize()));
    p->AddHeader(ip2);
    p->AddHeader(ppp);
    switch (mode) {
      case Fw::MCAST:
        MulticastArbor(p, ip.GetDestination().Get(), ch);
        break;
      case Fw::PORT:
        SendArborVia(port, p, ch);
        break;
      default:
        ForwardArbor(p, ch);
    }
  };
  auto restoreAndForward = [&](Ptr<Packet> p, ArborUdpHeader& hdr,
                               bool multicast) {
    restoreAndSend(p, hdr, multicast ? Fw::MCAST : Fw::UNICAST, 0);
  };

  using arbor::PacketType;
  switch (h.type) {
    case PacketType::RESPONSE: {
      // 配置错误一律 abort 无 fallback（fix 2）：途经本 dport 的 credit
      // 必有树角色（calib-4 单交换机也由 helper 装配根 LEVEL 角色）。
      NS_ABORT_MSG_IF(tree == nullptr,
                      "RESPONSE 无本 dport 树表项（helper 未装配树角色）");
      if (inPort != tree->parentPort) {
        // 根以下上行段（responder→根，§2.5“只转发不聚合”）：不压栈，
        // 沿树父端口继续上行。
        restoreAndSend(packet, ah, Fw::PORT, tree->parentPort);
        return true;
      }
      const bool isMcast = m_mcast.count(ip.GetDestination().Get()) != 0;
      NS_ABORT_MSG_IF(!isMcast,
                      "下行 response 无组播表项（helper 树配置错误）");
      if (!h.repair && h.creditValid && tree->role == TreeRole::LEVEL) {
        // 聚合级下行压栈：allocator 无条件分配（仿真无 pinned，恒成功）。
        // RELAY（中继段）不压栈只复制。
        uint32_t slotIdx = m_allocPtr;
        if (m_hashAddressing) {
          uint64_t x = (uint64_t(ah.dport) << 32) | h.offsetB;
          x ^= x >> 33;
          x *= 0xff51afd7ed558ccdull;
          x ^= x >> 33;
          slotIdx = static_cast<uint32_t>(x % m_slots.size());
        }
        Slot& s = m_slots[slotIdx];
        const uint64_t now = Simulator::Now().GetNanoSeconds();
        if (s.valid && !s.forwarded) {
          // 覆盖未完成聚合 slot：valid && !forwarded 即算金丝雀（含零贡献，
          // fix 3）；count>0 的子集另记 partialOverwrites（fix 4）。
          ++m_stats.overwriteLive;
          if (s.count > 0) {
            ++m_stats.partialOverwrites;
            AccountPartial(-1);
          }
          RecordResidence(now - s.allocNs);
          ChanOnLeave(s.ownerPort);
          AccountLive(-1);
        }
        s = Slot{};
        s.valid = true;
        s.ownerPort = ah.dport;
        s.ownerOffset = h.offsetB;  // credit_offset
        s.depthAtSlot = h.aggDepth;
        s.allocNs = now;
        // 位置栈溢出（树深>3）为配置错误，fail-fast（fix 2a）。
        NS_ABORT_MSG_IF(!h.PushAggLoc(static_cast<uint16_t>(slotIdx)),
                        "位置栈溢出：agg_depth 已达上限（树深>3 配置错误）");
        if (!m_hashAddressing) {
          m_allocPtr = (m_allocPtr + 1) % m_slots.size();
        }
        ++m_stats.allocs;
        ChanOnAlloc(ah.dport);
        AccountLive(+1);
      }
      restoreAndForward(packet, ah, isMcast);
      return true;
    }

    case PacketType::DATA_REQUEST: {
      if (h.repair || h.aggDepth == 0) {
        // 旁路/根后 master（aggDepth==0）/repair request。fix 5：有本 dport
        // 树表项且从非父端口进入（上行段）→ 沿父端口随本树上行；从父端口
        // 进入（根后下行 leg）或无表项 → 按 dip（根后 master 下行 responder）。
        if (tree != nullptr && inPort != tree->parentPort) {
          restoreAndSend(packet, ah, Fw::PORT, tree->parentPort);
        } else {
          restoreAndForward(packet, ah, false);
        }
        return true;
      }
      if (tree != nullptr && tree->role == TreeRole::RELAY) {
        // 纯中继：不弹栈不记账，沿树向根转发（相邻聚合级之间的转发段）。
        restoreAndSend(packet, ah, Fw::PORT, tree->parentPort);
        return true;
      }
      // 聚合级上行处理必有本 dport 树角色（fix 2：无 fallback）。
      NS_ABORT_MSG_IF(tree == nullptr,
                      "上行聚合无本 dport 树表项（helper 未装配树角色）");
      m_stats.bytesInData += payloadLen;
      HandleUpstream(packet, h, ah.dport, payloadLen, ch,
                     static_cast<int>(tree->parentPort), inCe);
      // HandleUpstream 内部完成转发/吞包/发出聚合产物。
      return true;
    }

    default: {
      // REGISTER / REGISTER_ACK / END / END_ACK / AGG_MISS：旁路直通。
      // fix 5：有本 dport 树表项且从非父端口进入（上行段）→ 沿本树父端口
      // 上行（与本树修复流量同路径，避免跨树串路）。这统一覆盖：
      //   - 下行控制（END/REGISTER_ACK）在根以下的上行段先送父端口达根；
      //   - 旁路单播（REGISTER/END_ACK/AGG_MISS）沿树上行至根。
      // 从父端口进入（根/leg 的下行段）或无树表项 → 下行分发：
      //   - 下行控制经组播树 PRE 复制；单播类按 dip 路由。
      const bool downlinkType =
          h.type == PacketType::END || h.type == PacketType::REGISTER_ACK;
      if (tree != nullptr && inPort != tree->parentPort) {
        restoreAndSend(packet, ah, Fw::PORT, tree->parentPort);
        return true;
      }
      const bool isMcast =
          m_mcast.count(ip.GetDestination().Get()) != 0 && downlinkType;
      NS_ABORT_MSG_IF(downlinkType && tree != nullptr && !isMcast,
                      "下行控制包无组播表项（helper 树配置错误）");
      restoreAndForward(packet, ah, isMcast);
      return true;
    }
  }
}

// 摘除头后的上行处理。packet 仅含 payload；ip/ppp 由调用方语境重建——
// 这里持有原 ip/ppp 的副本代价高，改为在本函数内自建转发帧：
//   - 继续上行：agg_depth>0（聚合产物/弹栈直转仍有待处理级）沿树父端口
//     经根上行；agg_depth==0（根后 master）单播至原目的（responder）。
//   - AGG_MISS：header-only 单播至原目的。
void ArborSwitchNode::HandleUpstream(Ptr<Packet> p, arbor::WireHeader& h,
                                     uint16_t udpDst, uint32_t payloadLen,
                                     CustomHeader& ch, int parentPort,
                                     bool inCe) {
  InterfaceTag ingressTag;
  NS_ABORT_MSG_IF(!p->PeekPacketTag(ingressTag),
                  "Arbor 上行包缺少 ingress InterfaceTag");
  const uint8_t f = h.TopFanin();
  const bool ctrlPkt = payloadLen == 0;

  // ce：写入出头的 ECN（CE 否则 ECT(0)）。bypassUnicast：AGG_MISS 等旁路
  // 单播沿本树父端口上行（fix 5），根/leg 处再按 dip；正常聚合/直转输出
  // 按 agg_depth 决定沿父端口上行（未到根）或按 dip 直达 responder（根后）。
  auto forwardAs = [&](Ptr<Packet> body, const arbor::WireHeader& nh,
                       uint32_t plen, bool ce, bool bypassUnicast) {
    ArborUdpHeader out;
    out.sport = 0;
    out.dport = udpDst;
    out.h = nh;
    out.payloadLen = plen;
    body->AddHeader(out);
    Ipv4Header ip;
    ip.SetSource(Ipv4Address(ch.sip));
    ip.SetDestination(Ipv4Address(ch.dip));
    ip.SetProtocol(ARBOR_PROTO);
    ip.SetTtl(64);
    ip.SetEcn(ce ? Ipv4Header::ECN_CE : Ipv4Header::ECN_ECT0);
    ip.SetPayloadSize(static_cast<uint16_t>(body->GetSize()));
    body->AddHeader(ip);
    PppHeader ppp;
    ppp.SetProtocol(0x0021);
    body->AddHeader(ppp);
    if (bypassUnicast) {
      // 旁路单播沿本树上行（有父端口时），根/leg 处回落 dip。
      if (parentPort >= 0) {
        SendArborVia(static_cast<uint32_t>(parentPort), body, ch);
      } else {
        ForwardArbor(body, ch);
      }
    } else if (nh.aggDepth > 0) {
      // 位置栈未弹空：本级不是根，聚合产物/直转输出必须沿树上行。
      NS_ABORT_MSG_IF(parentPort < 0,
                      "多级树：agg_depth>0 的上行输出需要父端口"
                      "（helper 未配置本级树角色）");
      SendArborVia(static_cast<uint32_t>(parentPort), body, ch);
    } else {
      ForwardArbor(body, ch);
    }
  };

  // f == 1：聚合规则的恒等式退化情形——弹栈直转，不触碰 slot；入头 ECN
  // 原样写出（§2.5 f==1 与统一规则观测等价：CE 透传）。
  if (f == 1 || (ctrlPkt && !m_controlAggregation)) {
    // 聚合数据通路仍是直接转发；这里仅结束 credit 分配产生的逻辑 live
    // 记账。若该位置已被覆盖，f==1 仍无需 owner 命中即可正确转发。
    Slot& promised = m_slots[h.TopAggLoc() % m_slots.size()];
    if (promised.valid && !promised.forwarded &&
        promised.ownerPort == udpDst && promised.ownerOffset == h.offsetA) {
      RecordResidence(Simulator::Now().GetNanoSeconds() - promised.allocNs);
      ChanOnLeave(promised.ownerPort);
      promised.forwarded = true;
      AccountLive(-1);
    }
    h.PopAggLoc();
    ++m_stats.popForward;
    m_stats.bytesOutData += payloadLen;
    forwardAs(p, h, payloadLen, inCe, /*bypassUnicast=*/false);
    return;
  }

  Slot& s = m_slots[h.TopAggLoc() % m_slots.size()];
  const uint16_t contribution =
      h.aggregated ? static_cast<uint16_t>(h.offsetB) : 1;
  const bool ownerMatch = s.valid && s.ownerPort == udpDst &&
                          s.ownerOffset == h.offsetA;
  const bool shapeOk =
      s.fanin == 0 || (s.op == h.op && s.isCtrl == ctrlPkt && s.fanin == f &&
                       payloadLen == s.masterPayloadLen);
  const bool overflow =
      s.fanin != 0 && (s.forwarded ||
                       s.count + contribution > s.fanin);

  if (!ownerMatch || !shapeOk || overflow || contribution == 0) {
    // 统一失配路径：丢 payload，改写 header-only AGG_MISS（固定 ECT(0)，
    // 不参与 CE 归约），沿本树父端口上行至 responder。
    ++m_stats.aggMiss;
    ChanOnAggMiss(udpDst);
    h.RewriteToAggMiss();
    Ptr<Packet> miss = Create<Packet>(0);
    miss->AddPacketTag(ingressTag);
    forwardAs(miss, h, 0, /*ce=*/false, /*bypassUnicast=*/true);
    return;
  }

  if (s.fanin == 0) {
    // 首个输入绑定聚合参数并成为聚合产物模板。
    s.fanin = f;
    s.op = h.op;
    s.isCtrl = ctrlPkt;
    s.master = p->Copy();
    s.masterHdr = h;
    s.masterPayloadLen = payloadLen;
  }
  if (s.count == 0) AccountPartial(+1);
  // slot CE-OR：任一贡献（含首输入）携带 CE 即置位；emit 时决定 master ECN。
  s.ceSeen = s.ceSeen || inCe;
  s.count = static_cast<uint16_t>(s.count + contribution);
  if (ctrlPkt) ++m_stats.ctrlInputs; else ++m_stats.aggInputs;

  if (s.count == s.fanin) {
    // 凑满：以首贡献为模板发出聚合产物，弹栈、写累计贡献数。
    arbor::WireHeader mh = s.masterHdr;
    mh.aggregated = true;
    mh.offsetB = s.count;
    mh.PopAggLoc();
    Ptr<Packet> master = s.master ? s.master->Copy()
                                  : Create<Packet>(s.masterPayloadLen);
    m_stats.bytesOutData += s.masterPayloadLen;
    ++m_stats.masters;
    const bool emitCe = s.ceSeen;
    if (emitCe) ++m_stats.mastersCe;
    RecordResidence(Simulator::Now().GetNanoSeconds() - s.allocNs);
    ChanOnMaster(s.ownerPort);
    s.forwarded = true;
    s.master = nullptr;
    AccountPartial(-1);
    AccountLive(-1);
    forwardAs(master, mh, s.masterPayloadLen, emitCe, /*bypassUnicast=*/false);
  }
  // 未凑满：输入被消耗（计数已入 slot），不转发。
}

void ArborSwitchNode::ForwardArbor(Ptr<Packet> p, CustomHeader& ch) {
  const int idx = GetOutDev(p, ch);
  NS_ABORT_MSG_IF(idx < 0, "Arbor 路由查找失败");
  SendArborVia(static_cast<uint32_t>(idx), p, ch);
}

void ArborSwitchNode::MulticastArbor(Ptr<Packet> p, uint32_t dip,
                                     CustomHeader& ch) {
  auto it = m_mcast.find(dip);
  NS_ABORT_MSG_IF(it == m_mcast.end(),
                  "Arbor multicast group not configured (dip=" << dip
                  << ") — helper 配置错误，禁止静默降级为单播");
  for (uint32_t port : it->second) {
    Ptr<Packet> copy = p->Copy();
    SendArborVia(port, copy, ch);
  }
}

// 复刻 SendToDev 的准入/入队段（qIndex 固定为无损 PG=3）。
void ArborSwitchNode::SendArborVia(uint32_t idx, Ptr<Packet> p,
                                   CustomHeader& ch) {
  const uint32_t qIndex = 3;
  InterfaceTag t;
  p->PeekPacketTag(t);
  const uint32_t inDev = t.GetPortId();
  if (m_mmu->CheckIngressAdmission(inDev, qIndex, p->GetSize(), false, 0) &&
      m_mmu->CheckEgressAdmission(idx, qIndex, p->GetSize(), false, 0)) {
    m_mmu->UpdateIngressAdmission(inDev, qIndex, p->GetSize(), false, 0);
    m_mmu->UpdateEgressAdmission(idx, qIndex, p->GetSize(), false);
    if (m_pfcEnabled) CheckAndSendPfc(inDev, qIndex);
  } else {
    NS_ABORT_MSG("Arbor lossless admission failed — PFC/buffer 配置错误，"
                 "禁止静默丢包");
  }
  m_devices[idx]->SwitchSend(qIndex, p, ch);
}

}  // namespace ns3
