// Players in their own colours (mpg_dress, mp_game_api.h). Mario's clothes,
// skin and hair are colours in his textures, so a player's colours are those
// textures repainted. The launcher sorts the textures' texels into colour
// regions once (src/mario-pack.js, bakePaint) and leaves the result in
// SMS_MARIO_PAINT: paint.txt names the regions (with each one's typical shade
// on the disc) and each texture's .paint file, which holds the picture the
// game shows for it (the HD one where a texture pack has it) and, for every
// texel, its region, the region it blends into at a border and how much, and
// its lightness. Repainting here is the launcher's own (src/mario-colors.js,
// recolor): each region's chosen colour as a ramp over lightness, darker
// texels keeping their share of the region's typical lightness, lighter ones
// moving as far towards white. A player without colours of their own is their
// character (mp_recolour.h) over the same picture, so everyone is drawn at the
// picture's size, and nobody wears this player's own colours (which their
// Mario takes from the launcher's SMS_MARIO_LOOK pack, by the texture's hash).
// FLUDD's texture (H_watergun_*) is painted the same way, from its own
// regions; a player without FLUDD colours has FLUDD's own, whatever their
// character.
#include "../mp_game_api.h"
#include "../mp_recolour.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" void GXPC_SetTextureOverride(const void* data, size_t bytes, const uint8_t* rgba, int w, int h)
    __attribute__((weak));
extern "C" void GXPC_ClearTextureOverrides(void) __attribute__((weak));

