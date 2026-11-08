#!/bin/zsh
# Builds Henge for iOS 15 and later from ios/project-ios15.yml and writes
# build/out/Henge-iOS15.ipa (HENGE_OUT sets another path).
#
# The app is built unsigned and signed ad hoc, inside out, with
# Henge/Henge.legacy.entitlements, as the stock packager does: a sideloader
# (TrollStore, SideStore, AltStore, Sideloadly) re-signs it with the user's own
# signature and keeps the entitlements it can. dynamic-codesigning and
# get-task-allow are the whole point on iOS 15–16: that is how the app gets
# executable memory for the JIT.
#
# The ipa is small: it holds the emulator and the installer, and the Mac's
# restore image is downloaded from Apple on the device.
set -e
# This build has no embedded frameworks and no app extensions, so the loops below
# must not trip over a glob that matches nothing.
setopt null_glob 2>/dev/null || true
IOS="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$IOS/.." && pwd)"
OUT="${HENGE_OUT:-$ROOT/build/out/Henge-iOS15.ipa}"
ENT_FILE="$IOS/${HENGE_ENTITLEMENTS:-Henge/Henge.legacy.entitlements}"
mkdir -p "$(dirname "$OUT")"
cd "$IOS"
xcodegen generate --spec project-ios15.yml >/dev/null
DD="$IOS/build/DerivedData-ios15"
mkdir -p "$IOS/build"
xcodebuild -project Henge.xcodeproj -scheme Henge -configuration Release -sdk iphoneos \
    -destination 'generic/platform=iOS' -derivedDataPath "$DD" \
    CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY="" build > "$DD.buildlog" 2>&1 || {
    grep -E "error:|BUILD" "$DD.buildlog" | sort -u
    echo "==> the build failed; nothing packaged" >&2
    exit 1
}
grep -E "BUILD" "$DD.buildlog" || true
APP="$DD/Build/Products/Release-iphoneos/Henge.app"
rc=0
for f in "$APP/Henge" "$APP/henge-jit.js" "$APP/henge-agent.gz"; do
    if [ -f "$f" ]; then printf "  ok       %s\n" "${f#$APP/}"; else echo "  MISSING  ${f#$APP/}" >&2; rc=1; fi
done
[ $rc -eq 0 ] || { echo "==> bundle is not installable; refusing to package" >&2; exit 1; }

STAGE="$(mktemp -d "$DD/stage.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/Payload"
cp -R "$APP" "$STAGE/Payload/"
ENT="$STAGE/entitlements.plist"
cp "$ENT_FILE" "$ENT"
SAPP="$STAGE/Payload/Henge.app"
plutil -insert HengeBuildCommit -string "$(git -C "$ROOT" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)" "$SAPP/Info.plist"
plutil -insert HengeBuildDate -string "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$SAPP/Info.plist"
plutil -insert HengeVariant -string "ios15-a9" "$SAPP/Info.plist"
# Apple's boot ROM is fetched on the device (BootROM.swift) and must never ship.
rm -f "$SAPP/AVPBooter.vmapple2.bin"
if [ -n "$(find "$STAGE" -iname 'AVPBooter*' -o -iname 'AVPSEPBooter*')" ]; then
  echo "Apple's boot ROM is in the app: not packaging." >&2
  exit 1
fi
sign() { codesign --force --sign - --timestamp=none "$@"; }
for fw in "$SAPP"/Frameworks/*.framework; do [ -d "$fw" ] && sign "$fw"; done
for ex in "$SAPP"/PlugIns/*.appex; do [ -d "$ex" ] && sign "$ex"; done
sign --entitlements "$ENT" "$SAPP"
EMBEDDED=$(codesign -d --entitlements - "$SAPP" 2>&1 || true)
for k in $(plutil -convert json -o - "$ENT" | python3 -I -c 'import json, sys; print(" ".join(json.load(sys.stdin)))'); do
    grep -q "$k" <<< "$EMBEDDED" || { echo "==> $k did not embed; refusing to package" >&2; exit 1; }
done
( cd "$STAGE" && zip -qry "$STAGE/package.tmp" Payload )
mv -f "$STAGE/package.tmp" "$OUT"
echo "==> $OUT ($(du -h "$OUT" | cut -f1))"
echo "==> the ipa holds the emulator only; macOS is downloaded on the device at install time"
