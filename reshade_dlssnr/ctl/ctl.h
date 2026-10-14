// ctl: declarative control settings framework.
//
// The core is domain-agnostic. It knows about values, sections, instances,
// parent links and opaque keys. It knows nothing about passes, layers,
// cameras or any other application concept -- those are expressed by the
// caller as section names and instance keys.
//
// Layout:
//   ctl/types.h    SettingBase, metadata, type-erased value storage
//   ctl/schema.h   Section, Schema, Hook
//   ctl/values.h   Instance, InstanceSet, ValueStore
//   ctl/registry.h value-type registry (add a type without touching the core)
//   ctl/serialize.h versioned save/load/delete persistence
//   ctl/draw.h     ImGui rendering of a schema
//
// Typical use:
//   ctl::Registry registry;
//   RegisterBuiltinTypes(registry);       // float/int/bool/enum/string
//   RegisterColorSetting(registry);       // one extra line per new type
//   ctl::Schema schema;
//   auto& section = schema.AddSection("Model", ctl::SectionKind::Instanced);
//   section.Add<ctl::FloatSetting>("intensity", ...).Range(0.0f, 2.0f);
//   schema.Build();                       // freeze, validate, index
//
// See ctl/README.md for the worked per-pass example.

#pragma once

#include "ctl/types.h"
#include "ctl/schema.h"
#include "ctl/values.h"
#include "ctl/registry.h"
#include "ctl/serialize.h"
#include "ctl/draw.h"
