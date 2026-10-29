#!/usr/bin/env bash
# Checks that the nr symbols used by the MultiObj scheduler / scenario exist in the installed nr sources.
# usage: ./verify_api.sh /path/to/ns-3-dev        (run BEFORE apply_patch.sh / building)
NR=${1:?usage: verify_api.sh <ns3 dir>}/contrib/nr
chk() { if grep -rqE "$2" "$NR/model" "$NR/helper" "$NR/examples" 2>/dev/null; then echo "  ok       $1"; else echo "  MISSING  $1   (pattern: $2)"; fi; }
echo "nr checkout: $(git -C "$NR" describe --tags --always 2>/dev/null)"
chk "NrMacSchedulerOfdmaQos class"         "class NR_EXPORT NrMacSchedulerOfdmaQos"
chk "NrMacSchedulerTdmaQos class"          "class NR_EXPORT NrMacSchedulerTdmaQos"
chk "NrMacSchedulerOfdmaRR/PF/MR classes"  "class NR_EXPORT NrMacSchedulerOfdmaMR"
chk "NrMacSchedulerUeInfoQos"              "class NrMacSchedulerUeInfoQos"
chk "m_potentialTputDl"                    "m_potentialTputDl"
chk "m_avgTputDl"                          "m_avgTputDl"
chk "m_rlcTransmissionQueueHolDelay"       "m_rlcTransmissionQueueHolDelay"
chk "LC m_fiveQi"                             "m_fiveQi"
chk "GetActiveLCIds"                       "GetActiveLCIds"
chk "GetUeCompareDlFn"                     "GetUeCompareDlFn"
chk "BeforeDlSched (virtual)"              "BeforeDlSched"
chk "UePtrAndBufferReq typedef"            "UePtrAndBufferReq"
chk "NrHelper::SetSchedulerAttribute"      "SetSchedulerAttribute"
chk "NrHelper::SetSchedulerTypeId"         "SetSchedulerTypeId"
chk "NrChannelHelper (nr >= 3.x)"          "class NR_EXPORT NrChannelHelper"
chk "SetupRemoteHost (epc helper)"         "SetupRemoteHost"
chk "ActivateDedicatedEpsBearer"           "ActivateDedicatedEpsBearer"
chk "NrEpsBearer DGBR_DISCRETE_AUT_SMALL"  "DGBR_DISCRETE_AUT_SMALL"
chk "NrEpsBearer NGBR_VIDEO_TCP_PREMIUM"   "NGBR_VIDEO_TCP_PREMIUM"
chk "SlotDataStats trace (utilisation)"    "SlotDataStats"
echo "Fix every MISSING line (rename symbols in nr-patch/* and scratch/*) before building."
