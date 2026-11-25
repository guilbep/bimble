#!/usr/bin/env python3
"""Writes the synthetic PDF fixtures of ccore/golden/reference/fixtures/ (ticket L1).

Run once from the repo root: python3 ccore/tools/ref_fixtures.py. The files are committed; the
golden dumper (crates/bimble-api/examples/ccore_golden.rs, `reference_files`) reads them and
writes reference/<name>.txt with what Rust's `bimble_drawing::reference::read` gives. All hand
made: no client file. Same bytes on every run (no dates, no randomness)."""
import os
import zlib

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "golden", "reference", "fixtures")


def stream(dict_body: str, data: bytes) -> bytes:
    return b"<< " + dict_body.encode() + b" /Length %d >>\nstream\n" % len(data) + data + b"\nendstream"


def pdf(objects, root=1, extra_trailer="") -> bytes:
    """A classic PDF: header, objects 1..n, xref table, trailer."""
    out = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offsets = []
    for i, o in enumerate(objects):
        offsets.append(len(out))
        body = o if isinstance(o, bytes) else o.encode()
        out += b"%d 0 obj\n" % (i + 1) + body + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objects) + 1)
    for off in offsets:
        out += b"%010d 00000 n \n" % off
    out += b"trailer\n<< /Size %d /Root %d 0 R%s >>\nstartxref\n%d\n%%%%EOF\n" % (
        len(objects) + 1, root, extra_trailer.encode(), xref)
    return bytes(out)


LINES_CONTENT = b"""% a comment line, then an indented one
   q 1 0 0 1 10 20 cm 0 0 100 50 re S Q
0 0 m 72 0 l S
q 0.5 0 0 0.5 0 0 cm
  100 100 m 200 100 l 200 150 l h S
Q
10 300 m 20 340 60 340 70 300 c S
10 400 m 30 440 70 400 v 90 360 100 400 y s
/GS1 gs 0.1 0.2 0.3 RG .5 w -.25 +3 m 1. 0.1 l 7 8 l S
BT /F1 12 Tf 100 700 Td (a (nested) \\) string) Tj [(T) -120 (J)] TJ <414243> Tj ET
300 300 50 50 re W n
q 2 0 0 2 300 500 cm /Fm1 Do Q
/Im1 Do
1 0 0 1 0 0 cm 400 100 m 450 100 l 450 150 l f
400 200 m 450 200 l 450 250 l b
500 10 m 500 20 l
510 10 m S
true false null /N 1 2 m 3 4 l S
"""

LINES_CONTENT_2 = b"""q 1 0 0 1 0 600 cm
0 0 m 0 100 l S Q
"""

FORM = b"0 0 m 10 0 l 10 10 l h S 5 5 m 0 0 2 2 re f /Fm2 Do"
FORM2 = b"0 0 m 1 1 l S"


def lines_pdf() -> bytes:
    objs = [
        "<< /Type /Catalog /Pages 2 0 R >>",
        # The page's resources come from the page tree's root (inherited).
        "<< /Type /Pages /Kids [3 0 R] /Count 1 /Resources << /XObject << /Fm1 6 0 R /Im1 7 0 R >> >> >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents [4 0 R 5 0 R] >>",
        stream("", LINES_CONTENT),
        stream("", LINES_CONTENT_2),
        stream("/Type /XObject /Subtype /Form /BBox [0 0 20 20] /Matrix [1 0 0 1 3 4] "
               "/Resources << /XObject << /Fm2 8 0 R >> >>", FORM),
        stream("/Type /XObject /Subtype /Image /Width 1 /Height 1 /BitsPerComponent 8 /ColorSpace /DeviceGray",
               b"\x80"),
        stream("/Type /XObject /Subtype /Form /BBox [0 0 2 2] /Matrix [0 1 -1 0 0 0]", FORM2),
    ]
    return pdf(objs)


def png_up(rows, width):
    """PNG predictor 12 (Up) over rows of `width` bytes."""
    out = bytearray()
    prev = bytes(width)
    for r in rows:
        out.append(2)
        out += bytes((r[i] - prev[i]) & 0xFF for i in range(width))
        prev = r
    return bytes(out)


