// PAD: controller 1 from the keyboard, an SDL game controller and/or a GameCube
// adapter (gc_adapter.cpp); ports 2-4 are empty. Input arrives as SDL2 events
// through the GX layer's event hook (sms_gx_set_event_callback /
// sms_gx_pump_events, weak here so the port still links and runs without
// platform/gx). Bindings load from bindings.txt
// (see README.md); SMS_BINDINGS=path overrides the location. Controller 1's
// rumble motor drives the controller in use (sms_gx_rumble).
//
// Only SDL2's stable event ABI is used (mirrored below), so this file needs no
// SDL headers or library: the 32-bit build works without i386 SDL packages.
#include "port_compat.h"
#include "port_platform.h"
#include "port_savestate.h"
#include "gc_adapter.h"
#include "port_setting_list.h"
#include "port_settings.h"
#include <dolphin/pad.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <dlfcn.h>
#include <algorithm>
#include <vector>

// --- SDL2 event ABI (SDL_events.h, SDL_scancode.h, SDL_gamecontroller.h) ---
namespace sdl {
enum {
	QUIT = 0x100, WINDOWEVENT = 0x200, KEYDOWN = 0x300, KEYUP = 0x301, MOUSEMOTION = 0x400,
	MOUSEBUTTONDOWN = 0x401, MOUSEBUTTONUP = 0x402, MOUSEWHEEL = 0x403,
	CAXIS = 0x650, CBUTTONDOWN = 0x651, CBUTTONUP = 0x652
};
enum { WINDOWEVENT_FOCUS_GAINED = 12, WINDOWEVENT_FOCUS_LOST = 13 };
struct WindowEvent {
	u32 type, timestamp, windowID;
	u8 event, p1, p2, p3;
	s32 data1, data2;
};
struct MouseMotionEvent {
	u32 type, timestamp, windowID, which, state;
	s32 x, y, xrel, yrel;
};
struct MouseButtonEvent {
	u32 type, timestamp, windowID, which;
	u8 button, state, clicks, p1;
	s32 x, y;
};
struct MouseWheelEvent {
	u32 type, timestamp, windowID, which;
	s32 x, y;
	u32 direction;
};
const u32 TOUCH_MOUSEID = 0xFFFFFFFFu; // a mouse event SDL made from a touch
const u32 MOUSEWHEEL_FLIPPED = 1;
struct KeyboardEvent {
	u32 type, timestamp, windowID;
	u8 state, repeat, pad2, pad3;
	s32 scancode, sym;
	u16 mod;
	u32 unused;
};
struct ControllerAxisEvent {
	u32 type, timestamp;
	s32 which;
	u8 axis, p1, p2, p3;
	s16 value;
	u16 p4;
};
struct ControllerButtonEvent {
	u32 type, timestamp;
	s32 which;
	u8 button, state, p1, p2;
};
} // namespace sdl

extern "C" {
__attribute__((weak)) void sms_gx_set_event_callback(void (*cb)(const union SDL_Event* ev));
__attribute__((weak)) void sms_gx_pump_events(void);
__attribute__((weak)) int sms_gx_rumble(unsigned short strength);
__attribute__((weak)) void sms_gx_rumble_shutdown(void);
__attribute__((weak)) int GXPC_IsHeadless(void);
__attribute__((weak)) int GXPC_MouseCaptured(void);
__attribute__((weak)) int GXPC_GameInputBlocked(void);
__attribute__((weak)) int GXPC_RequestQuit(void);
}

// PC camera options, read by the camera code (decomp-patches/zzz-pc-camera.patch):
//   SMS_CAMERA_INVERT_X / _Y   flip the C-stick (and mouse) on that axis; _Y
//                              also flips FLUDD's aim while spraying in place
//                              (decomp-patches/zzz-camera-03-fludd-aim-invert-y.patch)
//   SMS_CAMERA_SPEED=percent   scales manual camera rotation (100 = retail)
//   SMS_FREE_CAMERA=1          the camera stays where it is put (no auto swing-back)
//   SMS_MOUSE_SENSITIVITY=%    mouse look speed, when the GX layer captures the mouse
//   SMS_CLOSE_CAMERA_CSTICK=1  the C-stick turns (and aims FLUDD in) the Y close-up
//                              camera too (decomp-patches/zzz-camera-04-close-camera-cstick.patch)
//   SMS_MOUSE_ORBIT=0          the mouse's up/down acts as the C-stick's again
//                              (nearer and lower, or further and higher) instead
//                              of tilting the camera around Mario
// and the mouse wheel zooms the camera in and out (decomp-patches/zzz-camera-02).
extern "C" {
int port_free_camera      = 0;
int port_camera_invert_y  = 0;
int port_close_camera_cstick = 0;
f32 port_camera_speed_x   = 1.0f;
f32 port_camera_speed_y   = 1.0f;
// Mouse orbit (zzz-camera-02): whether the mouse's up/down tilts the camera,
// and the tilt so far in s16 angle units (the camera keeps it in range).
int port_mouse_orbit         = 1;
f32 port_camera_mouse_pitch  = 0.0f;
// The camera's distance from Mario as a share of the game's own, which the
// mouse wheel sets: it eases toward the wheel's choice.
f32 port_camera_zoom         = 1.0f;
// Frames left in which the camera follows the mouse as closely as it follows
// the C-stick (the game follows a camera left alone more slowly).
int port_camera_mouse_busy   = 0;
}

// SMS_RUMBLE_STRENGTH above 100: how much more often the game runs the motor,
// read by RumbleBase::update (decomp-patches/port-10-rumble-boost.patch).
extern "C" {
f32 port_rumble_boost = 1.0f;
}

