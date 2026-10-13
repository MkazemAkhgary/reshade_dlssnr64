#include "ctl/registry.h"

#include <memory>

#include "ctl/builtin_settings.h"

namespace reshade_dlssnr {
namespace ctl {

void Registry::Register(std::string name, Factory factory) {
  entries_[std::move(name)] = std::move(factory);
}

bool Registry::Has(const std::string& name) const {
  return entries_.find(name) != entries_.end();
}

std::unique_ptr<Setting> Registry::Create(const std::string& name) const {
  auto it = entries_.find(name);
  if (it == entries_.end()) return nullptr;
  return it->second();
}

}  // namespace ctl
}  // namespace reshade_dlssnr
