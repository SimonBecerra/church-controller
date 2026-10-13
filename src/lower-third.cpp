/*
 * Lower Third source for the Church Controller plug-in (written against OBS 32.2.2)
 *
 * Adds a source type called "Lower Third" to OBS (+ in the Sources list > Lower Third).
 *
 * Names
 *   By default the names come from a text source ("Speaker Names"), so whatever the BGH LIVE Controller
 *   puts there when you go live is what the lower thirds say. "Simon Becerra and Nathanael Alicea" becomes
 *   two names: "Simon Becerra" and "Nathanael Alicea" (split on "and", "&", commas, semicolons and new lines).
 *   The names can also be typed straight into the source's settings in OBS instead.
 *
 * Showing them
 *   One name is on screen at a time. Show a name from the controller, from the buttons in the source's
 *   settings, or from OBS hotkeys (Settings > Hotkeys > "Lower Third: show/hide speaker 1" ...).
 *   It hides itself after the number of seconds in the settings (8 by default; 0 = stays until hidden).
 *
 * Themes
 *   "barrington" (the name over a cropped B), "conference" (the name and subtitle as Barrington, with
 *   "Conference" written out in script behind them in a darker shade of its colour), "christmas" (red, with a sprig decoration on its left edge) or
 *   "sundayschool" (blue, with circles bouncing inside it). Conference has its own box colour (a hex code,
 *   #132932 to start with), always at the same 76% opacity as Barrington. Saved in the source's settings, so each scene collection keeps its own;
 *   the BGH LIVE Controller sets it from Settings → Scene collections (SetLowerThirdTheme).
 *
 * On / off
 *   "Show lower thirds" in the source's settings (or SetLowerThirdEnabled from the controller). When it's off,
 *   the source draws nothing at all and refuses to show names, so no lower third can reach the stream or a
 *   recording. The setting is saved with the scene collection, so it stays off after OBS restarts.
 *
 * The graphic
 *   The source draws data/lower-third/index.html (see browser-page.h). The page is told what to do with a
 *   JavaScript event named "churchLowerThird":
 *     { "action": "show", "name": "Simon Becerra", "subtitle": "Barrington, NJ", "theme": "conference", "color": "#132932" }
 *     { "action": "hide", "name": "" }
 *
 * Controller commands (obs-websocket vendor "church-controller", all answer immediately)
 *   GetLowerThirdStatus { sourceName? }                            -> { found, ...status }
 *   ShowLowerThird      { sourceName?, index? | name?, subtitle?, toggle? }   -> { ok, code, ...status }
 *   HideLowerThird      { sourceName? }                            -> { ok, code, ...status }
 *   SetLowerThirdEnabled { sourceName?, enabled }                  -> { ok, code, updated }
 *   SetLowerThirdTheme  { sourceName?, theme, color? }             -> { ok, code, updated }
 *     (both apply to every Lower Third source when sourceName is left out)
 *   status = { sourceName, enabled, theme, color, names[{name}], showing, subtitle, duration }
 *   sourceName is optional: without it, the first Lower Third source is used.
 *   codes: OK, NO_LOWER_THIRD (no Lower Third source in OBS), NO_SUCH_NAME, DISABLED
 * Event (sent whenever the names, what's on screen, or on/off change)
 *   LowerThirdChanged   status
 *
 * License: GPL-2.0-or-later (required for OBS plug-ins).
 */

#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <regex>
#include <string>
#include <vector>

#include "browser-page.h"
#include "sources.h"

#define LT_ID "church_lower_third"
#define LT_EVENT "churchLowerThird"
#define LT_BUTTONS 4
#define LT_LOG(level, fmt, ...) blog(level, "[church-controller] lower third: " fmt, ##__VA_ARGS__)

static obs_websocket_vendor ltVendor = nullptr;

struct LowerThird {
	obs_source_t *source = nullptr;
	BrowserPage page{"lower-third/index.html", "Lower Third"};

