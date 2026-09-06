/**
 * arbor-host.cc — Arbor 端侧协议状态机实现（implementation.md §4/§5 的
 * V3 协议级移植）。语义对照见 arbor-host.h 头注释；本文件只做协议行为，
 * 不建模 mbuf/NUMA/环形缓冲（§9 平台无关契约：map + 非限制性护栏）。
 *
 * 收发路径：
 *  - TX：构 [PPP][IPv4 proto=0x12][ArborUdpHeader][payload] 后经
 *    QbbNetDevice::RdmaEnqueueHighPrioQ 注入 ackQ（q3，无损 PG）。
 *    PFC pause(PG3) 在 GetNextQindex 停住整个 ackQ；这是 PFC 的端口/优先级
 *    语义，同一 PG 内不提供 per-channel 选择性暂停。
 *  - RX：QbbNetDevice::Receive 的 Arbor 钩子（l3Prot==0x12 →
 *    m_arborReceiveCb，fork patch 位置见 qbb-net-device.{h,cc}）。
 */
#include "arbor-host.h"

#include <algorithm>

#include "ns3/abort.h"
#include "ns3/custom-header.h"
#include "ns3/ipv4-header.h"
#include "ns3/log.h"
#include "ns3/node.h"
#include "ns3/packet.h"
#include "ns3/ppp-header.h"
#include "ns3/qbb-net-device.h"
#include "ns3/simulator.h"

#include "arbor-packet.h"
#include "arbor-switch-node.h"  // ARBOR_PROTO

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("ArborHost");
NS_OBJECT_ENSURE_REGISTERED(ArborHost);

namespace {
constexpr uint32_t kPayloadSize = 8192;       // implementation.md §7
constexpr uint32_t kMaxRepairRetries = 64;    // REPAIR_MAX_RETRIES
constexpr double kRepairBurstBytes = 1024.0 * 1024.0;  // 突发 1 MB
constexpr uint32_t kWireOverhead = 62;        // 14+20+8+20（预算口径）

inline ArborRankBitmap FullMask(uint32_t n) {
  ArborRankBitmap out{};
  const uint32_t fullWords = n / 64;
  for (uint32_t i = 0; i < fullWords; ++i) out[i] = ~0ull;
  if ((n & 63u) != 0) out[fullWords] = (1ull << (n & 63u)) - 1ull;
  return out;
}
inline bool RankBitSet(const ArborRankBitmap& b, uint32_t rank) {
  return (b[rank >> 6u] & (1ull << (rank & 63u))) != 0;
}
inline void SetRankBit(ArborRankBitmap& b, uint32_t rank) {
  b[rank >> 6u] |= 1ull << (rank & 63u);
}
inline uint64_t NowNs() { return Simulator::Now().GetNanoSeconds(); }
}  // namespace

TypeId ArborHost::GetTypeId(void) {
  static TypeId tid = TypeId("ns3::ArborHost")
                          .SetParent<Object>()
                          .AddConstructor<ArborHost>();
  return tid;
}

// ─────────────────────────── 装配 ───────────────────────────

void ArborHost::Setup(Ptr<Node> node, Ptr<QbbNetDevice> dev, uint32_t myIp,
                      arbor::CcMode mode, const arbor::CcParams& params,
                      uint32_t startupWindow, uint64_t repairTimeoutNs,
                      uint64_t repairBudgetBps) {
  m_node = node;
  m_dev = dev;
  m_myIp = myIp;
  m_ccMode = mode;
  m_ccParams = params;
  m_startupWindow = std::max(1u, startupWindow);
  m_repairTimeoutNs = repairTimeoutNs;
  m_repairBudgetBps = repairBudgetBps;
}

void ArborHost::AddChannel(const ChannelConfig& cfg) {
  NS_ABORT_MSG_IF(cfg.numRequesters < 1,
                  "channel 至少需要一个 requester");
  NS_ABORT_MSG_IF(cfg.subchannels.empty(), "channel 至少一个 subchannel");
  NS_ABORT_MSG_IF(cfg.numRequesters > arbor::kMaxRanksPerGroup,
                  "requester 数超过仿真位图上限");
  NS_ABORT_MSG_IF(!cfg.isResponder &&
                      cfg.myRequesterIndex >= cfg.numRequesters,
                  "requester 位图下标越界");
  // 组内 channel id 位宽 = kMaxRanksPerGroup（也是 requester 位图上限）；
  // 更大全局 channel 空间靠 groupId 维扩展（端口算术 group 维已备）。
  NS_ABORT_MSG_IF(cfg.channelId >= arbor::kMaxRanksPerGroup,
                  "channelId 必须 < kMaxRanksPerGroup（组内位宽），"
                  "更大规模用 groupId 维");
  NS_ABORT_MSG_IF(cfg.groupId >= arbor::kMaxGroups, "groupId 越界");
  const uint32_t key = arbor::ChannelKey(cfg.groupId, cfg.channelId);
  if (cfg.isResponder) {
    NS_ABORT_MSG_IF(cfg.requesterIps.size() != cfg.numRequesters,
                    "responder 需要 requesterIps 位图映射");
    NS_ABORT_MSG_IF(m_asResponder.count(key) != 0,
                    "(group,channel) 复合键重复装配 responder");
    ResponderChannel& sc = m_asResponder[key];
    sc.cfg = cfg;
    sc.key = key;
    for (uint32_t i = 0; i < cfg.requesterIps.size(); ++i) {
      NS_ABORT_MSG_IF(!sc.requesterRank.emplace(cfg.requesterIps[i], i).second,
                      "requester IP 重复");
    }
    sc.cc.resize(cfg.subchannels.size());
    const uint32_t share = std::max<uint32_t>(
        1, m_startupWindow / static_cast<uint32_t>(cfg.subchannels.size()));
    for (auto& cs : sc.cc) {
      arbor::ArborCc::Reset(cs, m_ccMode, m_ccParams, share);
    }
    sc.repairTokens = kRepairBurstBytes;
    sc.repairRefillNs = NowNs();
  } else {
    NS_ABORT_MSG_IF(m_asRequester.count(key) != 0,
                    "(group,channel) 复合键重复装配 requester");
    RequesterChannel& rc = m_asRequester[key];
    rc.cfg = cfg;
    rc.key = key;
  }
  m_channelIdToKey[cfg.channelId] = key;  // Submit 遗留（group-less）解析
}

uint32_t ArborHost::SubStartupWindow(const ResponderChannel& sc) const {
  return std::max<uint32_t>(
      1, m_startupWindow / static_cast<uint32_t>(sc.cfg.subchannels.size()));
}

