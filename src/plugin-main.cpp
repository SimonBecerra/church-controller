/*
 * Church Controller companion plug-in for OBS Studio (written against OBS 32.2.2)
 *
 * Adds commands to obs-websocket under the vendor name "church-controller" so the
 * touchscreen controller can use OBS's own YouTube features:
 *
 *   GetYouTubeStatus       Is OBS signed in to YouTube (Settings > Stream > Connect Account)?
 *   ShowOBS                Bring the OBS window to the front (used to send the operator to OBS).
 *   OpenStreamStart        Bring OBS to the front and run its own "Start Streaming" action.
 *   ListYouTubeBroadcasts  Read the upcoming broadcasts from OBS's "Select Existing Broadcast" tab.
 *   StartYouTubeBroadcast  Fill in OBS's "Create New Broadcast" tab (title, description, privacy only,
 *                          with "Remember these settings" ticked) and press OBS's go-live button,
 *                          or select an existing broadcast and start it.
 *
 * It also adds two sources with their own commands: "Lower Third" (lower-third.cpp) and
 * "Scripture & Hymns" (text-display.cpp), and the "Color Match" filter with its dock (color-match.cpp).
 *
 * It can also open the BGH LIVE Controller when OBS starts, if that's turned on in the controller's Settings.
 *
 * Every OBS setting other than those three fields is left exactly as OBS remembers it.
 * Nothing is ever started on a guess: if a field can't be found or checked, the plug-in stops,
 * leaves OBS's dialog open, and reports a problem code so the controller can send the operator to OBS.
 *
 * How results come back
 *   OpenStreamStart answers immediately.
 *   The two YouTube commands answer immediately with {"accepted": true, "jobId": "..."} and report the
 *   outcome later as a vendor event ("BroadcastList" or "BroadcastResult") carrying the same jobId:
 *     { jobId, ok, code, message, ... }
 *   Codes: OK, ALREADY_LIVE, OBS_BUSY, NOT_SIGNED_IN, SIGN_IN_EXPIRED, DIALOG_CHANGED,
 *          INCOMPLETE, NOT_FOUND, YOUTUBE_ERROR, TIMEOUT
 *
 * License: GPL-2.0-or-later (required for OBS plug-ins).
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QTabWidget>
#include <QTextDocument>
#include <QTimer>

#include <atomic>
#include <cstring>
#include <functional>
#include <memory>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <util/platform.h>
#endif

#include "obs-websocket-api.h"
#include "color-match.h"
#include "sources.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("church-controller", "en-US")

#define LOG(level, fmt, ...) blog(level, "[church-controller] " fmt, ##__VA_ARGS__)

static obs_websocket_vendor vendor = nullptr;
static std::atomic<int> nextJob{1};
static std::atomic<bool> jobRunning{false};

/* ------------------------------------------------------------------ helpers */

/*
 * Signed in to YouTube? OBS keeps the connected account in the current profile's settings (basic.ini):
 *   [Auth] Type = "YouTube - RTMPS" (the connected service), and the tokens in a section named
 *   "YouTube" (YoutubeAuth's SECTION_NAME), with RefreshToken set while signed in.
 * "Disconnect Account" removes Auth/Type. (frontend/oauth/Auth.cpp and YoutubeAuth.cpp in OBS)
 */
static bool youtubeSignedIn()
{
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg)
		return false;
	const char *type = config_get_string(cfg, "Auth", "Type");
	if (!type || !strstr(type, "YouTube"))
		return false;
	const char *token = config_get_string(cfg, "YouTube", "RefreshToken");
	if (!token || !*token)
		token = config_get_string(cfg, type, "RefreshToken"); // in case a future OBS keeps it by service name
	return token && *token;
}

static QMainWindow *obsMainWindow()
{
	return static_cast<QMainWindow *>(obs_frontend_get_main_window());
}

// obs-websocket calls our requests on its own threads; anything touching Qt must run on the UI thread.
static void runOnUi(std::function<void()> fn)
{
	QMetaObject::invokeMethod(qApp, std::move(fn), Qt::QueuedConnection);
}