namespace {

enum Control {
	C_A, C_B, C_X, C_Y, C_Z, C_L, C_R, C_START,
	C_DUP, C_DDOWN, C_DLEFT, C_DRIGHT,
	C_UP, C_DOWN, C_LEFT, C_RIGHT,
	C_CUP, C_CDOWN, C_CLEFT, C_CRIGHT,
	C_HALF, C_QUIT, C_LSOFT, C_RSOFT, C_SAVESTATE, C_LOADSTATE, C_COUNT
};
const char* const kControlNames[C_COUNT] = {
	"A", "B", "X", "Y", "Z", "L", "R", "START",
	"DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT",
	"STICK_UP", "STICK_DOWN", "STICK_LEFT", "STICK_RIGHT",
	"CSTICK_UP", "CSTICK_DOWN", "CSTICK_LEFT", "CSTICK_RIGHT",
	"HALF_TILT", "QUIT", "L_SOFT", "R_SOFT", "SAVE_STATE", "LOAD_STATE",
};
// SAVE_STATE / LOAD_STATE are the port's, not the GameCube's: a press saves a
// save state, or loads the newest (port_savestate.h). Their bindings are often
// two buttons held together (PAD_LB+PAD_RB), and holding one of the pair keeps
// the other from the game (combo_masks below).
bool is_port_action(int c) { return c == C_SAVESTATE || c == C_LOADSTATE; }
// L_SOFT / R_SOFT press L or R part of the way, analog only (no digital click):
// a light press, as on a GameCube trigger before it clicks. How far, 0..255,
// from SMS_SOFT_TRIGGER (percent, default 40). Unbound unless bindings.txt
// gives them keys or buttons.
int g_softTrigger = 102;

struct KeyName {
	const char* name;
	int code;
};
// SDL scancodes are USB HID usage IDs.
// Mouse buttons follow them, as keys: kMouseBase plus SDL's button number.
const int kMouseBase = 512, kKeyCodes = kMouseBase + 8;
const KeyName kKeys[] = {
	{ "A", 4 }, { "B", 5 }, { "C", 6 }, { "D", 7 }, { "E", 8 }, { "F", 9 }, { "G", 10 }, { "H", 11 },
	{ "I", 12 }, { "J", 13 }, { "K", 14 }, { "L", 15 }, { "M", 16 }, { "N", 17 }, { "O", 18 }, { "P", 19 },
	{ "Q", 20 }, { "R", 21 }, { "S", 22 }, { "T", 23 }, { "U", 24 }, { "V", 25 }, { "W", 26 }, { "X", 27 },
	{ "Y", 28 }, { "Z", 29 }, { "1", 30 }, { "2", 31 }, { "3", 32 }, { "4", 33 }, { "5", 34 }, { "6", 35 },
	{ "7", 36 }, { "8", 37 }, { "9", 38 }, { "0", 39 }, { "ENTER", 40 }, { "RETURN", 40 }, { "ESCAPE", 41 },
	{ "ESC", 41 }, { "BACKSPACE", 42 }, { "TAB", 43 }, { "SPACE", 44 }, { "MINUS", 45 }, { "EQUALS", 46 },
	{ "LBRACKET", 47 }, { "RBRACKET", 48 }, { "SEMICOLON", 51 }, { "APOSTROPHE", 52 }, { "COMMA", 54 },
	{ "PERIOD", 55 }, { "SLASH", 56 }, { "F1", 58 }, { "F2", 59 }, { "F3", 60 }, { "F4", 61 },
	{ "F5", 62 }, { "F6", 63 }, { "F7", 64 }, { "F8", 65 }, { "F9", 66 }, { "F10", 67 }, { "F11", 68 },
	{ "F12", 69 },
	{ "RIGHT", 79 }, { "LEFT", 80 }, { "DOWN", 81 }, { "UP", 82 },
	{ "KP_DIVIDE", 84 }, { "KP_MULTIPLY", 85 }, { "KP_MINUS", 86 }, { "KP_PLUS", 87 }, { "KP_ENTER", 88 },
	{ "KP_1", 89 }, { "KP_2", 90 }, { "KP_3", 91 }, { "KP_4", 92 }, { "KP_5", 93 }, { "KP_6", 94 },
	{ "KP_7", 95 }, { "KP_8", 96 }, { "KP_9", 97 }, { "KP_0", 98 },
	{ "LCTRL", 224 }, { "LSHIFT", 225 }, { "LALT", 226 }, { "RCTRL", 228 }, { "RSHIFT", 229 }, { "RALT", 230 },
	{ "MOUSE_LEFT", kMouseBase + 1 }, { "MOUSE_MIDDLE", kMouseBase + 2 }, { "MOUSE_RIGHT", kMouseBase + 3 },
	{ "MOUSE_BACK", kMouseBase + 4 }, { "MOUSE_FORWARD", kMouseBase + 5 },
	{ "MOUSE_X1", kMouseBase + 4 }, { "MOUSE_X2", kMouseBase + 5 },
};

// Default bindings (documented in README.md).
const char* const kDefaultBindings =
    "A = SPACE X\n"
    "B = LSHIFT RSHIFT C\n"
    "X = V\n"
    "Y = F\n"
    "Z = Z\n"
    "L = Q\n"
    "R = E\n"
    "START = ENTER\n"
    "DPAD_UP = 1 KP_8\n"
    "DPAD_DOWN = 2 KP_2\n"
    "DPAD_LEFT = 3 KP_4\n"
    "DPAD_RIGHT = 4 KP_6\n"
    "STICK_UP = UP W\n"
    "STICK_DOWN = DOWN S\n"
    "STICK_LEFT = LEFT A\n"
    "STICK_RIGHT = RIGHT D\n"
    "CSTICK_UP = I\n"
    "CSTICK_DOWN = K\n"
    "CSTICK_LEFT = J\n"
    "CSTICK_RIGHT = L\n"
    "HALF_TILT = LCTRL\n"
    "QUIT = ESCAPE\n"
    "SAVE_STATE = F5\n"
    "LOAD_STATE = F6\n";

// A binding is one key or controller button, or two held together, written
// FIRST+SECOND (PAD_LB+PAD_RB, LCTRL+S): the second's code plus one sits above
// kComboShift.
const int kComboShift = 10;
int combo(int first, int second) { return first | (second + 1) << kComboShift; }
int combo_first(int bind) { return bind & ((1 << kComboShift) - 1); }
int combo_second(int bind) { return (bind >> kComboShift) - 1; }

const int kMaxKeys = 8;
int g_bind[C_COUNT][kMaxKeys];
int g_nbind[C_COUNT];
bool g_key[kKeyCodes];
// Game controller state (first controller wins).
s16 g_axis[6];
bool g_cbtn[21];

// Controller buttons a control reads, from bindings.txt names starting PAD_:
// SDL_CONTROLLER_BUTTON_* numbers, plus the two triggers (analog axes 4 and 5).
// A line lists keys, controller buttons or both; each part replaces only its
// own defaults, so a keyboard-only line keeps the controller layout.
enum { PAD_SRC_LT = 21, PAD_SRC_RT = 22, PAD_SRC_COUNT = 23 };
const KeyName kPadButtons[] = {
	{ "PAD_A", 0 }, { "PAD_B", 1 }, { "PAD_X", 2 }, { "PAD_Y", 3 }, { "PAD_BACK", 4 }, { "PAD_GUIDE", 5 },
	{ "PAD_START", 6 }, { "PAD_LSTICK", 7 }, { "PAD_RSTICK", 8 }, { "PAD_LB", 9 }, { "PAD_RB", 10 },
	{ "PAD_DPUP", 11 }, { "PAD_DPDOWN", 12 }, { "PAD_DPLEFT", 13 }, { "PAD_DPRIGHT", 14 }, { "PAD_MISC", 15 },
	{ "PAD_PADDLE1", 16 }, { "PAD_PADDLE2", 17 }, { "PAD_PADDLE3", 18 }, { "PAD_PADDLE4", 19 },
	{ "PAD_TOUCHPAD", 20 }, { "PAD_LT", PAD_SRC_LT }, { "PAD_RT", PAD_SRC_RT },
};
const int kMaxPad = 4;
int g_pbind[C_COUNT][kMaxPad];
int g_npbind[C_COUNT];
// The built-in controller layout (the one before PAD_ bindings existed).
const struct { int control, source; } kDefaultPad[] = {
	{ C_A, 0 }, { C_B, 1 }, { C_X, 2 }, { C_Y, 3 }, { C_Z, 10 }, { C_L, PAD_SRC_LT }, { C_R, PAD_SRC_RT },
	{ C_START, 6 }, { C_DUP, 11 }, { C_DDOWN, 12 }, { C_DLEFT, 13 }, { C_DRIGHT, 14 },
	{ C_SAVESTATE, combo(9, 10) }, // L1 + R1 (LB + RB)
};
bool g_inited;
// SMS_CAMERA_INVERT_X / SMS_CAMERA_INVERT_Y=1 flip the C-stick, which only
// turns the camera.
bool g_invert_cx, g_invert_cy;
// SMS_CLOSE_CAMERA_CSTICK=1 (port_close_camera_cstick)
bool g_closeCameraCStick;
// mouse look (see above): its speed, and the motion not yet given to the camera
float g_mouseSens = 1.0f;
int g_mouseDX, g_mouseDY;
// mouse wheel zoom: the share of the game's distance chosen, its limits, and
// how much one notch changes it
float g_zoomGoal = 1.0f;
const float kZoomMin = 0.5f, kZoomMax = 2.5f, kZoomStep = 1.12f;
// The wheel turned: up (positive) brings the camera in, down moves it out.
void zoom_notches(int notches)
{
	for (int i = 0; i < (notches < 0 ? -notches : notches); i++)
		g_zoomGoal = notches < 0 ? g_zoomGoal * kZoomStep : g_zoomGoal / kZoomStep;
	g_zoomGoal = g_zoomGoal < kZoomMin ? kZoomMin : g_zoomGoal > kZoomMax ? kZoomMax : g_zoomGoal;
}
// Input is ignored while another window has focus: SDL still reports
// controllers then (SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS), so a controller used
// in another app would also play the game. Losing focus releases everything
// held; a button still down when focus returns counts once pressed again.
// SMS_BACKGROUND_INPUT=1 keeps input live without focus (scripted presses too).
// Starts focused, so a window system that never reports focus changes nothing.
bool g_focused = true, g_backgroundInput;

// Rumble. The GameCube's motor is only on or off: the game rumbles weakly by
// running it on some frames only (RumbleMgr). The GameCube adapter plays it
// the same way; other controllers rumble at the share of frames it is on
// (sms_gx_rumble). SMS_RUMBLE=0 turns it off; SMS_RUMBLE_STRENGTH=percent scales it
// (SDL controllers only: the GameCube adapter's motors are on or off too).
// Above 100 the motor is at full strength and the game runs it on more frames
// (port_rumble_boost, decomp-patches/port-10), on every controller.
u16 g_rumbleStrength = 0xFFFF;
bool g_motor; // controller 1's motor, as the game last set it

// Sends the motor's state, off while another window has focus.
void update_rumble()
{
	const bool on = g_motor && g_focused && g_rumbleStrength;
	if (sms_gx_rumble)
		sms_gx_rumble(on ? g_rumbleStrength : 0);
	gc_adapter_rumble(on);
}

void stop_rumble_at_exit()
{
	if (sms_gx_rumble_shutdown)
		sms_gx_rumble_shutdown();
	if (!g_motor)
		return;
	g_motor = false;
	update_rumble();
	gc_adapter_flush_rumble();
}

void release_all()
{
	memset(g_key, 0, sizeof g_key);
	memset(g_axis, 0, sizeof g_axis);
	memset(g_cbtn, 0, sizeof g_cbtn);
	g_mouseDX = g_mouseDY = 0;
}

bool env_on(const char* name)
{
	const char* e = getenv(name);
	return e && *e && strcmp(e, "0") != 0;
}
float env_percent(const char* name, float lo, float hi)
{
	const char* v = getenv(name);
	if (!v || !*v)
		return 1.0f;
	float f = (float)atof(v) / 100.0f;
	return f < lo ? lo : f > hi ? hi : f;
}

int pad_code(const char* name)
{
	for (size_t i = 0; i < sizeof kPadButtons / sizeof kPadButtons[0]; i++)
		if (strcasecmp(kPadButtons[i].name, name) == 0)
			return kPadButtons[i].code;
	return -1;
}

int key_code(const char* name)
{
	for (size_t i = 0; i < sizeof kKeys / sizeof kKeys[0]; i++)
		if (strcasecmp(kKeys[i].name, name) == 0)
			return kKeys[i].code;
	if (name[0] == '#')
		return atoi(name + 1); // raw scancode
	return -1;
}

void parse_bindings(const char* text, const char* source)
{
	const char* p = text;
	int line      = 0;
	while (*p) {
		line++;
		char buf[256];
		size_t n = strcspn(p, "\n");
		if (n >= sizeof buf)
			n = sizeof buf - 1;
		memcpy(buf, p, n);
		buf[n] = 0;
		p += n + (p[n] == '\n');
		char* hash = strchr(buf, '#');
		if (hash && (hash == buf || hash[-1] == ' ' || hash[-1] == '\t'))
			*hash = 0;
		char* eq = strchr(buf, '=');
		if (!eq)
			continue;
		*eq        = 0;
		char* name = strtok(buf, " \t");
		if (!name)
			continue;
		int c = -1;
		for (int i = 0; i < C_COUNT; i++)
			if (strcasecmp(kControlNames[i], name) == 0)
				c = i;
		if (c < 0) {
			port_log("[pad] %s:%d: unknown control '%s'\n", source, line, name);
			continue;
		}
		int keys[kMaxKeys], nkeys = 0, pads[kMaxPad], npads = 0;
		bool anyKey = false, anyPad = false;
		for (char* k = strtok(eq + 1, " \t,"); k; k = strtok(NULL, " \t,")) {
			// FIRST+SECOND: two keys or two controller buttons held together
			char* second = strchr(k, '+');
			if (second)
				*second++ = 0;
			const bool pad = strncasecmp(k, "PAD_", 4) == 0;
			if (second && (strncasecmp(second, "PAD_", 4) == 0) != pad) {
				port_log("[pad] %s:%d: '%s+%s' mixes a key and a controller button\n", source, line, k, second);
				continue;
			}
			if (pad) {
				anyPad   = true;
				int code = pad_code(k), code2 = second ? pad_code(second) : -1;
				if (code < 0 || (second && code2 < 0))
					port_log("[pad] %s:%d: unknown controller button '%s'\n", source, line, code < 0 ? k : second);
				else if (npads < kMaxPad)
					pads[npads++] = second ? combo(code, code2) : code;
				continue;
			}
			anyKey   = true;
			int code = key_code(k), code2 = second ? key_code(second) : -1;
			if (code < 0 || code >= kKeyCodes || (second && (code2 < 0 || code2 >= kKeyCodes)))
				port_log("[pad] %s:%d: unknown key '%s'\n", source, line, code < 0 || code >= kKeyCodes ? k : second);
			else if (nkeys < kMaxKeys)
				keys[nkeys++] = second ? combo(code, code2) : code;
		}
		// each part replaces only its own defaults; an empty line clears the keys
		if (anyKey || !anyPad) {
			memcpy(g_bind[c], keys, sizeof keys[0] * nkeys);
			g_nbind[c] = nkeys;
		}
		if (anyPad) {
			memcpy(g_pbind[c], pads, sizeof pads[0] * npads);
			g_npbind[c] = npads;
			char list[96] = "";
			for (int i = 0; i < npads; i++)
				for (int part = 0; part < 2; part++)
					for (size_t j = 0; j < sizeof kPadButtons / sizeof kPadButtons[0]; j++)
						if (kPadButtons[j].code == (part ? combo_second(pads[i]) : combo_first(pads[i])))
							snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s",
							         part ? "+" : " ", kPadButtons[j].name);
			port_log("[pad] %s: controller %s =%s\n", source, kControlNames[c], npads ? list : " (none)");
		}
	}
}

