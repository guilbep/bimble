#!/usr/bin/env bash
# Fit the large-model memory estimate (ADR 0033, crates/bimble-viewer/src/big.rs) on a folder
# of IFC files. Prints one anonymous row per file (sizes and counts, no names), then the fit:
#   peak ≈ BASE + PARSED_PER_DISPLAYED × displayed MB + PER_TRIANGLE × triangles
# Peak is the "peak memory footprint" of `bimble render` (macOS /usr/bin/time -l), which counts
# GPU buffers on unified memory. Usage: scripts/calibrate_memory.sh <dir-with-ifc> [bimble-binary]
set -euo pipefail
dir=${1:?usage: $0 <dir> [bimble]}
bin=${2:-target/release/bimble}
out=$(mktemp -d)
rows=$out/rows.txt
echo "file_mb displayed_mb stored_triangles peak_mb" > "$rows"
for f in "$dir"/*.ifc; do
  skip=$(LC_ALL=C awk '/^#[0-9]+ *=/{ s=$0; sub(/^#[0-9]+ *= */,"",s); t=substr(s,1,index(s,"(")-1)
    on = (t ~ /^IFC(PROPERTY|COMPLEXPROPERTY|RELDEFINESBYPROPERTIES|ELEMENTQUANTITY|QUANTITY)/) }
    on { b += length($0) + 1 } END { printf "%d", b }' "$f")
  size=$(wc -c < "$f")
  peak=$( { /usr/bin/time -l "$bin" render "$f" "$out/r.png" >/dev/null; } 2>&1 \
    | awk '/peak memory footprint/{ printf "%d", $1 / 1e6 }')
  # What memory follows: triangles stored (own + each shared shape once + one per copy).
  tris=$("$bin" info "$f" 2>/dev/null | awk '/^stored/{ print $2 }')
  echo "$((size / 1000000)) $(((size - skip) / 1000000)) $tris $peak" >> "$rows"
done
column -t "$rows"
python3 -I - "$rows" <<'PY'
import sys
rows = [list(map(float, l.split())) for l in open(sys.argv[1]).read().splitlines()[1:]]
X = [[1.0, r[1], r[2] / 1e6] for r in rows]
y = [r[3] for r in rows]
n = 3
A = [[sum(x[i] * x[j] for x in X) for j in range(n)] for i in range(n)]
b = [sum(x[i] * v for x, v in zip(X, y)) for i in range(n)]
for i in range(n):
    for j in range(i + 1, n):
        f = A[j][i] / A[i][i]
        A[j] = [a - f * c for a, c in zip(A[j], A[i])]
        b[j] -= f * b[i]
c = [0.0] * n
for i in reversed(range(n)):
    c[i] = (b[i] - sum(A[i][k] * c[k] for k in range(i + 1, n))) / A[i][i]
print(f"BASE {c[0]:.0f} MB, PARSED_PER_DISPLAYED {c[1]:.2f}, PER_TRIANGLE {c[2]:.0f} B")
for x, v in zip(X, y):
    e = sum(a * b for a, b in zip(c, x))
    print(f"  displayed {x[1]:6.0f} MB  peak {v:6.0f}  est {e:6.0f}  ratio {v / e:.2f}")
PY
