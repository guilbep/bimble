"""Time Bonsai loading one IFC in a background Blender.

Run:
  blender -b --python bonsai_load_bench.py -- <file.ifc> [--mode load|link] [--save out.blend]
          [--json out.json] [--force-large]

--mode load (default): bpy.ops.bim.load_project(filepath=...) with the operator's defaults (fresh
  session, not advanced). Bonsai then stops before creating objects when the file has more than
  30,000 IfcElement ("large model. Please review advanced settings"): the script reports that, and
  with --force-large it then calls bpy.ops.bim.load_project_elements() with the default advanced
  settings (what the "Load Project Elements" button does), timed apart.
--mode link: bpy.ops.bim.link_ifc(filepath=...) with defaults (use_cache=True). The first run
  starts a second background Blender that tessellates and writes <file>.ifc.cache.blend,
  <file>.ifc.cache.sqlite (properties) and <file>.ifc.cache.json beside the IFC; a later run reads
  the cache. The cache files are written beside the IFC: run on a copy.

Prints one line "BONSAI_BENCH {json}": wall times, peak RSS of this Blender (RUSAGE_SELF) and of
the largest waited-for child (RUSAGE_CHILDREN: the link's tessellating Blender), object and mesh
counts, triangles (per object, instanced meshes counted at each use) and the cache sizes.
Tested with Bonsai 0.9.0.post1 on Blender 5.3.0.
"""

import json
import os
import resource
import sys
import time

import bpy


def rss_mb(who):
    # ru_maxrss is in kB on Linux, in bytes on macOS.
    return resource.getrusage(who).ru_maxrss / (1024.0 * 1024.0 if sys.platform == "darwin" else 1024.0)


def args():
    argv = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else []
    out = {"ifc": None, "mode": "load", "save": None, "json": None, "force_large": False}
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--mode":
            out["mode"] = argv[i + 1]
            i += 1
        elif a == "--save":
            out["save"] = argv[i + 1]
            i += 1
        elif a == "--json":
            out["json"] = argv[i + 1]
            i += 1
        elif a == "--force-large":
            out["force_large"] = True
        else:
            out["ifc"] = os.path.abspath(a)
        i += 1
    if not out["ifc"]:
        raise SystemExit("usage: blender -b --python bonsai_load_bench.py -- <file.ifc> [--mode load|link]")
    return out


def ensure_bonsai():
    import addon_utils

    names = [m.__name__ for m in addon_utils.modules() if m.__name__.endswith("bonsai")]
    if not names:
        raise SystemExit("Bonsai is not installed in this Blender")
    for n in names:
        if not addon_utils.check(n)[1]:
            addon_utils.enable(n, default_set=True)
    if not hasattr(bpy.ops.bim, "load_project"):
        raise SystemExit("Bonsai did not register (bpy.ops.bim.load_project missing)")
    return names[0]


def scene_counts(objects):
    objs = 0
    mesh_objs = 0
    tris = 0
    meshes = set()
    for ob in objects:
        objs += 1
        if ob.type == "MESH" and ob.data is not None:
            mesh_objs += 1
            me = ob.data
            meshes.add(me.name_full)
            n = len(me.polygons)
            if n:
                tot = [0] * n
                me.polygons.foreach_get("loop_total", tot)
                tris += sum(t - 2 for t in tot)
    return {"objects": objs, "mesh_objects": mesh_objs, "meshes_used": len(meshes), "triangles": tris}


def all_scene_objects():
    # Objects reachable in the scene, including the contents of instanced (linked) collections.
    seen = []

    def walk(coll):
        for ob in coll.objects:
            seen.append(ob)
            if ob.instance_type == "COLLECTION" and ob.instance_collection:
                walk_inst(ob.instance_collection)
        for c in coll.children:
            walk(c)

    def walk_inst(coll):
        for ob in coll.all_objects:
            seen.append(ob)

    walk(bpy.context.scene.collection)
    return seen