namespace {

const int kRegions = 32; // at most (the launcher has 18: Mario's 14, FLUDD's 4)

struct Region {
	std::string id;
	bool known     = false; // the disc has this region
	int ref[3]     = {0, 0, 0};
	double refL    = 0;     // its typical lightness, 0..100
};

struct Picture {
	int w = 0, h = 0;
	std::vector<uint8_t> rgba, mask, second, mix, light;
};

struct Paint {
	bool loaded = false, ok = false;
	std::string dir;
	Region regions[kRegions];
	std::unordered_map<std::string, std::string> files;                  // texture name -> .paint file
	std::unordered_map<std::string, std::shared_ptr<Picture>> pictures;  // by file, read when first needed
	// painted pictures, by file and colours (a session has a few players)
	std::map<std::string, std::shared_ptr<std::vector<uint8_t>>> painted;
};

std::mutex s_lock;
Paint s_paint;

// ------------------------------------------------------------------ colour
// CIE Lab as the launcher computes it (src/mario-colors.js).
double linear(int v)
{
	const double c = (v < 0 ? 0 : v > 255 ? 255 : v) / 255.0;
	return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

void lab(const int rgb[3], double out[3])
{
	const double R = linear(rgb[0]), G = linear(rgb[1]), B = linear(rgb[2]);
	auto f = [](double t) { return t > 0.008856 ? cbrt(t) : 7.787 * t + 16.0 / 116.0; };
	const double x = f((0.4124 * R + 0.3576 * G + 0.1805 * B) / 0.95047), y = f(0.2126 * R + 0.7152 * G + 0.0722 * B);
	const double z = f((0.0193 * R + 0.1192 * G + 0.9505 * B) / 1.08883);
	out[0] = 116 * y - 16;
	out[1] = 500 * (x - y);
	out[2] = 200 * (y - z);
}

void rgbFromLab(const double L[3], uint8_t out[3])
{
	const double fy = (L[0] + 16) / 116, fx = fy + L[1] / 500, fz = fy - L[2] / 200;
	auto inv = [](double t) { return t * t * t > 0.008856 ? t * t * t : (t - 16.0 / 116.0) / 7.787; };
	const double X = inv(fx) * 0.95047, Y = inv(fy), Z = inv(fz) * 1.08883;
	const double lin[3] = {3.2406 * X - 1.5372 * Y - 0.4986 * Z, -0.9689 * X + 1.8758 * Y + 0.0415 * Z,
	                       0.0557 * X - 0.204 * Y + 1.057 * Z};
	for (int c = 0; c < 3; c++) {
		const double v = lin[c] > 0 ? lin[c] : 0;
		const double g = 255 * (v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1 / 2.4) - 0.055);
		const long r   = lround(g);
		out[c]         = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
	}
}

// A region's colour for every original lightness (0..255, as the paint files keep it).
void ramp(const int color[3], double reference, uint8_t out[256 * 3])
{
	double L[3];
	lab(color, L);
	for (int q = 0; q < 256; q++) {
		const double level = q / 2.55;
		double shade[3];
		if (level <= reference) {
			const double k = (level + 10) / (reference + 10);
			shade[0] = L[0] * k, shade[1] = L[1] * k, shade[2] = L[2] * k;
		} else {
			double u = (level - reference) / (100 - reference > 1 ? 100 - reference : 1);
			u        = u < 1 ? u : 1;
			shade[0] = L[0] + (100 - L[0]) * u, shade[1] = L[1] * (1 - u), shade[2] = L[2] * (1 - u);
		}
		rgbFromLab(shade, out + q * 3);
	}
}

bool hexColour(const char* s, size_t n, int out[3])
{
	if (n != 6)
		return false;
	for (size_t i = 0; i < 6; i++)
		if (!isxdigit((unsigned char)s[i]))
			return false;
	for (int c = 0; c < 3; c++) {
		char part[3] = {s[c * 2], s[c * 2 + 1], 0};
		out[c]       = (int)strtol(part, nullptr, 16);
	}
	return true;
}

// ------------------------------------------------------------------ the launcher's files

bool readFile(const std::string& path, std::vector<uint8_t>& out)
{
	FILE* f = fopen(path.c_str(), "rb");
	if (!f)
		return false;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	out.resize(size > 0 ? (size_t)size : 0);
	const bool ok = size > 0 && fread(out.data(), 1, out.size(), f) == out.size();
	fclose(f);
	return ok;
}

void load(Paint& p)
{
	p.loaded       = true;
	const char* dir = getenv("SMS_MARIO_PAINT");
	if (!dir || !*dir)
		return;
	p.dir = dir;
	std::vector<uint8_t> text;
	if (!readFile(p.dir + "/paint.txt", text)) {
		fprintf(stderr, "[multiplayer] no paint.txt in %s: players keep their characters' colours\n", dir);
		return;
	}
	text.push_back(0);
	bool versionOk = false;
	for (char* line = strtok((char*)text.data(), "\n"); line; line = strtok(nullptr, "\n")) {
		char kind[16], a[64], b[64], c[64], d[64];
		const int n = sscanf(line, "%15s %63s %63s %63s %63s", kind, a, b, c, d);
		if (n >= 2 && !strcmp(kind, "sms-paint"))
			versionOk = atoi(a) == 1;
		else if (n == 5 && !strcmp(kind, "region")) {
			const int i = atoi(a);
			if (i < 0 || i >= kRegions)
				continue;
			Region& r = p.regions[i];
			r.id      = b;
			r.known   = hexColour(c, strlen(c), r.ref);
			r.refL    = atof(d);
		} else if (n == 3 && !strcmp(kind, "texture"))
			p.files[a] = b;
	}
	p.ok = versionOk && !p.files.empty();
	if (!p.ok)
		fprintf(stderr, "[multiplayer] %s/paint.txt is not one this game reads: players keep their characters' colours\n", dir);
}

std::shared_ptr<Picture> picture(Paint& p, const std::string& file)
{
	auto it = p.pictures.find(file);
	if (it != p.pictures.end())
		return it->second;
	std::shared_ptr<Picture> pic;
	std::vector<uint8_t> data;
	if (file.find('/') == std::string::npos && file.find('\\') == std::string::npos && readFile(p.dir + "/" + file, data)
	    && data.size() >= 20 && !memcmp(data.data(), "SMSPAINT", 8)) {
		auto u32 = [&](size_t at) {
			return (uint32_t)data[at] | (uint32_t)data[at + 1] << 8 | (uint32_t)data[at + 2] << 16 | (uint32_t)data[at + 3] << 24;
		};
		const uint32_t version = u32(8), w = u32(12), h = u32(16);
		const size_t texels    = (size_t)w * h;
		if (version == 1 && w && h && w <= 4096 && h <= 4096 && data.size() == 20 + texels * 8) {
			pic       = std::make_shared<Picture>();
			pic->w    = (int)w;
			pic->h    = (int)h;
			const uint8_t* at = data.data() + 20;
			pic->rgba.assign(at, at + texels * 4), at += texels * 4;
			pic->mask.assign(at, at + texels), at += texels;
			pic->second.assign(at, at + texels), at += texels;
			pic->mix.assign(at, at + texels), at += texels;
			pic->light.assign(at, at + texels);
		}
	}
	if (!pic)
		fprintf(stderr, "[multiplayer] could not read %s/%s\n", p.dir.c_str(), file.c_str());
	p.pictures[file] = pic;
	return pic;
}

// ------------------------------------------------------------------ painting

// `look`'s colours by region ("hat=2fa846;shirt=..."); false when it has none.
bool chosenColours(const Paint& p, const std::string& look, bool chosen[kRegions], int colours[kRegions][3])
{
	memset(chosen, 0, sizeof(bool) * kRegions);
	bool any = false;
	size_t start = 0;
	while (start < look.size()) {
		size_t end = look.find(';', start);
		if (end == std::string::npos)
			end = look.size();
		const size_t eq = look.find('=', start);
		if (eq != std::string::npos && eq < end) {
			const std::string id = look.substr(start, eq - start);
			for (int i = 0; i < kRegions; i++)
				if (p.regions[i].known && p.regions[i].id == id && hexColour(look.c_str() + eq + 1, end - eq - 1, colours[i]))
					chosen[i] = any = true;
		}
		start = end + 1;
	}
	return any;
}

// FLUDD's texture: characters recolour Mario's clothes, not FLUDD.
bool isFludd(const char* textureName)
{
	return !strncmp(textureName, "H_watergun", 10);
}

std::shared_ptr<std::vector<uint8_t>> paint(const Paint& p, const Picture& pic, const std::string& look, int character)
{
	const size_t texels = (size_t)pic.w * pic.h;
	auto out            = std::make_shared<std::vector<uint8_t>>(pic.rgba);
	uint8_t* px         = out->data();
	bool chosen[kRegions];
	int colours[kRegions][3];
	if (!chosenColours(p, look, chosen, colours)) {
		// their character's colours, over Mario's picture
		if (character != MPG_CHAR_MARIO)
			for (size_t i = 0; i < texels; i++) {
				int r = px[i * 4], g = px[i * 4 + 1], b = px[i * 4 + 2];
				mp_recolour::recolour(character, r, g, b);
				px[i * 4]     = (uint8_t)mp_recolour::clamp255(r);
				px[i * 4 + 1] = (uint8_t)mp_recolour::clamp255(g);
				px[i * 4 + 2] = (uint8_t)mp_recolour::clamp255(b);
			}
		return out;
	}
	// a ramp for each region the disc has: its chosen colour, else its own
	// (for the edges it shares with a chosen one)
	static uint8_t ramps[kRegions][256 * 3];
	bool hasRamp[kRegions];
	for (int i = 0; i < kRegions; i++) {
		hasRamp[i] = p.regions[i].known;
		if (hasRamp[i])
			ramp(chosen[i] ? colours[i] : p.regions[i].ref, p.regions[i].refL, ramps[i]);
	}
	const uint8_t* src = pic.rgba.data();
	for (size_t i = 0; i < texels; i++) {
		const int r1 = pic.mask[i], r2 = pic.second[i];
		const bool c1 = r1 < kRegions && chosen[r1], c2 = r2 < kRegions && chosen[r2];
		if (!c1 && !c2)
			continue;
		const int q   = pic.light[i] * 3;
		const float w = pic.mix[i] / 255.0f;
		for (int c = 0; c < 3; c++) {
			const float one = r1 < kRegions && hasRamp[r1] ? ramps[r1][q + c] : src[i * 4 + c];
			const float two = r2 < kRegions && hasRamp[r2] ? ramps[r2][q + c] : src[i * 4 + c];
			const float v   = one + (two - one) * w;
			px[i * 4 + c]   = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : lrintf(v));
		}
	}
	return out;
}

} // namespace

