#!/usr/bin/env python3
"""A large synthetic .bimble model for the C core's benchmarks (ticket T16). Synthetic only.

    python3 ccore/bench/large_model.py <target elements> <out.bimble>

Deterministic: the same target gives the same bytes (no clock, no random state; GlobalIds are
BLAKE2b("bimble-t16/<seed>/<counter>") encoded as IFC GlobalIds, first char 0-3, checked unique).
The model is golden/frame.json's structure scaled up: a concrete grid frame of NX x NY bays
(6.0 m x 5.0 m) over K storeys (3.0 m) with
  - pad footings under every grid point (storey "Fondations"),
  - rect columns on every grid point, rect beams both ways, one slab per bay, per storey,
  - walls on the perimeter of every storey, plus two rotated walls (x axis (0.6, 0.8)) in the
    corner bays,
  - an IfcBuildingElementProxy extruded polygon (an L shape) with two rectangular voids in
    every 8th bay, every storey.
NX, NY, K are chosen so the element count is within 1.5 % of the target (dims()). Prints the counts as JSON.
"""
import hashlib
import json
import sys

SEED = 16
ALPHABET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_$"
BAY_X, BAY_Y, STOREY_H = 6000.0, 5000.0, 3000.0


class Ids:
    def __init__(self):
        self.n = 0
        self.seen = set()

    def next(self):
        self.n += 1
        h = hashlib.blake2b(f"bimble-t16/{SEED}/{self.n}".encode(), digest_size=16).digest()
        v = int.from_bytes(h, "big")
        s = ALPHABET[v >> 126] + "".join(ALPHABET[(v >> (6 * (20 - i))) & 63] for i in range(21))
        assert len(s) == 22 and s[0] in "0123" and s not in self.seen
        self.seen.add(s)
        return s


def counts(nx, ny, k):
    grid = (nx + 1) * (ny + 1)
    per = grid + nx * (ny + 1) + ny * (nx + 1) + nx * ny + 2 * (nx + ny) + 2 + (nx * ny + 7) // 8
    return grid + k * per


def dims(target):
    """Square-ish grid (nx = ny + 0..1) and as many storeys as bays (k close to nx), within 1.5 %
    of the target count; then the closest count."""
    best = None
    for nx in range(1, 80):
        for ny in (nx - 1, nx):
            if ny < 1:
                continue
            for k in range(1, 200):
                c = counts(nx, ny, k)
                score = (abs(c - target) > 0.015 * target, abs(k - nx), abs(c - target))
                if best is None or score < best[0]:
                    best = (score, (nx, ny, k))
    return best[1]


def placement(o, z=(0.0, 0.0, 1.0), x=(1.0, 0.0, 0.0)):
    return {"origin": [float(v) for v in o], "z_axis": list(z), "x_axis": list(x)}


