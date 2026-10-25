// A KeyValueWriter and KeyValueReader backed by a json::Value tree.
//
// The tree is the source of truth; this adapter lets ctl::Save/Load work
// against it without knowing anything about JSON. The tree is kept whole and
// rewritten by the caller, which is what removes stale keys: a subtree that
// is written from scratch cannot retain the keys of settings the schema no
// longer declares.
#pragma once

#include <string>

#include "ctl/serialize.h"
#include "ctl/usage/json.h"

namespace reshade_dlssnr::ctl {

class JsonConfigWriter : public KeyValueWriter {
 public:
  explicit JsonConfigWriter(json::Value& root) : root_(root) {}

  void WriteBool(const std::string& key, bool value) override;
  void WriteInt(const std::string& key, int value) override;
  void WriteFloat(const std::string& key, float value) override;
  void WriteString(const std::string& key, const std::string& value) override;
  void Erase(const std::string& key) override;

 private:
  json::Value& root_;
};

class JsonConfigReader : public KeyValueReader {
 public:
  explicit JsonConfigReader(const json::Value& root) : root_(root) {}

  bool ReadBool(const std::string& key, bool* out) override;
  bool ReadInt(const std::string& key, int* out) override;
  bool ReadFloat(const std::string& key, float* out) override;
  bool ReadString(const std::string& key, std::string* out) override;

 private:
  const json::Value& root_;
};

}  // namespace reshade_dlssnr::ctl
