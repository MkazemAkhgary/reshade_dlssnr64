#include "localization/overlay_font.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <windows.h>

#include <cfloat>
#include <climits>
#include <cstring>
#include <string>
#include <vector>

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

struct MappedFont {
  const void* data;
  int size;
};

// One group per script the add-on can display. EnsureOverlayFont maps the first
// existing file in every group, so the overlay covers every script its
// translations use at once. The font is only consulted for glyph presence
// (IsGlyphInFont); glyphs are baked lazily as text is drawn, so merging several
// large system fonts costs nothing until their characters actually appear.
const wchar_t* const kCjkFonts[] = {
    L"msyh.ttc", L"simsun.ttc", L"msjh.ttc", L"mingliu.ttc",
    L"Deng.ttf", L"simhei.ttf", L"BIZ-UDGothicR.ttc", L"msgothic.ttc",
    L"YuGothM.ttc", L"meiryo.ttc",
};
const wchar_t* const kKoreanFonts[] = {
    L"malgun.ttf", L"gulim.ttc", L"batang.ttc", L"malgungh.ttf",
};
// Tahoma carries Arabic, Cyrillic and Thai in one file; Segoe UI and Arial are
// system fallbacks with the same coverage.
const wchar_t* const kBroadFonts[] = {
    L"tahoma.ttf", L"segoeui.ttf", L"arial.ttf",
};
constexpr size_t kCjkCount = sizeof(kCjkFonts) / sizeof(kCjkFonts[0]);
constexpr size_t kKoreanCount = sizeof(kKoreanFonts) / sizeof(kKoreanFonts[0]);
constexpr size_t kBroadCount = sizeof(kBroadFonts) / sizeof(kBroadFonts[0]);

// Keep the views alive: ImGui reads FontData whenever it needs a new glyph, and
// an atlas rebuild drops the source but not the mapped view.
std::vector<MappedFont> g_fonts;

bool MapFont(const wchar_t* directory, const wchar_t* name, MappedFont& out) {
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

  out.data = view;
  out.size = static_cast<int>(bytes.QuadPart);
  return true;
}

// Maps the first existing file in `names`; appended to g_fonts if found.
void MapFirstExisting(const wchar_t* directory, const wchar_t* const* names,
                      size_t count) {
  for (size_t i = 0; i < count; ++i) {
    MappedFont mf = {};
    if (MapFont(directory, names[i], mf)) {
      g_fonts.push_back(mf);
      return;
    }
  }
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

  // Map each script's font once; reuse the views after an atlas rebuild.
  if (g_fonts.empty()) {
    wchar_t directory[MAX_PATH] = {};
    const UINT length = GetWindowsDirectoryW(directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return;
    MapFirstExisting(directory, kCjkFonts, kCjkCount);
    MapFirstExisting(directory, kKoreanFonts, kKoreanCount);
    MapFirstExisting(directory, kBroadFonts, kBroadCount);
  }

  bool added_any = false;
  for (MappedFont& mf : g_fonts) {
    if (mf.data == nullptr) continue;
    if (AlreadyAdded(atlas, mf.data)) continue;
    ImFontConfig config;
    config.FontData = const_cast<void*>(mf.data);
    config.FontDataSize = mf.size;
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
      UnmapViewOfFile(mf.data);
      mf.data = nullptr;
      continue;
    }
    added_any = true;
  }
  if (added_any) atlas.TexIsBuilt = false;
}

}  // namespace localization
}  // namespace reshade_dlssnr
