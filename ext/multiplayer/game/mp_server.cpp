// Servers' game side (mp_server.hpp): what a server mod on the host has this
// game do, through the commands of mp_game_api.h. Every game carries them
// out, so a player joining a server needs nothing but the game: the server
// mod only says when and where (a banner now, the clock counting, the player
// onto a Blooper there), and this game shows and does it with its own models,
// sounds and HUD. Game code, built with the game's flags: C++03, fixed arrays.
//
// A command's state lasts for the stage session it came in: a new session
// (the host moving everyone on) starts this game afresh, and the host replays
// the session's sticky commands as each player arrives in it.
#include "mp_server.hpp"

#include "../mp_game_api.h"
#include "mp_puppet.hpp"
#include "port_framerate.h"

#include <Camera/Camera.hpp>
#include <Enemy/Conductor.hpp>
#include <Enemy/Graph.hpp>
#include <GC2D/BoundPane.hpp>
#include <GC2D/ConsoleStr.hpp>
#include <GC2D/ExPane.hpp>
#include <GC2D/GCConsole2.hpp>
#include <JSystem/JDrama/JDRViewObjPtrList.hpp>
#include <JSystem/JKernel/JKRFileLoader.hpp>
#include <MSound/MSound.hpp>
#include <MSound/MSoundBGM.hpp>
#include <MoveBG/Item.hpp>
#include <MoveBG/MapObjRailBlock.hpp>
#include <MoveBG/MapObjRicco.hpp>
#include <Map/Map.hpp>
#include <Map/MapData.hpp>
#include <MarioUtil/RumbleMgr.hpp>
#include <Player/Mario.hpp>
#include <Player/MarioStatus.hpp>
#include <Player/WaterGun.hpp>
#include <Strategic/LiveManager.hpp>
#include <System/Application.hpp>
#include <System/EventWatcher.hpp>
#include <System/FlagManager.hpp>
#include <System/MarDirector.hpp>
#include <dolphin/pad.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern "C" void GXPC_ExtOverlaySet(int panel, const char* text) __attribute__((weak));
extern "C" void GXPC_ExtOverlayMarkers(const GXPCMarker* markers, int count) __attribute__((weak));
extern "C" void GXPC_ExtOverlayScreen(u32 rgba) __attribute__((weak));
void MpShowNotice(const char* text); // mp_game.cpp

