#!/usr/bin/env bash
# Build and run the localization test with MSVC.
# Usage: bash localization/build_test.sh
set -e

MSVC="/c/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231"
SDK="/c/Program Files (x86)/Windows Kits/10"
SDKV="10.0.26100.0"
MSVCW='C://Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231'
SDKW='C://Program Files (x86)\Windows Kits\10'

PROJ="$(cd "$(dirname "$0")/.." && pwd)"
PROJW="$(cygpath -w "$PROJ" 2>/dev/null || echo "$PROJ")"

export PATH="$MSVC/bin/Hostx64/x64:$SDK/bin/$SDKV/x64:$PATH"
export INCLUDE="$MSVCW\\include;$SDKW\\Include\\$SDKV\\ucrt;$SDKW\\Include\\$SDKV\\um;$SDKW\\Include\\$SDKV\\shared"
export LIB="$MSVCW\\lib\\x64;$SDKW\\Lib\\$SDKV\\ucrt\\x64;$SDKW\\Lib\\$SDKV\\um\\x64"

OUT="C:/Users/Coda/AppData/Local/Temp/localization_test"
mkdir -p "$OUT"
cd "$PROJ"

MSYS2_ARG_CONV_EXCL='*' cl.exe /nologo /W3 /permissive- /std:c++20 /EHsc \
  /DNDEBUG /DWIN32 /D_WINDOWS /D_USRDLL /DNOMINMAX /DWIN32_LEAN_AND_MEAN \
  "/I$PROJW" /utf-8 \
  "/Fo$OUT/" /Fe"$OUT/localization_test.exe" \
  localization/localization_test.cpp localization/localization.cpp \
  generated/localization_tables.cpp

echo "--- running ---"
"$OUT/localization_test.exe"
