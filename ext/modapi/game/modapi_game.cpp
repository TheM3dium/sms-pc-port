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
#include <Player/Mario.hpp>
#include <Player/WaterGun.hpp>
#include <Player/Yoshi.hpp>
#include <Strategic/HitActor.hpp>
#include <System/Application.hpp>
#include <System/FlagManager.hpp>
#include <System/MarDirector.hpp>

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

void frame()
{
	static uint32_t s_frame;
	if (gameMode() != SMS_MODE_STAGE)
		s_stageReady = false;
	stageStart();
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

int objectHit(void* object, void* sender, unsigned message)
{
	if (!modh_wants(SMS_EV_OBJECT_HIT))
		return 0;
	SmsEvHit ev;
	ev.object  = object ? obj((THitActor*)object) : 0;
	ev.sender  = sender ? obj((THitActor*)sender) : 0;
	ev.message = message;
	return modh_dispatch(SMS_EV_OBJECT_HIT, &ev, sizeof ev);
}

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

int playerDamage(void* m, int* amount)
{
	if (m != (void*)mario() || !modh_wants(SMS_EV_PLAYER_DAMAGE))
		return 0;
	TMario* p = (TMario*)m;
	SmsEvDamage ev;
	ev.amount     = *amount;
	ev.underwater = p->isUnderWater() || p->checkFlag(MARIO_FLAG_HELMET_FLW_CAMERA);
	if (modh_dispatch(SMS_EV_PLAYER_DAMAGE, &ev, sizeof ev))
		return 1;
	*amount = ev.amount;
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
		e.shineCollected      = shineCollected;
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

extern "C" void modg_flag_set(uint32_t id, int32_t value) { TFlagManager::getInstance()->setFlag(id, value); }

extern "C" int modg_warp(int area, int episode)
{
	if (!s_stageReady || !gpMarDirector || area < 0 || area > 254 || episode < 0 || episode > 255)
		return 0;
	s_warpAsked = true;
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
		}
		s.status       = m->mStatus;
		s.status_timer = m->mStatusTimer;
		s.on_ground    = (m->mStatus & MARIO_STATUS_FLAG_JUMPING) || (m->mStatus & MARIO_STATUS_FLAG_SWIMMING) ? 0 : 1;
		s.in_water     = m->checkFlag(MARIO_FLAG_IN_ANY_WATER) || (m->mStatus & MARIO_STATUS_FLAG_SWIMMING) ? 1 : 0;
		s.on_yoshi     = m->onYoshi() ? 1 : 0;
		setVec(&s.floor, m->mFloorPosition.x, m->mFloorPosition.y, m->mFloorPosition.z);
		s.object = obj(m);
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
