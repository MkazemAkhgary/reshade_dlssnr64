#!/usr/bin/env bash
# Build and run the ctl acceptance harness with MSVC.
# Usage: bash ctl/build_ctl_acceptance.sh
#
# Links the real ctl sources plus ImGui (needed by the built-in setting rows
# and the drawer), so every acceptance criterion is exercised against the
# actual implementation.
set -e

MSVC="/c/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231"
SDK="/c/Program Files (x86)/Windows Kits/10"
SDKV="10.0.26100.0"
MSVCW='C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231'
SDKW='C:\Program Files (x86)\Windows Kits\10'

PROJ="$(cd "$(dirname "$0")/.." && pwd)"
PROJW="$(cygpath -w "$PROJ" 2>/dev/null || echo "$PROJ")"

export PATH="$MSVC/bin/Hostx64/x64:$SDK/bin/$SDKV/x64:$PATH"
export INCLUDE="$MSVCW\\include;$SDKW\\Include\\$SDKV\\ucrt;$SDKW\\Include\\$SDKV\\um;$SDKW\\Include\\$SDKV\\shared;$SDKW\\Include\\$SDKV\\winrt;$SDKW\\Include\\$SDKV\\cppwinrt"
export LIB="$MSVCW\\lib\\x64;$SDKW\\Lib\\$SDKV\\ucrt\\x64;$SDKW\\Lib\\$SDKV\\um\\x64"

OUT="C:/Users/Coda/AppData/Local/Temp/ctl_acceptance"
mkdir -p "$OUT"
cd "$PROJ"

IMGUI="$PROJW/third_party/reshade/deps/imgui"

MSYS2_ARG_CONV_EXCL='*' cl.exe /nologo /W3 /permissive- /std:c++20 /EHsc \
  /DNDEBUG /DWIN32 /D_WINDOWS /D_USRDLL /DNOMINMAX /DWIN32_LEAN_AND_MEAN \
  "/I$PROJW" \
  "/I$IMGUI" \
  "/I$PROJW/ui" \
  "/I$PROJW/third_party/reshade/include" \
  "/I$PROJW/third_party/reshade" \
  "/Fo$OUT/" /Fe"$OUT/ctl_acceptance.exe" \
  ctl/ctl_acceptance.cpp ctl/types.cpp ctl/schema.cpp ctl/values.cpp \
  ctl/registry.cpp ctl/serialize.cpp ctl/builtin_settings.cpp ctl/draw.cpp \
  ctl/trace.cpp ctl/test_row_stubs.cpp ctl/test_imgui_stubs.cpp

echo "--- running ---"
"$OUT/ctl_acceptance.exe"
