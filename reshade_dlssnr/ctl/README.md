# ctl - the control settings framework

A declarative settings system. Settings are *declared* in code, one expression
each; the framework owns storage, UI rendering, and persistence.
Nothing in this directory knows what a "pass", "layer", "camera", or "preset"
is - those are the application's words, expressed through sections, instance
keys and parent links.

```
ctl/
  ctl.h                 umbrella header
  types.h/.cpp          Value, SettingMeta, Setting, SettingContext
  schema.h/.cpp         Section, Hook, Schema         (declaration)
  registry.h/.cpp       type registry                 (extensibility)
  values.h/.cpp         InstanceKey, Instance, InstanceSet, ValueStore (storage)
  serialize.h/.cpp      KeyValueWriter/Reader, Save/Load (persistence)
  draw.h/.cpp           DrawSection                  (rendering)
  builtin_settings.h/.cpp  FloatSetting, IntSetting, BoolSetting, EnumSetting, StringSetting, IntListSetting
  trace.h/.cpp             schema-driven setting trace recorder
  ctl_acceptance.cpp    acceptance harness
  build_ctl_acceptance.sh / build_ctl_check.sh  dev scripts
  test_row_stubs.cpp / test_imgui_stubs.cpp      harness-only link stubs
```

## Quick start

```cpp
using namespace reshade_dlssnr::ctl;

Schema schema;

// Single-instance section: exactly one value set.
Section& prefs = schema.AddSection("Preferences", SectionKind::Single);
prefs.Add<BoolSetting>("tooltips", "Show tooltips", true);
prefs.Add<EnumSetting>("preset", "Preset", 1)
     .Items(kPresetNames, kPresetTooltips, 3);

// Instanced section: one value set per key. Declared identically.
Section& model = schema.AddSection("Model", SectionKind::Instanced);
model.Add<FloatSetting>("intensity", "Intensity", 1.0f)
     .Range(0.0f, 2.0f)
     .Tooltip("How strongly the model edits the image.");
model.Add<EnumSetting>("upsample", "Upsample mode", 1)
     .Items(kUpsampleNames, nullptr, 2);
model.Add<FloatSetting>("skin_structure", "Skin Structure", 1.0f)
     .ClampRange(0.0f, 1.0f)
     .InlineToggle("auto_mask");
model.Add<BoolSetting>("auto_mask", "Auto Mask", true);

model.AddHookBefore("upsample", "PresetRow",
                    [](const void*, ValueStore&, const DrawOptions&,
                       const std::string&, const InstanceKey&, bool&) {
                      return DrawPresetRow();
                    });

std::string error;
if (!schema.Build(&error)) { /* name collision or bad hook anchor */ }

ValueStore store(schema);      // live state
```

Then, each frame, enumerate the sections the page shows:

```cpp
bool changed = false;
DrawSection(store, "Preferences", InstanceKey::FromInt(0), changed);
DrawSection(store, "Model", InstanceKey::FromInt(active_pass), changed);
if (changed) SaveSettings();
```

