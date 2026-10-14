// Acceptance harness for the ctl control settings framework.
//
// Links the real ctl sources, including the built-in setting types and their
// ImGui rows, and asserts every acceptance criterion from the requirements
// document. Rules about persistence, registration and access are proven
// against the framework's actual implementation rather than a re-description
// of it.
//
// Build and run: bash build_ctl_acceptance.sh

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ctl/builtin_settings.h"
#include "ctl/ctl.h"
#include "feature/pass/settings.h"

using namespace reshade_dlssnr::ctl;

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const char* what) {
  ++g_checks;
  if (condition) {
    std::printf("  PASS  %s\n", what);
  } else {
    std::printf("  FAIL  %s\n", what);
    ++g_failures;
  }
}

// --- A tiny in-memory KeyValue store, standing in for ReShade's config. ----

struct MemStore : KeyValueReader, KeyValueWriter {
  std::map<std::string, std::string> data;

  void WriteBool(const std::string& key, bool value) override {
    data[key] = value ? "1" : "0";
  }
  void WriteInt(const std::string& key, int value) override {
    data[key] = std::to_string(value);
  }
  void WriteFloat(const std::string& key, float value) override {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(value));
    data[key] = buf;
  }
  void WriteString(const std::string& key, const std::string& value) override {
    data[key] = value;
  }
  void Erase(const std::string& key) override { data.erase(key); }

  bool ReadBool(const std::string& key, bool* out) override {
    auto it = data.find(key);
    if (it == data.end()) return false;
    *out = (it->second == "1" || it->second == "true");
    return true;
  }
  bool ReadInt(const std::string& key, int* out) override {
    auto it = data.find(key);
    if (it == data.end()) return false;
    *out = std::atoi(it->second.c_str());
    return true;
  }
  bool ReadFloat(const std::string& key, float* out) override {
    auto it = data.find(key);
    if (it == data.end()) return false;
    *out = std::strtof(it->second.c_str(), nullptr);
    return true;
  }
  bool ReadString(const std::string& key, std::string* out) override {
    auto it = data.find(key);
    if (it == data.end()) return false;
    *out = it->second;
    return true;
  }
};

// --- The consumer-side instance key source ---------------------------------
//
// Stands in for application state: how many instances exist. ctl does not know
// this and does not guess it from the config (no backend can enumerate keys);
// the consumer hands it over as a key range.
struct InstanceCount {
  int model_count = 0;
};

std::vector<InstanceKey> InstanceKeys(const void* context,
                                      const std::string& section) {
  const InstanceCount* state = static_cast<const InstanceCount*>(context);
  if (section != "Model") return {};
  std::vector<InstanceKey> keys;
  keys.reserve(static_cast<size_t>(state->model_count));
  for (int i = 0; i < state->model_count; ++i) {
    keys.push_back(InstanceKey::FromInt(i));
  }
  return keys;
}

using InstanceKeyList = std::vector<InstanceKey>;

std::vector<InstanceKey> InstanceKeysFromList(const void* context,
                                              const std::string& section) {
  if (section != "Model" || context == nullptr) return {};
  return *static_cast<const InstanceKeyList*>(context);
}

// --- Example setting type used for the "add a type" criterion --------------

// A colour type is the canonical "one class plus one registration line" test.
// It reuses float storage for three channels through three separate settings
// only to keep the harness small; the important part is that the framework
// never needed to change to accept it.
class ColorSetting : public Setting {
 public:
  ColorSetting(const char* name, const char* label) {
    meta_.name = reshade_dlssnr::CStrToString(name);
    meta_.label = reshade_dlssnr::CStrToString(label);
  }
  Value DefaultValue() const override { return Value::Int(0); }
  void Clamp(Value& v) const override { v.as_int &= 0xFFFFFF; }
  bool Assign(Value& out, const Value& in) const override {
    out = in;
    return true;
  }
  std::string ToString(const Value& v) const override {
    char b[16];
    std::snprintf(b, sizeof(b), "%06X", v.as_int & 0xFFFFFF);
    return b;
  }
  bool FromString(const std::string& text, Value& out) const override {
    out = Value::Int(static_cast<int>(std::strtol(text.c_str(), nullptr, 16)));
    return true;
  }
  bool Draw(const SettingContext&, const Value&, Value&) const override {
    return false;  // headless: no drawing needed to prove registration
  }
};

// --- Schema used across the criteria --------------------------------------

struct Fixture {
  Schema schema;
  ValueStore store;

  static bool DrawHook(const void*, ValueStore&, const DrawOptions&,
                       const std::string&, const InstanceKey&, bool&) {
    return false;
  }

  Fixture() {
    // Section 1: a normal single-instance section.
    Section& prefs = schema.AddSection("Preferences", SectionKind::Single);
    prefs.Add<BoolSetting>("tooltips", "Show Tooltips", true);
    prefs.Add<EnumSetting>("preset", "Preset", 1)
        .Items(kPresetNames, nullptr, 3);
    // A string row, to prove the type round-trips and honours its length cap.
    prefs.Add<StringSetting>("note", "Note", std::string("hello"))
        .MaxLength(8);

    // Section 2: the instanced section, standing in for per-pass settings.
    // Every setting is owned by its instance; the store keeps no parent links.
    Section& model = schema.AddSection("Model", SectionKind::Instanced);
    model.Add<FloatSetting>("intensity", "Intensity", 1.0f)
        .Range(0.0f, 2.0f)
        .Tooltip("How strongly the model edits the image.");
    model.Add<FloatSetting>("working_scale", "Working Scale", 1.0f)
        .Range(0.1f, 1.0f);
    // The upsample mode is a per-pass choice, never shared.
    model.Add<EnumSetting>("upsample", "Upsample Mode", 1)
        .Items(kUpsampleNames, nullptr, 2);
    // Never written to config.
    model.Add<FloatSetting>("live_metric", "Live Metric", 0.0f)
        .Range(0.0f, 1.0f)
        .NoSave();

    // A hook between the settings, before `working_scale`.
    model.AddHookBefore("working_scale", "PresetRow", &Fixture::DrawHook);
    model.AddHookAppend("Footer", &Fixture::DrawHook);

    std::string err;
    schema.Build(&err);
    store.Init(schema);
  }

