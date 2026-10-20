// The settings menu's options for the screen format, HDR, button prompts and texture packs
// (port_settings.h). Each is a short list of values addressed by index; the options of the picture
// and the window are in gx_settings.cpp. What a change does, and when:
//
//  - Screen Format: the EFB is as wide as the format needs, and the game's camera must be widened by
//    the same factor, which it does once, as an area is made (TMarDirector::setupObjects, patch
//    widescreen-01), as do the area's title card and map screen when they load. So the menu only picks
//    a width; GXPC_ApplyWidescreen, which that patch calls first thing in setupObjects, rebuilds the
//    frame and tells the game which one is in force.
//  - Screen Icons At Edges, HDR (on or off, and its look), the button prompts and HD Textures apply at once.
//  - HD Texture Loading and Shader Warm-up are about what a load does, so they apply from the next
//    area's.
//  - HDR on or off makes or releases the Direct3D swap chain and its OpenGL interop on the spot (hdrEnable and
//    hdrDisable in gx_hdr.cpp). set() runs on the game's thread, the window's, which DXGI needs; the GL thread's
//    share goes through glt::sync. Off, the window swaps by itself again at the V-Sync the menu has now. It is
//    offered where Windows HDR is on for the monitor showing the window, and the options of its look show only
//    while it is on.
//
// Not here, because they cannot be changed while the game runs: the texture packs' folders.
#include "gx_internal.h"
#include "gx_glthread.h"
#include "gx_settings_extra.h"
#include "sms_gx/gx_pc.h"

#include "port_settings.h"

#include <math.h>
#include <stdio.h>
#include <algorithm>
#include <atomic>
#include <initializer_list>
#include <string>
#include <vector>

using namespace gx;

