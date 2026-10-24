#ifndef PORT_EXT_H
#define PORT_EXT_H

// Extensions (ext/, see ext/README.md): optional features built into the game
// that it calls at a few chosen points. An extension fills in a PortExtension
// with the hooks it needs (the others null) and registers it before the game
// starts; the game's patches (zzzzz-ext-*) call the port_ext_* functions
// below, which ask every registered extension in turn. With no extension
// switched on, each hook is a loop over nothing and the game plays as before.
//
// Every hook runs on the game thread, between or inside the game's own steps,
// so an extension may use the game's globals (gpApplication, gpMarDirector,
// gpMarioOriginal, TFlagManager) as the game does there.

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PortExtension {
	const char* name;
	// The top of every frame of TApplication's loop (title, file select,
	// stages and movies alike), before the frame's work.
	void (*frame)(void);
	// Every simulation tick of a stage, after everything has moved and
	// before anything is posed for drawing (TMarDirector::direct, after the
	// movement pass): an extension can put objects where they should be.
	void (*afterMovement)(void);
	// A stage's objects are loaded and set up (the end of
	// TMarDirector::setupObjects); the stage's heap is the current one, so
	// what is allocated here lasts as long as the stage.
	void (*stageSetup)(void);
	// TMarDirector::setNextStage(code, actor) asks to leave for another area;
	// return 0 to refuse (the game carries on where it is).
	int (*allowNextStage)(unsigned code, void* actor);
	// TMarDirector::moveStage is about to leave for gpApplication.mNextArea,
	// which the hook may change (an explicit scenario skips episode select).
	void (*moveStage)(void);
	// Mario lost a life and has lives left: gpApplication.mNextArea is where
	// he goes next, which the hook may change.
	void (*missNextArea)(void);
	// Mario lost his last life: return 1 to carry on as after any other
	// lost life (missNextArea says where) instead of the game over screen.
	int (*takeGameOver)(void);
	// Losing a life resets the coin count unless this returns 1.
	int (*keepCoinsOnMiss)(void);
	// A file was chosen at file select: gpApplication.mNextArea is where it
	// starts, which the hook may change.
	void (*afterFileSelect)(void);
	// A TItem (coin, blue or red coin, 1-up...) was picked up by `taker`.
	void (*itemTaken)(void* item, void* taker);
	// Return 0 to switch save states off (the controls then say why).
	int (*allowSaveStates)(void);
	// Why save states are off, for the message the controls show.
	const char* (*saveStatesOffReason)(void);
	// Return 1 to leave out the player's own models (body, hands, cap,
	// FLUDD) when `mario` is drawn; the extension draws the player itself.
	// Yoshi and the shadow are drawn as ever.
	int (*hidePlayerModel)(void* mario);
	// The controllers were just read (PADRead): `status` is the PADStatus[4]
	// the game is about to see, which the hook may change (scripted input,
	// input mods). Called from the game thread, before the game reads them.
	void (*padRead)(void* status);
	// A camera cutscene was asked for (TMarDirector::fireStartDemoCamera):
	// its camera's name, the point it centres on (or null), its arguments,
	// whether a callback or an actor came with it, and its flags. Return 1 to
	// leave it out (it played already: another game's, played here).
	int (*demoCamera)(const char* name, const float* pos, int frames, float angle, int option, int hasCallback,
	                  void* actor, unsigned flags);
	// Goop was cleaned at x, y, z over `size` (TPollutionManager::clean, as
	// FLUDD's water and Mario's feet do), about to be.
	void (*pollutionClean)(float x, float y, float z, float size);
	// An object (a JDrama::TViewObj) is about to move this tick (`after` 0),
	// or has (`after` 1): each "before" has its "after", nested as objects
	// move their parts. Many objects decide things by where Mario is
	// (gpMarioPos), which the hook may point elsewhere until the "after".
	void (*objectMove)(void* object, int after);
	// This game's player, or its water, hit `object` (a THitActor) with
	// `message` (HIT_MESSAGE_*: sprayed, ground-pounded, punched,
	// stomped...), from `sender` (the player, or the water's hit actor):
	// return 1 to stop it from reaching the object.
	int (*objectHit)(void* object, void* sender, unsigned message);
	// An effect was asked of Mario's particle manager (gpMarioParticleManager,
	// TMarioParticleManager::emit and its kin, `how` saying which: 0 emit, 1
	// emitWithRotate, 2 emitAndBindToPosPtr, 3 emitAndBindToMtxPtr, 4
	// emitAndBindToSRTMtxPtr, 5 emitAndBindToMtx): its effect, where (a
	// position for 0 to 2, a matrix for 3 to 5), the turn (1), its group and
	// owner, about to be.
	void (*particleEmit)(int how, int id, const void* at, int rx, int ry, int rz, int group, const void* owner);
	// The game camera (CPolarSubCamera) placed itself this tick and is about
	// to make the view it draws from its eye, the point it looks at and its
	// vertical field of view in degrees (copies, each a float[3] but fovy),
	// which the hook may change; the eye and the point it looks at change
	// only what is drawn this tick (the extension may set the camera's own
	// field of view as well).
	void (*camera)(void* camera, float* eye, float* at, float* fovy);
	// TMario::changePlayerStatus(*status, *arg, force) on any TMario (the
	// player's, or Shadow Mario's): the hook may change the status asked
	// for and its argument, or return 1 to refuse it (the call returns 0).
	int (*playerStatus)(void* mario, unsigned* status, unsigned* arg, int force);
	// TMario::decHP(*amount) on any TMario: the hook may change the amount
	// (health, or air under water), or return 1 to stop it.
	int (*playerDamage)(void* mario, int* amount);
	// TFlagManager::setShineFlag set Shine `shine`'s flag for the first
	// time; `total` Shines are on the file now.
	void (*shineCollected)(int shine, int total);
	// The stage is about to ask the player to save (TMarDirector's card save
	// prompt), for `kind`: 1 a blue coin, 3 or 4 a nozzle's box opened for
	// good, 5 Yoshi hatched, 6 a Shine. Return 1 to leave the prompt out (the
	// extension saved, or the game need not); the stage goes on as after it.
	int (*savePrompt)(int kind);
	// TMarDirector::moveStage is about to leave for gpApplication.mNextArea,
	// which the hook may change: every extension's chooseNextStage runs
	// before any extension's moveStage, so those see where the game goes.
	void (*chooseNextStage)(void);
	// FLUDD of a TMario (the player's, or Shadow Mario's) is about to change
	// nozzle, for `reason` (PORT_NOZZLE_*), or a nozzle's box is set up as
	// the stage loads (PORT_NOZZLE_BOX: `mario` null, `box` the TNozzleBox).
	// `*nozzle` is the one asked for (TWaterGun::TNozzleType), which the hook
	// may change (not for a box, nor the swap back to the spray; a number
	// FLUDD has no nozzle for is not taken), or return 1 to refuse it (a box
	// is then not in the stage).
	int (*playerNozzle)(void* mario, void* box, int* nozzle, int animate, int reason);
	// TMario::changePlayerStatus on any TMario took a new status: `from` is
	// the one he had, `to` the one he has now (after the game's own rewrite,
	// a jump on a bouncy floor, say), with its argument. Also when a spin
	// jump starts in the air (TMario::considerJumpRotate).
	void (*playerStatusChanged)(void* mario, unsigned from, unsigned to, unsigned arg);
	// TMario::incHP(*amount) on any TMario, or a pickup that fills his health
	// at once (a 1-up, a Shine, his cap): the hook may change the amount, or
	// return 1 to stop it. `air`: under water, air is gained, not health.
	int (*playerHeal)(void* mario, int* amount, int air);
	// A TMario has just lost his life (TMario::loserExec, or the stage's
	// bottomless pit), for `cause` (PORT_DEATH_*), once per death.
	void (*playerDeath)(void* mario, int cause);
	// TMarDirector::setMario is about to put the player at the stage's start:
	// `pos` (float[3]) and `yaw` (degrees), which the hook may change;
	// `*hasPos` 0 when the stage puts him where its scene file does (the
	// hook may set 1 to place him), `kind` the stage's way in (0 waiting, 1
	// rolling, 2 returning, 4 the Pinna Park coaster).
	void (*playerSpawn)(float* pos, float* yaw, int* hasPos, int* kind);
	// TMario::receiveMessage on any TMario: `sender` touched or hit him with
	// `message` (HIT_MESSAGE_*: an enemy's attack, fire, an item taken...).
	// Return 1 and he ignores it (receiveMessage returns false).
	int (*playerMessage)(void* mario, void* sender, unsigned message);
	// A TMario got on (on 1) or off (on 0) what he rides: PORT_MOUNT_YOSHI
	// (`object` the TYoshi) or PORT_MOUNT_BLOOPER (the surf Blooper; null
	// when getting off).
	void (*playerMount)(void* mario, int mount, int on, void* object);
	// A TMario went into the water (entered 1) or came out of it (0)
	// (TMario::thinkWaterSurface), at the water's surface `surfaceY`.
	void (*playerWater)(void* mario, int entered, float surfaceY);
	// Yoshi (`yoshi`, a TYoshi): PORT_YOSHI_HATCH out of his egg (`fruit` the
	// fruit's actor type that hatched him, or 0), PORT_YOSHI_EAT a fruit
	// (`fruit` its actor type).
	void (*yoshi)(void* yoshi, int what, unsigned fruit);
	// An actor came into the stage while it is played: an enemy (re)placed
	// by its manager or generator (TSpineEnemy::resetToPosition, resetSRTV),
	// or an item made to appear (TMapObjBaseManager::makeObjAppear*), as a
	// THitActor.
	void (*actorSpawn)(void* actor);
	// The hit objectHit (or objectStomp) let through has reached the object:
	// `result` is what its receiveMessage returned. Each objectHit or
	// objectStomp that returned 0 has one.
	void (*objectHitDone)(void* object, void* sender, unsigned message, int result);
	// An enemy was defeated (`forced` 0: its defeat's end, as it leaves its
	// coins; a boss as it falls) or killed by the stage (`forced` 1: a pit,
	// the death plane). `actor` is a THitActor.
	void (*actorDeath)(void* actor, int forced);
	// A TShine: PORT_SHINE_APPEAR it appears during play (an event's, a
	// boss's, the red coins'), PORT_SHINE_GET the player took it (every
	// time, a Shine the file has too). `shine` is a THitActor.
	void (*shine)(void* shine, int what);
	// TFlagManager::setFlag / incFlag is about to change save-file flag
	// `flag` from `old` to `*value`, which the hook may change, or return 1
	// to leave the flag as it is. Not for the loads and resets of the whole
	// table (a file loaded, a stage's flags reset).
	int (*flagChange)(unsigned flag, int old, int* value);
	// A sound effect is about to start (MSoundSE::startSoundActorInner, every
	// sound effect the game plays): `*id` (MSD_SE_*), which the hook may
	// change, where (`pos` a float[3], or null), and whether it is a system
	// sound (no place). Return 1 to leave it out. Many a frame.
	int (*sound)(unsigned* id, const float* pos, int system);
	// Music: MSBgm::startBGM(*id) is about to start a piece (start 1: the
	// hook may change `*id`, or return 1 to leave it out), or
	// MSBgm::stopTrackBGM is about to stop one (start 0).
	int (*music)(unsigned* id, int start);
	// A talk with an NPC (PORT_TALK_*): it starts (`npc` the TBaseNPC), a
	// message is shown (`*message` its id, which the hook may change;
	// `flags` bit 0 the talk's last), or it ends.
	void (*talk)(int what, void* npc, unsigned* message, unsigned flags);
	// A camera cutscene the director queued (fireStartDemoCamera) is over:
	// its camera's name (null for an actor's), the actor, and whether it
	// was cut short.
	void (*demoCameraEnd)(const char* name, void* actor, int forced);
	// The game was saved to file `slot` (the memory card's write is done:
	// the pause menu's save, the save prompt, auto save).
	void (*gameSaved)(int slot);
	// The HUD's timer (PORT_TIMER_*): started (`time` the limit, hundredths
	// of a second), stopped (`time` where it stopped), or a countdown ran
	// out.
	void (*timer)(int what, int time);
	// A stage's event script calls one of the game's functions (`name`,
	// "startTimer", "setNextStage"...; TSpcInterp::dispatchBuiltinDefault)
	// with `argc` arguments, from `owner` (its TEventWatcher, or an actor:
	// a JDrama::TNameRef).
	// Many a frame: scripts poll.
	void (*scriptCall)(const char* name, void* owner, int argc);
	// One of the stage's map events is set off (TMapEvent::perform): its goop
	// was cleaned and a building rises, a gate opens. `event` is the
	// TMapEvent, as a JDrama::TNameRef.
	void (*mapEvent)(void* event);
	// The player stomps `object` (a THitActor; TMario::trampleExec, with
	// HIT_MESSAGE_TRAMPLE from `mario`, as a THitActor). objectHit is not asked for stomps
	// (multiplayer shares them its own way). Return 1 to stop it.
	int (*objectStomp)(void* object, void* mario);
	// Every frame drawn, over the game's picture, its HUD and fades and under
	// the port's own menus (TApplication's loop, before the save state menu):
	// 2D in the game's screen coordinates, `width` x `height` (640 x 448).
	void (*draw)(float width, float height);
	// A stage is about to load, its episode chosen (TApplication::proc's
	// APP_STATE_GAMEPLAY, before the movie some stages open with and the
	// stage's TMarDirector): gpApplication.mCurrArea is the stage, mPrevArea
	// where Mario comes from (the plaza starts him by that world's way in).
	// The hook may load another stage (port_ext_load_instead) or change
	// mPrevArea. Once a load (not again after the opening movie); every
	// extension's chooseStageLoad runs before any extension's stageLoading,
	// which is given the stage the game chose (`area`, `episode`) and sees
	// mCurrArea, what loads.
	void (*chooseStageLoad)(void);
	void (*stageLoading)(int area, int episode);
	// At file select, a save file was loaded or started anew, or copied over
	// another or erased (PORT_SAVE_FILE_*; `from` the file copied, else -1).
	// TCardLoad, as the card is asked to write (copy, erase) or has read.
	void (*saveFile)(int what, int file, int from);
	// FLUDD's trigger (TNozzleTrigger::movement) of a TMario is about to
	// fire, `how` PORT_FIRE_*, with `nozzle` out: return 1 to stop it (a burst
	// is asked again at the next A; the pressure's is spent until R is let go).
	int (*nozzleFire)(void* mario, int nozzle, int how);
	// A TShine is about to make its model (TShine::makeMActors); `collected`:
	// the file has it (its see-through model). Return the name of a model the
	// hook registered in `keeper` (a TMActorKeeper's model data keeper) for
	// it to take, or null for the game's. The first that names one wins.
	const char* (*shineModel)(void* shine, void* keeper, int collected);
	// Whether Yoshi's egg is there (PORT_EGG_*; `egg` the TEggYoshi, or null
	// for the plaza's egg event): `*present` may be changed.
	void (*yoshiEgg)(int kind, void* egg, int* present);
	// The Shine an episode (0..7) of world `shineStage` (SMS_getShineStage's
	// number) stands for, where the episode select and the pause menu ask
	// (its name, whether it is collected): `*shine` may be changed.
	void (*episodeShine)(int shineStage, int episode, int* shine);
	// A file chosen at file select is about to start (TCardLoad's last step,
	// before setNextStage), each frame until it does: `file` 0..2 (-1 without
	// a memory card), `isNew` a new game. Return 1 to wait this frame (the
	// file select stays as it is).
	int (*fileStarting)(int file, int isNew);
	// FLUDD of a TMario is about to fill up, from `source` (PORT_REFILL_*;
	// `from` the item or barrel, null for the water around): return 1 to stop
	// it (the tank stays as it is).
	int (*fluddRefill)(void* mario, int source, void* from);
} PortExtension;

