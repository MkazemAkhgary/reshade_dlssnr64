// A KeyValueWriter and KeyValueReader backed by ReShade's config API.
//
// This is the only place the framework touches the host. Everything the store
// needs to round-trip goes through the two abstract interfaces in
// ctl/serialize.h, so this file can be swapped for another backend without any
// change above it.
//
// The interface is deliberately narrow: get a value by name, put a value by
// name, erase a value by name, and report absence. It cannot enumerate keys,
// because ReShade has no addon-facing API for that. ctl::Load and ctl::Delete
// therefore take an InstanceKeySource for instanced sections.

#pragma once

#include <string>

#include "ctl/serialize.h"

namespace reshade_dlssnr {
namespace ctl {

// The INI section every addon setting lives under.
inline constexpr const char* kConfigSection = "RESHADE_DLSSNR";

// Writes into the global ReShade config file (the one next to the executable),
// which stores both ctl settings and the addon's top-level/profile metadata.
class ReshadeConfigWriter : public KeyValueWriter {
 public:
  void WriteBool(const std::string& key, bool value) override;
  void WriteInt(const std::string& key, int value) override;
  void WriteFloat(const std::string& key, float value) override;
  void WriteString(const std::string& key, const std::string& value) override;

  // Removes a key from the global ReShade config.
  void Erase(const std::string& key) override;
};

// Reads from the same file. Each method returns false when the key is absent,
// which the loader treats as "keep the declared default" rather than as an
// error.
class ReshadeConfigReader : public KeyValueReader {
 public:
  bool ReadBool(const std::string& key, bool* out) override;
  bool ReadInt(const std::string& key, int* out) override;
  bool ReadFloat(const std::string& key, float* out) override;
  bool ReadString(const std::string& key, std::string* out) override;
};

}  // namespace ctl
}  // namespace reshade_dlssnr
