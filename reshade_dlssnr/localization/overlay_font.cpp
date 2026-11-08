#include "localization/overlay_font.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <windows.h>

#include <cfloat>
#include <climits>
#include <cstring>
#include <string>

// ReShade does not export this constructor, so provide ImGui's trivial one.
ImFontConfig::ImFontConfig() {
  std::memset(this, 0, sizeof(*this));
  FontDataOwnedByAtlas = true;
  GlyphMaxAdvanceX = FLT_MAX;
  RasterizerMultiply = 1.0f;
  RasterizerDensity = 1.0f;
}

namespace reshade_dlssnr {
namespace localization {
namespace {

// The fonts ReShade loads for its CJK languages, simplified Chinese first.
const wchar_t* const kFontNames[] = {
    L"msyh.ttc", L"simsun.ttc", L"msjh.ttc", L"mingliu.ttc",
    L"Deng.ttf", L"simhei.ttf", L"BIZ-UDGothicR.ttc", L"msgothic.ttc",
    L"YuGothM.ttc", L"meiryo.ttc", L"malgun.ttf",
};
constexpr size_t kFontCount = sizeof(kFontNames) / sizeof(kFontNames[0]);

// Keep the view alive: ImGui reads FontData whenever it needs a new glyph.
const void* g_font_data = nullptr;
int g_font_size = 0;
size_t g_next_font = 0;

bool MapFont(const wchar_t* directory, const wchar_t* name) {
  std::wstring path(directory);
  path += L"\\Fonts\\";
  path += name;

  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (mapping == nullptr) {
    CloseHandle(file);
    return false;
  }

  const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
  LARGE_INTEGER bytes = {};
  const bool sized = GetFileSizeEx(file, &bytes) && bytes.QuadPart > 0 &&
                     bytes.QuadPart <= INT_MAX;
  CloseHandle(mapping);
  CloseHandle(file);
  if (view == nullptr || !sized) {
    if (view != nullptr) UnmapViewOfFile(view);
    return false;
  }

  g_font_data = view;
  g_font_size = static_cast<int>(bytes.QuadPart);
  return true;
}

bool FindFont() {
  wchar_t directory[MAX_PATH] = {};
  const UINT length = GetWindowsDirectoryW(directory, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) return false;
  while (g_next_font < kFontCount)
    if (MapFont(directory, kFontNames[g_next_font++])) return true;
  return false;
}

bool AlreadyAdded(const ImFontAtlas& atlas, const void* data) {
  for (const ImFontConfig& source : atlas.Sources)
    if (source.FontData == data) return true;
  return false;
}

// Appending can reallocate Sources, leaving the fonts with dangling pointers.
void RebuildSourcePointers(ImFontAtlas& atlas) {
  for (ImFont* font : atlas.Fonts) font->Sources.resize(0);
  for (ImFontConfig& source : atlas.Sources)
    source.DstFont->Sources.push_back(&source);
}

}  // namespace

void EnsureOverlayFont() {
  ImFont* active = ImGui::GetFont();
  if (active == nullptr || active->OwnerAtlas == nullptr) return;
  ImFontAtlas& atlas = *active->OwnerAtlas;

  // Legacy backends lock the atlas; some loaders need storage per baked font.
  if (atlas.Locked || atlas.FontLoader == nullptr ||
      atlas.FontLoader->FontSrcInit == nullptr ||
      atlas.FontLoader->FontBakedSrcLoaderDataSize != 0) return;

  // An atlas rebuild drops the source but not the view.
  if (g_font_data != nullptr) {
    if (AlreadyAdded(atlas, g_font_data)) return;
  } else {
    if (g_next_font >= kFontCount || !FindFont()) return;
  }

  ImFontConfig config;
  config.FontData = const_cast<void*>(g_font_data);
  config.FontDataSize = g_font_size;
  config.FontDataOwnedByAtlas = false;
  config.SizePixels = 0.0f;
  config.MergeMode = true;
  config.PixelSnapH = true;
  config.DstFont = active;

  atlas.Sources.push_back(config);
  RebuildSourcePointers(atlas);

  ImFontConfig* added = &atlas.Sources.back();
  if (!atlas.FontLoader->FontSrcInit(&atlas, added)) {
    atlas.Sources.pop_back();
    RebuildSourcePointers(atlas);
    UnmapViewOfFile(g_font_data);
    g_font_data = nullptr;
    return;
  }
  atlas.TexIsBuilt = false;
}

}  // namespace localization
}  // namespace reshade_dlssnr
