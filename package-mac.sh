#!/bin/bash
# Packages SoundX for macOS: builds the app and wraps it in a .dmg with an Applications shortcut.
# Run this on a Mac (Xcode command line tools required). Version label comes from CMakeLists.txt.
#
#   ./package-mac.sh                 # universal (arm64 + x86_64), ad-hoc signed, no hardened runtime
#   CODESIGN_ID="Developer ID Application: ..." ./package-mac.sh
#                                    # real signature + hardened runtime + entitlements (notarise this)
#   SOUNDX_MAC_ARCHS=arm64 ./package-mac.sh   # native only (smaller, but cannot load plug-ins that
#                                             # only contain x86_64 code - and vice versa)
#   SOUNDX_MAC_HARDENED=1 ./package-mac.sh    # hardened runtime with an ad-hoc signature
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
CONFIG="${1:-Release}"
BUILD="$ROOT/build-mac"
DIST="$ROOT/dist"
ENTITLEMENTS="$ROOT/installer/SoundX.entitlements"

# A plug-in host has to match the architecture of the plug-ins it loads (a VST3 bundle contains
# either a universal binary or one architecture only, and dyld cannot mix). Building universal by
# default means an Intel-only plug-in still loads on an Apple silicon Mac and vice versa.
ARCHS="${SOUNDX_MAC_ARCHS:-arm64;x86_64}"

echo "== configure ($CONFIG, $ARCHS) =="
cmake -S "$ROOT" -B "$BUILD" -G "Unix Makefiles" \
      -DCMAKE_BUILD_TYPE="$CONFIG" \
      -DCMAKE_OSX_ARCHITECTURES="$ARCHS" \
      -DSOUNDX_VST2=OFF -DSOUNDX_ASIO=OFF

echo "== build =="
cmake --build "$BUILD" --config "$CONFIG" -j "$(sysctl -n hw.ncpu)"

APP="$BUILD/SoundX_artefacts/$CONFIG/SoundX.app"
if [ ! -d "$APP" ]; then
    # single-config generators put the bundle straight under the artefacts folder
    APP="$BUILD/SoundX_artefacts/SoundX.app"
fi
if [ ! -d "$APP" ]; then
    echo "error: SoundX.app was not produced" >&2
    exit 1
fi

# use the same version label as the Windows build
VERSION="$(sed -n 's/^set(SOUNDX_VERSION_LABEL "\(.*\)")/\1/p' "$ROOT/CMakeLists.txt")"
[ -z "$VERSION" ] && VERSION="0.03Alpha"

# codesign (ad-hoc keeps Gatekeeper happy for local use; set CODESIGN_ID for a real identity).
#
# The hardened runtime is only enabled when asked for (or when signing with a real identity, which
# means the build is going to be notarised): with the hardened runtime, macOS library validation
# blocks every plug-in bundle that is not signed by the same team, unless the app carries
# com.apple.security.cs.disable-library-validation - which is exactly what the entitlements file
# says. A local ad-hoc build therefore stays without it, so plug-ins load out of the box.
SIGN_ID="${CODESIGN_ID:--}"
HARDENED="${SOUNDX_MAC_HARDENED:-0}"
if [ "$SIGN_ID" != "-" ]; then HARDENED=1; fi

echo "== codesign ($SIGN_ID, hardened=$HARDENED) =="
if [ "$HARDENED" = "1" ]; then
    codesign --force --deep --options runtime --entitlements "$ENTITLEMENTS" --sign "$SIGN_ID" "$APP"
else
    codesign --force --deep --sign "$SIGN_ID" "$APP"
fi
codesign --verify --verbose=1 "$APP" || true

mkdir -p "$DIST"
DMG="$DIST/SoundX-$VERSION.dmg"
STAGE="$(mktemp -d)"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"

echo "== dmg =="
rm -f "$DMG"
hdiutil create -volname "SoundX $VERSION" -srcfolder "$STAGE" -ov -format UDZO "$DMG"
rm -rf "$STAGE"

echo ""
echo "packaged:"
echo "  app  $APP"
echo "  dmg  $DMG"
echo ""
echo "notes:"
echo "  - unsigned builds: right click the app > Open the first time, or run"
echo "      xattr -dr com.apple.quarantine /Applications/SoundX.app"
echo "  - plug-ins that still will not load can be checked one by one:"
echo "      /Applications/SoundX.app/Contents/MacOS/SoundX --selftest /tmp/sx --scan \\"
echo "          \"/Library/Audio/Plug-Ins/VST3/<name>.vst3\""
echo "      (the reason is printed in /tmp/sx/selftest.log and in"
echo "       ~/Library/Application Support/SoundX/plugin-scan.log)"
echo "  - notarisation (needs an Apple developer id):"
echo "      xcrun notarytool submit \"$DMG\" --keychain-profile <profile> --wait"
echo "      xcrun stapler staple \"$DMG\""
