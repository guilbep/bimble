"""Synthetic IFC files, one body kind each, for the tessellation study (ticket TESS1, ADR 0085).

    python3 ccore/bench/tess1_synth.py <out-dir>

Writes <out-dir>/<kind>.ifc (IFC4, millimetres), each element named after what it isolates:
circles of several radii, profiles with arcs and fillets, voids, revolved and swept-disk solids,
half-space clippings, openings, faceted and indexed meshes, mapped copies, a body given in two
contexts. Synthetic only; deterministic (the same bytes on every run); GlobalIds are
BLAKE2b("bimble-tess1/<file>/<n>") in the IFC alphabet. Compare with IfcOpenShell through
ccore/bench/tess1_compare.py.
"""
import hashlib
import math
import os
import sys

ALPHABET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_$"


def f(x):
    """A STEP real: always a dot, no exponent for these magnitudes."""
    s = repr(float(x))
    if "e" in s or "E" in s:
        s = f"{x:.6f}"
    return s if "." in s else s + "."


class Ifc:
    def __init__(self, name, what):
        self.name, self.what = name, what
        self.lines, self.n, self.g = [], 100, 0
        self.products = []

    def gid(self):
        self.g += 1
        h = hashlib.blake2b(f"bimble-tess1/{self.name}/{self.g}".encode(), digest_size=16).digest()
        v = int.from_bytes(h, "big")
        return ALPHABET[v >> 126] + "".join(ALPHABET[(v >> (6 * (20 - i))) & 63] for i in range(21))

    def e(self, text):
        self.n += 1
        self.lines.append(f"#{self.n}={text};")
        return f"#{self.n}"

    def pt(self, *c):
        return self.e(f"IFCCARTESIANPOINT(({','.join(f(x) for x in c)}))")

    def d(self, *c):
        return self.e(f"IFCDIRECTION(({','.join(f(x) for x in c)}))")

    def ax3(self, x=0.0, y=0.0, z=0.0, axis=None, ref=None):
        return self.e(f"IFCAXIS2PLACEMENT3D({self.pt(x, y, z)},{axis or '$'},{ref or '$'})")

    def ax2(self, x=0.0, y=0.0):
        return self.e(f"IFCAXIS2PLACEMENT2D({self.pt(x, y)},$)")

    def extrude(self, prof, h, pos=None, dirn=None):
        return self.e(f"IFCEXTRUDEDAREASOLID({prof},{pos or self.ax3()},{dirn or '#18'},{f(h)})")

    def product(self, cls, name, items, at=(0.0, 0.0, 0.0), rep_type="SweptSolid", extra="", reps=None):
        """A product with a Body of `items` placed at `at`; returns its placement (for openings)."""
        if reps is None:
            reps = [("Body", rep_type, items)]
        rs = [self.e(f"IFCSHAPEREPRESENTATION(#6,'{rid}','{rt}',({','.join(its)}))") for rid, rt, its in reps]
        pds = self.e(f"IFCPRODUCTDEFINITIONSHAPE($,$,({','.join(rs)}))")
        pl = self.e(f"IFCLOCALPLACEMENT(#13,{self.ax3(*at)})")
        tail = {"IFCCOLUMN": ",$,.COLUMN.", "IFCBEAM": ",$,.BEAM.", "IFCSLAB": ",$,.FLOOR.", "IFCWALL": ",$,.STANDARD.",
                "IFCMEMBER": ",$,.MEMBER.", "IFCBUILDINGELEMENTPROXY": ",$,.ELEMENT.", "IFCPIPESEGMENT": ",$,.RIGIDSEGMENT.",
                "IFCREINFORCINGBAR": ",$,$,$,$,$,.MAIN.,$"}[cls]
        el = self.e(f"{cls}('{self.gid()}',$,'{name}',$,$,{pl},{pds}{tail})")
        self.products.append(el)
        return el, pl

    def opening(self, host, host_pl, prof, h, at, axis=None, ref=None):
        rep = self.e(f"IFCSHAPEREPRESENTATION(#6,'Body','SweptSolid',({self.extrude(prof, h)}))")
        pds = self.e(f"IFCPRODUCTDEFINITIONSHAPE($,$,({rep}))")
        pl = self.e(f"IFCLOCALPLACEMENT({host_pl},{self.ax3(*at, axis, ref)})")
        op = self.e(f"IFCOPENINGELEMENT('{self.gid()}',$,'opening',$,$,{pl},{pds},$,.OPENING.)")
        self.e(f"IFCRELVOIDSELEMENT('{self.gid()}',$,$,$,{host},{op})")

    def write(self, path):
        g = [self.gid() for _ in range(8)]
        head = [
            "ISO-10303-21;", "HEADER;",
            f"FILE_DESCRIPTION(('synthetic: {self.what}'),'2;1');",
            f"FILE_NAME('{self.name}','2026-01-01T00:00:00',(''),(''),'tess1_synth.py','bimble bench','');",
            "FILE_SCHEMA(('IFC4'));", "ENDSEC;", "DATA;",
            "#1=IFCSIUNIT(*,.LENGTHUNIT.,.MILLI.,.METRE.);",
            "#2=IFCSIUNIT(*,.PLANEANGLEUNIT.,$,.RADIAN.);",
            "#3=IFCUNITASSIGNMENT((#1,#2));",
            "#4=IFCCARTESIANPOINT((0.,0.,0.));",
            "#5=IFCAXIS2PLACEMENT3D(#4,$,$);",
            "#6=IFCGEOMETRICREPRESENTATIONCONTEXT($,'Model',3,1.E-05,#5,$);",
            f"#7=IFCPROJECT('{g[0]}',$,'{self.name}',$,$,$,$,(#6),#3);",
            "#8=IFCLOCALPLACEMENT($,#5);",
            f"#9=IFCSITE('{g[1]}',$,'Site',$,$,#8,$,$,.ELEMENT.,$,$,$,$,$);",
            f"#10=IFCBUILDING('{g[2]}',$,'Building',$,$,#8,$,$,.ELEMENT.,$,$,$);",
            "#13=IFCLOCALPLACEMENT(#8,#5);",
            f"#14=IFCBUILDINGSTOREY('{g[3]}',$,'Level 0',$,$,#13,$,$,.ELEMENT.,0.);",
            f"#15=IFCRELAGGREGATES('{g[4]}',$,$,$,#7,(#9));",
            f"#16=IFCRELAGGREGATES('{g[5]}',$,$,$,#9,(#10));",
            f"#17=IFCRELAGGREGATES('{g[6]}',$,$,$,#10,(#14));",
            "#18=IFCDIRECTION((0.,0.,1.));",
        ]
        tail = [f"#99999=IFCRELCONTAINEDINSPATIALSTRUCTURE('{g[7]}',$,$,$,({','.join(self.products)}),#14);",
                "ENDSEC;", "END-ISO-10303-21;"]
        with open(path, "w", newline="\n") as fh:
            fh.write("\n".join(head + self.lines + tail) + "\n")


