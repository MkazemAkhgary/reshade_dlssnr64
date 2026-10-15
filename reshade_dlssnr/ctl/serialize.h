// ctl persistence: versioned serialization of a ValueStore.
//
// Rules the format guarantees:
//   - only persist-flagged settings are written;
//   - a single-instance section is written once, outside any instance block;
//   - an instanced section is written under its instance key;
//   - every persistable setting is written once per instance;
//   - an unknown key or an unknown setting is skipped, not an error;
//   - the format carries a version, and an older reader ignores keys it does
//     not know rather than failing.
//
// Who knows what:
//   - Save can walk all live instances, or use an optional consumer key source
//     to restrict an instance block to its owner-defined key set.
//   - Load and Delete need instance keys for instanced sections. ctl does not
//     know which instances belong to a serialized block -- see InstanceKeySource.

#pragma once

#include <string>
#include <vector>

#include "ctl/values.h"

namespace reshade_dlssnr {
namespace ctl {

// Current on-disk format version. Bump when the layout changes in a way an
// older reader would misread; adding keys does not require a bump.
constexpr int kSerializeVersion = 1;

// Abstract sink so the framework does not depend on ReShade's config API.
// A caller supplies an implementation backed by whatever store it uses.
class KeyValueWriter {
 public:
  virtual ~KeyValueWriter() = default;
  virtual void WriteBool(const std::string& key, bool value) = 0;
  virtual void WriteInt(const std::string& key, int value) = 0;
  virtual void WriteFloat(const std::string& key, float value) = 0;
  virtual void WriteString(const std::string& key, const std::string& value) = 0;
  virtual void Erase(const std::string& key) = 0;
};

// Abstract source, deliberately limited to what a config file can do: fetch a
// value by name, and report it when it is absent. It cannot enumerate its
// keys, because the host behind it (ReShade's config API) cannot either --
// so key discovery is not part of this interface and never will be.
class KeyValueReader {
 public:
  virtual ~KeyValueReader() = default;
  // Each returns false when the key is absent, leaving `out` untouched.
  virtual bool ReadBool(const std::string& key, bool* out) = 0;
  virtual bool ReadInt(const std::string& key, int* out) = 0;
  virtual bool ReadFloat(const std::string& key, float* out) = 0;
  virtual bool ReadString(const std::string& key, std::string* out) = 0;
};

// ---------------------------------------------------------------------------
// Instance keys, supplied by the consumer
// ---------------------------------------------------------------------------
//
// NON-GOAL: ctl does not track instance count. How many passes exist, and
// which keys they have, is application state -- it outlives the store (it is
// what the UI edits, what the schema is built for, and what a saved file
// describes). So ctl does not try to own it, and does not guess it from the
// config.
//
// The consumer supplies the keys instead, as a range over the section it is
// loading. That keeps exactly one owner of instance identity: the code that
// already owns the live instance count. The alternative -- having Load
// discover keys from the config -- requires a reader that can enumerate its
// keys, which no real backend offers, and would make ctl the second authority
// on how many
// instances exist.
//
// A source is only consulted for instanced sections. Returning an empty range
// means "this section has no instances", which loads nothing -- the same
// outcome as a config that never mentioned it.
using InstanceKeySource = std::vector<InstanceKey> (*)(
    const void* context, const std::string& section);

// Serialize the store under `prefix`. Only persist-flagged settings are
// written. A single-instance section is written once with no key. By default,
// an instanced section writes every live instance; when `source` is supplied,
// it writes only the instance keys supplied for that section. With a source,
// live instances outside that declared key set are intentionally excluded.
void Save(ValueStore& store, KeyValueWriter& writer,
          const std::string& prefix = {},
          const InstanceKeySource& source = nullptr,
          const void* context = nullptr);

// Erase every persistable key in a serialized block. The schema enumerates
// the setting names; for instanced sections, `source` supplies the instance
// keys belonging to this block. Unlike Save, Delete does not inspect override
// state: it removes a persisted key even when that setting is not overridden
// in the current live store.
void Delete(ValueStore& store, KeyValueWriter& writer,
            const std::string& prefix = {},
            const InstanceKeySource& source = nullptr,
            const void* context = nullptr);

// Load into the store. Unknown keys and unknown settings are skipped. Falls
// back to the declared default for every setting the config does not mention,
// per instance.
//
// `source` supplies the instance keys of each instanced section; a null
// pointer means no keys are known, so instanced sections load nothing.
// Single-instance sections never consult it.
//
// The instances are created here if they do not exist, so a store that starts
// empty ends up with exactly the instances the caller declared -- which is
// the point: the caller's key range and the loaded values cannot disagree.
void Load(ValueStore& store, KeyValueReader& reader,
          const std::string& prefix = {},
          const InstanceKeySource& source = nullptr,
          const void* context = nullptr);

}  // namespace ctl
}  // namespace reshade_dlssnr
