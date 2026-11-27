#!/usr/bin/env python3
"""Measure IfcOpenShell parse and geometry on one IFC, with the settings Bonsai's importer uses.

This is the engine under Bonsai's "Load Project", without Blender: no Blender objects
or meshes are created, so it is a lower bound for Bonsai, never a "Bonsai" number.

Settings copied from Bonsai 0.9.0.post1 (bonsai/tool/loader.py create_settings,
bonsai/bim/import_ifc.py IfcImportSettings + create_products, bim/module/project/prop.py):
  geometry library  hybrid-cgal-simple-opencascade   (prop.py geometry_library default)
  threads           multiprocessing.cpu_count()       (should_use_cpu_multiprocessing=True)
  one iterator per prioritised context, an element drawn in an earlier context is not
  sent to the next one (import_ifc.py create_generic_elements)
  mesher-linear-deflection 0.05, mesher-angular-deflection 0.5, CURVES_SURFACES_AND_SOLIDS,
  apply-default-materials False, keep-bounding-boxes True, layerset-first True,
  no-wire-intersection-check True, world coords off (local mesh + placement matrix,
  so mapped items share one geometry id).
  Elements with more than 30 openings ("gross") are meshed without opening cuts.
Element set as Bonsai's process_element_filter: IfcElement (+ IfcProxy in IFC2X3/IFC4)
minus IfcFeatureElement (IfcSurfaceFeature kept); then the spatial elements.
Annotations and element types (also loaded by Bonsai) are left out.

Usage: python -I ios_geom_bench.py <file.ifc> [--threads N] [--library L] [--json out.json]
"""

import argparse
import collections
import json
import multiprocessing
import resource
import sys
import time


def rss_mb():
    # ru_maxrss is in kB on Linux, in bytes on macOS.
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / (1024.0 * 1024.0 if sys.platform == "darwin" else 1024.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ifc")
    ap.add_argument("--threads", type=int, default=multiprocessing.cpu_count())
    ap.add_argument("--library", default="hybrid-cgal-simple-opencascade")
    ap.add_argument("--json")
    ap.add_argument("--parse-only", action="store_true")
    a = ap.parse_args()

    import ifcopenshell
    import ifcopenshell.geom
    import ifcopenshell.util.representation
    import ifcopenshell.util.shape

    out = {"ifcopenshell": ifcopenshell.version, "threads": a.threads, "library": a.library}
    rss0 = rss_mb()
    t0 = time.perf_counter()
    f = ifcopenshell.open(a.ifc)
    out["parse_s"] = time.perf_counter() - t0
    out["parse_peak_rss_mb"] = rss_mb()
    out["rss_before_open_mb"] = rss0
    out["schema"] = f.schema
    print(f"parse {out['parse_s']:.2f} s, peak RSS {out['parse_peak_rss_mb']:.0f} MB", flush=True)
    if a.parse_only:
        print(json.dumps(out))
        return

    if f.schema in ("IFC2X3", "IFC4"):
        elements = f.by_type("IfcElement") + f.by_type("IfcProxy")
    else:
        elements = f.by_type("IfcElement")
    elements = {e for e in elements if not e.is_a("IfcFeatureElement") or e.is_a("IfcSurfaceFeature")}
    gross = {e for e in elements if len(getattr(e, "HasOpenings", []) or []) > 30}
    elements -= gross
    spatial = set(f.by_type("IfcSpatialStructureElement" if f.schema == "IFC2X3" else "IfcSpatialElement"))
    out["elements"] = len(elements) + len(gross)
    out["gross_elements"] = len(gross)
    out["spatial_elements"] = len(spatial)
    out["elements_with_representation"] = sum(1 for e in elements | gross if e.Representation)

    contexts = ifcopenshell.util.representation.get_prioritised_contexts(f)

    def make_settings(context, is_gross):
        s = ifcopenshell.geom.settings()
        s.set("mesher-linear-deflection", 0.05)
        s.set("mesher-angular-deflection", 0.5)
        s.set("dimensionality", ifcopenshell.ifcopenshell_wrapper.CURVES_SURFACES_AND_SOLIDS)
        s.set("context-ids", [context.id()])
        s.set("apply-default-materials", False)
        s.set("keep-bounding-boxes", True)
        s.set("layerset-first", True)
        s.set("no-wire-intersection-check", True)
        if is_gross:
            s.set("disable-opening-subtractions", True)
        return s

    shapes = 0
    tris = 0
    by_class = collections.Counter()
    tris_by_class = collections.Counter()
    body_by_class = collections.Counter()
    body_tris_by_class = collections.Counter()
    geom_ids = {}
    drawn = set()
    t1 = time.perf_counter()
    init_s = 0.0

    def run(products, is_gross):
        nonlocal shapes, tris, init_s
        products = set(products)
        for ctx in contexts:
            if not products or not any(p.Representation for p in products):
                break
            it = ifcopenshell.geom.iterator(
                make_settings(ctx, is_gross), f, a.threads, include=list(products), geometry_library=a.library
            )
            ti = time.perf_counter()
            ok = it.initialize()
            init_s += time.perf_counter() - ti
            if not ok:
                continue
            done = set()
            is_body = (ctx.ContextIdentifier or "") == "Body" or (
                ctx.is_a("IfcGeometricRepresentationSubContext") is False and (ctx.ContextType or "") == "Model"
            )
            while True:
                sh = it.get()
                if sh:
                    el = f.by_id(sh.id)
                    done.add(el)
                    g = sh.geometry
                    n = len(g.faces) // 3
                    shapes += 1
                    tris += n
                    cls = el.is_a()
                    by_class[cls] += 1
                    tris_by_class[cls] += n
                    if is_body:
                        body_by_class[cls] += 1
                        body_tris_by_class[cls] += n
                    geom_ids.setdefault(g.id, n)
                if not it.next():
                    break
            drawn.update(done)
            products -= done

    run(elements, False)
    run(gross, True)
    n_el_shapes = shapes
    run(spatial, False)
    out["geometry_s"] = time.perf_counter() - t1
    out["iterator_initialize_s"] = init_s
    out["geometry_peak_rss_mb"] = rss_mb()
    out["shapes"] = shapes
    out["element_shapes"] = n_el_shapes
    out["triangles"] = tris
    out["unique_geometries"] = len(geom_ids)
    out["unique_geometry_triangles"] = sum(geom_ids.values())
    out["contexts"] = [f"{c.ContextType}/{c.ContextIdentifier}/{c.TargetView}" if c.is_a("IfcGeometricRepresentationSubContext") else f"{c.ContextType}" for c in contexts]
    with_rep = {e for e in (elements | gross) if e.Representation}
    not_drawn = collections.Counter(e.is_a() for e in with_rep - drawn)
    out["elements_with_representation_not_drawn"] = dict(not_drawn.most_common())
    out["shapes_by_class"] = dict(by_class.most_common())
    out["triangles_by_class"] = dict(tris_by_class.most_common())
    out["body_shapes_by_class"] = dict(body_by_class.most_common())
    out["body_triangles_by_class"] = dict(body_tris_by_class.most_common())
    out["total_s"] = time.perf_counter() - t0
    print(
        f"geometry {out['geometry_s']:.2f} s, shapes {shapes}, triangles {tris}, "
        f"unique geometries {len(geom_ids)}, peak RSS {out['geometry_peak_rss_mb']:.0f} MB",
        flush=True,
    )
    if a.json:
        with open(a.json, "w") as fh:
            json.dump(out, fh, indent=1, sort_keys=True)
    else:
        print(json.dumps(out, sort_keys=True))


if __name__ == "__main__":
    sys.exit(main())
