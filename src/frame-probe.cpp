/*
 * Is a video feed actually arriving? For the BGH LIVE Controller's slideshow "from the projector" (NDI).
 *
 * A still slide never changes, and an NDI source can keep showing its last picture after the feed stops, so the
 * picture can't tell a live feed from a dead one. Frames can: a working NDI feed keeps delivering frames even when
 * the slide is still. This counts them with a small hidden filter on the source, which passes every frame through
 * untouched.
 *
 * Controller command (obs-websocket vendor "church-controller"):
 *   GetSourceFrames { sourceName, deep } → { ok, source, fps, showing, started }
 *     sourceName  the source to check. With deep, a scene (or group) is searched for the feed inside it: its first
 *                 NDI source, otherwise its first source that delivers its own frames.
 *     fps         frames per second arriving since the last ask (-1 on the first ask: counting has just started)
 *     showing     whether the feed is in a scene on Program or Preview right now (some NDI sources disconnect
 *                 while they aren't shown, so no frames then doesn't have to mean a problem)
 * Several feeds can be watched at once (the projector, and Camera 2 to know whether a camera is plugged in), each with
 * its own count. The filter is only added after the controller asks, is removed from a feed nobody has asked about for
 * 30 seconds, and is taken off before the scene collection changes or OBS closes, so it isn't saved with the
 * collection.
 */
#include <obs-module.h>
#include <util/platform.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "obs-websocket-api.h"
#include "sources.h"

namespace {

const char *PROBE_ID = "bgh_frame_probe";
const char *PROBE_NAME = "BGH feed check";

struct Probe {
	std::atomic<uint64_t> frames{0};
};

const char *probeName(void *)
{
	return "BGH feed check";
}
void *probeCreate(obs_data_t *, obs_source_t *)
{
	return new Probe();
}
void probeDestroy(void *data)
{
	delete static_cast<Probe *>(data);
}
// Every frame the source delivers passes through here unchanged
struct obs_source_frame *probeFrame(void *data, struct obs_source_frame *frame)
{
	static_cast<Probe *>(data)->frames.fetch_add(1);
	return frame;
}

// The feeds being watched
struct Watch {
	obs_weak_source_t *weak = nullptr; // the source with the probe
	obs_source_t *filter = nullptr;    // the probe (we hold a reference)
	uint64_t lastFrames = 0;
	uint64_t lastAsk = 0; // os_gettime_ns
};
std::mutex lock;
std::vector<Watch *> watching;

void stopWatch(Watch *w)
{
	obs_source_t *s = obs_weak_source_get_source(w->weak);
	if (s) {
		obs_source_filter_remove(s, w->filter);
		obs_source_release(s);
	}
	obs_source_release(w->filter);
	obs_weak_source_release(w->weak);
	delete w;
}
void stopWatching()
{
	for (Watch *w : watching)
		stopWatch(w);
	watching.clear();
}

bool deliversFrames(obs_source_t *s)
{
	return (obs_source_get_output_flags(s) & OBS_SOURCE_ASYNC_VIDEO) == OBS_SOURCE_ASYNC_VIDEO;
}

struct Find {
	obs_source_t *ndi = nullptr;
	obs_source_t *any = nullptr;
};
bool findFeed(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *f = static_cast<Find *>(param);
	obs_source_t *s = obs_sceneitem_get_source(item);
	if (!s)
		return true;
	obs_scene_t *inner = obs_scene_from_source(s);
	if (!inner)
		inner = obs_group_from_source(s);
	if (inner) {
		obs_scene_enum_items(inner, findFeed, param);
		return !f->ndi;
	}
	const char *id = obs_source_get_unversioned_id(s);
	if (!f->ndi && id && strstr(id, "ndi") && deliversFrames(s))
		f->ndi = s;
	else if (!f->any && deliversFrames(s))
		f->any = s;
	return !f->ndi;
}

void reqGetSourceFrames(obs_data_t *request, obs_data_t *response, void *)
{
	const char *name = obs_data_get_string(request, "sourceName");
	const bool deep = obs_data_get_bool(request, "deep");
	std::lock_guard<std::mutex> g(lock);
	obs_source_t *src = name && *name ? obs_get_source_by_name(name) : nullptr;
	if (!src) {
		obs_data_set_bool(response, "ok", false);
		obs_data_set_string(response, "code", "NO_SOURCE");
		return;
	}
	// The feed itself: the source, or (deep) the NDI source inside the scene
	obs_source_t *feed = nullptr;
	obs_scene_t *scene = obs_scene_from_source(src);
	if (!scene)
		scene = obs_group_from_source(src);
	if (scene && deep) {
		Find f;
		obs_scene_enum_items(scene, findFeed, &f);
		feed = f.ndi ? f.ndi : f.any;
		if (feed)
			feed = obs_source_get_ref(feed);
	} else if (deliversFrames(src)) {
		feed = obs_source_get_ref(src);
	}
	obs_source_release(src);
	if (!feed) {
		obs_data_set_bool(response, "ok", false);
		obs_data_set_string(response, "code", "NO_FEED");
		return;
	}
	obs_data_set_bool(response, "ok", true);
	obs_data_set_string(response, "source", obs_source_get_name(feed));
	obs_data_set_bool(response, "showing", obs_source_showing(feed));
	const uint64_t now = os_gettime_ns();
	Watch *w = nullptr;
	for (Watch *x : watching)
		if (obs_weak_source_references_source(x->weak, feed)) {
			w = x;
			break;
		}
	if (w) {
		Probe *p = static_cast<Probe *>(obs_obj_get_data(w->filter));
		const uint64_t frames = p ? p->frames.load() : 0;
		const double secs = (double)(now - w->lastAsk) / 1e9;
		const double fps = secs > 0.2 ? (double)(frames - w->lastFrames) / secs : 0.0;
		w->lastFrames = frames;
		w->lastAsk = now;
		obs_data_set_int(response, "fps", (long long)std::lround(fps));
		obs_data_set_bool(response, "started", false);
	} else {
		w = new Watch();
		w->weak = obs_source_get_weak_source(feed);
		w->filter = obs_source_create_private(PROBE_ID, PROBE_NAME, nullptr);
		obs_source_filter_add(feed, w->filter);
		w->lastAsk = now;
		watching.push_back(w);
		obs_data_set_int(response, "fps", -1);
		obs_data_set_bool(response, "started", true);
	}
	obs_source_release(feed);
}

} // namespace