	std::mutex lock; // guards everything below
	bool enabled = true;
	std::vector<std::string> names;
	std::string showing;  // name on screen now, or empty
	std::string subtitle; // shown under the name in the Conference theme
	std::string theme = "barrington";
	std::string color = "#132932"; // Conference box colour
	double shownFor = 0.0;
	double duration = 8.0;
	bool fromTextSource = true;
	std::string textSource;
	std::string typedNames;
	double pollIn = 0.0;

	obs_hotkey_id showKeys[LT_BUTTONS] = {OBS_INVALID_HOTKEY_ID, OBS_INVALID_HOTKEY_ID, OBS_INVALID_HOTKEY_ID,
					      OBS_INVALID_HOTKEY_ID};
	obs_hotkey_id hideKey = OBS_INVALID_HOTKEY_ID;
};

/* ------------------------------------------------------------------ names */

static std::string trim(const std::string &s)
{
	const char *ws = " \t\r\n";
	const size_t a = s.find_first_not_of(ws);
	if (a == std::string::npos)
		return std::string();
	const size_t b = s.find_last_not_of(ws);
	return s.substr(a, b - a + 1);
}

// "#1a2B3c" -> "#1A2B3C"; anything that isn't a 6-digit hex colour -> the default
static std::string cleanColor(const char *c)
{
	static const std::regex hex("^#?([0-9a-fA-F]{6})$");
	std::cmatch m;
	if (!c || !std::regex_match(c, m, hex))
		return "#132932";
	std::string out = "#" + m[1].str();
	std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) { return (char)std::toupper(ch); });
	return out;
}

// One of the themes the page knows; anything else -> "barrington"
static std::string cleanTheme(const char *t)
{
	static const char *themes[] = {"barrington", "conference", "christmas", "sundayschool"};
	for (const char *known : themes)
		if (t && strcmp(t, known) == 0)
			return known;
	return "barrington";
}

// "Simon Becerra and Nathanael Alicea" -> {"Simon Becerra", "Nathanael Alicea"}
static std::vector<std::string> splitNames(const std::string &text)
{
	static const std::regex sep("\\s*(?:,|;|&|\\n|\\band\\b)\\s*", std::regex::icase);
	std::vector<std::string> out;
	for (std::sregex_token_iterator it(text.begin(), text.end(), sep, -1), end; it != end; ++it) {
		const std::string n = trim(*it);
		if (!n.empty() && std::find(out.begin(), out.end(), n) == out.end())
			out.push_back(n);
	}
	return out;
}

// The text a text source shows (GDI+ text on Windows, FreeType text elsewhere), including "read from file".
static std::string readSourceText(const std::string &name)
{
	if (name.empty())
		return std::string();
	obs_source_t *src = obs_get_source_by_name(name.c_str());
	if (!src)
		return std::string();
	obs_data_t *s = obs_source_get_settings(src);
	std::string text;
	const char *file = nullptr;
	if (obs_data_get_bool(s, "read_from_file"))
		file = obs_data_get_string(s, "file");
	else if (obs_data_get_bool(s, "from_file"))
		file = obs_data_get_string(s, "text_file");
	if (file && *file) {
		char *contents = os_quick_read_utf8_file(file);
		if (contents) {
			text = contents;
			bfree(contents);
		}
	} else {
		const char *t = obs_data_get_string(s, "text");
		text = t ? t : "";
	}
	obs_data_release(s);
	obs_source_release(src);
	return text;
}

// Read the names again. Returns true if they changed.
static bool refreshNames(LowerThird *lt)
{
	bool fromSource;
	std::string sourceName, typed;
	{
		std::lock_guard<std::mutex> g(lt->lock);
		fromSource = lt->fromTextSource;
		sourceName = lt->textSource;
		typed = lt->typedNames;
	}
	std::vector<std::string> names = splitNames(fromSource ? readSourceText(sourceName) : typed);
	std::lock_guard<std::mutex> g(lt->lock);
	if (names == lt->names)
		return false;
	lt->names = std::move(names);
	return true;
}

/* ------------------------------------------------------------------ page and controller */

