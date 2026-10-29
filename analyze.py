#!/usr/bin/env python3
"""Analysis of tuning + comparison runs -> tables, figures, tuning_summary.csv.

  python3 analyze.py --backend sim --mode full [--fairness F_norm|F_raw]
Reads  results/<backend>/raw/runs_<mode>.csv, quantiles_<mode>.csv
Writes results/<backend>/<mode>/{tuning_summary.csv, tables/*.csv|.tex, figs/*.png, summary.json}
"""
import argparse, json, os
import numpy as np, pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from common import BASELINES, J_SETS, add_J

HERE = os.path.dirname(os.path.abspath(__file__))
SCHEDS = ["RR", "MR", "PF", "QOS", "MULTIOBJ"]
# Okabe-Ito colour-blind-safe palette
COL = {"RR": "#999999", "MR": "#D55E00", "PF": "#0072B2", "QOS": "#009E73", "MULTIOBJ": "#CC79A7"}
plt.rcParams.update({"figure.dpi": 100, "savefig.dpi": 300, "font.size": 9, "axes.grid": True, "grid.alpha": .25,
                     "axes.spines.top": False, "axes.spines.right": False})

def wstr(w):
    return "(%.2f, %.2f, %.2f, %.2f)" % tuple(w)

def wtuple(df):
    """weight tuples encoded as strings (safe as pandas index keys)"""
    return [wstr(t) for t in zip(df.wC, df.wF, df.wD, df.wQ)]

def unstr(s):
    return tuple(float(x) for x in s.strip("()").split(","))

