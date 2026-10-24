// The multiplayer extension's game half (ext/multiplayer/README.md): what the
// game does with the controller's decisions (mp_game_api.h). Game code, built
// with the game's flags: C++03, fixed arrays, nothing allocated but the
// stage's puppets.
//
// Each frame (port_ext frame hook):
//   1. tell the controller what the game is doing; it takes in the network;
//   2. share the save file's flags (mpg_flags_sync), and take away the Shines
//      and blue coins someone else collected;
//   3. take away the stage's items someone else picked up;
//   4. send our look (pose) for the others' puppets;
//   5. a client follows the host into their stage; the host leaves when a
//      client got a Shine.
#include "../mp_game_api.h"
#include "mp_boss.hpp"
#include "mp_mirror.hpp"
#include "mp_puppet.hpp"
#include "mp_pvp.hpp"
#include "mp_server.hpp"
#include "port_ext.h"
#include "port_framerate.h"

#include <Enemy/BossPakkun.hpp>
#include <Enemy/Conductor.hpp>
#include <Map/Map.hpp>
#include <Map/MapData.hpp>
#include <Map/PollutionManager.hpp>
#include <Enemy/EnemyManager.hpp>
#include <Enemy/SmallEnemy.hpp>
#include <GC2D/ConsoleStr.hpp>
#include <GC2D/GCConsole2.hpp>
#include <GC2D/ScrnFader.hpp>
#include <GC2D/SelectMenu.hpp>
#include <MSound/MSound.hpp>
#include <MSound/MSoundBGM.hpp>
#include <MSound/MSoundSE.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DJoint.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DSys.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DTexture.hpp>
#include <JSystem/JUtility/JUTNameTab.hpp>
#include <M3DUtil/M3UModelMario.hpp>
#include <M3DUtil/MActor.hpp>
#include <Enemy/FruitsBoat.hpp>
#include <Enemy/Killer.hpp>
#include <Enemy/Koopa.hpp>
#include <Enemy/KoopaNerve.hpp>
#include <MoveBG/MapObjSirena.hpp>
#include <MoveBG/ModelGate.hpp>
#include <GC2D/BoundPane.hpp>
#include <GC2D/ExPane.hpp>
#include <JSystem/JUtility/JUTTexture.hpp>
#include <JSystem/J2D/J2DPicture.hpp>
#include <Camera/Camera.hpp>
#include <Camera/CameraMapTool.hpp>
#include <MoveBG/MapObjCorona.hpp>
#include <MoveBG/Item.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <MoveBG/ItemManager.hpp>
#include <NPC/NpcBase.hpp>
#include <MoveBG/MapObjBall.hpp>
#include <MoveBG/MapObjHide.hpp>
#include <MoveBG/MapObjManager.hpp>
#include <MoveBG/MapObjTown.hpp>
#include <Player/ModelWaterManager.hpp>
#include <Player/Mario.hpp>
#include <Player/NozzleBase.hpp>
#include <Player/MarioCap.hpp>
#include <Player/WaterGun.hpp>
#include <Player/Yoshi.hpp>
#include <Strategic/Strategy.hpp>
#include <System/Application.hpp>
#include <System/FlagManager.hpp>
#include <System/MarDirector.hpp>
#include <System/SelectDir.hpp>
#include <System/StageUtil.hpp>
#include <math.h>
#include <new>
#include <typeinfo>
#include <time.h>
#include <dolphin/pad.h>
#include <dolphin/vi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The window's text panels (platform/gx; absent in a build without a window).
extern "C" void GXPC_ExtOverlaySet(int panel, const char* text) __attribute__((weak));
extern "C" void GXPC_ExtOverlayLabels(const GXPCLabel* labels, int count) __attribute__((weak));
enum { OVERLAY_NOTICE = 0, OVERLAY_STATUS = 1 };

namespace {

const int kMaxFlags = 1024;
const int kMaxItems = 2048;
const int kNoticeFrames = 150; // about five seconds at 30 frames a second

int s_role = -1; // MPG_ROLE_*, -1 before the first frame
bool s_playing, s_wasPlaying;
bool s_arrivedNow; // this frame is the first of play in a stage just loaded
int32_t s_flags[kMaxFlags];
uint32_t s_flagIds[kMaxFlags];
int s_flagCount;
uint8_t s_itemAlive[kMaxItems]; // the stage's own items, alive when it started
int s_itemCount;
uint8_t s_itemLive[kMaxItems]; // every item, alive last frame
int s_takenAtSeen;             // the session's taken-at list, looked at so far
int s_fxSent[2], s_fxShown[MPG_MAX_PLAYERS][2]; // debug: effects sent and shown lately
int s_soundSent, s_soundHeard[MPG_MAX_PLAYERS];    // debug: sounds sent and heard lately
int s_lastCoins = -1;          // the coin count after the last sync (the 100-coin Shine)
TMpPuppet* s_puppets[MPG_MAX_PLAYERS];
int s_clock; // frames since the game started

TMpPuppet* s_self; // our own character, when we are not Mario
unsigned s_stageNumber;
// Out of lives: watching another player instead of playing (until the
// session changes: the host moves on, or everyone is out and it restarts).
bool s_spectating;
u32 s_spectateSession;
int s_spectateArea; // the stage they ran out of lives in: anywhere else, they play again
bool s_arrivedHere;  // the stage loaded last is being played (its arrival is past)
int s_watching = -1;  // the slot whose player we watch
bool s_restarting;    // host: everyone is out, the stage is starting again
bool s_missReload;    // this stage again after a lost life (or a game over): no new session
char s_notice[256];
int s_noticeFrames, s_statusFrames;
// Client: we got a Shine and left for the plaza ahead of the host, who is
// still in the stage (saving): wait for the host there, not back in the stage.
int s_shineHoldFrames;
int s_shineHoldArea = -1, s_shineHoldEpisode = -1;
// The Shine Get together (syncShineGets)
bool s_inWinDemo;    // our player was in the Shine Get last frame
bool s_winRemote;    // ...because another player's Shine was played here
int s_winCollector = -1; // ...whose (they hold it here: their character has it, in the middle)
TShine* s_winShine;
int s_shineGetFrames; // a Shine Get played here lately: the flag-only exit stays out of it

int s_titleFrames; // a client's frames on the title screen
TMarDirector* s_readyDir; // the director whose stage stageSetup saw set up

bool enabled() { return s_role > MPG_ROLE_OFF; }

TMarDirector* director()
{
	return gpApplication.mAppState == TApplication::APP_STATE_GAMEPLAY ? gpMarDirector : nullptr;
}

// In a stage (not the title's file select) whose objects are all set up,
// past its opening (the director's setup states): flags can be shared.
bool inStage()
{
	TMarDirector* dir = director();
	return dir && dir == s_readyDir && gpMarioOriginal && gpApplication.mCurrArea.getStage() != 15
	       && dir->mState >= TMarDirector::STATE_UNK4;
}

// Playing it: loaded, not leaving.
bool playing()
{
	TMarDirector* dir = director();
	return inStage() && dir->mState == TMarDirector::STATE_UNK4
	       && !dir->checkFlag(TMarDirector::DIRECTOR_FLAG_STAGE_TRANSITION_PENDING);
}

void overlay(int panel, const char* text)
{
	if (GXPC_ExtOverlaySet)
		GXPC_ExtOverlaySet(panel, text);
}

void showNotice(const char* text)
{
	snprintf(s_notice, sizeof s_notice, "%s", text);
	s_noticeFrames = kNoticeFrames;
	s_statusFrames = kNoticeFrames * 2;
}

// ------------------------------------------------------------------- items

TItemManager* items() { return gpItemManager; }

void snapshotItems()
{
	memset(s_itemAlive, 0, sizeof s_itemAlive);
	s_itemCount   = 0;
	s_takenAtSeen = 0;
	s_lastCoins   = -1;
	TItemManager* mgr = items();
	if (!mgr)
		return;
	s_itemCount = mgr->getObjNum() < kMaxItems ? mgr->getObjNum() : kMaxItems;
	for (int i = 0; i < s_itemCount; i++) {
		TLiveActor* a = mgr->getObj(i);
		s_itemAlive[i] = a && !a->checkLiveFlag(LIVE_FLAG_DEAD);
	}
	memcpy(s_itemLive, s_itemAlive, sizeof s_itemLive);
}

// Items that appear during the stage (from a box, a defeated enemy, a red
// coin switch) are matched by type and place: each game makes its own, a
// little apart where the enemy fell differently.
const f32 kTakenAtRadius = 60.0f;
void removeItem(TItem* item);

bool takenAtMatches(const MpgTakenAt& t, TLiveActor* a)
{
	if ((u8)(a->getActorType() & 0xFF) != t.type)
		return false;
	const f32 dx = a->mPosition.x - t.pos[0], dy = a->mPosition.y - t.pos[1], dz = a->mPosition.z - t.pos[2];
	return dx * dx + dy * dy + dz * dz <= kTakenAtRadius * kTakenAtRadius;
}

bool appearedDuringStage(int i) { return i >= s_itemCount || !s_itemAlive[i]; }

// Switches pressed in any game are pressed in every game (and for a player
// who joins later): a red coin switch, or a switch that throws out hidden
// coins. They go through the taken-at list as a type of their own; the
// coins they bring out are shared like any item that appears.
const u8 kSwitchPressed = 0xF0;
const int kMaxSwitches  = 32;
TMapObjBase* s_switches[kMaxSwitches];
bool s_switchDown[kMaxSwitches];
int s_switchCount;

// The plaza's M gates (Shadow Mario paints them): a stage script opens one
// as its story event plays and sets its save flag; the others' scripts wait
// on their own copies of that event, so there the flag comes (shared) but the
// gate stays shut until the stage loads again. Each game opens a gate whose
// flag is set, as the script does.
TModelGate* s_gates[5];
bool s_gatesFound;

void openGatesFromFlags()
{
	static const char* const names[] = {"Gate", "GateToRicco", "GateToMamma", "GateToMonte", "GateToMare"};
	static const u32 flags[]         = {0x10385, 0x10386, 0x10387, 0x10387, 0x10387}; // as TModelGate's load
	if (!s_gatesFound) {
		s_gatesFound = true;
		for (int i = 0; i < 5; i++)
			s_gates[i] = JDrama::TNameRefGen::search<TModelGate>(names[i]);
	}
	for (int i = 0; i < 5; i++) {
		TModelGate* g = s_gates[i];
		if (g && !(g->mFlags & TModelGate::GATE_FLAG_ACTIVE) && TFlagManager::getInstance()->getBool(flags[i])) {
			fprintf(stderr, "[multiplayer] the %s M gate opened in the host's game: opened here\n", names[i]);
			g->startOpen();
		}
	}
}

void collectSwitches()
{
	s_switchCount = 0;
	if (!gpMapObjManager)
		return;
	for (int i = 0; i < gpMapObjManager->getObjNum() && s_switchCount < kMaxSwitches; i++) {
		TLiveActor* a = gpMapObjManager->getObj(i);
		if (!a || !(dynamic_cast<TRedCoinSwitch*>(a) || dynamic_cast<TMapObjSwitch*>(a)))
			continue;
		s_switches[s_switchCount]   = static_cast<TMapObjBase*>(a);
		s_switchDown[s_switchCount] = a->checkHitFlag(HIT_FLAG_NO_COLLISION) != 0;
		s_switchCount++;
	}
}

// Pressed in another game: as a ground pound would, but without taking this
// player's controls (the red coin switch holds them for its animation) or
// the camera (the coin switch shows where its coins go).
void pressSwitch(int k)
{
	TMapObjBase* sw  = s_switches[k];
	s_switchDown[k]  = true;
	if (TRedCoinSwitch* red = dynamic_cast<TRedCoinSwitch*>(sw)) {
		TMarioGamePad* pad = director() ? director()->getGamePad() : nullptr;
		const s32 held     = pad ? pad->mDisabledFrames : 0;
		red->receiveMessage(gpMarioOriginal, HIT_MESSAGE_HIP_DROP);
		if (pad)
			pad->mDisabledFrames = held;
	} else if (TMapObjSwitch* coins = dynamic_cast<TMapObjSwitch*>(sw)) {
		coins->startBck("objswitch");
		coins->removeMapCollision();
		for (int i = 0; i < coins->unk13C; ++i)
			coins->unk144[i]->action(coins->unk140);
		coins->startStateTimer(coins->unk140);
		coins->onHitFlag(HIT_FLAG_NO_COLLISION);
	}
}

// Debug: red coins out in this stage, not yet taken.
int redCoinsOut()
{
	int n = 0;
	for (int i = 0; gpItemManager && i < gpItemManager->getObjNum(); i++) {
		TLiveActor* a = gpItemManager->getObj(i);
		n += a && a->getActorType() == 0x2000000F && !a->checkLiveFlag(LIVE_FLAG_DEAD) && !a->checkLiveFlag(LIVE_FLAG_HIDDEN);
	}
	return n;
}

void syncSwitches()
{
	static bool redOn;
	static int redFrames;
	const bool on = TFlagManager::getInstance()->getBool(0x50009);
	if (on != redOn && getenv("SMS_MULTIPLAYER_DEBUG"))
		fprintf(stderr, "[multiplayer] red coin switch event %s\n", on ? "on (its timer runs)" : "off");
	redOn = on;
	if (on && getenv("SMS_MULTIPLAYER_DEBUG") && director() && ++redFrames % 150 == 0)
		fprintf(stderr, "[multiplayer] red coin event: %d red coins here, %d of them out (talk/demo %d, on ground %d, "
		        "jumping %d, Mario status %x)\n",
		        (int)TFlagManager::getInstance()->getFlag(0x60000), redCoinsOut(), (int)director()->isTalkOrDemoModeNow(),
		        (int)gpMarioOriginal->isTouchGround4cm(), (int)gpMarioOriginal->checkStatusType(MARIO_STATUS_FLAG_JUMPING),
		        (unsigned)gpMarioOriginal->getStatus());
	if (!on)
		redFrames = 0;
	for (int k = 0; k < s_switchCount; k++) {
		const bool down = s_switches[k]->checkHitFlag(HIT_FLAG_NO_COLLISION) != 0;
		if (down && !s_switchDown[k]) {
			const float pos[3] = {s_switches[k]->mPosition.x, s_switches[k]->mPosition.y, s_switches[k]->mPosition.z};
			mpg_item_taken_at(kSwitchPressed, pos);
		}
		s_switchDown[k] = down;
	}
}

// One from the taken-at list: the nearest switch not yet down, if close.
void pressSwitchAt(const MpgTakenAt& t)
{
	int best    = -1;
	f32 bestD   = 200.0f * 200.0f;
	for (int k = 0; k < s_switchCount; k++) {
		if (s_switchDown[k])
			continue;
		const JGeometry::TVec3<f32>& p = s_switches[k]->mPosition;
		const f32 d = (p.x - t.pos[0]) * (p.x - t.pos[0]) + (p.y - t.pos[1]) * (p.y - t.pos[1])
		              + (p.z - t.pos[2]) * (p.z - t.pos[2]);
		if (d < bestD)
			best = k, bestD = d;
	}
	if (best >= 0)
		pressSwitch(best);
}

// Every frame: items someone else took that this game has too go; and items
// that appear here after someone took theirs (their own red coin switch
// pressed later, say) go as they appear, so nobody gets one twice.
void syncTakenAt()
{
	TItemManager* mgr = items();
	if (!mgr)
		return;
	const int n = mgr->getObjNum() < kMaxItems ? mgr->getObjNum() : kMaxItems;
	syncSwitches();
	MpgTakenAt t;
	for (; mpg_taken_at(s_takenAtSeen, &t); s_takenAtSeen++) {
		if (t.type == kSwitchPressed) {
			pressSwitchAt(t);
			continue;
		}
		for (int i = 0; i < n; i++) {
			TLiveActor* a = mgr->getObj(i);
			if (a && appearedDuringStage(i) && !a->checkLiveFlag(LIVE_FLAG_DEAD) && takenAtMatches(t, a)) {
				if (getenv("SMS_MULTIPLAYER_DEBUG"))
					fprintf(stderr, "[multiplayer] item %d taken in another game: removed\n", i);
				removeItem(static_cast<TItem*>(a));
			}
		}
	}
	for (int i = 0; i < n; i++) {
		TLiveActor* a   = mgr->getObj(i);
		const bool live = a && !a->checkLiveFlag(LIVE_FLAG_DEAD);
		if (live && !s_itemLive[i] && appearedDuringStage(i))
			for (int k = 0; mpg_taken_at(k, &t); k++)
				if (takenAtMatches(t, a)) {
					if (getenv("SMS_MULTIPLAYER_DEBUG"))
						fprintf(stderr, "[multiplayer] item %d appeared, already taken: removed\n", i);
					removeItem(static_cast<TItem*>(a));
					break;
				}
		s_itemLive[i] = a && !a->checkLiveFlag(LIVE_FLAG_DEAD);
	}
}

uint32_t itemKey(int index, const TLiveActor* a) { return (uint32_t)index | ((uint32_t)(a->getActorType() & 0xFF) << 16); }

// Removes an item as TItem::taken does, without anyone getting it.
void removeItem(TItem* item)
{
	if (item->checkLiveFlag(LIVE_FLAG_DEAD))
		return;
	if (item->mContainer)
		item->mContainer->receiveMessage(item, HIT_MESSAGE_UNK8);
	item->kill();
	if (item->checkMapObjFlag(TMapObjBase::MAP_OBJ_FLAG_RESPAWNING)) {
		item->makeObjDefault();
		item->appear();
	}
}

const uint32_t kErasedKey = 0x80000000u; // a taken-list key: a graffiti's change (| number << 24 | manager << 16 | index)
void changeGraffiti(uint32_t where, int changes); // shared objects, below

void takeRemovals()
{
	uint32_t key;
	TItemManager* mgr = items();
	// graffiti changes, the newest of each (a session's list comes all at once)
	uint32_t graffitiWhere[32];
	int graffitiChanges[32], graffitiCount = 0;
	while (mpg_next_removal(&key)) {
		if (key & kErasedKey) {
			const uint32_t where = key & 0x00FFFFFFu;
			const int changes    = (int)(key >> 24 & 0x7F);
			int k                = 0;
			while (k < graffitiCount && graffitiWhere[k] != where)
				k++;
			if (k == graffitiCount && graffitiCount < 32) {
				graffitiWhere[graffitiCount]     = where;
				graffitiChanges[graffitiCount++] = changes;
			} else if (k < graffitiCount && changes > graffitiChanges[k]) {
				graffitiChanges[k] = changes;
			}
			continue;
		}
		const int index = (int)(key & 0xFFFF);
		if (!mgr || index >= mgr->getObjNum())
			continue;
		TLiveActor* a = mgr->getObj(index);
		if (!a || (uint32_t)(a->getActorType() & 0xFF) != (key >> 16) || (a->getActorType() & 0xFFFFFF00) != 0x20000000)
			continue;
		removeItem(static_cast<TItem*>(a));
	}
	for (int k = 0; k < graffitiCount; k++)
		changeGraffiti(graffitiWhere[k], graffitiChanges[k]);
}

void hideTakenBlueCoins(const int32_t* before); // shared objects, below

// After the flags were shared: the Shines and blue coins someone else got
// leave this stage too.
void takeCollectedBy(const int32_t* before)
{
	hideTakenBlueCoins(before);
	TItemManager* mgr = items();
	if (!mgr)
		return;
	const u8 world = SMS_getShineStage(gpApplication.mCurrArea.getStage());
	for (int i = 0; i < mgr->getObjNum(); i++) {
		TLiveActor* a = mgr->getObj(i);
		if (!a || a->checkLiveFlag(LIVE_FLAG_DEAD))
			continue;
		uint32_t flag = 0;
		if (a->getActorType() == 0x20000013) { // Shine
			u32 id = static_cast<TMapObjBase*>(a)->getEventId() & 0xFF;
			flag   = 0x10000 + (id < 120 ? id : 0);
		} else if (a->getActorType() == 0x20000010) { // blue coin
			u32 id = static_cast<TMapObjBase*>(a)->getEventId();
			u8 w   = world == 0 || world >= 10 ? 1 : world;
			flag   = 0x10078 + (w - 1) * 50 + (id < 50 ? id : 0);
		} else {
			continue;
		}
		const int idx = (int)(flag - 0x10000); // card bools come first, by number
		if (idx >= 0 && idx < s_flagCount && !before[idx] && s_flags[idx])
			removeItem(static_cast<TItem*>(a));
	}
}

// ------------------------------------------------------------------- flags

void syncFlags()
{
	TFlagManager* fm = TFlagManager::getInstance();
	if (!fm)
		return;
	if (!s_flagCount) {
		s_flagCount = mpg_flag_count() < kMaxFlags ? mpg_flag_count() : kMaxFlags;
		for (int i = 0; i < s_flagCount; i++)
			s_flagIds[i] = mpg_flag_id(i);
	}
	static int32_t before[kMaxFlags];
	int livesAt = -1;
	for (int i = 0; i < s_flagCount; i++) {
		before[i] = s_flags[i] = fm->getFlag(s_flagIds[i]);
		if (s_flagIds[i] == 0x20001)
			livesAt = i;
	}
	const s32 coinsHere = fm->getFlag(0x40002); // with this frame's own pickups
	// Every 50 coins is a life (TMario::getCoin): the game gives it when its
	// own pickup makes the count a multiple of 50, but the count is
	// everyone's, so the others' coins take it past 50 with nobody's pickup
	// landing on it (no life), or two games land on it at once (two). The
	// game's own are taken back here, and the host gives one for each 50 the
	// shared count passes, once (below).
	int ownCoinLives = 0;
	if (inStage() && s_lastCoins >= 0 && coinsHere > s_lastCoins)
		ownCoinLives = coinsHere / 50 - s_lastCoins / 50;
	if (ownCoinLives > 0 && livesAt >= 0) {
		s_flags[livesAt] -= ownCoinLives;
		if (getenv("SMS_MULTIPLAYER_DEBUG"))
			fprintf(stderr, "[multiplayer] coins %d -> %d here: this game's own 50-coin life taken back (the host gives it)\n",
			        (int)s_lastCoins, (int)coinsHere);
	}
	mpg_flags_sync(s_flags);
	bool counts = false, hud = false;
	for (int i = 0; i < s_flagCount; i++) {
		if (s_flags[i] == before[i])
			continue;
		fm->setFlag(s_flagIds[i], s_flags[i]);
		if (s_flagIds[i] < 0x20000)
			counts = true;
		else
			hud = true;
	}
	if (counts) {
		// the Shine and blue coin totals, as the game counts them
		int shines = 0, blue = 0;
		for (int i = 0; i < 0x78; i++)
			shines += fm->getFlag(0x10000 + i) != 0;
		for (int i = 0; i < 0x2EE; i++)
			blue += fm->getFlag(0x10078 + i) != 0;
		fm->setFlag(0x40000, shines);
		fm->setFlag(0x40001, blue);
		if (inStage())
			takeCollectedBy(before);
	}
	// The 100-coin Shine: the game brings it out when its own pickup makes
	// exactly 100 (TCoin::taken). The count is everyone's now, so when the
	// others' coins take it past 100, this game brings it out by its own
	// player as that pickup would have; whoever reaches it first gets it.
	const s32 coins = fm->getFlag(0x40002);
	if (inStage() && s_playing && !s_spectating && s_lastCoins >= 0 && s_lastCoins < 100 && coins >= 100
	    && coinsHere != 100 && gpItemManager && gpMarioOriginal
	    // a stage without one (the plaza): the game's own call would crash
	    && JDrama::TNameRefGen::search2("シャイン（１００枚コイン用）")) {
		const JGeometry::TVec3<f32>& at = gpMarioOriginal->mPosition;
		fprintf(stderr, "[multiplayer] the coins passed 100 (%d): the 100-coin Shine comes out\n", (int)coins);
		gpItemManager->makeShineAppearWithDemo("シャイン（１００枚コイン用）", "シャイン（１００枚コイン用）カメラ", at.x, at.y,
		                                       at.z);
	}
	// a life for each 50 the shared count passed: the host's to give (the
	// others' come with its lives); everyone hears it, as the game plays it
	// (the game that landed on it played it already)
	const int passed = inStage() && s_lastCoins >= 0 && coins > s_lastCoins ? coins / 50 - s_lastCoins / 50 : 0;
	if (passed > 0) {
		if (s_role == MPG_ROLE_HOST && livesAt >= 0) {
			fm->incMario(passed);
			s_flags[livesAt] = fm->getFlag(0x20001);
			hud              = true;
			fprintf(stderr, "[multiplayer] the coins passed %d (%d): a life for everyone\n", coins / 50 * 50, (int)coins);
		}
		if (ownCoinLives <= 0)
			SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_1UP, 0, nullptr, 0);
	}
	if (inStage())
		s_lastCoins = coins;
	if (hud && inStage()) {
		// another player's lost life: the lives panel comes down and counts
		// down, as at a start over after a miss (TGCConsole2::startAppearMario
		// shows one more, then the count). One gained (a 1-up of theirs) the
		// console shows by itself, counting up, so its count is left to it.
		for (int i = 0; i < s_flagCount; i++)
			if (s_flagIds[i] == 0x20001 && s_flags[i] < before[i])
				if (TGCConsole2* console = director()->getConsole()) {
					const int lives = s_flags[i] > 99 ? 99 : s_flags[i] < 0 ? 0 : s_flags[i];
					console->unk3AC = (u8)lives;
					if (console->unk3A8->getPane()->isVisible() && !console->unk3B) {
						// up already (a stage's start): the new count, kept up a while
						((J2DPicture*)console->unk39C[0]->getPane())
						    ->changeTexture(console->unkE0[(int)(lives * 0.1f)]->getTexInfo(), 0);
						((J2DPicture*)console->unk39C[1]->getPane())->changeTexture(console->unkE0[lives % 10]->getTexInfo(), 0);
						console->unk3AE_US = 0;
						console->unk70     = 0;
					} else {
						console->startAppearMario(true);
					}
				}
	}
}

// ------------------------------------------------------------------- pose

} // namespace

unsigned MpStageNumber() { return s_stageNumber; }

bool MpPausedHere()
{
	TMarDirector* dir = director();
	return dir && dir->mState == TMarDirector::STATE_PAUSE_MENU;
}

bool MpHostPaused()
{
	MpgPose pose;
	float age;
	return s_role == MPG_ROLE_CLIENT && mpg_player_present(0) && mpg_pose(0, &pose, &age) && age < 2.0f
	       && (pose.more & MPG_POSE2_PAUSED);
}

// Mario's head and waist controls (MarioDraw.cpp's MarioHeadCtrl and
// MarioWaistCtrl): TMario::calcAnim sets them on his joints just for his
// calc, and takes them off again, so they are picked up from there, by a
// callback of ours on his root joint (the first one calculated).
J3DNodeCallBack s_marioHeadCtrl, s_marioWaistCtrl;

static BOOL pickUpMarioCtrls(J3DNode*, BOOL)
{
	TMario* m = gpMarioForCallBack;
	if (!m || !m->getM3UModel())
		return TRUE;
	J3DModelData* data = m->getM3UModel()->getModel()->getModelData();
	if (m->mJointIdHead < data->getJointNum())
		if (J3DNodeCallBack c = data->getJointNodePointer(m->mJointIdHead)->getCallBack())
			s_marioHeadCtrl = c;
	if (m->mJointIdChnChest < data->getJointNum())
		if (J3DNodeCallBack c = data->getJointNodePointer(m->mJointIdChnChest)->getCallBack())
			s_marioWaistCtrl = c;
	return TRUE;
}

// The turn one of those adds to his animation this frame, which a puppet's
// model, without them, cannot work out: the control run once more over
// nothing. They only read Mario (the waist's writes what it wrote this frame
// already).
static void captureJointTurn(TMario* m, u16 joint, J3DNodeCallBack ctrl, int16_t out[4])
{
	out[0] = out[1] = out[2] = 0;
	out[3]                   = 32767;
	J3DModelData* data       = m->getM3UModel()->getModel()->getModelData();
	if (!ctrl || joint >= data->getJointNum())
		return;
	Mtx saved;
	MTXCopy(J3DSys::mCurrentMtx, saved);
	TMario* was        = gpMarioForCallBack;
	gpMarioForCallBack = m;
	MTXIdentity(J3DSys::mCurrentMtx);
	ctrl(data->getJointNodePointer(joint), 0);
	f32 q[4];
	MpMtxToQuat(J3DSys::mCurrentMtx, q);
	gpMarioForCallBack = was;
	MTXCopy(saved, J3DSys::mCurrentMtx);
	if (q[3] < 0.0f)
		for (int i = 0; i < 4; i++)
			q[i] = -q[i];
	for (int i = 0; i < 4; i++)
		out[i] = (int16_t)lroundf((q[i] < -1.0f ? -1.0f : q[i] > 1.0f ? 1.0f : q[i]) * 32767.0f);
}

// This player's controller as read, before a server holds it (MpgPose's pad;
// testPadRead keeps it).
static PADStatus s_padNow;

extern "C" int mpg_local_pad(uint16_t* buttons, int8_t stick[4], uint8_t trigger[2])
{
	*buttons   = s_padNow.button;
	stick[0]   = s_padNow.stickX;
	stick[1]   = s_padNow.stickY;
	stick[2]   = s_padNow.substickX;
	stick[3]   = s_padNow.substickY;
	trigger[0] = s_padNow.triggerLeft;
	trigger[1] = s_padNow.triggerRight;
	return 1;
}

