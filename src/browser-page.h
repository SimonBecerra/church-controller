#pragma once

/*
 * A web page drawn inside one of our sources, through a hidden (private) browser source that the source owns.
 * Used by the Lower Third and Scripture & Hymns sources. Each page lives in the plug-in's data folder and can
 * be swapped for a custom .html file from the source's settings without rebuilding the plug-in.
 */

#include <obs-module.h>

#include <mutex>
#include <string>

class BrowserPage {
public:
	// `builtin` is the page's path inside the plug-in's data folder, e.g. "lower-third/index.html".
	BrowserPage(const char *builtin, const char *debugName);
	~BrowserPage();

	// Call from the source's update(): picks the custom page (if set) or the built-in one, sizes it to OBS's
	// canvas, and reloads the page only if something changed. Returns true if it reloaded.
	bool configure(const std::string &customPath);

	// Call once from the source's create(), after configure(). Logs a warning if OBS has no Browser source.
	void create(obs_source_t *owner);

	// Send a JavaScript event to the page: window.addEventListener(eventName, e => e.detail)
	void send(const char *eventName, obs_data_t *detail);

	// Reload the page (the "Reload design" button).
	void reload();

	void render();
	uint32_t width();
	uint32_t height();
	void enumSources(obs_source_enum_proc_t cb, void *param);
	bool available() const { return browser != nullptr; }

private:
	obs_data_t *settings(); // browser source settings for the current page
	void apply();

	const char *builtin;
	const char *debugName;
	obs_source_t *owner = nullptr;
	obs_source_t *browser = nullptr;

	std::mutex lock;
	std::string path;
	uint32_t w = 1920;
	uint32_t h = 1080;
	int reloads = 0;
};