def circles(m):
    """Round columns of several radii (and two round hollow sections)."""
    for i, r in enumerate((10.0, 25.0, 50.0, 100.0, 150.0, 300.0, 600.0, 1500.0, 5000.0)):
        prof = m.e(f"IFCCIRCLEPROFILEDEF(.AREA.,$,{m.ax2()},{f(r)})")
        m.product("IFCCOLUMN", f"circle r{r:g}", [m.extrude(prof, 3000.0)], at=(i * 12000.0, 0.0, 0.0))
    for i, (r, t) in enumerate(((50.0, 5.0), (200.0, 10.0))):
        prof = m.e(f"IFCCIRCLEHOLLOWPROFILEDEF(.AREA.,$,{m.ax2()},{f(r)},{f(t)})")
        m.product("IFCCOLUMN", f"hollow r{r:g}", [m.extrude(prof, 3000.0)], at=(i * 2000.0, 20000.0, 0.0))


def profiles(m):
    """Parametric profiles with fillets, and arbitrary profiles with arcs and voids."""
    ish = m.e(f"IFCISHAPEPROFILEDEF(.AREA.,'HEA300',{m.ax2()},300.,290.,8.5,14.,27.,$,$)")
    m.product("IFCBEAM", "I with fillets", [m.extrude(ish, 6000.0)])
    ish0 = m.e(f"IFCISHAPEPROFILEDEF(.AREA.,'I no fillet',{m.ax2()},300.,290.,8.5,14.,$,$,$)")
    m.product("IFCBEAM", "I without fillets", [m.extrude(ish0, 6000.0)], at=(2000.0, 0.0, 0.0))
    rr = m.e(f"IFCROUNDEDRECTANGLEPROFILEDEF(.AREA.,$,{m.ax2()},400.,300.,40.)")
    m.product("IFCCOLUMN", "rounded rectangle r40", [m.extrude(rr, 3000.0)], at=(4000.0, 0.0, 0.0))
    # an arbitrary profile: a rectangle with two semicircular ends (composite curve of trimmed circles)
    l, w = 4000.0, 1000.0
    a, b, c, dd = m.pt(0.0, 0.0), m.pt(l, 0.0), m.pt(l, w), m.pt(0.0, w)
    seg1 = m.e(f"IFCCOMPOSITECURVESEGMENT(.CONTINUOUS.,.T.,{m.e(f'IFCPOLYLINE(({a},{b}))')})")
    circ_r = m.e(f"IFCCIRCLE({m.ax2(l, w / 2)},{f(w / 2)})")
    tr_r = m.e(f"IFCTRIMMEDCURVE({circ_r},(IFCPARAMETERVALUE({f(-math.pi / 2)})),(IFCPARAMETERVALUE({f(math.pi / 2)})),.T.,.PARAMETER.)")
    seg2 = m.e(f"IFCCOMPOSITECURVESEGMENT(.CONTINUOUS.,.T.,{tr_r})")
    seg3 = m.e(f"IFCCOMPOSITECURVESEGMENT(.CONTINUOUS.,.T.,{m.e(f'IFCPOLYLINE(({c},{dd}))')})")
    circ_l = m.e(f"IFCCIRCLE({m.ax2(0.0, w / 2)},{f(w / 2)})")
    tr_l = m.e(f"IFCTRIMMEDCURVE({circ_l},(IFCPARAMETERVALUE({f(math.pi / 2)})),(IFCPARAMETERVALUE({f(3 * math.pi / 2)})),.T.,.PARAMETER.)")
    seg4 = m.e(f"IFCCOMPOSITECURVESEGMENT(.CONTINUOUS.,.T.,{tr_l})")
    cc = m.e(f"IFCCOMPOSITECURVE(({seg1},{seg2},{seg3},{seg4}),.F.)")
    stadium = m.e(f"IFCARBITRARYCLOSEDPROFILEDEF(.AREA.,'stadium',{cc})")
    m.product("IFCSLAB", "stadium (composite curve arcs)", [m.extrude(stadium, 250.0)], at=(0.0, 4000.0, 0.0))
    # an indexed poly curve with an arc (IFC4)
    pl = m.e("IFCCARTESIANPOINTLIST2D(((0.,0.),(4000.,0.),(4000.,3000.),(2000.,5000.),(0.,3000.)),$)")
    ipc = m.e(f"IFCINDEXEDPOLYCURVE({pl},(IFCLINEINDEX((1,2,3)),IFCARCINDEX((3,4,5)),IFCLINEINDEX((5,1))),.F.)")
    dslab = m.e(f"IFCARBITRARYCLOSEDPROFILEDEF(.AREA.,'D outline',{ipc})")
    m.product("IFCSLAB", "D slab (indexed arc r2000)", [m.extrude(dslab, 250.0)], at=(6000.0, 4000.0, 0.0))
    # a slab with round voids: a rectangle with 4 circles r 100
    outer = m.e(f"IFCPOLYLINE(({m.pt(0., 0.)},{m.pt(3000., 0.)},{m.pt(3000., 2000.)},{m.pt(0., 2000.)},{m.pt(0., 0.)}))")
    inner = [m.e(f"IFCCIRCLE({m.ax2(600.0 + 600.0 * k, 1000.0)},100.)") for k in range(4)]
    wv = m.e(f"IFCARBITRARYPROFILEDEFWITHVOIDS(.AREA.,'voids',{outer},({','.join(inner)}))")
    m.product("IFCSLAB", "slab with 4 round voids r100", [m.extrude(wv, 250.0)], at=(12000.0, 4000.0, 0.0))


