// The native mod API's game half (ext/modapi/README.md): an extension whose
// hooks become mod events (modh_dispatch in the host half), and the reads and
// writes of the game that the SmsApi tables offer. Built into the game's
// library with the game's flags and headers (C++03); every function runs on
// the game thread.
//
// Objects reach mods as JDrama::TNameRef pointers, the root of every named
// game object, so modg_object_* can ask what an object is (dynamic_cast)
// before reading it as one.
#include "../modapi_bridge.h"
#include "port_ext.h"
#include "port_framerate.h"

#include <Camera/Camera.hpp>
#include <Camera/CameraBck.hpp>
#include <Camera/CameraMapTool.hpp>
#include <Camera/CubeManagerBase.hpp>
#include <GC2D/MessageUtil.hpp>
#include <GC2D/ShineTable.hpp>
#include <JSystem/JKernel/JKRFileLoader.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <JSystem/JDrama/JDRPlacement.hpp>
#include <MSound/MSound.hpp>
#include <Map/Map.hpp>
#include <MoveBG/Item.hpp>
#include <MoveBG/ItemManager.hpp>
#include <NPC/NpcBase.hpp>
#include <Enemy/Enemy.hpp>
#include <Player/Mario.hpp>
#include <Player/MarioCap.hpp>
#include <Player/WaterGun.hpp>
#include <Player/Yoshi.hpp>
#include <Strategic/HitActor.hpp>
#include <System/Application.hpp>
#include <System/FlagManager.hpp>
#include <System/MarDirector.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DMaterial.hpp>
#include <JSystem/J3D/J3DGraphLoader/J3DModelLoader.hpp>
#include <M3DUtil/SDLModel.hpp>
#include <Strategic/ObjModel.hpp>
#include <dolphin/dvd.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

bool s_stageReady; // between a stage's setup and leaving it
uint32_t s_tick;   // ticks since the stage's setup
bool s_started;    // SMS_EV_STAGE_START sent for this stage

// Why the game leaves (SmsEvStage.reason): what asked since the stage was set
// up, read when it goes (chooseNextStage); kept for the next stage's events.
bool s_wayOutAsked, s_warpAsked, s_missAsked;
int s_lastReason = SMS_LEAVE_OTHER, s_lastShine = -1;
int s_stageShine = -1; // the last Shine collected in this stage

SmsObject obj(JDrama::TNameRef* p) { return (SmsObject)p; }
JDrama::TNameRef* ref(SmsObject o) { return (JDrama::TNameRef*)o; }

TMario* mario()
{
	return s_stageReady ? gpMarioOriginal : 0;
}

int gameMode()
{
	switch (gpApplication.mAppState) {
	case TApplication::APP_STATE_BOOT:
	case TApplication::APP_STATE_NLOGO:
		return SMS_MODE_BOOT;
	case TApplication::APP_STATE_GAMEPLAY:
		return SMS_MODE_STAGE;
	case TApplication::APP_STATE_MOVIE:
		return SMS_MODE_MOVIE;
	case TApplication::APP_STATE_TITLE:
		return SMS_MODE_TITLE;
	case TApplication::APP_STATE_MENU:
		return SMS_MODE_MENU;
	default:
		return SMS_MODE_OTHER;
	}
}

void setVec(SmsVec3* v, f32 x, f32 y, f32 z)
{
	v->x = x;
	v->y = y;
	v->z = z;
}

// ------------------------------------------------------------------ hooks

void stageStart();
void watchModeAndPause();

void frame()
{
	static uint32_t s_frame;
	if (gameMode() != SMS_MODE_STAGE)
		s_stageReady = false;
	stageStart();
	watchModeAndPause();
	modh_frame(s_frame++, gameMode());
}

// The pause menu, the map and the save screens: the stage stands still (its
// objects get no move cue), though its ticks still run.
bool paused()
{
	if (!s_stageReady || !gpMarDirector)
		return false;
	switch (gpMarDirector->mState) {
	case TMarDirector::STATE_PAUSE_MENU:
	case TMarDirector::STATE_CARD_SAVE:
	case TMarDirector::STATE_UNK12:
	case TMarDirector::STATE_GUIDE:
		return true;
	default:
		return false;
	}
}

void afterMovement()
{
	if (!s_stageReady || paused())
		return;
	SmsEvTick ev;
	ev.tick             = s_tick++;
	ev.ticks_per_second = port_active_frame_rate > 0 ? port_active_frame_rate : 30;
	ev.dt               = 1.0f / (float)ev.ticks_per_second;
	modh_dispatch(SMS_EV_TICK, &ev, sizeof ev);
}

void stageSetup()
{
	s_stageReady  = true;
	s_tick        = 0;
	s_started     = false;
	s_wayOutAsked = s_warpAsked = s_missAsked = false;
	s_stageShine  = -1;
	SmsEvStage ev;
	ev.area    = gpApplication.mCurrArea.unk0;
	ev.episode = gpApplication.mCurrArea.unk1;
	ev.reason  = s_lastReason;
	ev.shine   = s_lastShine;
	modh_dispatch(SMS_EV_STAGE_LOADED, &ev, sizeof ev);
}

// The first frame of play after the stage's opening (TMarDirector::changeState
// goes through its opening states to STATE_UNK4, where the game is played).
void stageStart()
{
	if (s_started || !s_stageReady || !gpMarDirector || !gpMarioOriginal
	    || gpMarDirector->mState != TMarDirector::STATE_UNK4)
		return;
	s_started = true;
	SmsEvStage ev;
	ev.area    = gpApplication.mCurrArea.unk0;
	ev.episode = gpApplication.mCurrArea.unk1;
	ev.reason  = s_lastReason;
	ev.shine   = s_lastShine;
	modh_dispatch(SMS_EV_STAGE_START, &ev, sizeof ev);
}

int allowNextStage(unsigned code, void* actor)
{
	if (modh_wants(SMS_EV_STAGE_EXIT_REQUEST)) {
		SmsEvStageExit ev;
		ev.code    = code;
		ev.area    = (int)((code >> 8) & 0xFF) - 1;
		ev.episode = (int)(code & 0xFF);
		ev.actor   = actor ? obj((JDrama::TActor*)actor) : 0;
		if (modh_dispatch(SMS_EV_STAGE_EXIT_REQUEST, &ev, sizeof ev))
			return 0;
	}
	s_wayOutAsked = true;
	return 1;
}

// The Shine that ended the stage: the one the player touched, else the last
// one whose flag was set here (multiplayer's host saving another player's).
int stageShine()
{
	TMarDirector* d = gpMarDirector;
	if (d && d->unk25C)
		return (int)d->unk25C->getEventId();
	return s_stageShine;
}

// Why the game is leaving now (the director's state as it goes).
int leaveReason()
{
	TMarDirector* d = gpMarDirector;
	if (s_missAsked)
		return SMS_LEAVE_LIFE_LOST;
	if (d && (d->mState == TMarDirector::STATE_UNK7 || (d->mState == TMarDirector::STATE_CARD_SAVE && d->unk261 == 7)))
		return SMS_LEAVE_GAME_OVER;
	if (d && d->mState == TMarDirector::STATE_PAUSE_MENU)
		return SMS_LEAVE_EXIT_AREA;
	if (d && d->checkFlag(TMarDirector::DIRECTOR_FLAG_SHINE_TAKEN))
		return SMS_LEAVE_SHINE;
	if (s_warpAsked)
		return SMS_LEAVE_WARP;
	if (s_wayOutAsked)
		return SMS_LEAVE_WAY_OUT;
	return SMS_LEAVE_OTHER;
}

// A world's way in (TMarDirector::moveStage: with no episode, its select).
bool isWorldEntry(int area)
{
	return area == 2 || area == 3 || area == 4 || area == 5 || area == 6 || area == 8 || area == 9;
}

// An event whose payload is a stage the handlers may change, applied to `seq`.
// Returns 1 when a handler changed it.
int stageEvent(uint32_t type, TGameSequence& seq, int reason, int shine)
{
	if (!modh_wants(type))
		return 0;
	SmsEvStage ev;
	ev.area    = seq.unk0;
	ev.episode = seq.unk1;
	ev.reason  = reason;
	ev.shine   = shine;
	modh_dispatch(type, &ev, sizeof ev);
	if (ev.area < 0 || ev.area > 254 || ev.episode < 0 || ev.episode > 255 || (ev.area == seq.unk0 && ev.episode == seq.unk1))
		return 0;
	seq.unk0 = (u8)ev.area;
	seq.unk1 = (u8)ev.episode;
	return 1;
}