def build(target):
    nx, ny, k = dims(target)
    ids = Ids()
    project_id = ids.next()
    mat_c, mat_s = ids.next(), ids.next()
    materials = {
        mat_c: {"id": mat_c, "name": "C30/37", "category": "concrete",
                "props": {"density_kg_m3": 2500.0, "e_mpa": 33000.0, "strength_mpa": 30.0}},
        mat_s: {"id": mat_s, "name": "S355", "category": "steel",
                "props": {"density_kg_m3": 7850.0, "e_mpa": 210000.0, "strength_mpa": 355.0}},
    }
    storeys, elements = {}, {}
    found = ids.next()
    storeys[found] = {"id": found, "name": "Fondations", "elevation": -800.0}
    levels = []
    for s in range(k):
        sid = ids.next()
        storeys[sid] = {"id": sid, "name": f"Niveau {s + 1}", "elevation": s * STOREY_H}
        levels.append(sid)

    def add(cls, name, storey, mat, pl, gen):
        gid = ids.next()
        elements[gid] = {"id": gid, "class": cls, "name": name, "storey": storey, "material": mat,
                         "placement": pl, "generator": gen}

    for i in range(nx + 1):
        for j in range(ny + 1):
            add("IfcFooting", f"S{i}.{j}", found, mat_c, placement((i * BAY_X, j * BAY_Y, 0.0)),
                {"kind": "pad_footing", "width": 1200.0, "depth": 1200.0, "height": 800.0})
    col_h, beam_h, slab_t = 2780.0, 500.0, 220.0
    for s, sid in enumerate(levels):
        for i in range(nx + 1):
            for j in range(ny + 1):
                add("IfcColumn", f"P{s}.{i}.{j}", sid, mat_c, placement((i * BAY_X, j * BAY_Y, 0.0)),
                    {"kind": "rect_column", "width": 300.0, "depth": 300.0, "height": col_h})
        for j in range(ny + 1):  # beams along x
            for i in range(nx):
                add("IfcBeam", f"B{s}.x{i}.{j}", sid, mat_c,
                    placement((i * BAY_X, j * BAY_Y, col_h - beam_h / 2), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)),
                    {"kind": "rect_beam", "width": 250.0, "height": beam_h, "length": BAY_X})
        for i in range(nx + 1):  # beams along y
            for j in range(ny):
                add("IfcBeam", f"B{s}.y{i}.{j}", sid, mat_c,
                    placement((i * BAY_X, j * BAY_Y, col_h - beam_h / 2), (0.0, 1.0, 0.0), (-1.0, 0.0, 0.0)),
                    {"kind": "rect_beam", "width": 250.0, "height": beam_h, "length": BAY_Y})
        for i in range(nx):
            for j in range(ny):
                add("IfcSlab", f"D{s}.{i}.{j}", sid, mat_c, placement((i * BAY_X, j * BAY_Y, col_h)),
                    {"kind": "slab", "outline": [[0.0, 0.0], [BAY_X, 0.0], [BAY_X, BAY_Y], [0.0, BAY_Y]],
                     "thickness": slab_t})
        wall = lambda length: {"kind": "wall", "length": length, "thickness": 200.0, "height": col_h}
        for i in range(nx):  # perimeter walls, along x on both sides, along y on both ends
            add("IfcWall", f"W{s}.s{i}", sid, mat_c, placement((i * BAY_X, -300.0, 0.0)), wall(BAY_X))
            add("IfcWall", f"W{s}.n{i}", sid, mat_c, placement((i * BAY_X, ny * BAY_Y + 300.0, 0.0)), wall(BAY_X))
        for j in range(ny):
            add("IfcWall", f"W{s}.w{j}", sid, mat_c,
                placement((-300.0, j * BAY_Y, 0.0), x=(0.0, 1.0, 0.0)), wall(BAY_Y))
            add("IfcWall", f"W{s}.e{j}", sid, mat_c,
                placement((nx * BAY_X + 300.0, j * BAY_Y, 0.0), x=(0.0, 1.0, 0.0)), wall(BAY_Y))
        add("IfcWall", f"W{s}.r0", sid, mat_c, placement((500.0, 500.0, 0.0), x=(0.6, 0.8, 0.0)), wall(4000.0))
        add("IfcWall", f"W{s}.r1", sid, mat_c,
            placement(((nx - 1) * BAY_X + 500.0, 500.0, 0.0), x=(0.6, 0.8, 0.0)), wall(4000.0))
        for b in range(0, nx * ny, 8):
            i, j = b % nx, b // nx
            add("IfcBuildingElementProxy", f"X{s}.{b}", sid, mat_s,
                placement((i * BAY_X + 1000.0, j * BAY_Y + 1000.0, 0.0)),
                {"kind": "extruded_polygon",
                 "outline": [[0.0, 0.0], [3000.0, 0.0], [3000.0, 1000.0], [1000.0, 1000.0], [1000.0, 2500.0], [0.0, 2500.0]],
                 "voids": [[[200.0, 200.0], [800.0, 200.0], [800.0, 800.0], [200.0, 800.0]],
                           [[1500.0, 300.0], [2600.0, 300.0], [2600.0, 700.0], [1500.0, 700.0]]],
                 "height": 1200.0})
    assert len(elements) == counts(nx, ny, k)
    model = {"project_id": project_id, "project_name": f"Synthetic grid frame {nx}x{ny}x{k} (T16)",
             "storeys": dict(sorted(storeys.items())), "materials": dict(sorted(materials.items())),
             "elements": dict(sorted(elements.items()))}
    return {"format": "bimble", "version": 1, "model": model}, (nx, ny, k)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    target, out = int(sys.argv[1]), sys.argv[2]
    doc, (nx, ny, k) = build(target)
    text = json.dumps(doc, indent=2, ensure_ascii=False)
    with open(out, "w", encoding="utf-8") as f:
        f.write(text)
    els = doc["model"]["elements"].values()
    by = {}
    for e in els:
        by[e["class"]] = by.get(e["class"], 0) + 1
    print(json.dumps({"file": out, "bytes": len(text.encode()), "nx": nx, "ny": ny, "storeys": k,
                      "elements": len(doc["model"]["elements"]), "by_class": dict(sorted(by.items())),
                      "storey_ids": list(doc["model"]["storeys"])[:2]}))


if __name__ == "__main__":
    main()