void on_event(const union SDL_Event* ev)
{
	u32 type = *(const u32*)ev;
	if (type == sdl::WINDOWEVENT) {
		const sdl::WindowEvent* w = (const sdl::WindowEvent*)ev;
		if (w->event == sdl::WINDOWEVENT_FOCUS_LOST && g_focused && !g_backgroundInput) {
			g_focused = false;
			release_all();
			update_rumble();
			port_log("[pad] window lost focus: ignoring input\n");
		} else if (w->event == sdl::WINDOWEVENT_FOCUS_GAINED && !g_focused) {
			g_focused = true;
			update_rumble();
			port_log("[pad] window has focus: input back on\n");
		}
		return;
	}
	if (!g_focused)
		return;
	switch (type) {
	case sdl::KEYDOWN:
	case sdl::KEYUP: {
		const sdl::KeyboardEvent* k = (const sdl::KeyboardEvent*)ev;
		if (k->scancode >= 0 && k->scancode < 512)
			g_key[k->scancode] = type == sdl::KEYDOWN;
		// The quit key asks first, in the window (the quit prompt); a held
		// key's repeats do not ask again.
		if (type == sdl::KEYDOWN && !k->repeat)
			for (int i = 0; i < g_nbind[C_QUIT]; i++)
				if (g_bind[C_QUIT][i] == k->scancode) {
					port_log("[pad] quit key pressed\n");
					if (!(GXPC_RequestQuit && GXPC_RequestQuit()))
						exit(0);
					break;
				}
		break;
	}
	case sdl::MOUSEMOTION: {
		const sdl::MouseMotionEvent* m = (const sdl::MouseMotionEvent*)ev;
		if (GXPC_MouseCaptured && GXPC_MouseCaptured()) {
			g_mouseDX += m->xrel;
			g_mouseDY += m->yrel;
		}
		break;
	}
	// Mouse buttons are keys (MOUSE_LEFT ...). The click that gives mouse look
	// its capture back never gets here (gx_platform.cpp), nor do touches.
	case sdl::MOUSEBUTTONDOWN:
	case sdl::MOUSEBUTTONUP: {
		const sdl::MouseButtonEvent* m = (const sdl::MouseButtonEvent*)ev;
		if (m->which != sdl::TOUCH_MOUSEID && m->button >= 1 && kMouseBase + m->button < kKeyCodes)
			g_key[kMouseBase + m->button] = type == sdl::MOUSEBUTTONDOWN;
		break;
	}
	// Scrolling down moves the camera out, up brings it in.
	case sdl::MOUSEWHEEL: {
		const sdl::MouseWheelEvent* w = (const sdl::MouseWheelEvent*)ev;
		const int notches = w->direction == sdl::MOUSEWHEEL_FLIPPED ? -w->y : w->y;
		if (w->which == sdl::TOUCH_MOUSEID || !notches || (GXPC_GameInputBlocked && GXPC_GameInputBlocked()))
			break;
		zoom_notches(notches);
		break;
	}
	case sdl::CAXIS: {
		const sdl::ControllerAxisEvent* a = (const sdl::ControllerAxisEvent*)ev;
		if (a->axis < 6)
			g_axis[a->axis] = a->value;
		break;
	}
	case sdl::CBUTTONDOWN:
	case sdl::CBUTTONUP: {
		const sdl::ControllerButtonEvent* b = (const sdl::ControllerButtonEvent*)ev;
		if (b->button < 21)
			g_cbtn[b->button] = type == sdl::CBUTTONDOWN;
		break;
	}
	}
}