extern "C" int mpg_dress(int slot, int character, const char* textureName, const void* data, size_t bytes)
{
	if (!textureName || !data || !bytes || !GXPC_SetTextureOverride)
		return 0;
	std::lock_guard<std::mutex> hold(s_lock);
	Paint& p = s_paint;
	if (!p.loaded)
		load(p);
	if (!p.ok)
		return 0;
	auto file = p.files.find(textureName);
	if (file == p.files.end())
		return 0;
	std::shared_ptr<Picture> pic = picture(p, file->second);
	if (!pic)
		return 0;
	char look[512];
	mpg_look(slot, look, sizeof look);
	if (isFludd(textureName))
		character = MPG_CHAR_MARIO;
	bool chosen[kRegions];
	int colours[kRegions][3];
	// the same picture for everyone in the same colours
	const std::string key = file->second + "|" + (chosenColours(p, look, chosen, colours) ? std::string(look)
	                                                                                         : "#" + std::to_string(character));
	std::shared_ptr<std::vector<uint8_t>>& painted = p.painted[key];
	if (!painted)
		painted = paint(p, *pic, look, character);
	GXPC_SetTextureOverride(data, bytes, painted->data(), pic->w, pic->h);
	return 1;
}

extern "C" void mpg_undress(const void* data, size_t bytes)
{
	if (data && bytes && GXPC_SetTextureOverride)
		GXPC_SetTextureOverride(data, bytes, nullptr, 0, 0);
}

extern "C" void mpg_undress_all(void)
{
	if (GXPC_ClearTextureOverrides)
		GXPC_ClearTextureOverrides();
}