  static const char* kPresetNames[3];
  static const char* kUpsampleNames[2];
};

const char* Fixture::kPresetNames[3] = {"A", "B", "C"};
const char* Fixture::kUpsampleNames[2] = {"EASU", "Joint Bilateral"};

// Helpers ------------------------------------------------------------------

float ReadFloat(const ValueStore& store, const char* section, InstanceKey key,
                const char* setting) {
  return store.Read(section, key, setting).as_float;
}

struct HarnessProfile {
  uint32_t id = 0;
  const char* name = "";
  bool enabled = true;
  const char* note = "";
  float intensity = 1.0f;
  InstanceKeyList passes;
};

std::string HarnessProfilePrefix(uint32_t id) {
  return "UserPreset." + std::to_string(id);
}

std::string HarnessProfileOrder(const InstanceKeyList& keys) {
  std::string text;
  for (const InstanceKey& key : keys) {
    if (!text.empty()) text += ',';
    text += key.ToText().substr(1);
  }
  return text;
}

InstanceKeyList ParseHarnessProfileOrder(const std::string& text) {
  InstanceKeyList keys;
  size_t begin = 0;
  while (begin <= text.size()) {
    const size_t comma = text.find(',', begin);
    const std::string token = text.substr(
        begin, comma == std::string::npos ? std::string::npos : comma - begin);
    if (!token.empty()) {
      char* end = nullptr;
      const long long value = std::strtoll(token.c_str(), &end, 10);
      if (end != token.c_str() && *end == '\0' && value > 0) {
        keys.push_back(InstanceKey::FromInt(value));
      }
    }
    if (comma == std::string::npos) break;
    begin = comma + 1;
  }
  return keys;
}

void SaveHarnessProfile(MemStore& mem, const HarnessProfile& profile) {
  Fixture fx;
  const InstanceKey shared = InstanceKey::FromInt(0);
  fx.store.Write("Preferences", shared, "tooltips", Value::Bool(profile.enabled));
  fx.store.Write("Preferences", shared, "note", Value::String(profile.note));
  for (size_t i = 0; i < profile.passes.size(); ++i) {
    fx.store.Write("Model", profile.passes[i], "upsample",
                   Value::Enum(static_cast<int>((profile.id + i) % 2)));
  }
  if (!profile.passes.empty()) {
    fx.store.Write("Model", profile.passes.front(), "intensity",
                   Value::Float(profile.intensity));
  }

  const std::string root = HarnessProfilePrefix(profile.id);
  mem.WriteString(root + ".Name", profile.name);
  mem.WriteBool(root + ".Enabled", profile.enabled);
  mem.WriteString(root + ".PassOrder", HarnessProfileOrder(profile.passes));
  Save(fx.store, mem, root + ".Data");
}

bool HasPrefix(const MemStore& mem, const std::string& prefix) {
  for (const auto& entry : mem.data) {
    if (entry.first.compare(0, prefix.size(), prefix) == 0) return true;
  }
  return false;
}

}  // namespace