// How far a trigger is pressed, 0..255.
int trigger(int source) { return g_axis[source == PAD_SRC_LT ? 4 : 5] > 0 ? g_axis[source == PAD_SRC_LT ? 4 : 5] * 255 / 32767 : 0; }

// Keys and controller buttons the game does not see, because the other half
// of a port action's two-button binding is held (combo_masks).
bool g_keyMasked[kKeyCodes], g_padMasked[PAD_SRC_COUNT];

bool pad_down(int s) { return s == PAD_SRC_LT || s == PAD_SRC_RT ? trigger(s) >= 250 : g_cbtn[s]; }
// Pressed at all: a trigger part of the way counts.
bool pad_touched(int s) { return s == PAD_SRC_LT || s == PAD_SRC_RT ? trigger(s) >= 26 : g_cbtn[s]; }

// Holding one half of a port action's two-button binding (LB of LB+RB) hides
// the other half from the game until that is let go, so saving a state with
// L1 + R1 (LB + RB) does not also press Z.
void combo_masks()
{
	bool key[kKeyCodes] = {}, pad[PAD_SRC_COUNT] = {};
	for (int c = 0; c < C_COUNT; c++) {
		if (!is_port_action(c))
			continue;
		for (int i = 0; i < g_nbind[c]; i++) {
			const int a = combo_first(g_bind[c][i]), b = combo_second(g_bind[c][i]);
			if (b >= 0) {
				key[a] = key[a] || g_key[b];
				key[b] = key[b] || g_key[a];
			}
		}
		for (int i = 0; i < g_npbind[c]; i++) {
			const int a = combo_first(g_pbind[c][i]), b = combo_second(g_pbind[c][i]);
			if (b >= 0) {
				pad[a] = pad[a] || pad_touched(b);
				pad[b] = pad[b] || pad_touched(a);
			}
		}
	}
	for (int k = 0; k < kKeyCodes; k++)
		g_keyMasked[k] = key[k] || (g_keyMasked[k] && g_key[k]);
	for (int s = 0; s < PAD_SRC_COUNT; s++)
		g_padMasked[s] = pad[s] || (g_padMasked[s] && pad_touched(s));
}

// A control's bound keys or controller buttons (a trigger counts once it
// clicks); a two-button binding needs both. The game's controls skip what
// combo_masks hides.
bool held(int c)
{
	const bool game = !is_port_action(c);
	for (int i = 0; i < g_nbind[c]; i++) {
		const int a = combo_first(g_bind[c][i]), b = combo_second(g_bind[c][i]);
		if (g_key[a] && !(game && g_keyMasked[a]) && (b < 0 || (g_key[b] && !(game && g_keyMasked[b]))))
			return true;
	}
	for (int i = 0; i < g_npbind[c]; i++) {
		const int a = combo_first(g_pbind[c][i]), b = combo_second(g_pbind[c][i]);
		if (pad_down(a) && !(game && g_padMasked[a]) && (b < 0 || (pad_down(b) && !(game && g_padMasked[b]))))
			return true;
	}
	return false;
}

