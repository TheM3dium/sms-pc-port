# ext/: extensions

Features built into the game that sit beside it rather than in it: the game calls them at a few chosen points, and each keeps its own code in its own folder here.
They are the start of a code-mod framework for the port: a new feature that needs to run every frame, react to a stage loading, or add objects to a stage does it through these hooks instead of patching the game in many places.

| Folder | What it is |
| --- | --- |
| [`core/`](core/port_ext.cpp) | the registry: the game's hooks call each registered extension in turn |
| [`multiplayer/`](multiplayer/README.md) | up to eight players in one game over the network |
| [`demo/`](demo/README.md) | plays Mario from a script, as a player holds the controller, for trailers and footage |
| [`autosave/`](autosave/README.md) | saves by itself where the game would ask to save, with an indicator and the save sound |
| [`outfit/`](outfit/README.md) | Mario always wears his Shine shirt, his sunglasses, or both (the launcher's Customize Mario) |
| [`achievements/`](achievements/README.md) | the game's achievements: unlocked as you play, kept per player, announced to the launcher |
| [`modapi/`](modapi/README.md) | native mods: loads mods from `mods/` (shared libraries) and turns these hooks into their events ([MODDING.md](https://github.com/chasem-dev/sms-mod-sdk/blob/main/docs/MODDING.md)) |

## The hooks

The game calls the functions of [`src/port_include/port_ext.h`](../src/port_include/port_ext.h) from its patches, [`decomp-patches/zzzzz-ext-01-hooks.patch`](../decomp-patches/zzzzz-ext-01-hooks.patch) and the other `zzzzz-ext-*` patches.
An extension fills in a `PortExtension` with the hooks it needs, leaving the rest null:

| Hook | When the game calls it | What it may do |
| --- | --- | --- |
| `frame` | the top of every frame of `TApplication`'s loop: title, file select, stages and movies alike | anything between frames |
| `draw` | every frame drawn, over the game's picture, HUD and fades and under the port's own menus, in the 640 x 448 screen's coordinates | draw 2D over the game (the achievements' banner) |
| `afterMovement` | every simulation tick of a stage, after the movement pass and before anything is posed for drawing | move objects to where they should be |
| `stageSetup` | a stage's objects are loaded and set up (the end of `TMarDirector::setupObjects`), with the stage's heap current | add objects to the stage; what it allocates lasts as long as the stage |
| `allowNextStage` | `TMarDirector::setNextStage` asks to leave for another area (pipes, paintings, scripts) | return 0 to refuse |
| `moveStage` | `TMarDirector::moveStage` is about to leave for `gpApplication.mNextArea` (every way out of a stage) | change the destination |
| `missNextArea` | Mario lost a life and has lives left | change where he goes |
| `takeGameOver` | Mario lost his last life | return 1 to carry on as after any lost life instead of the game over screen |
| `keepCoinsOnMiss` | losing a life is about to reset the coins | return 1 to keep them |
| `afterFileSelect` | a file was chosen at file select | change where it starts |
| `itemTaken` | a `TItem` (coin, blue or red coin, 1-up) is picked up | note it |
| `allowSaveStates`, `saveStatesOffReason` | the controls or menu ask for a save state | return 0 to refuse, and say why |
| `hidePlayerModel` | Mario's models are about to be drawn | return 1 to leave them out (and draw the player yourself) |
| `padRead` | the controllers were read (`PADRead`), before the game sees them | change the input (scripted play, input mods) |
| `demoCamera` | a camera cutscene is asked for (`TMarDirector::fireStartDemoCamera`) | note it (multiplayer plays it in every game), or return 1 to leave it out |
| `pollutionClean` | goop is about to be cleaned (`TPollutionManager::clean`) | note it |
| `objectMove` | an object is about to move this tick, and has (`JDrama::TViewObj::testPerform`) | change what it decides by (`gpMarioPos`) until it has moved |
| `objectHit` | the player or their water hits an object | return 1 to stop the hit |
| `particleEmit` | an effect is asked of Mario's particle manager (`TMarioParticleManager::emit` and its kin) | note it (multiplayer shows the player's at them in the other games) |
| `camera` | the game camera is about to make this tick's view (`CPolarSubCamera::perform`) | change the eye, the point looked at, the field of view |
| `playerStatus` | `TMario::changePlayerStatus` | change the status, or return 1 to refuse it |
| `playerDamage` | `TMario::decHP` | change the amount, or return 1 to stop it |
| `playerNozzle` | FLUDD is about to change nozzle (`TWaterGun::changeNozzle`; the nozzle button's `changeBackup`; a stage's start giving the file's nozzle, in `TMarDirector::setMario`), or a nozzle's box is set up (`TNozzleBox::loadAfter`) | change the nozzle, or return 1 to refuse it (a box is then not in the stage) |
| `shineCollected` | `TFlagManager::setShineFlag` sets a Shine's flag for the first time | note it |
| `savePrompt` | the stage is about to ask the player to save, after a Shine, a blue coin, a nozzle's box or Yoshi (not a game over's) | return 1 to leave the prompt out (auto save saves instead) |
| `chooseNextStage` | `TMarDirector::moveStage` is about to leave, before any extension's `moveStage` | change the destination, which every `moveStage` then sees (multiplayer's host sends the players where its mods chose) |
| `saveFile` | at file select (`TCardLoad`), a save file was loaded or started anew, or is copied over another or erased (`PORT_SAVE_FILE_*`) | keep its own per-file data alongside (the mod API's `api->files`) |
| `nozzleFire` | FLUDD's trigger (`TNozzleTrigger::movement`) is about to fire: a burst (A while R is held) or the pressure built up (`PORT_FIRE_*`) | return 1 to stop it |
| `shineModel` | a `TShine` is about to make its model (`TShine::makeMActors`) | register a model in the given keeper and return its name for the Shine to take |
| `yoshiEgg` | whether Yoshi's egg is there (`TEggYoshi::load`, the plaza's egg event in `TMarDirector::setupObjects`; `PORT_EGG_*`) | change it |
| `fileStarting` | a file chosen at file select is about to start (`TCardLoad`'s last step, before `setNextStage`), each frame until it does, with its file and whether it is a new game | return 1 to wait this frame (the file select stays as it is) |
| `episodeShine` | the episode select and the pause menu ask which Shine an episode stands for | change it |
| `fluddRefill` | FLUDD is about to fill up: from the water around (`TWaterGun::suck`), a bottle, fruit or 1-up, or a barrel's hip drop (`PORT_REFILL_*`, and the item or barrel; `zzzzz-ext-26`) | return 1 to stop it |
| `chooseStageLoad`, `stageLoading` | a stage is about to load, its episode chosen (`TApplication`'s `APP_STATE_GAMEPLAY`: after a world's episode select, after `moveStage` picks the plaza's episode), before the movie it opens with; once a load, every `chooseStageLoad` before any `stageLoading` (given the stage the game chose) | load another (`port_ext_load_instead`) or change `mPrevArea`, where Mario comes from (multiplayer's host sends the players where its mods chose, a travelling client loads the host's) |
| `playerStatusChanged` | `TMario::changePlayerStatus` took a new status (after the game's own rewrites), or a spin jump started in the air (`considerJumpRotate`) | note it |
| `playerHeal` | `TMario::incHP`, or a pickup that fills Mario's health at once (a 1-up, a Shine, his cap) | change the amount, or return 1 to stop it |
| `playerDeath` | a `TMario` lost his life (`TMario::loserExec`, the bottomless pit in `thinkSituation`), once, with its cause (`PORT_DEATH_*`; `port_ext_death_cause` set by a caller that knows it) | note it |
| `playerSpawn` | `TMarDirector::setMario` is about to put the player at the stage's start, with its kind (0 waiting, 1 rolling in, 2 back from a world, `PORT_START_COASTER`) | change the place and turn, or give one to a stage that has none; change the kind (a coaster start changed leaves the coaster out: `port_ext_no_coaster_start`) |
| `playerMessage` | `TMario::receiveMessage`: something touched or hit Mario (an enemy's attack, an item) | return 1 and he ignores it |
| `playerMount` | Mario gets on or off Yoshi (`TYoshi::ride`, `TMario::getOffYoshi`) or on a Blooper (`TMario::getGesso`) | note it |
| `playerWater` | Mario goes into the water or comes out of it (`TMario::thinkWaterSurface`) | note it |
| `yoshi` | Yoshi hatches (`TYoshi::appearFromEgg`) or eats a fruit (`TYoshi::doEat`) | note it |
| `actorSpawn` | an enemy is placed by its manager or generator (`TSpineEnemy::resetToPosition`, `resetSRTV`), an item made to appear (`TMapObjBaseManager::makeObjAppear*`) | note it |
| `objectHitDone` | a hit `objectHit` or `objectStomp` let through has reached the object, with what its `receiveMessage` returned | note it (the mod API tells of an enemy's health lost) |
| `actorDeath` | an enemy is defeated (`TSmallEnemy::genRandomItem`, the bosses' defeat nerves), or killed by the stage | note it |
| `shine` | a Shine appears during play (`TShine::appearWithTime`, `appearSimple`), or the player takes one (`TMarDirector::fireGetStar`) | note it |
| `flagChange` | `TFlagManager::setFlag` or `incFlag` is about to change a save-file flag | change the value, or return 1 to leave it |
| `sound` | a sound effect is about to start (`MSoundSE::startSoundActorInner`, every one the game plays) | change it, or return 1 to leave it out (multiplayer notes the player's own and plays them at them in the other games) |
| `music` | `MSBgm::startBGM` is about to start a piece, or `stopTrackBGM` to stop one | change the piece, or return 1 to leave it out |
| `talk` | a talk with an NPC starts or ends (`TMarDirector::updateGameMode`), or shows a message (`TTalk2D2::setMessageID`) | change the message |
| `demoCameraEnd` | a camera cutscene the director queued is over | note it |
| `gameSaved` | the save file is written (`TCardSave`'s pause menu save and prompt, auto save) | note it |
| `timer` | the HUD's timer starts, stops (`TGCConsole2::startMoveTimer`, `stopMoveTimer`) or a countdown runs out (`setTimer`) | note it |
| `scriptCall` | a stage's event script calls one of the game's functions (`TSpcInterp::dispatchBuiltin*`) | note it |
| `mapEvent` | one of the stage's map events is set off (`TMapEvent::perform`: its goop cleaned) | note it |
| `objectStomp` | the player stomps an object (`TMario::trampleExec`; `objectHit` is not asked for stomps) | return 1 to stop it |

Every hook runs on the game thread inside the game's own steps, so an extension uses the game's globals (`gpApplication`, `gpMarDirector`, `gpMarioOriginal`, `TFlagManager`) as the game does there.
With no extension switched on, each hook is a loop over nothing.

## Writing an extension

An extension is a folder `ext/<name>/` with:

- `game/*.cpp`: game code, built into the game's library with the game's flags and headers (C++03, Shift-JIS string literals, the decomp's include paths). It registers itself from a static constructor:
  ```cpp
  namespace {
  PortExtension s_extension = { "name", frame, stageSetup, /* ... */ };
  struct Register { Register() { port_ext_register(&s_extension); } } s_register;
  }
  ```
  Game code's `new` allocates from the game's current JKR heap: allocate in `stageSetup` for things that belong to a stage, and keep fixed arrays for the rest.
- optionally `net/*.cpp`, `sync/*.cpp` and `host/*.cpp`: platform code, built into the executable with the host's compiler settings (C++11 and the host's libraries, sockets and threads included). Keep the two halves apart behind a small C header, as multiplayer does with [`mp_game_api.h`](multiplayer/mp_game_api.h).
- `tests/` folders, left out of the game build, for host tests.

[`ext.cmake`](ext.cmake) picks up every `ext/*/game`, `ext/*/net`, `ext/*/sync` and `ext/*/host` source by itself.
Features that need not be built into the game belong in a native mod instead ([MODDING.md](https://github.com/chasem-dev/sms-mod-sdk/blob/main/docs/MODDING.md)), which players install and switch on and off themselves.
A feature that needs a hook the game does not have yet adds it to `port_ext.h`, `core/port_ext.cpp` and the hooks patch, each hook general enough for other extensions to use.
On screen, `GXPC_ExtOverlaySet` (`platform/gx`) shows a notice near the top of the window and a status panel at its right.
