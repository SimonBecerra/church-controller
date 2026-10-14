#pragma once

/* The Color Match filter (color-match.cpp) and its dock (color-match-dock.cpp). */

#include <obs.h>

#include <string>
#include <vector>

#include "obs-websocket-api.h"

#define CM_FILTER_ID "church_color_match"

// Call from obs_module_load(): registers the filter with OBS.
void color_match_load(void);
// Call from obs_module_post_load() once the vendor exists: adds the filter's commands to obs-websocket.
void color_match_register_requests(obs_websocket_vendor vendor);
// Call from obs_module_post_load(): adds View > Docks > Color Match.
void color_match_dock_load(void);
// Call from obs_module_unload(): waits briefly for any measurement still running.
void color_match_unload(void);

namespace colormatch {

struct Status {
	std::string target;      // the source being corrected
	std::string reference;   // the source whose colors are kept
	std::string mode;        // "once" or "continuous"
	std::string state;       // none, matching, matched, declined, error
	std::string message;     // plain words for the operator
	int strength = 100;      // 0-100
	int confidence = 0;      // of the last measurement, 0-100
	bool enabled = true;     // the filter's eye in OBS
	bool applied = false;    // a correction is in use
	bool busy = false;       // measuring now
	long long matchedAt = 0; // last successful match, milliseconds since 1970 (0 = never)
};

struct SourceInfo {
	std::string name;
	bool camera = false; // a capture device, NDI or DeckLink input
};

// Inputs that can be a reference or a target (sources with video, cameras first, then by name)
std::vector<SourceInfo> videoSources();
// The first camera in the named scene ("Camera 1"), or empty
std::string cameraInScene(const char *sceneName);

// The Color Match filter on `target`, as a new reference (release it), or null
obs_source_t *findFilter(obs_source_t *target);
// The same, adding one first if there isn't one
obs_source_t *ensureFilter(obs_source_t *target);
bool isFilter(obs_source_t *source);

bool status(obs_source_t *filter, Status &out);
// "" if `reference` can be the reference for the filter's target, otherwise a code
// (NO_REFERENCE, SAME_SOURCE, FEEDBACK) with an explanation in `message`
std::string checkReference(obs_source_t *filter, const std::string &reference, std::string &message);

// Change settings; leave a field empty (or negative) to keep it. Saved with the scene collection.
void setOptions(obs_source_t *filter, const std::string &reference, const std::string &mode, int strength);
// Measure both cameras and apply the result if it's reliable. Returns a job id; the outcome arrives as a
// ColorMatchResult event with the same id (and in status()).
std::string requestMatch(obs_source_t *filter);
// Go back to the camera's own colors (fades out over half a second)
void reset(obs_source_t *filter);

} // namespace colormatch
