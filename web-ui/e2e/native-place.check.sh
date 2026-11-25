#!/usr/bin/env bash
# Done check of native-place.mjs without a server: syntax, the no-server hint and exit code, the npm script.
# Run from the repository root. Exit 0 when all three hold.
set -u
log=$(mktemp)
trap 'rm -f "$log"' EXIT
node --check web-ui/e2e/native-place.mjs || { echo "check: syntax"; exit 1; }
node web-ui/e2e/native-place.mjs --endpoint http://127.0.0.1:9 --token x >"$log" 2>&1
code=$?
cat "$log"
[ "$code" -ne 0 ] || { echo "check: exit code 0 with no server"; exit 1; }
grep -qi 'bimble view' "$log" || { echo "check: no start hint"; exit 1; }
node -e 'JSON.parse(require("fs").readFileSync("web-ui/package.json","utf8")).scripts["e2e:native"] || process.exit(1)' \
  || { echo "check: no e2e:native script"; exit 1; }
echo "check: ok (no-server exit $code)"