void MpCapturePose(TMario* m, MpgPose* out)
{
	MpgPose& p = *out;
	memset(&p, 0, sizeof p);
	p.session       = mpg_session();
	p.hasPad        = 1;
	p.padButtons    = s_padNow.button;
	p.padStick[0]   = s_padNow.stickX;
	p.padStick[1]   = s_padNow.stickY;
	p.padStick[2]   = s_padNow.substickX;
	p.padStick[3]   = s_padNow.substickY;
	p.padTrigger[0] = s_padNow.triggerLeft;
	p.padTrigger[1] = s_padNow.triggerRight;
	if (!m || !m->getM3UModel())
		return;
	M3UModelMario* mm = m->getM3UModel();
	if (s_spectating) {
		p.flags = MPG_POSE_SPECTATING;
		return;
	}
	if (TMarDirector* dir = director())
		if (dir->isDemoModeNow())
			p.flags |= MPG_POSE_DEMO;
	if (!m->checkFlag(MARIO_FLAG_UNK4) && !m->checkFlag(MARIO_FLAG_IS_PERFORMING))
		p.flags |= MPG_POSE_VISIBLE;
	if (m->checkFlag(MARIO_FLAG_HAS_FLUDD))
		p.flags |= MPG_POSE_FLUDD;
	if (m->mWaterGun && m->checkFlag(MARIO_FLAG_HAS_FLUDD) && m->mWaterGun->isEmitting())
		p.flags |= MPG_POSE_SPRAYING;
	if (m->onYoshi())
		p.flags |= MPG_POSE_YOSHI;
	if (!m->mCap || m->mCap->unkC == m->mCap->unk10[0]) // the cap, not the hair under it
		p.flags |= MPG_POSE_CAP;
	if (mm->unk24[1].unk3 != 0xff)
		p.flags |= MPG_POSE_UPPER;
	// surfing on a Blooper: the game draws it with Mario (TMario::entryModels)
	if (m->checkStatusType(MARIO_STATUS_FLAG_UNK10000) && m->mSurfGesso)
		p.more |= MPG_POSE2_SURF | (u8)((m->mSurfGessoType & 3) << 2);
	// riding the coaster car (drawn with Mario: TMario::entryModels)
	if (m->mStatus == MARIO_STATUS_TOROCCO && m->mTorocco) {
		p.more |= MPG_POSE2_COASTER;
		p.coasterAngle = m->mToroccoAngle;
	}
	memcpy(p.mtx, mm->getModel()->getBaseTRMtx(), sizeof p.mtx);
	p.bodyBck    = mm->unk24[0].unk4[0];
	p.bodyFrame  = mm->getFrameCtrl(0).getFrame();
	p.bodyBlend  = mm->unk20->unk18[0].mMotionBlendRatio;
	p.upperBck   = mm->unk24[1].unk4[0];
	p.upperFrame = mm->getFrameCtrl(1).getFrame();
	p.btp        = mm->unk1C ? mm->unk1C->unk0 : 0xff;
	p.btpFrame   = mm->getFrameCtrl(2).getFrame();
	// which hands: the body's own (shape 5 shown), or ma_hnd2, or ma_hnd3
	J3DModelData* body = mm->getModel()->getModelData();
	for (u16 i = 0; i < body->getShapeNum() && i < 32; i++)
		if (body->getShapeNodePointer(i)->checkFlag(J3DShpFlag_Visible))
			p.hiddenShapes |= 1u << i;
	if (!body->getShapeNodePointer(5)->checkFlag(J3DShpFlag_Visible))
		p.hand = 0;
	else if (m->mHandModels[0][0]
	         && !m->mHandModels[0][0]->getModelData()->getShapeNodePointer(0)->checkFlag(J3DShpFlag_Visible))
		p.hand = 1;
	else
		p.hand = 2;
	J3DJoint* root = body->getJointNodePointer(0);
	if (!root->getCallBack())
		root->setCallBack(&pickUpMarioCtrls);
	captureJointTurn(m, m->mJointIdHead, m->isMario() ? s_marioHeadCtrl : nullptr, p.headTurn);
	captureJointTurn(m, m->mJointIdChnChest, s_marioWaistCtrl, p.waistTurn);
	if (TWaterGun* gun = m->mWaterGun) {
		p.nozzle = gun->mCurrentNozzle;
		if (gun->mCurrentNozzle < 6 && gun->mNozzleList[gun->mCurrentNozzle])
			p.gunAngle = gun->mNozzleList[gun->mCurrentNozzle]->getGunAngle();
		p.hoverTiltL = (int16_t)gun->getNozzleSpeedY();
		p.hoverTiltR = (int16_t)gun->getNozzleSpeedZ();
		p.propAngle  = gun->getHoverAngle();
		if (MActor* f = gun->mFluddModel) {
			p.fluddBck = (u16)f->getCurAnmIdx(ANM_TYPE_BCK);
			if (J3DFrameCtrl* fc = f->getFrameCtrl(ANM_TYPE_BCK))
				p.fluddFrame = fc->getFrame();
		}
	}
	if (TYoshi* y = m->mYoshi) {
		if (y->mActor) {
			p.yoshiBck = (u16)y->mActor->getCurAnmIdx(ANM_TYPE_BCK);
			if (J3DFrameCtrl* fc = y->mActor->getFrameCtrl(ANM_TYPE_BCK))
				p.yoshiFrame = fc->getFrame();
		}
		p.yoshiColor[0] = (u8)y->unk84.x;
		p.yoshiColor[1] = (u8)y->unk84.y;
		p.yoshiColor[2] = (u8)y->unk84.z;
	}
	// where the water leaves, while it does (the others' games spray it too)
	TWaterGun* gun = m->mWaterGun;
	if ((p.flags & MPG_POSE_SPRAYING) && gun && gun->mCurrentNozzle < 6 && gun->mNozzleList[gun->mCurrentNozzle]) {
		const int count = nozzleBmdData.getEmitterCount(gun->mCurrentNozzle);
		p.emitters      = (u8)(count < 2 ? count : 2);
		for (int i = 0; i < p.emitters; i++) {
			JGeometry::TVec3<f32> pos, dir, speed;
			gun->getEmitPosDirSpeed(i, &pos, &dir, &speed);
			p.emitPos[i][0] = pos.x, p.emitPos[i][1] = pos.y, p.emitPos[i][2] = pos.z;
			p.emitDir[i][0] = dir.x, p.emitDir[i][1] = dir.y, p.emitDir[i][2] = dir.z;
			p.emitSpeed[0] = speed.x, p.emitSpeed[1] = speed.y, p.emitSpeed[2] = speed.z;
		}
		p.pressure = gun->mNozzleList[gun->mCurrentNozzle]->unk378;
	}
}

// A drop of water drawn for another player (a bit of its flags the game leaves alone).
static const u16 kDrawnWater = 0x8000;

// Another player's water, emitted here as their FLUDD emits it
// (TNozzleBase::emit): the same nozzle's parameters, from where their game
// says it leaves. It is Mario's kind of water, so it cleans goop and hits
// what it hits in this game too. `carry` keeps the fraction of a particle
// between frames, as the nozzle does.
//
// Its drops carry kDrawnWater: they do not hit NPCs or map objects here
// (that player's own game sends the hits it lands, mp_game.cpp's
// objectHit), so nothing is hit twice.
void MpCountEffect(int slot, bool ongoing)
{
	if (slot >= 0 && slot < MPG_MAX_PLAYERS)
		s_fxShown[slot][ongoing]++;
}

void MpCountSound(int slot)
{
	if (slot >= 0 && slot < MPG_MAX_PLAYERS)
		s_soundHeard[slot]++;
}

void MpEmitWater(int slot, const MpgPose& p, f32* carry)
{
	TMario* m = gpMarioOriginal;
	if (!m || !m->mWaterGun || !gpModelWaterManager || p.nozzle >= 6 || !p.emitters)
		return;
	TWaterGun* gun     = m->mWaterGun;
	TNozzleBase* nozzle = gun->mNozzleList[p.nozzle];
	TWaterEmitInfo* info = gun->mEmitInfo;
	if (!nozzle || !info)
		return;
	const TNozzleBase::TEmitParams& e = nozzle->mEmitParams;
	*carry += e.mNum.get();
	const s32 num = (s32)*carry;
	if (num <= 0)
		return;
	*carry -= (f32)num;
	const s32 type = e.mType.get();
	if (type < 0 || type >= 17 || !gpModelWaterManager->mWaterParticleTypes[type])
		return;
	for (int i = 0; i < p.emitters; i++) {
		info->mAlive.set(gpModelWaterManager->mWaterParticleTypes[type]->mAlive.get());
		info->mPos.value.set(p.emitPos[i][0], p.emitPos[i][1], p.emitPos[i][2]);
		info->mDir.value.set(p.emitDir[i][0], p.emitDir[i][1], p.emitDir[i][2]);
		info->mV.value.set(p.emitSpeed[0], p.emitSpeed[1], p.emitSpeed[2]);
		info->mDirTremble  = e.mDirTremble;
		info->mPowTremble  = e.mPowTremble;
		info->mSize        = e.mSize;
		info->mSizeTremble = e.mSizeTremble;
		info->mType        = e.mType;
		info->mHitRadius   = e.mHitRadius;
		info->mHitHeight   = e.mHitHeight;
		info->mNum.set(num);
		info->mAttack = e.mAttack;
		const f32 pow  = e.mEmitPow.get();
		const f32 ctrl = e.mEmitCtrl.get();
		info->mPow.set(pow * p.pressure * ctrl + pow * (1.0f - ctrl));
		info->mFlag.set(0x40 | kDrawnWater | MpPvpWaterTag(slot)); // whose (a water fight's, mp_pvp.cpp)
		gpModelWaterManager->emitRequest(*info);
	}
}

