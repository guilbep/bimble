"""Triangles per element: the C reader against IfcOpenShell (ticket TESS1, ADR 0085).

    $HOME/.cache/bimble-ifc-corpus/venv/bin/python -I ccore/bench/tess1_compare.py \
        <ifc_dump> <scratch-dir> <file.ifc>... [--detail exact|standard|light] [--json out.json]

For each file: `<ifc_dump> read` (its scene.txt, `i=` triangles per element; `--detail` is passed
through as `ifc_dump read --detail <d>`), and IfcOpenShell's iterator twice: with Bonsai's settings
(hybrid-cgal-simple-opencascade, mesher-linear-deflection 0.05 m, angular 0.5 rad: what the Bonsai
comparison measured, scripts/ios_geom_bench.py) and with IfcOpenShell's defaults (linear 0.001 m,
angular 0.5 rad). Prints a markdown table, one row per element (by Name), and the totals.
Untrusted files are only read; the scratch directory gets the dumps.
"""
import json
import multiprocessing
import os
import re
import subprocess
import sys

import ifcopenshell
import ifcopenshell.geom


def ios_counts(path, linear):
    f = ifcopenshell.open(path)
    s = ifcopenshell.geom.settings()
    s.set("mesher-linear-deflection", linear)
    s.set("mesher-angular-deflection", 0.5)
    s.set("apply-default-materials", False)
    s.set("layerset-first", True)
    s.set("no-wire-intersection-check", True)
    els = [e for e in f.by_type("IfcElement") if not e.is_a("IfcFeatureElement")]
    out = {}
    it = ifcopenshell.geom.iterator(s, f, multiprocessing.cpu_count(), include=els,
                                    geometry_library="hybrid-cgal-simple-opencascade")
    if it.initialize():
        while True:
            sh = it.get()
            if sh:
                out[f.by_id(sh.id).GlobalId] = len(sh.geometry.faces) // 3
            if not it.next():
                break
    return {e.GlobalId: (e.Name or "", e.is_a(), out.get(e.GlobalId)) for e in els}


def bimble_counts(dump, path, scratch, detail):
    d = os.path.join(scratch, os.path.basename(path) + (f".{detail}" if detail else ""))
    os.makedirs(d, exist_ok=True)
    cmd = [dump, "read", path, d] + (["--detail", detail] if detail else [])
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    out = {}
    for line in open(os.path.join(d, "scene.txt")):
        m = re.match(r"^\d+ (\S+) \S+ .* i=(\d+) ", line)
        if m:
            out[m.group(1)] = int(m.group(2))
    return out


def main():
    args, detail, jout = [], None, None
    it = iter(sys.argv[1:])
    for a in it:
        if a == "--detail":
            detail = next(it)
        elif a == "--json":
            jout = next(it)
        else:
            args.append(a)
    if len(args) < 3:
        sys.exit(__doc__)
    dump, scratch, files = args[0], args[1], args[2:]
    rows, tot = [], [0, 0, 0]
    print("| file | element | class | C | IfcOpenShell (Bonsai: 0.05 m, 0.5 rad) | IfcOpenShell (defaults: 0.001 m, 0.5 rad) |")
    print("|---|---|---|---|---|---|")
    for p in files:
        b = bimble_counts(dump, p, scratch, detail)
        bon = ios_counts(p, 0.05)
        dflt = ios_counts(p, 0.001)
        for g, (name, cls, n1) in sorted(bon.items(), key=lambda kv: kv[1][0]):
            n2 = dflt[g][2]
            c = b.get(g, 0)
            rows.append({"file": os.path.basename(p), "element": name, "class": cls, "c": c, "ios_bonsai": n1, "ios_default": n2})
            print(f"| {os.path.basename(p)} | {name} | {cls} | {c} | {n1} | {n2} |")
            tot[0] += c
            tot[1] += n1 or 0
            tot[2] += n2 or 0
    print(f"| total | | | {tot[0]} | {tot[1]} | {tot[2]} |")
    if jout:
        with open(jout, "w") as fh:
            json.dump(rows, fh, indent=1)


if __name__ == "__main__":
    main()
