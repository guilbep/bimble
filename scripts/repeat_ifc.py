"""Make a large synthetic IFC by repeating a public model N times side by side (ADR 0039).

Usage: python3 scripts/repeat_ifc.py in.ifc out.ifc N [--spacing-m 200]
       python3 scripts/repeat_ifc.py in.ifc out.ifc --size-gb 5 [--spacing-m 200]

Streams: one statement at a time, the output is never held in memory (the input is read
once to scan, then once per copy). Copies sit on a square grid, `spacing` apart.

Per copy k >= 1:
  - every entity id #n becomes #(n + k * stride) (stride = max id + 4), outside strings;
  - every IfcRoot GlobalId (an entity's first argument when it is a 22-character IFC
    base64 string) becomes a fresh one, derived from (k, old GlobalId) so runs are
    reproducible;
  - one IfcProject for the whole file: copy 0's. Later copies drop theirs and point at it
    (their spatial trees aggregate under it, like several sites of one project), so the
    file stays one project as a viewer or IfcOpenShell expects;
  - placements relative to nothing (the site's, usually) get a new parent placement at the
    copy's offset: shared points are left alone.
"""

import argparse
import hashlib
import math
import os
import re
import sys

B64 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_$"
REF_OR_STRING = re.compile(r"'(?:[^']|'')*'|#(\d+)")
HEAD = re.compile(r"\s*#(\d+)\s*=\s*([A-Za-z0-9_]+)\s*\(")
GLOBAL_ID = re.compile(r"(\s*#\d+\s*=\s*[A-Za-z0-9_]+\s*\(\s*)'([0-9A-Za-z_$]{22})'")
ROOT_PLACEMENT = re.compile(r"(\s*#\d+\s*=\s*IFCLOCALPLACEMENT\s*\(\s*)\$", re.I)


def global_id(seed: bytes) -> str:
    """A GlobalId (128 bits in IFC's 22-character base64) from a hash of `seed`."""
    n = int.from_bytes(hashlib.md5(seed).digest(), "big")
    return "".join(B64[(n >> (6 * i)) & 63] for i in range(21, -1, -1))


def statements(path):
    """(text, part) for each statement, with its line ends; part is "head" (up to DATA;),
    "data", or "tail" (the data section's ENDSEC; and after). Strings may span lines."""
    with open(path, "r", encoding="latin-1", newline="") as f:
        buf, quotes, part = [], 0, "head"
        for line in f:
            buf.append(line)
            quotes += line.count("'")
            if quotes % 2 == 0 and line.rstrip().endswith(";"):
                s = "".join(buf)
                t = s.strip()
                if part == "data" and t == "ENDSEC;":
                    part = "tail"
                yield s, part
                if part == "head" and t == "DATA;":
                    part = "data"
                buf, quotes = [], 0
        if buf:
            yield "".join(buf), "tail"


def scan(path):
    """Largest id, the IfcProject's id, and file length units per metre."""
    max_id, project, per_m = 0, None, 1.0
    for s, part in statements(path):
        m = HEAD.match(s) if part == "data" else None
        if not m:
            continue
        max_id = max(max_id, int(m.group(1)))
        name = m.group(2).upper()
        if name == "IFCPROJECT":
            project = int(m.group(1))
        elif name == "IFCSIUNIT" and ".LENGTHUNIT." in s.upper():
            per_m = 1000.0 if ".MILLI." in s.upper() else 1.0
    return max_id, project, per_m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("copies", type=int, nargs="?")
    ap.add_argument("--size-gb", type=float, help="as many copies as reach about this size")
    ap.add_argument("--spacing-m", type=float, default=200.0)
    a = ap.parse_args()
    if a.copies is None and a.size_gb is None:
        ap.error("give N or --size-gb")
    copies = a.copies or max(1, round(a.size_gb * 1e9 / os.path.getsize(a.src)))
    max_id, project, per_m = scan(a.src)
    stride = max_id + 4  # 3 new entities per copy: a point, an axis placement, a placement
    side = math.ceil(math.sqrt(copies))

    with open(a.out, "w", encoding="latin-1", newline="") as out:
        tail = []
        for s, part in statements(a.src):
            if part == "tail":
                tail.append(s)
            else:
                out.write(s)
        for k in range(1, copies):
            off = k * stride

            def renumber(m):
                if m.group(1) is None:
                    return m.group(0)
                n = int(m.group(1))
                return "#%d" % (n if n == project else n + off)

            x, y = (k % side) * a.spacing_m * per_m, (k // side) * a.spacing_m * per_m
            base = max_id + 1 + off
            parent = "#%d" % (base + 2)
            out.write(
                "#%d= IFCCARTESIANPOINT((%r,%r,0.));\n"
                "#%d= IFCAXIS2PLACEMENT3D(#%d,$,$);\n"
                "#%d= IFCLOCALPLACEMENT($,#%d);\n" % (base, x, y, base + 1, base, base + 2, base + 1)
            )
            for s, part in statements(a.src):
                if part != "data":
                    continue
                m = HEAD.match(s)
                if m and int(m.group(1)) == project:
                    continue
                g = GLOBAL_ID.match(s)
                if g:
                    fresh = global_id(b"%d:%s" % (k, g.group(2).encode()))
                    s = "%s'%s'%s" % (g.group(1), fresh, s[g.end():])
                s = REF_OR_STRING.sub(renumber, s)
                s = ROOT_PLACEMENT.sub(lambda p: p.group(1) + parent, s, count=1)
                out.write(s)
        out.write("".join(tail))
    size = os.path.getsize(a.out) / 1e9
    print(f"{a.out}: {copies} copies, id stride {stride}, {size:.2f} GB", file=sys.stderr)


if __name__ == "__main__":
    main()
