// Port: the settings menu, drawn by the game in its pause menu (see
// port_settings.h for what it changes). Y there opens it: first the pages of
// the launcher's settings (Graphics, Display, Gameplay, Controls), then on a
// page its options as rows on the HUD's brushstrokes, the selection green and
// bouncing as the pause menu's options do, with the pointing hand beside it.
// Up and down choose, left and right (or A) change an option or open a page, B
// goes back a level and, from the pages, to the pause menu. The changes show at
// once, behind the menu. Started by SMS Launcher, they are saved for it to keep
// (port_settings_set); otherwise they last until the game closes.
//
// It draws as the save state menu does, with the game's system font and the
// HUD's art (port_menu_draw.h), and plays the pause menu's own sounds.
//
// Built into the game library (CMakeLists.txt), so its statics are game
// globals: an exact load of a save state puts them back too, as the menu was
// when the state was saved, closed.
#include <JSystem/JDrama/JDRGraphics.hpp>
#include <JSystem/JDrama/JDRRect.hpp>
#include <JSystem/J2D/J2DOrthoGraph.hpp>
#include <MSound/MSound.hpp>
#include <MSound/MSoundSE.hpp>
#include <System/MarioGamePad.hpp>
#include <dolphin/gx.h>
#include <math.h>
#include <stdio.h>

#include "port_framerate.h"
#include "port_menu_draw.h"
#include "port_settings.h"
#include "port_settings_menu.h"

// The GX layer's: the settings menu has the mouse (mouse look lets go of it) while it is open.
extern "C" void GXPC_SettingsMenuOpen(int open);

