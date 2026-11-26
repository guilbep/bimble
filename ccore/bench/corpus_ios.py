"""IfcOpenShell's count of products with geometry in one IFC file (bench/corpus.py ios): the
products it triangulates to at least one face, without the classes the C reader skips
(src/ifc_geom.h SKIP_CLASSES). Prints one JSON line. Run with the venv's python, `-I`."""
import json, multiprocessing, sys
import ifcopenshell, ifcopenshell.geom

SKIP = {"IFCOPENINGELEMENT", "IFCOPENINGSTANDARDCASE", "IFCSPACE", "IFCSITE", "IFCBUILDING",
        "IFCBUILDINGSTOREY", "IFCVIRTUALELEMENT", "IFCANNOTATION", "IFCGRID", "IFCSPATIALZONE",
        "IFCEXTERNALSPATIALELEMENT"}

f = ifcopenshell.open(sys.argv[1])
s = ifcopenshell.geom.settings()
it = ifcopenshell.geom.iterator(s, f, multiprocessing.cpu_count())
withg, tris = set(), 0
if it.initialize():
    while True:
        sh = it.get()
        if sh.type.upper() not in SKIP and len(sh.geometry.faces) > 0:
            withg.add(sh.id)
            tris += len(sh.geometry.faces) // 3
        if not it.next():
            break
print(json.dumps({"with_geometry": len(withg), "triangles": tris, "schema": f.schema}))
