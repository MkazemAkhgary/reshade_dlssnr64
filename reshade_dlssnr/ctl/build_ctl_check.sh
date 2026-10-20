#!/usr/bin/env bash
# Type-check the ctl framework TUs only. The shared check.sh hardcodes an
# object-dir argument; this variant passes an explicit /Fo and adds the ctl
# include root so ctl/*.h resolves.
MSVC="/c/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231"
SDK="/c/Program Files (x86)/Windows Kits/10"; SDKV="10.0.26100.0"
MSVCW='C:/Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.51.36231'
SDKW='C:/Program Files (x86)\Windows Kits\10'
export PATH="$MSVC/bin/Hostx64/x64:$SDK/bin/$SDKV/x64:$PATH"
export INCLUDE="$MSVCW\\include;$SDKW\\Include\\$SDKV\\ucrt;$SDKW\\Include\\$SDKV\\um;$SDKW\\Include\\$SDKV\\shared;$SDKW\\Include\\$SDKV\\winrt;$SDKW\\Include\\$SDKV\\cppwinrt"
PROJ="${PROJ_ROOT:?set PROJ_ROOT}"
PROJW="$(cygpath -w "$PROJ")"
RE="$(cygpath -w "$PROJ/third_party/reshade")"
OUT="C:/Users/Coda/AppData/Local/Temp/dlss5nrobj/ctl"
mkdir -p /c/Users/Coda/AppData/Local/Temp/dlss5nrobj/ctl
cd "$PROJ" || exit 1
FAIL=0
for f in "$@"; do
  out=$(MSYS2_ARG_CONV_EXCL='*' cl.exe /nologo /c /W3 /permissive- /std:c++20 /EHsc \
    /DNDEBUG /DWIN32 /D_WINDOWS /D_USRDLL /DNOMINMAX /DWIN32_LEAN_AND_MEAN \
    "/I$PROJW" "/I$PROJW/generated" "/I$RE/include" "/I$RE" "/I$RE/deps/imgui" \
    "/I$PROJW/third_party" \
    "/I$PROJW/third_party/nvngx_dlss_sdk" "/I$PROJW/third_party/d3dx" \
    "/I$PROJW/third_party/detours" "/I$PROJW/gpu" "/I$PROJW/model" \
    "/I$PROJW/pipeline" "/I$PROJW/hooks" "/I$PROJW/ui" "/I$PROJW/state" \
    "/I$PROJW/compose" "/I$PROJW/grade" "/I$PROJW/hdr" \
    "/I$PROJW/importance_mask" "/I$PROJW/debug" \
    "/Fo$OUT/" "$f" 2>&1)
  rc=$?
  ne=$(printf '%s\n' "$out" | grep -c "error C")
  nw=$(printf '%s\n' "$out" | grep -c "warning C")
  echo "$f  exit=$rc warnings=$nw errors=$ne"
  if [ "$rc" != "0" ] || [ "$ne" != "0" ]; then
    printf '%s\n' "$out" | grep -E "error|warning" | head -25
    FAIL=1
  fi
done
[ "$FAIL" = "0" ] && echo "ALL CLEAN" || echo "SEE ERRORS ABOVE"
