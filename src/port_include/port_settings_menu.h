#ifndef PORT_SETTINGS_MENU_H
#define PORT_SETTINGS_MENU_H

// The settings menu the game draws in its pause menu
// (src/settings_menu.cpp). Y in the pause menu opens it (patch
// zzzz-ui-settings-01), which gives it the pad while it is open and draws it over
// itself. What it changes is port_settings.h's.

class TMarioGamePad;
namespace JDrama {
struct TRect;
}

bool SettingsMenuIsOpen();
void SettingsMenuOpen();
void SettingsMenuClose();
// Once a frame while it is open: the pad's presses and the animations.
void SettingsMenuMove(TMarioGamePad* pad);
// The pause menu's fade, as it draws itself; the menu fades with it.
void SettingsMenuSetAlpha(u8 alpha);
// Last in the frame (TApplication::gameLoop, after the HUD and the fader), so
// nothing of the paused game draws over it; screen is the 2D area.
void SettingsMenuDraw(const JDrama::TRect& screen);

#endif