void chooseNextStage()
{
	const int reason = leaveReason();
	const int shine  = reason == SMS_LEAVE_SHINE ? stageShine() : -1;
	TGameSequence& next = gpApplication.mNextArea;
	if (stageEvent(SMS_EV_STAGE_LEAVING, next, reason, shine) && next.unk1 != 0xFF && isWorldEntry(next.unk0)) {
		// a mod named a world's episode: as its episode select would have
		// (TSelectDir records the choice), and as a new visit starts, with no coins
		// (TMarDirector::moveStage resets them only on going to another world)
		TFlagManager::getInstance()->setFlag(0x40003, next.unk1);
		const TGameSequence& cur = gpApplication.mCurrArea;
		if (SMS_getShineStage(next.unk0) == SMS_getShineStage(cur.unk0) && next.unk1 != cur.unk1)
			TFlagManager::getInstance()->setFlag(0x40002, 0);
	}
	s_lastReason = reason;
	s_lastShine  = shine;
}

void moveStage() { s_stageReady = false; }

// The stage about to load, its episode chosen (port_ext.h's chooseStageLoad):
// a mod may load another, or have Mario come from elsewhere.
void chooseStageLoad()
{
	if (!modh_wants(SMS_EV_STAGE_LOADING))
		return;
	TGameSequence& cur  = gpApplication.mCurrArea;
	TGameSequence& prev = gpApplication.mPrevArea;
	SmsEvStageLoading ev;
	ev.area         = cur.unk0;
	ev.episode      = cur.unk1;
	ev.reason       = s_lastReason;
	ev.shine        = s_lastShine;
	ev.from_area    = prev.unk0;
	ev.from_episode = prev.unk1;
	modh_dispatch(SMS_EV_STAGE_LOADING, &ev, sizeof ev);
	if (!port_ext_load_instead(ev.area, ev.episode))
		fprintf(stderr, "[mods] SMS_EV_STAGE_LOADING: area %d, episode %d not taken (no such stage, or the title); "
		                "loading area %d, episode %d\n",
		        ev.area, ev.episode, cur.unk0, cur.unk1);
	if (ev.from_area >= 0 && ev.from_area <= 254 && ev.from_episode >= 0 && ev.from_episode <= 255) {
		prev.unk0 = (u8)ev.from_area;
		prev.unk1 = (u8)ev.from_episode;
	}
}

void missNextArea()
{
	s_missAsked = true;
	stageEvent(SMS_EV_LIFE_LOST, gpApplication.mNextArea, SMS_LEAVE_LIFE_LOST, -1);
}

void afterFileSelect()
{
	stageEvent(SMS_EV_FILE_SELECTED, gpApplication.mNextArea, SMS_LEAVE_FILE, -1);
	s_lastReason = SMS_LEAVE_FILE;
	s_lastShine  = -1;
}
int takeGameOver() { return modh_dispatch(SMS_EV_GAME_OVER, 0, 0); }
int keepCoinsOnMiss() { return modh_dispatch(SMS_EV_COINS_RESET, 0, 0); }

void itemTaken(void* item, void* taker)
{
	if (!modh_wants(SMS_EV_ITEM_COLLECTED))
		return;
	TItem* it = (TItem*)item;
	SmsEvItem ev;
	ev.item       = obj(it);
	ev.taker      = taker ? obj((THitActor*)taker) : 0;
	ev.actor_type = it->getActorType();
	ev.kind       = dynamic_cast<TCoinBlue*>(it) ? SMS_ITEM_BLUE_COIN
	                : dynamic_cast<TCoinRed*>(it) ? SMS_ITEM_RED_COIN
	                : dynamic_cast<TCoin*>(it)    ? SMS_ITEM_COIN
	                                              : SMS_ITEM_OTHER;
	setVec(&ev.position, it->mPosition.x, it->mPosition.y, it->mPosition.z);
	modh_dispatch(SMS_EV_ITEM_COLLECTED, &ev, sizeof ev);
}

int allowSaveStates()
{
	SmsEvSaveState ev;
	ev.reason = 0;
	return !modh_dispatch(SMS_EV_SAVE_STATE_REQUEST, &ev, sizeof ev);
}

const char* saveStatesOffReason() { return modh_save_state_reason(); }

int hidePlayerModel(void* m)
{
	if (!modh_wants(SMS_EV_HIDE_PLAYER_MODEL))
		return 0;
	SmsEvObject ev;
	ev.object = obj((TMario*)m);
	return modh_dispatch(SMS_EV_HIDE_PLAYER_MODEL, &ev, sizeof ev);
}

void padRead(void* status) { modh_pad_read((SmsPadStatus*)status); }

int demoCamera(const char* name, const float* pos, int frames, float, int, int, void* actor, unsigned)
{
	if (!modh_wants(SMS_EV_CUTSCENE))
		return 0;
	SmsEvCutscene ev;
	ev.name         = name ? name : "";
	ev.has_position = pos != 0;
	if (pos)
		setVec(&ev.position, pos[0], pos[1], pos[2]);
	else
		setVec(&ev.position, 0, 0, 0);
	ev.frames = frames;
	ev.actor  = actor ? obj((JDrama::TActor*)actor) : 0;
	modh_dispatch(SMS_EV_CUTSCENE, &ev, sizeof ev);
	return 0; // mods hear of it; none leaves it out
}

void pollutionClean(float x, float y, float z, float size)
{
	if (!modh_wants(SMS_EV_GOOP_CLEANED))
		return;
	SmsEvGoop ev;
	setVec(&ev.position, x, y, z);
	ev.size = size;
	modh_dispatch(SMS_EV_GOOP_CLEANED, &ev, sizeof ev);
}

void objectMove(void* object, int after)
{
	if (!modh_wants(SMS_EV_OBJECT_MOVE))
		return;
	SmsEvObjectMove ev;
	ev.object = obj((JDrama::TViewObj*)object);
	ev.after  = after;
	modh_dispatch(SMS_EV_OBJECT_MOVE, &ev, sizeof ev);
}

int objectHitNote(void* object, void* sender, unsigned message);

int objectHit(void* object, void* sender, unsigned message)
{
	objectHitNote(object, sender, message);
	if (!modh_wants(SMS_EV_OBJECT_HIT))
		return 0;
	SmsEvHit ev;
	ev.object  = object ? obj((THitActor*)object) : 0;
	ev.sender  = sender ? obj((THitActor*)sender) : 0;
	ev.message = message;
	return modh_dispatch(SMS_EV_OBJECT_HIT, &ev, sizeof ev);
}

// A stomp: as any other of the player's hits, for mods (port_ext.h).
int objectStomp(void* object, void* m) { return objectHit(object, m, HIT_MESSAGE_TRAMPLE); }

// The field of view a mod gave the camera last tick, and the camera's own: the
// camera's mFovy takes the mod's (the culling and the screen effects read it
// too, not only the projection), and gets its own back the next tick unless
// the camera set a new one itself (cutscenes keep theirs from tick to tick).
f32 s_fovyGiven, s_fovyOwn;

void camera(void* cam, float* eye, float* at, float* fovy)
{
	if (cam != (void*)gpCamera)
		return;
	CPolarSubCamera* c = (CPolarSubCamera*)cam;
	if (s_fovyGiven != 0.0f) {
		if (c->mFovy == s_fovyGiven)
			c->mFovy = s_fovyOwn;
		s_fovyGiven = 0.0f;
		*fovy       = c->mFovy;
	}
	if (!modh_wants(SMS_EV_CAMERA))
		return;
	SmsEvCamera ev;
	setVec(&ev.position, eye[0], eye[1], eye[2]);
	setVec(&ev.target, at[0], at[1], at[2]);
	ev.fovy   = *fovy;
	ev.aspect = c->mAspect;
	ev.roll   = 0;
	modh_dispatch(SMS_EV_CAMERA, &ev, sizeof ev);
	eye[0] = ev.position.x;
	eye[1] = ev.position.y;
	eye[2] = ev.position.z;
	at[0]  = ev.target.x;
	at[1]  = ev.target.y;
	at[2]  = ev.target.z;
	// a field of view the projection can make
	if (ev.fovy > 1.0f && ev.fovy < 179.0f && ev.fovy != *fovy) {
		s_fovyOwn   = c->mFovy;
		s_fovyGiven = ev.fovy;
		c->mFovy    = ev.fovy;
		*fovy       = ev.fovy;
	}
}