// fluddRefill's `source`
enum {
	PORT_REFILL_SUCK,   // the water around (TWaterGun::suck: sea, pools, fountains)
	PORT_REFILL_ITEM,   // a bottle, fruit, a 1-up (TWaterGun::addWater)
	PORT_REFILL_BARREL  // a barrel's hip drop (TMario::hitBarrel)
};

// saveFile's `what` (the mod API's SMS_SAVE_FILE_*)
enum {
	PORT_SAVE_FILE_LOADED,
	PORT_SAVE_FILE_NEW,
	PORT_SAVE_FILE_SAVED,
	PORT_SAVE_FILE_COPIED,
	PORT_SAVE_FILE_ERASED
};
// nozzleFire's `how` (SMS_FIRE_*)
enum { PORT_FIRE_BURST, PORT_FIRE_PRESSURE };
// yoshiEgg's `kind` (SMS_EGG_*)
enum { PORT_EGG_STAGE, PORT_EGG_PLAZA_EVENT, PORT_EGG_EVENT_EGG };
// playerSpawn's start kinds: TMarDirector::setMario's 0 waiting, 1 rolling
// in, 2 back from a world, and Pinna Park's coaster (SMS_START_*)
enum { PORT_START_COASTER = 4 };

// playerDeath's causes (the mod API's SMS_DEATH_*)
enum {
	PORT_DEATH_OTHER,    // a script (a timed event failed), the coaster
	PORT_DEATH_HEALTH,   // no health left
	PORT_DEATH_DROWN,    // no air left
	PORT_DEATH_PIT,      // a bottomless pit, the death plane
	PORT_DEATH_GOOP,     // sinking goop, deadly goop
	PORT_DEATH_ELECTRIC, // electric goop
	PORT_DEATH_CRASH     // a Blooper crashed into a wall
};

