// Port: the save state menu, drawn by the game in its pause menu (save
// states, port_savestate.h). L1 + R1 there opens it: the states as a list on
// the HUD's brushstrokes, beside a stack of their pictures that scrolls as
// the list does, the selection in front, the states before and after it
// behind, above and below. Both show one selection in one list
// (port_savestate_scan), which up and down (or left and right) step through.
// A loads, Y deletes (pressed twice), B goes back to the pause menu.
//
// Everything it draws is the game's: the system font (its button glyphs swap
// for the controller in use, patch zzz-prompts-01), the HUD's art from
// game_6 (the brushstroke behind its counters, the Shine, coin and life
// icons, the counter digits, the pointing hand), drawn as the font draws its
// glyphs, in GX quads, and the worlds' names as the game's own art: the file
// select's lettering in the list, the episode select's banner under the
// pictures (port_savestate_world_art). The pictures are the states' own, as
// GX textures the port decodes (port_savestate_picture).
//
// Built into the game library (CMakeLists.txt), so its statics are game
// globals: an exact load puts them back too, as the menu was when the state
// was saved, closed.
#include <JSystem/J2D/J2DOrthoGraph.hpp>
#include <JSystem/JDrama/JDRGraphics.hpp>
#include <JSystem/JDrama/JDRRect.hpp>
#include <JSystem/JKernel/JKRFileLoader.hpp>
#include <JSystem/JUtility/JUTResFont.hpp>
#include <JSystem/JUtility/JUTTexture.hpp>
#include <MSound/MSound.hpp>
#include <MSound/MSoundSE.hpp>
#include <System/Application.hpp>
#include <System/MarioGamePad.hpp>
#include <System/StageUtil.hpp>
#include <dolphin/gx.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "port_framerate.h"
#include "port_savestate.h"
#include "port_menu_draw.h"
#include "port_savestate_menu.h"

