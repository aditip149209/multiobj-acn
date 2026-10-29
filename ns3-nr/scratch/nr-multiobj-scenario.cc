// SPDX-License-Identifier: GPL-2.0-only
/*
 * nr-multiobj-scenario.cc -- single-cell 5G-LENA scenario used to tune / evaluate the multi-objective scheduler.
 *
 * One gNB, N UEs, DL UDP traffic with three service classes (eMBB 5QI 8, URLLC 5QI 82, mMTC 5QI 9).
 * Scheduler (RR | MR | PF | QOS | MULTIOBJ) is the only thing that changes between comparison runs:
 * identical numerology, bandwidth, power, channel, UE positions (RNG run = seed) and traffic.
 * OFDMA scheduling is the default (--ofdma=1); --ofdma=0 selects the TDMA variants.
 *
 * Output on stdout (parsed by run_experiments.py):
 *   ROW,<csv header>   ROW,<csv values>   Q,<class>,<quantile>,<delay_ms>
 * with the same schema as the stand-in simulator sim/nrsim.cc.
 *
 * !! NOT COMPILED in the authoring environment. Lines tagged [VERIFY] depend on the nr release;
 * run ../verify_api.sh and compare with contrib/nr/examples/cttc-nr-demo.cc of YOUR nr version.
 */
#include "ns3/antenna-module.h"
#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/nr-module.h"
#include "ns3/point-to-point-module.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

using namespace ns3;
NS_LOG_COMPONENT_DEFINE("NrMultiObjScenario");

struct ClsDef { const char* name; uint8_t fiveqi; double prio; double pdbMs; uint32_t pkt; double intervalS; uint16_t basePort; };
static const ClsDef CLS[3] = {{"embb", 8, 80, 300, 1200, 1200 * 8 / 20e6, 1000},
                              {"urllc", 82, 19, 10, 200, 0.001, 2000},
                              {"mmtc", 9, 90, 300, 100, 0.010, 3000}};

static double Jain(const std::vector<double>& x)
{
    double s = 0, s2 = 0;
    for (double v : x) { s += v; s2 += v * v; }
    return (s2 <= 0 || x.empty()) ? 1.0 : s * s / (x.size() * s2);
}

// ---- utilisation trace: fraction of data symbol x RB resources scheduled (post warm-up)   [VERIFY signature]
static double g_usedReg = 0, g_availReg = 0, g_warm = 0;
static void
SlotDataStats(const SfnSf& sfn, uint32_t scheduledUe, uint32_t usedReg, uint32_t usedSym, uint32_t availableRb, uint32_t availableSym,
              uint16_t bwpId, uint16_t cellId)
{
    if (Simulator::Now().GetSeconds() < g_warm) { return; }
    g_usedReg += usedReg;
    g_availReg += static_cast<double>(availableRb) * availableSym;
}

