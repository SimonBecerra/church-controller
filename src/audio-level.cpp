/*
 * The microphone's level as it comes in, for the BGH LIVE Controller's Ready check.
 *
 * OBS's own meters (and obs-websocket's InputVolumeMeters) read zero while an input is muted, so a muted mic
 * looked silent even when the mixer was sending sound. This listens to the input's audio before OBS applies
 * its mute and volume, so the controller can tell that sound is arriving while the mic is still muted.
 *
 * Controller command (obs-websocket vendor "church-controller"):
 *   GetInputLevel { inputName } → { ok, peakDb }   the loudest moment since the last ask, in dBFS (-90 = silence)
 * The first ask for an input starts listening to it and answers -90; asking for another input moves the
 * listening there. Only one input is listened to at a time, and only after the controller asks.
 */
#include <obs-module.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>

#include "obs-websocket-api.h"
#include "sources.h"

namespace {

struct Listener {
	obs_weak_source_t *weak = nullptr;
	std::string name;
	std::atomic<float> peak{0.0f}; // highest sample since the last ask, 0-1
};

std::mutex lock;
Listener *current = nullptr;

// OBS's audio thread: the input's audio before mute and volume (planar float)
void onAudio(void *param, obs_source_t *, const struct audio_data *data, bool)
{
	auto *l = static_cast<Listener *>(param);
	float p = 0.0f;
	for (size_t ch = 0; ch < MAX_AV_PLANES; ++ch) {
		const float *s = reinterpret_cast<const float *>(data->data[ch]);
		if (!s)
			continue;
		for (uint32_t i = 0; i < data->frames; ++i)
			p = std::max(p, std::fabs(s[i]));
	}
	float prev = l->peak.load();
	while (p > prev && !l->peak.compare_exchange_weak(prev, p)) {
	}
}

void stopListening()
{
	if (!current)
		return;
	obs_source_t *s = obs_weak_source_get_source(current->weak);
	if (s) {
		obs_source_remove_audio_capture_callback(s, onAudio, current);
		obs_source_release(s);
	}
	obs_weak_source_release(current->weak);
	delete current;
	current = nullptr;
}

void reqGetInputLevel(obs_data_t *request, obs_data_t *response, void *)
{
	const char *name = obs_data_get_string(request, "inputName");
	std::lock_guard<std::mutex> g(lock);
	obs_source_t *src = name && *name ? obs_get_source_by_name(name) : nullptr;
	if (!src) {
		obs_data_set_bool(response, "ok", false);
		obs_data_set_string(response, "code", "NO_INPUT");
		return;
	}
	// Still listening to the same input (the same source, not just the same name)?
	obs_source_t *was = current ? obs_weak_source_get_source(current->weak) : nullptr;
	const bool same = was == src;
	obs_source_release(was);
	float peak = 0.0f;
	if (same) {
		peak = current->peak.exchange(0.0f);
	} else {
		stopListening();
		current = new Listener();
		current->weak = obs_source_get_weak_source(src);
		current->name = name;
		obs_source_add_audio_capture_callback(src, onAudio, current);
	}
	obs_source_release(src);
	obs_data_set_bool(response, "ok", true);
	obs_data_set_double(response, "peakDb", peak > 0.0000316f ? 20.0 * std::log10(peak) : -90.0);
}

} // namespace

void audio_level_register_requests(obs_websocket_vendor vendor)
{
	obs_websocket_vendor_register_request(vendor, "GetInputLevel", reqGetInputLevel, nullptr);
}

void audio_level_unload(void)
{
	std::lock_guard<std::mutex> g(lock);
	stopListening();
}
