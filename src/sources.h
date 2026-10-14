#pragma once

/* The Church Controller plug-in's OBS sources. See lower-third.cpp and text-display.cpp. */

#include "obs-websocket-api.h"

// Call from obs_module_load(): registers the sources with OBS.
void lower_third_load(void);
void text_display_load(void);

// Call from obs_module_post_load() once the vendor exists: adds each source's commands to obs-websocket.
void lower_third_register_requests(obs_websocket_vendor vendor);
void text_display_register_requests(obs_websocket_vendor vendor);
// The microphone's level before mute and volume, for the controller's Ready check (audio-level.cpp)
void audio_level_register_requests(obs_websocket_vendor vendor);
void audio_level_unload(void);
