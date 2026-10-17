#!/usr/bin/env bash
# 光影 —— 一键：构建 → 同步产物到 Package/Resources/lib → 打安装包 + 源码包
# 用法: ./pack.sh [程序集名后缀]（默认 android.arm64）
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SUFFIX="${1:-android.arm64}"

"$DIR/../make.sh" "$DIR" -t android -a arm64-v8a

mkdir -p "$DIR/Package/Resources/lib"
cp -f "$DIR/build/libLightsAndShadows.so" \
      "$DIR/Package/Resources/lib/libLightsAndShadows.${SUFFIX}.so"

cd "$DIR/Package"
rm -f "$DIR/../dist/光影(LightsAndShadows)v1.0.0.zip"
zip -qr "$DIR/../dist/光影(LightsAndShadows)v1.0.0.zip" Info.json Manifest.json mymod.json Resources

cd "$DIR"
rm -f "$DIR/../dist/光影(LightsAndShadows)-源码-v1.0.0.zip"
zip -qr "$DIR/../dist/光影(LightsAndShadows)-源码-v1.0.0.zip" \
       main.c CMakeLists.txt LICENSE Package mod-api tefkernel xdl LICENSES

echo "== 完成 =="
ls -la "$DIR/Package/Resources/lib/"
md5sum "$DIR/build/libLightsAndShadows.so" "$DIR/Package/Resources/lib/"*.so
ls -la "$DIR/../dist/" | grep -i lights