int64_t ArborHost::Submit(const MessageDesc& desc) {
  // dtype 双重 gate 的 API 层：不支持在网归约的 dtype 直接拒绝——协议
  // 无绕行模式（无 no_ina / INA-off）。控制聚合（op==NOP，含 push 类
  // Broadcast/AllGather）与 dtype 无关：push 无归约，任意 dtype 合法，
  // -ENOTSUP 只落在归约原语（op!=NOP）。
  if (desc.op != arbor::Op::NOP && !arbor::DatatypeInaSupported(desc.dtype)) {
    return -1;
  }
  const uint32_t total = static_cast<uint32_t>(
      std::max<uint64_t>(1, (desc.bytes + kPayloadSize - 1) / kPayloadSize));
  NS_ABORT_MSG_IF(total >= (1u << 23), "单条 message 必须短于 2^23 个包");

  // 复合键解析：先按 (groupId, channelId) 直解；未命中再按逻辑 channelId
  // 走遗留二级索引（harness 全局唯一 channelId、不带 groupId 的调用方）。
  uint32_t key = arbor::ChannelKey(desc.groupId, desc.channelId);
  if (m_asResponder.count(key) == 0 && m_asRequester.count(key) == 0) {
    auto sit = m_channelIdToKey.find(desc.channelId);
    if (sit != m_channelIdToKey.end()) key = sit->second;
  }

  auto rit = m_asResponder.find(key);
  if (rit != m_asResponder.end()) {
    ResponderChannel& sc = rit->second;
    RespMessage m;
    m.startSeq = sc.nextSeq;
    m.totalPackets = total;
    m.msgId = static_cast<uint8_t>(sc.nextMsgId++ & 0xFF);
    m.desc = desc;
    m.submitNs = NowNs();
    m.regBitmap.assign(sc.cfg.subchannels.size(), ArborRankBitmap{});
    sc.nextSeq = arbor::SeqAdd(sc.nextSeq, total);
    const int64_t start = m.startSeq;
    sc.messages.emplace(static_cast<uint32_t>(start), std::move(m));
    IssueCredits(sc);  // 注册门未开时无害
    return start;
  }

  auto qit = m_asRequester.find(key);
  NS_ABORT_MSG_IF(qit == m_asRequester.end(), "Submit 到未配置的 channel");
  RequesterChannel& rc = qit->second;
  ReqMessage rm;
  rm.startSeq = rc.nextSeq;
  rm.totalPackets = total;
  rm.msgId = static_cast<uint8_t>(rc.nextMsgId++ & 0xFF);
  rm.bytes = desc.bytes;
  rm.pull = desc.pull;
  rm.op = desc.op;
  rm.dtype = desc.dtype;
  rc.nextSeq = arbor::SeqAdd(rc.nextSeq, total);
  const int64_t start = rm.startSeq;
  rc.messages[rm.startSeq] = rm;
  for (uint32_t s = 0; s < rc.cfg.subchannels.size(); ++s) {
    rc.registerPending.emplace_back(rm.startSeq, static_cast<uint8_t>(s));
  }
  ScheduleRegister(rc);
  return start;
}

// ─────────────────────────── 通用工具 ───────────────────────────

uint32_t ArborHost::RankOfIp(const ResponderChannel& sc, uint32_t ip) const {
  auto it = sc.requesterRank.find(ip);
  if (it != sc.requesterRank.end()) return it->second;
  NS_ABORT_MSG("未知 requester 源 IP=" << ip << " — helper 配置错误");
  return 0;
}

uint32_t ArborHost::OffsetBytes(uint32_t startSeq, uint32_t totalPackets,
                                uint64_t bytes, uint32_t offset) const {
  if (bytes == 0) return 0;  // 控制消息（Barrier 类）
  const uint32_t idx = arbor::SeqDist(offset, startSeq);
  NS_ABORT_MSG_IF(idx >= totalPackets, "offset 不属于该 message");
  if (idx + 1 < totalPackets) return kPayloadSize;
  const uint32_t tail = static_cast<uint32_t>(bytes % kPayloadSize);
  return tail == 0 ? kPayloadSize : tail;
}

ArborHost::RespMessage* ArborHost::FindRespMessage(ResponderChannel& sc,
                                                   uint32_t offset) {
  if (sc.messages.empty()) return nullptr;
  auto it = sc.messages.upper_bound(offset);
  if (it != sc.messages.begin()) {
    auto prev = std::prev(it);
    if (arbor::SeqDist(offset, prev->second.startSeq) <
        prev->second.totalPackets) {
      return &prev->second;
    }
  }
  // 活跃区间跨 24-bit 回绕时，数值最大的 startSeq 可能覆盖低 offset。
  auto last = std::prev(sc.messages.end());
  if (arbor::SeqDist(offset, last->second.startSeq) < last->second.totalPackets) {
    return &last->second;
  }
  return nullptr;
}

ArborHost::ReqMessage* ArborHost::FindReqMessage(RequesterChannel& rc,
                                                 uint32_t offset) {
  if (rc.messages.empty()) return nullptr;
  auto it = rc.messages.upper_bound(offset);
  if (it != rc.messages.begin()) {
    auto prev = std::prev(it);
    if (arbor::SeqDist(offset, prev->second.startSeq) <
        prev->second.totalPackets) {
      return &prev->second;
    }
  }
  auto last = std::prev(rc.messages.end());
  if (arbor::SeqDist(offset, last->second.startSeq) < last->second.totalPackets) {
    return &last->second;
  }
  return nullptr;
}

void ArborHost::SendPacket(uint32_t dip, uint16_t dport,
                           const arbor::WireHeader& h, uint32_t payloadLen,
                           bool markCe) {
  Ptr<Packet> p = Create<Packet>(payloadLen);
  ArborUdpHeader ah;
  ah.sport = 0;
  ah.dport = dport;
  ah.h = h;
  ah.payloadLen = payloadLen;
  p->AddHeader(ah);
  Ipv4Header ip;
  ip.SetSource(Ipv4Address(m_myIp));
  ip.SetDestination(Ipv4Address(dip));
  ip.SetProtocol(ARBOR_PROTO);
  ip.SetTtl(64);
  ip.SetPayloadSize(static_cast<uint16_t>(p->GetSize()));
  // normal request/response 置 ECT(0)；CE 回显时升级为 CE（§2.1）。
  ip.SetEcn(markCe ? Ipv4Header::ECN_CE : Ipv4Header::ECN_ECT0);
  p->AddHeader(ip);
  PppHeader ppp;
  ppp.SetProtocol(0x0021);
  p->AddHeader(ppp);
  m_dev->RdmaEnqueueHighPrioQ(p);
  m_dev->TriggerTransmit();
}

// ─────────────────────────── 设备接收入口 ───────────────────────────