namespace {

// The player's effects so far (effects, below).
int s_fxMoving;                     // inside Mario's move (objectMove)
MpgFx s_fxTick[MPG_MAX_FX];         // this tick's ongoing ones so far
int s_fxTickCount;
MpgFx s_fxOngoing[MPG_MAX_FX];      // the last whole tick's
int s_fxOngoingCount;
MpgFx s_fxShots[MPG_MAX_FX];        // the newest asked for once, oldest first
int s_fxShotCount;
uint8_t s_fxShotSeq;
// The player's sounds so far (soundHook, below): the newest, oldest first.
uint16_t s_sounds[MPG_MAX_SOUNDS];
int s_soundCount;
uint8_t s_soundSeq;

void sendPose()
{
	MpgPose p;
	MpCapturePose(gpMarioOriginal, &p);
	if (MpMirrorOnTub())
		p.more |= MPG_POSE2_ON_TUB;
	if (MpPausedHere())
		p.more |= MPG_POSE2_PAUSED;
	if (p.flags & MPG_POSE_VISIBLE) {
		MpMeasureSmoothness(mpg_slot(), p);
		p.fxCount = (uint8_t)s_fxOngoingCount;
		memcpy(p.fx, s_fxOngoing, sizeof p.fx);
	}
	p.shotCount = (uint8_t)s_fxShotCount;
	p.shotSeq   = s_fxShotSeq;
	memcpy(p.shots, s_fxShots, sizeof p.shots);
	p.soundCount = (uint8_t)s_soundCount;
	p.soundSeq   = s_soundSeq;
	memcpy(p.sounds, s_sounds, sizeof p.sounds);
	mpg_set_pose(&p);
}

// ------------------------------------------------------------------- warps

bool leading(int area); // below
void leadTrip(TMarDirector* dir);
bool hasEpisodeSelect(int area);
// A secret course (an area with an archive of its own: the others are
// unused): one episode, so a way in that names none needs no waiting for.
bool isSecretCourse(int area)
{
	static const u8 areas[] = {21, 22, 23, 24, 29, 30, 31, 32, 33, 40, 41, 42, 44, 46, 47, 48, 50, 51, 52};
	for (size_t i = 0; i < sizeof areas; i++)
		if (areas[i] == area)
			return true;
	return false;
}

const char* worldName(int area)
{
	// SMS_getShineStage's worlds: Pianta Village (monte) is 7, Noki Bay (mare) 8
	static const char* const names[] = {"the Airstrip", "Delfino Plaza", "Bianco Hills", "Ricco Harbor",
	                                    "Gelato Beach", "Pinna Park",   "Sirena Beach", "Pianta Village",
	                                    "Noki Bay", "Corona Mountain"};
	u8 w = area >= 0 && area < 61 ? SMS_getShineStage((u8)area) : 1;
	return names[w < 10 ? w : 1];
}

// The host's travel (TMarDirector::moveStage, as it went): the session
// flags that shape how a stage starts (0x30000 on: this game came from an
// event, a movie was seen) and the movie played on the way. A client takes
// them, so it arrives as the host did (the same opening, the same movie).
const int kSessionFlags = 0x1D; // 0x30000 to 0x3001C
int s_travelWait; // frames this client has waited for the host's travel

void takeTravel(TMarDirector* dir)
{
	uint32_t flags;
	int movie;
	if (!mpg_travel(&flags, &movie))
		return;
	for (int i = 0; i < kSessionFlags; i++) {
		// the secret courses' movie of Shadow Mario taking FLUDD, seen here
		// (0x3000D, cleared only in the plaza): not seen again for a trip
		// that left before the host's own was played (its flags say unseen)
		if (i == 0xD && TFlagManager::getInstance()->getBool(0x3000D))
			continue;
		TFlagManager::getInstance()->setBool((flags >> i) & 1, 0x30000 + i);
	}
	if (movie >= 0 && dir) {
		gpApplication.setMovie((u32)movie);
		dir->onUnk4CFlag(TMarDirector::DIRECTOR_FLAG_MOVIE_PENDING);
	}
	mpg_travel_done();
}

void followHost(TMarDirector* dir)
{
	int area, episode;
	if (!mpg_flags_ready() || !mpg_target(&area, &episode) || mpg_in_target())
		return;
	if (s_shineHoldFrames > 0 && area == s_shineHoldArea && episode == s_shineHoldEpisode)
		return; // the host is still saving the Shine we got: they come to us
	if (leading(gpApplication.mCurrArea.getStage()))
		return; // we led the way here: the others are coming
	s_shineHoldFrames = 0;
	// the host is on its way: how it goes decides how everyone arrives (a
	// few seconds at most; then this game goes as it would)
	uint32_t flags;
	int movie;
	const int fps = port_active_frame_rate > 0 ? port_active_frame_rate : 30;
	// off to a world whose episode the host is still choosing (its episode
	// select): this game goes to its own, where it watches the host choose
	// (selectFrame); another way in that the game decides (the plaza) is
	// waited for here, and taken once the host is there
	if (episode == 0xFF && mpg_target_traveling() && !hasEpisodeSelect(area) && !isSecretCourse(area)) {
		static int told;
		if (told++ % (5 * fps) == 0) {
			char text[128];
			snprintf(text, sizeof text, "%s is choosing an episode of %s", mpg_display_name(0), worldName(area));
			showNotice(text);
		}
		return;
	}
	if (mpg_target_traveling() && !mpg_travel(&flags, &movie) && s_travelWait < 4 * fps) {
		s_travelWait++;
		return;
	}
	s_travelWait = 0;
	takeTravel(dir);
	// the host's flags are the game's now (synced this frame): load their stage
	if (episode != 0xFF) // 0xFF: the game picks the episode from the flags, as the host's does
		TFlagManager::getInstance()->setFlag(0x40003, episode);
	gpApplication.mNextArea.set((u8)area, (u8)episode, 0);
	dir->onFlag(TMarDirector::DIRECTOR_FLAG_STAGE_TRANSITION_PENDING);
	char text[128];
	snprintf(text, sizeof text, "Joining the host in %s", worldName(area));
	showNotice(text);
}

// ------------------------------------------------------------------- episode select

// The host chooses the episode for everyone. Its episode select is sent as it
// goes: where it points and whether it chose (mpg_host_select). When the host
// sets off for a world with an episode select, everyone sets off with it to
// their own, where they watch: their controller does nothing there, their
// cursor follows the host's, and the host's choice is pressed for them.
u16 s_selectPress;    // client: the button the host's choosing presses here this frame (0: none)
bool s_selectLocked;  // client: on an episode select the host is choosing for
int s_selectFrames, s_selectSteps;
TSelectMenu* s_selectSeen;
bool s_hostChose;

bool hasEpisodeSelect(int area)
{
	// TMarDirector::moveStage: a world's way in with no episode named goes to its select
	return area == 2 || area == 3 || area == 4 || area == 5 || area == 6 || area == 8;
}

TSelectMenu* selectMenu()
{
	if (gpApplication.mAppState != TApplication::APP_STATE_TITLE || !gpApplication.mDirector)
		return nullptr;
	TSelectDir* dir = dynamic_cast<TSelectDir*>(gpApplication.mDirector);
	return dir ? dir->getSelectMenu() : nullptr;
}

void selectFrame()
{
	s_selectPress     = 0;
	s_selectLocked    = false;
	TSelectMenu* menu = selectMenu();
	if (menu != s_selectSeen) {
		s_selectSeen  = menu;
		s_hostChose   = false;
		s_selectSteps = 0;
		s_selectFrames = 0;
	}
	if (!menu)
		return;
	const int state = menu->mMenuState;
	if (s_role == MPG_ROLE_HOST) {
		// test aid (pickep@N:E): N frames into an episode select, the host
		// moves to episode E and chooses it
		static int pickAt = -2, pickEpisode;
		if (pickAt == -2) {
			pickAt = -1;
			if (const char* plan = getenv("SMS_MULTIPLAYER_TEST"))
				if (const char* pe = strstr(plan, "pickep@"))
					sscanf(pe, "pickep@%d:%d", &pickAt, &pickEpisode);
		}
		if (pickAt >= 0 && ++s_selectFrames >= pickAt && state == TSelectMenu::MENU_INPUT_LOOP && !(s_selectFrames & 1)) {
			s_selectLocked = true; // its own pad pressed for it, as a client's is
			if (menu->mSelectedShine == pickEpisode) {
				s_selectPress = PAD_BUTTON_A;
				fprintf(stderr, "[multiplayer] test: choosing episode %d\n", pickEpisode);
			} else
				s_selectPress = pickEpisode < menu->mSelectedShine ? PAD_BUTTON_LEFT : PAD_BUTTON_RIGHT;
		}
		// chosen: from its press of A on (the menu fades, then closes)
		if (state == TSelectMenu::DISAPPEAR_MENU || state == TSelectMenu::WAIT_BEFORE_CLOSE)
			s_hostChose = true;
		if (state != TSelectMenu::CLOSE_MENU || s_hostChose) // (not before it is set up)
			mpg_host_select(menu->mStage, menu->mSelectedShine, s_hostChose);
		return;
	}
	int area, episode, selArea, selEpisode, chosen;
	if (s_role != MPG_ROLE_CLIENT || !mpg_target(&area, &episode) || area != menu->mStage)
		return; // not where the host is going: this player chooses
	s_selectLocked = true;
	int want  = -1;
	bool take = false;
	if (episode != 0xFF) {
		want = episode, take = true; // the host is there already
	} else if (mpg_select(&selArea, &selEpisode, &chosen) && selArea == area) {
		want = selEpisode, take = chosen != 0;
	}
	if (s_selectFrames++ == 0) {
		char text[128];
		snprintf(text, sizeof text, "%s is choosing the episode", mpg_display_name(0));
		showNotice(text);
	}
	s_noticeFrames = kNoticeFrames; // the line stays while we watch
	if (want < 0 || want > 7 || state != TSelectMenu::MENU_INPUT_LOOP || (s_selectFrames & 1))
		return; // a press, then a frame without (the menu takes each as it starts)
	if (menu->mSelectedShine != want && s_selectSteps > 16) {
		menu->mSelectedShine = (u8)want; // out of the cursor's reach here: as the host's
		fprintf(stderr, "[multiplayer] episode select: the host's episode %d set outright\n", want);
	}
	if (menu->mSelectedShine == want) {
		if (take) {
			fprintf(stderr, "[multiplayer] episode select: the host chose episode %d\n", want);
			s_selectPress = PAD_BUTTON_A;
		}
		return;
	}
	s_selectSteps++;
	s_selectPress = want < menu->mSelectedShine ? PAD_BUTTON_LEFT : PAD_BUTTON_RIGHT;
}

// A client needs no file of its own (it plays the host's): from the title
// screen it goes straight to the host's stage with the host's flags, once
// both have arrived.
void skipTitle(TMarDirector* dir)
{
	int area, episode;
	static int debugFrames;
	if (getenv("SMS_MULTIPLAYER_DEBUG") && ++debugFrames % 120 == 0)
		fprintf(stderr, "[multiplayer] title: app %d dir %p area %d state %d flags %d target %d\n",
		        (int)gpApplication.mAppState, (void*)dir, (int)gpApplication.mCurrArea.getStage(),
		        dir ? (int)dir->mState : -1, mpg_flags_ready(), mpg_target(&area, &episode));
	if (!dir || gpApplication.mCurrArea.getStage() != 15 || dir->mState != TMarDirector::STATE_UNK4
	    || dir->checkFlag(TMarDirector::DIRECTOR_FLAG_STAGE_TRANSITION_PENDING)) {
		s_titleFrames = 0;
		return;
	}
	// the title's own setup first (about a second)
	if (++s_titleFrames < 30 || !mpg_flags_ready() || !mpg_target(&area, &episode))
		return;
	syncFlags();
	TFlagManager::getInstance()->setFlag(0x40003, episode);
	dir->setNextStage((u16)(((area + 1) << 8) | episode), nullptr);
	char text[128];
	snprintf(text, sizeof text, "Joining the host in %s", worldName(area));
	showNotice(text);
	s_titleFrames = 0;
}

// A client's game plays on the multiplayer card the launcher gives it (the
// host's save is the one played), not the player's own. Back at the title with
// no host to take it along, its file select shows that card's files, three
// new ones as a rule: the player is told whose they are, for as long as it is
// there, rather than taking them for their own saves gone.
const char kGuestCard[] = "Not in the host's game: these files are on the multiplayer card, not your own. "
                          "For your own saves, close the game and press Play.";

// Why the host's game ended for us (mpg_host_left), told again at the title.
char s_leftWhy[128];

void guestCardNotice()
{
	if (gpApplication.mCurrArea.getStage() != 15 || !mpg_link_over())
		return;
	char text[sizeof s_notice];
	snprintf(text, sizeof text, "%s%s%s", s_leftWhy, s_leftWhy[0] ? " " : "", kGuestCard);
	if (strcmp(s_notice, text)) {
		showNotice(text);
		s_statusFrames = 0; // the player list would cover it (why the link ended was its own notice)
	}
	s_noticeFrames = kNoticeFrames;
}

// The host left while we were in its game (closed it, went back to the title,
// or was lost and not found again): there is no host's stage to be in, so
// this game leaves it for the title as the reset does (its file select is the
// multiplayer card's: guestCardNotice), a moment after saying why. A game on
// its way somewhere goes once it is there; one at the title stays.
int s_leaveFrames = -1; // until leaving for the title; -1: not leaving

void leaveWithHost(TMarDirector* dir)
{
	if (mpg_host_left(s_leftWhy, sizeof s_leftWhy)) {
		const int fps = port_active_frame_rate > 0 ? port_active_frame_rate : 30;
		s_leaveFrames = 2 * fps;
		fprintf(stderr, "[multiplayer] the host left (%s): back to the title\n", s_leftWhy);
	}
	if (s_leaveFrames < 0)
		return;
	if (dir && gpApplication.mCurrArea.getStage() == 15) {
		s_leaveFrames = -1;
		return;
	}
	if (!dir || dir->mState == TMarDirector::STATE_UNK9 || dir->mState == TMarDirector::STATE_UNK12)
		return;
	if (s_leaveFrames > 0) {
		s_leaveFrames--;
		return;
	}
	s_leaveFrames = -1;
	s_spectating  = false; // playing alone from here
	// as TMarDirector::changeState takes the reset: a wipe out, then the title
	dir->unkE4 = 4;
	dir->unkB4 = TApplication::APP_STATE_DONE;
	dir->currentStateFinalize(TMarDirector::STATE_UNK12);
	dir->nextStateInitialize(TMarDirector::STATE_UNK12);
	dir->mState = TMarDirector::STATE_UNK12;
	fprintf(stderr, "[multiplayer] leaving the host's stage for the title\n");
}

// A client got a Shine: the host leaves with everyone, as when its own Mario
// gets one (TMarDirector::updateGameMode): the "Shine Get!" banner, the
// fanfare and the fade, then the save prompt and the plaza. The Shine's flag
// is set already (it came with the client's report). There is no Shine
// camera here (that needs the game's own pickup, TMarDirector::fireGetStar),
// so the save prompt waits for the fade instead of the camera.
int s_hostShineFrames; // until the save prompt

void hostShineExit(TMarDirector* dir)
{
	if (s_hostShineFrames > 0) {
		if (--s_hostShineFrames == 0) {
			dir->onFlag(TMarDirector::DIRECTOR_FLAG_SHINE_TAKEN | TMarDirector::DIRECTOR_FLAG_CARD_SAVE_PENDING
			            | TMarDirector::DIRECTOR_FLAG_STAGE_TRANSITION_PENDING);
			dir->unk261 = 6;
			gpApplication.mNextArea.set(1, 0xFF, 0);
		}
		return;
	}
	if (!mpg_take_shine_exit())
		return;
	// the Shine Get played here already (everyone was taken to it): the
	// game's own sequence leads to the prompt
	if (s_shineGetFrames > 0 || dir->checkFlag(TMarDirector::DIRECTOR_FLAG_SHINE_TAKEN))
		return;
	if (TGCConsole2* console = dir->getConsole()) {
		if (console->unk94)
			console->unk94->startAppearShineGet();
		console->unk47 = 1;
	}
	MSBgm::startBGM(MSD_BGM_GET_SHINE);
	TFlagManager::getInstance()->setBool(true, 0x30006);
	if (dir->unkDC) {
		const f32 rate = dir->unkDC->getRate();
		dir->unkDC->registFadeout((u16)rate, (u16)(rate * 5.3333333f));
	}
	// the banner and the fade (about six seconds), then the prompt
	s_hostShineFrames = (int)(6.5f * (port_active_frame_rate > 0 ? port_active_frame_rate : 30));
}

// A blue coin asks the player who got it to save (TMarDirector::
// fireGetBlueCoin: the card save prompt, kind 1). Only the host's card holds
// the save everyone plays, so a client's own prompt is skipped (as the coin is
// taken, itemTaken, or after the tick, afterMovement: before the game's next
// state change opens it) and the host's game asks
// instead, once its player is on the ground and nothing else is on (a Shine's
// exit, a talk, a cutscene: the game holds the prompt back for those too).
int s_blueSaveFrom = -1; // host: the player whose blue coin is to be saved

// The same for a nozzle's box opened for good (kinds 3 and 4: the nozzle's
// help balloon, which the game shows once its prompt is over, shows at once)
// and Yoshi hatched (5).
void clientSkipBlueSave(TMarDirector* dir)
{
	if (!dir || !dir->checkFlag(TMarDirector::DIRECTOR_FLAG_CARD_SAVE_PENDING))
		return;
	const int kind = dir->unk261;
	if (kind != 1 && kind != 3 && kind != 4 && kind != 5)
		return; // a Shine's (6: clientShineExit) or a game over's
	dir->offFlag(TMarDirector::DIRECTOR_FLAG_CARD_SAVE_PENDING);
	if ((kind == 3 || kind == 4) && dir->getConsole())
		dir->getConsole()->startAppearBalloon(kind == 3 ? 0x4B : 0x4C, true);
	showNotice(kind == 1 ? "Blue coin! The host is asked to save it" : "The host is asked to save");
	fprintf(stderr, "[multiplayer] a save prompt here (kind %d): the host saves\n", kind);
}

void hostBlueSave(TMarDirector* dir)
{
	const int from = mpg_take_blue_save();
	if (from > 0) {
		s_blueSaveFrom = from - 1;
		fprintf(stderr, "[multiplayer] %s got a blue coin (or a nozzle, or Yoshi): the host is asked to save\n",
		        mpg_character_name(from - 1));
	}
	TMario* m = gpMarioOriginal;
	if (s_blueSaveFrom < 0 || !m || !playing() || s_hostShineFrames > 0 || s_shineGetFrames > 0
	    || m->mStatus == MARIO_STATUS_WIN_DEMO || dir->isTalkOrDemoModeNow()
	    || dir->checkFlag(TMarDirector::DIRECTOR_FLAG_SHINE_TAKEN | TMarDirector::DIRECTOR_FLAG_CARD_SAVE_PENDING
	                      | TMarDirector::DIRECTOR_FLAG_STAGE_TRANSITION_PENDING))
		return;
	if (!s_spectating && !m->isTouchGround4cm())
		return; // not in the middle of a jump
	dir->onFlag(TMarDirector::DIRECTOR_FLAG_CARD_SAVE_PENDING);
	dir->unk261 = 1;
	fprintf(stderr, "[multiplayer] the host's save prompt, for %s\n", mpg_character_name(s_blueSaveFrom));
	char text[96];
	snprintf(text, sizeof text, "Saving what %s got", mpg_display_name(s_blueSaveFrom));
	showNotice(text);
	s_blueSaveFrom = -1;
}

// ------------------------------------------------------------------- Shine Get

// A Shine got by anyone is got by everyone together: when a player touches
// one, every other game moves its player to that Shine and has it touch them
// (TShine::touchPlayer), so each game plays the game's own Shine Get: the
// camera, the victory pose, the banner, the fanfare and the fade. Only the
// host saves (clientShineExit).

// The stage's Shines, wherever they are kept (any of the conductor's object
// lists, or among its loose actors), in turn: `index` from 0 until null.
TShine* shineAt(int index)
{
	if (!gpConductor)
		return nullptr;
	for (JGadget::TList<TLiveManager*>::iterator it = gpConductor->unk10.begin(); it != gpConductor->unk10.end(); ++it)
		for (int i = 0; *it && i < (*it)->getObjNum(); i++) {
			TLiveActor* a = (*it)->getObj(i);
			if (a && a->getActorType() == 0x20000013 && index-- == 0)
				return static_cast<TShine*>(a);
		}
	for (JGadget::TList<TLiveActor*>::iterator it = gpConductor->unk30.begin(); it != gpConductor->unk30.end(); ++it)
		if (*it && (*it)->getActorType() == 0x20000013 && index-- == 0)
			return static_cast<TShine*>(*it);
	return nullptr;
}

TShine* findShine(int eventId, const float pos[3])
{
	TShine* best = nullptr;
	f32 bestD    = 1e30f;
	for (int i = 0; shineAt(i); i++) {
		TLiveActor* a = shineAt(i);
		if (!a || a->getActorType() != 0x20000013 || (int)(static_cast<TMapObjBase*>(a)->getEventId() & 0xFF) != eventId)
			continue;
		const f32 dx = a->mPosition.x - pos[0], dy = a->mPosition.y - pos[1], dz = a->mPosition.z - pos[2];
		const f32 d = dx * dx + dy * dy + dz * dz;
		if (d < bestD)
			best = static_cast<TShine*>(a), bestD = d;
	}
	return best;
}

// Every frame in a stage (the Shine Get takes the game out of play at once,
// so this looks whether or not it is playing).
void noticeOwnShineGet()
{
	TMario* m = gpMarioOriginal;
	if (!m)
		return;
	if (s_shineGetFrames > 0)
		s_shineGetFrames--;
	const bool win = m->mStatus == MARIO_STATUS_WIN_DEMO;
	if (win && !s_inWinDemo) {
		s_shineGetFrames = 30 * (port_active_frame_rate > 0 ? port_active_frame_rate : 30);
		THitActor* shine = m->unk384;
		if (!s_winRemote && shine && shine->getActorType() == 0x20000013) {
			// we touched it: the others come to it
			const float pos[3] = {shine->mPosition.x, shine->mPosition.y, shine->mPosition.z};
			fprintf(stderr, "[multiplayer] we touched Shine %d: everyone comes to it\n",
			        (int)(static_cast<TMapObjBase*>(shine)->getEventId() & 0xFF));
			mpg_shine_get((int)(static_cast<TMapObjBase*>(shine)->getEventId() & 0xFF), pos, m->mFaceAngle.y);
		}
	}
	if (!win) {
		s_winRemote    = false;
		s_winCollector = -1;
		s_winShine     = nullptr;
	}
	s_inWinDemo = win;
}

bool watchable(int slot); // below

// Where this game's player stands for another player's Shine Get: in a line
// beside the one who touched it, who is in the middle holding it, each at
// their own place (by their slot's order among the others here), a little
// behind, facing the way they face. A place without floor at about their
// height (a ledge, water) gives way to a nearer one, the other side, or at
// last the Shine's own spot.
JGeometry::TVec3<f32> shineGetSpot(int collector, const float pos[3], int yaw)
{
	const JGeometry::TVec3<f32> at(pos[0], pos[1], pos[2]);
	if (!gpMap)
		return at;
	int rank = 0;
	for (int slot = 0; slot < mpg_slot(); slot++)
		if (slot != collector && mpg_player_present(slot))
			rank++;
	static const f32 kSide[MPG_MAX_PLAYERS - 1] = {170.0f, -170.0f, 340.0f, -340.0f, 510.0f, -510.0f, 680.0f};
	const f32 side = kSide[rank < MPG_MAX_PLAYERS - 1 ? rank : MPG_MAX_PLAYERS - 2];
	// tries: the place; nearer; the other side; the other side nearer; behind
	const f32 tries[5][2] = {{side, 50.0f}, {side * 0.6f, 30.0f}, {-side, 50.0f}, {-side * 0.6f, 30.0f}, {0.0f, 170.0f}};
	const f32 a  = yaw * (3.14159265f / 32768.0f);
	const f32 rx = -cosf(a), rz = sinf(a);  // their right
	const f32 bx = -sinf(a), bz = -cosf(a); // behind them
	const TBGCheckData* floor = nullptr;
	const f32 theirs = gpMap->checkGround(at.x, at.y + 80.0f, at.z, &floor);
	const f32 base   = floor && !floor->isIllegalData() ? theirs : at.y; // checkGround's "none" is its illegal floor
	for (int i = 0; i < 5; i++) {
		JGeometry::TVec3<f32> p(at.x + rx * tries[i][0] + bx * tries[i][1], base, at.z + rz * tries[i][0] + bz * tries[i][1]);
		const TBGCheckData* f = nullptr;
		const f32 ground = gpMap->checkGround(p.x, base + 100.0f, p.z, &f);
		if (!f || f->isIllegalData() || ground < base - 80.0f || ground > base + 80.0f || f->isWaterSurface())
			continue;
		p.y = ground;
		return p;
	}
	return at;
}

// Every tick of another player's Shine Get here: the Shine is in their
// character's hands (the game puts it where this game's Mario is, TShine::control).
void holdOthersShine()
{
	if (!s_winRemote || !s_winShine || s_winShine->mState != TShine::STATE_UNK10 || !watchable(s_winCollector))
		return;
	TMpPuppet* p        = s_puppets[s_winCollector];
	const MpgPose& pose = p->pose();
	TShine* shine       = s_winShine;
	shine->mPosition    = p->mPosition;
	shine->mRotation.y  = atan2f(pose.mtx[0][2], pose.mtx[2][2]) * (180.0f / 3.14159265f);
	if (J3DModel* model = shine->getModel())
		MsMtxSetXYZRPH(model->getBaseTRMtx(), shine->mPosition.x, shine->mPosition.y - shine->mYOffset, shine->mPosition.z,
		               shine->mRotation.x, shine->mRotation.y, shine->mRotation.z);
}

// While playing: another player's Shine Get comes here.
void playOthersShineGets()
{
	TMario* m = gpMarioOriginal;
	if (!m)
		return;
	const bool win = m->mStatus == MARIO_STATUS_WIN_DEMO;
	// out of play: losing a life (Mario takes no touch then), or not moved at
	// all (the Shine Get would wait for him to land forever). The host's
	// game then plays the Shine's exit without him (hostShineExit).
	const bool out = m->checkFlag(MARIO_FLAG_GAME_OVER | MARIO_FLAG_IS_PERFORMING);
	int slot, eventId, yaw;
	float pos[3];
	while (mpg_next_shine_get(&slot, &eventId, pos, &yaw)) {
		if (win || s_spectating || out)
			continue; // in one already, or watching, or out of play
		TShine* shine = findShine(eventId, pos);
		if (!shine) {
			fprintf(stderr, "[multiplayer] %s got Shine %d, which this game does not have\n", mpg_character_name(slot),
			        eventId);
			continue;
		}
		if (shine->checkLiveFlag(LIVE_FLAG_DEAD)) {
			// out only in their game so far (an event brought it there): here too
			shine->mPosition.set(pos[0], pos[1], pos[2]);
			shine->appear();
		}
		fprintf(stderr, "[multiplayer] %s got Shine %d: everyone to it\n", mpg_character_name(slot), eventId);
		char text[96];
		snprintf(text, sizeof text, "%s got a Shine!", mpg_display_name(slot));
		showNotice(text);
		// beside them, as they stand where they touched it (the touch puts
		// Mario on the Shine and turns him its way: he is put back after)
		const JGeometry::TVec3<f32> spot = shineGetSpot(slot, pos, yaw);
		fprintf(stderr, "[multiplayer] Shine Get beside %s: at %.0f %.0f %.0f (they are at %.0f %.0f %.0f)\n",
		        mpg_character_name(slot), spot.x, spot.y, spot.z, pos[0], pos[1], pos[2]);
		s_winRemote    = true;
		s_winCollector = slot;
		s_winShine     = shine;
		shine->touchPlayer(m);
		m->mPosition       = spot;
		m->mFaceAngle.y    = (s16)yaw;
		m->mModelFaceAngle = (s16)yaw;
		m->mForwardVel     = 0.0f;
		m->mVel.set(0.0f, 0.0f, 0.0f);
		break;
	}
}

// Whether the host left the stage of the Shine this game got (moveStage:
// after its Shine this game goes where the host went, if it went already).
bool hostWentOn(int area, int episode)
{
	return s_shineHoldFrames > 0 && (area != s_shineHoldArea || episode != s_shineHoldEpisode);
}

// A client's own Shine: no save prompt (its game plays the host's save, which
// the host saves), and the host is waited for (clientShineWait).
void clientShineExit(TMarDirector* dir)
{
	if (!dir || !dir->checkFlag(TMarDirector::DIRECTOR_FLAG_SHINE_TAKEN))
		return;
	dir->offFlag(TMarDirector::DIRECTOR_FLAG_CARD_SAVE_PENDING);
	int area, episode;
	if (!s_shineHoldFrames && mpg_target(&area, &episode)) {
		s_shineHoldArea    = area;
		s_shineHoldEpisode = episode;
		s_shineHoldFrames  = 60 * (port_active_frame_rate > 0 ? port_active_frame_rate : 30);
		showNotice("Shine Get! The host saves it, then everyone goes on together");
	}
}

// Once a client's Shine Get is over (its cutscene and banner), it would leave
// at once (TMarDirector::updateGameMode, this tick's changeState): it waits a
// few seconds for the host to save and set off instead, and then goes where
// the host goes (moveStage): the plaza, or where a mod of the host's sends
// everyone (the next episode). A host that takes longer is waited for in the
// plaza. Each tick, after the movement and before changeState.
int s_shineWaitTicks;

void clientShineWait(TMarDirector* dir)
{
	if (!dir || !dir->checkFlag(TMarDirector::DIRECTOR_FLAG_SHINE_TAKEN)) {
		s_shineWaitTicks = 0;
		return;
	}
	if (dir->mState != TMarDirector::STATE_UNK4 || dir->unk124 != 0 || dir->unk126 != 0 || s_shineWaitTicks < 0)
		return;
	if (s_shineWaitTicks == 0 && !dir->checkFlag(TMarDirector::DIRECTOR_FLAG_STAGE_TRANSITION_PENDING))
		return; // a Shine that does not end the stage: nothing to hold
	int area, episode;
	const bool known    = mpg_target(&area, &episode);
	const bool hostGone = !known || hostWentOn(area, episode) || mpg_target_traveling();
	if (!hostGone && s_shineWaitTicks < 5 * (port_active_frame_rate > 0 ? port_active_frame_rate : 30)) {
		if (s_shineWaitTicks++ == 0)
			fprintf(stderr, "[multiplayer] Shine Get over: waiting for the host to save and go on\n");
		dir->offFlag(TMarDirector::DIRECTOR_FLAG_STAGE_TRANSITION_PENDING);
		return;
	}
	fprintf(stderr, "[multiplayer] Shine Get over: off %s (waited %d ticks)\n", hostGone ? "with the host" : "to the plaza",
	        s_shineWaitTicks);
	s_shineWaitTicks = -1;
	dir->onFlag(TMarDirector::DIRECTOR_FLAG_STAGE_TRANSITION_PENDING);
}

// ------------------------------------------------------------------- enemies

// The host's enemies: clients put each of their small enemies (the stage's
// ordinary enemies; bosses keep their own) where the host's game has it, and
// an enemy defeated in any game is defeated in all. Enemies are named by
// their manager's place in the conductor's list and their index in it,
// which every game that loaded the stage shares.
const int kMaxManagers = 48;
const int kMaxEnemies  = 128; // per manager
TEnemyManager* s_managers[kMaxManagers];
int s_managerCount;
u8 s_enemyWasDown[kMaxManagers][kMaxEnemies]; // dying or dead last frame
u8 s_enemySyncKill[kMaxManagers][kMaxEnemies]; // we killed it on the host's word
u8 s_enemyWeDefeated[kMaxManagers][kMaxEnemies]; // this game's player defeated it (the host was told)
int s_spawnsHeld; // debug: enemies that came out here by themselves and went again
u16 s_enemyDownFrames[kMaxManagers][kMaxEnemies];
bool s_enemiesSeen; // the first look at this stage's enemies is not news
u8 s_hostAlive[kMaxManagers][kMaxEnemies];  // as the host's last word had it (2: no word yet)
// Whose game runs each enemy: the player nearest it, when near (host: as it
// decides; client: as it last heard). The owner's game runs it by itself, as
// a game alone does, so it chases, grabs and lets go of that player as it
// should; it reports where it is, and the host's copy (which the others
// follow) follows that.
u8 s_enemyOwner[kMaxManagers][kMaxEnemies];
u8 s_enemyHeldBy[kMaxManagers][kMaxEnemies];   // host: the client whose player it holds (0: none), lately
u16 s_enemyHeldAt[kMaxManagers][kMaxEnemies];  // host: when that was last said (s_clock, low bits)
u16 s_enemyOwnedAt[kMaxManagers][kMaxEnemies]; // host: when its owner last changed (s_clock, low bits)
const f32 kOwnNear   = 1500.0f; // nearer than this (and nearer than the host), a client's game runs it
const f32 kOwnFar    = 2500.0f; // farther than this from them, it goes back
const f32 kOwnMargin = 300.0f;  // how much nearer another player must be to take it over
const int kEnemyMirror = 253;   // the mirror channel's "manager" for enemies a client runs (index: manager << 8 | index)
u16 s_enemyGrace[kMaxManagers][kMaxEnemies]; // frames since we brought it back

// The host's last word on each enemy: where it was, which way it faced, how
// fast it went, and when that came (this game's ticks). Every enemy is
// steered toward it every tick (each update carries only some of them),
// a little ahead by its speed, smoothly whatever the frame rate.
struct EnemyTarget {
	JGeometry::TVec3<f32> pos, vel;
	f32 yaw;
	int at;
	bool known;
};
EnemyTarget s_targets[kMaxManagers][kMaxEnemies];
f32 s_lastWorldAge = 99.0f;
const f32 kEnemySettle  = 0.1f;  // seconds to close most of a gap
const f32 kEnemyAhead   = 0.15f; // the furthest an enemy is led on by its speed
const f32 kEnemyStale   = 1.5f;  // seconds without word: left to itself

void collectManagers()
{
	s_managerCount = 0;
	memset(s_enemyWasDown, 0, sizeof s_enemyWasDown);
	memset(s_enemySyncKill, 0, sizeof s_enemySyncKill);
	memset(s_enemyDownFrames, 0, sizeof s_enemyDownFrames);
	memset(s_hostAlive, 2, sizeof s_hostAlive);
	memset(s_enemyOwner, 0, sizeof s_enemyOwner);
	memset(s_enemyHeldBy, 0, sizeof s_enemyHeldBy);
	memset(s_enemyGrace, 0xFF, sizeof s_enemyGrace);
	memset(s_targets, 0, sizeof s_targets);
	s_enemiesSeen = false;
	if (!gpConductor)
		return;
	for (JGadget::TList<TEnemyManager*>::iterator it = gpConductor->unk20.begin();
	     it != gpConductor->unk20.end() && s_managerCount < kMaxManagers; ++it)
		s_managers[s_managerCount++] = *it;
}

TSmallEnemy* enemyAt(int manager, int index)
{
	if (manager < 0 || manager >= s_managerCount || index < 0 || index >= kMaxEnemies)
		return nullptr;
	TEnemyManager* mgr = s_managers[manager];
	if (!mgr || index >= mgr->getObjNum())
		return nullptr;
	return dynamic_cast<TSmallEnemy*>(mgr->getObj(index));
}

// Defeated, or on the way (its death animation playing).
bool enemyDown(TSmallEnemy* e)
{
	return e->checkLiveFlag(LIVE_FLAG_DEAD) || e->mSpine->getCurrentNerve() == &TNerveSmallEnemyDie::theNerve();
}

s16 yaw(f32 degrees) { return (s16)(degrees * (65536.0f / 360.0f)); }

// Host, once a frame: kills from the clients' games, then every enemy as it is.
f32 distance2(const JGeometry::TVec3<f32>& a, const JGeometry::TVec3<f32>& b); // below

unsigned mp_clock_ms_game()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

const JGeometry::TVec3<f32>& aimOf(int slot); // below

// Host, each frame: whose game runs each enemy. A client's whose player is
// near it and nearer than the host (and nearer than whoever ran it by a
// step, so it does not flit between two players side by side); or whose
// player it holds (a fish carrying them off). Else the host's.
void decideEnemyOwners()
{
	const u16 now = (u16)s_clock;
	for (int m = 0; m < s_managerCount; m++)
		for (int i = 0; i < s_managers[m]->getObjNum() && i < kMaxEnemies; i++) {
			TSmallEnemy* e = enemyAt(m, i);
			u8& owner      = s_enemyOwner[m][i];
			const u8 was   = owner;
			if (!e || enemyDown(e)) {
				owner = 0;
			} else if (s_enemyHeldBy[m][i] && (u16)(now - s_enemyHeldAt[m][i]) < 30 && watchable(s_enemyHeldBy[m][i])) {
				owner = s_enemyHeldBy[m][i];
			} else {
				s_enemyHeldBy[m][i] = 0;
				const f32 host = s_spectating || !gpMarioOriginal ? 1e9f : sqrtf(distance2(e->mPosition, gpMarioOriginal->mPosition));
				int best      = 0;
				f32 bestD     = host;
				for (int slot = 1; slot < MPG_MAX_PLAYERS; slot++)
					if (watchable(slot)) {
						const f32 d = sqrtf(distance2(e->mPosition, aimOf(slot)));
						if (d < bestD)
							best = slot, bestD = d;
					}
				const f32 ownerD = owner && watchable(owner) ? sqrtf(distance2(e->mPosition, aimOf(owner))) : 1e9f;
				const int fps    = port_active_frame_rate > 0 ? port_active_frame_rate : 30;
				const bool held  = (u16)(now - s_enemyOwnedAt[m][i]) < 2 * fps; // a change stays a while
				if (owner && ownerD < 1e8f && (held || (ownerD < kOwnFar && ownerD < bestD + kOwnMargin && ownerD < host + kOwnMargin)))
					; // stays with them
				else if (!owner && held)
					; // the host's a while yet
				else if (best && bestD < kOwnNear && bestD + kOwnMargin * 0.5f < host)
					owner = (u8)best;
				else
					owner = 0;
			}
			if (owner != was) {
				s_enemyOwnedAt[m][i]  = now;
				s_targets[m][i].known = false; // whatever its last owner said is old news
				if (getenv("SMS_MULTIPLAYER_DEBUG"))
					fprintf(stderr, "[multiplayer] enemy %d.%d: now run by %s's game\n", m, i, mpg_character_name(owner));
			}
		}
}

// Client, each tick: whether this game runs `e` (the host said so, or it
// holds this game's player: it carries them off and lets go here).
bool runsEnemy(int m, int i, TSmallEnemy* e)
{
	return s_role == MPG_ROLE_CLIENT && e
	       && (s_enemyOwner[m][i] == mpg_slot() || (gpMarioOriginal && gpMarioOriginal->mHolder == e));
}

// Client, every other tick: where the enemies this game runs are, for the host.
void reportOwnEnemies()
{
	if (s_clock & 1)
		return;
	for (int m = 0; m < s_managerCount; m++)
		for (int i = 0; i < s_managers[m]->getObjNum() && i < kMaxEnemies; i++) {
			TSmallEnemy* e = enemyAt(m, i);
			if (!runsEnemy(m, i, e) || enemyDown(e))
				continue;
			static int said;
			if (getenv("SMS_MULTIPLAYER_DEBUG") && gpMarioOriginal && said++ % 60 == 0)
				fprintf(stderr, "[multiplayer] enemy %d.%d run here (%s): %.0f from our player\n", m, i, typeid(*e).name(),
				        sqrtf(distance2(e->mPosition, gpMarioOriginal->mPosition)));
			u8 data[17];
			memcpy(data, &e->mPosition.x, 12);
			memcpy(data + 12, &e->mRotation.y, 4);
			data[16] = gpMarioOriginal && gpMarioOriginal->mHolder == e ? 1 : 0;
			mpg_mirror(kEnemyMirror, m << 8 | i, data, sizeof data);
		}
}

void hostEnemies()
{
	int manager, index;
	while (mpg_next_enemy_kill(&manager, &index))
		if (TSmallEnemy* e = enemyAt(manager, index))
			if (!enemyDown(e)) {
				fprintf(stderr, "[multiplayer] enemy %d.%d defeated in a client's game\n", manager, index);
				e->kill();
			}
	if (const char* sl = getenv("SMS_MULTIPLAYER_SPEEDLOG")) {
		static JGeometry::TVec3<f32> prev[3];
		static int frames;
		const int fps = port_active_frame_rate > 0 ? port_active_frame_rate : 30;
		int m = 0;
		while (m < s_managerCount && !(s_managers[m]->getObjNum() && enemyAt(m, 0) && strstr(typeid(*enemyAt(m, 0)).name(), sl)))
			m++;
		frames++;
		for (int i = 0; m < s_managerCount && i < 3 && i < s_managers[m]->getObjNum(); i++)
			if (TSmallEnemy* e = enemyAt(m, i)) {
				const f32 d = sqrtf(distance2(e->mPosition, prev[i])) * fps;
				if (frames % 10 == 0 && !enemyDown(e))
					fprintf(stderr, "[speed] host %d.%d speed %.0f\n", m, i, d);
				prev[i] = e->mPosition;
			}
	}
	decideEnemyOwners();
	static MpgEnemy list[kMaxManagers * 16];
	int n = 0;
	for (int m = 0; m < s_managerCount; m++)
		for (int i = 0; i < s_managers[m]->getObjNum() && i < kMaxEnemies && n < (int)(sizeof list / sizeof *list); i++) {
			TSmallEnemy* e = enemyAt(m, i);
			if (!e)
				continue;
			MpgEnemy& out = list[n++];
			out.manager   = (u8)m;
			out.index     = (u16)i;
			out.alive     = !enemyDown(e);
			out.pos[0]    = e->mPosition.x;
			out.pos[1]    = e->mPosition.y;
			out.pos[2]    = e->mPosition.z;
			out.rotY      = yaw(e->mRotation.y);
			out.owner     = s_enemyOwner[m][i];
			out.pad       = 0;
		}
	mpg_set_enemies(list, n);
	static int debugFrames;
	if (getenv("SMS_MULTIPLAYER_DEBUG") && ++debugFrames % 150 == 0)
		for (int k = 0; k < n && k < 4; k++)
			fprintf(stderr, "[multiplayer] enemy %d.%d %s at %.0f %.0f %.0f\n", list[k].manager, list[k].index,
			        list[k].alive ? "up" : "down", list[k].pos[0], list[k].pos[1], list[k].pos[2]);
}

// Client, once a frame: our own defeats go to the host.
void reportEnemyDefeats()
{
	for (int m = 0; m < s_managerCount; m++)
		for (int i = 0; i < s_managers[m]->getObjNum() && i < kMaxEnemies; i++) {
			TSmallEnemy* e = enemyAt(m, i);
			if (!e)
				continue;
			const bool down = enemyDown(e);
			// not one we just brought back (it can fall while it settles)
			if (s_enemyGrace[m][i] < 0xFFFF)
				s_enemyGrace[m][i]++;
			if (down && !s_enemyWasDown[m][i] && !s_enemySyncKill[m][i] && s_enemiesSeen && s_enemyGrace[m][i] > 180) {
				mpg_enemy_killed(m, i);
				s_enemyWeDefeated[m][i] = 1;
			}
			// come out here by itself (its own cannon or spawner) while the host's
			// game has it gone: the host's word decides what is about, so it
			// goes again, quietly (if it is about there, it comes back at once,
			// where the host's is)
			if (!down && s_enemyWasDown[m][i] && s_enemiesSeen && s_hostAlive[m][i] == 0 && s_enemyGrace[m][i] > 2
			    && s_lastWorldAge < 1.0f) {
				e->TLiveActor::kill();
				e->onLiveFlag(LIVE_FLAG_HIDDEN);
				s_enemySyncKill[m][i] = 1;
				s_enemyWasDown[m][i]  = true;
				s_spawnsHeld++;
				continue;
			}
			if (!down)
				s_enemySyncKill[m][i] = s_enemyWeDefeated[m][i] = 0;
			s_enemyDownFrames[m][i] = down ? (u16)(s_enemyDownFrames[m][i] < 60000 ? s_enemyDownFrames[m][i] + 1 : 60000) : 0;
			s_enemyWasDown[m][i] = down;
		}
	s_enemiesSeen = true;
	static int debugFrames;
	if (getenv("SMS_MULTIPLAYER_DEBUG") && ++debugFrames % 150 == 0) {
		int shown = 0;
		for (int m = 0; m < s_managerCount && shown < 4; m++)
			for (int i = 0; i < s_managers[m]->getObjNum() && i < kMaxEnemies && shown < 4; i++)
				if (TSmallEnemy* e = enemyAt(m, i)) {
					fprintf(stderr, "[multiplayer] enemy %d.%d %s at %.0f %.0f %.0f (here)\n", m, i,
					        enemyDown(e) ? "down" : "up", e->mPosition.x, e->mPosition.y, e->mPosition.z);
					shown++;
				}
	}
}

// Client, every tick after the movement pass: the host's enemies where the
// host has them (eased in), defeated or back as the host's are.
f32 shortestTurn(f32 from, f32 to)
{
	f32 d = to - from;
	while (d > 180.0f)
		d -= 360.0f;
	while (d < -180.0f)
		d += 360.0f;
	return d;
}

void steerEnemies()
{
	const int fps  = port_active_frame_rate > 0 ? port_active_frame_rate : 30;
	const f32 tick = 1.0f / fps;
	const f32 k    = 1.0f - expf(-tick / kEnemySettle);
	int followed = 0;
	f32 off = 0.0f, worst = 0.0f;
	for (int m = 0; m < s_managerCount; m++)
		for (int i = 0; i < kMaxEnemies; i++) {
			EnemyTarget& t = s_targets[m][i];
			if (!t.known)
				continue;
			const f32 since = (s_clock - t.at) * tick;
			if (since > kEnemyStale)
				continue;
			TSmallEnemy* e = enemyAt(m, i);
			if (!e || enemyDown(e) || runsEnemy(m, i, e))
				continue;
			const f32 lead = since < kEnemyAhead ? since : kEnemyAhead;
			const JGeometry::TVec3<f32> want(t.pos.x + t.vel.x * lead, t.pos.y + t.vel.y * lead, t.pos.z + t.vel.z * lead);
			const f32 dx = want.x - e->mPosition.x, dy = want.y - e->mPosition.y, dz = want.z - e->mPosition.z;
			if (dx * dx + dy * dy + dz * dz > 500.0f * 500.0f) {
				e->mPosition = want;
				e->mRotation.y = t.yaw;
				continue;
			}
			e->mPosition.x += dx * k;
			e->mPosition.y += dy * k;
			e->mPosition.z += dz * k;
			e->mRotation.y += shortestTurn(e->mRotation.y, t.yaw) * k;
			const f32 d = sqrtf(dx * dx + dy * dy + dz * dz);
			followed++;
			off += d;
			worst = d > worst ? d : worst;
		}
	if (getenv("SMS_MULTIPLAYER_DEBUG") && s_clock % (10 * fps) == 0 && followed)
		fprintf(stderr, "[multiplayer] enemies: %d followed%s, %.0f units off on average, %.0f at most; %d came out here alone\n",
		        followed, s_role == MPG_ROLE_HOST ? " (as the clients that run them say)" : "", off / followed, worst,
		        s_spawnsHeld);
}

void followHostEnemies()
{
	static MpgEnemy list[64];
	f32 age        = 0.0f;
	const int n    = mpg_enemies(list, 64, &age);
	// a new snapshot, by its number (its age cannot tell: snapshots come
	// about as often as frames, so most are as old at each frame as the last)
	static uint32_t lastSeq;
	const uint32_t seq = mpg_world_seq();
	const bool fresh   = n > 0 && seq != lastSeq;
	lastSeq            = seq;
	if (getenv("SMS_MULTIPLAYER_SPEEDLOG")) {
		static int frames, freshes, entries;
		frames++;
		freshes += fresh;
		entries += fresh ? n : 0;
		if (frames % 300 == 0) {
			fprintf(stderr, "[speed] snapshots: %d fresh in 300 frames, %d enemies in them on average\n", freshes,
			        freshes ? entries / freshes : 0);
			freshes = entries = 0;
		}
	}
	// nothing from the host for a second (its game is paused, or the link
	// is down): enemies here go by themselves until it comes again
	// (or the host's pose says it is paused: at once)
	if (MpHostPaused())
		age = 99.0f;
	s_lastWorldAge = age;
	static bool onOwn;
	if ((age > 1.0f) != onOwn) {
		onOwn = age > 1.0f;
		if (getenv("SMS_MULTIPLAYER_DEBUG"))
			fprintf(stderr, "[multiplayer] enemies: %s\n",
			        onOwn ? "nothing from the host lately, they go by themselves here" : "following the host's again");
	}
	if (age > 1.0f) {
		if (!MpHostPaused())
			steerEnemies();
		return;
	}
	for (int k = 0; fresh && k < n; k++) {
		const MpgEnemy& h = list[k];
		TSmallEnemy* e    = enemyAt(h.manager, h.index);
		if (!e)
			continue;
		if (h.manager < kMaxManagers && h.index < kMaxEnemies && s_enemyOwner[h.manager][h.index] != h.owner) {
			s_enemyOwner[h.manager][h.index] = h.owner;
			if (getenv("SMS_MULTIPLAYER_DEBUG") && (h.owner == mpg_slot() || h.owner == 0))
				fprintf(stderr, "[multiplayer] enemy %d.%d: %s\n", h.manager, h.index,
				        h.owner == mpg_slot() ? "run here now (our player is nearest)" : "the host's again");
		}
		const bool down = enemyDown(e);
		const u8 hostWas = s_hostAlive[h.manager][h.index];
		s_hostAlive[h.manager][h.index] = h.alive;
		if (!h.alive) {
			if (!down) {
				s_enemySyncKill[h.manager][h.index] = 1;
				if (hostWas == 1) {
					// defeated in the host's game just now: defeated here. A
					// Bullet Bill gone there blew up at its flight's end (one
					// sprayed here drops its coins here, from the water drawn
					// here): it blows up here too, without coins
					if (getenv("SMS_MULTIPLAYER_DEBUG"))
						fprintf(stderr, "[multiplayer] enemy %d.%d defeated in the host's game\n", h.manager, h.index);
					if (TKiller* killer = dynamic_cast<TKiller*>(e)) {
						if (killer->getSpine()->getCurrentNerve() != &TNerveKillerExplosion::theNerve()) {
							// last seen at the sea's face in the host's game: it
							// splashed in there, which drops its coins (as here)
							const EnemyTarget& last = s_targets[h.manager][h.index];
							if (last.known && last.pos.y < 60.0f) {
								killer->mPosition = last.pos;
								killer->genEventCoin();
							}
							killer->getSpine()->pushNerve(&TNerveKillerExplosion::theNerve());
							killer->onHitFlag(HIT_FLAG_NO_COLLISION);
						}
					} else {
						e->kill();
					}
				} else {
					// one this game brought out by itself (its own spawner, near
					// this player) that the host's game has not: gone, quietly
					e->TLiveActor::kill();
					e->onLiveFlag(LIVE_FLAG_HIDDEN);
				}
			}
			continue;
		}
		const JGeometry::TVec3<f32> at(h.pos[0], h.pos[1], h.pos[2]);
		if (down) {
			// back on the host: back here, at once if ours went by itself or
			// never came (a Bullet Bill the host's cannon fired), after a while
			// if this player defeated it (the news is on its way to the host)
			const u16 wait = s_enemyWeDefeated[h.manager][h.index] ? 90 : 2;
			if (e->checkLiveFlag(LIVE_FLAG_DEAD) && s_enemyDownFrames[h.manager][h.index] > wait) {
				const JGeometry::TVec3<f32> rot(0.0f, h.rotY * (360.0f / 65536.0f), 0.0f);
				const JGeometry::TVec3<f32> still(0.0f, 0.0f, 0.0f);
				if (getenv("SMS_MULTIPLAYER_DEBUG"))
					fprintf(stderr, "[multiplayer] enemy %d.%d back, as in the host's game\n", h.manager, h.index);
				e->resetSRTV(at, rot, e->mScaling, still);
				s_enemyGrace[h.manager][h.index] = 0;
			}
			continue;
		}
		if (runsEnemy(h.manager, h.index, e)) {
			s_targets[h.manager][h.index].known = false; // ours: the host's copy follows it
			continue;
		}
		// the host's word on it: kept, with how fast it went since the last
		EnemyTarget& t = s_targets[h.manager][h.index];
		const int fps  = port_active_frame_rate > 0 ? port_active_frame_rate : 30;
		const f32 dt   = (s_clock - t.at) / (f32)fps;
		if (t.known && dt > 0.02f && dt < 1.0f)
			t.vel.set((at.x - t.pos.x) / dt, (at.y - t.pos.y) / dt, (at.z - t.pos.z) / dt);
		else
			t.vel.set(0.0f, 0.0f, 0.0f);
		if (getenv("SMS_MULTIPLAYER_SPEEDLOG") && h.index < 3 && enemyAt(h.manager, h.index)
		    && strstr(typeid(*enemyAt(h.manager, h.index)).name(), getenv("SMS_MULTIPLAYER_SPEEDLOG")))
			fprintf(stderr, "[speed] client %d.%d at %u ms dt %.3f speed %.0f\n", h.manager, h.index, mp_clock_ms_game(), dt,
			        sqrtf(t.vel.x * t.vel.x + t.vel.y * t.vel.y + t.vel.z * t.vel.z));
		t.pos   = at;
		t.yaw   = h.rotY * (360.0f / 65536.0f);
		t.at    = s_clock;
		t.known = true;
	}
	steerEnemies();
}

// ------------------------------------------------------------------- arriving

// A player who joins starts beside the stage's start, not inside the host:
// a step to the side by slot (Luigi right, Wario left, Metal Mario two steps
// right), where there is floor at about the same height. Where a wall, a
// ledge or water is in the way, the next of the slot's other places is tried
// (the other side, a step behind), so no two players start in one spot.
void spreadStart()
{
	TMario* m = gpMarioOriginal;
	const int slot = mpg_slot();
	if (!m || !gpMap || slot <= 0 || slot >= MPG_MAX_PLAYERS || m->onYoshi())
		return;
	// to the right and behind, per slot, in order of preference
	static const f32 kPlaces[MPG_MAX_PLAYERS][4][2] = {
		{{0, 0}, {0, 0}, {0, 0}, {0, 0}},
		{{130, 0}, {130, -130}, {0, -130}, {-130, -260}},
		{{-130, 0}, {-130, -130}, {130, -260}, {0, -260}},
		{{260, 0}, {-260, 0}, {260, -130}, {-130, -390}},
		{{-260, 0}, {260, -260}, {-260, -130}, {130, -390}},
		{{390, 0}, {-390, 0}, {390, -130}, {-260, -390}},
		{{-390, 0}, {0, -390}, {-390, -130}, {260, -390}},
		{{0, -130}, {520, 0}, {-520, 0}, {0, -520}},
	};
	const f32 yaw  = m->mFaceAngle.y * (3.14159265f / 32768.0f);
	// the right of the way Mario faces, and his back
	const f32 rx = -cosf(yaw), rz = sinf(yaw);
	const f32 bx = -sinf(yaw), bz = -cosf(yaw);
	for (int i = 0; i < 4; i++) {
		const f32 side = kPlaces[slot][i][0], back = -kPlaces[slot][i][1];
		JGeometry::TVec3<f32> at(m->mPosition.x + rx * side + bx * back, m->mPosition.y,
		                         m->mPosition.z + rz * side + bz * back);
		const TBGCheckData* floor = nullptr;
		const f32 ground = gpMap->checkGround(at.x, at.y + 80.0f, at.z, &floor);
		if (!floor || floor->isIllegalData() || ground < m->mPosition.y - 60.0f || ground > m->mPosition.y + 60.0f)
			continue;
		at.y = ground;
		m->warpRequest(at, m->mFaceAngle.y * (360.0f / 65536.0f));
		if (getenv("SMS_MULTIPLAYER_DEBUG"))
			fprintf(stderr, "[multiplayer] arrived: put beside the start\n");
		return;
	}
	// nowhere beside it: start where the stage says
}

// Puts Mario beside where he is now, on floor at about the same height (to
// the side or behind, the first that has some); false if there is none.
bool placeBeside(TMario* m)
{
	if (!m || !gpMap)
		return false;
	static const f32 kSpots[4][2] = {{130.0f, 0.0f}, {-130.0f, 0.0f}, {0.0f, 130.0f}, {0.0f, -130.0f}};
	const f32 yaw = m->mFaceAngle.y * (3.14159265f / 32768.0f);
	const f32 rx = -cosf(yaw), rz = sinf(yaw), bx = -sinf(yaw), bz = -cosf(yaw);
	for (int i = 0; i < 4; i++) {
		JGeometry::TVec3<f32> at(m->mPosition.x + rx * kSpots[i][0] + bx * kSpots[i][1], m->mPosition.y,
		                         m->mPosition.z + rz * kSpots[i][0] + bz * kSpots[i][1]);
		const TBGCheckData* floor = nullptr;
		const f32 ground = gpMap->checkGround(at.x, at.y + 80.0f, at.z, &floor);
		if (!floor || floor->isIllegalData() || ground < m->mPosition.y - 60.0f || ground > m->mPosition.y + 60.0f)
			continue;
		at.y = ground;
		m->warpRequest(at, m->mFaceAngle.y * (360.0f / 65536.0f));
		return true;
	}
	return false;
}

// ------------------------------------------------------------------- spectating

bool watchable(int slot)
{
	TMpPuppet* p = slot >= 0 && slot < MPG_MAX_PLAYERS ? s_puppets[slot] : nullptr;
	return p && slot != mpg_slot() && p->active() && !(p->pose().flags & MPG_POSE_SPECTATING);
}

f32 distance2(const JGeometry::TVec3<f32>& a, const JGeometry::TVec3<f32>& b)
{
	const f32 x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
	return x * x + y * y + z * z;
}

// The next player to watch from `from`, in `step`'s direction; -1 if none.
int nextWatch(int from, int step)
{
	for (int k = 1; k <= MPG_MAX_PLAYERS; k++) {
		const int slot = ((from < 0 ? 0 : from) + step * k + 2 * MPG_MAX_PLAYERS) % MPG_MAX_PLAYERS;
		if (watchable(slot))
			return slot;
	}
	return -1;
}

void showWatching()
{
	char text[128];
	if (s_watching >= 0)
		snprintf(text, sizeof text, "Out of lives: watching %s. L / R to watch another player", mpg_display_name(s_watching));
	else
		snprintf(text, sizeof text, "Out of lives: waiting for the others");
	showNotice(text);
}

// Each frame while out of lives: switch who we watch.
void spectateFrame(TMarDirector* dir)
{
	TMarioGamePad* pad = dir->getGamePad();
	int step           = 0;
	if (pad && pad->testTrigger(PAD_TRIGGER_R | PAD_BUTTON_RIGHT))
		step = 1;
	else if (pad && pad->testTrigger(PAD_TRIGGER_L | PAD_BUTTON_LEFT))
		step = -1;
	if (step || !watchable(s_watching)) {
		const int next = nextWatch(s_watching, step ? step : 1);
		if (next != s_watching || step) {
			s_watching = next;
			showWatching();
		}
	}
	s_noticeFrames = kNoticeFrames; // the line stays while we watch
}

// Every tick while out of lives: Mario is out of play (not moved, not drawn,
// touching nothing) and rides along with the player watched, so the camera
// follows them.
void spectateTick()
{
	TMario* m = gpMarioOriginal;
	if (!m)
		return;
	m->onFlag(MARIO_FLAG_IS_PERFORMING);
	m->onHitFlag(HIT_FLAG_NO_COLLISION);
	if (watchable(s_watching)) {
		TMpPuppet* p    = s_puppets[s_watching];
		m->mPosition    = p->mPosition;
		const MpgPose& pose = p->pose();
		// their facing, from their body's forward axis
		m->mFaceAngle.y = (s16)(atan2f(pose.mtx[0][2], pose.mtx[2][2]) * (32768.0f / 3.14159265f));
	}
}

// Out of lives, a 1-up brings a player back. The lives are shared, so each
// player who comes back takes one: when the lives reach the number of players
// watching ahead of us (lower slots first), we take ours and come back
// beside the player we were watching. Held for half a second, so that one
// coming back ahead of us has reached every game first.
int s_reviveFrames;

void reviveWithOneUp()
{
	TMario* m = gpMarioOriginal;
	if (!s_spectating || !m) {
		s_reviveFrames = 0;
		return;
	}
	const s32 lives = TFlagManager::getInstance()->getFlag(0x20001);
	int ahead       = 0;
	for (int slot = 0; slot < mpg_slot(); slot++) {
		MpgPose pose;
		float age;
		if (mpg_player_present(slot) && mpg_pose(slot, &pose, &age) && (pose.flags & MPG_POSE_SPECTATING))
			ahead++;
	}
	if (lives < ahead) {
		s_reviveFrames = 0;
		return;
	}
	if (++s_reviveFrames < (port_active_frame_rate > 0 ? port_active_frame_rate : 30) / 2)
		return;
	s_reviveFrames = 0;
	s_spectating   = false;
	TFlagManager::getInstance()->setFlag(0x20001, lives - 1);
	m->offFlag(MARIO_FLAG_IS_PERFORMING);
	m->offHitFlag(HIT_FLAG_NO_COLLISION);
	placeBeside(m);
	fprintf(stderr, "[multiplayer] back in the game with a 1-up (%d lives left)\n", lives - 1);
	showNotice("A 1-up! Back in the game");
}

// Host: everyone out of lives, every player watching the others? Then
// everyone leaves the world for Delfino Plaza (in the plaza, it starts
// again), with lives as a continue gives them. A player watches from the end
// of their game over, and says so through the stage's reload too (their pose
// while loading carries it, frame()), so the trip starts as the last of them
// is out, not once the reloads and the stage's opening have played out
// (which took a long while, with everyone back in the stage watching nobody).
u32 s_outIn[MPG_MAX_PLAYERS];      // host: the session in which each player was last seen watching
int s_poseSeenAt[MPG_MAX_PLAYERS]; // host: when each player's pose of this session was last seen (s_clock)
bool s_allOutTrip;                 // host: everyone out: this game's own game over takes everyone (missNextArea)
bool s_allOutLives;                // host: lives to set as a continue gives, once arrived

// Whether every other player is watching (the host's own part is the caller's).
bool othersAllOut()
{
	const int fps = port_active_frame_rate > 0 ? port_active_frame_rate : 30;
	const u32 now = mpg_session();
	for (int slot = 0; slot < MPG_MAX_PLAYERS; slot++) {
		if (slot == mpg_slot() || !mpg_player_present(slot))
			continue;
		MpgPose pose;
		float age;
		if (mpg_pose(slot, &pose, &age) && pose.session == now) {
			s_poseSeenAt[slot] = s_clock;
			if (pose.flags & MPG_POSE_SPECTATING) {
				s_outIn[slot] = now;
				continue;
			}
			if (age < 2.0f) {
				s_outIn[slot] = 0; // playing (back with a 1-up, say)
				return false;
			}
		}
		if (s_outIn[slot] == now)
			continue; // watching, last heard
		if (s_clock - s_poseSeenAt[slot] > 10 * fps)
			continue; // not in this stage for a while (at the title, say): not holding the others back
		return false;
	}
	return true;
}

void allOutNotice()
{
	s_allOutLives = true; // set as the group arrives (set now, a 1-up would bring the watching back here first)
	if (gpApplication.mCurrArea.getStage() == 1) {
		showNotice("Everyone is out of lives: back to the start");
		fprintf(stderr, "[multiplayer] everyone is out of lives: the plaza starts again\n");
	} else {
		showNotice("Everyone is out of lives: game over");
		fprintf(stderr, "[multiplayer] everyone is out of lives: out of the world to Delfino Plaza\n");
	}
}

void hostCheckEveryoneOut(TMarDirector* dir)
{
	if (s_restarting || !dir || !s_spectating || dir->mState != TMarDirector::STATE_UNK4)
		return; // paused, the map, a save prompt: once back
	if (!othersAllOut())
		return;
	s_restarting = true;
	allOutNotice();
	const TGameSequence& cur = gpApplication.mCurrArea;
	if (cur.getStage() == 1) {
		// the plaza: nowhere to leave to; it starts again
		mpg_restart_session();
		gpApplication.mNextArea.set(cur.getStage(), cur.getScenario(), 0);
	} else {
		// a real game over: out of the world, as the pause menu's Exit Area
		// leaves it (TMarDirector::changeState: the plaza, by the world's
		// way in), everyone with it
		gpApplication.mNextArea.set(1, 0xFF, 0);
		dir->unkE4 = 2;
	}
	dir->onFlag(TMarDirector::DIRECTOR_FLAG_STAGE_TRANSITION_PENDING);
}

int takeGameOver()
{
	if (!enabled())
		return 0;
	// no game over screen: back into the stage, watching the others
	s_missReload      = true;
	s_spectating      = true;
	s_spectateSession = mpg_session();
	s_spectateArea    = gpApplication.mCurrArea.getStage();
	s_watching        = -1;
	// the host, the last one out: its reload is everyone's way out instead
	if (s_role == MPG_ROLE_HOST && !s_restarting && othersAllOut()) {
		s_restarting = true;
		s_allOutTrip = true;
		allOutNotice();
	}
	return 1;
}

// ------------------------------------------------------------------- tests

// SMS_MULTIPLAYER_TEST="take@N:K;kill@N:K;die@N:L;warp@N:area,episode"
// (tools/mp_shots.py): N frames into each stage, take K of its coins as if
// Mario had touched them, defeat K of its enemies, lose a life (with L lives
// left first, when L is given), and leave for another area as if through a
// pipe. Test runs only.
struct TestPlan {
	bool parsed;
	int takeAt, takeCount;
	int againAt, againCount; // takes the first coins even if gone (as one who had not heard): two take one
	int hatchAt;             // feeds Yoshi's egg its fruit
	bool hatched;
	int tubAt;               // goes to the edge of Bowser's tub
	bool onTub;
	int poundAt;             // ground-pounds Bowser's tub (as landing on it does)
	bool pounded;
	int claimAt;             // claims an NPC as a talk does, and walks it off
	bool claimed;
	int demoAt;              // endlessdemo@N: the plaza's Shadow Mario gate camera, endless, ended 4 s on
	int flAt;                // fludd@N: touches the Airstrip's FLUDD (its story event)
	bool flTook;
	int redAt;               // ground-pounds the first red coin switch
	bool redPressed;
	int redTakeAt, redTakeCount; // pickred@N:K: takes K red coins that are out
	bool redTook;
	int toRedAt;             // goes to the red coin switch
	bool atRed;
	int gateAt, gateDistance; // gotogate@N:D: stands D units from the first Gatekeeper
	bool atGate;
	int enemyAt, enemyDistance; // nearenemy@N:D: stands D units from the enemy up farthest from the host
	bool atEnemy;
	int tearAt;                 // tear@N: Gooper Blooper's tentacle torn off, the game's own way
	int blueAt;                 // bluecoin@N: a blue coin of the stage taken, as touching it does
	bool blued;
	int voidAt;                 // void@N: out of the world (far under the stage), falling
	int aimAt;                  // aim@N: spray in place (R held down), the stick up a while, then down
	int graffitiAt;             // graffiti@N: spray the first graffiti hiding a blue coin (as water does), till it is clean
	bool graffitiQuiet;         // graffitilocal@N: the same, its water not sent (only the erasing is)
	int graffitiCoinAt;         // graffiticoin@N: take that graffiti's blue coin, as if from behind it
	int shineOutAt;             // shineout@N: the stage's first hidden Shine comes out, with its camera (an event's)
	bool voided;
	int cornerAt;               // tubmiddle@N: onto the middle of Bowser's tub, then where Mario stands, now and then
	bool cornered;
	bool tore;
	int surfAt;                 // surf@N: this player gets on a Blooper (surfing)
	bool surfed;
	int routeAt;                // route@N:x,y,z/x,y,z/...: this player put on each point in turn, 10 frames apart
	                            // (surfroute@N:...: N frames after getting on a Blooper, each time)
	bool routeSurf;
	int surfFrames;             // frames on a Blooper so far (surfroute)
	int routeCount;
	float route[8][3];
	int boatAt;                 // nearboat@N: this player stands on the first fruit boat
	int titleQuitAt;            // titlequit@N: this game goes back to the title, as the pause menu's Save & Quit does
	int quitAt;                 // quit@N: this game closes, as closing its window does
	int doorAt;                 // turbodoor@N: this game's first Turbo nozzle door broken down
	int mantaAt;                // mantaout@N: Phantamanta comes out (its fight begun)
	int rideAt, rideSeconds;    // rideboat@N[:S]: this player rides the nearest mud or leaf boat, driven on S seconds
	bool rode;
	bool onBoat;
	int sandAt;                 // sandblock@N: this player drops onto the first sand block
	bool onSand;
	int manholeAt, manholeWatch; // manhole@N[:1]: ground pounds into the nearest manhole lid (:1 stands beside it)
	bool manholeDone;
	int nearAt;              // stands before an NPC (presses of B talk to it), up to 1800 frames
	bool talked;
	bool tookAgain;
	int warpAt, warpArea, warpEpisode;
	int killAt, killCount;
	int dieAt, dieLives;
	int shineAt, shineId;  // shine@N:ID: this game gets Shine ID (its flag)
	int flagsAt, flagsCount; // flags@N:K: K save flags set (a well-played save)
	int livesAt, livesValue; // lives@N:L: the lives set to L (a 1-up, say)
	int dropAt;              // drop@N: a coin appears in front of Mario (as from a box)
	int takeDropAt;          // takedrop@N: this player takes the coins that appeared during the stage
	int together;            // frames this stage that another player was in sight (drop@ and takedrop@ count these)
	int coinsAt, coinsValue; // coins@N:K: the coin count set to K
	int touchAt;             // touchshine@N: a Shine appears where this player is and they touch it
	int bossWaterAt, bossWaterCount; // bosswater@N:K: K sprays in the boss's mouth, in this game
	int bossDownAt;                  // bossdown@N: the boss loses all its hit points to ground pounds
	int bossHitAt;                   // bosshit@N: a hit on this game's copy of the boss (mp_boss.cpp)
	int bossSprayAt, bossSprayFrames; // bossspray@N:K: that hit every tick for K ticks (a Gatekeeper sprayed)
	int shadowSprayAt, shadowSprayFrames; // shadowspray@N:K: Shadow Mario sprayed every 4 ticks for K ticks
	int shadowRunAt, shadowRunFast;   // shadowrun@N[:1]: Shadow Mario down, 150 ticks later running away (host); :1 his vanished run cut short
	int npcSprayAt, npcSprayFrames;   // npcspray@N:K: the nearest NPC sprayed every tick for K ticks
	int slotSprayAt;                  // slotspray@N: the first item slot machine sprayed (it spins)
	int melonAt;                      // rollmelon@N: the big watermelon rolled along for 150 frames
	bool slotSprayed;
	int carryAt, carryFrames;         // carry@N:K: pick up the nearest thing that can be carried, put it down K ticks on
	int fruitAt, fruitIndex;          // nearfruit@N[:K]: stand facing the nearest fruit (or the K-th) and press B (picking it up)
	int setFlagAt;                   // setflag@N:ID: a flag of the game's set (hex), e.g. what starts a fight
	unsigned setFlagId;
	int frames; // in this stage
	bool tookHere, warped, died, shone, flagged, livesSet, dropped, tookDrop, coinsSet, touched, bossWatered, bossDowned, bossHit, flagSet;
};
TestPlan s_test;
int s_testPressA; // test aids: frames left to hold B down (padRead; B talks)
int s_testAim;    // aim@N: frames left spraying in place (padRead: R down, the stick up, then down)

void testPadRead(void* status)
{
	s_padNow = static_cast<PADStatus*>(status)[0];
	MpServerPadRead(status); // held by the server (a race's start line), or watching
	if (s_selectLocked) {
		// the host's episode select: this player's controller does nothing
		// there, the host's choosing presses for them
		PADStatus& pad = static_cast<PADStatus*>(status)[0];
		pad.button     = s_selectPress;
		pad.stickX = pad.stickY = pad.substickX = pad.substickY = 0;
		pad.triggerLeft = pad.triggerRight = pad.analogA = pad.analogB = 0;
		return;
	}
	if (s_testPressA > 0) {
		static_cast<PADStatus*>(status)[0].button |= PAD_BUTTON_B;
		s_testPressA--;
	}
	if (s_testAim > 0) {
		PADStatus& pad   = static_cast<PADStatus*>(status)[0];
		pad.button      |= PAD_TRIGGER_R;
		pad.triggerRight = 255;
		pad.stickX       = 0;
		pad.stickY       = s_testAim > 240 ? 0 : s_testAim > 120 ? 72 : -72;
		s_testAim--;
	}
}

void testSprayNpc(bool first); // shared objects, below
void testNearFruit(int index);
void testSprayGraffiti(int frame, bool quiet);
void watchGraffiti();
TWaterHitPictureHideObj* firstBlueGraffiti();
TShine* testHiddenShine(const char** camera);
TResetFruit* s_testFruit;
bool testPoundTub();
bool testSpraySlot();
void testCarry(bool take);

void runTests(TMarDirector* dir, bool arrived)
{
	if (!s_test.parsed) {
		s_test.parsed = true;
		s_test.againAt = s_test.hatchAt = s_test.tubAt = s_test.poundAt = s_test.claimAt = s_test.nearAt = s_test.redAt = s_test.redTakeAt = s_test.toRedAt = s_test.gateAt = s_test.flAt = s_test.demoAt = s_test.enemyAt = s_test.tearAt = s_test.blueAt = s_test.cornerAt = s_test.voidAt = s_test.aimAt = s_test.graffitiAt = s_test.graffitiCoinAt = s_test.shineOutAt = s_test.surfAt = s_test.routeAt = s_test.boatAt = s_test.sandAt = s_test.rideAt = s_test.titleQuitAt = s_test.quitAt = s_test.doorAt = s_test.mantaAt = -1;
		s_test.takeAt = s_test.warpAt = s_test.killAt = s_test.dieAt = s_test.shineAt = s_test.flagsAt = -1;
		s_test.manholeAt = -1;
		s_test.livesAt = s_test.dropAt = s_test.takeDropAt = s_test.coinsAt = s_test.touchAt = s_test.bossWaterAt = s_test.bossDownAt = s_test.bossHitAt = s_test.setFlagAt = -1;
		s_test.bossSprayAt = s_test.shadowSprayAt = s_test.shadowRunAt = s_test.npcSprayAt = s_test.carryAt = s_test.fruitAt = s_test.slotSprayAt = s_test.melonAt = -1;
		s_test.dieLives = -1;
		if (const char* plan = getenv("SMS_MULTIPLAYER_TEST")) {
			const char* t = strstr(plan, "take@");
			if (t)
				sscanf(t, "take@%d:%d", &s_test.takeAt, &s_test.takeCount);
			if (const char* ed = strstr(plan, "endlessdemo@"))
				sscanf(ed, "endlessdemo@%d", &s_test.demoAt);
			if (const char* fl = strstr(plan, "fludd@"))
				sscanf(fl, "fludd@%d", &s_test.flAt);
			if (const char* tr = strstr(plan, "gotoswitch@"))
				sscanf(tr, "gotoswitch@%d", &s_test.toRedAt);
			if (const char* gg = strstr(plan, "gotogate@"))
				sscanf(gg, "gotogate@%d:%d", &s_test.gateAt, &s_test.gateDistance);
			if (const char* ne = strstr(plan, "nearenemy@"))
				sscanf(ne, "nearenemy@%d:%d", &s_test.enemyAt, &s_test.enemyDistance);
			if (const char* te = strstr(plan, "tear@"))
				sscanf(te, "tear@%d", &s_test.tearAt);
			if (const char* bc = strstr(plan, "bluecoin@"))
				sscanf(bc, "bluecoin@%d", &s_test.blueAt);
			if (const char* vo = strstr(plan, "void@"))
				sscanf(vo, "void@%d", &s_test.voidAt);
			if (const char* am = strstr(plan, "aim@"))
				sscanf(am, "aim@%d", &s_test.aimAt);
			if (const char* so = strstr(plan, "shineout@"))
				sscanf(so, "shineout@%d", &s_test.shineOutAt);
			if (const char* gc = strstr(plan, "graffiticoin@"))
				sscanf(gc, "graffiticoin@%d", &s_test.graffitiCoinAt);
			if (const char* gr = strstr(plan, "graffiti@"))
				sscanf(gr, "graffiti@%d", &s_test.graffitiAt);
			if (const char* gl = strstr(plan, "graffitilocal@")) {
				sscanf(gl, "graffitilocal@%d", &s_test.graffitiAt);
				s_test.graffitiQuiet = true;
			}
			if (const char* co = strstr(plan, "tubmiddle@"))
				sscanf(co, "tubmiddle@%d", &s_test.cornerAt);
			if (const char* su = strstr(plan, "surf@"))
				sscanf(su, "surf@%d", &s_test.surfAt);
			if (const char* ro = strstr(plan, "route@")) {
				int used = 0;
				s_test.routeSurf = ro >= plan + 4 && !strncmp(ro - 4, "surf", 4);
				if (sscanf(ro, "route@%d:%n", &s_test.routeAt, &used) == 1 && used > 0)
					for (const char* pt = ro + used; s_test.routeCount < 8;) {
						float* xyz = s_test.route[s_test.routeCount];
						if (sscanf(pt, "%f,%f,%f", &xyz[0], &xyz[1], &xyz[2]) != 3)
							break;
						s_test.routeCount++;
						pt = strchr(pt, '/');
						if (!pt)
							break;
						pt++;
					}
			}
			if (const char* nb = strstr(plan, "nearboat@"))
				sscanf(nb, "nearboat@%d", &s_test.boatAt);
			if (const char* tq = strstr(plan, "titlequit@"))
				sscanf(tq, "titlequit@%d", &s_test.titleQuitAt);
			if (const char* q = strstr(plan, ";quit@") ? strstr(plan, ";quit@") + 1 : !strncmp(plan, "quit@", 5) ? plan : nullptr)
				sscanf(q, "quit@%d", &s_test.quitAt);
			if (const char* mo = strstr(plan, "mantaout@"))
				sscanf(mo, "mantaout@%d", &s_test.mantaAt);
			if (const char* td = strstr(plan, "turbodoor@"))
				sscanf(td, "turbodoor@%d", &s_test.doorAt);
			if (const char* rb = strstr(plan, "rideboat@"))
				if (sscanf(rb, "rideboat@%d:%d", &s_test.rideAt, &s_test.rideSeconds) < 2)
					s_test.rideSeconds = 10;
			if (const char* sb = strstr(plan, "sandblock@"))
				sscanf(sb, "sandblock@%d", &s_test.sandAt);
			if (const char* mh = strstr(plan, "manhole@"))
				sscanf(mh, "manhole@%d:%d", &s_test.manholeAt, &s_test.manholeWatch);
			if (const char* rt = strstr(plan, "pickred@"))
				sscanf(rt, "pickred@%d:%d", &s_test.redTakeAt, &s_test.redTakeCount);
			if (const char* rd = strstr(plan, "redswitch@"))
				sscanf(rd, "redswitch@%d", &s_test.redAt);
			if (const char* nr = strstr(plan, "nearnpc@"))
				sscanf(nr, "nearnpc@%d", &s_test.nearAt);
			if (const char* cl = strstr(plan, "claim@"))
				sscanf(cl, "claim@%d", &s_test.claimAt);
			if (const char* pd = strstr(plan, "pound@"))
				sscanf(pd, "pound@%d", &s_test.poundAt);
			if (const char* tb = strstr(plan, "tubedge@"))
				sscanf(tb, "tubedge@%d", &s_test.tubAt);
			if (const char* h = strstr(plan, "hatch@"))
				sscanf(h, "hatch@%d", &s_test.hatchAt);
			if (const char* a = strstr(plan, "again@"))
				sscanf(a, "again@%d:%d", &s_test.againAt, &s_test.againCount);
			const char* k = strstr(plan, "kill@");
			if (k)
				sscanf(k, "kill@%d:%d", &s_test.killAt, &s_test.killCount);
			const char* d = strstr(plan, "die@");
			if (d)
				sscanf(d, "die@%d:%d", &s_test.dieAt, &s_test.dieLives);
			const char* w = strstr(plan, "warp@");
			if (w)
				sscanf(w, "warp@%d:%d,%d", &s_test.warpAt, &s_test.warpArea, &s_test.warpEpisode);
			const char* sh = strstr(plan, "shine@");
			while (sh && sh > plan && sh[-1] == 'h') // touchshine@
				sh = strstr(sh + 1, "shine@");
			if (sh)
				sscanf(sh, "shine@%d:%d", &s_test.shineAt, &s_test.shineId);
			const char* f = strstr(plan, "flags@");
			if (f)
				sscanf(f, "flags@%d:%d", &s_test.flagsAt, &s_test.flagsCount);
			if (const char* l = strstr(plan, "lives@"))
				sscanf(l, "lives@%d:%d", &s_test.livesAt, &s_test.livesValue);
			if (const char* dr = strstr(plan, "drop@"))
				if (dr == plan || dr[-1] != 'e') // not takedrop@
					sscanf(dr, "drop@%d", &s_test.dropAt);
			if (const char* td = strstr(plan, "takedrop@"))
				sscanf(td, "takedrop@%d", &s_test.takeDropAt);
			if (const char* c = strstr(plan, "coins@"))
				sscanf(c, "coins@%d:%d", &s_test.coinsAt, &s_test.coinsValue);
			if (const char* ts = strstr(plan, "touchshine@"))
				sscanf(ts, "touchshine@%d", &s_test.touchAt);
			if (const char* bw = strstr(plan, "bosswater@"))
				sscanf(bw, "bosswater@%d:%d", &s_test.bossWaterAt, &s_test.bossWaterCount);
			if (const char* bd = strstr(plan, "bossdown@"))
				sscanf(bd, "bossdown@%d", &s_test.bossDownAt);
			if (const char* bh = strstr(plan, "bosshit@"))
				sscanf(bh, "bosshit@%d", &s_test.bossHitAt);
			if (const char* bs = strstr(plan, "bossspray@"))
				sscanf(bs, "bossspray@%d:%d", &s_test.bossSprayAt, &s_test.bossSprayFrames);
			if (const char* ss = strstr(plan, "shadowspray@"))
				sscanf(ss, "shadowspray@%d:%d", &s_test.shadowSprayAt, &s_test.shadowSprayFrames);
			if (const char* sr = strstr(plan, "shadowrun@"))
				sscanf(sr, "shadowrun@%d:%d", &s_test.shadowRunAt, &s_test.shadowRunFast);
			if (const char* ns = strstr(plan, "npcspray@"))
				sscanf(ns, "npcspray@%d:%d", &s_test.npcSprayAt, &s_test.npcSprayFrames);
			if (const char* rm = strstr(plan, "rollmelon@"))
				sscanf(rm, "rollmelon@%d", &s_test.melonAt);
			if (const char* ss = strstr(plan, "slotspray@"))
				sscanf(ss, "slotspray@%d", &s_test.slotSprayAt);
			if (const char* cs = strstr(plan, "carry@"))
				sscanf(cs, "carry@%d:%d", &s_test.carryAt, &s_test.carryFrames);
			if (const char* nf = strstr(plan, "nearfruit@"))
				if (sscanf(nf, "nearfruit@%d:%d", &s_test.fruitAt, &s_test.fruitIndex) < 2)
					s_test.fruitIndex = -1;
			if (const char* sf = strstr(plan, "setflag@"))
				sscanf(sf, "setflag@%d:%x", &s_test.setFlagAt, &s_test.setFlagId);
		}
	}
	if (arrived) {
		s_test.frames   = 0;
		s_test.together = 0;
		s_test.tookHere = false;
	}
	s_test.frames++;
	for (int slot = 0; slot < MPG_MAX_PLAYERS; slot++) {
		MpgPose pose;
		float age;
		if (slot != mpg_slot() && mpg_pose(slot, &pose, &age) && (pose.flags & MPG_POSE_VISIBLE)) {
			s_test.together++;
			break;
		}
	}
	if (getenv("SMS_MULTIPLAYER_TEST") && s_test.frames % 600 == 0)
		fprintf(stderr, "[multiplayer] test: frame %d, %d together, flag set %d, hatched %d\n", s_test.frames,
		        s_test.together, (int)s_test.flagSet, (int)s_test.hatched);
	if (s_test.nearAt >= 0 && !s_test.talked && s_test.frames >= s_test.nearAt && s_test.frames < s_test.nearAt + 1800) {
		// stood there now and then, left to stand still a while (a talk wants
		// him waiting), then B pressed, as a player talks
		if ((s_test.frames - s_test.nearAt) % 150 == 0)
			MpMirrorTestNearNpc();
		if ((s_test.frames - s_test.nearAt) % 150 == 90) {
			s_testPressA = 3;
			TBaseNPC* near = gpMarDirector ? gpMarDirector->getTalkingNPC() : nullptr;
			fprintf(stderr, "[multiplayer] test: B pressed (Mario status %x, talk target %p%s)\n",
			        (unsigned)gpMarioOriginal->getStatus(), (void*)near,
			        near && near->checkLiveFlag(LIVE_FLAG_UNK20000) ? ", in range" : "");
		}
		if (gpMarDirector && gpMarDirector->isTalkModeNow()) {
			s_test.talked = true;
			fprintf(stderr, "[multiplayer] test: talking to an NPC\n");
		}
	}
	if (s_test.demoAt >= 0 && dir && s_test.frames == s_test.demoAt) {
		// "影マリオゲートお絵かきカメラ", the camera the plaza's script holds while Shadow Mario paints
		static const char kGateCamera[] = "\x89\x65\x83\x7d\x83\x8a\x83\x49\x83\x51\x81\x5b\x83\x67\x82\xa8"
		                                  "\x8a\x47\x82\xa9\x82\xab\x83\x4a\x83\x81\x83\x89";
		fprintf(stderr, "[multiplayer] test: an endless cutscene\n");
		dir->fireStartDemoCamera(kGateCamera, nullptr, -1, 0.0f, false, nullptr, 0, nullptr, JDrama::TFlagT<u16>(0));
	}
	if (s_test.demoAt >= 0 && dir && s_test.frames == s_test.demoAt + 120) {
		fprintf(stderr, "[multiplayer] test: ending it\n");
		dir->fireEndDemoCamera();
	}
	if (s_test.flAt >= 0 && !s_test.flTook && s_test.frames >= s_test.flAt && gpItemManager && gpMarioOriginal) {
		// "ウォーターガン", FLUDD lying on the Airstrip
		static const char kWaterGun[] = "\x83\x45\x83\x48\x81\x5b\x83\x5e\x81\x5b\x83\x4b\x83\x93";
		for (int i = 0; i < gpItemManager->getObjNum(); i++) {
			TLiveActor* a = gpItemManager->getObj(i);
			if (a && a->getName() && !strcmp(a->getName(), kWaterGun) && !a->checkLiveFlag(LIVE_FLAG_DEAD)) {
				gpMarioOriginal->mPosition = a->mPosition;
				fprintf(stderr, "[multiplayer] test: touching FLUDD (item %d)\n", i);
				static_cast<TItem*>(a)->touchPlayer(gpMarioOriginal);
				break;
			}
		}
		s_test.flTook = true;
	}
	if (s_test.toRedAt >= 0 && !s_test.atRed && s_test.frames >= s_test.toRedAt) {
		for (int k = 0; k < s_switchCount; k++)
			if (dynamic_cast<TRedCoinSwitch*>(s_switches[k])) {
				const JGeometry::TVec3<f32>& at = s_switches[k]->mPosition;
				gpMarioOriginal->mPosition.set(at.x + 150.0f, at.y + 100.0f, at.z);
				fprintf(stderr, "[multiplayer] test: to the red coin switch\n");
			}
		s_test.atRed = true;
	}
	if (s_test.sandAt >= 0 && !s_test.onSand && s_test.frames >= s_test.sandAt)
		s_test.onSand = MpMirrorTestSandBlock();
	if (s_test.manholeAt >= 0 && !s_test.manholeDone && s_test.frames >= s_test.manholeAt)
		s_test.manholeDone = MpMirrorTestManhole(s_test.frames - s_test.manholeAt, s_test.manholeWatch != 0);
	if (s_test.tearAt >= 0 && !s_test.tore && s_test.frames >= s_test.tearAt)
		s_test.tore = MpBossTestTear() != 0;
	if (s_test.graffitiAt >= 0 && s_test.frames >= s_test.graffitiAt && s_test.frames < s_test.graffitiAt + 600)
		testSprayGraffiti(s_test.frames - s_test.graffitiAt, s_test.graffitiQuiet);
	if (getenv("SMS_MULTIPLAYER_TEST") && s_test.frames % 30 == 0)
		watchGraffiti();
	if (s_test.shineOutAt >= 0 && s_test.frames == s_test.shineOutAt) {
		const char* camera = nullptr;
		TShine* hidden     = testHiddenShine(&camera);
		if (hidden) {
			fprintf(stderr, "[multiplayer] test: a Shine comes out at %.0f %.0f %.0f\n", hidden->mPosition.x,
			        hidden->mPosition.y, hidden->mPosition.z);
			hidden->appearWithDemo(camera);
		} else {
			fprintf(stderr, "[multiplayer] test: no hidden Shine (or no camera for one) here\n");
		}
	}
	if (s_test.shineOutAt >= 0 && s_test.frames % 15 == 0) {
		static int outBefore = -1;
		int out = 0;
		for (int i = 0; TShine* sh = shineAt(i); i++)
			out += !sh->checkLiveFlag(LIVE_FLAG_DEAD);
		if (out != outBefore)
			fprintf(stderr, "[multiplayer] test: %d Shines out here (stage frame %d)\n", out, s_test.frames);
		outBefore = out;
	}
	if (s_test.graffitiCoinAt >= 0 && s_test.frames == s_test.graffitiCoinAt)
		if (TWaterHitPictureHideObj* g = firstBlueGraffiti())
			if (TItem* coin = dynamic_cast<TItem*>(g->mHiddenObj)) {
				fprintf(stderr, "[multiplayer] test: taking the graffiti's blue coin\n");
				coin->taken(gpMarioOriginal);
			}
	if (s_test.aimAt >= 0 && s_test.frames == s_test.aimAt) {
		s_testAim = 300;
		fprintf(stderr, "[multiplayer] test: spraying in place, aiming up, then down\n");
	}
	if (s_testAim > 0 && s_testAim % 30 == 0 && gpMarioOriginal && mpg_slot() >= 0 && s_puppets[mpg_slot()]) {
		// this player's own puppet against Mario's own models, posed alike:
		// each joint's axes, how far apart (0: the same turn)
		TMpPuppet* self   = s_puppets[mpg_slot()];
		TMario* m         = gpMarioOriginal;
		TWaterGun* gun    = m->mWaterGun;
		struct Joint {
			const char* name;
			bool nozzle;
		};
		static const Joint joints[] = {{"jnt_head", false}, {"chn_chest", false}, {"chn_muzzle_1", true}};
		for (const Joint& j : joints) {
			J3DModel* real = j.nozzle ? (gun && gun->mCurrentNozzle < 6 && gun->mNozzleList[gun->mCurrentNozzle]
			                                 ? gun->mNozzleList[gun->mCurrentNozzle]->getMActor()->getModel()
			                                 : nullptr)
			                          : m->getM3UModel()->getModel();
			const int idx = real ? real->getModelData()->getJointName()->getIndex(j.name) : -1;
			MtxPtr a      = idx >= 0 && idx < real->getModelData()->getJointNum() ? real->getAnmMtx((u16)idx) : nullptr;
			MtxPtr b      = self->jointMtx(j.name, j.nozzle);
			if (!a || !b)
				continue;
			f32 off = 0.0f;
			for (int c = 0; c < 3; c++) {
				f32 la = 0.0f, lb = 0.0f, dot = 0.0f;
				for (int r = 0; r < 3; r++)
					la += a[r][c] * a[r][c], lb += b[r][c] * b[r][c], dot += a[r][c] * b[r][c];
				const f32 cosine = dot / sqrtf(la * lb + 1e-12f);
				off += 1.0f - cosine;
			}
			fprintf(stderr, "[multiplayer] test: %s: puppet against Mario %.4f (up %.2f %.2f %.2f / %.2f %.2f %.2f)\n", j.name, off,
			        a[0][1], a[1][1], a[2][1], b[0][1], b[1][1], b[2][1]);
		}
	}
	if (s_testAim > 0 && s_testAim % 30 == 0 && gpMarioOriginal) {
		MpgPose p;
		MpCapturePose(gpMarioOriginal, &p);
		fprintf(stderr, "[multiplayer] test: field %u: aim %d, head %d %d %d %d, waist %d %d %d %d, hover %d %d\n",
		        (unsigned)VIGetRetraceCount(), p.gunAngle,
		        p.headTurn[0], p.headTurn[1], p.headTurn[2], p.headTurn[3], p.waistTurn[0], p.waistTurn[1],
		        p.waistTurn[2], p.waistTurn[3], p.hoverTiltL, p.hoverTiltR);
	}
	if (s_test.voidAt >= 0 && !s_test.voided && s_test.frames >= s_test.voidAt && gpMarioOriginal) {
		s_test.voided = true;
		gpMarioOriginal->mPosition.y -= 30000.0f;
		fprintf(stderr, "[multiplayer] test: out of the world at %.0f %.0f %.0f\n", gpMarioOriginal->mPosition.x,
		        gpMarioOriginal->mPosition.y, gpMarioOriginal->mPosition.z);
	}
	if (s_test.cornerAt >= 0 && !s_test.cornered && s_test.frames >= s_test.cornerAt && gpMarioOriginal) {
		float at[3];
		if (MpBossTestTubMiddle(at)) {
			s_test.cornered = true;
			gpMarioOriginal->mPosition.set(at[0], at[1] + 300.0f, at[2]);
			fprintf(stderr, "[multiplayer] test: onto the tub's middle (%.0f %.0f %.0f)\n", at[0], at[1], at[2]);
		}
	}
	if (s_test.cornered && s_test.frames % 30 == 0 && gpMarioOriginal) {
		TMario* m = gpMarioOriginal;
		const TBGCheckData* g = m->mGroundPlane;
		fprintf(stderr, "[multiplayer] test: standing at %.0f %.0f %.0f on %s (floor %.0f)\n", m->mPosition.x, m->mPosition.y,
		        m->mPosition.z, !g || g->isIllegalData() ? "nothing" : g->getActor() ? typeid(*g->getActor()).name() : "the map",
		        m->mFloorPosition.y);
	}
	if (s_test.blueAt >= 0 && !s_test.blued && s_test.frames >= s_test.blueAt && gpItemManager && gpMarioOriginal) {
		s_test.blued = true;
		for (int i = 0; i < gpItemManager->getObjNum(); i++) {
			TLiveActor* a = gpItemManager->getObj(i);
			if (a && a->getActorType() == 0x20000010 && !a->checkLiveFlag(LIVE_FLAG_DEAD)) {
				fprintf(stderr, "[multiplayer] test: taking blue coin %d at %.0f %.0f %.0f\n", i, a->mPosition.x, a->mPosition.y,
				        a->mPosition.z);
				static_cast<TItem*>(a)->taken(gpMarioOriginal);
				break;
			}
		}
	}
	if (gpMarioOriginal && s_test.routeSurf) {
		const bool on = gpMarioOriginal->checkStatusType(MARIO_STATUS_FLAG_UNK10000) && gpMarioOriginal->mSurfGesso;
		s_test.surfFrames = on ? s_test.surfFrames + 1 : 0;
	}
	const int routeFrames = s_test.routeSurf ? s_test.surfFrames : s_test.frames;
	if (s_test.routeAt >= 0 && routeFrames >= s_test.routeAt && gpMarioOriginal) {
		const int step = (routeFrames - s_test.routeAt) / 10;
		if (step < s_test.routeCount && (routeFrames - s_test.routeAt) % 10 == 0) {
			const float* xyz = s_test.route[step];
			gpMarioOriginal->mPosition.set(xyz[0], xyz[1], xyz[2]);
			gpMarioOriginal->mForwardVel = 0.0f;
			fprintf(stderr, "[multiplayer] test: route point %d of %d (%.0f %.0f %.0f)\n", step + 1, s_test.routeCount, xyz[0],
			        xyz[1], xyz[2]);
		}
	}
	if (s_test.surfAt >= 0 && !s_test.surfed && s_test.frames >= s_test.surfAt && gpConductor) {
		// a Blooper to ride (a map object of the three kinds TMario::receiveMessage knows)
		s_test.surfed = true;
		TLiveActor* blooper = nullptr;
		for (JGadget::TList<TLiveManager*>::iterator it = gpConductor->unk10.begin(); it != gpConductor->unk10.end() && !blooper;
		     ++it)
			for (int i = 0; *it && i < (*it)->getObjNum() && !blooper; i++) {
				TLiveActor* a = (*it)->getObj(i);
				const u32 type = a ? (u32)a->getActorType() : 0;
				if (type >= 0x400000C5 && type <= 0x400000C7)
					blooper = a;
			}
		if (blooper) {
			gpMarioOriginal->mPosition = blooper->mPosition;
			gpMarioOriginal->receiveMessage(blooper, HIT_MESSAGE_ATTACK);
			fprintf(stderr, "[multiplayer] test: on a Blooper (%x) at %.0f %.0f %.0f, Mario's status %x (retrace %u)\n",
			        (unsigned)blooper->getActorType(), blooper->mPosition.x, blooper->mPosition.y, blooper->mPosition.z,
			        (unsigned)gpMarioOriginal->mStatus, (unsigned)VIGetRetraceCount());
		}
		if (!(gpMarioOriginal->mStatus & MARIO_STATUS_FLAG_UNK10000))
			fprintf(stderr, "[multiplayer] test: no Blooper to ride here\n");
	}
	if (s_test.titleQuitAt >= 0 && s_test.frames == s_test.titleQuitAt) {
		// the way the pause menu's Save & Quit leaves (TMarDirector::direct,
		// STATE_CARD_SAVE): to the title, without moveStage
		fprintf(stderr, "[multiplayer] test: back to the title\n");
		dir->unkE4 = 4;
		dir->unkB4 = TApplication::APP_STATE_DONE;
		gpApplication.getFader()->setFadeStatus(TSMSFader::FADE_STATUS_FULLY_FADED_OUT);
		dir->mState = TMarDirector::STATE_UNK12;
	}
	if (s_test.quitAt >= 0 && s_test.frames == s_test.quitAt) {
		fprintf(stderr, "[multiplayer] test: closing the game\n");
		exit(0);
	}
	if (s_test.mantaAt >= 0 && s_test.frames == s_test.mantaAt && !MpBossTestMantaOut())
		fprintf(stderr, "[multiplayer] test: no Phantamanta here\n");
	if (s_test.doorAt >= 0 && s_test.frames == s_test.doorAt && !MpMirrorTestTurboDoor())
		fprintf(stderr, "[multiplayer] test: no Turbo nozzle door here\n");
	if (s_test.rideAt >= 0 && !s_test.rode && s_test.frames >= s_test.rideAt) {
		s_test.rode = true;
		if (!MpMirrorTestRide(s_test.rideSeconds))
			fprintf(stderr, "[multiplayer] test: no mud or leaf boat here\n");
	}
	if (s_test.boatAt >= 0 && !s_test.onBoat && s_test.frames >= s_test.boatAt && gpConductor) {
		s_test.onBoat = true;
		for (JGadget::TList<TEnemyManager*>::iterator it = gpConductor->unk20.begin(); it != gpConductor->unk20.end(); ++it)
			if (*it && (*it)->getObjNum())
				if (TFruitsBoat* b = dynamic_cast<TFruitsBoat*>((*it)->getObj(0))) {
					gpMarioOriginal->mPosition.set(b->mPosition.x, b->mPosition.y + 400.0f, b->mPosition.z);
					fprintf(stderr, "[multiplayer] test: on the boat at %.0f %.0f %.0f\n", b->mPosition.x, b->mPosition.y,
					        b->mPosition.z);
					break;
				}
	}
	if (s_test.enemyAt >= 0 && !s_test.atEnemy && s_test.frames >= s_test.enemyAt && s_puppets[0]) {
		TSmallEnemy* best = nullptr;
		f32 farthest      = 0.0f;
		for (int m = 0; m < s_managerCount; m++)
			for (int i = 0; i < s_managers[m]->getObjNum() && i < kMaxEnemies; i++)
				if (TSmallEnemy* e = enemyAt(m, i))
					if (!enemyDown(e) && distance2(e->mPosition, s_puppets[0]->mPosition) > farthest)
						best = e, farthest = distance2(e->mPosition, s_puppets[0]->mPosition);
		if (best) {
			gpMarioOriginal->mPosition.set(best->mPosition.x + s_test.enemyDistance, best->mPosition.y + 100.0f,
			                               best->mPosition.z);
			fprintf(stderr, "[multiplayer] test: %d from a %s at %.0f %.0f %.0f (%.0f from the host)\n", s_test.enemyDistance,
			        typeid(*best).name(), best->mPosition.x, best->mPosition.y, best->mPosition.z, sqrtf(farthest));
		}
		s_test.atEnemy = true;
	}
	if (s_test.gateAt >= 0 && !s_test.atGate && s_test.frames >= s_test.gateAt) {
		float at[3];
		if (MpBossTestGatePos(at)) {
			gpMarioOriginal->mPosition.set(at[0] + s_test.gateDistance, at[1] + 100.0f, at[2]);
			fprintf(stderr, "[multiplayer] test: %d from the Gatekeeper at %.0f %.0f %.0f\n", s_test.gateDistance,
			        at[0], at[1], at[2]);
		}
		s_test.atGate = true;
	}
	if (s_test.redTakeAt >= 0 && !s_test.redTook && s_test.frames >= s_test.redTakeAt && gpItemManager) {
		int taken = 0;
		for (int i = 0; i < gpItemManager->getObjNum() && taken < s_test.redTakeCount; i++) {
			TLiveActor* a = gpItemManager->getObj(i);
			if (a && a->getActorType() == 0x2000000F && !a->checkLiveFlag(LIVE_FLAG_DEAD) && !a->checkLiveFlag(LIVE_FLAG_HIDDEN)) {
				static_cast<TItem*>(a)->taken(gpMarioOriginal);
				taken++;
			}
		}
		s_test.redTook = taken > 0;
		if (taken)
			fprintf(stderr, "[multiplayer] test: took %d red coins (%d here now)\n", taken,
			        (int)TFlagManager::getInstance()->getFlag(0x60000));
	}
	if (s_test.redAt >= 0 && !s_test.redPressed && s_test.frames >= s_test.redAt) {
		for (int k = 0; k < s_switchCount && !s_test.redPressed; k++)
			if (TRedCoinSwitch* red = dynamic_cast<TRedCoinSwitch*>(s_switches[k])) {
				fprintf(stderr, "[multiplayer] test: ground-pounding the red coin switch\n");
				red->receiveMessage(gpMarioOriginal, HIT_MESSAGE_HIP_DROP);
			}
		s_test.redPressed = true;
	}
	if (s_test.claimAt >= 0 && !s_test.claimed && s_test.frames >= s_test.claimAt)
		s_test.claimed = MpMirrorTestClaimNpc();
	if (s_test.poundAt >= 0 && !s_test.pounded && s_test.frames >= s_test.poundAt)
		s_test.pounded = testPoundTub();
	if (s_test.tubAt >= 0 && !s_test.onTub && s_test.frames >= s_test.tubAt)
		s_test.onTub = MpMirrorTestTubEdge();
	if (s_test.hatchAt >= 0 && !s_test.hatched && s_test.frames >= s_test.hatchAt)
		s_test.hatched = MpMirrorTestHatch();
	if (s_test.againAt >= 0 && s_test.frames == s_test.againAt + 300)
		fprintf(stderr, "[multiplayer] test: coins now %d\n", (int)TFlagManager::getInstance()->getFlag(0x40002));
	if (s_test.againAt >= 0 && !s_test.tookAgain && s_test.frames >= s_test.againAt) {
		s_test.tookAgain = true;
		TItemManager* mgr = items();
		int taken         = 0;
		for (int i = 0; mgr && i < mgr->getObjNum() && i < s_itemCount && taken < s_test.againCount; i++) {
			TLiveActor* a = mgr->getObj(i);
			if (a && a->getActorType() == 0x2000000E) {
				static_cast<TItem*>(a)->taken(gpMarioOriginal);
				taken++;
			}
		}
		fprintf(stderr, "[multiplayer] test: took %d coins again (coins %d)\n", taken,
		        (int)TFlagManager::getInstance()->getFlag(0x40002));
	}
	if (s_test.takeAt >= 0 && !s_test.tookHere && s_test.frames >= s_test.takeAt) {
		s_test.tookHere = true;
		TItemManager* mgr = items();
		int taken         = 0;
		for (int i = 0; mgr && i < mgr->getObjNum() && taken < s_test.takeCount; i++) {
			TLiveActor* a = mgr->getObj(i);
			if (a && a->getActorType() == 0x2000000E && !a->checkLiveFlag(LIVE_FLAG_DEAD) && i < s_itemCount
			    && s_itemAlive[i]) {
				static_cast<TItem*>(a)->taken(gpMarioOriginal);
				taken++;
			}
		}
		fprintf(stderr, "[multiplayer] test: took %d coins (coins %d)\n", taken,
		        (int)TFlagManager::getInstance()->getFlag(0x40002));
	}
	if (s_test.killAt >= 0 && s_test.frames == s_test.killAt) {
		int killed = 0;
		for (int m = 0; m < s_managerCount && killed < s_test.killCount; m++)
			for (int i = 0; i < s_managers[m]->getObjNum() && killed < s_test.killCount; i++)
				if (TSmallEnemy* e = enemyAt(m, i))
					if (!enemyDown(e)) {
						fprintf(stderr, "[multiplayer] test: defeating enemy %d.%d\n", m, i);
						e->kill();
						killed++;
					}
		fprintf(stderr, "[multiplayer] test: defeated %d enemies\n", killed);
	}
	if (s_test.dieAt >= 0 && !s_test.died && s_test.frames == s_test.dieAt && gpMarioOriginal && !s_spectating) {
		s_test.died = true;
		if (s_test.dieLives >= 0)
			TFlagManager::getInstance()->setFlag(0x20001, s_test.dieLives);
		fprintf(stderr, "[multiplayer] test: losing a life (%d left)\n", TFlagManager::getInstance()->getFlag(0x20001));
		gpMarioOriginal->loserExec();
	}
	if (s_test.flagsAt >= 0 && !s_test.flagged && s_test.frames >= s_test.flagsAt) {
		s_test.flagged = true;
		// after the 120 Shines: blue coins, then the story's own flags
		for (int i = 0; i < s_test.flagsCount && 0x78 + i < 0x3B4; i++)
			TFlagManager::getInstance()->setBool(true, 0x10078 + i);
		fprintf(stderr, "[multiplayer] test: set %d save flags\n", s_test.flagsCount);
	}
	if (s_test.livesAt >= 0 && !s_test.livesSet && s_test.frames >= s_test.livesAt) {
		s_test.livesSet = true;
		TFlagManager::getInstance()->setFlag(0x20001, s_test.livesValue);
		fprintf(stderr, "[multiplayer] test: lives set to %d\n", s_test.livesValue);
	}
	if (s_test.coinsAt >= 0 && !s_test.coinsSet && s_test.frames >= s_test.coinsAt) {
		s_test.coinsSet = true;
		TFlagManager::getInstance()->setFlag(0x40002, s_test.coinsValue);
		fprintf(stderr, "[multiplayer] test: coins set to %d\n", s_test.coinsValue);
	}
	if (s_test.dropAt >= 0 && !s_test.dropped && s_test.together >= s_test.dropAt && gpItemManager && gpMarioOriginal) {
		s_test.dropped = true;
		// the same place in every game: the stage's start, a little ahead
		const JGeometry::TVec3<f32>& at = gpMarioOriginal->mPosition;
		TMapObjBase* coin = gpItemManager->makeObjAppear(6500.0f, at.y + 50.0f, -3500.0f, 0x2000000E, true);
		fprintf(stderr, "[multiplayer] test: a coin appeared (%p)\n", (void*)coin);
	}
	if (s_test.takeDropAt >= 0 && !s_test.tookDrop && s_test.together >= s_test.takeDropAt) {
		s_test.tookDrop   = true;
		TItemManager* mgr = items();
		int taken         = 0;
		for (int i = 0; mgr && i < mgr->getObjNum() && i < kMaxItems; i++) {
			TLiveActor* a = mgr->getObj(i);
			if (a && a->getActorType() == 0x2000000E && !a->checkLiveFlag(LIVE_FLAG_DEAD) && appearedDuringStage(i)) {
				static_cast<TItem*>(a)->taken(gpMarioOriginal);
				taken++;
			}
		}
		fprintf(stderr, "[multiplayer] test: took %d coins that appeared during the stage\n", taken);
	}
	if (s_test.bossWaterAt >= 0 && !s_test.bossWatered && s_test.together >= s_test.bossWaterAt && gpConductor) {
		for (JGadget::TList<TEnemyManager*>::iterator it = gpConductor->unk20.begin(); it != gpConductor->unk20.end(); ++it)
			for (int i = 0; *it && i < (*it)->getObjNum(); i++)
				if (TBossPakkun* b = dynamic_cast<TBossPakkun*>((*it)->getObj(i))) {
					if (!dynamic_cast<TBossPakkunManager*>(*it) || dynamic_cast<TBossPakkunManager*>(*it)->mIsLightVersion)
						continue; // the light one (no fight) takes no water
					s_test.bossWatered = true;
					for (int k = 0; k < s_test.bossWaterCount; k++)
						b->gotWaterDamage();
					fprintf(stderr, "[multiplayer] test: %d sprays in Petey's mouth, water now %d\n", s_test.bossWaterCount,
					        b->mWaterMark);
				}
	}
	if (s_test.bossDownAt >= 0 && !s_test.bossDowned && s_test.together >= s_test.bossDownAt && gpConductor) {
		for (JGadget::TList<TEnemyManager*>::iterator it = gpConductor->unk20.begin(); it != gpConductor->unk20.end(); ++it) {
			TBossPakkunManager* mgr = dynamic_cast<TBossPakkunManager*>(*it);
			for (int i = 0; mgr && !mgr->mIsLightVersion && i < mgr->getObjNum(); i++)
				if (TBossPakkun* b = dynamic_cast<TBossPakkun*>(mgr->getObj(i))) {
					s_test.bossDowned = true;
					while (b->getHitPoints() > 0) {
						b->mState = BOSSPAKU_STATE_BELLY_UP;
						b->gotHipDropDamage();
					}
					fprintf(stderr, "[multiplayer] test: Petey ground-pounded down\n");
				}
		}
		if (!s_test.bossDowned && MpBossTestDown())
			s_test.bossDowned = true;
	}
	// (after the test's own trip, when it has one: in the stage it went to)
	if (s_test.setFlagAt >= 0 && !s_test.flagSet && s_test.frames >= s_test.setFlagAt && s_test.together > 0
	    && (s_test.warpAt < 0 || (s_test.warped && !arrived && s_test.frames > 1 && gpApplication.mCurrArea.getStage()
	                                                                                       == s_test.warpArea))) {
		s_test.flagSet = true;
		TFlagManager::getInstance()->setBool(true, s_test.setFlagId);
		fprintf(stderr, "[multiplayer] test: flag %x set\n", s_test.setFlagId);
	}
	if (s_test.bossSprayAt >= 0 && s_test.together >= s_test.bossSprayAt
	    && s_test.together < s_test.bossSprayAt + s_test.bossSprayFrames) {
		const int kind = MpBossTestHit();
		if (s_test.together == s_test.bossSprayAt || s_test.together + 1 == s_test.bossSprayAt + s_test.bossSprayFrames)
			fprintf(stderr, "[multiplayer] test: spraying this game's boss (kind %d), %s\n", kind,
			        s_test.together == s_test.bossSprayAt ? "starting" : "stopping");
	}
	if (s_test.shadowSprayAt >= 0 && s_test.frames >= s_test.shadowSprayAt
	    && s_test.frames < s_test.shadowSprayAt + s_test.shadowSprayFrames && s_test.frames % 4 == 0) {
		const int kind = MpBossTestShadow();
		if (s_test.frames < s_test.shadowSprayAt + 4)
			fprintf(stderr, "[multiplayer] test: spraying this game's Shadow Mario (kind %d)\n", kind);
	}
	if (s_test.shadowRunAt >= 0
	    && (s_test.frames == s_test.shadowRunAt || s_test.frames == s_test.shadowRunAt + 150
	        || (s_test.shadowRunFast && s_test.frames == s_test.shadowRunAt + 160))) {
		const int step = s_test.frames == s_test.shadowRunAt ? 0 : s_test.frames == s_test.shadowRunAt + 150 ? 1 : 2;
		static const char* const said[] = {"goes down", "runs away", "is back on his node at once"};
		if (MpBossTestShadowRun(step))
			fprintf(stderr, "[multiplayer] test: this game's Shadow Mario %s\n", said[step]);
	}
	if (s_test.carryAt >= 0 && (s_test.frames == s_test.carryAt || s_test.frames == s_test.carryAt + s_test.carryFrames))
		testCarry(s_test.frames == s_test.carryAt);
	if (s_test.fruitAt >= 0 && s_test.frames == s_test.fruitAt)
		testNearFruit(s_test.fruitIndex);
	if (s_test.fruitAt >= 0 && s_test.frames == s_test.fruitAt + 20 && gpMarioOriginal && s_testFruit) {
		// as TMario::taking does (landed by then)
		TMario* m = gpMarioOriginal;
		if (s_testFruit->receiveMessage(m, HIT_MESSAGE_TAKE)) {
			m->mHeldObject = s_testFruit;
			m->mUpperState = TMario::UPPER_STATE_HOLDING_OBJECT;
		}
		fprintf(stderr, "[multiplayer] test: taking the fruit (field %u): %s\n", (unsigned)VIGetRetraceCount(),
		        m->mHeldObject ? "holding it" : "refused");
	}
	if (s_test.fruitAt >= 0 && s_test.frames == s_test.fruitAt + 200 && gpMarioOriginal && gpMarioOriginal->mHeldObject) {
		s_testPressA = 6; // and throw it
		fprintf(stderr, "[multiplayer] test: throwing it (field %u)\n", (unsigned)VIGetRetraceCount());
	}
	if (s_test.fruitAt >= 0 && s_test.frames > s_test.fruitAt && s_test.frames <= s_test.fruitAt + 300
	    && (s_test.frames - s_test.fruitAt) % 30 == 0 && gpMarioOriginal)
		fprintf(stderr, "[multiplayer] test: holding %s (status %08x)\n",
		        gpMarioOriginal->mHeldObject ? typeid(*gpMarioOriginal->mHeldObject).name() : "nothing",
		        (unsigned)gpMarioOriginal->mStatus);
	if (s_test.melonAt >= 0 && s_test.frames >= s_test.melonAt && s_test.frames < s_test.melonAt + 150)
		MpMirrorTestRollMelon(s_test.frames - s_test.melonAt);
	// sprayed again every 20 seconds, six times: some spins pay out
	if (s_test.slotSprayAt >= 0 && s_test.frames >= s_test.slotSprayAt && (s_test.frames - s_test.slotSprayAt) % 600 == 0
	    && (s_test.frames - s_test.slotSprayAt) / 600 < 6)
		testSpraySlot();
	if (getenv("SMS_MULTIPLAYER_DEBUG") && s_test.frames % 150 == 0 && gpItemManager) {
		int coins = 0;
		for (int i = 0; i < gpItemManager->getObjNum(); i++) {
			TLiveActor* a = gpItemManager->getObj(i);
			coins += a && a->getActorType() == 0x2000000E && appearedDuringStage(i) && !a->checkLiveFlag(LIVE_FLAG_DEAD);
		}
		if (coins)
			fprintf(stderr, "[multiplayer] coins that came out during the stage: %d\n", coins);
	}
	if (s_test.npcSprayAt >= 0 && s_test.frames >= s_test.npcSprayAt
	    && s_test.frames < s_test.npcSprayAt + s_test.npcSprayFrames)
		testSprayNpc(s_test.frames == s_test.npcSprayAt);
	if (s_test.bossHitAt >= 0 && !s_test.bossHit && s_test.together >= s_test.bossHitAt) {
		const int kind = MpBossTestHit();
		if (kind) {
			s_test.bossHit = true;
			fprintf(stderr, "[multiplayer] test: a hit on this game's boss (kind %d)\n", kind);
		}
	}
	if (s_test.touchAt >= 0 && !s_test.touched && s_test.together >= s_test.touchAt && gpMarioOriginal) {
		int shines = 0;
		while (shineAt(shines))
			shines++;
		s_test.touched = shines > 0; // none here: try again in the next stage
		static int told = -1;
		if (told != shines)
			fprintf(stderr, "[multiplayer] test: %d Shines in this stage\n", shines);
		told = shines;
		for (int i = 0; shineAt(i); i++) {
			TShine* shine = shineAt(i);
			const JGeometry::TVec3<f32>& at = gpMarioOriginal->mPosition;
			shine->mPosition.set(at.x, at.y + 60.0f, at.z);
			if (shine->checkLiveFlag(LIVE_FLAG_DEAD))
				shine->appear();
			fprintf(stderr, "[multiplayer] test: touching Shine %d\n", (int)(shine->getEventId() & 0xFF));
			shine->touchPlayer(gpMarioOriginal);
			fprintf(stderr, "[multiplayer] test: Mario's status now %x (Shine Get %x), holding %p\n",
			        (unsigned)gpMarioOriginal->mStatus, (unsigned)MARIO_STATUS_WIN_DEMO, (void*)gpMarioOriginal->unk384);
			break;
		}
	}
	if (s_test.shineAt >= 0 && !s_test.shone && s_test.frames >= s_test.shineAt) {
		s_test.shone = true;
		TFlagManager::getInstance()->setShineFlag(s_test.shineId);
		fprintf(stderr, "[multiplayer] test: got Shine %d\n", s_test.shineId);
	}
	if (s_test.warpAt >= 0 && !s_test.warped && s_test.frames >= s_test.warpAt) {
		s_test.warped = true;
		fprintf(stderr, "[multiplayer] test: leaving for area %d, episode %d\n", s_test.warpArea, s_test.warpEpisode);
		dir->setNextStage((u16)(((s_test.warpArea + 1) << 8) | s_test.warpEpisode), nullptr);
	}
}

// ------------------------------------------------------------------- text

// The names over the other players near ours (SMS_MULTIPLAYER_NAMES=0: none),
// placed where the last frame drew them.
void updateNames()
{
	if (!GXPC_ExtOverlayLabels)
		return;
	static const bool namesOn = !getenv("SMS_MULTIPLAYER_NAMES") || strcmp(getenv("SMS_MULTIPLAYER_NAMES"), "0");
	GXPCLabel labels[GXPC_MAX_LABELS];
	int count = 0;
	if (namesOn && playing() && gpMarioOriginal) {
		const f32 viewer[3] = {gpMarioOriginal->mPosition.x, gpMarioOriginal->mPosition.y, gpMarioOriginal->mPosition.z};
		const int me        = mpg_slot();
		for (int slot = 0; slot < MPG_MAX_PLAYERS && count < GXPC_MAX_LABELS; slot++) {
			TMpPuppet* p = s_puppets[slot];
			if (!p || slot == me)
				continue;
			GXPCLabel& l = labels[count];
			if (!p->nameSpot(viewer, &l.x, &l.y, &l.alpha))
				continue;
			const char* name = mpg_display_name(slot);
			// the overlay's font has ASCII only
			size_t n = 0;
			for (const char* c = name; *c && n < sizeof l.text - 1; c++)
				l.text[n++] = (unsigned char)*c >= 0x20 && (unsigned char)*c < 0x7F ? *c : '?';
			l.text[n] = 0;
			count++;
		}
	}
	GXPC_ExtOverlayLabels(labels, count);
}

void updateText()
{
	updateNames();
	char line[256];
	while (mpg_take_notice(line, sizeof line))
		showNotice(line);
	if (s_noticeFrames > 0 && --s_noticeFrames == 0)
		s_notice[0] = 0;
	// a server's standing line (MPG_CMD_STATUS: a race's state) when there is no news
	overlay(OVERLAY_NOTICE, s_notice[0] ? s_notice : MpServerStatusLine());
	TMarDirector* dir = director();
	const bool paused = dir && dir->mState == TMarDirector::STATE_PAUSE_MENU;
	if (s_statusFrames > 0)
		s_statusFrames--;
	static const bool statusOn = !getenv("SMS_MULTIPLAYER_STATUS") || strcmp(getenv("SMS_MULTIPLAYER_STATUS"), "0");
	if (statusOn && (paused || s_statusFrames > 0)) {
		char status[512];
		mpg_status(status, sizeof status);
		overlay(OVERLAY_STATUS, status);
	} else {
		overlay(OVERLAY_STATUS, "");
	}
}

// ------------------------------------------------------------------- cutscenes

// A camera cutscene started in one game plays in every game: the others are
// taken to the player who started it and watch it with them. Not the stage's
// opening (every game plays its own), the Shine Get (syncShineGets) or a
// gate's camera (it follows the gate, which the others cannot be handed).
bool s_replayingDemo; // playing another game's cutscene: not news to send back
int s_replaySlot = -1; // whose cutscene is playing here (-1: none of the others')
int s_replayFrames, s_starterOut;
bool s_replayStarted;

// Cutscenes seen lately, by name. One that two games both start (the host
// leads a boss, and every copy plays its defeat) is one cutscene: the game
// that gets the other's after starting its own neither plays it again nor
// sends it back, so nobody is pulled to anybody.
struct RecentDemo {
	char name[64];
	int frame;
};
RecentDemo s_recentDemos[8];
int s_recentNext;

bool seenDemoLately(const char* name)
{
	const int fps = port_active_frame_rate > 0 ? port_active_frame_rate : 30;
	for (const RecentDemo& d : s_recentDemos)
		if (d.name[0] && !strcmp(d.name, name) && s_clock - d.frame < 10 * fps)
			return true;
	return false;
}

void rememberDemo(const char* name)
{
	RecentDemo& d = s_recentDemos[s_recentNext++ & 7];
	snprintf(d.name, sizeof d.name, "%s", name);
	d.frame = s_clock;
}
// Cameras a shared boss's own steps start (its fall, its Shine coming out):
// every game's copy takes those steps when the host's does (mp_boss.cpp),
// and plays its own, Shine and all. Passed on, the others would watch a
// second one, and a Shine camera with no Shine in it. The boss Shine's
// camera is also a few events' own (a cannon's on Noki Bay): theirs come out
// in that player's game only, and are got for everyone by the Shine Get.
bool ownCameraEverywhere(const char* name)
{
	static const char* const names[] = {"bgeso_fall_camera", "bgeso_fall_camera2", "bgeso_fall_camera3", "sirena_manta",
	                                    "bwanwan_down_camera", "btelesa_roll_camera", "meoto_mogu_camera",
	                                    "meoto_end_camera", "ボスシャインカメラ", "めおとウナギシャインカメラ"};
	for (size_t i = 0; i < sizeof names / sizeof *names; i++)
		if (!strcmp(name, names[i]))
			return true;
	return false;
}

int s_stageFrames;    // frames of play in this stage
const f32 kNearDemo = 3000.0f; // a player this near a cutscene's place watches it from where they are
char s_demoNames[8][64];
JGeometry::TVec3<f32> s_demoPos[8];
int s_demoNext;

// A camera that only shows something coming out: an item (a blue coin from a
// graffiti sprayed away, THideObjPictureTwin; coins), a Shine appearing
// (TShine::appearWithDemo, for any event's), or what a switch did. The
// others watch it from where they are, not taken across the stage to it:
// the Shine is got for everyone by whoever touches it (the Shine Get lines
// everyone up then), a coin by whoever takes it.
bool atPlace(const JGeometry::TVec3<f32>& a, const float* pos)
{
	const f32 dx = a.x - pos[0], dy = a.y - pos[1], dz = a.z - pos[2];
	return (const float*)&a.x == pos || dx * dx + dy * dy + dz * dz < 50.0f * 50.0f;
}

bool watchedFromHere(const char* name, const float* pos)
{
	static const char* const names[] = {"オブジェスイッチ用カメラ"}; // a switch's (TMapObjSwitch: coins thrown out)
	for (size_t i = 0; i < sizeof names / sizeof *names; i++)
		if (!strcmp(name, names[i]))
			return true;
	if (!pos)
		return false;
	for (int i = 0; gpItemManager && i < gpItemManager->getObjNum(); i++)
		if (TLiveActor* a = gpItemManager->getObj(i))
			if (atPlace(a->mPosition, pos))
				return true;
	for (int i = 0; TShine* sh = shineAt(i); i++)
		if (atPlace(sh->mPosition, pos))
			return true;
	return false;
}

// Shines another game's cutscene brought out here (TShine::appearWithDemo,
// as that game's event did), this stage: when this game's own event gets
// there, its cutscene for them is left out.
TShine* s_shinesOutEarly[8];
int s_shinesOutEarlyNext;
unsigned s_shinesOutEarlyStage;

// The Shine a cutscene is about (its camera centres on it), if any.
TShine* shineOfDemo(const float* pos, bool exact)
{
	if (!pos)
		return nullptr;
	for (int i = 0; TShine* sh = shineAt(i); i++)
		if (exact ? (const float*)&sh->mPosition.x == pos : atPlace(sh->mPosition, pos))
			return sh;
	return nullptr;
}

bool shineOutEarly(TShine* shine)
{
	if (s_shinesOutEarlyStage != s_stageNumber)
		return false;
	for (TShine* sh : s_shinesOutEarly)
		if (sh == shine)
			return true;
	return false;
}

int demoCamera(const char* name, const float* pos, int frames, float angle, int option, int hasCallback, void* actor,
               unsigned flags)
{
	(void)actor;
	if (getenv("SMS_MULTIPLAYER_DEBUG") && name)
		fprintf(stderr, "[multiplayer] cutscene asked for: %s (stage frame %d)\n", name, s_stageFrames);
	TMario* m = gpMarioOriginal;
	// a Shine coming out (TShine::appearWithDemo) that another game's
	// cutscene brought out here already, or that is out: it played
	if (enabled() && !s_replayingDemo && hasCallback && inStage()) {
		TShine* shine = shineOfDemo(pos, true);
		if (shine && (shineOutEarly(shine) || !shine->checkLiveFlag(LIVE_FLAG_DEAD))) {
			fprintf(stderr, "[multiplayer] the Shine's cutscene (%s) played here already: left out\n", name);
			return 1;
		}
	}
	if (!enabled() || s_replayingDemo || !inStage() || !s_playing || !name || !m || s_spectating || flags == 1
	    || m->mStatus == MARIO_STATUS_WIN_DEMO)
		return 0;
	// not in this player's first seconds in the stage: what plays then is
	// their arrival's (an opening, an episode's introduction), theirs alone;
	// passed on, a player who joins would pull everyone else to the start
	// to watch it
	const int fps = port_active_frame_rate > 0 ? port_active_frame_rate : 30;
	if (s_stageFrames < 12 * fps || ownCameraEverywhere(name))
		return 0;
	if (getenv("SMS_MULTIPLAYER_DEBUG"))
		fprintf(stderr, "[multiplayer] cutscene here: %s%s\n", name, seenDemoLately(name) ? " (seen lately)" : "");
	// another game's, just got (or ours, again): nothing to tell anyone
	if (seenDemoLately(name)) {
		rememberDemo(name);
		return 0;
	}
	rememberDemo(name);
	uint8_t buf[128];
	size_t n         = 0;
	const size_t len = strnlen(name, 63);
	buf[n++]         = (uint8_t)len;
	memcpy(buf + n, name, len);
	n += len;
	buf[n++] = pos ? 1 : 0;
	const float values[8] = {pos ? pos[0] : 0.0f, pos ? pos[1] : 0.0f, pos ? pos[2] : 0.0f, angle,
	                         m->mPosition.x, m->mPosition.y, m->mPosition.z, m->mFaceAngle.y * (360.0f / 65536.0f)};
	memcpy(buf + n, values, sizeof values); // the same build on both sides: the same float layout
	n += sizeof values;
	const s32 f = frames;
	memcpy(buf + n, &f, 4);
	n += 4;
	buf[n++] = (uint8_t)((option != 0) | (watchedFromHere(name, pos) ? 2 : 0)); // 2: everyone watches from where they are
	mpg_demo(buf, n);
	return 0;
}

void playOthersCutscenes(TMarDirector* dir)
{
	uint8_t buf[256];
	int slot;
	size_t n;
	while ((n = mpg_next_demo(&slot, buf, sizeof buf)) != 0) {
		TMario* m = gpMarioOriginal;
		if (!m || s_spectating || dir->isDemoModeNow() || m->mStatus == MARIO_STATUS_WIN_DEMO)
			continue; // watching one already
		const size_t len = buf[0];
		if (n < 1 + len + 1 + 32 + 4 + 1 || len > 63)
			continue;
		const int k = s_demoNext++ & 7;
		memcpy(s_demoNames[k], buf + 1, len);
		s_demoNames[k][len] = 0;
		if (seenDemoLately(s_demoNames[k]) || ownCameraEverywhere(s_demoNames[k]))
			continue; // this game played it already (its own copy started it too), or plays its own
		rememberDemo(s_demoNames[k]);
		const bool hasPos   = buf[1 + len] != 0;
		float values[8];
		memcpy(values, buf + 2 + len, sizeof values);
		s32 frames;
		memcpy(&frames, buf + 2 + len + sizeof values, 4);
		const bool option   = (buf[2 + len + sizeof values + 4] & 1) != 0;
		const bool fromHere = (buf[2 + len + sizeof values + 4] & 2) != 0;
		s_demoPos[k].set(values[0], values[1], values[2]);
		// to the player who started it, then their cutscene; one about a
		// place (a boss, a statue) brings everyone to the player nearest it,
		// and leaves that player where they are, and anyone already near it
		// (round a Gatekeeper's puddle, say)
		JGeometry::TVec3<f32> at(values[4], values[5], values[6]);
		f32 face = values[7];
		bool warp = !fromHere;
		if (hasPos && !fromHere) {
			const JGeometry::TVec3<f32>& focus = s_demoPos[k];
			f32 best = distance2(m->mPosition, focus);
			warp     = false;
			const bool nearAlready = best < kNearDemo * kNearDemo;
			for (int other = 0; other < MPG_MAX_PLAYERS && !nearAlready; other++)
				if (watchable(other) && distance2(s_puppets[other]->mPosition, focus) < best) {
					best = distance2(s_puppets[other]->mPosition, focus);
					at   = s_puppets[other]->mPosition;
					face = m->mFaceAngle.y * (360.0f / 65536.0f);
					warp = true;
				}
		}
		if (warp && gpMap) {
			// not to where there is no floor (they were in the air over
			// nothing this game knows): watched from here instead
			const TBGCheckData* floor = nullptr;
			gpMap->checkGround(at.x, at.y + 80.0f, at.z, &floor);
			if (!floor || floor->isIllegalData())
				warp = false;
		}
		if (warp)
			m->warpRequest(at, face);
		fprintf(stderr, "[multiplayer] %s started a cutscene (%s): everyone watches%s\n", mpg_character_name(slot),
		        s_demoNames[k], warp ? "" : " (from here)");
		s_replayingDemo = true;
		// a Shine coming out there (its event's TShine::appearWithDemo): this
		// game's comes out with it, as there, not seconds later when this
		// game's own event gets there (the cutscene showed no Shine until then)
		TShine* shine = hasPos ? shineOfDemo(&s_demoPos[k].x, false) : nullptr;
		if (shine && shine->checkLiveFlag(LIVE_FLAG_DEAD) && JDrama::TNameRefGen::search<TCameraMapTool>(s_demoNames[k])) {
			if (s_shinesOutEarlyStage != s_stageNumber) {
				memset(s_shinesOutEarly, 0, sizeof s_shinesOutEarly);
				s_shinesOutEarlyStage = s_stageNumber;
			}
			s_shinesOutEarly[s_shinesOutEarlyNext++ & 7] = shine;
			fprintf(stderr, "[multiplayer] its Shine comes out here too\n");
			shine->appearWithDemo(s_demoNames[k]);
		} else {
			dir->fireStartDemoCamera(s_demoNames[k], hasPos ? &s_demoPos[k] : nullptr, frames, values[3], option, nullptr,
			                         0, nullptr, JDrama::TFlagT<u16>(0));
		}
		s_replayingDemo = false;
		s_replaySlot    = slot;
		s_replayFrames  = 0;
		s_starterOut    = 0;
		s_replayStarted = false;
	}
}

// Another player's cutscene, played here, ends when theirs does. Some run
// until the stage's script that started them ends them (Shadow Mario
// painting an M), and this game's script did not start this one: it would
// play on for good. Once the player who started it is out of theirs (their
// pose says so) for half a second, or after a minute, it ends here too.
void endReplayWithStarter(TMarDirector* dir)
{
	if (s_replaySlot < 0)
		return;
	const int fps = port_active_frame_rate > 0 ? port_active_frame_rate : 30;
	// (it starts a frame or two after it is asked for)
	if (!dir || !dir->isDemoModeNow()) {
		if (s_replayStarted || ++s_replayFrames > 3 * fps)
			s_replaySlot = -1; // over by itself (or never began)
		return;
	}
	s_replayStarted = true;
	MpgPose pose;
	float age;
	const bool theirsOn = mpg_player_present(s_replaySlot) && mpg_pose(s_replaySlot, &pose, &age)
	                      && (pose.flags & MPG_POSE_DEMO);
	s_starterOut = theirsOn ? 0 : s_starterOut + 1;
	if (s_starterOut > fps / 2 || ++s_replayFrames > 60 * fps) {
		fprintf(stderr, "[multiplayer] %s's cutscene is over there: ended here too\n", mpg_character_name(s_replaySlot));
		dir->fireEndDemoCamera();
		s_replaySlot = -1;
	}
}

// The others hear that a cutscene is on (the pose's flag), so a friend
// standing still is not a mystery.
bool s_inDemo[MPG_MAX_PLAYERS];

void noticeCutscenes()
{
	for (int slot = 0; slot < MPG_MAX_PLAYERS; slot++) {
		MpgPose pose;
		float age;
		const bool demo = slot != mpg_slot() && mpg_player_present(slot) && mpg_pose(slot, &pose, &age)
		                  && (pose.flags & MPG_POSE_DEMO);
		if (demo && !s_inDemo[slot]) {
			char text[96];
			snprintf(text, sizeof text, "%s is watching a cutscene", mpg_display_name(slot));
			showNotice(text);
		}
		s_inDemo[slot] = demo;
	}
}

// ------------------------------------------------------------------- the nearest player

// Most of the game decides by where Mario is (gpMarioPos): an enemy chases
// him, a goop blob leaps at him, a switch or an event waits for him, a
// clipped-out object waits for him to come near. On the host, which runs the
// shared world, "Mario" is the player nearest each object while it moves
// (JDrama::TViewObj::testPerform: before and after), so all of that happens
// for every player, not just the host. An object near another player and
// off the host's screen counts as on screen while it moves (some wait for
// that), as if the host were watching it.
const f32 kNearOnScreen = 2500.0f;

bool s_nearestOn; // host, in a stage, with another player in it
int s_swapCount[MPG_MAX_PLAYERS]; // debug: object moves led by each other player lately
struct MoveSwap {
	void* object;
	JGeometry::TVec3<f32>* was;
	bool unclipped;
};
MoveSwap s_swaps[32];
int s_swapDepth;

// Where each other player is now, as near as the host can tell: their newest
// pose (their puppet is drawn a little behind it, for smoothness), carried on
// at their pace for as long as it took to get here (half the round trip, and
// since it came). What the host's enemies chase and wait for.
struct Aim {
	JGeometry::TVec3<f32> at, last;
	f32 vx, vz; // units a millisecond, along the ground
	uint32_t lastTime;
	bool have;
};
Aim s_aims[MPG_MAX_PLAYERS];

// Debug: how far the aim (and the drawn puppet, which the host chased before)
// is from where the player turns out to have been at that moment, once their
// poses from then arrive.
struct AimCheck {
	uint32_t when; // their game's time the aim was for
	JGeometry::TVec3<f32> aim, drawn;
};
AimCheck s_aimChecks[MPG_MAX_PLAYERS][64];
int s_aimCheckNext[MPG_MAX_PLAYERS];
double s_aimErr[MPG_MAX_PLAYERS], s_drawnErr[MPG_MAX_PLAYERS];
int s_aimErrCount[MPG_MAX_PLAYERS];

void checkAims(int slot, const JGeometry::TVec3<f32>& from, uint32_t fromTime, const JGeometry::TVec3<f32>& to,
               uint32_t toTime)
{
	if (!getenv("SMS_MULTIPLAYER_DEBUG") || toTime == fromTime)
		return;
	for (int i = 0; i < 64; i++) {
		AimCheck& c = s_aimChecks[slot][i];
		if (!c.when || (int32_t)(c.when - fromTime) <= 0 || (int32_t)(c.when - toTime) > 0)
			continue;
		const f32 t = (f32)(int32_t)(c.when - fromTime) / (f32)(int32_t)(toTime - fromTime);
		const JGeometry::TVec3<f32> was(from.x + (to.x - from.x) * t, from.y, from.z + (to.z - from.z) * t);
		const f32 ax = c.aim.x - was.x, az = c.aim.z - was.z, dx = c.drawn.x - was.x, dz = c.drawn.z - was.z;
		s_aimErr[slot] += sqrtf(ax * ax + az * az);
		s_drawnErr[slot] += sqrtf(dx * dx + dz * dz);
		s_aimErrCount[slot]++;
		c.when = 0;
	}
	if (s_aimErrCount[slot] >= 300) {
		fprintf(stderr, "[multiplayer] aim at %s: %.0f units off on average (the drawn puppet: %.0f)\n",
		        mpg_character_name(slot), s_aimErr[slot] / s_aimErrCount[slot], s_drawnErr[slot] / s_aimErrCount[slot]);
		s_aimErr[slot] = s_drawnErr[slot] = 0.0;
		s_aimErrCount[slot]                = 0;
	}
}

void updateAims()
{
	for (int slot = 0; slot < MPG_MAX_PLAYERS; slot++) {
		Aim& a = s_aims[slot];
		MpgPose pose;
		float age = 0.0f;
		if (!watchable(slot) || !mpg_pose(slot, &pose, &age)) {
			a.have = false;
			continue;
		}
		const JGeometry::TVec3<f32> pos(pose.mtx[0][3], pose.mtx[1][3], pose.mtx[2][3]);
		if (!a.have) {
			a.vx = a.vz = 0.0f;
		} else if (pose.time != a.lastTime) {
			const f32 dt = (f32)(int32_t)(pose.time - a.lastTime);
			const f32 dx = pos.x - a.last.x, dz = pos.z - a.last.z;
			if (dt > 0.0f && dt < 500.0f && dx * dx + dz * dz < 400.0f * 400.0f) {
				a.vx += (dx / dt - a.vx) * 0.5f;
				a.vz += (dz / dt - a.vz) * 0.5f;
			} else {
				a.vx = a.vz = 0.0f; // a warp, or a long gap: no pace to go by
			}
		}
		if (!a.have || pose.time != a.lastTime) {
			if (a.have)
				checkAims(slot, a.last, a.lastTime, pos, pose.time);
			a.last     = pos;
			a.lastTime = pose.time;
		}
		a.have         = true;
		const int ping = mpg_ping(slot);
		f32 lead       = (ping > 0 ? ping * 0.5f : 0.0f) + age * 1000.0f;
		if (lead > 200.0f)
			lead = 200.0f;
		a.at.set(a.last.x + a.vx * lead, a.last.y, a.last.z + a.vz * lead);
		if (getenv("SMS_MULTIPLAYER_DEBUG")) {
			AimCheck& c = s_aimChecks[slot][s_aimCheckNext[slot]++ & 63];
			c.when      = a.lastTime + (uint32_t)lead;
			c.aim       = a.at;
			c.drawn     = s_puppets[slot]->mPosition;
		}
	}
}

const JGeometry::TVec3<f32>& aimOf(int slot)
{
	return s_aims[slot].have ? s_aims[slot].at : s_puppets[slot]->mPosition;
}

// ------------------------------------------------------------------- effects

// The player's effects (Mario's particle manager's, as his own move asks for
// them: a slide's spray, a landing's dust, the steps' splashes), sent with
// the pose for the others to show at them (TMpPuppet::showEffects). Those
// asked for every frame while they go on (groups 1 and 3) are sent as a
// list, the newest tick's; those asked for once are numbered, and the
// newest few go with every pose, so a pose lost or not sent loses none.

// Which joint of `model` this matrix is (the game binds effects to them), or -1.
int jointOf(J3DModel* model, const void* mtx)
{
	if (!model)
		return -1;
	for (u16 j = 0; j < model->getModelData()->getJointNum() && j < 255; j++)
		if ((const void*)model->getAnmMtx(j) == mtx)
			return j;
	return -1;
}

int8_t fxTurnByte(f32 v) { return (int8_t)(v > 1.0f ? 127 : v < -1.0f ? -127 : lroundf(v * 127.0f)); }

void particleEmit(int how, int id, const void* at, int rx, int ry, int rz, int group, const void* owner)
{
	TMario* m = gpMarioOriginal;
	if (s_fxMoving <= 0 || !m || !at || !enabled() || !inStage() || id < 0 || id > 0xFFFF)
		return;
	// Mario's, or his FLUDD's: not what he sets off in something else (an
	// enemy's, its own game shows it)
	if (owner && owner != m && owner != m->mWaterGun)
		return;
	MtxPtr root = m->getM3UModel()->getModel()->getBaseTRMtx();
	MpgFx f;
	memset(&f, 0, sizeof f);
	f.id    = (uint16_t)id;
	f.how   = (uint8_t)how;
	f.group = (uint8_t)group;
	f32 pos[3];
	if (how <= 2) {
		const JGeometry::TVec3<f32>* v = static_cast<const JGeometry::TVec3<f32>*>(at);
		pos[0] = v->x, pos[1] = v->y, pos[2] = v->z;
		if (how == 2 && v == &m->mPosition)
			f.bind = MPG_FX_BIND_ROOT;
		if (how == 1) {
			f.turn[0] = (int8_t)(rx >> 8);
			f.turn[1] = (int8_t)(ry >> 8);
			f.turn[2] = (int8_t)(rz >> 8);
		}
	} else {
		const f32 (*mtx)[4] = static_cast<const f32 (*)[4]>(at);
		pos[0] = mtx[0][3], pos[1] = mtx[1][3], pos[2] = mtx[2][3];
		int joint = jointOf(m->getM3UModel()->getModel(), at);
		if (joint >= 0) {
			f.bind = MPG_FX_BIND_BODY;
		} else if (m->mWaterGun && m->mWaterGun->mFluddModel
		           && (joint = jointOf(m->mWaterGun->mFluddModel->getModel(), at)) >= 0) {
			f.bind = MPG_FX_BIND_FLUDD;
		} else {
			f32 q[4];
			Mtx turn;
			for (int r = 0; r < 3; r++)
				for (int c = 0; c < 3; c++)
					turn[r][c] = mtx[r][c];
			MpMtxToQuat(turn, q);
			for (int i = 0; i < 4; i++)
				f.turn[i] = fxTurnByte(q[i]);
		}
		f.joint = (uint8_t)(joint >= 0 ? joint : 0);
	}
	for (int i = 0; i < 3; i++) {
		const f32 d = pos[i] - root[i][3];
		if (d > 2000.0f || d < -2000.0f)
			return; // not at him (something he set off far away)
		f.off[i] = (int16_t)lroundf(d);
	}
	s_fxSent[group == 1 || group == 3]++;
	if (group == 1 || group == 3) {
		if (s_fxTickCount < MPG_MAX_FX)
			s_fxTick[s_fxTickCount++] = f;
		return;
	}
	if (s_fxShotCount == MPG_MAX_FX) {
		memmove(s_fxShots, s_fxShots + 1, sizeof s_fxShots[0] * (MPG_MAX_FX - 1));
		s_fxShotCount--;
	}
	s_fxShots[s_fxShotCount++] = f;
	s_fxShotSeq++;
}

// ------------------------------------------------------------------- sounds

// The player's own sounds, as their move starts them (their steps, jumps,
// landings, a ground pound, FLUDD's clicks, their voice), numbered as the
// effects asked for once are, the newest few with every pose, for the others
// to hear at them (TMpPuppet::playSounds). Only those the game starts once
// (0x800 in the id): those it starts every frame while they go on (a slide's
// scrape) would only be heard as their start.
int soundHook(unsigned* idp, const float* at, int)
{
	TMario* m         = gpMarioOriginal;
	const unsigned id = *idp;
	if (s_fxMoving <= 0 || !m || !at || !enabled() || !inStage() || id > 0xFFFF || !(id & 0x800))
		return 0;
	// FLUDD's (0), Mario's and Yoshi's (1), the voices (7): not what he sets
	// off in something else (a coin, an enemy: its own game sounds it)
	const unsigned category = id >> 12 & 0xF;
	if (category != 0 && category != 1 && category != 7)
		return 0;
	const f32 d[3] = {at[0] - m->mPosition.x, at[1] - m->mPosition.y, at[2] - m->mPosition.z};
	if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 1000.0f * 1000.0f)
		return 0; // not at him
	if (s_soundCount == MPG_MAX_SOUNDS) {
		memmove(s_sounds, s_sounds + 1, sizeof s_sounds[0] * (MPG_MAX_SOUNDS - 1));
		s_soundCount--;
	}
	s_sounds[s_soundCount++] = (uint16_t)id;
	s_soundSeq++;
	s_soundSent++;
	return 0; // played here as ever
}

