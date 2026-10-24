// Players against each other (mp_pvp.hpp). Game code, built with the game's
// flags: C++03, fixed arrays.
//
// Collision (mpg_collision, the host's word): each game keeps its own player
// out of the others as the game keeps Mario out of an enemy he walks into
// (TMario::keepDistance: running into one at speed bumps him back), and lands
// him on a head as on an enemy's (TMario::trampleExec: the stomp's bounce,
// dust and sound). Each player is moved only by their own game, so two players
// pressing into each other are kept apart from both sides.
//
// Water (MPG_RULE_WATER_HITS, a server mod's rule): each game judges its own
// player's water, as it judges what that water hits of the stage's. A FLUDD
// drop in flight that reaches another player stops there, and the hits are
// gathered up and reported to the host (mpg_splash), about ten times a second
// while they keep coming. The other players' water drawn here carries its
// player's slot (MpPvpWaterTag), so every game stops its copy of a drop at
// the player it reaches, and never at the one who sprayed it.
//
// Splashes (MPG_CMD_SPLASH, a server mod's word on a hit): what Shadow Mario
// does when FLUDD's water hits him (TEnemyMario::hitWater): his water splash
// (PARTICLE_MS_ENM_WATHIT) and hit sound (MSD_SE_EN_COMMON_W_HIT_OK) where the
// water hit, his model flushed with the damage fog for 120 of his frames
// after the last hit (SMS_AddDamageFogEffect), and his water meter over his
// head for 600 (drawHPMeter): every game shows it on its copy of the player.
// Down (the meter empty): the player's own game throws them back, onto their
// back, as his falls down.
#include "mp_pvp.hpp"

#include "../mp_game_api.h"
#include "mp_puppet.hpp"
#include "mp_server.hpp"
#include "port_ext.h"

#include <Camera/Camera.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DMaterial.hpp>
#include <JSystem/JDrama/JDRGraphics.hpp>
#include <MSound/MSound.hpp>
#include <Map/Map.hpp>
#include <Map/MapData.hpp>
#include <MSound/SoundEffects.hpp>
#include <M3DUtil/M3UModelMario.hpp>
#include <MarioUtil/DrawUtil.hpp>
#include <Player/Mario.hpp>
#include <Player/MarioCap.hpp>
#include <Player/MarioStatus.hpp>
#include <Player/ModelWaterManager.hpp>
#include <Player/WaterGun.hpp>
#include <Strategic/HitActor.hpp>
#include <Strategic/LiveActor.hpp>
#include <System/EmitterViewObj.hpp>
#include <System/MarDirector.hpp>
#include <System/Particles.hpp>
#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include <dolphin/pad.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace {

const u16 kDrawnWater  = 0x8000; // mp_game.cpp's: a drop drawn for another player
const u16 kOwnerShift  = 8;      // its player's slot + 1, in the bits the game leaves alone
const u16 kOwnerMask   = 0x0F00;
const f32 kBodyRadius  = 55.0f;  // a player's body, for water (Shadow Mario's damage radius, 60, near enough)
const f32 kBodyHeight  = 170.0f; // feet to the top of the cap
const f32 kHeadHeight  = 210.0f; // the meter's height over the feet (drawHPMeter's)
const f32 kNearHeight  = 140.0f; // collision: closer than this up or down, they overlap
const unsigned kReportMs = 100;  // hits gathered for this long before they are reported
const unsigned kFlushMs  = 800;  // a hit's flush: a moment, so it says "hit just now" (Shadow Mario's lasts 4 s: one attacker)
const unsigned kMeterMs  = 20000; // and 600

unsigned clockMs()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

bool debug()
{
	static const bool on = getenv("SMS_MULTIPLAYER_DEBUG") != nullptr;
	return on;
}

// How each player looks after their last splash (as every game was told).
struct Splashed {
	unsigned flushUntil, meterUntil;
	int meter, full;
};
Splashed s_splashed[MPG_MAX_PLAYERS];

// This game's water's hits on each player, not reported yet.
struct Hits {
	int drops;
	unsigned since;
	f32 at[3];
};
Hits s_hits[MPG_MAX_PLAYERS];
int s_totalHits[MPG_MAX_PLAYERS]; // debug: every hit landed here

unsigned s_entryFrame, s_drawFrame; // MpPvpEntry's and MpPvpDraw's once a frame
bool s_marioFogged;

// Test aids (SMS_MULTIPLAYER_TEST, seconds into each stage): pvphunt@S[:T]
// stands this game's player 250 units from the nearest other player (or slot
// T) facing them, every two seconds, spraying all the while; pvpoverlap@S puts
// them inside the nearest player and logs how far apart they are for a second;
// pvpstomp@S drops them onto the nearest player's head; pvpout@S puts them
// 500 units beyond the arena's edge (MPG_CMD_BOUNDS) and logs where they are
// for a second and a half; pvppickup@S puts them on the nearest marker over a
// place every three seconds (a server mod's pickups); with debug, the tank
// is logged every second.
struct Test {
	bool parsed;
	int huntAt, huntSlot, overlapAt, stompAt, outAt, pickupAt, seaAt, botAt;
	unsigned seaUntil;
	// pvpbot@S: a player that plays (stick and buttons as a person would)
	s8 botStickX, botStickY;
	u8 botTrigger;
	u16 botButtons;
	bool botOn;
	unsigned botTurnAt, botJumpAt, botJumpUntil;
	f32 botStrafe, botWander;
	unsigned stageAt, lastHunt, overlapUntil, outUntil, lastPickup, lastTank;
	bool overlapped, stomped, hunting, outDone;
};
Test s_test;

bool after(unsigned now, unsigned until) { return (int)(until - now) <= 0; }

