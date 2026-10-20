// The settings menu's view of the options (port_settings.h): every module's table
// joined into the launcher's pages, the launcher's graphics presets on top, and
// what the menu changed saved for the launcher to keep (SMS_SETTINGS_SAVE).
#include "port_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <iterator>
#include <string>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

extern "C" void port_log(const char* fmt, ...);

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

// The value a preset gives `s`: 16x anisotropy or 8x MSAA on a machine that tops out lower is its highest.
int presetIndex(const PortSetting* s, const PresetValue& v) {
    const int i = valueNamed(s, v.value);
    if (i < 0 && (!strcmp(v.id, "anisotropic") || !strcmp(v.id, "msaa"))) return s->count() - 1;
    return i;
}

bool presetApplies(const Preset& p) {
    for (const PresetValue& v : p.values) {
        const PortSetting* s = findById(v.id);
        if (!s) continue;  // not on this machine (no anisotropic filtering, say): not a difference
        if (s->available && !s->available()) continue;
        if (presetIndex(s, v) != s->get()) return false;
    }
    return true;
}

// The preset's values with the upscaler first: leaving FSR 1 or NIS gives the resolution back (it is not
// offered while one is on).
std::vector<PresetValue> presetOrder(const Preset& p) {
    std::vector<PresetValue> out(std::begin(p.values), std::end(p.values));
    std::stable_partition(out.begin(), out.end(), [](const PresetValue& v) { return !strcmp(v.id, "presentFilter"); });
    return out;
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
    for (const PresetValue& v : presetOrder(kPresets[value])) {
        const PortSetting* s = findById(v.id);
        if (!s || (s->available && !s->available())) continue;
        const int i = presetIndex(s, v);
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

// ------------------------------------------------------------------ saving for the launcher
// SMS Launcher names a file in SMS_SETTINGS_SAVE; each change the menu makes is written there, with the
// others made before it, as JSON in the launcher's own values ({"settings": {"msaa": 4, ...}}), and the
// launcher makes them its settings when the game closes (sms-launcher src/game-settings.js). What the
// value is in the launcher's terms comes from the text the menu shows: the names below are the menu's.
// A value the launcher has no word for (a resolution FSR made, a width settings.txt gave) is not saved.
struct Word {
    const char* text;
    const char* value;  // JSON
};
struct Words {
    const char* id;
    Word words[7];
};
const Words kWords[] = {
    {"graphicsPreset", {{"Low", "\"low\""}, {"Medium", "\"medium\""}, {"High", "\"high\""}, {"Ultra", "\"ultra\""}, {"Custom", "\"custom\""}}},
    {"presentFilter", {{"Bilinear", "\"bilinear\""}, {"Sharp", "\"sharp\""}, {"Nearest", "\"nearest\""}, {"FSR 1", "\"fsr\""}, {"NIS", "\"nis\""}}},
    {"fsrMode", {{"Native", "\"native\""}, {"Quality", "\"quality\""}, {"Balanced", "\"balanced\""}, {"Performance", "\"performance\""},
                 {"Ultra Performance", "\"ultraperformance\""}}},
    {"textureLoading", {{"Per Stage", "\"stage\""}, {"All At Once", "\"all\""}, {"While Playing", "\"play\""}}},
    {"fullscreenMode", {{"Borderless", "\"desktop\""}, {"Exclusive", "\"exclusive\""}}},
    {"widescreen", {{"Original (4 by 3)", "\"off\""}, {"16 by 10", "\"16:10\""}, {"16 by 9", "\"16:9\""}, {"21 by 9", "\"21:9\""}}},
    {"aspect", {{"Keep", "\"keep\""}, {"Stretch", "\"stretch\""}, {"Whole Multiples", "\"integer\""}}},
    {"vsync", {{"Off", "\"off\""}, {"On", "\"on\""}, {"Adaptive", "\"adaptive\""}}},
    {"buttonPrompts", {{"GameCube", "\"gamecube\""}, {"Auto", "\"auto\""}, {"Xbox", "\"xbox\""}, {"PlayStation", "\"playstation\""},
                       {"Steam Deck", "\"steamdeck\""}, {"Keyboard", "\"keyboard\""}}},
    {"promptPad", {{"Match", "\"match\""}, {"GameCube", "\"gamecube\""}, {"Xbox", "\"xbox\""}, {"PlayStation", "\"playstation\""},
                   {"Steam Deck", "\"steamdeck\""}}},
};
// Off and On, as false and true.
const char* const kSwitches[] = {"fxaa", "heatHaze", "textures", "shaderWarmup", "fullscreen", "hudEdges", "overlay", "hdr",
                                 "hdrCalibration", "skipMovies", "invertCameraX", "invertCameraY", "freeCamera", "mouseCamera"};
// Whole numbers, as shown; Off is 0.
const char* const kNumbers[] = {"sharpen", "brightness", "frameRate", "hdrPaperWhite", "hdrPeak", "hdrContrast", "hdrSaturation",
                                "hdrHighlights", "volume", "musicVolume", "sfxVolume", "cameraSpeed", "mouseSensitivity",
                                "softTrigger", "rumbleStrength"};
// 2x and so on as the number; Native is 1 and Off 0.
const char* const kTimes[] = {"resolution", "msaa", "anisotropic"};

bool listed(const char* const* ids, size_t n, const char* id) {
    for (size_t i = 0; i < n; i++)
        if (!strcmp(ids[i], id)) return true;
    return false;
}

bool wholeNumber(const char* text, int* out) {
    char* end = nullptr;
    const long v = strtol(text, &end, 10);
    if (end == text || *end) return false;
    *out = int(v);
    return true;
}

// The launcher's value (JSON) for the value of `s` at `index` that shows `text`; empty when it has none.
std::string launcherValue(const PortSetting* s, int index, const char* text) {
    const char* id = s->id;
    char out[64];
    int n = 0;
    for (const Words& w : kWords) {
        if (strcmp(w.id, id)) continue;
        for (const Word& word : w.words)
            if (word.text && !strcmp(word.text, text)) return word.value;
        return "";
    }
    if (listed(kSwitches, sizeof kSwitches / sizeof *kSwitches, id))
        return !strcmp(text, "On") ? "true" : !strcmp(text, "Off") ? "false" : "";
    if (listed(kNumbers, sizeof kNumbers / sizeof *kNumbers, id)) {
        if (!strcmp(text, "Off")) return "0";
        if (!wholeNumber(text, &n)) return "";
        snprintf(out, sizeof out, "%d", n);
        return out;
    }
    if (listed(kTimes, sizeof kTimes / sizeof *kTimes, id)) {
        if (!strcmp(text, "Native")) return "1";
        if (!strcmp(text, "Off")) return "0";
        const size_t len = strlen(text);
        if (len < 2 || text[len - 1] != 'x') return "";
        std::string number(text, len - 1);
        if (!wholeNumber(number.c_str(), &n)) return "";
        snprintf(out, sizeof out, "%d", n);
        return out;
    }
    if (!strcmp(id, "exclusiveResolution")) {  // Desktop, or "2560 x 1440"
        int w = 0, h = 0;
        if (!strcmp(text, "Desktop")) return "\"desktop\"";
        if (sscanf(text, "%d x %d", &w, &h) != 2) return "";
        snprintf(out, sizeof out, "\"%dx%d\"", w, h);
        return out;
    }
    if (!strcmp(id, "display")) {  // Monitor 1 is the primary one
        if (index == 0) return "\"primary\"";
        snprintf(out, sizeof out, "\"monitor%d\"", index + 1);
        return out;
    }
    return "";
}

std::vector<std::pair<std::string, std::string>> s_saved;  // id, the launcher's value; in the order first changed

void record(const PortSetting* s, int index, const char* text) {
    if (!s->id) return;
    const std::string value = launcherValue(s, index, text);
    if (value.empty()) {
        port_log("[settings] %s: \"%s\" is not one of the launcher's values; not saved\n", s->id, text);
        return;
    }
    for (auto& saved : s_saved)
        if (saved.first == s->id) {
            saved.second = value;
            return;
        }
    s_saved.emplace_back(s->id, value);
}

// The file is written whole and then put in place, so the launcher never reads half of it.
FILE* openFile(const std::string& path) {
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wide(size_t(n > 0 ? n : 1), L'\0');
    if (n <= 0 || !MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wide[0], n)) return nullptr;
    return _wfopen(wide.c_str(), L"wb");
#else
    return fopen(path.c_str(), "wb");
#endif
}

bool replaceFile(const std::string& from, const std::string& to) {
#ifdef _WIN32
    const auto wide = [](const std::string& s) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring out(size_t(n > 0 ? n : 1), L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], n);
        return out;
    };
    return MoveFileExW(wide(from).c_str(), wide(to).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return rename(from.c_str(), to.c_str()) == 0;
#endif
}

