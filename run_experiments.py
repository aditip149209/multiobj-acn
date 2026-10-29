#!/usr/bin/env python3
"""Experiment driver: grid search over the weight simplex + baselines, parallel, resumable.

  python3 run_experiments.py --backend sim --mode smoke
  python3 run_experiments.py --backend sim --mode quick
  python3 run_experiments.py --backend sim --mode full      # includes the 0.05 refinement stage
  python3 run_experiments.py --backend ns3 --ns3dir ~/ns-3-dev --mode quick   # real 5G-LENA (needs setup.sh)

Raw output: results/<backend>/raw/runs_<mode>.csv  (one row per run) and quantiles_<mode>.csv (delay quantiles).
Re-running skips run_ids already present (resume). Failures are logged to failures_<mode>.log.
"""
import argparse, csv, os, subprocess, sys, time, multiprocessing as mp
import yaml
import pandas as pd
from common import BASELINES, simplex, run_id, add_J, wkey

HERE = os.path.dirname(os.path.abspath(__file__))

def build_cmd(a, sc, sched, w, seed, md):
    wv = w if w is not None else (0.25, 0.25, 0.25, 0.25)
    if a.backend == "sim":
        return [os.path.join(HERE, "sim", "nrsim"), "--header", "--scenario", sc["name"], "--sched", sched,
                "--numUes", str(sc["numUes"]), "--traffic", sc["traffic"], "--channel", sc["channel"],
                "--dmin", str(sc["dmin"]), "--dmax", str(sc["dmax"]), "--speed", str(sc["speed"]),
                "--wC", str(wv[0]), "--wF", str(wv[1]), "--wD", str(wv[2]), "--wQ", str(wv[3]),
                "--seed", str(seed), "--simTime", str(md["simTime"]), "--warmup", str(md["warmup"])]
    prog = ("nr-multiobj-scenario --header=1 --scenario=%s --sched=%s --numUes=%d --traffic=%s --channel=%s "
            "--dmin=%s --dmax=%s --speed=%s --wC=%s --wF=%s --wD=%s --wQ=%s --seed=%d --simTime=%s --warmup=%s"
            % (sc["name"], sched, sc["numUes"], sc["traffic"], sc["channel"], sc["dmin"], sc["dmax"], sc["speed"],
               wv[0], wv[1], wv[2], wv[3], seed, md["simTime"], md["warmup"]))
    return [os.path.join(a.ns3dir, "ns3"), "run", "--no-build", prog]

def work(job):
    a, sc, sched, w, seed, md = job
    rid = run_id(sc["name"], sched, w, seed)
    cmd = build_cmd(a, sc, sched, w, seed, md)
    t0 = time.time()
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=a.timeout,
                           cwd=a.ns3dir if a.backend == "ns3" else None)
        rows = [l[4:] for l in p.stdout.splitlines() if l.startswith("ROW,")]
        qs = [l[2:].split(",") for l in p.stdout.splitlines() if l.startswith("Q,")]
        if p.returncode != 0 or len(rows) < 2:
            return rid, None, None, "rc=%s %s" % (p.returncode, p.stderr[-300:])
        hdr = next(csv.reader([rows[0]])); val = next(csv.reader([rows[1]]))
        rec = dict(zip(hdr, val)); rec["run_id"] = rid; rec["wall_s"] = "%.3f" % (time.time() - t0)
        if sched != "MULTIOBJ":
            for k in ("wC", "wF", "wD", "wQ"): rec[k] = "nan"
        return rid, rec, [[rid] + q for q in qs], None
    except Exception as e:  # noqa
        return rid, None, None, repr(e)

def load_done(path):
    return set(pd.read_csv(path, usecols=["run_id"])["run_id"]) if os.path.exists(path) else set()

