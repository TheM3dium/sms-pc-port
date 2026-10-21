// Servers' game side (mp_server.hpp): MPG_CMD_SPAWN, new objects in the stage
// played. A copy is made as the stage's loader made the original, from its
// record (scene.bin's, kept here as the loader read it: the record patch on
// TNameRef::genObject), with a new place: its manager takes it, its hit group
// has it checked against Mario, and it moves, animates and draws as the rest.
// An item (a coin, a 1-up) is made as the game makes them during play
// (TItemManager). Every game makes the same, in the same order, so a copy has
// the same index in its manager everywhere (multiplayer's item and enemy sync
// go by it), and the server names it by its id. Game code: C++03, fixed arrays.
#include "mp_server.hpp"

#include "../mp_game_api.h"

#include <JSystem/JDrama/JDRNameRef.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <JSystem/JSupport/JSUMemoryInputStream.hpp>
#include <MoveBG/Item.hpp>
#include <MoveBG/ItemManager.hpp>
#include <MoveBG/MapObjBase.hpp>
#include <MoveBG/MapObjManager.hpp>
#include <Strategic/LiveActor.hpp>
#include <Strategic/LiveManager.hpp>
#include <Strategic/ObjManager.hpp>
#include <Strategic/Strategy.hpp>
#include <Enemy/Conductor.hpp>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

const int kMostRecords   = 4096;     // the stage's objects made from records
const int kMostSpawned   = 128;      // copies and items a stage may get
const int kMostSolid     = 12;       // copies with map collision (its pools are the map's, fixed)
const u32 kHeapRoom      = 2u << 20; // free on the stage's heap before making one
const u32 kCoinType      = 0x2000000E;

struct Record {
	const JDrama::TNameRef* object;
	const u8* bytes;
	u32 size;
};
Record s_records[kMostRecords];
int s_recordCount;
bool s_fresh = true;  // the next record starts a new stage's list
bool s_copying;       // our own copies' records are not the stage's

struct Spawned {
	int id;
	TLiveActor* actor;
};
Spawned s_spawned[kMostSpawned];
int s_spawnedCount, s_solidCount;

u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
u16 be16(const u8* p) { return (u16)(p[0] << 8 | p[1]); }
void putF32(u8* p, f32 v)
{
	u32 u;
	memcpy(&u, &v, 4);
	p[0] = (u8)(u >> 24), p[1] = (u8)(u >> 16), p[2] = (u8)(u >> 8), p[3] = (u8)u;
}

const Record* recordOf(const TLiveActor* a)
{
	const JDrama::TNameRef* ref = a;
	for (int k = 0; k < s_recordCount; k++)
		if (s_records[k].object == ref)
			return &s_records[k];
	return nullptr;
}

// Where a record's place is: after its size, its type (key, string) and its
// name (key, string): a placement's three floats, then (an actor's) its turn.
// 0 when the record is too short for one.
u32 placeAt(const u8* r, u32 size)
{
	u32 at = 4 + 2;
	if (at + 2 > size)
		return 0;
	at += 2 + be16(r + at); // the type
	at += 2;                // the name's key
	if (at + 2 > size)
		return 0;
	at += 2 + be16(r + at); // the name
	return at + 24 <= size ? at : 0;
}

// The hit group (gpStrategy's) the original is checked in.
TIdxGroupObj* groupOf(const TLiveActor* a)
{
	if (!gpStrategy)
		return nullptr;
	for (int i = 0; i < 16; i++) {
		TIdxGroupObj* g = gpStrategy->unk10[i];
		if (!g)
			continue;
		JGadget::TList_pointer<THitActor*>& list = g->getChildren();
		for (JGadget::TList_pointer<THitActor*>::iterator it = list.begin(); it != list.end(); ++it)
			if (*it == a)
				return g;
	}
	return nullptr;
}

// Room in a manager's array for one more (its size is the stage's own count).
void roomIn(TLiveManager* manager)
{
	TObjManager* m = manager;
	if (!m || m->mObjNum < m->mCapacity)
		return;
	const int more = m->mCapacity + (m->mCapacity < 8 ? 8 : m->mCapacity / 2);
	THitActor** list = new THitActor*[more];
	memcpy(list, m->unk18, sizeof(THitActor*) * m->mObjNum);
	m->unk18     = list; // the old one stays on the stage's heap, as everything there does
	m->mCapacity = more;
}