TMario* player()
{
	TMario* m = gpMarioOriginal;
	if (!m || MpServerDedicated() || m->checkFlag(MARIO_FLAG_IS_PERFORMING))
		return nullptr;
	return m;
}

bool surfing(const TMario* m) { return m->checkStatusType(MARIO_STATUS_FLAG_UNK10000) && m->mSurfGesso; }

// Another player here who is in play, and where they stand.
TMpPuppet* other(int slot)
{
	return slot != mpg_slot() && MpWatchable(slot) ? MpPuppet(slot) : nullptr;
}

// ------------------------------------------------------------------- collision

// The puppet takes a stomp as an enemy does (TMario::trampleExec asks it).
void collide(TMario* m)
{
	if (surfing(m))
		return; // a Blooper's rider surfs through
	for (int slot = 0; slot < MPG_MAX_PLAYERS; slot++) {
		TMpPuppet* p = other(slot);
		if (!p || (p->pose().more & MPG_POSE2_SURF))
			continue;
		const JGeometry::TVec3<f32> at(p->mPosition.x, p->mPosition.y, p->mPosition.z);
		const f32 dy = m->mPosition.y - at.y;
		const f32 dx = m->mPosition.x - at.x, dz = m->mPosition.z - at.z;
		const f32 reach = m->unk15C * 2.0f;
		if (dx * dx + dz * dz >= reach * reach * 1.44f)
			continue;
		// coming down on their head: a stomp's bounce, as on an enemy
		if (dy > kNearHeight * 0.6f && dy < kBodyHeight + 30.0f && m->mVel.y < 0.0f
		    && m->checkStatusType(MARIO_STATUS_FLAG_JUMPING) && m->mStatus != MARIO_STATUS_HIP_DROP) {
			if (m->trampleExec(p)) {
				if (debug())
					fprintf(stderr, "[multiplayer] collision: on %s's head\n", mpg_display_name(slot));
				continue;
			}
		}
		if (dy > -kNearHeight && dy < kNearHeight)
			m->keepDistance(at, m->unk15C, 0.0f);
	}
}

// ------------------------------------------------------------------- water

// Each FLUDD drop in flight against the players it may hit: this game's own
// against the others, another player's (drawn here) against everyone but them.
void water(TMario* m)
{
	TModelWaterManager* w = gpModelWaterManager;
	if (!w)
		return;
	const int me = mpg_slot();
	f32 at[MPG_MAX_PLAYERS][3];
	bool here[MPG_MAX_PLAYERS];
	int targets = 0;
	for (int slot = 0; slot < MPG_MAX_PLAYERS; slot++) {
		here[slot] = false;
		if (slot == me && m) {
			at[slot][0] = m->mPosition.x, at[slot][1] = m->mPosition.y, at[slot][2] = m->mPosition.z;
			here[slot] = true;
		} else if (TMpPuppet* p = other(slot)) {
			at[slot][0] = p->mPosition.x, at[slot][1] = p->mPosition.y, at[slot][2] = p->mPosition.z;
			here[slot] = true;
		}
		targets += here[slot];
	}
	if (!targets)
		return;
	u16* flags = w->getParticleFlagSOA();
	for (int i = 0; i < w->mParticleCount; i++) {
		const u16 f = flags[i];
		if ((f & 0xF) != 1 || !(f & 0x40) || w->mParticleLifetimeSOA[i] <= 0.0f)
			continue; // in flight, FLUDD's
		const int owner = (f & kDrawnWater) ? ((f & kOwnerMask) >> kOwnerShift) - 1 : me;
		if (owner < 0)
			continue;
		const JGeometry::TVec3<f32>& d = w->mParticlePositionSOA[i];
		const f32 r = kBodyRadius + w->mParticleSizeSOA[i] * 0.5f;
		for (int slot = 0; slot < MPG_MAX_PLAYERS; slot++) {
			if (!here[slot] || slot == owner)
				continue;
			const f32 dx = d.x - at[slot][0], dz = d.z - at[slot][2], dy = d.y - at[slot][1];
			if (dx * dx + dz * dz >= r * r || dy < -20.0f || dy > kBodyHeight)
				continue;
			w->mParticleLifetimeSOA[i] = 0.0f; // it stops at them
			if (owner == me) {
				Hits& h = s_hits[slot];
				if (!h.drops)
					h.since = clockMs();
				h.drops++;
				h.at[0] = d.x, h.at[1] = d.y, h.at[2] = d.z;
				s_totalHits[slot]++;
			}
			break;
		}
	}
	const unsigned now = clockMs();
	for (int slot = 0; slot < MPG_MAX_PLAYERS; slot++) {
		Hits& h = s_hits[slot];
		if (h.drops && now - h.since >= kReportMs) {
			mpg_splash(slot, h.drops, h.at);
			if (debug())
				fprintf(stderr, "[multiplayer] water: %d drops on %s (%d so far, at %u ms)\n", h.drops,
				        mpg_display_name(slot), s_totalHits[slot], now);
			h.drops = 0;
		}
	}
}

// ------------------------------------------------------------------- splashes

// Where the sound plays from while it plays (it reads its place as it goes).
Vec* soundAt(const f32* p)
{
	static Vec s_at[8];
	static int s_next;
	Vec& at = s_at[s_next++ & 7];
	at.x = p[0], at.y = p[1], at.z = p[2];
	return &at;
}

