/**
 * arbor-packet.h — [UDP 8B][Arbor V3 20B] 合一的 ns3::Header。
 *
 * 线上布局与 testbed 完全一致（标准 UDP 头 + V3 头，UDP checksum 恒 0）；
 * 合并成单个 Header 类以便在交换机上一次摘除/改写/回填。IPv4 层使用
 * 标准 ns3::Ipv4Header（protocol = ARBOR_PROTO），CustomHeader 因此能
 * 照常解析 sip/dip/l3Prot 供路由与 ECN 使用。
 */
#ifndef ARBOR_PACKET_H
#define ARBOR_PACKET_H

#include "ns3/header.h"
#include "arbor-wire.h"

namespace ns3 {

class ArborUdpHeader : public Header {
 public:
  static constexpr uint32_t kLen = 8 + arbor::kArborHdrLen;  // 28 B

  uint16_t sport = 0;
  uint16_t dport = 0;
  arbor::WireHeader h;
  uint32_t payloadLen = 0;  ///< 序列化时写入 UDP length（8+20+payload）

  static TypeId GetTypeId(void) {
    static TypeId tid = TypeId("ns3::ArborUdpHeader")
                            .SetParent<Header>()
                            .AddConstructor<ArborUdpHeader>();
    return tid;
  }
  TypeId GetInstanceTypeId(void) const override { return GetTypeId(); }
  uint32_t GetSerializedSize(void) const override { return kLen; }

  void Serialize(Buffer::Iterator start) const override {
    uint8_t buf[kLen];
    buf[0] = sport >> 8; buf[1] = sport & 0xff;
    buf[2] = dport >> 8; buf[3] = dport & 0xff;
    const uint16_t ulen = static_cast<uint16_t>(8 + arbor::kArborHdrLen + payloadLen);
    buf[4] = ulen >> 8; buf[5] = ulen & 0xff;
    buf[6] = 0; buf[7] = 0;  // UDP checksum 恒 0
    h.Serialize(buf + 8);
    start.Write(buf, kLen);
  }

  uint32_t Deserialize(Buffer::Iterator start) override {
    uint8_t buf[kLen];
    start.Read(buf, kLen);
    sport = static_cast<uint16_t>(buf[0]) << 8 | buf[1];
    dport = static_cast<uint16_t>(buf[2]) << 8 | buf[3];
    const uint16_t ulen = static_cast<uint16_t>(buf[4]) << 8 | buf[5];
    payloadLen = ulen >= 8 + arbor::kArborHdrLen
                     ? ulen - 8 - arbor::kArborHdrLen : 0;
    h = arbor::WireHeader::Deserialize(buf + 8);
    return kLen;
  }

  void Print(std::ostream& os) const override {
    os << "Arbor type=" << unsigned(static_cast<uint8_t>(h.type))
       << " dport=" << dport << " offA=" << h.offsetA << " offB=" << h.offsetB
       << " depth=" << unsigned(h.aggDepth) << " aggd=" << h.aggregated;
  }
};

}  // namespace ns3

#endif  // ARBOR_PACKET_H
