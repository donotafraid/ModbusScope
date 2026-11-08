#!/usr/bin/env python3
"""对归档内每个 label 的 p99/p999 中位数做 bootstrap 95% CI（run 数由文件发现）。"""
import sys, glob, re, os
import numpy as np

out = sys.argv[1]
labels = sorted({
    os.path.basename(f).rsplit("_run", 1)[0]
    for f in glob.glob(f"{out}/*_run*.txt")
})

def vals(label, m):
    v = []
    for f in sorted(glob.glob(f"{out}/{label}_run*.txt")):
        t = open(f).read()
        mm = re.search(rf"{m}=([0-9.]+)", t)
        if mm: v.append(float(mm.group(1)))
    return np.array(v)

rng = np.random.default_rng(42)
for label in labels:
    for m in ("p99", "p999"):
        x = vals(label, m)
        if len(x) < 2:
            print(f"{label} {m}: n={len(x)} (insufficient for CI)")
            continue
        bs = np.array([np.median(rng.choice(x, len(x), replace=True)) for _ in range(10000)])
        lo, hi = np.percentile(bs, [2.5, 97.5])
        print(f"{label} {m}: median={np.median(x):.3f} CI=[{lo:.3f},{hi:.3f}] n={len(x)}")