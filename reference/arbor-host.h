/**
 * arbor-host.h — Arbor 端侧协议状态机（requester + responder）。
 *
 * implementation.md §4/§5 的协议级移植（V3）：状态用关联容器而非固定环
 * （§9 平台无关契约），无 mbuf/NUMA 细节；一个 ArborHost 挂在一个 host 节点
 * 上，同时承担该节点作为 responder 的全部 channel 与作为 requester 的
 * 全部 channel。
 *
 * 关键语义（与冻结规范一致）：
 *  - response 即 credit：credit 组播下发；requester 只凭 credit 发送。
 *  - V3 位置栈：requester 原样回显 credit 的栈与 agg_depth，并按本分支
 *    拓扑写 fanin[0..depth-1]；normal request 单注入。
 *  - responder 提交：aggregated master 且贡献计数 == numRequesters；路径上
 *    本级 fan-in==1 由交换机弹栈直转。
 *  - AGG_MISS / credit 超时 → 原子切 repair mode → 组播 repair credit
 *    （独立预算）；repair request 各级旁路，responder 位图收齐回退提交。
 *  - CC：每 (channel, subchannel) 一个 CcState；样本 = normal credit
 *    首次位图收齐；repair/重放/AGG_MISS 不取样。
 *  - END/END_ACK 收尾消息并回收 message 状态。
 *  - δ 偏斜注入（G-Sync）：全部下行控制/数据在 requester 处理前增加
 *    δ_i=iδ/(N-1) 的 per-rank 延迟；δ 是组内最大附加时差。
 */
#ifndef ARBOR_HOST_H
#define ARBOR_HOST_H

#include <array>
#include <bitset>
#include <deque>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ns3/callback.h"
#include "ns3/event-id.h"
#include "ns3/object.h"
#include "ns3/ptr.h"

#include "arbor-cc.h"
#include "arbor-wire.h"

namespace ns3 {

class Node;
class Packet;
class QbbNetDevice;
class CustomHeader;

using ArborRankBitmap =
    std::array<uint64_t, (arbor::kMaxRanksPerGroup + 63) / 64>;

class ArborHost : public Object {
 public:
  static TypeId GetTypeId(void);

  enum class CreditPolicy : uint8_t { DYNAMIC = 0, STATIC_RR, SINGLE };

  struct SubchannelConfig {
    uint16_t udpPort = 0;    ///< (group, channel, subchannel) 端口算术值
    uint32_t mcastIp = 0;    ///< 该树的 response/END 组播 IP（主机序）
    uint8_t stackDepth = 1;  ///< 本 rank 分支的聚合级数（1..3）
    uint8_t fanin[arbor::kMaxAggLevels] = {0, 0, 0};
    ///< 本 rank 分支各栈位的凑满数（fanin[k] 对应 agg_loc[k]；requester 用）
  };

  struct ChannelConfig {
    uint32_t groupId = 0;
    uint32_t channelId = 0;        ///< = responder rank
    bool isResponder = false;
    uint32_t numRequesters = 0;    ///< 网络侧 fan-in（不含 responder 本地）
    uint32_t myRequesterIndex = 0; ///< requester 位图位（requester 侧有效）
    uint32_t responderIp = 0;      ///< 主机序
    /// requester 位图位 → 源 IP（responder 侧记账用；实现期新增，记录）
    std::vector<uint32_t> requesterIps;
    std::vector<SubchannelConfig> subchannels;
    CreditPolicy creditPolicy = CreditPolicy::DYNAMIC;
    bool piggyback = true;
    bool controlAggregation = true;
    double creditRateCapBps = 0;  ///< 全 subchannel 合计；0=不限制
    uint32_t delayedSubchannels = 0;
    uint64_t subchannelDelayNs = 0;
    uint64_t subchannelDelayStartNs = 0;
    uint64_t subchannelDelayEndNs = 0;
  };

  /// 一条 message（原语在本 channel 上的连续数据段）。
  struct MessageDesc {
    uint32_t groupId = 0;   ///< (group, channel) 复合寻址；缺省 0（单组）
    uint32_t channelId = 0;
    uint64_t bytes = 0;
    arbor::Op op = arbor::Op::SUM;
    arbor::Datatype dtype = arbor::Datatype::F32;
    bool pull = true;   ///< pull：request 带 payload；push：response 带 payload
    bool responsePayload = true;  ///< AllReduce/push=true；Reduce/RS=false
    Callback<void> onComplete;    ///< responder 侧 message 完成（全部 done+END）
  };