void knockDown(TMario* m, const f32* from)
{
	if (m->onYoshi())
		m->getOffYoshi(false);
	if (surfing(m)) // thrown off it, as a crash throws him
		m->changePlayerStatus(MARIO_STATUS_JUMP_BACK_DOWN, 0, true);
	JGeometry::TVec3<f32> dir(m->mPosition.x - from[0], 0.0f, m->mPosition.z - from[2]);
	f32 len = sqrtf(dir.x * dir.x + dir.z * dir.z);
	if (len < 1.0f) {
		dir.set(-sinf(m->mFaceAngle.y * (3.14159265f / 32768.0f)), 0.0f, -cosf(m->mFaceAngle.y * (3.14159265f / 32768.0f)));
		len = 1.0f;
	}
	dir.x /= len, dir.z /= len;
	dir.y = 0.7f;
	m->changePlayerStatus(MARIO_STATUS_THROWN_DOWN, 0, true);
	m->throwMario(dir, 28.0f);
	if (debug())
		fprintf(stderr, "[multiplayer] knocked down (status %x)\n", (unsigned)m->mStatus);
}

// Shadow Mario's model's fog (TEnemyMario::perform's CUE_ENTRY), in Mario's
// damage colour (TMario::addDamageFog's).
void fogModel(J3DModelData* data, const f32* at, JDrama::TGraphics* graphics)
{
	const GXColor colour = {0xFF, 0x00, 0x80, 0xFF};
	for (u16 i = 0; i < data->getMaterialNum(); i++)
		if (J3DPEBlock* pe = data->getMaterialNodePointer(i)->getPEBlock())
			if (J3DFog* fog = pe->getFog())
				fog->mColor = colour;
	const JGeometry::TVec3<f32> p(at[0], at[1], at[2]);
	SMS_AddDamageFogEffect(data, p, graphics);
}

// Direct GX drawing in one colour, `pos` the position matrix (as
// TEnemyMario::drawHPMeter sets it up), see-through, over the scene's depth.
void flatGX(MtxPtr pos)
{
	Mtx identity;
	MTXIdentity(identity);
	GXSetCurrentMtx(GX_PNMTX0);
	GXLoadPosMtxImm(pos, GX_PNMTX0);
	GXLoadNrmMtxImm(identity, GX_PNMTX0);
	GXClearVtxDesc();
	GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GXSetNumChans(1);
	GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
	GXSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
	GXSetNumTexGens(0);
	GXSetNumTevStages(1);
	GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	GXSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
	GXSetCullMode(GX_CULL_NONE);
	GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_NOOP);
	GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
	GXSetNumIndStages(0);
	GXSetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, (GXColor){0, 0, 0, 0});
}

// TEnemyMario::drawHPMeter: the bar over their head, in view space.
void meter(MtxPtr view, const f32* feet, f32 share)
{
	JGeometry::TVec3<f32> world(feet[0], feet[1] + kHeadHeight, feet[2]);
	JGeometry::TVec3<f32> screen;
	MTXMultVec(view, &world, &screen);
	if (screen.z > -10.0f)
		return; // behind the camera
	Mtx identity;
	MTXIdentity(identity);
	flatGX(identity);

	const f32 top = screen.y - 10.0f, bottom = screen.y + 10.0f, left = screen.x - 48.0f;
	GXSetChanMatColor(GX_COLOR0A0, (GXColor){0, 0, 0, 0xC0});
	GXBegin(GX_QUADS, GX_VTXFMT0, 4);
	GXPosition3f32(left - 5.0f, top - 5.0f, screen.z);
	GXPosition3f32(left + 101.0f, top - 5.0f, screen.z);
	GXPosition3f32(left + 101.0f, bottom + 5.0f, screen.z);
	GXPosition3f32(left - 5.0f, bottom + 5.0f, screen.z);
	GXEnd();
	const f32 right = left + 96.0f * (share < 0.0f ? 0.0f : share > 1.0f ? 1.0f : share);
	GXSetChanMatColor(GX_COLOR0A0, (GXColor){0x40, 0x40, 0xFF, 0xFF});
	GXBegin(GX_QUADS, GX_VTXFMT0, 4);
	GXPosition3f32(left, top, screen.z);
	GXPosition3f32(right, top, screen.z);
	GXPosition3f32(right, bottom, screen.z);
	GXPosition3f32(left, bottom, screen.z);
	GXEnd();
}

// ------------------------------------------------------------------- the arena

// MPG_CMD_BOUNDS: a circle the player is kept in, gliding from one radius to
// another over a time, and drawn as a band along the ground when asked.
struct Bounds {
	bool on, ring;
	f32 at[3];
	f32 from, to;       // the radius, gliding from `from` to `to`
	unsigned since, ms; // over ms from since (0: there at once)
	GXColor colour;
};
Bounds s_bounds;
const f32 kBandWidth = 32.0f; // the line on the ground
const f32 kBandLift  = 6.0f;  // above it, against the ground's own depth
const f32 kEdgeMargin = 30.0f; // the player's middle is kept this far inside
enum { kRingMost = 720 };
f32 s_ringY[kRingMost + 1]; // the ground under each of the ring's points
int s_ringN;
f32 s_ringR = -1.0f, s_ringAt[3];

f32 boundsRadius(unsigned now)
{
	if (!s_bounds.on)
		return 0.0f;
	if (!s_bounds.ms || now - s_bounds.since >= s_bounds.ms)
		return s_bounds.to;
	const f32 t = (f32)(now - s_bounds.since) / (f32)s_bounds.ms;
	return s_bounds.from + (s_bounds.to - s_bounds.from) * t;
}

