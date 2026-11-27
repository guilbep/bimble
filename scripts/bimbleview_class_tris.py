#!/usr/bin/env python3
"""Elements and triangles per IFC class in a .bimbleview (layout: golden/README.md, view/).
Triangles of a shared shape are counted at each element that uses it. Prints JSON."""

import gzip
import json
import struct
import sys


class R:
    def __init__(self, b, p=0):
        self.b, self.p = b, p

    def varint(self):
        v = s = 0
        while True:
            c = self.b[self.p]
            self.p += 1
            v |= (c & 0x7F) << s
            if c < 0x80:
                return v
            s += 7

    def str(self):
        n = self.varint()
        v = self.b[self.p : self.p + n].decode("utf-8", "replace")
        self.p += n
        return v

    def mesh_tris(self):
        n = self.varint()
        end = self.p + n
        assert self.b[self.p : self.p + 4] == b"BMG1"
        r = R(self.b, self.p + 5)
        r.varint()  # positions
        idx = r.varint()
        self.p = end
        return idx // 3


def main(path):
    b = open(path, "rb").read()
    if b[:2] == b"\x1f\x8b":
        b = gzip.decompress(b)
    pos = 12
    table = []
    shapes = []
    els, tris = {}, {}
    while pos + 12 <= len(b):
        kind = b[pos : pos + 4]
        (n,) = struct.unpack_from("<Q", b, pos + 4)
        r = R(b, pos + 12)
        if kind == b"META":
            table = [r.str() for _ in range(r.varint())]
        elif kind == b"ELEM":
            for _ in range(r.varint()):
                shapes.append(r.mesh_tris())
            for _ in range(r.varint()):
                r.str(), r.str()
                cls = table[r.varint()]
                r.varint(), r.varint(), r.varint()
                tag = r.b[r.p]
                r.p += 1
                if tag == 0:
                    t = r.mesh_tris()
                else:
                    t = shapes[r.varint()]
                    r.p += 48
                els[cls] = els.get(cls, 0) + 1
                tris[cls] = tris.get(cls, 0) + t
        pos += 12 + n
    print(json.dumps({"elements": els, "triangles": tris, "total_triangles": sum(tris.values())}, sort_keys=True))


if __name__ == "__main__":
    main(sys.argv[1])
