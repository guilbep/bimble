#!/usr/bin/env bash
# Regenerate the app icons packaging/icon-*.png and packaging/icon.png from packaging/logo-mark.svg
# (cargo-packager builds the macOS, Windows and Linux packages from them).
#   scripts/make-icons.sh
# Renders with headless Chrome ($CHROME, else the usual install locations). The mark is drawn at
# 80 % of the side, centred: the 10 % margin all round is how macOS icons sit in the Dock.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
chrome="${CHROME:-}"
if [ -z "$chrome" ]; then
  if [ -x "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" ]; then
    chrome="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
  else
    for c in google-chrome chromium chromium-browser; do
      if command -v "$c" >/dev/null; then chrome="$(command -v "$c")"; break; fi
    done
  fi
fi
[ -n "$chrome" ] || { echo "no Chrome found: set CHROME to a Chrome or Chromium binary" >&2; exit 1; }
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
for spec in 32:icon-32 128:icon-128 256:icon-256 512:icon-512 1024:icon; do
  s="${spec%%:*}"; out="$root/packaging/${spec#*:}.png"
  m=$(awk -v s="$s" 'BEGIN{printf "%.4f", s*0.1}'); w=$(awk -v s="$s" 'BEGIN{printf "%.4f", s*0.8}')
  cat > "$tmp/page.html" <<EOF
<!doctype html><body style="margin:0"><img src="file://$root/packaging/logo-mark.svg"
style="position:absolute;left:${m}px;top:${m}px;width:${w}px;height:${w}px">
EOF
  "$chrome" --headless --disable-gpu --screenshot="$out" --window-size="$s,$s" \
    --force-device-scale-factor=1 --hide-scrollbars --default-background-color=00000000 \
    --user-data-dir="$tmp/profile" "file://$tmp/page.html" >/dev/null 2>&1
  if command -v sips >/dev/null; then
    got=$(sips -g pixelWidth -g pixelHeight "$out" | awk '/pixel/ {printf "%s ", $2}')
    [ "$got" = "$s $s " ] || { echo "$out is $got px, wanted $s $s" >&2; exit 1; }
  fi
  echo "wrote packaging/${spec#*:}.png ($s px)"
done
