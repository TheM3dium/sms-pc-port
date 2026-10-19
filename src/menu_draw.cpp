// Port: what the port's own menus draw with (port_menu_draw.h). Moved from the
// save state menu unchanged, so that the settings menu looks the same.
#include <JSystem/JKernel/JKRFileLoader.hpp>
#include <JSystem/JUtility/JUTResFont.hpp>
#include <MSound/MSound.hpp>
#include <MSound/MSoundSE.hpp>
#include <System/Application.hpp>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "port_menu_draw.h"

namespace PortMenu {

static bool loadTexture(JUTTexture& tex, const char* name)
{
	char path[64];
	snprintf(path, sizeof path, "/game_6/timg/%s.bti", name);
	const ResTIMG* timg = (const ResTIMG*)JKRGetResource(path);
	if (!timg)
		return false;
	tex.storeTIMG(timg);
	return true;
}

bool loadArt(Art& a)
{
	bool ok = loadTexture(a.brush, "coin_back") && loadTexture(a.shine, "shine_icon")
	          && loadTexture(a.blueCoin, "bluecoin_icon") && loadTexture(a.coin, "coin_icon")
	          && loadTexture(a.life, "mario_icon") && loadTexture(a.cursor, "cursor")
	          && loadTexture(a.cross, "coin_number_x");
	for (int i = 0; i < 10 && ok; i++) {
		char name[24];
		snprintf(name, sizeof name, "coin_number_%d", i);
		ok = loadTexture(a.digit[i], name);
	}
	return ok;
}

void colourOf(u32 c, u8 alpha, GXColor* out)
{
	out->r = c >> 24;
	out->g = c >> 16;
	out->b = c >> 8;
	out->a = (u8)(((c & 0xFF) * alpha) / 255);
}

// The corners of a w x h box centred on (cx, cy), turned `degrees`
// anticlockwise as seen, in drawing order.
static void corners(f32 cx, f32 cy, f32 w, f32 h, f32 degrees, f32 out[4][2])
{
	const f32 r = degrees * 3.14159265f / 180.0f;
	const f32 c = cosf(r), s = sinf(r);
	const f32 dx[4] = { -w / 2, w / 2, w / 2, -w / 2 };
	const f32 dy[4] = { -h / 2, -h / 2, h / 2, h / 2 };
	for (int i = 0; i < 4; i++) {
		out[i][0] = cx + dx[i] * c + dy[i] * s;
		out[i][1] = cy - dx[i] * s + dy[i] * c;
	}
}

void setupTextured()
{
	GXSetNumChans(1);
	GXSetNumTevStages(1);
	GXSetNumTexGens(1);
	GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
	GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	GXClearVtxDesc();
	GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
	GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
}

void quad(f32 cx, f32 cy, f32 w, f32 h, f32 degrees, u32 colour, u8 alpha, bool textured)
{
	f32 at[4][2];
	corners(cx, cy, w, h, degrees, at);
	static const f32 kU[4] = { 0, 1, 1, 0 }, kV[4] = { 0, 0, 1, 1 };
	GXColor c;
	colourOf(colour, alpha, &c);
	GXBegin(GX_QUADS, GX_VTXFMT0, 4);
	for (int i = 0; i < 4; i++) {
		GXPosition3f32(at[i][0], at[i][1], 0.0f);
		GXColor4u8(c.r, c.g, c.b, c.a);
		if (textured)
			GXTexCoord2f32(kU[i], kV[i]);
	}
	GXEnd();
}

static void setupFlat()
{
	GXSetNumChans(1);
	GXSetNumTevStages(1);
	GXSetNumTexGens(0);
	GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GXClearVtxDesc();
	GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
	GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
}

void fill(f32 cx, f32 cy, f32 w, f32 h, f32 degrees, u32 colour, u8 alpha)
{
	setupFlat();
	quad(cx, cy, w, h, degrees, colour, alpha, false);
}

void triangle(f32 cx, f32 cy, f32 w, f32 h, bool right, u32 colour, u8 alpha)
{
	setupFlat();
	GXColor c;
	colourOf(colour, alpha, &c);
	// the tip on the side it points to, the base on the other, the box w x h centred on (cx, cy)
	const f32 tip = cx + (right ? w / 2 : -w / 2), base = cx + (right ? -w / 2 : w / 2);
	const f32 at[3][2] = { { base, cy - h / 2 }, { tip, cy }, { base, cy + h / 2 } };
	GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
	for (int i = 0; i < 3; i++) {
		GXPosition3f32(at[i][0], at[i][1], 0.0f);
		GXColor4u8(c.r, c.g, c.b, c.a);
	}
	GXEnd();
}

void art(JUTTexture& tex, f32 cx, f32 cy, f32 w, f32 h, f32 degrees, u32 colour, u8 alpha)
{
	tex.load(GX_TEXMAP0);
	setupTextured();
	quad(cx, cy, w, h, degrees, colour, alpha, true);
}

f32 icon(JUTTexture& tex, f32 x, f32 y, f32 h, u8 alpha)
{
	const ResTIMG* t = tex.getTexInfo();
	const f32 w = t ? h * t->width / t->height : h;
	art(tex, x + w / 2, y, w, h, 0.0f, 0xFFFFFFFF, alpha);
	return w;
}

f32 count(Art& a, int value, f32 x, f32 y, f32 h, u8 alpha)
{
	char digits[16];
	snprintf(digits, sizeof digits, "%d", value < 0 ? 0 : value);
	const f32 step = h * 0.86f;
	art(a.cross, x + h / 2, y, h, h, 0.0f, 0xFFFFFFFF, alpha);
	x += step;
	for (const char* p = digits; *p; p++, x += step)
		art(a.digit[*p - '0'], x + h / 2, y, h, h, 0.0f, 0xFFFFFFFF, alpha);
	return step * (1 + strlen(digits));
}

f32 textWidth(const char* s, f32 size)
{
	JUTFont* font = gpSystemFont;
	f32 w = 0.0f;
	for (; *s; s++)
		w += font->getWidth((u8)*s) * (size / font->getWidth());
	return w;
}

f32 text(const char* s, f32 x, f32 y, f32 size, u32 colour, int align, u8 alpha)
{
	JUTFont* font = gpSystemFont;
	const f32 w = textWidth(s, size);
	if (align == CENTRE)
		x -= w / 2;
	else if (align == RIGHT)
		x -= w;
	const f32 baseline = y + size;
	font->setGX();
	font->setCharColor(JUtility::TColor(0, 0, 0, alpha / 2));
	font->drawString_size_scale(x + size * 0.09f, baseline + size * 0.09f, size, size, s, strlen(s), false);
	GXColor c;
	colourOf(colour, alpha, &c);
	font->setCharColor(JUtility::TColor(c.r, c.g, c.b, c.a));
	font->drawString_size_scale(x, baseline, size, size, s, strlen(s), false);
	return w;
}

void sound(u32 id) { SMSGetMSound()->startSoundSystemSE(id, 0, nullptr, 0); }

} // namespace PortMenu
