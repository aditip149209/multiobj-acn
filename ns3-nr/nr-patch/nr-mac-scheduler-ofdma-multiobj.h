// SPDX-License-Identifier: GPL-2.0-only
// Multi-objective OFDMA scheduler for 5G-LENA (nr module).
//   S_i = wC*C_i + wF*phi_i + wD*tau_i + wQ*Q_i      (w's sum to 1)
//   C_i = R_i / Rmax   phi_i = 1 - rbar_i / max_j rbar_j   tau_i = min(1, HOL_i / PDB_i)   Q_i = (Pmax-P_i)/(Pmax-Pmin)
// UEs are ranked by descending S_i; the *existing* OFDMA allocator then assigns RBGs in that order.
// The class derives from NrMacSchedulerOfdmaQos so that the UE state (PF averaged throughput, potential rate,
// per-LC HOL delay / 5QI) is reused and ONLY the compare function is new.
//
// !! NOT COMPILED in the authoring environment (nr sources were unreachable). Run ./verify_api.sh first;
// every symbol marked  [VERIFY]  below is taken from memory of the nr sources and may differ between releases.
#ifndef NR_MAC_SCHEDULER_OFDMA_MULTIOBJ_H
#define NR_MAC_SCHEDULER_OFDMA_MULTIOBJ_H

#include "nr-mac-scheduler-ofdma-qos.h"

namespace ns3
{

class NrMacSchedulerOfdmaMultiObj : public NrMacSchedulerOfdmaQos
{
  public:
    static TypeId GetTypeId();
    NrMacSchedulerOfdmaMultiObj();
    ~NrMacSchedulerOfdmaMultiObj() override = default;

  protected:
    void NotifyConstructionCompleted() override;  // validates / normalises the weights

    std::function<bool(const NrMacSchedulerNs3::UePtrAndBufferReq& lhs,
                       const NrMacSchedulerNs3::UePtrAndBufferReq& rhs)>
    GetUeCompareDlFn() const override;
    std::function<bool(const NrMacSchedulerNs3::UePtrAndBufferReq& lhs,
                       const NrMacSchedulerNs3::UePtrAndBufferReq& rhs)>
    GetUeCompareUlFn() const override;

    // keep the running normalisers (max averaged throughput / max potential rate) up to date
    void BeforeDlSched(const NrMacSchedulerNs3::UePtrAndBufferReq& ue,
                       const NrMacSchedulerNs3::FTResources& assignableInIteration) const override;

  private:
    struct Factors
    {
        double C{0}, phi{0}, tau{0}, Q{0}, score{0};
    };
    Factors ComputeFactors(const NrMacSchedulerNs3::UePtrAndBufferReq& ue) const;
    static double FiveQiPriority(uint8_t qci);   // TS 23.501 Table 5.7.4-1 default priority level
    static double FiveQiPdbMs(uint8_t qci);      // packet delay budget [ms]

    double m_wC{0.25}, m_wF{0.25}, m_wD{0.25}, m_wQ{0.25};  // attributes
    double m_prioMin{19}, m_prioMax{90};                      // attributes: range over 5QIs in use
    double m_avgWindowSlots{100};                              // documented EMA constant (PF alpha is inherited)

    mutable double m_maxAvgTput{0};     // max_j rbar_j (slowly decaying running max)
    mutable double m_maxPotential{0};   // R_max proxy (running max of potential rate)
};

} // namespace ns3
#endif
