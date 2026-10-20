#ifndef PORT_SETTINGS_H
#define PORT_SETTINGS_H

// The options of the launcher's settings that can be changed while the game
// runs, for the settings menu the game draws in its pause menu
// (src/settings_menu.cpp). They are the options of the same names in the
// launcher and settings.txt (SMS_GX_SCALE, SMS_MSAA, SMS_VOLUME, ...): a change
// here takes effect at once (or when `deferred` says) and lasts until the game
// closes; what the game starts with is still the launcher's or settings.txt's.
// Started by SMS Launcher, the menu's changes are saved for it to keep
// (port_settings_set), so the next game starts with them.
//
// Every module that owns an option describes it with a PortSetting and returns
// its table from a function (gx_port_settings, port_pad_settings,
// port_audio_settings, port_runtime_settings); platform/port_settings.cpp joins
// them into pages for the menu. Nothing here is a game header: the settings are
// plain C so that the GX layer, the pad layer and the game can all include it.
//
// An option shows one of a short list of values, by index. The list is fixed
// except where the game started with a value that is not in it (SMS_SHARPEN=33,
// say): that value is added in its place, so what the menu shows is always what
// is in force.
//
// The menu draws text in the game's system font, where `%`, `<`, `>` and a few
// other characters are button glyphs: names and hints use plain letters, digits,
// spaces and `. , - ( ) '` only.

#ifdef __cplusplus
extern "C" {
#endif

enum {
	PORT_SETTINGS_PAGE_GRAPHICS,
	PORT_SETTINGS_PAGE_DISPLAY,
	PORT_SETTINGS_PAGE_GAMEPLAY,
	PORT_SETTINGS_PAGE_CONTROLS,
	PORT_SETTINGS_PAGE_COUNT
};

typedef struct PortSetting {
	const char* id;           // the launcher's key for it ("msaa", "frameRate", "invertCameraX", ...)
	int page;                 // PORT_SETTINGS_PAGE_*
	int order;                // position on its page, ascending (ties keep table order)
	const char* name;         // the label
	const char* hint;         // one line about what it does and costs, 80 characters at most
	int deferred;             // 1: it takes effect later (the next area, say), and the hint says when
	// Whether the option is shown: 0 where this machine or this run cannot change it (no window,
	// a driver without the feature, no HDR display). NULL: always.
	int (*available)(void);
	int (*count)(void);                    // how many values it has (at least 1)
	const char* (*valueName)(int value);   // a value's text; valid until the next call
	int (*get)(void);                      // the index of the value in force
	// Applies a value now; 1 done, 0 refused (the old value stays), as when the picture's
	// buffers at a new resolution do not fit in the graphics card's memory.
	int (*set)(int value);
} PortSetting;

// Each module's table; *count is its length. The tables are static.
const PortSetting* gx_port_settings(int* count);        // the picture and the window (gx_settings.cpp)
const PortSetting* gx_port_settings_extra(int* count);  // widescreen, HDR, prompts, texture packs (gx_settings_extra.cpp)
const PortSetting* port_pad_settings(int* count);
const PortSetting* port_audio_settings(int* count);
const PortSetting* port_runtime_settings(int* count);

// The menu's view (platform/port_settings.cpp): the pages in order, and on each the available
// options in `order`.
const char* port_settings_page_name(int page);
int port_settings_page_available(int page);   // 1 when the page has at least one option to show
int port_settings_count(int page);
const PortSetting* port_settings_option(int page, int index);
// Sets an option to a value (as its set()), and when that is done, keeps the change for the launcher:
// SMS Launcher names a file in SMS_SETTINGS_SAVE, where the changes the menu made are written in the
// launcher's own values, for it to keep when the game closes. Without it the changes last until the
// game closes. The value names are what is saved, so a name the launcher does not know is not.
int port_settings_set(const PortSetting* option, int value);

// The picture as it is now, as "2560 x 2112 with 4x MSAA", for the menu's footer.
void port_gfx_describe(char* out, int size);

#ifdef __cplusplus
}
#endif

#endif