// The player inside the edge: back to it where they crossed it, the speed
// that took them out taken away (they slide along it, as along a wall).
void keepIn(TMario* m)
{
	const f32 r = boundsRadius(clockMs());
	if (r <= 0.0f || surfing(m))
		return;
	f32 limit = r - kEdgeMargin;
	if (limit < 50.0f)
		limit = 50.0f;
	const f32 dx = m->mPosition.x - s_bounds.at[0], dz = m->mPosition.z - s_bounds.at[2];
	const f32 d  = sqrtf(dx * dx + dz * dz);
	if (d <= limit || d <= 0.0f)
		return;
	const f32 nx = dx / d, nz = dz / d;
	Vec in = {s_bounds.at[0] + nx * limit, m->mPosition.y, s_bounds.at[2] + nz * limit};
	m->checkWallPlane(&in, 60.0f, m->unk15C); // not into a wall on the way in
	m->mPosition.x = in.x;
	m->mPosition.z = in.z;
	const f32 out = m->mVel.x * nx + m->mVel.z * nz;
	if (out > 0.0f) {
		m->mVel.x -= out * nx;
		m->mVel.z -= out * nz;
	}
	// running at the edge: no further out
	const f32 face = (f32)m->mFaceAngle.y * (3.14159265f / 32768.0f);
	if (m->mForwardVel > 0.0f && sinf(face) * nx + cosf(face) * nz > 0.7f)
		m->mForwardVel = 0.0f;
}

// The ground (or water) under each of the ring's points, found again when
// the ring moves or grows: each from a little above the last, so it follows
// the ground up and down without settling on what overhangs it.
void ringGround(f32 r)
{
	int n = (int)(2.0f * 3.14159265f * r / 30.0f);
	s_ringN = n < 48 ? 48 : n > kRingMost ? kRingMost : n;
	s_ringR = r;
	memcpy(s_ringAt, s_bounds.at, sizeof s_ringAt);
	f32 last = s_bounds.at[1];
	for (int i = 0; i <= s_ringN; i++) {
		const f32 a = 2.0f * 3.14159265f * (f32)i / (f32)s_ringN;
		const f32 x = s_bounds.at[0] + r * sinf(a), z = s_bounds.at[2] + r * cosf(a);
		const TBGCheckData* floor = nullptr;
		f32 y = gpMap ? gpMap->checkGround(x, last + 400.0f, z, &floor) : last;
		if (!floor || floor->isIllegalData() || y < last - 3000.0f)
			y = last; // off the map, or a drop: keep the line level
		s_ringY[i] = y;
		last       = y;
	}
}

void drawRing(MtxPtr view)
{
	const f32 r = boundsRadius(clockMs());
	if (r <= 0.0f)
		return;
	if (s_ringR < 0.0f || fabsf(r - s_ringR) > 8.0f || memcmp(s_ringAt, s_bounds.at, sizeof s_ringAt))
		ringGround(r);
	flatGX(view);
	GXSetChanMatColor(GX_COLOR0A0, s_bounds.colour);
	GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, (u16)(2 * (s_ringN + 1)));
	for (int i = 0; i <= s_ringN; i++) {
		const f32 a = 2.0f * 3.14159265f * (f32)i / (f32)s_ringN;
		const f32 sx = sinf(a), cz = cosf(a), y = s_ringY[i] + kBandLift;
		GXPosition3f32(s_bounds.at[0] + (r - kBandWidth * 0.5f) * sx, y, s_bounds.at[2] + (r - kBandWidth * 0.5f) * cz);
		GXPosition3f32(s_bounds.at[0] + (r + kBandWidth * 0.5f) * sx, y, s_bounds.at[2] + (r + kBandWidth * 0.5f) * cz);
	}
	GXEnd();
}

// ------------------------------------------------------------------- FLUDD's water

int s_usePercent = 100; // MPG_PLAYER_WATER_USE
s32 s_lastWater  = -1;  // the tank as the last tick left it
bool s_waterSet;        // the server set it since
f32 s_useCarry;         // the fraction of extra use carried over

// FLUDD's own use since the last tick, made more (or less) as asked.
void waterUse(TMario* m)
{
	TWaterGun* gun = m ? m->mWaterGun : nullptr;
	if (!gun) {
		s_lastWater = -1;
		return;
	}
	s32 w = gun->mCurrentWater;
	if (s_usePercent != 100 && s_lastWater >= 0 && !s_waterSet && w < s_lastWater) {
		const f32 extra = (f32)(s_lastWater - w) * (f32)(s_usePercent - 100) / 100.0f + s_useCarry;
		const s32 whole = (s32)extra;
		s_useCarry      = extra - (f32)whole;
		w -= whole;
		w = w < 0 ? 0 : w > gun->getMaxWater() ? gun->getMaxWater() : w;
		gun->mCurrentWater = w;
	}
	s_waterSet  = false;
	s_lastWater = w;
}

// The nearest other player in play here (or `want`), or -1.
int nearest(TMario* m, int want)
{
	int best = -1;
	f32 bestD = 1e30f;
	for (int slot = 0; slot < MPG_MAX_PLAYERS; slot++) {
		TMpPuppet* p = other(slot);
		if (!p || (want >= 0 && slot != want))
			continue;
		const f32 dx = p->mPosition.x - m->mPosition.x, dz = p->mPosition.z - m->mPosition.z;
		if (dx * dx + dz * dz < bestD) {
			bestD = dx * dx + dz * dz;
			best  = slot;
		}
	}
	return best;
}