def best_weights(d, by=None):
    """d has J; returns mean J (over seeds [and scenarios]) per weight tuple, sorted."""
    m = d[d.sched == "MULTIOBJ"].copy()
    m["w"] = wtuple(m)
    g = m.groupby("w")["J"]
    s = pd.DataFrame({"J_mean": g.mean(), "J_std": g.std()}).sort_values("J_mean", ascending=False)
    return s

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--backend", default="sim"); ap.add_argument("--mode", default="full")
    ap.add_argument("--fairness", default="F_norm", choices=["F_norm", "F_raw"])
    a = ap.parse_args()
    raw = os.path.join(HERE, "results", a.backend, "raw")
    out = os.path.join(HERE, "results", a.backend, a.mode)
    for sub in ("tables", "figs"): os.makedirs(os.path.join(out, sub), exist_ok=True)
    runs = pd.read_csv(os.path.join(raw, "runs_%s.csv" % a.mode))
    qs = pd.read_csv(os.path.join(raw, "quantiles_%s.csv" % a.mode))
    scn_order = list(dict.fromkeys(runs.scenario))
    summ = {"backend": a.backend, "mode": a.mode, "fairness_metric": a.fairness, "n_runs": int(len(runs))}

    # ---------------- validation checks
    chk = runs.groupby(["scenario", "seed"])["pkts_tx"].nunique()
    summ["identical_traffic_across_schedulers"] = bool((chk == 1).all())
    summ["nan_in_metrics"] = bool(runs[["T_mbps", "D_ms", "F_raw", "F_norm", "PLR", "U"]].isna().any().any())

    d = add_J(runs, *J_SETS["default"], fairness=a.fairness)
    nseeds = d.seed.nunique()
    # ---------------- tuning summary
    bw = best_weights(d)
    wstar_s = bw.index[0]; wstar = unstr(wstar_s)
    # per-scenario best
    rows = []
    for sc in scn_order:
        b = best_weights(d[d.scenario == sc])
        w = unstr(b.index[0])
        rows.append({"scenario": sc, "wC": w[0], "wF": w[1], "wD": w[2], "wQ": w[3], "J_mean": b.iloc[0].J_mean,
                     "J_at_global_wstar": b.loc[wstar_s, "J_mean"]})
    ts = pd.DataFrame(rows)
    # per-scenario mean J of w* to quantify its std across scenarios
    perscn = d[(d.sched == "MULTIOBJ")].copy(); perscn["w"] = wtuple(perscn)
    jps = perscn[perscn.w == wstar_s].groupby("scenario")["J"].mean()
    overall = {"scenario": "OVERALL_w*", "wC": wstar[0], "wF": wstar[1], "wD": wstar[2], "wQ": wstar[3],
               "J_mean": bw.iloc[0].J_mean, "J_at_global_wstar": bw.iloc[0].J_mean}
    ts = pd.concat([ts, pd.DataFrame([overall])], ignore_index=True)
    ts["J_std_across_scenarios_at_wstar"] = np.nan
    ts.loc[ts.scenario == "OVERALL_w*", "J_std_across_scenarios_at_wstar"] = jps.std()
    ts.to_csv(os.path.join(out, "tuning_summary.csv"), index=False)
    top = bw.head(10).reset_index(); pass
    top.to_csv(os.path.join(out, "tables", "top10_weights.csv"), index=False)
    summ["w_star"] = list(wstar); summ["J_star"] = float(bw.iloc[0].J_mean)
    summ["n_weight_points"] = int(len(bw)); summ["n_seeds"] = int(nseeds)
    summ["top5"] = [(k, float(v)) for k, v in bw.J_mean.head(5).items()]

    # ---------------- sensitivity over (alpha,beta,gamma,delta)
    sens = []
    for name, (al, be, ga, de) in J_SETS.items():
        dd = add_J(runs, al, be, ga, de, fairness=a.fairness)
        b = best_weights(dd)
        w = unstr(b.index[0])
        sens.append({"J_set": name, "alpha": al, "beta": be, "gamma": ga, "delta": de, "wC": w[0], "wF": w[1],
                     "wD": w[2], "wQ": w[3], "J_mean": b.iloc[0].J_mean,
                     "J_of_default_wstar": b.loc[wstar_s, "J_mean"]})
    sens = pd.DataFrame(sens); sens.to_csv(os.path.join(out, "tables", "weight_sensitivity.csv"), index=False)
    summ["sensitivity"] = sens[["J_set", "wC", "wF", "wD", "wQ"]].values.tolist()

    # ---------------- selected scheduler table (w*), plus per-scenario oracle
    m = d[d.sched == "MULTIOBJ"].copy(); m["w"] = wtuple(m)
    star = m[m.w == wstar_s].copy(); star["sched"] = "MULTIOBJ"
    orc = []
    for sc in scn_order:
        w = wstr(ts.loc[ts.scenario == sc, ["wC", "wF", "wD", "wQ"]].iloc[0])
        t = m[(m.scenario == sc) & (m.w == w)].copy(); t["sched"] = "MULTIOBJ-oracle"; orc.append(t)
    orc = pd.concat(orc)
    base = d[d.sched.isin(BASELINES)]
    cmp_all = pd.concat([base, star, orc], ignore_index=True)
    metrics = ["T_mbps", "D_ms", "D_p99_ms", "F_norm", "F_raw", "PLR", "U", "fill", "urllc_viol", "D_urllc_ms", "J"]
    per_scn = cmp_all.groupby(["scenario", "sched"])[metrics].mean().reset_index()
    per_scn.to_csv(os.path.join(out, "tables", "per_scenario_comparison.csv"), index=False)
    order = SCHEDS + ["MULTIOBJ-oracle"]
    # average over scenarios (each scenario weighted equally), seeds averaged first
    avg = per_scn.groupby("sched")[metrics].mean().reindex(order)
    avg.to_csv(os.path.join(out, "tables", "comparison_avg.csv"))
    imp = {}
    for b in BASELINES:
        r = {}
        for k in metrics[:-1]:
            r[k] = 100 * (avg.loc["MULTIOBJ", k] - avg.loc[b, k]) / avg.loc[b, k] if avg.loc[b, k] else np.nan
        imp[b] = r
    imp = pd.DataFrame(imp).T; imp.index.name = "baseline"
    imp.to_csv(os.path.join(out, "tables", "multiobj_vs_baseline_pct.csv"))
    with open(os.path.join(out, "tables", "comparison_avg.tex"), "w") as f:
        f.write(avg.round(3).to_latex(caption="Average over the nine scenarios (MULTIOBJ at $w^*$).", label="tab:cmp"))
    with open(os.path.join(out, "tables", "multiobj_vs_baseline_pct.tex"), "w") as f:
        f.write(imp.round(1).to_latex(caption="Relative change of MULTIOBJ vs. each baseline (\\%).", label="tab:pct"))
    # win/loss count per metric over scenarios
    better_high = {"T_mbps": True, "D_ms": False, "F_norm": True, "PLR": False, "U": True, "urllc_viol": False}
    wl = []
    for k, hi in better_high.items():
        pv = per_scn[per_scn.sched.isin(SCHEDS)].pivot(index="scenario", columns="sched", values=k).dropna(how="all")
        best = pv.idxmax(axis=1) if hi else pv.idxmin(axis=1)
        wl.append(best.value_counts().rename(k))
    pd.concat(wl, axis=1).fillna(0).astype(int).to_csv(os.path.join(out, "tables", "best_scheduler_counts.csv"))
    summ["avg"] = avg.round(4).reset_index().to_dict("records")
    summ["pct_vs_baseline"] = imp.round(2).reset_index().to_dict("records")

    # ================= FIGURES
    figd = os.path.join(out, "figs")
    # 1) heatmaps of mean J over (wC, wF) for fixed wQ
    ms = m.groupby(["wC", "wF", "wD", "wQ"])["J"].mean().reset_index()
    uq = np.sort(ms.wQ.round(3).unique())
    qvals = list(uq[np.unique(np.linspace(0, min(len(uq) - 1, 6), 4).round().astype(int))])  # up to 4 low-wQ slices
    fig, axs = plt.subplots(1, len(qvals), figsize=(3.3 * len(qvals), 3.4), sharey=True)
    axs = np.atleast_1d(axs)
    vmin, vmax = ms.J.min(), ms.J.max()
    hs = np.diff(np.sort(ms.wC.unique())).min() / 2
    for ax, q in zip(axs, qvals):
        sub = ms[np.isclose(ms.wQ, q)]
        if sub.empty: continue
        piv = sub.pivot_table(index="wF", columns="wC", values="J")
        im = ax.imshow(piv.values, origin="lower", aspect="auto", cmap="viridis", vmin=vmin, vmax=vmax,
                       extent=[piv.columns.min() - hs, piv.columns.max() + hs, piv.index.min() - hs, piv.index.max() + hs])
        ax.set_title("w_Q = %.2f" % q, fontsize=9); ax.set_xlabel("w_C"); ax.grid(False)
        if np.isclose(wstar[3], q):
            ax.plot(wstar[0], wstar[1], "r*", ms=12, label="w*")
            ax.legend(loc="upper right", fontsize=7)
    axs[0].set_ylabel("w_F"); fig.colorbar(im, ax=axs, label="mean J (all scenarios, seeds)")
    fig.suptitle("Objective J over the weight simplex", y=1.02); fig.savefig(os.path.join(figd, "fig_heatmap_J.png"), bbox_inches="tight"); plt.close(fig)

    # 2) trade-off scatter (scenario-averaged), colour = J, Pareto front highlighted
    allr = d.copy(); allr["w"] = [x if s == "MULTIOBJ" else "" for x, s in zip(wtuple(allr), allr.sched)]
    sc_avg = allr.groupby(["sched", "w"])[["T_n", "D_n", a.fairness, "J"]].mean().reset_index()
    fig, axs = plt.subplots(1, 2, figsize=(10, 4))
    for ax, (xk, yk, yl, hi) in zip(axs, [("T_n", a.fairness, "Fairness (Jain)", True), ("T_n", "D_n", "Normalised delay D", False)]):
        mo = sc_avg[sc_avg.sched == "MULTIOBJ"]
        sc = ax.scatter(mo[xk], mo[yk], c=mo.J, cmap="viridis", s=14, alpha=.85, label="MULTIOBJ weight points")
        pts = sc_avg[["T_n", yk]].values
        pareto = []
        for i, p in enumerate(pts):
            dom = False
            for j, q in enumerate(pts):
                if j != i and q[0] >= p[0] and ((q[1] >= p[1]) if hi else (q[1] <= p[1])) and (q[0] > p[0] or q[1] != p[1]):
                    dom = True; break
            if not dom: pareto.append(i)
        pf = sc_avg.iloc[pareto].sort_values(xk)
        ax.plot(pf[xk], pf[yk], "-", color="k", lw=1, alpha=.6, label="Pareto front")
        for s_ in BASELINES:
            r = sc_avg[sc_avg.sched == s_].iloc[0]; ax.scatter(r[xk], r[yk], marker="s", s=70, color=COL[s_], edgecolor="k", label=s_, zorder=5)
        r = sc_avg[(sc_avg.sched == "MULTIOBJ") & (sc_avg.w == wstar_s)].iloc[0]
        ax.scatter(r[xk], r[yk], marker="*", s=220, color=COL["MULTIOBJ"], edgecolor="k", label="MULTIOBJ w*", zorder=6)
        ax.set_xlabel("Normalised throughput T"); ax.set_ylabel(yl)
    axs[0].legend(fontsize=7, loc="lower left"); fig.colorbar(sc, ax=axs, label="J", shrink=.85)
    fig.suptitle("Throughput / fairness / delay trade-offs (mean over scenarios and seeds)")
    fig.savefig(os.path.join(figd, "fig_tradeoff.png"), bbox_inches="tight"); plt.close(fig)

    # 3) bars per scenario, 5 metrics, 5 schedulers
    cmp5 = cmp_all[cmp_all.sched.isin(SCHEDS)]
    mets = [("T_mbps", "Throughput T (Mbps)", False), ("D_ms", "Mean delay D (ms)", True), (a.fairness, "Jain fairness F", False),
            ("PLR", "Packet loss ratio", False), ("U", "Resource utilisation U", False)]
    fig, axs = plt.subplots(len(mets), 1, figsize=(11, 12), sharex=True)
    x = np.arange(len(scn_order)); wdt = .16
    for ax, (k, lab, lg) in zip(axs, mets):
        for i, s_ in enumerate(SCHEDS):
            g = cmp5[cmp5.sched == s_].groupby(["scenario", "seed"])[k].mean().reset_index()
            mu = g.groupby("scenario")[k].mean().reindex(scn_order); sd = g.groupby("scenario")[k].std().reindex(scn_order).fillna(0)
            ax.bar(x + (i - 2) * wdt, mu, wdt, yerr=sd if nseeds > 1 else None, color=COL[s_], label=s_ if s_ != "MULTIOBJ" else "MULTIOBJ (w*)", capsize=1.5)
        ax.set_ylabel(lab)
        if lg: ax.set_yscale("log")
    axs[0].legend(ncol=5, fontsize=8, loc="upper center", bbox_to_anchor=(.5, 1.25))
    axs[-1].set_xticks(x); axs[-1].set_xticklabels(scn_order, rotation=25, ha="right")
    fig.savefig(os.path.join(figd, "fig_bars_per_scenario.png"), bbox_inches="tight"); plt.close(fig)
    # averaged bars
    fig, axs = plt.subplots(1, 5, figsize=(14, 3))
    for ax, (k, lab, lg) in zip(axs, mets):
        vals = [per_scn[per_scn.sched == s_][k].mean() for s_ in SCHEDS]
        ax.bar(SCHEDS, vals, color=[COL[s_] for s_ in SCHEDS]); ax.set_title(lab, fontsize=9); ax.tick_params(axis="x", rotation=40)
        if lg: ax.set_yscale("log")
    fig.suptitle("Average over the nine scenarios"); fig.tight_layout(); fig.savefig(os.path.join(figd, "fig_bars_avg.png"), bbox_inches="tight"); plt.close(fig)

    # 4) delay CDFs from quantiles (mixed medium-density scenario, seed 1)
    def rid_of(sc, sched, seed):
        if sched == "MULTIOBJ":
            r = star[(star.scenario == sc) & (star.seed == seed)]
        else:
            r = base[(base.scenario == sc) & (base.sched == sched) & (base.seed == seed)]
        return r.run_id.iloc[0] if len(r) else None
    sc_cdf = "S2_med_mixed" if "S2_med_mixed" in scn_order else scn_order[0]
    seed0 = int(d.seed.min())
    fig, axs = plt.subplots(1, 4, figsize=(15, 3.4))
    for ax, cls in zip(axs, ["all", "embb", "urllc", "mmtc"]):
        for s_ in SCHEDS:
            rid = rid_of(sc_cdf, s_, seed0)
            if rid is None: continue
            q = qs[(qs.run_id == rid) & (qs.cls == cls)].sort_values("q")
            if len(q): ax.plot(q.delay_ms.clip(lower=1e-2), q.q, color=COL[s_], label=s_ if s_ != "MULTIOBJ" else "MULTIOBJ (w*)")
        ax.set_xscale("log"); ax.set_xlabel("packet delay (ms)"); ax.set_title(cls); ax.set_ylim(0, 1.01)
        if cls == "urllc": ax.axvline(10, color="k", ls=":", lw=1)
    axs[0].set_ylabel("CDF"); axs[0].legend(fontsize=7)
    fig.suptitle("Delay CDFs, %s (dotted line = URLLC PDB 10 ms)" % sc_cdf, y=1.03); fig.savefig(os.path.join(figd, "fig_delay_cdf.png"), bbox_inches="tight"); plt.close(fig)

    # 5) scalability (S1,S2,S3 mixed density)
    dens = [s for s in scn_order if "low" in s or "med" in s or "high" in s]
    if dens:
        sub = cmp5[cmp5.scenario.isin(dens)]
        fig, axs = plt.subplots(1, 5, figsize=(15, 3))
        for ax, (k, lab, lg) in zip(axs, mets):
            for s_ in SCHEDS:
                g = sub[sub.sched == s_].groupby("numUes")[k].mean()
                ax.plot(g.index, g.values, "o-", color=COL[s_], label=s_)
            ax.set_xlabel("number of UEs"); ax.set_title(lab, fontsize=9)
            if lg: ax.set_yscale("log")
        axs[0].legend(fontsize=7); fig.tight_layout(); fig.savefig(os.path.join(figd, "fig_scalability.png"), bbox_inches="tight"); plt.close(fig)

    # 6) sensitivity of w*
    fig, ax = plt.subplots(figsize=(7, 3.4))
    bottom = np.zeros(len(sens))
    for k, c in zip(["wC", "wF", "wD", "wQ"], ["#0072B2", "#009E73", "#D55E00", "#CC79A7"]):
        ax.bar(sens.J_set, sens[k], bottom=bottom, label=k, color=c); bottom += sens[k].values
    ax.set_ylabel("selected weight"); ax.set_title("w* under different (alpha, beta, gamma, delta)"); ax.legend(ncol=4, fontsize=8)
    ax.tick_params(axis="x", rotation=15); fig.savefig(os.path.join(figd, "fig_weight_sensitivity.png"), bbox_inches="tight"); plt.close(fig)

    # 7) URLLC deadline violation
    ur = [s for s in scn_order if per_scn[(per_scn.scenario == s)].urllc_viol.notna().all()]
    fig, ax = plt.subplots(figsize=(9, 3.4)); x = np.arange(len(ur))
    for i, s_ in enumerate(SCHEDS):
        v = per_scn[per_scn.sched == s_].set_index("scenario").reindex(ur).urllc_viol
        ax.bar(x + (i - 2) * wdt, v.values, wdt, color=COL[s_], label=s_)
    ax.set_xticks(x); ax.set_xticklabels(ur, rotation=25, ha="right"); ax.set_ylabel("URLLC packets late or lost (fraction)")
    ax.legend(ncol=5, fontsize=8); fig.savefig(os.path.join(figd, "fig_urllc_violation.png"), bbox_inches="tight"); plt.close(fig)

    json.dump(summ, open(os.path.join(out, "summary.json"), "w"), indent=1, default=str)
    print(json.dumps({k: summ[k] for k in ["n_runs", "w_star", "J_star", "n_weight_points", "identical_traffic_across_schedulers", "nan_in_metrics"]}, indent=1))
    print(avg.round(3).to_string())
    print(imp.round(1).to_string())
    print(sens[["J_set", "wC", "wF", "wD", "wQ"]].to_string())

if __name__ == "__main__":
    main()
