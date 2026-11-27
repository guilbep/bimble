#!/usr/bin/env python3
"""Tables of the Bonsai / IfcOpenShell / Bimble series (bonsai_series.sh) as Markdown.
Usage: python3 -I bonsai_series_table.py <work dir> [--fidelity 05,06,07]"""

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


def bimble_log(work, n):
    logs = glob.glob(f"{work}/bimble-cache/links/*-f{n}.ifc.log.txt")
    if not logs:
        return None
    t = open(logs[0]).read()
    d = {}
    for key, rx in [
        ("products_with_body", r"products with a body: (\d+)"),
        ("elements_drawn", r"elements drawn: (\d+)"),
        ("triangles", r"triangles: (\d+)"),
        ("shapes", r"shapes after sharing: (\d+)"),
        ("read_ms", r"read time \(ms\): (\d+)"),
        ("cache_bytes", r"\((\d+) bytes\)\n"),
    ]:
        m = re.search(rx, t)
        d[key] = int(m.group(1)) if m else None

    def block(title):
        m = re.search(re.escape(title) + r"[^\n]*\n((?:[^\n]+\n)*)", t)
        out = {}
        if m:
            for line in m.group(1).splitlines():
                parts = line.split()
                if len(parts) >= 2 and parts[-1].isdigit():
                    out[" ".join(parts[:-1])] = int(parts[-1])
        return out

    d["drawn_by_class"] = block("elements drawn per IFC class")
    d["empty_by_entity"] = block("products with a body drawn empty (left out), per entity")
    m = re.search(r"skipped and why \(the reader's warnings\):\n((?:  .*\n)*)", t)
    d["warnings"] = m.group(1).strip() if m else ""
    views = glob.glob(f"{work}/bimble-cache/links/*-f{n}.ifc.bimbleview")
    if views:
        here = os.path.dirname(os.path.abspath(__file__))
        out = subprocess.run(
            [sys.executable, "-I", os.path.join(here, "bimbleview_sections.py"), views[0]], capture_output=True, text=True
        ).stdout
        d["sections"] = json.loads(out)["sections"] if out else {}
    return d


def mb(x):
    return "-" if x is None else f"{x / 1e6:.1f}"


def fmt_s(x):
    return "-" if x is None else (f"{x:.2f}" if x < 10 else f"{x:.1f}")


