Partial results: ns-3 / 5G-LENA MultiObj scheduler, "quick" settings (3 s sim, 1 warm-up s, seed 1), weight grid step 0.2.
This run was stopped early; it is NOT the final dataset.

Contents of runs_*.csv (159 rows):
  S1_low_mixed   60 rows  = 4 baselines (RR, MR, PF, QOS) + 56 MultiObj weight vectors  (complete grid)
  S2_med_mixed   60 rows  = same, complete
  S3_high_mixed  31 rows  = PARTIAL, do not analyse
  S4_embb_only    8 rows  = PARTIAL, do not analyse

RELIABLE columns: T_mbps, D_ms (mean delay), F_raw, F_norm, PLR, U, per-class mean delay/throughput, pkts_tx/rx.
UNRELIABLE columns: D_p95_ms, D_p99_ms, D_urllc_p99_ms, urllc_viol, and everything in quantiles_*.csv.
  Reason: a bug in the scenario's histogram code (read past the end of the warm-up snapshot histogram) was fixed
  mid-run. 40/60 S1 rows have urllc_viol outside [0,1]. Tail metrics need re-running with the fixed binary.
  runs_quick_reliable_columns.csv is the same data with those four columns removed.
failures_quick_stale.log lists crashes from that same bug; those S1 runs were later retried and are present in the CSV.

Weights are (wC, wF, wD, wQ). C is based on DL MCS (so wC=1 reproduces nr's MR exactly; verified in a smoke test).
Single cell, one seed: indicative only.