def revolved(m):
    """A revolved dome-like ring and a full turn."""
    prof = m.e(f"IFCRECTANGLEPROFILEDEF(.AREA.,$,{m.ax2(1000.0, 0.0)},200.,400.)")
    axis = m.e(f"IFCAXIS1PLACEMENT({m.pt(0., 0., 0.)},{m.d(0., 1., 0.)})")
    full = m.e(f"IFCREVOLVEDAREASOLID({prof},{m.ax3()},{axis},{f(2 * math.pi)})")
    m.product("IFCBUILDINGELEMENTPROXY", "revolved full r1100", [full])
    half = m.e(f"IFCREVOLVEDAREASOLID({prof},{m.ax3()},{axis},{f(math.pi / 2)})")
    m.product("IFCBUILDINGELEMENTPROXY", "revolved quarter", [half], at=(4000.0, 0.0, 0.0))


def swept(m):
    """Pipes and rebar: straight, with bends (an arc in a composite curve)."""
    line = m.e(f"IFCPOLYLINE(({m.pt(0., 0., 0.)},{m.pt(6000., 0., 0.)}))")
    m.product("IFCPIPESEGMENT", "pipe r50 straight", [m.e(f"IFCSWEPTDISKSOLID({line},50.,$,$,$)")])
    m.product("IFCREINFORCINGBAR", "bar r6 straight", [m.e(f"IFCSWEPTDISKSOLID({line},6.,$,$,$)")], at=(0.0, 500.0, 0.0))
    # an L bar: leg, a 90 degree bend r 48 (4 d), leg
    p0, p1 = m.pt(0., 0., 0.), m.pt(1000., 0., 0.)
    leg1 = m.e(f"IFCCOMPOSITECURVESEGMENT(.CONTINUOUS.,.T.,{m.e(f'IFCPOLYLINE(({p0},{p1}))')})")
    circ = m.e(f"IFCCIRCLE({m.ax3(1000.0, 48.0, 0.0)},48.)")
    bend = m.e(f"IFCTRIMMEDCURVE({circ},(IFCPARAMETERVALUE({f(-math.pi / 2)})),(IFCPARAMETERVALUE(0.)),.T.,.PARAMETER.)")
    seg = m.e(f"IFCCOMPOSITECURVESEGMENT(.CONTINUOUS.,.T.,{bend})")
    p2, p3 = m.pt(1048., 48., 0.), m.pt(1048., 1048., 0.)
    leg2 = m.e(f"IFCCOMPOSITECURVESEGMENT(.CONTINUOUS.,.T.,{m.e(f'IFCPOLYLINE(({p2},{p3}))')})")
    cc = m.e(f"IFCCOMPOSITECURVE(({leg1},{seg},{leg2}),.F.)")
    m.product("IFCREINFORCINGBAR", "bar r6 L bend r48", [m.e(f"IFCSWEPTDISKSOLID({cc},6.,$,$,$)")], at=(0.0, 1000.0, 0.0))
    m.product("IFCPIPESEGMENT", "pipe r50 L bend r48", [m.e(f"IFCSWEPTDISKSOLID({cc},50.,$,$,$)")], at=(0.0, 3000.0, 0.0))