void ArborHost::Receive(Ptr<Packet> p, CustomHeader& ch) {
  PppHeader ppp;
  Ipv4Header ip;
  ArborUdpHeader ah;
  p->RemoveHeader(ppp);
  p->RemoveHeader(ip);
  p->RemoveHeader(ah);
  NS_ABORT_MSG_IF(!arbor::PortInRange(ah.dport),
                  "Arbor 包 UDP 端口越界: " << ah.dport);
  // 复合 channel 键（group*kMaxRanks + channel），跨 group 同 channel 不碰撞。
  const uint32_t channel = arbor::PortToChannelKey(ah.dport);
  // 以太网最小帧填充口径：payload 长度取自 UDP 长度字段（§9）。
  const uint32_t payloadLen = ah.payloadLen;
  const bool ce = (ch.m_tos & 0x3) == 0x3;

  using arbor::PacketType;
  switch (ah.h.type) {
    case PacketType::RESPONSE:
    case PacketType::REGISTER_ACK:
    case PacketType::END: {
      auto it = m_asRequester.find(channel);
      NS_ABORT_MSG_IF(it == m_asRequester.end(),
                      "下行包到非 requester 端（组播剪枝/路由配置错误）");
      RequesterChannel& rc = it->second;
      // G-Sync δ 偏斜注入点：requester 处理下行包前按 rank 斜坡延迟
      // rank·δ/(N−1)。三类下行同一延迟以保持 per-rank FIFO——只延迟
      // credit 会把 END 重排到尾部交付之前，人为触发 50ms END 重试。
      const uint32_t denom = std::max(1u, rc.cfg.numRequesters - 1);
      uint64_t skew = m_creditSkewNs * rc.cfg.myRequesterIndex / denom;
      const uint32_t sub = arbor::PortToSubchannel(ah.dport);
      const uint64_t now = NowNs();
      const bool delayActive =
          (rc.cfg.subchannelDelayStartNs == 0 ||
           now >= rc.cfg.subchannelDelayStartNs) &&
          (rc.cfg.subchannelDelayEndNs == 0 ||
           now < rc.cfg.subchannelDelayEndNs);
      if (delayActive && sub < rc.cfg.delayedSubchannels) {
        skew += rc.cfg.subchannelDelayNs;
      }
      if (skew == 0) {
        OnDownlinkDelayed(channel, ah.h, ah.dport, payloadLen, ce);
      } else {
        Simulator::Schedule(NanoSeconds(skew), &ArborHost::OnDownlinkDelayed,
                            this, channel, ah.h, ah.dport, payloadLen, ce);
      }
      return;
    }
    case PacketType::DATA_REQUEST: {
      auto it = m_asResponder.find(channel);
      NS_ABORT_MSG_IF(it == m_asResponder.end(),
                      "DATA_REQUEST 到非 responder 端");
      OnRequest(it->second, ah.h, ch.sip, ah.dport, payloadLen, ce);
      return;
    }
    case PacketType::REGISTER: {
      auto it = m_asResponder.find(channel);
      NS_ABORT_MSG_IF(it == m_asResponder.end(), "REGISTER 到非 responder 端");
      OnRegister(it->second, ah.h, ch.sip, ah.dport);
      return;
    }
    case PacketType::END_ACK: {
      auto it = m_asResponder.find(channel);
      if (it == m_asResponder.end()) return;
      OnEndAck(it->second, ah.h, ch.sip);
      return;
    }
    case PacketType::AGG_MISS: {
      auto it = m_asResponder.find(channel);
      NS_ABORT_MSG_IF(it == m_asResponder.end(), "AGG_MISS 到非 responder 端");
      OnAggMiss(it->second, ah.h, ah.dport);
      return;
    }
  }
  NS_ABORT_MSG("未知 Arbor packet_type");
}

void ArborHost::OnDownlinkDelayed(uint32_t channelKey, arbor::WireHeader h,
                                  uint16_t dport, uint32_t payloadLen,
                                  bool ce) {
  auto it = m_asRequester.find(channelKey);
  if (it == m_asRequester.end()) return;
  switch (h.type) {
    case arbor::PacketType::RESPONSE:
      OnResponse(it->second, h, dport, payloadLen, ce);
      return;
    case arbor::PacketType::REGISTER_ACK:
      OnRegisterAck(it->second, h, dport);
      return;
    case arbor::PacketType::END:
      OnEnd(it->second, h, dport);
      return;
    default:
      NS_ABORT_MSG("非下行 packet_type 进入 OnDownlinkDelayed");
  }
}

// ─────────────────────────── requester 状态机 ───────────────────────────

void ArborHost::ScheduleRegister(RequesterChannel& rc) {
  if (!rc.registerBeacon.IsExpired()) return;  // 信标已在跑
  rc.registerBeacon = Simulator::ScheduleNow(&ArborHost::RegisterBeaconTick,
                                             this, rc.key);
}

void ArborHost::RegisterBeaconTick(uint32_t channelKey) {
  auto it = m_asRequester.find(channelKey);
  if (it == m_asRequester.end()) return;
  RequesterChannel& rc = it->second;
  // 队头已确认/已回收项惰性清理；找到一个仍待确认项后发一次并回插。
  ReqMessage* rm = nullptr;
  uint8_t sub = 0;
  const uint32_t scan = static_cast<uint32_t>(rc.registerPending.size());
  for (uint32_t i = 0; i < scan; ++i) {
    const auto pick = rc.registerPending.front();
    rc.registerPending.pop_front();
    auto mit = rc.messages.find(pick.first);
    if (mit == rc.messages.end() || mit->second.complete ||
        ((mit->second.ackedSubs >> pick.second) & 1u) != 0) {
      continue;
    }
    rm = &mit->second;
    sub = pick.second;
    rc.registerPending.push_back(pick);
    break;
  }
  if (rm == nullptr) return;  // 全部确认，信标停止（新 Submit 重启）
  arbor::WireHeader h;
  h.type = arbor::PacketType::REGISTER;
  h.repair = true;  // §2.4：REGISTER 恒置 repair，加速器旁路
  h.messageId = rm->msgId;
  h.offsetA = rm->startSeq;
  SendPacket(rc.cfg.responderIp, rc.cfg.subchannels[sub].udpPort, h, 0);
  // 信标预算：整个 channel 每 max(N−1,2)·P/B 至多一个 REGISTER（§4.2）。
  const double bps = std::max(1.0, m_ccParams.lineRateBps);
  const uint64_t intervalNs = static_cast<uint64_t>(
      std::max<uint32_t>(rc.cfg.numRequesters, 2) * kPayloadSize * 8e9 / bps);
  rc.registerBeacon =
      Simulator::Schedule(NanoSeconds(std::max<uint64_t>(intervalNs, 500)),
                          &ArborHost::RegisterBeaconTick, this, channelKey);
}