There is deliberately no "draw every section" entry point. Which sections
appear, and in what order, is a property of the page drawing them, and a page
that folds some sections into a tab bar (HDR's exposure layers) must not also
let them fall out of a blanket iteration. The caller enumerates.

## The pieces

### Settings are declarations

`Section::Add<T>` returns `T&`, so a setting is configured with fluent setters
and the whole declaration stays one expression. Declaration order is render
order - `Schema::Build` records it and the drawer replays it.

`SettingMeta` carries `name`, `label`, `tooltip`, `category`, `persist`, and
`persist_in_profiles` (both persistence flags default to true). Mark a setting
`GlobalOnly()` when it belongs to global application state rather than a saved
profile. The framework implements this generically: it does not infer what a
setting means or which feature owns it.

### Hooks

A hook is an insertion point for UI the settings model cannot express - a tab
bar, a live readout, a histogram. **Identity is data; only the rendering is
code.** Where a hook sits (its anchor, placement and name) is declared as plain
values, which is the whole of what the framework knows about it. The rendering
is a lambda at that same declaration site:

```cpp
hdr.AddHookBefore("format", "HdrStatus",
                  [](const void*, ValueStore&, const DrawOptions&,
                     const std::string&, const InstanceKey&, bool&) {
                    return DrawHdrStatus();
                  });
hdr.AddHookAfter("highlight_headroom", "ExposureTabs", &DrawExposureTabs);
hdr.AddHookAppend("Footer",
                  [](const void*, ValueStore&, const DrawOptions&,
                     const std::string&, const InstanceKey&, bool& changed) {
                    return DrawFooter(changed);
                  });
```

The lambda must be captureless, so it converts to a plain function pointer:
the `Hook` stays trivially copyable and the schema needs no per-hook
allocation or `std::function`. A lambda that captures is a compile error, and
that is the point - a hook's position is data, so the code it runs must not
smuggle in per-instance state. Anything a hook needs at run time comes from the
context it was handed: the store and the instance key.

The callback signature is
`bool(const void* context, ValueStore&, const DrawOptions&, const std::string& section, const InstanceKey& key, bool& changed)`.
A hook is handed the store, the options, the section it sits in, and the
instance key it is running for - the key whole, never a hash or an index
derived from it. A hook that draws another section's body for the same
instance (HDR's layer tabs) gives that key straight back to `DrawSection`, and
it can only do that if it was given the key. An integer surrogate would work
for a numeric key and silently collapse two distinct string keys. A hook draws
from the store and reports whether it changed anything, exactly like a setting
row.

`Schema::Build` resolves each anchor and splices the hook into the section's row
list, so a hook participates in ordering exactly like a setting. Within a
category sub-group a hook obeys the group's collapse state, so a tab bar or
readout cannot leak out of a collapsed sub-section.

### Types are one class plus one registration line

To add `ColorSetting`:

1. Write the class - derive from `Setting`, implement `DefaultValue`, `Clamp`,
   `Assign`, `ToString`, `FromString`, `Draw`.
2. Register it - `registry.Register("color", [] { return std::make_unique<ColorSetting>("", ""); });`

Nothing else changes: not the other types, not the manager, not any call site.
The acceptance harness proves this with its `ColorSetting` (test 27).

### Keys are opaque

```cpp
InstanceKey::FromInt(pass_index)
InstanceKey::FromString("camera.main")
```

The framework assigns them no meaning. They are not required to be indices,
monotonic, or contiguous. `InstanceKey::ToText()` writes an int key as `i<digits>`
and a string key as `s<text>` so the two flavours can never collide in config.
Instances are created lazily on first access, so declaring a key that nothing
ever touches costs nothing.

### Persistence

```cpp
using InstanceKeySource =
    std::vector<InstanceKey> (*)(const void* context, const std::string& section);

void Save(ValueStore&, KeyValueWriter&, const std::string& prefix = {},
          const InstanceKeySource& source = nullptr, const void* context = nullptr,
          PersistenceTarget target = PersistenceTarget::Global,
          PersistenceBackend backend = PersistenceBackend::Json);
void Delete(ValueStore&, KeyValueWriter&,
            const InstanceKeySource& source = nullptr,
            const void* context = nullptr, const std::string& prefix = {},
            PersistenceTarget target = PersistenceTarget::Global,
            PersistenceBackend backend = PersistenceBackend::Json);
void Load(ValueStore&, KeyValueReader&, const InstanceKeySource& source = nullptr,
          const void* context = nullptr, const std::string& prefix = {},
          PersistenceTarget target = PersistenceTarget::Global,
          PersistenceBackend backend = PersistenceBackend::Json);
```

Both key-value interfaces are abstract, so the framework never depends on
ReShade's config API. Keys are laid out as:

```
<prefix>.Version
<prefix>.S.<Section>.<setting>              single-instance section
<prefix>.S.<Section>.<keytext>.<setting>    instanced section
```

When `prefix` is empty, the leading dot is omitted. `Delete` uses the same
schema traversal and supplied instance keys to erase every persistable setting
key in the block, regardless of whether the setting is overridden in the live
store. It also erases the version key. Missing keys are a silent no-op.

Guarantees: only `persist` settings whose backend matches are written,
and profile operations also require `persist_in_profiles`; unknown keys
and settings are skipped rather than failing; an instanced setting left
unset stays at its declared default.
`IntListSetting` stores an ordered typed integer list with optional item bounds,
range clamping, and uniqueness enforcement, so a domain can persist ordering as
ordinary schema data rather than hand-encoding a comma-separated config field.

**Who knows the instance keys.** `Load` and `Delete` need them, and `Save` can
take them when it should write only a selected instance set. The reader and
writer cannot enumerate keys because a backend may not expose that capability.
The consumer supplies the current keys. `source` is called once per instanced
section and returns the keys to load, which the loader creates if they are not
present yet. If ordering or selection is persisted as an ordinary setting, the
application may read that setting and provide its values through `source`; the
framework does not special-case the setting or interpret its contents.

**NON-GOAL: ctl does not infer instance keys from persisted data.** The backend
may not enumerate stored keys, and interpreting a domain's ordering, selection,
or count would introduce domain policy into the framework. The caller remains
responsible for supplying the intended instance-key set.

**`ReshadeConfigWriter` / `ReshadeConfigReader`** (`ctl/usage/config.{h,cpp}`)
are the production implementations over `reshade::get_config_value` /
`set_config_value`. The writer erases a key by passing a null value to
ReShade's config API. Neither adapter can enumerate keys, so the consumer must
still supply instance keys to `Load` and `Delete`.


## Setting trace

`TraceRecorder` walks the built schema and current `ValueStore` instances to
record every declared setting. The caller supplies only a sink callback; ctl
formats no feature names and has no dependency on the application layer. The
first record emits all current values, later records emit only changed, added,
or removed entries. New settings become trace-visible automatically when they
are registered in the schema.

## Worked example: per-pass settings

The application has N effect passes, each owning its own values. ctl sees
one instanced section and integer keys, and nothing else: a pass's values
are its own and survive any reorder of the tabs.

```cpp
// --- Declaration, once at startup -----------------------------------------

Schema schema;

Section& pass = schema.AddSection("Pass", SectionKind::Instanced);

pass.Add<BoolSetting>("enabled", "Enabled", true);

pass.Add<FloatSetting>("intensity", "Intensity", 1.0f)
    .Range(0.0f, 2.0f)
    .Tooltip("Blend weight for this pass.");

pass.Add<FloatSetting>("working_scale", "Working scale", 1.0f)
    .Range(0.1f, 1.0f);

pass.Add<EnumSetting>("upsample", "Upsample kernel", 0)
    .Items(kUpsampleNames, kUpsampleTooltips, kUpsampleCount);

std::string error;
schema.Build(&error);
// (error is non-empty on a duplicate name or an anchor that does not resolve)

ValueStore store(schema);
```

```cpp
// --- Per frame ------------------------------------------------------------

void DrawPassSection(App& app, int pass_index) {
  const InstanceKey key = InstanceKey::FromInt(pass_index);

  bool changed = false;
  DrawSection(app.store, "Pass", key, changed);
  if (changed) app.MarkDirty();
}
```

```cpp
// --- Reading, in the pipeline --------------------------------------------

const float intensity =
    app.store.Read("Pass", InstanceKey::FromInt(pass_index), "intensity").as_float;
// Each pass reads its own value. Reordering the tabs permutes which id sits
// at which position; it does not move a value from one pass to another.

// Programmatic write, e.g. from a sampler that auto-tunes:
app.store.Write("Pass", InstanceKey::FromInt(pass_index), "intensity",
                Value::Float(measured), /*from_user=*/false);
// The value is now that pass's own; no other pass sees it.
```

## Extending

| To add... | Do this |
| --- | --- |
| a new setting type | one class + one `registry.Register(...)` line |
| a new metadata field | add it to `SettingMeta` with a default |
| a hook | `AddHookBefore` / `AddHookAfter` / `AddHookAppend` |
| a new scope (layer, camera, preset, material) | an instanced section with a key of that flavour - no framework change |
| a new config backend | implement `KeyValueReader` / `KeyValueWriter` |

## Tests

```bash
bash ctl/build_ctl_acceptance.sh   # build + run the ctl acceptance harness
bash ctl/build_ctl_check.sh ctl/*.cpp   # type-check (PROJ_ROOT must be set)
```

The acceptance harness links the real `ctl` sources and the real row code,
substituting only inert ImGui entry points, so every criterion is exercised
against the shipped implementation rather than a mock.