namespace {

const int kOverlayBanner = 2;     // GXPC_EXT_OVERLAY_BANNER: big, in the middle
const int kOverlayBoard  = 3;     // GXPC_EXT_OVERLAY_BOARD: lines at the left
const int kBoardRows     = 8;
const int kMarkers       = 8;     // GXPC_MAX_MARKERS
const int kMostGliding   = 32;    // objects gliding at once (MPG_OBJECT_MOVE)
const int kMountTries    = 30;    // frames to keep trying to get on (held up landing, talking)
const u32 kShineType     = 0x20000013;
const int kMoveTicks     = 120;   // movement ticks a second (TMarDirector::direct's, at any frame rate)
const float kMostPhase   = 60.0f; // seconds an object may be moved on at once
const int kMostPaced     = 64;    // objects at a speed of their own

TMarDirector* director()
{
	return gpApplication.mAppState == TApplication::APP_STATE_GAMEPLAY ? gpMarDirector : nullptr;
}

unsigned clockMs()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

void overlay(int panel, const char* text)
{
	if (GXPC_ExtOverlaySet)
		GXPC_ExtOverlaySet(panel, text);
}

// what the commands of this session have set up
uint32_t s_session;            // the session they belong to
char s_banner[MPG_CMD_TEXT + 8]; // "#rrggbb text", as the overlay takes it
char s_bannerShown[MPG_CMD_TEXT + 8];
char s_status[MPG_CMD_TEXT];
int s_timerMode;               // MPG_CMD_TIMER's: 0 hidden, 1 stopped, 2 up, 3 down
s32 s_timerBase;               // hundredths, as of s_timerAt
unsigned s_timerAt;
bool s_timerShown;             // the console shows it (this stage's console)
bool s_hold;
int s_watch = -1;              // the slot watched; -1 none
bool s_watching;               // Mario out of play, riding with them
bool s_stay;
MpgCmd s_mount;                // getting on a Blooper, until on (s_mountTries)
int s_mountTries;
u32 s_bgm;                     // the music a command started
unsigned s_readyAt;            // when to start "Ready... Go!" (s_readyDue)
bool s_readyDue;
// the host's own: a stage a server mod asked for
int s_loadArea = -1, s_loadEpisode = -1;
bool s_loading;
int s_titleFrames;
// objects at a speed of their own (MPG_OBJECT_SPEED): this stage's, until the next
struct Paced {
	TLiveActor* actor;
	bool rail; // along a rail (else a spinner)
	f32 times;
	f32 base; // its own: a spinner's turn a tick; along a rail, as its rail last set it
	f32 set;  // what we made it (a rail sets its own again at each point)
};
Paced s_paced[kMostPaced];
int s_pacedCount;
// how this stage's movers were as it loaded, for MPG_OBJECT_PHASE: every
// game's copy then goes on from the same state
struct Start {
	TLiveActor* actor;
	JGeometry::TVec3<f32> position, rotation;
	f32 angle; // a spinner's
	// on a rail: TRailMapObj's, its tracer's, and TNormalLift's waits
	TGraphTracer tracer;
	int step;
	u32 railFlags;
	f32 speed;
	s8 recycle, drawn;
	s16 recycleTimer;
	u16 wait, hold;
	s8 holding;
};
Start s_starts[kMostPaced];
int s_startCount;
// objects gliding to a place (MPG_OBJECT_MOVE), a movement tick at a time
struct Glide {
	TLiveActor* actor;
	JGeometry::TVec3<f32> from, to;
	f32 fromYaw, toYaw;
	int tick, ticks;
};
Glide s_glides[kMostGliding];
int s_glideCount;
// the rules (MPG_CMD_RULES), the HUD's hidden parts (MPG_CMD_HUD) and the
// parts' own shown state from before
u32 s_rules;
u32 s_hudHidden;
// the board's rows (MPG_CMD_BOARD), "#rrggbb text"
char s_board[kBoardRows][MPG_CMD_TEXT + 8];
// the markers (MPG_CMD_MARKER)
struct Marker {
	bool on;
	int slot;                      // over this player, or -1: over `at`
	JGeometry::TVec3<f32> at;
	u32 rgba;
	char text[24];
};
Marker s_markers[kMarkers];
// the screen's colour (MPG_CMD_SCREEN): from when, how long, which way
int s_screenMode;
u32 s_screenRgba;
unsigned s_screenAt;
f32 s_screenSeconds;
// the camera shaking (MPG_CMD_SHAKE): how hard, until when
f32 s_shake;
unsigned s_shakeAt, s_shakeMs;
// objects a command hid (MPG_OBJECT_HIDE), for mpg_objects: hidden, not gone
TLiveActor* s_hidden[256];
int s_hiddenCount;
// items made untouchable by MPG_RULE_NO_PICKUPS, to give back
TLiveActor* s_untouchable[256];
int s_untouchableCount;

void resetSession()
{
	s_banner[0] = 0;
	s_status[0] = 0;
	s_timerMode = 0;
	s_hold      = false;
	s_watch     = -1;
	s_stay      = false;
	s_mountTries = 0;
	s_readyDue   = false;
	s_rules      = 0;
	s_untouchableCount = 0; // the stage's items go with it
	s_hudHidden  = 0;
	memset(s_board, 0, sizeof s_board);
	if (GXPC_ExtOverlaySet)
		GXPC_ExtOverlaySet(kOverlayBoard, "");
	memset(s_markers, 0, sizeof s_markers);
	if (GXPC_ExtOverlayMarkers)
		GXPC_ExtOverlayMarkers(nullptr, 0);
	s_screenMode = MPG_SCREEN_CLEAR;
	if (GXPC_ExtOverlayScreen)
		GXPC_ExtOverlayScreen(0);
	s_shake = 0.0f;
	if (s_watching) {
		s_watching = false;
		if (TMario* m = gpMarioOriginal) {
			m->offFlag(MARIO_FLAG_IS_PERFORMING);
			m->offHitFlag(HIT_FLAG_NO_COLLISION);
		}
	}
}

bool surfing(TMario* m) { return m && m->checkStatusType(MARIO_STATUS_FLAG_UNK10000) && m->mSurfGesso; }

// ------------------------------------------------------------------- the HUD's clock

s32 timerNow()
{
	const s32 gone = (s32)((clockMs() - s_timerAt) / 10);
	switch (s_timerMode) {
	case 2: return s_timerBase + gone;
	case 3: return s_timerBase > gone ? s_timerBase - gone : 0;
	default: return s_timerBase;
	}
}

void updateTimer(TMarDirector* dir)
{
	TGCConsole2* console = dir ? dir->getConsole() : nullptr;
	if (!console)
		return;
	if (s_timerMode == 0) {
		if (s_timerShown)
			console->startDisappearTimer();
		s_timerShown = false;
		return;
	}
	if (!s_timerShown) {
		console->startAppearTimer(0, 0); // minutes, seconds, hundredths: set each frame
		s_timerShown = true;
	}
	console->setTimer(timerNow());
}

// The game's "Ready... Go!" (its letters are in every stage's HUD, though
// nothing in the game plays them): "Go!" comes kReadyFrames frames later.
const int kReadyFrames = 60;
int readyMs() { return kReadyFrames * 1000 / (port_active_frame_rate > 0 ? port_active_frame_rate : 30); }
void readyGo(TMarDirector* dir)
{
	TGCConsole2* console = dir ? dir->getConsole() : nullptr;
	TConsoleStr* str     = console ? console->unk94 : nullptr;
	if (!str || str->unk2B4 != 0)
		return;
	str->startAppearReady(); // "Go!" made ready (its panes hidden, its clock at 0)...
	str->unk2B4 = 5;         // ...after "Ready..."
}

// ------------------------------------------------------------------- the stage's actors

// Each live actor of types lo to hi: f(actor) for each.
template <class F> void eachActor(u32 lo, u32 hi, F& f)
{
	if (!gpConductor)
		return;
	for (JGadget::TList<TLiveManager*>::iterator it = gpConductor->unk10.begin(); it != gpConductor->unk10.end(); ++it)
		for (int i = 0; *it && i < (*it)->getObjNum(); i++) {
			TLiveActor* a = (*it)->getObj(i);
			if (!a)
				continue;
			const u32 type = (u32)a->getActorType();
			if (type >= lo && type <= hi)
				f(a);
		}
}

struct SetActor {
	int mode;
	int count;
	void operator()(TLiveActor* a)
	{
		count++;
		if (mode == 2) {
			if (TShine* shine = dynamic_cast<TShine*>(a)) {
				if (!shine->checkLiveFlag(LIVE_FLAG_DEAD))
					shine->kill();
				return;
			}
		}
		// out of the stage, as event scripts take things away
		// (setDead4LiveActor; a lone NO_COLLISION an item's perform undoes)
		if (mode) {
			a->onLiveFlag(LIVE_FLAG_DEAD);
			a->onHitFlag(HIT_FLAG_NO_COLLISION);
		} else {
			a->offLiveFlag(LIVE_FLAG_DEAD);
			a->offHitFlag(HIT_FLAG_NO_COLLISION);
		}
	}
};

struct FirstActor {
	TLiveActor* found;
	void operator()(TLiveActor* a)
	{
		if (!found)
			found = a;
	}
};

// Onto a Blooper of this game's own, at the command's place: as touching it does.
bool mount(const MpgCmd& c)
{
	TMario* m = gpMarioOriginal;
	if (!m || MpServerDedicated())
		return true;
	if (surfing(m))
		return true;
	FirstActor first = {nullptr};
	eachActor((u32)c.i[0], (u32)c.i[1], first);
	TItem* blooper = (TItem*)first.found;
	if (!blooper)
		return false;
	blooper->offLiveFlag(LIVE_FLAG_DEAD);
	blooper->offHitFlag(HIT_FLAG_NO_COLLISION);
	blooper->mPosition.set(c.f[0], c.f[1], c.f[2]);
	m->mPosition   = blooper->mPosition;
	m->mForwardVel = 0.0f;
	m->mVel.set(0.0f, 0.0f, 0.0f);
	m->mFaceAngle.y = (s16)(c.f[3] * (65536.0f / 360.0f));
	blooper->taken(m);
	return surfing(m);
}

// The stage's event script loaded from `path`, stopped (a course's time trial,
// a story event).
void stopScript(TMarDirector* dir, const char* path)
{
	void* script = JKRGetResource(path);
	bool stopped = false;
	if (script && dir->unk80) {
		JGadget::TList_pointer<JDrama::TViewObj*>& objs = dir->unk80->getChildren();
		for (JGadget::TList_pointer<JDrama::TViewObj*>::iterator it = objs.begin(); it != objs.end(); ++it) {
			TEventWatcher* w = dynamic_cast<TEventWatcher*>(*it);
			if (w && w->mBinary && (void*)w->mBinary->getHeader() == script) {
				w->mInterp = nullptr;
				stopped    = true;
			}
		}
	}
	fprintf(stderr, "[server] script %s %s\n", path, stopped ? "stopped" : "not running here");
}

// ------------------------------------------------------------------- one object

// On a rail of the stage's (a lift, a moving platform), not a dummy one.
TRailMapObj* onRail(TLiveActor* a)
{
	TRailMapObj* rail = dynamic_cast<TRailMapObj*>(a);
	const TGraphWeb* graph = rail && rail->getTracer() ? rail->getTracer()->getGraph() : nullptr;
	return graph && !graph->isDummy() ? rail : nullptr;
}

// The stage's movers as it loaded (stage setup, before it plays).
struct TakeStart {
	void operator()(TLiveActor* a)
	{
		TRollBlock* roll  = dynamic_cast<TRollBlock*>(a);
		TRailMapObj* rail = roll ? nullptr : onRail(a);
		if ((!roll && !rail) || s_startCount >= kMostPaced)
			return;
		Start& s   = s_starts[s_startCount++];
		s.actor    = a;
		s.position = a->mPosition;
		s.rotation = a->mRotation;
		s.angle    = roll ? roll->unk138 : 0.0f;
		if (!rail)
			return;
		s.tracer       = *rail->getTracer();
		s.step         = rail->unk13C;
		s.railFlags    = rail->unk140;
		s.speed        = rail->unk144;
		s.recycle      = rail->unk148;
		s.recycleTimer = rail->unk14A;
		s.drawn        = rail->unk14C;
		if (TNormalLift* lift = dynamic_cast<TNormalLift*>(a)) {
			s.wait    = lift->unk150;
			s.hold    = lift->unk152;
			s.holding = lift->unk158;
		}
	}
};

Paced* pacedOf(TLiveActor* a)
{
	for (int k = 0; k < s_pacedCount; k++)
		if (s_paced[k].actor == a)
			return &s_paced[k];
	return nullptr;
}

// Along a rail, our speed: again where the rail has set its own (at each
// point), or now from `base` (own).
void pace(Paced& p, bool own = false)
{
	TRailMapObj* rail = static_cast<TRailMapObj*>(p.actor);
	if (!own) {
		if (rail->unk144 == p.set)
			return;
		p.base = rail->unk144;
	}
	rail->unk144 = p.set = p.base * p.times;
	rail->resetStep(rail->unk144);
}

bool setSpeed(TLiveActor* a, f32 times)
{
	TRollBlock* roll  = dynamic_cast<TRollBlock*>(a);
	TRailMapObj* rail = roll ? nullptr : onRail(a);
	if (!roll && !rail)
		return false;
	Paced* p = pacedOf(a);
	if (!p) {
		if (s_pacedCount >= kMostPaced)
			return false;
		p        = &s_paced[s_pacedCount++];
		p->actor = a;
		p->rail  = rail != nullptr;
		p->base  = roll ? roll->unk13C : rail->unk144;
	}
	if (times > 20.0f || times < -20.0f)
		times = times > 0 ? 20.0f : -20.0f;
	if (roll) {
		p->times     = times; // the other way round too
		roll->unk13C = p->set = p->base * times;
	} else {
		p->times = times < 0 ? -times : times;
		if (p->times < 0.05f)
			p->times = 0.05f; // along a rail it must get somewhere
		pace(*p, true);
	}
	return true;
}

// Back to how it was as the stage loaded, then `seconds` of its own movement
// at once: the same place in every game, whenever it loaded the stage.
bool setPhase(TLiveActor* a, f32 seconds)
{
	Start* s = nullptr;
	for (int k = 0; k < s_startCount && !s; k++)
		if (s_starts[k].actor == a)
			s = &s_starts[k];
	if (!s)
		return false;
	a->mPosition = s->position;
	a->mRotation = s->rotation;
	if (TRollBlock* roll = dynamic_cast<TRollBlock*>(a))
		roll->unk138 = s->angle;
	Paced* p = pacedOf(a);
	if (TRailMapObj* rail = onRail(a)) {
		*rail->unk138 = s->tracer;
		rail->unk13C  = s->step;
		rail->unk140  = s->railFlags;
		rail->unk144  = s->speed;
		rail->unk148  = s->recycle;
		rail->unk14A  = s->recycleTimer;
		rail->unk14C  = s->drawn;
		if (TNormalLift* lift = dynamic_cast<TNormalLift*>(a)) {
			lift->unk150 = s->wait;
			lift->unk152 = s->hold;
			lift->unk158 = s->holding;
		}
		if (p) {
			p->base = s->speed;
			pace(*p, true);
		}
	} else {
		p = nullptr; // a spinner keeps the speed it was given: nothing to keep up
	}
	if (seconds > kMostPhase)
		seconds = kMostPhase;
	// its own movement (moveObject's, without the rest): its collision follows
	// on the next movement tick, as the map's lists take one a tick
	const int ticks = seconds > 0 ? (int)(seconds * kMoveTicks + 0.5f) : 0;
	for (int k = 0; k < ticks; k++) {
		a->mLinearVelocity.zero();
		a->mAngularVelocity.zero();
		a->control();
		a->mPosition += a->mLinearVelocity;
		a->mRotation += a->mAngularVelocity;
		if (p)
			pace(*p);
	}
	return true;
}

bool hiddenByUs(const TLiveActor* a)
{
	for (int k = 0; k < s_hiddenCount; k++)
		if (s_hidden[k] == a)
			return true;
	return false;
}

void markHidden(TLiveActor* a, bool hidden)
{
	for (int k = 0; k < s_hiddenCount; k++)
		if (s_hidden[k] == a) {
			if (!hidden)
				s_hidden[k] = s_hidden[--s_hiddenCount];
			return;
		}
	if (hidden && s_hiddenCount < 256)
		s_hidden[s_hiddenCount++] = a;
}

// Its collision where it is now (a map object that has any, in the stage).
void collisionHere(TMapObjBase* o)
{
	if (o && !o->checkLiveFlag(LIVE_FLAG_DEAD))
		o->setUpCurrentMapCollision();
}

// Gliding to the place, a tick at a time: where it goes back to as well.
void glideStart(TLiveActor* a, const MpgCmd& c)
{
	Glide* g = nullptr;
	for (int k = 0; k < s_glideCount && !g; k++)
		if (s_glides[k].actor == a)
			g = &s_glides[k]; // a new place for one on its way
	if (!g) {
		if (s_glideCount >= kMostGliding)
			return;
		g = &s_glides[s_glideCount++];
	}
	g->actor   = a;
	g->from    = a->mPosition;
	g->fromYaw = a->mRotation.y;
	g->to.set(c.f[0], c.f[1], c.f[2]);
	g->toYaw = c.f[3];
	g->tick  = 0;
	g->ticks = c.f[4] > 0.0f ? (int)(c.f[4] * kMoveTicks + 0.5f) : 0;
	if (g->ticks > 600 * kMoveTicks)
		g->ticks = 600 * kMoveTicks;
}

void glideTick()
{
	for (int k = 0; k < s_glideCount;) {
		Glide& g      = s_glides[k];
		TLiveActor* a = g.actor;
		g.tick++;
		const f32 t = g.ticks > 0 && g.tick < g.ticks ? (f32)g.tick / (f32)g.ticks : 1.0f;
		const f32 e = t * t * (3.0f - 2.0f * t); // easing in and out
		a->mPosition.set(g.from.x + (g.to.x - g.from.x) * e, g.from.y + (g.to.y - g.from.y) * e,
		                 g.from.z + (g.to.z - g.from.z) * e);
		a->mRotation.y = g.fromYaw + (g.toYaw - g.fromYaw) * e;
		TMapObjBase* o = dynamic_cast<TMapObjBase*>(a);
		if (o) {
			o->mInitialPosition   = a->mPosition;
			o->mInitialRotation.y = a->mRotation.y;
		}
		if (t >= 1.0f) {
			collisionHere(o);
			s_glides[k] = s_glides[--s_glideCount];
		} else {
			k++;
		}
	}
}

void object(TLiveActor* a, const MpgCmd& c)
{
	TMapObjBase* o = dynamic_cast<TMapObjBase*>(a);
	switch (c.i[3]) {
	case MPG_OBJECT_SCALE:
		if (c.f[0] <= 0.0f || c.f[1] <= 0.0f || c.f[2] <= 0.0f)
			break;
		a->mScaling.set(c.f[0], c.f[1], c.f[2]);
		if (o)
			o->mInitialScaling = a->mScaling;
		collisionHere(o);
		break;
	case MPG_OBJECT_TURN:
		if (onRail(a))
			break;
		a->mRotation.set(c.f[0], c.f[1], c.f[2]);
		if (o)
			o->mInitialRotation = a->mRotation;
		collisionHere(o);
		break;
	case MPG_OBJECT_MOVE:
		if (!onRail(a))
			glideStart(a, c);
		break;
	case MPG_OBJECT_KILL:
		markHidden(a, false);
		if (o) {
			o->kill();
		} else {
			SetActor set = {2, 0};
			set(a);
		}
		break;
	case MPG_OBJECT_PLACE:
		if (onRail(a))
			break; // its rail says where it goes
		a->mPosition.set(c.f[0], c.f[1], c.f[2]);
		a->mRotation.y = c.f[3];
		if (o) {
			o->mInitialPosition   = a->mPosition;
			o->mInitialRotation.y = c.f[3];
			if (!o->checkLiveFlag(LIVE_FLAG_DEAD))
				o->setUpCurrentMapCollision();
		}
		break;
	case MPG_OBJECT_SPEED:
		setSpeed(a, c.f[0]);
		break;
	case MPG_OBJECT_PHASE:
		setPhase(a, c.f[0]);
		break;
	case MPG_OBJECT_SHOW:
	case MPG_OBJECT_HIDE: {
		const bool hide = c.i[3] == MPG_OBJECT_HIDE;
		markHidden(a, hide);
		if (!o) {
			SetActor set = {hide ? 1 : 0, 0};
			set(a);
		} else if (hide && !o->checkLiveFlag(LIVE_FLAG_DEAD)) {
			o->makeObjDead(); // its collision too: nothing left to bump into
		} else if (!hide && o->checkLiveFlag(LIVE_FLAG_DEAD)) {
			o->makeObjAppeared();
		}
		break;
	}
	default:
		break;
	}
}

// The objects a command names: of its types and kind (its text, as the stage
// names the object: "WoodBlockCone"; "" any), the i[2]th of them, or each (-1).
struct EachObject {
	const MpgCmd* c;
	int seen;
	int count;
	JGeometry::TVec3<f32> was; // the first one's place before (for the log)
	void operator()(TLiveActor* a)
	{
		if (c->text[0] == '@') { // a copy SPAWN made, by its id
			if (a != MpSpawned(atoi(c->text + 1)))
				return;
		} else if (c->text[0]) {
			TMapObjBase* o = dynamic_cast<TMapObjBase*>(a);
			if (!o || !o->unkF4 || strcmp(o->unkF4, c->text) != 0)
				return;
		}
		if (c->i[2] >= 0 && seen++ != c->i[2])
			return;
		if (count++ == 0)
			was = a->mPosition;
		object(a, *c);
	}
};

// ------------------------------------------------------------------- the player

void player(TMario* m, const MpgCmd& c)
{
	if (!m || MpServerDedicated())
		return;
	switch (c.i[0]) {
	case MPG_PLAYER_LOSE_LIFE:
		if (!m->checkFlag(MARIO_FLAG_IS_PERFORMING))
			m->loserExec(); // the game's own miss: past the damage hooks
		break;
	case MPG_PLAYER_LAUNCH: {
		JGeometry::TVec3<f32> dir(c.f[0], c.f[1], c.f[2]);
		const f32 len = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
		if (len <= 0.0f || !(c.f[3] > 0.0f))
			break;
		dir.scale(1.0f / len);
		if (surfing(m) || m->onYoshi())
			break; // a rider stays on: dismount first
		m->changePlayerStatus(MARIO_STATUS_THROWN_DOWN, 0, true);
		m->throwMario(dir, c.f[3] < 200.0f ? c.f[3] : 200.0f);
		break;
	}
	case MPG_PLAYER_DISMOUNT:
		if (surfing(m)) {
			// as a crash throws him off, without the crash's damage
			m->changePlayerStatus(MARIO_STATUS_JUMP_BACK_DOWN, 0, true);
			m->mVel.y      = 50.0f;
			m->mForwardVel = -8.0f;
		} else if (m->onYoshi()) {
			m->getOffYoshi(false);
		}
		break;
	case MPG_PLAYER_HEALTH: {
		const int most = m->mDeParams.mHPMax.get();
		m->mHealth     = (s16)(c.i[1] < 0 ? 0 : c.i[1] > most ? most : c.i[1]);
		break;
	}
	case MPG_PLAYER_WATER:
		if (TWaterGun* gun = m->mWaterGun) {
			const int pct      = c.i[1] < 0 ? 0 : c.i[1] > 100 ? 100 : c.i[1];
			gun->mCurrentWater = gun->getMaxWater() * pct / 100;
		}
		break;
	case MPG_PLAYER_NOZZLE:
		if (TWaterGun* gun = m->mWaterGun)
			if (c.i[1] >= 0 && c.i[1] <= 5 && c.i[1] != 3) // not Yoshi's
				gun->changeNozzle((TWaterGun::TNozzleType)c.i[1], true);
		break;
	default:
		break;
	}
}

// ------------------------------------------------------------------- the rules

// MPG_RULE_NO_PICKUPS: the stage's items out of reach (as a hidden one is)
// while it holds, given back after.
struct Untouchable {
	void operator()(TLiveActor* a)
	{
		if (s_untouchableCount >= 256 || a->checkLiveFlag(LIVE_FLAG_DEAD) || a->checkHitFlag(HIT_FLAG_NO_COLLISION)
		    || !dynamic_cast<TItem*>(a))
			return;
		a->onHitFlag(HIT_FLAG_NO_COLLISION);
		s_untouchable[s_untouchableCount++] = a;
	}
};

void pickupsTick()
{
	if (!(s_rules & MPG_RULE_NO_PICKUPS)) {
		for (int k = 0; k < s_untouchableCount; k++)
			s_untouchable[k]->offHitFlag(HIT_FLAG_NO_COLLISION);
		s_untouchableCount = 0;
		return;
	}
	Untouchable take;
	eachActor(0, 0xFFFFFFFFu, take);
}

// ------------------------------------------------------------------- the HUD

// The HUD's parts hidden (s_hudHidden), each frame, as the game shows them
// again of its own; shown once when no longer hidden.
J2DPane* hudPane(TGCConsole2* c, u32 bit)
{
	switch (bit) {
	case MPG_HUD_COINS: return c->unk108 ? c->unk108->getPane() : nullptr;
	case MPG_HUD_SHINES: return c->unk140 ? c->unk140->getPane() : nullptr;
	case MPG_HUD_BLUE_COINS: return c->unk160 ? c->unk160->getPane() : nullptr;
	case MPG_HUD_HEALTH: return c->unk1C4 ? c->unk1C4->getPane() : nullptr;
	case MPG_HUD_WATER: return c->unk2F8 ? c->unk2F8->getPane() : nullptr;
	case MPG_HUD_LIVES: return c->unk3A8 ? c->unk3A8->getPane() : nullptr;
	case MPG_HUD_RED_COINS: return c->unk428 ? c->unk428->getPane() : nullptr;
	default: return nullptr;
	}
}

void hudFrame(TMarDirector* dir)
{
	static u32 s_hiddenWas;
	TGCConsole2* console = dir ? dir->getConsole() : nullptr;
	if (!console)
		return;
	for (u32 bit = 1; bit <= MPG_HUD_RED_COINS; bit <<= 1) {
		J2DPane* pane = hudPane(console, bit);
		if (!pane)
			continue;
		if (s_hudHidden & bit)
			pane->hide();
		else if (s_hiddenWas & bit)
			pane->show(); // the game's own animations take it from here
	}
	if (s_hudHidden & MPG_HUD_WATER)
		console->unk45 = console->unk46 = 0; // the gauge's back, drawn apart from its pane
	s_hiddenWas = s_hudHidden;
}

// ------------------------------------------------------------------- markers, the board, the screen

void boardShow()
{
	char text[kBoardRows * (MPG_CMD_TEXT + 9)];
	int last = -1, n = 0;
	for (int r = 0; r < kBoardRows; r++)
		if (s_board[r][0])
			last = r;
	text[0] = 0;
	for (int r = 0; r <= last; r++)
		n += snprintf(text + n, sizeof text - n, "%s%s", r ? "\n" : "", s_board[r][0] ? s_board[r] : " ");
	if (GXPC_ExtOverlaySet)
		GXPC_ExtOverlaySet(kOverlayBoard, text);
}

// A point in the world on the game's picture (0..1 from its top left), as
// the game camera saw it last.
bool onScreen(const JGeometry::TVec3<f32>& p, f32* x, f32* y)
{
	CPolarSubCamera* cam = gpCamera;
	if (!cam)
		return false;
	MtxPtr view = cam->getUnk1EC();
	f32(*proj)[4] = (f32(*)[4])cam->getUnk16C();
	f32 v[3];
	for (int r = 0; r < 3; r++)
		v[r] = view[r][0] * p.x + view[r][1] * p.y + view[r][2] * p.z + view[r][3];
	f32 clip[4];
	for (int r = 0; r < 4; r++)
		clip[r] = proj[r][0] * v[0] + proj[r][1] * v[1] + proj[r][2] * v[2] + proj[r][3];
	if (clip[3] <= 1.0f)
		return false; // behind the camera
	*x = (clip[0] / clip[3] + 1.0f) * 0.5f;
	*y = (1.0f - clip[1] / clip[3]) * 0.5f;
	return *x > -0.1f && *x < 1.1f && *y > -0.1f && *y < 1.1f;
}

void markersFrame()
{
	if (!GXPC_ExtOverlayMarkers)
		return;
	GXPCMarker out[kMarkers];
	int n = 0;
	for (int k = 0; k < kMarkers; k++) {
		const Marker& m = s_markers[k];
		if (!m.on)
			continue;
		JGeometry::TVec3<f32> at = m.at;
		if (m.slot >= 0) {
			if (m.slot == mpg_slot()) {
				TMario* me = gpMarioOriginal;
				if (!me || MpServerDedicated())
					continue;
				at = me->mPosition;
			} else {
				TMpPuppet* p = MpWatchable(m.slot) ? MpPuppet(m.slot) : nullptr;
				if (!p)
					continue;
				at = p->mPosition;
			}
			at.y += 260.0f; // over their cap, above the name
		}
		GXPCMarker& o = out[n];
		if (!onScreen(at, &o.x, &o.y))
			continue;
		snprintf(o.text, sizeof o.text, "%s", m.text);
		o.alpha = 1.0f;
		o.rgba  = m.rgba ? m.rgba : 0xFFD020FFu;
		n++;
	}
	GXPC_ExtOverlayMarkers(out, n);
}

void screenFrame()
{
	if (!GXPC_ExtOverlayScreen)
		return;
	const f32 t = s_screenSeconds > 0.0f ? (f32)(clockMs() - s_screenAt) / (s_screenSeconds * 1000.0f) : 1.0f;
	const f32 u = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
	f32 level;
	switch (s_screenMode) {
	case MPG_SCREEN_FADE_OUT: level = u; break;          // to the colour, and held there
	case MPG_SCREEN_FADE_IN: level = 1.0f - u; break;    // from it
	case MPG_SCREEN_FLASH: level = 1.0f - u; break;
	default: level = 0.0f; break;
	}
	const u32 most = (s_screenRgba & 0xFF) ? (s_screenRgba & 0xFF) : 0xFF;
	GXPC_ExtOverlayScreen((s_screenRgba & 0xFFFFFF00u) | (u32)(most * level + 0.5f));
}

// ------------------------------------------------------------------- each command

void run(TMarDirector* dir, const MpgCmd& c)
{
	TMario* m = gpMarioOriginal;
	switch (c.type) {
	case MPG_CMD_BANNER:
		if (c.text[0])
			snprintf(s_banner, sizeof s_banner, "#%06x %s", (unsigned)((u32)c.i[0] >> 8), c.text);
		else
			s_banner[0] = 0;
		break;
	case MPG_CMD_NOTICE:
		MpShowNotice(c.text);
		break;
	case MPG_CMD_STATUS:
		snprintf(s_status, sizeof s_status, "%s", c.text);
		break;
	case MPG_CMD_TIMER:
		s_timerMode = c.i[0] >= 0 && c.i[0] <= 3 ? c.i[0] : 0;
		s_timerBase = c.i[1] > 0 ? c.i[1] : 0;
		s_timerAt   = clockMs();
		break;
	case MPG_CMD_READY_GO:
		// "Go!" i[0] ms from now: "Ready..." started that long before at this
		// game's frame rate (its letters move a frame at a time)
		s_readyAt = clockMs() + (c.i[0] > readyMs() ? (unsigned)(c.i[0] - readyMs()) : 0);
		s_readyDue = true;
		break;
	case MPG_CMD_SOUND:
		if (c.i[0] != 0 && c.i[1] == 1) {
			// in the world: the sound reads its place while it plays
			static Vec s_at[8];
			static int s_next;
			Vec& at = s_at[s_next++ & 7];
			at.x = c.f[0], at.y = c.f[1], at.z = c.f[2];
			SMSGetMSound()->startSoundActor((u32)c.i[0], &at, 0, nullptr, 0, 4);
		} else if (c.i[0] != 0) {
			SMSGetMSound()->startSoundSystemSE((u32)c.i[0], 0, nullptr, 0);
		}
		break;
	case MPG_CMD_MUSIC:
		if (c.i[0] != 0) {
			s_bgm = (u32)c.i[0];
			MSBgm::startBGM(s_bgm);
		} else if (s_bgm) {
			MSBgm::stopBGM(s_bgm, 30);
			s_bgm = 0;
		}
		break;
	case MPG_CMD_WARP:
		if (m && !MpServerDedicated()) {
			JGeometry::TVec3<f32> at(c.f[0], c.f[1], c.f[2]);
			m->warpRequest(at, c.f[3]);
		}
		break;
	case MPG_CMD_HOLD:
		s_hold = c.i[0] != 0;
		break;
	case MPG_CMD_MOUNT:
		s_mount      = c;
		s_mountTries = mount(c) ? 0 : 1;
		break;
	case MPG_CMD_WATCH:
		s_watch = c.i[0] >= 0 && c.i[0] < MPG_MAX_PLAYERS && c.i[0] != mpg_slot() ? c.i[0] : -1;
		if (s_watch < 0 && s_watching && m) {
			s_watching = false;
			m->offFlag(MARIO_FLAG_IS_PERFORMING);
			m->offHitFlag(HIT_FLAG_NO_COLLISION);
		}
		break;
	case MPG_CMD_ACTORS: {
		SetActor set = {c.i[2], 0};
		eachActor((u32)c.i[0], (u32)c.i[1], set);
		fprintf(stderr, "[server] actors %x-%x: %d %s\n", (unsigned)c.i[0], (unsigned)c.i[1], set.count,
		        c.i[2] == 0 ? "shown" : c.i[2] == 1 ? "hidden" : "gone");
		break;
	}
	case MPG_CMD_STOP_SCRIPT:
		stopScript(dir, c.text);
		break;
	case MPG_CMD_STAY:
		s_stay = c.i[0] != 0;
		break;
	case MPG_CMD_SPAWN:
		MpSpawn(c);
		break;
	case MPG_CMD_PLAYER:
		player(m, c);
		break;
	case MPG_CMD_RULES:
		s_rules = (u32)c.i[0];
		break;
	case MPG_CMD_HUD:
		s_hudHidden = (u32)c.i[0] & MPG_HUD_ALL;
		break;
	case MPG_CMD_BOARD:
		for (int r = 0; r < kBoardRows; r++)
			if (c.i[0] == r || c.i[0] < 0) {
				if (c.text[0] && c.i[0] >= 0)
					snprintf(s_board[r], sizeof s_board[r], "#%06x %s",
					         c.i[1] ? (unsigned)((u32)c.i[1] >> 8) : 0xFFFFFFu, c.text);
				else
					s_board[r][0] = 0;
			}
		boardShow();
		break;
	case MPG_CMD_MARKER:
		if (c.i[0] >= 0 && c.i[0] < kMarkers) {
			Marker& mk = s_markers[c.i[0]];
			mk.on      = c.i[3] != 1;
			mk.slot    = c.i[1] >= 0 && c.i[1] < MPG_MAX_PLAYERS ? c.i[1] : -1;
			mk.at.set(c.f[0], c.f[1], c.f[2]);
			mk.rgba = (u32)c.i[2];
			snprintf(mk.text, sizeof mk.text, "%s", c.text);
		}
		break;
	case MPG_CMD_SCREEN:
		s_screenMode    = c.i[0];
		s_screenRgba    = c.i[1] ? (u32)c.i[1] : 0x000000FFu;
		s_screenAt      = clockMs();
		s_screenSeconds = c.f[0] > 0.0f ? c.f[0] : 0.0f;
		break;
	case MPG_CMD_SHAKE:
		s_shake   = c.f[0] > 0.0f ? (c.f[0] < 5.0f ? c.f[0] : 5.0f) : 0.0f;
		s_shakeAt = clockMs();
		s_shakeMs = c.f[1] > 0.0f ? (unsigned)(c.f[1] * 1000.0f) : 0;
		break;
	case MPG_CMD_RUMBLE:
		if (SMSRumbleMgr && !MpServerDedicated())
			SMSRumbleMgr->start(c.i[0] ? 21 : 20, c.i[1] > 1 ? (c.i[1] < 20 ? c.i[1] : 20) : 1, (f32*)nullptr);
		break;
	case MPG_CMD_OBJECT: {
		static const char* const kWhat[] = {"placed", "speed", "phase", "shown",  "hidden",
		                                   "scaled", "turned", "moving", "killed"};
		EachObject each;
		each.c     = &c;
		each.seen  = each.count = 0;
		each.was.set(0.0f, 0.0f, 0.0f);
		eachActor((u32)c.i[0], (u32)c.i[1], each);
		fprintf(stderr, "[server] object %s %x-%x #%d: %d %s (%.2f %.2f %.2f %.2f), the first at %.0f %.0f %.0f\n",
		        c.text[0] ? c.text : "*", (unsigned)c.i[0], (unsigned)c.i[1], (int)c.i[2], each.count,
		        c.i[3] >= 0 && c.i[3] <= MPG_OBJECT_KILL ? kWhat[c.i[3]] : "?", c.f[0], c.f[1], c.f[2], c.f[3],
		        each.was.x, each.was.y, each.was.z);
		break;
	}
	default:
		break;
	}
}

// ------------------------------------------------------------------- the host's own

// A dedicated server's player: out of play, unseen, touching nothing.
void bench()
{
	if (TMario* m = gpMarioOriginal) {
		m->onFlag(MARIO_FLAG_IS_PERFORMING);
		m->onHitFlag(HIT_FLAG_NO_COLLISION);
	}
}

// A dedicated server goes from the title straight to its stage, with a new
// save's flags (as the game starts with): the one a server mod asked for,
// else SMS_MULTIPLAYER_STAGE ("area,episode"), else Delfino Plaza.
void serverTitle(TMarDirector* dir)
{
	if (!dir || gpApplication.mCurrArea.getStage() != 15 || dir->mState != TMarDirector::STATE_UNK4
	    || dir->checkFlag(TMarDirector::DIRECTOR_FLAG_STAGE_TRANSITION_PENDING)) {
		s_titleFrames = 0;
		return;
	}
	if (++s_titleFrames < 30)
		return; // the title's own setup first (about a second)
	s_titleFrames = 0;
	int area = 1, episode = 0;
	if (s_loadArea >= 0) {
		area    = s_loadArea;
		episode = s_loadEpisode;
		s_loadArea = -1;
	} else if (const char* stage = getenv("SMS_MULTIPLAYER_STAGE")) {
		sscanf(stage, "%d,%d", &area, &episode);
	}
	fprintf(stderr, "[server] from the title to area %d, episode %d\n", area, episode);
	TFlagManager::getInstance()->setFlag(0x40003, episode);
	dir->setNextStage((u16)(((area + 1) << 8) | episode), nullptr);
}

// The host to the stage a server mod asked for, afresh: once nothing else is
// on (a fade, a lost life's reload).
void hostLoad(TMarDirector* dir)
{
	if (s_loadArea < 0 || !dir || gpApplication.mCurrArea.getStage() == 15)
		return;
	if (dir->mState != TMarDirector::STATE_UNK4 || dir->checkFlag(TMarDirector::DIRECTOR_FLAG_STAGE_TRANSITION_PENDING))
		return;
	fprintf(stderr, "[server] everyone to area %d, episode %d (a new session)\n", s_loadArea, s_loadEpisode);
	s_loading = true;
	mpg_restart_session(); // a new session even for the stage played now
	TFlagManager::getInstance()->setFlag(0x40003, s_loadEpisode);
	dir->setNextStage((u16)(((s_loadArea + 1) << 8) | s_loadEpisode), nullptr);
	s_loadArea = -1;
}

} // namespace