void ArborHost::OnRegisterAck(RequesterChannel& rc, const arbor::WireHeader& h,
                              uint16_t dport) {
  auto it = rc.messages.find(h.offsetA);  // epoch 精确匹配
  if (it == rc.messages.end()) return;
  it->second.ackedSubs |= 1u << arbor::PortToSubchannel(dport);
  const uint32_t all = (1u << rc.cfg.subchannels.size()) - 1;
  it->second.registerAcked = (it->second.ackedSubs & all) == all;
}

void ArborHost::OnResponse(RequesterChannel& rc, const arbor::WireHeader& h,
                           uint16_t dport, uint32_t payloadLen, bool ce) {
  const uint32_t sub = arbor::PortToSubchannel(dport);
  NS_ABORT_MSG_IF(sub >= rc.cfg.subchannels.size(), "subchannel 越界");

  // ── payload 交付（去重）──
  if (h.payloadValid) {
    const uint32_t p = h.offsetA;
    ReqMessage* rm = FindReqMessage(rc, p);
    if (rm == nullptr) {
      // 找不到覆盖 offset 的消息（§4.3）：offset 早于本地注册游标 =
      // 已 END_ACK 回收的消息的迟到 payload → 静默丢弃（缺失交付由
      // responder 超时 repair 兜底）；否则 = 未来 offset 的注册门违例，
      // fail-fast 计数，不暂存不降级。
      if (!arbor::SeqBefore(p, rc.nextSeq)) ++m_stats.protocolViolations;
    } else {
      rm->ackedSubs |= 1u << sub;  // 首 credit/response 亦确认注册
      if (!rm->complete) {
        uint8_t& st = rc.offsetState[p];
        if (!(st & kOffsetDelivered)) {
          st |= kOffsetDelivered;
          ++rm->delivered;
          ++m_stats.payloadsDelivered;
          if (rm->delivered == rm->totalPackets) rm->complete = true;
        }
      }
    }
  }

  // ── credit 消费 ──
  if (h.creditValid) {
    const uint32_t k = h.offsetB;
    ReqMessage* rm = FindReqMessage(rc, k);
    if (rm == nullptr) {
      // 已回收消息的迟到 credit（offset 早于注册游标）→ 静默丢弃；
      // 否则未来 offset 注册门违例（§4.3）。
      if (!arbor::SeqBefore(k, rc.nextSeq)) ++m_stats.protocolViolations;
      return;
    }
    rm->ackedSubs |= 1u << sub;
    if (h.repair) {
      // repair credit → 无条件 repair request（位图幂等）；已 complete 的
      // pull 消息丢弃迟到 credit（src_buffer 已归还应用）。
      if (rm->complete && rm->pull) return;
      SendRepairRequest(rc, *rm, k, dport);
    } else {
      // G0：normal credit 恒携带完整位置栈（交换机逐级压栈保证）。
      NS_ABORT_MSG_IF(
          h.aggDepth != rc.cfg.subchannels[sub].stackDepth,
          "G0 违例：normal credit 位置栈不完整 aggDepth="
              << unsigned(h.aggDepth) << " 期望 "
              << unsigned(rc.cfg.subchannels[sub].stackDepth));
      if (rm->complete && rm->pull) return;  // 迟到 normal credit
      uint8_t& st = rc.offsetState[k];
      if (st & kOffsetSent) return;  // normal request 单注入去重
      st |= kOffsetSent;
      SendNormalRequest(rc, *rm, k, h, dport, ce && !h.repair);
    }
  }
}

void ArborHost::SendNormalRequest(RequesterChannel& rc, ReqMessage& rm,
                                  uint32_t offset,
                                  const arbor::WireHeader& credit,
                                  uint16_t dport, bool echoCe) {
  const uint32_t sub = arbor::PortToSubchannel(dport);
  const SubchannelConfig& scfg = rc.cfg.subchannels[sub];
  arbor::WireHeader r;
  r.type = arbor::PacketType::DATA_REQUEST;
  r.repair = false;
  r.op = static_cast<uint8_t>(rm.op) & 0x3;
  r.messageId = rm.msgId;
  r.offsetA = offset;
  r.offsetB = 0;
  r.aggregated = false;
  r.dtype = static_cast<uint8_t>(rm.dtype);
  // 回显 credit 的位置栈 + 按本分支拓扑写 fanin[0..depth-1]。
  r.aggDepth = credit.aggDepth;
  for (uint32_t i = 0; i < credit.aggDepth; ++i) {
    r.aggLoc[i] = credit.aggLoc[i];
    r.fanin[i] = scfg.fanin[i];
  }
  const uint32_t plen =
      rm.pull ? OffsetBytes(rm.startSeq, rm.totalPackets, rm.bytes, offset) : 0;
  SendPacket(rc.cfg.responderIp, dport, r, plen, echoCe);
  ++m_stats.requestsSent;
}

void ArborHost::SendRepairRequest(RequesterChannel& rc, ReqMessage& rm,
                                  uint32_t offset, uint16_t dport) {
  arbor::WireHeader r;
  r.type = arbor::PacketType::DATA_REQUEST;
  r.repair = true;  // 各级无条件旁路
  r.op = static_cast<uint8_t>(rm.op) & 0x3;
  r.messageId = rm.msgId;
  r.offsetA = offset;
  r.offsetB = 0;
  r.aggDepth = 0;  // agg_loc 强制无效（§2.4）
  r.dtype = static_cast<uint8_t>(rm.dtype);
  const uint32_t plen =
      rm.pull ? OffsetBytes(rm.startSeq, rm.totalPackets, rm.bytes, offset) : 0;
  SendPacket(rc.cfg.responderIp, dport, r, plen);
  ++m_stats.requestsSent;
}

void ArborHost::OnEnd(RequesterChannel& rc, const arbor::WireHeader& h,
                      uint16_t dport) {
  auto it = rc.messages.find(h.offsetA);  // epoch 匹配
  if (it == rc.messages.end()) {
    // 已 END_ACK 回收的完成 epoch：tombstone 命中 → 幂等重发 END_ACK
    // （responder END 重试兜底——ACK 丢失后旧 END 重试不与新 message 混淆，
    // §5.4）。用 END 包自带的 messageId/start_sequence 构 ACK，无需存 msgId。
    if (rc.endAckedValid.test(h.messageId) &&
        rc.endAckedSeqs[h.messageId] == h.offsetA) {
      arbor::WireHeader a;
      a.type = arbor::PacketType::END_ACK;
      a.messageId = h.messageId;
      a.offsetA = h.offsetA;
      SendPacket(rc.cfg.responderIp, dport, a, 0);
    }
    return;  // 未知/未来 epoch → 忽略
  }
  ReqMessage& rm = it->second;
  if (!rm.complete) return;  // 未 complete：等 responder 的尾部重放补齐
  arbor::WireHeader a;
  a.type = arbor::PacketType::END_ACK;
  a.messageId = rm.msgId;
  a.offsetA = rm.startSeq;
  SendPacket(rc.cfg.responderIp, dport, a, 0);
  // tombstone + 回收：offsetState 释放 + 按 message_id 保留最近 epoch +
  // 删除重量 ReqMessage。
  const uint32_t startSeq = rm.startSeq;
  const uint32_t totalPackets = rm.totalPackets;
  for (uint32_t i = 0; i < totalPackets; ++i) {
    rc.offsetState.erase(arbor::SeqAdd(startSeq, i));
  }
  rc.endAckedSeqs[rm.msgId] = startSeq;
  rc.endAckedValid.set(rm.msgId);
  rc.messages.erase(it);
}

