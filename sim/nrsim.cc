// nrsim.cc -- STAND-IN slot-level NR downlink scheduler simulator (NOT ns-3 / NOT 5G-LENA).
//
// Purpose: exercise the multi-objective scheduler logic, the weight-tuning loop and the analysis
// pipeline when the real ns-3 + 5G-LENA stack is unavailable. Results from this program are
// results of THIS simplified model only and must not be reported as 5G-LENA results.
//
// Model summary (see README for the full list of simplifications):
//  * one gNB, OFDMA-style DL, numerology 1 (0.5 ms slot), 100 MHz, 17 RBGs of 16 PRBs
//  * 3GPP UMi/UMa path loss + shadowing, wideband Rayleigh fading (Gauss-Markov), no interference
//  * wideband CQI reported every 10 slots (CQI-table-1 efficiencies), logistic BLER, HARQ (chase, 4 tx)
//  * UDP-like CBR traffic per UE: eMBB (5QI 8), URLLC (5QI 82), mMTC (5QI 9); RLC queue with tail drop
//  * schedulers: RR, MR, PF, QOS (approximation of nr's QoS scheduler), MULTIOBJ (the project's score)
// Identical traffic + channel for all schedulers: traffic/channel RNG streams depend only on (seed, ue).
//
// Output (stdout):  "ROW,<csv header>" / "ROW,<values>"  and  "Q,<class>,<quantile>,<delay_ms>" lines.

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <random>
#include <string>
#include <vector>
using namespace std;

static const double CQI_EFF[15] = {0.1523, 0.2344, 0.3770, 0.6016, 0.8770, 1.1758, 1.4766, 1.9141,
                                   2.4063, 2.7305, 3.3223, 3.9023, 4.5234, 5.1152, 5.5547};
static const double CQI_SINR[15] = {-6.7, -4.7, -2.3, 0.2, 2.4, 4.3, 5.9, 8.1,
                                    10.3, 11.7, 14.1, 16.3, 18.7, 21.0, 22.7};

// ---- cell constants
static const double SLOT_S = 0.0005;     // numerology 1
static const int NRBG = 17;              // 17 x 16 PRB = 272 PRB (100 MHz, 30 kHz SCS)
static const double RE_PER_RBG = 16 * 12 * 13 * 0.9; // 13 data symbols, 10% overhead
static const double FC_GHZ = 3.5;
static const double BW_HZ = 100e6;
static const double NOISE_DBM = -174 + 10 * log10(BW_HZ) + 9.0; // NF 9 dB

// ---- traffic classes: 0 eMBB (5QI 8), 1 URLLC (5QI 82), 2 mMTC (5QI 9)
struct ClsDef { const char* name; int fiveqi; int prio; double pdbMs; int pktBytes; double intervalS; };
static ClsDef CLS[3] = {
    {"embb", 8, 80, 300.0, 1200, 1200 * 8 / 20e6},   // 20 Mbps CBR
    {"urllc", 82, 19, 10.0, 200, 0.001},             // 1.6 Mbps, PDB 10 ms
    {"mmtc", 9, 90, 300.0, 100, 0.010}               // 80 kbps
};

struct Pkt { double t; int size; int unsent; int undeliv; bool lost; };
struct TB { vector<pair<int, int>> seg; int bytes; int attempts; long ready; };

struct UE {
    int cls = 0;
    double x = 0, y = 0, vx = 0, vy = 0, shadow = 0, baseDb = 0, sinrDb = 0, rho = 0.999;
    complex<double> h{1, 0};
    int cqi = 0;
    double nextArr = 0;
    vector<Pkt> pk; size_t head = 0; long qBytes = 0;
    deque<TB> retx;
    double rbar = 0;  // EMA of delivered bytes per slot
    mt19937_64 rngCh, rngTr, rngB;
    // stats (post warm-up)
    double delivBytes = 0, offeredBytes = 0;
    long arrived = 0, delivered = 0, lostPk = 0, urllcLate = 0;
};

