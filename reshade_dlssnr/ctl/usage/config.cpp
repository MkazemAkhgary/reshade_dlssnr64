// ReshadeConfigWriter / ReshadeConfigReader. See config.h.

#include "ctl/usage/config.h"

#include <cstring>
#include <vector>

#include <reshade.hpp>

namespace reshade_dlssnr {
namespace ctl {

// The global config, not a runtime's. `state/settings.cpp` has always used
// nullptr here for the addon's own keys, and the overlay draws its settings
// from the same file on every game.
//
// Typed as the parameter's own type rather than `void*`: ReShade's overloads
// are declared for `effect_runtime*`, and a void* would not convert.
constexpr reshade::api::effect_runtime* kGlobalConfig = nullptr;

void ReshadeConfigWriter::WriteBool(const std::string& key, bool value) {
  // ReShade stores bools as ints; its own set_config_value<bool> does exactly
  // this, so spell it out rather than rely on the template resolution.
  reshade::set_config_value(kGlobalConfig, kConfigSection, key.c_str(),
                            value ? 1 : 0);
}

void ReshadeConfigWriter::WriteInt(const std::string& key, int value) {
  reshade::set_config_value(kGlobalConfig, kConfigSection, key.c_str(), value);
}

void ReshadeConfigWriter::WriteFloat(const std::string& key, float value) {
  reshade::set_config_value(kGlobalConfig, kConfigSection, key.c_str(), value);
}

void ReshadeConfigWriter::WriteString(const std::string& key,
                                      const std::string& value) {
  reshade::set_config_value(kGlobalConfig, kConfigSection, key.c_str(),
                            value.c_str());
}

void ReshadeConfigWriter::Erase(const std::string& key) {
  reshade::set_config_value(kGlobalConfig, kConfigSection, key.c_str(),
                            static_cast<const char*>(nullptr));
}

bool ReshadeConfigReader::ReadBool(const std::string& key, bool* out) {
  int value = 0;
  if (!reshade::get_config_value(kGlobalConfig, kConfigSection, key.c_str(),
                                 value)) {
    return false;
  }
  *out = value != 0;
  return true;
}

bool ReshadeConfigReader::ReadInt(const std::string& key, int* out) {
  int value = 0;
  if (!reshade::get_config_value(kGlobalConfig, kConfigSection, key.c_str(),
                                 value)) {
    return false;
  }
  *out = value;
  return true;
}

bool ReshadeConfigReader::ReadFloat(const std::string& key, float* out) {
  float value = 0.0f;
  if (!reshade::get_config_value(kGlobalConfig, kConfigSection, key.c_str(),
                                 value)) {
    return false;
  }
  *out = value;
  return true;
}

bool ReshadeConfigReader::ReadString(const std::string& key, std::string* out) {
  // ReShade's scalar template reads into a fixed 32-byte buffer, which is far
  // too small for a settings string. The char* overload takes the size as an
  // in/out parameter: pass a null buffer to query the needed size, then read
  // into a buffer of that size.
  size_t needed = 0;
  if (!reshade::get_config_value(kGlobalConfig, kConfigSection, key.c_str(),
                                 nullptr, &needed)) {
    return false;  // absent
  }
  if (needed == 0) {
    *out = std::string();
    return true;
  }

  std::vector<char> buffer(needed + 1, '\0');
  size_t length = buffer.size();
  if (!reshade::get_config_value(kGlobalConfig, kConfigSection, key.c_str(),
                                 buffer.data(), &length)) {
    return false;
  }
  // `length` is documented to include the null terminator. Trim to the text
  // itself so the stored value never carries a stray null.
  const char* text_end = static_cast<const char*>(
      std::memchr(buffer.data(), '\0', buffer.size()));
  out->assign(buffer.data(),
              text_end != nullptr ? static_cast<size_t>(text_end - buffer.data())
                                  : buffer.size());
  return true;
}

}  // namespace ctl
}  // namespace reshade_dlssnr