def clipping(m):
    """Walls clipped by a half-space (a sloped top) and by a polygonal bounded half-space."""
    rect = m.e(f"IFCRECTANGLEPROFILEDEF(.AREA.,$,{m.ax2(2500.0, 100.0)},5000.,200.)")
    body = m.extrude(rect, 3000.0)
    plane = m.e(f"IFCPLANE({m.ax3(0.0, 0.0, 2500.0, m.d(-0.2, 0.0, 1.0))})")
    hs = m.e(f"IFCHALFSPACESOLID({plane},.F.)")
    clip = m.e(f"IFCBOOLEANCLIPPINGRESULT(.DIFFERENCE.,{body},{hs})")
    m.product("IFCWALL", "wall sloped top (half-space)", [clip], rep_type="Clipping")
    body2 = m.extrude(rect, 3000.0)
    plane2 = m.e(f"IFCPLANE({m.ax3(0.0, 0.0, 2500.0, m.d(-0.2, 0.0, 1.0))})")
    bnd = m.e(f"IFCPOLYLINE(({m.pt(-100., -100.)},{m.pt(2500., -100.)},{m.pt(2500., 400.)},{m.pt(-100., 400.)},{m.pt(-100., -100.)}))")
    pbh = m.e(f"IFCPOLYGONALBOUNDEDHALFSPACE({plane2},.F.,{m.ax3()},{bnd})")
    clip2 = m.e(f"IFCBOOLEANCLIPPINGRESULT(.DIFFERENCE.,{body2},{pbh})")
    m.product("IFCWALL", "wall half-clipped (polygonal bounded)", [clip2], rep_type="Clipping", at=(0.0, 2000.0, 0.0))
    # a beam cut by two end planes (mitres), as Revit writes them
    beam = m.e(f"IFCRECTANGLEPROFILEDEF(.AREA.,$,{m.ax2()},300.,600.)")
    b = m.extrude(beam, 6000.0, pos=m.ax3(0.0, 0.0, 0.0, m.d(1.0, 0.0, 0.0), m.d(0.0, 0.0, 1.0)))
    for k, (x, nx) in enumerate(((0.0, -1.0), (6000.0, 1.0))):
        pln = m.e(f"IFCPLANE({m.ax3(x, 0.0, 0.0, m.d(nx, 0.3 * nx, 0.0))})")
        b = m.e(f"IFCBOOLEANCLIPPINGRESULT(.DIFFERENCE.,{b},{m.e(f'IFCHALFSPACESOLID({pln},.F.)')})")
    m.product("IFCBEAM", "beam mitred both ends", [b], rep_type="Clipping", at=(0.0, 4000.0, 0.0))