// Debug: effects sent and shown, and sounds sent and heard, every 10 seconds.

void fxDebug()
{
	static const bool debug = getenv("SMS_MULTIPLAYER_DEBUG") != nullptr;
	static unsigned at;
	if (!debug || s_clock - at < 300)
		return;
	at = s_clock;
	fprintf(stderr, "[multiplayer] effects: sent %d once, %d going on;", s_fxSent[0], s_fxSent[1]);
	for (int slot = 0; slot < MPG_MAX_PLAYERS; slot++)
		if (s_fxShown[slot][0] || s_fxShown[slot][1])
			fprintf(stderr, " %s's shown %d once, %d going on;", mpg_display_name(slot), s_fxShown[slot][0], s_fxShown[slot][1]);
	fprintf(stderr, "\n");
	fprintf(stderr, "[multiplayer] sounds: sent %d;", s_soundSent);
	for (int slot = 0; slot < MPG_MAX_PLAYERS; slot++)
		if (s_soundHeard[slot])
			fprintf(stderr, " %s's heard %d;", mpg_display_name(slot), s_soundHeard[slot]);
	fprintf(stderr, "\n");
	memset(s_fxSent, 0, sizeof s_fxSent);
	memset(s_fxShown, 0, sizeof s_fxShown);
	s_soundSent = 0;
	memset(s_soundHeard, 0, sizeof s_soundHeard);
}