bool MpServerDedicated() { return mpg_role() == MPG_ROLE_HOST && mpg_dedicated(); }

bool MpServerLoading() { return s_loading; }

const char* MpServerStatusLine() { return s_status; }

void MpServerStageSetup()
{
	s_timerShown = false; // the new stage's console
	s_loading    = false;
	s_watching   = false; // a new Mario
	s_mountTries = 0;
	s_pacedCount = 0;     // the new stage's objects
	s_startCount = 0;
	s_glideCount = 0;
	s_untouchableCount = 0;
	s_hiddenCount = 0;
	TakeStart start;
	eachActor(0, 0xFFFFFFFFu, start);
}

void MpServerFrame(TMarDirector* dir, bool arrived, bool playing)
{
	if (mpg_role() == MPG_ROLE_HOST) {
		int area, episode;
		if (mpg_take_load_stage(&area, &episode)) {
			s_loadArea    = area;
			s_loadEpisode = episode;
		}
		if (MpServerDedicated())
			serverTitle(dir);
		if (playing)
			hostLoad(dir);
	}
	// a new session: what the last one's commands set up is over
	const uint32_t session = mpg_session();
	if (session != s_session) {
		s_session = session;
		resetSession();
	}
	if (playing && dir) {
		(void)arrived;
		MpgCmd c;
		while (mpg_next_cmd(&c))
			run(dir, c);
		if (s_readyDue && (int)(clockMs() - s_readyAt) >= 0) {
			s_readyDue = false;
			readyGo(dir);
		}
		updateTimer(dir);
		hudFrame(dir);
		markersFrame();
		if (s_mountTries > 0 && s_mountTries < kMountTries && !surfing(gpMarioOriginal))
			s_mountTries = mount(s_mount) ? 0 : s_mountTries + 1;
	}
	screenFrame();
	const char* banner = playing || s_banner[0] == 0 ? s_banner : "";
	if (strcmp(banner, s_bannerShown) != 0) {
		snprintf(s_bannerShown, sizeof s_bannerShown, "%s", banner);
		overlay(kOverlayBanner, s_bannerShown);
	}
}