void frame_probe_load(void)
{
	struct obs_source_info info = {};
	info.id = PROBE_ID;
	info.type = OBS_SOURCE_TYPE_FILTER;
	// Sees each frame a source delivers; never offered in OBS's filter menu
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_CAP_DISABLED;
	info.get_name = probeName;
	info.create = probeCreate;
	info.destroy = probeDestroy;
	info.filter_video = probeFrame;
	obs_register_source(&info);
}

void frame_probe_register_requests(obs_websocket_vendor vendor)
{
	obs_websocket_vendor_register_request(vendor, "GetSourceFrames", reqGetSourceFrames, nullptr);
}

// A feed nobody has asked about for 30 seconds (the slideshow is off, or the controller closed): its probe comes off.
// Checked every few seconds from OBS's tick.
static float sinceCheck = 0.0f;
static void probeTick(void *, float seconds)
{
	sinceCheck += seconds;
	if (sinceCheck < 5.0f)
		return;
	sinceCheck = 0.0f;
	std::unique_lock<std::mutex> g(lock, std::try_to_lock);
	if (!g.owns_lock())
		return;
	const uint64_t now = os_gettime_ns();
	for (auto it = watching.begin(); it != watching.end();) {
		if (now - (*it)->lastAsk > 30ULL * 1000000000ULL) {
			stopWatch(*it);
			it = watching.erase(it);
		} else {
			++it;
		}
	}
}
void frame_probe_start_ticks(void)
{
	obs_add_tick_callback(probeTick, nullptr);
}

// Before the scene collection changes or OBS closes, so the probe is never saved in the collection
void frame_probe_unload(void)
{
	std::lock_guard<std::mutex> g(lock);
	stopWatching();
}
void frame_probe_stop_ticks(void)
{
	obs_remove_tick_callback(probeTick, nullptr);
}