  // ── 装配（helper 调用） ──
  void Setup(Ptr<Node> node, Ptr<QbbNetDevice> dev, uint32_t myIp,
             arbor::CcMode mode, const arbor::CcParams& params,
             uint32_t startupWindow, uint64_t repairTimeoutNs,
             uint64_t repairBudgetBps);
  void AddChannel(const ChannelConfig& cfg);
  /// 提交 message（responder 侧调用；requester 侧由 REGISTER 流程发现）。
  /// 返回该 message 的 start_sequence。dtype 不支持在网归约时返回 -1
  /// （API 层拒绝——协议无绕行模式）。
  int64_t Submit(const MessageDesc& desc);

  /// G-Sync δ 偏斜：requester 处理全部下行包前延迟 rank·δ/(N−1)。
  void SetCreditSkew(uint64_t deltaNs) { m_creditSkewNs = deltaNs; }

  /// 设备接收回调（qbb 侧对 l3Prot==ARBOR_PROTO 调用）。
  void Receive(Ptr<Packet> p, CustomHeader& ch);

  // ── 统计 ──
  struct HostStats {
    uint64_t creditsIssued = 0;
    uint64_t requestsSent = 0;
    uint64_t mastersCommitted = 0;
    uint64_t repairTriggers = 0;
    uint64_t aggMissRx = 0;
    uint64_t repairCommits = 0;
    uint64_t ccSamples = 0;
    uint64_t protocolViolations = 0;  ///< 注册门违例等 fail-fast 丢弃计数
    uint64_t payloadsDelivered = 0;   ///< requester 侧去重后的交付数
    uint64_t pushCtrlMasters = 0;     ///< push 类：收齐的上行控制聚合 ACK 数
    uint64_t aggregatedRx = 0;        ///< responder 收到的聚合 master 数
    uint64_t rawNormalRx = 0;         ///< responder 收到的未聚合 normal 输入数
    std::vector<uint64_t> messageCctNs;  ///< 每条 message 的完成时延
  };
  const HostStats& GetStats() const { return m_stats; }

  /// 编译级修补（L2c 记录）：只读 CC 状态快照导出，供 arbor-stats 周期
  /// 采样 r_s/W_s/rtt——纯观测接口，不触碰任何协议状态或行为。
  /// responder 侧每 (channel, subchannel) 追加一行。
  struct CcSnapshotRow {
    uint32_t channelId = 0;
    uint32_t subchannel = 0;
    uint64_t pacingNs = 0;      ///< 相邻 credit 最小间隔（r_s = P·8e9/pacing）
    uint32_t windowCredits = 0; ///< W_s（0 = 未收敛，用启动窗）
    uint32_t outstanding = 0;
    uint32_t peakOutstanding = 0;
    uint64_t rttBaseNs = 0;
    uint64_t lastRttNs = 0;
    uint64_t maxRttNs = 0;
    uint64_t samples = 0;
    uint64_t issued = 0;
  };
  void CollectCcSnapshots(std::vector<CcSnapshotRow>& out) const {
    for (const auto& kv : m_asResponder) {
      // 报告逻辑 channelId（cfg），而非复合内部键——离线 CSV 口径不变。
      const uint32_t channelId = kv.second.cfg.channelId;
      for (uint32_t s = 0; s < kv.second.cc.size(); ++s) {
        const arbor::CcState& cs = kv.second.cc[s];
        out.push_back(CcSnapshotRow{channelId, s, cs.pacingNs, cs.windowCredits,
                                    cs.outstanding, cs.peakOutstanding,
                                    cs.rttBaseNs, cs.lastRttNs, cs.maxRttNs,
                                    cs.samples, cs.issued});
      }
    }
  }

 private:
  static constexpr uint32_t kInvalidOffset = 0xFFFFFFFFu;

