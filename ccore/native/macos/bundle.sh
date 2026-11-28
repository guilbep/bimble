#!/usr/bin/env bash
# bimble.app and its disk image (ticket MACAPP, ADR 0088), from the binary native/build.sh app made on macOS.
#   native/macos/bundle.sh [binary]     default out/app/bimble (its .dSYM beside it: <binary>.dSYM)
#     -> out/macapp/bimble.app                         Info.plist (ch.bimble.bimble, ccore/VERSION, the file types
#                                                      .bimble .bimbleview .ifc), bimble.icns, licences, signed
#        out/macapp/bimble-<v>-macos-<arch>.dmg        bimble.app, an Applications link, README.txt (first launch)
#        out/macapp/bimble-<v>-macos-<arch>.dSYM.zip   the debug information (native/symbolize.sh with atos)
#   <arch>: arm64, x86_64, or universal (a lipo'd binary). DMG_FORMAT: ULMO (default: lzma, macOS 10.15 and newer;
#   arm64: 2.40 MB, against 3.09 ULFO and 3.25 UDZO, docs/adr/0088), UDZO or ULFO.
# Signing: ad hoc (codesign -s -) by default, which Apple Silicon needs to run anything at all. Developer ID later,
# without rework: SIGN_IDENTITY="Developer ID Application: ..." (a keychain holding it) signs with the hardened runtime
# (--options runtime, entitlements.plist) and a secure timestamp; then NOTARIZE=1 with NOTARY_KEY (an App Store
# Connect API key file .p8), NOTARY_KEY_ID and NOTARY_ISSUER submits the .dmg (xcrun notarytool) and staples the
# ticket. Nothing of that exists yet: unsigned to start (Pierre, 28 Nov 2026).
set -euo pipefail
cd "$(dirname "$0")/../.."
[[ $(uname -s) == Darwin ]] || { echo "FAIL native/macos/bundle.sh: macOS only (codesign, iconutil, hdiutil)" >&2; exit 1; }
BIN=${1:-out/app/bimble}
[[ -x $BIN ]] || { echo "FAIL native/macos/bundle.sh: $BIN not built (native/build.sh app)" >&2; exit 1; }
V=$(< VERSION)
SHORT=$(echo "$V" | sed -E 's/^([0-9]+\.[0-9]+\.[0-9]+).*/\1/')
MINOS=${MACOSX_DEPLOYMENT_TARGET:-11.0}
archs=$(lipo -archs "$BIN")
case $archs in arm64 | x86_64) arch=$archs ;; *arm64* | *x86_64*) arch=universal ;; *) echo "FAIL bundle.sh: $BIN is $archs" >&2; exit 1 ;; esac
O=out/macapp
APP=$O/bimble.app
NAME=bimble-$V-macos-$arch
SIGN_IDENTITY=${SIGN_IDENTITY:--}
FMT=${DMG_FORMAT:-ULMO}
rm -rf "$APP" "$O/stage" "$O/$NAME.dmg" "$O/$NAME.dSYM.zip"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/licences"
# 1. the executable, Info.plist, the icon (from packaging/icon.png, 1024 px), the licences
cp "$BIN" "$APP/Contents/MacOS/bimble"
sed -e "s/@VERSION@/$V/g" -e "s/@SHORT@/$SHORT/g" -e "s/@MINOS@/$MINOS/g" native/macos/Info.plist.in > "$APP/Contents/Info.plist"
plutil -lint "$APP/Contents/Info.plist" > /dev/null
printf 'APPL????' > "$APP/Contents/PkgInfo"
ics=$O/stage-icon/bimble.iconset
rm -rf "$O/stage-icon"
mkdir -p "$ics"
for s in 16 32 128 256 512; do
    sips -z $s $s ../packaging/icon.png --out "$ics/icon_${s}x${s}.png" > /dev/null
    sips -z $((s * 2)) $((s * 2)) ../packaging/icon.png --out "$ics/icon_${s}x${s}@2x.png" > /dev/null