// L or R's analog pressure: its triggers' own travel, full for a key or button.
int analog_trigger(int c)
{
	int v = 0;
	for (int i = 0; i < g_npbind[c]; i++) {
		const int s = g_pbind[c][i];
		if ((s == PAD_SRC_LT || s == PAD_SRC_RT) && !g_padMasked[s])
			v = std::max(v, trigger(s));
	}
	return held(c) ? 255 : v;
}

s8 axis8(int v, int range)
{
	int x = v * range / 32767;
	if (x > -8 && x < 8)
		x = 0; // dead zone
	return (s8)(x > 127 ? 127 : x < -128 ? -128 : x);
}

void autopress_init();

void init()
{
	if (g_inited)
		return;
	g_inited = true;
	port_free_camera    = env_on("SMS_FREE_CAMERA");
	port_camera_speed_x = port_camera_speed_y = env_percent("SMS_CAMERA_SPEED", 0.1f, 4.0f);
	g_mouseSens         = env_percent("SMS_MOUSE_SENSITIVITY", 0.05f, 10.0f);
	if (const char* e = getenv("SMS_MOUSE_ORBIT"))
		port_mouse_orbit = !*e || env_on("SMS_MOUSE_ORBIT");
	g_backgroundInput   = env_on("SMS_BACKGROUND_INPUT") || env_on("SMS_AUTOPRESS");
	if (g_backgroundInput)
		port_log("[pad] input stays on while the window is in the background\n");
	if (const char* e = getenv("SMS_SOFT_TRIGGER"))
		if (*e) {
			// stays below the click (250), or it would no longer be a soft press
			const int pct = atoi(e) < 5 ? 5 : atoi(e) > 95 ? 95 : atoi(e);
			g_softTrigger = pct * 255 / 100;
			port_log("[pad] soft L/R press: %d%%\n", pct);
		}
	if (const char* e = getenv("SMS_RUMBLE"))
		if (*e && !env_on("SMS_RUMBLE")) {
			g_rumbleStrength = 0;
			port_log("[pad] rumble off\n");
		}
	if (const char* e = getenv("SMS_RUMBLE_STRENGTH"))
		if (*e && g_rumbleStrength) {
			const int pct    = atoi(e) < 0 ? 0 : atoi(e) > 200 ? 200 : atoi(e);
			g_rumbleStrength = (u16)((pct < 100 ? pct : 100) * 0xFFFF / 100);
			port_rumble_boost = pct > 100 ? pct / 100.0f : 1.0f;
			port_log("[pad] rumble strength: %d%%\n", pct);
		}
	atexit(stop_rumble_at_exit);
	if (port_free_camera || port_camera_speed_x != 1.0f)
		port_log("[pad] camera: free camera %s, speed %d%%\n", port_free_camera ? "on" : "off",
		         (int)(port_camera_speed_x * 100.0f + 0.5f));
	for (size_t i = 0; i < sizeof kDefaultPad / sizeof kDefaultPad[0]; i++)
		g_pbind[kDefaultPad[i].control][g_npbind[kDefaultPad[i].control]++] = kDefaultPad[i].source;
	parse_bindings(kDefaultBindings, "defaults");
	g_invert_cx = env_on("SMS_CAMERA_INVERT_X");
	g_invert_cy = env_on("SMS_CAMERA_INVERT_Y");
	g_closeCameraCStick = env_on("SMS_CLOSE_CAMERA_CSTICK");
	if (g_closeCameraCStick)
		port_log("[pad] the C-stick turns the close-up camera too\n");
	if (g_invert_cx || g_invert_cy)
		port_log("[pad] camera inverted:%s%s\n", g_invert_cx ? " X" : "", g_invert_cy ? " Y" : "");
	const char* path = getenv("SMS_BINDINGS");
	if (!path)
		path = "bindings.txt";
	FILE* f = fopen(path, "r");
	if (!f && !getenv("SMS_BINDINGS")) {
		path = "../../bindings.txt"; // running from build/<os>-<arch>/
		f    = fopen(path, "r");
	}
	if (f) {
		static char text[16384];
		size_t n = fread(text, 1, sizeof text - 1, f);
		text[n]  = 0;
		fclose(f);
		parse_bindings(text, path);
		port_log("[pad] loaded key bindings from %s\n", path);
	}
	gc_adapter_start();
	if (sms_gx_set_event_callback)
		sms_gx_set_event_callback(on_event);
	else
		port_log("[pad] no GX/SDL event source linked: controller 1 stays idle\n");
}

// --- Test input: SMS_AUTOPRESS=control@field[+hold],... ------------------------
// e.g. SMS_AUTOPRESS=start@600,a@900+20 presses Start at retrace 600 and A at
// 900 (held for 20 retraces; default 8). With the GX layer's SDL window the
// press goes through the real keyboard path: an SDL key event carrying the
// control's first bound key is pushed with SDL_PushEvent. Without SDL the key
// state is set directly.
struct AutoPress {
	int control;
	u32 at, until;
	bool down, done;
};
std::vector<AutoPress> g_auto;
typedef int (*PushEventFn)(void* ev);
PushEventFn g_push;

void autopress_init()
{
	const char* e = getenv("SMS_AUTOPRESS");
	if (!e || !*e)
		return;
	char buf[1024];
	strncpy(buf, e, sizeof buf - 1);
	buf[sizeof buf - 1] = 0;
	for (char* tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
		char* at = strchr(tok, '@');
		if (!at)
			continue;
		*at        = 0;
		u32 hold   = 8;
		char* plus = strchr(at + 1, '+');
		if (plus)
			hold = (u32)atoi(plus + 1);
		int c = -1;
		for (int i = 0; i < C_COUNT; i++)
			if (strcasecmp(kControlNames[i], tok) == 0)
				c = i;
		if (c < 0 || g_nbind[c] == 0) {
			port_log("[pad] SMS_AUTOPRESS: unknown or unbound control '%s'\n", tok);
			continue;
		}
		AutoPress a = { c, (u32)atoi(at + 1), 0, false, false };
		a.until     = a.at + hold;
		g_auto.push_back(a);
	}
	// Headless runs have no SDL event loop: set the key state directly.
	if (!(GXPC_IsHeadless && GXPC_IsHeadless()))
		g_push = (PushEventFn)dlsym(RTLD_DEFAULT, "SDL_PushEvent");
	port_log("[pad] SMS_AUTOPRESS: %u scripted presses via %s\n", (unsigned)g_auto.size(),
	         g_push ? "SDL_PushEvent (keyboard path)" : "direct key state");
}

void send_key(int scancode, bool down)
{
	if (g_push && scancode < kMouseBase) {
		union {
			sdl::KeyboardEvent k;
			u8 raw[56];
		} ev;
		memset(&ev, 0, sizeof ev);
		ev.k.type     = down ? sdl::KEYDOWN : sdl::KEYUP;
		ev.k.state    = down ? 1 : 0;
		ev.k.scancode = scancode;
		if (g_push(&ev) >= 0)
			return;
	}
	g_key[scancode] = down;
}

// Both keys of a two-key binding, the first pressed first.
void send_binding(int bind, bool down)
{
	send_key(combo_first(bind), down);
	if (combo_second(bind) >= 0)
		send_key(combo_second(bind), down);
}