// Mario's move begins (0) or ends (1): what it asks for is the player's.
void fxMove(int after)
{
	if (!after) {
		if (s_fxMoving++ == 0)
			s_fxTickCount = 0;
		return;
	}
	if (s_fxMoving > 0 && --s_fxMoving == 0) {
		memcpy(s_fxOngoing, s_fxTick, sizeof s_fxTick);
		s_fxOngoingCount = s_fxTickCount;
	}
}

void objectMove(void* object, int after)
{
	if (object && object == gpMarioOriginal) {
		fxMove(after);
		if (after)
			fxDebug();
	}
	if (after) {
		if (s_swapDepth > 0 && s_swaps[s_swapDepth - 1].object == object) {
			MoveSwap& w = s_swaps[--s_swapDepth];
			gpMarioPos  = w.was;
			if (w.unclipped)
				static_cast<TLiveActor*>(object)->onLiveFlag(LIVE_FLAG_CLIPPED_OUT);
		}
		return;
	}
	if (!s_nearestOn || s_swapDepth >= 32 || !gpMarioPos)
		return;
	TLiveActor* a = dynamic_cast<TLiveActor*>(static_cast<JDrama::TViewObj*>(object));
	if (!a)
		return;
	// Bowser's hot tub sets up its floor and its corners' collision only
	// around Mario (TBathtub::setupCollisions_), and the corner he is at
	// moves its pieces as it breaks: around another player, the host's own
	// corner and floor had none, and the host fell through them
	if (MpBossBuildsAroundMario(a))
		return;
	int best    = -1;
	f32 nearest = distance2(a->mPosition, *gpMarioPos);
	for (int slot = 0; slot < MPG_MAX_PLAYERS; slot++)
		if (watchable(slot)) {
			const f32 d = distance2(a->mPosition, aimOf(slot));
			if (d < nearest)
				nearest = d, best = slot;
		}
	if (best < 0)
		return; // the host is nearest: as ever
	s_swapCount[best]++;
	MoveSwap& w = s_swaps[s_swapDepth++];
	w.object    = object;
	w.was       = gpMarioPos;
	w.unclipped = false;
	gpMarioPos  = const_cast<JGeometry::TVec3<f32>*>(&aimOf(best));
	if (a->checkLiveFlag(LIVE_FLAG_CLIPPED_OUT) && nearest < kNearOnScreen * kNearOnScreen) {
		a->offLiveFlag(LIVE_FLAG_CLIPPED_OUT);
		w.unclipped = true;
	}
}

