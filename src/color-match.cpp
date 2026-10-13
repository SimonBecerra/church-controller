/*
 * Color Match filter for the Church Controller plug-in (written against OBS 32.2.2)
 *
 * Adds a video filter called "Color Match" (right-click a source > Filters > + > Color Match). It goes on the
 * target camera only, and makes that camera's colours resemble a reference camera's: white balance, tint,
 * exposure, contrast, gamma, saturation, RGB balance, shadows, midtones and highlights (and skin tones when
 * there's enough skin to measure). The reference camera is never touched, and neither are the target
 * camera's own settings: everything lives in this filter, so turning it off or deleting it undoes it all.
 *
 * Matching
 *   One-time match (the default): "Match Colors" measures both cameras once and keeps the result until the
 *     next match or Reset.
 *   Continuous: measures again every few seconds and eases toward each new result over several seconds,
 *     ignoring changes too small to see, so lighting changes are followed without flicker.
 *   Each measurement takes small pictures (256 pixels wide) of the reference and of the target as it is
 *   before this filter, works out a correction (color-match-math.cpp) and scores how confident it is.
 *   Below 60, nothing changes and the status says why. A one-time match fades in over about a second.
 *
 * Applying
 *   A small shader on the target: gains in linear light, then a tone curve, then saturation, mixed with the
 *   original by Match Strength. It runs once in OBS's video mix, so streaming and recording get the same
 *   picture, and it adds no delay. With no correction (or strength 0) the filter does nothing at all.
 *   HDR (Rec. 2100) video is passed through unchanged.
 *
 * Saved with the scene collection (the filter's settings)
 *   reference, reference_uuid, mode ("once" | "continuous"), strength (0-100),
 *   params (the correction), params_rev, state, status, confidence, matched_at
 *
 * Safety
 *   The reference must be another input (not a scene, so it can't contain the target), and can't itself
 *   have a Color Match filter (two cameras matched to each other would chase each other).
 *   A missing, black, frozen or unavailable camera is reported and the current colours are kept.
 *
 * Controller commands (obs-websocket vendor "church-controller")
 *   GetColorMatchStatus {}                                   -> { sources[{name, camera}], defaultReference,
 *                                                                defaultTarget, matches[status] }
 *   SetColorMatch { target, reference?, mode?, strength?, enabled? } -> { ok, code, message, ...status }
 *   MatchColors   { target }   -> { accepted, jobId, code, message }, then the ColorMatchResult event
 *   ResetColorMatch { target } -> { ok, code, ...status }
 *   RemoveColorMatch { target } -> { ok, code }
 *   status = { target, reference, enabled, mode, strength, state, message, confidence, applied, busy, matchedAt }
 *   codes: OK, NO_TARGET, NO_MATCH (no Color Match on that source), NO_REFERENCE, SAME_SOURCE, FEEDBACK
 * Events
 *   ColorMatchResult  { jobId, target, ok, code, message, confidence }
 *     codes: OK, NO_REFERENCE, SAME_SOURCE, FEEDBACK, NO_PICTURE, FROZEN, LOW_CONFIDENCE, REMOVED
 *   ColorMatchChanged status   (whenever a match's settings or state change)
 *
 * License: GPL-2.0-or-later (required for OBS plug-ins).
 */

#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>

#include "color-match-math.h"
#include "color-match.h"

#define CM_LOG(level, fmt, ...) blog(level, "[church-controller] color match: " fmt, ##__VA_ARGS__)

static obs_websocket_vendor cmVendor = nullptr;
static std::atomic<int> nextJob{1};
static std::atomic<int> liveWorkers{0};

static constexpr int kSampleWidth = 256;
static constexpr float kOnceFade = 0.5f;         // seconds (time constant) for a one-time match
static constexpr float kContinuousFade = 4.0f;   // seconds (time constant) in Continuous mode
static constexpr int kContinuousEveryMs = 3000;  // between measurements in Continuous mode
static constexpr float kContinuousIgnore = 2.0f; // changes smaller than this (about 8-bit steps) are ignored

static long long nowMs()
{
	return (long long)(os_gettime_ns() / 1000000ULL);
}

static long long wallMs()
{
	return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
		       std::chrono::system_clock::now().time_since_epoch())
		.count();
}

/* ------------------------------------------------------------------ state */

// Everything the measuring thread shares with the filter. The thread keeps it alive after the filter is gone.
struct Shared {
	std::mutex lock; // guards everything below
	std::condition_variable wake;
	bool quit = false;
	std::vector<std::string> jobs; // job ids waiting for a measurement
	bool busy = false;
	long long nextAuto = 0; // Continuous: when to measure next (nowMs)

	obs_weak_source_t *weakFilter = nullptr;
	std::string reference, referenceUuid;
	std::string mode = "once";
	int strength = 100;

	cm::Params goal, shown; // goal: the correction; shown: what's on screen now (fading toward goal)
	float fade = kOnceFade;
	bool fading = false;
	bool tableDirty = true;
	long long paramsRev = 0;

	std::string state = "none";
	std::string message = "Not matched yet. Press Match Colors.";
	int confidence = 0;
	long long matchedAt = 0;
};

struct Filter {
	obs_source_t *context = nullptr;
	std::shared_ptr<Shared> sh;
	gs_effect_t *effect = nullptr;
	gs_eparam_t *pImage = nullptr, *pCurve = nullptr, *pGains = nullptr, *pSat = nullptr, *pStrength = nullptr;
	gs_texture_t *curveTex = nullptr;
};