static void sendToPage(LowerThird *lt, const char *action, const std::string &name,
		       const std::string &subtitle = std::string())
{
	std::string theme, color;
	{
		std::lock_guard<std::mutex> g(lt->lock);
		theme = lt->theme;
		color = lt->color;
	}
	obs_data_t *d = obs_data_create();
	obs_data_set_string(d, "action", action);
	obs_data_set_string(d, "name", name.c_str());
	obs_data_set_string(d, "subtitle", subtitle.c_str());
	obs_data_set_string(d, "theme", theme.c_str());
	obs_data_set_string(d, "color", color.c_str());
	lt->page.send(LT_EVENT, d);
	obs_data_release(d);
}

static void fillState(LowerThird *lt, obs_data_t *d)
{
	std::lock_guard<std::mutex> g(lt->lock);
	obs_data_set_string(d, "sourceName", obs_source_get_name(lt->source));
	obs_data_set_bool(d, "enabled", lt->enabled);
	obs_data_array_t *arr = obs_data_array_create();
	for (const std::string &n : lt->names) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "name", n.c_str());
		obs_data_array_push_back(arr, item);
		obs_data_release(item);
	}
	obs_data_set_array(d, "names", arr);
	obs_data_array_release(arr);
	obs_data_set_string(d, "showing", lt->showing.c_str());
	obs_data_set_string(d, "subtitle", lt->subtitle.c_str());
	obs_data_set_string(d, "theme", lt->theme.c_str());
	obs_data_set_string(d, "color", lt->color.c_str());
	obs_data_set_int(d, "duration", (long long)lt->duration);
}

// Let the controller know so its buttons can relabel and light up.
static void emitState(LowerThird *lt)
{
	if (!ltVendor)
		return;
	obs_data_t *d = obs_data_create();
	fillState(lt, d);
	obs_websocket_vendor_emit_event(ltVendor, "LowerThirdChanged", d);
	obs_data_release(d);
}

// Returns false (and shows nothing) when lower thirds are switched off.
static bool showName(LowerThird *lt, const std::string &name, const std::string &subtitle = std::string())
{
	if (name.empty())
		return false;
	{
		std::lock_guard<std::mutex> g(lt->lock);
		if (!lt->enabled)
			return false;
		lt->showing = name;
		lt->subtitle = subtitle;
		lt->shownFor = 0.0;
	}
	sendToPage(lt, "show", name, subtitle);
	emitState(lt);
	LT_LOG(LOG_INFO, "showing \"%s\"", name.c_str());
	return true;
}

static void hideName(LowerThird *lt)
{
	bool wasShowing;
	{
		std::lock_guard<std::mutex> g(lt->lock);
		wasShowing = !lt->showing.empty();
		lt->showing.clear();
		lt->subtitle.clear();
	}
	sendToPage(lt, "hide", std::string()); // sent even if nothing is up, in case the page and plug-in disagree
	if (wasShowing)
		emitState(lt);
}

// Show speaker `index` (0-based), or hide it if it's the one already showing.
static void toggleIndex(LowerThird *lt, size_t index)
{
	refreshNames(lt);
	std::string name;
	bool alreadyShowing;
	{
		std::lock_guard<std::mutex> g(lt->lock);
		if (index >= lt->names.size())
			return;
		name = lt->names[index];
		alreadyShowing = lt->showing == name;
	}
	if (alreadyShowing)
		hideName(lt);
	else
		showName(lt, name);
}

/* ------------------------------------------------------------------ source callbacks */

static const char *lt_get_name(void *)
{
	return "Lower Third";
}

static void lt_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "enabled", true);
	obs_data_set_default_string(settings, "theme", "barrington");
	obs_data_set_default_string(settings, "color", "#132932");
	obs_data_set_default_string(settings, "names_from", "source");
	obs_data_set_default_string(settings, "text_source", "Speaker Names");
	obs_data_set_default_string(settings, "typed_names", "");
	obs_data_set_default_int(settings, "duration", 8);
	obs_data_set_default_string(settings, "design_file", "");
}