def openings(m):
    """Openings: one window, a door to the floor, six windows, a slab with 20 holes, a round hole."""
    rect = m.e(f"IFCRECTANGLEPROFILEDEF(.AREA.,$,{m.ax2(2500.0, 100.0)},5000.,200.)")
    win = m.e(f"IFCRECTANGLEPROFILEDEF(.AREA.,$,{m.ax2(600.0, 100.0)},1200.,400.)")
    w, pl = m.product("IFCWALL", "wall 1 window", [m.extrude(rect, 2800.0)])
    m.opening(w, pl, win, 1400.0, (1500.0, 0.0, 900.0))
    w, pl = m.product("IFCWALL", "wall 1 door to the floor", [m.extrude(rect, 2800.0)], at=(0.0, 1000.0, 0.0))
    door = m.e(f"IFCRECTANGLEPROFILEDEF(.AREA.,$,{m.ax2(450.0, 100.0)},900.,400.)")
    m.opening(w, pl, door, 2100.0, (2000.0, 0.0, 0.0))
    w, pl = m.product("IFCWALL", "wall 3 windows 1 door", [m.extrude(rect, 2800.0)], at=(0.0, 2000.0, 0.0))
    for k in range(3):
        m.opening(w, pl, m.e(f"IFCRECTANGLEPROFILEDEF(.AREA.,$,{m.ax2(300.0, 100.0)},600.,400.)"), 1200.0,
                  (200.0 + 1200.0 * k, 0.0, 1000.0))
    m.opening(w, pl, door, 2100.0, (3900.0, 0.0, 0.0))
    # a door flush with the wall's end: the opening shares the end face
    w, pl = m.product("IFCWALL", "wall door at its end", [m.extrude(rect, 2800.0)], at=(0.0, 3000.0, 0.0))
    m.opening(w, pl, door, 2100.0, (4100.0, 0.0, 0.0))
    # a slab with 20 small holes (sleeves), 5 square, 15 round r 50
    sl = m.e(f"IFCRECTANGLEPROFILEDEF(.AREA.,$,{m.ax2(4000.0, 3000.0)},8000.,6000.)")
    s, spl = m.product("IFCSLAB", "slab 5 square + 15 round holes", [m.extrude(sl, 250.0)], at=(0.0, 6000.0, 0.0))
    sq = m.e(f"IFCRECTANGLEPROFILEDEF(.AREA.,$,{m.ax2()},300.,300.)")
    rd = m.e(f"IFCCIRCLEPROFILEDEF(.AREA.,$,{m.ax2()},50.)")
    for k in range(20):
        m.opening(s, spl, sq if k < 5 else rd, 450.0, (500.0 + 350.0 * k, 1000.0 + 200.0 * (k % 4), -100.0))
    # a wall with a round hole r 150 through it
    w, pl = m.product("IFCWALL", "wall round hole r150", [m.extrude(rect, 2800.0)], at=(0.0, 14000.0, 0.0))
    rh = m.e(f"IFCCIRCLEPROFILEDEF(.AREA.,$,{m.ax2()},150.)")
    m.opening(w, pl, rh, 600.0, (2500.0, -200.0, 1400.0), m.d(0.0, 1.0, 0.0), m.d(1.0, 0.0, 0.0))