// ─────────────────────────── responder 状态机 ───────────────────────────

void ArborHost::OnRegister(ResponderChannel& sc, const arbor::WireHeader& h,
                           uint32_t srcIp, uint16_t dport) {
  const uint32_t sub = arbor::PortToSubchannel(dport);
  NS_ABORT_MSG_IF(sub >= sc.cfg.subchannels.size(), "subchannel 越界");
  RespMessage* m = nullptr;
  auto mit = sc.messages.find(h.offsetA);
  if (mit != sc.messages.end()) m = &mit->second;
  if (m == nullptr) return;  // 本地未提交/已释放——信标重试兜底
  const ArborRankBitmap full = FullMask(sc.cfg.numRequesters);
  SetRankBit(m->regBitmap[sub], RankOfIp(sc, srcIp));
  if (m->regBitmap[sub] == full) {
    // 位图收齐 → REGISTER_ACK（重复 REGISTER 重新开放 ACK，幂等）。
    arbor::WireHeader a;
    a.type = arbor::PacketType::REGISTER_ACK;
    a.messageId = m->msgId;
    a.offsetA = m->startSeq;
    SendPacket(sc.cfg.subchannels[sub].mcastIp,
               sc.cfg.subchannels[sub].udpPort, a, 0);
    bool all = true;
    for (auto& b : m->regBitmap) all = all && (b == full);
    m->registered = all;
    IssueCredits(sc);
  }
}

void ArborHost::IssueCreditsTimer(uint32_t channelKey) {
  auto it = m_asResponder.find(channelKey);
  if (it == m_asResponder.end()) return;
  IssueCredits(it->second);
}

void ArborHost::IssueCredits(ResponderChannel& sc) {
  const uint64_t now = NowNs();
  for (auto& cs : sc.cc) {
    arbor::ArborCc::PeriodicUpdate(m_ccMode, cs, m_ccParams);  // Oscar 批式
  }
  const ArborRankBitmap full = FullMask(sc.cfg.numRequesters);
  const uint32_t nsub = static_cast<uint32_t>(sc.cfg.subchannels.size());
  uint64_t earliestPace = UINT64_MAX;

  // 全局护栏在 ns-3 非限制性（§9：map 代替环，outstanding 大上限）。
  while (sc.creditsOutstanding < (1u << 20)) {
    if (sc.cfg.creditRateCapBps > 0.0 &&
        now < sc.nextChannelCreditNs) {
      earliestPace = std::min(earliestPace, sc.nextChannelCreditNs);
      break;
    }
    RespMessage* m = FindRespMessage(sc, sc.creditNextOffset);
    if (m == nullptr) break;  // 全部 message 的 credit 已发放
    // 选 subchannel（§5.1）：注册门 + 窗口 + pacing，轮转。
    int pick = -1;
    const bool fixed = sc.cfg.creditPolicy != CreditPolicy::DYNAMIC;
    const uint32_t candidates = fixed ? 1 : nsub;
    for (uint32_t i = 0; i < candidates; ++i) {
      const uint32_t s =
          sc.cfg.creditPolicy == CreditPolicy::SINGLE
              ? 0
              : (sc.subRR + i) % nsub;
      if (m->regBitmap[s] != full) continue;  // 注册门
      arbor::CcState& cs = sc.cc[s];
      const uint32_t ws =
          cs.windowCredits > 0 ? cs.windowCredits : SubStartupWindow(sc);
      if (cs.outstanding >= ws) continue;  // 窗口门
      if (cs.pacingNs > 0 && now < cs.nextCreditNs) {  // pacing 门
        earliestPace = std::min(earliestPace, cs.nextCreditNs);
        continue;
      }
      pick = static_cast<int>(s);
      break;
    }
    if (pick < 0) break;
    sc.subRR = static_cast<uint32_t>(pick) + 1;
    arbor::CcState& cs = sc.cc[pick];

    const uint32_t k = sc.creditNextOffset;
    NS_ABORT_MSG_IF(sc.entries.count(k) != 0,
                    "24-bit offset 环绕撞上未回收 entry（护栏）");
    CreditEntry& e = sc.entries[k];
    e.subchannel = static_cast<uint32_t>(pick);
    e.issueNs = now;
    sc.creditNextOffset = arbor::SeqAdd(k, 1);
    ++sc.creditsOutstanding;
    ++cs.outstanding;
    cs.peakOutstanding = std::max(cs.peakOutstanding, cs.outstanding);
    ++cs.issued;
    e.inflightAtIssue = cs.outstanding;
    ++m->issuedPackets;
    ++m_stats.creditsIssued;
    cs.nextCreditNs = arbor::ArborCc::AdvancePacing(cs.nextCreditNs, now,
                                                    cs.pacingNs,
                                                    m_ccParams.maxPacingBurst);
    if (sc.cfg.creditRateCapBps > 0.0) {
      const uint64_t channelPacingNs = std::max<uint64_t>(
          1, static_cast<uint64_t>(std::ceil(
                 double(m_ccParams.payloadSize) * 8e9 /
                 sc.cfg.creditRateCapBps)));
      sc.nextChannelCreditNs = arbor::ArborCc::AdvancePacing(
          sc.nextChannelCreditNs, now, channelPacingNs,
          m_ccParams.maxPacingBurst);
    }

    // 载体三选一（§5.1）：data-push（push 类）/ piggyback（pull 稳态）/
    // credit-only（pull 启动/补洞）。
    if (!m->desc.pull) {
      // push（Broadcast/AllGather，§1.1/§5.1 data-push response）：
      // master = 模板头 + 本地数据 payload，self-credit（credit_offset =
      // payload_offset = 自身 offset k），立即 commit（不等 requester）——
      // 组播 payload 下行；消费由上行 header-only 控制聚合 ACK 收齐确认。
      // 自绑定 boundPayload=k：credit 超时 repair 重发本 offset 的 payload。
      e.hasPayload = true;
      e.payloadLen = m->desc.responsePayload
                         ? OffsetBytes(m->startSeq, m->totalPackets,
                                       m->desc.bytes, k)
                         : 0;
      e.committed = true;             // self-commit（提交守恒计一个 master）
      e.boundPayload = k;             // 自绑定：repair 重发 payload+credit
      e.boundBy = k;                  // 尾部 END 重放跳过（credit-repair 兜底）
      ++m_stats.mastersCommitted;
      arbor::WireHeader h;
      h.type = arbor::PacketType::RESPONSE;
      h.op = static_cast<uint8_t>(m->desc.op) & 0x3;  // push 恒 NOP
      h.messageId = m->msgId;
      h.payloadValid = true;
      h.payloadKind = m->desc.responsePayload ? arbor::PayloadKind::DATA
                                              : arbor::PayloadKind::COMPLETION;
      h.offsetA = k;                  // payload_offset = 自身
      h.creditValid = true;
      h.offsetB = k;                  // self-credit
      h.dtype = static_cast<uint8_t>(m->desc.dtype);
      SendPacket(sc.cfg.subchannels[pick].mcastIp,
                 sc.cfg.subchannels[pick].udpPort, h, e.payloadLen);
    } else if (sc.cfg.piggyback && !sc.pendingResponses.empty()) {
      const uint32_t p = sc.pendingResponses.front();
      sc.pendingResponses.pop_front();
      e.boundPayload = p;
      sc.entries.at(p).boundBy = k;
      SendResponseFor(sc, p, true, k);
    } else {
      arbor::WireHeader h;
      h.type = arbor::PacketType::RESPONSE;
      h.creditValid = true;
      h.offsetB = k;
      h.messageId = m->msgId;
      h.dtype = static_cast<uint8_t>(m->desc.dtype);
      h.op = static_cast<uint8_t>(m->desc.op) & 0x3;
      SendPacket(sc.cfg.subchannels[pick].mcastIp,
                 sc.cfg.subchannels[pick].udpPort, h, 0);
    }
    e.timer = Simulator::Schedule(NanoSeconds(m_repairTimeoutNs),
                                  &ArborHost::OnCreditTimeout, this, sc.key, k);
  }

  if (earliestPace != UINT64_MAX && earliestPace > now &&
      sc.issueTimer.IsExpired()) {
    sc.issueTimer =
        Simulator::Schedule(NanoSeconds(earliestPace - now),
                            &ArborHost::IssueCreditsTimer, this, sc.key);
  }
}