int
main(int argc, char* argv[])
{
    std::string sched = "PF", traffic = "mixed", channel = "uma_nlos", scenario = "adhoc";
    uint32_t numUes = 20, seed = 1, ofdma = 1, header = 0;
    double wC = .25, wF = .25, wD = .25, wQ = .25, simTime = 3.0, warmup = 1.0, dmin = 35, dmax = 300, speed = 1.0;
    double txDbm = 30.0, bwHz = 100e6, fc = 3.5e9;
    uint16_t numerology = 1;
    CommandLine cmd(__FILE__);
    cmd.AddValue("sched", "RR|MR|PF|QOS|MULTIOBJ", sched);
    cmd.AddValue("traffic", "embb|urllc|mmtc|mixed", traffic);
    cmd.AddValue("channel", "umi_los|umi_nlos|uma_los|uma_nlos", channel);
    cmd.AddValue("scenario", "scenario label (copied to output)", scenario);
    cmd.AddValue("numUes", "number of UEs", numUes);
    cmd.AddValue("seed", "RNG run (traffic/positions identical across schedulers)", seed);
    cmd.AddValue("ofdma", "1 = OFDMA, 0 = TDMA scheduler variants", ofdma);
    cmd.AddValue("wC", "weight channel", wC);
    cmd.AddValue("wF", "weight fairness", wF);
    cmd.AddValue("wD", "weight delay", wD);
    cmd.AddValue("wQ", "weight QoS", wQ);
    cmd.AddValue("simTime", "total simulated time [s]", simTime);
    cmd.AddValue("warmup", "measurement starts after this time [s]", warmup);
    cmd.AddValue("dmin", "min UE distance [m]", dmin);
    cmd.AddValue("dmax", "max UE distance [m]", dmax);
    cmd.AddValue("speed", "UE speed [m/s]", speed);
    cmd.AddValue("header", "print CSV header row", header);
    cmd.Parse(argc, argv);
    g_warm = warmup;

    RngSeedManager::SetSeed(1);
    RngSeedManager::SetRun(seed);
    // --- per-UE class (same rule as the stand-in simulator: 50/25/25 for "mixed")
    std::vector<int> cls(numUes);
    double pmin = 1e9, pmax = -1;
    for (uint32_t u = 0; u < numUes; ++u)
    {
        if (traffic == "embb") cls[u] = 0;
        else if (traffic == "urllc") cls[u] = 1;
        else if (traffic == "mmtc") cls[u] = 2;
        else { uint32_t m = u % 4; cls[u] = (m < 2) ? 0 : (m == 2 ? 1 : 2); }
        pmin = std::min(pmin, CLS[cls[u]].prio);
        pmax = std::max(pmax, CLS[cls[u]].prio);
    }

    // --- nodes & mobility
    NodeContainer gnbNodes, ueNodes;
    gnbNodes.Create(1);
    ueNodes.Create(numUes);
    MobilityHelper mob;
    mob.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    Ptr<ListPositionAllocator> gp = CreateObject<ListPositionAllocator>();
    const double hBS = (channel.rfind("umi", 0) == 0) ? 10.0 : 25.0;
    gp->Add(Vector(0, 0, hBS));
    mob.SetPositionAllocator(gp);
    mob.Install(gnbNodes);
    Ptr<UniformRandomVariable> rv = CreateObject<UniformRandomVariable>();
    Ptr<ListPositionAllocator> up = CreateObject<ListPositionAllocator>();
    std::vector<Vector> vel(numUes);
    for (uint32_t u = 0; u < numUes; ++u)
    {
        double r = std::sqrt(rv->GetValue(0, 1) * (dmax * dmax - dmin * dmin) + dmin * dmin);
        double th = rv->GetValue(0, 2 * M_PI), dir = rv->GetValue(0, 2 * M_PI);
        up->Add(Vector(r * std::cos(th), r * std::sin(th), 1.5));
        vel[u] = Vector(speed * std::cos(dir), speed * std::sin(dir), 0);
    }
    mob.SetPositionAllocator(up);
    mob.SetMobilityModel(speed > 5.0 ? "ns3::ConstantVelocityMobilityModel" : "ns3::ConstantPositionMobilityModel");
    mob.Install(ueNodes);
    if (speed > 5.0)
    {
        for (uint32_t u = 0; u < numUes; ++u)
            ueNodes.Get(u)->GetObject<ConstantVelocityMobilityModel>()->SetVelocity(vel[u]);
    }

    // --- NR helpers (structure follows cttc-nr-demo.cc)   [VERIFY against your nr version]
    Ptr<NrPointToPointEpcHelper> epcHelper = CreateObject<NrPointToPointEpcHelper>();
    Ptr<IdealBeamformingHelper> bfHelper = CreateObject<IdealBeamformingHelper>();
    Ptr<NrHelper> nrHelper = CreateObject<NrHelper>();
    nrHelper->SetBeamformingHelper(bfHelper);
    nrHelper->SetEpcHelper(epcHelper);

    CcBwpCreator ccBwpCreator;
    CcBwpCreator::SimpleOperationBandConf bandConf(fc, bwHz, 1);          // [VERIFY] ctor (fc, bw, numCc)
    OperationBandInfo band = ccBwpCreator.CreateOperationBandContiguousCc(bandConf);
    // channel: 3GPP UMa/UMi, LOS forced or NLOS forced          [VERIFY] NrChannelHelper API (nr >= 3.0)
    Ptr<NrChannelHelper> channelHelper = CreateObject<NrChannelHelper>();
    const bool los = channel.find("_los") != std::string::npos;
    channelHelper->ConfigureFactories((channel.rfind("umi", 0) == 0) ? "UMi" : "UMa",
                                      los ? "LOS" : "NLOS", "ThreeGpp");
    channelHelper->AssignChannelsToBands({band});
    BandwidthPartInfoPtrVector allBwps = CcBwpCreator::GetAllBwps({band});

    bfHelper->SetAttribute("BeamformingMethod", TypeIdValue(DirectPathBeamforming::GetTypeId()));
    nrHelper->SetUeAntennaAttribute("NumRows", UintegerValue(1));
    nrHelper->SetUeAntennaAttribute("NumColumns", UintegerValue(1));
    nrHelper->SetGnbAntennaAttribute("NumRows", UintegerValue(8));
    nrHelper->SetGnbAntennaAttribute("NumColumns", UintegerValue(8));    // ~ +10 dB array gain, as in the stand-in
    nrHelper->SetGnbPhyAttribute("TxPower", DoubleValue(txDbm));
    nrHelper->SetGnbPhyAttribute("Numerology", UintegerValue(numerology));

    // --- scheduler: only this differs between runs
    const std::string pre = ofdma ? "ns3::NrMacSchedulerOfdma" : "ns3::NrMacSchedulerTdma";
    std::string tid = pre + (sched == "RR" ? "RR" : sched == "MR" ? "MR" : sched == "PF" ? "PF"
                                            : sched == "QOS" ? "Qos" : "MultiObj");
    nrHelper->SetSchedulerTypeId(TypeId::LookupByName(tid));
    if (sched == "MULTIOBJ")
    {
        nrHelper->SetSchedulerAttribute("WeightChannel", DoubleValue(wC));
        nrHelper->SetSchedulerAttribute("WeightFairness", DoubleValue(wF));
        nrHelper->SetSchedulerAttribute("WeightDelay", DoubleValue(wD));
        nrHelper->SetSchedulerAttribute("WeightQos", DoubleValue(wQ));
        nrHelper->SetSchedulerAttribute("PriorityMin", DoubleValue(pmin));
        nrHelper->SetSchedulerAttribute("PriorityMax", DoubleValue(pmax));
    }

    NetDeviceContainer gnbDev = nrHelper->InstallGnbDevice(gnbNodes, allBwps);
    NetDeviceContainer ueDev = nrHelper->InstallUeDevice(ueNodes, allBwps);
    int64_t rs = 100;  // fixed stream assignment -> identical channel realisations across schedulers
    rs += nrHelper->AssignStreams(gnbDev, rs);
    rs += nrHelper->AssignStreams(ueDev, rs);
    for (auto it = gnbDev.Begin(); it != gnbDev.End(); ++it)
        DynamicCast<NrGnbNetDevice>(*it)->UpdateConfig();
    for (auto it = ueDev.Begin(); it != ueDev.End(); ++it)
        DynamicCast<NrUeNetDevice>(*it)->UpdateConfig();

    // --- core network, remote host, IP
    auto [remoteHost, remoteAddr] = epcHelper->SetupRemoteHost("100Gb/s", 2500, Seconds(0.0));   // [VERIFY]
    InternetStackHelper internet;
    internet.Install(ueNodes);
    Ipv4InterfaceContainer ueIf = epcHelper->AssignUeIpv4Address(ueDev);
    Ipv4StaticRoutingHelper srh;
    for (uint32_t u = 0; u < numUes; ++u)
    {
        Ptr<Ipv4StaticRouting> r = srh.GetStaticRouting(ueNodes.Get(u)->GetObject<Ipv4>());
        r->SetDefaultRoute(epcHelper->GetUeDefaultGatewayAddress(), 1);
    }
    nrHelper->AttachToClosestGnb(ueDev, gnbDev);

    // --- traffic: one DL UDP flow per UE, own dedicated bearer with the class 5QI
    ApplicationContainer servers, clients;
    const double tStart = 0.4;  // after attach; measurement window starts at `warmup`
    for (uint32_t u = 0; u < numUes; ++u)
    {
        const ClsDef& c = CLS[cls[u]];
        uint16_t port = c.basePort + u;
        UdpServerHelper srv(port);
        servers.Add(srv.Install(ueNodes.Get(u)));
        UdpClientHelper cli(ueIf.GetAddress(u), port);
        cli.SetAttribute("MaxPackets", UintegerValue(0xFFFFFFFF));
        cli.SetAttribute("PacketSize", UintegerValue(c.pkt));
        cli.SetAttribute("Interval", TimeValue(Seconds(c.intervalS)));
        ApplicationContainer a = cli.Install(remoteHost);
        a.Start(Seconds(tStart + rv->GetValue(0, c.intervalS)));   // random phase, like the stand-in
        clients.Add(a);

        Ptr<NrQosRule> tft = Create<NrQosRule>();
        NrQosRule::PacketFilter pf;
        pf.localPortStart = port;
        pf.localPortEnd = port;
        tft->Add(pf);
        NrQosFlow::FiveQi q = cls[u] == 0 ? NrQosFlow::NGBR_VIDEO_TCP_PREMIUM            // 5QI 8   [VERIFY enum]
                           : cls[u] == 1 ? NrQosFlow::DGBR_DISCRETE_AUT_SMALL           // 5QI 82  [VERIFY enum]
                                         : NrQosFlow::NGBR_VIDEO_TCP_DEFAULT;           // 5QI 9
        NrQosFlow bearer(q);
        if (cls[u] == 1)   // delay-critical GBR needs QoS info   [VERIFY field names]
        {
            NrGbrQosInformation qos;
            qos.gbrDl = static_cast<uint64_t>(c.pkt * 8 / c.intervalS);
            qos.mbrDl = qos.gbrDl;
            bearer.gbrQosInfo = qos;
        }
        nrHelper->ActivateDedicatedQosFlow(ueDev.Get(u), bearer, tft);
    }
    servers.Start(Seconds(0.0));
    servers.Stop(Seconds(simTime));
    clients.Stop(Seconds(simTime));

    // --- measurement: FlowMonitor with a snapshot at the end of warm-up (stats are differenced)
    FlowMonitorHelper fmh;
    NodeContainer ends;
    ends.Add(remoteHost);
    ends.Add(ueNodes);
    Ptr<FlowMonitor> mon = fmh.Install(ends);
    mon->SetAttribute("DelayBinWidth", DoubleValue(0.0005));
    mon->SetAttribute("MaxPerHopDelay", TimeValue(Seconds(5)));
    std::map<FlowId, FlowMonitor::FlowStats> snap;
    Simulator::Schedule(Seconds(warmup), [&]() { mon->CheckForLostPackets(); snap = mon->GetFlowStats(); });
    Config::ConnectWithoutContext(   // [VERIFY] trace path/name
        "/NodeList/*/DeviceList/*/$ns3::NrGnbNetDevice/BandwidthPartMap/*/NrGnbPhy/SlotDataStats",
        MakeCallback(&SlotDataStats));

    Simulator::Stop(Seconds(simTime));
    Simulator::Run();
    mon->CheckForLostPackets();
    auto stats = mon->GetFlowStats();
    Ptr<Ipv4FlowClassifier> fc4 = DynamicCast<Ipv4FlowClassifier>(fmh.GetClassifier());

    // --- aggregate per UE / class
    const double Tm = simTime - warmup;
    std::vector<double> thr(numUes, 0), thrN(numUes, 0), thrCls(3, 0);
    std::vector<std::vector<double>> hist(4);   // per class (+all): delay bin counts
    const double bw = 0.0005;
    for (auto& h : hist) h.assign(20000, 0.0);
    double sumDelay[4] = {0, 0, 0, 0}, rxP[4] = {0, 0, 0, 0}, txP[4] = {0, 0, 0, 0}, late = 0;
    for (auto& kv : stats)
    {
        auto t = fc4->FindFlow(kv.first);
        if (t.destinationAddress == remoteAddr) continue;          // ignore UL/ACK flows
        int u = -1;
        for (uint32_t i = 0; i < numUes; ++i)
            if (t.destinationAddress == ueIf.GetAddress(i)) u = i;
        if (u < 0) continue;
        FlowMonitor::FlowStats s = kv.second, s0 = FlowMonitor::FlowStats();   // value-init: members are otherwise uninitialised
        if (snap.count(kv.first)) s0 = snap[kv.first];
        const int k = cls[u];
        double rxB = s.rxBytes - s0.rxBytes, rxN = s.rxPackets - s0.rxPackets, txN = s.txPackets - s0.txPackets;
        double dS = (s.delaySum - s0.delaySum).GetSeconds();
        thr[u] = rxB * 8 / Tm / 1e6;
        double off = CLS[k].pkt * 8 / CLS[k].intervalS / 1e6;
        thrN[u] = std::min(1.0, thr[u] / off);
        thrCls[k] += thr[u];
        for (int idx : {k, 3})
        {
            sumDelay[idx] += dS; rxP[idx] += rxN; txP[idx] += txN;
            for (uint32_t b = 0; b < s.delayHistogram.GetNBins() && b < 20000; ++b)
            {
                double c1 = s.delayHistogram.GetBinCount(b) - (b < s0.delayHistogram.GetNBins() ? s0.delayHistogram.GetBinCount(b) : 0);   // snapshot histogram has fewer bins if delays grew later
                hist[idx][b] += c1;
            }
        }
        if (k == 1)   // URLLC late packets: histogram bins above the PDB
            for (uint32_t b = static_cast<uint32_t>(CLS[1].pdbMs / 1000.0 / bw); b < s.delayHistogram.GetNBins() && b < 20000; ++b)
                late += s.delayHistogram.GetBinCount(b) - (b < s0.delayHistogram.GetNBins() ? s0.delayHistogram.GetBinCount(b) : 0);
    }
    auto quant = [&](int idx, double q) {
        double tot = 0; for (double v : hist[idx]) tot += v;
        if (tot <= 0) return std::nan("");
        double acc = 0;
        for (size_t b = 0; b < hist[idx].size(); ++b) { acc += hist[idx][b]; if (acc >= q * tot) return (b + 0.5) * bw * 1e3; }
        return std::nan("");
    };
    auto mean = [&](int i) { return rxP[i] > 0 ? sumDelay[i] / rxP[i] * 1e3 : std::nan(""); };
    double T = 0; for (double v : thr) T += v;
    double plr = txP[3] > 0 ? 1.0 - rxP[3] / txP[3] : 0.0;
    double viol = txP[1] > 0 ? (late + (txP[1] - rxP[1])) / txP[1] : std::nan("");
    double U = g_availReg > 0 ? g_usedReg / g_availReg : std::nan("");
    bool mo = sched == "MULTIOBJ";
    std::ostringstream id;
    id << scenario << "|" << sched << "|" << seed;
    if (header)
        std::cout << "ROW,run_id,scenario,sched,wC,wF,wD,wQ,seed,numUes,T_mbps,D_ms,D_p95_ms,D_p99_ms,F_raw,F_norm,PLR,PLR_hard,U,"
                     "D_embb_ms,D_urllc_ms,D_mmtc_ms,D_urllc_p99_ms,urllc_viol,T_embb,T_urllc,T_mmtc,pkts_tx,pkts_rx,fill\n";
    std::cout << "ROW," << id.str() << "," << scenario << "," << sched << "," << (mo ? wC : NAN) << "," << (mo ? wF : NAN) << ","
              << (mo ? wD : NAN) << "," << (mo ? wQ : NAN) << "," << seed << "," << numUes << "," << T << "," << mean(3) << ","
              << quant(3, .95) << "," << quant(3, .99) << "," << Jain(thr) << "," << Jain(thrN) << "," << plr << "," << plr << ","
              << U << "," << mean(0) << "," << mean(1) << "," << mean(2) << "," << quant(1, .99) << "," << viol << "," << thrCls[0]
              << "," << thrCls[1] << "," << thrCls[2] << "," << txP[3] << "," << rxP[3] << ",nan\n";
    const char* cn[4] = {"embb", "urllc", "mmtc", "all"};
    for (int i : {3, 0, 1, 2})
        for (int q = 0; q <= 20; ++q)
            std::cout << "Q," << cn[i] << "," << q / 20.0 << "," << quant(i, q / 20.0) << "\n";
    Simulator::Destroy();
    return 0;
}
