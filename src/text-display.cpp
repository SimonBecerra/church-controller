/*
 * Scripture & Hymns source for the Church Controller plug-in (written against OBS 32.2.2)
 *
 * Adds a source type called "Scripture & Hymns" to OBS (+ in the Sources list > Scripture & Hymns).
 * It shows whatever the BGH LIVE Controller sends it: lines of a hymn (with the hymn book's logo, number and
 * title) or Bible verses (with the reference and translation), in a band across the bottom of the screen.
 * The controller keeps the hymn books and looks up the verses; this source only draws what it's given.
 *
 * The graphic
 *   The source draws data/text-display/index.html (see browser-page.h), which can be swapped for a custom
 *   .html file in the source's settings. The page is told what to do with a JavaScript event named
 *   "churchText":
 *     { "action": "show", "slide": { ...see below... } }   or   { "action": "clear" }
 *   slide (everything optional except kind):
 *     kind: "hymn" | "verse"
 *     key: an id the controller uses to recognise the slide
 *     hymn:  book, logo (data: URL), number, title, label ("Verse 2", "Chorus"), lines[], page, pages
 *     verse: reference ("John 3:16-17"), translation ("KJV"), text, verses[{n, text}], page, pages
 *
 * Controller commands (obs-websocket vendor "church-controller", all answer immediately)
 *   GetTextStatus { sourceName? }          -> { found, sourceName, showing, key }
 *   ShowText      { sourceName?, slide }   -> { ok, code, sourceName, showing, key }
 *   ClearText     { sourceName? }          -> { ok, code, sourceName, showing, key }
 *   sourceName is optional: without it, the first Scripture & Hymns source is used.
 *   codes: OK, NO_TEXT_SOURCE (no Scripture & Hymns source in OBS), NO_SLIDE
 * Event (sent whenever what's on screen changes)
 *   TextChanged { sourceName, showing, key }
 *
 * License: GPL-2.0-or-later (required for OBS plug-ins).
 */

#include <obs-module.h>

#include <cstring>
#include <mutex>
#include <string>

#include "browser-page.h"
#include "sources.h"

#define TD_ID "church_text_display"
#define TD_EVENT "churchText"

static obs_websocket_vendor tdVendor = nullptr;

struct TextDisplay {
	obs_source_t *source = nullptr;
	BrowserPage page{"text-display/index.html", "Scripture & Hymns"};

	std::mutex lock; // guards everything below
	bool showing = false;
	std::string key; // the slide's key, for the controller
};

/* ------------------------------------------------------------------ page and controller */

static void fillState(TextDisplay *td, obs_data_t *d)
{
	std::lock_guard<std::mutex> g(td->lock);
	obs_data_set_string(d, "sourceName", obs_source_get_name(td->source));
	obs_data_set_bool(d, "showing", td->showing);
	obs_data_set_string(d, "key", td->key.c_str());
}

static void emitState(TextDisplay *td)
{
	if (!tdVendor)
		return;
	obs_data_t *d = obs_data_create();
	fillState(td, d);
	obs_websocket_vendor_emit_event(tdVendor, "TextChanged", d);
	obs_data_release(d);
}

// Takes the slide as given by the controller and passes it to the page unchanged.
static void showSlide(TextDisplay *td, obs_data_t *slide)
{
	{
		std::lock_guard<std::mutex> g(td->lock);
		td->showing = true;
		td->key = obs_data_get_string(slide, "key");
	}
	obs_data_t *d = obs_data_create();
	obs_data_set_string(d, "action", "show");
	obs_data_set_obj(d, "slide", slide);
	td->page.send(TD_EVENT, d);
	obs_data_release(d);
	emitState(td);
}

static void clearSlide(TextDisplay *td)
{
	bool was;
	{
		std::lock_guard<std::mutex> g(td->lock);
		was = td->showing;
		td->showing = false;
		td->key.clear();
	}
	obs_data_t *d = obs_data_create();
	obs_data_set_string(d, "action", "clear");
	td->page.send(TD_EVENT, d);
	obs_data_release(d);
	if (was)
		emitState(td);
}

/* ------------------------------------------------------------------ source callbacks */

static const char *td_get_name(void *)
{
	return "Scripture & Hymns";
}

static void td_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "design_file", "");
}

static void td_update(void *data, obs_data_t *settings)
{
	auto *td = static_cast<TextDisplay *>(data);
	if (td->page.configure(obs_data_get_string(settings, "design_file"))) {
		// the page starts empty after a reload
		{
			std::lock_guard<std::mutex> g(td->lock);
			td->showing = false;
			td->key.clear();
		}
		emitState(td);
	}
}

static void *td_create(obs_data_t *settings, obs_source_t *source)
{
	auto *td = new TextDisplay();
	td->source = source;
	td_update(td, settings);
	td->page.create(source);
	return td;
}

static void td_destroy(void *data)
{
	delete static_cast<TextDisplay *>(data);
}

static uint32_t td_width(void *data)
{
	return static_cast<TextDisplay *>(data)->page.width();
}

static uint32_t td_height(void *data)
{
	return static_cast<TextDisplay *>(data)->page.height();
}

static void td_render(void *data, gs_effect_t *)
{
	static_cast<TextDisplay *>(data)->page.render();
}

static void td_enum_sources(void *data, obs_source_enum_proc_t cb, void *param)
{
	static_cast<TextDisplay *>(data)->page.enumSources(cb, param);
}

/* ------------------------------------------------------------------ settings window */

