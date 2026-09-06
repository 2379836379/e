/**
 * arbor-wire.h — Arbor V3 线格式编解码（ns-3 侧独立移植）。
 *
 * 与 testbed 的 host/include/arbor_header.h 逐字节一致（implementation.md
 * §2.5 为冻结规范）；本文件不依赖 ns-3 类型，切勿引入 DPDK/主机头。
 *
 * V3 头 20 字节，大端：
 *   byte 0     : version[7:6]=2 | packet_type[5:3] | repair[2] | op[1:0]
 *   byte 1     : message_id
 *   byte 2     : payload_valid[7] | payload_kind[6:5] | credit_valid[4]
 *                | rsvd[3] | agg_depth[2:1] | aggregated[0]
 *   byte 3..5  : offset_a   byte 6..8: offset_b
 *   byte 9..10 : agg_loc[0]（位置栈栈底 = 根级）
 *   byte 11..13: 上行 fanin[0..2]（各 8b，与 agg_loc 对齐）；下行恒 0
 *   byte 14    : 保留 0     byte 15: dtype
 *   byte 16..17: agg_loc[1] byte 18..19: agg_loc[2]
 */
#ifndef ARBOR_WIRE_H
#define ARBOR_WIRE_H

#include <cstdint>
#include <cstring>

namespace ns3 {
namespace arbor {

static constexpr uint8_t kWireVersion = 2;
static constexpr uint32_t kArborHdrLen = 20;
static constexpr uint32_t kMaxAggLevels = 3;
static constexpr uint16_t kAggIndexUnused = 0xFFFF;

// 端口算术（与 testbed channel.h 一致）：
// udp_port = 10000 + (group*128 + channel)*4 + subchannel
//
// ns-3 将组内 channel 空间扩为 128；testbed 单级 profile 只使用 group 0，
// 因而其现有端口不受 group stride 变化影响。ns-3 的多 job 用 group 位
// 区分状态。
// 端口→(group, channel, sub) 的解码位分配容量校核：
//   - G5.2 的 32 个 job 各占一个 group，可在组内复用 channel id；
//   - 128-rank collective 需要组内 128 个 responder channel。
// 64 组 × 128 channel = 8192 个 (group, channel) 槽，最大端口 42767，
// 仍落在 uint16_t。requester 位图是端侧状态，不进入 wire header。
static constexpr uint16_t kChannelPortBase = 10000;
static constexpr uint32_t kMaxRanksPerGroup = 128;
static constexpr uint32_t kMaxSubchannels = 4;
static constexpr uint32_t kMaxGroups = 64;
static constexpr uint32_t kChannelPortLimit =
    kChannelPortBase + kMaxGroups * kMaxRanksPerGroup * kMaxSubchannels;

enum class PacketType : uint8_t {
  REGISTER = 0,
  DATA_REQUEST = 1,
  RESPONSE = 2,
  END = 3,
  END_ACK = 4,
  REGISTER_ACK = 5,
  AGG_MISS = 6,
};

enum class Op : uint8_t { NOP = 0, SUM = 1, MAX = 2, MIN = 3 };

enum class PayloadKind : uint8_t { DATA = 0, COMPLETION = 1, REPLAY = 2 };

enum class Datatype : uint8_t {
  F16 = 0, BF16 = 1, F32 = 2, F64 = 3, I32 = 4, I64 = 5,
};

inline bool DatatypeInaSupported(Datatype dt) { return dt == Datatype::F32; }

/// 解码视图。字段语义随方向变化，由调用方按 packet_type 解释。
struct WireHeader {
  PacketType type = PacketType::DATA_REQUEST;
  bool repair = false;
  uint8_t op = 0;
  uint8_t messageId = 0;
  bool payloadValid = false;
  PayloadKind payloadKind = PayloadKind::DATA;
  bool creditValid = false;
  uint8_t aggDepth = 0;
  bool aggregated = false;
  uint32_t offsetA = 0;   ///< payload/request offset（24b）
  uint32_t offsetB = 0;   ///< credit offset / 聚合贡献计数（24b）
  uint16_t aggLoc[kMaxAggLevels] = {0, 0, 0};
  uint8_t fanin[kMaxAggLevels] = {0, 0, 0};
  uint8_t dtype = 0;

  // ── 序列化 ──
  void Serialize(uint8_t* w) const {
    w[0] = static_cast<uint8_t>((kWireVersion & 0x3u) << 6 |
                                (static_cast<uint8_t>(type) & 0x7u) << 3 |
                                (repair ? 1u : 0u) << 2 | (op & 0x3u));
    w[1] = messageId;
    w[2] = static_cast<uint8_t>((payloadValid ? 1u : 0u) << 7 |
                                (static_cast<uint8_t>(payloadKind) & 0x3u) << 5 |
                                (creditValid ? 1u : 0u) << 4 |
                                (aggDepth & 0x3u) << 1 | (aggregated ? 1u : 0u));
    Store24(w + 3, offsetA);
    Store24(w + 6, offsetB);
    Store16(w + 9, aggLoc[0]);
    w[11] = fanin[0];
    w[12] = fanin[1];
    w[13] = fanin[2];
    w[14] = 0;
    w[15] = dtype;
    Store16(w + 16, aggLoc[1]);
    Store16(w + 18, aggLoc[2]);
  }

