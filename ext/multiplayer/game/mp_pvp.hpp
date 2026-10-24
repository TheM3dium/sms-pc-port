// Players against each other (mp_pvp.cpp): players colliding (mpg_collision),
// FLUDD's water hitting the other players (MPG_RULE_WATER_HITS) and a splash
// shown as Shadow Mario shows his (MPG_CMD_SPLASH). Game code: C++03.
#ifndef SMS_MP_PVP_HPP
#define SMS_MP_PVP_HPP

#include <dolphin/types.h>

namespace JDrama {
class TGraphics;
}
class J3DModelData;
struct MpgCmd;

// A new stage: nobody splashed.
void MpPvpStageSetup();
// After each movement tick in a stage (MpServerTick): this game's player kept
// out of the others, and its water's hits on them found and reported.
void MpPvpTick();
// MPG_CMD_SPLASH, carried out.
void MpPvpSplash(const MpgCmd& c);
// Player `slot`'s model flushed now (Shadow Mario's after his water hits).
bool MpPvpFlushed(int slot);
// The damage fog on a model of player `slot`'s, as Shadow Mario's (in CUE_ENTRY,
// before its models are entered): on while flushed, off after.
void MpPvpFog(J3DModelData* data, bool on, const f32* at, JDrama::TGraphics* graphics);
// Once a frame from the players' group (a puppet's perform; each call after the
// first in a frame does nothing): this game's own Mario's fog (CUE_ENTRY,
// after his own entry) and every player's water meter (the translucent pass).
void MpPvpEntry(JDrama::TGraphics* graphics);
void MpPvpDraw(JDrama::TGraphics* graphics);
// The drawn water of player `slot` (MpEmitWater): its drops carry its slot, so
// they hit everyone but them.
u16 MpPvpWaterTag(int slot);
// MPG_CMD_BOUNDS, carried out: the arena's edge.
void MpPvpBounds(const MpgCmd& c);
// MPG_PLAYER_WATER_USE: FLUDD's water used at `percent` of its own rate.
void MpPvpWaterUse(int percent);
// The server set the tank (MPG_PLAYER_WATER, ADD_WATER): not FLUDD's own use.
void MpPvpWaterSet();
// The fluddRefill hook: 1 while MPG_RULE_NO_REFILL holds for this game's Mario.
int MpPvpRefill(void* mario, int source, void* from);
// Test aids' controller (SMS_MULTIPLAYER_TEST pvphunt@: R held down).
void MpPvpPadRead(void* status);

#endif
