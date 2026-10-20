// The picture and window options the settings menu changes while the game runs (port_settings.h),
// in the launcher's words. Each option is a short list of values; a change that touches GL runs on the
// GL thread, between frames (the menu runs in the paused game), and one that touches the window or the
// mouse is only asked of the event pump (gx_platform.cpp). Either way it takes effect at once.
#include "gx_internal.h"
#include "gx_glthread.h"
#include "sms_gx/gx_pc.h"

#include "port_settings.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <initializer_list>
#include <vector>

using namespace gx;

namespace {

enum Id {
    RESOLUTION,
    MSAA,
    FXAA,
    ANISOTROPIC,
    SHARPEN,
    FILTER,
    FSR_MODE,
    FULLSCREEN,
    FULLSCREEN_TYPE,
    FULLSCREEN_SIZE,
    MONITOR,
    ASPECT,
    BRIGHTNESS,
    VSYNC,
    OVERLAY,
    MOUSE_LOOK,
    OPTION_COUNT
};

// One value of an option: `value` is what the option is in its own units (the internal resolution's
// scale, a renderer filter code, ...); an exclusive fullscreen size also has w and h.
struct Choice {
    float value;
    int w, h;
    char name[24];
};

std::vector<Choice> s_choices[OPTION_COUNT];
bool s_built;
float s_maxAniso;
int s_fsrPreferred;       // the FSR mode picked last (or the launcher's, Quality when it gave none)
float s_scaleOutsideFsr;  // the resolution without FSR 1 or NIS, which the upscaling mode replaces while one is on
int s_sizesDisplay = -1;  // the display the exclusive sizes were listed for

// The launcher's choices (Picture sharpness and Anti-aliasing), so that what the menu picks is one it can keep.
const float kScales[] = {1, 2, 3, 4};

bool upscaling(int filter) { return filter == 3 || filter == 4; }

void resolutionName(float v, char* out, size_t n) {
    if (v == 1.0f) snprintf(out, n, "Native");
    else if (v == floorf(v)) snprintf(out, n, "%dx", int(v));
    else snprintf(out, n, "%.3gx", double(v));
}

// The sorted list of values `fixed` makes, and the one in force now (a launcher setting need not be
// one of them) added in its place; name(v, out, size) writes a value's name.
template <size_t N, class Name>
void list(int option, const float (&fixed)[N], float now, float limit, Name name) {
    std::vector<Choice>& out = s_choices[option];
    out.clear();
    std::vector<float> values;
    for (float v : fixed)
        if (limit <= 0.0f || v <= limit) values.push_back(v);
    if (std::none_of(values.begin(), values.end(), [&](float v) { return fabsf(v - now) < 0.005f; }))
        values.push_back(now);
    std::sort(values.begin(), values.end());
    for (float v : values) {
        Choice c = {};
        c.value = v;
        name(v, c.name, sizeof c.name);
        out.push_back(c);
    }
}

void listNames(int option, std::initializer_list<const char*> names) {
    std::vector<Choice>& out = s_choices[option];
    out.clear();
    float v = 0.0f;
    for (const char* n : names) {
        Choice c = {};
        c.value = v++;
        snprintf(c.name, sizeof c.name, "%s", n);
        out.push_back(c);
    }
}

// The picture can leave the list: the upscaling modes make fractional scales. Lists again, with the
// scale in force added, when it is not in it.
void ensureResolution() {
    static float listedFor = -1.0f;  // the scale the list was made with (a scale FSR left behind leaves it again)
    const float now = efbScale();
    if (!s_choices[RESOLUTION].empty() && fabsf(now - listedFor) < 0.005f) return;
    listedFor = now;
    list(RESOLUTION, kScales, now, 0.0f, resolutionName);
}

bool lessSize(const DisplaySize& a, const DisplaySize& b) { return a.w != b.w ? a.w < b.w : a.h < b.h; }

// Desktop, then what the display the window is on (or goes to) can switch to; again when the display
// changes, or when the size in force (SMS_FULLSCREEN_MODE) is not among them.
void ensureSizes() {
    const int display = platformDisplay();
    int w = 0, h = 0;
    platformFullscreenSize(&w, &h);
    std::vector<Choice>& c = s_choices[FULLSCREEN_SIZE];
    const bool listed = w == 0 || std::any_of(c.begin(), c.end(), [&](const Choice& x) { return x.w == w && x.h == h; });
    if (!c.empty() && display == s_sizesDisplay && listed) return;
    s_sizesDisplay = display;
    std::vector<DisplaySize> sizes = platformFullscreenSizes(display);
    if (w > 0 && std::none_of(sizes.begin(), sizes.end(), [&](const DisplaySize& s) { return s.w == w && s.h == h; })) {
        sizes.push_back({w, h});
        std::sort(sizes.begin(), sizes.end(), lessSize);
    }
    c.clear();
    Choice desktop = {};
    snprintf(desktop.name, sizeof desktop.name, "Desktop");
    c.push_back(desktop);
    for (const DisplaySize& s : sizes) {
        Choice size = {};
        size.value = float(c.size());
        size.w = s.w;
        size.h = s.h;
        snprintf(size.name, sizeof size.name, "%d x %d", s.w, s.h);
        c.push_back(size);
    }
}

// Monitor 1 (the primary one) and the others, as many as there are.
void ensureMonitors() {
    const int count = platformDisplayCount();
    std::vector<Choice>& c = s_choices[MONITOR];
    if (int(c.size()) == count) return;
    c.clear();
    for (int i = 0; i < count; i++) {
        Choice m = {};
        m.value = float(i);
        snprintf(m.name, sizeof m.name, i == 0 ? "Monitor %d (primary)" : "Monitor %d", i + 1);
        c.push_back(m);
    }
}

// On the GL thread: the driver's limits are asked there.
void build() {
    if (s_built) return;
    s_built = true;
    glt::sync([] { s_maxAniso = textureMaxAniso(); });
    const PostSettings post = postSettings();
    s_fsrPreferred = platformFsrModeAtStart() >= 0 ? platformFsrModeAtStart() : 1;
    // With FSR 1 or NIS at the start, the mode made the scale; SMS_GX_SCALE is the one to go back to without them.
    s_scaleOutsideFsr = efbScale();
    if (upscaling(post.filter)) {
        const char* e = getenv("SMS_GX_SCALE");
        const float asked = e ? float(atof(e)) : 0.0f;
        s_scaleOutsideFsr = asked >= 1.0f ? std::min(8.0f, asked) : std::max(1.0f, floorf(efbScale() + 0.5f));
    }
    ensureResolution();
    static const float kSamples[] = {0, 2, 4, 8};
    list(MSAA, kSamples, float(rendererMsaa()), float(rendererMaxSamples()), [](float v, char* out, size_t n) {
        if (v < 2.0f) snprintf(out, n, "Off");
        else snprintf(out, n, "%dx", int(v));
    });
    static const float kAniso[] = {0, 2, 4, 8, 16};
    list(ANISOTROPIC, kAniso, textureAniso(), s_maxAniso, [](float v, char* out, size_t n) {
        if (v < 2.0f) snprintf(out, n, "Off");
        else snprintf(out, n, "%dx", int(v));
    });
    listNames(FXAA, {"Off", "On"});
    // the launcher's order; each value is the renderer's filter code (0 bilinear, 1 nearest, 2 sharp, 3 fsr, 4 nis)
    listNames(FILTER, {"Bilinear", "Sharp", "Nearest", "FSR 1", "NIS"});
    static const float kFilterCodes[] = {0, 2, 1, 3, 4};
    for (size_t i = 0; i < s_choices[FILTER].size(); i++) s_choices[FILTER][i].value = kFilterCodes[i];
    static const float kSharpen[] = {0, 0.25f, 0.5f, 0.75f, 1.0f};
    list(SHARPEN, kSharpen, post.sharpen, 0.0f, [](float v, char* out, size_t n) {
        if (v < 0.005f) snprintf(out, n, "Off");
        else snprintf(out, n, "%d", int(v * 100.0f + 0.5f));  // no %: the game's font draws it as a button
    });
    listNames(FSR_MODE, {"Native", "Quality", "Balanced", "Performance", "Ultra Performance"});
    listNames(FULLSCREEN, {"Off", "On"});
    listNames(FULLSCREEN_TYPE, {"Borderless", "Exclusive"});
    listNames(ASPECT, {"Keep", "Stretch", "Whole Multiples"});
    static const float kGamma[] = {0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f, 1.1f, 1.2f, 1.3f, 1.4f, 1.5f, 1.6f, 1.7f, 1.8f, 1.9f, 2.0f};
    list(BRIGHTNESS, kGamma, post.gamma, 0.0f,
         [](float v, char* out, size_t n) { snprintf(out, n, "%d", int(v * 100.0f + 0.5f)); });
    listNames(VSYNC, {"Off", "On", "Adaptive"});
    listNames(OVERLAY, {"Off", "On"});
    listNames(MOUSE_LOOK, {"Off", "On"});
}

// The values of an option, with the lists that follow the window or the picture brought up to date.
const std::vector<Choice>& choices(int id) {
    static const std::vector<Choice> none;
    if (!rendererReady()) return none;
    build();
    switch (id) {
    case RESOLUTION: ensureResolution(); break;
    case FULLSCREEN_SIZE: ensureSizes(); break;
    case MONITOR: ensureMonitors(); break;
    }
    return s_choices[id];
}

int nearest(const std::vector<Choice>& c, float now) {
    int best = 0;
    for (size_t i = 0; i < c.size(); i++)
        if (fabsf(c[i].value - now) < fabsf(c[size_t(best)].value - now)) best = int(i);
    return best;
}

int available(int id) {
    if (!rendererReady()) return 0;
    build();
    switch (id) {
    case RESOLUTION: return !upscaling(postSettings().filter);  // the upscaling mode sets it then
    case MSAA: return rendererMaxSamples() >= 2;
    case ANISOTROPIC: return s_maxAniso >= 2.0f;
    case FSR_MODE: {
        const int filter = postSettings().filter;
        return platformHasWindow() && (filter == 3 || filter == 4);
    }
    case FULLSCREEN_SIZE: return platformHasWindow() && platformExclusive();
    case MONITOR: return platformDisplayCount() >= 2;
    case FULLSCREEN:
    case FULLSCREEN_TYPE:
    case VSYNC:
    case OVERLAY:
    case MOUSE_LOOK: return platformHasWindow();
    default: return 1;
    }
}

// The index of the value in force: the one nearest what the option is now.
int get(int id) {
    const std::vector<Choice>& c = choices(id);
    if (c.empty()) return 0;
    const PostSettings post = postSettings();
    switch (id) {
    case RESOLUTION: return nearest(c, efbScale());
    case MSAA: return nearest(c, float(rendererMsaa()));
    case ANISOTROPIC: return nearest(c, textureAniso());
    case FXAA: return post.fxaa ? 1 : 0;
    case FILTER: return nearest(c, float(post.filter));
    case SHARPEN: return nearest(c, post.sharpen);
    // The mode picked last (or the launcher's): modes can make the same scale (a small window clamps them all to
    // native), and the window can change size since, so the scale in force does not say which it was.
    case FSR_MODE: return std::min(s_fsrPreferred, int(c.size()) - 1);
    case FULLSCREEN: return platformFullscreen() ? 1 : 0;
    case FULLSCREEN_TYPE: return platformExclusive() ? 1 : 0;
    case FULLSCREEN_SIZE: {
        int w = 0, h = 0;
        platformFullscreenSize(&w, &h);
        for (size_t i = 0; i < c.size(); i++)
            if (c[i].w == w && c[i].h == h) return int(i);
        return 0;
    }
    case MONITOR: return std::min(platformDisplay(), int(c.size()) - 1);
    case ASPECT: return nearest(c, float(post.aspect));
    case BRIGHTNESS: return nearest(c, post.gamma);
    case VSYNC: return platformVsync() < 0 ? 2 : std::min(platformVsync(), 1);
    case OVERLAY: return GXPC_OverlayVisible() ? 1 : 0;
    case MOUSE_LOOK: return platformMouseLook() ? 1 : 0;
    }
    return 0;
}

int set(int id, int value) {
    const std::vector<Choice>& c = choices(id);
    if (value < 0 || value >= int(c.size())) return 0;
    const Choice choice = c[size_t(value)];  // a copy: the lists can be made again meanwhile
    const float v = choice.value;
    bool ok = true;
    switch (id) {
    case FULLSCREEN: platformRequestFullscreen(v >= 0.5f); return 1;
    case FULLSCREEN_TYPE: platformRequestExclusive(v >= 0.5f); return 1;
    case FULLSCREEN_SIZE: platformRequestFullscreenSize(choice.w, choice.h); return 1;
    case MONITOR: platformRequestDisplay(int(v + 0.5f)); return 1;
    case MOUSE_LOOK: platformRequestMouseLook(v >= 0.5f); return 1;
    case FSR_MODE: {
        const float scale = platformFsrScale(value);
        if (scale <= 0.0f) return 0;
        glt::sync([&] {
            if (fabsf(scale - efbScale()) < 0.005f) return;
            ok = rendererResize(scale, rendererMsaa());
            if (ok) textureResetSamplers(textureAniso());  // an effect's LOD bias follows the resolution
        });
        if (ok) s_fsrPreferred = value;
        return ok;
    }
    }
    // the window is the game thread's to measure
    const float fsrScale = id == FILTER ? platformFsrScale(s_fsrPreferred) : 0.0f;
    glt::sync([&] {
        PostSettings post = postSettings();
        switch (id) {
        case RESOLUTION:
            ok = rendererResize(v, rendererMsaa());
            if (ok) {
                s_scaleOutsideFsr = v;
                textureResetSamplers(textureAniso());  // an effect's LOD bias follows the resolution
            }
            return;
        case MSAA: ok = rendererResize(efbScale(), int(v)); return;
        case ANISOTROPIC: textureResetSamplers(v); return;
        case FXAA: post.fxaa = v >= 0.5f; break;
        case FILTER: {
            // FSR 1 and NIS draw at their mode's scale, as when the game starts with one; without them it is the
            // resolution again
            const int filter = int(v + 0.5f);
            if (upscaling(filter) != upscaling(post.filter)) {
                const float scale = upscaling(filter) ? fsrScale : s_scaleOutsideFsr;
                if (upscaling(filter)) s_scaleOutsideFsr = efbScale();
                if (scale > 0.0f && fabsf(scale - efbScale()) >= 0.005f) {
                    ok = rendererResize(scale, rendererMsaa());
                    if (!ok) return;
                    textureResetSamplers(textureAniso());
                }
            }
            post.filter = filter;
            break;
        }
        case SHARPEN: post.sharpen = v; break;
        case BRIGHTNESS: post.gamma = v; break;
        case ASPECT: post.aspect = int(v + 0.5f); break;
        case VSYNC: ok = platformSetVsync(v >= 1.5f ? -1 : int(v + 0.5f)); return;
        case OVERLAY:
            if ((v >= 0.5f) != (GXPC_OverlayVisible() != 0)) GXPC_OverlayToggle();
            return;
        }
        postApply(post);
    });
    return ok;
}

template <int ID>
int rowAvailable() { return available(ID); }
template <int ID>
int rowCount() { return int(choices(ID).size()); }
template <int ID>
const char* rowValueName(int value) {
    const std::vector<Choice>& c = choices(ID);
    return value >= 0 && value < int(c.size()) ? c[size_t(value)].name : "";
}
template <int ID>
int rowGet() { return get(ID); }
template <int ID>
int rowSet(int value) { return set(ID, value); }

template <int ID>
PortSetting row(const char* id, int page, int order, const char* name, const char* hint) {
    return {id, page, order, name, hint, 0, rowAvailable<ID>, rowCount<ID>, rowValueName<ID>, rowGet<ID>, rowSet<ID>};
}

}  // namespace