static Filter *filterOf(obs_source_t *s)
{
	return colormatch::isFilter(s) ? static_cast<Filter *>(obs_obj_get_data(s)) : nullptr;
}

/* ------------------------------------------------------------------ params <-> text */

static std::string paramsText(const cm::Params &p)
{
	std::ostringstream o;
	o.precision(5);
	o << p.gain[0] << ' ' << p.gain[1] << ' ' << p.gain[2] << ' ' << p.saturation;
	for (float c : p.curve)
		o << ' ' << c;
	return o.str();
}

static bool paramsFromText(const char *text, cm::Params &p)
{
	if (!text || !*text)
		return false;
	std::istringstream in(text);
	cm::Params q;
	in >> q.gain[0] >> q.gain[1] >> q.gain[2] >> q.saturation;
	for (float &c : q.curve)
		in >> c;
	if (in.fail())
		return false;
	for (float g : q.gain)
		if (!(g > 0.3f && g < 3.0f))
			return false;
	if (!(q.saturation > 0.3f && q.saturation < 3.0f))
		return false;
	p = q;
	return true;
}

/* ------------------------------------------------------------------ helpers */

static bool isCameraKind(const char *id)
{
	static const std::regex cam(
		"dshow_input|av_capture_input|macos-avcapture|v4l2_input|decklink-input|ndi_source");
	return id && std::regex_search(id, cam);
}

static bool usableInput(obs_source_t *s)
{
	if (obs_source_get_type(s) != OBS_SOURCE_TYPE_INPUT)
		return false;
	if (!(obs_source_get_output_flags(s) & OBS_SOURCE_VIDEO))
		return false;
	const char *id = obs_source_get_unversioned_id(s);
	return id && strncmp(id, "church_", 7) != 0 && strncmp(id, "text_", 5) != 0;
}

static std::string targetName(Shared *sh)
{
	std::string name;
	obs_source_t *f = obs_weak_source_get_source(sh->weakFilter);
	if (f) {
		if (obs_source_t *p = obs_filter_get_parent(f))
			name = obs_source_get_name(p);
		obs_source_release(f);
	}
	return name;
}

// The reference source (new reference, release it), found by its id first so a renamed source still works
static obs_source_t *findReference(Shared *sh)
{
	std::string uuid, name;
	{
		std::lock_guard<std::mutex> g(sh->lock);
		uuid = sh->referenceUuid;
		name = sh->reference;
	}
	obs_source_t *s = uuid.empty() ? nullptr : obs_get_source_by_uuid(uuid.c_str());
	if (!s && !name.empty())
		s = obs_get_source_by_name(name.c_str());
	return s;
}

static void fillStatus(Filter *f, obs_data_t *d);

static void emitChanged(Filter *f)
{
	if (!cmVendor || !f)
		return;
	obs_data_t *d = obs_data_create();
	fillStatus(f, d);
	obs_websocket_vendor_emit_event(cmVendor, "ColorMatchChanged", d);
	obs_data_release(d);
}

/* ------------------------------------------------------------------ measuring */

struct Capture {
	gs_texrender_t *tex = nullptr;
	gs_stagesurf_t *stage = nullptr;
	uint32_t cx = 0, cy = 0;
	~Capture()
	{
		if (!tex && !stage)
			return;
		obs_enter_graphics();
		gs_texrender_destroy(tex);
		gs_stagesurface_destroy(stage);
		obs_leave_graphics();
	}
};

// One small picture. `filter` set: the target as it is before the filter; otherwise `source` as OBS shows it.
static bool grab(Capture &cap, obs_source_t *source, obs_source_t *filter, cm::Frame &out)
{
	obs_source_t *sizeOf = filter ? obs_filter_get_target(filter) : source;
	const uint32_t sw = sizeOf ? obs_source_get_width(sizeOf) : 0;
	const uint32_t sh = sizeOf ? obs_source_get_height(sizeOf) : 0;
	if (!sw || !sh)
		return false;
	const uint32_t cx = kSampleWidth;
	const uint32_t cy = std::clamp<uint32_t>((uint32_t)std::lround((double)kSampleWidth * sh / sw), 16, 256);

	bool ok = false;
	obs_enter_graphics();
	if (cap.cx != cx || cap.cy != cy) {
		gs_texrender_destroy(cap.tex);
		gs_stagesurface_destroy(cap.stage);
		cap.tex = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		cap.stage = gs_stagesurface_create(cx, cy, GS_RGBA);
		cap.cx = cx;
		cap.cy = cy;
	}
	gs_texrender_reset(cap.tex);
	if (cap.tex && cap.stage && gs_texrender_begin(cap.tex, cx, cy)) {
		vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)sw, 0.0f, (float)sh, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		if (filter)
			obs_source_skip_video_filter(filter);
		else
			obs_source_video_render(source);
		gs_blend_state_pop();
		gs_texrender_end(cap.tex);

		gs_stage_texture(cap.stage, gs_texrender_get_texture(cap.tex));
		uint8_t *data = nullptr;
		uint32_t linesize = 0;
		if (gs_stagesurface_map(cap.stage, &data, &linesize)) {
			out.width = (int)cx;
			out.height = (int)cy;
			out.rgba.resize((size_t)cx * cy * 4);
			for (uint32_t y = 0; y < cy; ++y)
				memcpy(&out.rgba[(size_t)y * cx * 4], data + (size_t)y * linesize, (size_t)cx * 4);
			gs_stagesurface_unmap(cap.stage);
			ok = true;
		}
	}
	obs_leave_graphics();
	return ok;
}