// --- Test input: SMS_AUTOMOUSE=dx:dy[:wheel]@field[+fields],... -------------
// e.g. SMS_AUTOMOUSE=0:-12@900+20,0:0:-3@1200 moves the mouse 12 pixels up a
// field for 20 fields from field 900 (as mouse look would read it, captured or
// not), then scrolls the wheel 3 notches down (out) at 1200.
struct AutoMouse {
	int dx, dy, wheel;
	u32 at, until;
};
std::vector<AutoMouse> g_autoMouse;

void automouse_init()
{
	const char* e = getenv("SMS_AUTOMOUSE");
	if (!e || !*e)
		return;
	char buf[1024];
	strncpy(buf, e, sizeof buf - 1);
	buf[sizeof buf - 1] = 0;
	for (char* tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
		AutoMouse m = {};
		u32 fields  = 1;
		if (sscanf(tok, "%d:%d:%d@%u+%u", &m.dx, &m.dy, &m.wheel, &m.at, &fields) < 4
		    && sscanf(tok, "%d:%d@%u+%u", &m.dx, &m.dy, &m.at, &fields) < 3) {
			port_log("[pad] SMS_AUTOMOUSE: cannot read '%s'\n", tok);
			continue;
		}
		m.until = m.at + (fields ? fields : 1);
		g_autoMouse.push_back(m);
	}
	port_log("[pad] SMS_AUTOMOUSE: %u scripted mouse moves\n", (unsigned)g_autoMouse.size());
}

void automouse_field(u32 field)
{
	for (size_t i = 0; i < g_autoMouse.size(); i++) {
		const AutoMouse& m = g_autoMouse[i];
		if (field < m.at || field >= m.until)
			continue;
		g_mouseDX += m.dx;
		g_mouseDY += m.dy;
		if (m.wheel && field == m.at) {
			zoom_notches(m.wheel);
			port_log("[pad] automouse wheel %+d at field %u: zoom %.2f\n", m.wheel, field, g_zoomGoal);
		}
	}
}

} // namespace

extern "C" void port_pad_autopress_field(u32 field)
{
	automouse_field(field);
	for (size_t i = 0; i < g_auto.size(); i++) {
		AutoPress& a = g_auto[i];
		if (a.done)
			continue;
		if (!a.down && field >= a.at) {
			a.down = true;
			port_log("[pad] autopress %s down at field %u\n", kControlNames[a.control], field);
			send_binding(g_bind[a.control][0], true);
		} else if (a.down && field >= a.until) {
			a.done = true;
			send_binding(g_bind[a.control][0], false);
		}
	}
}

// SAVE_STATE's first binding as the pause menu's hint names it
// (port_savestate.h): the keyboard's key names, or a controller's buttons as
// that kind of controller labels them, indexed by SDL button (kPadButtons).
extern "C" int port_pad_savestate_labels(int style, char labels[2][16])
{
	static const char* const kXbox[PAD_SRC_COUNT] = {
		"A", "B", "X", "Y", "View", "Xbox", "Menu", "LS", "RS", "LB", "RB", "Up", "Down", "Left", "Right",
		"Share", "P1", "P2", "P3", "P4", "Touchpad", "LT", "RT",
	};
	static const char* const kPlayStation[PAD_SRC_COUNT] = {
		"Cross", "Circle", "Square", "Triangle", "Create", "PS", "Options", "L3", "R3", "L1", "R1", "Up", "Down",
		"Left", "Right", "Mic", "P1", "P2", "P3", "P4", "Touchpad", "L2", "R2",
	};
	static const char* const kSteamDeck[PAD_SRC_COUNT] = {
		"A", "B", "X", "Y", "View", "Steam", "Menu", "L3", "R3", "L1", "R1", "Up", "Down", "Left", "Right",
		"...", "L4", "R4", "L5", "R5", "Touchpad", "L2", "R2",
	};
	init();
	labels[0][0] = labels[1][0] = 0;
	const bool keyboard = style == 0;
	if (keyboard ? !g_nbind[C_SAVESTATE] : !g_npbind[C_SAVESTATE])
		return 0;
	const int bind = keyboard ? g_bind[C_SAVESTATE][0] : g_pbind[C_SAVESTATE][0];
	const int parts[2] = { combo_first(bind), combo_second(bind) };
	int n = 0;
	for (int i = 0; i < 2 && parts[i] >= 0; i++, n++) {
		const char* name = nullptr;
		if (keyboard) {
			for (size_t k = 0; k < sizeof kKeys / sizeof kKeys[0] && !name; k++)
				if (kKeys[k].code == parts[i])
					name = kKeys[k].name;
		} else if (parts[i] < PAD_SRC_COUNT) {
			name = (style == 2 ? kPlayStation : style == 3 ? kSteamDeck : kXbox)[parts[i]];
		}
		if (name)
			snprintf(labels[i], 16, "%s", name);
		else
			snprintf(labels[i], 16, "#%d", parts[i]);
	}
	return n;
}

extern "C" BOOL PADInit()
{
	init();
	return TRUE;
}
extern "C" int PADReset(unsigned long) { return TRUE; }
extern "C" BOOL PADRecalibrate(u32) { return TRUE; }
extern "C" BOOL PADSync(void) { return TRUE; }
extern "C" void PADSetSpec(u32) {}
extern "C" void PADSetAnalogMode(u32) {}
extern "C" void PADSetSamplingRate(unsigned long) {}
extern "C" void PADControlMotor(s32 chan, u32 command)
{
	if (chan != 0)
		return;
	init();
	g_motor = command == PAD_MOTOR_RUMBLE;
	update_rumble();
}
extern "C" void PADControlAllMotors(const u32* commands) { PADControlMotor(0, commands[0]); }

extern "C" __attribute__((weak)) int port_trace_pad_read(struct PADStatus* status);
// Extensions (ext/, src/port_include/port_ext.h) may change what was read.
extern "C" __attribute__((weak)) void port_ext_pad_read(void* status);

