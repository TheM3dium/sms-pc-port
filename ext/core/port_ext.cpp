// The extension registry (src/port_include/port_ext.h): the game's hooks call
// each registered extension in the order they registered. Built into the
// game's library with the game's own flags, so it keeps to C++03 and fixed
// arrays (no allocation: the game's operator new is its JKR heaps').
#include "port_ext.h"

#include <string.h>

namespace {

enum { kMaxExtensions = 16 };
const PortExtension* s_ext[kMaxExtensions];
int s_count;

} // namespace

// The file chosen at file select, for fileStarting: as saveFile last said;
// none (-1, a new game) for one played without a memory card.
static int s_startFile = -1, s_startNew = 1;

extern "C" void port_ext_register(const PortExtension* ext)
{
	if (!ext || s_count >= kMaxExtensions)
		return;
	for (int i = 0; i < s_count; i++)
		if (s_ext[i] == ext)
			return;
	s_ext[s_count++] = ext;
}

extern "C" const PortExtension* port_ext_find(const char* name)
{
	for (int i = 0; i < s_count; i++)
		if (s_ext[i]->name && name && !strcmp(s_ext[i]->name, name))
			return s_ext[i];
	return 0;
}

#define EACH(hook) \
	for (int i = 0; i < s_count; i++) \
		if (s_ext[i]->hook)

extern "C" void port_ext_frame(void)
{
	EACH(frame) s_ext[i]->frame();
}

extern "C" void port_ext_draw(float width, float height)
{
	EACH(draw) s_ext[i]->draw(width, height);
}

extern "C" void port_ext_after_movement(void)
{
	EACH(afterMovement) s_ext[i]->afterMovement();
}

extern "C" void port_ext_stage_setup(void)
{
	EACH(stageSetup) s_ext[i]->stageSetup();
}

extern "C" int port_ext_allow_next_stage(unsigned code, void* actor)
{
	EACH(allowNextStage) if (!s_ext[i]->allowNextStage(code, actor)) return 0;
	return 1;
}

extern "C" void port_ext_move_stage(void)
{
	EACH(chooseNextStage) s_ext[i]->chooseNextStage();
	EACH(moveStage) s_ext[i]->moveStage();
}

extern "C" void port_ext_choose_stage_load(int area, int episode)
{
	EACH(chooseStageLoad) s_ext[i]->chooseStageLoad();
	EACH(stageLoading) s_ext[i]->stageLoading(area, episode);
}

extern "C" void port_ext_miss_next_area(void)
{
	EACH(missNextArea) s_ext[i]->missNextArea();
}

extern "C" int port_ext_take_game_over(void)
{
	EACH(takeGameOver) if (s_ext[i]->takeGameOver()) return 1;
	return 0;
}

extern "C" int port_ext_keep_coins_on_miss(void)
{
	EACH(keepCoinsOnMiss) if (s_ext[i]->keepCoinsOnMiss()) return 1;
	return 0;
}

extern "C" void port_ext_after_file_select(void)
{
	s_startFile = -1; // the next game's file comes with its own saveFile
	s_startNew  = 1;
	EACH(afterFileSelect) s_ext[i]->afterFileSelect();
}

extern "C" void port_ext_item_taken(void* item, void* taker)
{
	EACH(itemTaken) s_ext[i]->itemTaken(item, taker);
}

extern "C" int port_ext_allow_save_states(const char** reason)
{
	EACH(allowSaveStates) if (!s_ext[i]->allowSaveStates()) {
		if (reason)
			*reason = s_ext[i]->saveStatesOffReason ? s_ext[i]->saveStatesOffReason() : "Save states are off";
		return 0;
	}
	return 1;
}

extern "C" int port_ext_hide_player_model(void* mario)
{
	EACH(hidePlayerModel) if (s_ext[i]->hidePlayerModel(mario)) return 1;
	return 0;
}

extern "C" void port_ext_pad_read(void* status)
{
	EACH(padRead) s_ext[i]->padRead(status);
}