// Sleep in small steps, stopping early if the filter goes away
static bool nap(Shared *sh, int ms)
{
	std::unique_lock<std::mutex> l(sh->lock);
	return !sh->wake.wait_for(l, std::chrono::milliseconds(ms), [sh] { return sh->quit; });
}

static void saveResult(obs_source_t *filterSrc, Shared *sh, bool newParams)
{
	obs_data_t *change = obs_data_create();
	{
		std::lock_guard<std::mutex> g(sh->lock);
		if (newParams) {
			obs_data_set_string(change, "params", paramsText(sh->goal).c_str());
			obs_data_set_int(change, "params_rev", sh->paramsRev);
		}
		obs_data_set_string(change, "state", sh->state.c_str());
		obs_data_set_string(change, "status", sh->message.c_str());
		obs_data_set_int(change, "confidence", sh->confidence);
		obs_data_set_int(change, "matched_at", sh->matchedAt);
	}
	obs_source_update(filterSrc, change);
	obs_data_release(change);
}

static void emitResult(const std::vector<std::string> &jobs, const std::string &target, bool ok,
		       const std::string &code, const std::string &message, int confidence)
{
	if (!cmVendor)
		return;
	for (const std::string &id : jobs) {
		obs_data_t *d = obs_data_create();
		obs_data_set_string(d, "jobId", id.c_str());
		obs_data_set_string(d, "target", target.c_str());
		obs_data_set_bool(d, "ok", ok);
		obs_data_set_string(d, "code", code.c_str());
		obs_data_set_string(d, "message", message.c_str());
		obs_data_set_int(d, "confidence", confidence);
		obs_websocket_vendor_emit_event(cmVendor, "ColorMatchResult", d);
		obs_data_release(d);
	}
}

// One measurement. `jobs` empty = an automatic one (Continuous mode).
static void measure(Shared *sh, const std::vector<std::string> &jobs)
{
	const bool automatic = jobs.empty();
	obs_source_t *filterSrc = obs_weak_source_get_source(sh->weakFilter);
	if (!filterSrc)
		return;
	Filter *self = filterOf(filterSrc);
	obs_source_t *parent = obs_filter_get_parent(filterSrc);
	const std::string tName = parent ? obs_source_get_name(parent) : "the target camera";

	auto finish = [&](bool ok, const std::string &code, const std::string &msg, int confidence, bool newParams,
			  const std::string &state) {
		{
			std::lock_guard<std::mutex> g(sh->lock);
			sh->busy = false;
			sh->confidence = confidence;
			// Continuous mode keeps showing the last good match when one measurement fails
			if (automatic && !ok && sh->state == "matched") {
				sh->message = "Holding the last match. " + msg;
			} else {
				sh->state = state;
				sh->message = msg;
			}
		}
		saveResult(filterSrc, sh, newParams);
		emitResult(jobs, tName, ok, code, msg, confidence);
		emitChanged(self);
		if (!automatic || !ok)
			CM_LOG(ok ? LOG_INFO : LOG_WARNING, "%s: %s %s", tName.c_str(), code.c_str(), msg.c_str());
	};

	obs_source_t *ref = findReference(sh);
	std::string refName, checkMsg;
	if (ref) {
		refName = obs_source_get_name(ref);
		std::lock_guard<std::mutex> g(sh->lock);
		sh->reference = refName; // follows a rename
	} else {
		std::lock_guard<std::mutex> g(sh->lock);
		refName = sh->reference;
	}
	const std::string code = colormatch::checkReference(filterSrc, refName, checkMsg);
	if (!parent || !ref || !code.empty()) {
		finish(false, code.empty() ? "NO_REFERENCE" : code,
		       checkMsg.empty() ? "The reference camera isn't available." : checkMsg, 0, false, "error");
		obs_source_release(ref);
		obs_source_release(filterSrc);
		return;
	}
	// Continuous mode only follows cameras that are in use, so it never wakes a sleeping camera every few seconds
	if (automatic && (!obs_source_showing(ref) || !obs_source_showing(parent))) {
		obs_source_release(ref);
		obs_source_release(filterSrc);
		return;
	}

	// A camera that isn't in a visible scene may be switched off ("Deactivate when not showing"):
	// keep both showing while measuring, and give a sleeping one a moment to start
	const bool refWasShowing = obs_source_showing(ref), tgtWasShowing = obs_source_showing(parent);
	obs_source_inc_showing(ref);
	obs_source_inc_showing(parent);
	bool alive = true;
	if (!refWasShowing || !tgtWasShowing)
		alive = nap(sh, 1500);

	std::vector<cm::Frame> refFrames, tgtFrames;
	const int count = automatic ? 4 : 8, gap = automatic ? 200 : 140;
	std::string noPicture; // which camera gave no picture
	{
		Capture capRef, capTgt;
		for (int i = 0; alive && i < count; ++i) {
			cm::Frame a, b;
			if (!grab(capRef, ref, nullptr, a)) {
				noPicture = refName;
				break;
			}
			if (!grab(capTgt, parent, filterSrc, b)) {
				noPicture = tName;
				break;
			}
			refFrames.push_back(std::move(a));
			tgtFrames.push_back(std::move(b));
			if (i + 1 < count)
				alive = nap(sh, gap);
		}
	}
	obs_source_dec_showing(ref);
	obs_source_dec_showing(parent);
	obs_source_release(ref);

	if (!alive) {
		emitResult(jobs, tName, false, "REMOVED", "The Color Match filter was removed.", 0);
		obs_source_release(filterSrc);
		return;
	}
	if (!noPicture.empty()) {
		finish(false, "NO_PICTURE",
		       "OBS isn't getting a picture from " + noPicture + ". Check that the camera is connected and on.",
		       0, false, "error");
		obs_source_release(filterSrc);
		return;
	}

	const cm::Result r = cm::colorMatch(refFrames, tgtFrames, refName, tName);
	if (!automatic || !r.ok)
		CM_LOG(LOG_INFO, "%s -> %s: confidence %d, %s", refName.c_str(), tName.c_str(), r.confidence,
		       r.details.c_str());

	if (!r.ok) {
		finish(false, r.code, r.message, r.confidence, false,
		       r.code == "LOW_CONFIDENCE" ? "declined" : "error");
		obs_source_release(filterSrc);
		return;
	}

	bool changed = true;
	std::string msg = r.message;
	{
		std::lock_guard<std::mutex> g(sh->lock);
		if (automatic && cm::difference(sh->goal, r.params) < kContinuousIgnore) {
			changed = false; // too small to see: leave it alone
		} else {
			sh->goal = r.params;
			sh->fade = automatic ? kContinuousFade : kOnceFade;
			sh->fading = true;
			sh->paramsRev++;
		}
		sh->matchedAt = wallMs();
		if (automatic)
			msg = "Following " + refName + " (confidence " + std::to_string(r.confidence) + ").";
	}
	if (automatic && !changed) {
		// Nothing visible changed: update the status quietly, without saving or telling the controller each time
		bool wasMatched;
		{
			std::lock_guard<std::mutex> g(sh->lock);
			wasMatched = sh->state == "matched";
			sh->busy = false;
			sh->confidence = r.confidence;
			sh->state = "matched";
			sh->message = msg;
		}
		if (!wasMatched)
			emitChanged(self);
	} else {
		finish(true, "OK", msg, r.confidence, changed, "matched");
	}
	obs_source_release(filterSrc);
}

