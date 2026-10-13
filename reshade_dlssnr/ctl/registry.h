// ctl value-type registry.
//
// A setting type registers a name and a factory. The registry is consulted
// only when deserializing a value whose type the framework did not itself
// write, and by callers who want to declare a setting by type name.
//
// The point of the registry is the acceptance rule: adding a setting type is
// one new class plus one registration line. Nothing in the core, in the
// existing types, or at any call site changes.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "ctl/types.h"

namespace reshade_dlssnr {
namespace ctl {

class Registry {
 public:
  using Factory = std::function<std::unique_ptr<Setting>()>;

  // Register `name` -> factory. Re-registering the same name overwrites,
  // which keeps a stale plugin from blocking a corrected type.
  void Register(std::string name, Factory factory);

  bool Has(const std::string& name) const;
  std::unique_ptr<Setting> Create(const std::string& name) const;

  const std::unordered_map<std::string, Factory>& entries() const {
    return entries_;
  }

 private:
  std::unordered_map<std::string, Factory> entries_;
};

// Registers float, int, bool and enum. Called once at startup.
void RegisterBuiltinTypes(Registry& registry);

}  // namespace ctl
}  // namespace reshade_dlssnr