TLiveActor* pick(const MpgCmd& c)
{
	struct Pick {
		const MpgCmd* c;
		int seen;
		TLiveActor* found;
		void operator()(TLiveActor* a)
		{
			if (found || MpSpawnedId(a))
				return; // copies are not originals
			if (c->text[0]) {
				TMapObjBase* o = dynamic_cast<TMapObjBase*>(a);
				if (!o || !o->unkF4 || strcmp(o->unkF4, c->text) != 0)
					return;
			}
			if (seen++ == (c->i[2] > 0 ? c->i[2] : 0))
				found = a;
		}
	} p = {&c, 0, nullptr};
	if (!gpConductor)
		return nullptr;
	for (JGadget::TList<TLiveManager*>::iterator it = gpConductor->unk10.begin(); it != gpConductor->unk10.end(); ++it)
		for (int i = 0; *it && i < (*it)->getObjNum(); i++) {
			TLiveActor* a = (*it)->getObj(i);
			const u32 type = a ? (u32)a->getActorType() : 0;
			if (a && type >= (u32)c.i[0] && type <= (u32)c.i[1])
				p(a);
		}
	return p.found;
}

// A copy of the original: its record again with the new place, made and
// loaded as the stage's loader did. Null when it cannot be.
TLiveActor* copyOf(TLiveActor* orig, const MpgCmd& c)
{
	const Record* rec = recordOf(orig);
	if (!rec) {
		fprintf(stderr, "[server] spawn: no record for that object (made by the game, not the stage)\n");
		return nullptr;
	}
	const u32 place = placeAt(rec->bytes, rec->size);
	if (!place || be32(rec->bytes) != rec->size)
		return nullptr;
	TMapObjBase* o = dynamic_cast<TMapObjBase*>(orig);
	const bool solid = o && o->mMapCollisionManager;
	if (solid && s_solidCount >= kMostSolid) {
		fprintf(stderr, "[server] spawn: no more copies with collision this stage (%d)\n", kMostSolid);
		return nullptr;
	}
	u8* bytes = new u8[rec->size]; // kept: a loader may keep what it read
	memcpy(bytes, rec->bytes, rec->size);
	putF32(bytes + place, c.f[0]);
	putF32(bytes + place + 4, c.f[1]);
	putF32(bytes + place + 8, c.f[2]);
	putF32(bytes + place + 16, c.f[3]); // its turn about the vertical
	roomIn(orig->getManager());
	JSUMemoryInputStream whole(bytes, rec->size), body(nullptr, 0);
	s_copying = true;
	JDrama::TNameRef* made = JDrama::TNameRef::genObject(whole, body);
	TLiveActor* copy       = made ? dynamic_cast<TLiveActor*>(made) : nullptr;
	if (copy) {
		copy->load(body);
		copy->loadAfter();
	}
	s_copying = false;
	if (!copy)
		return nullptr;
	if (TIdxGroupObj* g = groupOf(orig))
		g->getChildren().push_back(copy);
	if (solid)
		s_solidCount++;
	return copy;
}

// An item by its name, made as the game makes them during play: only one the
// stage has the model of, or the game would make a model of nothing: coins
// (every stage has them), and another only while one of its kind is in the
// stage (its manager has loaded the model).
TLiveActor* itemNamed(const char* name, const MpgCmd& c)
{
	if (!strcmp(name, "1up"))
		name = "mushroom1up";
	static const char* const kItems[] = {"coin",          "coin_red",    "mushroom1up", "mushroom1upR", "mushroom1upX",
	                                     "bottle_large",  "bottle_short", "FruitCoconut", "FruitPine",   "FruitDurian",
	                                     "FruitPapaya",   "FruitBanana",  nullptr};
	bool known = false;
	for (int i = 0; kItems[i]; i++)
		known = known || !strcmp(name, kItems[i]);
	if (!known || !gpItemManager) {
		fprintf(stderr, "[server] spawn: no item called %s\n", name);
		return nullptr;
	}
	const JGeometry::TVec3<f32> at(c.f[0], c.f[1], c.f[2]), turn(0.0f, c.f[3], 0.0f), size(1.0f, 1.0f, 1.0f);
	if (!strcmp(name, "coin")) {
		TCoin* coin = gpItemManager->newAndRegisterCoinReal();
		if (!coin)
			return nullptr;
		coin->mPosition = at;
		coin->mRotation = turn;
		coin->appear(); // as the game brings one out: its sparkle, its sound
		return coin;
	}
	// its model: an object of the kind in the stage, or the archive's
	struct Have {
		const char* name;
		bool found;
		void operator()(TLiveActor* a)
		{
			TMapObjBase* o = dynamic_cast<TMapObjBase*>(a);
			found = found || (o && o->unkF4 && !strcmp(o->unkF4, name));
		}
	} have = {name, false};
	for (JGadget::TList<TLiveManager*>::iterator it = gpConductor->unk10.begin(); it != gpConductor->unk10.end(); ++it)
		for (int i = 0; *it && i < (*it)->getObjNum() && !have.found; i++)
			if (TLiveActor* a = (*it)->getObj(i))
				have(a);
	if (!have.found) {
		fprintf(stderr, "[server] spawn: this stage has no %s to copy the look of\n", name);
		return nullptr;
	}
	TMapObjBase* item = TMapObjBaseManager::newAndRegisterObj(name, at, turn, size);
	if (item)
		item->appear();
	return item;
}

} // namespace