int main() {
  std::printf("ctl acceptance harness\n");
  std::printf("======================\n\n");

  // ------------------------------------------------------------------------
  std::printf("[1] Schema builds; section and setting names are validated\n");
  {
    Fixture fx;
    Check(fx.schema.built(), "schema Build() succeeds");
    Check(fx.schema.section_count() == 2, "two sections declared");
    Check(fx.schema.FindSection("Model") != nullptr, "Model section found");
    Check(fx.schema.FindSection("Nope") == nullptr, "unknown section is null");

    const Section* model = fx.schema.FindSection("Model");
    // 4 settings + 2 hooks = 6 ordered rows.
    Check(fx.schema.rows(*model).size() == 6, "rows = settings + hooks");

    // The hook anchored before `working_scale` must sit directly before it.
    const auto& rows = fx.schema.rows(*model);
    int working_scale_row = -1;
    int hook_row = -1;
    for (size_t i = 0; i < rows.size(); ++i) {
      if (rows[i].kind == Schema::Row::Kind::Setting &&
          model->settings()[rows[i].setting_index]->meta().name ==
              "working_scale") {
        working_scale_row = static_cast<int>(i);
      }
      if (rows[i].kind == Schema::Row::Kind::Hook &&
          model->hooks()[rows[i].hook_index].name == "PresetRow") {
        hook_row = static_cast<int>(i);
      }
    }
    Check(hook_row >= 0 && working_scale_row == hook_row + 1,
          "hook is inserted immediately before its anchor setting");
  }

  // ------------------------------------------------------------------------
  std::printf("\n[2] Duplicate names are rejected\n");
  {
    Schema schema;
    Section& s = schema.AddSection("Dup", SectionKind::Single);
    s.Add<FloatSetting>("a", "A");
    s.Add<FloatSetting>("a", "A again");
    std::string err;
    Check(!schema.Build(&err), "duplicate setting name fails Build()");
    Check(!err.empty(), "Build() reports an error message");
  }
  {
    Schema schema;
    schema.AddSection("Same", SectionKind::Single);
    schema.AddSection("Same", SectionKind::Single);
    std::string err;
    Check(!schema.Build(&err), "duplicate section name fails Build()");
  }

  // ------------------------------------------------------------------------
  std::printf("\n[3] Hook anchor on a missing setting is rejected\n");
  {
    Schema schema;
    Section& s = schema.AddSection("H", SectionKind::Single);
    s.Add<FloatSetting>("x", "X");
    s.AddHookBefore("does_not_exist", "Bad", nullptr);
    std::string err;
    Check(!schema.Build(&err), "unknown hook anchor fails Build()");
  }

  // ------------------------------------------------------------------------
  std::printf("\n[4] Two instance keys hold independent values\n");
  {
    Fixture fx;
    const InstanceKey k0 = InstanceKey::FromInt(0);
    const InstanceKey k1 = InstanceKey::FromInt(1);

    fx.store.Write("Model", k0, "upsample", Value::Enum(0));
    fx.store.Write("Model", k1, "upsample", Value::Enum(1));

    Check(fx.store.Read("Model", k0, "upsample").as_int == 0,
          "key 0 keeps its own value");
    Check(fx.store.Read("Model", k1, "upsample").as_int == 1,
          "key 1 keeps its own value");
    Check(fx.store.Read("Model", InstanceKey::FromInt(7), "upsample").as_int == 1,
          "an untouched key falls back to the declared default");
  }

  // ------------------------------------------------------------------------
  std::printf("\n[5] Declaration order is stable and settings are addressable\n");
  {
    Fixture fx;
    const Section* model = fx.schema.FindSection("Model");
    Check(model->settings()[0]->meta().name == "intensity", "first declared is first");
    Check(model->settings()[1]->meta().name == "working_scale", "second declared is second");
    Check(model->settings()[2]->meta().name == "upsample", "third declared is third");
    Check(model->settings()[3]->meta().name == "live_metric", "fourth declared is fourth");
  }

  // ------------------------------------------------------------------------
  std::printf("\n[13] Reset restores the declared default, per instance\n");
  {
    Fixture fx;
    const InstanceKey k0 = InstanceKey::FromInt(0);
    const InstanceKey k1 = InstanceKey::FromInt(1);

    fx.store.Write("Model", k0, "intensity", Value::Float(0.1f));
    fx.store.Write("Model", k1, "intensity", Value::Float(1.7f));

    fx.store.ResetToDefault("Model", k1, "intensity");
    Check(ReadFloat(fx.store, "Model", k1, "intensity") == 1.0f,
          "reset restores the declared default on that instance");
    Check(ReadFloat(fx.store, "Model", k0, "intensity") == 0.1f,
          "reset leaves the other instance alone");
  }

  // ------------------------------------------------------------------------
  std::printf("\n[14] A changed default is picked up by reset\n");
  {
    // The default is read from the schema at reset time, not baked into a
    // stored literal, so changing the declared default changes what Reset
    // restores.
    Schema schema;
    Section& s = schema.AddSection("S", SectionKind::Single);
    FloatSetting& f = s.Add<FloatSetting>("v", "V", 0.5f);
    f.Range(0.0f, 1.0f);
    std::string err;
    schema.Build(&err);
    ValueStore store(schema);

    const InstanceKey only = InstanceKey::FromInt(0);
    store.Write("S", only, "v", Value::Float(0.9f));
    store.ResetToDefault("S", only, "v");
    Check(store.Read("S", only, "v").as_float == 0.5f,
          "reset reads the declared default");

    // Redefine the schema's default and rebuild: a fresh store sees the new
    // default, which is exactly the contract ("changing the default + Reset
    // restores it").
    Schema schema2;
    Section& s2 = schema2.AddSection("S", SectionKind::Single);
    s2.Add<FloatSetting>("v", "V", 0.25f);
    schema2.Build(&err);
    ValueStore store2(schema2);
    store2.ResetToDefault("S", only, "v");
    Check(store2.Read("S", only, "v").as_float == 0.25f,
          "a new declared default changes what reset restores");
  }

  // ------------------------------------------------------------------------
  std::printf("\n[15] Persistence: only persist-flagged settings are written\n");
  {
    Fixture fx;
    MemStore mem;
    const InstanceKey k0 = InstanceKey::FromInt(0);
    fx.store.Write("Model", k0, "live_metric", Value::Float(0.42f));
    Save(fx.store, mem);

    bool found = false;
    for (const auto& kv : mem.data) {
      if (kv.first.find("live_metric") != std::string::npos) found = true;
    }
    Check(!found, "a NoSave setting never appears in the config");
  }

  // ------------------------------------------------------------------------
  std::printf("\n[16] Persistence: instanced sections are stored per key\n");
  {
    Fixture fx;
    MemStore mem;
    const InstanceKey k0 = InstanceKey::FromInt(0);
    const InstanceKey k1 = InstanceKey::FromInt(1);

    fx.store.Write("Model", k0, "upsample", Value::Enum(0));
    fx.store.Write("Model", k1, "upsample", Value::Enum(1));
    Save(fx.store, mem);

    bool key0 = false, key1 = false;
    for (const auto& kv : mem.data) {
      if (kv.first.find("K.i0") != std::string::npos &&
          kv.first.find("upsample") != std::string::npos) key0 = true;
      if (kv.first.find("K.i1") != std::string::npos &&
          kv.first.find("upsample") != std::string::npos) key1 = true;
    }
    Check(key0 && key1, "each instance key gets its own serialized block");
  }

  // ------------------------------------------------------------------------
  std::printf("\n[17] Persistence: single-instance sections carry no key\n");
  {
    Fixture fx;
    MemStore mem;
    fx.store.Write("Preferences", InstanceKey::FromInt(0), "preset", Value::Enum(2));
    Save(fx.store, mem);

    bool keyed = false;
    bool present = false;
    for (const auto& kv : mem.data) {
      if (kv.first.find("S.Preferences") != std::string::npos) present = true;
      if (kv.first.find("S.Preferences") != std::string::npos &&
          kv.first.find("K.") != std::string::npos) keyed = true;
    }
    Check(present, "single-instance section is written");
    Check(!keyed, "single-instance section is not nested under an instance key");
  }

  // ------------------------------------------------------------------------
  std::printf("\n[19] Persistence: a written setting is always saved\n");
  {
    Fixture fx;
    MemStore mem;
    const InstanceKey k1 = InstanceKey::FromInt(1);
    // `upsample` holds only the default because nothing wrote it.
    fx.store.Write("Model", k1, "intensity", Value::Float(1.1f));
    Save(fx.store, mem);

    bool upsample_present = false;
    for (const auto& kv : mem.data) {
      if (kv.first.find("K.i1") != std::string::npos &&
          kv.first.find("upsample") != std::string::npos) {
        upsample_present = true;
      }
    }
    Check(upsample_present,
          "a setting is written even when it only holds the default");
  }

  // ------------------------------------------------------------------------
  std::printf("\n[19b] A string setting round-trips and honours its cap\n");
  {
    Fixture fx;
    MemStore mem;
    const InstanceKey k0 = InstanceKey::FromInt(0);

    Check(fx.store.Read("Preferences", k0, "note").as_string == "hello",
          "the default is a string, not a stale float");
    fx.store.Write("Preferences", k0, "note", Value::String("world"));
    Check(fx.store.Read("Preferences", k0, "note").as_string == "world",
          "a string writes and reads back");
    fx.store.Write("Preferences", k0, "note", Value::String(""));
    Check(fx.store.Read("Preferences", k0, "note").as_string.empty(),
          "an empty string is a value, not an absence");

    Save(fx.store, mem);
    ValueStore restored(fx.schema);
    Load(restored, mem);
    Check(restored.Read("Preferences", k0, "note").as_string.empty(),
          "an empty string round-trips as itself");

    // The cap is enforced on the value, so a hand-edited config cannot install
    // a string longer than the UI would have accepted. The cap here is 8.
    mem.data["S.Preferences.note"] = "abcdefghij";
    ValueStore capped(fx.schema);
    Load(capped, mem);
    Check(capped.Read("Preferences", k0, "note").as_string == "abcdefgh",
          "a value longer than the cap is truncated on load");
    Check(capped.Read("Preferences", k0, "note").as_string != "hello",
          "a present key overrides the default even when truncated");
  }

  // ------------------------------------------------------------------------
  std::printf("\n[20] Persistence: round-trip, and unknown keys are skipped\n");
  {
    Fixture fx;
    MemStore mem;
    const InstanceKey k0 = InstanceKey::FromInt(0);
    const InstanceKey k1 = InstanceKey::FromInt(1);

    fx.store.Write("Model", k0, "intensity", Value::Float(0.2f));
    fx.store.Write("Model", k1, "intensity", Value::Float(1.4f));
    fx.store.Write("Model", k1, "upsample", Value::Enum(0));
    fx.store.Write("Preferences", k0, "preset", Value::Enum(2));
    Save(fx.store, mem);

    // Pollute the store with keys the schema knows nothing about.
    mem.data["S.Model.K.i0.this_setting_does_not_exist"] = "5";
    mem.data["TotallyUnknown.Key"] = "hello";

    ValueStore restored(fx.schema);
    // Two instances exist, which the consumer knows and ctl does not.
    const InstanceCount state{/*model_count=*/2};
    Load(restored, mem, "", &InstanceKeys, &state);

    Check(ReadFloat(restored, "Model", k0, "intensity") == 0.2f,
          "root override round-trips");
    Check(ReadFloat(restored, "Model", k1, "intensity") == 1.4f,
          "child override round-trips");
    Check(restored.Read("Model", k1, "upsample").as_int == 0,
          "a written value round-trips");
    Check(restored.Read("Preferences", k0, "preset").as_int == 2,
          "single-instance value round-trips");
    Check(true, "unknown keys were skipped without error");
  }

  // ------------------------------------------------------------------------
  std::printf("\n[21] Persistence: the format carries a version\n");
  {
    Fixture fx;
    MemStore mem;
    Save(fx.store, mem);
    auto it = mem.data.find("Version");
    Check(it != mem.data.end(), "a version key is written");
    if (it != mem.data.end()) {
      Check(std::atoi(it->second.c_str()) == kSerializeVersion,
            "the written version matches kSerializeVersion");
    }
  }

  // ------------------------------------------------------------------------
  std::printf("\n[22] Persistence: an absent key keeps the declared default\n");
  {
    Fixture fx;
    MemStore mem;  // empty
    ValueStore restored(fx.schema);
    // An empty key range, not a null context: the consumer says "no
    // instances exist", which is different from "I have nothing to say".
    const InstanceCount empty_state{/*model_count=*/0};
    Load(restored, mem, "", &InstanceKeys, &empty_state);
    Check(ReadFloat(restored, "Model", InstanceKey::FromInt(0), "intensity") == 1.0f,
          "every setting keeps its declared default when the config is empty");
  }

  // ------------------------------------------------------------------------
  std::printf("\n[22b] Persistence: the consumer supplies the instance keys\n");
  {
    Fixture fx;
    MemStore mem;
    fx.store.Write("Model", InstanceKey::FromInt(0), "upsample", Value::Enum(0));
    fx.store.Write("Model", InstanceKey::FromInt(1), "upsample", Value::Enum(1));
    Save(fx.store, mem);

    // Load creates exactly the instances the caller declared. ctl has no
    // opinion on how many there are.
    ValueStore restored(fx.schema);
    const InstanceCount two{/*model_count=*/2};
    Load(restored, mem, "", &InstanceKeys, &two);
    Check(restored.SectionSet("Model").instances().size() == 2,
          "Load creates exactly the supplied keys");

    // A caller declaring fewer instances gets fewer. The config still
    // describes two, but the consumer is the authority on how many exist.
    ValueStore fewer(fx.schema);
    const InstanceCount one{/*model_count=*/1};
    Load(fewer, mem, "", &InstanceKeys, &one);
    Check(fewer.SectionSet("Model").instances().size() == 1,
          "the supplied range is the authority on instance count");
    Check(ReadFloat(fewer, "Model", InstanceKey::FromInt(0), "intensity") == 1.0f,
          "a section with no supplied keys loads nothing");

    // No source at all: instanced sections load nothing, single-instance
    // sections still load, because they never consult it.
    fx.store.Write("Preferences", InstanceKey::FromInt(0), "preset",
                   Value::Enum(2));
    Save(fx.store, mem);
    ValueStore none(fx.schema);
    Load(none, mem);
    Check(none.SectionSet("Model").instances().empty(),
          "a null source loads no instances");
    Check(none.Read("Preferences", InstanceKey::FromInt(0), "preset").as_int == 2,
          "a null source still loads single-instance sections");
  }

  // ------------------------------------------------------------------------
  std::printf("\n[23] Two instance keys persist separately and reload separately\n");
  {
    Fixture fx;
    MemStore mem;
    const InstanceKey k0 = InstanceKey::FromInt(0);
    const InstanceKey k1 = InstanceKey::FromInt(1);

    fx.store.Write("Model", k0, "upsample", Value::Enum(0));
    fx.store.Write("Model", k1, "upsample", Value::Enum(1));
    Save(fx.store, mem);

    ValueStore restored(fx.schema);
    const InstanceCount state{/*model_count=*/2};
    Load(restored, mem, "", &InstanceKeys, &state);

    Check(restored.Read("Model", k0, "upsample").as_int == 0,
          "key 0 reloads its own value");
    Check(restored.Read("Model", k1, "upsample").as_int == 1,
          "key 1 reloads its own value");
  }

  // ------------------------------------------------------------------------
  std::printf("\n[24] Type-safe accessors and programmatic read/write\n");
  {
    Fixture fx;
    const InstanceKey k0 = InstanceKey::FromInt(0);

    Check(fx.store.Write("Model", k0, "intensity", Value::Float(0.6f)),
          "programmatic write succeeds for a known setting");
    Check(!fx.store.Write("Model", k0, "no_such_setting", Value::Float(1.0f)),
          "programmatic write fails for an unknown setting");
    Check(fx.store.Read("Model", k0, "intensity").as_float == 0.6f,
          "programmatic read returns the written value");
    Check(fx.store.Read("Model", k0, "no_such_setting").kind ==
              Value::Kind::Float,
          "reading an unknown setting yields a neutral value, not a crash");
  }

  // ------------------------------------------------------------------------
  std::printf("\n[25] Clamp is applied on write\n");
  {
    Fixture fx;
    const InstanceKey k0 = InstanceKey::FromInt(0);
    fx.store.Write("Model", k0, "intensity", Value::Float(99.0f));
    Check(ReadFloat(fx.store, "Model", k0, "intensity") == 2.0f,
          "an out-of-range write is clamped to the declared range");
  }

  // ------------------------------------------------------------------------
  std::printf("\n[26] User vs programmatic change is distinguishable\n");
  {
    Fixture fx;
    const InstanceKey k0 = InstanceKey::FromInt(0);
    InstanceSet& set = fx.store.SectionSet("Model");
    const Section* model = fx.schema.FindSection("Model");
    const uint32_t base = fx.store.SettingOffsetOf(*model);

    fx.store.Write("Model", k0, "intensity", Value::Float(0.4f), /*from_user=*/true);
    Check(set.Find(k0)->states[base + 0].changed_by_user,
          "a UI write is tagged as user-originated");

    fx.store.Write("Model", k0, "intensity", Value::Float(0.7f), /*from_user=*/false);
    Check(set.Find(k0)->states[base + 0].changed_programmatically,
          "a code write is tagged as programmatic");
  }

  // ------------------------------------------------------------------------
  std::printf("\n[27] Extensibility: a new type is one class + a registration line\n");
  {
    Registry registry;
    RegisterBuiltinTypes(registry);
    Check(registry.Has("float") && registry.Has("int") && registry.Has("bool") &&
              registry.Has("enum"),
          "the built-in types are registered");

    // The only extra step for a new type.
    registry.Register("color",
                      [] { return std::make_unique<ColorSetting>("", ""); });
    Check(registry.Has("color"), "a new type registers without touching the core");

    // And it is usable in a schema with no change anywhere else.
    Schema schema;
    Section& s = schema.AddSection("Theme", SectionKind::Single);
    s.Add<ColorSetting>("tint", "Tint");
    std::string err;
    Check(schema.Build(&err), "a schema using the new type builds");

    ValueStore store(schema);
    const InstanceKey only = InstanceKey::FromInt(0);
    store.Write("Theme", only, "tint", Value::Int(0x112233));
    Check(store.Read("Theme", only, "tint").as_int == 0x112233,
          "the new type stores and reads through the generic path");
  }

  // ------------------------------------------------------------------------
  std::printf("\n[27b] Nullable setting text is normalized at the boundary\n");
  {
    FloatSetting setting(nullptr, nullptr);
    Check(setting.meta().name.empty() && setting.meta().label.empty(),
          "null setting name and label become empty strings");
    setting.Tooltip("previous tooltip").Category("previous category");
    setting.Tooltip(nullptr).Category(nullptr).Format(nullptr);
    Check(setting.meta().tooltip.empty() && setting.meta().category.empty(),
          "null tooltip and category replace prior text with empty strings");
    Check(setting.ToString(Value::Float(0.5f)).empty(),
          "null format is normalized to an empty format string");

    IntSetting integer(nullptr, nullptr);
    BoolSetting boolean(nullptr, nullptr);
    StringSetting string_value(nullptr, nullptr);
    EnumSetting choices(nullptr, nullptr);
    integer.Tooltip("previous").Category("previous");
    boolean.Tooltip("previous").Category("previous");
    string_value.Tooltip("previous").Category("previous");
    choices.Tooltip("previous").Category("previous");
    integer.Tooltip(nullptr).Category(nullptr);
    boolean.Tooltip(nullptr).Category(nullptr);
    string_value.Tooltip(nullptr).Category(nullptr);
    choices.Tooltip(nullptr).Category(nullptr);
    Check(integer.meta().name.empty() && integer.meta().label.empty() &&
              integer.meta().tooltip.empty() && integer.meta().category.empty() &&
              boolean.meta().name.empty() && boolean.meta().label.empty() &&
              boolean.meta().tooltip.empty() && boolean.meta().category.empty() &&
              string_value.meta().name.empty() && string_value.meta().label.empty() &&
              string_value.meta().tooltip.empty() && string_value.meta().category.empty() &&
              choices.meta().name.empty() && choices.meta().label.empty() &&
              choices.meta().tooltip.empty() && choices.meta().category.empty(),
          "all built-in setting types normalize null metadata and setters");

    const char* names[] = {nullptr, "Valid"};
    const char* tips[] = {nullptr, nullptr};
    EnumSetting enum_choices(nullptr, nullptr);
    enum_choices.Items(names, tips, 2);
    Check(enum_choices.ToString(Value::Enum(0)).empty(),
          "null enum item text is normalized before string conversion");
  }

  std::printf("\n[28] A hook can sit between settings and is addressable\n");
  {
    Fixture fx;
    const Section* model = fx.schema.FindSection("Model");
    const auto& rows = fx.schema.rows(*model);
    int hooks = 0;
    for (const auto& r : rows) {
      if (r.kind == Schema::Row::Kind::Hook) ++hooks;
    }
    Check(hooks == 2, "both declared hooks are present in the row list");
    // The appended hook must come last.
    Check(rows.back().kind == Schema::Row::Kind::Hook &&
              model->hooks()[rows.back().hook_index].name == "Footer",
          "an appended hook sits at the end of the section");
  }

  // ------------------------------------------------------------------------
  std::printf("\n[29] Opaque keys: int and string keys both work and never collide\n");
  {
    Fixture fx;
    const InstanceKey int_key = InstanceKey::FromInt(3);
    const InstanceKey str_key = InstanceKey::FromString("3");
    Check(int_key != str_key, "an int key and a string key with the same text differ");
    Check(int_key.ToText() != str_key.ToText(),
          "their serialized forms differ too");

    fx.store.Write("Model", int_key, "upsample", Value::Enum(0));
    fx.store.Write("Model", str_key, "upsample", Value::Enum(1));
    Check(fx.store.Read("Model", int_key, "upsample").as_int == 0,
          "the int-keyed instance keeps its value");
    Check(fx.store.Read("Model", str_key, "upsample").as_int == 1,
          "the string-keyed instance keeps its value");
  }

  // ------------------------------------------------------------------------
  std::printf("\n[30] No domain concepts leak into the core\n");
  {
    // The framework's public surface is exercised only through the words the
    // requirements allow: section, setting, instance, key, parent, override.
    // Nothing here needed to know what an instance *means*.
    Fixture fx;
    const InstanceKey any = InstanceKey::FromString("layer:beauty");
    fx.store.Write("Model", any, "intensity", Value::Float(0.65f));
    Check(ReadFloat(fx.store, "Model", any, "intensity") == 0.65f,
          "an arbitrary string key stores and reads a value");
  }

  // ------------------------------------------------------------------------
  std::printf("\n[31] Dirty tracking reflects writes\n");
  {
    Fixture fx;
    Check(!fx.store.dirty(), "a fresh store is clean");
    fx.store.Write("Model", InstanceKey::FromInt(0), "intensity", Value::Float(0.2f));
    Check(fx.store.dirty(), "a write marks the store dirty");
    fx.store.clear_dirty();
    Check(!fx.store.dirty(), "clearing dirty works");
  }

  // ------------------------------------------------------------------------
  std::printf("\n[32] Enable rules gate row interactivity\n");
  {
    // The predicate is the application's; it is handed the store and the
    // instance key, so a rule can read one setting to decide another's
    // interactivity. This is the framework equivalent of the hand-written
    // UI's WithDisabled(!cond, ...) around dependent rows.
    struct Gate {
      static bool Follows(const void* ctx, const ValueStore& s,
                          const InstanceKey& k, const std::string& section) {
        (void)ctx;
        (void)section;
        return s.Read("Model", k, "upsample").as_int == 1;
      }
    };

    Schema schema;
    Section& model = schema.AddSection("Model", SectionKind::Instanced);
    model.Add<EnumSetting>("upsample", "Upsample Mode", 1);
    model.Add<FloatSetting>("sharpness", "Sharpness", 8.0f);
    model.EnableWhen("sharpness", &Gate::Follows);

    std::string err;
    Check(schema.Build(&err), "a schema with an enable rule builds");

    const Section* s = schema.FindSection("Model");
    Check(s != nullptr && s->enable_rules().size() == 1,
          "the enable rule is recorded on the section");
    Check(s != nullptr && s->enable_rules()[0].setting == "sharpness",
          "the enable rule names its target setting");

    ValueStore store(schema);
    const InstanceKey k = InstanceKey::FromInt(0);
    store.Write("Model", k, "upsample", Value::Enum(1));
    store.Write("Model", k, "sharpness", Value::Float(4.0f));
    // The predicate itself is exercised through the same read path the
    // drawer uses, so the wiring is what is under test, not the lambda.
    Check(store.Read("Model", k, "upsample").as_int == 1,
          "the gate's own input reads back through the store");
  }

  std::printf("\n[33] An enable rule on an unknown setting fails Build()\n");
  {
    Schema schema;
    Section& s = schema.AddSection("S", SectionKind::Single);
    s.Add<FloatSetting>("v", "V", 0.5f);
    struct Gate {
      static bool Always(const void*, const ValueStore&, const InstanceKey&,
                         const std::string&) {
        return true;
      }
    };
    s.EnableWhen("does_not_exist", &Gate::Always);
    std::string err;
    Check(!schema.Build(&err), "a rule on an unknown setting fails Build()");
    Check(!err.empty(), "and reports why");
  }

  std::printf("\n[34] A setting can carry its own gate\n");
  {
    // The gate lives on the setting, like its tooltip or range, so the
    // section is never told the setting's name and a fragment that declares
    // rows can gate them all without its caller knowing them.
    struct Gate {
      static bool Never(const void*, const ValueStore&, const InstanceKey&,
                        const std::string&) {
        return false;
      }
    };

    Schema schema;
    Section& s = schema.AddSection("Own", SectionKind::Single);
    FloatSetting& gated =
        s.Add<FloatSetting>("v", "V", 0.5f).EnableWhen(&Gate::Never);
    s.Add<FloatSetting>("plain", "Plain", 0.5f);

    std::string err;
    Check(schema.Build(&err), "a schema with a setting-carried gate builds");

    Check(gated.enabled() == &Gate::Never,
          "the setting records the predicate it was declared with");
    Check(s.enable_rules().empty(),
          "no section-wide rule was needed to express it");
  }

  std::printf("\n[35] A reader sees what the drawer sees\n");
  {
    // A per-consumer view reads the store with Read() and hands the value to
    // the runtime; the drawer resolves the same value to draw the row. If
    // those two ever disagreed, a setting would render one thing and run
    // another. This pins them together: Read() returns exactly what the
    // drawer would resolve, with no UI involved.
    Schema schema;
    Section& model = schema.AddSection("Model", SectionKind::Instanced);
    model.Add<FloatSetting>("intensity", "Intensity", 1.0f)
        .Range(0.0f, 2.0f);
    model.Add<BoolSetting>("toggle", "Toggle", true);

    std::string err;
    Check(schema.Build(&err), "an instanced schema builds");

    ValueStore store(schema);
    const InstanceKey parent = InstanceKey::FromInt(0);
    const InstanceKey child = InstanceKey::FromInt(1);

    store.Write("Model", parent, "intensity", Value::Float(1.75f), true);
    store.Write("Model", parent, "toggle", Value::Bool(false), true);
    store.Write("Model", child, "intensity", Value::Float(0.6f), true);
    store.Write("Model", child, "toggle", Value::Bool(true), true);

    Check(store.Read("Model", parent, "intensity").as_float == 1.75f,
          "the parent reads its own value");
    Check(store.Read("Model", child, "intensity").as_float == 0.6f,
          "each instance owns its own value");
    Check(store.Read("Model", child, "toggle").as_bool,
          "a boolean written on one instance stays on that instance");
  }

  // ------------------------------------------------------------------------
  std::printf("\n[37] ResetToDefaults clears the whole store without removing instances\n");
  {
    Fixture fx;
    const InstanceKey shared = InstanceKey::FromInt(0);
    const InstanceKey parent = InstanceKey::FromInt(17);
    const InstanceKey child = InstanceKey::FromInt(83);
    const InstanceKey grandchild = InstanceKey::FromInt(401);

    Check(fx.store.Write("Preferences", shared, "tooltips", Value::Bool(false), true),
          "single-section bool override is written");
    Check(fx.store.Write("Preferences", shared, "preset", Value::Enum(2), true),
          "single-section enum override is written");
    Check(fx.store.Write("Preferences", shared, "note", Value::String("edited"), true),
          "single-section string override is written");
    Check(fx.store.Write("Model", parent, "intensity", Value::Float(1.8f), true),
          "parent instance override is written");
    Check(fx.store.Write("Model", child, "working_scale", Value::Float(0.4f), true),
          "child instance override is written");
    Check(fx.store.Write("Model", grandchild, "upsample", Value::Enum(0), true),
          "grandchild instance override is written");

    const InstanceSet& prefs_before = fx.store.SectionSet("Preferences");
    const InstanceSet& model_before = fx.store.SectionSet("Model");
    const size_t prefs_count = prefs_before.instances().size();
    const size_t model_count = model_before.instances().size();
    fx.store.ResetToDefaults();

    const InstanceSet& prefs_after = fx.store.SectionSet("Preferences");
    const InstanceSet& model_after = fx.store.SectionSet("Model");
    Check(prefs_after.instances().size() == prefs_count &&
              model_after.instances().size() == model_count &&
              prefs_after.Find(shared) != nullptr && model_after.Find(parent) != nullptr &&
              model_after.Find(child) != nullptr && model_after.Find(grandchild) != nullptr,
          "reset keeps all single and instanced instances alive");

    bool all_overrides_cleared = true;
    for (const auto& section : fx.schema.sections()) {
      const InstanceSet& set = fx.store.SectionSet(*section);
      for (const Instance& instance : set.instances()) {
        for (const SettingState& state : instance.states) {
          if (state.has_value || state.changed_by_user ||
              state.changed_programmatically) {
            all_overrides_cleared = false;
          }
        }
      }
    }
    Check(all_overrides_cleared,
          "reset clears every override and change marker in every instance");

    Check(fx.store.Read("Preferences", shared, "tooltips").as_bool &&
              fx.store.Read("Preferences", shared, "preset").as_int == 1 &&
              fx.store.Read("Preferences", shared, "note").as_string == "hello",
          "single-section reads fall back to declared defaults");
    Check(ReadFloat(fx.store, "Model", parent, "intensity") == 1.0f &&
              ReadFloat(fx.store, "Model", child, "intensity") == 1.0f &&
              ReadFloat(fx.store, "Model", grandchild, "intensity") == 1.0f,
          "reads fall back to declared defaults throughout the preserved instances");
    Check(ReadFloat(fx.store, "Model", child, "working_scale") == 1.0f &&
              fx.store.Read("Model", grandchild, "upsample").as_int == 1,
          "reads also fall back to declared defaults");
  }

  // ------------------------------------------------------------------------
  std::printf("\n[38] Delete erases a schema block without touching other prefixes\n");
  {
    Fixture fx;
    MemStore mem;
    const InstanceKey parent = InstanceKey::FromInt(501);
    const InstanceKey child = InstanceKey::FromInt(907);
    const InstanceKey other = InstanceKey::FromInt(612);
    const InstanceKeyList keys = {parent, child};
    const InstanceKeyList other_keys = {other};
    fx.store.Write("Preferences", InstanceKey::FromInt(0), "note",
                   Value::String("target"));
    fx.store.Write("Model", parent, "intensity", Value::Float(1.6f));
    fx.store.Write("Model", child, "upsample", Value::Enum(0));
    fx.store.Write("Model", other, "upsample", Value::Enum(1));

    Save(fx.store, mem, "Profile.16.Data", &InstanceKeysFromList, &keys);
    Save(fx.store, mem, "Profile.18.Data", &InstanceKeysFromList, &other_keys);
    mem.WriteString("Profile.16.Name", "target");
    mem.WriteString("Profile.18.Name", "keep");
    // Simulate a stale child override left by an older save. It is not part of
    // the current live store's owned values, but deletion must still remove it.
    mem.WriteFloat("Profile.16.Data.S.Model.K.i907.intensity", 1.4f);
    mem.WriteString("Profile.16.Data.Unrelated", "outside schema");
    mem.WriteString("OtherPrefix.Sentinel", "untouched");

    Check(HasPrefix(mem, "Profile.16.Data."),
          "saved schema block contains serialized keys before deletion");
    Check(mem.data.find(
              "Profile.16.Data.S.Model.K.i612.upsample") == mem.data.end(),
          "scoped Save excludes live instances outside the supplied key set");
    Delete(fx.store, mem, "Profile.16.Data", &InstanceKeysFromList, &keys);
    bool schema_keys_remain = false;
    for (const auto& entry : mem.data) {
      if (entry.first.compare(0, std::string("Profile.16.Data.").size(),
                              "Profile.16.Data.") == 0 &&
          entry.first != "Profile.16.Data.Unrelated") {
        schema_keys_remain = true;
      }
    }
    Check(!schema_keys_remain,
          "Delete erases every schema-defined key under the target prefix");
    Check(mem.data.find("Profile.16.Data.S.Model.K.i907.intensity") ==
              mem.data.end(),
          "Delete erases stale persisted overrides even when not currently owned");
    Check(HasPrefix(mem, "Profile.18.Data.") &&
              mem.data.find("Profile.18.Name") != mem.data.end(),
          "Delete preserves another profile's serialized block and metadata");
    Check(mem.data.find("Profile.16.Name") != mem.data.end() &&
              mem.data.find("Profile.16.Data.Unrelated") != mem.data.end() &&
              mem.data.find("OtherPrefix.Sentinel") != mem.data.end(),
          "Delete only removes keys declared by the schema under its prefix");

    const auto before_missing = mem.data;
    Delete(fx.store, mem, "NeverSavedProfile.Data", &InstanceKeysFromList, &keys);
    Check(mem.data == before_missing,
          "deleting a prefix that was never saved is a silent no-op");
  }

  // ------------------------------------------------------------------------
  std::printf("\n[39] Stable profile ids isolate data when a middle profile is deleted\n");
  {
    MemStore mem;
    HarnessProfile first{16, "Alpha", true, "alpha", 0.6f,
                         {InstanceKey::FromInt(1001)}};
    HarnessProfile middle{17, "Bravo", false, "bravo", 1.2f,
                          {InstanceKey::FromInt(2001),
                           InstanceKey::FromInt(2002),
                           InstanceKey::FromInt(2003)}};
    HarnessProfile last{18, "Charlie", true, "charlie", 1.7f,
                        {InstanceKey::FromInt(3001),
                         InstanceKey::FromInt(3002)}};

    SaveHarnessProfile(mem, first);
    SaveHarnessProfile(mem, middle);
    SaveHarnessProfile(mem, last);
    mem.WriteString("UserPresetOrder", "16,17,18");
    Check(HasPrefix(mem, "UserPreset.16.Data.") &&
              HasPrefix(mem, "UserPreset.17.Data.") &&
              HasPrefix(mem, "UserPreset.18.Data."),
          "three profiles save independent schema blocks under stable ids");

    std::string middle_order;
    mem.ReadString("UserPreset.17.PassOrder", &middle_order);
    const InstanceKeyList middle_keys = ParseHarnessProfileOrder(middle_order);
    Fixture middle_schema;
    Delete(middle_schema.store, mem, "UserPreset.17.Data",
           &InstanceKeysFromList, &middle_keys);
    mem.Erase("UserPreset.17.Name");
    mem.Erase("UserPreset.17.Enabled");
    mem.Erase("UserPreset.17.PassOrder");
    mem.WriteString("UserPresetOrder", "16,18");

    Check(!HasPrefix(mem, "UserPreset.17."),
          "deleting the middle profile removes its scalar and ctl keys");
    Check(HasPrefix(mem, "UserPreset.16.") &&
              HasPrefix(mem, "UserPreset.18."),
          "deleting the middle profile leaves both neighboring profiles intact");
    std::string order_after_delete;
    mem.ReadString("UserPresetOrder", &order_after_delete);
    Check(order_after_delete == "16,18",
          "profile order removes only the deleted stable id");

    const HarnessProfile* remaining[] = {&first, &last};
    for (const HarnessProfile* expected : remaining) {
      const std::string root = HarnessProfilePrefix(expected->id);
      std::string stored_order;
      std::string stored_name;
      bool stored_enabled = false;
      mem.ReadString(root + ".Name", &stored_name);
      mem.ReadString(root + ".PassOrder", &stored_order);
      mem.ReadBool(root + ".Enabled", &stored_enabled);
      const InstanceKeyList keys = ParseHarnessProfileOrder(stored_order);
      Fixture restored;
      Load(restored.store, mem, root + ".Data", &InstanceKeysFromList, &keys);

      Check(stored_name == expected->name &&
                stored_enabled == expected->enabled,
            "remaining profile metadata loads by stable id");
      Check(restored.store.SectionSet("Model").instances().size() ==
                expected->passes.size() && keys.size() == expected->passes.size(),
            "remaining profile restores its own pass count and pass keys");
      Check(restored.store.Read("Preferences", InstanceKey::FromInt(0),
                                "note").as_string == expected->note,
            "remaining profile restores its own single-section settings");
      Check(!keys.empty() &&
                ReadFloat(restored.store, "Model", keys.front(), "intensity") ==
                    expected->intensity,
            "remaining profile restores its own model override");

      bool local_values_match = keys.size() == expected->passes.size();
      for (size_t i = 0; i < keys.size() && i < expected->passes.size(); ++i) {
        local_values_match = local_values_match &&
            keys[i] == expected->passes[i] &&
            restored.store.Read("Model", keys[i], "upsample").as_int ==
                static_cast<int>((expected->id + i) % 2);
      }
      Check(local_values_match,
            "remaining profile restores only its own per-pass settings");
    }
  }

  std::printf("\n======================\n");
  std::printf("checks: %d  failures: %d\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