// playerMount's mounts
enum { PORT_MOUNT_YOSHI, PORT_MOUNT_BLOOPER };

// yoshi's what
enum { PORT_YOSHI_HATCH, PORT_YOSHI_EAT };

// shine's what
enum { PORT_SHINE_APPEAR, PORT_SHINE_GET };

// talk's what
enum { PORT_TALK_START, PORT_TALK_MESSAGE, PORT_TALK_END };

// timer's what
enum { PORT_TIMER_START, PORT_TIMER_STOP, PORT_TIMER_OUT };

// playerNozzle's reasons (the mod API's SMS_NOZZLE_*)
enum {
	PORT_NOZZLE_SWITCH,     // TWaterGun::changeNozzle: a pickup, Yoshi, the game's own
	PORT_NOZZLE_SWAP,       // TWaterGun::changeBackup: the nozzle button
	PORT_NOZZLE_BOX,        // TNozzleBox::loadAfter
	PORT_NOZZLE_STAGE_START // TMarDirector::setMario: the file's second nozzle
};

// Adds an extension; call from a static constructor or before the first frame.
void port_ext_register(const PortExtension* ext);
// The registered extension called `name`, or null.
const PortExtension* port_ext_find(const char* name);

// The hooks, as the game's patches call them.
void port_ext_frame(void);
void port_ext_draw(float width, float height);
void port_ext_after_movement(void);
void port_ext_stage_setup(void);
int port_ext_allow_next_stage(unsigned code, void* actor);
void port_ext_move_stage(void);
void port_ext_miss_next_area(void);
int port_ext_keep_coins_on_miss(void);
int port_ext_take_game_over(void);
void port_ext_after_file_select(void);
void port_ext_item_taken(void* item, void* taker);
// 1 when save states may be used; otherwise 0 and *reason says why.
int port_ext_allow_save_states(const char** reason);
int port_ext_hide_player_model(void* mario);
void port_ext_pad_read(void* status);
int port_ext_demo_camera(const char* name, const float* pos, int frames, float angle, int option, int hasCallback,
                         void* actor, unsigned flags);
