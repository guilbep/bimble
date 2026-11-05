#!/usr/bin/env python3
"""Compare `bimble bench --json` results with a baseline and flag slowdowns.

    bench_compare.py NEW.json [NEW2.json ...] --baseline BASE.json [BASE2.json ...] [--threshold 0.25] [--fail]

Repeated runs on either side are reduced to the best value per metric, which damps noise from
shared CI runners. Prints a Markdown table (also appended to $GITHUB_STEP_SUMMARY when set)
and emits a GitHub `::warning::` per metric slower than the threshold. Exits 0 unless --fail is
given, because wall-clock times on shared runners are a trend, not a gate (ADR 0011). The gate
is the deterministic geometry budget test in crates/bimble-ifc/tests/budgets.rs.

Standard library only; run with `python -I`.
"""

import argparse
import json
import os
import sys

# metric -> (label, absolute slack). Below the slack a change is noise whatever the ratio.
METRICS = {
    "load_ms": ("load", 5.0),
    "upload_ms": ("upload", 5.0),
    "frame_median_ms": ("frame median", 2.0),
    "frame_p95_ms": ("frame p95", 4.0),
    "memory_mb": ("memory MB", 20.0),
}


def cases(doc):
    return {c["name"]: c for c in doc.get("cases", [])}


def best(runs):
    """Per case and metric, the minimum over repeated runs."""
    out = {}
    for run in runs:
        for name, c in cases(run).items():
            o = out.setdefault(name, dict(c))
            for m in METRICS:
                if c.get(m) is not None and (o.get(m) is None or c[m] < o[m]):
                    o[m] = c[m]
    return out


def compare(base, new, threshold):
    """Returns (rows, regressions). A row is (case, label, base, new, ratio, flag)."""
    rows, bad = [], []
    for name, n in new.items():
        b = base.get(name)
        if b is None:
            rows.append((name, "(new case)", None, None, None, ""))
            continue
        if b.get("triangles") != n.get("triangles"):
            rows.append((name, "triangles", b.get("triangles"), n.get("triangles"), None, "changed"))
        for m, (label, slack) in METRICS.items():
            bv, nv = b.get(m), n.get(m)
            if bv is None or nv is None:
                continue
            ratio = nv / bv if bv > 0 else float("inf")
            flag = ""
            if nv - bv > slack and ratio > 1 + threshold:
                flag = "slower"
                bad.append(f"{name}: {label} {bv:.1f} -> {nv:.1f} (+{(ratio - 1) * 100:.0f}%)")
            elif bv - nv > slack and ratio < 1 / (1 + threshold):
                flag = "faster"
            rows.append((name, label, bv, nv, ratio, flag))
    return rows, bad


def table(rows, base_doc, new_doc, threshold):
    fmt = lambda v: "" if v is None else (f"{v:.1f}" if isinstance(v, float) else str(v))
    lines = [
        "### Performance trend",
        "",
        f"Baseline: bimble {base_doc.get('bimble', '?')} · {base_doc.get('system', '?')}",
        f"This run: bimble {new_doc.get('bimble', '?')} · {new_doc.get('system', '?')}",
        "",
        f"Shared runners are noisy: only changes above {threshold * 100:.0f}% (and above a small absolute slack) are flagged.",
        "",
        "| case | metric | baseline | this run | ratio | |",
        "|---|---|---:|---:|---:|---|",
    ]
    for name, label, bv, nv, ratio, flag in rows:
        r = "" if ratio is None else f"{ratio:.2f}×"
        mark = {"slower": "⚠️ slower", "faster": "✅ faster", "changed": "changed"}.get(flag, "")
        lines.append(f"| {name} | {label} | {fmt(bv)} | {fmt(nv)} | {r} | {mark} |")
    return "\n".join(lines) + "\n"


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("new", nargs="+", help="bench --json files of this build")
    p.add_argument("--baseline", nargs="+", required=True, help="bench --json files to compare with")
    p.add_argument("--threshold", type=float, default=0.25)
    p.add_argument("--fail", action="store_true", help="exit 1 on a flagged slowdown")
    a = p.parse_args(argv)

    def load(paths):
        docs = []
        for path in paths:
            with open(path) as f:
                docs.append(json.load(f))
        return docs

    base_docs, new_docs = load(a.baseline), load(a.new)
    rows, bad = compare(best(base_docs), best(new_docs), a.threshold)
    md = table(rows, base_docs[0], new_docs[0], a.threshold)
    print(md)
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a") as f:
            f.write(md)
    for msg in bad:
        print(f"::warning title=Performance::{msg}")
    return 1 if (bad and a.fail) else 0


if __name__ == "__main__":
    sys.exit(main())