void testParse()
{
	if (s_test.parsed)
		return;
	s_test.parsed = true;
	s_test.huntAt = s_test.huntSlot = s_test.overlapAt = s_test.stompAt = s_test.outAt = s_test.pickupAt = s_test.seaAt = s_test.botAt = -1;
	const char* plan = getenv("SMS_MULTIPLAYER_TEST");
	if (!plan)
		return;
	if (const char* h = strstr(plan, "pvphunt@"))
		sscanf(h, "pvphunt@%d:%d", &s_test.huntAt, &s_test.huntSlot);
	if (const char* o = strstr(plan, "pvpoverlap@"))
		sscanf(o, "pvpoverlap@%d", &s_test.overlapAt);
	if (const char* t = strstr(plan, "pvpstomp@"))
		sscanf(t, "pvpstomp@%d", &s_test.stompAt);
	if (const char* o = strstr(plan, "pvpout@"))
		sscanf(o, "pvpout@%d", &s_test.outAt);
	if (const char* b = strstr(plan, "pvpbot@"))
		sscanf(b, "pvpbot@%d", &s_test.botAt);
	if (const char* w = strstr(plan, "pvpsea@"))
		sscanf(w, "pvpsea@%d", &s_test.seaAt);
	if (const char* k = strstr(plan, "pvppickup@"))
		sscanf(k, "pvppickup@%d", &s_test.pickupAt);
}

// The stick that takes Mario along world yaw `yaw` (TMario::checkController:
// the stick's angle, matan(-v, h), plus the camera's), `mag` 0..1.
void botStick(f32 yaw, f32 mag)
{
	const f32 cam = gpCamera ? (f32)gpCamera->getUnk258() * (3.14159265f / 32768.0f) : 0.0f;
	const f32 d   = yaw - cam;
	s_test.botStickX = (s8)(sinf(d) * 100.0f * mag);
	s_test.botStickY = (s8)(-cosf(d) * 100.0f * mag);
}

f32 yawTo(const TMario* m, f32 x, f32 z) { return atan2f(x - m->mPosition.x, z - m->mPosition.z); }

// pvpbot@S: plays as a person would, by its controller. Without FLUDD (a
// lobby) it runs about the edge's middle, jumping now and then; with it, it
// goes for the nearest player, circles them spraying, strafing one way then
// the other, and when its tank runs low runs for the nearest of the server's
// spawned pickups. Near the arena's edge it heads back in.
void botTick(TMario* m)
{
	const unsigned now = clockMs();
	const int me       = mpg_slot() < 0 ? 0 : mpg_slot();
	s_test.botButtons  = 0;
	s_test.botTrigger  = 0;
	s_test.botStickX = s_test.botStickY = 0;
	s_test.botOn       = true;
	if (now >= s_test.botTurnAt) { // a new whim every two to four seconds
		s_test.botTurnAt = now + 2000 + (now * 7 + me * 911) % 2000;
		s_test.botStrafe = ((now / 1000 + me) & 1) ? 1.0f : -1.0f;
		s_test.botWander += 1.3f + 0.4f * me;
	}
	if (now >= s_test.botJumpAt) {
		s_test.botJumpAt    = now + 2500 + (now * 13 + me * 577) % 3500;
		s_test.botJumpUntil = now + 120;
	}
	const bool jump = now < s_test.botJumpUntil;
	TWaterGun* gun  = m->mWaterGun;
	const bool fludd = gun && m->checkFlag(MARIO_FLAG_HAS_FLUDD);
	const f32 r      = boundsRadius(now);
	f32 cx = m->mPosition.x, cz = m->mPosition.z;
	if (s_bounds.on)
		cx = s_bounds.at[0], cz = s_bounds.at[2];
	const f32 fromMiddle = sqrtf((m->mPosition.x - cx) * (m->mPosition.x - cx) + (m->mPosition.z - cz) * (m->mPosition.z - cz));
	if (!fludd) {
		// the lobby: about the middle, half the edge out
		const f32 aim = s_bounds.on ? r * 0.5f : 300.0f;
		botStick(yawTo(m, cx + aim * sinf(s_test.botWander), cz + aim * cosf(s_test.botWander)), 0.9f);
		if (jump)
			s_test.botButtons |= PAD_BUTTON_A;
		return;
	}
	if (s_bounds.on && fromMiddle > r * 0.85f) { // the edge: back in
		botStick(yawTo(m, cx, cz), 1.0f);
		return;
	}
	const f32 water = gun->getMaxWater() > 0 ? (f32)gun->mCurrentWater / (f32)gun->getMaxWater() : 0.0f;
	static unsigned said;
	if (debug() && now - said >= 1000) {
		said = now;
		fprintf(stderr, "[multiplayer] bot: water %.2f, %.0f from the middle (edge %.0f), status %x, stick %d %d\n", water,
		        fromMiddle, r, (unsigned)m->mStatus, s_test.botStickX, s_test.botStickY);
	}
	if (water < 0.12f) { // the nearest pickup
		// the pickups it has not been to yet (each one taken is gone, and
		// comes back as a new one)
		static bool been[128];
		static u32 beenSession;
		if (beenSession != mpg_session()) {
			beenSession = mpg_session();
			memset(been, 0, sizeof been);
		}
		f32 at[3], best[3], bestD = 1e30f;
		for (int i = 0; i < 128; i++)
			if (!been[i] && MpSpawnedPos(i, at)) {
				const f32 dx = at[0] - m->mPosition.x, dz = at[2] - m->mPosition.z;
				if (dx * dx + dz * dz < 180.0f * 180.0f) {
					been[i] = true;
					continue;
				}
				if (dx * dx + dz * dz < bestD) {
					bestD = dx * dx + dz * dz;
					memcpy(best, at, sizeof best);
				}
			}
		static unsigned toldCapsule;
		if (debug() && now - toldCapsule >= 1000) {
			toldCapsule = now;
			fprintf(stderr, "[multiplayer] bot: for a capsule %.0f away (at %.0f %.0f)\n", sqrtf(bestD), bestD < 1e29f ? best[0] : 0.0f,
			        bestD < 1e29f ? best[2] : 0.0f);
		}
		if (bestD < 1e29f) {
			botStick(yawTo(m, best[0], best[2]), 1.0f);
			if (jump && bestD > 600.0f * 600.0f)
				s_test.botButtons |= PAD_BUTTON_A;
			return;
		}
	}
	const int t = nearest(m, -1);
	if (t < 0 || water < 0.03f) { // nobody here, or nothing to spray: keep moving
		botStick(yawTo(m, cx + r * 0.4f * sinf(s_test.botWander), cz + r * 0.4f * cosf(s_test.botWander)), 1.0f);
		if (jump)
			s_test.botButtons |= PAD_BUTTON_A;
		return;
	}
	TMpPuppet* p = MpPuppet(t);
	const f32 dx = p->mPosition.x - m->mPosition.x, dz = p->mPosition.z - m->mPosition.z;
	const f32 d  = sqrtf(dx * dx + dz * dz);
	const f32 toward = atan2f(dx, dz);
	if (d > 750.0f) { // run at them
		botStick(toward, 1.0f);
		if (jump)
			s_test.botButtons |= PAD_BUTTON_A;
		return;
	}
	// close: aim and spray (facing them: Mario sprays the way he faces), then
	// run a few steps round them to a new angle, and again
	const bool aiming = (now + (unsigned)me * 900u) % 2700u < 1700u;
	if (aiming && water > 0.03f && d < 700.0f) {
		botStick(toward, d > 450.0f ? 0.35f : 0.2f);
		s_test.botButtons |= PAD_TRIGGER_R;
		s_test.botTrigger = 255;
		return;
	}
	const f32 off = d < 300.0f ? 2.0f : d > 550.0f ? 0.7f : 1.4f;
	botStick(toward + off * s_test.botStrafe, 1.0f);
	if (jump)
		s_test.botButtons |= PAD_BUTTON_A;
}

