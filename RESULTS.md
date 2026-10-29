# Results and discussion (DRAFT text for the report)

> **Provenance, read first.** Every number below comes from `sim/nrsim.cc`, a *stand-in* slot-level NR downlink
> simulator written because the 5G-LENA `nr` module could not be downloaded in the authoring environment
> (gitlab.com and nsnam.org were blocked). It is **not ns-3 and not 5G-LENA**. The scheduler logic (Eq. 1-5), the
> grid search (Eq. 7), the scenarios and the metrics are the project's; the radio/MAC model underneath is simplified
> (see README, "Model limitations"). Re-run the same pipeline on the real stack with `--backend ns3` and replace the
> numbers before putting them in the report. Run counts: 8,235 attempted, 8,235 succeeded, 0 failed
> (9 scenarios x 3 seeds x [286 grid points + 4 baselines] + 405 refinement runs at step 0.05).

## 1. Selected weights

The coarse-to-fine search (step 0.1 over the whole simplex, then step 0.05 within L1 distance 0.2 of the best point)
selected

**w\* = (wC, wF, wD, wQ) = (0.65, 0.00, 0.35, 0.00)**, mean J = 0.800 (std of J across scenarios 0.094).

The next four best points are (0.60,0,0.40,0), (0.70,0,0.30,0), (0.80,0,0.20,0), (0.60,0,0.35,0.05) with J within 0.003,
so the optimum is a broad ridge in the channel/delay plane rather than a sharp peak (Fig. `fig_heatmap_J`).