void ArborHost::SendResponseFor(ResponderChannel& sc, uint32_t payloadOffset,
                                bool withCredit, uint32_t creditOffset) {
  CreditEntry& ep = sc.entries.at(payloadOffset);
  RespMessage* m = FindRespMessage(sc, payloadOffset);
  NS_ABORT_MSG_IF(m == nullptr, "payload response 无所属 message");
  arbor::WireHeader h;
  h.type = arbor::PacketType::RESPONSE;
  h.repair = false;
  h.op = static_cast<uint8_t>(m->desc.op) & 0x3;
  h.messageId = m->msgId;
  h.payloadValid = true;
  h.payloadKind = m->desc.responsePayload ? arbor::PayloadKind::DATA
                                          : arbor::PayloadKind::COMPLETION;
  h.offsetA = payloadOffset;
  h.creditValid = withCredit;
  h.offsetB = withCredit ? creditOffset : 0;
  h.dtype = static_cast<uint8_t>(m->desc.dtype);
  // piggyback：端口改为 s(k)；纯尾部首发：端口 = s(p)。
  const uint32_t sub =
      withCredit ? sc.entries.at(creditOffset).subchannel : ep.subchannel;
  SendPacket(sc.cfg.subchannels[sub].mcastIp, sc.cfg.subchannels[sub].udpPort,
             h, ep.payloadLen);
}

void ArborHost::FlushPendingResponses(ResponderChannel& sc) {
  while (!sc.pendingResponses.empty()) {
    const uint32_t p = sc.pendingResponses.front();
    sc.pendingResponses.pop_front();
    SendResponseFor(sc, p, false, 0);
  }
}

void ArborHost::OnRequest(ResponderChannel& sc, const arbor::WireHeader& h,
                          uint32_t srcIp, uint16_t dport, uint32_t payloadLen,
                          bool ce) {
  const uint32_t k = h.offsetA;
  RespMessage* m = FindRespMessage(sc, k);
  if (m == nullptr) return;  // 已释放消息的迟到包
  auto eit = sc.entries.find(k);
  NS_ABORT_MSG_IF(eit == sc.entries.end(),
                  "协议违例：offset 未发放 credit 却收到 request");
  CreditEntry& e = eit->second;
  const ArborRankBitmap full = FullMask(sc.cfg.numRequesters);

  if (h.aggregated) {
    ++m_stats.aggregatedRx;
    // 聚合 master：responder 以自身已知组规模校验贡献计数（不信任 fanin）。
    if (h.offsetB != sc.cfg.numRequesters) {
      // §5.2：丢弃，credit 保持未完成等待 repair。
      ++m_stats.protocolViolations;
      return;
    }
    // push 类的上行聚合 master 是 header-only 控制聚合 ACK（消费确认）；
    // 计数供 push 冒烟断言（每 offset 收齐一份控制 master）。
    if (!m->desc.pull) ++m_stats.pushCtrlMasters;
    if (!e.committed) {
      if (e.repairMode) return;  // repair 切换后迟到的 master：丢弃
      e.ceSeen = e.ceSeen || ce;
      e.bitmap = full;  // 聚合凑满 ⇒ 全组已消费该 credit
      CommitEntry(sc, k, e, *m, false, ce);
    } else {
      e.bitmap = full;  // 重复 master：仅消费记账
      CheckDone(sc, *m, k, e);
      if (m->donePackets == m->totalPackets && !m->endPending) {
        StartEndHandshake(sc, *m);
      }
    }
    return;
  }

  // 普通输入。V3 无旁路收齐提交：numRequesters>1 时 normal 单包到达
  // responder 违反协议不变量（fail-fast）；repair request 是恢复路径。
  if (!h.repair) ++m_stats.rawNormalRx;
  NS_ABORT_MSG_IF(!h.repair && sc.cfg.numRequesters > 1 &&
                      sc.cfg.controlAggregation,
                  "协议违例：未聚合 normal request 到达 responder"
                  "（offset=" << k << "）——无旁路/降级路径");
  const uint32_t rank = RankOfIp(sc, srcIp);
  if (RankBitSet(e.bitmap, rank)) return;  // 位图去重（重复 repair request）
  SetRankBit(e.bitmap, rank);
  e.ceSeen = e.ceSeen || ce;
  if (!e.committed) {
    // repair 输入收齐走回退提交；2-rank 的单个 normal 输入由 fanin=1
    // 直转到 responder，仍属于正常提交并产生 CC 样本。
    if (e.bitmap == full) CommitEntry(sc, k, e, *m, h.repair, ce);
  } else {
    CheckDone(sc, *m, k, e);
    if (m->donePackets == m->totalPackets && !m->endPending) {
      StartEndHandshake(sc, *m);
    }
  }
}