done
iconutil -c icns "$ics" -o "$APP/Contents/Resources/bimble.icns"
rm -rf "$O/stage-icon"
L=$APP/Contents/Resources/licences
cp ../LICENSE "$APP/Contents/Resources/LICENSE"
cp THIRD-PARTY.txt "$APP/Contents/Resources/THIRD-PARTY.txt"
cp out/native/deps/imgui/LICENSE.txt "$L/DearImGui-LICENSE.txt"
cp out/native/deps/cimgui/LICENSE "$L/cimgui-LICENSE.txt"
cp out/native/deps/nanosvg/LICENSE.txt "$L/nanosvg-LICENSE.txt"
for lf in LICENSE_CC0 LICENSE_A2 LICENSE_A2LLVM; do cp "vendor/blake3/$lf" "$L/BLAKE3-$lf"; done
for lf in LICENSE-Apache2 LICENSE-Boost; do cp "vendor/ryu/$lf" "$L/Ryu-$lf"; done
cp vendor/earcutr/LICENSE "$L/earcutr-LICENSE"
for lf in OFL-Sora.txt OFL-SourceSans3.txt OFL-DMMono.txt; do cp "../web-ui/src/brand/$lf" "$L/$lf"; done
cat > "$L/README.txt" << 'EOF'
Linked into bimble statically, besides the files here: SDL 3 (zlib licence, https://libsdl.org), FreeType (FreeType
Licence, https://freetype.org), brotli (MIT, https://github.com/google/brotli), manifold and Clipper2 (Apache-2.0 and
BSL-1.0: THIRD-PARTY.txt). Their licence texts are in their sources at the commits native/macos/deps.sh and ccore/deps.sh pin.
EOF
# 2. signed: ad hoc now; Developer ID with the hardened runtime when SIGN_IDENTITY names one
if [[ $SIGN_IDENTITY == - ]]; then
    codesign --force --sign - "$APP"
    sign_kind="ad hoc (not a Developer ID: Gatekeeper asks once, README.txt)"
else
    codesign --force --options runtime --timestamp --entitlements native/macos/entitlements.plist --sign "$SIGN_IDENTITY" "$APP"
    sign_kind="$SIGN_IDENTITY, hardened runtime"
fi
codesign --verify --strict --verbose=1 "$APP"
# 3. the disk image: the app, a link to /Applications, the first launch's steps
st=$O/stage
mkdir -p "$st"
cp -R "$APP" "$st/"
ln -s /Applications "$st/Applications"
cat > "$st/README.txt" << EOF
Bimble $V ($arch, macOS $MINOS or newer): the native app, a preview. Open-source structural BIM (MIT licence).
https://github.com/guilbep/bimble

Install: drag bimble.app onto Applications.

First launch. This preview is signed ad hoc, not with an Apple Developer ID, and not notarised, so macOS
refuses it the first time:
  macOS 15 (Sequoia) and newer: open bimble.app once (macOS says it cannot verify it: click Done), then open
    System Settings > Privacy & Security, scroll to Security, click "Open Anyway" (there for about an hour after
    the attempt), enter your login password, then Open. Once only: macOS keeps the exception.
  macOS 11 to 14: right-click (Control-click) bimble.app > Open, then Open in the dialog. Once only.
  Or, in Terminal, once (any version):  xattr -dr com.apple.quarantine /Applications/bimble.app

Files: double-click a .bimble, .bimbleview or .ifc (Open With > bimble the first time), drop one on the window,
or File > Open (Cmd+O). From Terminal: open -a /Applications/bimble.app model.ifc

Session logs (they stay on this Mac): ~/Library/Logs/bimble/sessions; the newest:
  /Applications/bimble.app/Contents/MacOS/bimble --last-log
A measured run (the same summary as on Linux):
  /Applications/bimble.app/Contents/MacOS/bimble --offscreen --preset orbit-zoom --out ~/bimble-run model.ifc
EOF
hdiutil create -quiet -volname "Bimble $V" -srcfolder "$st" -fs HFS+ -format "$FMT" -ov "$O/$NAME.dmg"
rm -rf "$st"
[[ $SIGN_IDENTITY == - ]] || codesign --force --timestamp --sign "$SIGN_IDENTITY" "$O/$NAME.dmg"
# 4. notarised (later): App Store Connect API key; skipped unless NOTARIZE=1
if [[ ${NOTARIZE:-0} == 1 ]]; then
    [[ $SIGN_IDENTITY != - ]] || { echo "FAIL bundle.sh: NOTARIZE=1 needs a Developer ID (SIGN_IDENTITY)" >&2; exit 1; }
    xcrun notarytool submit "$O/$NAME.dmg" --key "${NOTARY_KEY:?}" --key-id "${NOTARY_KEY_ID:?}" --issuer "${NOTARY_ISSUER:?}" --wait
    xcrun stapler staple "$O/$NAME.dmg"
fi
# 5. the debug information
dsym=$BIN.dSYM
if [[ -d $dsym ]]; then (cd "$(dirname "$dsym")" && ditto -c -k --keepParent "$(basename "$dsym")" "$OLDPWD/$O/$NAME.dSYM.zip"); fi
printf 'native/macos/bundle.sh: %s (%s), signed %s\n' "$APP" "$archs" "$sign_kind"
printf '  %-34s %9d bytes (executable)\n' "bimble.app/Contents/MacOS/bimble" "$(wc -c < "$APP/Contents/MacOS/bimble")"
printf '  %-34s %9d bytes (%s)\n' "$NAME.dmg" "$(wc -c < "$O/$NAME.dmg")" "$FMT"
[[ -f $O/$NAME.dSYM.zip ]] && printf '  %-34s %9d bytes\n' "$NAME.dSYM.zip" "$(wc -c < "$O/$NAME.dSYM.zip")"
exit 0