// Bring the OBS window to the front on its monitor without changing its size.
static void bringObsForward()
{
	QMainWindow *w = obsMainWindow();
	if (!w)
		return;
	if (w->isMinimized())
		w->setWindowState((w->windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
	w->show();
	w->raise();
	w->activateWindow();
#ifdef _WIN32
	// Windows only lets the foreground program hand focus to another one. The controller app also calls
	// AllowSetForegroundWindow(obs pid) before sending the command; this is the fallback.
	HWND hwnd = reinterpret_cast<HWND>(w->winId());
	HWND fg = GetForegroundWindow();
	DWORD fgThread = GetWindowThreadProcessId(fg, nullptr);
	DWORD myThread = GetCurrentThreadId();
	if (fg && fgThread != myThread) {
		AttachThreadInput(fgThread, myThread, TRUE);
		SetForegroundWindow(hwnd);
		AttachThreadInput(fgThread, myThread, FALSE);
	} else {
		SetForegroundWindow(hwnd);
	}
#endif
}

// Send the outcome of a YouTube command back to the controller.
// Takes ownership of `data` (may be null).
static void emitResult(const char *eventName, const QString &jobId, bool ok, const char *code, const QString &message,
		       obs_data_t *data = nullptr)
{
	obs_data_t *d = data ? data : obs_data_create();
	obs_data_set_string(d, "jobId", jobId.toUtf8().constData());
	obs_data_set_bool(d, "ok", ok);
	obs_data_set_string(d, "code", code);
	obs_data_set_string(d, "message", message.toUtf8().constData());
	if (vendor)
		obs_websocket_vendor_emit_event(vendor, eventName, d);
	obs_data_release(d);
	LOG(ok ? LOG_INFO : LOG_WARNING, "%s %s: %s %s", eventName, jobId.toUtf8().constData(), code,
	    message.toUtf8().constData());
	jobRunning = false;
}

// Check `ready` every 200 ms until it returns true or `timeoutMs` passes, then call `done(ready)`.
// Uses timers, so it keeps working inside OBS's modal dialog (which runs its own event loop).
static void waitFor(std::function<bool()> ready, int timeoutMs, std::function<void(bool)> done)
{
	auto *timer = new QTimer(qApp);
	const qint64 start = QDateTime::currentMSecsSinceEpoch();
	timer->setInterval(200);
	QObject::connect(timer, &QTimer::timeout, [=]() {
		const bool ok = ready();
		if (ok || QDateTime::currentMSecsSinceEpoch() - start > timeoutMs) {
			timer->stop();
			timer->deleteLater();
			done(ok);
		}
	});
	timer->start();
}

// OBS's YouTube "Manage Broadcast" dialog (objectName comes from OBSYoutubeActions.ui)
static QDialog *findYouTubeDialog()
{
	for (QWidget *w : QApplication::topLevelWidgets())
		if (w->isVisible() && w->objectName() == QLatin1String("OBSYoutubeActions"))
			return qobject_cast<QDialog *>(w);
	return nullptr;
}

// Any visible OBS message box (sign-in prompts, YouTube errors). Returns its text, or empty.
// Any message OBS is showing, including ones that belong to its YouTube window (those aren't top-level)
static QString visibleMessageBoxText()
{
	for (QWidget *w : QApplication::allWidgets())
		if (auto *box = qobject_cast<QMessageBox *>(w); box && box->isVisible()) {
			QString t = box->text();
			if (!box->informativeText().isEmpty())
				t += " " + box->informativeText();
			QTextDocument doc;
			doc.setHtml(t);
			return doc.toPlainText().trimmed();
		}
	return QString();
}

template<typename T> static T *child(QWidget *parent, const char *name)
{
	return parent ? parent->findChild<T *>(QLatin1String(name)) : nullptr;
}

// Existing-broadcast rows: labels OBS creates at runtime inside the "Select Existing Broadcast" tab.
// The four sample labels from the .ui file have names (label_11 ... label_14) and are skipped.
static QList<QLabel *> broadcastRows(QDialog *dlg)
{
	QList<QLabel *> rows;
	QWidget *list = child<QWidget>(dlg, "scrollAreaWidgetContents");
	if (!list)
		return rows;
	for (QLabel *l : list->findChildren<QLabel *>(QString(), Qt::FindDirectChildrenOnly))
		if (l->objectName().isEmpty() && l->isVisible() && l->metaObject()->indexOfSignal("clicked()") >= 0)
			rows << l;
	return rows;
}

// Row text is "<big>Title</big><br/>Scheduled for ..." -> {title, detail}
static QPair<QString, QString> parseRow(QLabel *l)
{
	const QString html = l->text();
	QString title, detail;
	QRegularExpressionMatch m =
		QRegularExpression("<big>(.*?)</big>", QRegularExpression::DotMatchesEverythingOption).match(html);
	QTextDocument doc;
	if (m.hasMatch()) {
		doc.setHtml(m.captured(1));
		title = doc.toPlainText().trimmed();
		doc.setHtml(QString(html).remove(m.capturedStart(0), m.capturedLength(0)));
		detail = doc.toPlainText().trimmed();
	} else {
		doc.setHtml(html);
		title = doc.toPlainText().trimmed();
	}
	return {title, detail};
}

/*
 * Open OBS's YouTube dialog (the same thing OBS's "Manage Broadcast" button does), then call `ready(dialog)`.
 * Reports a problem code instead if OBS is busy, not signed in, or shows a message instead of the dialog.
 */
static void openYouTubeDialog(const QString &jobId, const char *eventName, std::function<void(QDialog *)> ready)
{
	if (obs_frontend_streaming_active())
		return emitResult(eventName, jobId, false, "ALREADY_LIVE", "OBS is already streaming.");
	// Check the sign-in first: OBS only opens its YouTube dialog when an account is connected
	if (!youtubeSignedIn())
		return emitResult(
			eventName, jobId, false, "NOT_SIGNED_IN",
			"OBS isn't signed in to YouTube. In OBS, open Settings > Stream and press Connect Account.");
	if (findYouTubeDialog() || QApplication::activeModalWidget())
		return emitResult(eventName, jobId, false, "OBS_BUSY", "Another window is open in OBS.");

	QMainWindow *main = obsMainWindow();
	bringObsForward();
	// SetupBroadcast is OBS's own slot behind the "Manage Broadcast" button. It shows the dialog with exec(),
	// so it's queued; our timers keep running inside the dialog's event loop.
	if (!QMetaObject::invokeMethod(main, "SetupBroadcast", Qt::QueuedConnection))
		return emitResult(eventName, jobId, false, "DIALOG_CHANGED",
				  "This version of OBS has no SetupBroadcast action.");

	waitFor([] { return findYouTubeDialog() != nullptr || !visibleMessageBoxText().isEmpty(); }, 8000,
		[=](bool appeared) {
			if (QDialog *dlg = findYouTubeDialog())
				return ready(dlg);
			const QString box = visibleMessageBoxText();
			if (!box.isEmpty())
				return emitResult(eventName, jobId, false, "SIGN_IN_EXPIRED", box);
			emitResult(
				eventName, jobId, false, appeared ? "OBS_BUSY" : "NOT_SIGNED_IN",
				"OBS didn't open its YouTube dialog. Check that OBS is connected to YouTube under Settings > Stream.");
		});
}

// After pressing OBS's go-live button: wait until OBS is streaming, or report what OBS showed instead.
// After "Create broadcast and start streaming": wait for the stream, an error message, or OBS's YouTube window
// being closed without the stream starting (cancelled, or closed after an error).
static void waitForStreamStart(const QString &jobId)
{
	auto closedSince = std::make_shared<qint64>(0);
	waitFor(
		[closedSince] {
			if (obs_frontend_streaming_active() || !visibleMessageBoxText().isEmpty())
				return true;
			if (findYouTubeDialog()) {
				*closedSince = 0;
				return false;
			}
			// OBS closes its window itself when it succeeds, then starts streaming a moment later
			const qint64 now = QDateTime::currentMSecsSinceEpoch();
			if (!*closedSince)
				*closedSince = now;
			return now - *closedSince > 15000;
		},
		60000,
		[=](bool) {
			if (obs_frontend_streaming_active())
				return emitResult("BroadcastResult", jobId, true, "OK", "Streaming started.");
			const QString box = visibleMessageBoxText();
			if (!box.isEmpty())
				return emitResult("BroadcastResult", jobId, false, "YOUTUBE_ERROR", box);
			if (!findYouTubeDialog())
				return emitResult("BroadcastResult", jobId, false, "CANCELLED",
						  "OBS's YouTube window was closed without the stream starting.");
			emitResult("BroadcastResult", jobId, false, "TIMEOUT",
				   "OBS didn't start streaming. Check OBS on the main monitor.");
		});
}

/* ------------------------------------------------------------------ requests */

static void reqGetYouTubeStatus(obs_data_t *, obs_data_t *response, void *)
{
	obs_data_set_bool(response, "signedIn", youtubeSignedIn());
	obs_data_set_bool(response, "streaming", obs_frontend_streaming_active());
}

static void reqShowOBS(obs_data_t *, obs_data_t *response, void *)
{
	runOnUi([] { bringObsForward(); });
	obs_data_set_bool(response, "shown", true);
}

static void reqOpenStreamStart(obs_data_t *, obs_data_t *response, void *)
{
	if (obs_frontend_streaming_active()) {
		obs_data_set_bool(response, "opened", false);
		obs_data_set_string(response, "code", "ALREADY_LIVE");
		return;
	}
	if (!youtubeSignedIn()) {
		obs_data_set_bool(response, "opened", false);
		obs_data_set_string(response, "code", "NOT_SIGNED_IN");
		return;
	}
	runOnUi([] {
		bringObsForward();
		// Same action as clicking OBS's "Start Streaming" button, including any dialog OBS shows first.
		QMetaObject::invokeMethod(obsMainWindow(), "StreamActionTriggered", Qt::QueuedConnection);
	});
	obs_data_set_bool(response, "opened", true);
}

static void reqListBroadcasts(obs_data_t *, obs_data_t *response, void *)
{
	if (jobRunning.exchange(true)) {
		obs_data_set_bool(response, "accepted", false);
		obs_data_set_string(response, "code", "OBS_BUSY");
		return;
	}
	const QString jobId = QString::number(nextJob++);
	obs_data_set_bool(response, "accepted", true);
	obs_data_set_string(response, "jobId", jobId.toUtf8().constData());

	runOnUi([jobId] {
		openYouTubeDialog(jobId, "BroadcastList", [jobId](QDialog *dlg) {
			auto *tabs = child<QTabWidget>(dlg, "tabWidget");
			if (!tabs)
				return emitResult("BroadcastList", jobId, false, "DIALOG_CHANGED",
						  "OBS's YouTube dialog has changed.");
			tabs->setCurrentIndex(1); // "Select Existing Broadcast"
			QPointer<QDialog> guard(dlg);
			// OBS loads the list from YouTube in the background; wait for rows (or give up after 10 s = none).
			waitFor([guard] { return guard && !broadcastRows(guard).isEmpty(); }, 10000,
				[jobId, guard](bool) {
					obs_data_t *data = obs_data_create();
					obs_data_array_t *arr = obs_data_array_create();
					if (guard) {
						const QList<QLabel *> rows = broadcastRows(guard);
						for (int i = 0; i < rows.size(); ++i) {
							const auto [title, detail] = parseRow(rows[i]);
							obs_data_t *item = obs_data_create();
							obs_data_set_int(item, "index", i);
							obs_data_set_string(item, "title", title.toUtf8().constData());
							obs_data_set_string(item, "detail",
									    detail.toUtf8().constData());
							obs_data_array_push_back(arr, item);
							obs_data_release(item);
						}
						// Close OBS's dialog the normal way (stops its background work)
						if (auto *cancel = child<QPushButton>(guard, "cancelButton"))
							cancel->click();
						else
							guard->reject();
					}
					obs_data_set_array(data, "broadcasts", arr);
					obs_data_array_release(arr);
					emitResult("BroadcastList", jobId, true, "OK", "", data);
				});
		});
	});
}

static void reqStartBroadcast(obs_data_t *request, obs_data_t *response, void *)
{
	if (jobRunning.exchange(true)) {
		obs_data_set_bool(response, "accepted", false);
		obs_data_set_string(response, "code", "OBS_BUSY");
		return;
	}
	const QString mode = obs_data_get_string(request, "mode");
	const QString title = obs_data_get_string(request, "title");
	const QString description = obs_data_get_string(request, "description");
	const QString privacy = obs_data_get_string(request, "privacyStatus"); // public | unlisted | private
	const QString existingTitle = obs_data_get_string(request, "broadcastTitle");
	const long long existingIndex =
		obs_data_has_user_value(request, "broadcastIndex") ? obs_data_get_int(request, "broadcastIndex") : -1;

	const QString jobId = QString::number(nextJob++);
	obs_data_set_bool(response, "accepted", true);
	obs_data_set_string(response, "jobId", jobId.toUtf8().constData());

	runOnUi([=] {
		openYouTubeDialog(jobId, "BroadcastResult", [=](QDialog *dlg) {
			auto *tabs = child<QTabWidget>(dlg, "tabWidget");
			auto *okButton = child<QPushButton>(dlg, "okButton");
			if (!tabs || !okButton)
				return emitResult("BroadcastResult", jobId, false, "DIALOG_CHANGED",
						  "OBS's YouTube dialog has changed.");

			if (mode == "existing") {
				/* ---------- Select an existing broadcast ---------- */
				tabs->setCurrentIndex(1);
				QPointer<QDialog> guard(dlg);
				waitFor([guard] { return guard && !broadcastRows(guard).isEmpty(); }, 10000,
					[=](bool found) {
						if (!guard)
							return emitResult("BroadcastResult", jobId, false,
									  "YOUTUBE_ERROR", "OBS's dialog closed.");
						if (!found)
							return emitResult(
								"BroadcastResult", jobId, false, "NOT_FOUND",
								"OBS shows no upcoming broadcasts. Finish in OBS.");
						// Match by title (index only breaks ties), so a reordered list can't pick the wrong one
						QLabel *pick = nullptr;
						const QList<QLabel *> rows = broadcastRows(guard);
						for (int i = 0; i < rows.size(); ++i)
							if (parseRow(rows[i]).first == existingTitle &&
							    (!pick || i == existingIndex))
								pick = rows[i];
						if (!pick)
							return emitResult(
								"BroadcastResult", jobId, false, "NOT_FOUND",
								"That broadcast isn't in OBS's list anymore. Finish in OBS.");
						QMetaObject::invokeMethod(pick,
									  "clicked"); // same as tapping the row in OBS
						QTimer::singleShot(150, [=] {
							if (!guard)
								return;
							auto *ok = child<QPushButton>(guard, "okButton");
							if (!ok || !ok->isEnabled())
								return emitResult(
									"BroadcastResult", jobId, false, "INCOMPLETE",
									"OBS needs more information in its dialog. Finish in OBS.");
							ok->click();
							waitForStreamStart(jobId);
						});
					});
				return;
			}

			/* ---------- Create a new broadcast: title, description, privacy only ---------- */
			auto *titleEdit = child<QLineEdit>(dlg, "title");
			auto *descEdit = child<QPlainTextEdit>(dlg, "description");
			auto *privacyBox = child<QComboBox>(dlg, "privacyBox");
			auto *remember = child<QCheckBox>(dlg, "checkRememberSettings");
			auto *later = child<QCheckBox>(dlg, "checkScheduledLater");
			if (!titleEdit || !descEdit || !privacyBox || !remember)
				return emitResult("BroadcastResult", jobId, false, "DIALOG_CHANGED",
						  "OBS's YouTube dialog has changed.");
			const int privacyIndex = privacyBox->findData(privacy);
			if (privacyIndex < 0)
				return emitResult("BroadcastResult", jobId, false, "DIALOG_CHANGED",
						  "OBS's privacy choices have changed.");

			tabs->setCurrentIndex(0);
			titleEdit->setText(title);
			descEdit->setPlainText(description);
			privacyBox->setCurrentIndex(privacyIndex);
			remember->setChecked(true);
			if (later)
				later->setChecked(false); // "Schedule for later" would stop OBS from going live now

			// Check every field we touched before pressing anything
			QPointer<QDialog> guard(dlg);
			QTimer::singleShot(150, [=] {
				if (!guard)
					return emitResult("BroadcastResult", jobId, false, "YOUTUBE_ERROR",
							  "OBS's dialog closed.");
				const bool filled = titleEdit->text() == title &&
						    descEdit->toPlainText() == description &&
						    privacyBox->currentData().toString() == privacy &&
						    remember->isChecked() && (!later || !later->isChecked());
				if (!filled)
					return emitResult(
						"BroadcastResult", jobId, false, "DIALOG_CHANGED",
						"The controller couldn't fill in OBS's dialog. Finish in OBS.");
				auto *ok = child<QPushButton>(guard, "okButton");
				if (!ok || !ok->isEnabled())
					return emitResult(
						"BroadcastResult", jobId, false, "INCOMPLETE",
						"OBS needs more information in its dialog (for example \"Made for kids\"). Finish in OBS.");
				ok->click(); // OBS's "Create broadcast and start streaming"
				waitForStreamStart(jobId);
			});
		});
	});
}

/* ------------------------------------------------------------------ open the controller with OBS */

/*
 * The controller saves %APPDATA%\BGH LIVE Controller\open-with-obs.json:
 *   { "enabled": true, "exePath": "C:\\...\\BGH LIVE Controller.exe" }
 * When enabled, OBS starting opens the controller. The controller only allows one copy of itself,
 * so this is harmless if it's already open. It connects as soon as OBS's WebSocket server is ready.
 */
static void openControllerIfWanted()
{
#ifdef _WIN32
	PWSTR appdata = nullptr;
	if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata) != S_OK)
		return;
	char *appdataUtf8 = nullptr;
	os_wcs_to_utf8_ptr(appdata, 0, &appdataUtf8);
	CoTaskMemFree(appdata);
	if (!appdataUtf8)
		return;
	const QString file = QString::fromUtf8(appdataUtf8) + "\\BGH LIVE Controller\\open-with-obs.json";
	bfree(appdataUtf8);

	obs_data_t *cfg = obs_data_create_from_json_file(file.toUtf8().constData());
	if (!cfg)
		return; // the controller hasn't saved the setting yet
	const bool enabled = obs_data_get_bool(cfg, "enabled");
	const QString exe = QString::fromUtf8(obs_data_get_string(cfg, "exePath"));
	obs_data_release(cfg);
	if (!enabled || exe.isEmpty())
		return;

	HINSTANCE r = ShellExecuteW(nullptr, L"open", reinterpret_cast<LPCWSTR>(exe.utf16()), L"--opened-by-obs",
				    nullptr, SW_SHOWNORMAL);
	if (reinterpret_cast<INT_PTR>(r) <= 32)
		LOG(LOG_WARNING, "couldn't open the controller at %s", exe.toUtf8().constData());
	else
		LOG(LOG_INFO, "opened the controller");
#endif
}

