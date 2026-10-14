/*
 * The Color Match dock (View > Docks > Color Match) for the Church Controller plug-in.
 *
 * A small panel for the Color Match filter (color-match.cpp): Enable, Reference camera, Target camera,
 * Matching mode, Match strength, Match Colors, Reset, and a status line. Choosing a target doesn't change
 * anything by itself; the filter is added to the target the first time you press Match Colors or change one
 * of its settings. Everything is kept in the filter, so the dock, the filter's own settings window in OBS and
 * the BGH LIVE Controller always show the same thing.
 *
 * License: GPL-2.0-or-later (required for OBS plug-ins).
 */

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "color-match.h"

namespace {

struct SourceRef {
	obs_source_t *s;
	explicit SourceRef(obs_source_t *src) : s(src) {}
	~SourceRef() { obs_source_release(s); }
	SourceRef(const SourceRef &) = delete;
	SourceRef &operator=(const SourceRef &) = delete;
	operator obs_source_t *() const { return s; }
};

class ColorMatchDock : public QWidget {
public:
	ColorMatchDock()
	{
		auto *outer = new QVBoxLayout(this);
		status = new QLabel(this);
		status->setWordWrap(true);
		status->setTextFormat(Qt::PlainText);
		status->setMinimumHeight(40);
		outer->addWidget(status);

		enable = new QCheckBox("Enable color matching", this);
		outer->addWidget(enable);

		auto *form = new QFormLayout();
		reference = new QComboBox(this);
		target = new QComboBox(this);
		mode = new QComboBox(this);
		mode->addItem("One-time match", "once");
		mode->addItem("Continuous", "continuous");
		reference->setToolTip("The camera whose colors are kept. It's never changed.");
		target->setToolTip("The camera whose colors are adjusted to match the reference.");
		mode->setToolTip("One-time: match when you press Match Colors.\n"
				 "Continuous: keep matching every few seconds, easing gently into lighting changes.");
		form->addRow("Reference", reference);
		form->addRow("Target", target);
		form->addRow("Mode", mode);

		auto *strengthRow = new QHBoxLayout();
		strength = new QSlider(Qt::Horizontal, this);
		strength->setRange(0, 100);
		strength->setValue(100);
		strengthLabel = new QLabel("100%", this);
		strengthLabel->setMinimumWidth(40);
		strengthRow->addWidget(strength, 1);
		strengthRow->addWidget(strengthLabel);
		form->addRow("Strength", strengthRow);
		outer->addLayout(form);

		auto *buttons = new QHBoxLayout();
		match = new QPushButton("Match Colors", this);
		reset = new QPushButton("Reset", this);
		buttons->addWidget(match, 2);
		buttons->addWidget(reset, 1);
		outer->addLayout(buttons);
		outer->addStretch(1);

		connect(target, &QComboBox::currentIndexChanged, this, [this] { poll(true); });
		connect(reference, &QComboBox::activated, this, [this] { referenceChosen(); });
		connect(mode, &QComboBox::activated, this, [this] { apply(); });
		connect(strength, &QSlider::valueChanged, this, [this](int v) {
			strengthLabel->setText(QString::number(v) + "%");
			if (!syncing)
				apply();
		});
		connect(enable, &QCheckBox::toggled, this, [this](bool on) {
			if (syncing)
				return;
			SourceRef f(filter(true));
			if (f)
				obs_source_set_enabled(f, on);
			poll(false);
		});
		connect(match, &QPushButton::clicked, this, [this] { matchNow(); });
		connect(reset, &QPushButton::clicked, this, [this] {
			SourceRef f(filter(false));
			if (f)
				colormatch::reset(f);
			poll(false);
		});

		timer = new QTimer(this);
		connect(timer, &QTimer::timeout, this, [this] { poll(false); });
		timer->start(500);
		poll(true);
	}

private:
	QLabel *status, *strengthLabel;
	QCheckBox *enable;
	QComboBox *reference, *target, *mode;
	QSlider *strength;
	QPushButton *match, *reset;
	QTimer *timer;
	bool syncing = false;
	QString errorText; // a problem with the last thing the operator chose, shown until the next change
	std::vector<colormatch::SourceInfo> lastSources;

	// The Color Match filter on the chosen target (new reference, or null). create: add one if missing.
	obs_source_t *filter(bool create)
	{
		const QString name = target->currentData().toString();
		if (name.isEmpty())
			return nullptr;
		SourceRef t(obs_get_source_by_name(name.toUtf8().constData()));
		return create ? colormatch::ensureFilter(t) : colormatch::findFilter(t);
	}

	void fill(QComboBox *box, const std::vector<colormatch::SourceInfo> &sources, const QString &keep)
	{
		box->clear();
		box->addItem("Choose a camera", QString());
		for (const colormatch::SourceInfo &s : sources) {
			const QString n = QString::fromStdString(s.name);
			box->addItem(n, n);
		}
		const int i = box->findData(keep);
		box->setCurrentIndex(i < 0 ? 0 : i);
	}