static void worker(std::shared_ptr<Shared> sh)
{
	for (;;) {
		std::vector<std::string> jobs;
		{
			std::unique_lock<std::mutex> l(sh->lock);
			sh->wake.wait_for(l, std::chrono::milliseconds(250),
					  [&] { return sh->quit || !sh->jobs.empty(); });
			if (sh->quit)
				break;
			bool automatic = false;
			if (sh->jobs.empty() && sh->mode == "continuous" && nowMs() >= sh->nextAuto) {
				l.unlock();
				obs_source_t *f = obs_weak_source_get_source(sh->weakFilter);
				// Only while the filter is on, and not when it's been switched to strength 0
				automatic = f && obs_source_enabled(f);
				obs_source_release(f);
				l.lock();
				automatic = automatic && sh->strength > 0 && !sh->quit;
				sh->nextAuto = nowMs() + kContinuousEveryMs;
			}
			if (sh->jobs.empty() && !automatic)
				continue;
			jobs.swap(sh->jobs);
			sh->busy = true;
		}
		measure(sh.get(), jobs);
		{
			std::lock_guard<std::mutex> g(sh->lock);
			sh->busy = false;
			sh->nextAuto = nowMs() + kContinuousEveryMs;
		}
	}
	obs_weak_source_release(sh->weakFilter);
	sh->weakFilter = nullptr;
	liveWorkers--;
}

/* ------------------------------------------------------------------ filter callbacks */

static const char *cm_get_name(void *)
{
	return "Color Match";
}

static void cm_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "reference", "");
	obs_data_set_default_string(settings, "mode", "once");
	obs_data_set_default_int(settings, "strength", 100);
	obs_data_set_default_string(settings, "state", "none");
	obs_data_set_default_string(settings, "status", "Not matched yet. Press Match Colors.");
}

static void cm_update(void *data, obs_data_t *settings)
{
	auto *f = static_cast<Filter *>(data);
	Shared *sh = f->sh.get();
	const std::string ref = obs_data_get_string(settings, "reference");
	std::string uuid = obs_data_get_string(settings, "reference_uuid");
	// Chosen by name in the settings window: remember the source's id too, so a rename doesn't lose it
	if (!ref.empty()) {
		obs_source_t *byUuid = uuid.empty() ? nullptr : obs_get_source_by_uuid(uuid.c_str());
		if (!byUuid || ref != obs_source_get_name(byUuid)) {
			obs_source_t *byName = obs_get_source_by_name(ref.c_str());
			if (byName) {
				uuid = obs_source_get_uuid(byName);
				obs_data_set_string(settings, "reference_uuid", uuid.c_str());
				obs_source_release(byName);
			}
		}
		obs_source_release(byUuid);
	}
	bool changed;
	{
		std::lock_guard<std::mutex> g(sh->lock);
		const std::string mode = strcmp(obs_data_get_string(settings, "mode"), "continuous") == 0 ? "continuous"
													  : "once";
		const int strength = (int)std::clamp<long long>(obs_data_get_int(settings, "strength"), 0, 100);
		changed = ref != sh->reference || mode != sh->mode || strength != sh->strength;
		if (mode == "continuous" && sh->mode != "continuous")
			sh->nextAuto = 0; // start following straight away
		sh->reference = ref;
		sh->referenceUuid = uuid;
		sh->mode = mode;
		sh->strength = strength;

		// A correction loaded from the scene collection, or put there by something else (not by this filter)
		const long long rev = obs_data_get_int(settings, "params_rev");
		if (rev != sh->paramsRev) {
			cm::Params p;
			if (!paramsFromText(obs_data_get_string(settings, "params"), p))
				p = cm::Params();
			sh->goal = sh->shown = p;
			sh->fading = false;
			sh->tableDirty = true;
			sh->paramsRev = rev;
			sh->state = obs_data_get_string(settings, "state");
			sh->message = obs_data_get_string(settings, "status");
			sh->confidence = (int)obs_data_get_int(settings, "confidence");
			sh->matchedAt = obs_data_get_int(settings, "matched_at");
		}
	}
	if (changed)
		emitChanged(f);
}

