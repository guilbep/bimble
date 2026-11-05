"""Test oracle: open a Bimble IFC export with IfcOpenShell and check it.

Usage: python3 scripts/check_ifc.py model.bimble model.ifc

Checks, against the .bimble source:
  - schema is IFC4, units are mm
  - every element exists with the same GlobalId, IFC class and name
  - spatial containment matches the element's storey
  - Bimble_Generator pset parameters match the source generator
  - IfcOpenShell's geometry engine builds every element, and its volume matches
    the analytic volume of the generator within 0.01 %
"""
import json
import sys

import ifcopenshell
import ifcopenshell.geom
import ifcopenshell.util.element as eu
import ifcopenshell.util.shape as su
import ifcopenshell.util.unit as uu


def analytic_volume_mm3(g):
    k = g["kind"]
    if k in ("rect_column", "pad_footing"):
        return g["width"] * g["depth"] * g["height"]
    if k == "rect_beam":
        return g["width"] * g["height"] * g["length"]
    if k == "wall":
        return g["length"] * g["thickness"] * g["height"]
    if k == "slab":
        pts = g["outline"]
        a2 = sum(p[0] * q[1] - q[0] * p[1] for p, q in zip(pts, pts[1:] + pts[:1]))
        return a2 / 2 * g["thickness"]
    raise ValueError(k)


def main(bimble_path, ifc_path):
    src = json.load(open(bimble_path))["model"]
    f = ifcopenshell.open(ifc_path)
    errors = []
    assert f.schema == "IFC4", f.schema
    scale = uu.calculate_unit_scale(f)
    assert abs(scale - 0.001) < 1e-12, f"length unit scale {scale}, expected mm"

    settings = ifcopenshell.geom.settings()
    settings.set("use-world-coords", True)
    storeys = src["storeys"]

    for gid, e in src["elements"].items():
        el = f.by_guid(gid)
        if el is None:
            errors.append(f"missing {gid} {e['name']}")
            continue
        if el.is_a() != e["class"] or el.Name != e["name"]:
            errors.append(f"{gid}: {el.is_a()} {el.Name} != {e['class']} {e['name']}")
        container = eu.get_container(el)
        if container is None or container.GlobalId != e["storey"]:
            errors.append(f"{e['name']}: wrong container {container}")
        pset = eu.get_pset(el, "Bimble_Generator")
        if not pset or json.loads(pset["Parameters"]) != e["generator"]:
            errors.append(f"{e['name']}: generator pset mismatch")
        try:
            shape = ifcopenshell.geom.create_shape(settings, el)
        except Exception as ex:  # noqa: BLE001
            errors.append(f"{e['name']}: geometry failed: {ex}")
            continue
        # IfcOpenShell geometry is in metres.
        vol_mm3 = su.get_volume(shape.geometry) * 1e9
        want = analytic_volume_mm3(e["generator"])
        if abs(vol_mm3 - want) > 1e-4 * want:
            errors.append(f"{e['name']}: volume {vol_mm3:.0f} mm3 != {want:.0f} mm3")
        # Bottom of a column sits on its storey level.
        if e["class"] == "IfcColumn":
            zmin_mm = min(shape.geometry.verts[2::3]) * 1000
            want_z = storeys[e["storey"]]["elevation"] + e["placement"]["origin"][2]
            if abs(zmin_mm - want_z) > 1e-3:
                errors.append(f"{e['name']}: base z {zmin_mm} != {want_z}")

    n_ifc = len(f.by_type("IfcBuildingElement"))
    if n_ifc != len(src["elements"]):
        errors.append(f"IFC has {n_ifc} building elements, source has {len(src['elements'])}")

    if errors:
        print("\n".join(errors[:50]))
        print(f"FAILED: {len(errors)} problems")
        sys.exit(1)
    print(f"OK: {len(src['elements'])} elements checked with IfcOpenShell {ifcopenshell.version}")


if __name__ == "__main__":
    main(*sys.argv[1:3])