def flate_pdf() -> bytes:
    """Compressed: a FlateDecode content stream, the catalog, page tree and page in an object
    stream, and a cross-reference stream with the PNG Up predictor (as most producers write)."""
    content = b"q 1 0 0 1 50 60 cm 0 0 m 100 0 l 100 100 l 0 100 l h S Q 10 10 200 5 re f\n" * 3
    content += b"0 0 m 1.25 2.5 l 3.75 -1 l S\n"
    cstream = zlib.compress(content, 9)
    # Object stream 5 holds objects 1, 2, 3.
    members = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 4 0 R /Resources << >> >>",
    ]
    body = b""
    header = b""
    for i, m in enumerate(members):
        header += b"%d %d " % (i + 1, len(body))
        body += m + b"\n"
    objstm_data = header + body
    out = bytearray(b"%PDF-1.5\n%\xe2\xe3\xcf\xd3\n")
    off4 = len(out)
    out += b"4 0 obj\n" + stream("/Filter /FlateDecode", cstream) + b"\nendobj\n"
    off5 = len(out)
    out += b"5 0 obj\n" + stream("/Type /ObjStm /N 3 /First %d /Filter /FlateDecode" % len(header),
                                 zlib.compress(objstm_data)) + b"\nendobj\n"
    off6 = len(out)
    # /W [1 2 1]: type, field 2, field 3.
    rows = [
        bytes([0, 0, 0, 255]),
        bytes([2, 0, 5, 0]),
        bytes([2, 0, 5, 1]),
        bytes([2, 0, 5, 2]),
        bytes([1, off4 >> 8, off4 & 255, 0]),
        bytes([1, off5 >> 8, off5 & 255, 0]),
        bytes([1, off6 >> 8, off6 & 255, 0]),
    ]
    xdata = zlib.compress(png_up(rows, 4))
    out += b"6 0 obj\n" + stream("/Type /XRef /Size 7 /W [1 2 1] /Root 1 0 R /Filter /FlateDecode "
                                 "/DecodeParms << /Columns 4 /Predictor 12 >>", xdata) + b"\nendobj\n"
    out += b"startxref\n%d\n%%%%EOF\n" % off6
    return bytes(out)


def cycle_pdf() -> bytes:
    """The page tree's root lists itself among its kids."""
    objs = [
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R 2 0 R] /Count 2 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 4 0 R >>",
        stream("", b"0 0 m 10 10 l S"),
    ]
    return pdf(objs)


def self_form_pdf() -> bytes:
    """A form XObject that draws itself: stops at the nesting limit."""
    objs = [
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 4 0 R "
        "/Resources << /XObject << /Me 5 0 R >> >> >>",
        stream("", b"/Me Do"),
        stream("/Type /XObject /Subtype /Form /BBox [0 0 100 100] /Matrix [1 0 0 1 1 0] "
               "/Resources << /XObject << /Me 5 0 R >> >>", b"0 0 m 0 10 l S /Me Do"),
    ]
    return pdf(objs)


def text_only_pdf() -> bytes:
    objs = [
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R 5 0 R] /Count 2 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 4 0 R >>",
        stream("", b"BT /F1 12 Tf 72 720 Td (scanned) Tj ET"),
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] >>",
    ]
    return pdf(objs)


def encrypted_pdf() -> bytes:
    objs = [
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 4 0 R >>",
        stream("", b"0 0 m 10 10 l S"),
        "<< /Filter /Standard /V 1 /R 2 /Length 40 /P -4 /O <" + "00" * 32 + "> /U <" + "00" * 32 + "> >>",
    ]
    return pdf(objs, extra_trailer=" /Encrypt 5 0 R /ID [<00112233445566778899aabbccddeeff> <00112233445566778899aabbccddeeff>]")


def many_pdf() -> bytes:
    """30,000 segments: 1,000 polylines of 31 points, compressed (the lab's frame time)."""
    parts = []
    for i in range(1000):
        y = 20 + (i % 100) * 8
        x0 = 20 + (i // 100) * 55
        pts = ["%d %d m" % (x0, y)]
        for k in range(1, 31):
            pts.append("%d %.1f l" % (x0 + k * 1.6, y + (3 if k % 2 else 0)))
        parts.append(" ".join(pts) + " S")
    content = ("\n".join(parts) + "\n").encode()
    objs = [
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 4 0 R >>",
        stream("/Filter /FlateDecode", zlib.compress(content, 9)),
    ]
    return pdf(objs)


def main():
    os.makedirs(OUT, exist_ok=True)
    lines = lines_pdf()
    files = {
        "lines.pdf": lines,
        "flate.pdf": flate_pdf(),
        "cycle.pdf": cycle_pdf(),
        "self-form.pdf": self_form_pdf(),
        "text-only.pdf": text_only_pdf(),
        "encrypted.pdf": encrypted_pdf(),
        "many.pdf": many_pdf(),
        # Cut inside the second content stream: the xref table and trailer are gone.
        "truncated.pdf": lines[: lines.index(b"0 0 m 0 100 l") + 4],
        "garbage.pdf": b"%PDF-1.4 garbage",
        "not-a-pdf.pdf": b"hello, this is not a PDF\n",
        "empty.pdf": b"",
    }
    for name, data in files.items():
        with open(os.path.join(OUT, name), "wb") as f:
            f.write(data)
    print("wrote", len(files), "fixtures to", os.path.normpath(OUT))


if __name__ == "__main__":
    main()