int playerStatus(void* m, unsigned* status, unsigned* arg, int force)
{
	if (m != (void*)mario() || !modh_wants(SMS_EV_PLAYER_STATUS))
		return 0;
	SmsEvPlayerStatus ev;
	ev.from   = ((TMario*)m)->mStatus;
	ev.to     = *status;
	ev.arg    = *arg;
	ev.forced = force;
	if (modh_dispatch(SMS_EV_PLAYER_STATUS, &ev, sizeof ev))
		return 1;
	*status = ev.to;
	*arg    = ev.arg;
	return 0;
}

int playerNozzle(void* m, void* box, int* nozzle, int animate, int reason)
{
	// The player's Mario; a box comes as the stage loads, before mario() is
	// there, and a stage's nozzle once the director has placed him.
	if (reason == PORT_NOZZLE_BOX ? !box
	    : reason == PORT_NOZZLE_STAGE_START ? !m || m != (void*)gpMarioOriginal
	                                        : m != (void*)mario())
		return 0;
	if (!modh_wants(SMS_EV_PLAYER_NOZZLE))
		return 0;
	TWaterGun* gun = m ? ((TMario*)m)->mWaterGun : 0;
	SmsEvPlayerNozzle ev;
	ev.from    = reason == PORT_NOZZLE_BOX || !gun ? -1 : gun->getCurrentNozzleIndex();
	ev.to      = *nozzle;
	ev.animate = animate;
	ev.reason  = reason;
	ev.box     = box ? obj((JDrama::TNameRef*)box) : 0;
	if (modh_dispatch(SMS_EV_PLAYER_NOZZLE, &ev, sizeof ev))
		return 1;
	*nozzle = ev.to; // port_ext_player_nozzle keeps only a nozzle FLUDD has
	return 0;
}

// What took the player's health last (for SMS_EV_PLAYER_DEATH), and when.
SmsObject s_lastHurt;
uint32_t s_lastHurtTick;

SmsObject damageSource(TMario* p)
{
	THitActor* a = (THitActor*)port_ext_damage_source;
	return a && a != p->getFloorHitActor() ? obj(a) : 0;
}

int playerDamage(void* m, int* amount)
{
	if (m != (void*)mario())
		return 0;
	TMario* p = (TMario*)m;
	if (p->checkFlag(MARIO_FLAG_GAME_OVER))
		return 0; // he has lost his life: the game goes on taking what is left
	if (!modh_wants(SMS_EV_PLAYER_DAMAGE)) {
		s_lastHurt     = damageSource(p);
		s_lastHurtTick = s_tick;
		return 0;
	}
	SmsEvDamage ev;
	ev.amount     = *amount;
	ev.underwater = p->isUnderWater() || p->checkFlag(MARIO_FLAG_HELMET_FLW_CAMERA);
	ev.source     = ev.underwater ? 0 : damageSource(p);
	if (modh_dispatch(SMS_EV_PLAYER_DAMAGE, &ev, sizeof ev))
		return 1;
	*amount = ev.amount;
	if (ev.amount > 0 && !ev.underwater) {
		s_lastHurt     = ev.source;
		s_lastHurtTick = s_tick;
	}
	return 0;
}

void shineCollected(int shine, int total)
{
	s_stageShine = shine;
	SmsEvShine ev;
	ev.shine = shine;
	ev.total = total;
	modh_dispatch(SMS_EV_SHINE_COLLECTED, &ev, sizeof ev);
}

// ------------------------------------------------------------------ API 1.5 beta 2

// An object the hooks hand over as a THitActor (port_ext.h), as mods get it.
SmsObject hitObj(void* p) { return p ? obj((THitActor*)p) : 0; }

void vecOf(SmsVec3* v, const JGeometry::TVec3<f32>& p) { setVec(v, p.x, p.y, p.z); }

// The game mode and the pause, as they change (SMS_EV_MODE_CHANGED,
// SMS_EV_PAUSE), from the top of each frame.
int s_lastMode = -1, s_pauseKind = -1;

int pauseKind()
{
	if (!paused())
		return -1;
	switch (gpMarDirector->mState) {
	case TMarDirector::STATE_PAUSE_MENU:
		return SMS_PAUSE_MENU;
	case TMarDirector::STATE_GUIDE:
		return SMS_PAUSE_MAP;
	default:
		return SMS_PAUSE_SAVE;
	}
}

void watchModeAndPause()
{
	const int mode = gameMode();
	if (mode != s_lastMode) {
		if (s_lastMode >= 0 && modh_wants(SMS_EV_MODE_CHANGED)) {
			SmsEvMode ev;
			ev.from = s_lastMode;
			ev.to   = mode;
			modh_dispatch(SMS_EV_MODE_CHANGED, &ev, sizeof ev);
		}
		s_lastMode = mode;
	}
	const int kind = pauseKind();
	if (kind != s_pauseKind) {
		// a close first, then what opened (the pause menu to its save screen)
		if (s_pauseKind >= 0) {
			SmsEvPause ev;
			ev.paused = 0;
			ev.kind   = s_pauseKind;
			modh_dispatch(SMS_EV_PAUSE, &ev, sizeof ev);
		}
		if (kind >= 0) {
			SmsEvPause ev;
			ev.paused = 1;
			ev.kind   = kind;
			modh_dispatch(SMS_EV_PAUSE, &ev, sizeof ev);
		}
		s_pauseKind = kind;
	}
}

// The jumps SMS_EV_PLAYER_JUMP tells of (MarioStatus.hpp): a status Mario
// leaves the ground with, not the ones he goes on with in the air.
bool isJump(u32 status)
{
	switch (status) {
	case 0x02000880: // jump
	case 0x02000881: // second jump
	case 0x00000882: // triple jump
	case 0x00000883: // back flip
	case 0x00000884: // bounced off a springy floor
	case 0x02000886: // wall jump
	case 0x00000887: // side flip
	case 0x00000888: // long jump
	case 0x02000889: // spinning long jump
	case 0x0080088A: // dive (out of a run)
	case 0x0000088B: // rocket
	case 0x02000890: // a stomp's bounce
	case 0x00000891: // dive
	case 0x00000892: // off a wire
	case 0x00000893: // off a wire, spinning
	case 0x00000894: // pulled up
	case 0x00000895: // spin jump
	case 0x00000896: // spin jump
	case 0x0000089C: // off Yoshi, flying
	case 0x0281089A: // a Blooper's jump
		return true;
	default:
		return false;
	}
}

// MARIO_STATUS_LANDING, MARIO_STATUS_ROCKET_LANDING: on the ground, landing
bool isLanding(u32 status) { return status == 0x88C || status == 0x88D; }

void playerStatusChanged(void* m, unsigned from, unsigned to, unsigned arg)
{
	if (m != (void*)mario())
		return;
	TMario* p = (TMario*)m;
	if (modh_wants(SMS_EV_PLAYER_STATUS_CHANGED)) {
		SmsEvPlayerStatus ev;
		ev.from   = from;
		ev.to     = to;
		ev.arg    = arg;
		ev.forced = 0;
		modh_dispatch(SMS_EV_PLAYER_STATUS_CHANGED, &ev, sizeof ev);
	}
	if (isJump(to) && to != from && modh_wants(SMS_EV_PLAYER_JUMP)) {
		SmsEvPlayerJump ev;
		ev.status = to;
		ev.from   = from;
		vecOf(&ev.position, p->mPosition);
		vecOf(&ev.velocity, p->mVel);
		modh_dispatch(SMS_EV_PLAYER_JUMP, &ev, sizeof ev);
	}
	// out of the air onto the ground (not into the water: SMS_EV_PLAYER_WATER);
	// a landing's own statuses (0x88C, 0x88D) are on the ground already
	if ((from & 0x800) && !isLanding(from) && (!(to & 0x800) || isLanding(to)) && !(to & 0x2000)
	    && !p->checkFlag(MARIO_FLAG_GAME_OVER) && modh_wants(SMS_EV_PLAYER_LAND)) {
		SmsEvPlayerLand ev;
		ev.status = to;
		ev.from   = from;
		ev.fall   = p->unk104 - p->mPosition.y;
		if (ev.fall < 0.0f)
			ev.fall = 0.0f;
		ev.speed = p->unkBC < 0.0f ? -p->unkBC : 0.0f;
		vecOf(&ev.position, p->mPosition);
		modh_dispatch(SMS_EV_PLAYER_LAND, &ev, sizeof ev);
	}
	// off a Blooper: the status leaves its riding ones (bit 0x10000)
	if ((from & 0x10000) && !(to & 0x10000) && modh_wants(SMS_EV_PLAYER_MOUNT)) {
		SmsEvPlayerMount ev;
		ev.mount  = SMS_MOUNT_BLOOPER;
		ev.on     = 0;
		ev.object = 0;
		ev.color  = -1;
		modh_dispatch(SMS_EV_PLAYER_MOUNT, &ev, sizeof ev);
	}
}

