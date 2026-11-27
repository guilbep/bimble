#!/usr/bin/env python3
"""Rows of the cgroup-capped series (bonsai_series.sh, MemoryMax=8G) as Markdown, three tables.
RSS = wait4 peak RSS (largest process); cg = the scope's memory.peak (all processes, page cache
included). Usage: python3 -I bonsai_cgroup_rows.py <work dir> <first nn> <last nn>"""

import glob
import json
import os
import re
import subprocess
import sys


def load(p):
    try:
        with open(p) as fh:
            return json.load(fh)
    except Exception:
        return None


def s(x):
    return "-" if x is None else (f"{x:.2f}" if x < 10 else f"{x:.1f}")


def fail(m):
    if m is None:
        return "not run"
    why = "**OOM-killed** (cgroup 8 GB)" if m.get("cgroup_oom_kills") else (
        "**timed out** (20 min)" if m["timed_out"] else f"**failed** rc={m['exit_code']}")
    return f"{why} after {m['wall_s']:.0f} s, RSS {m['max_rss_mb']:.0f} MB, cg {m['cgroup_memory_peak_mb']:.0f} MB"


def ok(m):
    return m is not None and not m["exit_code"] and not m["timed_out"]


def main():
    work, a, b = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    here = os.path.dirname(os.path.abspath(__file__))
    A, B, C = [], [], []
    for i in range(a, b + 1):
        n = f"{i:02d}"
        mb = os.path.getsize(f"{work}/files/f{n}.ifc") / 1e6 if os.path.exists(f"{work}/files/f{n}.ifc") else 0
        h = f"| #{i} | {mb:.1f} |"
        ios, iosm = load(f"{work}/res/ios_f{n}.json"), load(f"{work}/res/ios_f{n}.m.json")
        if ok(iosm) and ios:
            A.append(f"{h} {s(ios['parse_s'])} | {ios['parse_peak_rss_mb']:.0f} | {s(ios['geometry_s'])} | "
                     f"{iosm['max_rss_mb']:.0f} / {iosm['cgroup_memory_peak_mb']:.0f} | {ios['shapes']} | {ios['triangles']} |")
        else:
            log = f"{work}/logs/ios_f{n}.log"
            m = re.search(r"parse ([\d.]+) s, peak RSS (\d+) MB", open(log).read()) if os.path.exists(log) else None
            pre = f"{m.group(1)} | {m.group(2)} |" if m else "| |"
            A.append(f"{h} {pre} {fail(iosm)} | | | |")
        bl, blm = load(f"{work}/res/bload_f{n}.json"), load(f"{work}/res/bload_f{n}.m.json")
        if ok(blm) and bl:
            st = f" (stopped at {s(bl['load_project_s'])} s, then elements)" if bl.get("stopped_as_large_model") else ""
            load_cell = (f"{s(bl['total_s'])}{st} | {blm['max_rss_mb']:.0f} / {blm['cgroup_memory_peak_mb']:.0f} | "
                         f"{bl['objects']} | {bl['triangles']} | {s((bl.get('blend_bytes') or 0) / 1e6)}")
        else:
            load_cell = f"{fail(blm)} | | | |"
        k1, k1m = load(f"{work}/res/blink1_f{n}.json"), load(f"{work}/res/blink1_f{n}.m.json")
        k2, k2m = load(f"{work}/res/blink2_f{n}.json"), load(f"{work}/res/blink2_f{n}.m.json")
        if ok(k1m) and k1:
            cb = k1["cache_bytes"]
            l1 = (f"{s(k1['link_s'])} | {k1['peak_rss_self_mb']:.0f} + {k1['peak_rss_children_mb']:.0f} / "
                  f"{k1m['cgroup_memory_peak_mb']:.0f} | {s(cb['.cache.blend'] / 1e6)} + {s(cb['.cache.sqlite'] / 1e6)}")
        else:
            l1 = f"{fail(k1m)} | |"
        l2 = (f"{s(k2['link_s'])} | {k2m['max_rss_mb']:.0f} / {k2m['cgroup_memory_peak_mb']:.0f}"
              if ok(k2m) and k2 else f"{fail(k2m) if k2m else 'not run'} |")
        B.append(f"{h} {load_cell} | {l1} | {l2} |")
        bm = load(f"{work}/res/bim_f{n}.m.json")
        logs = glob.glob(f"{work}/bimble-cache/links/*-f{n}.ifc.log.txt")
        views = glob.glob(f"{work}/bimble-cache/links/*-f{n}.ifc.bimbleview")
        if ok(bm) and logs and views:
            t = open(logs[0]).read()
            el = re.search(r"elements drawn: (\d+)", t).group(1)
            tr = re.search(r"\ntriangles: (\d+)", t).group(1)
            sec = json.loads(subprocess.run([sys.executable, "-I", os.path.join(here, "bimbleview_sections.py"), views[0]],
                                            capture_output=True, text=True).stdout)
            sz = sec["file_bytes"] / 1e6
            ss = sec["sections"]
            C.append(f"{h} {s(bm['wall_s'])} | {bm['max_rss_mb']:.0f} / {bm['cgroup_memory_peak_mb']:.0f} | {sz:.1f} | "
                     f"{ss.get('ELEM', 0) / 1e6:.1f} / {(ss.get('PROP', 0) + ss.get('PRPZ', 0)) / 1e6:.1f} / {ss.get('HLOD', 0) / 1e6:.1f} | {el} | {tr} |")
        else:
            C.append(f"{h} {fail(bm)} | | | | | |")
    print("\n".join(A))
    print()
    print("\n".join(B))
    print()
    print("\n".join(C))


if __name__ == "__main__":
    main()