static void *cm_create(obs_data_t *settings, obs_source_t *source)
{
	auto *f = new Filter();
	f->context = source;
	f->sh = std::make_shared<Shared>();
	f->sh->weakFilter = obs_source_get_weak_source(source);
	f->sh->paramsRev = -1; // so the first update loads the saved correction
	cm_update(f, settings);

	char *path = obs_module_file("color-match/color_match.effect");
	obs_enter_graphics();
	f->effect = path ? gs_effect_create_from_file(path, nullptr) : nullptr;
	if (f->effect) {
		f->pImage = gs_effect_get_param_by_name(f->effect, "image");
		f->pCurve = gs_effect_get_param_by_name(f->effect, "curve");
		f->pGains = gs_effect_get_param_by_name(f->effect, "gains");
		f->pSat = gs_effect_get_param_by_name(f->effect, "saturation");
		f->pStrength = gs_effect_get_param_by_name(f->effect, "strength");
	}
	obs_leave_graphics();
	bfree(path);
	if (!f->effect)
		CM_LOG(LOG_WARNING, "couldn't load color_match.effect; the filter will pass video through unchanged");

	liveWorkers++;
	std::thread(worker, f->sh).detach();
	return f;
}

static void cm_destroy(void *data)
{
	auto *f = static_cast<Filter *>(data);
	{
		std::lock_guard<std::mutex> g(f->sh->lock);
		f->sh->quit = true;
	}
	f->sh->wake.notify_all();
	obs_enter_graphics();
	gs_effect_destroy(f->effect);
	gs_texture_destroy(f->curveTex);
	obs_leave_graphics();
	delete f;
}

// Fade what's on screen toward the correction
static void cm_tick(void *data, float seconds)
{
	auto *f = static_cast<Filter *>(data);
	Shared *sh = f->sh.get();
	std::lock_guard<std::mutex> g(sh->lock);
	if (!sh->fading)
		return;
	const float t = 1.0f - std::exp(-seconds / std::max(0.05f, sh->fade));
	sh->shown = cm::blend(sh->shown, sh->goal, t);
	if (cm::difference(sh->shown, sh->goal) < 0.1f) {
		sh->shown = sh->goal;
		sh->fading = false;
	}
	sh->tableDirty = true;
}

static void cm_render(void *data, gs_effect_t *)
{
	auto *f = static_cast<Filter *>(data);
	Shared *sh = f->sh.get();

	cm::Params p;
	float strength;
	bool dirty, identity;
	{
		std::lock_guard<std::mutex> g(sh->lock);
		p = sh->shown;
		strength = sh->strength / 100.0f;
		dirty = sh->tableDirty;
		identity = !sh->fading && sh->goal.isIdentity();
	}
	const enum gs_color_space spaces[] = {GS_CS_SRGB, GS_CS_SRGB_16F, GS_CS_709_EXTENDED};
	const enum gs_color_space space =
		obs_source_get_color_space(obs_filter_get_target(f->context), OBS_COUNTOF(spaces), spaces);
	// Nothing to do: hand the picture on untouched, at no cost
	if (!f->effect || strength <= 0.0f || space == GS_CS_709_EXTENDED || identity) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	if (dirty || !f->curveTex) {
		float table[256];
		cm::curveTable(p.curve, table);
		uint16_t px[256];
		for (int i = 0; i < 256; ++i)
			px[i] = (uint16_t)std::lround(table[i] * 65535.0f);
		if (!f->curveTex) {
			const uint8_t *init = reinterpret_cast<const uint8_t *>(px);
			f->curveTex = gs_texture_create(256, 1, GS_R16, 1, &init, GS_DYNAMIC);
		} else {
			gs_texture_set_image(f->curveTex, reinterpret_cast<const uint8_t *>(px), 256 * 2, false);
		}
		std::lock_guard<std::mutex> g(sh->lock);
		if (cm::difference(sh->shown, p) < 0.01f)
			sh->tableDirty = false;
	}
	if (!f->curveTex) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	const enum gs_color_format format = gs_get_format_from_space(space);
	if (obs_source_process_filter_begin_with_color_space(f->context, format, space, OBS_ALLOW_DIRECT_RENDERING)) {
		vec3 gains;
		vec3_set(&gains, p.gain[0], p.gain[1], p.gain[2]);
		gs_effect_set_texture(f->pCurve, f->curveTex);
		gs_effect_set_vec3(f->pGains, &gains);
		gs_effect_set_float(f->pSat, p.saturation);
		gs_effect_set_float(f->pStrength, strength);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		obs_source_process_filter_end(f->context, f->effect, 0, 0);
		gs_blend_state_pop();
	}
}

