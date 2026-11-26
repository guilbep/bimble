"""A synthetic mesh-heavy model for the Measure study (ticket UXMAP). Synthetic only.

    python3 ccore/bench/mesh_heavy_model.py <proxies> <triangles per proxy> <out.json>

golden/frame.json (72 elements) plus <proxies> IfcBuildingElementProxy elements, each a closed
mesh (generator "mesh": a UV sphere of about <triangles per proxy> triangles, 400 mm radius) on a
grid of 1.5 m over the frame's ground floor, as an imported model's heavy proxies are (fixtures,
furniture, equipment read from IFC as triangle meshes). Deterministic: the same arguments give the
same bytes; GlobalIds are BLAKE2b("bimble-uxmap/<counter>") in the IFC alphabet, first char 0-3.
"""
import hashlib
import json
import math
import os
import sys

ALPHABET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_$"


def gid(n, seen):
    h = hashlib.blake2b(f"bimble-uxmap/{n}".encode(), digest_size=16).digest()
    v = int.from_bytes(h, "big")
    s = ALPHABET[v >> 126] + "".join(ALPHABET[(v >> (6 * (20 - i))) & 63] for i in range(21))
    assert len(s) == 22 and s[0] in "0123" and s not in seen
    seen.add(s)
    return s


def sphere(tris, r=400.0):
    """A UV sphere: rings x segments with 2 * segments * (rings - 1) triangles."""
    seg = max(4, int(math.sqrt(tris)))
    rings = max(2, tris // (2 * seg) + 1)
    pos = [[0.0, 0.0, 2 * r]]
    for i in range(1, rings):
        t = math.pi * i / rings
        for j in range(seg):
            p = 2 * math.pi * j / seg
            pos.append([round(r * math.sin(t) * math.cos(p), 3), round(r * math.sin(t) * math.sin(p), 3), round(r + r * math.cos(t), 3)])
    pos.append([0.0, 0.0, 0.0])
    last = len(pos) - 1
    idx = []
    for j in range(seg):
        idx += [0, 1 + j, 1 + (j + 1) % seg]
    for i in range(rings - 2):
        a, b = 1 + i * seg, 1 + (i + 1) * seg
        for j in range(seg):
            k = (j + 1) % seg
            idx += [a + j, b + j, b + k, a + j, b + k, a + k]
    a = 1 + (rings - 2) * seg
    for j in range(seg):
        idx += [a + j, last, a + (j + 1) % seg]
    return pos, idx


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    n, tris, out = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
    here = os.path.dirname(os.path.abspath(__file__))
    with open(os.path.join(here, "..", "golden", "frame.json")) as f:
        doc = json.load(f)
    m = doc["model"]
    storey = next(s for s in m["storeys"].values() if s["elevation"] == 0.0)["id"]
    material = next(iter(m["materials"]))
    seen = set(m["elements"]) | set(m["storeys"]) | set(m["materials"])
    pos, idx = sphere(tris)
    side = math.ceil(math.sqrt(n))
    for k in range(n):
        i = gid(k + 1, seen)
        m["elements"][i] = {
            "id": i, "class": "IfcBuildingElementProxy", "name": f"Proxy {k + 1}", "storey": storey, "material": material,
            "placement": {"origin": [float(1500 * (k % side)), float(1500 * (k // side)), 0.0], "z_axis": [0.0, 0.0, 1.0], "x_axis": [1.0, 0.0, 0.0]},
            "generator": {"kind": "mesh", "positions": pos, "indices": idx}, "properties": {},
        }
    m["elements"] = dict(sorted(m["elements"].items()))
    with open(out, "w") as f:
        json.dump(doc, f, separators=(",", ":"))
    print(json.dumps({"elements": len(m["elements"]), "proxies": n, "triangles_per_proxy": len(idx) // 3, "triangles": n * len(idx) // 3}))


main()