static void lt_update(void *data, obs_data_t *settings)
{
	auto *lt = static_cast<LowerThird *>(data);
	const bool reloaded = lt->page.configure(obs_data_get_string(settings, "design_file"));

	bool enabledChanged, turnedOff, themeChanged;
	{
		std::lock_guard<std::mutex> g(lt->lock);
		const bool enabled = obs_data_get_bool(settings, "enabled");
		enabledChanged = enabled != lt->enabled;
		turnedOff = !enabled;
		lt->enabled = enabled;
		lt->fromTextSource = strcmp(obs_data_get_string(settings, "names_from"), "typed") != 0;
		lt->textSource = obs_data_get_string(settings, "text_source");
		lt->typedNames = obs_data_get_string(settings, "typed_names");
		lt->duration = (double)obs_data_get_int(settings, "duration");
		const std::string theme = cleanTheme(obs_data_get_string(settings, "theme"));
		const bool conference = theme == "conference";
		const std::string color = cleanColor(obs_data_get_string(settings, "color"));
		themeChanged = lt->theme != theme || (conference && lt->color != color);
		lt->theme = theme;
		lt->color = color;
		if (reloaded)
			lt->showing.clear(); // the page starts empty after a reload
	}
	if (turnedOff)
		hideName(lt);
	if (enabledChanged)
		LT_LOG(LOG_INFO, "%s", turnedOff ? "switched off" : "switched on");
	if (themeChanged)
		hideName(lt); // the next name shows in the new theme
	if (refreshNames(lt) || enabledChanged || themeChanged || reloaded)
		emitState(lt);
}

static void lt_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	auto *lt = static_cast<LowerThird *>(data);
	if (id == lt->hideKey) {
		hideName(lt);
		return;
	}
	for (size_t i = 0; i < LT_BUTTONS; ++i)
		if (id == lt->showKeys[i])
			toggleIndex(lt, i);
}

static void *lt_create(obs_data_t *settings, obs_source_t *source)
{
	auto *lt = new LowerThird();
	lt->source = source;
	lt_update(lt, settings);
	lt->page.create(source);

	for (size_t i = 0; i < LT_BUTTONS; ++i) {
		const std::string id = "ChurchLowerThird.Show" + std::to_string(i + 1);
		const std::string desc = "Lower Third: show/hide speaker " + std::to_string(i + 1);
		lt->showKeys[i] = obs_hotkey_register_source(source, id.c_str(), desc.c_str(), lt_hotkey, lt);
	}
	lt->hideKey = obs_hotkey_register_source(source, "ChurchLowerThird.Hide", "Lower Third: hide", lt_hotkey, lt);
	return lt;
}

