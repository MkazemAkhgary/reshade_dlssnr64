#include "localization/localization.h"

#include <cstring>

#include "localization/embedded.h"

namespace reshade_dlssnr {
namespace localization {

namespace {

// Index into kEmbeddedLanguages. English is index 0.
size_t g_language = 0;

int Compare(const Translation& entry, const TextRef& ref) {
  const int by_ns = std::strcmp(entry.ns, ref.ns);
  return by_ns != 0 ? by_ns : std::strcmp(entry.key, ref.key);
}

}  // namespace

const char* TextRef::Resolve(const TextRef& ref) {
  // English needs no table: every declaration already carries its English text.
  if (g_language == 0 || ref.ns == nullptr || ref.key == nullptr) {
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

}  // namespace localization
}  // namespace reshade_dlssnr