extern "C" u32 PADRead(PADStatus* status)
{
	// A .dtm movie (platform/trace, SMS_MOVIE) replaces live input, and is
	// played as recorded: the game code's own inversion is off too.
	if (port_trace_pad_read && port_trace_pad_read(status)) {
		port_camera_invert_y     = 0;
		port_close_camera_cstick = 0;
		return PAD_CHAN0_BIT;
	}
	init();
	port_camera_invert_y     = g_invert_cy;
	port_close_camera_cstick = g_closeCameraCStick;
	static bool autoInited;
	if (!autoInited) {
		autoInited = true;
		autopress_init();
		automouse_init();
	}
	if (sms_gx_pump_events)
		sms_gx_pump_events();
	for (int i = 0; i < 4; i++) {
		memset(&status[i], 0, sizeof status[i]);
		status[i].err = i == 0 ? PAD_ERR_NONE : PAD_ERR_NO_CONTROLLER;
	}
	combo_masks();
	if (port_camera_mouse_busy > 0)
		port_camera_mouse_busy--;
	const bool blocked = GXPC_GameInputBlocked && GXPC_GameInputBlocked();
	// Port actions on their press, not while a menu has the input.
	static bool wasSave, wasLoad;
	const bool save = held(C_SAVESTATE), load = held(C_LOADSTATE);
	if (save && !wasSave && !blocked)
		port_savestate_hotkey(PORT_SAVESTATE_HOTKEY_SAVE);
	if (load && !wasLoad && !blocked)
		port_savestate_hotkey(PORT_SAVESTATE_HOTKEY_LOAD_NEWEST);
	wasSave = save;
	wasLoad = load;
	if (blocked)
		return PAD_CHAN0_BIT;
	PADStatus& s = status[0];
	u16 b        = 0;
	static const struct {
		int control;
		u16 bit;
	} map[] = {
		{ C_A, PAD_BUTTON_A },          { C_B, PAD_BUTTON_B },          { C_X, PAD_BUTTON_X },
		{ C_Y, PAD_BUTTON_Y },          { C_Z, PAD_TRIGGER_Z },         { C_START, PAD_BUTTON_START },
		{ C_DUP, PAD_BUTTON_UP },       { C_DDOWN, PAD_BUTTON_DOWN },   { C_DLEFT, PAD_BUTTON_LEFT },
		{ C_DRIGHT, PAD_BUTTON_RIGHT },
	};
	for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
		if (held(map[i].control))
			b |= map[i].bit;
	// Triggers: analog from the bound triggers; a key or button is a full press
	// (analog 255 plus the digital click).
	int tl = analog_trigger(C_L);
	int tr = analog_trigger(C_R);
	if (held(C_LSOFT))
		tl = std::max(tl, g_softTrigger);
	if (held(C_RSOFT))
		tr = std::max(tr, g_softTrigger);
	if (tl >= 250)
		b |= PAD_TRIGGER_L;
	if (tr >= 250)
		b |= PAD_TRIGGER_R;
	// GameCube adapter (gc_adapter.cpp): buttons, sticks, triggers.
	GcAdapterInput gc;
	const bool haveGc = g_focused && gc_adapter_read(&gc);
	if (haveGc) {
		if (gc.b1 & 0x01) b |= PAD_BUTTON_A;
		if (gc.b1 & 0x02) b |= PAD_BUTTON_B;
		if (gc.b1 & 0x04) b |= PAD_BUTTON_X;
		if (gc.b1 & 0x08) b |= PAD_BUTTON_Y;
		if (gc.b1 & 0x10) b |= PAD_BUTTON_LEFT;
		if (gc.b1 & 0x20) b |= PAD_BUTTON_RIGHT;
		if (gc.b1 & 0x40) b |= PAD_BUTTON_DOWN;
		if (gc.b1 & 0x80) b |= PAD_BUTTON_UP;
		if (gc.b2 & 0x01) b |= PAD_BUTTON_START;
		if (gc.b2 & 0x02) b |= PAD_TRIGGER_Z;
		if (gc.b2 & 0x04) b |= PAD_TRIGGER_R;
		if (gc.b2 & 0x08) b |= PAD_TRIGGER_L;
		int al = gc.lt, ar = gc.rt; // less a fixed rest margin
		al = al > 30 ? (al - 30) * 255 / 225 : 0;
		ar = ar > 30 ? (ar - 30) * 255 / 225 : 0;
		if (al > tl) tl = al;
		if (ar > tr) tr = ar;
		if (gc.b2 & 0x08) tl = 255;
		if (gc.b2 & 0x04) tr = 255;
	}
	s.triggerLeft  = (u8)tl;
	s.triggerRight = (u8)tr;
	s.analogA      = (b & PAD_BUTTON_A) ? 255 : 0;
	s.analogB      = (b & PAD_BUTTON_B) ? 255 : 0;
	s.button       = b;
	// Sticks are reported raw (a real stick reads about +-100 at the rim);
	// the decomp's PADClamp (libs/dolphin/src/pad/Padclamp.c) then applies the dead
	// zone and the octagon exactly as on hardware. Keys give full deflection
	// (half with HALF_TILT).
	int full = held(C_HALF) ? 50 : 100;
	int x = held(C_RIGHT) * full - held(C_LEFT) * full;
	int y = held(C_UP) * full - held(C_DOWN) * full;
	if (x && y) {
		x = x * 7 / 10;
		y = y * 7 / 10;
	}
	int sx = axis8(g_axis[0], 100), sy = axis8(-g_axis[1], 100);
	int cx = held(C_CRIGHT) * 100 - held(C_CLEFT) * 100;
	int cy = held(C_CUP) * 100 - held(C_CDOWN) * 100;
	int pcx = axis8(g_axis[2], 100), pcy = axis8(-g_axis[3], 100);
	if (haveGc && (gc.lx || gc.ly)) { sx = gc.lx; sy = gc.ly; }
	if (haveGc && (gc.cx || gc.cy)) { pcx = gc.cx; pcy = gc.cy; }
	s.stickX    = (s8)(x ? x : sx);
	s.stickY    = (s8)(y ? y : sy);
	cx          = cx ? cx : pcx;
	cy          = cy ? cy : pcy;
	s.substickX = (s8)(g_invert_cx ? -cx : cx);
	s.substickY = (s8)(g_invert_cy ? -cy : cy);
	if (port_ext_pad_read)
		port_ext_pad_read(status);
	return PAD_CHAN0_BIT;
}

// Mouse look: the motion since the last call, in C-stick units (1.0 is full
// deflection for one frame), with the camera's inversion applied. Returns 0
// when the mouse has not moved. The normal camera calls it once a frame, which
// also moves the wheel's zoom along.
extern "C" int port_camera_take_mouse(f32* dx, f32* dy)
{
	*dx = *dy = 0.0f;
	if (port_camera_zoom != g_zoomGoal) {
		port_camera_zoom += (g_zoomGoal - port_camera_zoom) * 0.25f;
		if (fabsf(g_zoomGoal - port_camera_zoom) < 0.002f)
			port_camera_zoom = g_zoomGoal;
		port_camera_mouse_busy = 8;
	}
	if (!g_mouseDX && !g_mouseDY)
		return 0;
	port_camera_mouse_busy = 8;
	const float k = 0.02f * g_mouseSens;
	// moving the mouse right turns the view right, as the C-stick pushed left does
	*dx = -(float)g_mouseDX * k;
	*dy = -(float)g_mouseDY * k;
	if (g_invert_cx)
		*dx = -*dx;
	if (g_invert_cy)
		*dy = -*dy;
	g_mouseDX = g_mouseDY = 0;
	return 1;
}

