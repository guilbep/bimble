#!/usr/bin/env bash
# Makes golden/mcp/ (ticket M1, golden/README.md "mcp/"):
#   ccore/tools/mcp_golden.sh            Rust's transcript: builds `bimble` (crates/bimble-cli, -j 2),
#                                        feeds it golden/mcp/requests.jsonl through
#                                        test/mcp_test.mjs --generate, writes responses.jsonl and
#                                        saved.json (masked)
#   ccore/tools/mcp_golden.sh additions  the C additions' transcript, the C core's own golden: runs
#                                        out/tcc/bimble-ccore (build.sh tcc first) on
#                                        additions_requests.jsonl, writes additions_responses.jsonl
# Review the diff before committing: the C side is checked against these files by
# `node test/mcp_test.mjs`. Honours CARGO_TARGET_DIR. The Rust mode needs the Rust tree: the branch
# oracle/rust (not main, ADR 0072).
set -euo pipefail
cd "$(dirname "$0")/.."
root=$(cd .. && pwd)
case "${1:-rust}" in
rust)
    if [[ ! -f $root/Cargo.toml ]] || ! command -v cargo > /dev/null; then
        echo "mcp_golden.sh rust: no Rust here (crates/ is not on main, ADR 0072). Run it in a worktree of the branch oracle/rust (git worktree add ../bimble-rust oracle/rust), then copy golden/mcp/ back." >&2
        exit 2
    fi
    (cd "$root" && cargo build -j 2 -p bimble-cli --bin bimble)
    bin="${CARGO_TARGET_DIR:-$root/target}/debug/bimble"
    node test/mcp_test.mjs --generate rust "$bin"
    ;;
additions)
    node test/mcp_test.mjs --generate additions out/tcc/bimble-ccore
    ;;
*)
    echo "usage: $0 [rust|additions]" >&2
    exit 2
    ;;
esac
