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
