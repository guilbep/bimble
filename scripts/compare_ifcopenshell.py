"""Compare Bimble's IFC reader against IfcOpenShell, element by element.

Usage:
  bimble info model.ifc --elements-json > ours.jsonl
  python3 scripts/compare_ifcopenshell.py model.ifc ours.jsonl

Checks, per GlobalId: same set of elements, same IFC class, volume within 0.5 %,
world bounding box within 1 mm. Openings are not subtracted on either side
(Bimble doesn't cut openings yet), so geometry is compared like for like.
Exit code 1 on any mismatch.
"""
import json
import sys

import ifcopenshell
import ifcopenshell.geom
import ifcopenshell.util.shape as su
import ifcopenshell.util.unit as uu

VOL_TOL = 5e-3
BBOX_TOL_MM = 1.0


def main(ifc_path, ours_path):
    ours = {}
    for line in open(ours_path):
        e = json.loads(line)
        ours[e["global_id"]] = e

    f = ifcopenshell.open(ifc_path)
    to_mm = 1000.0  # IfcOpenShell geometry is in metres
    s = ifcopenshell.geom.settings()
    s.set("use-world-coords", True)
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
                "volume_mm3": su.get_volume(sh.geometry) * to_mm**3,
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
        rel = abs(abs(a["volume_mm3"]) - vb) / max(vb, 1.0)
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
    neg = [g for g, e in ours.items() if e["volume_mm3"] < 0]
    if neg:
        problems.append(f"{len(neg)} element(s) with inward-facing meshes (negative volume), e.g. {neg[0]}")
    if problems:
        print("\n".join(problems[:40]))
        print(f"FAILED: {len(problems)} problem(s)")
        sys.exit(1)
    print("OK")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