void updateNearest()
{
	const int fps = port_active_frame_rate > 0 ? port_active_frame_rate : 30;
	if (s_clock % (10 * fps) == 0) {
		if (getenv("SMS_MULTIPLAYER_DEBUG"))
			for (int slot = 0; slot < MPG_MAX_PLAYERS; slot++)
				if (s_swapCount[slot])
					fprintf(stderr, "[multiplayer] nearest: %d object moves led by %s lately\n", s_swapCount[slot],
					        mpg_character_name(slot));
		memset(s_swapCount, 0, sizeof s_swapCount);
	}
	s_nearestOn = false;
	if (s_role != MPG_ROLE_HOST || !inStage() || s_swapDepth)
		return;
	updateAims();
	for (int slot = 0; slot < MPG_MAX_PLAYERS; slot++)
		if (watchable(slot))
			s_nearestOn = true;
}

// ------------------------------------------------------------------- shared objects

// What players do to NPCs and map objects (spray a goop-covered Pianta,
// ground-pound a box, stomp a switch) happens in every game: each game sends
// the hits its own player lands (objectHit, from the game's hit points), and
// the others land them on their copies. Objects are named by their manager's
// place in the stage's list and their place in it (every game loads a stage
// the same way). Enemies, bosses and items have their own sharing and are
// left out.
// object -> manager << 16 | index, in an open-addressed table (0: empty)
const int kSharedSlots = 8192;
struct SharedEntry {
	const void* object;
	uint32_t where;
};
SharedEntry s_shared[kSharedSlots];
TLiveManager* s_sharedManagers[256];
int s_sharedCount;
// the stage's lone actors (not in a manager: some NPCs, a statue, the
// plaza's Shadow Mario), as manager kAlone
const int kAlone = 255;
TLiveActor* s_alone[512];
int s_aloneCount;
bool s_landingHit;

uint32_t sharedSlot(const void* object) { return (uint32_t)(((uintptr_t)object >> 4) * 2654435761u) & (kSharedSlots - 1); }

void shareObject(const void* object, uint32_t where)
{
	for (uint32_t i = sharedSlot(object), n = 0; n < kSharedSlots / 2; i = (i + 1) & (kSharedSlots - 1), n++)
		if (!s_shared[i].object || s_shared[i].object == object) {
			s_shared[i].object = object;
			s_shared[i].where  = where;
			return;
		}
}

// Where `object` is in the shared lists; false if it is not shared.
bool sharedWhere(const void* object, uint32_t* where)
{
	for (uint32_t i = sharedSlot(object), n = 0; n < kSharedSlots / 2; i = (i + 1) & (kSharedSlots - 1), n++) {
		if (!s_shared[i].object)
			return false;
		if (s_shared[i].object == object) {
			*where = s_shared[i].where;
			return true;
		}
	}
	return false;
}