namespace {

// layout, in the pause menu's 2D units (the viewport, 640 wide)
const int kListRows    = 5;
const f32 kListX       = 26.0f;
const f32 kListTop     = 62.0f;
const f32 kRowW        = 282.0f;
const f32 kRowH        = 64.0f;
const f32 kPictureFromRight = 162.0f; // the front picture's centre, from the right edge
const f32 kPictureY    = 168.0f;
const f32 kPictureW    = 220.0f;
const f32 kStackStep   = 74.0f;  // how far above and below the front the neighbours sit
const f32 kTextSize    = 14.0f;

const u32 kSelectedRow = 0x28AA28FF; // the HUD counters' green
const u32 kOtherRow    = 0x6E28A0FF; // the pause menu's stage panel purple
const u32 kWhite       = 0xFFFFFFFF;
const u32 kPale        = 0xE1EBFFFF;
const u32 kNoteColour  = 0xFFD25AFF;
const u32 kFrame       = 0xFFF8D2FF; // the front picture's photo border
const u32 kOtherFrame  = 0xFFFFFFFF;

bool sOpen;
int sCount;
int sSelected;
f32 sShown;      // the picture stack's position, chasing sSelected
f32 sListFirst;  // the list's first row shown, chasing its target
bool sDeleteArmed;
u8 sAlpha;       // the pause menu's fade
u32 sTicks;      // frames open, for the selected row's bounce
char sNote[96];

using namespace PortMenu;

// the HUD's art, from game_6 (loaded with every stage)
Art sArt;
bool sHaveArt;

// A state's picture with a photo border, tilted, with its shadow.
void picture(int index, int slot, f32 cx, f32 cy, f32 w, f32 degrees, bool front, u8 alpha)
{
	int tw = 0, th = 0;
	const void* data = port_savestate_picture(index, slot, &tw, &th);
	const f32 h   = data ? w * th / tw : w * 0.7f;
	const f32 pad = front ? 6.0f : 4.0f;
	fill(cx + 4.0f, cy + 6.0f, w + 2 * pad, h + 2 * pad, degrees, 0x00000078, alpha);
	fill(cx, cy, w + 2 * pad, h + 2 * pad, degrees, front ? kFrame : kOtherFrame, alpha);
	if (!data) {
		fill(cx, cy, w, h, degrees, 0x203040FF, alpha);
		return;
	}
	GXTexObj obj;
	GXInitTexObj(&obj, (void*)data, tw, th, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GXInitTexObjLOD(&obj, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
	GXLoadTexObj(&obj, GX_TEXMAP0);
	setupTextured();
	quad(cx, cy, w, h, degrees, kWhite, alpha, true);
}

// The state's world as SMS_getShineStage numbers them (0 the Airstrip to 9
// Corona Mountain), -1 for an area outside retail's table (a code mod's).
int worldOf(const PortSaveStateInfo* st)
{
	if (st->area < 0 || st->area >= 0x3D)
		return -1;
	const int world = SMS_getShineStage((u8)st->area);
	return world <= 9 ? world : -1;
}

// The world's name as the game's art (banner: the episode select's, else the
// file select's lettering), h tall, centred on y, from x as align says; its
// width, or 0 when there is none to draw (the caller writes the name).
f32 worldName(const PortSaveStateInfo* st, bool banner, f32 x, f32 y, f32 h, int align, u8 alpha)
{
	int tw = 0, th = 0, artW = 0;
	const int world      = worldOf(st);
	const void* data     = world < 0 ? nullptr : port_savestate_world_art(world, banner, &tw, &th, &artW);
	if (!data)
		return 0.0f;
	const f32 scale = h / th, w = tw * scale, shown = artW * scale;
	const f32 left = align == CENTRE ? x - shown / 2 : align == RIGHT ? x - shown : x;
	GXTexObj obj;
	GXInitTexObj(&obj, (void*)data, tw, th, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GXInitTexObjLOD(&obj, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
	GXLoadTexObj(&obj, GX_TEXMAP0);
	setupTextured();
	quad(left + w / 2, y, w, h, 0.0f, kWhite, alpha, true);
	return shown;
}


void selectState(int index)
{
	if (sCount == 0)
		return;
	index = index < 0 ? 0 : index >= sCount ? sCount - 1 : index;
	if (index == sSelected)
		return;
	sSelected    = index;
	sDeleteArmed = false;
	sNote[0]     = 0;
	sTicks       = 0;
	sound(MSD_SE_SY_CURSOR);
}

int listTarget()
{
	int first = sSelected - kListRows / 2;
	if (first > sCount - kListRows)
		first = sCount - kListRows;
	return first < 0 ? 0 : first;
}

void drawRow(int index, f32 y, u8 alpha)
{
	const PortSaveStateInfo* st = port_savestate_scanned(index);
	if (!st)
		return;
	const bool selected = index == sSelected;
	// the selected row breathes as the pause menu's options do
	const f32 pulse = selected ? 1.0f + 0.025f * sinf(sTicks * port_frame_scale() * 0.2f) : 1.0f;
	art(sArt.brush, kListX + kRowW / 2, y + 23.0f, kRowW * pulse, 58.0f * pulse, 0.0f,
	    selected ? kSelectedRow : kOtherRow, (u8)(alpha * (selected ? 0.95f : 0.72f)));
	// the world's name in the file select's lettering, else in the font
	if (worldName(st, false, kListX + 20.0f, y + 9.0f, selected ? 17.0f : 15.0f, LEFT, alpha) == 0.0f)
		text(st->stageName[0] ? st->stageName : "SAVE STATE", kListX + 20.0f, y + 1.0f, selected ? 15.0f : 13.0f,
		     kWhite, LEFT, alpha);
	text(st->when, kListX + 20.0f, y + 21.0f, 9.5f, kPale, LEFT, alpha);
	if (st->hasCounts) {
		f32 x = kListX + kRowW - 128.0f;
		x += icon(sArt.shine, x, y + 25.0f, 17.0f, alpha) + 1.0f;
		x += count(sArt, st->shines, x, y + 26.0f, 13.0f, alpha) + 9.0f;
		x += icon(sArt.blueCoin, x, y + 25.0f, 17.0f, alpha) + 1.0f;
		count(sArt, st->blueCoins, x, y + 26.0f, 13.0f, alpha);
	}
	// the glove's finger points up in its texture: turned clockwise, it points
	// right, at the row
	if (selected)
		art(sArt.cursor, kListX - 2.0f, y + 18.0f, 24.0f * 32 / 46, 24.0f, -90.0f, kWhite, alpha);
}

} // namespace

bool SaveStateMenuIsOpen() { return sOpen; }

void SaveStateMenuOpen()
{
	if (sOpen)
		return;
	sHaveArt = loadArt(sArt);
	if (!sHaveArt)
		return;
	sCount       = port_savestate_scan();
	sSelected    = 0;
	sShown       = 0.0f;
	sListFirst   = 0.0f;
	sDeleteArmed = false;
	sTicks       = 0;
	sNote[0]     = 0;
	sOpen        = true;
	sound(MSD_SE_SY_2D_IN);
}

void SaveStateMenuClose()
{
	sOpen = false;
}

void SaveStateMenuMove(TMarioGamePad* pad)
{
	if (!sOpen)
		return;
	++sTicks;
	// two ways through one list: up and down (the list), left and right (the pictures)
	if (pad->checkFrameMeaning(0x2) || pad->checkFrameMeaning(0x8))
		selectState(sSelected - 1);
	else if (pad->checkFrameMeaning(0x4) || pad->checkFrameMeaning(0x10))
		selectState(sSelected + 1);

	if (pad->checkFrameMeaning(0x40)) {
		sound(MSD_SE_SY_2D_OUT);
		sOpen = false;
		return;
	}
	const PortSaveStateInfo* st = port_savestate_scanned(sSelected);
	if (pad->checkFrameMeaning(0x20) && st) {
		if (!st->sameGame) {
			snprintf(sNote, sizeof sNote, "This state is from another game");
		} else {
			sound(MSD_SE_SY_DECIDE);
			port_savestate_request_load(st->path);
			sOpen = false;
			return;
		}
	}
	if (pad->testTrigger(JUTGamePad::Y) && st) {
		if (!sDeleteArmed) {
			sDeleteArmed = true;
			snprintf(sNote, sizeof sNote, "Press \xA5 again to delete this state");
		} else {
			sDeleteArmed = false;
			snprintf(sNote, sizeof sNote, port_savestate_delete(st->path) ? "State deleted" : "Could not delete the state");
			sound(MSD_SE_SY_CANCEL_COMMON);
			sCount = port_savestate_scan();
			if (sSelected >= sCount)
				sSelected = sCount - 1;
			if (sSelected < 0)
				sSelected = 0;
		}
	}
	// the pictures and the list follow the selection
	const f32 chase = port_frame_chase(0.3f);
	sShown += (sSelected - sShown) * chase;
	sListFirst += (listTarget() - sListFirst) * chase;
}

void SaveStateMenuSetAlpha(u8 alpha) { sAlpha = alpha; }

void SaveStateMenuDraw(const JDrama::TRect& screen)
{
	const u8 alpha = sAlpha;
	if (!sOpen || !alpha)
		return;
	const f32 width = screen.getWidth(), height = screen.getHeight();
	J2DOrthoGraph graph(screen);
	graph.setPort();
	graph.setup2D();

	// the paused game behind, its HUD and pause menu too, darker
	fill(width / 2, height / 2, width, height, 0.0f, 0x000014C8, alpha);

	// the heading, with the buttons that opened it
	f32 x = 28.0f + text("LOAD SAVE STATE", 28.0f, 14.0f, 22.0f, kWhite, LEFT, alpha) + 12.0f;
	PortSaveStateHint hint;
	port_savestate_hint(&hint);
	for (int i = 0; i < hint.count; i++) {
		gpSystemFont->setGX();
		gpSystemFont->setCharColor(JUtility::TColor(255, 255, 255, alpha));
		if (hint.hasImage[i]) {
			gpSystemFont->drawChar_scale(x, 40.0f, 26.0f, 26.0f, PORT_PROMPT_SAVESTATE + i, false);
			x += 30.0f;
		} else {
			x += text(hint.label[i], x, 18.0f, 14.0f, kPale, LEFT, alpha) + 8.0f;
		}
	}

	if (sCount == 0) {
		text("No save states yet", width / 2, 180.0f, 20.0f, kWhite, CENTRE, alpha);
		text("Save one in a level with the buttons above", width / 2, 210.0f, 13.0f, kPale, CENTRE, alpha);
		text("# Back", width / 2, height - 36.0f, 15.0f, kWhite, CENTRE, alpha);
		return;
	}

	// the list: the rows near the selection, sliding as it moves
	const int first = (int)sListFirst;
	for (int i = first - 1; i <= first + kListRows; i++) {
		if (i < 0 || i >= sCount)
			continue;
		const f32 y = kListTop + (i - sListFirst) * kRowH;
		if (y < kListTop - kRowH * 0.5f || y > kListTop + kRowH * (kListRows - 0.5f))
			continue;
		drawRow(i, y, alpha);
	}

	const f32 kPictureX = width - kPictureFromRight;
	// the pictures, scrolling as the list does: the selection in front, the
	// states before and after it behind, above and below, where the stack
	// has slid to (sShown); the farthest first
	const int nearIndex = (int)floorf(sShown + 0.5f);
	const int order[5] = { nearIndex - 2, nearIndex + 2, nearIndex - 1, nearIndex + 1, nearIndex };
	for (int k = 0; k < 5; k++) {
		const int i = order[k];
		if (i < 0 || i >= sCount)
			continue;
		const f32 off   = i - sShown;
		if (fabsf(off) > 1.6f)
			continue;
		const f32 apart = off < -1.0f ? -1.0f : off > 1.0f ? 1.0f : off;
		const f32 scale = 1.0f - 0.26f * fabsf(apart);
		picture(i, (i % PORT_SAVESTATE_PICTURE_SLOTS + PORT_SAVESTATE_PICTURE_SLOTS) % PORT_SAVESTATE_PICTURE_SLOTS,
		        kPictureX + fabsf(apart) * 12.0f, kPictureY + off * kStackStep, kPictureW * scale, apart * 5.0f,
		        k == 4, (u8)(alpha * (fabsf(off) > 1.0f ? 2.0f - fabsf(off) : 1.0f)));
	}

	// about the selection
	const PortSaveStateInfo* st = port_savestate_scanned(sSelected);
	if (st) {
		// the world's banner from the episode select, else its name
		if (worldName(st, true, kPictureX, 316.0f, 24.0f, CENTRE, alpha) == 0.0f)
			text(st->stageName, kPictureX, 304.0f, 20.0f, kWhite, CENTRE, alpha);
		if (st->episodeName[0])
			text(st->episodeName, kPictureX, 332.0f, 12.5f, kWhite, CENTRE, alpha);
		if (st->hasCounts) {
			f32 cx = kPictureX - 86.0f;
			cx += icon(sArt.coin, cx, 362.0f, 20.0f, alpha) + 2.0f;
			cx += count(sArt, st->coins, cx, 363.0f, 16.0f, alpha) + 22.0f;
			cx += icon(sArt.life, cx, 362.0f, 20.0f, alpha) + 2.0f;
			count(sArt, st->lives, cx, 363.0f, 16.0f, alpha);
		}
		char position[24];
		snprintf(position, sizeof position, "%d of %d", sSelected + 1, sCount);
		text(position, width - 28.0f, 20.0f, 12.0f, kPale, RIGHT, alpha);
	}

	// a note, then the buttons (the font's button glyphs, as the game writes them)
	const char* note = sNote;
	if (!note[0] && st && st->sameGame && !st->exact)
		note = "Saved by another version: loads Mario's place and progress";
	if (note[0])
		text(note, width / 2, height - 62.0f, 12.0f, kNoteColour, CENTRE, alpha);
	text("@ Load     \xA5 Delete     # Back", width / 2, height - 36.0f, 15.0f, kWhite, CENTRE, alpha);
}
