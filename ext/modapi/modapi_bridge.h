// The native mod API's two halves (ext/modapi/README.md): the game half
// (game/, built into the game's library with its flags and headers) reads and
// changes the game and turns the extension hooks into mod events; the host
// half (host/, built into the executable) loads the mods, keeps their
// subscriptions, settings and drawing, and hands them the SmsApi tables. This
// header is all the two share: plain C, the public SDK's types.
#ifndef MODAPI_BRIDGE_H
#define MODAPI_BRIDGE_H

#include <sms_mod.h> /* the mod SDK: sdk/include, the sdk/ submodule */

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------- host half

// The top of every frame: loads the mods the first time, then sends
// SMS_EV_FRAME and SMS_EV_DRAW and hands the frame's drawing to the window.
void modh_frame(uint32_t frame, int mode);
// Sends an event to its subscribers; returns 1 if a handler cancelled it.
// Consumable events stop at the first handler that consumes them.
int modh_dispatch(uint32_t type, void* data, uint32_t size);
// 1 when a mod listens for `type` (to skip building payloads nobody reads).
int modh_wants(uint32_t type);
// The controllers were read: keeps them as read (input->pad), sends
// SMS_EV_PAD_READ, and blanks them for the game while a mod has the input.
void modh_pad_read(SmsPadStatus* pads);
// A cancelled SMS_EV_SAVE_STATE_REQUEST's reason, kept until the next.
const char* modh_save_state_reason(void);

// ---------------------------------------------------------------- game half

void modg_state(SmsGameState* out);
int32_t modg_flag_get(uint32_t id);
void modg_flag_set(uint32_t id, int32_t value);
int modg_warp(int area, int episode);
int modg_cube_count(int set);
int modg_in_cube(int set, int index, float x, float y, float z); /* -1: no such cube */
SmsObject modg_find_object(const char* name);
const char* modg_object_name(SmsObject obj);
int modg_object_position(SmsObject obj, SmsVec3* out);
int modg_object_set_position(SmsObject obj, const SmsVec3* pos);

int modg_player_get(SmsPlayer* out);
int modg_player_set_position(const SmsVec3* pos);
int modg_player_set_velocity(const SmsVec3* vel);
int modg_player_set_forward_speed(float speed);
int modg_player_set_health(int health);
int modg_player_set_water(int water);
int modg_player_set_status(uint32_t status, uint32_t arg);

int modg_camera_get(SmsCamera* out);

int modg_world_area_kind(int area);
int modg_world_of(int area);
int modg_world_episode_count(int area);
int modg_world_episode_shine(int area, int episode);
int modg_world_episode_name(int area, int episode, char* out, int size);
int modg_world_next_episode(int area);
int modg_world_shine_episode(int shine, int* area);
int modg_world_has_cutscene(const char* name);
int modg_world_play_cutscene(const char* name, const SmsVec3* focus, int frames);
int modg_world_cutscene_playing(void);
void modg_play_system_sound(uint32_t id);
void* modg_native(const char* name);

#ifdef __cplusplus
}
#endif

#endif