static enum gs_color_space cm_color_space(void *data, size_t, const enum gs_color_space *)
{
	auto *f = static_cast<Filter *>(data);
	const enum gs_color_space spaces[] = {GS_CS_SRGB, GS_CS_SRGB_16F, GS_CS_709_EXTENDED};
	return obs_source_get_color_space(obs_filter_get_target(f->context), OBS_COUNTOF(spaces), spaces);
}

/* ------------------------------------------------------------------ settings window */

static bool cm_match_button(obs_properties_t *, obs_property_t *, void *data)
{
	colormatch::requestMatch(static_cast<Filter *>(data)->context);
	return false;
}

static bool cm_reset_button(obs_properties_t *, obs_property_t *, void *data)
{
	colormatch::reset(static_cast<Filter *>(data)->context);
	return false;
}

static bool cm_refresh_button(obs_properties_t *props, obs_property_t *, void *data)
{
	colormatch::Status st;
	if (colormatch::status(static_cast<Filter *>(data)->context, st))
		obs_property_set_description(obs_properties_get(props, "status_info"), st.message.c_str());
	return true;
}

static obs_properties_t *cm_properties(void *data)
{
	auto *f = static_cast<Filter *>(data);
	obs_properties_t *props = obs_properties_create();

	colormatch::Status st;
	if (f)
		colormatch::status(f->context, st);
	obs_properties_add_text(props, "status_info", st.message.empty() ? "Not matched yet." : st.message.c_str(),
				OBS_TEXT_INFO);

	obs_property_t *ref = obs_properties_add_list(props, "reference", "Reference camera (colours kept)",
						      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(ref, "Choose a camera", "");
	obs_source_t *parent = f ? obs_filter_get_parent(f->context) : nullptr;
	for (const colormatch::SourceInfo &s : colormatch::videoSources()) {
		if (parent && s.name == obs_source_get_name(parent))
			continue;
		obs_property_list_add_string(ref, s.name.c_str(), s.name.c_str());
	}
	obs_property_set_long_description(ref,
					  "The camera whose colours this camera should match. It's never changed.");

	obs_property_t *mode =
		obs_properties_add_list(props, "mode", "Matching mode", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(mode, "One-time match (when you press Match Colors)", "once");
	obs_property_list_add_string(mode, "Continuous (follows lighting changes)", "continuous");

	obs_property_t *strength = obs_properties_add_int_slider(props, "strength", "Match strength", 0, 100, 1);
	obs_property_int_set_suffix(strength, "%");

	obs_properties_add_button2(props, "match", "Match Colors", cm_match_button, data);
	obs_properties_add_button2(props, "reset", "Reset", cm_reset_button, data);
	obs_properties_add_button2(props, "refresh", "Refresh status", cm_refresh_button, data);
	return props;
}

/* ------------------------------------------------------------------ colormatch:: */

namespace colormatch {

bool isFilter(obs_source_t *source)
{
	const char *id = source ? obs_source_get_unversioned_id(source) : nullptr;
	return id && strcmp(id, CM_FILTER_ID) == 0;
}

std::vector<SourceInfo> videoSources()
{
	std::vector<SourceInfo> out;
	obs_enum_sources(
		[](void *param, obs_source_t *s) {
			if (usableInput(s))
				static_cast<std::vector<SourceInfo> *>(param)->push_back(
					{obs_source_get_name(s), isCameraKind(obs_source_get_unversioned_id(s))});
			return true;
		},
		&out);
	std::sort(out.begin(), out.end(), [](const SourceInfo &a, const SourceInfo &b) {
		return a.camera != b.camera ? a.camera : a.name < b.name;
	});
	return out;
}

std::string cameraInScene(const char *sceneName)
{
	std::string found;
	obs_source_t *src = obs_get_source_by_name(sceneName);
	obs_scene_t *scene = src ? obs_scene_from_source(src) : nullptr;
	if (scene) {
		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
				obs_source_t *s = obs_sceneitem_get_source(item);
				if (isCameraKind(obs_source_get_unversioned_id(s))) {
					*static_cast<std::string *>(param) = obs_source_get_name(s);
					return false;
				}
				return true;
			},
			&found);
	}
	obs_source_release(src);
	return found;
}

obs_source_t *findFilter(obs_source_t *target)
{
	if (!target)
		return nullptr;
	obs_source_t *found = nullptr;
	obs_source_enum_filters(
		target,
		[](obs_source_t *, obs_source_t *child, void *param) {
			auto **out = static_cast<obs_source_t **>(param);
			if (!*out && isFilter(child))
				*out = obs_source_get_ref(child);
		},
		&found);
	return found;
}

obs_source_t *ensureFilter(obs_source_t *target)
{
	obs_source_t *f = findFilter(target);
	if (f || !target || !usableInput(target))
		return f;
	f = obs_source_create(CM_FILTER_ID, "Color Match", nullptr, nullptr);
	if (f) {
		obs_source_filter_add(target, f);
		CM_LOG(LOG_INFO, "added to %s", obs_source_get_name(target));
	}
	return f;
}

bool status(obs_source_t *filter, Status &out)
{
	Filter *f = filterOf(filter);
	if (!f)
		return false;
	obs_source_t *parent = obs_filter_get_parent(filter);
	out.target = parent ? obs_source_get_name(parent) : "";
	out.enabled = obs_source_enabled(filter);
	std::lock_guard<std::mutex> g(f->sh->lock);
	out.reference = f->sh->reference;
	out.mode = f->sh->mode;
	out.strength = f->sh->strength;
	out.state = f->sh->busy ? "matching" : f->sh->state;
	out.message = f->sh->busy ? "Measuring both cameras…" : f->sh->message;
	out.confidence = f->sh->confidence;
	out.applied = !f->sh->goal.isIdentity();
	out.busy = f->sh->busy;
	out.matchedAt = f->sh->matchedAt;
	return true;
}

std::string checkReference(obs_source_t *filter, const std::string &reference, std::string &message)
{
	if (reference.empty()) {
		message = "Choose the reference camera first.";
		return "NO_REFERENCE";
	}
	obs_source_t *ref = obs_get_source_by_name(reference.c_str());
	std::string code;
	obs_source_t *parent = filter ? obs_filter_get_parent(filter) : nullptr;
	if (!ref) {
		code = "NO_REFERENCE";
		message = "The reference camera \"" + reference + "\" isn't in this scene collection.";
	} else if (ref == parent) {
		code = "SAME_SOURCE";
		message = "The reference and the camera being matched are the same source. Choose another reference.";
	} else if (!usableInput(ref)) {
		code = "NO_REFERENCE";
		message = "\"" + reference + "\" can't be a reference. Choose a camera.";
	} else if (obs_source_t *other = findFilter(ref)) {
		obs_source_release(other);
		code = "FEEDBACK";
		message =
			"\"" + reference +
			"\" is itself being colour matched, so it can't be the reference (the two would chase each other).";
	}
	obs_source_release(ref);
	return code;
}

void setOptions(obs_source_t *filter, const std::string &reference, const std::string &mode, int strength)
{
	if (!isFilter(filter))
		return;
	obs_data_t *change = obs_data_create();
	if (!reference.empty()) {
		obs_data_set_string(change, "reference", reference.c_str());
		if (obs_source_t *r = obs_get_source_by_name(reference.c_str())) {
			obs_data_set_string(change, "reference_uuid", obs_source_get_uuid(r));
			obs_source_release(r);
		}
	}
	if (mode == "once" || mode == "continuous")
		obs_data_set_string(change, "mode", mode.c_str());
	if (strength >= 0)
		obs_data_set_int(change, "strength", std::min(strength, 100));
	obs_source_update(filter, change);
	obs_data_release(change);
}

std::string requestMatch(obs_source_t *filter)
{
	Filter *f = filterOf(filter);
	if (!f)
		return "";
	const std::string id = std::to_string(nextJob++);
	{
		std::lock_guard<std::mutex> g(f->sh->lock);
		f->sh->jobs.push_back(id);
	}
	f->sh->wake.notify_all();
	emitChanged(f);
	return id;
}

void reset(obs_source_t *filter)
{
	Filter *f = filterOf(filter);
	if (!f)
		return;
	{
		std::lock_guard<std::mutex> g(f->sh->lock);
		f->sh->goal = cm::Params();
		f->sh->fade = kOnceFade;
		f->sh->fading = true;
		f->sh->paramsRev++;
		f->sh->state = "none";
		f->sh->message = "Reset: showing the camera's own colours.";
		f->sh->confidence = 0;
		f->sh->matchedAt = 0;
		// Continuous mode would match again straight away; reset means "stop", so switch to one-time
		f->sh->mode = "once";
	}
	obs_data_t *change = obs_data_create();
	obs_data_set_string(change, "mode", "once");
	obs_source_update(filter, change);
	obs_data_release(change);
	saveResult(filter, f->sh.get(), true);
	emitChanged(f);
	obs_source_t *parent = obs_filter_get_parent(filter);
	CM_LOG(LOG_INFO, "%s: reset", parent ? obs_source_get_name(parent) : "?");
}

} // namespace colormatch