void collectShared()
{
	memset(s_shared, 0, sizeof s_shared);
	s_sharedCount = 0;
	s_aloneCount  = 0;
	if (!gpConductor)
		return;
	for (JGadget::TList<TLiveActor*>::iterator it = gpConductor->unk30.begin();
	     it != gpConductor->unk30.end() && s_aloneCount < 512; ++it) {
		TLiveActor* a       = *it;
		const int i         = s_aloneCount++;
		s_alone[i]          = nullptr;
		if (!a || dynamic_cast<TSmallEnemy*>(a) || MpBossIsBoss(a) || dynamic_cast<TItem*>(a))
			continue; // enemies, bosses and items share their own way
		s_alone[i] = a;
		shareObject(a, (uint32_t)kAlone << 16 | (uint32_t)i);
	}
	for (JGadget::TList<TLiveManager*>::iterator it = gpConductor->unk10.begin();
	     it != gpConductor->unk10.end() && s_sharedCount < kAlone; ++it) {
		TLiveManager* mgr = *it;
		const int m       = s_sharedCount++;
		s_sharedManagers[m] = nullptr;
		// enemies (the host's run them), bosses (mp_boss.cpp) and items share
		// their own way; NPCs and animals sit in enemy managers too, and are
		// shared here, as are the item manager's objects that are not items
		// (fruit: carried, thrown, sprayed)
		TLiveActor* first = mgr && mgr->getObjNum() ? mgr->getObj(0) : nullptr;
		const bool items  = dynamic_cast<TItemManager*>(mgr) != nullptr;
		const bool skip   = !mgr || (!items && (dynamic_cast<TSmallEnemy*>(first) || MpBossIsBoss(first)));
		if (getenv("SMS_MULTIPLAYER_DEBUG") && mgr)
			fprintf(stderr, "[multiplayer] objects: manager %d %s, %d of %s%s\n", m, typeid(*mgr).name(), mgr->getObjNum(),
			        mgr->getObjNum() && mgr->getObj(0) ? typeid(*mgr->getObj(0)).name() : "-", skip ? " (not shared here)" : "");
		if (skip)
			continue;
		s_sharedManagers[m] = mgr;
		for (int i = 0; i < mgr->getObjNum() && i < 0x10000; i++)
			if (TLiveActor* a = mgr->getObj(i))
				if (!items || !dynamic_cast<TItem*>(a))
					shareObject(a, (uint32_t)m << 16 | (uint32_t)i);
	}
}

THitActor* sharedAt(int manager, int index)
{
	if (manager == kAlone)
		return index >= 0 && index < s_aloneCount ? s_alone[index] : nullptr;
	if (manager < 0 || manager >= s_sharedCount || !s_sharedManagers[manager])
		return nullptr;
	TLiveManager* mgr = s_sharedManagers[manager];
	TLiveActor* a     = index >= 0 && index < mgr->getObjNum() ? mgr->getObj(index) : nullptr;
	uint32_t where;
	return a && sharedWhere(a, &where) ? a : nullptr; // not the item manager's items
}

bool sharedHit(unsigned message)
{
	return message == HIT_MESSAGE_SPRAYED_BY_WATER || message == HIT_MESSAGE_HIP_DROP
	       || message == HIT_MESSAGE_SUPER_HIP_DROP || message == HIT_MESSAGE_PUNCH || message == HIT_MESSAGE_TRAMPLE
	       || message == HIT_MESSAGE_PUSH_UP;
}

int objectHit(void* object, void* sender, unsigned message)
{
	if (!enabled() || !inStage() || s_landingHit || !gpModelWaterManager)
		return 0;
	uint32_t where;
	if (!sharedWhere(object, &where))
		return 0;
	TWaterHitActor& waterHit = TModelWaterManager::mStaticHitActor;
	const bool water         = sender == &waterHit;
	if (water && waterHit.mParticleIndex >= 0 && waterHit.mParticleIndex < TModelWaterManager::SLOT_NUM
	    && (gpModelWaterManager->getParticleFlagSOA()[waterHit.mParticleIndex] & kDrawnWater))
		return 1; // another player's water, drawn here: their game sends what it hits
	if ((!water && sender != gpMarioOriginal) || !sharedHit(message))
		return 0;
	MpMirrorUsed(object); // this game runs it a while (a slot machine sprayed here)
	const THitActor* from = static_cast<THitActor*>(sender);
	mpg_hit((int)(where >> 16), (int)(where & 0xFFFF), (int)message, water, &from->mPosition.x);
	return 0;
}

// Test aid (npcspray@N:K): this game's water on the NPC nearest its Mario,
// through the game's own hit point (so it is sent like any spray).
void testSprayNpc(bool first)
{
	TBaseNPC* best = nullptr;
	f32 nearest    = 0.0f;
	uint32_t where = 0;
	for (int m = 0; m < s_sharedCount; m++)
		for (int i = 0; s_sharedManagers[m] && i < s_sharedManagers[m]->getObjNum(); i++)
			if (TBaseNPC* npc = dynamic_cast<TBaseNPC*>(s_sharedManagers[m]->getObj(i))) {
				const f32 d = distance2(npc->mPosition, gpMarioOriginal->mPosition);
				if (!best || d < nearest)
					best = npc, nearest = d, where = (uint32_t)m << 16 | (uint32_t)i;
			}
	if (!best)
		return;
	TWaterHitActor& water = TModelWaterManager::mStaticHitActor;
	water.mPosition       = best->mPosition;
	water.mParticleIndex  = -1; // not a drawn drop: this game's own
	if (first)
		fprintf(stderr, "[multiplayer] test: spraying NPC %u.%u\n", (unsigned)(where >> 16), (unsigned)(where & 0xFFFF));
	if (!objectHit(best, &water, HIT_MESSAGE_SPRAYED_BY_WATER))
		best->receiveMessage(&water, HIT_MESSAGE_SPRAYED_BY_WATER);
}

// Test aid (slotspray@N): this game's water on the first item slot machine,
// through the hit hook (so it is sent like any spray, and runs it here).
bool testSpraySlot()
{
	for (int m = 0; m < s_sharedCount; m++)
		for (int i = 0; s_sharedManagers[m] && i < s_sharedManagers[m]->getObjNum(); i++)
			if (TItemSlotDrum* slot = dynamic_cast<TItemSlotDrum*>(s_sharedManagers[m]->getObj(i))) {
				TWaterHitActor& water = TModelWaterManager::mStaticHitActor;
				water.mPosition       = slot->mPosition;
				water.mParticleIndex  = -1; // not a drawn drop: this game's own
				fprintf(stderr, "[multiplayer] test: spraying item slot machine %d.%d\n", m, i);
				if (!objectHit(slot, &water, HIT_MESSAGE_SPRAYED_BY_WATER))
					slot->receiveMessage(&water, HIT_MESSAGE_SPRAYED_BY_WATER);
				fprintf(stderr, "[multiplayer] test: its reels now %.1f %.1f %.1f (ready %d, won %d)\n",
				        slot->unk138 ? slot->unk138[0] : -1.0f, slot->unk138 ? slot->unk138[1] : -1.0f,
				        slot->unk138 ? slot->unk138[2] : -1.0f, (int)slot->unk1A2, (int)slot->unk194);
				return true;
			}
	return false;
}

// Test aid (pound@N): this game's Mario ground-pounds Bowser's tub, through
// the hit hook as his landing does (patch zzzzz-ext-06).
bool testPoundTub()
{
	// when Bowser can stagger (standing or turning: not breathing fire, as
	// TKoopa::stagger leaves those be)
	TKoopa* koopa = JDrama::TNameRefGen::search<TKoopa>("\x83\x4e\x83\x62\x83\x70");
	if (!koopa)
		return false;
	const void* n = koopa->getSpine()->getCurrentNerve();
	if (n != &TNerveKoopaWait::theNerve() && n != &TNerveKoopaTurnL::theNerve() && n != &TNerveKoopaTurnR::theNerve())
		return false;
	for (int m = 0; m < s_sharedCount; m++)
		for (int i = 0; s_sharedManagers[m] && i < s_sharedManagers[m]->getObjNum(); i++)
			if (TBathtub* tub = dynamic_cast<TBathtub*>(s_sharedManagers[m]->getObj(i))) {
				fprintf(stderr, "[multiplayer] test: ground-pounding the tub (%d.%d)\n", m, i);
				if (!objectHit(tub, gpMarioOriginal, HIT_MESSAGE_HIP_DROP))
					tub->receiveMessage(gpMarioOriginal, HIT_MESSAGE_HIP_DROP);
				return true;
			}
	return false;
}

// The flag of the blue coin a hiding place hides (a graffiti, a box), or 0.
uint32_t hiddenBlueCoinFlag(THideObjBase* hide)
{
	if (!hide->mHiddenObj || !hide->mHiddenObj->isActorType(0x20000010))
		return 0;
	const u8 world = SMS_getShineStage(gpApplication.mCurrArea.getStage());
	const u8 w     = world == 0 || world >= 10 ? 1 : world;
	const u32 id   = hide->getEventId();
	return 0x10078 + (w - 1) * 50 + (id < 50 ? id : 0);
}

// After the flags were shared: a graffiti (or a box) hiding a blue coin
// someone else got is as the game loads it once the coin is taken (gone,
// TWaterHitPictureHideObj::loadAfter; or nothing comes out, THideObjBase's),
// not left to clean for nothing.
void hideTakenBlueCoins(const int32_t* before)
{
	for (int k = 0; k < kSharedSlots; k++) {
		THideObjBase* hide = s_shared[k].object ? dynamic_cast<THideObjBase*>((TLiveActor*)s_shared[k].object) : nullptr;
		const uint32_t flag = hide ? hiddenBlueCoinFlag(hide) : 0;
		const int idx       = (int)flag - 0x10000;
		if (!flag || idx < 0 || idx >= s_flagCount || before[idx] || !s_flags[idx])
			continue;
		hide->mAllowReveal = false;
		TWaterHitPictureHideObj* graffiti = dynamic_cast<TWaterHitPictureHideObj*>(hide);
		if (!graffiti || graffiti->isState(TWaterHitPictureHideObj::STATE_FINISHED))
			continue;
		graffiti->makeObjDead();
		if (THideObjPictureTwin* twin = dynamic_cast<THideObjPictureTwin*>(graffiti))
			if (twin->unk174 && !twin->unk174->isState(TWaterHitPictureHideObj::STATE_FINISHED))
				twin->unk174->makeObjDead();
		fprintf(stderr, "[multiplayer] someone else's blue coin %05x: its graffiti here is gone\n", (unsigned)flag);
	}
}

// Test aid (shineout@N): a Shine a hiding place (a graffiti, a box) hides,
// still hidden, and its camera (named for the hiding place).
TShine* testHiddenShine(const char** camera)
{
	for (int k = 0; k < kSharedSlots; k++) {
		THideObjBase* hide = s_shared[k].object ? dynamic_cast<THideObjBase*>((TLiveActor*)s_shared[k].object) : nullptr;
		TShine* sh         = hide ? dynamic_cast<TShine*>(hide->mHiddenObj) : nullptr;
		if (sh && hide->mHiddenShineDemoName && sh->checkLiveFlag(LIVE_FLAG_DEAD)
		    && JDrama::TNameRefGen::search<TCameraMapTool>(hide->mHiddenShineDemoName)) {
			*camera = hide->mHiddenShineDemoName;
			return sh;
		}
	}
	return nullptr;
}

// The first graffiti hiding a blue coin (test aids).
TWaterHitPictureHideObj* firstBlueGraffiti()
{
	for (int m = 0; m < s_sharedCount; m++)
		for (int i = 0; s_sharedManagers[m] && i < s_sharedManagers[m]->getObjNum(); i++)
			if (TWaterHitPictureHideObj* g = dynamic_cast<TWaterHitPictureHideObj*>(s_sharedManagers[m]->getObj(i)))
				if (hiddenBlueCoinFlag(g))
					return g;
	return nullptr;
}

// Test aid (graffiti@N): this game's water on that graffiti, a drop a frame
// through the hit hook (sent as any spray is), facing it, till it is clean.
void testSprayGraffiti(int frame, bool quiet)
{
	TWaterHitPictureHideObj* g = firstBlueGraffiti();
	if (!g || g->checkLiveFlag(LIVE_FLAG_DEAD) || g->isState(TWaterHitPictureHideObj::STATE_FINISHED))
		return;
	if (frame == 0)
		fprintf(stderr, "[multiplayer] test: spraying the graffiti at %.0f %.0f %.0f\n", g->mPosition.x, g->mPosition.y,
		        g->mPosition.z);
	TWaterHitActor& water = TModelWaterManager::mStaticHitActor;
	water.mPosition       = g->mPosition;
	water.mParticleIndex  = -1; // not a drawn drop: this game's own
	for (int i = 0; i < 6; i++) // a few drops a frame, as a stream is
		if (quiet || !objectHit(g, &water, HIT_MESSAGE_SPRAYED_BY_WATER))
			g->forward(g->mSprayProgressSpeed); // as touchWater does for water coming at its face
}

// Test aid: that graffiti's state here, when it changes.
void watchGraffiti()
{
	static f32 lastProgress = -1.0f;
	static int lastState = -1, lastDead = -1;
	TWaterHitPictureHideObj* g = firstBlueGraffiti();
	if (!g)
		return;
	const int dead = g->checkLiveFlag(LIVE_FLAG_DEAD) ? 1 : 0;
	if (g->mProgress == lastProgress && (int)g->mState == lastState && dead == lastDead)
		return;
	lastProgress = g->mProgress, lastState = (int)g->mState, lastDead = dead;
	TMapObjBase* coin = g->mHiddenObj;
	fprintf(stderr, "[multiplayer] test: graffiti progress %.1f, state %d, gone %d; its coin %s\n", g->mProgress, lastState,
	        dead, coin ? (coin->checkLiveFlag(LIVE_FLAG_DEAD) ? "hidden or taken" : "out") : "-");
}

// Graffiti erased (sprayed till clean: TWaterHitPictureHideObj's
// STATE_FINISHED) in any game are erased in every game, and back in every
// game when they come back (what one hid, a coin nobody took, went back in
// it: TItem::appeared). Each game numbers each graffiti's changes, odd
// erased, even back, and reports its own in the session's list of taken
// items (a key of their own: kErasedKey | number << 24 | where), which the
// host keeps and gives again to a player who joins or loads the stage
// again: the newest number is how the graffiti is.
struct Graffiti {
	TWaterHitPictureHideObj* g;
	uint32_t where;
	u8 changes;  // its number of changes here, as above
	bool erased; // as last seen here
};
Graffiti s_graffiti[128];
int s_graffitiCount;
const int kGraffitiChanges = 127;

bool graffitiErased(TWaterHitPictureHideObj* g)
{
	return g->isState(TWaterHitPictureHideObj::STATE_FINISHED) && !g->checkLiveFlag(LIVE_FLAG_DEAD);
}

void collectGraffiti()
{
	s_graffitiCount = 0;
	for (int k = 0; k < kSharedSlots && s_graffitiCount < 128; k++)
		if (TWaterHitPictureHideObj* g
		    = s_shared[k].object ? dynamic_cast<TWaterHitPictureHideObj*>((TLiveActor*)s_shared[k].object) : nullptr) {
			Graffiti& e = s_graffiti[s_graffitiCount++];
			e.g         = g;
			e.where     = s_shared[k].where;
			e.erased    = graffitiErased(g);
			e.changes   = e.erased ? 1 : 0;
		}
}

// Each tick: a graffiti erased here, or back, just now is told to the others.
void reportGraffiti()
{
	for (int i = 0; i < s_graffitiCount; i++) {
		Graffiti& e       = s_graffiti[i];
		const bool erased = graffitiErased(e.g);
		if (erased == e.erased)
			continue;
		e.erased = erased;
		if (e.changes >= kGraffitiChanges - 1)
			continue;
		e.changes++;
		if ((e.changes & 1) != (erased ? 1 : 0)) // the odd ones erased
			e.changes++;
		mpg_item_taken(kErasedKey | (uint32_t)e.changes << 24 | e.where);
		if (getenv("SMS_MULTIPLAYER_DEBUG"))
			fprintf(stderr, "[multiplayer] graffiti %u.%u %s here (change %d): everyone's is\n", (unsigned)(e.where >> 16),
			        (unsigned)(e.where & 0xFFFF), erased ? "erased" : "back", e.changes);
	}
}

// Another game's graffiti change (or the newest of this session's so far,
// arriving): erased, it is as its own finishing leaves it, what it hid out;
// back, as the game brings it back when that goes back in.
void changeGraffiti(uint32_t where, int changes)
{
	for (int i = 0; i < s_graffitiCount; i++) {
		Graffiti& e = s_graffiti[i];
		if (e.where != where)
			continue;
		if (changes <= e.changes)
			return;
		e.changes                  = (u8)changes;
		const bool erased          = (changes & 1) != 0;
		TWaterHitPictureHideObj* g = e.g;
		if (g->checkLiveFlag(LIVE_FLAG_DEAD) || graffitiErased(g) == erased) {
			e.erased = graffitiErased(g);
			return;
		}
		if (erased) {
			g->mProgress = g->mReverse ? g->mMinProgress : g->mMaxProgress;
			g->mColor.a  = (u16)g->mProgress & 0xff;
			g->afterFinishedAnim();
		} else {
			// as TWaterHitPictureHideObj::receiveMessage does as its coin goes back in
			g->offHitFlag(HIT_FLAG_NO_COLLISION);
			g->offHitFlag(HIT_FLAG_CANNOT_GET_HIT);
			g->offHitFlag(HIT_FLAG_CANNOT_ATTACK);
			g->mState       = TMapObjBase::STATE_NORMAL;
			g->mAllowReveal = true;
		}
		e.erased = graffitiErased(g);
		if (getenv("SMS_MULTIPLAYER_DEBUG"))
			fprintf(stderr, "[multiplayer] graffiti %u.%u %s elsewhere (change %d): here too\n", (unsigned)(where >> 16),
			        (unsigned)(where & 0xFFFF), erased ? "erased" : "back", changes);
		return;
	}
}

// Another player's water on a graffiti here: cleaned as much as their game
// cleaned theirs. Their drops' speed is not known here (touchWater keeps only
// water coming at its face, by the drop's speed), and their game sends only
// the hits it took.
bool landWaterOnGraffiti(THitActor* to, int count)
{
	TWaterHitPictureHideObj* graffiti = dynamic_cast<TWaterHitPictureHideObj*>(to);
	if (!graffiti || graffiti->checkLiveFlag(LIVE_FLAG_DEAD))
		return graffiti != nullptr;
	for (int i = 0; i < count && i < 30 && !graffiti->isState(TWaterHitPictureHideObj::STATE_FINISHED); i++)
		graffiti->forward(graffiti->mSprayProgressSpeed);
	return true;
}

// The others' hits on shared objects, landed here as theirs were.
void landHits()
{
	int slot, manager, index, message, water, count;
	float pos[3];
	while (mpg_next_hit(&slot, &manager, &index, &message, &water, &count, pos)) {
		THitActor* to = sharedAt(manager, index);
		if (!to || MpMirrorKeepsHits(to))
			continue; // its owner runs it, and sends how it went
		if (water && message == HIT_MESSAGE_SPRAYED_BY_WATER && landWaterOnGraffiti(to, count))
			continue;
		THitActor* from = nullptr;
		if (water) {
			TWaterHitActor& waterHit = TModelWaterManager::mStaticHitActor;
			waterHit.mPosition.set(pos[0], pos[1], pos[2]);
			waterHit.mParticleIndex = 0;
			from = &waterHit;
		} else if (slot >= 0 && slot < MPG_MAX_PLAYERS && s_puppets[slot] && s_puppets[slot]->active()) {
			from = s_puppets[slot];
		}
		if (!from)
			continue;
		// a stomp or a ground pound counts as the player's when it comes from
		// Mario's kind of actor: the other player's character is, while it lands
		const int type = from->mActorType;
		if (!water)
			from->mActorType = 0x80000001;
		s_landingHit = true;
		for (int i = 0; i < count && i < 30; i++)
			to->receiveMessage(from, (u32)message);
		s_landingHit = false;
		from->mActorType = type;
		static int landed;
		if (getenv("SMS_MULTIPLAYER_DEBUG") && landed++ % 50 == 0)
			fprintf(stderr, "[multiplayer] hits: %s's %d (message %d) landed on %d.%d here\n", mpg_character_name(slot), count,
			        message, manager, index);
	}
}

// ------------------------------------------------------------------- carried objects

// Another player's hands, here: an object they carry is held by it, and
// follows it (in front of their character, chest high), as one follows
// Mario's hands. Nobody here can take it from them meanwhile.
class TMpHolder : public TTakeActor {
public:
	TMpHolder() : TTakeActor("<TMpHolder>") { MTXIdentity(mMtx); }
	virtual MtxPtr getTakingMtx() { return mMtx; }
	Mtx mMtx;
};
union HolderStore {
	char bytes[sizeof(TMpHolder)];
	double align;
};
HolderStore s_holderStore[MPG_MAX_PLAYERS]; // made once, kept: not on a stage's heap
TMpHolder* s_holders[MPG_MAX_PLAYERS];
TTakeActor* s_myHeld; // what this game's Mario held last tick

TMpHolder* holderFor(int slot)
{
	if (!s_holders[slot])
		s_holders[slot] = new (&s_holderStore[slot]) TMpHolder();
	return s_holders[slot];
}

void placeHolder(int slot)
{
	TMpHolder* h = s_holders[slot];
	TMpPuppet* p = s_puppets[slot];
	if (!h || !p)
		return;
	const f32 yaw = p->mRotation.y * (3.14159265f / 180.0f);
	const f32 x = p->mPosition.x + sinf(yaw) * 50.0f, y = p->mPosition.y + 100.0f, z = p->mPosition.z + cosf(yaw) * 50.0f;
	MsMtxSetXYZRPH(h->mMtx, x, y, z, 0, (s16)(p->mRotation.y * (65536.0f / 360.0f)), 0);
	h->mPosition.set(x, y, z);
}

// Each tick: what other players carry follows them; one who left drops it.
void tickHolders()
{
	for (int slot = 0; slot < MPG_MAX_PLAYERS; slot++) {
		TMpHolder* h = s_holders[slot];
		if (!h || !h->mHeldObject)
			continue;
		if (!watchable(slot)) {
			h->mHeldObject->mHolder = nullptr; // the object goes back to lying where it is
			h->mHeldObject          = nullptr;
			continue;
		}
		placeHolder(slot);
	}
}

// This game's Mario picked something up, put it down or threw it: the
// others do the same with theirs (where it ends up, and how it moves).
void watchMyCarry()
{
	TTakeActor* now = gpMarioOriginal ? gpMarioOriginal->mHeldObject : nullptr;
	if (now == s_myHeld)
		return;
	const int yaw = gpMarioOriginal ? gpMarioOriginal->mFaceAngle.y : 0;
	uint32_t where;
	if (s_myHeld && sharedWhere(s_myHeld, &where)) {
		TLiveActor* a       = static_cast<TLiveActor*>(s_myHeld);
		// thrown: on its way sideways (put down, or dropped, it only falls)
		const bool thrown = a->mVelocity.x * a->mVelocity.x + a->mVelocity.z * a->mVelocity.z > 1.0f;
		mpg_carry((int)(where >> 16), (int)(where & 0xFFFF), thrown ? 2 : 1, &a->mPosition.x, &a->mVelocity.x, yaw);
	}
	if (now && sharedWhere(now, &where)) {
		const float still[3] = {0.0f, 0.0f, 0.0f};
		mpg_carry((int)(where >> 16), (int)(where & 0xFFFF), 0, &now->mPosition.x, still, yaw);
	}
	if (getenv("SMS_MULTIPLAYER_DEBUG"))
		fprintf(stderr, "[multiplayer] carry: this player holds %s (was %s)\n", now ? typeid(*now).name() : "nothing",
		        s_myHeld ? typeid(*s_myHeld).name() : "nothing");
	s_myHeld = now;
}

// Test aid (carry@N:K): this game's Mario takes the nearest shared thing
// that can be carried, then puts it down (as the game's own take and put do).
TLiveActor* s_testCarried;

void testCarry(bool take)
{
	TMario* m = gpMarioOriginal;
	if (!m)
		return;
	if (!take) {
		if (s_testCarried && m->mHeldObject == s_testCarried) {
			s_testCarried->receiveMessage(m, HIT_MESSAGE_PUT);
			m->mHeldObject = nullptr;
			fprintf(stderr, "[multiplayer] test: put it down\n");
		}
		return;
	}
	TMapObjBase* best = nullptr;
	f32 nearest       = 0.0f;
	int takeable      = 0;
	for (int k = 0; k < kSharedSlots; k++) {
		TMapObjBase* o = s_shared[k].object ? dynamic_cast<TMapObjBase*>((TLiveActor*)s_shared[k].object) : nullptr;
		if (!o || !o->checkMapObjFlag(TMapObjBase::MAP_OBJ_FLAG_UNK100000))
			continue;
		takeable++;
		const f32 d = distance2(o->mPosition, m->mPosition);
		if (!best || d < nearest)
			best = o, nearest = d;
	}
	fprintf(stderr, "[multiplayer] test: %d things here can be carried\n", takeable);
	if (best && best->receiveMessage(m, HIT_MESSAGE_TAKE)) {
		m->mHeldObject  = best;
		s_testCarried   = best;
		watchMyCarry(); // at once: this Mario is not really holding, and lets go next tick
		uint32_t where  = 0;
		sharedWhere(best, &where);
		fprintf(stderr, "[multiplayer] test: picked up %u.%u\n", (unsigned)(where >> 16), (unsigned)(where & 0xFFFF));
	}
}

// Test aid (nearfruit@N): this game's Mario stands facing the nearest fruit,
// and presses B (picks it up, as the player does).
void testNearFruit(int index)
{
	TMario* m = gpMarioOriginal;
	if (!m)
		return;
	TResetFruit* best = nullptr;
	f32 nearest       = 0.0f;
	int n             = 0;
	for (JGadget::TList<TLiveManager*>::iterator it = gpConductor->unk10.begin(); it != gpConductor->unk10.end(); ++it)
		for (int i = 0; *it && i < (*it)->getObjNum(); i++) {
			TResetFruit* f = dynamic_cast<TResetFruit*>((*it)->getObj(i));
			if (!f)
				continue;
			uint32_t at = 0;
			const bool shared = sharedWhere(f, &at);
			const int k       = n++;
			fprintf(stderr, "[multiplayer] test: fruit %d: %s in %s (%s), dead %d, at %.0f %.0f %.0f\n", k, typeid(*f).name(),
			        typeid(**it).name(), shared ? "shared" : "not shared", (int)f->checkLiveFlag(LIVE_FLAG_DEAD),
			        f->mPosition.x, f->mPosition.y, f->mPosition.z);
			if (f->checkLiveFlag(LIVE_FLAG_DEAD) || (index >= 0 && k != index))
				continue;
			const f32 d = distance2(f->mPosition, m->mPosition);
			if (!best || d < nearest)
				best = f, nearest = d;
		}
	s_testFruit = best;
	if (!best) {
		fprintf(stderr, "[multiplayer] test: no fruit here\n");
		return;
	}
	uint32_t where = 0;
	sharedWhere(best, &where);
	const f32 yaw = 0.0f;
	m->mPosition.set(best->mPosition.x - sinf(yaw) * 150.0f, best->mPosition.y + 10.0f, best->mPosition.z - cosf(yaw) * 150.0f);
	m->mFaceAngle.y = 0;
	m->mForwardVel  = 0.0f;
	fprintf(stderr, "[multiplayer] test: by the fruit %s %u.%u at %.0f %.0f %.0f\n", typeid(*best).name(), (unsigned)(where >> 16),
	        (unsigned)(where & 0xFFFF), best->mPosition.x, best->mPosition.y, best->mPosition.z);
}

// What the other players picked up, put down and threw, done here.
void landCarries()
{
	int slot, manager, index, action, yaw;
	float pos[3], vel[3];
	while (mpg_next_carry(&slot, &manager, &index, &action, pos, vel, &yaw)) {
		TLiveActor* obj = dynamic_cast<TLiveActor*>(sharedAt(manager, index));
		if (getenv("SMS_MULTIPLAYER_DEBUG"))
			fprintf(stderr, "[multiplayer] carry: from slot %d: %d.%d action %d (%s here)\n", slot, manager, index, action,
			        obj ? typeid(*obj).name() : "nothing");
		if (!obj || slot < 0 || slot >= MPG_MAX_PLAYERS || slot == mpg_slot())
			continue;
		TMpHolder* h = holderFor(slot);
		placeHolder(slot);
		if (action == 0) {
			if (obj->mHolder && obj->mHolder != h)
				continue; // held here already (both took it at once): it stays with ours
			if (obj->receiveMessage(h, HIT_MESSAGE_TAKE))
				h->mHeldObject = obj;
			if (getenv("SMS_MULTIPLAYER_DEBUG"))
				fprintf(stderr, "[multiplayer] carry: %s picked up %d.%d (%s here)\n", mpg_character_name(slot), manager,
				        index, h->mHeldObject == obj ? "held" : "not held");
			continue;
		}
		if (obj->mHolder == h)
			obj->receiveMessage(h, action == 2 ? HIT_MESSAGE_THROWN : HIT_MESSAGE_PUT);
		if (obj->mHolder == h)
			obj->mHolder = nullptr;
		if (h->mHeldObject == obj)
			h->mHeldObject = nullptr;
		// where it is and how it moves, as in their game
		obj->mPosition.set(pos[0], pos[1], pos[2]);
		obj->mVelocity.set(vel[0], vel[1], vel[2]);
		if (getenv("SMS_MULTIPLAYER_DEBUG"))
			fprintf(stderr, "[multiplayer] carry: %s %s %d.%d\n", mpg_character_name(slot), action == 2 ? "threw" : "put down",
			        manager, index);
	}
}

// ------------------------------------------------------------------- hooks


void takeTrips(TMarDirector* dir); // below
unsigned g_moves; // debug: movement passes so far

// Our own Mario (when we play Mario, no puppet draws us) in a look a server
// mod gave us: painted as the puppets are; our own look again, the
// launcher's texture pack (the paint undone). Again when looks change and as
// each stage is set up (mpg_undress_all forgot the paint).
uint32_t s_ownDressed = ~0u;
void dressSelf()
{
	TMario* m = gpMarioOriginal;
	if (!m || s_self || mpg_looks_version() == s_ownDressed)
		return;
	s_ownDressed       = mpg_looks_version();
	const bool forced  = mpg_own_look_forced() != 0;
	J3DModel* fludd    = m->mWaterGun && m->mWaterGun->mFluddModel ? m->mWaterGun->mFluddModel->getModel() : nullptr;
	J3DModelData* const models[2] = {m->mBodyModelData, fludd ? fludd->getModelData() : nullptr};
	for (J3DModelData* model : models) {
		J3DTexture* textures = model ? model->getTexture() : nullptr;
		JUTNameTab* names    = model ? model->getTextureName() : nullptr;
		if (!textures || !names)
			continue;
		for (u16 i = 0; i < textures->getNum(); i++) {
			ResTIMG* timg    = textures->getResTIMG(i);
			const char* name = names->getName(i);
			if (!timg || !name)
				continue;
			const void* data = (const u8*)timg + timg->imageDataOffset;
			const u32 bytes  = GXGetTexBufferSize(timg->width, timg->height, timg->format, GX_FALSE, 0);
			if (forced)
				mpg_dress(mpg_slot(), MPG_CHAR_MARIO, name, data, bytes);
			else
				mpg_undress(data, bytes);
		}
	}
}

