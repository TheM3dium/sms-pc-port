// Extensions' text in the window (gx_ext_overlay.h), drawn with the debug
// overlay's renderer: the notice ("Connecting to 1.2.3.4...") centred near the
// top, the status panel (who is playing) at the right, and the banner (big
// text: a server mod's, a race's countdown and winner) in the middle. Each is
// one or more lines, wrapped to the window; an empty text hides it. And labels
// on the game's picture: the names over other players. The font has ASCII only.
#include "gx_ext_overlay.h"
#include "gx_internal.h"

#include "sms_gx/gx_pc.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <array>
#include <mutex>
#include <string>
#include <vector>

namespace {

const int kCapH = 7;  // stb_easy_font's capitals are 7 font pixels high
const int kLineH = 12;
const uint8_t kPanelBg[4] = {15, 15, 15, 220};
const uint8_t kText[4] = {255, 255, 255, 255};
const uint8_t kAccent[4] = {250, 190, 40, 255};

struct Panel {
    std::string text;      // set by any thread
    std::string shown;     // what the bitmap holds (GL thread)
    std::vector<uint8_t> pixels;
    int w = 0, h = 0, glyph = 0;
};

std::mutex s_lock;
Panel s_panels[GXPC_EXT_OVERLAY_PANELS];

// The names over players: the list as last set (any thread), and each slot's
// bitmap as drawn (GL thread), made again when its text, size or fade changes.
GXPCLabel s_labels[GXPC_MAX_LABELS];
int s_labelCount = 0;
struct LabelBitmap {
    std::string text;
    int glyph = 0, fade = -1, w = 0, h = 0;
    std::vector<uint8_t> pixels;
};
LabelBitmap s_labelBitmaps[GXPC_MAX_LABELS];
const int kLabelFades = 16;  // fade steps (a bitmap each)

// A server mod's markers and the colour over the window, as last set (any
// thread), and the markers' bitmaps (GL thread).
GXPCMarker s_markers[GXPC_MAX_MARKERS];
int s_markerCount = 0;
struct MarkerBitmap {
    std::string text;
    int glyph = 0, fade = -1, w = 0, h = 0;
    uint32_t rgba = 0;
    std::vector<uint8_t> pixels;
};
MarkerBitmap s_markerBitmaps[GXPC_MAX_MARKERS];
uint32_t s_screen = 0, s_screenShown = 0;

// "#rrggbb " at the start of `line`: its colour into `colour`, and the line without it.
bool lineColour(std::string& line, uint8_t colour[4]) {
    unsigned rgb = 0;
    if (line.size() < 8 || line[0] != '#' || line[7] != ' ' || sscanf(line.c_str() + 1, "%6x", &rgb) != 1) return false;
    colour[0] = uint8_t(rgb >> 16), colour[1] = uint8_t(rgb >> 8), colour[2] = uint8_t(rgb);
    line = line.substr(8);
    return true;
}

// The text's lines, each wrapped at spaces to at most `maxWidth` font pixels.
std::vector<std::string> lines(const std::string& text, int maxWidth) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t nl = text.find('\n', start);
        std::string line = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        while (gx::overlayTextWidth(line.c_str()) > maxWidth) {
            size_t cut = line.size();
            // the last space that leaves a line short enough
            while (cut > 0) {
                cut = line.rfind(' ', cut - 1);
                if (cut == std::string::npos || cut == 0) break;
                if (gx::overlayTextWidth(line.substr(0, cut).c_str()) <= maxWidth) break;
            }
            if (cut == std::string::npos || cut == 0) break;  // one long word: leave it
            out.push_back(line.substr(0, cut));
            line = line.substr(cut + 1);
        }
        out.push_back(line);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

void renderPanel(int index, gx::OverlaySlot slot, int winW, int winH) {
    Panel& p = s_panels[index];
    std::string text;
    {
        std::lock_guard<std::mutex> hold(s_lock);
        text = p.text;
    }
    if (text.empty()) {
        p.shown.clear();
        return;
    }
    const bool banner = index == GXPC_EXT_OVERLAY_BANNER;
    const int glyph = banner ? std::max(4, winH / 110) : std::max(2, (winH + 360) / 720);
    const bool changed = text != p.shown || glyph != p.glyph;
    if (changed) {
        p.shown = text;
        p.glyph = glyph;
        // "#rrggbb ": the text's colour, and a line's own
        uint8_t colour[4] = {kText[0], kText[1], kText[2], kText[3]};
        std::string body = text;
        lineColour(body, colour);
        std::vector<std::string> rows;
        std::vector<std::array<uint8_t, 4>> rowColours;
        for (size_t start = 0;;) {
            const size_t nl = body.find('\n', start);
            std::string line = body.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            uint8_t own[4] = {colour[0], colour[1], colour[2], colour[3]};
            lineColour(line, own);
            for (const std::string& r : lines(line, winW * (banner ? 9 : 7) / 10 / glyph)) {
                rows.push_back(r);
                rowColours.push_back({{own[0], own[1], own[2], own[3]}});
            }
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
        const int pad = (banner ? 3 : 6) * glyph;
        int width = 0;
        for (const std::string& r : rows) width = std::max(width, gx::overlayTextWidth(r.c_str()));
        p.w = width * glyph + 2 * pad;
        p.h = (kCapH + kLineH * int(rows.size() - 1)) * glyph + 2 * pad;
        p.pixels.assign(size_t(p.w) * p.h * 4, 0);
        gx::overlayFillRect(p.pixels, p.w, p.h, 0, 0, p.w, p.h, kPanelBg);
        if (banner)
            gx::overlayFillRect(p.pixels, p.w, p.h, 0, p.h - glyph, p.w, p.h, colour);  // underlined in its colour
        else
            gx::overlayFillRect(p.pixels, p.w, p.h, 0, 0, 2 * glyph, p.h, kAccent);
        for (size_t i = 0; i < rows.size(); i++) {
            // the banner's rows centred, the others' left-aligned
            const int x = banner ? (p.w - gx::overlayTextWidth(rows[i].c_str()) * glyph) / 2 : pad;
            gx::overlayText(p.pixels, p.w, p.h, x, pad + int(i) * kLineH * glyph, rows[i].c_str(), rowColours[i].data(), glyph);
        }
    }
    // the notice centred just below the HUD's top row (the lives), the status
    // panel at the right below the counters, the board at the left across
    // from it (rows count from the top)
    const int margin = winH / 30;
    const int x = index == GXPC_EXT_OVERLAY_STATUS ? winW - p.w - margin
                  : index == GXPC_EXT_OVERLAY_BOARD ? margin
                                                    : (winW - p.w) / 2;
    const int y = banner ? winH * 36 / 100 : index == GXPC_EXT_OVERLAY_NOTICE ? winH * 17 / 100 : winH * 27 / 100;
    gx::drawOverlayPanel(slot, changed ? p.pixels.data() : nullptr, p.w, p.h, x, y, 1, winW, winH);
}

// A name: white on a dark rounded pill, its bottom centre at the label's
// place, fading out by the label's alpha.
void renderLabels(int winW, int winH) {
    GXPCLabel labels[GXPC_MAX_LABELS];
    int count;
    {
        std::lock_guard<std::mutex> hold(s_lock);
        count = s_labelCount;
        memcpy(labels, s_labels, sizeof labels);
    }
    const gx::PresentRect r = gx::lastPresentRect();
    if (r.w <= 0 || r.h <= 0) return;
    const int glyph = std::max(2, (r.h + 270) / 540);  // font pixels: 2 up to 1080 lines, then more
    for (int i = 0; i < count; i++) {
        const GXPCLabel& l = labels[i];
        const int fade = std::max(0, std::min(kLabelFades, int(l.alpha * kLabelFades + 0.5f)));
        if (!fade || !l.text[0]) continue;
        LabelBitmap& b = s_labelBitmaps[i];
        std::string text(l.text, strnlen(l.text, sizeof l.text));
        const bool changed = text != b.text || glyph != b.glyph || fade != b.fade;
        if (changed) {
            b.text = text;
            b.glyph = glyph;
            b.fade = fade;
            const int padX = 4 * glyph, padY = 3 * glyph;
            b.w = gx::overlayTextWidth(text.c_str()) * glyph + 2 * padX;
            b.h = kCapH * glyph + 2 * padY;
            b.pixels.assign(size_t(b.w) * b.h * 4, 0);
            const uint8_t bg[4] = {12, 20, 28, uint8_t(170 * fade / kLabelFades)};
            const uint8_t fg[4] = {255, 255, 255, uint8_t(255 * fade / kLabelFades)};
            // rounded ends: the corners' texels left clear
            const int round = std::min(b.h / 2, 3 * glyph);
            for (int y = 0; y < b.h; y++) {
                const int dy = y < round ? round - y : y >= b.h - round ? y - (b.h - round - 1) : 0;
                const int inset = dy ? round - int(sqrtf(float(round * round - dy * dy)) + 0.5f) : 0;
                gx::overlayFillRect(b.pixels, b.w, b.h, inset, y, b.w - inset, y + 1, bg);
            }
            gx::overlayText(b.pixels, b.w, b.h, padX, padY, text.c_str(), fg, glyph);
        }
        const int x = r.x + int(l.x * float(r.w)) - b.w / 2;
        const int y = r.y + int(l.y * float(r.h)) - b.h;
        if (x + b.w < 0 || y + b.h < 0 || x > winW || y > winH) continue;
        gx::drawOverlayPanel(gx::OverlaySlot(gx::OVERLAY_EXT_LABEL + i), changed ? b.pixels.data() : nullptr, b.w, b.h, x, y,
                             1, winW, winH);
    }
}

// The markers: a pill as the names have, in the marker's colour, or an arrow
// pointing down at the place; the colour over the window under them.
void renderMarkers(int winW, int winH) {
    GXPCMarker markers[GXPC_MAX_MARKERS];
    int count;
    uint32_t screen;
    {
        std::lock_guard<std::mutex> hold(s_lock);
        count = s_markerCount;
        memcpy(markers, s_markers, sizeof markers);
        screen = s_screen;
    }
    if (screen & 0xFF) {
        static uint8_t px[4];
        const bool changed = screen != s_screenShown;
        px[0] = uint8_t(screen >> 24), px[1] = uint8_t(screen >> 16), px[2] = uint8_t(screen >> 8), px[3] = uint8_t(screen);
        s_screenShown = screen;
        gx::drawOverlayPanel(gx::OVERLAY_EXT_SCREEN, changed ? px : nullptr, 1, 1, 0, 0, std::max(winW, winH), winW, winH);
    }
    const gx::PresentRect r = gx::lastPresentRect();
    if (r.w <= 0 || r.h <= 0) return;
    const int glyph = std::max(2, (r.h + 270) / 540);
    for (int i = 0; i < count; i++) {
        const GXPCMarker& m = markers[i];
        const int fade = std::max(0, std::min(kLabelFades, int(m.alpha * kLabelFades + 0.5f)));
        if (!fade) continue;
        MarkerBitmap& b = s_markerBitmaps[i];
        std::string text(m.text, strnlen(m.text, sizeof m.text));
        const bool changed = text != b.text || glyph != b.glyph || fade != b.fade || m.rgba != b.rgba;
        if (changed) {
            b.text = text;
            b.glyph = glyph;
            b.fade = fade;
            b.rgba = m.rgba;
            const uint8_t a = uint8_t(((m.rgba & 0xFF) ? (m.rgba & 0xFF) : 255) * fade / kLabelFades);
            const uint8_t fg[4] = {uint8_t(m.rgba >> 24), uint8_t(m.rgba >> 16), uint8_t(m.rgba >> 8), a};
            if (text.empty()) {
                // an arrow pointing down, outlined dark
                const int side = 9 * glyph;
                b.w = side + 2 * glyph;
                b.h = side + 2 * glyph;
                b.pixels.assign(size_t(b.w) * b.h * 4, 0);
                const uint8_t edge[4] = {12, 20, 28, uint8_t(200 * fade / kLabelFades)};
                for (int y = 0; y < b.h; y++) {
                    const int half = (b.h - y) * b.w / 2 / b.h;
                    gx::overlayFillRect(b.pixels, b.w, b.h, b.w / 2 - half, y, b.w / 2 + half, y + 1, edge);
                    const int inner = half - glyph;
                    if (inner > 0 && y >= glyph && y < b.h - glyph)
                        gx::overlayFillRect(b.pixels, b.w, b.h, b.w / 2 - inner, y, b.w / 2 + inner, y + 1, fg);
                }
            } else {
                const int padX = 4 * glyph, padY = 3 * glyph;
                b.w = gx::overlayTextWidth(text.c_str()) * glyph + 2 * padX;
                b.h = kCapH * glyph + 2 * padY;
                b.pixels.assign(size_t(b.w) * b.h * 4, 0);
                const uint8_t bg[4] = {12, 20, 28, uint8_t(170 * fade / kLabelFades)};
                gx::overlayFillRect(b.pixels, b.w, b.h, 0, 0, b.w, b.h, bg);
                gx::overlayFillRect(b.pixels, b.w, b.h, 0, b.h - glyph, b.w, b.h, fg);  // underlined in its colour
                gx::overlayText(b.pixels, b.w, b.h, padX, padY, text.c_str(), fg, glyph);
            }
        }
        const int x = r.x + int(m.x * float(r.w)) - b.w / 2;
        const int y = r.y + int(m.y * float(r.h)) - b.h;
        if (x + b.w < 0 || y + b.h < 0 || x > winW || y > winH) continue;
        gx::drawOverlayPanel(gx::OverlaySlot(gx::OVERLAY_EXT_MARKER + i), changed ? b.pixels.data() : nullptr, b.w, b.h, x,
                             y, 1, winW, winH);
    }
}

}  // namespace

extern "C" void GXPC_ExtOverlayMarkers(const GXPCMarker* markers, int count) {
    std::lock_guard<std::mutex> hold(s_lock);
    s_markerCount = markers ? std::max(0, std::min(count, GXPC_MAX_MARKERS)) : 0;
    if (s_markerCount) memcpy(s_markers, markers, sizeof(GXPCMarker) * size_t(s_markerCount));
}

extern "C" void GXPC_ExtOverlayScreen(uint32_t rgba) {
    std::lock_guard<std::mutex> hold(s_lock);
    s_screen = rgba;
}

extern "C" void GXPC_ExtOverlayLabels(const GXPCLabel* labels, int count) {
    std::lock_guard<std::mutex> hold(s_lock);
    s_labelCount = labels ? std::max(0, std::min(count, GXPC_MAX_LABELS)) : 0;
    if (s_labelCount) memcpy(s_labels, labels, sizeof(GXPCLabel) * size_t(s_labelCount));
}

extern "C" void GXPC_ExtOverlaySet(int panel, const char* text) {
    if (panel < 0 || panel >= GXPC_EXT_OVERLAY_PANELS) return;
    std::lock_guard<std::mutex> hold(s_lock);
    s_panels[panel].text = text ? text : "";
}

namespace gx {

void extOverlayRender(int winW, int winH) {
    if (winW <= 0 || winH <= 0) return;
    renderMarkers(winW, winH);  // the colour over the window first: the text stays over it
    renderLabels(winW, winH);
    renderPanel(GXPC_EXT_OVERLAY_NOTICE, OVERLAY_EXT_NOTICE, winW, winH);
    renderPanel(GXPC_EXT_OVERLAY_STATUS, OVERLAY_EXT_STATUS, winW, winH);
    renderPanel(GXPC_EXT_OVERLAY_BANNER, OVERLAY_EXT_BANNER, winW, winH);
    renderPanel(GXPC_EXT_OVERLAY_BOARD, OVERLAY_EXT_BOARD, winW, winH);
}

}  // namespace gx
