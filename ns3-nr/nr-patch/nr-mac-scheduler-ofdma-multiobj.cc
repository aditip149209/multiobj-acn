// SPDX-License-Identifier: GPL-2.0-only
#include "nr-mac-scheduler-ofdma-multiobj.h"

#include "nr-mac-scheduler-ue-info-rr.h"
#include "nr-mac-scheduler-ue-info-qos.h"   // [VERIFY] header name of NrMacSchedulerUeInfoQos

#include "ns3/double.h"
#include "ns3/log.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace ns3
{
NS_LOG_COMPONENT_DEFINE("NrMacSchedulerOfdmaMultiObj");
NS_OBJECT_ENSURE_REGISTERED(NrMacSchedulerOfdmaMultiObj);

TypeId
NrMacSchedulerOfdmaMultiObj::GetTypeId()
{
    static TypeId tid =
        TypeId("ns3::NrMacSchedulerOfdmaMultiObj")
            .SetParent<NrMacSchedulerOfdmaQos>()
            .AddConstructor<NrMacSchedulerOfdmaMultiObj>()
            .AddAttribute("WeightChannel", "wC: weight of channel quality C_i", DoubleValue(0.25),
                          MakeDoubleAccessor(&NrMacSchedulerOfdmaMultiObj::m_wC), MakeDoubleChecker<double>(0.0))
            .AddAttribute("WeightFairness", "wF: weight of fairness phi_i", DoubleValue(0.25),
                          MakeDoubleAccessor(&NrMacSchedulerOfdmaMultiObj::m_wF), MakeDoubleChecker<double>(0.0))
            .AddAttribute("WeightDelay", "wD: weight of delay urgency tau_i", DoubleValue(0.25),
                          MakeDoubleAccessor(&NrMacSchedulerOfdmaMultiObj::m_wD), MakeDoubleChecker<double>(0.0))
            .AddAttribute("WeightQos", "wQ: weight of QoS priority Q_i", DoubleValue(0.25),
                          MakeDoubleAccessor(&NrMacSchedulerOfdmaMultiObj::m_wQ), MakeDoubleChecker<double>(0.0))
            .AddAttribute("PriorityMin", "P_min: lowest 5QI priority NUMBER in use (= most important flow)",
                          DoubleValue(19), MakeDoubleAccessor(&NrMacSchedulerOfdmaMultiObj::m_prioMin),
                          MakeDoubleChecker<double>(0.0))
            .AddAttribute("PriorityMax", "P_max: highest 5QI priority NUMBER in use (= least important flow)",
                          DoubleValue(90), MakeDoubleAccessor(&NrMacSchedulerOfdmaMultiObj::m_prioMax),
                          MakeDoubleChecker<double>(0.0));
    return tid;
}

NrMacSchedulerOfdmaMultiObj::NrMacSchedulerOfdmaMultiObj()
    : NrMacSchedulerOfdmaQos()
{
    NS_LOG_FUNCTION(this);
}

void
NrMacSchedulerOfdmaMultiObj::NotifyConstructionCompleted()
{
    NrMacSchedulerOfdmaQos::NotifyConstructionCompleted();
    const double s = m_wC + m_wF + m_wD + m_wQ;
    NS_ABORT_MSG_IF(s <= 0.0, "MultiObj: weights must not all be zero");
    if (std::fabs(s - 1.0) > 1e-6)
    {
        NS_LOG_WARN("MultiObj: weights sum to " << s << ", normalising to 1");
        m_wC /= s; m_wF /= s; m_wD /= s; m_wQ /= s;
    }
}

// 5QI -> default priority level and PDB (3GPP TS 23.501 Table 5.7.4-1; subset)
double
NrMacSchedulerOfdmaMultiObj::FiveQiPriority(uint8_t q)
{
    static const std::map<uint8_t, double> p{{1, 20}, {2, 40}, {3, 30}, {4, 50}, {5, 10}, {6, 60}, {7, 70},
        {8, 80}, {9, 90}, {65, 7}, {66, 20}, {67, 15}, {75, 25}, {79, 65}, {80, 68}, {82, 19}, {83, 22},
        {84, 24}, {85, 21}, {86, 18}};
    auto it = p.find(q);
    return it == p.end() ? 90.0 : it->second;
}

double
NrMacSchedulerOfdmaMultiObj::FiveQiPdbMs(uint8_t q)
{
    static const std::map<uint8_t, double> p{{1, 100}, {2, 150}, {3, 50}, {4, 300}, {5, 100}, {6, 300},
        {7, 100}, {8, 300}, {9, 300}, {65, 75}, {66, 100}, {67, 100}, {75, 50}, {79, 50}, {80, 10},
        {82, 10}, {83, 10}, {84, 30}, {85, 5}, {86, 5}};
    auto it = p.find(q);
    return it == p.end() ? 300.0 : it->second;
}

NrMacSchedulerOfdmaMultiObj::Factors
NrMacSchedulerOfdmaMultiObj::ComputeFactors(const NrMacSchedulerNs3::UePtrAndBufferReq& ue) const
{
    Factors f;
    // [VERIFY] UE info type and member names (inherited from the PF/QoS UE info)
    auto info = dynamic_cast<NrMacSchedulerUeInfoQos*>(ue.first.get());
    NS_ASSERT_MSG(info, "MultiObj scheduler requires NrMacSchedulerUeInfoQos UE representation");

    // C_i: achievable rate of the UE this slot relative to the highest rate seen in the cell
    // MCS is used (not potential throughput) so that wC=1 reproduces nr's MR ordering exactly (MR ranks by DL MCS).
    const double R = info->GetDlMcs();
    f.C = (m_maxPotential > 0) ? std::min(1.0, R / m_maxPotential) : 0.0;

    // phi_i: 1 - rbar_i / max_j rbar_j   (guard against 0/0)
    const double rbar = info->m_avgTputDl;                            // [VERIFY]
    f.phi = (m_maxAvgTput > 1e-9) ? 1.0 - std::min(1.0, rbar / m_maxAvgTput) : 1.0;

    // tau_i, Q_i from the most urgent / highest priority active logical channel of the UE
    double tau = 0.0, q = 0.0;
    const double span = m_prioMax - m_prioMin;
    for (const auto& lcg : info->m_dlLCG)                             // [VERIFY] std::map<uint8_t, LCGPtr>
    {
        for (uint8_t lcId : lcg.second->GetActiveLCIds())             // [VERIFY]
        {
            const auto& lc = lcg.second->GetLC(lcId);                 // [VERIFY]
            const double hol = lc->m_rlcTransmissionQueueHolDelay;    // [VERIFY] ms
            tau = std::max(tau, std::min(1.0, hol / FiveQiPdbMs(lc->m_fiveQi)));   // m_fiveQi (nr v5.1)
            if (span > 0)
            {
                q = std::max(q, (m_prioMax - FiveQiPriority(lc->m_fiveQi)) / span);
            }
        }
    }
    f.tau = tau;
    f.Q = std::min(1.0, std::max(0.0, q));
    f.score = m_wC * f.C + m_wF * f.phi + m_wD * f.tau + m_wQ * f.Q;
    return f;
}

void
NrMacSchedulerOfdmaMultiObj::BeforeDlSched(const NrMacSchedulerNs3::UePtrAndBufferReq& ue,
                                           const NrMacSchedulerNs3::FTResources& assignableInIteration) const
{
    NrMacSchedulerOfdmaQos::BeforeDlSched(ue, assignableInIteration);   // updates potential rate, PF/QoS metrics
    auto info = dynamic_cast<NrMacSchedulerUeInfoQos*>(ue.first.get());
    if (info)
    {
        m_maxAvgTput = std::max(m_maxAvgTput * 0.9999, static_cast<double>(info->m_avgTputDl));
        m_maxPotential = std::max(m_maxPotential * 0.9999, static_cast<double>(info->GetDlMcs()));
    }
}

std::function<bool(const NrMacSchedulerNs3::UePtrAndBufferReq&, const NrMacSchedulerNs3::UePtrAndBufferReq&)>
NrMacSchedulerOfdmaMultiObj::GetUeCompareDlFn() const
{
    return [this](const NrMacSchedulerNs3::UePtrAndBufferReq& l, const NrMacSchedulerNs3::UePtrAndBufferReq& r) {
        const double sl = ComputeFactors(l).score, sr = ComputeFactors(r).score;
        if (sl != sr) { return sl > sr; }                     // higher score first
        return NrMacSchedulerUeInfoRR::CompareUeWeightsDl(l, r);   // same tie-break as nr's MR/PF (round-robin)
    };
}

std::function<bool(const NrMacSchedulerNs3::UePtrAndBufferReq&, const NrMacSchedulerNs3::UePtrAndBufferReq&)>
NrMacSchedulerOfdmaMultiObj::GetUeCompareUlFn() const
{
    // UL is not part of the study: use the QoS scheduler's UL ordering unchanged.
    return NrMacSchedulerOfdmaQos::GetUeCompareUlFn();
}

} // namespace ns3