void frame()
{
	if (s_role < 0)
		s_role = mpg_start();
	if (!enabled())
		return;
	s_clock++;
	dressSelf();
	if (getenv("SMS_MULTIPLAYER_SPEEDLOG") && s_clock % 600 == 0) {
		static unsigned lastMs, lastMoves;
		const unsigned ms = mp_clock_ms_game();
		static unsigned lastRetrace;
		const unsigned retrace = VIGetRetraceCount();
		fprintf(stderr, "[speed] clock: 600 frames in %u ms, %u movement passes, %u retraces, game frame rate %d\n",
		        ms - lastMs, g_moves - lastMoves, retrace - lastRetrace, port_active_frame_rate);
		lastRetrace = retrace;
		lastMs    = ms;
		lastMoves = g_moves;
	}
	updateNearest();
	TMarDirector* dir = director();
	const int area    = gpApplication.mCurrArea.getStage();
	MpgGame g;
	g.playing = playing();
	g.area    = area;
	g.episode = gpApplication.mCurrArea.getScenario();
	g.world   = SMS_getShineStage((u8)area);
	g.paused  = dir && dir->mState == TMarDirector::STATE_PAUSE_MENU;
	g.frameMs = 1000.0f / (port_active_frame_rate > 0 ? port_active_frame_rate : 30);
	g.loads   = (unsigned)s_stageNumber;
	mpg_frame(&g);

	s_wasPlaying = s_playing;
	s_playing    = g.playing != 0;
	// arriving: playing a stage just loaded. Playing again after a pause, a
	// save prompt (a blue coin's) or the map is not: nothing of the stage's is
	// gathered again, and nobody is moved (a joining player was put beside
	// where they stood each time, as on arriving)
	static unsigned arrivedStage;
	s_arrivedNow = s_playing && arrivedStage != s_stageNumber;
	if (s_playing)
		arrivedStage = s_stageNumber;
	if (s_arrivedNow) {
		s_arrivedHere = true;
		snapshotItems();
		collectManagers();
		collectShared();
		collectGraffiti();
		MpMirrorCollect();
		s_myHeld = nullptr;
		for (int slot = 0; slot < MPG_MAX_PLAYERS; slot++)
			if (s_holders[slot])
				s_holders[slot]->mHeldObject = nullptr; // last stage's objects are gone
		collectSwitches();
		s_stageFrames = 0;
		s_restarting = false;
		for (int slot = 0; slot < MPG_MAX_PLAYERS; slot++)
			s_poseSeenAt[slot] = s_clock; // each has a while to be seen here (hostCheckEveryoneOut)
		if (getenv("SMS_MULTIPLAYER_DEBUG"))
			fprintf(stderr, "[multiplayer] arrived in area %d (the secret courses' FLUDD movie seen: %d, last movie %d)\n",
			        area, (int)TFlagManager::getInstance()->getBool(0x3000D), (int)gpApplication.mMovie);
		if (s_allOutLives && s_role == MPG_ROLE_HOST) {
			s_allOutLives = false;
			TFlagManager::getInstance()->setFlag(0x20001, 3); // as a continue gives
			fprintf(stderr, "[multiplayer] everyone back in with 3 lives\n");
		}
		// a new start, or another stage (out of the world with the group, or
		// out of lives just as it left): alive again
		if (s_spectating && (mpg_session() != s_spectateSession || area != s_spectateArea)) {
			s_spectating = false;
			if (TMario* m = gpMarioOriginal) {
				m->offFlag(MARIO_FLAG_IS_PERFORMING);
				m->offHitFlag(HIT_FLAG_NO_COLLISION);
			}
			showNotice("Back in the game");
		}
		if (s_spectating)
			showWatching();
		else if (s_role == MPG_ROLE_CLIENT)
			spreadStart();
	}
	if (s_role == MPG_ROLE_CLIENT) {
		skipTitle(dir);
		leaveWithHost(dir);
		guestCardNotice();
		leadTrip(dir);
		clientShineExit(dir);
		if (s_shineHoldFrames > 0)
			s_shineHoldFrames--;
	}
	if (inStage()) {
		syncFlags();
		takeRemovals();
		syncTakenAt();
		if (s_playing)
			openGatesFromFlags();
		sendPose();
		if (s_role == MPG_ROLE_HOST) {
			// paused, the host's enemies stand still: the others' games would
			// hold theirs still too while they still bite, so send none
			if (g.paused)
				mpg_set_enemies(nullptr, 0);
			else {
				MpBossHostFrame(); // before the enemies: it rides in their snapshot
				hostEnemies();
			}
		} else
			reportEnemyDefeats();
		noticeOwnShineGet();
		if (s_playing) {
			playOthersShineGets();
			if (s_role == MPG_ROLE_CLIENT)
				followHost(dir);
			else
			{
				hostShineExit(dir);
				hostBlueSave(dir);
				takeTrips(dir);
			}
			runTests(dir, s_arrivedNow);
			MpMirrorSurvey(s_stageFrames);
			noticeCutscenes();
			playOthersCutscenes(dir);
			endReplayWithStarter(dir);
			s_stageFrames++;
			if (s_spectating) {
				spectateFrame(dir);
				if (!s_restarting)
					reviveWithOneUp();
			}
		}
		if (s_role == MPG_ROLE_HOST)
			hostCheckEveryoneOut(dir);
		static int lastState = -1;
		if (getenv("SMS_MULTIPLAYER_DEBUG") && dir->mState != lastState
		    && (dir->mState == TMarDirector::STATE_CARD_SAVE || lastState == TMarDirector::STATE_CARD_SAVE))
			fprintf(stderr, "[multiplayer] save prompt %s (frame %u)\n", dir->mState == TMarDirector::STATE_CARD_SAVE ? "open" : "closed",
			        s_clock);
		lastState = dir->mState;
	} else {
		MpgPose none;
		memset(&none, 0, sizeof none);
		if (s_spectating) {
			// out of lives and reloading the stage: still watching (the host
			// counts who is, hostCheckEveryoneOut)
			none.session = s_spectateSession;
			none.flags   = MPG_POSE_SPECTATING;
		}
		mpg_set_pose(&none);
	}
	selectFrame();
	MpServerFrame(dir, s_arrivedNow, s_playing && inStage());
	updateText();
}

void stageSetup()
{
	s_hostShineFrames = 0; // a Shine's exit belongs to the stage it was got in
	MpBossStageSetup();
	s_inWinDemo = s_winRemote = false;
	s_winCollector    = -1;
	s_winShine        = nullptr;
	s_shineGetFrames  = 0;
	MpSpawnStageSetup(); // the stage's records are in, multiplayer or not
	if (!enabled())
		return;
	MpServerStageSetup();
	s_ownDressed = ~0u; // a new Mario: painted again if he should be
	s_readyDir = gpMarDirector;
	s_arrivedHere = false;
	s_stageNumber++;
	// the last stage's objects are gone from here on: nothing of theirs is
	// touched again (the lists of this stage's are made once it plays, and
	// the game moves its objects before that, in its opening)
	s_managerCount = 0;
	s_sharedCount  = 0;
	s_aloneCount   = 0;
	memset(s_shared, 0, sizeof s_shared);
	s_graffitiCount = 0;
	s_switchCount = 0;
	s_gatesFound  = false;
	memset(s_gates, 0, sizeof s_gates);
	s_myHeld      = nullptr;
	for (int slot = 0; slot < MPG_MAX_PLAYERS; slot++)
		if (s_holders[slot])
			s_holders[slot]->mHeldObject = nullptr;
	MpMirrorForget();
	if (gpApplication.mCurrArea.getStage() == 15 || !gpStrategy)
		return;
	if (getenv("SMS_MULTIPLAYER_DEBUG"))
		fprintf(stderr, "[multiplayer] stage %d: %d bytes free in the stage's heap\n",
		        (int)gpApplication.mCurrArea.getStage(), (int)JKRHeap::getCurrentHeap()->getTotalFreeSize());
	MpMirrorPrepareRides();
	TIdxGroupObj* group = gpStrategy->unk10[6]; // "プレーヤーグループ": Mario's
	memset(s_puppets, 0, sizeof s_puppets);
	mpg_undress_all(); // the last stage's puppets' pictures, by their data's place in its memory
	if (!group)
		return;
	const int me = mpg_slot();
	s_self       = nullptr;
	for (int slot = 0; slot < MPG_MAX_PLAYERS; slot++) {
		// our own character too, unless we are Mario (the game draws him)
		const bool self = slot == me;
		if (self && mpg_character(slot) == MPG_CHAR_MARIO)
			continue;
		const s32 before   = JKRHeap::getCurrentHeap()->getTotalFreeSize();
		// a puppet takes about 450 KB of the stage's memory;
		// the port's stages have some 44 MB to spare (its arena is larger than
		// the console's), so this only guards against a stage that has not
		if (before < 1024 * 1024) {
			fprintf(stderr, "[multiplayer] not enough memory in this stage for %s (%d bytes free)\n",
			        mpg_character_name(slot), (int)before);
			continue;
		}
		TMpPuppet* puppet  = new TMpPuppet(slot, self);
		if (!puppet->init()) {
			fprintf(stderr, "[multiplayer] no room for %s in this stage\n", mpg_character_name(slot));
			continue;
		}
		THitActor* actor = puppet;
		group->add(actor);
		s_puppets[slot] = puppet;
		if (self)
			s_self = puppet;
		fprintf(stderr, "[multiplayer] %s's model ready (%d bytes)\n", mpg_character_name(slot),
		        (int)(before - JKRHeap::getCurrentHeap()->getTotalFreeSize()));
	}
}

// ------------------------------------------------------------------- goop

// Cleaning what another game cleaned (not this game's own to send).
bool s_cleaning;

// Goop cleaned here (FLUDD's water, Mario's feet, an event): the others clean it too.
void pollutionClean(float x, float y, float z, float size)
{
	if (s_cleaning || !enabled() || !inStage())
		return;
	mpg_clean(x, y, z, size);
}

// What the others cleaned, cleaned here: a few hundred a tick, so catching up
// on a whole stage's cleaning does not hold up a frame.
void applyCleans()
{
	if (!gpPollution)
		return;
	float x, y, z, size;
	int n = 0;
	for (; n < 400 && mpg_next_clean(&x, &y, &z, &size); n++) {
		s_cleaning = true;
		gpPollution->clean(x, y, z, size);
		s_cleaning = false;
	}
	static int cleaned, reported;
	cleaned += n;
	if (getenv("SMS_MULTIPLAYER_DEBUG") && cleaned >= reported + 100) {
		fprintf(stderr, "[multiplayer] goop: %d spots cleaned here that the others cleaned\n", cleaned);
		reported = cleaned;
	}
}

// Debug (SMS_MULTIPLAYER_DUMP=1): every actor of the stage, where it is and
// how it is, every two seconds on the wall clock, so two games on one
// computer can be laid side by side (tools/mp_desync.py) to find what each
// game runs its own way.
void dumpActors()
{
	static const bool on = getenv("SMS_MULTIPLAYER_DUMP") != nullptr;
	if (!on || !gpConductor)
		return;
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	const long long ms = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
	static long long last;
	if (ms / 2000 == last / 2000)
		return;
	last = ms;
	const long long at = ms / 2000 * 2000;
	int m = 0;
	for (JGadget::TList<TLiveManager*>::iterator it = gpConductor->unk10.begin(); it != gpConductor->unk10.end(); ++it, ++m)
		for (int i = 0; *it && i < (*it)->getObjNum(); i++)
			if (TLiveActor* a = (*it)->getObj(i))
				fprintf(stderr, "[mpdump] %lld %d.%d %s %.0f %.0f %.0f %.0f %x %.0f %.0f %s\n", at, m, i, typeid(*a).name(),
				        a->mPosition.x, a->mPosition.y, a->mPosition.z, a->mRotation.y, (unsigned)a->mLiveFlag, a->mRotation.x,
				        a->mRotation.z, a->getName() ? a->getName() : "-");
	int i = 0;
	for (JGadget::TList<TLiveActor*>::iterator it = gpConductor->unk30.begin(); it != gpConductor->unk30.end(); ++it, ++i)
		if (TLiveActor* a = *it)
			fprintf(stderr, "[mpdump] %lld 255.%d %s %.0f %.0f %.0f %.0f %x %.0f %.0f %s\n", at, i, typeid(*a).name(),
			        a->mPosition.x, a->mPosition.y, a->mPosition.z, a->mRotation.y, (unsigned)a->mLiveFlag, a->mRotation.x,
			        a->mRotation.z, a->getName() ? a->getName() : "-");
}

// Out of the world: falling with no floor at all beneath (fallen through
// one: a moving piece's collision gone from under a player, a place set
// from another game's) for a second and a half. The game catches a fall
// only onto its death floors, so the player would fall for good; instead
// they lose a life as into a pit (TMario's death-floor fall, as
// MarioMove.cpp has it), and come back as after any lost life.
int s_voidTicks;

void catchFallOut()
{
	TMario* m         = gpMarioOriginal;
	TMarDirector* dir = director();
	const int fps     = port_active_frame_rate > 0 ? port_active_frame_rate : 30;
	if (!m || !dir || !gpMap || s_spectating || !playing() || dir->isDemoModeNow() || m->checkFlag(MARIO_FLAG_GAME_OVER)
	    || m->checkFlag(MARIO_FLAG_IS_PERFORMING) || m->checkFlag(MARIO_FLAG_IN_WATER) || m->isTouchGround4cm()
	    || m->mVel.y > -5.0f) {
		s_voidTicks = 0;
		return;
	}
	const TBGCheckData* floor = nullptr;
	gpMap->checkGround(m->mPosition.x, m->mPosition.y + 80.0f, m->mPosition.z, &floor);
	if (!floor || !floor->isIllegalData()) {
		s_voidTicks = 0;
		return;
	}
	if (++s_voidTicks < fps * 3 / 2)
		return;
	s_voidTicks = 0;
	fprintf(stderr, "[multiplayer] fell out of the world at %.0f %.0f %.0f: a lost life\n", m->mPosition.x, m->mPosition.y,
	        m->mPosition.z);
	m->onFlag(MARIO_FLAG_GAME_OVER);
	m->mHealth = 0;
	if (m->mYoshi)
		m->mYoshi->kill();
	m->changePlayerStatus(MARIO_STATUS_FALL_DEAD, 0, true);
	gpCamera->unk64 |= CPolarSubCamera::CAMERA_FLAG_HELL_DEAD_DEMO;
	dir->onUnk4EFlag(8);
}

void afterMovement()
{
	g_moves++;
	if (!enabled() || !inStage())
		return;
	dumpActors();
	applyCleans();
	landHits();
	reportGraffiti();
	watchMyCarry();
	landCarries();
	tickHolders();
	MpMirrorTick();
	catchFallOut();
	holdOthersShine();
	if (s_role == MPG_ROLE_CLIENT) {
		clientSkipBlueSave(director());
		clientShineWait(director());
		followHostEnemies();
		reportOwnEnemies();
		MpBossClientTick();
	} else {
		steerEnemies(); // those a client's game runs, where it says
	}
	// not before the stage is played: while it loads, the player watched is
	// still where they were in the last stage, and Mario would be left there
	// (out of the world, here) as the arrival brings him back
	if (s_spectating && s_arrivedHere)
		spectateTick();
	if (s_arrivedHere)
		MpServerTick();
}

// A client's own way out (a pipe, a painting, a secret course's entrance, a
// story event): it leads. The trip goes ahead here, and the host is asked to
// take everyone the same way (mpg_request_trip, sent next frame, once the
// movie the way plays is known: TMarDirector::fireStreamingMovie sets it
// after setNextStage); meanwhile this game does not go back to the host's
// stage, and waits a while where it went for the others to come.
int s_leadCode = -1;           // a way out this game took, to ask the host to take (setNextStage's code)
int s_leadArea = -1, s_leadEpisode = -1; // where this game leads
int s_leadFrames;              // frames left to wait there for the host (0: not leading)
bool s_leadArrived;

int allowNextStage(unsigned code, void* actor)
{
	int area, episode;
	const int next        = code >= 0x100 ? (int)(code >> 8) - 1 : (int)code;
	const int nextEpisode = code >= 0x100 ? (int)(code & 0xFF) : 0xFF;
	if (inStage() && !MpServerAllowLeave(next))
		return 0; // the server keeps everyone in its stage (MPG_CMD_STAY)
	if (s_role == MPG_ROLE_HOST && inStage()) {
		// setting off: everyone sets off now, so the fades go together. Not
		// into the stage it is in (a lost life's start over is the host's
		// alone; a way back in brings a new session, which the others follow)
		const TGameSequence& cur = gpApplication.mCurrArea;
		if (next != cur.getStage() || (nextEpisode != 0xFF && nextEpisode != cur.getScenario()))
			mpg_host_leaving(next, nextEpisode);
		return 1;
	}
	(void)actor;
	if (s_role != MPG_ROLE_CLIENT || !mpg_target(&area, &episode) || !inStage() || s_spectating)
		return 1;
	if (next == area && (nextEpisode == 0xFF || nextEpisode == episode) && next == gpApplication.mCurrArea.getStage())
		return 1; // the stage everyone is in, again
	s_leadCode    = (int)code;
	s_leadArea    = next;
	s_leadEpisode = nextEpisode;
	s_leadFrames  = 0;
	s_leadArrived = false;
	if (getenv("SMS_MULTIPLAYER_DEBUG"))
		fprintf(stderr, "[multiplayer] our own way out to area %d, episode %d: the host is asked to take everyone\n", next,
		        nextEpisode);
	return 1;
}

// Client, each frame: the way out this game took, asked of the host (with the
// movie it plays on the way); and, arrived there, the wait for the others.
void leadTrip(TMarDirector* dir)
{
	const int fps = port_active_frame_rate > 0 ? port_active_frame_rate : 30;
	if (s_leadCode >= 0) {
		const int movie = dir && dir->checkUnk4CFlag(TMarDirector::DIRECTOR_FLAG_MOVIE_PENDING) ? (int)gpApplication.getMovie() : -1;
		if (mpg_request_trip(s_leadArea, s_leadEpisode, movie)) {
			char text[128];
			if (SMS_getShineStage((u8)s_leadArea) == SMS_getShineStage(gpApplication.mCurrArea.getStage()))
				snprintf(text, sizeof text, "Leading everyone on");
			else
				snprintf(text, sizeof text, "Leading everyone to %s", worldName(s_leadArea));
			showNotice(text);
			fprintf(stderr, "[multiplayer] leading everyone to area %d, episode %d (movie %d)\n", s_leadArea, s_leadEpisode,
			        movie);
			s_leadFrames = 60 * fps; // on the way (a movie, a load) and a while there
		} else {
			s_leadArea = -1; // not in the host's session: this game goes back to the host's stage
		}
		s_leadCode = -1;
	}
	if (s_leadFrames <= 0)
		return;
	// there: the others have this long to come (the host may not have set
	// off: busy leaving already); then this game goes back to the host's
	if (s_playing && gpApplication.mCurrArea.getStage() == s_leadArea && !s_leadArrived) {
		s_leadArrived = true;
		s_leadFrames  = 12 * fps;
	}
	if (--s_leadFrames == 0)
		s_leadArea = -1;
}

// Whether this game is leading the others to where it is going (or is).
// Also a way out just taken and not yet asked of the host (leadTrip asks next
// frame): the game leaves at once (TMarDirector::moveStage in the same tick
// as setNextStage), and was sent back to the host's stage instead, its
// opening played again, and then taken where it went by the host's trip
// (into a secret course: Bianco Hills' opening, then the FLUDD movie).
bool leading(int area)
{
	return (s_leadFrames > 0 || s_leadCode >= 0) && area == s_leadArea;
}

// Host: the clients' ways out (mpg_request_trip), taken as the host's own:
// everyone goes, with the movie on the way.
void takeTrips(TMarDirector* dir)
{
	int slot, area, episode, movie;
	static uint32_t tookIn;
	while (mpg_next_trip(&slot, &area, &episode, &movie)) {
		if (!dir || !inStage() || tookIn == mpg_session() || dir->checkFlag(TMarDirector::DIRECTOR_FLAG_STAGE_TRANSITION_PENDING))
			continue;
		tookIn = mpg_session();
		char text[128];
		const u8 here = gpApplication.mCurrArea.getStage();
		if (area == here)
			snprintf(text, sizeof text, "%s moved the story on: everyone goes", mpg_display_name(slot));
		else if (SMS_getShineStage((u8)area) == SMS_getShineStage(here))
			snprintf(text, sizeof text, "%s leads everyone on", mpg_display_name(slot));
		else
			snprintf(text, sizeof text, "%s leads everyone to %s", mpg_display_name(slot), worldName(area));
		showNotice(text);
		fprintf(stderr, "[multiplayer] %s (area %d, episode %d, movie %d)\n", text, area, episode, movie);
		dir->setNextStage(episode == 0xFF ? (u16)area : (u16)(((area + 1) << 8) | episode), nullptr);
		if (movie >= 0) {
			gpApplication.setMovie((u32)movie);
			dir->onUnk4CFlag(TMarDirector::DIRECTOR_FLAG_MOVIE_PENDING);
		}
	}
}

// The host's session flags as they are (mpg_host_going's).
uint32_t sessionFlags()
{
	uint32_t flags = 0;
	for (int i = 0; i < kSessionFlags; i++)
		if (TFlagManager::getInstance()->getBool(0x30000 + i))
			flags |= 1u << i;
	return flags;
}

void moveStage()
{
	int area, episode;
	if (s_role == MPG_ROLE_HOST) {
		// on its way (any way out: the pause menu's, a Shine's, an event's),
		// with what shapes the arrival: the others go too, and arrive alike
		const TGameSequence& next = gpApplication.mNextArea;
		const uint32_t flags      = sessionFlags();
		TMarDirector* dir = director();
		const int movie   = dir && dir->checkUnk4CFlag(TMarDirector::DIRECTOR_FLAG_MOVIE_PENDING)
		                        ? (int)gpApplication.getMovie() : -1;
		// the stage it is in, again (a way back in, a restart): a new session
		// all the same, so the others load it afresh too
		// (not a lost life's: that player alone starts over, the others play on)
		const TGameSequence& cur = gpApplication.mCurrArea;
		// to the title (it reloads itself now and then): nothing for the
		// others, who play on where they are (the controller told them the
		// host is at the title; they set off with it once it leaves for a stage)
		if (next.getStage() == 15)
			return;
		const bool same = next.getStage() == cur.getStage() && next.getScenario() == cur.getScenario();
		const bool miss = s_missReload && !MpServerLoading(); // a server mod's stage afresh is everyone's
		s_missReload    = false;
		if (same && miss)
			return; // the host's own start over: nothing for the others
		if (same)
			mpg_restart_session();
		mpg_host_going(next.getStage(), next.getScenario(), flags, movie);
		return;
	}
	const bool miss = s_missReload;
	s_missReload    = false;
	if (s_role != MPG_ROLE_CLIENT || !mpg_target(&area, &episode))
		return;
	// if it came after this game set off; not for this player's own lost
	// life in the host's stage (a trip's record left over from coming here
	// would play its movie again: the secret courses' FLUDD movie)
	if (!miss || area != gpApplication.mCurrArea.getStage())
		takeTravel(director());
	// after its own Shine it goes to the plaza, as the host will once saved;
	// unless the host went on already (clientShineExit waited for it)
	TMarDirector* dir = director();
	if (dir && dir->checkFlag(TMarDirector::DIRECTOR_FLAG_SHINE_TAKEN) && !hostWentOn(area, episode))
		return;
	// wherever this game meant to go, it goes where the host is; unless it
	// leads the others there (they come after it)
	TGameSequence& next = gpApplication.mNextArea;
	if (leading(next.getStage()))
		return;
	if (next.getStage() != area || (episode != 0xFF && next.getScenario() != episode)) {
		next.set((u8)area, (u8)episode, 0);
		if (episode != 0xFF)
			TFlagManager::getInstance()->setFlag(0x40003, episode);
	}
}

// A stage about to load, its episode chosen (port_ext.h's stageLoading;
// `area`, `episode` the one the game chose). On the host, another stage a mod
// chose then (SMS_EV_STAGE_LOADING) is everyone's: the others set off for it,
// as for the host's moveStage. A client on its way with the host loads the
// stage the host goes to, when it has heard of it, instead of its own (after
// the host's episode select, the host's mod may load another); one that has
// not goes there after (followHost).
void stageLoading(int area, int episode)
{
	if (!enabled())
		return;
	const TGameSequence& cur = gpApplication.mCurrArea;
	if (s_role == MPG_ROLE_HOST) {
		if (cur.getStage() == area && cur.getScenario() == episode)
			return;
		fprintf(stderr, "[multiplayer] the host loads area %d, episode %d instead of area %d, episode %d: everyone goes\n",
		        cur.getStage(), cur.getScenario(), area, episode);
		mpg_host_going(cur.getStage(), cur.getScenario(), sessionFlags(), -1);
		return;
	}
	int hostArea, hostEpisode;
	if (s_role != MPG_ROLE_CLIENT || !mpg_target(&hostArea, &hostEpisode) || !mpg_target_traveling()
	    || hostEpisode == 0xFF || leading(cur.getStage()))
		return;
	if (cur.getStage() == hostArea && cur.getScenario() == hostEpisode)
		return;
	const int wasArea = cur.getStage(), wasEpisode = cur.getScenario();
	if (!port_ext_load_instead(hostArea, hostEpisode))
		return;
	takeTravel(nullptr); // the host's session flags, as it went
	fprintf(stderr, "[multiplayer] loading the host's area %d, episode %d instead of area %d, episode %d\n", hostArea,
	        hostEpisode, wasArea, wasEpisode);
}

void missNextArea()
{
	if (!enabled())
		return;
	const TGameSequence& cur = gpApplication.mCurrArea;
	if (s_allOutTrip) {
		// the host's last life, and everyone else's gone already: everyone
		// leaves (moveStage takes them along), to the plaza or the plaza again
		s_allOutTrip = false;
		s_missReload = false;
		s_spectating = false;
		if (cur.getStage() == 1) {
			mpg_restart_session();
			gpApplication.mNextArea.set(cur.getStage(), cur.getScenario(), 0);
		} else {
			gpApplication.mNextArea.set(1, 0xFF, 0);
		}
		return;
	}
	s_missReload = true; // the same stage again for this player only (moveStage)
	// back into the same stage, not out to the plaza: the others are still here
	gpApplication.mNextArea.set(cur.getStage(), cur.getScenario(), 0);
}

int keepCoinsOnMiss() { return enabled(); }

void afterFileSelect()
{
	int area, episode;
	if (s_role == MPG_ROLE_HOST && MpServerAfterFileSelect())
		return; // a server mod's stage, not the file's
	if (s_role != MPG_ROLE_CLIENT || !mpg_flags_ready() || !mpg_target(&area, &episode))
		return;
	// the host's save, then straight to the host's stage
	syncFlags();
	TFlagManager::getInstance()->setFlag(0x40003, episode);
	gpApplication.mNextArea.set((u8)area, (u8)episode, 0);
}

void itemTaken(void* item, void*)
{
	if (!enabled() || !inStage())
		return;
	if (s_role == MPG_ROLE_CLIENT)
		clientSkipBlueSave(director()); // a blue coin's prompt, asked for as it was taken (TCoinBlue::taken)
	TItemManager* mgr = items();
	if (!mgr)
		return;
	const int n = mgr->getObjNum() < kMaxItems ? mgr->getObjNum() : kMaxItems;
	for (int i = 0; i < n; i++)
		if ((void*)mgr->getObj(i) == item) {
			TLiveActor* a = mgr->getObj(i);
			// a nozzle from a nozzle box: each player's own (every game has
			// its own box, which brings out another only once its nozzle is
			// taken there), not taken from the others
			if (dynamic_cast<TItemNozzle*>(a))
				return;
			if (!appearedDuringStage(i)) {
				mpg_item_taken(itemKey(i, a));
			} else {
				const float pos[3] = {a->mPosition.x, a->mPosition.y, a->mPosition.z};
				mpg_item_taken_at(a->getActorType() & 0xFF, pos);
			}
			return;
		}
}

int allowSaveStates() { return !enabled(); }

// The camera shaken by the server (MPG_CMD_SHAKE).
void cameraHook(void*, float* eye, float* at, float*)
{
	if (enabled())
		MpServerCamera(eye, at);
}

// MPG_RULE_NO_DAMAGE: this game's player loses nothing (health, or air under water).
int playerDamage(void* mario, int*)
{
	return enabled() && mario == gpMarioOriginal && MpServerNoDamage();
}

// Mario's own models make way for our character's (s_self draws them).
int hidePlayerModel(void* mario)
{
	return enabled() && s_self && mario == gpMarioOriginal && inStage();
}
const char* saveStatesOffReason() { return "Save states are off in multiplayer"; }

PortExtension s_extension = {
	"multiplayer",   frame,           afterMovement,   stageSetup,     allowNextStage, moveStage,
	missNextArea,    takeGameOver,    keepCoinsOnMiss, afterFileSelect, itemTaken,      allowSaveStates,
	saveStatesOffReason, hidePlayerModel, testPadRead, demoCamera, pollutionClean, objectMove, objectHit,
	particleEmit,    cameraHook,      nullptr,         playerDamage,
};

struct Register {
	Register()
	{
		s_extension.sound        = soundHook;
		s_extension.stageLoading = stageLoading;
		s_extension.fluddRefill  = MpPvpRefill;
		port_ext_register(&s_extension);
	}
} s_register;

} // namespace

TMpPuppet* MpPuppet(int slot) { return slot >= 0 && slot < MPG_MAX_PLAYERS ? s_puppets[slot] : nullptr; }
bool MpWatchable(int slot) { return enabled() && watchable(slot); }
void MpShowNotice(const char* text)
{
	if (enabled() && text && text[0])
		showNotice(text);
}

// Where player `slot` is in this game's stage (another extension's question,
// e.g. ext/demo): 1 and xyz filled in when they are here and in play.
extern "C" int mp_player_position(int slot, float* xyz)
{
	if (!enabled() || !inStage() || slot < 0 || slot >= MPG_MAX_PLAYERS)
		return 0;
	if (slot == mpg_slot()) {
		xyz[0] = gpMarioOriginal->mPosition.x;
		xyz[1] = gpMarioOriginal->mPosition.y;
		xyz[2] = gpMarioOriginal->mPosition.z;
		return 1;
	}
	if (!watchable(slot))
		return 0;
	xyz[0] = s_puppets[slot]->mPosition.x;
	xyz[1] = s_puppets[slot]->mPosition.y;
	xyz[2] = s_puppets[slot]->mPosition.z;
	return 1;
}

// Another player for the radar (src/radar.cpp): where they are on the
// ground (x, z), the way they face there (a unit x, z, from their root
// matrix as drawn) and their name; 1 when they are in play in this stage
// (not this game's own player, nor one out of lives watching).
extern "C" int mp_radar_player(int slot, float* xz, float* facing, char* name, int nameSize)
{
	if (!enabled() || !inStage() || !watchable(slot))
		return 0;
	const TMpPuppet* p  = s_puppets[slot];
	const MpgPose& pose = p->pose();
	xz[0]               = p->mPosition.x;
	xz[1]               = p->mPosition.z;
	f32 fx = pose.mtx[0][2], fz = pose.mtx[2][2];
	const f32 len = sqrtf(fx * fx + fz * fz);
	facing[0]     = len > 1e-4f ? fx / len : 0.0f;
	facing[1]     = len > 1e-4f ? fz / len : 1.0f;
	if (name && nameSize > 0)
		snprintf(name, nameSize, "%s", mpg_display_name(slot));
	return 1;
}

// Host: a client's word on an enemy its game runs (mp_mirror.cpp passes it
// on): the host's copy goes where it is, as a client's copy follows the host's.
void MpEnemyReport(int slot, int key, const void* data, int size)
{
	const int m = key >> 8, i = key & 0xFF;
	if (s_role != MPG_ROLE_HOST || size < 17 || slot <= 0 || slot >= MPG_MAX_PLAYERS || m >= s_managerCount || i >= kMaxEnemies)
		return;
	const u8* d = static_cast<const u8*>(data);
	if (d[16]) {
		s_enemyHeldBy[m][i] = (u8)slot; // it holds their player
		s_enemyHeldAt[m][i] = (u16)s_clock;
	}
	if (s_enemyOwner[m][i] != slot && !d[16])
		return; // not theirs (any more)
	JGeometry::TVec3<f32> at;
	f32 yawDeg;
	memcpy(&at.x, d, 12);
	memcpy(&yawDeg, d + 12, 4);
	if (at.x != at.x || at.y != at.y || at.z != at.z || yawDeg != yawDeg)
		return;
	EnemyTarget& t = s_targets[m][i];
	const int fps  = port_active_frame_rate > 0 ? port_active_frame_rate : 30;
	const f32 dt   = (s_clock - t.at) / (f32)fps;
	if (t.known && dt > 0.02f && dt < 1.0f)
		t.vel.set((at.x - t.pos.x) / dt, (at.y - t.pos.y) / dt, (at.z - t.pos.z) / dt);
	else
		t.vel.set(0.0f, 0.0f, 0.0f);
	t.pos   = at;
	t.yaw   = yawDeg;
	t.at    = s_clock;
	t.known = true;
}
