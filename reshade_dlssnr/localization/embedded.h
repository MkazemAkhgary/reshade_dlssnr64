// The translation tables compiled into this binary.
//
// Written by tools/localization.py from localization/translations and the
// languages this build was told to embed. The generated half lives in
// generated/localization_tables.cpp; never edit it by hand, the next build
// overwrites it.

#pragma once

#include <cstddef>

namespace reshade_dlssnr {
namespace localization {

struct Translation {
  const char* ns;
  const char* key;
  const char* text;
};

struct EmbeddedLanguage {
  const char* code;
  const char* name;
  // Sorted by (ns, key), so a lookup can bisect instead of scanning.
  const Translation* translations;
  size_t translation_count;
};

// English is always first and carries no table: its text is the fallback each
// TextRef already holds.
extern const EmbeddedLanguage kEmbeddedLanguages[];
extern const size_t kEmbeddedLanguageCount;

}  // namespace localization
}  // namespace reshade_dlssnr