  static WireHeader Deserialize(const uint8_t* w) {
    WireHeader h;
    h.type = static_cast<PacketType>((w[0] >> 3) & 0x7u);
    h.repair = ((w[0] >> 2) & 0x1u) != 0;
    h.op = w[0] & 0x3u;
    h.messageId = w[1];
    h.payloadValid = ((w[2] >> 7) & 0x1u) != 0;
    h.payloadKind = static_cast<PayloadKind>((w[2] >> 5) & 0x3u);
    h.creditValid = ((w[2] >> 4) & 0x1u) != 0;
    h.aggDepth = (w[2] >> 1) & 0x3u;
    h.aggregated = (w[2] & 0x1u) != 0;
    h.offsetA = Load24(w + 3);
    h.offsetB = Load24(w + 6);
    h.aggLoc[0] = Load16(w + 9);
    h.fanin[0] = w[11];
    h.fanin[1] = w[12];
    h.fanin[2] = w[13];
    h.dtype = w[15];
    h.aggLoc[1] = Load16(w + 16);
    h.aggLoc[2] = Load16(w + 18);
    return h;
  }

  static uint8_t Version(const uint8_t* w) { return (w[0] >> 6) & 0x3u; }

  // ── 位置栈操作（压栈/弹栈/栈顶） ──
  bool PushAggLoc(uint16_t slot) {
    if (aggDepth >= kMaxAggLevels) return false;
    aggLoc[aggDepth++] = slot;
    return true;
  }
  uint16_t TopAggLoc() const { return aggLoc[aggDepth - 1]; }
  uint8_t TopFanin() const { return fanin[aggDepth - 1]; }
  void PopAggLoc() { --aggDepth; }

  /// 失配 → header-only AGG_MISS（保留 messageId 与 offsetA=request_offset）。
  void RewriteToAggMiss() {
    type = PacketType::AGG_MISS;
    repair = false;
    payloadValid = false;
    creditValid = false;
    aggregated = false;
    aggDepth = 0;
    offsetB = 0;
    fanin[0] = fanin[1] = fanin[2] = 0;
    aggLoc[0] = aggLoc[1] = aggLoc[2] = kAggIndexUnused;
  }

 private:
  static void Store16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v & 0xffu);
  }
  static uint16_t Load16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) << 8 | p[1];
  }
  static void Store24(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 16) & 0xffu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xffu);
    p[2] = static_cast<uint8_t>(v & 0xffu);
  }
  static uint32_t Load24(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 16 |
           static_cast<uint32_t>(p[1]) << 8 | p[2];
  }
};

/// 24-bit packet offset 的半空间序列号比较（implementation.md §6 前提 2）。
inline bool SeqBefore(uint32_t a, uint32_t b) {
  return ((b - a) & 0xFFFFFFu) != 0 && ((b - a) & 0xFFFFFFu) < (1u << 23);
}
inline uint32_t SeqAdd(uint32_t a, uint32_t d) { return (a + d) & 0xFFFFFFu; }
inline uint32_t SeqDist(uint32_t from, uint32_t base) {
  return (from - base) & 0xFFFFFFu;
}

/// 端口 ⟺ (group, channel, subchannel) 算术，无查表。
inline uint16_t ChannelPort(uint32_t group, uint32_t channel, uint32_t sub) {
  return static_cast<uint16_t>(kChannelPortBase +
                               (group * kMaxRanksPerGroup + channel) *
                                   kMaxSubchannels + sub);
}
inline bool PortInRange(uint16_t port) {
  return port >= kChannelPortBase && port < kChannelPortLimit;
}
inline uint32_t PortToChannel(uint16_t port) {
  return ((port - kChannelPortBase) / kMaxSubchannels) % kMaxRanksPerGroup;
}
inline uint32_t PortToGroup(uint16_t port) {
  return (port - kChannelPortBase) / (kMaxSubchannels * kMaxRanksPerGroup);
}
inline uint32_t PortToSubchannel(uint16_t port) {
  return (port - kChannelPortBase) % kMaxSubchannels;
}

/// (group, channel) → 复合 channel 键（= 端口空间中 sub 剥离后的 channel
/// 下标 group*kMaxRanks + channel）。host 侧 channel 表用它复合索引：跨
/// group 复用同一 channel id 在同一 host 上不再碰撞。
inline uint32_t ChannelKey(uint32_t group, uint32_t channel) {
  return group * kMaxRanksPerGroup + channel;
}
/// 端口 → 复合 channel 键（sub 位剥离，无 group/channel 分解、无取模丢
/// 位）：与 ChannelKey(PortToGroup, PortToChannel) 恒等，但直接整除得出。
inline uint32_t PortToChannelKey(uint16_t port) {
  return (static_cast<uint32_t>(port) - kChannelPortBase) / kMaxSubchannels;
}

}  // namespace arbor
}  // namespace ns3

#endif  // ARBOR_WIRE_H
