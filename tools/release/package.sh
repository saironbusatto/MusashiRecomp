#!/usr/bin/env bash
# Build release packages of Brave Fencer Musashi Recompiled in Docker.
#
#   tools/release/package.sh linux|windows|all [version]
#
# Needs the locally generated code (games/musashi/generated/ and generated/),
# which is derived from the user's own disc/BIOS and never committed — so this
# runs on a dev machine, not in CI. Packages ship no BIOS, disc, or save data:
# the launcher asks the player for their own BIOS and disc dump.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TARGET="${1:-all}"
VERSION="${2:-$(git -C "$ROOT" describe --tags --always)}"
NAME="MusashiRecomp"
BIN="Brave_Fencer_Musashi__PSXRecomp_"
OUT="$ROOT/dist"
SYSTEM_DLLS='kernel32|user32|gdi32|shell32|msvcrt|advapi32|ws2_32|comdlg32|dbghelp|ole32|oleaut32|winmm|imm32|version|setupapi|dinput8|rpcrt4|hid|cfgmgr32|opengl32|shlwapi|uxtheme|dwmapi|bcrypt|ucrtbase|api-ms-win-.*'
mkdir -p "$OUT" "$ROOT/release-build"

[ -f "$ROOT/games/musashi/generated/SLUS_007.26_full.c" ] ||
    { echo "games/musashi/generated/ missing - run the recompiler on your dump first" >&2; exit 1; }

in_docker() {  # image dockerfile cmd
    local image="$1" dockerfile="$2"; shift 2
    docker build -q -t "$image" -f "$ROOT/tools/release/$dockerfile" "$ROOT/tools/release" >/dev/null
    docker run --rm --user "$(id -u):$(id -g)" -v "$ROOT":/src -w /src "$image" bash -c "$*"
}

# Files every package carries next to the executable.
stage_common() {  # build_dir stage_dir
    local build="$1" stage="$2"
    mkdir -p "$stage/games/musashi"
    cp -r "$build/launcher.rml" "$build/fonts" "$build/img" "$stage/"
    # Drop the dev-tree disc path: the player picks their own dump in the launcher.
    # Ship 4:3 by default: 16:9 still has black patches in the revealed margins
    # (BFM_WIDESCREEN_PLAN.md); players can opt in via Settings > Widescreen.
    sed -e 's|^disc *=.*|disc         = ""|' \
        -e 's|^aspect_ratio *=.*|aspect_ratio      = "4:3"|' \
        "$ROOT/games/musashi/game.toml" > "$stage/games/musashi/game.toml"
    cp "$ROOT/LICENSE" "$ROOT/THIRD_PARTY_ATTRIBUTION.md" "$stage/"
    sed "s/@VERSION@/$VERSION/" "$ROOT/tools/release/START_HERE.txt" > "$stage/START_HERE.txt"
}

build_linux() {
    local build=/src/release-build/linux
    in_docker musashi-release-linux Dockerfile.linux "
        cmake -S runtime -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release \
              -DPSX_DEBUG_TOOLS=OFF -DPSX_LAUNCHER=ON \
              '-DCMAKE_EXE_LINKER_FLAGS=-static-libstdc++ -static-libgcc' \
              '-DCMAKE_BUILD_RPATH=\$ORIGIN/lib' >/dev/null &&
        ninja -C $build musashi-runtime"

    local appdir="$ROOT/release-build/AppDir"
    local game="$appdir/usr/share/$NAME"
    rm -rf "$appdir"; mkdir -p "$game/lib"
    cp "$ROOT/release-build/linux/$BIN" "$game/$NAME"
    stage_common "$ROOT/release-build/linux" "$game"
    sed "s/@VERSION@/$VERSION/" "$ROOT/tools/release/AppRun" > "$appdir/AppRun"
    chmod +x "$appdir/AppRun"
    cp "$ROOT/tools/release/$NAME.desktop" "$appdir/"
    cp "$ROOT/tools/release/$NAME.png" "$appdir/"

    # Bundle SDL2 (dynamic on purpose: it dlopens X11/Wayland/audio backends
    # from the host). libGL/libOpenGL always come from the host driver.
    in_docker musashi-release-linux Dockerfile.linux "
        strip release-build/AppDir/usr/share/$NAME/$NAME &&
        cp -L \$(ldconfig -p | awk '/libSDL2-2.0.so.0 /{print \$NF; exit}') release-build/AppDir/usr/share/$NAME/lib/"

    # Assert the binary needs nothing beyond glibc/GL/X11 and the bundled SDL2.
    local extra
    extra=$(objdump -p "$game/$NAME" | awk '/NEEDED/{print $2}' |
            grep -vE '^(libc|libm|libdl|libpthread|librt|ld-linux-x86-64)\.so|^libSDL2-2\.0\.so|^lib(OpenGL|GL|GLX|X11|Xext)\.so' || true)
    [ -z "$extra" ] || { echo "unexpected shared deps: $extra" >&2; exit 1; }

    # appimagetool runs on the host: inside the container (uid without a passwd
    # entry) it exits 1 with no message.
    local tool="$ROOT/release-build/appimagetool"
    [ -x "$tool" ] || { curl -fsSL -o "$tool" \
        https://github.com/AppImage/appimagetool/releases/download/1.9.0/appimagetool-x86_64.AppImage &&
        chmod +x "$tool"; }
    ARCH=x86_64 "$tool" --appimage-extract-and-run "$appdir" \
        "$OUT/$NAME-$VERSION-linux-x86_64.AppImage" >/dev/null
    echo "wrote dist/$NAME-$VERSION-linux-x86_64.AppImage"
}

build_windows() {
    local build=/src/release-build/windows
    in_docker musashi-release-windows Dockerfile.windows "
        mingw64-cmake -S runtime -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release \
              -DPSX_DEBUG_TOOLS=OFF -DPSX_LAUNCHER=ON >/dev/null &&
        ninja -C $build musashi-runtime"

    local stage="$ROOT/release-build/$NAME-windows-x64"
    rm -rf "$stage"; mkdir -p "$stage"
    cp "$ROOT/release-build/windows/$BIN.exe" "$stage/$NAME.exe"
    stage_common "$ROOT/release-build/windows" "$stage"

    # Same self-containment rule as tools/package_release.ps1: system DLLs only.
    local nonsys
    nonsys=$(in_docker musashi-release-windows Dockerfile.windows \
                 "x86_64-w64-mingw32-strip release-build/$NAME-windows-x64/$NAME.exe &&
                  x86_64-w64-mingw32-objdump -p release-build/$NAME-windows-x64/$NAME.exe" |
             awk '/DLL Name:/{print tolower($3)}' | sed 's/\.dll$//' |
             grep -vxE "$SYSTEM_DLLS" || true)
    [ -z "$nonsys" ] || { echo "exe imports non-system DLLs: $nonsys" >&2; exit 1; }

    (cd "$ROOT/release-build" && rm -f "$OUT/$NAME-$VERSION-windows-x64.zip" &&
     python3 -m zipfile -c "$OUT/$NAME-$VERSION-windows-x64.zip" "$NAME-windows-x64")
    echo "wrote dist/$NAME-$VERSION-windows-x64.zip"
}

case "$TARGET" in
    linux)   build_linux ;;
    windows) build_windows ;;
    all)     build_linux; build_windows ;;
    *) echo "usage: $0 linux|windows|all [version]" >&2; exit 2 ;;
esac
