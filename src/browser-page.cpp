/*
 * See browser-page.h.
 * License: GPL-2.0-or-later (required for OBS plug-ins).
 */

#include "browser-page.h"

#include <callback/calldata.h>
#include <util/bmem.h>

// obs-browser's own default page style, plus a counter so reload() can force a reload
static const char *baseCss = "body { background-color: rgba(0, 0, 0, 0); margin: 0px auto; overflow: hidden; }";

BrowserPage::BrowserPage(const char *builtinPage, const char *name) : builtin(builtinPage), debugName(name) {}

BrowserPage::~BrowserPage()
{
	if (browser)
		obs_source_release(browser);
}

obs_data_t *BrowserPage::settings()
{
	std::lock_guard<std::mutex> g(lock);
	obs_data_t *s = obs_data_create();
	obs_data_set_bool(s, "is_local_file", true);
	obs_data_set_string(s, "local_file", path.c_str());
	obs_data_set_int(s, "width", w);
	obs_data_set_int(s, "height", h);
	obs_data_set_bool(s, "shutdown", false);
	obs_data_set_bool(s, "restart_when_active", false);
	obs_data_set_bool(s, "reroute_audio", false);
	const std::string css = std::string(baseCss) + " /* " + std::to_string(reloads) + " */";
	obs_data_set_string(s, "css", css.c_str());
	return s;
}

void BrowserPage::apply()
{
	if (!browser)
		return;
	obs_data_t *s = settings();
	obs_source_update(browser, s);
	obs_data_release(s);
}

bool BrowserPage::configure(const std::string &customPath)
{
	std::string page = customPath;
	if (page.empty()) {
		char *p = obs_module_file(builtin);
		if (p) {
			page = p;
			bfree(p);
		} else {
			blog(LOG_WARNING, "[church-controller] %s: couldn't find %s in the plug-in's data folder",
			     debugName, builtin);
		}
	}
	uint32_t nw = 1920, nh = 1080;
	obs_video_info ovi;
	if (obs_get_video_info(&ovi)) {
		nw = ovi.base_width;
		nh = ovi.base_height;
	}
	bool changed;
	{
		std::lock_guard<std::mutex> g(lock);
		changed = page != path || nw != w || nh != h;
		path = page;
		w = nw;
		h = nh;
	}
	if (changed)
		apply();
	return changed && browser;
}

void BrowserPage::create(obs_source_t *ownerSource)
{
	owner = ownerSource;
	obs_data_t *s = settings();
	const std::string name = std::string(debugName) + " page";
	browser = obs_source_create_private("browser_source", name.c_str(), s);
	obs_data_release(s);
	if (!browser)
		blog(LOG_WARNING, "[church-controller] %s: OBS's Browser source isn't available, so it can't be drawn.",
		     debugName);
}

void BrowserPage::send(const char *eventName, obs_data_t *detail)
{
	if (!browser)
		return;
	calldata_t cd;
	calldata_init(&cd);
	calldata_set_string(&cd, "eventName", eventName);
	calldata_set_string(&cd, "jsonString", detail ? obs_data_get_json(detail) : "null");
	proc_handler_call(obs_source_get_proc_handler(browser), "javascript_event", &cd);
	calldata_free(&cd);
}

void BrowserPage::reload()
{
	{
		std::lock_guard<std::mutex> g(lock);
		reloads++;
	}
	apply();
}

void BrowserPage::render()
{
	if (browser)
		obs_source_video_render(browser);
}

uint32_t BrowserPage::width()
{
	return browser ? obs_source_get_width(browser) : 0;
}

uint32_t BrowserPage::height()
{
	return browser ? obs_source_get_height(browser) : 0;
}

// The browser counts as part of the owning source, so it's "showing" (and drawing) whenever that source is.
void BrowserPage::enumSources(obs_source_enum_proc_t cb, void *param)
{
	if (browser && owner)
		cb(owner, browser, param);
}