	// A target to start with: one that already has Color Match, otherwise the camera in "Camera 2"
	QString startingTarget(const std::vector<colormatch::SourceInfo> &sources)
	{
		for (const colormatch::SourceInfo &s : sources) {
			SourceRef src(obs_get_source_by_name(s.name.c_str()));
			SourceRef f(colormatch::findFilter(src));
			if (f)
				return QString::fromStdString(s.name);
		}
		return QString::fromStdString(colormatch::cameraInScene("Camera 2"));
	}

	void poll(bool targetChanged)
	{
		syncing = true;
		const std::vector<colormatch::SourceInfo> sources = colormatch::videoSources();
		bool listChanged = sources.size() != lastSources.size();
		for (size_t i = 0; !listChanged && i < sources.size(); ++i)
			listChanged = sources[i].name != lastSources[i].name;
		if (listChanged) {
			// Sources were added, removed or renamed, or another scene collection was loaded
			QString keepTarget = target->currentData().toString();
			if (keepTarget.isEmpty() || target->findData(keepTarget) < 0 || lastSources.empty())
				keepTarget = startingTarget(sources);
			const QString keepRef = reference->currentData().toString();
			target->blockSignals(true);
			fill(target, sources, keepTarget);
			target->blockSignals(false);
			fill(reference, sources, keepRef);
			lastSources = sources;
			targetChanged = true;
		}
		if (targetChanged)
			errorText.clear();

		SourceRef f(filter(false));
		colormatch::Status st;
		const bool have = f && colormatch::status(f, st);
		if (have) {
			if (!reference->hasFocus() && !st.reference.empty()) {
				const int i = reference->findData(QString::fromStdString(st.reference));
				if (i >= 0)
					reference->setCurrentIndex(i);
			}
			if (!mode->hasFocus())
				mode->setCurrentIndex(st.mode == "continuous" ? 1 : 0);
			if (!strength->isSliderDown() && strength->value() != st.strength) {
				strength->setValue(st.strength);
				strengthLabel->setText(QString::number(st.strength) + "%");
			}
			enable->setChecked(st.enabled);
		} else if (targetChanged) {
			// Not set up for this camera yet: suggest the camera in "Camera 1" as the reference
			if (reference->currentData().toString().isEmpty()) {
				const int i = reference->findData(
					QString::fromStdString(colormatch::cameraInScene("Camera 1")));
				if (i >= 0)
					reference->setCurrentIndex(i);
			}
			mode->setCurrentIndex(0);
			strength->setValue(100);
			strengthLabel->setText("100%");
			enable->setChecked(true);
		}

		const bool hasTarget = !target->currentData().toString().isEmpty();
		match->setEnabled(hasTarget && !(have && st.busy));
		reset->setEnabled(have && (st.applied || st.state != "none"));
		QString text;
		if (!errorText.isEmpty())
			text = errorText;
		else if (!hasTarget)
			text = "Choose the camera to adjust (Target).";
		else if (!have)
			text = "Not set up for this camera yet. Check the reference, then press Match Colors.";
		else {
			text = QString::fromStdString(st.message);
			if (!st.enabled)
				text = "Switched off. " + text;
			if (st.matchedAt > 0 && st.state == "matched")
				text += "\nLast matched " +
					QDateTime::fromMSecsSinceEpoch(st.matchedAt).toString("h:mm ap, MMM d") + ".";
		}
		status->setText(text);
		syncing = false;
	}

	// Saves the dock's choices into the filter (adding it if needed). Returns false if the reference is wrong.
	bool apply()
	{
		if (syncing)
			return false;
		errorText.clear();
		SourceRef f(filter(true));
		if (!f) {
			poll(false);
			return false;
		}
		const std::string ref = reference->currentData().toString().toStdString();
		std::string message;
		if (!ref.empty() && !colormatch::checkReference(f, ref, message).empty()) {
			errorText = QString::fromStdString(message);
			colormatch::setOptions(f, "", mode->currentData().toString().toStdString(), strength->value());
			poll(false);
			return false;
		}
		colormatch::setOptions(f, ref, mode->currentData().toString().toStdString(), strength->value());
		poll(false);
		return true;
	}

	void referenceChosen() { apply(); }

	void matchNow()
	{
		errorText.clear();
		const std::string ref = reference->currentData().toString().toStdString();
		SourceRef f(filter(true));
		if (!f) {
			errorText = "Color Match can't be added to that source.";
			poll(false);
			return;
		}
		std::string message;
		if (!colormatch::checkReference(f, ref, message).empty()) {
			errorText = QString::fromStdString(message);
			poll(false);
			return;
		}
		colormatch::setOptions(f, ref, mode->currentData().toString().toStdString(), strength->value());
		if (!obs_source_enabled(f))
			obs_source_set_enabled(f, true);
		colormatch::requestMatch(f);
		poll(false);
	}
};

} // namespace

void color_match_dock_load(void)
{
	auto *dock = new ColorMatchDock();
	if (!obs_frontend_add_dock_by_id("church-color-match", "Color Match", dock))
		delete dock;
}