void MpServerTick()
{
	for (int k = 0; k < s_pacedCount; k++)
		if (s_paced[k].rail)
			pace(s_paced[k]);
	glideTick();
	pickupsTick();
	TMario* m = gpMarioOriginal;
	if (!m)
		return;
	if (MpServerDedicated()) {
		bench();
		return;
	}
	// held: a Blooper ridden stops where it is, rather than surfing on unsteered
	if (s_hold && surfing(m))
		m->mForwardVel = 0.0f;
	// watching: out of play, riding with that player (the camera follows Mario)
	if (s_watch >= 0 && MpWatchable(s_watch)) {
		TMpPuppet* p = MpPuppet(s_watch);
		s_watching   = true;
		m->onFlag(MARIO_FLAG_IS_PERFORMING);
		m->onHitFlag(HIT_FLAG_NO_COLLISION);
		m->mPosition        = p->mPosition;
		const MpgPose& pose = p->pose();
		m->mFaceAngle.y     = (s16)(atan2f(pose.mtx[0][2], pose.mtx[2][2]) * (32768.0f / 3.14159265f));
	}
}

void MpServerPadRead(void* status)
{
	PADStatus* pads = (PADStatus*)status;
	if (s_rules & MPG_RULE_NO_FLUDD) {
		pads[0].button &= ~PAD_TRIGGER_R; // FLUDD sprays with R
		pads[0].triggerRight = 0;
	}
	if (s_rules & MPG_RULE_NO_PAUSE)
		pads[0].button &= ~(PAD_BUTTON_START | PAD_TRIGGER_Z); // the pause menu, the map
	if (!s_hold && !s_watching)
		return;
	pads[0].button   = 0;
	pads[0].stickX = pads[0].stickY = pads[0].substickX = pads[0].substickY = 0;
	pads[0].triggerLeft = pads[0].triggerRight = 0;
	pads[0].analogA = pads[0].analogB = 0;
}

