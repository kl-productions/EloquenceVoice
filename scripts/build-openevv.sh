#!/usr/bin/env bash
# Builds the OpenEVV engine for iPhone and the iOS Simulator and packs both into
# Vendor/OpenEVV.xcframework. Runs on macOS with Xcode, GNU make 4.3 or later
# (`brew install make`, which installs `gmake`) and python3.
#
#   OPENEVV_LANGS  languages to link, e.g. "enus engb dede" (default: enus)
#   OPENEVV_RULES  c (fast speech, slow build) or bytecode (default: c)
#   OPENEVV_REF    openevv branch, tag or commit (default: main)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/Vendor/openevv"
LANGS="${OPENEVV_LANGS:-enus}"
RULES="${OPENEVV_RULES:-c}"
REF="${OPENEVV_REF:-main}"
MAKE="${MAKE:-gmake}"
MIN_IOS=17.0

command -v "$MAKE" >/dev/null || { echo "GNU make is needed: brew install make" >&2; exit 1; }
command -v python3 >/dev/null || { echo "python3 is needed" >&2; exit 1; }
case " $LANGS " in *" jajp "*) echo "note: jajp builds but the app does not offer it" >&2 ;; esac

if [ ! -d "$SRC/.git" ]; then
    git clone https://github.com/mudb0y/openevv.git "$SRC"
fi
git -C "$SRC" fetch --quiet origin "$REF"
git -C "$SRC" checkout --quiet --force FETCH_HEAD
# Fixes of ours the engine needs on Apple's arm64, put back clean each run.
for p in "$ROOT"/patches/*.patch; do
    [ -e "$p" ] || continue
    echo "== applying $(basename "$p")"
    git -C "$SRC" apply --whitespace=nowarn "$p"
done
cp "$ROOT/scripts/ios.mk" "$SRC/ios.mk"

langpaths=""
for t in $LANGS; do langpaths="$langpaths lang/$t"; done
langpaths="${langpaths# }"

# The Makefile's own TRIM ends in -Wl,--gc-sections, which Apple's linker does
# not know; the archive needs only the compile half.
trim=""
[ "$RULES" = c ] && trim="-DEVV_NO_BYTECODE -ffunction-sections -fdata-sections"

jobs="$(sysctl -n hw.ncpu)"

build_slice() {
    local sdk="$1" dir="$2" minflag="$3"
    local cc
    cc="$(xcrun --sdk "$sdk" --find clang) -isysroot $(xcrun --sdk "$sdk" --show-sdk-path) -arch arm64 $minflag"
    echo "== building the engine for $sdk ($LANGS, rules as $RULES)"
    (cd "$SRC" && "$MAKE" -f Makefile -f ios.mk -j"$jobs" \
        CC="$cc" RULES="$RULES" LANGS="$langpaths" BUILD="$dir" TRIM="$trim" ios-lib)
}

build_slice iphoneos build-ios-device "-miphoneos-version-min=$MIN_IOS"
build_slice iphonesimulator build-ios-sim "-mios-simulator-version-min=$MIN_IOS"

rm -rf "$ROOT/Vendor/OpenEVV.xcframework"
xcodebuild -create-xcframework \
    -library "$SRC/build-ios-device/libopenevv-ios.a" \
    -library "$SRC/build-ios-sim/libopenevv-ios.a" \
    -output "$ROOT/Vendor/OpenEVV.xcframework"

{
    echo "// Written by scripts/build-openevv.sh: the languages linked into the extension."
    echo "enum BuiltLanguages {"
    printf '    static let tags: Set<String> = ['
    sep=""
    for t in $LANGS; do printf '%s"%s"' "$sep" "$t"; sep=", "; done
    echo ']'
    echo "}"
} > "$ROOT/Shared/Swift/BuiltLanguages.generated.swift"

echo "== done: Vendor/OpenEVV.xcframework"
