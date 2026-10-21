# ext/modapi: the native mod runtime

Loads native mods from `mods/` and gives them the API of `sms_mod.h`, from the mod SDK ([sms-mod-sdk](https://github.com/chasem-dev/sms-mod-sdk), the `sdk/` submodule).
What mods see is documented in the [mod SDK](https://github.com/chasem-dev/sms-mod-sdk)'s manual, [MODDING.md](https://github.com/chasem-dev/sms-mod-sdk/blob/main/docs/MODDING.md); this page is how it is built.
The SDK is the `sdk/` submodule: the API changes there first (append-only, with a release tag), then `sdk/` moves to that tag and the runtime implements it.

| File | Half | What it does |
| --- | --- | --- |
| [`modapi_bridge.h`](modapi_bridge.h) | both | the C functions the two halves call each other through, in the SDK's types |
| [`game/modapi_game.cpp`](game/modapi_game.cpp) | game (`sms_game`, C++03, the decomp's headers) | an extension (`port_ext.h`) whose hooks become mod events; the reads and writes of Mario, the flags, objects and the camera |
| [`host/mod_runtime.cpp`](host/mod_runtime.cpp) | host (`sms`, C++11) | finding, ordering, loading and unloading mods; events, services, settings, keys, input capture, hot reload; the `core`, `game`, `player`, `camera`, `input`, `config`, `audio`, `native` and `world` tables |
| [`host/mod_draw.cpp`](host/mod_draw.cpp) | host | the `draw` table: each frame's triangles (text from a mipmapped atlas of stb_easy_font's glyphs, the world through this frame's camera), HUD slots |
| [`host/mod_menu.cpp`](host/mod_menu.cpp) | host | the Mods menu (F1) |
| [`platform/gx/src/gx_mod_layer.cpp`](../../platform/gx/src/gx_mod_layer.cpp) | sms_gx | draws the newest frame's triangles over the presented picture on the GL thread (`GXPC_ModLayerSubmit`), mod images, SDL key names |

How a frame goes: the extension's `frame` hook (top of `TApplication`'s loop) calls `modh_frame`, which loads the mods the first time, applies the Mods menu's switches, polls for hot reloads, sends `SMS_EV_FRAME`, then `SMS_EV_DRAW` between `drawBegin` and `drawEnd` (with the menu drawn last), and submits the frame's drawing.
`gx_platform.cpp` presents the game's picture, then `modLayerRender`, then the port's own overlays.
Keys come from the SDL event pump (`GXPC_SetKeyHook`, in `PADRead` on the game thread) after the port's own keys and menus, before the controls; the controllers from the extension's `padRead` hook after `PADRead`.

Script mods (`runtime = <name>` in `mod.ini`) have no library: `loadScript` hands them to the native mod that called `core->host_runtime` for that runtime (the lua mod, the SDK's `lua/`, an optional install: `tools/mods/get.py lua`), after every native mod has loaded; a host's script mods stop before it unloads and start again when it is back (`startWaitingScripts`).

Multiplayer (`api->net`, API 1.2) is the multiplayer extension's, through [`mp_game_api.h`](../multiplayer/mp_game_api.h): the players and where they are, and on the host the server commands, a stage for everyone, the mode's name and state for the launcher's list ([ext/multiplayer/README.md](../multiplayer/README.md#servers-and-server-mods)). What it hears (joined, left, arrived, a new session) becomes `SMS_EV_NET_*` at the top of each frame. A server mod's `client_api` (the mod API players' games need) goes to the multiplayer extension once mods are loaded (`mpg_set_min_client_api`), which refuses older games as they join. A mod whose `mod.ini` says `side = server` loads only in a game hosting multiplayer (in any other it is listed, off, saying so); mods load once multiplayer has started, so its role is known while they load.

Worlds (`api->world`, API 1.4) are the game half's reads of the game's own tables, as its episode select and pause menu read them (`GC2D/ShineTable.hpp`, `SMS_getShineStage`); the host half answers "none" in Super Mario Eclipse builds, whose stages are numbered their own way. Cutscenes are queued as the game queues its own (`TMarDirector::fireStartDemoCamera`), only while the stage is played with nothing else on.
Why a stage is left (`SmsEvStage.reason`) is read as it goes, from the director's state and what asked since the stage was set up (a way out's `setNextStage`, a mod's `game->warp`, a lost life); `SMS_EV_STAGE_LEAVING` is sent from the extension's `chooseNextStage` hook, which runs before every extension's `moveStage`, so the multiplayer host tells its players the stage a mod chose. A mod that names a world's episode gets what the episode select does (`0x40003`, the episode) and, as a new visit, no coins (`0x40002`). `SMS_EV_STAGE_START` is the first frame the director is in its play state (4) after the stage's opening.

Ownership: every subscription, service, key binding, option, image and the input capture is recorded against its mod, so unloading a mod (exit, the Mods menu, hot reload) drops all of it before the library is closed.
Handlers added or removed during a dispatch take effect when it ends.

Testing without a display: `SDL_VIDEODRIVER=offscreen` runs a real window (the overlay draws), `SMS_WINDOW_SHOTS=frame,...` captures the window with the mods' drawing, and `SMS_NATIVE_MODS_DIR` / `SMS_NATIVE_MODS_DATA` keep a test's mods and settings in a folder of its own.
