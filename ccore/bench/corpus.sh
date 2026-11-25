#!/usr/bin/env bash
# The public IFC corpora through the C display reader, Rust's reader, the C reader under ASan+UBSan
# and (a sample) IfcOpenShell (ticket IFC1, results/ifc_corpus.md). Re-runnable; nothing private.
#
#   ccore/bench/corpus.sh fetch     clone each corpus at its pinned commit (shallow) into $BIMBLE_IFC_CORPUS
#   ccore/bench/corpus.sh asan      build out/corpus-asan/ifc_dump (clang -O1 -g, ASan+UBSan)
#   ccore/bench/corpus.sh run       every file: C (out/native/ifc_dump), Rust ($RUST_DUMP), ASan; resumes
#   ccore/bench/corpus.sh ios       IfcOpenShell on <= 40 files ($IOS_PYTHON: a python with ifcopenshell)
#   ccore/bench/corpus.sh report    markdown tables on stdout
#
# BIMBLE_IFC_CORPUS: the corpus folder (default ~/.cache/bimble-ifc-corpus), never inside the repo.
# RUST_DUMP: the ccore_golden example (cargo build --release -p bimble-api --example ccore_golden);
# unset: <repo>/target/release/examples/ccore_golden when it exists, else Rust is not run.
# Results: $BIMBLE_IFC_CORPUS/_runs/$CORPUS_TAG-results.jsonl (CORPUS_TAG default ifc1; delete it,
# or `run --fresh`, to start over). Each C record has "hash": scene.txt and meshes.txt, for
# before/after comparisons.
# Files are untrusted data: never executed; one at a time, 120 s and 6 GB each (bench/corpus.py).
# git-lfs pointers are fetched only when git-lfs is installed; else they are counted and skipped.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
CORPUS=${BIMBLE_IFC_CORPUS:-$HOME/.cache/bimble-ifc-corpus}
TAG=${CORPUS_TAG:-ifc1} # the results' name prefix: one per ticket (IFC2: CORPUS_TAG=ifc2-after ...)
RESULTS=$CORPUS/_runs/$TAG-results.jsonl
IOS=$CORPUS/_runs/$TAG-ios.jsonl

# name | URL | pinned commit | sparse path (empty: all). Licence as stated by the repo, in results/ifc_corpus.md.
CORPORA=(
  "Certification-datasets|https://github.com/buildingSMART/Certification-datasets|5030f652e85d3e679b392fcb38bfa3328d6d9f79|"
  "files|https://github.com/IfcOpenShell/files|0e6e21bc314c4f49be297c3ce10f557b3ff272c8|"
  "IfcSampleFiles|https://github.com/youshengCode/IfcSampleFiles|7e6927982d7e57b29409bb631ef93d6770ef0665|"
  "IFC4.x-specification-models|https://github.com/buildingSMART/IFC4.x-specification-models|ef22440ec501551ccc9689309560bbc7e26aa9d7|"
  "IFC-Rail-Sample-Files|https://github.com/bSI-RailwayRoom/IFC-Rail-Sample-Files|b5a0a289e10fcbf3008e154f68989fb7ee973199|"
  "IFC-infra-unit-test|https://github.com/bSI-InfraRoom/IFC-infra-unit-test|bc13603cc4899084edbf9f1d9151443a7fb4cf0e|"
  "Community-Sample-Test-Files|https://github.com/buildingsmart-community/Community-Sample-Test-Files|31eb1dd0a0406fb9ebf0ebd823b48e1ec95afc08|"
  "bim-whale-ifc-samples|https://github.com/andrewisen/bim-whale-ifc-samples|595fa90e3af7120d004fcb37a79d8657f1d1c9c2|"
  "IFC-files|https://github.com/opensourceBIM/IFC-files|cb96c50f511248cf0df7a16fd4bef0c89a3159f0|"
  "ifc-sample-files|https://github.com/flinker-app/ifc-sample-files|157dda49b68ef40eb48bd6cf9c920dc428b08f1e|"
  "engine_web-ifc|https://github.com/ThatOpen/engine_web-ifc|f25c51bcedff3c78a02f48e2697240f499ff7b1e|tests/ifcfiles/public"
)