namespace {

// A list of numbers and the name of each.
struct Values {
    std::vector<float> value;
    std::vector<std::string> name;
};

std::vector<float> steps(float from, float to, float by) {
    std::vector<float> out;
    for (float v = from; v <= to + by * 0.01f; v += by) out.push_back(v);
    return out;
}

// The sorted values of `fixed`, and `now` (what the game started with, which a launcher setting
// need not have made one of them) added in its place; name(v, out, size) writes a value's name.
template <class Name>
Values numbers(const std::vector<float>& fixed, float now, float same, Name name) {
    std::vector<float> v = fixed;
    if (std::none_of(v.begin(), v.end(), [&](float x) { return fabsf(x - now) < same; })) v.push_back(now);
    std::sort(v.begin(), v.end());
    Values out;
    for (float x : v) {
        char text[32];
        name(x, text, sizeof text);
        out.value.push_back(x);
        out.name.push_back(text);
    }
    return out;
}

int nearest(const Values& list, float now) {
    int best = 0;
    for (size_t i = 0; i < list.value.size(); i++)
        if (fabsf(list.value[i] - now) < fabsf(list.value[size_t(best)] - now)) best = int(i);
    return best;
}

int countOf(const Values& list) { return std::max(1, int(list.value.size())); }

const char* nameOf(const Values& list, int i) { return i >= 0 && i < int(list.name.size()) ? list.name[size_t(i)].c_str() : ""; }

void whole(float v, char* out, size_t n) { snprintf(out, n, "%d", int(v + 0.5f)); }

int offOnCount() { return 2; }
const char* offOnName(int i) { return i == 1 ? "On" : i == 0 ? "Off" : ""; }

// ------------------------------------------------------------------ screen format
const float kSameWide = 0.002f;
std::atomic<float> s_pendingWide{0.0f};  // the width chosen, 0 while it is the one in force

const Values& wideValues() {
    static Values list;
    static bool built;
    if (!built) {
        built = true;
        // the widths of 16:10, 16:9 and 21:9 over 4:3
        list = numbers({1.0f, 1.2f, 4.0f / 3.0f, 1.75f}, GXPC_GetWidescreen(), kSameWide, [](float v, char* out, size_t n) {
            const auto is = [&](float w) { return fabsf(v - w) < kSameWide; };
            if (v < 1.0f + kSameWide) snprintf(out, n, "Original (4 by 3)");
            else if (is(1.2f)) snprintf(out, n, "16 by 10");
            else if (is(4.0f / 3.0f)) snprintf(out, n, "16 by 9");
            else if (is(1.75f)) snprintf(out, n, "21 by 9");
            else snprintf(out, n, "Wide %.2f", double(v * 4.0f / 3.0f));
        });
    }
    return list;
}

// The width the frame has, or will have once the next area is made.
float wideAfter() {
    const float pending = s_pendingWide.load();
    return pending > 0.0f ? pending : GXPC_GetWidescreen();
}

int wideAvailable() { return rendererReady(); }
int wideCount() { return countOf(wideValues()); }
const char* wideName(int i) { return nameOf(wideValues(), i); }
int wideGet() { return nearest(wideValues(), wideAfter()); }

int wideSet(int i) {
    const Values& list = wideValues();
    if (i < 0 || i >= int(list.value.size())) return 0;
    const float v = list.value[size_t(i)];
    s_pendingWide = fabsf(v - GXPC_GetWidescreen()) < kSameWide ? 0.0f : v;
    return 1;
}

// ------------------------------------------------------------------ screen icons at the edges
int hudAvailable() { return rendererReady() && wideAfter() > 1.0f + kSameWide; }
int hudGet() { return rendererHudEdges() ? 1 : 0; }

int hudSet(int i) {
    if (i < 0 || i > 1) return 0;
    rendererSetHudEdges(i == 1);
    return 1;
}

// ------------------------------------------------------------------ HDR
// HDR itself. Offered where Windows HDR is on for the monitor showing the window, and whenever it is on (to turn
// it off). Turning it on can still fail (no Direct3D 11 device, no swap chain for the window): the picture then
// stays as it was and the menu says so.
int hdrToggleAvailable() { return hdrCanEnable(); }
int hdrToggleGet() { return hdrActive() ? 1 : 0; }

int hdrToggleSet(int i) {
    if (i < 0 || i > 1) return 0;
    if ((i == 1) == hdrActive()) return 1;
    if (i == 1) return hdrEnable() ? 1 : 0;
    hdrDisable();
    // the window is OpenGL's to swap again, at the V-Sync the menu has now (changed while HDR presented, it was
    // only kept)
    glt::sync([] { platformSetVsync(platformVsync()); });
    return 1;
}

// The lists are made when HDR is found on, with the values the game started with among them.
struct HdrLists {
    Values paper, peak, contrast, saturation, highlights;
    bool built = false;
};

const HdrLists& hdrLists() {
    static HdrLists lists;
    if (!lists.built && hdrActive()) {
        lists.built = true;
        const HdrLook now = hdrLook();
        lists.paper = numbers(steps(80, 500, 20), now.paperWhite, 0.5f, whole);
        lists.peak = numbers({400, 500, 600, 800, 1000, 1200, 1500, 2000, 2500, 3000, 4000}, now.peak, 0.5f, whole);
        lists.contrast = numbers(steps(50, 150, 10), now.contrast, 0.5f, whole);
        lists.saturation = numbers(steps(0, 200, 10), now.saturation, 0.5f, whole);
        lists.highlights = numbers(steps(0, 100, 10), now.highlights, 0.5f, whole);
    }
    return lists;
}

int hdrAvailable() { return hdrActive(); }
// With calibration on, Windows gives both, and the two below it would change nothing.
int hdrManualAvailable() { return hdrActive() && !hdrLook().calibration; }

// Sets one field of the look, on the GL thread (the frame being presented reads it there).
template <class Apply>
int hdrChange(const Values& list, int i, Apply apply) {
    if (!hdrActive() || i < 0 || i >= int(list.value.size())) return 0;
    const float v = list.value[size_t(i)];
    glt::sync([&] {
        HdrLook look = hdrLook();
        apply(look, v);
        hdrSetLook(look);
    });
    return 1;
}

int calibrationGet() { return hdrLook().calibration ? 1 : 0; }

int calibrationSet(int i) {
    if (!hdrActive() || i < 0 || i > 1) return 0;
    glt::sync([&] {
        HdrLook look = hdrLook();
        look.calibration = i == 1;
        hdrSetLook(look);
    });
    return 1;
}

int paperCount() { return countOf(hdrLists().paper); }
const char* paperName(int i) { return nameOf(hdrLists().paper, i); }
int paperGet() { return nearest(hdrLists().paper, hdrLook().paperWhite); }
int paperSet(int i) {
    return hdrChange(hdrLists().paper, i, [](HdrLook& look, float v) { look.paperWhite = v; });
}

int peakCount() { return countOf(hdrLists().peak); }
const char* peakName(int i) { return nameOf(hdrLists().peak, i); }
int peakGet() { return nearest(hdrLists().peak, hdrLook().peak); }
int peakSet(int i) {
    return hdrChange(hdrLists().peak, i, [](HdrLook& look, float v) { look.peak = v; });
}

int contrastCount() { return countOf(hdrLists().contrast); }
const char* contrastName(int i) { return nameOf(hdrLists().contrast, i); }
int contrastGet() { return nearest(hdrLists().contrast, hdrLook().contrast); }
int contrastSet(int i) {
    return hdrChange(hdrLists().contrast, i, [](HdrLook& look, float v) { look.contrast = v; });
}

int saturationCount() { return countOf(hdrLists().saturation); }
const char* saturationName(int i) { return nameOf(hdrLists().saturation, i); }
int saturationGet() { return nearest(hdrLists().saturation, hdrLook().saturation); }
int saturationSet(int i) {
    return hdrChange(hdrLists().saturation, i, [](HdrLook& look, float v) { look.saturation = v; });
}

int highlightsCount() { return countOf(hdrLists().highlights); }
const char* highlightsName(int i) { return nameOf(hdrLists().highlights, i); }
int highlightsGet() { return nearest(hdrLists().highlights, hdrLook().highlights); }
int highlightsSet(int i) {
    return hdrChange(hdrLists().highlights, i, [](HdrLook& look, float v) { look.highlights = v; });
}

// ------------------------------------------------------------------ button prompts
int promptsAvail() { return promptsAvailable(); }
int promptStyleCount() { return 6; }
const char* promptStyleName(int i) {
    static const char* const kNames[] = {"GameCube", "Auto", "Xbox", "PlayStation", "Steam Deck", "Keyboard"};
    return i >= 0 && i < 6 ? kNames[i] : "";
}
int promptStyleGet() { return promptsStyle(); }
int promptStyleSet(int i) { return promptsSetStyle(i) ? 1 : 0; }

int promptPadCount() { return 5; }
const char* promptPadName(int i) {
    static const char* const kNames[] = {"Match", "GameCube", "Xbox", "PlayStation", "Steam Deck"};
    return i >= 0 && i < 5 ? kNames[i] : "";
}
int promptPadGet() { return promptsPad(); }
int promptPadSet(int i) { return promptsSetPad(i) ? 1 : 0; }

// ------------------------------------------------------------------ texture packs
// Found at the start: a game started with HD textures off has no pack to turn on (Mario's look, which the
// texture index also holds, is not one).
int texturesAvailable() { return rendererReady() && hiresHasPack(); }
int texturesGet() { return hiresActive() ? 1 : 0; }

int texturesSet(int i) {
    if (i < 0 || i > 1) return 0;
    hiresSetActive(i == 1);
    return 1;
}

// the values are Per Stage, All At Once, While Playing: the loading modes 1, 2 and 0
const int kLoadModes[3] = {1, 2, 0};
int loadingCount() { return 3; }
const char* loadingName(int i) {
    static const char* const kNames[] = {"Per Stage", "All At Once", "While Playing"};
    return i >= 0 && i < 3 ? kNames[i] : "";
}

int loadingGet() {
    const int mode = hiresLoadMode();
    for (int i = 0; i < 3; i++)
        if (kLoadModes[i] == mode) return i;
    return 0;
}

int loadingSet(int i) {
    if (i < 0 || i >= 3) return 0;
    hiresSetLoadMode(kLoadModes[i]);
    return 1;
}

int warmupAvailable() { return shaderWarmupAvailable(); }
int warmupGet() { return shaderWarmupWanted() ? 1 : 0; }

int warmupSet(int i) {
    if (i < 0 || i > 1) return 0;
    shaderSetWarmup(i == 1);
    return 1;
}

const PortSetting kSettings[] = {
    {"widescreen", PORT_SETTINGS_PAGE_DISPLAY, 50, "Screen Format",
     "A wider view, with the HUD and menus kept in the middle. From the next area.", 1, wideAvailable, wideCount,
     wideName, wideGet, wideSet},
    {"hudEdges", PORT_SETTINGS_PAGE_DISPLAY, 55, "Screen Icons At Edges",
     "With a wide view, moves the HUD's counters to the edges of the screen.", 0, hudAvailable, offOnCount, offOnName,
     hudGet, hudSet},
    {"hdr", PORT_SETTINGS_PAGE_DISPLAY, 110, "HDR",
     "Shows the picture in HDR. Needs Windows HDR turned on for this display.", 0, hdrToggleAvailable, offOnCount,
     offOnName, hdrToggleGet, hdrToggleSet},
    {"hdrCalibration", PORT_SETTINGS_PAGE_DISPLAY, 120, "Use Windows HDR Calibration",
     "Paper white and peak brightness from Windows. Off lets you set them here.", 0, hdrAvailable, offOnCount,
     offOnName, calibrationGet, calibrationSet},
    {"hdrPaperWhite", PORT_SETTINGS_PAGE_DISPLAY, 130, "Paper White",
     "How bright ordinary whites, menus and text are, in nits.", 0, hdrManualAvailable, paperCount, paperName,
     paperGet, paperSet},
    {"hdrPeak", PORT_SETTINGS_PAGE_DISPLAY, 140, "Peak Brightness",
     "The brightest a highlight can reach, in nits. Keep it within your display's.", 0, hdrManualAvailable, peakCount,
     peakName, peakGet, peakSet},
    {"hdrContrast", PORT_SETTINGS_PAGE_DISPLAY, 150, "HDR Contrast",
     "Contrast around middle grey. 100 leaves the picture as the game draws it.", 0, hdrAvailable, contrastCount,
     contrastName, contrastGet, contrastSet},
    {"hdrSaturation", PORT_SETTINGS_PAGE_DISPLAY, 160, "HDR Saturation",
     "Strength of colour. 100 leaves it as the game draws it, 0 is black and white.", 0, hdrAvailable,
     saturationCount, saturationName, saturationGet, saturationSet},
    {"hdrHighlights", PORT_SETTINGS_PAGE_DISPLAY, 170, "HDR Highlights",
     "How far the brightest parts reach toward the peak. 0 keeps white at paper white.", 0, hdrAvailable,
     highlightsCount, highlightsName, highlightsGet, highlightsSet},
    {"buttonPrompts", PORT_SETTINGS_PAGE_CONTROLS, 30, "Button Prompts",
     "Which buttons the game's text shows. Auto follows the controller last used.", 0, promptsAvail,
     promptStyleCount, promptStyleName, promptStyleGet, promptStyleSet},
    {"promptPad", PORT_SETTINGS_PAGE_CONTROLS, 40, "Prompt Controller",
     "With Auto prompts, how every controller's buttons look. Match follows the pad.", 0, promptsAvail,
     promptPadCount, promptPadName, promptPadGet, promptPadSet},
    {"textures", PORT_SETTINGS_PAGE_GRAPHICS, 100, "HD Textures",
     "Replaces the game's textures with the HD pack. Needs a lot of video memory.", 0, texturesAvailable,
     offOnCount, offOnName, texturesGet, texturesSet},
    {"textureLoading", PORT_SETTINGS_PAGE_GRAPHICS, 110, "HD Texture Loading",
     "Read per area, all at once (4 GB) or as drawn. Takes effect from the next area.", 1, texturesAvailable,
     loadingCount, loadingName, loadingGet, loadingSet},
    {"shaderWarmup", PORT_SETTINGS_PAGE_GRAPHICS, 120, "Shader Warm-up",
     "Compiles an area's shaders as it loads, not as you play. From the next area.", 1, warmupAvailable,
     offOnCount, offOnName, warmupGet, warmupSet},
};

}  // namespace

extern "C" {

const PortSetting* gx_port_settings_extra(int* count) {
    *count = int(sizeof kSettings / sizeof kSettings[0]);
    return kSettings;
}

// Called as the game starts making an area (see the top of this file), on the game's thread:
// builds the frame at the width the menu chose, if it chose one, and returns the width in force.
float GXPC_ApplyWidescreen(void) {
    const float want = s_pendingWide.exchange(0.0f);
    if (want > 0.0f && rendererReady() && fabsf(want - GXPC_GetWidescreen()) >= kSameWide) {
        bool ok = true;
        glt::sync([&] { ok = rendererSetWide(want); });
        if (!ok) logmsg("widescreen: a frame %.3f times 4:3 wide did not fit; keeping %.3f", double(want),
                        double(GXPC_GetWidescreen()));
    }
    return GXPC_GetWidescreen();
}

}  // extern "C"