extern "C" {

// Hints say what the option does and costs, in the game's font: letters, digits, spaces and . , - ( ) ' only.
const PortSetting* gx_port_settings(int* count) {
    static const PortSetting kTable[] = {
        row<RESOLUTION>("resolution", PORT_SETTINGS_PAGE_GRAPHICS, 10, "Resolution",
                        "Sharper picture, but costs speed and memory. Native is the GameCube's own."),
        row<MSAA>("msaa", PORT_SETTINGS_PAGE_GRAPHICS, 20, "Anti-Aliasing",
                  "Smooths the edges of shapes. Costs speed and memory at high resolutions."),
        row<FXAA>("fxaa", PORT_SETTINGS_PAGE_GRAPHICS, 30, "FXAA",
                  "Smooths the jagged edges left over, such as fences, at a small cost."),
        row<ANISOTROPIC>("anisotropic", PORT_SETTINGS_PAGE_GRAPHICS, 40, "Anisotropic Filtering",
                         "Keeps textures sharp at a slant, such as the ground ahead of Mario."),
        row<SHARPEN>("sharpen", PORT_SETTINGS_PAGE_GRAPHICS, 50, "Sharpening",
                     "Sharpens the picture as it is scaled to the window."),
        row<FILTER>("presentFilter", PORT_SETTINGS_PAGE_GRAPHICS, 60, "Upscaler",
                    "How the picture fits the window. FSR 1 and NIS suit a low resolution."),
        row<FSR_MODE>("fsrMode", PORT_SETTINGS_PAGE_GRAPHICS, 65, "Upscaling Quality",
                      "Picks the drawing resolution for the picture's size. Performance is fastest."),
        row<FULLSCREEN>("fullscreen", PORT_SETTINGS_PAGE_DISPLAY, 10, "Fullscreen",
                        "Fills the whole screen. F11 or Alt and Enter do this too."),
        row<FULLSCREEN_TYPE>("fullscreenMode", PORT_SETTINGS_PAGE_DISPLAY, 20, "Fullscreen Type",
                             "Borderless fills the desktop. Exclusive changes the display's resolution."),
        row<FULLSCREEN_SIZE>("exclusiveResolution", PORT_SETTINGS_PAGE_DISPLAY, 30, "Fullscreen Resolution",
                             "The display's size in exclusive fullscreen. Desktop leaves it as it is."),
        row<MONITOR>("display", PORT_SETTINGS_PAGE_DISPLAY, 40, "Monitor",
                     "Moves the game to another monitor, in fullscreen as well."),
        row<ASPECT>("aspect", PORT_SETTINGS_PAGE_DISPLAY, 60, "Aspect Ratio",
                    "Keep the picture's shape, fill the window, or scale in whole multiples."),
        row<BRIGHTNESS>("brightness", PORT_SETTINGS_PAGE_DISPLAY, 70, "Brightness",
                        "The brightness curve. 100 leaves the picture as the game draws it."),
        row<VSYNC>("vsync", PORT_SETTINGS_PAGE_DISPLAY, 90, "V-Sync",
                   "Waits for the display. No tearing, but it can hold the frame rate down."),
        row<OVERLAY>("overlay", PORT_SETTINGS_PAGE_DISPLAY, 100, "Performance Overlay",
                     "Shows the frame rate and where the time goes. The backtick key does this too."),
        row<MOUSE_LOOK>("mouseCamera", PORT_SETTINGS_PAGE_GAMEPLAY, 80, "Mouse Look",
                        "Turns the camera with the mouse. F10 frees it, a click takes it back."),
    };
    *count = int(sizeof kTable / sizeof kTable[0]);
    return kTable;
}

void port_gfx_describe(char* out, int size) {
    if (size <= 0) return;
    if (!rendererReady()) {
        out[0] = 0;
        return;
    }
    int w = 0, h = 0;
    rendererEfbSize(&w, &h);
    const int samples = rendererMsaa();
    if (samples) snprintf(out, size_t(size), "Drawing at %d x %d with %dx MSAA", w, h, samples);
    else snprintf(out, size_t(size), "Drawing at %d x %d", w, h);
}

}  // extern "C"