int playerHeal(void* m, int* amount, int air)
{
	if (m != (void*)mario() || !modh_wants(SMS_EV_PLAYER_HEAL))
		return 0;
	SmsEvDamage ev;
	ev.amount     = *amount;
	ev.underwater = air;
	ev.source     = 0;
	if (modh_dispatch(SMS_EV_PLAYER_HEAL, &ev, sizeof ev))
		return 1;
	*amount = ev.amount;
	return 0;
}

void playerDeath(void* m, int cause)
{
	if (m != (void*)mario())
		return;
	TMario* p = (TMario*)m;
	SmsEvPlayerDeath ev;
	ev.cause = cause;
	vecOf(&ev.position, p->mPosition);
	ev.source = cause == SMS_DEATH_HEALTH && s_lastHurtTick + 2 >= s_tick ? s_lastHurt : 0;
	modh_dispatch(SMS_EV_PLAYER_DEATH, &ev, sizeof ev);
}

void playerSpawn(float* pos, float* yaw, int* hasPos, int* kind)
{
	if (!modh_wants(SMS_EV_PLAYER_SPAWN))
		return;
	SmsEvPlayerSpawn ev;
	setVec(&ev.position, pos[0], pos[1], pos[2]);
	ev.yaw          = *yaw;
	ev.has_position = *hasPos;
	ev.kind         = *kind;
	modh_dispatch(SMS_EV_PLAYER_SPAWN, &ev, sizeof ev);
	// another start (beta 6): wait, roll or back from a world; none to the coaster
	if (ev.kind >= SMS_START_WAIT && ev.kind <= SMS_START_RETURN)
		*kind = ev.kind;
	if (*kind == SMS_START_COASTER)
		return;
	pos[0]  = ev.position.x;
	pos[1]  = ev.position.y;
	pos[2]  = ev.position.z;
	*yaw    = ev.yaw;
	*hasPos = ev.has_position ? 1 : 0;
}

int playerMessage(void* m, void* sender, unsigned message)
{
	if (m != (void*)mario() || !sender || !modh_wants(SMS_EV_PLAYER_HIT))
		return 0;
	SmsEvHit ev;
	ev.object  = obj((TMario*)m);
	ev.sender  = obj((THitActor*)sender);
	ev.message = message;
	return modh_dispatch(SMS_EV_PLAYER_HIT, &ev, sizeof ev);
}

void playerMount(void* m, int mount, int on, void* object)
{
	if (m != (void*)mario() || !modh_wants(SMS_EV_PLAYER_MOUNT))
		return;
	TMario* p = (TMario*)m;
	SmsEvPlayerMount ev;
	ev.mount  = mount == PORT_MOUNT_YOSHI ? SMS_MOUNT_YOSHI : SMS_MOUNT_BLOOPER;
	ev.on     = on;
	ev.object = mount == PORT_MOUNT_BLOOPER ? hitObj(object) : 0;
	ev.color  = !on                         ? -1
	            : mount == PORT_MOUNT_YOSHI ? (object ? ((TYoshi*)object)->mType : -1)
	                                        : (int)p->mSurfGessoType;
	modh_dispatch(SMS_EV_PLAYER_MOUNT, &ev, sizeof ev);
}

void playerWater(void* m, int entered, float surfaceY)
{
	if (m != (void*)mario() || !modh_wants(SMS_EV_PLAYER_WATER))
		return;
	SmsEvPlayerWater ev;
	ev.entered   = entered;
	ev.surface_y = surfaceY;
	vecOf(&ev.position, ((TMario*)m)->mPosition);
	modh_dispatch(SMS_EV_PLAYER_WATER, &ev, sizeof ev);
}

void yoshi(void* y, int what, unsigned fruit)
{
	const uint32_t type = what == PORT_YOSHI_HATCH ? SMS_EV_YOSHI_HATCH : SMS_EV_YOSHI_EAT;
	if (!y || !modh_wants(type))
		return;
	TYoshi* yo = (TYoshi*)y;
	SmsEvYoshi ev;
	ev.fruit = fruit;
	ev.color = yo->mType;
	vecOf(&ev.position, yo->mTranslation);
	modh_dispatch(type, &ev, sizeof ev);
}

void actorSpawn(void* a)
{
	if (!a || !modh_wants(SMS_EV_ACTOR_SPAWN))
		return;
	THitActor* h = (THitActor*)a;
	SmsEvActor ev;
	ev.object     = obj(h);
	ev.actor_type = h->getActorType();
	vecOf(&ev.position, h->mPosition);
	ev.other  = 0;
	ev.forced = 0;
	modh_dispatch(SMS_EV_ACTOR_SPAWN, &ev, sizeof ev);
}

// The player's hits: an enemy's health before each (SMS_EV_ACTOR_DAMAGE), and
// the last hit on each enemy (SMS_EV_ACTOR_DEATH's `other`), a few of each.
struct HitNote {
	THitActor* object;
	THitActor* sender;
	int health;
};
enum { kHitNotes = 16 };
HitNote s_hits[kHitNotes];
int s_nextHit;

HitNote* hitNote(THitActor* o)
{
	for (int i = 0; i < kHitNotes; i++)
		if (s_hits[i].object == o)
			return &s_hits[i];
	return 0;
}

int objectHitNote(void* object, void* sender, unsigned)
{
	if (!object || !(modh_wants(SMS_EV_ACTOR_DAMAGE) || modh_wants(SMS_EV_ACTOR_DEATH)))
		return 0;
	THitActor* h   = (THitActor*)object;
	TSpineEnemy* e = dynamic_cast<TSpineEnemy*>(h);
	if (!e)
		return 0;
	HitNote* n = hitNote(h);
	if (!n) {
		n = &s_hits[s_nextHit];
		s_nextHit = (s_nextHit + 1) % kHitNotes;
	}
	n->object = h;
	n->sender = (THitActor*)sender;
	n->health = e->getHitPoints();
	return 0; // only noted: modapi's objectHit sends SMS_EV_OBJECT_HIT
}

void objectHitDone(void* object, void* sender, unsigned message, int)
{
	if (!object || !modh_wants(SMS_EV_ACTOR_DAMAGE))
		return;
	THitActor* h   = (THitActor*)object;
	HitNote* n     = hitNote(h);
	TSpineEnemy* e = n ? dynamic_cast<TSpineEnemy*>(h) : 0;
	if (!e || e->getHitPoints() >= n->health)
		return;
	SmsEvActorDamage ev;
	ev.object        = obj(h);
	ev.attacker      = sender ? obj((THitActor*)sender) : 0;
	ev.actor_type    = h->getActorType();
	ev.message       = message;
	ev.health_before = n->health;
	ev.health        = e->getHitPoints();
	ev.max_health    = e->getSaveParam() ? (int)e->getSaveParam()->mSLHitPointMax.get() : ev.health_before;
	vecOf(&ev.position, h->mPosition);
	n->health = ev.health;
	modh_dispatch(SMS_EV_ACTOR_DAMAGE, &ev, sizeof ev);
}

void actorDeath(void* a, int forced)
{
	if (!a)
		return;
	THitActor* h = (THitActor*)a;
	HitNote* n   = hitNote(h);
	if (modh_wants(SMS_EV_ACTOR_DEATH)) {
		SmsEvActor ev;
		ev.object     = obj(h);
		ev.actor_type = h->getActorType();
		vecOf(&ev.position, h->mPosition);
		ev.other  = n && n->sender ? obj(n->sender) : 0;
		ev.forced = forced;
		modh_dispatch(SMS_EV_ACTOR_DEATH, &ev, sizeof ev);
	}
	if (n)
		n->object = 0;
}