def meshes(m):
    """Given meshes: a faceted box (quads), a faceted box of triangles, polygonal and triangulated sets."""
    def brep(faces_pts):
        fs = []
        for poly in faces_pts:
            loop = m.e(f"IFCPOLYLOOP(({','.join(m.pt(*p) for p in poly)}))")
            fs.append(m.e(f"IFCFACE(({m.e(f'IFCFACEOUTERBOUND({loop},.T.)')}))"))
        return m.e(f"IFCFACETEDBREP({m.e(f'IFCCLOSEDSHELL(({chr(44).join(fs)}))')})")
    X, Y, Z = 1000.0, 600.0, 400.0
    v = [(0, 0, 0), (X, 0, 0), (X, Y, 0), (0, Y, 0), (0, 0, Z), (X, 0, Z), (X, Y, Z), (0, Y, Z)]
    quads = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    m.product("IFCBUILDINGELEMENTPROXY", "faceted box, 6 quads", [brep([[v[i] for i in q] for q in quads])], rep_type="Brep")
    tris = []
    for q in quads:
        tris += [[v[q[0]], v[q[1]], v[q[2]]], [v[q[0]], v[q[2]], v[q[3]]]]
    m.product("IFCBUILDINGELEMENTPROXY", "faceted box, 12 triangles", [brep(tris)], rep_type="Brep", at=(2000.0, 0.0, 0.0))
    # an octagonal prism as a faceted brep: 2 octagon faces (6 triangles each) + 8 quads
    oc = [(500 * math.cos(k * math.pi / 4), 500 * math.sin(k * math.pi / 4)) for k in range(8)]
    faces = [[(x, y, 0.0) for x, y in reversed(oc)], [(x, y, 800.0) for x, y in oc]]
    for k in range(8):
        a, b = oc[k], oc[(k + 1) % 8]
        faces.append([(a[0], a[1], 0.0), (b[0], b[1], 0.0), (b[0], b[1], 800.0), (a[0], a[1], 800.0)])
    m.product("IFCBUILDINGELEMENTPROXY", "faceted octagonal prism", [brep(faces)], rep_type="Brep", at=(4000.0, 0.0, 0.0))
    coords = m.e("IFCCARTESIANPOINTLIST3D((" + ",".join(f"({f(p[0])},{f(p[1])},{f(p[2])})" for p in v) + "),$)")
    pfaces = ",".join(m.e(f"IFCINDEXEDPOLYGONALFACE(({','.join(str(i + 1) for i in q)}))") for q in quads)
    m.product("IFCBUILDINGELEMENTPROXY", "polygonal face set box", [m.e(f"IFCPOLYGONALFACESET({coords},.T.,({pfaces}),$)")],
              rep_type="Tessellation", at=(6000.0, 0.0, 0.0))
    tl = ",".join(f"({q[0] + 1},{q[1] + 1},{q[2] + 1}),({q[0] + 1},{q[2] + 1},{q[3] + 1})" for q in quads)
    m.product("IFCBUILDINGELEMENTPROXY", "triangulated face set box", [m.e(f"IFCTRIANGULATEDFACESET({coords},$,.T.,({tl}),$)")],
              rep_type="Tessellation", at=(8000.0, 0.0, 0.0))