void port_ext_pollution_clean(float x, float y, float z, float size);
void port_ext_object_move(void* object, int after);
int port_ext_object_hit(void* object, void* sender, unsigned message);
void port_ext_particle_emit(int how, int id, const void* at, int rx, int ry, int rz, int group, const void* owner);
void port_ext_camera(void* camera, float* eye, float* at, float* fovy);
int port_ext_player_status(void* mario, unsigned* status, unsigned* arg, int force);
int port_ext_player_damage(void* mario, int* amount);
void port_ext_shine_collected(int shine, int total);
int port_ext_save_prompt(int kind);
int port_ext_player_nozzle(void* mario, void* box, int* nozzle, int animate, int reason);
// While set, TWaterGun::changeNozzle does not ask playerNozzle: its caller
// asked already (a stage's start), or it is a step of a swap that was asked.
extern int port_ext_nozzle_asked;
void port_ext_player_status_changed(void* mario, unsigned from, unsigned to, unsigned arg);
int port_ext_player_heal(void* mario, int* amount, int air);
void port_ext_player_death(void* mario, int cause);
void port_ext_player_spawn(float* pos, float* yaw, int* hasPos, int* kind);
// Set by setMario when an extension changed the coaster start to another:
// TMario::isUnUsualStageStart leaves the coaster out.
extern int port_ext_no_coaster_start;
void port_ext_save_file(int what, int file, int from);
int port_ext_nozzle_fire(void* mario, int nozzle, int how);
const char* port_ext_shine_model(void* shine, void* keeper, int collected);
void port_ext_yoshi_egg(int kind, void* egg, int* present);
void port_ext_episode_shine(int shineStage, int episode, int* shine);
// 1 while an extension keeps the chosen file from starting (fileStarting).
int port_ext_file_starting(void);
int port_ext_player_message(void* mario, void* sender, unsigned message);
void port_ext_player_mount(void* mario, int mount, int on, void* object);
void port_ext_player_water(void* mario, int entered, float surfaceY);
int port_ext_fludd_refill(void* mario, int source, void* from);
void port_ext_yoshi(void* yoshi, int what, unsigned fruit);
void port_ext_actor_spawn(void* actor);
void port_ext_object_hit_done(void* object, void* sender, unsigned message, int result);
void port_ext_actor_death(void* actor, int forced);
void port_ext_shine(void* shine, int what);
int port_ext_flag_change(unsigned flag, int old, int* value);
int port_ext_sound(unsigned* id, const float* pos, int system);
int port_ext_music(unsigned* id, int start);
void port_ext_talk(int what, void* npc, unsigned* message, unsigned flags);
void port_ext_demo_camera_end(const char* name, void* actor, int forced);
void port_ext_game_saved(int slot);
void port_ext_timer(int what, int time);
void port_ext_script_call(const char* name, void* owner, int argc);
void port_ext_map_event(void* event);
int port_ext_object_stomp(void* object, void* mario);
// TApplication's APP_STATE_GAMEPLAY, before its opening movie check:
// `afterMovie` 1 on the pass that follows the stage's opening movie (the same
// load: chooseStageLoad and stageLoading are not asked again).
void port_ext_stage_loading(int afterMovie);
// For chooseStageLoad and stageLoading: load `area`, `episode` instead of
// gpApplication.mCurrArea, as if the game had chosen it (mNextArea too when it
// was the same stage; a world's episode recorded as its episode select does;
// a new visit's coins afresh). 0 when the game has no such stage, or it is the
// title (area 15), to or from; 1 when taken (or it is the stage already).
int port_ext_load_instead(int area, int episode);
// Whether the game has a stage for `area`, `episode` (its scene table).
int port_ext_stage_exists(int area, int episode);
// port_ext.cpp's side of port_ext_stage_loading (ext/core/game/port_stage_load.cpp).
void port_ext_choose_stage_load(int area, int episode);
// What hurts the player as TMario::damageExec takes his health (the enemy,
// or his own floor actor for the ground's damage), while it does; else null.
extern void* port_ext_damage_source;
// The cause the next playerDeath gives (PORT_DEATH_*), set just before
// TMario::loserExec by a caller that knows it (sinking goop); OTHER: worked
// out from Mario as he dies.
extern int port_ext_death_cause;