case $CORPUS in "$(cd "$here/.." && pwd)"/*) echo "corpus.sh: BIMBLE_IFC_CORPUS is inside the repository" >&2; exit 2 ;; esac

fetch() {
    mkdir -p "$CORPUS"
    local lfs=0
    command -v git-lfs >/dev/null && lfs=1
    for c in "${CORPORA[@]}"; do
        IFS='|' read -r name url sha sparse <<< "$c"
        local d=$CORPUS/$name
        if [[ -d $d/.git ]] && [[ $(git -C "$d" rev-parse HEAD) == "$sha" ]]; then
            echo "$name: at $sha"
            ((lfs)) && git -C "$d" lfs pull -q 2>/dev/null || true
            continue
        fi
        rm -rf "$d"
        git init -q "$d"
        git -C "$d" remote add origin "$url"
        if [[ -n $sparse ]]; then
            git -C "$d" config remote.origin.partialclonefilter blob:none
            git -C "$d" sparse-checkout set "$sparse"
        fi
        GIT_LFS_SKIP_SMUDGE=$((1 - lfs)) git -C "$d" fetch -q --depth 1 ${sparse:+--filter=blob:none} origin "$sha"
        GIT_LFS_SKIP_SMUDGE=$((1 - lfs)) git -C "$d" checkout -q FETCH_HEAD
        echo "$name: fetched $sha"
    done
}

asan() {
    cd "$here"
    source ./deps.sh
    kernel_setup
    local out=out/corpus-asan SRC=()
    mkdir -p "$out"
    for f in src/*.c; do [[ $f == src/wasm_stub.c ]] || SRC+=("$f"); done
    clang -std=c11 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -DRYU_ONLY_64_BIT_OPS \
        "${KFLAGS[@]}" -Isrc -Ivendor/ryu -Isession "${SRC[@]}" tools/ifc_dump.c "${KLINK_CLANG[@]}" -o "$out/ifc_dump"
    echo "$out/ifc_dump"
}

rust_bin() {
    if [[ -n ${RUST_DUMP:-} ]]; then echo "$RUST_DUMP"; return; fi
    local b=$here/../target/release/examples/ccore_golden
    [[ -x $b ]] && echo "$b"
}

cmd=${1:-}; shift || true
case $cmd in
    fetch) fetch ;;
    asan) asan ;;
    run)
        mkdir -p "$CORPUS/_runs"
        exec 9> "$CORPUS/_runs/$TAG.lock"
        flock -n 9 || { echo "corpus.sh: another run holds $CORPUS/_runs/$TAG.lock" >&2; exit 1; }
        [[ -n ${C_DUMP:-} || -x $here/out/native/ifc_dump ]] || "$here/build.sh" native
        args=(--c "${C_DUMP:-$here/out/native/ifc_dump}") # C_DUMP: another build (a before/after run)
        rb=$(rust_bin || true)
        [[ -n $rb ]] && args+=(--rust "$rb")
        [[ -x $here/out/corpus-asan/ifc_dump ]] && args+=(--asan "$here/out/corpus-asan/ifc_dump")
        python3 -I "$here/bench/corpus.py" run "$CORPUS" "$RESULTS" "${args[@]}" "$@" ;;
    ios)
        py=${IOS_PYTHON:-$CORPUS/venv/bin/python}
        python3 -I "$here/bench/corpus.py" ios "$CORPUS" "$RESULTS" "$IOS" --python "$py" "$@" ;;
    report) python3 -I "$here/bench/corpus.py" report "$RESULTS" "$IOS" ;;
    *) sed -n '2,16p' "$0" >&2; exit 2 ;;
esac