static void lt_destroy(void *data)
{
	auto *lt = static_cast<LowerThird *>(data);
	for (obs_hotkey_id id : lt->showKeys)
		if (id != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(id);
	if (lt->hideKey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(lt->hideKey);
	delete lt;
}

static uint32_t lt_width(void *data)
{
	return static_cast<LowerThird *>(data)->page.width();
}

static uint32_t lt_height(void *data)
{
	return static_cast<LowerThird *>(data)->page.height();
}

static void lt_render(void *data, gs_effect_t *)
{
	auto *lt = static_cast<LowerThird *>(data);
	bool enabled;
	{
		std::lock_guard<std::mutex> g(lt->lock);
		enabled = lt->enabled;
	}
	if (enabled) // switched off: draw nothing, so nothing can reach the stream or a recording
		lt->page.render();
}

// Runs every frame: hides the name when its time is up, and checks the names twice a second.
static void lt_tick(void *data, float seconds)
{
	auto *lt = static_cast<LowerThird *>(data);
	bool expired = false, poll = false;
	{
		std::lock_guard<std::mutex> g(lt->lock);
		if (!lt->showing.empty() && lt->duration > 0.0) {
			lt->shownFor += seconds;
			expired = lt->shownFor >= lt->duration;
		}
		lt->pollIn -= seconds;
		if (lt->pollIn <= 0.0) {
			lt->pollIn = 0.5;
			poll = true;
		}
	}
	if (expired)
		hideName(lt);
	if (poll && refreshNames(lt))
		emitState(lt);
}

static void lt_enum_sources(void *data, obs_source_enum_proc_t cb, void *param)
{
	static_cast<LowerThird *>(data)->page.enumSources(cb, param);
}

/* ------------------------------------------------------------------ settings window */

template<size_t I> static bool lt_show_button(obs_properties_t *, obs_property_t *, void *data)
{
	toggleIndex(static_cast<LowerThird *>(data), I);
	return false;
}

static bool lt_hide_button(obs_properties_t *, obs_property_t *, void *data)
{
	hideName(static_cast<LowerThird *>(data));
	return false;
}

static bool lt_reload_button(obs_properties_t *, obs_property_t *, void *data)
{
	auto *lt = static_cast<LowerThird *>(data);
	{
		std::lock_guard<std::mutex> g(lt->lock);
		lt->showing.clear();
	}
	lt->page.reload();
	emitState(lt);
	return false;
}

static bool lt_names_from_changed(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const bool typed = strcmp(obs_data_get_string(settings, "names_from"), "typed") == 0;
	obs_property_set_visible(obs_properties_get(props, "text_source"), !typed);
	obs_property_set_visible(obs_properties_get(props, "typed_names"), typed);
	return true;
}

static bool addTextSource(void *param, obs_source_t *src)
{
	const char *id = obs_source_get_unversioned_id(src);
	const char *name = obs_source_get_name(src);
	if (id && name && strncmp(id, "text_", 5) == 0)
		obs_property_list_add_string(static_cast<obs_property_t *>(param), name, name);
	return true;
}

static obs_properties_t *lt_properties(void *data)
{
	auto *lt = static_cast<LowerThird *>(data);
	obs_properties_t *props = obs_properties_create();

	std::vector<std::string> names;
	if (lt) {
		refreshNames(lt);
		std::lock_guard<std::mutex> g(lt->lock);
		names = lt->names;
	}

	obs_property_t *on = obs_properties_add_bool(props, "enabled", "Show lower thirds");
	obs_property_set_long_description(
		on, "When this is off, no lower third can appear on the stream or in a recording. "
		    "The BGH LIVE Controller sets this from its own Settings.");

	if (names.empty()) {
		obs_properties_add_text(props, "names_info",
					"No names yet. Choose speakers in the controller, or type names below.",
					OBS_TEXT_INFO);
	}
	using ButtonFn = bool (*)(obs_properties_t *, obs_property_t *, void *);
	static const ButtonFn showFns[LT_BUTTONS] = {lt_show_button<0>, lt_show_button<1>, lt_show_button<2>,
						     lt_show_button<3>};
	for (size_t i = 0; i < names.size() && i < LT_BUTTONS; ++i) {
		const std::string id = "show_" + std::to_string(i + 1);
		const std::string label = "Show / hide: " + names[i];
		obs_properties_add_button2(props, id.c_str(), label.c_str(), showFns[i], data);
	}
	obs_properties_add_button2(props, "hide", "Hide", lt_hide_button, data);

	obs_property_t *theme =
		obs_properties_add_list(props, "theme", "Theme", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(theme, "Barrington (name over a large B)", "barrington");
	obs_property_list_add_string(theme, "Conference (script word behind the name)", "conference");
	obs_property_list_add_string(theme, "Christmas (red, with a sprig on the left)", "christmas");
	obs_property_list_add_string(theme, "Sunday School (blue, with bouncing circles)", "sundayschool");
	obs_property_set_long_description(theme,
					  "The BGH LIVE Controller sets this from Settings → Scene collections.");
	obs_property_t *color = obs_properties_add_text(props, "color", "Conference colour (hex)", OBS_TEXT_DEFAULT);
	obs_property_set_long_description(color,
					  "Like #132932. Shown at the same see-through level as the Barrington theme.");

	obs_property_t *from = obs_properties_add_list(props, "names_from", "Names come from", OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(from, "A text source (follows the controller)", "source");
	obs_property_list_add_string(from, "Names typed here", "typed");
	obs_property_set_modified_callback(from, lt_names_from_changed);

	obs_property_t *src = obs_properties_add_list(props, "text_source", "Text source", OBS_COMBO_TYPE_LIST,
						      OBS_COMBO_FORMAT_STRING);
	obs_enum_sources(addTextSource, src);

	obs_property_t *typed = obs_properties_add_text(props, "typed_names", "Names", OBS_TEXT_MULTILINE);
	obs_property_set_long_description(typed, "One name per line, or separated by \"and\".");

	obs_property_t *dur = obs_properties_add_int(props, "duration", "Hide after", 0, 300, 1);
	obs_property_int_set_suffix(dur, " s");
	obs_property_set_long_description(dur, "0 keeps it up until you hide it.");

	obs_properties_add_path(props, "design_file", "Custom design (optional)", OBS_PATH_FILE,
				"Web pages (*.html *.htm)", nullptr);
	obs_properties_add_button2(props, "reload", "Reload design", lt_reload_button, data);
	return props;
}

/* ------------------------------------------------------------------ controller commands */

static bool isLowerThird(obs_source_t *s)
{
	const char *id = obs_source_get_unversioned_id(s);
	return id && strcmp(id, LT_ID) == 0;
}

// The Lower Third named in the request, or the first one in OBS. Returns a reference (release it) or null.
static obs_source_t *findLowerThird(obs_data_t *request)
{
	const char *name = request ? obs_data_get_string(request, "sourceName") : nullptr;
	if (name && *name) {
		obs_source_t *s = obs_get_source_by_name(name);
		if (s && !isLowerThird(s)) {
			obs_source_release(s);
			s = nullptr;
		}
		return s;
	}
	obs_source_t *found = nullptr;
	obs_enum_sources(
		[](void *param, obs_source_t *s) {
			if (!isLowerThird(s))
				return true;
			*static_cast<obs_source_t **>(param) = obs_source_get_ref(s);
			return false;
		},
		&found);
	return found;
}

static LowerThird *ltOf(obs_source_t *s)
{
	return static_cast<LowerThird *>(obs_obj_get_data(s));
}

static void reqStatus(obs_data_t *request, obs_data_t *response, void *)
{
	obs_source_t *s = findLowerThird(request);
	obs_data_set_bool(response, "found", s != nullptr);
	if (!s)
		return;
	refreshNames(ltOf(s));
	fillState(ltOf(s), response);
	obs_source_release(s);
}

static void reqShow(obs_data_t *request, obs_data_t *response, void *)
{
	obs_source_t *s = findLowerThird(request);
	if (!s) {
		obs_data_set_bool(response, "ok", false);
		obs_data_set_string(response, "code", "NO_LOWER_THIRD");
		return;
	}
	LowerThird *lt = ltOf(s);
	refreshNames(lt);

	std::string name = obs_data_get_string(request, "name");
	const std::string subtitle = trim(obs_data_get_string(request, "subtitle"));
	bool enabled, alreadyShowing;
	{
		std::lock_guard<std::mutex> g(lt->lock);
		if (name.empty() && obs_data_has_user_value(request, "index")) {
			const long long index = obs_data_get_int(request, "index");
			if (index >= 0 && (size_t)index < lt->names.size())
				name = lt->names[(size_t)index];
		}
		name = trim(name);
		enabled = lt->enabled;
		alreadyShowing = !name.empty() && lt->showing == name && lt->subtitle == subtitle;
	}

	const char *code = "OK";
	if (!enabled)
		code = "DISABLED";
	else if (name.empty())
		code = "NO_SUCH_NAME";
	else if (alreadyShowing && obs_data_get_bool(request, "toggle"))
		hideName(lt);
	else
		showName(lt, name, subtitle);

	obs_data_set_bool(response, "ok", strcmp(code, "OK") == 0);
	obs_data_set_string(response, "code", code);
	fillState(lt, response);
	obs_source_release(s);
}

static void reqHide(obs_data_t *request, obs_data_t *response, void *)
{
	obs_source_t *s = findLowerThird(request);
	if (!s) {
		obs_data_set_bool(response, "ok", false);
		obs_data_set_string(response, "code", "NO_LOWER_THIRD");
		return;
	}
	hideName(ltOf(s));
	obs_data_set_bool(response, "ok", true);
	obs_data_set_string(response, "code", "OK");
	fillState(ltOf(s), response);
	obs_source_release(s);
}

// Changes are saved into the source's own settings, so OBS remembers them and the settings window shows them.
struct UpdateCtx {
	obs_data_t *change;
	int count;
};

static bool updateEach(void *param, obs_source_t *s)
{
	if (isLowerThird(s)) {
		auto *c = static_cast<UpdateCtx *>(param);
		obs_source_update(s, c->change); // runs lt_update on OBS's next frame
		c->count++;
	}
	return true;
}

// The named Lower Third, or every one when no name is given
static void updateLowerThirds(obs_data_t *request, obs_data_t *change, obs_data_t *response)
{
	UpdateCtx ctx{change, 0};
	const char *name = obs_data_get_string(request, "sourceName");
	if (name && *name) {
		if (obs_source_t *s = findLowerThird(request)) {
			obs_source_update(s, change);
			obs_source_release(s);
			ctx.count = 1;
		}
	} else {
		obs_enum_sources(updateEach, &ctx);
	}
	obs_data_set_bool(response, "ok", ctx.count > 0);
	obs_data_set_string(response, "code", ctx.count > 0 ? "OK" : "NO_LOWER_THIRD");
	obs_data_set_int(response, "updated", ctx.count);
}

static void reqSetEnabled(obs_data_t *request, obs_data_t *response, void *)
{
	obs_data_t *change = obs_data_create();
	obs_data_set_bool(change, "enabled", obs_data_get_bool(request, "enabled"));
	updateLowerThirds(request, change, response);
	obs_data_release(change);
}

static void reqSetTheme(obs_data_t *request, obs_data_t *response, void *)
{
	obs_data_t *change = obs_data_create();
	obs_data_set_string(change, "theme", cleanTheme(obs_data_get_string(request, "theme")).c_str());
	if (obs_data_has_user_value(request, "color"))
		obs_data_set_string(change, "color", cleanColor(obs_data_get_string(request, "color")).c_str());
	updateLowerThirds(request, change, response);
	obs_data_release(change);
}

/* ------------------------------------------------------------------ registration */

void lower_third_load(void)
{
	static obs_source_info info = [] {
		obs_source_info i = {};
		i.id = LT_ID;
		i.type = OBS_SOURCE_TYPE_INPUT;
		i.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_DO_NOT_DUPLICATE;
		i.icon_type = OBS_ICON_TYPE_TEXT;
		i.get_name = lt_get_name;
		i.create = lt_create;
		i.destroy = lt_destroy;
		i.update = lt_update;
		i.get_defaults = lt_defaults;
		i.get_properties = lt_properties;
		i.get_width = lt_width;
		i.get_height = lt_height;
		i.video_render = lt_render;
		i.video_tick = lt_tick;
		i.enum_active_sources = lt_enum_sources;
		i.enum_all_sources = lt_enum_sources;
		return i;
	}();
	obs_register_source(&info);
}

void lower_third_register_requests(obs_websocket_vendor vendor)
{
	ltVendor = vendor;
	obs_websocket_vendor_register_request(vendor, "GetLowerThirdStatus", reqStatus, nullptr);
	obs_websocket_vendor_register_request(vendor, "ShowLowerThird", reqShow, nullptr);
	obs_websocket_vendor_register_request(vendor, "HideLowerThird", reqHide, nullptr);
	obs_websocket_vendor_register_request(vendor, "SetLowerThirdEnabled", reqSetEnabled, nullptr);
	obs_websocket_vendor_register_request(vendor, "SetLowerThirdTheme", reqSetTheme, nullptr);
}