struct Args {
    string sched = "PF", traffic = "mixed", channel = "uma_nlos", scenario = "adhoc";
    int numUes = 20, seed = 1;
    double wC = .25, wF = .25, wD = .25, wQ = .25;
    double simTime = 3.0, warmup = 1.0, dmin = 35, dmax = 300, speed = 1.0, txDbm = 30, bfGain = 10;
    double pfWindow = 100;  // slots
    int header = 0;
};

static double plDb(const string& ch, double d2, double hBS, double hUT = 1.5) {
    double d3 = sqrt(d2 * d2 + (hBS - hUT) * (hBS - hUT)), lf = log10(FC_GHZ);
    double los, nlos;
    if (ch.rfind("umi", 0) == 0) {
        los = 32.4 + 21 * log10(d3) + 20 * lf;
        nlos = max(los, 35.3 * log10(d3) + 22.4 + 21.3 * lf - 0.3 * (hUT - 1.5));
    } else {
        los = 28.0 + 22 * log10(d3) + 20 * lf;
        nlos = max(los, 13.54 + 39.08 * log10(d3) + 20 * lf - 0.6 * (hUT - 1.5));
    }
    return (ch.find("_los") != string::npos) ? los : nlos;
}
static double shadowSigma(const string& ch) {
    if (ch == "umi_los" || ch == "uma_los") return 4.0;
    return ch == "umi_nlos" ? 7.82 : 6.0;
}
static int cqiFromSinr(double s) {
    int c = 0;
    for (int i = 0; i < 15; i++) if (s >= CQI_SINR[i]) c = i + 1;
    return c;
}
static double bler(double sinr, int cqi) {  // 10% at the CQI threshold
    double m = sinr - CQI_SINR[cqi - 1];
    return 1.0 / (1.0 + exp(1.5 * m + log(9.0)));
}
static double jain(const vector<double>& x) {
    double s = 0, s2 = 0; int n = 0;
    for (double v : x) { s += v; s2 += v * v; n++; }
    return (n == 0 || s2 <= 0) ? 1.0 : s * s / (n * s2);
}
static double quant(vector<double>& v, double q) {  // q in [0,1], v sorted
    if (v.empty()) return NAN;
    double pos = q * (v.size() - 1); size_t i = (size_t)floor(pos);
    double f = pos - i; return i + 1 < v.size() ? v[i] * (1 - f) + v[i + 1] * f : v[i];
}