namespace {

using namespace PortMenu;

// layout, in the pause menu's 2D units (the viewport, 640 wide)
const int kVisibleRows = 7;
const f32 kRowX        = 62.0f;
const f32 kRowW        = 516.0f;
const f32 kListTop     = 58.0f;
const f32 kRowH        = 42.0f; // the bars are kBarH tall: the rest is the space between them
const f32 kBarH        = 34.0f;
const f32 kValueFromRight = 112.0f; // the value's centre, from the row's right edge
const f32 kArrowSpread    = 74.0f;  // each value arrow's distance from it

const u32 kSelectedRow = 0x28AA28FF; // the HUD counters' green
const u32 kOtherRow    = 0x6E28A0FF; // the pause menu's stage panel purple
const u32 kWhite       = 0xFFFFFFFF;
const u32 kPale        = 0xE1EBFFFF;
const u32 kDim         = 0xA8A0C8FF;
const u32 kNoteColour  = 0xFFD25AFF;

bool sOpen;
int sPage;               // the page whose options are listed, or -1 for the list of pages
int sPages[PORT_SETTINGS_PAGE_COUNT]; // the pages that have options, in order
int sPageCount;
int sRowCount;           // rows in the list now: the pages, or the page's options
int sSelected;           // the selected row of the list being shown
int sSelectedPage;       // the page row to come back to
f32 sFirst;              // the first row shown, chasing its target
u8 sAlpha;               // the pause menu's fade
u32 sTicks;              // frames open, for the selected row's bounce
char sNote[96];          // what the last change did, when it did not go as asked
bool sPreview;           // X is held: the menu steps aside to show the picture
Art sArt;

bool onPages() { return sPage < 0; }

const PortSetting* optionAt(int row) { return port_settings_option(sPage, row); }

// The option the selection is on, so that it stays on it when rows above it come or go.
const PortSetting* sCurrent;
void remember() { sCurrent = onPages() ? NULL : optionAt(sSelected); }

// The rows change with the settings (an option shows only while it applies): count them again,
// keeping the selection on the option it was on when rows above it come or go.
void recount()
{
	const PortSetting* selected = onPages() ? NULL : sCurrent;
	if (onPages()) {
		sPageCount = 0;
		for (int page = 0; page < PORT_SETTINGS_PAGE_COUNT; page++)
			if (port_settings_page_available(page))
				sPages[sPageCount++] = page;
		sRowCount = sPageCount;
	} else {
		sRowCount = port_settings_count(sPage);
		for (int row = 0; selected && row < sRowCount; row++)
			if (optionAt(row) == selected) {
				sSelected = row;
				break;
			}
	}
	if (sSelected >= sRowCount)
		sSelected = sRowCount > 0 ? sRowCount - 1 : 0;
	remember();
}

int listTarget()
{
	int first = sSelected - kVisibleRows / 2;
	if (first > sRowCount - kVisibleRows)
		first = sRowCount - kVisibleRows;
	return first < 0 ? 0 : first;
}

void select(int row)
{
	if (sRowCount <= 0)
		return;
	row = (row + sRowCount) % sRowCount;
	if (row == sSelected)
		return;
	sSelected = row;
	sNote[0]  = 0;
	sTicks    = 0;
	remember();
	sound(MSD_SE_SY_CURSOR);
}

void openPage(int row)
{
	sSelectedPage = row;
	sPage         = sPages[row];
	sSelected     = 0;
	sCurrent      = NULL;
	sFirst        = 0.0f;
	sNote[0]      = 0;
	sTicks        = 0;
	recount();
	sound(MSD_SE_SY_DECIDE);
}

void backToPages()
{
	sPage     = -1;
	sSelected = sSelectedPage;
	sNote[0]  = 0;
	sTicks    = 0;
	recount();
	sFirst = (f32)listTarget();
	sound(MSD_SE_SY_2D_OUT);
}

// Steps the selected option's value; with wrap, past the last one is the first.
void change(int delta, bool wrap)
{
	const PortSetting* option = optionAt(sSelected);
	if (!option)
		return;
	const int count = option->count();
	if (count <= 0)
		return;
	const int now   = option->get();
	int next        = now + delta;
	if (wrap)
		next = (next + count) % count;
	else if (next < 0 || next >= count)
		return;
	sNote[0] = 0;
	if (next == now)
		return;
	if (port_settings_set(option, next)) {
		sound(MSD_SE_SY_DECIDE);
	} else {
		snprintf(sNote, sizeof sNote, "This computer cannot do that");
		sound(MSD_SE_SY_CANCEL_COMMON);
	}
	recount(); // another option may now apply, or not
}

// The arrow beside a value, pointing the way the value would change, with the text's drop shadow.
void arrow(f32 x, f32 y, bool right, u32 colour, u8 alpha)
{
	triangle(x + 1.5f, y + kBarH / 2 + 1.5f, 9.0f, 13.0f, right, 0x000000FF, alpha / 2);
	triangle(x, y + kBarH / 2, 9.0f, 13.0f, right, colour, alpha);
}

void drawRow(int index, f32 y, u8 alpha)
{
	const bool selected = index == sSelected;
	// the selected row breathes as the pause menu's options do
	const f32 pulse = selected ? 1.0f + 0.025f * sinf(sTicks * port_frame_scale() * 0.2f) : 1.0f;
	art(sArt.brush, kRowX + kRowW / 2, y + kBarH / 2, kRowW * pulse, kBarH * pulse, 0.0f,
	    selected ? kSelectedRow : kOtherRow, (u8)(alpha * (selected ? 0.95f : 0.72f)));
	const f32 textTop = y + (selected ? 7.0f : 8.0f);
	const u32 arrowColour = selected ? kWhite : kPale;
	if (onPages()) {
		text(port_settings_page_name(sPages[index]), kRowX + 30.0f, textTop, selected ? 16.0f : 14.0f, kWhite, LEFT, alpha);
		arrow(kRowX + kRowW - 40.0f, y, true, arrowColour, alpha);
	} else {
		const PortSetting* option = optionAt(index);
		if (!option)
			return;
		text(option->name, kRowX + 30.0f, textTop, selected ? 16.0f : 14.0f, kWhite, LEFT, alpha);
		const int count = option->count();
		const int value = option->get();
		const f32 centre = kRowX + kRowW - kValueFromRight;
		text(option->valueName(value), centre, y + 8.0f, 14.0f, kWhite, CENTRE, alpha);
		if (value > 0)
			arrow(centre - kArrowSpread, y, false, arrowColour, alpha);
		if (value < count - 1)
			arrow(centre + kArrowSpread, y, true, arrowColour, alpha);
	}
	// the glove's finger points up in its texture: turned clockwise it points right, at the option
	if (selected)
		art(sArt.cursor, kRowX - 4.0f, y + kBarH / 2, 24.0f * 32 / 46, 24.0f, -90.0f, kWhite, alpha);
}

} // namespace

bool SettingsMenuIsOpen() { return sOpen; }