  // ── responder 侧状态 ──
  struct CreditEntry {
    uint32_t subchannel = 0;
    uint64_t issueNs = 0;
    uint32_t inflightAtIssue = 0;
    ArborRankBitmap bitmap{};  ///< requester 消费/贡献位图
    bool committed = false;
    bool done = false;
    bool repairMode = false;
    bool ceSeen = false;
    uint32_t retries = 0;
    bool hasPayload = false;   ///< response 携带 payload（piggyback/重放用）
    uint32_t payloadLen = 0;
    uint32_t boundPayload = kInvalidOffset;  ///< 本 credit piggyback 的 payload
    uint32_t boundBy = kInvalidOffset;       ///< 本 payload 被哪个 credit 绑定
    EventId timer;
  };
  struct RespMessage {
    uint32_t startSeq = 0;
    uint32_t totalPackets = 0;
    uint32_t issuedPackets = 0;
    uint32_t donePackets = 0;
    uint8_t msgId = 0;         ///< 8-bit channel 内编号（两侧游标同步分配）
    MessageDesc desc;
    uint64_t submitNs = 0;
    // 注册：每 subchannel 的 requester 位图
    std::vector<ArborRankBitmap> regBitmap;
    bool registered = false;
    bool endPending = false;
    ArborRankBitmap endAckBitmap{};
    uint32_t endRetries = 0;
    EventId endTimer;
  };
  struct ResponderChannel {
    ChannelConfig cfg;
    uint32_t key = 0;              ///< (group, channel) 复合内部键（调度句柄）
    uint32_t nextSeq = 0;          ///< 24-bit 序号分配游标
    uint32_t nextMsgId = 0;
    uint32_t creditNextOffset = 0;
    uint32_t creditsOutstanding = 0;
    uint32_t subRR = 0;            ///< subchannel 轮转游标
    uint64_t nextChannelCreditNs = 0;  ///< channel 总速率门
    std::deque<uint32_t> pendingResponses;  ///< 已 commit 待首发（同事件内排干）
    std::map<uint32_t, RespMessage> messages;  ///< startSeq → message
    std::unordered_map<uint32_t, CreditEntry> entries;  ///< offset → entry
    std::unordered_map<uint32_t, uint32_t> requesterRank;  ///< src IP → bitmap 位
    std::vector<arbor::CcState> cc;           ///< per subchannel
    EventId issueTimer;                       ///< pacing 到点重试
    // repair 独立预算 token bucket（5 Gbps；突发至少容纳一个 repair 单元）
    double repairTokens = 1024.0 * 1024.0;
    uint64_t repairRefillNs = 0;
  };

  // ── requester 侧状态 ──
  struct ReqMessage {
    uint32_t startSeq = 0;
    uint32_t totalPackets = 0;
    uint32_t delivered = 0;
    uint8_t msgId = 0;
    uint64_t bytes = 0;
    bool pull = true;
    arbor::Op op = arbor::Op::SUM;
    arbor::Datatype dtype = arbor::Datatype::F32;
    uint32_t ackedSubs = 0;     ///< REGISTER 确认掩码（ACK 或首 credit）
    bool registerAcked = false;
    bool complete = false;
    // tombstone（已回 END_ACK）改由 RequesterChannel::endAckedSeqs 记录：
    // ReqMessage 在 END_ACK 后回收，轻量 set 保留 startSeq。
  };
  struct RequesterChannel {
    ChannelConfig cfg;
    uint32_t key = 0;           ///< (group, channel) 复合内部键（调度句柄）
    uint32_t nextSeq = 0;       ///< 与 responder 同步推进的本地序号游标
    uint32_t nextMsgId = 0;
    std::map<uint32_t, ReqMessage> messages;      ///< startSeq → message
    std::unordered_map<uint32_t, uint8_t> offsetState; ///< offset → 状态
    /// 待确认 (message,subchannel) 队列。每 tick 只弹/回插一个活跃项，
    /// 避免按信标频率扫描全部异步 message。
    std::deque<std::pair<uint32_t, uint8_t>> registerPending;
    /// 每个 8-bit message_id 只保留最近已回 END_ACK 的 epoch；有界 tombstone
    /// 足以处理 END 重放，并与 testbed 的 256-entry 复用状态一致。
    std::array<uint32_t, 256> endAckedSeqs{};
    std::bitset<256> endAckedValid;
    EventId registerBeacon;
  };
  static constexpr uint8_t kOffsetSent = 0x1;
  static constexpr uint8_t kOffsetDelivered = 0x2;