def main():
    try:
        sys.stdout.reconfigure(line_buffering=True)  # keep the progress if Blender dies
    except Exception:
        pass
    a = args()
    res = {"ifc_bytes": os.path.getsize(a["ifc"]), "mode": a["mode"], "blender": bpy.app.version_string}
    t = time.perf_counter()
    addon = ensure_bonsai()
    res["bonsai_enable_s"] = time.perf_counter() - t
    import bonsai

    res["bonsai"] = ".".join(str(v) for v in getattr(bonsai, "bl_info", {}).get("version", ())) or None
    try:
        import tomllib

        mf = os.path.join(os.path.dirname(os.path.dirname(bonsai.__file__)), "blender_manifest.toml")
        for cand in (mf, os.path.join(os.path.dirname(bonsai.__file__), "blender_manifest.toml")):
            if os.path.exists(cand):
                res["bonsai"] = tomllib.load(open(cand, "rb"))["version"]
    except Exception:
        pass
    import ifcopenshell

    res["ifcopenshell"] = ifcopenshell.version
    res["rss_after_start_mb"] = rss_mb(resource.RUSAGE_SELF)

    if a["mode"] == "load":
        t = time.perf_counter()
        bpy.ops.bim.load_project(filepath=a["ifc"])
        res["load_project_s"] = time.perf_counter() - t
        import bonsai.tool as tool

        f = tool.Ifc.get()
        n_el = len(f.by_type("IfcElement")) if f else None
        res["ifc_elements"] = n_el
        res["stopped_as_large_model"] = bool(n_el and n_el > 30000)
        res.update(scene_counts(bpy.data.objects))
        res["rss_after_load_project_mb"] = rss_mb(resource.RUSAGE_SELF)
        if res["stopped_as_large_model"] and a["force_large"]:
            print(f"BENCH phase: load_project stopped (large model) after {res['load_project_s']:.1f} s, RSS {res['rss_after_load_project_mb']:.0f} MB; load_project_elements", flush=True)
            t = time.perf_counter()
            bpy.ops.bim.load_project_elements()
            res["load_project_elements_s"] = time.perf_counter() - t
            res.update(scene_counts(bpy.data.objects))
        res["blender_meshes"] = len(bpy.data.meshes)
        res["total_s"] = res["load_project_s"] + res.get("load_project_elements_s", 0.0)
    elif a["mode"] == "link":
        base = a["ifc"]
        cache = {k: base + k for k in (".cache.blend", ".cache.sqlite", ".cache.json")}
        res["cache_existed"] = os.path.exists(cache[".cache.blend"])
        t = time.perf_counter()
        bpy.ops.bim.link_ifc(filepath=a["ifc"])
        res["link_s"] = time.perf_counter() - t
        res["total_s"] = res["link_s"]
        res["cache_bytes"] = {k: (os.path.getsize(p) if os.path.exists(p) else None) for k, p in cache.items()}
        res.update(scene_counts(all_scene_objects()))
        res["blender_meshes"] = len(bpy.data.meshes)
    else:
        raise SystemExit("unknown --mode")

    res["peak_rss_self_mb"] = rss_mb(resource.RUSAGE_SELF)
    res["peak_rss_children_mb"] = rss_mb(resource.RUSAGE_CHILDREN)

    print(f"BENCH phase: loaded, peak RSS {res['peak_rss_self_mb']:.0f} MB", flush=True)
    if a["save"]:
        t = time.perf_counter()
        bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(a["save"]))
        res["save_s"] = time.perf_counter() - t
        res["blend_bytes"] = os.path.getsize(os.path.abspath(a["save"]))

    line = json.dumps(res, sort_keys=True)
    print("BONSAI_BENCH " + line, flush=True)
    if a["json"]:
        with open(a["json"], "w") as fh:
            fh.write(line + "\n")


try:
    main()
except SystemExit:
    raise
except Exception:
    import traceback

    traceback.print_exc()
    sys.exit(1)
