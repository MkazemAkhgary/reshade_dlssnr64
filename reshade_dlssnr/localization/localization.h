// Localized text for the overlay.
//
// A string is declared where it is used: its translation namespace, its key
// within that namespace, and the English text. The key is what a translation
// table refers to, so moving a string to another file or reflowing its English
// wording leaves the translations intact.
//
// The English text travels with the declaration. That is what makes English
// always available without a table, and what lets an untranslated string fall
// back to something readable instead of a key.

#pragma once

#include <vector>

// Defined by the project; a translation-free build still has to compile, so an
// undefined switch means the feature is on.
#ifndef DLSSNR_LOCALIZATION_ENABLED
#define DLSSNR_LOCALIZATION_ENABLED 1
#endif

namespace reshade_dlssnr {
namespace localization {

// One declared string. Every field points at a string literal, so a TextRef is
// trivially copyable and safe to hold in static tables.
struct TextRef {
  const char* ns = nullptr;        // translation namespace
  const char* key = nullptr;       // unique within `ns`
  const char* fallback = nullptr;  // English text

  TextRef() = default;

  // A string with no translation identity, such as a section name that doubles
  // as a persistence key. It is never translated.
  TextRef(const char* text)  // NOLINT(google-explicit-constructor)
      : ns(nullptr), key(text), fallback(text) {}

  TextRef(const char* text_ns, const char* text_key, const char* text_fallback)
      : ns(text_ns), key(text_key), fallback(text_fallback) {}

  // The text to display: the translation for the current language, or the
  // English fallback when this build has no translation for it.
  const char* c_str() const {
#if DLSSNR_LOCALIZATION_ENABLED
    return Resolve(*this);
#else
    return fallback_text();
#endif
  }
  operator const char*() const { return c_str(); }  // NOLINT

  bool empty() const { return fallback == nullptr || fallback[0] == '\0'; }

  // The language-independent form. Traces and config keys use this, so
  // changing the language never changes what gets persisted.
  const char* fallback_text() const {
    return fallback != nullptr ? fallback : "";
  }

 private:
  static const char* Resolve(const TextRef& ref);
};

// One selectable language. `code` is what the settings file stores; `name` is
// the language's own name, so the list stays readable whatever language is
// active.
struct Language {
  const char* code;
  const char* name;
};

// Every language compiled into this binary, English first.
const std::vector<Language>& Languages();

// Select a language by code. A code this build does not carry selects English,
// so a settings file written by another build cannot leave the overlay blank.
void SetLanguage(const char* code);

// Supplied by the overlay so this module stays free of font-library types.
using GlyphLookup = bool (*)(unsigned int codepoint, const void* context);

// Re-test coverage when the language or font changes; returns true when it did.
// font_revision must change with the font's sources; a rebuilt atlas can reuse the address.
bool SyncFontCoverage(GlyphLookup has_glyph, const void* context,
                      unsigned int font_revision);

// False makes every string resolve to English.
bool ActiveLanguageRenderable();

// Endonym, or its code when the font cannot draw it.
const char* LanguageDisplayName(size_t index);

// Maps a host language tag onto an embedded code, or nullptr.
const char* MatchLanguage(const char* tag);

}  // namespace localization
}  // namespace reshade_dlssnr

// Declare a localized string: translation namespace, key, English text.
#define NSLOC_TEXT(ns, key, default_text) \
  ::reshade_dlssnr::localization::TextRef(ns, key, default_text)

// Per-file shorthand. A file defines LOC_NAMESPACE itself, at the top, to its
// translation namespace as a string literal:
//
//   #define LOC_NAMESPACE "pass"
//
// and undefines it at the end. LOC_TEXT then needs only a key and the English
// text. The namespace belongs to one file, so a file that forgets the undef
// leaks it into whatever it includes next, and a file that never defines it
// fails to compile instead of quietly declaring into a stale namespace.
#define LOC_TEXT(key, default_text) NSLOC_TEXT(LOC_NAMESPACE, key, default_text)