void ArborHost::CommitEntry(ResponderChannel& sc, uint32_t offset,
                            CreditEntry& e, RespMessage& m, bool fromRepair,
                            bool ce) {
  NS_ABORT_MSG_IF(e.committed, "重复 commit");
  e.committed = true;
  if (fromRepair) {
    ++m_stats.repairCommits;
  } else {
    ++m_stats.mastersCommitted;
  }
  // commit = 写回本地 dst + 重写 master 头 + piggyback 下一份 credit + 首发。
  // 仿真不建模数值，只保留载荷长度与事件顺序。
  e.hasPayload = true;
  e.payloadLen = m.desc.responsePayload
                     ? OffsetBytes(m.startSeq, m.totalPackets, m.desc.bytes,
                                   offset)
                     : 0;
  sc.pendingResponses.push_back(offset);
  // done 先于首发记账：释放的窗口让 IssueCredits 把下一份 credit
  // piggyback 到本 offset 的 response 上（稳态路径）。
  CheckDone(sc, m, offset, e);
  IssueCredits(sc);
  FlushPendingResponses(sc);  // 没被 piggyback → payload-only 首发
  if (m.donePackets == m.totalPackets && !m.endPending) {
    StartEndHandshake(sc, m);
  }
}

void ArborHost::CheckDone(ResponderChannel& sc, RespMessage& m,
                          uint32_t offset, CreditEntry& e) {
  const ArborRankBitmap full = FullMask(sc.cfg.numRequesters);
  if (e.done || !e.committed || e.bitmap != full) return;
  e.done = true;
  e.timer.Cancel();
  NS_ABORT_MSG_IF(sc.creditsOutstanding == 0, "creditsOutstanding 记账为负");
  --sc.creditsOutstanding;
  arbor::CcState& cs = sc.cc[e.subchannel];
  if (cs.outstanding > 0) --cs.outstanding;
  // CC 样本 = normal credit 首次位图收齐；repair/AGG_MISS 不取样。
  if (!e.repairMode && m_ccMode != arbor::CcMode::DISABLED) {
    arbor::ArborCc::OnSample(m_ccMode, cs, NowNs(), e.issueNs,
                             e.inflightAtIssue, e.ceSeen, m_ccParams);
    ++m_stats.ccSamples;
  }
  ++m.donePackets;
  IssueCredits(sc);  // 释放的预算立即可用
}

// ─────────────────────────── repair 路径 ───────────────────────────

void ArborHost::OnCreditTimeout(uint32_t channelKey, uint32_t offset) {
  auto it = m_asResponder.find(channelKey);
  if (it == m_asResponder.end()) return;
  ResponderChannel& sc = it->second;
  auto eit = sc.entries.find(offset);
  if (eit == sc.entries.end()) return;
  CreditEntry& e = eit->second;
  if (e.done) return;
  EnterRepair(sc, offset, e);
}

void ArborHost::OnAggMiss(ResponderChannel& sc, const arbor::WireHeader& h,
                          uint16_t /*dport*/) {
  ++m_stats.aggMissRx;
  const uint32_t k = h.offsetA;  // request_offset
  auto eit = sc.entries.find(k);
  if (eit == sc.entries.end()) return;  // 不存在 → 忽略
  CreditEntry& e = eit->second;
  if (e.committed || e.done) return;  // 已提交 → 忽略
  if (e.repairMode) return;           // 已在 repair → 幂等忽略
  EnterRepair(sc, k, e);  // 原子切 repair mode + 组播 repair credit
}

void ArborHost::EnterRepair(ResponderChannel& sc, uint32_t offset,
                            CreditEntry& e) {
  if (!e.repairMode) {
    e.repairMode = true;  // 此后迟到聚合 master 丢弃
    ++m_stats.repairTriggers;
  }
  // 独立预算 token bucket，不参与正常速率闭环。
  const uint64_t now = NowNs();
  RespMessage* repairMsg = FindRespMessage(sc, offset);
  NS_ABORT_MSG_IF(repairMsg == nullptr, "repair credit 无所属 message");
  uint32_t replayLen = 0;
  if (e.boundPayload != kInvalidOffset) {
    // L2c 修补（编译/健壮性级，记录）：piggyback 可跨 message 边界绑定
    // （credit k 属下一条 message、payload p 属上一条）。若 p 所属
    // message 已完成释放（END_ACK 收齐 ⇒ 全员已交付 p），重放不再必要
    // ——解除绑定退化为 header-only repair credit；丢包注入首次触达该
    // 路径，原 .at() 会对已释放 entry 越界。语义不变。
    auto pit = sc.entries.find(e.boundPayload);
    if (pit == sc.entries.end()) {
      e.boundPayload = kInvalidOffset;
    } else {
      replayLen = pit->second.payloadLen;
    }
  }
  // token charge 按该 trigger 诱发的 responder 侧总流量计。pull repair 会
  // 让每个 requester 回一份 payload；若只按 64B credit 计费，相关故障会
  // 把 N 倍回退流量瞬间放开，重复 trigger 再放大成 repair storm。
  const uint32_t requestLen =
      repairMsg->desc.pull
          ? OffsetBytes(repairMsg->startSeq, repairMsg->totalPackets,
                        repairMsg->desc.bytes, offset)
          : 0;
  const double size =
      double(replayLen + kWireOverhead) +
      double(sc.cfg.numRequesters) * double(requestLen + kWireOverhead);
  // burst 至少容纳一次最大 fan-in repair，否则 128-rank 的单次 charge
  // 可能大于固定 1MB 上限而永远拿不到 token。
  const double burstCap = std::max(kRepairBurstBytes, size);
  sc.repairTokens =
      std::min(burstCap,
               sc.repairTokens + double(now - sc.repairRefillNs) *
                                     double(m_repairBudgetBps) / 8e9);
  sc.repairRefillNs = now;
  e.timer.Cancel();
  if (sc.repairTokens < size) {
    // 预算不足：到 refill 时刻重试（不计入 retries）。
    const uint64_t waitNs = static_cast<uint64_t>(
        (size - sc.repairTokens) * 8e9 / double(m_repairBudgetBps)) + 1;
    e.timer = Simulator::Schedule(NanoSeconds(waitNs),
                                  &ArborHost::OnCreditTimeout, this, sc.key,
                                  offset);
    return;
  }
  sc.repairTokens -= size;
  ++e.retries;
  NS_ABORT_MSG_IF(e.retries > kMaxRepairRetries,
                  "-ETIMEDOUT：repair 重试超限 channelKey=" << sc.key
                      << " offset=" << offset << " sub=" << e.subchannel
                      << " bitmap=[0x" << std::hex << e.bitmap[0] << ",0x"
                      << e.bitmap[1] << std::dec << "]");
  SendRepairCredit(sc, offset, e);
  e.timer = Simulator::Schedule(NanoSeconds(m_repairTimeoutNs),
                                &ArborHost::OnCreditTimeout, this, sc.key,
                                offset);
}