extern "C" int port_ext_demo_camera(const char* name, const float* pos, int frames, float angle, int option,
                                    int hasCallback, void* actor, unsigned flags)
{
	int skip = 0;
	EACH(demoCamera) skip |= s_ext[i]->demoCamera(name, pos, frames, angle, option, hasCallback, actor, flags);
	return skip;
}

extern "C" void port_ext_pollution_clean(float x, float y, float z, float size)
{
	EACH(pollutionClean) s_ext[i]->pollutionClean(x, y, z, size);
}

extern "C" void port_ext_object_move(void* object, int after)
{
	EACH(objectMove) s_ext[i]->objectMove(object, after);
}

extern "C" void port_ext_particle_emit(int how, int id, const void* at, int rx, int ry, int rz, int group,
                                       const void* owner)
{
	EACH(particleEmit) s_ext[i]->particleEmit(how, id, at, rx, ry, rz, group, owner);
}

extern "C" int port_ext_object_hit(void* object, void* sender, unsigned message)
{
	EACH(objectHit) if (s_ext[i]->objectHit(object, sender, message)) return 1;
	return 0;
}

extern "C" void port_ext_camera(void* camera, float* eye, float* at, float* fovy)
{
	EACH(camera) s_ext[i]->camera(camera, eye, at, fovy);
}

extern "C" int port_ext_player_status(void* mario, unsigned* status, unsigned* arg, int force)
{
	EACH(playerStatus) if (s_ext[i]->playerStatus(mario, status, arg, force)) return 1;
	return 0;
}

int port_ext_nozzle_asked; // C linkage: declared so in port_ext.h

extern "C" int port_ext_player_nozzle(void* mario, void* box, int* nozzle, int animate, int reason)
{
	const int asked = *nozzle;
	EACH(playerNozzle) if (s_ext[i]->playerNozzle(mario, box, nozzle, animate, reason)) {
		*nozzle = asked;
		return 1;
	}
	// A nozzle FLUDD has (TWaterGun::mNozzleList holds 0 to 5); Yoshi's (3)
	// only for getting on him, and nothing else then.
	if (*nozzle != asked && (*nozzle < 0 || *nozzle > 5 || (*nozzle == 3) != (asked == 3)))
		*nozzle = asked;
	return 0;
}

extern "C" int port_ext_player_damage(void* mario, int* amount)
{
	EACH(playerDamage) if (s_ext[i]->playerDamage(mario, amount)) return 1;
	return 0;
}

extern "C" void port_ext_shine_collected(int shine, int total)
{
	EACH(shineCollected) s_ext[i]->shineCollected(shine, total);
}

extern "C" int port_ext_save_prompt(int kind)
{
	EACH(savePrompt) if (s_ext[i]->savePrompt(kind)) return 1;
	return 0;
}

extern "C" void port_ext_player_status_changed(void* mario, unsigned from, unsigned to, unsigned arg)
{
	EACH(playerStatusChanged) s_ext[i]->playerStatusChanged(mario, from, to, arg);
}

extern "C" int port_ext_player_heal(void* mario, int* amount, int air)
{
	EACH(playerHeal) if (s_ext[i]->playerHeal(mario, amount, air)) return 1;
	return 0;
}

extern "C" void port_ext_player_death(void* mario, int cause)
{
	EACH(playerDeath) s_ext[i]->playerDeath(mario, cause);
}

extern "C" void port_ext_player_spawn(float* pos, float* yaw, int* hasPos, int* kind)
{
	EACH(playerSpawn) s_ext[i]->playerSpawn(pos, yaw, hasPos, kind);
}

int port_ext_no_coaster_start;

extern "C" void port_ext_save_file(int what, int file, int from)
{
	if (what == PORT_SAVE_FILE_LOADED || what == PORT_SAVE_FILE_NEW) {
		s_startFile = file;
		s_startNew  = what == PORT_SAVE_FILE_NEW;
	}
	EACH(saveFile) s_ext[i]->saveFile(what, file, from);
}

extern "C" int port_ext_file_starting(void)
{
	EACH(fileStarting) if (s_ext[i]->fileStarting(s_startFile, s_startNew)) return 1;
	return 0;
}

extern "C" int port_ext_nozzle_fire(void* mario, int nozzle, int how)
{
	EACH(nozzleFire) if (s_ext[i]->nozzleFire(mario, nozzle, how)) return 1;
	return 0;
}

