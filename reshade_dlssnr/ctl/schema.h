// ctl declaration surface: sections, hooks and the schema.
//
// A schema is declared once, in code, as a single expression per setting.
// Declaration order is render order. Sections are either single-instance or
// instanced; both are declared identically -- one schema, instantiated per
// key.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ctl/types.h"

namespace reshade_dlssnr {
namespace ctl {

enum class SectionKind : uint8_t {
  Single,     // exactly one value set
  Instanced,  // one value set per opaque key
};

// ---------------------------------------------------------------------------
// Hook
// ---------------------------------------------------------------------------

// A user-supplied insertion point between settings.
//
// Identity is data, rendering is code. Where a hook sits -- its placement
// relative to `anchor`, its `name` -- is declared as plain values and is the
// whole of what the framework knows about it: Build() resolves the anchor and
// splices the row, and nothing downstream ever needs to look at the rendering
// to know where the hook belongs. Only "what to place" is code, supplied as a
// lambda at the declaration site.
//
// A hook is handed the store and the draw options, because what a hook renders
// is usually another part of the same UI -- a tab bar over sections, a
// diagnostics readout -- and that needs the same store and options the drawer
// used. Nothing has to be captured at declaration time to get at it.
//
// `key` is the instance key the hook is running for, passed through as the
// key itself -- never a hash or an index derived from it. A hook that draws
// another section's body for the same instance must hand that key straight
// back to DrawSection, and it can only do that if it was given the key. An
// integer surrogate would work for a numeric key and silently collapse two
// distinct string keys, so the key is passed whole.
//
// `context` is the optional declaration-time payload, for the rare hook that
// needs a constant it cannot re-derive. ValueStore, DrawOptions and
// InstanceKey are forward-declared here and defined in values.h and draw.h.
class ValueStore;
struct DrawOptions;
class InstanceKey;
using HookDraw = bool (*)(const void* context, ValueStore& store,
                          const DrawOptions& options, const std::string& section,
                          const InstanceKey& key, bool& changed_any);

struct Hook {
  std::string name;

  // The rendering. A captureless lambda converts to this implicitly, so the
  // declaration site can pass the code inline and the Hook stays trivially
  // copyable -- one schema, no per-hook allocation.
  HookDraw draw = nullptr;
  const void* context = nullptr;

  // Where the hook sits: immediately before `anchor`, immediately after
  // `anchor`, or at the end of the section when `anchor` is empty.
  enum class Placement : uint8_t { Before, After, Append };
  Placement placement = Placement::Append;
  std::string anchor;
  std::string category;
  bool has_category = false;

  // Override the category path inherited from preceding rows. An empty path
  // places the hook at the section's top level.
  Hook& Category(const char* text) {
    category = CStrToString(text);
    has_category = true;
    return *this;
  }
};

// ---------------------------------------------------------------------------
// Section
// ---------------------------------------------------------------------------

class Setting;

// A predicate deciding whether a row is interactive this frame. It is handed
// the instance key the row is being drawn for, plus the store, and answers
// with a plain bool. The framework attaches no meaning to *why* a row is off:
// "this slider follows an enable toggle" is the application's sentence, not
// the framework's.
//
// A null predicate means "always enabled", which is the overwhelmingly common
// case, so a setting that does not opt in pays nothing.
//
// The alias lives in types.h so a Setting can carry a gate as its own
// metadata. It is re-exported here for callers that only include schema.h.
using ctl::EnablePredicate;

// One enable rule. `setting` empty means it applies to the whole section;
// otherwise it applies to the named setting.
struct EnableRule {
  std::string setting;
  EnablePredicate enabled = nullptr;
  const void* context = nullptr;
};

class Section {
 public:
  Section(std::string name, SectionKind kind)
      : name_(std::move(name)), kind_(kind) {}

  const std::string& name() const { return name_; }
  SectionKind kind() const { return kind_; }
  bool instanced() const { return kind_ == SectionKind::Instanced; }

  Section& DefaultPersistenceBackend(PersistenceBackend backend) {
    default_backend_ = backend;
    return *this;
  }

  // Render the named bool setting as a checkbox in this section's header.
  // The setting remains the source of truth; its normal row is omitted.
  Section& HeaderToggle(const char* setting) {
    header_toggle_setting_ = CStrToString(setting);
    return *this;
  }
  const std::string& header_toggle_setting() const {
    return header_toggle_setting_;
  }

  // Declare a setting. Returns a reference so the whole declaration stays a
  // single expression, e.g.
  //   section.Add<FloatSetting>("intensity", "Intensity")
  //          .Tooltip("...").Range(0, 2);
  template <typename T, typename... Args>
  T& Add(Args&&... args) {
    auto setting = std::make_unique<T>(std::forward<Args>(args)...);
    setting->meta_mutable().backend = default_backend_;
    T& ref = *setting;
    settings_.push_back(std::move(setting));
    return ref;
  }