void save() {
    const char* path = getenv("SMS_SETTINGS_SAVE");
    if (!path || !*path || s_saved.empty()) return;
    std::string text = "{\n  \"settings\": {";
    for (size_t i = 0; i < s_saved.size(); i++)
        text += (i ? ",\n    \"" : "\n    \"") + s_saved[i].first + "\": " + s_saved[i].second;
    text += "\n  }\n}\n";
    const std::string temporary = std::string(path) + ".tmp";
    FILE* f = openFile(temporary);
    bool ok = f && fwrite(text.data(), 1, text.size(), f) == text.size();
    if (f) ok = fclose(f) == 0 && ok;
    ok = ok && replaceFile(temporary, path);
    static bool told;
    if (!ok && !told) {
        told = true;
        port_log("[settings] could not save the menu's changes for the launcher to %s\n", path);
    }
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

int port_settings_set(const PortSetting* s, int value) {
    if (!s || value < 0 || value >= s->count()) return 0;
    const std::string text = s->valueName(value);  // before set: a list can be made again by it
    if (!s->set(value)) return 0;
    if (s == &kPreset && value < kPresetCount) {  // the preset, and every setting it set as each is now
        for (const PresetValue& v : kPresets[value].values) {
            const PortSetting* changed = findById(v.id);
            if (!changed || (changed->available && !changed->available())) continue;
            const int now = changed->get();
            const std::string shown = changed->valueName(now);
            record(changed, now, shown.c_str());
        }
    }
    record(s, value, text.c_str());
    save();
    return 1;
}

}  // extern "C"