/* ------------------------------------------------------------------ start the stream on the title slide */

/*
 * The controller saves %APPDATA%\BGH LIVE Controller\start-title.json:
 *   { "enabled": true, "scenes": { "Sunday Service": "Title Slide", ... } }   (title slide per scene collection)
 * When enabled, starting the stream (from OBS or the controller) first puts that collection's title slide on
 * Program with a cut, in the moment before the stream starts, so viewers never see what was there before.
 * A collection that isn't listed uses a scene called "Title Slide", if it has one.
 */
static obs_data_t *readControllerFile(const char *name)
{
#ifdef _WIN32
	PWSTR appdata = nullptr;
	if (SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata) != S_OK)
		return nullptr;
	char *appdataUtf8 = nullptr;
	os_wcs_to_utf8_ptr(appdata, 0, &appdataUtf8);
	CoTaskMemFree(appdata);
	if (!appdataUtf8)
		return nullptr;
	const QString file = QString::fromUtf8(appdataUtf8) + "\\BGH LIVE Controller\\" + QString::fromUtf8(name);
	bfree(appdataUtf8);
	return obs_data_create_from_json_file(file.toUtf8().constData());
#else
	(void)name;
	return nullptr;
#endif
}

static obs_source_t *titleSlideFor(const char *collection)
{
	obs_data_t *cfg = readControllerFile("start-title.json");
	if (!cfg)
		return nullptr;
	obs_source_t *found = nullptr;
	if (obs_data_get_bool(cfg, "enabled")) {
		obs_data_t *scenes = obs_data_get_obj(cfg, "scenes");
		const char *want = scenes && collection ? obs_data_get_string(scenes, collection) : "";
		obs_data_release(scenes);
		if (want && *want) {
			found = obs_get_source_by_name(want);
		} else {
			// Not listed: a scene called "Title Slide" (any capitals)
			obs_frontend_source_list list = {};
			obs_frontend_get_scenes(&list);
			for (size_t i = 0; i < list.sources.num && !found; ++i) {
				const QString n =
					QString::fromUtf8(obs_source_get_name(list.sources.array[i])).trimmed();
				if (n.compare("Title Slide", Qt::CaseInsensitive) == 0)
					found = obs_source_get_ref(list.sources.array[i]);
			}
			obs_frontend_source_list_free(&list);
		}
		if (found && !obs_source_is_scene(found)) {
			obs_source_release(found);
			found = nullptr;
		}
	}
	obs_data_release(cfg);
	return found;
}