int MpServerNoDamage() { return (s_rules & MPG_RULE_NO_DAMAGE) != 0; }

// ------------------------------------------------------------------- what the host's mods ask

namespace {
struct CountObjects {
	const char* kind;
	int index, seen;
	TLiveActor* found;
	void operator()(TLiveActor* a)
	{
		if (kind && kind[0] == '@') {
			if (a != MpSpawned(atoi(kind + 1)))
				return;
		} else if (kind && kind[0]) {
			TMapObjBase* o = dynamic_cast<TMapObjBase*>(a);
			if (!o || !o->unkF4 || strcmp(o->unkF4, kind) != 0)
				return;
		}
		if (seen++ == index)
			found = a;
	}
};
} // namespace

extern "C" int mpg_objects(uint32_t lo, uint32_t hi, const char* kind, int index, MpgObjectInfo* out)
{
	CountObjects count = {kind, index, 0, nullptr};
	if (!director())
		return 0;
	eachActor(lo, hi, count);
	if (index < 0)
		return count.seen;
	TLiveActor* a = count.found;
	if (!a || !out)
		return 0;
	memset(out, 0, sizeof *out);
	TMapObjBase* o = dynamic_cast<TMapObjBase*>(a);
	out->type      = (uint32_t)a->getActorType();
	snprintf(out->kind, sizeof out->kind, "%s", o && o->unkF4 ? o->unkF4 : "");
	const JGeometry::TVec3<f32>* v[3] = {&a->mPosition, &a->mRotation, &a->mScaling};
	float* to[3]                      = {out->position, out->rotation, out->scale};
	for (int k = 0; k < 3; k++)
		to[k][0] = v[k]->x, to[k][1] = v[k]->y, to[k][2] = v[k]->z;
	out->hidden  = hiddenByUs(a);
	out->alive   = !a->checkLiveFlag(LIVE_FLAG_DEAD) || out->hidden;
	out->spawned = MpSpawnedId(a);
	return 1;
}