/* ------------------------------------------------------------------ controller commands */

static void fillStatus(Filter *f, obs_data_t *d)
{
	colormatch::Status st;
	if (!f || !colormatch::status(f->context, st))
		return;
	obs_data_set_string(d, "target", st.target.c_str());
	obs_data_set_string(d, "reference", st.reference.c_str());
	obs_data_set_bool(d, "enabled", st.enabled);
	obs_data_set_string(d, "mode", st.mode.c_str());
	obs_data_set_int(d, "strength", st.strength);
	obs_data_set_string(d, "state", st.state.c_str());
	obs_data_set_string(d, "message", st.message.c_str());
	obs_data_set_int(d, "confidence", st.confidence);
	obs_data_set_bool(d, "applied", st.applied);
	obs_data_set_bool(d, "busy", st.busy);
	obs_data_set_int(d, "matchedAt", st.matchedAt);
}

static void setCode(obs_data_t *response, bool ok, const char *code, const std::string &message = std::string())
{
	obs_data_set_bool(response, "ok", ok);
	obs_data_set_string(response, "code", code);
	obs_data_set_string(response, "message", message.c_str());
}

// The source named in the request's "target", as a new reference, or null
static obs_source_t *requestTarget(obs_data_t *request)
{
	const char *name = obs_data_get_string(request, "target");
	return name && *name ? obs_get_source_by_name(name) : nullptr;
}

