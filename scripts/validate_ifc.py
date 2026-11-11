"""Schema check of IFC files with IfcOpenShell's validator: every entity's attributes against
its schema (IFC4 or IFC2X3, from the file). Exits 1 when a file has an issue.

Usage: python3 -I scripts/validate_ifc.py a.ifc [b.ifc …]
"""

import sys

import ifcopenshell
import ifcopenshell.validate


def main(paths):
    bad = 0
    for p in paths:
        f = ifcopenshell.open(p)
        log = ifcopenshell.validate.json_logger()
        ifcopenshell.validate.validate(f, log)
        print(f"{p}: {f.schema}, {len(log.statements)} issue(s)")
        for s in log.statements[:10]:
            print("  ", s.get("instance"), s.get("attribute"), str(s.get("message"))[:200])
        bad += bool(log.statements)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