int main(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; i++) {
        string k = argv[i];
        auto nx = [&]() { return string(argv[++i]); };
        if (k == "--sched") a.sched = nx();
        else if (k == "--traffic") a.traffic = nx();
        else if (k == "--channel") a.channel = nx();
        else if (k == "--scenario") a.scenario = nx();
        else if (k == "--numUes") a.numUes = stoi(nx());
        else if (k == "--seed") a.seed = stoi(nx());
        else if (k == "--wC") a.wC = stod(nx());
        else if (k == "--wF") a.wF = stod(nx());
        else if (k == "--wD") a.wD = stod(nx());
        else if (k == "--wQ") a.wQ = stod(nx());
        else if (k == "--simTime") a.simTime = stod(nx());
        else if (k == "--warmup") a.warmup = stod(nx());
        else if (k == "--dmin") a.dmin = stod(nx());
        else if (k == "--dmax") a.dmax = stod(nx());
        else if (k == "--speed") a.speed = stod(nx());
        else if (k == "--pfWindow") a.pfWindow = stod(nx());
        else if (k == "--header") a.header = 1;
        else { fprintf(stderr, "unknown arg %s\n", k.c_str()); return 2; }
    }
    // weights: validate / normalise (abort on nonsense)
    double ws = a.wC + a.wF + a.wD + a.wQ;
    if (a.wC < 0 || a.wF < 0 || a.wD < 0 || a.wQ < 0 || ws <= 0) { fprintf(stderr, "bad weights\n"); return 2; }
    if (fabs(ws - 1.0) > 1e-6) { a.wC /= ws; a.wF /= ws; a.wD /= ws; a.wQ /= ws; }

    const int N = a.numUes;
    vector<UE> ue(N);
    // class assignment (deterministic given N and traffic)
    for (int u = 0; u < N; u++) {
        if (a.traffic == "embb") ue[u].cls = 0;
        else if (a.traffic == "urllc") ue[u].cls = 1;
        else if (a.traffic == "mmtc") ue[u].cls = 2;
        else { int m = u % 4; ue[u].cls = (m < 2) ? 0 : (m == 2 ? 1 : 2); } // 50/25/25
    }
    // 5QI priority range over classes present (for Q_i)
    int pmin = 1000, pmax = -1;
    for (auto& e : ue) { pmin = min(pmin, CLS[e.cls].prio); pmax = max(pmax, CLS[e.cls].prio); }

    const double hBS = (a.channel.rfind("umi", 0) == 0) ? 10.0 : 25.0;
    const double ptx = a.txDbm + a.bfGain;
    const double fd = a.speed * FC_GHZ * 1e9 / 3e8;
    double rho = (a.speed < 0.01) ? 0.9999 : std::cyl_bessel_j(0.0, 2 * M_PI * fd * SLOT_S);
    rho = min(0.99999, max(0.0, rho));

    for (int u = 0; u < N; u++) {
        UE& e = ue[u];
        uint64_t base = 1000003ULL * (uint64_t)a.seed + 7919ULL * (uint64_t)u;
        e.rngCh.seed(base + 1); e.rngTr.seed(base + 2); e.rngB.seed(base + 3 + (uint64_t)(a.sched.size()) * 0);
        uniform_real_distribution<double> U01(0, 1);
        normal_distribution<double> G(0, 1);
        double r = sqrt(U01(e.rngCh) * (a.dmax * a.dmax - a.dmin * a.dmin) + a.dmin * a.dmin);
        double th = 2 * M_PI * U01(e.rngCh);
        e.x = r * cos(th); e.y = r * sin(th);
        double dir = 2 * M_PI * U01(e.rngCh);
        e.vx = a.speed * cos(dir); e.vy = a.speed * sin(dir);
        e.shadow = G(e.rngCh) * shadowSigma(a.channel);
        e.h = complex<double>(G(e.rngCh), G(e.rngCh)) * sqrt(0.5);
        e.rho = rho;
        e.baseDb = ptx - plDb(a.channel, max(10.0, r), hBS) + e.shadow - NOISE_DBM;
        e.nextArr = U01(e.rngTr) * CLS[e.cls].intervalS;
    }
    // BLER RNG: separate stream per UE; decorrelate from scheduler choice is NOT needed.

    const long nSlots = (long)llround(a.simTime / SLOT_S);
    const long warmSlot = (long)llround(a.warmup / SLOT_S);
    const double QCAP = 500e3;  // RLC buffer, bytes
    vector<double> dAll, dCls[3];
    long arrCls[3] = {0, 0, 0}, delCls[3] = {0, 0, 0}, lostCls[3] = {0, 0, 0}, urllcViol = 0;
    double rbgUsed = 0, rbgTot = 0, capAlloc = 0, capSent = 0;
    vector<int> asgRbg(N);
    vector<double> asgBytes(N), dem(N);
    vector<char> cand(N);
    vector<double> rate(N);

    for (long s = 0; s < nSlots; s++) {
        double t = s * SLOT_S;
        bool meas = s >= warmSlot;
        // ---- arrivals, channel, CQI
        for (int u = 0; u < N; u++) {
            UE& e = ue[u];
            const ClsDef& c = CLS[e.cls];
            while (e.nextArr <= t) {
                if (e.nextArr >= a.warmup) { arrCls[e.cls]++; e.arrived++; e.offeredBytes += c.pktBytes; }
                if (e.qBytes + c.pktBytes <= QCAP) {
                    e.pk.push_back({e.nextArr, c.pktBytes, c.pktBytes, c.pktBytes, false});
                    e.qBytes += c.pktBytes;
                } else if (e.nextArr >= a.warmup) { lostCls[e.cls]++; e.lostPk++; }
                e.nextArr += c.intervalS;
            }
            normal_distribution<double> G(0, 1);
            e.h = e.rho * e.h + sqrt(1 - e.rho * e.rho) * complex<double>(G(e.rngCh), G(e.rngCh)) * sqrt(0.5);
            if (a.speed > 0.01 && s % 20 == 0) {  // movement: recompute mean SINR
                e.x += e.vx * SLOT_S * 20; e.y += e.vy * SLOT_S * 20;
                double d = max(10.0, sqrt(e.x * e.x + e.y * e.y));
                e.baseDb = ptx - plDb(a.channel, d, hBS) + e.shadow - NOISE_DBM;
            }
            e.sinrDb = e.baseDb + 10 * log10(max(1e-6, norm(e.h)));
            if ((s + u) % 10 == 0 || s == 0) e.cqi = cqiFromSinr(e.sinrDb);
            // discard timer (5 x PDB) on untouched head packets
            double disc = 5 * c.pdbMs / 1000.0;
            while (e.head < e.pk.size()) {
                Pkt& p = e.pk[e.head];
                if (p.unsent == 0) { e.head++; continue; }
                if (p.unsent == p.size && t - p.t > disc) {
                    p.lost = true; e.qBytes -= p.unsent; p.unsent = 0; p.undeliv = 0; e.head++;
                    if (p.t >= a.warmup) { lostCls[e.cls]++; e.lostPk++; }
                    continue;
                }
                break;
            }
        }
        // ---- demand
        double maxRbar = 0;
        for (int u = 0; u < N; u++) {
            UE& e = ue[u];
            double rb = 0;
            for (auto& tb : e.retx) if (tb.ready <= s) rb += tb.bytes;
            dem[u] = e.qBytes + rb;
            asgRbg[u] = 0; asgBytes[u] = 0;
            rate[u] = e.cqi > 0 ? CQI_EFF[e.cqi - 1] * RE_PER_RBG / 8.0 : 0.0;  // bytes per RBG
            cand[u] = (dem[u] > 0 && e.cqi > 0);
            maxRbar = max(maxRbar, e.rbar);
        }
        // ---- RBG loop
        const int S = (a.sched == "RR") ? 0 : a.sched == "MR" ? 1 : a.sched == "PF" ? 2 : a.sched == "QOS" ? 3 : 4;
        for (int g = 0; g < NRBG; g++) {
            int best = -1; double bs = -1e300;
            for (int u = 0; u < N; u++) {
                if (!cand[u]) continue;
                UE& e = ue[u];
                double sc;
                double rtil = e.rbar + asgBytes[u] / a.pfWindow;  // in-slot update of avg throughput
                switch (S) {
                    case 0: sc = -asgRbg[u] - 1e-3 * (double)((u - (int)(s % N) + N) % N) / N; break;
                    case 1: sc = rate[u]; break;
                    case 2: sc = rate[u] / (rtil + 1.0); break;
                    case 3: {
                        double pf = rate[u] / (rtil + 1.0);
                        if (e.cls == 1) {  // delay-critical GBR first, most urgent first
                            double hol = e.head < e.pk.size() ? t - e.pk[e.head].t : 0.0;
                            sc = 1e9 + hol / (CLS[1].pdbMs / 1000.0) * 1e3 + pf;
                        } else sc = pf;
                        break;
                    }
                    default: {
                        double C = CQI_EFF[e.cqi - 1] / CQI_EFF[14];
                        double mx = maxRbar;
                        double phi = (mx > 1e-9) ? 1.0 - min(1.0, rtil / max(mx, rtil)) : 1.0;
                        double hol = e.head < e.pk.size() ? t - e.pk[e.head].t : 0.0;
                        double tau = min(1.0, hol / (CLS[e.cls].pdbMs / 1000.0));
                        double Q = (pmax > pmin) ? double(pmax - CLS[e.cls].prio) / double(pmax - pmin) : 0.0;
                        sc = a.wC * C + a.wF * phi + a.wD * tau + a.wQ * Q;
                    }
                }
                sc -= 1e-9 * u;  // deterministic tie-break
                if (sc > bs) { bs = sc; best = u; }
            }
            if (best < 0) break;
            asgRbg[best]++; asgBytes[best] += rate[best];
            if (asgBytes[best] >= dem[best]) cand[best] = 0;
            if (meas) rbgUsed += 1;
        }
        if (meas) rbgTot += NRBG;
        // ---- transmission
        for (int u = 0; u < N; u++) {
            UE& e = ue[u];
            double servedNow = 0;
            if (asgBytes[u] > 0 && e.cqi > 0) {
                double c = asgBytes[u];
                double b0 = bler(e.sinrDb, e.cqi);
                uniform_real_distribution<double> U01(0, 1);
                if (meas) capAlloc += asgBytes[u];
                auto attempt = [&](TB& tb) -> bool {
                    if (meas) capSent += tb.bytes;
                    double pb = b0 * pow(0.25, tb.attempts);
                    tb.attempts++;
                    bool ok = U01(e.rngB) >= pb;
                    if (ok) {
                        for (auto& sg : tb.seg) {
                            Pkt& p = e.pk[sg.first];
                            p.undeliv -= sg.second;
                            if (t >= 0 && sg.second > 0 && s >= warmSlot) { e.delivBytes += sg.second; servedNow += sg.second; }
                            else servedNow += sg.second;
                            if (p.undeliv == 0 && !p.lost) {
                                double d = t + SLOT_S - p.t;
                                if (p.t >= a.warmup) {
                                    int k = e.cls; delCls[k]++; e.delivered++;
                                    dAll.push_back(d * 1e3); dCls[k].push_back(d * 1e3);
                                    if (k == 1 && d * 1e3 > CLS[1].pdbMs) { urllcViol++; e.urllcLate++; }
                                }
                            }
                        }
                        return true;
                    }
                    if (tb.attempts >= 4) {
                        for (auto& sg : tb.seg) {
                            Pkt& p = e.pk[sg.first];
                            if (!p.lost && p.undeliv > 0) {
                                p.lost = true;
                                if (p.t >= a.warmup) { lostCls[e.cls]++; e.lostPk++; }
                            }
                        }
                        return true;  // finished (failed)
                    }
                    tb.ready = s + 8;
                    return false;
                };
                while (!e.retx.empty() && e.retx.front().ready <= s && c > 0) {
                    TB tb = e.retx.front(); e.retx.pop_front();
                    c -= tb.bytes;
                    if (!attempt(tb)) e.retx.push_back(tb);
                }
                if (c > 0 && e.qBytes > 0) {
                    long need = (long)min<double>(c, (double)e.qBytes);
                    TB tb; tb.bytes = 0; tb.attempts = 0; tb.ready = 0;
                    while (need > 0 && e.head < e.pk.size()) {
                        Pkt& p = e.pk[e.head];
                        if (p.unsent == 0) { e.head++; continue; }
                        int take = (int)min<long>(need, p.unsent);
                        p.unsent -= take; e.qBytes -= take; need -= take; tb.bytes += take;
                        tb.seg.push_back({(int)e.head, take});
                        if (p.unsent == 0) e.head++;
                    }
                    if (tb.bytes > 0 && !attempt(tb)) e.retx.push_back(tb);
                }
            }
            e.rbar = (1.0 - 1.0 / a.pfWindow) * e.rbar + servedNow / a.pfWindow;
        }
    }
    // ---- metrics
    const double Tm = a.simTime - a.warmup;
    vector<double> thr, thrN, thrCls(3, 0.0);
    for (auto& e : ue) {
        double x = e.delivBytes * 8 / Tm / 1e6;
        thr.push_back(x); thrCls[e.cls] += x;
        double off = e.offeredBytes * 8 / Tm / 1e6;
        thrN.push_back(off > 0 ? min(1.0, x / off) : 1.0);
    }
    double T = 0; for (double v : thr) T += v;
    auto mean = [](vector<double>& v) { double s = 0; for (double x : v) s += x; return v.empty() ? NAN : s / v.size(); };
    for (auto* v : {&dAll, &dCls[0], &dCls[1], &dCls[2]}) sort(v->begin(), v->end());
    long arrAll = arrCls[0] + arrCls[1] + arrCls[2];
    long delAll = delCls[0] + delCls[1] + delCls[2];
    // PLR (FlowMonitor semantics): tx - rx over packets sent post-warm-up (in-flight at end counts as lost)
    double plr = arrAll > 0 ? 1.0 - double(delAll) / arrAll : 0.0;
    double plrHard = arrAll > 0 ? double(lostCls[0] + lostCls[1] + lostCls[2]) / arrAll : 0.0;
    double viol = arrCls[1] > 0 ? double(urllcViol + (arrCls[1] - delCls[1])) / arrCls[1] : NAN; // late or undelivered
    double U = rbgTot > 0 ? rbgUsed / rbgTot : 0;
    string runId = a.scenario + "|" + a.sched + "|" + to_string(a.wC).substr(0, 4) + ";" + to_string(a.wF).substr(0, 4) + ";" +
                   to_string(a.wD).substr(0, 4) + ";" + to_string(a.wQ).substr(0, 4) + "|" + to_string(a.seed);
    const char* hdr = "run_id,scenario,sched,wC,wF,wD,wQ,seed,numUes,T_mbps,D_ms,D_p95_ms,D_p99_ms,F_raw,F_norm,PLR,PLR_hard,U,"
                      "D_embb_ms,D_urllc_ms,D_mmtc_ms,D_urllc_p99_ms,urllc_viol,T_embb,T_urllc,T_mmtc,pkts_tx,pkts_rx,fill";
    if (a.header) printf("ROW,%s\n", hdr);
    bool mo = (a.sched == "MULTIOBJ");
    printf("ROW,%s,%s,%s,%.4f,%.4f,%.4f,%.4f,%d,%d,%.4f,%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%.5f,%.5f,%.4f,%.4f,%.4f,%.4f,%.5f,%.4f,%.4f,%.4f,%ld,%ld,%.5f\n",
           runId.c_str(), a.scenario.c_str(), a.sched.c_str(), mo ? a.wC : NAN, mo ? a.wF : NAN, mo ? a.wD : NAN, mo ? a.wQ : NAN,
           a.seed, N, T, mean(dAll), quant(dAll, .95), quant(dAll, .99), jain(thr), jain(thrN), plr, plrHard, U,
           mean(dCls[0]), mean(dCls[1]), mean(dCls[2]), quant(dCls[1], .99), viol, thrCls[0], thrCls[1], thrCls[2], arrAll, delAll, capAlloc > 0 ? min(1.0, capSent / capAlloc) : 0.0);
    const char* cn[4] = {"all", "embb", "urllc", "mmtc"};
    vector<double>* dv[4] = {&dAll, &dCls[0], &dCls[1], &dCls[2]};
    for (int c = 0; c < 4; c++)
        if (!dv[c]->empty())
            for (int q = 0; q <= 20; q++) printf("Q,%s,%.2f,%.4f\n", cn[c], q / 20.0, quant(*dv[c], q / 20.0));
    return 0;
}