void testTick(TMario* m)
{
	testParse();
	const unsigned now = clockMs();
	const int secs     = (int)((now - s_test.stageAt) / 1000);
	s_test.hunting     = false;
	s_test.botOn       = false;
	if (!m)
		return;
	if (s_test.botAt >= 0 && secs >= s_test.botAt)
		botTick(m);
	// pvpsea@S: into the nearest water deep enough to wade in, spraying for five seconds
	if (s_test.seaAt >= 0 && secs >= s_test.seaAt && !s_test.seaUntil && gpMap) {
		s_test.seaUntil = now + 5000;
		for (int ring = 1; ring <= 16 && s_test.seaAt >= 0; ring++)
			for (int k = 0; k < 32; k++) {
				const f32 a = 6.2831853f * k / 32.0f, x = m->mPosition.x + ring * 250.0f * sinf(a),
				          z = m->mPosition.z + ring * 250.0f * cosf(a);
				const TBGCheckData* top = nullptr;
				const f32 y = gpMap->checkGround(x, m->mPosition.y + 1000.0f, z, &top);
				if (top && !top->isIllegalData() && top->isWaterSurface()) {
					m->mPosition.x = x, m->mPosition.y = y, m->mPosition.z = z;
					fprintf(stderr, "[multiplayer] test: into the water at %.0f %.0f %.0f, tank %d\n", x, y, z,
					        m->mWaterGun ? (int)m->mWaterGun->mCurrentWater : -1);
					s_test.seaAt = -1;
					break;
				}
			}
	}
	if (s_test.seaUntil && !after(now, s_test.seaUntil))
		s_test.hunting = true; // R held
	if (s_test.huntAt >= 0 && secs >= s_test.huntAt) {
		const int t = nearest(m, s_test.huntSlot);
		s_test.hunting = t >= 0;
		if (t >= 0 && now - s_test.lastHunt >= 2000) {
			s_test.lastHunt = now;
			TMpPuppet* p    = MpPuppet(t);
			const f32 a     = (f32)(now % 6283) / 1000.0f; // round them, a new side each time
			m->mPosition.x  = p->mPosition.x + 250.0f * sinf(a);
			m->mPosition.y  = p->mPosition.y;
			m->mPosition.z  = p->mPosition.z + 250.0f * cosf(a);
			m->mFaceAngle.y = (s16)(atan2f(p->mPosition.x - m->mPosition.x, p->mPosition.z - m->mPosition.z)
			                        * (32768.0f / 3.14159265f));
			m->mModelFaceAngle = m->mFaceAngle.y;
			fprintf(stderr, "[multiplayer] test: spraying %s from 250 units\n", mpg_display_name(t));
		}
	}
	if (s_test.overlapAt >= 0 && !s_test.overlapped && secs >= s_test.overlapAt) {
		const int t = nearest(m, -1);
		if (t >= 0) {
			s_test.overlapped   = true;
			s_test.overlapUntil = now + 1500;
			m->mPosition.x      = MpPuppet(t)->mPosition.x + 10.0f;
			m->mPosition.y      = MpPuppet(t)->mPosition.y;
			m->mPosition.z      = MpPuppet(t)->mPosition.z;
			fprintf(stderr, "[multiplayer] test: inside %s (collision %s)\n", mpg_display_name(t),
			        mpg_collision() ? "on" : "off");
		}
	}
	if (s_test.overlapped && !after(now, s_test.overlapUntil)) {
		const int t = nearest(m, -1);
		static unsigned said;
		if (t >= 0 && now - said >= 250) {
			said = now;
			const f32 dx = MpPuppet(t)->mPosition.x - m->mPosition.x, dz = MpPuppet(t)->mPosition.z - m->mPosition.z;
			fprintf(stderr, "[multiplayer] test: %.0f units from %s\n", sqrtf(dx * dx + dz * dz), mpg_display_name(t));
		}
	}
	if (s_test.outAt >= 0 && !s_test.outDone && secs >= s_test.outAt && s_bounds.on) {
		s_test.outDone  = true;
		s_test.outUntil = now + 1500;
		m->mPosition.x  = s_bounds.at[0] + boundsRadius(now) + 500.0f;
		m->mPosition.z  = s_bounds.at[2];
		fprintf(stderr, "[multiplayer] test: 500 units beyond the arena's edge (%.0f)\n", boundsRadius(now));
	}
	if (s_test.outDone && !after(now, s_test.outUntil) && now - s_test.lastHunt >= 250) {
		s_test.lastHunt = now;
		const f32 dx = m->mPosition.x - s_bounds.at[0], dz = m->mPosition.z - s_bounds.at[2];
		fprintf(stderr, "[multiplayer] test: %.0f units from the arena's middle (edge %.0f)\n", sqrtf(dx * dx + dz * dz),
		        boundsRadius(now));
	}
	if (s_test.pickupAt >= 0 && secs >= s_test.pickupAt && now - s_test.lastPickup >= 3000) {
		f32 best[3], at[3], bestD = 1e30f;
		for (int i = 0; i < 8; i++)
			if (MpServerMarker(i, at)) {
				const f32 dx = at[0] - m->mPosition.x, dz = at[2] - m->mPosition.z;
				if (dx * dx + dz * dz < bestD) {
					bestD = dx * dx + dz * dz;
					memcpy(best, at, sizeof best);
				}
			}
		if (bestD < 1e29f) {
			s_test.lastPickup = now;
			m->mPosition.x = best[0], m->mPosition.y = best[1] - 120.0f, m->mPosition.z = best[2];
			fprintf(stderr, "[multiplayer] test: onto the marker at %.0f %.0f %.0f\n", best[0], best[1], best[2]);
		}
	}
	if (debug() && m->mWaterGun && now - s_test.lastTank >= 1000) {
		s_test.lastTank = now;
		fprintf(stderr, "[multiplayer] test: tank %d of %d\n", (int)m->mWaterGun->mCurrentWater, (int)m->mWaterGun->getMaxWater());
	}
	if (s_test.stompAt >= 0 && !s_test.stomped && secs >= s_test.stompAt) {
		const int t = nearest(m, -1);
		if (t >= 0) {
			s_test.stomped = true;
			m->mPosition.x = MpPuppet(t)->mPosition.x;
			m->mPosition.y = MpPuppet(t)->mPosition.y + 400.0f;
			m->mPosition.z = MpPuppet(t)->mPosition.z;
			m->changePlayerStatus(MARIO_STATUS_JUMP, 0, false);
			m->mVel.y = 0.0f;
			fprintf(stderr, "[multiplayer] test: dropped onto %s's head\n", mpg_display_name(t));
		}
	}
}

} // namespace