def mapped(m):
    """Ten copies of one round column through IfcMappedItem, and ten unmapped equal copies."""
    prof = m.e(f"IFCCIRCLEPROFILEDEF(.AREA.,$,{m.ax2()},200.)")
    rep = m.e(f"IFCSHAPEREPRESENTATION(#6,'Body','SweptSolid',({m.extrude(prof, 3000.0)}))")
    rmap = m.e(f"IFCREPRESENTATIONMAP({m.ax3()},{rep})")
    for k in range(10):
        op = m.e(f"IFCCARTESIANTRANSFORMATIONOPERATOR3D($,$,{m.pt(0., 0., 0.)},$,$)")
        m.product("IFCCOLUMN", f"mapped copy {k}", [m.e(f"IFCMAPPEDITEM({rmap},{op})")], rep_type="MappedRepresentation",
                  at=(k * 3000.0, 0.0, 0.0))
    for k in range(10):
        p = m.e(f"IFCCIRCLEPROFILEDEF(.AREA.,$,{m.ax2()},200.)")
        m.product("IFCCOLUMN", f"unmapped copy {k}", [m.extrude(p, 3000.0)], at=(k * 3000.0, 5000.0, 0.0))


def contexts(m):
    """One body in two representations ('Body' and 'Facetation'), and layers as items of one body."""
    rect = m.e(f"IFCRECTANGLEPROFILEDEF(.AREA.,$,{m.ax2(2500.0, 100.0)},5000.,200.)")
    X, Y, Z = 5000.0, 200.0, 2800.0
    v = [(0, 0, 0), (X, 0, 0), (X, Y, 0), (0, Y, 0), (0, 0, Z), (X, 0, Z), (X, Y, Z), (0, Y, Z)]
    quads = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    coords = m.e("IFCCARTESIANPOINTLIST3D((" + ",".join(f"({f(p[0])},{f(p[1])},{f(p[2])})" for p in v) + "),$)")
    pfaces = ",".join(m.e(f"IFCINDEXEDPOLYGONALFACE(({','.join(str(i + 1) for i in q)}))") for q in quads)
    pfs = m.e(f"IFCPOLYGONALFACESET({coords},.T.,({pfaces}),$)")
    m.product("IFCWALL", "Body and Facetation", None,
              reps=[("Body", "SweptSolid", [m.extrude(rect, 2800.0)]), ("Facetation", "Tessellation", [pfs])])
    layers = []
    for k, t in enumerate((20.0, 160.0, 20.0)):
        y0 = sum((20.0, 160.0, 20.0)[:k])
        lp = m.e(f"IFCRECTANGLEPROFILEDEF(.AREA.,$,{m.ax2(2500.0, y0 + t / 2)},5000.,{f(t)})")
        layers.append(m.extrude(lp, 2800.0))
    m.product("IFCWALL", "three layers as three items", layers, at=(0.0, 2000.0, 0.0))


KINDS = {
    "circles": (circles, "round columns of radius 10 to 5000 mm and round hollow sections"),
    "profiles": (profiles, "fillets (I, rounded rectangle), arcs in composite and indexed curves, round voids"),
    "revolved": (revolved, "IfcRevolvedAreaSolid, a full turn and a quarter"),
    "swept": (swept, "IfcSweptDiskSolid: pipes and bars, straight and with a bend"),
    "clipping": (clipping, "IfcBooleanClippingResult with half-spaces"),
    "openings": (openings, "IfcOpeningElement voids: windows, doors, a slab with 20 holes, a round hole"),
    "meshes": (meshes, "faceted breps (quads, triangles, octagons), IfcPolygonalFaceSet, IfcTriangulatedFaceSet"),
    "mapped": (mapped, "ten IfcMappedItem copies and ten equal unmapped copies"),
    "contexts": (contexts, "a body in Body and Facetation, three layers as items of one body"),
}


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for name, (fn, what) in KINDS.items():
        m = Ifc(name, what)
        fn(m)
        m.write(os.path.join(out, name + ".ifc"))


if __name__ == "__main__":
    main()