static void titleBeforeStreaming()
{
	char *collection = obs_frontend_get_current_scene_collection();
	obs_source_t *title = titleSlideFor(collection);
	bfree(collection);
	if (!title)
		return;
	obs_source_t *now = obs_frontend_get_current_scene();
	if (now != title) {
		// A cut, so the stream's first picture is the title slide and not the middle of a fade
		obs_source_t *was = obs_frontend_get_current_transition();
		obs_source_t *cut = nullptr;
		obs_frontend_source_list list = {};
		obs_frontend_get_transitions(&list);
		for (size_t i = 0; i < list.sources.num && !cut; ++i)
			if (strcmp(obs_source_get_unversioned_id(list.sources.array[i]), "cut_transition") == 0)
				cut = obs_source_get_ref(list.sources.array[i]);
		obs_frontend_source_list_free(&list);
		if (cut && cut != was)
			obs_frontend_set_current_transition(cut);
		obs_frontend_set_current_scene(title);
		if (cut && was && cut != was)
			obs_frontend_set_current_transition(was);
		obs_source_release(cut);
		obs_source_release(was);
		LOG(LOG_INFO, "stream starting: switched to %s first", obs_source_get_name(title));
	}
	obs_source_release(now);
	obs_source_release(title);
}

/* ------------------------------------------------------------------ module */

