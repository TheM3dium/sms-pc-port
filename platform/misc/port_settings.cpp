// The settings menu's view of the options (port_settings.h): every module's table
// joined into the launcher's pages, and the launcher's graphics presets on top.
#include "port_settings.h"

#include <string.h>
#include <algorithm>
#include <vector>

namespace {

const char* const kPageNames[PORT_SETTINGS_PAGE_COUNT] = {"Graphics", "Display", "Gameplay", "Controls"};

// ------------------------------------------------------------------ presets
// The launcher's Low, Medium, High and Ultra (sms-launcher src/presets.js): the settings each
// sets, by the launcher's id and the text of the value. Custom is what is shown when the
// settings are no longer one of them.
struct PresetValue {
    const char* id;
    const char* value;
};
struct Preset {
    const char* name;
    PresetValue values[7];
};
const Preset kPresets[] = {
    {"Low", {{"resolution", "Native"}, {"msaa", "Off"}, {"fxaa", "Off"}, {"anisotropic", "Off"}, {"sharpen", "Off"},
             {"presentFilter", "Bilinear"}, {"heatHaze", "Off"}}},
    {"Medium", {{"resolution", "2x"}, {"msaa", "Off"}, {"fxaa", "On"}, {"anisotropic", "4x"}, {"sharpen", "Off"},
                {"presentFilter", "Bilinear"}, {"heatHaze", "On"}}},
    {"High", {{"resolution", "3x"}, {"msaa", "4x"}, {"fxaa", "On"}, {"anisotropic", "8x"}, {"sharpen", "Off"},
              {"presentFilter", "Bilinear"}, {"heatHaze", "On"}}},
    {"Ultra", {{"resolution", "4x"}, {"msaa", "8x"}, {"fxaa", "On"}, {"anisotropic", "16x"}, {"sharpen", "Off"},
               {"presentFilter", "Bilinear"}, {"heatHaze", "On"}}},
};
const int kPresetCount = int(sizeof kPresets / sizeof kPresets[0]);
const int kCustom = kPresetCount;  // the value after the presets

std::vector<const PortSetting*> s_all;  // every module's options, with the preset's, in page order
bool s_built;

const PortSetting* findById(const char* id) {
    for (const PortSetting* s : s_all)
        if (s->id && !strcmp(s->id, id)) return s;
    return nullptr;
}

// The value of `s` whose text is `text`, or -1.
int valueNamed(const PortSetting* s, const char* text) {
    for (int i = 0, n = s->count(); i < n; i++)
        if (!strcmp(s->valueName(i), text)) return i;
    return -1;
}

bool presetApplies(const Preset& p) {
    for (const PresetValue& v : p.values) {
        const PortSetting* s = findById(v.id);
        if (!s) continue;  // not on this machine (no anisotropic filtering, say): not a difference
        if (s->available && !s->available()) continue;
        if (valueNamed(s, v.value) != s->get()) return false;
    }
    return true;
}

int presetAvailable() { return 1; }
int presetCount() { return kPresetCount + 1; }
const char* presetName(int value) { return value >= 0 && value < kPresetCount ? kPresets[value].name : "Custom"; }
int presetGet() {
    for (int i = 0; i < kPresetCount; i++)
        if (presetApplies(kPresets[i])) return i;
    return kCustom;
}
// Applies every setting the preset sets; one that cannot be applied (a resolution that does not fit,
// a driver without 16x) leaves the rest as they were set: the menu shows Custom then.
int presetSet(int value) {
    if (value < 0 || value >= kPresetCount) return 1;  // Custom changes nothing
    int ok = 1;
    for (const PresetValue& v : kPresets[value].values) {
        const PortSetting* s = findById(v.id);
        if (!s || (s->available && !s->available())) continue;
        int i = valueNamed(s, v.value);
        // 16x anisotropy or 8x MSAA on a machine that tops out lower: its highest
        if (i < 0 && (!strcmp(v.id, "anisotropic") || !strcmp(v.id, "msaa"))) i = s->count() - 1;
        if (i < 0) continue;
        if (i != s->get() && !s->set(i)) ok = 0;
    }
    return ok;
}

const PortSetting kPreset = {
    "graphicsPreset", PORT_SETTINGS_PAGE_GRAPHICS, 5, "Graphics Preset",
    "Sets resolution, smoothing and filtering together. Custom is your own mix.", 0, presetAvailable, presetCount,
    presetName, presetGet, presetSet};

void build() {
    if (s_built) return;
    s_built = true;
    auto add = [&](const PortSetting* table, int count) {
        for (int i = 0; i < count; i++) s_all.push_back(&table[i]);
    };
    int n = 0;
    const PortSetting* t;
    t = gx_port_settings(&n);
    add(t, n);
    t = gx_port_settings_extra(&n);
    add(t, n);
    t = port_pad_settings(&n);
    add(t, n);
    t = port_audio_settings(&n);
    add(t, n);
    t = port_runtime_settings(&n);
    add(t, n);
    s_all.push_back(&kPreset);
    // by page, then by order; a stable sort keeps a module's table order for ties
    std::stable_sort(s_all.begin(), s_all.end(), [](const PortSetting* a, const PortSetting* b) {
        return a->page != b->page ? a->page < b->page : a->order < b->order;
    });
}

// The index-th option of the page that is available now.
const PortSetting* nth(int page, int index) {
    int seen = 0;
    for (const PortSetting* s : s_all) {
        if (s->page != page || (s->available && !s->available())) continue;
        if (seen++ == index) return s;
    }
    return nullptr;
}

}  // namespace

extern "C" {

const char* port_settings_page_name(int page) { return page >= 0 && page < PORT_SETTINGS_PAGE_COUNT ? kPageNames[page] : ""; }

int port_settings_count(int page) {
    build();
    int n = 0;
    for (const PortSetting* s : s_all)
        if (s->page == page && (!s->available || s->available())) n++;
    return n;
}

int port_settings_page_available(int page) { return port_settings_count(page) > 0; }

const PortSetting* port_settings_option(int page, int index) {
    build();
    return nth(page, index);
}

}  // extern "C"
