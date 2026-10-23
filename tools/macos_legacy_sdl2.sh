#!/usr/bin/env bash
# tools/macos_legacy_sdl2.sh
#
# Builds an x86_64 SDL2.framework for macOS 10.9 into
# build/deps/SDL2-10.9/SDL2.framework, for a CMAKE_OSX_DEPLOYMENT_TARGET=10.9
# build (BUILD.md#macos-109): the official framework ./build.sh downloads needs
# 10.11. tools/macos_legacy_sdl2.patch keeps SDL's live-resize timer off a
# macOS 10.12 NSTimer method, which would crash on resizing the window.
set -euo pipefail

cd "$(dirname "$0")/.."
ver=2.30.11
deps=$PWD/build/deps
src=$deps/SDL2-$ver-src
out=$deps/SDL2-10.9
fw=$out/SDL2.framework

mkdir -p "$deps"
tarball=$deps/SDL2-$ver.tar.gz
if [[ ! -f "$tarball" ]]; then
  curl -fsSL -o "$tarball.part" "https://github.com/libsdl-org/SDL/releases/download/release-$ver/SDL2-$ver.tar.gz"
  mv "$tarball.part" "$tarball"
fi
rm -rf "$src"
mkdir -p "$src"
tar -xzf "$tarball" -C "$src" --strip-components 1
patch -d "$src" -p1 < tools/macos_legacy_sdl2.patch

cmake -S "$src" -B "$src/build" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=10.9 \
  -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF
cmake --build "$src/build" --parallel

# The framework layout the game links and bundles (Versions/A, as SDL's own).
rm -rf "$out"
mkdir -p "$fw/Versions/A/Headers" "$fw/Versions/A/Resources"
cp -L "$src/build/libSDL2-2.0.0.dylib" "$fw/Versions/A/SDL2"
install_name_tool -id @rpath/SDL2.framework/Versions/A/SDL2 "$fw/Versions/A/SDL2"
cp "$src"/include/*.h "$fw/Versions/A/Headers/"
cp "$src"/build/include/SDL2/*.h "$src"/build/include-config-release/SDL2/*.h "$fw/Versions/A/Headers/"
cat > "$fw/Versions/A/Resources/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key>
	<string>SDL2</string>
	<key>CFBundleIdentifier</key>
	<string>org.libsdl.SDL2</string>
	<key>CFBundleName</key>
	<string>SDL2</string>
	<key>CFBundlePackageType</key>
	<string>FMWK</string>
	<key>CFBundleShortVersionString</key>
	<string>$ver</string>
	<key>CFBundleVersion</key>
	<string>$ver</string>
	<key>LSMinimumSystemVersion</key>
	<string>10.9</string>
</dict>
</plist>
EOF
ln -s A "$fw/Versions/Current"
for name in SDL2 Headers Resources; do ln -s "Versions/Current/$name" "$fw/$name"; done
codesign -f -s - "$fw"
echo "Built $fw"
