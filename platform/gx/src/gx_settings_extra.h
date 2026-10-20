// What gx_settings_extra.cpp (the settings menu's screen format, HDR, button prompt and texture
// pack options) asks of the rest of the GX layer. Each is a small accessor beside the state it
// changes.
#ifndef SMS_GX_SETTINGS_EXTRA_H
#define SMS_GX_SETTINGS_EXTRA_H

namespace gx {

// gx_render.cpp. The width over 4:3 the EFB is built for (the GL thread, between frames); false,
// with the old frame back, when the new one does not fit. The gameplay HUD's pieces at the screen
// edges apply in widescreen only (the game thread).
bool rendererSetWide(float widthOver43);
void rendererSetHudEdges(bool on);
bool rendererHudEdges();

// gx_hdr.cpp. The HDR picture's look. Paper white and peak are in nits and used with calibration
// off; with it on they are what Windows reports (the auto ones). Contrast, saturation and
// highlights are percent. hdrSetLook is for the GL thread; the next frame draws with it.
struct HdrLook {
    bool calibration = true;
    float paperWhite = 200.0f, peak = 1000.0f;
    float contrast = 100.0f, saturation = 100.0f, highlights = 40.0f;
    float autoPaperWhite = 200.0f, autoPeak = 1000.0f;
};
HdrLook hdrLook();
void hdrSetLook(const HdrLook& look);
// HDR itself, on the game's thread (the one that runs the window, and the one the settings menu's set() runs on),
// between frames. hdrEnable makes what start-up makes with SMS_HDR=1 and presents through Direct3D from the next
// frame; false, with the window as it was, when it cannot. hdrDisable gives it all back and the window swaps by
// itself again (the caller sets the swap interval again). hdrCanEnable: HDR is on, or the window is there and
// Windows HDR is on for the display showing it. It is cheap: the menu asks it for every row, every frame.
bool hdrEnable();
void hdrDisable();
bool hdrCanEnable();

// gx_prompts.cpp. The style is 0 the game's own (GameCube), 1 automatic, 2 Xbox, 3 PlayStation,
// 4 Steam Deck, 5 keyboard; the pad (for automatic) is 0 matched to the controller, then GameCube,
// Xbox, PlayStation, Steam Deck. The setters are false where the style's pictures cannot be read.
bool promptsAvailable();
int promptsStyle();
bool promptsSetStyle(int style);
int promptsPad();
bool promptsSetPad(int pad);

// gx_hires.cpp. Whether the replacements are shown (the pack is found at the start: hiresEnabled),
// and when they are read: 0 as drawn, 1 as a stage loads, 2 the whole pack.
bool hiresActive();
void hiresSetActive(bool on);
// Texture packs were found (not only Mario's look, SMS_MARIO_LOOK, which stays when they are off).
bool hiresHasPack();
int hiresLoadMode();
void hiresSetLoadMode(int mode);

// gx_shader.cpp. The warm-up when a stage loads: available where its tables are loaded.
bool shaderWarmupAvailable();
bool shaderWarmupWanted();
void shaderSetWarmup(bool on);

}  // namespace gx

#endif
