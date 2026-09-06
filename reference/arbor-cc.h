/**
 * arbor-cc.h — responder 侧端到端拥塞控制（host/src/endpoint_cc.h 的
 * ns-3 移植，时间单位从 TSC 改为纳秒，控制律逐行对应）。
 *
 * 统一接口：每 (channel, subchannel) 一个 State；样本 = 一份 normal
 * credit 的闭环完成时延（发出 → 位图收齐）；输出 pacing 间隔 pacingNs
 * 与在途 credit 上限 windowCredits（0 = 未收敛，使用启动窗）。
 * repair / 重放 / AGG_MISS 不取样。
 */
#ifndef ARBOR_CC_H
#define ARBOR_CC_H

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ns3 {
namespace arbor {

enum class CcMode : uint8_t {
  DISABLED = 0,
  FIXED,
  OSCAR,
  TIMELY,
  SWIFT,
  DCQCN_STYLE
};

struct CcParams {
  double lineRateBps = 100e9;      ///< subchannel 瓶颈线速 µ_s
  double initialRateBps = 0;       ///< 冷启动速率；0 = 线速（harness 可自动分摊）
  uint32_t payloadSize = 8192;     ///< 一份 credit 授权的数据字节数 P

  // TIMELY（SIGCOMM'15，按采样间隔归一的适配见 implementation.md §6.1）
  double timelyTlowNs = 10e3;
  double timelyThighNs = 40e3;
  double timelyAdditiveBps = 100e6;
  double timelyMinRateBps = 100e6;
  double timelyAlpha = 0.875;
  double timelyBeta = 0.2;

  // Swift（SIGCOMM'20）
  double swiftTargetQueueNs = 5e3;
  double swiftFlowScalingRangeNs = 50e3;
  double swiftFlowScalingMinCwnd = 0.1;
  double swiftFlowScalingMaxCwnd = 100.0;
  double swiftAi = 0.5;
  double swiftBeta = 0.8;
  double swiftMaxMdf = 0.5;
  double swiftMinCwnd = 0.001;

  // Oscar
  double oscarUAi = 0.001;
  double oscarUHai = 0.05;
  double oscarUMin = 0.001;
  double oscarTargetRttMultiplier = 1.25;
  double oscarBaseEpsilon = 1.0 / 64.0;
  uint32_t oscarMinBatch = 3;

  // DCQCN-style group ECN
  double dcqcnG = 1.0 / 32.0;
  double dcqcnDecreaseFactor = 0.25;
  double dcqcnAdditiveBps = 12.5e6;
  double dcqcnMinRateBps = 100e6;

  uint32_t maxWindow = 1u << 20;   ///< 仿真中非限制性（实现护栏不建模）
  uint32_t maxPacingBurst = 1;     ///< 轮询迟到后的有界补发份数
};

struct CcState {
  // 调度输出
  uint64_t pacingNs = 0;       ///< 相邻 credit 最小间隔（0 = 无 pacing）
  uint32_t windowCredits = 0;  ///< W_s（0 = 未收敛 → 启动窗）
  uint64_t nextCreditNs = 0;   ///< pacing 截止时刻
  uint32_t outstanding = 0;
  uint32_t peakOutstanding = 0;  ///< 事件级未完成 credit 高水位（只读遥测）
  uint64_t issued = 0;

  uint64_t rttBaseNs = 0;
  uint64_t lastRttNs = 0;
  uint64_t maxRttNs = 0;
  uint64_t samples = 0;

  // Oscar 批式估计
  double sumX = 0, sumY = 0, sumXX = 0, sumXY = 0;
  uint64_t sumInfl = 0;
  uint32_t batchCnt = 0;
  uint64_t batchStartNs = 0, batchLastX = 0;
  double u = 1.0;

  // TIMELY
  double timelyRateBps = 0;    // bytes/s
  double timelyRttDiffNs = 0;
  uint64_t timelyPrevRttNs = 0, timelyLastUpdateNs = 0;
  uint32_t timelyNegGrad = 0;

  // Swift
  double swiftCwnd = 0;
  double swiftFlowScalingAlphaNs = 0;
  double swiftFlowScalingBetaNs = 0;
  double swiftFlowScalingRangeNs = 0;
  uint64_t swiftLastDecreaseNs = 0;
};

class ArborCc {
 public:
  static void Reset(CcState& s, CcMode mode, const CcParams& p,
                    uint32_t startupWindow) {
    const uint64_t keptRttBase = s.rttBaseNs;  // idle reset 保留学到的基准
    const uint64_t keptIssued = s.issued;      // 观测计数跨 message/idle 保留
    const uint64_t keptLastRtt = s.lastRttNs;
    const uint64_t keptMaxRtt = s.maxRttNs;
    const uint64_t keptSamples = s.samples;
    s = CcState{};
    s.rttBaseNs = keptRttBase;
    s.issued = keptIssued;
    s.lastRttNs = keptLastRtt;
    s.maxRttNs = keptMaxRtt;
    s.samples = keptSamples;
    const double initialRate =
        p.initialRateBps > 0
            ? std::clamp(p.initialRateBps, 1.0, p.lineRateBps)
            : p.lineRateBps;
    s.u = std::clamp(initialRate / p.lineRateBps, p.oscarUMin, 1.0);
    if (mode == CcMode::TIMELY || mode == CcMode::DCQCN_STYLE) {
      s.timelyRateBps = initialRate / 8.0;  // bytes/s
    } else if (mode == CcMode::SWIFT) {
      s.swiftFlowScalingRangeNs = p.swiftFlowScalingRangeNs;
      if (s.swiftFlowScalingRangeNs > 0.0) {
        const double invSqrtMin = 1.0 / std::sqrt(p.swiftFlowScalingMinCwnd);
        const double invSqrtMax = 1.0 / std::sqrt(p.swiftFlowScalingMaxCwnd);
        s.swiftFlowScalingAlphaNs =
            s.swiftFlowScalingRangeNs / (invSqrtMin - invSqrtMax);
        s.swiftFlowScalingBetaNs =
            -s.swiftFlowScalingAlphaNs * invSqrtMax;
      }
      s.swiftCwnd = std::max(1u, startupWindow);
      PublishSwift(s, p, 1, 0);
    }
    if (mode != CcMode::DISABLED && initialRate < p.lineRateBps) {
      s.pacingNs = std::max<uint64_t>(
          1, static_cast<uint64_t>(
                 std::ceil(double(p.payloadSize) * 8e9 / initialRate)));
      s.nextCreditNs = 0;
    }
  }

  /// 一份 normal credit 首次完成（位图收齐）。ce 供 DCQCN_STYLE 使用。
  static void OnSample(CcMode mode, CcState& s, uint64_t nowNs,
                       uint64_t issueNs, uint32_t inflightAtIssue, bool ce,
                       const CcParams& p) {
    if (nowNs > issueNs) {
      s.lastRttNs = nowNs - issueNs;
      s.maxRttNs = std::max(s.maxRttNs, s.lastRttNs);
      ++s.samples;
    }
    switch (mode) {
      case CcMode::DISABLED: return;
      case CcMode::FIXED: return;
      case CcMode::OSCAR: OscarOnSample(s, nowNs, issueNs, inflightAtIssue); return;
      case CcMode::TIMELY: TimelyOnSample(s, nowNs, issueNs, p); return;
      case CcMode::SWIFT: SwiftOnSample(s, nowNs, issueNs, p); return;
      case CcMode::DCQCN_STYLE: DcqcnOnSample(s, nowNs, issueNs, ce, p); return;
    }
  }

  /// 冷路径周期更新（Oscar 批式；发放循环内调用）。
  static void PeriodicUpdate(CcMode mode, CcState& s, const CcParams& p) {
    if (mode == CcMode::OSCAR) OscarUpdate(s, p);
  }

  /// pacing 截止时刻推进：有界亏空补偿（CC_MAX_PACING_BURST=8）。
  static uint64_t AdvancePacing(uint64_t nextNs, uint64_t nowNs,
                                uint64_t pacingNs, uint32_t maxBurst) {
    if (pacingNs == 0) return nowNs;
    const uint64_t maxDebt = pacingNs * (std::max(1u, maxBurst) - 1u);
    const uint64_t floorNs = nowNs > maxDebt ? nowNs - maxDebt : 0;
    return std::max(nextNs, floorNs) + pacingNs;
  }

 private:
  // ── TIMELY ──
  static void PublishTimely(CcState& s, const CcParams& p) {
    const double minRate = p.timelyMinRateBps / 8.0;
    const double maxRate = p.lineRateBps / 8.0;
    s.timelyRateBps = std::clamp(s.timelyRateBps, minRate, maxRate);
    s.pacingNs = std::max<uint64_t>(
        1, static_cast<uint64_t>(std::ceil(p.payloadSize * 1e9 / s.timelyRateBps)));
    s.windowCredits = p.maxWindow;
  }
  static void PublishDcqcn(CcState& s, const CcParams& p) {
    const double minRate = p.dcqcnMinRateBps / 8.0;
    const double maxRate = p.lineRateBps / 8.0;
    s.timelyRateBps = std::clamp(s.timelyRateBps, minRate, maxRate);
    s.pacingNs = std::max<uint64_t>(
        1, static_cast<uint64_t>(
               std::ceil(p.payloadSize * 1e9 / s.timelyRateBps)));
    s.windowCredits = p.maxWindow;
  }
  static void TimelyOnSample(CcState& s, uint64_t now, uint64_t issue,
                             const CcParams& p) {
    const uint64_t rtt = now - issue;
    if (rtt == 0) return;
    if (s.rttBaseNs == 0 || rtt < s.rttBaseNs) s.rttBaseNs = rtt;
    if (s.timelyRateBps == 0) s.timelyRateBps = p.lineRateBps / 8.0;
    if (s.timelyPrevRttNs == 0) {
      s.timelyPrevRttNs = rtt;
      s.timelyLastUpdateNs = now;
      PublishTimely(s, p);
      return;
    }
    const uint64_t elapsed = now - s.timelyLastUpdateNs;
    const double uw = std::min(1.0, double(elapsed) / double(std::max<uint64_t>(1, s.rttBaseNs)));
    const double diff = double(rtt) - double(s.timelyPrevRttNs);
    const double alpha = p.timelyAlpha * uw;
    s.timelyRttDiffNs = (1.0 - alpha) * s.timelyRttDiffNs + alpha * diff;
    const double gradient = s.timelyRttDiffNs / double(s.rttBaseNs);
    s.timelyPrevRttNs = rtt;
    s.timelyLastUpdateNs = now;

    const double low = double(s.rttBaseNs) + p.timelyTlowNs;
    const double high = double(s.rttBaseNs) + p.timelyThighNs;
    const double additive = (p.timelyAdditiveBps / 8.0) * uw;
    if (double(rtt) <= low) {
      s.timelyNegGrad = 0;
      s.timelyRateBps += additive;
    } else if (double(rtt) >= high) {
      s.timelyNegGrad = 0;
      const double pressure = 1.0 - high / double(rtt);
      s.timelyRateBps *= 1.0 - std::clamp(p.timelyBeta * uw * pressure, 0.0, 0.99);
    } else if (gradient <= 0.0) {
      if (gradient < 0.0) { if (s.timelyNegGrad < 5) ++s.timelyNegGrad; }
      else s.timelyNegGrad = 0;
      s.timelyRateBps += (s.timelyNegGrad >= 5 ? 5.0 : 1.0) * additive;
    } else {
      s.timelyNegGrad = 0;
      // 梯度自含采样间隔因子，不再乘 uw（修复过的双重缩放）。
      s.timelyRateBps *= 1.0 - std::clamp(p.timelyBeta * gradient, 0.0, 0.99);
    }
    PublishTimely(s, p);
  }

  // ── Swift ──
  static void PublishSwift(CcState& s, const CcParams& p, uint64_t rtt,
                           uint64_t now) {
    if (s.swiftCwnd < 1.0) {
      s.windowCredits = 1;
      s.pacingNs = std::max<uint64_t>(
          1, static_cast<uint64_t>(std::ceil(double(std::max<uint64_t>(1, rtt)) / s.swiftCwnd)));
      return;
    }
    s.windowCredits = std::clamp<uint32_t>(
        static_cast<uint32_t>(std::floor(s.swiftCwnd)), 1u, p.maxWindow);
    s.nextCreditNs = now;
    s.pacingNs = 0;
  }
  static void SwiftOnSample(CcState& s, uint64_t now, uint64_t issue,
                            const CcParams& p) {
    const uint64_t rtt = now - issue;
    if (rtt == 0) return;
    if (s.rttBaseNs == 0 || rtt < s.rttBaseNs) s.rttBaseNs = rtt;
    if (s.swiftCwnd <= 0.0) s.swiftCwnd = 1.0;
    const double flowScaling = std::clamp(
        s.swiftFlowScalingAlphaNs / std::sqrt(s.swiftCwnd) +
            s.swiftFlowScalingBetaNs,
        0.0, s.swiftFlowScalingRangeNs);
    const double target =
        double(s.rttBaseNs) + p.swiftTargetQueueNs + flowScaling;
    if (double(rtt) < target) {
      s.swiftCwnd += s.swiftCwnd >= 1.0 ? p.swiftAi / s.swiftCwnd : p.swiftAi;
    } else if (s.swiftLastDecreaseNs == 0 || now - s.swiftLastDecreaseNs >= rtt) {
      const double pressure = (double(rtt) - target) / double(rtt);
      const double mdf = std::min(p.swiftMaxMdf, p.swiftBeta * pressure);
      if (mdf > 0.0) {
        s.swiftCwnd *= 1.0 - mdf;
        s.swiftLastDecreaseNs = now;
      }
    }
    s.swiftCwnd = std::clamp(s.swiftCwnd, p.swiftMinCwnd, double(p.maxWindow));
    PublishSwift(s, p, rtt, now);
  }

  // ── Oscar ──
  static void OscarOnSample(CcState& s, uint64_t now, uint64_t issue,
                            uint32_t inflight) {
    const uint64_t delay = now - issue;
    if (delay == 0) return;
    if (s.rttBaseNs == 0 || delay < s.rttBaseNs) s.rttBaseNs = delay;
    if (s.batchCnt == 0 && s.batchStartNs == 0) s.batchStartNs = issue;
    const double x = issue >= s.batchStartNs ? double(issue - s.batchStartNs)
                                             : -double(s.batchStartNs - issue);
    const double y = double(delay);
    s.sumX += x; s.sumY += y; s.sumXX += x * x; s.sumXY += x * y;
    s.sumInfl += inflight;
    s.batchLastX = std::max(s.batchLastX, issue);
    ++s.batchCnt;
  }
  static void OscarUpdate(CcState& s, const CcParams& p) {
    if (s.rttBaseNs == 0) return;
    const uint64_t tau = s.rttBaseNs >> 1;
    if (s.batchStartNs == 0 || s.batchLastX <= s.batchStartNs ||
        s.batchLastX - s.batchStartNs < tau || s.batchCnt < p.oscarMinBatch)
      return;
    const double n = s.batchCnt;
    const double d = s.sumY / n;
    const double det = n * s.sumXX - s.sumX * s.sumX;
    const double g = det > 0.0 ? (n * s.sumXY - s.sumX * s.sumY) / det : 0.0;
    const double base = double(s.rttBaseNs);
    const double dTarget = base * p.oscarTargetRttMultiplier;
    const double mu = p.lineRateBps / 8.0;  // bytes/s
    if (d <= base * (1.0 + p.oscarBaseEpsilon)) {
      s.u += p.oscarUHai;
    } else {
      const double wBytes = (double(s.sumInfl) / n) * p.payloadSize;
      const double uW = wBytes * 1e9 / (d * mu);
      const double span = double(s.batchLastX - s.batchStartNs);
      const double rate = n * p.payloadSize * 1e9 / span;
      const double uR = rate / ((1.0 + g) * mu);
      s.u = d < dTarget ? std::max(uW, uR) : std::min(uW, uR);
      s.u += p.oscarUAi;
    }
    s.u = std::clamp(s.u, p.oscarUMin, 1.0);
    const double targetRate = s.u * mu;
    s.pacingNs = std::max<uint64_t>(
        1, static_cast<uint64_t>(std::ceil(p.payloadSize * 1e9 / targetRate)));
    const double bdpBytes = base / 1e9 * mu;
    double windowBytes = s.u * (dTarget / 1e9) * mu;
    if (windowBytes < bdpBytes) windowBytes = bdpBytes;
    s.windowCredits = std::clamp<uint32_t>(
        static_cast<uint32_t>(windowBytes / p.payloadSize), 1u, p.maxWindow);
    s.sumX = s.sumY = s.sumXX = s.sumXY = 0.0;
    s.sumInfl = 0;
    s.batchCnt = 0;
    s.batchStartNs = s.batchLastX;
    s.batchLastX = 0;
  }

  // ── DCQCN-style group ECN（appendix 口径：组级 CE 比例 → AIMD） ──
  //    实验计划 G4 所需；实现为按样本 CE 的 α-EWMA + 乘性减/加性增。
  static void DcqcnOnSample(CcState& s, uint64_t now, uint64_t issue, bool ce,
                            const CcParams& p) {
    const uint64_t rtt = now - issue;
    if (rtt == 0) return;
    if (s.rttBaseNs == 0 || rtt < s.rttBaseNs) s.rttBaseNs = rtt;
    if (s.timelyRateBps == 0) s.timelyRateBps = p.lineRateBps / 8.0;
    // 复用 timelyRttDiffNs 存 CE EWMA α。credit 粒度可能远小于 RTT，
    // 因此把 g=1/16 按 elapsed/RTT 缩放；否则 group-OR 的密集样本会在
    // 一个 RTT 内重复更新几十次。乘性减速同样每 RTT 至多一次。
    const uint64_t elapsed =
        s.timelyLastUpdateNs == 0 ? rtt : now - s.timelyLastUpdateNs;
    const double uw = std::min(
        1.0, double(elapsed) / double(std::max<uint64_t>(1, s.rttBaseNs)));
    const double gain = 1.0 - std::pow(1.0 - p.dcqcnG, uw);
    s.timelyRttDiffNs =
        (1.0 - gain) * s.timelyRttDiffNs + gain * (ce ? 1.0 : 0.0);
    if (ce && (s.swiftLastDecreaseNs == 0 ||
               now - s.swiftLastDecreaseNs >= rtt)) {
      s.timelyRateBps *=
          1.0 - p.dcqcnDecreaseFactor * s.timelyRttDiffNs;
      s.swiftLastDecreaseNs = now;
    } else if (!ce) {
      s.timelyRateBps += (p.dcqcnAdditiveBps / 8.0) * uw;
    }
    s.timelyLastUpdateNs = now;
    PublishDcqcn(s, p);
  }
};

}  // namespace arbor
}  // namespace ns3

#endif  // ARBOR_CC_H