// --- Settings menu (port_settings.h) ---
// Every pad option lives in state that only the emulated CPU's thread touches: PADRead and
// PADControlMotor, and the window events (they arrive through sms_gx_pump_events, which PADRead
// and the frame present call on that thread). The menu's set() runs on the same thread while the
// game is paused, so a plain store is enough; the camera code reads the globals each frame. The
// GameCube adapter's own thread only reads gcad::g_state, which update_rumble writes atomically.
namespace {

int off_on_count() { return 2; }
const char* off_on_name(int i) { return i ? "On" : "Off"; }

const char* percent_name(const PortValueList& l, int i)
{
	static char text[16];
	snprintf(text, sizeof text, "%d", l.v[i < 0 ? 0 : i >= l.n ? l.n - 1 : i]);
	return text;
}

// The launcher's values come from the standard steps, plus the one the game started with.
PortValueList g_cameraSpeedList, g_mouseSensList, g_softTriggerList, g_rumbleList;

int camera_speed_percent() { return (int)(port_camera_speed_x * 100.0f + 0.5f); }
int mouse_sens_percent() { return (int)(g_mouseSens * 100.0f + 0.5f); }
// The press depth is kept as 0..255, from percent * 255 / 100; this undoes that rounding.
int soft_trigger_percent() { return (g_softTrigger * 100 + 127) / 255; }
// 0 is off (SMS_RUMBLE=0 starts with the strength at 0 too, so one number says both).
int rumble_percent() { return (int)(((u32)g_rumbleStrength * 100 + 0x7FFF) / 0xFFFF); }

const PortValueList& camera_speed_list()
{
	static const int steps[] = { 25, 50, 75, 100, 125, 150, 175, 200, 225, 250, 275, 300 };
	if (!g_cameraSpeedList.built) {
		init(); // the start values come from the environment
		g_cameraSpeedList.build(steps, (int)(sizeof steps / sizeof steps[0]), camera_speed_percent());
	}
	return g_cameraSpeedList;
}

const PortValueList& mouse_sens_list()
{
	static const int steps[] = { 25, 50, 75, 100, 125, 150, 175, 200, 225, 250, 275, 300, 325, 350, 375, 400 };
	if (!g_mouseSensList.built) {
		init();
		g_mouseSensList.build(steps, (int)(sizeof steps / sizeof steps[0]), mouse_sens_percent());
	}
	return g_mouseSensList;
}

const PortValueList& soft_trigger_list()
{
	static const int steps[] = { 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 65, 70, 75, 80, 85, 90, 95 };
	if (!g_softTriggerList.built) {
		init();
		g_softTriggerList.build(steps, (int)(sizeof steps / sizeof steps[0]), soft_trigger_percent());
	}
	return g_softTriggerList;
}

const PortValueList& rumble_list()
{
	static const int steps[] = { 0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 };
	if (!g_rumbleList.built) {
		init();
		g_rumbleList.build(steps, (int)(sizeof steps / sizeof steps[0]), rumble_percent());
	}
	return g_rumbleList;
}

int invert_x_get() { return g_invert_cx; }
int invert_x_set(int v) { g_invert_cx = v != 0; return 1; }
int invert_y_get() { return g_invert_cy; }
int invert_y_set(int v) { g_invert_cy = v != 0; return 1; }
int free_camera_get() { return port_free_camera != 0; }
int free_camera_set(int v) { port_free_camera = v != 0; return 1; }

int camera_speed_count() { return camera_speed_list().n; }
const char* camera_speed_name(int i) { return percent_name(camera_speed_list(), i); }
int camera_speed_get() { return camera_speed_list().indexOf(camera_speed_percent()); }
int camera_speed_set(int i)
{
	const PortValueList& l = camera_speed_list();
	if (i < 0 || i >= l.n)
		return 0;
	port_camera_speed_x = port_camera_speed_y = (f32)l.v[i] / 100.0f;
	return 1;
}

int mouse_sens_count() { return mouse_sens_list().n; }
const char* mouse_sens_name(int i) { return percent_name(mouse_sens_list(), i); }
int mouse_sens_get() { return mouse_sens_list().indexOf(mouse_sens_percent()); }
int mouse_sens_set(int i)
{
	const PortValueList& l = mouse_sens_list();
	if (i < 0 || i >= l.n)
		return 0;
	g_mouseSens = (float)l.v[i] / 100.0f;
	return 1;
}

int soft_trigger_count() { return soft_trigger_list().n; }
const char* soft_trigger_name(int i) { return percent_name(soft_trigger_list(), i); }
int soft_trigger_get() { return soft_trigger_list().indexOf(soft_trigger_percent()); }
int soft_trigger_set(int i)
{
	const PortValueList& l = soft_trigger_list();
	if (i < 0 || i >= l.n)
		return 0;
	g_softTrigger = l.v[i] * 255 / 100; // as init() does
	return 1;
}

int rumble_count() { return rumble_list().n; }
const char* rumble_name(int i)
{
	const PortValueList& l = rumble_list();
	return i >= 0 && i < l.n && l.v[i] == 0 ? "Off" : percent_name(l, i);
}
int rumble_get() { return rumble_list().indexOf(rumble_percent()); }
int rumble_set(int i)
{
	const PortValueList& l = rumble_list();
	if (i < 0 || i >= l.n)
		return 0;
	// A strength above 0 is all it takes to turn the motor back on from Off: update_rumble's only
	// switches are the game's own motor request, focus and this strength. It is sent at once, so a
	// motor the game has running follows the new strength.
	g_rumbleStrength = (u16)(l.v[i] * 0xFFFF / 100);
	update_rumble();
	return 1;
}

const PortSetting kPadSettings[] = {
	{ "softTrigger", PORT_SETTINGS_PAGE_CONTROLS, 10, "Soft Trigger Depth",
	  "How far the soft L and R keys press, out of 100. Only if they are bound.", 0, NULL, soft_trigger_count,
	  soft_trigger_name, soft_trigger_get, soft_trigger_set },
	{ "rumbleStrength", PORT_SETTINGS_PAGE_CONTROLS, 20, "Rumble Strength",
	  "Rumble strength. Off turns it off. Some adapters are on or off only.", 0, NULL, rumble_count,
	  rumble_name, rumble_get, rumble_set },
	{ "invertCameraX", PORT_SETTINGS_PAGE_GAMEPLAY, 50, "Invert Camera X",
	  "Flips the C-stick and mouse look left and right.", 0, NULL, off_on_count, off_on_name, invert_x_get,
	  invert_x_set },
	{ "invertCameraY", PORT_SETTINGS_PAGE_GAMEPLAY, 55, "Invert Camera Y",
	  "Flips the C-stick and mouse look up and down.", 0, NULL, off_on_count, off_on_name, invert_y_get,
	  invert_y_set },
	{ "freeCamera", PORT_SETTINGS_PAGE_GAMEPLAY, 60, "Free Camera",
	  "The camera stays where you put it instead of swinging back behind Mario.", 0, NULL, off_on_count,
	  off_on_name, free_camera_get, free_camera_set },
	{ "cameraSpeed", PORT_SETTINGS_PAGE_GAMEPLAY, 70, "Camera Speed",
	  "How fast the C-stick turns the camera. 100 is the original speed.", 0, NULL, camera_speed_count,
	  camera_speed_name, camera_speed_get, camera_speed_set },
	{ "mouseSensitivity", PORT_SETTINGS_PAGE_GAMEPLAY, 85, "Mouse Sensitivity",
	  "How fast mouse look turns the camera, when mouse look is on. 100 is normal.", 0, NULL, mouse_sens_count,
	  mouse_sens_name, mouse_sens_get, mouse_sens_set },
};

} // namespace

extern "C" const PortSetting* port_pad_settings(int* count)
{
	*count = (int)(sizeof kPadSettings / sizeof kPadSettings[0]);
	return kPadSettings;
}

// PADClamp comes from the decomp (libs/dolphin/src/pad/Padclamp.c, see CMakeLists.txt).