extern "C" int mpg_ground(float x, float y, float z, MpgGround* out)
{
	if (!gpMap || !director() || !out)
		return 0;
	memset(out, 0, sizeof *out);
	const TBGCheckData* floor = nullptr;
	out->floor                = gpMap->checkGround(x, y, z, &floor);
	if (!floor || floor->isIllegalData())
		return 0; // off the map
	out->floorType = floor->mBGType & 0x3FFF;
	out->water     = floor->isWaterSurface();
	// water over the point: what is found from above it, as Mario does
	const TBGCheckData* top = nullptr;
	const f32 h             = gpMap->checkGround(x, y + 2000.0f, z, &top);
	if (top && !top->isIllegalData() && top->isWaterSurface() && h >= y - 78.0f) {
		out->hasWater = 1;
		out->waterY   = h;
	} else if (out->water) {
		out->hasWater = 1;
		out->waterY   = out->floor;
	}
	const TBGCheckData* roof = nullptr;
	out->roof                = gpMap->checkRoof(x, y, z, &roof);
	return 1;
}

void MpServerCamera(float* eye, float* at)
{
	if (s_shake <= 0.0f)
		return;
	const unsigned gone = clockMs() - s_shakeAt;
	if (gone >= s_shakeMs) {
		s_shake = 0.0f;
		return;
	}
	// a shake dying away: both moved alike, so the view jolts rather than turns
	const f32 left = 1.0f - (f32)gone / (f32)s_shakeMs;
	const f32 amp  = 18.0f * s_shake * left;
	const f32 t    = (f32)gone * 0.001f;
	const f32 dx = amp * sinf(t * 57.0f), dy = amp * sinf(t * 71.0f + 1.3f), dz = amp * sinf(t * 63.0f + 2.1f);
	eye[0] += dx, eye[1] += dy, eye[2] += dz;
	at[0] += dx, at[1] += dy, at[2] += dz;
}

int MpServerAllowLeave(int area)
{
	if (!s_stay || s_loading || area == gpApplication.mCurrArea.getStage())
		return 1; // free, the host's own stage afresh, or back into this stage (a lost life's reload)
	int target, episode;
	return mpg_role() == MPG_ROLE_CLIENT && mpg_target(&target, &episode) && area == target; // following the host
}

int MpServerAfterFileSelect()
{
	if (mpg_role() != MPG_ROLE_HOST || s_loadArea < 0)
		return 0;
	TFlagManager::getInstance()->setFlag(0x40003, s_loadEpisode);
	gpApplication.mNextArea.set((u8)s_loadArea, (u8)s_loadEpisode, 0);
	s_loadArea = -1;
	return 1;
}