Observations that need to be stated honestly in the report:
* The tuning drove **wF and wQ to (almost) zero**. In this model the delay-urgency term already acts as an
  anti-starvation mechanism (a starved UE's head-of-line delay grows), and J rewards throughput and low *packet-averaged*
  delay, both of which are dominated by eMBB traffic. The fairness and QoS terms are therefore not "wrong"; the
  objective J as specified simply does not pay for them.
* The best weights are **scenario dependent** (`tuning_summary.csv`): channel-dominated in cell-edge (0.9,0,0,0.1),
  near-LOS (0.8,0,0.2,0), low-density mixed (0.8,0,0.1,0.1); but delay/QoS-dominated in URLLC-only and mMTC-only
  (0.1,0,0.5,0.4). A single w\* loses J relative to the per-scenario optimum (e.g. S5: 0.864 vs 0.982; S7: 0.674 vs
  0.779), which supports the report's motivation for adaptive weights as future work.
* **Sensitivity to (alpha,beta,gamma,delta)** (`weight_sensitivity.csv`): the default and the "equal" set select the same
  w\*; throughput-heavy gives (0.60,0,0.40,0); fairness-heavy gives (0.50,0.05,0.45,0); delay-heavy gives
  (1,0,0,0), i.e. Max-Rate behaviour. So w\* is stable for balanced objectives but not when delay is weighted
  far above the rest, because the packet-average delay is minimised by simply being efficient.

## 2. Comparison with baselines (w\* fixed, mean over 9 scenarios x 3 seeds)

| Scheduler | T (Mbps) | D (ms) | Jain F (demand-norm.) | Jain F (raw) | PLR | U | URLLC late/lost |
|---|---|---|---|---|---|---|---|
| RR | 138.4 | 230.4 | 0.798 | 0.590 | 0.442 | 0.92 | 0.374 |
| MR | 170.3 | **51.1** | 0.643 | 0.456 | 0.436 | 0.92 | 0.588 |
| PF | 131.7 | 238.9 | 0.790 | 0.595 | 0.467 | 0.92 | 0.308 |
| QoS (approx.) | 114.0 | 196.4 | 0.770 | 0.617 | 0.497 | 0.92 | **0.232** |
| **MULTIOBJ (w\*)** | **201.4** | 119.4 | 0.760 | 0.532 | **0.352** | 0.92 | 0.444 |
| MULTIOBJ per-scenario best (oracle) | 199.6 | 85.3 | 0.758 | 0.529 | 0.339 | 0.92 | 0.400 |

Relative to the baselines, MULTIOBJ(w\*) changes: throughput +45.6% (RR), +18.3% (MR), +52.9% (PF), +76.7% (QoS);
mean delay -48% (RR), -50% (PF), -39% (QoS) but **+134% vs MR**; packet loss -20% to -29% vs all baselines;
Jain index (demand-normalised) -4.8% vs RR, -3.8% vs PF, +18.3% vs MR. Across the nine scenarios MULTIOBJ has the best
throughput in 7, the lowest PLR in 7, the lowest mean delay in 0 (MR wins 8), the best fairness in 3 (tied with RR and PF),
and QoS is best on URLLC deadline violations in 5 of 7 scenarios that contain URLLC traffic.

Where it wins / loses, candidly:
* **Wins:** aggregate throughput and loss, and a better fairness-throughput-delay compromise than MR. In the near-LOS
  scenario it almost eliminates loss (0.4% vs 19-35% for the baselines) at the highest throughput.
* **Loses on delay to MR**, which is the delay reference here. **Loses on URLLC to QoS and PF** in the mixed-traffic
  scenarios (e.g. S2: URLLC late/lost 0.50 vs 0.04 for QoS), because w\* (wQ = 0) gives no priority to the 5QI-82 flow
  and J counts delay per packet, which eMBB packets dominate. A URLLC-aware objective (class-weighted or
  deadline-violation term in J) would select different weights; this is the most important caveat for the report.
* **MR does not give the maximum throughput in this model.** MR ranks UEs by CQI rate and ignores buffer size, so at RBG
  granularity it hands whole RBGs to UEs with a few hundred queued bytes. "Fill" (bytes sent / capacity of allocated RBGs)
  is 0.65 for MR versus 0.79 for MULTIOBJ. Because U counts allocated RBGs, U is ~1 for every scheduler (0.28 in the
  light mMTC-only scenario) and does not separate them, as the report anticipated.
* The `QOS` baseline is **my approximation** of the nr QoS scheduler (delay-critical flows first by HOL/PDB, others PF);
  on the real stack use the built-in `NrMacSchedulerOfdmaQos`.

## 3. Limitations (put these in the report)
* Stand-in simulator, not 5G-LENA: wideband flat fading, no interference, no TDD pattern, no frequency-selective
  scheduling, simplified HARQ/CQI/BLER, UDP-like CBR only, single cell, static or straight-line UEs.
* Weights were tuned and evaluated on the **same** scenarios and seeds (in-sample); 3 seeds only. A held-out scenario set is
  needed before claiming generalisation.
* Fairness in J is Jain's index over throughput **normalised by offered load** (`F_norm`); the raw-throughput Jain index of
  Eq. (10) is also reported (`F_raw`, `--fairness F_raw` in `analyze.py`) because raw Jain mostly measures traffic-mix
  heterogeneity in mixed scenarios.
* Short runs (5 s, 1 s warm-up discarded); packets still queued at the end count as lost, as in FlowMonitor.

## 4. Suggested captions
* Fig. A (`fig_heatmap_J.png`): *Mean objective J over the weight simplex (slices of constant wQ); star = selected w\*.*
* Fig. B (`fig_tradeoff.png`): *Throughput-fairness and throughput-delay trade-offs of all 301 weight settings and the four baselines; black line = Pareto front.*
* Fig. C (`fig_bars_per_scenario.png`, `fig_bars_avg.png`): *MULTIOBJ(w\*) versus RR/MR/PF/QoS per scenario (error bars: std over 3 seeds).*
* Fig. D (`fig_delay_cdf.png`): *Packet-delay CDFs per service class, medium-density mixed scenario.*
* Fig. E (`fig_scalability.png`), Fig. F (`fig_weight_sensitivity.png`), Fig. G (`fig_urllc_violation.png`).
* Table I (`tables/comparison_avg.tex`), Table II (`tables/multiobj_vs_baseline_pct.tex`).

Suggested paragraph: "Tuning over 301 weight combinations, nine scenarios and three seeds selected w\* = (0.65, 0, 0.35, 0).
At w\*, the proposed scheduler delivered the highest average throughput and lowest packet loss among the evaluated
schedulers, and reduced mean delay by 39-50% relative to RR, PF and QoS, while Max-Rate retained the lowest delay and
QoS the lowest URLLC deadline-violation ratio. These results were obtained with a simplified slot-level simulator and
should be confirmed in 5G-LENA." (Edit once real ns-3 numbers exist.)