// ------------------------------------------------------------------ the HUD
// Parts of the game's own HUD (TGCConsole2) left out of its drawing. Each
// owner (multiplayer's SMS_CMD_HUD, the mod API's game->set_hud_hidden) says
// which parts it hides; the game hides the union, with the settings menu's
// HUD page's (port_hud_settings.h, asked every draw). A part is hidden for the
// HUD's draw only and put back after it (ext/core/game/port_hud.cpp), so the
// HUD's own logic, its animations and sounds, never sees it hidden. The bits
// are the mod API's SMS_HUD_* (and multiplayer's MPG_HUD_*).
#define PORT_HUD_COINS      1u
#define PORT_HUD_SHINES     2u
#define PORT_HUD_BLUE_COINS 4u
#define PORT_HUD_HEALTH     8u
#define PORT_HUD_WATER      16u
#define PORT_HUD_LIVES      32u
#define PORT_HUD_RED_COINS  64u
#define PORT_HUD_ALL        0xFFFFu
enum { PORT_HUD_BY_MULTIPLAYER = 0, PORT_HUD_BY_MODS = 1, PORT_HUD_OWNERS = 2 };
void port_hud_set_hidden(int owner, unsigned mask);
// The union: every owner's parts.
unsigned port_hud_hidden(void);
// TGCConsole2::perform's draw pass is about to draw the HUD (`after` 0), or
// has (`after` 1): the game's patch calls it, `console` the TGCConsole2.
void port_hud_draw(void* console, int after);

#ifdef __cplusplus
}
#endif

#endif