def main():
    work = sys.argv[1]
    sizes = {}
    for p in sorted(glob.glob(f"{work}/files/f*.ifc")):
        n = os.path.basename(p)[1:3]
        sizes[n] = os.path.getsize(p)
    rows = []
    print("| # | MB | IOS parse s | IOS parse RSS MB | IOS geom s | IOS peak RSS MB | IOS shapes | IOS tris |"
          " Bonsai load s | Bonsai load RSS MB | objects | tris | .blend MB |"
          " link 1st s | link 1st RSS MB (self+child) | link cache MB (blend+sqlite) | link 2nd s | link 2nd RSS MB |"
          " Bimble wall s | Bimble RSS MB | Bimble cache MB | of which ELEM / PROP / HLOD MB | Bimble elements | Bimble tris |")
    print("|" + "---|" * 24)
    for n in sorted(sizes):
        ios, iosm = load(f"{work}/res/ios_f{n}.json"), load(f"{work}/res/ios_f{n}.m.json")
        bl, blm = load(f"{work}/res/bload_f{n}.json"), load(f"{work}/res/bload_f{n}.m.json")
        k1, k1m = load(f"{work}/res/blink1_f{n}.json"), load(f"{work}/res/blink1_f{n}.m.json")
        k2, k2m = load(f"{work}/res/blink2_f{n}.json"), load(f"{work}/res/blink2_f{n}.m.json")
        bm = load(f"{work}/res/bim_f{n}.m.json")
        bg = bimble_log(work, n)

        def fail(m):
            if m is None:
                return "not run"
            if m["timed_out"]:
                return f"TIME >{m['wall_s']:.0f}s"
            return f"FAIL rc={m['exit_code']} at {m['max_rss_mb']:.0f} MB, {m['wall_s']:.0f}s (limit {m['vmem_limit_gb']} GB)"

        c = [f"#{int(n)}", f"{sizes[n] / 1e6:.1f}"]
        if ios and iosm and not iosm["exit_code"]:
            c += [fmt_s(ios["parse_s"]), f"{ios['parse_peak_rss_mb']:.0f}", fmt_s(ios["geometry_s"]),
                  f"{iosm['max_rss_mb']:.0f}", str(ios["shapes"]), str(ios["triangles"])]
        else:
            c += [fail(iosm)] + [""] * 5
        if bl and blm and not blm["exit_code"]:
            stop = " (stopped >30k, then elements)" if bl.get("stopped_as_large_model") else ""
            c += [fmt_s(bl["total_s"]) + stop, f"{blm['max_rss_mb']:.0f}", str(bl["objects"]), str(bl["triangles"]),
                  mb(bl.get("blend_bytes"))]
        else:
            c += [fail(blm)] + [""] * 4
        if k1 and k1m and not k1m["exit_code"]:
            cb = k1["cache_bytes"]
            c += [fmt_s(k1["link_s"]), f"{k1['peak_rss_self_mb']:.0f}+{k1['peak_rss_children_mb']:.0f}",
                  f"{mb(cb['.cache.blend'])}+{mb(cb['.cache.sqlite'])}"]
        else:
            c += [fail(k1m), "", ""]
        if k2 and k2m and not k2m["exit_code"]:
            c += [fmt_s(k2["link_s"]), f"{k2m['max_rss_mb']:.0f}"]
        else:
            c += [fail(k2m), ""]
        if bm and not bm["exit_code"] and bg:
            s = bg.get("sections", {})
            c += [fmt_s(bm["wall_s"]), f"{bm['max_rss_mb']:.0f}", mb(bg["cache_bytes"]),
                  f"{mb(s.get('ELEM'))} / {mb(s.get('PROP'))} / {mb(s.get('HLOD'))}",
                  str(bg["elements_drawn"]), str(bg["triangles"])]
        else:
            c += [fail(bm)] + [""] * 5
        rows.append(c)
        print("| " + " | ".join(c) + " |")

    if "--fidelity" in sys.argv:
        for n in sys.argv[sys.argv.index("--fidelity") + 1].split(","):
            ios, bg = load(f"{work}/res/ios_f{n}.json"), bimble_log(work, n)
            if not ios or not bg:
                continue
            print(f"\n#### #{int(n)}\n")
            print(f"IfcOpenShell: elements {ios['elements']}, with a representation {ios['elements_with_representation']}, "
                  f"shapes {ios['shapes']} (elements {ios['element_shapes']}), triangles {ios['triangles']}, "
                  f"unique geometries {ios['unique_geometries']}; not drawn: {ios['elements_with_representation_not_drawn']}")
            print(f"Bimble: products with a body {bg['products_with_body']}, drawn {bg['elements_drawn']}, triangles "
                  f"{bg['triangles']}, shapes {bg['shapes']}; drawn empty: {bg['empty_by_entity']}")
            print(f"Bimble warnings: {bg['warnings'][:600]!r}")
            ib = {k.upper(): v for k, v in ios["body_shapes_by_class"].items()}
            it = {k.upper(): v for k, v in ios["body_triangles_by_class"].items()}
            ia = {k.upper(): v for k, v in ios["shapes_by_class"].items()}
            bb = {k.upper(): v for k, v in bg["drawn_by_class"].items()}
            print("\n| class | IOS shapes (any context) | IOS Body shapes | Bimble drawn | IOS Body tris |\n|---|---|---|---|---|")
            for k in sorted(set(ib) | set(bb) | set(ia), key=lambda k: -(ia.get(k, 0) + bb.get(k, 0))):
                if ia.get(k, 0) != bb.get(k, 0) or ib.get(k, 0) != bb.get(k, 0):
                    print(f"| {k} | {ia.get(k, 0)} | {ib.get(k, 0)} | {bb.get(k, 0)} | {it.get(k, 0)} |")


if __name__ == "__main__":
    main()
