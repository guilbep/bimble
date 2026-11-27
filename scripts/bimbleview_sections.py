#!/usr/bin/env python3
"""Bytes per section kind of a .bimbleview ("BIMBLEVW", u32 version, then [4-byte kind, u64 length,
payload]...; golden/README.md view/). Gzipped files are inflated first. Prints JSON."""

import gzip
import json
import struct
import sys


def main(path):
    data = open(path, "rb").read()
    total = len(data)
    if data[:2] == b"\x1f\x8b":
        data = gzip.decompress(data)
    assert data[:8] == b"BIMBLEVW", "not a view file"
    pos = 12
    sizes = {}
    while pos + 12 <= len(data):
        kind = data[pos : pos + 4].decode("ascii", "replace")
        (n,) = struct.unpack_from("<Q", data, pos + 4)
        sizes[kind] = sizes.get(kind, 0) + 12 + n
        pos += 12 + n
    print(json.dumps({"file_bytes": total, "stream_bytes": len(data), "sections": sizes}, sort_keys=True))


if __name__ == "__main__":
    main(sys.argv[1])
