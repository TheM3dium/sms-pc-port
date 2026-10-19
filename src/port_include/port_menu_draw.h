#ifndef PORT_MENU_DRAW_H
#define PORT_MENU_DRAW_H

// What the port's own menus draw with (src/menu_draw.cpp): the game's system
// font and the HUD's art from game_6, in GX quads as the font draws its glyphs.
// Used by the save state menu (savestate_menu.cpp) and the settings menu
// (settings_menu.cpp). Coordinates are the 2D area's, 640 wide; colours are
// 0xRRGGBBAA, and alpha scales them (the pause menu's fade).

#include <JSystem/JUtility/JUTTexture.hpp>
#include <dolphin/gx.h>

namespace PortMenu {

enum Align { LEFT, CENTRE, RIGHT };

// The HUD's art from game_6 (loaded with every stage).
struct Art {
	JUTTexture brush;  // the brushstroke behind the HUD's counters
	JUTTexture shine, blueCoin, coin, life;
	JUTTexture cursor; // the pointing hand
	JUTTexture cross;  // the counters' x
	JUTTexture digit[10];
};

// Loads the art; false when game_6 is not there (a menu then stays closed).
bool loadArt(Art& art);

void colourOf(u32 colour, u8 alpha, GXColor* out);
void setupTextured();
void quad(f32 cx, f32 cy, f32 w, f32 h, f32 degrees, u32 colour, u8 alpha, bool textured);
// A w x h box centred on (cx, cy), turned `degrees` anticlockwise as seen.
void fill(f32 cx, f32 cy, f32 w, f32 h, f32 degrees, u32 colour, u8 alpha);
// A solid triangle in the w x h box centred on (cx, cy), its tip on the right or the left.
void triangle(f32 cx, f32 cy, f32 w, f32 h, bool right, u32 colour, u8 alpha);
void art(JUTTexture& tex, f32 cx, f32 cy, f32 w, f32 h, f32 degrees, u32 colour, u8 alpha);
// An icon of height h with its left edge at x, centred on y; its width.
f32 icon(JUTTexture& tex, f32 x, f32 y, f32 h, u8 alpha);
// "x12" in the HUD's counter digits, its left edge at x and centred on y; its width.
f32 count(Art& art, int value, f32 x, f32 y, f32 h, u8 alpha);
f32 textWidth(const char* text, f32 size);
// Text with its top at y, in the system font, with the game's drop shadow; its width.
f32 text(const char* s, f32 x, f32 y, f32 size, u32 colour, int align, u8 alpha);
// A Sunshine menu sound (MSD_SE_SY_*).
void sound(u32 id);

} // namespace PortMenu

#endif