// A sample verse, so the design can be checked in OBS before the controller is set up
static bool td_test_button(obs_properties_t *, obs_property_t *, void *data)
{
	obs_data_t *slide = obs_data_create();
	obs_data_set_string(slide, "kind", "verse");
	obs_data_set_string(slide, "key", "test");
	obs_data_set_string(slide, "reference", "Psalm 46:1");
	obs_data_set_string(slide, "translation", "KJV");
	obs_data_set_string(slide, "text", "God is our refuge and strength, a very present help in trouble.");
	showSlide(static_cast<TextDisplay *>(data), slide);
	obs_data_release(slide);
	return false;
}

static bool td_clear_button(obs_properties_t *, obs_property_t *, void *data)
{
	clearSlide(static_cast<TextDisplay *>(data));
	return false;
}

static bool td_reload_button(obs_properties_t *, obs_property_t *, void *data)
{
	auto *td = static_cast<TextDisplay *>(data);
	{
		std::lock_guard<std::mutex> g(td->lock);
		td->showing = false;
		td->key.clear();
	}
	td->page.reload();
	emitState(td);
	return false;
}

static obs_properties_t *td_properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "info",
				"Hymns and Bible verses are chosen in the BGH LIVE Controller. Keep this source at the top "
				"of the scenes where they should appear.",
				OBS_TEXT_INFO);
	obs_properties_add_button(props, "test", "Show a test verse", td_test_button);
	obs_properties_add_button(props, "clear", "Clear", td_clear_button);
	obs_properties_add_path(props, "design_file", "Custom design (optional)", OBS_PATH_FILE,
				"Web pages (*.html *.htm)", nullptr);
	obs_properties_add_button(props, "reload", "Reload design", td_reload_button);
	return props;
}

/* ------------------------------------------------------------------ controller commands */

static bool isTextDisplay(obs_source_t *s)
{
	const char *id = obs_source_get_unversioned_id(s);
	return id && strcmp(id, TD_ID) == 0;
}

// The source named in the request, or the first one in OBS. Returns a reference (release it) or null.
static obs_source_t *findTextDisplay(obs_data_t *request)
{
	const char *name = request ? obs_data_get_string(request, "sourceName") : nullptr;
	if (name && *name) {
		obs_source_t *s = obs_get_source_by_name(name);
		if (s && !isTextDisplay(s)) {
			obs_source_release(s);
			s = nullptr;
		}
		return s;
	}
	obs_source_t *found = nullptr;
	obs_enum_sources(
		[](void *param, obs_source_t *s) {
			if (!isTextDisplay(s))
				return true;
			*static_cast<obs_source_t **>(param) = obs_source_get_ref(s);
			return false;
		},
		&found);
	return found;
}

static TextDisplay *tdOf(obs_source_t *s)
{
	return static_cast<TextDisplay *>(obs_obj_get_data(s));
}

static void notFound(obs_data_t *response)
{
	obs_data_set_bool(response, "ok", false);
	obs_data_set_bool(response, "found", false);
	obs_data_set_string(response, "code", "NO_TEXT_SOURCE");
}

static void reqStatus(obs_data_t *request, obs_data_t *response, void *)
{
	obs_source_t *s = findTextDisplay(request);
	if (!s)
		return notFound(response);
	obs_data_set_bool(response, "found", true);
	fillState(tdOf(s), response);
	obs_source_release(s);
}

static void reqShow(obs_data_t *request, obs_data_t *response, void *)
{
	obs_source_t *s = findTextDisplay(request);
	if (!s)
		return notFound(response);
	obs_data_t *slide = obs_data_get_obj(request, "slide");
	const bool ok = slide && *obs_data_get_string(slide, "kind");
	if (ok)
		showSlide(tdOf(s), slide);
	if (slide)
		obs_data_release(slide);
	obs_data_set_bool(response, "ok", ok);
	obs_data_set_bool(response, "found", true);
	obs_data_set_string(response, "code", ok ? "OK" : "NO_SLIDE");
	fillState(tdOf(s), response);
	obs_source_release(s);
}

static void reqClear(obs_data_t *request, obs_data_t *response, void *)
{
	obs_source_t *s = findTextDisplay(request);
	if (!s)
		return notFound(response);
	clearSlide(tdOf(s));
	obs_data_set_bool(response, "ok", true);
	obs_data_set_bool(response, "found", true);
	obs_data_set_string(response, "code", "OK");
	fillState(tdOf(s), response);
	obs_source_release(s);
}

/* ------------------------------------------------------------------ registration */

void text_display_load(void)
{
	static obs_source_info info = [] {
		obs_source_info i = {};
		i.id = TD_ID;
		i.type = OBS_SOURCE_TYPE_INPUT;
		i.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_DO_NOT_DUPLICATE;
		i.icon_type = OBS_ICON_TYPE_TEXT;
		i.get_name = td_get_name;
		i.create = td_create;
		i.destroy = td_destroy;
		i.update = td_update;
		i.get_defaults = td_defaults;
		i.get_properties = td_properties;
		i.get_width = td_width;
		i.get_height = td_height;
		i.video_render = td_render;
		i.enum_active_sources = td_enum_sources;
		i.enum_all_sources = td_enum_sources;
		return i;
	}();
	obs_register_source(&info);
}

void text_display_register_requests(obs_websocket_vendor vendor)
{
	tdVendor = vendor;
	obs_websocket_vendor_register_request(vendor, "GetTextStatus", reqStatus, nullptr);
	obs_websocket_vendor_register_request(vendor, "ShowText", reqShow, nullptr);
	obs_websocket_vendor_register_request(vendor, "ClearText", reqClear, nullptr);
}
