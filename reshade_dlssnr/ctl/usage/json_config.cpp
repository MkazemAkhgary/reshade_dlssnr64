#include "ctl/usage/json_config.h"
#include "profiling/profile.h"

namespace reshade_dlssnr::ctl {

void JsonConfigWriter::WriteBool(const std::string& key, bool value) {
  DLSSNR_PROFILE_CTL_L3("JsonConfigWriter::WriteBool");
  root_.ensure(key) = json::Value(value);
}

void JsonConfigWriter::WriteInt(const std::string& key, int value) {
  DLSSNR_PROFILE_CTL_L3("JsonConfigWriter::WriteInt");
  root_.ensure(key) = json::Value(value);
}

void JsonConfigWriter::WriteFloat(const std::string& key, float value) {
  DLSSNR_PROFILE_CTL_L3("JsonConfigWriter::WriteFloat");
  root_.ensure(key) = json::Value(value);
}

void JsonConfigWriter::WriteString(const std::string& key,
                                   const std::string& value) {
  DLSSNR_PROFILE_CTL_L3("JsonConfigWriter::WriteString");
  root_.ensure(key) = json::Value(value);
}

void JsonConfigWriter::Erase(const std::string& key) {
  DLSSNR_PROFILE_CTL_L3("JsonConfigWriter::Erase");
  root_.erase_path(key);
}

bool JsonConfigReader::ReadBool(const std::string& key, bool* out) {
  DLSSNR_PROFILE_CTL_L3("JsonConfigReader::ReadBool");
  const json::Value* v = root_.find(key);
  if (v == nullptr) return false;
  *out = v->as_bool();
  return true;
}

bool JsonConfigReader::ReadInt(const std::string& key, int* out) {
  DLSSNR_PROFILE_CTL_L3("JsonConfigReader::ReadInt");
  const json::Value* v = root_.find(key);
  if (v == nullptr) return false;
  *out = v->as_int();
  return true;
}

bool JsonConfigReader::ReadFloat(const std::string& key, float* out) {
  DLSSNR_PROFILE_CTL_L3("JsonConfigReader::ReadFloat");
  const json::Value* v = root_.find(key);
  if (v == nullptr) return false;
  *out = v->as_float();
  return true;
}

bool JsonConfigReader::ReadString(const std::string& key, std::string* out) {
  DLSSNR_PROFILE_CTL_L3("JsonConfigReader::ReadString");
  const json::Value* v = root_.find(key);
  if (v == nullptr) return false;
  *out = v->as_string();
  return true;
}

}  // namespace reshade_dlssnr::ctl