extern "C" int port_ext_fludd_refill(void* mario, int source, void* from)
{
	EACH(fluddRefill) if (s_ext[i]->fluddRefill(mario, source, from)) return 1;
	return 0;
}

extern "C" const char* port_ext_shine_model(void* shine, void* keeper, int collected)
{
	EACH(shineModel) if (const char* name = s_ext[i]->shineModel(shine, keeper, collected)) return name;
	return 0;
}

extern "C" void port_ext_yoshi_egg(int kind, void* egg, int* present)
{
	EACH(yoshiEgg) s_ext[i]->yoshiEgg(kind, egg, present);
}

extern "C" void port_ext_episode_shine(int shineStage, int episode, int* shine)
{
	EACH(episodeShine) s_ext[i]->episodeShine(shineStage, episode, shine);
}

extern "C" int port_ext_player_message(void* mario, void* sender, unsigned message)
{
	EACH(playerMessage) if (s_ext[i]->playerMessage(mario, sender, message)) return 1;
	return 0;
}

extern "C" void port_ext_player_mount(void* mario, int mount, int on, void* object)
{
	EACH(playerMount) s_ext[i]->playerMount(mario, mount, on, object);
}

extern "C" void port_ext_player_water(void* mario, int entered, float surfaceY)
{
	EACH(playerWater) s_ext[i]->playerWater(mario, entered, surfaceY);
}

extern "C" void port_ext_yoshi(void* yoshi, int what, unsigned fruit)
{
	EACH(yoshi) s_ext[i]->yoshi(yoshi, what, fruit);
}

extern "C" void port_ext_actor_spawn(void* actor)
{
	EACH(actorSpawn) s_ext[i]->actorSpawn(actor);
}

extern "C" void port_ext_object_hit_done(void* object, void* sender, unsigned message, int result)
{
	EACH(objectHitDone) s_ext[i]->objectHitDone(object, sender, message, result);
}

extern "C" void port_ext_actor_death(void* actor, int forced)
{
	EACH(actorDeath) s_ext[i]->actorDeath(actor, forced);
}

extern "C" void port_ext_shine(void* shine, int what)
{
	EACH(shine) s_ext[i]->shine(shine, what);
}

extern "C" int port_ext_flag_change(unsigned flag, int old, int* value)
{
	const int asked = *value;
	EACH(flagChange) if (s_ext[i]->flagChange(flag, old, value)) {
		*value = asked;
		return 1;
	}
	return 0;
}

extern "C" int port_ext_sound(unsigned* id, const float* pos, int system)
{
	EACH(sound) if (s_ext[i]->sound(id, pos, system)) return 1;
	return 0;
}

extern "C" int port_ext_music(unsigned* id, int start)
{
	EACH(music) if (s_ext[i]->music(id, start)) return 1;
	return 0;
}

extern "C" void port_ext_talk(int what, void* npc, unsigned* message, unsigned flags)
{
	EACH(talk) s_ext[i]->talk(what, npc, message, flags);
}

extern "C" void port_ext_demo_camera_end(const char* name, void* actor, int forced)
{
	EACH(demoCameraEnd) s_ext[i]->demoCameraEnd(name, actor, forced);
}

extern "C" void port_ext_game_saved(int slot)
{
	EACH(gameSaved) s_ext[i]->gameSaved(slot);
}

extern "C" void port_ext_timer(int what, int time)
{
	EACH(timer) s_ext[i]->timer(what, time);
}

extern "C" void port_ext_script_call(const char* name, void* owner, int argc)
{
	EACH(scriptCall) s_ext[i]->scriptCall(name, owner, argc);
}

extern "C" void port_ext_map_event(void* event)
{
	EACH(mapEvent) s_ext[i]->mapEvent(event);
}

extern "C" int port_ext_object_stomp(void* object, void* mario)
{
	EACH(objectStomp) if (s_ext[i]->objectStomp(object, mario)) return 1;
	return 0;
}

void* port_ext_damage_source; // C linkage: declared so in port_ext.h
int port_ext_death_cause;      // C linkage: declared so in port_ext.h