bool obs_module_load(void)
{
	lower_third_load();
	text_display_load();
	color_match_load();
	LOG(LOG_INFO, "loaded");
	return true;
}

// Open the controller once OBS's window is up, so the controller opens in front of OBS instead of behind it
static void onFrontendEvent(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING)
		openControllerIfWanted();
	else if (event == OBS_FRONTEND_EVENT_STREAMING_STARTING)
		titleBeforeStreaming();
}

void obs_module_post_load(void)
{
	obs_frontend_add_event_callback(onFrontendEvent, nullptr);
	color_match_dock_load();
	vendor = obs_websocket_register_vendor("church-controller");
	if (!vendor) {
		LOG(LOG_WARNING, "obs-websocket isn't available; the controller's Go Live features won't work.");
		return;
	}
	obs_websocket_vendor_register_request(vendor, "GetYouTubeStatus", reqGetYouTubeStatus, nullptr);
	obs_websocket_vendor_register_request(vendor, "ShowOBS", reqShowOBS, nullptr);
	obs_websocket_vendor_register_request(vendor, "OpenStreamStart", reqOpenStreamStart, nullptr);
	obs_websocket_vendor_register_request(vendor, "ListYouTubeBroadcasts", reqListBroadcasts, nullptr);
	obs_websocket_vendor_register_request(vendor, "StartYouTubeBroadcast", reqStartBroadcast, nullptr);
	lower_third_register_requests(vendor);
	text_display_register_requests(vendor);
	color_match_register_requests(vendor);
	audio_level_register_requests(vendor);
	LOG(LOG_INFO, "commands registered with obs-websocket");
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
	color_match_unload();
	audio_level_unload();
	LOG(LOG_INFO, "unloaded");
}