def run_jobs(a, jobs, runs_path, q_path, fail_path):
    done = load_done(runs_path)
    jobs = [j for j in jobs if run_id(j[1]["name"], j[2], j[3], j[4]) not in done]
    print("jobs to run: %d (skipping %d already done)" % (len(jobs), len(done)), flush=True)
    if not jobs: return 0, 0
    new = not os.path.exists(runs_path)
    ok = bad = 0
    t0 = time.time()
    with mp.Pool(a.jobs) as pool, open(runs_path, "a", newline="") as fr, open(q_path, "a", newline="") as fq, \
            open(fail_path, "a") as ff:
        wr = None; wq = csv.writer(fq)
        if os.path.getsize(q_path) == 0: wq.writerow(["run_id", "cls", "q", "delay_ms"])
        for i, (rid, rec, qs, err) in enumerate(pool.imap_unordered(work, jobs, chunksize=4), 1):
            if err:
                bad += 1; ff.write("%s\t%s\n" % (rid, err)); ff.flush()
            else:
                ok += 1
                if wr is None:
                    wr = csv.DictWriter(fr, fieldnames=list(rec.keys()))
                    if new: wr.writeheader()
                wr.writerow(rec); wq.writerows(qs)
            if i % 200 == 0:
                fr.flush(); fq.flush()
                el = time.time() - t0
                print("  %d/%d done  (%.0fs elapsed, ETA %.0fs)" % (i, len(jobs), el, el / i * (len(jobs) - i)), flush=True)
    return ok, bad

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--backend", choices=["sim", "ns3"], default="sim")
    ap.add_argument("--mode", choices=["smoke", "quick", "full"], default="smoke")
    ap.add_argument("--ns3dir", default=os.path.expanduser("~/ns-3-dev"))
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    ap.add_argument("--timeout", type=int, default=3600)
    ap.add_argument("--no-refine", action="store_true")
    a = ap.parse_args()
    cfg = yaml.safe_load(open(os.path.join(HERE, "scenarios.yaml")))
    md = cfg["modes"][a.mode]
    scs = [s for s in cfg["scenarios"] if "scenarios" not in md or s["name"] in md["scenarios"]]
    out = os.path.join(HERE, "results", a.backend, "raw"); os.makedirs(out, exist_ok=True)
    runs_path = os.path.join(out, "runs_%s.csv" % a.mode)
    q_path = os.path.join(out, "quantiles_%s.csv" % a.mode)
    fail_path = os.path.join(out, "failures_%s.log" % a.mode)
    open(q_path, "a").close()
    grid = simplex(md["grid_step"])
    if a.mode == "smoke":
        grid = [(1, 0, 0, 0), (0, 1, 0, 0), (0.25, 0.25, 0.25, 0.25)]
    jobs = []
    for sc in scs:
        for seed in md["seeds"]:
            for b in BASELINES: jobs.append((a, sc, b, None, seed, md))
            for w in grid: jobs.append((a, sc, "MULTIOBJ", w, seed, md))
    t0 = time.time()
    print("mode=%s backend=%s scenarios=%d seeds=%s grid=%d points -> %d runs" % (a.mode, a.backend, len(scs), md["seeds"], len(grid), len(jobs)))
    ok, bad = run_jobs(a, jobs, runs_path, q_path, fail_path)
    # ---- refinement around the best region (full mode only)
    if a.mode == "full" and not a.no_refine and os.path.exists(runs_path):
        df = pd.read_csv(runs_path)
        d = add_J(df)
        m = d[d.sched == "MULTIOBJ"].copy()
        m["w"] = list(zip(m.wC.round(3), m.wF.round(3), m.wD.round(3), m.wQ.round(3)))
        best = m.groupby("w")["J"].mean().sort_values(ascending=False).index[0]
        fine = [w for w in simplex(md["refine_step"]) if sum(abs(x - y) for x, y in zip(w, best)) <= 0.2 + 1e-9]
        print("refine: coarse best w*=%s ; %d fine points (L1<=0.2, step %.2f)" % (best, len(fine), md["refine_step"]))
        rj = [(a, sc, "MULTIOBJ", w, seed, md) for sc in scs for seed in md["seeds"] for w in fine]
        ok2, bad2 = run_jobs(a, rj, runs_path, q_path, fail_path); ok += ok2; bad += bad2
    print("attempted=%d succeeded=%d failed=%d  wall=%.1fs" % (ok + bad, ok, bad, time.time() - t0))

if __name__ == "__main__":
    main()