void MpPvpPadRead(void* status)
{
	if (s_test.botOn) {
		PADStatus& pad   = static_cast<PADStatus*>(status)[0];
		pad.button       = s_test.botButtons;
		pad.stickX       = s_test.botStickX;
		pad.stickY       = s_test.botStickY;
		pad.triggerRight = s_test.botTrigger;
		pad.substickX = pad.substickY = 0;
		return;
	}
	if (!s_test.hunting)
		return;
	PADStatus& pad   = static_cast<PADStatus*>(status)[0];
	pad.button      |= PAD_TRIGGER_R;
	pad.triggerRight = 255;
	pad.stickX = pad.stickY = 0;
}

u16 MpPvpWaterTag(int slot)
{
	return slot >= 0 && slot < MPG_MAX_PLAYERS ? (u16)((slot + 1) << kOwnerShift) : 0;
}

void MpPvpStageSetup()
{
	memset(s_splashed, 0, sizeof s_splashed);
	memset(s_hits, 0, sizeof s_hits);
	s_marioFogged     = false;
	s_test.stageAt    = clockMs();
	s_test.overlapped = s_test.stomped = s_test.outDone = false;
	memset(&s_bounds, 0, sizeof s_bounds); // the commands of the session come again
	s_ringR      = -1.0f;
	s_usePercent = 100;
	s_lastWater  = -1;
	s_useCarry   = 0.0f;
}

void MpPvpBounds(const MpgCmd& c)
{
	const unsigned now = clockMs();
	const f32 was      = s_bounds.on ? boundsRadius(now) : c.f[3];
	s_bounds.on        = c.f[3] > 0.0f && c.f[3] == c.f[3];
	s_bounds.ring      = (c.i[0] & MPG_BOUNDS_RING) != 0;
	s_bounds.at[0] = c.f[0], s_bounds.at[1] = c.f[1], s_bounds.at[2] = c.f[2];
	s_bounds.from  = was;
	s_bounds.to    = c.f[3];
	s_bounds.since = now;
	s_bounds.ms    = c.f[4] > 0.0f ? (unsigned)(c.f[4] < 600.0f ? c.f[4] * 1000.0f : 600000.0f) : 0;
	const u32 rgba = c.i[1] ? (u32)c.i[1] : 0xE81E1EE6u;
	s_bounds.colour.r = (u8)(rgba >> 24), s_bounds.colour.g = (u8)(rgba >> 16);
	s_bounds.colour.b = (u8)(rgba >> 8), s_bounds.colour.a = (u8)rgba;
	if (debug())
		fprintf(stderr, "[multiplayer] arena: %.0f around %.0f %.0f %.0f%s, over %u ms\n", c.f[3], c.f[0], c.f[1], c.f[2],
		        s_bounds.ring ? ", drawn" : "", s_bounds.ms);
}

