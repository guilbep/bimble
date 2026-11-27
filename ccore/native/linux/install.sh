#!/usr/bin/env bash
# NUI3: adds the native app `bimble` of this folder to the desktop, for this user only (nothing as root): its menu entry
# (ch.bimble.bimble.desktop, the app id the window carries: a compositor's rules match it), its icon, and the file types
# .bimble, .bimbleview and .ifc opened with it (double click, "Open with"). Again with --remove: all of it removed.
#   desktop/install.sh [--remove]
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
bin=$(cd "$here/.." && pwd)/bimble
data=${XDG_DATA_HOME:-$HOME/.local/share}
app=$data/applications/ch.bimble.bimble.desktop mime=$data/mime/packages/ch.bimble.bimble.xml
if [[ ${1:-} == --remove ]]; then
    rm -f "$app" "$mime"
    for s in 32 128 256 512; do rm -f "$data/icons/hicolor/${s}x${s}/apps/ch.bimble.bimble.png"; done
else
    [[ -x $bin ]] || { echo "install.sh: $bin not found (keep desktop/ next to bimble)" >&2; exit 1; }
    mkdir -p "$data/applications" "$data/mime/packages"
    sed "s|^Exec=bimble |Exec=\"$bin\" |" "$here/ch.bimble.bimble.desktop" > "$app"
    cp "$here/ch.bimble.bimble.xml" "$mime"
    for s in 32 128 256 512; do
        mkdir -p "$data/icons/hicolor/${s}x${s}/apps"
        cp "$here/icon-$s.png" "$data/icons/hicolor/${s}x${s}/apps/ch.bimble.bimble.png"
    done
fi
command -v update-mime-database > /dev/null && update-mime-database "$data/mime" || true
command -v update-desktop-database > /dev/null && update-desktop-database "$data/applications" || true
command -v gtk-update-icon-cache > /dev/null && gtk-update-icon-cache -q -t "$data/icons/hicolor" 2> /dev/null || true
if [[ ${1:-} == --remove ]]; then echo "bimble: removed from the desktop"; else echo "bimble: in the menu as Bimble; .bimble, .bimbleview and .ifc open with $bin"; fi