static void reqStatus(obs_data_t *, obs_data_t *response, void *)
{
	obs_data_array_t *sources = obs_data_array_create();
	obs_data_array_t *matches = obs_data_array_create();
	for (const colormatch::SourceInfo &s : colormatch::videoSources()) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "name", s.name.c_str());
		obs_data_set_bool(item, "camera", s.camera);
		obs_data_array_push_back(sources, item);
		obs_data_release(item);
		obs_source_t *src = obs_get_source_by_name(s.name.c_str());
		if (obs_source_t *filter = colormatch::findFilter(src)) {
			obs_data_t *m = obs_data_create();
			fillStatus(filterOf(filter), m);
			obs_data_array_push_back(matches, m);
			obs_data_release(m);
			obs_source_release(filter);
		}
		obs_source_release(src);
	}
	obs_data_set_array(response, "sources", sources);
	obs_data_set_array(response, "matches", matches);
	obs_data_array_release(sources);
	obs_data_array_release(matches);
	obs_data_set_string(response, "defaultReference", colormatch::cameraInScene("Camera 1").c_str());
	obs_data_set_string(response, "defaultTarget", colormatch::cameraInScene("Camera 2").c_str());
}

static void reqSet(obs_data_t *request, obs_data_t *response, void *)
{
	obs_source_t *target = requestTarget(request);
	if (!target)
		return setCode(response, false, "NO_TARGET", "That camera isn't in this scene collection.");
	obs_source_t *filter = colormatch::ensureFilter(target);
	obs_source_release(target);
	if (!filter)
		return setCode(response, false, "NO_TARGET", "Color Match can't be added to that source.");

	std::string reference = obs_data_get_string(request, "reference");
	if (!reference.empty()) {
		std::string message;
		const std::string code = colormatch::checkReference(filter, reference, message);
		if (!code.empty()) {
			setCode(response, false, code.c_str(), message);
			fillStatus(filterOf(filter), response);
			obs_source_release(filter);
			return;
		}
	} else {
		// A new match starts with Camera 1's camera as the reference
		colormatch::Status st;
		colormatch::status(filter, st);
		if (st.reference.empty())
			reference = colormatch::cameraInScene("Camera 1");
	}
	const int strength = obs_data_has_user_value(request, "strength") ? (int)obs_data_get_int(request, "strength")
									  : -1;
	colormatch::setOptions(filter, reference, obs_data_get_string(request, "mode"), strength);
	if (obs_data_has_user_value(request, "enabled"))
		obs_source_set_enabled(filter, obs_data_get_bool(request, "enabled"));
	setCode(response, true, "OK");
	fillStatus(filterOf(filter), response);
	obs_source_release(filter);
}

static void reqMatch(obs_data_t *request, obs_data_t *response, void *)
{
	obs_source_t *target = requestTarget(request);
	obs_source_t *filter = colormatch::findFilter(target);
	obs_source_release(target);
	if (!filter) {
		obs_data_set_bool(response, "accepted", false);
		setCode(response, false, target ? "NO_MATCH" : "NO_TARGET",
			"Set up colour matching for that camera first.");
		return;
	}
	const std::string id = colormatch::requestMatch(filter);
	obs_data_set_bool(response, "accepted", !id.empty());
	obs_data_set_string(response, "jobId", id.c_str());
	setCode(response, !id.empty(), id.empty() ? "NO_MATCH" : "OK");
	obs_source_release(filter);
}

static void reqReset(obs_data_t *request, obs_data_t *response, void *)
{
	obs_source_t *target = requestTarget(request);
	obs_source_t *filter = colormatch::findFilter(target);
	obs_source_release(target);
	if (!filter)
		return setCode(response, false, "NO_MATCH", "That camera has no colour matching.");
	colormatch::reset(filter);
	setCode(response, true, "OK");
	fillStatus(filterOf(filter), response);
	obs_source_release(filter);
}

static void reqRemove(obs_data_t *request, obs_data_t *response, void *)
{
	obs_source_t *target = requestTarget(request);
	obs_source_t *filter = colormatch::findFilter(target);
	if (filter) {
		obs_source_filter_remove(target, filter);
		CM_LOG(LOG_INFO, "removed from %s", obs_source_get_name(target));
	}
	setCode(response, filter != nullptr, filter ? "OK" : "NO_MATCH");
	obs_source_release(filter);
	obs_source_release(target);
}

/* ------------------------------------------------------------------ registration */

void color_match_load(void)
{
	static obs_source_info info = [] {
		obs_source_info i = {};
		i.id = CM_FILTER_ID;
		i.type = OBS_SOURCE_TYPE_FILTER;
		i.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
		i.get_name = cm_get_name;
		i.create = cm_create;
		i.destroy = cm_destroy;
		i.update = cm_update;
		i.get_defaults = cm_defaults;
		i.get_properties = cm_properties;
		i.video_tick = cm_tick;
		i.video_render = cm_render;
		i.video_get_color_space = cm_color_space;
		return i;
	}();
	obs_register_source(&info);
}

void color_match_register_requests(obs_websocket_vendor vendor)
{
	cmVendor = vendor;
	obs_websocket_vendor_register_request(vendor, "GetColorMatchStatus", reqStatus, nullptr);
	obs_websocket_vendor_register_request(vendor, "SetColorMatch", reqSet, nullptr);
	obs_websocket_vendor_register_request(vendor, "MatchColors", reqMatch, nullptr);
	obs_websocket_vendor_register_request(vendor, "ResetColorMatch", reqReset, nullptr);
	obs_websocket_vendor_register_request(vendor, "RemoveColorMatch", reqRemove, nullptr);
}

void color_match_unload(void)
{
	// Measuring threads stop within a quarter of a second of their filter going away
	for (int i = 0; i < 40 && liveWorkers > 0; ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
}