  // Gate a single setting's interactivity.
  Section& EnableWhen(const std::string& setting, EnablePredicate enabled,
                      const void* context = nullptr) {
    enable_rules_.push_back(EnableRule{setting, enabled, context});
    return *this;
  }

  // Gate every setting in the section. A per-setting rule, if any, is
  // consulted first, so a section-wide gate acts as the default.
  Section& EnableSectionWhen(EnablePredicate enabled,
                             const void* context = nullptr) {
    enable_rules_.push_back(EnableRule{{}, enabled, context});
    return *this;
  }

  // Insert a hook relative to a named setting, or append it.
  //
  // `draw` is taken as a template parameter so a lambda can be passed straight
  // at the declaration site. A captureless lambda converts to HookDraw with no
  // allocation and no `std::function`; a lambda that captures is rejected at
  // compile time, which is the point -- a hook's identity is data, so the code
  // it runs must not smuggle in per-instance state.
  template <typename Draw>
  Hook& AddHookBefore(const std::string& anchor, std::string name,
                      Draw draw, const void* context = nullptr) {
    return AddHook(Hook::Placement::Before, anchor, std::move(name), draw, context);
  }
  template <typename Draw>
  Hook& AddHookAfter(const std::string& anchor, std::string name,
                     Draw draw, const void* context = nullptr) {
    return AddHook(Hook::Placement::After, anchor, std::move(name), draw, context);
  }
  template <typename Draw>
  Hook& AddHookAppend(std::string name, Draw draw,
                      const void* context = nullptr) {
    return AddHook(Hook::Placement::Append, {}, std::move(name), draw, context);
  }

  const std::vector<std::unique_ptr<Setting>>& settings() const { return settings_; }
  const std::vector<Hook>& hooks() const { return hooks_; }
  const std::vector<EnableRule>& enable_rules() const { return enable_rules_; }

  // Validated after Schema::Build(). Index of this section within the schema.
  uint32_t index() const { return index_; }

 private:
  friend class Schema;

  template <typename Draw>
  Hook& AddHook(Hook::Placement placement, const std::string& anchor,
                std::string name, Draw draw, const void* context) {
    Hook hook;
    hook.name = std::move(name);
    hook.draw = draw;  // captureless lambda -> HookDraw
    hook.context = context;
    hook.placement = placement;
    hook.anchor = anchor;
    hooks_.push_back(std::move(hook));
    return hooks_.back();
  }

  std::string name_;
  SectionKind kind_;
  PersistenceBackend default_backend_ = PersistenceBackend::Json;
  uint32_t index_ = 0;
  std::vector<std::unique_ptr<Setting>> settings_;
  std::string header_toggle_setting_;
  std::vector<Hook> hooks_;
  std::vector<EnableRule> enable_rules_;
};

// ---------------------------------------------------------------------------
// Schema
// ---------------------------------------------------------------------------

// The frozen, validated description of every section. Built once at startup.
class Schema {
 public:
  Schema& DefaultPersistenceBackend(PersistenceBackend backend) {
    default_backend_ = backend;
    return *this;
  }

  Section& AddSection(std::string name, SectionKind kind) {
    sections_.push_back(std::make_unique<Section>(std::move(name), kind));
    sections_.back()->DefaultPersistenceBackend(default_backend_);
    return *sections_.back();
  }

  // Freeze the schema: assign section indices, index settings by name, and
  // resolve hook anchors. Returns false and fills `error` when a section
  // name or setting name collides, or a hook anchor does not resolve.
  bool Build(std::string* error = nullptr);

  bool built() const { return built_; }

  const std::vector<std::unique_ptr<Section>>& sections() const { return sections_; }
  uint32_t section_count() const { return static_cast<uint32_t>(sections_.size()); }

  // Look up a section by name. Sections are declared, then frozen by Build();
  // the schema is immutable afterwards, so there is no mutable accessor.
  const Section* FindSection(const std::string& name) const;

  // Index of a rendered row within a section, in declaration order with
  // hooks interleaved. Used by the drawer to walk one section.
  struct Row {
    enum class Kind : uint8_t { Setting, Hook };
    Kind kind = Kind::Setting;
    uint32_t setting_index = 0;  // valid when kind == Setting
    uint32_t hook_index = 0;     // valid when kind == Hook
  };
  const std::vector<Row>& rows(const Section& section) const {
    return rows_[section.index()];
  }

 private:
  std::vector<std::unique_ptr<Section>> sections_;
  std::vector<std::vector<Row>> rows_;
  PersistenceBackend default_backend_ = PersistenceBackend::Json;
  bool built_ = false;
};

}  // namespace ctl
}  // namespace reshade_dlssnr