void SettingsMenuOpen()
{
	if (sOpen)
		return;
	if (!loadArt(sArt))
		return;
	sPage = -1;
	recount();
	if (!sRowCount)
		return;
	sSelected     = 0;
	sSelectedPage = 0;
	sFirst        = 0.0f;
	sTicks        = 0;
	sNote[0]      = 0;
	sPreview      = false;
	sOpen         = true;
	GXPC_SettingsMenuOpen(1);
	sound(MSD_SE_SY_2D_IN);
}

void SettingsMenuClose()
{
	if (sOpen)
		GXPC_SettingsMenuOpen(0);
	sOpen = false;
}

void SettingsMenuMove(TMarioGamePad* pad)
{
	if (!sOpen)
		return;
	++sTicks;
	if (!onPages())
		recount(); // an option can come or go meanwhile (HDR as the window moves, a monitor plugged in)
	sPreview = (pad->getButton() & JUTGamePad::X) != 0;
	if (sPreview)
		return;
	if (pad->checkFrameMeaning(0x2)) {
		select(sSelected - 1);
	} else if (pad->checkFrameMeaning(0x4)) {
		select(sSelected + 1);
	} else if (pad->checkFrameMeaning(0x8)) {
		if (!onPages())
			change(-1, false);
	} else if (pad->checkFrameMeaning(0x10)) {
		if (onPages())
			openPage(sSelected);
		else
			change(1, false);
	} else if (pad->checkFrameMeaning(0x20)) {
		if (onPages())
			openPage(sSelected);
		else
			change(1, true);
	}
	if (pad->checkFrameMeaning(0x40)) {
		if (onPages()) {
			sound(MSD_SE_SY_2D_OUT);
			SettingsMenuClose();
		} else {
			backToPages();
		}
		return;
	}
	sFirst += (listTarget() - sFirst) * port_frame_chase(0.3f);
}

void SettingsMenuSetAlpha(u8 alpha) { sAlpha = alpha; }

void SettingsMenuDraw(const JDrama::TRect& screen)
{
	// The menu's state is the game's (see the top): after a save state's load, the window's idea of
	// whether the menu is open (the mouse it frees) follows it here.
	GXPC_SettingsMenuOpen(sOpen ? 1 : 0);
	const u8 alpha = sAlpha;
	if (!sOpen || !alpha || sPreview)
		return;
	const f32 width = screen.getWidth(), height = screen.getHeight();
	J2DOrthoGraph graph(screen);
	graph.setPort();
	graph.setup2D();

	// the paused game behind, its HUD and pause menu too, darker
	fill(width / 2, height / 2, width, height, 0.0f, 0x000014A8, alpha);

	char heading[48];
	if (onPages())
		snprintf(heading, sizeof heading, "SETTINGS");
	else
		snprintf(heading, sizeof heading, "%s", port_settings_page_name(sPage));
	for (char* c = heading; *c; c++)
		if (*c >= 'a' && *c <= 'z')
			*c -= 'a' - 'A';
	text(heading, 28.0f, 14.0f, 22.0f, kWhite, LEFT, alpha);
	char position[24];
	snprintf(position, sizeof position, "%d of %d", sSelected + 1, sRowCount);
	text(position, width - 28.0f, 20.0f, 12.0f, kPale, RIGHT, alpha);

	// the rows near the selection, sliding as it moves
	const int first = (int)sFirst;
	for (int i = first - 1; i <= first + kVisibleRows; i++) {
		if (i < 0 || i >= sRowCount)
			continue;
		const f32 y = kListTop + (i - sFirst) * kRowH;
		if (y < kListTop - kRowH * 0.5f || y > kListTop + kRowH * (kVisibleRows - 0.5f))
			continue;
		drawRow(i, y, alpha);
	}

	// what the option does, or what went wrong; then, for the picture, how it is drawn now
	const PortSetting* option = onPages() ? nullptr : optionAt(sSelected);
	const char* note = sNote[0] ? sNote : option ? option->hint : "";
	text(note, width / 2, height - 98.0f, 12.5f, sNote[0] ? kNoteColour : kPale, CENTRE, alpha);
	if (!onPages() && (sPage == PORT_SETTINGS_PAGE_GRAPHICS || sPage == PORT_SETTINGS_PAGE_DISPLAY)) {
		char described[96];
		port_gfx_describe(described, sizeof described);
		text(described, width / 2, height - 76.0f, 12.0f, kDim, CENTRE, alpha);
	}
	text(onPages() ? "@ Open     Hold X Preview     # Back" : "@ Change     Hold X Preview     # Back", width / 2,
	     height - 36.0f, 15.0f, kWhite, CENTRE, alpha);
}