extern "C" void port_spawn_record(void* object, const void* record, unsigned size)
{
	if (s_copying)
		return;
	if (s_fresh) {
		s_fresh       = false;
		s_recordCount = 0;
	}
	if (s_recordCount < kMostRecords) {
		Record& r = s_records[s_recordCount++];
		r.object  = (const JDrama::TNameRef*)object;
		r.bytes   = (const u8*)record;
		r.size    = size;
	}
}

void MpSpawnRecord(void* object, const void* record, unsigned size) { port_spawn_record(object, record, size); }

void MpSpawnStageSetup()
{
	// this stage's records are all in; the next ones are another stage's
	s_fresh        = true;
	s_spawnedCount = 0;
	s_solidCount   = 0;
}

TLiveActor* MpSpawned(int id)
{
	for (int k = 0; k < s_spawnedCount && id > 0; k++)
		if (s_spawned[k].id == id)
			return s_spawned[k].actor;
	return nullptr;
}

int MpSpawnedId(const TLiveActor* actor)
{
	for (int k = 0; k < s_spawnedCount; k++)
		if (s_spawned[k].actor == actor)
			return s_spawned[k].id;
	return 0;
}

void MpSpawn(const MpgCmd& c)
{
	const int id = c.i[3];
	if (id <= 0)
		return;
	// one that is there already: to the new place
	if (TLiveActor* there = MpSpawned(id)) {
		there->mPosition.set(c.f[0], c.f[1], c.f[2]);
		there->mRotation.y = c.f[3];
		if (TMapObjBase* o = dynamic_cast<TMapObjBase*>(there)) {
			o->mInitialPosition   = there->mPosition;
			o->mInitialRotation.y = c.f[3];
			if (!o->checkLiveFlag(LIVE_FLAG_DEAD))
				o->setUpCurrentMapCollision();
		}
		return;
	}
	if (s_spawnedCount >= kMostSpawned) {
		fprintf(stderr, "[server] spawn %d: no more this stage (%d)\n", id, kMostSpawned);
		return;
	}
	JKRHeap* heap = JKRHeap::getCurrentHeap();
	if (heap && (u32)heap->getTotalFreeSize() < kHeapRoom) {
		fprintf(stderr, "[server] spawn %d: the stage's heap is nearly full\n", id);
		return;
	}
	TLiveActor* made = nullptr;
	if (c.i[0] == 0 && c.i[1] == 0) {
		made = itemNamed(c.text, c);
	} else if (TLiveActor* orig = pick(c)) {
		made = copyOf(orig, c);
	} else {
		fprintf(stderr, "[server] spawn %d: no %s #%d of types %x-%x here\n", id, c.text[0] ? c.text : "object", c.i[2],
		        (unsigned)c.i[0], (unsigned)c.i[1]);
	}
	if (!made)
		return;
	s_spawned[s_spawnedCount].id    = id;
	s_spawned[s_spawnedCount].actor = made;
	s_spawnedCount++;
	fprintf(stderr, "[server] spawn %d: %s at %.0f %.0f %.0f (type %x)\n", id, c.text[0] ? c.text : "a copy", c.f[0], c.f[1],
	        c.f[2], (unsigned)made->getActorType());
}