void shine(void* p, int what)
{
	const uint32_t type = what == PORT_SHINE_APPEAR ? SMS_EV_SHINE_APPEAR : SMS_EV_SHINE_GET;
	if (!p || !modh_wants(type))
		return;
	TShine* sh = (TShine*)(THitActor*)p;
	SmsEvShineActor ev;
	ev.object = obj(sh);
	ev.shine  = (int)sh->getEventId();
	ev.had    = TFlagManager::getInstance()->getShineFlag((u8)ev.shine) ? 1 : 0;
	vecOf(&ev.position, sh->mPosition);
	modh_dispatch(type, &ev, sizeof ev);
}

int flagChange(unsigned flag, int old, int* value)
{
	if (!modh_wants(SMS_EV_FLAG_CHANGED))
		return 0;
	SmsEvFlag ev;
	ev.id    = flag;
	ev.old   = old;
	ev.value = *value;
	if (modh_dispatch(SMS_EV_FLAG_CHANGED, &ev, sizeof ev))
		return 1;
	*value = ev.value;
	return 0;
}

int sound(unsigned* id, const float* pos, int system)
{
	if (!modh_wants(SMS_EV_SOUND))
		return 0;
	SmsEvSound ev;
	ev.id           = *id;
	ev.system       = system;
	ev.has_position = pos != 0;
	if (pos)
		setVec(&ev.position, pos[0], pos[1], pos[2]);
	else
		setVec(&ev.position, 0, 0, 0);
	if (modh_dispatch(SMS_EV_SOUND, &ev, sizeof ev))
		return 1;
	*id = ev.id;
	return 0;
}

int music(unsigned* id, int start)
{
	if (!modh_wants(SMS_EV_MUSIC))
		return 0;
	SmsEvMusic ev;
	ev.id    = *id;
	ev.start = start;
	if (modh_dispatch(SMS_EV_MUSIC, &ev, sizeof ev) && start)
		return 1;
	if (start)
		*id = ev.id;
	return 0;
}

void talk(int what, void* npc, unsigned* message, unsigned flags)
{
	if (!modh_wants(SMS_EV_TALK))
		return;
	SmsEvTalk ev;
	ev.what    = what == PORT_TALK_START ? SMS_TALK_START : what == PORT_TALK_MESSAGE ? SMS_TALK_MESSAGE : SMS_TALK_END;
	ev.npc     = npc ? obj((TBaseNPC*)npc) : 0;
	ev.message = message ? *message : 0;
	ev.last    = what == PORT_TALK_MESSAGE && (flags & 1) ? 1 : 0;
	modh_dispatch(SMS_EV_TALK, &ev, sizeof ev);
	if (message)
		*message = ev.message;
}

void demoCameraEnd(const char* name, void* actor, int forced)
{
	if (!modh_wants(SMS_EV_CUTSCENE_END))
		return;
	SmsEvCutsceneEnd ev;
	ev.name   = name ? name : "";
	ev.actor  = actor ? obj((JDrama::TActor*)actor) : 0;
	ev.forced = forced;
	modh_dispatch(SMS_EV_CUTSCENE_END, &ev, sizeof ev);
}

void gameSaved(int slot)
{
	// the mods' files of the file go with it (api->files)
	modh_files_event(SMS_SAVE_FILE_SAVED, slot, -1);
	SmsEvGameSaved ev;
	ev.file = slot;
	modh_dispatch(SMS_EV_GAME_SAVED, &ev, sizeof ev);
	SmsEvSaveFile sf;
	sf.what = SMS_SAVE_FILE_SAVED;
	sf.file = slot;
	sf.from = -1;
	modh_dispatch(SMS_EV_SAVE_FILE, &sf, sizeof sf);
}

// At file select (port_ext.h saveFile): the mods' files of save files follow
// the card (api->files), then SMS_EV_SAVE_FILE.
void saveFile(int what, int file, int from)
{
	modh_files_event(what, file, from);
	SmsEvSaveFile ev;
	ev.what = what;
	ev.file = file;
	ev.from = from;
	modh_dispatch(SMS_EV_SAVE_FILE, &ev, sizeof ev);
}

int fileStarting(int file, int isNew)
{
	if (!modh_wants(SMS_EV_FILE_STARTING))
		return 0;
	SmsEvFileStarting ev;
	ev.file   = file;
	ev.is_new = isNew;
	return modh_dispatch(SMS_EV_FILE_STARTING, &ev, sizeof ev);
}

int nozzleFire(void* m, int nozzle, int how)
{
	if (!m || m != (void*)gpMarioOriginal || !modh_wants(SMS_EV_NOZZLE_FIRE))
		return 0;
	SmsEvNozzleFire ev;
	ev.nozzle = nozzle;
	ev.how    = how;
	return modh_dispatch(SMS_EV_NOZZLE_FIRE, &ev, sizeof ev);
}

// A Shine's model from a mod's file (SMS_EV_SHINE_SETUP), loaded into the
// Shine manager's model data keeper under its path, once a stage (the keeper
// and the file's bytes live in the stage's heap, as the stage's own models).
const char* shineModel(void* p, void* k, int collected)
{
	if (!p || !k || !modh_wants(SMS_EV_SHINE_SETUP))
		return 0;
	TShine* sh = (TShine*)p;
	SmsEvShineSetup ev;
	ev.object    = obj(sh);
	ev.shine     = sh->getEventId() < 120 ? (int)sh->getEventId() : -1;
	ev.collected = collected;
	ev.model     = 0;
	modh_dispatch(SMS_EV_SHINE_SETUP, &ev, sizeof ev);
	if (!ev.model || !ev.model[0])
		return 0;
	TModelDataKeeper* keeper = ((TMActorKeeper*)k)->getModelDataKeeper();
	if (!keeper)
		return 0;
	if (keeper->getIndex(ev.model) >= 0)
		return ev.model;
	FILE* f = fopen(ev.model, "rb");
	if (!f) {
		fprintf(stderr, "[mods] SMS_EV_SHINE_SETUP: no model %s\n", ev.model);
		return 0;
	}
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	u8* data = size > 32 && size < (16 << 20) ? new (0x20) u8[size] : 0;
	const bool read = data && fread(data, 1, (size_t)size, f) == (size_t)size;
	fclose(f);
	if (!read || memcmp(data, "J3D2bmd", 7) != 0) {
		fprintf(stderr, "[mods] SMS_EV_SHINE_SETUP: %s is not a model (.bmd)\n", ev.model);
		return 0;
	}
	port_res_to_native_named(data, (u32)size, ev.model);
	J3DModelData* model = J3DModelLoaderDataBase::load(data, ((TMActorKeeper*)k)->getModelLoaderFlags());
	if (!model)
		return 0;
	const size_t n = strlen(ev.model) + 1;
	char* name     = new char[n];
	memcpy(name, ev.model, n);
	TModelDataNode* node = &keeper->mHead;
	while (node->getNext())
		node = node->getNext();
	node->registerDataAndJoinNewNode(new SDLModelData(model), name);
	return name;
}

void yoshiEgg(int kind, void* egg, int* present)
{
	if (!modh_wants(SMS_EV_YOSHI_EGG))
		return;
	SmsEvYoshiEgg ev;
	ev.kind    = kind;
	ev.present = *present ? 1 : 0;
	ev.object  = egg ? obj((TEggYoshi*)egg) : 0;
	modh_dispatch(SMS_EV_YOSHI_EGG, &ev, sizeof ev);
	*present = ev.present ? 1 : 0;
}

int worldArea(int shineStage); // below, with the worlds

void episodeShine(int shineStage, int episode, int* shine)
{
	if (!modh_wants(SMS_EV_EPISODE_SHINE))
		return;
	SmsEvEpisodeShine ev;
	ev.area    = worldArea(shineStage);
	ev.episode = episode;
	ev.shine   = *shine;
	modh_dispatch(SMS_EV_EPISODE_SHINE, &ev, sizeof ev);
	if (ev.shine >= -1 && ev.shine < 120)
		*shine = ev.shine;
}

void timer(int what, int time)
{
	SmsEvTimer ev;
	ev.what = what == PORT_TIMER_START ? SMS_TIMER_STARTED : what == PORT_TIMER_STOP ? SMS_TIMER_STOPPED : SMS_TIMER_OUT;
	ev.time = time;
	modh_dispatch(SMS_EV_TIMER, &ev, sizeof ev);
}