void MpPvpWaterUse(int percent)
{
	s_usePercent = percent < 0 ? 0 : percent > 1000 ? 1000 : percent;
	s_useCarry   = 0.0f;
}

void MpPvpWaterSet() { s_waterSet = true; }

int MpPvpRefill(void* mario, int source, void* from)
{
	(void)source;
	(void)from;
	return mario && mario == gpMarioOriginal && mpg_role() != MPG_ROLE_OFF && (MpServerRules() & MPG_RULE_NO_REFILL);
}

void MpPvpTick()
{
	if (mpg_role() == MPG_ROLE_OFF || MpServerDedicated())
		return;
	TMario* m = player();
	if (getenv("SMS_MULTIPLAYER_TEST"))
		testTick(m);
	if (m && mpg_collision())
		collide(m);
	if (m && s_bounds.on)
		keepIn(m); // the arena's edge last: it wins over a push
	waterUse(m);
	if (MpServerRules() & MPG_RULE_WATER_HITS)
		water(m);
}

void MpPvpSplash(const MpgCmd& c)
{
	const int slot = c.i[0];
	if (slot < 0 || slot >= MPG_MAX_PLAYERS)
		return;
	Splashed& s       = s_splashed[slot];
	const unsigned now = clockMs();
	s.full            = c.i[2] > 0 ? c.i[2] : 0;
	s.meter           = c.i[1] < 0 ? 0 : c.i[1] > s.full ? s.full : c.i[1];
	s.meterUntil      = s.full ? now + kMeterMs : now;
	const bool quiet  = (c.i[3] & MPG_SPLASH_QUIET) != 0;
	if (!quiet) {
		// how long the flush lasts: the command's f[3] seconds, else a moment
		const unsigned flush = c.f[3] > 0.0f ? (unsigned)(c.f[3] < 10.0f ? c.f[3] * 1000.0f : 10000.0f) : kFlushMs;
		s.flushUntil         = now + flush;
		JGeometry::TVec3<f32> at(c.f[0], c.f[1], c.f[2]);
		if (gpMarioParticleManager)
			gpMarioParticleManager->emit(PARTICLE_MS_ENM_WATHIT, &at, 0, nullptr);
		if (!MpServerDedicated())
			SMSGetMSound()->startSoundSet(MSD_SE_EN_COMMON_W_HIT_OK, soundAt(c.f), 0, 30.0f, 0, 0, 4);
	}
	if ((c.i[3] & MPG_SPLASH_DOWN) && slot == mpg_slot())
		if (TMario* m = player())
			knockDown(m, c.f);
	if (debug())
		fprintf(stderr, "[multiplayer] %s splashed: meter %d of %d%s\n", mpg_display_name(slot), s.meter, s.full,
		        c.i[3] & MPG_SPLASH_DOWN ? ", down" : "");
}

bool MpPvpFlushed(int slot)
{
	return slot >= 0 && slot < MPG_MAX_PLAYERS && !after(clockMs(), s_splashed[slot].flushUntil);
}

void MpPvpFog(J3DModelData* data, bool on, const f32* at, JDrama::TGraphics* graphics)
{
	if (!data)
		return;
	if (on)
		fogModel(data, at, graphics);
	else
		SMS_ResetDamageFogEffect(data);
}

void MpPvpEntry(JDrama::TGraphics* graphics)
{
	TMarDirector* dir = gpMarDirector;
	if (!dir || s_entryFrame == (unsigned)dir->unk58 + 1u)
		return;
	s_entryFrame = (unsigned)dir->unk58 + 1u;
	// our own Mario, when the game draws him (we play Mario): after his own
	// entry, which set his fog as his own damage has it
	TMario* m = gpMarioOriginal;
	const int me = mpg_slot();
	if (!m || MpPuppet(me) || !m->getM3UModel())
		return;
	const bool on = MpPvpFlushed(me);
	if (!on && !s_marioFogged)
		return;
	if (!on) { // his own fog again (TMario::addDamageFog resets it each frame he is not hurt)
		s_marioFogged = false;
		return;
	}
	s_marioFogged = true;
	const f32 at[3] = {m->mPosition.x, m->mPosition.y, m->mPosition.z};
	MpPvpFog(m->getM3UModel()->getModel()->getModelData(), true, at, graphics);
	if (m->mCap)
		MpPvpFog(m->mCap->unkC->getModelData(), true, at, graphics);
}

void MpPvpDraw(JDrama::TGraphics* graphics)
{
	TMarDirector* dir = gpMarDirector;
	if (!dir || s_drawFrame == (unsigned)dir->unk58 + 1u)
		return;
	s_drawFrame       = (unsigned)dir->unk58 + 1u;
	const unsigned now = clockMs();
	const int me       = mpg_slot();
	bool drew          = false;
	if (s_bounds.on && s_bounds.ring) {
		drawRing(graphics->getViewMtx());
		drew = true;
	}
	for (int slot = 0; slot < MPG_MAX_PLAYERS; slot++) {
		const Splashed& s = s_splashed[slot];
		if (!s.full || after(now, s.meterUntil))
			continue;
		f32 at[3];
		if (slot == me) {
			TMario* m = player();
			if (!m)
				continue;
			at[0] = m->mPosition.x, at[1] = m->mPosition.y, at[2] = m->mPosition.z;
		} else if (TMpPuppet* p = other(slot)) {
			at[0] = p->mPosition.x, at[1] = p->mPosition.y, at[2] = p->mPosition.z;
		} else {
			continue;
		}
		meter(graphics->getViewMtx(), at, (f32)s.meter / (f32)s.full);
		drew = true;
	}
	if (drew)
		GXSetCullMode(GX_CULL_BACK);
}
