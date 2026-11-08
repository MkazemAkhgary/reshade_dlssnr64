// Checks language matching and the resolve cache against the generated table.
#include <cstdio>
#include <cstring>

#include "localization/localization.h"

using namespace reshade_dlssnr::localization;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_failures;
}

}  // namespace

int main() {
  Check(std::strcmp(MatchLanguage("zh-CN"), "zh-Hans") == 0,
        "zh-CN maps to zh-Hans");
  Check(std::strcmp(MatchLanguage("zh-SG"), "zh-Hans") == 0,
        "zh-SG maps to zh-Hans");
  Check(MatchLanguage("zh-TW") == nullptr, "zh-TW is not simplified");
  Check(std::strcmp(MatchLanguage("en-US"), "en") == 0, "en-US maps to en");
  Check(MatchLanguage("") == nullptr, "an empty tag has no match");

  const TextRef ref("prefs", "language", "Language");
  const TextRef missing("prefs", "no.such.key", "Fallback");

  SetLanguage("en");
  Check(std::strcmp(ref.c_str(), "Language") == 0,
        "English uses the fallback text");

  SetLanguage("zh-Hans");
  const char* resolved = ref.c_str();
  Check(std::strcmp(resolved, "语言") == 0,
        "zh-Hans resolves the translation");
  Check(ref.c_str() == resolved, "a repeated resolve hits the cache");
  Check(std::strcmp(missing.c_str(), "Fallback") == 0,
        "a missing key uses the fallback");

  SetLanguage("en");
  Check(std::strcmp(ref.c_str(), "Language") == 0,
        "switching back invalidates the cache");

  SetLanguage("zh-Hans");
  Check(std::strcmp(ref.c_str(), "语言") == 0,
        "switching again resolves again");

  std::printf("failures: %d\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