void scriptCall(const char* name, void* owner, int argc)
{
	if (!modh_wants(SMS_EV_SCRIPT_CALL))
		return;
	SmsEvScriptCall ev;
	ev.name  = name ? name : "";
	ev.owner = owner ? obj((JDrama::TNameRef*)owner) : 0;
	ev.argc  = argc;
	modh_dispatch(SMS_EV_SCRIPT_CALL, &ev, sizeof ev);
}

void mapEvent(void* event)
{
	SmsEvObject ev;
	ev.object = event ? obj((JDrama::TNameRef*)event) : 0;
	modh_dispatch(SMS_EV_MAP_EVENT, &ev, sizeof ev);
}

int savePrompt(int kind)
{
	SmsEvSavePrompt ev;
	ev.kind = kind;
	return modh_dispatch(SMS_EV_SAVE_PROMPT, &ev, sizeof ev);
}

// Filled by name, so a hook added to PortExtension anywhere (main's
// particleEmit, before the mod API's) never shifts these into the wrong places.
PortExtension s_extension;

struct Register {
	Register()
	{
		memset(&s_extension, 0, sizeof s_extension);
		PortExtension& e      = s_extension;
		e.name                = "modapi";
		e.frame               = frame;
		e.afterMovement       = afterMovement;
		e.stageSetup          = stageSetup;
		e.allowNextStage      = allowNextStage;
		e.moveStage           = moveStage;
		e.chooseNextStage     = chooseNextStage;
		e.chooseStageLoad     = chooseStageLoad;
		e.missNextArea        = missNextArea;
		e.takeGameOver        = takeGameOver;
		e.keepCoinsOnMiss     = keepCoinsOnMiss;
		e.afterFileSelect     = afterFileSelect;
		e.itemTaken           = itemTaken;
		e.allowSaveStates     = allowSaveStates;
		e.saveStatesOffReason = saveStatesOffReason;
		e.hidePlayerModel     = hidePlayerModel;
		e.padRead             = padRead;
		e.demoCamera          = demoCamera;
		e.pollutionClean      = pollutionClean;
		e.objectMove          = objectMove;
		e.objectHit           = objectHit;
		e.camera              = camera;
		e.playerStatus        = playerStatus;
		e.playerDamage        = playerDamage;
		e.playerNozzle        = playerNozzle;
		e.shineCollected      = shineCollected;
		e.playerStatusChanged = playerStatusChanged;
		e.playerHeal          = playerHeal;
		e.playerDeath         = playerDeath;
		e.playerSpawn         = playerSpawn;
		e.playerMessage       = playerMessage;
		e.playerMount         = playerMount;
		e.playerWater         = playerWater;
		e.yoshi               = yoshi;
		e.actorSpawn          = actorSpawn;
		e.objectHitDone       = objectHitDone;
		e.actorDeath          = actorDeath;
		e.shine               = shine;
		e.flagChange          = flagChange;
		e.sound               = sound;
		e.music               = music;
		e.talk                = talk;
		e.demoCameraEnd       = demoCameraEnd;
		e.gameSaved           = gameSaved;
		e.timer               = timer;
		e.scriptCall          = scriptCall;
		e.mapEvent            = mapEvent;
		e.savePrompt          = savePrompt;
		e.objectStomp         = objectStomp;
		e.saveFile            = saveFile;
		e.nozzleFire          = nozzleFire;
		e.shineModel          = shineModel;
		e.yoshiEgg            = yoshiEgg;
		e.episodeShine        = episodeShine;
		e.fileStarting        = fileStarting;
		port_ext_register(&s_extension);
	}
} s_register;

} // namespace

// ------------------------------------------------------------------ reads and writes

extern "C" void modg_state(SmsGameState* out)
{
	SmsGameState s;
	memset(&s, 0, sizeof s);
	s.size             = sizeof s;
	s.mode             = gameMode();
	s.stage_ready      = s_stageReady && gpMarioOriginal;
	s.area             = s.stage_ready ? gpApplication.mCurrArea.unk0 : -1;
	s.episode          = s.stage_ready ? gpApplication.mCurrArea.unk1 : -1;
	s.tick             = s_tick;
	s.ticks_per_second = port_active_frame_rate > 0 ? port_active_frame_rate : 30;
	s.save_file        = gpApplication.mSaveFile;
	s.paused           = paused() ? 1 : 0;
	// the host half fills in frame and eclipse
	const uint32_t n = out->size && out->size < sizeof s ? out->size : (uint32_t)sizeof s;
	memcpy(out, &s, n);
	out->size = n;
}

extern "C" int32_t modg_flag_get(uint32_t id) { return TFlagManager::getInstance()->getFlag(id); }

// A mod that sets the save file's flags or warps the player: play after it doesn't earn achievements (ext/achievements).
extern "C" int port_play_unchecked;

extern "C" void modg_flag_set(uint32_t id, int32_t value)
{
	++port_play_unchecked;
	TFlagManager::getInstance()->setFlag(id, value);
}

extern "C" int modg_warp(int area, int episode)
{
	if (!s_stageReady || !gpMarDirector || area < 0 || area > 254 || episode < 0 || episode > 255)
		return 0;
	s_warpAsked = true;
	++port_play_unchecked;
	gpMarDirector->setNextStage((u16)(((area + 1) << 8) | episode), 0);
	return 1;
}

// The stage's cube sets (SmsCubeSet's order).
static TCubeManagerBase* cubeSet(int set)
{
	if (!s_stageReady)
		return nullptr;
	switch (set) {
	case 0: return gpCubeCamera;
	case 1: return gpCubeArea;
	case 2: return gpCubeFastA;
	case 3: return gpCubeFastB;
	case 4: return gpCubeFastC;
	case 5: return gpCubeMirror;
	case 6: return gpCubeWire;
	case 7: return gpCubeStream;
	case 8: return gpCubeShadow;
	case 9: return gpCubeSoundChange;
	case 10: return gpCubeSoundEffect;
	default: return nullptr;
	}
}

extern "C" int modg_cube_count(int set)
{
	TCubeManagerBase* c = cubeSet(set);
	return c ? (int)c->unk10 : 0;
}

extern "C" int modg_in_cube(int set, int index, float x, float y, float z)
{
	TCubeManagerBase* c = cubeSet(set);
	if (!c || index < 0 || index >= (int)c->unk10)
		return -1;
	Vec v = {x, y, z};
	return c->isInCube(v, index) ? 1 : 0;
}

extern "C" SmsObject modg_find_object(const char* name)
{
	if (!name || !s_stageReady || !JDrama::TNameRefGen::getInstance() || !JDrama::TNameRefGen::getInstance()->getRootNameRef())
		return 0;
	return obj(JDrama::TNameRefGen::getInstance()->getRootNameRef()->search(name));
}

extern "C" const char* modg_object_name(SmsObject o)
{
	return o && ref(o)->getName() ? ref(o)->getName() : "";
}

extern "C" int modg_object_position(SmsObject o, SmsVec3* out)
{
	JDrama::TPlacement* p = o ? dynamic_cast<JDrama::TPlacement*>(ref(o)) : 0;
	if (!p || !out)
		return 0;
	setVec(out, p->mPosition.x, p->mPosition.y, p->mPosition.z);
	return 1;
}

extern "C" uint32_t modg_object_type(SmsObject o)
{
	THitActor* h = o ? dynamic_cast<THitActor*>(ref(o)) : 0;
	return h ? h->getActorType() : 0;
}

extern "C" int modg_object_health(SmsObject o, int* max)
{
	TSpineEnemy* e = o ? dynamic_cast<TSpineEnemy*>(ref(o)) : 0;
	if (!e)
		return -1;
	if (max)
		*max = e->getSaveParam() ? (int)e->getSaveParam()->mSLHitPointMax.get() : e->getHitPoints();
	return e->getHitPoints();
}

// The HUD's parts mods hide (game->set_hud_hidden: every mod's ORed by the
// host half): the game leaves them out of the HUD's draw (port_hud_draw).
extern "C" void modg_set_hud_hidden(uint32_t mask)
{
	port_hud_set_hidden(PORT_HUD_BY_MODS, mask);
}

extern "C" uint32_t modg_hud_hidden(void)
{
	return port_hud_hidden();
}

