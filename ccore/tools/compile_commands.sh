#!/usr/bin/env bash
# Writes ccore/compile_commands.json from ccore/compile_flags.txt: one entry per C file of src/,
# session/, host/, tools/ and test/, for editors that read a compilation database but not
# compile_flags.txt (the Microsoft C/C++ extension: "C_Cpp.default.compileCommands"). clangd reads
# compile_flags.txt directly. The file is not committed (it holds absolute paths).
#   ccore/tools/compile_commands.sh
set -euo pipefail
cd "$(dirname "$0")/.."
dir=$(pwd)
flags=$(grep -v '^-xc$' compile_flags.txt | sed 's/"/\\"/g' | awk '{printf " %s", $0}')
{
    echo "["
    first=1
    for f in src/*.c session/*.c host/*.c tools/*.c test/*_test.c; do
        [[ $f == src/wasm_stub.c ]] && continue
        ((first)) || echo ","
        first=0
        printf '  {"directory": "%s", "file": "%s/%s", "command": "clang -xc%s -c %s"}' "$dir" "$dir" "$f" "$flags" "$f"
    done
    echo
    echo "]"
} > compile_commands.json
echo "wrote ccore/compile_commands.json ($(grep -c '"file"' compile_commands.json) files)"
