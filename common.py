"""Shared helpers: weight simplex, objective J, file layout."""
import itertools
import numpy as np
import pandas as pd

BASELINES = ["RR", "MR", "PF", "QOS"]
J_SETS = {  # (alpha, beta, gamma, delta) for T, F, 1-D, U
    "default":     (0.3, 0.3, 0.3, 0.1),
    "tput_heavy":  (0.6, 0.15, 0.15, 0.1),
    "fair_heavy":  (0.15, 0.6, 0.15, 0.1),
    "delay_heavy": (0.15, 0.15, 0.6, 0.1),
    "equal":       (0.25, 0.25, 0.25, 0.25),
}

def simplex(step):
    """All (wC,wF,wD,wQ) with entries multiples of `step` summing to 1."""
    n = int(round(1 / step))
    out = []
    for a in range(n + 1):
        for b in range(n + 1 - a):
            for c in range(n + 1 - a - b):
                d = n - a - b - c
                out.append(tuple(round(x * step, 4) for x in (a, b, c, d)))
    return out

def wkey(w):
    return "%.3f;%.3f;%.3f;%.3f" % tuple(w)

def run_id(scn, sched, w, seed):
    return "%s|%s|%s|%d" % (scn, sched, wkey(w) if w is not None else "-", seed)

def minmax(x):
    lo, hi = np.nanmin(x), np.nanmax(x)
    return (x - lo) / (hi - lo) if hi > lo else np.zeros_like(x)

def add_J(df, alpha=0.3, beta=0.3, gamma=0.3, delta=0.1, fairness="F_norm"):
    """J = alpha*T + beta*F + gamma*(1-D) + delta*U.
    T and D are min-max normalised over ALL runs (MULTIOBJ candidates + baselines) that share the same
    (scenario, seed); F (Jain index) and U (fraction of RBGs used) are already in [0,1]."""
    df = df.copy()
    df["T_n"] = df.groupby(["scenario", "seed"])["T_mbps"].transform(minmax)
    df["D_n"] = df.groupby(["scenario", "seed"])["D_ms"].transform(minmax)
    U = df["U"]
    if U.isna().all():          # e.g. utilisation trace unavailable in this ns-3 build: drop the term (documented in README)
        U = U.fillna(0.0)
    df["J"] = alpha * df["T_n"] + beta * df[fairness] + gamma * (1 - df["D_n"]) + delta * U.fillna(0.0)
    return df

def weight_cols(df):
    return df[["wC", "wF", "wD", "wQ"]].round(3).astype(str).agg(";".join, axis=1)