void ArborHost::SendRepairCredit(ResponderChannel& sc, uint32_t offset,
                                 CreditEntry& e) {
  RespMessage* m = FindRespMessage(sc, offset);
  NS_ABORT_MSG_IF(m == nullptr, "repair credit 无所属 message");
  arbor::WireHeader h;
  h.type = arbor::PacketType::RESPONSE;
  h.repair = true;  // 加速器不分配 slot、不压栈
  h.creditValid = true;
  h.offsetB = offset;
  h.messageId = m->msgId;
  h.dtype = static_cast<uint8_t>(m->desc.dtype);
  h.op = static_cast<uint8_t>(m->desc.op) & 0x3;
  uint32_t plen = 0;
  if (e.boundPayload != kInvalidOffset) {
    // 绑定重放语义：同一包重放 payload p 与 repair credit k。
    // （防御同 EnterRepair 的跨 message 释放修补；语义不变。）
    auto pit = sc.entries.find(e.boundPayload);
    if (pit != sc.entries.end()) {
      h.payloadValid = true;
      h.payloadKind = arbor::PayloadKind::REPLAY;
      h.offsetA = e.boundPayload;
      plen = pit->second.payloadLen;
    } else {
      e.boundPayload = kInvalidOffset;
    }
  }
  const uint32_t sub = e.subchannel;
  SendPacket(sc.cfg.subchannels[sub].mcastIp, sc.cfg.subchannels[sub].udpPort,
             h, plen);
}

// ─────────────────────────── END 握手 ───────────────────────────

void ArborHost::StartEndHandshake(ResponderChannel& sc, RespMessage& m) {
  m.endPending = true;
  for (uint32_t s = 0; s < sc.cfg.subchannels.size(); ++s) {
    arbor::WireHeader h;
    h.type = arbor::PacketType::END;
    h.messageId = m.msgId;
    h.offsetA = m.startSeq;
    SendPacket(sc.cfg.subchannels[s].mcastIp, sc.cfg.subchannels[s].udpPort, h,
               0);
  }
  m.endTimer =
      Simulator::Schedule(NanoSeconds(m_repairTimeoutNs),
                          &ArborHost::OnEndTimeout, this, sc.key, m.startSeq);
}

void ArborHost::OnEndTimeout(uint32_t channelKey, uint32_t startSeq) {
  auto it = m_asResponder.find(channelKey);
  if (it == m_asResponder.end()) return;
  ResponderChannel& sc = it->second;
  RespMessage* m = nullptr;
  auto mit = sc.messages.find(startSeq);
  if (mit != sc.messages.end()) m = &mit->second;
  if (m == nullptr || !m->endPending) return;
  ++m->endRetries;
  NS_ABORT_MSG_IF(m->endRetries > kMaxRepairRetries,
                  "-ETIMEDOUT：END 重试超限 startSeq=" << startSeq);
  // 重放未被任何 credit 反向绑定的尾部 payload response（REPLAY，无
  // credit），受 repair 预算限制；然后重发 END。
  const uint64_t now = NowNs();
  sc.repairTokens =
      std::min(kRepairBurstBytes,
               sc.repairTokens + double(now - sc.repairRefillNs) *
                                     double(m_repairBudgetBps) / 8e9);
  sc.repairRefillNs = now;
  for (uint32_t i = 0; i < m->totalPackets; ++i) {
    const uint32_t p = arbor::SeqAdd(m->startSeq, i);
    auto eit = sc.entries.find(p);
    if (eit == sc.entries.end()) continue;
    CreditEntry& e = eit->second;
    if (!e.committed || !e.hasPayload || e.boundBy != kInvalidOffset) continue;
    const double size = double(e.payloadLen + kWireOverhead);
    if (sc.repairTokens < size) break;  // 预算用尽，下轮超时续传
    sc.repairTokens -= size;
    arbor::WireHeader h;
    h.type = arbor::PacketType::RESPONSE;
    h.repair = true;
    h.payloadValid = true;
    h.payloadKind = arbor::PayloadKind::REPLAY;
    h.offsetA = p;
    h.messageId = m->msgId;
    h.dtype = static_cast<uint8_t>(m->desc.dtype);
    const uint32_t sub = e.subchannel;
    SendPacket(sc.cfg.subchannels[sub].mcastIp,
               sc.cfg.subchannels[sub].udpPort, h, e.payloadLen);
  }
  for (uint32_t s = 0; s < sc.cfg.subchannels.size(); ++s) {
    arbor::WireHeader h;
    h.type = arbor::PacketType::END;
    h.messageId = m->msgId;
    h.offsetA = m->startSeq;
    SendPacket(sc.cfg.subchannels[s].mcastIp, sc.cfg.subchannels[s].udpPort, h,
               0);
  }
  m->endTimer =
      Simulator::Schedule(NanoSeconds(m_repairTimeoutNs),
                          &ArborHost::OnEndTimeout, this, channelKey, startSeq);
}

void ArborHost::OnEndAck(ResponderChannel& sc, const arbor::WireHeader& h,
                         uint32_t srcIp) {
  RespMessage* m = nullptr;
  auto mit = sc.messages.find(h.offsetA);
  if (mit != sc.messages.end()) m = &mit->second;
  if (m == nullptr) return;  // 已完成释放的重复 END_ACK
  SetRankBit(m->endAckBitmap, RankOfIp(sc, srcIp));
  if (m->endAckBitmap == FullMask(sc.cfg.numRequesters)) {
    m->endTimer.Cancel();
    m->endPending = false;
    MaybeFinishMessage(sc, *m);
  }
}

void ArborHost::MaybeFinishMessage(ResponderChannel& sc, RespMessage& m) {
  if (m.donePackets != m.totalPackets ||
      m.endAckBitmap != FullMask(sc.cfg.numRequesters)) {
    return;
  }
  m_stats.messageCctNs.push_back(NowNs() - m.submitNs);
  if (!m.desc.onComplete.IsNull()) m.desc.onComplete();
  for (uint32_t i = 0; i < m.totalPackets; ++i) {
    sc.entries.erase(arbor::SeqAdd(m.startSeq, i));
  }
  sc.messages.erase(m.startSeq);
}

}  // namespace ns3
