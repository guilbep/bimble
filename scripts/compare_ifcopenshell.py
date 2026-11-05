"""Compare Bimble's IFC reader against IfcOpenShell, element by element.

Usage:
  bimble info model.ifc --elements-json > ours.jsonl
  python3 scripts/compare_ifcopenshell.py model.ifc ours.jsonl [--no-openings]

Checks, per GlobalId: same set of elements, same IFC class, volume within 0.5 % (flat or
open elements with ~zero volume are compared against 0.1 % of their bounding-box volume),
world bounding box within 1 mm. Openings (IfcRelVoidsElement) are subtracted on both sides;
--no-openings compares uncut geometry (for readers that don't cut them).
Exit code 1 on any mismatch.
"""
import json
import sys

import ifcopenshell
import ifcopenshell.geom
import ifcopenshell.util.unit as uu

def local_volume(geometry):
    """Signed mesh volume (m3) computed relative to the mesh's own first vertex.

    ifcopenshell.util.shape.get_volume sums terms in world coordinates; with real site
    coordinates (millions of metres) those terms cancel catastrophically and the result can
    exceed the bounding-box volume by orders of magnitude. Shifting to a local origin first
    keeps full precision.
    """
    v, f = geometry.verts, geometry.faces
    if not f:
        return 0.0
    ox, oy, oz = v[0], v[1], v[2]
    total = 0.0
    for i in range(0, len(f), 3):
        a, b, c = (f[i + k] * 3 for k in range(3))
        ax, ay, az = v[a] - ox, v[a + 1] - oy, v[a + 2] - oz
        bx, by, bz = v[b] - ox, v[b + 1] - oy, v[b + 2] - oz
        cx, cy, cz = v[c] - ox, v[c + 1] - oy, v[c + 2] - oz
        total += ax * (by * cz - bz * cy) - ay * (bx * cz - bz * cx) + az * (bx * cy - by * cx)
    return total / 6.0


VOL_TOL = 5e-3
BBOX_TOL_MM = 1.0


def main(ifc_path, ours_path, no_openings=False):
    ours = {}
    for line in open(ours_path):
        e = json.loads(line)
        ours[e["global_id"]] = e

    f = ifcopenshell.open(ifc_path)
    to_mm = 1000.0  # IfcOpenShell geometry is in metres
    s = ifcopenshell.geom.settings()
    s.set("use-world-coords", True)
    if no_openings:
        s.set("disable-opening-subtractions", True)
    it = ifcopenshell.geom.iterator(s, f, 2)
    theirs = {}
    assert it.initialize()
    while True:
        sh = it.get()
        el = f.by_id(sh.id)
        if not el.is_a("IfcOpeningElement"):
            v = sh.geometry.verts
            xs, ys, zs = v[0::3], v[1::3], v[2::3]
            theirs[el.GlobalId] = {
                "class": el.is_a(),
                "volume_mm3": local_volume(sh.geometry) * to_mm**3,
                "bbox_min_mm": [min(xs) * to_mm, min(ys) * to_mm, min(zs) * to_mm],
                "bbox_max_mm": [max(xs) * to_mm, max(ys) * to_mm, max(zs) * to_mm],
            }
        if not it.next():
            break

    problems = []
    only_ours = sorted(set(ours) - set(theirs))
    only_theirs = sorted(set(theirs) - set(ours))
    for g in only_ours:
        problems.append(f"only in Bimble: {g} {ours[g]['class']} {ours[g]['name']}")
    for g in only_theirs:
        problems.append(f"only in IfcOpenShell: {g} {theirs[g]['class']}")

    worst_vol = (0.0, None)
    worst_bb = (0.0, None)
    for g in sorted(set(ours) & set(theirs)):
        a, b = ours[g], theirs[g]
        if a["class"] != b["class"]:
            problems.append(f"{g}: class {a['class']} != {b['class']}")
        vb = abs(b["volume_mm3"])
        # Flat or open surfaces (plates drawn as sheets, symbols) have ~zero volume on both
        # sides; compare those against a scale from the bounding box, not a 1 mm3 floor.
        size = [hi - lo for lo, hi in zip(a["bbox_min_mm"], a["bbox_max_mm"])]
        bbox_vol = max(size[0] * size[1] * size[2], 1.0)
        rel = abs(abs(a["volume_mm3"]) - vb) / max(vb, 1e-3 * bbox_vol, 1.0)
        if rel > worst_vol[0]:
            worst_vol = (rel, g)
        if rel > VOL_TOL:
            problems.append(
                f"{g} {a['class']} {a['name']}: volume {a['volume_mm3']:.4g} vs {b['volume_mm3']:.4g} mm3 ({rel:.2%})"
            )
        d = max(
            max(abs(x - y) for x, y in zip(a["bbox_min_mm"], b["bbox_min_mm"])),
            max(abs(x - y) for x, y in zip(a["bbox_max_mm"], b["bbox_max_mm"])),
        )
        if d > worst_bb[0]:
            worst_bb = (d, g)
        if d > BBOX_TOL_MM:
            problems.append(f"{g} {a['class']} {a['name']}: bounding box off by {d:.1f} mm")

    common = len(set(ours) & set(theirs))
    print(f"elements: Bimble {len(ours)}, IfcOpenShell {len(theirs)}, common {common}")
    print(f"worst volume difference: {worst_vol[0]:.4%} ({worst_vol[1]})")
    print(f"worst bounding-box difference: {worst_bb[0]:.3f} mm ({worst_bb[1]})")
    def box_vol(e):
        s = [hi - lo for lo, hi in zip(e["bbox_min_mm"], e["bbox_max_mm"])]
        return s[0] * s[1] * s[2]

    # Inward-facing solids only; a flat sheet's sign means nothing.
    neg = [g for g, e in ours.items() if e["volume_mm3"] < -1e-3 * box_vol(e)]
    if neg:
        problems.append(f"{len(neg)} element(s) with inward-facing meshes (negative volume), e.g. {neg[0]}")
    if problems:
        print("\n".join(problems[:40]))
        print(f"FAILED: {len(problems)} problem(s)")
        sys.exit(1)
    print("OK")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], "--no-openings" in sys.argv[3:])