// One colour of every material of an object's model (game->object_set_color).
extern "C" int modg_object_set_color(SmsObject o, int which, SmsColor c)
{
	JDrama::TNameRef* r = o ? ref(o) : 0;
	J3DModel* model     = 0;
	if (TLiveActor* a = r ? dynamic_cast<TLiveActor*>(r) : 0)
		model = a->getMActor() ? a->getMActor()->getModel() : a->getModel();
	if (!model || !model->getModelData() || which < SMS_COLOR_MATERIAL || which > SMS_COLOR_TEV2)
		return 0;
	J3DModelData* data = model->getModelData();
	GXColor gx         = {(u8)(c >> 24), (u8)(c >> 16), (u8)(c >> 8), (u8)c};
	GXColorS10 tevColor = {gx.r, gx.g, gx.b, gx.a};
	J3DGXColorS10 tev(tevColor);
	for (u16 i = 0; i < data->getMaterialNum(); i++) {
		J3DMaterial* m = data->getMaterialNodePointer(i);
		if (!m)
			continue;
		if (which == SMS_COLOR_MATERIAL) {
			if (m->getColorBlock())
				m->getColorBlock()->setMatColor(0, J3DGXColor(gx));
		} else {
			m->setTevColor((u32)(which - SMS_COLOR_TEV0), &tev);
		}
	}
	return 1;
}

extern "C" void modg_game_code(char* out)
{
	const DVDDiskID* id = DVDGetCurrentDiskID();
	if (id && id->gameName[0]) {
		memcpy(out, id->gameName, 4);
		memcpy(out + 4, id->company, 2);
	} else {
		memcpy(out, "GMSE01", 6);
	}
	out[6] = 0;
}

extern "C" int modg_object_set_position(SmsObject o, const SmsVec3* pos)
{
	JDrama::TPlacement* p = o ? dynamic_cast<JDrama::TPlacement*>(ref(o)) : 0;
	if (!p || !pos)
		return 0;
	p->mPosition.set(pos->x, pos->y, pos->z);
	return 1;
}

extern "C" int modg_player_get(SmsPlayer* out)
{
	SmsPlayer s;
	memset(&s, 0, sizeof s);
	s.size   = sizeof s;
	TMario* m = mario();
	if (m) {
		s.valid = 1;
		setVec(&s.position, m->mPosition.x, m->mPosition.y, m->mPosition.z);
		setVec(&s.velocity, m->mVel.x, m->mVel.y, m->mVel.z);
		s.forward_speed = m->mForwardVel;
		s.face_angle[0] = m->mFaceAngle.x;
		s.face_angle[1] = m->mFaceAngle.y;
		s.face_angle[2] = m->mFaceAngle.z;
		s.yaw           = (float)m->mFaceAngle.y * (360.0f / 65536.0f);
		s.health        = m->getHealth();
		s.max_health    = m->mDeParams.mHPMax.get();
		s.has_fludd     = m->checkFlag(MARIO_FLAG_HAS_FLUDD) ? 1 : 0;
		if (m->mWaterGun) {
			s.water     = m->mWaterGun->getCurrentWater();
			s.max_water = m->mWaterGun->getMaxWater();
			s.nozzle    = m->mWaterGun->mSecondNozzle;
			s.current_nozzle = m->mWaterGun->getCurrentNozzleIndex();
		}
		s.status       = m->mStatus;
		s.status_timer = m->mStatusTimer;
		s.on_ground    = (m->mStatus & MARIO_STATUS_FLAG_JUMPING) || (m->mStatus & MARIO_STATUS_FLAG_SWIMMING) ? 0 : 1;
		s.in_water     = m->checkFlag(MARIO_FLAG_IN_ANY_WATER) || (m->mStatus & MARIO_STATUS_FLAG_SWIMMING) ? 1 : 0;
		s.on_yoshi     = m->onYoshi() ? 1 : 0;
		setVec(&s.floor, m->mFloorPosition.x, m->mFloorPosition.y, m->mFloorPosition.z);
		s.object = obj(m);
		s.shirt  = m->checkFlag(MARIO_FLAG_HAS_SHIRT) ? 1 : 0;
		if (m->mCap) {
			s.sunglasses = m->mCap->isModelActive(TMarioCap::E_CAP_MODEL_SUNGLASSES) ? 1 : 0;
			s.helmet     = m->mCap->isModelActive(TMarioCap::E_CAP_MODEL_HELMET) ? 1 : 0;
		}
	}
	const uint32_t n = out->size && out->size < sizeof s ? out->size : (uint32_t)sizeof s;
	memcpy(out, &s, n);
	out->size = n;
	return s.valid;
}

extern "C" int modg_player_set_position(const SmsVec3* pos)
{
	TMario* m = mario();
	if (!m || !pos)
		return 0;
	m->mPosition.set(pos->x, pos->y, pos->z);
	return 1;
}

extern "C" int modg_player_set_velocity(const SmsVec3* vel)
{
	TMario* m = mario();
	if (!m || !vel)
		return 0;
	m->mVel.set(vel->x, vel->y, vel->z);
	return 1;
}

extern "C" int modg_player_set_forward_speed(float speed)
{
	TMario* m = mario();
	if (!m)
		return 0;
	m->mForwardVel = speed;
	return 1;
}

extern "C" int modg_player_set_health(int health)
{
	TMario* m = mario();
	if (!m)
		return 0;
	const int max = m->mDeParams.mHPMax.get();
	m->mHealth    = (s16)(health < 0 ? 0 : health > max ? max : health);
	return 1;
}

extern "C" int modg_player_set_water(int water)
{
	TMario* m = mario();
	if (!m || !m->mWaterGun)
		return 0;
	const int max                  = m->mWaterGun->getMaxWater();
	m->mWaterGun->mCurrentWater = water < 0 ? 0 : water > max ? max : water;
	return 1;
}

extern "C" int modg_player_set_status(uint32_t status, uint32_t arg)
{
	TMario* m = mario();
	return m ? m->changePlayerStatus(status, arg, false) != 0 : 0;
}

extern "C" int modg_player_set_nozzle(int nozzle, int animate)
{
	TMario* m = mario();
	// a nozzle FLUDD has (0 to 5), but Yoshi's: that one comes with him
	if (!m || !m->mWaterGun || !m->checkFlag(MARIO_FLAG_HAS_FLUDD) || m->onYoshi() || nozzle < TWaterGun::Spray
	    || nozzle > TWaterGun::Turbo || nozzle == TWaterGun::Yoshi)
		return 0;
	m->mWaterGun->changeNozzle((TWaterGun::TNozzleType)nozzle, animate != 0); // SMS_EV_PLAYER_NOZZLE may refuse it
	return m->mWaterGun->getCurrentNozzleIndex() == nozzle;
}

extern "C" int modg_player_set_fludd(int has)
{
	TMario* m = mario();
	if (!m)
		return 0;
	if (has)
		m->onFlag(MARIO_FLAG_HAS_FLUDD);
	else
		m->offFlag(MARIO_FLAG_HAS_FLUDD);
	return 1;
}

extern "C" int modg_player_set_outfit(int shirt, int sunglasses, int helmet)
{
	TMario* m = mario();
	if (!m)
		return 0;
	// each: 1 on, 0 off, -1 as it is
	if (shirt == 1)
		m->onFlag(MARIO_FLAG_HAS_SHIRT);
	else if (shirt == 0)
		m->offFlag(MARIO_FLAG_HAS_SHIRT);
	if (TMarioCap* cap = m->mCap) {
		if (sunglasses == 1)
			cap->setModelActive(TMarioCap::E_CAP_MODEL_SUNGLASSES);
		else if (sunglasses == 0)
			cap->setModelInactive(TMarioCap::E_CAP_MODEL_SUNGLASSES);
		if (helmet == 1)
			cap->setModelActive(TMarioCap::E_CAP_MODEL_HELMET);
		else if (helmet == 0)
			cap->setModelInactive(TMarioCap::E_CAP_MODEL_HELMET);
	}
	return 1;
}

