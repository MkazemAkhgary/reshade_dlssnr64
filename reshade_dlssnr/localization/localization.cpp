#include "localization/localization.h"

#include <cstring>
#include <vector>

#include "localization/embedded.h"

namespace reshade_dlssnr {
namespace localization {

namespace {

// Index into kEmbeddedLanguages. English is index 0.
size_t g_language = 0;

// Missing glyphs resolve the whole language to English.
bool g_renderable = true;

// Endonym, or the code when the font cannot draw it.
std::vector<const char*> g_language_names;

// Skip the per-frame scan when nothing relevant changed.
bool g_coverage_tested = false;
size_t g_coverage_language = 0;
const void* g_coverage_font = nullptr;
unsigned int g_coverage_revision = 0;

int Compare(const Translation& entry, const TextRef& ref) {
  const int by_ns = std::strcmp(entry.ns, ref.ns);
  return by_ns != 0 ? by_ns : std::strcmp(entry.key, ref.key);
}

// Invalid bytes are returned as-is so a malformed table cannot stall the walk.
unsigned int NextCodepoint(const char*& text) {
  const unsigned char lead = static_cast<unsigned char>(*text++);
  if (lead < 0x80) return lead;

  unsigned int value = 0;
  int continuation = 0;
  if ((lead & 0xE0) == 0xC0) {
    value = lead & 0x1Fu;
    continuation = 1;
  } else if ((lead & 0xF0) == 0xE0) {
    value = lead & 0x0Fu;
    continuation = 2;
  } else if ((lead & 0xF8) == 0xF0) {
    value = lead & 0x07u;
    continuation = 3;
  } else {
    return lead;
  }

  for (int i = 0; i < continuation; ++i) {
    const unsigned char next = static_cast<unsigned char>(*text);
    if ((next & 0xC0) != 0x80) return value;
    value = (value << 6) | (next & 0x3Fu);
    ++text;
  }
  return value;
}

bool TextRenderable(const char* text, GlyphLookup has_glyph,
                    const void* context) {
  if (text == nullptr) return true;
  while (*text != '\0') {
    if (!has_glyph(NextCodepoint(text), context)) return false;
  }
  return true;
}

}  // namespace

const char* TextRef::Resolve(const TextRef& ref) {
  // English needs no table: every declaration already carries its English text.
  if (g_language == 0 || !g_renderable || ref.ns == nullptr ||
      ref.key == nullptr) {
    return ref.fallback_text();
  }

  const EmbeddedLanguage& language = kEmbeddedLanguages[g_language];
  size_t lo = 0;
  size_t hi = language.translation_count;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const int order = Compare(language.translations[mid], ref);
    if (order == 0) return language.translations[mid].text;
    if (order < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return ref.fallback_text();
}

const std::vector<Language>& Languages() {
  static const std::vector<Language> languages = [] {
    std::vector<Language> result;
    result.reserve(kEmbeddedLanguageCount);
    for (size_t i = 0; i < kEmbeddedLanguageCount; ++i) {
      result.push_back({kEmbeddedLanguages[i].code, kEmbeddedLanguages[i].name});
    }
    return result;
  }();
  return languages;
}

void SetLanguage(const char* code) {
  g_language = 0;
  if (code == nullptr) return;
  for (size_t i = 0; i < kEmbeddedLanguageCount; ++i) {
    if (std::strcmp(kEmbeddedLanguages[i].code, code) == 0) {
      g_language = i;
      return;
    }
  }
}

bool SyncFontCoverage(GlyphLookup has_glyph, const void* context,
                      unsigned int font_revision) {
  if (has_glyph == nullptr) return false;
  if (g_coverage_tested && g_coverage_language == g_language &&
      g_coverage_font == context &&
      g_coverage_revision == font_revision) {
    return false;
  }
  g_coverage_tested = true;
  g_coverage_language = g_language;
  g_coverage_font = context;
  g_coverage_revision = font_revision;

  // English text travels with every declaration, so it needs no test.
  g_renderable = true;
  if (g_language != 0) {
    const EmbeddedLanguage& language = kEmbeddedLanguages[g_language];
    for (size_t i = 0; i < language.translation_count; ++i) {
      if (!TextRenderable(language.translations[i].text, has_glyph, context)) {
        g_renderable = false;
        break;
      }
    }
  }

  g_language_names.clear();
  g_language_names.reserve(kEmbeddedLanguageCount);
  for (size_t i = 0; i < kEmbeddedLanguageCount; ++i) {
    const char* name = kEmbeddedLanguages[i].name;
    g_language_names.push_back(
        TextRenderable(name, has_glyph, context) ? name
                                                 : kEmbeddedLanguages[i].code);
  }
  return true;
}

bool ActiveLanguageRenderable() { return g_renderable; }

const char* LanguageDisplayName(size_t index) {
  if (index < g_language_names.size()) return g_language_names[index];
  return index < kEmbeddedLanguageCount ? kEmbeddedLanguages[index].name : "";
}

}  // namespace localization
}  // namespace reshade_dlssnr
