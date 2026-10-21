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
} PortExtension;

// Adds an extension; call from a static constructor or before the first frame.
void port_ext_register(const PortExtension* ext);
// The registered extension called `name`, or null.
const PortExtension* port_ext_find(const char* name);

// The hooks, as the game's patches call them.
void port_ext_frame(void);
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

#ifdef __cplusplus
}
#endif

#endif