extern "C" int modg_camera_get(SmsCamera* out)
{
	SmsCamera s;
	memset(&s, 0, sizeof s);
	s.size = sizeof s;
	CPolarSubCamera* c = s_stageReady ? gpCamera : 0;
	if (c) {
		s.valid = 1;
		const Vec& eye = c->getUnk124Vec();
		const Vec& at  = c->getUnk148Vec();
		setVec(&s.position, eye.x, eye.y, eye.z);
		setVec(&s.target, at.x, at.y, at.z);
		setVec(&s.up, c->mUp.x, c->mUp.y, c->mUp.z);
		s.fovy   = c->mFovy;
		s.aspect = c->mAspect;
		s.near_z = c->mNear;
		s.far_z  = c->mFar;
		MtxPtr view = c->getUnk1EC();
		MtxPtr proj = c->getUnk16C();
		for (int r = 0; r < 3; r++)
			for (int k = 0; k < 4; k++)
				s.view[r][k] = view[r][k];
		for (int r = 0; r < 4; r++)
			for (int k = 0; k < 4; k++)
				s.proj[r][k] = proj[r][k];
	}
	const uint32_t n = out->size && out->size < sizeof s ? out->size : (uint32_t)sizeof s;
	memcpy(out, &s, n);
	out->size = n;
	return s.valid;
}

extern "C" void modg_play_system_sound(uint32_t id)
{
	if (MSound* sound = SMSGetMSound())
		sound->startSoundSystemSE(id, 0, 0, 0);
}

extern "C" void* modg_native(const char* name)
{
	if (!name)
		return 0;
	if (!strcmp(name, "gpApplication"))
		return &gpApplication;
	if (!strcmp(name, "TFlagManager"))
		return TFlagManager::getInstance();
	if (!s_stageReady)
		return 0;
	if (!strcmp(name, "gpMarioOriginal"))
		return gpMarioOriginal;
	if (!strcmp(name, "gpMarDirector"))
		return gpMarDirector;
	if (!strcmp(name, "gpCamera"))
		return gpCamera;
	if (!strcmp(name, "gpItemManager"))
		return gpItemManager;
	if (!strcmp(name, "gpMap"))
		return gpMap;
	return 0;
}

// ------------------------------------------------------------------ worlds and episodes
//
// The game's own tables (GC2D/ShineTable.hpp, System/StageUtil): an area's
// shine stage is its world (0 the Airstrip, 1 the plaza, 2 to 8 the seven
// worlds, 9 Corona Mountain), and a world's eight episodes are the shines of
// scShineConvTable, as the episode select and the pause menu read them.

namespace {

const int kAreas = 61; // System/StageUtil.cpp's shineStageTable
const int kWorldArea[10] = {0, 1, 2, 3, 4, 5, 6, 8, 9, 52}; // the area each world is entered by

int worldIndex(int area) { return area >= 0 && area < kAreas ? SMS_getShineStage((u8)area) : -1; }
int worldArea(int shineStage) { return shineStage >= 0 && shineStage <= 9 ? kWorldArea[shineStage] : -1; }

// A world with episodes (its shine table), by world index.
const u8* episodeTable(int area)
{
	const int w = worldIndex(area);
	return w >= 2 && w <= 8 && area != 15 ? scShineConvTable[w] : 0;
}

// The overlay's text: the message bank's escape sequences (0x1A, then their
// length) left out, accented letters without their accents, one line (as the
// warp menu's, zzz-warp-01).
int plainText(char* out, int n, const char* msg)
{
	static const char kFold[] = "AAAAAAACEEEEIIIIDNOOOOO OUUUUY saaaaaaaceeeeiiiidnooooo ouuuuy y";
	int len = 0;
	const u8* p = (const u8*)msg;
	while (p && *p && len + 1 < n) {
		u8 c = *p;
		if (c == 0x1A) {
			const u8 size = p[1];
			if (size < 2)
				break;
			for (u8 i = 0; i < size && *p; i++)
				p++;
			continue;
		}
		p++;
		if (c == '\n' || c == '\r' || c == '\t')
			c = ' ';
		else if (c >= 0xC0)
			c = (u8)kFold[c - 0xC0];
		if (c < 0x20 || c >= 0x7F || (c == ' ' && (len == 0 || out[len - 1] == ' ')))
			continue;
		out[len++] = (char)c;
	}
	while (len && out[len - 1] == ' ')
		len--;
	out[len] = 0;
	return len;
}

char s_cutsceneName[64];          // fireStartDemoCamera keeps the pointers it is given
JGeometry::TVec3<f32> s_cutsceneFocus;

} // namespace

extern "C" int modg_world_area_kind(int area)
{
	const int w = worldIndex(area);
	if (w < 0)
		return SMS_AREA_NONE;
	if (area == 15)
		return SMS_AREA_TITLE;
	if (w == 0)
		return SMS_AREA_AIRSTRIP;
	if (w == 9)
		return SMS_AREA_FINAL;
	if (area > 0x14 && area < 0x35) // SMS_isExMap
		return SMS_AREA_SECRET;
	if (w == 1)
		return SMS_AREA_PLAZA;
	return isWorldEntry(area) ? SMS_AREA_WORLD : SMS_AREA_WORLD_PART;
}

extern "C" int modg_world_of(int area)
{
	const int w = worldIndex(area);
	return w >= 0 && w <= 9 && area != 15 ? kWorldArea[w] : -1;
}

extern "C" int modg_world_episode_count(int area) { return episodeTable(area) ? 8 : 0; }

extern "C" int modg_world_episode_shine(int area, int episode)
{
	const u8* t = episodeTable(area);
	return t && episode >= 0 && episode < 8 ? t[episode] : -1;
}

extern "C" int modg_world_next_episode(int area)
{
	// TSelectMenu::initData: the one after the newest whose Shine is had
	const u8* t = episodeTable(area);
	if (!t)
		return -1;
	int next = 0;
	for (int i = 0; i < 8; i++)
		if (TFlagManager::getInstance()->getShineFlag(t[i]))
			next = i + 1;
	return next > 7 ? 7 : next;
}

extern "C" int modg_world_shine_episode(int shine, int* area)
{
	for (int w = 2; w <= 8; w++)
		for (int i = 0; i < 8; i++)
			if (scShineConvTable[w][i] == shine) {
				if (area)
					*area = kWorldArea[w];
				return i;
			}
	return -1;
}

extern "C" int modg_world_episode_name(int area, int episode, char* out, int size)
{
	if (!out || size <= 0)
		return 0;
	out[0]          = 0;
	const int shine = modg_world_episode_shine(area, episode);
	void* bmg       = JKRFileLoader::getGlbResource("/common/2d/scenarioname.bmg");
	if (shine < 0 || shine >= (int)(sizeof scScenarioNameTable / sizeof scScenarioNameTable[0]) || !bmg)
		return 0;
	return plainText(out, size, SMSGetMessageData(bmg, SMS_getNormalStage(shine)));
}

extern "C" int modg_world_has_cutscene(const char* name)
{
	if (!name || !*name || !s_stageReady || !gpCamera)
		return 0;
	if (gpCamera->unk2B0 && gpCamera->unk2B0->isFileExist(name))
		return 1;
	return gpCamMapToolTable && gpCamMapToolTable->searchF(JDrama::TNameRef::calcKeyCode(name), name) ? 1 : 0;
}

extern "C" int modg_world_cutscene_playing(void)
{
	TMarDirector* d = s_stageReady ? gpMarDirector : 0;
	return d && (d->isTalkOrDemoModeNow() || d->checkFlag(TMarDirector::DIRECTOR_FLAG_DEMO_PENDING)
	             || (gpCamera && gpCamera->isDemoCamera()))
	           ? 1
	           : 0;
}

extern "C" int modg_world_play_cutscene(const char* name, const SmsVec3* focus, int frames)
{
	TMarDirector* d = s_stageReady ? gpMarDirector : 0;
	if (!d || !gpMarioOriginal || d->mState != TMarDirector::STATE_UNK4 || modg_world_cutscene_playing()
	    || !modg_world_has_cutscene(name)
	    || d->checkFlag(TMarDirector::DIRECTOR_FLAG_SHINE_TAKEN | TMarDirector::DIRECTOR_FLAG_STAGE_TRANSITION_PENDING))
		return 0;
	snprintf(s_cutsceneName, sizeof s_cutsceneName, "%s", name);
	if (focus)
		s_cutsceneFocus.set(focus->x, focus->y, focus->z);
	d->fireStartDemoCamera(s_cutsceneName, focus ? &s_cutsceneFocus : 0, frames, 0.0f, true, 0, 0, 0, 0);
	return 1;
}