  // ── 内部流程（实现见 arbor-host.cc） ──
  void OnResponse(RequesterChannel& rc, const arbor::WireHeader& h,
                  uint16_t dport, uint32_t payloadLen, bool ce);
  /// δ 偏斜后的下行统一入口（RESPONSE/REGISTER_ACK/END 同一斜坡延迟，
  /// 保持 per-rank FIFO——只延迟 credit 会人为重排 END 与尾部交付）。
  void OnDownlinkDelayed(uint32_t channelKey, arbor::WireHeader h,
                         uint16_t dport, uint32_t payloadLen, bool ce);
  void OnRequest(ResponderChannel& sc, const arbor::WireHeader& h,
                 uint32_t srcIp, uint16_t dport, uint32_t payloadLen, bool ce);
  void OnAggMiss(ResponderChannel& sc, const arbor::WireHeader& h,
                 uint16_t dport);
  void OnRegister(ResponderChannel& sc, const arbor::WireHeader& h,
                  uint32_t srcIp, uint16_t dport);
  void OnEndAck(ResponderChannel& sc, const arbor::WireHeader& h,
                uint32_t srcIp);
  void OnRegisterAck(RequesterChannel& rc, const arbor::WireHeader& h,
                     uint16_t dport);
  void OnEnd(RequesterChannel& rc, const arbor::WireHeader& h, uint16_t dport);
  void IssueCredits(ResponderChannel& sc);
  void IssueCreditsTimer(uint32_t channelKey);
  void CommitEntry(ResponderChannel& sc, uint32_t offset, CreditEntry& e,
                   RespMessage& m, bool fromRepair, bool ce);
  void CheckDone(ResponderChannel& sc, RespMessage& m, uint32_t offset,
                 CreditEntry& e);
  void FlushPendingResponses(ResponderChannel& sc);
  void SendResponseFor(ResponderChannel& sc, uint32_t payloadOffset,
                       bool withCredit, uint32_t creditOffset);
  void EnterRepair(ResponderChannel& sc, uint32_t offset, CreditEntry& e);
  void SendRepairCredit(ResponderChannel& sc, uint32_t offset, CreditEntry& e);
  void StartEndHandshake(ResponderChannel& sc, RespMessage& m);
  void OnEndTimeout(uint32_t channelKey, uint32_t startSeq);
  void OnCreditTimeout(uint32_t channelKey, uint32_t offset);
  void SendPacket(uint32_t dip, uint16_t dport, const arbor::WireHeader& h,
                  uint32_t payloadLen, bool markCe = false);
  void ScheduleRegister(RequesterChannel& rc);
  void RegisterBeaconTick(uint32_t channelKey);
  void MaybeFinishMessage(ResponderChannel& sc, RespMessage& m);
  void SendNormalRequest(RequesterChannel& rc, ReqMessage& rm, uint32_t offset,
                         const arbor::WireHeader& credit, uint16_t dport,
                         bool echoCe);
  void SendRepairRequest(RequesterChannel& rc, ReqMessage& rm, uint32_t offset,
                         uint16_t dport);
  RespMessage* FindRespMessage(ResponderChannel& sc, uint32_t offset);
  ReqMessage* FindReqMessage(RequesterChannel& rc, uint32_t offset);
  uint32_t RankOfIp(const ResponderChannel& sc, uint32_t ip) const;
  uint32_t OffsetBytes(uint32_t startSeq, uint32_t totalPackets, uint64_t bytes,
                       uint32_t offset) const;
  uint32_t SubStartupWindow(const ResponderChannel& sc) const;

  Ptr<Node> m_node;
  Ptr<QbbNetDevice> m_dev;
  uint32_t m_myIp = 0;
  arbor::CcMode m_ccMode = arbor::CcMode::DISABLED;
  arbor::CcParams m_ccParams;
  uint32_t m_startupWindow = 16;
  uint64_t m_repairTimeoutNs = 50ull * 1000 * 1000;
  uint64_t m_repairBudgetBps = 5ull * 1000 * 1000 * 1000;
  uint64_t m_creditSkewNs = 0;

  std::map<uint32_t, ResponderChannel> m_asResponder;  ///< (group,channel) 键 →
  std::map<uint32_t, RequesterChannel> m_asRequester;
  /// Submit 遗留解析：逻辑 channelId → 复合内部键（供只带 channelId、不带
  /// groupId 的调用方；harness 全局唯一 channelId 时无歧义，跨 group 复用
  /// 同 id 时调用方须传 groupId 走复合直解）。
  std::map<uint32_t, uint32_t> m_channelIdToKey;
  HostStats m_stats;
};

}  // namespace ns3

#endif  // ARBOR_HOST_H
