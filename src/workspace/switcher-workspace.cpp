#include "switcher-workspace.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <utility>
#include <vector>

#include <QApplication>
#include <QAbstractItemView>
#include <QAction>
#include <QAbstractSpinBox>
#include <QBoxLayout>
#include <QComboBox>
#include <QCheckBox>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDockWidget>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QKeyEvent>
#include <QFormLayout>
#include <QGuiApplication>
#include <QGroupBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScreen>
#include <QScopedValueRollback>
#include <QPalette>
#include <QShowEvent>
#include <QHideEvent>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSlider>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QStringList>
#include <QTabWidget>
#include <QToolButton>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWindow>
#include <QWidgetAction>
#include <QSpinBox>

#include <obs-module.h>

#include "switcher-ui.hpp"
#include "switcher-remote-manager.hpp"
#include "util/config-file.h"

#ifndef QT_UTF8
#define QT_UTF8(str) QString::fromUtf8(str)
#endif
#ifndef QT_TO_UTF8
#define QT_TO_UTF8(str) str.toUtf8().constData()
#endif

extern void SwitcherEmitVendorEvent(const char *eventName, obs_data_t *data);

namespace {
constexpr int kMaxSwitcherSlots = 25;
constexpr auto kRemoteStateKey = "remote";
constexpr auto kCanvasStateKey = "canvas";
constexpr auto kAutomationStateKey = "automation";
constexpr auto kMotionStateKey = "motion";
constexpr auto kSwitchVerticalDockId = "switch-vertical-canvas";
constexpr auto kSwitchVerticalScenesDockId = "switch-vertical-scenes";
constexpr auto kSwitchVerticalSourcesDockId = "switch-vertical-sources";
constexpr auto kSwitchVerticalTransitionsDockId = "switch-vertical-transitions";
constexpr auto kSwitchVerticalSettingsDockId = "switch-vertical-settings";
constexpr int kSourceItemIdRole = Qt::UserRole + 30;
constexpr int kSourceItemVisibleRole = Qt::UserRole + 31;
constexpr int kSourceItemLockedRole = Qt::UserRole + 32;
constexpr int kSourceItemNameRole = Qt::UserRole + 33;
constexpr int kMotionShotSceneUuidRole = Qt::UserRole + 40;
constexpr int kMotionShotSceneNameRole = Qt::UserRole + 41;
constexpr int kMotionShotItemIdRole = Qt::UserRole + 42;
constexpr int kMotionShotSourceUuidRole = Qt::UserRole + 43;
constexpr int kMotionShotSourceNameRole = Qt::UserRole + 44;
SwitcherWorkspaceDock *gWorkspaceDock = nullptr;

QEvent::Type ObsDockClosedEventType()
{
	return static_cast<QEvent::Type>(QEvent::User + QEvent::Close);
}

int NormalizeVisibleSlotCount(int slotCount)
{
	switch (slotCount) {
	case 4:
	case 9:
	case 16:
	case 25:
		return slotCount;
	default:
		return 4;
	}
}

QColor WithAlpha(QColor color, int alpha)
{
	color.setAlpha(std::clamp(alpha, 0, 255));
	return color;
}

QColor Blend(const QColor &first, const QColor &second, qreal ratio)
{
	ratio = std::clamp(ratio, 0.0, 1.0);
	const auto inverse = 1.0 - ratio;
	return QColor::fromRgbF(first.redF() * inverse + second.redF() * ratio,
				 first.greenF() * inverse + second.greenF() * ratio,
				 first.blueF() * inverse + second.blueF() * ratio,
				 first.alphaF() * inverse + second.alphaF() * ratio);
}

QString CssColor(const QColor &color)
{
	return QString("rgba(%1, %2, %3, %4)")
		.arg(color.red())
		.arg(color.green())
		.arg(color.blue())
		.arg(color.alpha());
}

QColor ContrastTextFor(const QColor &background)
{
	const double luminance = background.redF() * 0.2126 + background.greenF() * 0.7152 + background.blueF() * 0.0722;
	return luminance > 0.58 ? QColor(22, 24, 27) : QColor(255, 255, 255);
}

QString QuickOutputActiveStyle(const QString &objectName, const QPalette &palette, bool multiview)
{
	const QColor surface = palette.color(QPalette::Button);
	const QColor accent = multiview ? QColor(34, 132, 214) : QColor(35, 158, 92);
	const QColor fill = Blend(accent, surface, 0.12);
	const QColor hover = Blend(accent, surface, 0.05);
	const QColor pressed = Blend(accent.darker(118), surface, 0.06);
	const QColor border = WithAlpha(accent.lighter(128), 245);
	const QColor text = ContrastTextFor(fill);

	return QStringLiteral(
		       "QPushButton#%1 {"
		       "  background-color: %2;"
		       "  color: %3;"
		       "  border: 1px solid %4;"
		       "  font-weight: 700;"
		       "}"
		       "QPushButton#%1:hover {"
		       "  background-color: %5;"
		       "  border-color: %4;"
		       "}"
		       "QPushButton#%1:pressed {"
		       "  background-color: %6;"
		       "  border-color: %4;"
		       "}")
		.arg(objectName, CssColor(fill), CssColor(text), CssColor(border), CssColor(hover), CssColor(pressed));
}

void ApplyQuickOutputActiveStyle(QPushButton *button, bool active, bool multiview)
{
	if (!button)
		return;

	button->setProperty("switchQuickOutputActive", active);
	if (!active) {
		button->setStyleSheet(QString());
		return;
	}

	button->setStyleSheet(QuickOutputActiveStyle(button->objectName(), button->palette(), multiview));
}

obs_source_t *ResolveStoredSource(obs_data_t *data)
{
	if (!data)
		return nullptr;

	const char *sourceUuid = obs_data_get_string(data, "source_uuid");
	if (sourceUuid && strlen(sourceUuid) > 0) {
		if (auto *source = obs_get_source_by_uuid(sourceUuid))
			return source;
	}

	const char *sourceName = obs_data_get_string(data, "source_name");
	if (sourceName && strlen(sourceName) > 0)
		return obs_get_source_by_name(sourceName);

	return nullptr;
}

bool AddMotionSourceOption(void *data, obs_source_t *source)
{
	auto *combo = static_cast<QComboBox *>(data);
	if (!combo || !source)
		return true;

	const char *uuid = obs_source_get_uuid(source);
	const char *name = obs_source_get_name(source);
	if (!uuid || !*uuid || !name || !*name)
		return true;

	const uint32_t flags = obs_source_get_output_flags(source);
	if ((flags & OBS_SOURCE_VIDEO) == 0)
		return true;

	combo->addItem(QString::fromUtf8(name), QString::fromUtf8(uuid));
	return true;
}

struct MotionSceneItemCollector {
	QComboBox *combo = nullptr;
	int depth = 0;
};

struct MotionShotBindingCandidate {
	QString sceneUuid;
	QString sceneName;
	qint64 sceneItemId = -1;
	QString sourceUuid;
	QString sourceName;
};

struct MotionShotDefaultItemCollector {
	MotionShotBindingCandidate *candidate = nullptr;
};

bool AddMotionSceneItemOption(obs_scene_t *scene, obs_sceneitem_t *item, void *data)
{
	UNUSED_PARAMETER(scene);
	auto *collector = static_cast<MotionSceneItemCollector *>(data);
	if (!collector || !collector->combo || !item)
		return true;

	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source)
		return true;

	const char *sourceUuid = obs_source_get_uuid(source);
	const char *sourceName = obs_source_get_name(source);
	if (sourceUuid && *sourceUuid && sourceName && *sourceName) {
		const QString label = QStringLiteral("%1%2").arg(QString(collector->depth * 2, QLatin1Char(' ')),
								 QString::fromUtf8(sourceName));
		collector->combo->addItem(label);
		const int row = collector->combo->count() - 1;
		collector->combo->setItemData(row, static_cast<qlonglong>(obs_sceneitem_get_id(item)), kMotionShotItemIdRole);
		collector->combo->setItemData(row, QString::fromUtf8(sourceUuid), kMotionShotSourceUuidRole);
		collector->combo->setItemData(row, QString::fromUtf8(sourceName), kMotionShotSourceNameRole);
	}

	if (obs_sceneitem_is_group(item)) {
		MotionSceneItemCollector nested{collector->combo, collector->depth + 1};
		obs_scene_enum_items(obs_group_from_source(source), AddMotionSceneItemOption, &nested);
	}
	return true;
}

bool FindDefaultMotionSceneItem(obs_scene_t *scene, obs_sceneitem_t *item, void *data)
{
	UNUSED_PARAMETER(scene);
	auto *collector = static_cast<MotionShotDefaultItemCollector *>(data);
	if (!collector || !collector->candidate || !item || collector->candidate->sceneItemId >= 0)
		return false;

	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source)
		return true;

	if (obs_sceneitem_is_group(item)) {
		obs_scene_enum_items(obs_group_from_source(source), FindDefaultMotionSceneItem, collector);
		if (collector->candidate->sceneItemId >= 0)
			return false;
	}

	const uint32_t flags = obs_source_get_output_flags(source);
	if ((flags & OBS_SOURCE_VIDEO) == 0 || !obs_sceneitem_visible(item))
		return true;

	const char *sourceUuid = obs_source_get_uuid(source);
	const char *sourceName = obs_source_get_name(source);
	if (!sourceUuid || !*sourceUuid || !sourceName || !*sourceName)
		return true;

	collector->candidate->sceneItemId = static_cast<qint64>(obs_sceneitem_get_id(item));
	collector->candidate->sourceUuid = QString::fromUtf8(sourceUuid);
	collector->candidate->sourceName = QString::fromUtf8(sourceName);
	return false;
}

MotionShotBindingCandidate CurrentProgramMotionShotBinding()
{
	MotionShotBindingCandidate candidate;
	obs_source_t *sceneSource = obs_frontend_get_current_scene();
	if (!sceneSource)
		return candidate;

	const char *sceneUuid = obs_source_get_uuid(sceneSource);
	const char *sceneName = obs_source_get_name(sceneSource);
	if (sceneUuid && *sceneUuid)
		candidate.sceneUuid = QString::fromUtf8(sceneUuid);
	if (sceneName && *sceneName)
		candidate.sceneName = QString::fromUtf8(sceneName);

	if (obs_scene_t *scene = obs_scene_from_source(sceneSource)) {
		MotionShotDefaultItemCollector collector{&candidate};
		obs_scene_enum_items(scene, FindDefaultMotionSceneItem, &collector);
	}

	obs_source_release(sceneSource);
	return candidate;
}

void ApplyWorkspaceUndoState(const char *json)
{
	if (!gWorkspaceDock || !json || !strlen(json))
		return;

	gWorkspaceDock->ApplySerializedContentState(json);
}

void GetScaleAndCenterPos(int baseCX, int baseCY, int windowCX, int windowCY, int &x, int &y, float &scale)
{
	int newCX = 0;
	int newCY = 0;

	const double windowAspect = double(windowCX) / double(windowCY);
	const double baseAspect = double(baseCX) / double(baseCY);

	if (windowAspect > baseAspect) {
		scale = float(windowCY) / float(baseCY);
		newCX = int(double(windowCY) * baseAspect);
		newCY = windowCY;
	} else {
		scale = float(windowCX) / float(baseCX);
		newCX = windowCX;
		newCY = int(float(windowCX) / baseAspect);
	}

	x = windowCX / 2 - newCX / 2;
	y = windowCY / 2 - newCY / 2;
}

QDockWidget *FindParentDockWidget(QWidget *widget)
{
	for (QWidget *parent = widget ? widget->parentWidget() : nullptr; parent; parent = parent->parentWidget()) {
		if (auto *dockWidget = qobject_cast<QDockWidget *>(parent))
			return dockWidget;
	}
	return nullptr;
}

bool LayoutItemContainsWidget(QLayoutItem *item, QWidget *widget)
{
	if (!item || !widget)
		return false;
	if (item->widget() == widget)
		return true;
	if (auto *layout = item->layout()) {
		for (int index = 0; index < layout->count(); index++) {
			if (LayoutItemContainsWidget(layout->itemAt(index), widget))
				return true;
		}
	}
	return false;
}

int LayoutIndexContainingWidget(QLayout *layout, QWidget *widget)
{
	if (!layout || !widget)
		return -1;
	for (int index = 0; index < layout->count(); index++) {
		if (LayoutItemContainsWidget(layout->itemAt(index), widget))
			return index;
	}
	return -1;
}

int ClampedMonitorIndex(int monitor)
{
	const int screenCount = QGuiApplication::screens().size();
	if (monitor < 0)
		return -1;
	return screenCount > 0 ? std::min(monitor, screenCount - 1) : -1;
}

int DefaultQuickOutputMonitor()
{
	const auto screens = QGuiApplication::screens();
	if (screens.size() <= 1)
		return -1;

	QScreen *primary = QGuiApplication::primaryScreen();
	for (int index = 0; index < screens.size(); index++) {
		if (screens.at(index) != primary)
			return index;
	}
	return 0;
}

int EffectiveQuickOutputMonitor(int monitor)
{
	return monitor == -2 ? DefaultQuickOutputMonitor() : ClampedMonitorIndex(monitor);
}

QString MonitorLabel(int monitor)
{
	if (monitor == -2)
		monitor = DefaultQuickOutputMonitor();
	if (monitor < 0)
		return QStringLiteral("Windowed projector");

	const auto screens = QGuiApplication::screens();
	if (monitor >= screens.size())
		return QStringLiteral("Display %1").arg(monitor + 1);

	QScreen *screen = screens.at(monitor);
	const QRect geometry = screen ? screen->geometry() : QRect();
	const QString name = screen && !screen->name().trimmed().isEmpty() ? screen->name().trimmed()
									: QStringLiteral("Display %1").arg(monitor + 1);
	return QStringLiteral("%1 (%2x%3)").arg(name).arg(geometry.width()).arg(geometry.height());
}

QIcon SettingsButtonIcon(QWidget *widget)
{
	QIcon icon(QStringLiteral(":/settings/images/settings/general.svg"));
	if (icon.isNull())
		icon = QIcon::fromTheme(QStringLiteral("preferences-system"));
	if (icon.isNull())
		icon = QIcon::fromTheme(QStringLiteral("settings-configure"));
	if (icon.isNull() && widget)
		icon = widget->style()->standardIcon(QStyle::SP_FileDialogDetailedView);
	return icon;
}

void ConfigureFormLayout(QFormLayout *form)
{
	if (!form)
		return;
	form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
	form->setFormAlignment(Qt::AlignTop);
	form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
	form->setHorizontalSpacing(16);
	form->setVerticalSpacing(10);
}

void SetMinimumControlHeight(std::initializer_list<QWidget *> widgets, int height = 34)
{
	for (auto *widget : widgets) {
		if (widget)
			widget->setMinimumHeight(height);
	}
}

void SetMinimumControlWidth(std::initializer_list<QWidget *> widgets, int width)
{
	for (auto *widget : widgets) {
		if (widget)
			widget->setMinimumWidth(width);
	}
}

QPointer<QDockWidget> RegisterObsDockWidget(QMainWindow *mainWindow, QWidget *widget, const char *dockId,
					      const char *dockTitle)
{
	if (!mainWindow || !widget || !dockId || !dockTitle)
		return nullptr;

	if (!obs_frontend_add_dock_by_id(dockId, dockTitle, widget))
		return nullptr;

	QPointer<QDockWidget> dockContainer = FindParentDockWidget(widget);
	if (!dockContainer) {
		obs_frontend_remove_dock(dockId);
		return nullptr;
	}

	dockContainer->setFloating(false);
	return dockContainer;
}

struct CanvasSourceCollector {
	QVector<SwitchCanvasSourceDescriptor> *sources = nullptr;
	int depth = 0;
};

bool CollectCanvasSourceItems(obs_scene_t *scene, obs_sceneitem_t *item, void *data)
{
	UNUSED_PARAMETER(scene);
	auto *collector = static_cast<CanvasSourceCollector *>(data);
	if (!collector || !collector->sources)
		return true;

	obs_source_t *source = obs_sceneitem_get_source(item);
	if (!source)
		return true;

	collector->sources->push_back({int(obs_sceneitem_get_id(item)),
				       QString::fromUtf8(obs_source_get_name(source)),
				       obs_sceneitem_visible(item),
				       obs_sceneitem_locked(item),
				       collector->depth,
				       obs_sceneitem_is_group(item)});

	if (obs_sceneitem_is_group(item)) {
		CanvasSourceCollector nested{collector->sources, collector->depth + 1};
		obs_scene_enum_items(obs_group_from_source(source), CollectCanvasSourceItems, &nested);
	}

	return true;
}

void ConfigureVerticalSourceTree(QTreeWidget *tree)
{
	if (!tree)
		return;

	tree->setObjectName(QStringLiteral("switcherVerticalSourceTree"));
	tree->setColumnCount(3);
	tree->setRootIsDecorated(false);
	tree->setUniformRowHeights(true);
	tree->setAlternatingRowColors(true);
	tree->setSelectionMode(QAbstractItemView::NoSelection);
	tree->setHeaderLabels({QStringLiteral("Source"), QStringLiteral("Show"), QStringLiteral("Lock")});
	tree->header()->setStretchLastSection(false);
	tree->header()->setMinimumSectionSize(28);
	tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
	tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
}

void PopulateVerticalSourceTree(QTreeWidget *tree, const QVector<SwitchCanvasSourceDescriptor> &sources)
{
	if (!tree)
		return;

	QSignalBlocker blocker(tree);
	tree->clear();

	if (sources.isEmpty()) {
		auto *emptyItem = new QTreeWidgetItem(tree);
		emptyItem->setText(0, QStringLiteral("No sources in the active vertical scene"));
		emptyItem->setFlags(Qt::ItemIsEnabled);
		return;
	}

	for (const auto &source : sources) {
		auto *item = new QTreeWidgetItem(tree);
		const QString prefix = source.depth > 0 ? QStringLiteral("  ").repeated(source.depth) : QString();
		item->setText(0, prefix + source.name);
		item->setData(0, kSourceItemIdRole, source.id);
		item->setData(0, kSourceItemVisibleRole, source.visible);
		item->setData(0, kSourceItemLockedRole, source.locked);
		item->setData(0, kSourceItemNameRole, source.name);
		item->setCheckState(1, source.visible ? Qt::Checked : Qt::Unchecked);
		item->setCheckState(2, source.locked ? Qt::Checked : Qt::Unchecked);
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
		if (source.isGroup)
			item->setText(0, prefix + QStringLiteral("[Group] %1").arg(source.name));
	}

	if (tree->topLevelItemCount() > 0)
		tree->topLevelItem(0)->setSelected(true);
}

obs_source_t *ResolveSceneSourceByDescriptor(SwitchCanvasManager *manager, const QString &canvasId,
					     const SwitchCanvasDescriptor &descriptor)
{
	if (manager && canvasId == manager->VerticalCanvasId()) {
		const QString sceneIdOrName =
			!descriptor.activeSceneUuid.isEmpty() ? descriptor.activeSceneUuid : descriptor.activeSceneName;
		return manager->CanvasSceneSource(canvasId, sceneIdOrName);
	}

	if (!descriptor.activeSceneUuid.isEmpty()) {
		if (auto *source = obs_get_source_by_uuid(descriptor.activeSceneUuid.toUtf8().constData()))
			return source;
	}
	if (!descriptor.activeSceneName.isEmpty())
		return obs_get_source_by_name(descriptor.activeSceneName.toUtf8().constData());
	return nullptr;
}

struct EncoderOption {
	QString id;
	QString name;
};

QVector<EncoderOption> AvailableVideoEncoders()
{
	QVector<EncoderOption> encoders;
	size_t index = 0;
	const char *type = nullptr;
	while (obs_enum_encoder_types(index++, &type)) {
		if (!type || obs_get_encoder_type(type) != OBS_ENCODER_VIDEO)
			continue;
		encoders.push_back({QString::fromUtf8(type), QString::fromUtf8(obs_encoder_get_display_name(type))});
	}
	std::sort(encoders.begin(), encoders.end(), [](const EncoderOption &left, const EncoderOption &right) {
		return left.name.localeAwareCompare(right.name) < 0;
	});
	return encoders;
}

QString EncoderNameForId(const QVector<EncoderOption> &encoders, const QString &id)
{
	for (const auto &encoder : encoders) {
		if (encoder.id == id)
			return encoder.name;
	}
	return id.isEmpty() ? QStringLiteral("Match main OBS encoder") : id;
}

QString AudioTracksSummary(uint32_t trackMask)
{
	QStringList tracks;
	for (int index = 0; index < 6; index++) {
		if (trackMask & (1u << index))
			tracks.push_back(QString::number(index + 1));
	}
	return tracks.isEmpty() ? QStringLiteral("Track 1") : tracks.join(QStringLiteral(", "));
}

class SwitchVerticalOutputSettingsDialog : public QDialog {
public:
	explicit SwitchVerticalOutputSettingsDialog(SwitcherWorkspaceDock *workspace_, QWidget *parent = nullptr)
		: QDialog(parent), workspace(workspace_)
	{
		setWindowTitle(QStringLiteral("Switch Vertical Output Settings"));
		setModal(true);
		resize(680, 560);

		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(18, 18, 18, 18);
		layout->setSpacing(12);

		auto *intro = new QLabel(
			QStringLiteral("Prepare the Switch vertical output profile for encoder choice, bitrate, delay, recording "
				       "paths, replay/backtrack retention, and audio routing. These settings are saved now and "
				       "will be used by the isolated vertical output backend when it is enabled."),
			this);
		intro->setWordWrap(true);
		layout->addWidget(intro);

		tabs = new QTabWidget(this);
		layout->addWidget(tabs, 1);

		const auto encoders = AvailableVideoEncoders();

		auto *streamingPage = new QWidget(tabs);
		auto *streamingLayout = new QVBoxLayout(streamingPage);
		streamingLayout->setContentsMargins(12, 12, 12, 12);
		streamingLayout->setSpacing(12);
		followMainStreaming = new QCheckBox(QStringLiteral("Use main OBS streaming settings"), streamingPage);
		streamingLayout->addWidget(followMainStreaming);
		auto *streamingForm = new QFormLayout;
		streamingForm->setLabelAlignment(Qt::AlignLeft);
		streamEncoderCombo = new QComboBox(streamingPage);
		streamEncoderCombo->addItem(QStringLiteral("Match main OBS encoder"), QString());
		for (const auto &encoder : encoders)
			streamEncoderCombo->addItem(encoder.name, encoder.id);
		streamingBitrateSpin = new QSpinBox(streamingPage);
		streamingBitrateSpin->setRange(500, 500000);
		streamingBitrateSpin->setSuffix(QStringLiteral(" kbps"));
		streamDelayEnabled = new QCheckBox(QStringLiteral("Enable stream delay"), streamingPage);
		streamDelaySpin = new QSpinBox(streamingPage);
		streamDelaySpin->setRange(0, 600000);
		streamDelaySpin->setSuffix(QStringLiteral(" ms"));
		streamDelayPreserve = new QCheckBox(QStringLiteral("Preserve cutoff point"), streamingPage);
		streamingForm->addRow(QStringLiteral("Video encoder"), streamEncoderCombo);
		streamingForm->addRow(QStringLiteral("Bitrate / quality"), streamingBitrateSpin);
		streamingForm->addRow(QString(), streamDelayEnabled);
		streamingForm->addRow(QStringLiteral("Delay"), streamDelaySpin);
		streamingForm->addRow(QString(), streamDelayPreserve);
		streamingLayout->addLayout(streamingForm);
		streamingLayout->addStretch(1);
		tabs->addTab(streamingPage, QStringLiteral("Streaming"));

		auto *recordingPage = new QWidget(tabs);
		auto *recordingLayout = new QVBoxLayout(recordingPage);
		recordingLayout->setContentsMargins(12, 12, 12, 12);
		recordingLayout->setSpacing(12);
		followMainRecording = new QCheckBox(QStringLiteral("Use main OBS recording settings"), recordingPage);
		recordingLayout->addWidget(followMainRecording);
		auto *recordingForm = new QFormLayout;
		recordingForm->setLabelAlignment(Qt::AlignLeft);
		recordEncoderCombo = new QComboBox(recordingPage);
		recordEncoderCombo->addItem(QStringLiteral("Match main OBS encoder"), QString());
		for (const auto &encoder : encoders)
			recordEncoderCombo->addItem(encoder.name, encoder.id);
		recordingBitrateSpin = new QSpinBox(recordingPage);
		recordingBitrateSpin->setRange(1000, 500000);
		recordingBitrateSpin->setSuffix(QStringLiteral(" kbps"));
		recordingPathEdit = new QLineEdit(recordingPage);
		recordingFilenameEdit = new QLineEdit(recordingPage);
		recordingSplitEnabled = new QCheckBox(QStringLiteral("Split long vertical recordings automatically"), recordingPage);
		recordingSplitMinutesSpin = new QSpinBox(recordingPage);
		recordingSplitMinutesSpin->setRange(1, 24 * 60);
		recordingSplitMinutesSpin->setSuffix(QStringLiteral(" min"));
		auto *recordingPathWidget = new QWidget(recordingPage);
		auto *recordingPathLayout = new QHBoxLayout;
		recordingPathLayout->setContentsMargins(0, 0, 0, 0);
		recordingPathLayout->setSpacing(8);
		recordingPathLayout->addWidget(recordingPathEdit, 1);
		auto *recordingBrowseButton = new QPushButton(QStringLiteral("Browse"), recordingPage);
		recordingPathLayout->addWidget(recordingBrowseButton);
		recordingPathWidget->setLayout(recordingPathLayout);
		recordingForm->addRow(QStringLiteral("Video encoder"), recordEncoderCombo);
		recordingForm->addRow(QStringLiteral("Bitrate / quality"), recordingBitrateSpin);
		recordingForm->addRow(QStringLiteral("Recording path"), recordingPathWidget);
		recordingForm->addRow(QStringLiteral("Filename pattern"), recordingFilenameEdit);
		recordingForm->addRow(QString(), recordingSplitEnabled);
		recordingForm->addRow(QStringLiteral("Split interval"), recordingSplitMinutesSpin);
		recordingLayout->addLayout(recordingForm);
		recordingLayout->addStretch(1);
		tabs->addTab(recordingPage, QStringLiteral("Recording"));

		auto *replayPage = new QWidget(tabs);
		auto *replayLayout = new QVBoxLayout(replayPage);
		replayLayout->setContentsMargins(12, 12, 12, 12);
		replayLayout->setSpacing(12);
		followMainReplay = new QCheckBox(QStringLiteral("Use main OBS replay buffer settings"), replayPage);
		replayLayout->addWidget(followMainReplay);
		auto *replayForm = new QFormLayout;
		replayForm->setLabelAlignment(Qt::AlignLeft);
		replayAlwaysOn = new QCheckBox(QStringLiteral("Keep vertical backtrack armed"), replayPage);
		replayDurationSpin = new QSpinBox(replayPage);
		replayDurationSpin->setRange(5, 21600);
		replayDurationSpin->setSuffix(QStringLiteral(" s"));
		replayPathEdit = new QLineEdit(replayPage);
		auto *replayPathWidget = new QWidget(replayPage);
		auto *replayPathLayout = new QHBoxLayout;
		replayPathLayout->setContentsMargins(0, 0, 0, 0);
		replayPathLayout->setSpacing(8);
		replayPathLayout->addWidget(replayPathEdit, 1);
		auto *replayBrowseButton = new QPushButton(QStringLiteral("Browse"), replayPage);
		replayPathLayout->addWidget(replayBrowseButton);
		replayPathWidget->setLayout(replayPathLayout);
		replayForm->addRow(QString(), replayAlwaysOn);
		replayForm->addRow(QStringLiteral("Retention"), replayDurationSpin);
		replayForm->addRow(QStringLiteral("Replay path"), replayPathWidget);
		replayLayout->addLayout(replayForm);
		replayLayout->addStretch(1);
		tabs->addTab(replayPage, QStringLiteral("Replay"));

		auto *audioPage = new QWidget(tabs);
		auto *audioLayout = new QVBoxLayout(audioPage);
		audioLayout->setContentsMargins(12, 12, 12, 12);
		audioLayout->setSpacing(12);
		followMainVirtualCamera = new QCheckBox(QStringLiteral("Use main OBS virtual camera pipeline"), audioPage);
		audioLayout->addWidget(followMainVirtualCamera);
		auto *audioForm = new QFormLayout;
		audioForm->setLabelAlignment(Qt::AlignLeft);
		audioBitrateSpin = new QSpinBox(audioPage);
		audioBitrateSpin->setRange(64, 512);
		audioBitrateSpin->setSuffix(QStringLiteral(" kbps"));
		auto *tracksWidget = new QWidget(audioPage);
		auto *tracksLayout = new QHBoxLayout(tracksWidget);
		tracksLayout->setContentsMargins(0, 0, 0, 0);
		tracksLayout->setSpacing(8);
		for (int index = 0; index < int(audioTrackChecks.size()); index++) {
			audioTrackChecks[index] = new QCheckBox(QString::number(index + 1), tracksWidget);
			tracksLayout->addWidget(audioTrackChecks[index]);
		}
		tracksLayout->addStretch(1);
		audioForm->addRow(QStringLiteral("Audio bitrate"), audioBitrateSpin);
		audioForm->addRow(QStringLiteral("Audio tracks"), tracksWidget);
		audioLayout->addLayout(audioForm);
		audioLayout->addStretch(1);
		tabs->addTab(audioPage, QStringLiteral("Audio"));

		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
		layout->addWidget(buttons);

		connect(recordingBrowseButton, &QPushButton::clicked, this, [this]() { ChooseDirectory(recordingPathEdit); });
		connect(replayBrowseButton, &QPushButton::clicked, this, [this]() { ChooseDirectory(replayPathEdit); });
		connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
		connect(followMainStreaming, &QCheckBox::checkStateChanged, this, [this](Qt::CheckState) { RefreshEnabledState(); });
		connect(followMainRecording, &QCheckBox::checkStateChanged, this, [this](Qt::CheckState) { RefreshEnabledState(); });
		connect(followMainReplay, &QCheckBox::checkStateChanged, this, [this](Qt::CheckState) { RefreshEnabledState(); });
		connect(recordingSplitEnabled, &QCheckBox::checkStateChanged, this, [this](Qt::CheckState) { RefreshEnabledState(); });

		Load();
		RefreshEnabledState();
	}

	SwitchCanvasOutputSettings Settings() const
	{
		SwitchCanvasOutputSettings settings;
		settings.followMainStreaming = followMainStreaming->isChecked();
		settings.followMainRecording = followMainRecording->isChecked();
		settings.followMainReplay = followMainReplay->isChecked();
		settings.followMainVirtualCamera = followMainVirtualCamera->isChecked();
		settings.streamEncoderId = streamEncoderCombo->currentData().toString();
		settings.recordEncoderId = recordEncoderCombo->currentData().toString();
		settings.streamingVideoBitrateKbps = streamingBitrateSpin->value();
		settings.recordingVideoBitrateKbps = recordingBitrateSpin->value();
		settings.audioBitrateKbps = audioBitrateSpin->value();
		settings.streamDelayEnabled = streamDelayEnabled->isChecked();
		settings.streamDelayMs = streamDelaySpin->value();
		settings.streamDelayPreserve = streamDelayPreserve->isChecked();
		settings.recordingPath = recordingPathEdit->text().trimmed();
		settings.recordingFilenamePattern = recordingFilenameEdit->text().trimmed();
		settings.recordingSplitEnabled = recordingSplitEnabled->isChecked();
		settings.recordingSplitMinutes = recordingSplitMinutesSpin->value();
		settings.replayPath = replayPathEdit->text().trimmed();
		settings.replayDurationSeconds = replayDurationSpin->value();
		settings.replayAlwaysOn = replayAlwaysOn->isChecked();
		settings.audioTrackMask = 0;
		for (int index = 0; index < int(audioTrackChecks.size()); index++) {
			if (audioTrackChecks[index] && audioTrackChecks[index]->isChecked())
				settings.audioTrackMask |= (1u << index);
		}
		if (!settings.audioTrackMask)
			settings.audioTrackMask = 0x1;
		return settings;
	}

private:
	void Load()
	{
		if (!workspace || !workspace->CanvasManager())
			return;
		const auto settings = workspace->CanvasManager()->OutputSettings(workspace->VerticalCanvasId());
		const auto selectById = [](QComboBox *combo, const QString &id) {
			const int found = combo->findData(id);
			combo->setCurrentIndex(found >= 0 ? found : 0);
		};
		followMainStreaming->setChecked(settings.followMainStreaming);
		followMainRecording->setChecked(settings.followMainRecording);
		followMainReplay->setChecked(settings.followMainReplay);
		followMainVirtualCamera->setChecked(settings.followMainVirtualCamera);
		selectById(streamEncoderCombo, settings.streamEncoderId);
		selectById(recordEncoderCombo, settings.recordEncoderId);
		streamingBitrateSpin->setValue(settings.streamingVideoBitrateKbps);
		recordingBitrateSpin->setValue(settings.recordingVideoBitrateKbps);
		audioBitrateSpin->setValue(settings.audioBitrateKbps);
		streamDelayEnabled->setChecked(settings.streamDelayEnabled);
		streamDelaySpin->setValue(settings.streamDelayMs);
		streamDelayPreserve->setChecked(settings.streamDelayPreserve);
		recordingPathEdit->setText(settings.recordingPath);
		recordingFilenameEdit->setText(settings.recordingFilenamePattern);
		recordingSplitEnabled->setChecked(settings.recordingSplitEnabled);
		recordingSplitMinutesSpin->setValue(settings.recordingSplitMinutes);
		replayPathEdit->setText(settings.replayPath);
		replayDurationSpin->setValue(settings.replayDurationSeconds);
		replayAlwaysOn->setChecked(settings.replayAlwaysOn);
		for (int index = 0; index < int(audioTrackChecks.size()); index++) {
			if (audioTrackChecks[index])
				audioTrackChecks[index]->setChecked((settings.audioTrackMask & (1u << index)) != 0);
		}
	}

	void RefreshEnabledState()
	{
		streamEncoderCombo->setEnabled(!followMainStreaming->isChecked());
		streamingBitrateSpin->setEnabled(!followMainStreaming->isChecked());
		streamDelayEnabled->setEnabled(!followMainStreaming->isChecked());
		streamDelaySpin->setEnabled(!followMainStreaming->isChecked() && streamDelayEnabled->isChecked());
		streamDelayPreserve->setEnabled(!followMainStreaming->isChecked() && streamDelayEnabled->isChecked());
		recordEncoderCombo->setEnabled(!followMainRecording->isChecked());
		recordingBitrateSpin->setEnabled(!followMainRecording->isChecked());
		recordingPathEdit->setEnabled(!followMainRecording->isChecked());
		recordingFilenameEdit->setEnabled(!followMainRecording->isChecked());
		recordingSplitEnabled->setEnabled(!followMainRecording->isChecked());
		recordingSplitMinutesSpin->setEnabled(!followMainRecording->isChecked() && recordingSplitEnabled->isChecked());
		replayAlwaysOn->setEnabled(!followMainReplay->isChecked());
		replayDurationSpin->setEnabled(!followMainReplay->isChecked());
		replayPathEdit->setEnabled(!followMainReplay->isChecked());
	}

	void ChooseDirectory(QLineEdit *edit)
	{
		const QString start = edit->text().trimmed().isEmpty() ? QDir::homePath() : edit->text().trimmed();
		const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Choose Folder"), start);
		if (!dir.isEmpty())
			edit->setText(dir);
	}

	SwitcherWorkspaceDock *workspace = nullptr;
	QTabWidget *tabs = nullptr;
	QCheckBox *followMainStreaming = nullptr;
	QCheckBox *followMainRecording = nullptr;
	QCheckBox *followMainReplay = nullptr;
	QCheckBox *followMainVirtualCamera = nullptr;
	QComboBox *streamEncoderCombo = nullptr;
	QComboBox *recordEncoderCombo = nullptr;
	QSpinBox *streamingBitrateSpin = nullptr;
	QSpinBox *recordingBitrateSpin = nullptr;
	QSpinBox *audioBitrateSpin = nullptr;
	QCheckBox *streamDelayEnabled = nullptr;
	QSpinBox *streamDelaySpin = nullptr;
	QCheckBox *streamDelayPreserve = nullptr;
	QLineEdit *recordingPathEdit = nullptr;
	QLineEdit *recordingFilenameEdit = nullptr;
	QCheckBox *recordingSplitEnabled = nullptr;
	QSpinBox *recordingSplitMinutesSpin = nullptr;
	QLineEdit *replayPathEdit = nullptr;
	QSpinBox *replayDurationSpin = nullptr;
	QCheckBox *replayAlwaysOn = nullptr;
	std::array<QCheckBox *, 6> audioTrackChecks{};
};
} // namespace

class SwitchVerticalDockWidget : public QWidget {
public:
	explicit SwitchVerticalDockWidget(SwitcherWorkspaceDock *workspace_, QWidget *parent = nullptr)
		: QWidget(parent),
		  workspace(workspace_),
		  preview(new SwitchCanvasPreview(workspace_ ? workspace_->CanvasManager() : nullptr, this)),
		  statusLabel(new QLabel(this)),
		  menuButton(new QToolButton(this)),
		  controlsMenu(new QMenu(this)),
		  recordAction(controlsMenu->addAction(QString())),
		  pauseRecordAction(controlsMenu->addAction(QString())),
		  splitRecordAction(controlsMenu->addAction(QStringLiteral("Split Recording"))),
		  chapterAction(controlsMenu->addAction(QStringLiteral("Add Chapter Marker"))),
		  streamAction(controlsMenu->addAction(QString())),
		  replayAction(controlsMenu->addAction(QString())),
		  saveReplayAction(controlsMenu->addAction(QStringLiteral("Save Replay"))),
		  virtualCamAction(controlsMenu->addAction(QString())),
		  openSettingsAction(controlsMenu->addAction(QStringLiteral("Vertical Settings"))),
		  openProjectorAction(controlsMenu->addAction(QStringLiteral("Open Projector"))),
		  openSwitchAction(controlsMenu->addAction(QStringLiteral("Open Switch")))
	{
		setObjectName(QStringLiteral("switcherVerticalSurface"));
		setMinimumSize(260, 420);

		controlsMenu->insertSeparator(streamAction);
		controlsMenu->insertSeparator(openProjectorAction);

		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(12, 12, 12, 12);
		layout->setSpacing(10);

		auto *previewCard = new QFrame(this);
		previewCard->setObjectName(QStringLiteral("switcherVerticalPreviewCard"));
		auto *previewLayout = new QVBoxLayout(previewCard);
		previewLayout->setContentsMargins(12, 12, 12, 12);
		previewLayout->setSpacing(10);

		statusLabel->setObjectName(QStringLiteral("switcherVerticalPreviewStatus"));
		statusLabel->setWordWrap(true);
		preview->SetCanvasId(workspace ? workspace->VerticalCanvasId() : QString());
		preview->setMinimumSize(QSize(220, 360));
		preview->setObjectName(QStringLiteral("switcherVerticalCanvasPreview"));
		preview->SetRenderingEnabled(false);
		preview->hide();
		menuButton->setObjectName(QStringLiteral("switcherVerticalDockMenuButton"));
		menuButton->setText(QStringLiteral("Controls"));
		menuButton->setPopupMode(QToolButton::InstantPopup);
		menuButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
		menuButton->setMenu(controlsMenu);
		menuButton->setMinimumHeight(32);

		if (kPreviewEnabled)
			previewLayout->addWidget(preview, 1);
		else
			previewLayout->addStretch(1);
		auto *footerLayout = new QHBoxLayout;
		footerLayout->setContentsMargins(0, 0, 0, 0);
		footerLayout->setSpacing(8);
		footerLayout->addWidget(statusLabel, 1);
		footerLayout->addWidget(menuButton, 0, Qt::AlignRight);
		previewLayout->addLayout(footerLayout);
		layout->addWidget(previewCard, 1);

		connect(recordAction, &QAction::triggered, this, [this]() {
			if (workspace)
				workspace->PerformOutputAction(QStringLiteral("toggle_recording"));
		});
		connect(pauseRecordAction, &QAction::triggered, this, [this]() {
			if (workspace)
				workspace->PerformOutputAction(QStringLiteral("toggle_recording_pause"));
		});
		connect(splitRecordAction, &QAction::triggered, this, [this]() {
			if (workspace)
				workspace->PerformOutputAction(QStringLiteral("split_recording"));
		});
		connect(chapterAction, &QAction::triggered, this, [this]() {
			if (workspace)
				workspace->PerformOutputAction(QStringLiteral("add_recording_chapter"));
		});
		connect(streamAction, &QAction::triggered, this, [this]() {
			if (workspace)
				workspace->PerformOutputAction(QStringLiteral("toggle_streaming"));
		});
		connect(replayAction, &QAction::triggered, this, [this]() {
			if (workspace)
				workspace->PerformOutputAction(QStringLiteral("toggle_replay"));
		});
		connect(saveReplayAction, &QAction::triggered, this, [this]() {
			if (workspace)
				workspace->PerformOutputAction(QStringLiteral("save_replay"));
		});
		connect(virtualCamAction, &QAction::triggered, this, [this]() {
			if (workspace)
				workspace->PerformOutputAction(QStringLiteral("toggle_virtual_camera"));
		});
		connect(openSettingsAction, &QAction::triggered, this, [this]() {
			if (workspace)
				QMetaObject::invokeMethod(workspace, "OpenVerticalOutputSettings", Qt::QueuedConnection);
		});
		connect(openProjectorAction, &QAction::triggered, this, [this]() {
			if (workspace)
				workspace->OpenCanvasProjector(workspace->VerticalCanvasId());
		});
		connect(openSwitchAction, &QAction::triggered, this, [this]() {
			if (workspace)
				workspace->OpenWorkspaceWindow();
		});
	}

	void Refresh(bool refreshPreview = true)
	{
		if (!workspace)
			return;

		const auto descriptor = workspace->CanvasDescriptorForId(workspace->VerticalCanvasId());
		const QString sceneName =
			descriptor.activeSceneName.isEmpty() ? QStringLiteral("No vertical scene selected") : descriptor.activeSceneName;
		QStringList liveStates;
		if (obs_frontend_streaming_active())
			liveStates.push_back(QStringLiteral("Live"));
		if (obs_frontend_recording_active())
			liveStates.push_back(obs_frontend_recording_paused() ? QStringLiteral("Paused") : QStringLiteral("Recording"));
		if (obs_frontend_replay_buffer_active())
			liveStates.push_back(QStringLiteral("Replay"));
		if (obs_frontend_virtualcam_active())
			liveStates.push_back(QStringLiteral("Virtual Cam"));
		if (liveStates.isEmpty())
			liveStates.push_back(QStringLiteral("Idle"));

		statusLabel->setText(QStringLiteral("%1 | %2").arg(sceneName, liveStates.join(QStringLiteral(" | "))));
		recordAction->setText(obs_frontend_recording_active() ? QStringLiteral("Stop Recording")
							      : QStringLiteral("Start Recording"));
		pauseRecordAction->setText(obs_frontend_recording_paused() ? QStringLiteral("Resume Recording")
								   : QStringLiteral("Pause Recording"));
		streamAction->setText(obs_frontend_streaming_active() ? QStringLiteral("Stop Streaming")
							      : QStringLiteral("Start Streaming"));
		replayAction->setText(obs_frontend_replay_buffer_active() ? QStringLiteral("Stop Replay Buffer")
							      : QStringLiteral("Start Replay Buffer"));
		virtualCamAction->setText(obs_frontend_virtualcam_active() ? QStringLiteral("Stop Virtual Camera")
								   : QStringLiteral("Start Virtual Camera"));
		pauseRecordAction->setEnabled(obs_frontend_recording_active());
		splitRecordAction->setEnabled(obs_frontend_recording_active() && !obs_frontend_recording_paused());
		chapterAction->setEnabled(obs_frontend_recording_active() && !obs_frontend_recording_paused());
		saveReplayAction->setEnabled(obs_frontend_replay_buffer_active());
		openProjectorAction->setEnabled(!descriptor.activeSceneName.isEmpty());
		if (kPreviewEnabled && refreshPreview)
			preview->Refresh();
	}

	void SetPreviewRenderingEnabled(bool enabled)
	{
		if (preview)
			preview->SetRenderingEnabled(kPreviewEnabled && enabled);
	}

protected:
	bool event(QEvent *event) override
	{
		if (event && event->type() == ObsDockClosedEventType() && preview)
			preview->SetRenderingEnabled(false);
		return QWidget::event(event);
	}

	void showEvent(QShowEvent *event) override
	{
		QWidget::showEvent(event);
		if (preview)
			preview->SetRenderingEnabled(kPreviewEnabled);
		Refresh(kPreviewEnabled);
	}

	void hideEvent(QHideEvent *event) override
	{
		if (preview)
			preview->SetRenderingEnabled(false);
		QWidget::hideEvent(event);
	}

private:
	static constexpr bool kPreviewEnabled = false;

	SwitcherWorkspaceDock *workspace = nullptr;
	SwitchCanvasPreview *preview = nullptr;
	QLabel *statusLabel = nullptr;
	QToolButton *menuButton = nullptr;
	QMenu *controlsMenu = nullptr;
	QAction *recordAction = nullptr;
	QAction *pauseRecordAction = nullptr;
	QAction *splitRecordAction = nullptr;
	QAction *chapterAction = nullptr;
	QAction *streamAction = nullptr;
	QAction *replayAction = nullptr;
	QAction *saveReplayAction = nullptr;
	QAction *virtualCamAction = nullptr;
	QAction *openSettingsAction = nullptr;
	QAction *openProjectorAction = nullptr;
	QAction *openSwitchAction = nullptr;
};

class SwitchVerticalScenesDockWidget : public QWidget {
public:
	explicit SwitchVerticalScenesDockWidget(SwitcherWorkspaceDock *workspace_, QWidget *parent = nullptr)
		: QWidget(parent),
		  workspace(workspace_),
		  sceneList(new QListWidget(this)),
		  linkList(new QListWidget(this)),
		  addButton(new QPushButton(QStringLiteral("Add"), this)),
		  duplicateButton(new QPushButton(QStringLiteral("Duplicate"), this)),
		  renameButton(new QPushButton(QStringLiteral("Rename"), this)),
		  removeButton(new QPushButton(QStringLiteral("Remove"), this)),
		  linkButton(new QPushButton(QStringLiteral("Link Current Program"), this)),
		  clearLinkButton(new QPushButton(QStringLiteral("Clear Link"), this)),
		  openWindowButton(new QPushButton(QStringLiteral("Open Window"), this)),
		  openProjectorButton(new QPushButton(QStringLiteral("Open Projector"), this))
	{
		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(12, 12, 12, 12);
		layout->setSpacing(10);

		sceneList->setAlternatingRowColors(true);
		linkList->setAlternatingRowColors(true);
		layout->addWidget(sceneList, 1);

		auto *buttonRow = new QGridLayout;
		buttonRow->setContentsMargins(0, 0, 0, 0);
		buttonRow->setHorizontalSpacing(8);
		buttonRow->setVerticalSpacing(8);
		buttonRow->addWidget(addButton, 0, 0);
		buttonRow->addWidget(duplicateButton, 0, 1);
		buttonRow->addWidget(renameButton, 1, 0);
		buttonRow->addWidget(removeButton, 1, 1);
		buttonRow->addWidget(openWindowButton, 2, 0);
		buttonRow->addWidget(openProjectorButton, 2, 1);
		layout->addLayout(buttonRow);

		auto *linksGroup = new QGroupBox(QStringLiteral("Linked Scenes"), this);
		auto *linksLayout = new QVBoxLayout(linksGroup);
		linksLayout->setContentsMargins(12, 12, 12, 12);
		linksLayout->setSpacing(8);
		linksLayout->addWidget(linkButton);
		linksLayout->addWidget(linkList, 1);
		linksLayout->addWidget(clearLinkButton);
		layout->addWidget(linksGroup, 0);

		connect(sceneList, &QListWidget::currentRowChanged, this, [this]() {
			if (refreshing || !workspace)
				return;
			const QString sceneId = SelectedSceneId();
			if (!sceneId.isEmpty())
				workspace->SwitchCanvasScene(workspace->VerticalCanvasId(), sceneId);
			RefreshButtons();
		});
		connect(sceneList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) {
			if (workspace)
				workspace->OpenCanvasProjector(workspace->VerticalCanvasId());
		});
		connect(linkList, &QListWidget::currentRowChanged, this, [this]() { RefreshButtons(); });
		connect(addButton, &QPushButton::clicked, this, [this]() {
			if (!workspace)
				return;
			bool ok = false;
			const QString name = QInputDialog::getText(this, QStringLiteral("Add Vertical Scene"),
								  QStringLiteral("Scene Name"), QLineEdit::Normal,
								  QStringLiteral("Vertical Scene"), &ok);
			if (ok)
				workspace->CreateCanvasScene(workspace->VerticalCanvasId(), name.trimmed());
		});
		connect(duplicateButton, &QPushButton::clicked, this, [this]() {
			if (!workspace)
				return;
			const QString sceneId = SelectedSceneId();
			if (sceneId.isEmpty())
				return;
			bool ok = false;
			const QString baseName =
				QInputDialog::getText(this, QStringLiteral("Duplicate Vertical Scene"),
						      QStringLiteral("Duplicate Name"), QLineEdit::Normal,
						      QStringLiteral("Vertical Scene"), &ok);
			if (ok)
				workspace->DuplicateCanvasScene(workspace->VerticalCanvasId(), sceneId, baseName.trimmed());
		});
		connect(renameButton, &QPushButton::clicked, this, [this]() {
			if (!workspace)
				return;
			const QString sceneId = SelectedSceneId();
			if (sceneId.isEmpty())
				return;
			bool ok = false;
			const QString name = QInputDialog::getText(this, QStringLiteral("Rename Vertical Scene"),
								  QStringLiteral("Scene Name"), QLineEdit::Normal,
								  SelectedSceneName(), &ok);
			if (ok)
				workspace->RenameCanvasScene(workspace->VerticalCanvasId(), sceneId, name.trimmed());
		});
		connect(removeButton, &QPushButton::clicked, this, [this]() {
			if (workspace) {
				const QString sceneId = SelectedSceneId();
				if (!sceneId.isEmpty())
					workspace->DeleteCanvasScene(workspace->VerticalCanvasId(), sceneId);
			}
		});
		connect(linkButton, &QPushButton::clicked, this, [this]() {
			if (!workspace || !workspace->CanvasManager())
				return;
			OBSSourceAutoRelease currentScene = obs_frontend_get_current_scene();
			if (!currentScene)
				return;
			const QString sceneId = SelectedSceneId();
			const QString sceneName = SelectedSceneName();
			if (!sceneId.isEmpty())
				workspace->CanvasManager()->SetLinkedScene(QString::fromUtf8(obs_source_get_uuid(currentScene)),
									 QString::fromUtf8(obs_source_get_name(currentScene)),
									 sceneId, sceneName);
		});
		connect(clearLinkButton, &QPushButton::clicked, this, [this]() {
			if (!workspace || !workspace->CanvasManager() || !linkList->currentItem())
				return;
			const QString mainSceneUuid = linkList->currentItem()->data(Qt::UserRole).toString();
			if (!mainSceneUuid.isEmpty())
				workspace->CanvasManager()->ClearLinkedScene(mainSceneUuid);
		});
		connect(openWindowButton, &QPushButton::clicked, this, [this]() {
			if (workspace)
				workspace->OpenCanvasWindow(workspace->VerticalCanvasId());
		});
		connect(openProjectorButton, &QPushButton::clicked, this, [this]() {
			if (workspace)
				workspace->OpenCanvasProjector(workspace->VerticalCanvasId());
		});

		Refresh();
	}

	void Refresh()
	{
		if (!workspace)
			return;

		QScopedValueRollback<bool> guard(refreshing, true);
		const auto canvasId = workspace->VerticalCanvasId();
		const auto descriptor = workspace->CanvasDescriptorForId(canvasId);
		const auto scenes = workspace->CanvasScenes(canvasId);
		const auto links = workspace->CanvasManager() ? workspace->CanvasManager()->Links() : QVector<SwitchCanvasLink>{};

		QSignalBlocker sceneBlocker(sceneList);
		sceneList->clear();
		for (const auto &scene : scenes) {
			auto *item = new QListWidgetItem(scene.name, sceneList);
			item->setData(Qt::UserRole, scene.uuid);
			if (scene.uuid == descriptor.activeSceneUuid ||
			    (descriptor.activeSceneUuid.isEmpty() && scene.name == descriptor.activeSceneName))
				sceneList->setCurrentItem(item);
		}
		if (!descriptor.activeSceneName.isEmpty() && !sceneList->currentItem()) {
			for (int index = 0; index < sceneList->count(); index++) {
				auto *item = sceneList->item(index);
				if (item && item->text() == descriptor.activeSceneName) {
					sceneList->setCurrentItem(item);
					break;
				}
			}
		}
		if (!sceneList->currentItem() && sceneList->count() > 0)
			sceneList->setCurrentRow(0);

		QSignalBlocker linkBlocker(linkList);
		linkList->clear();
		for (const auto &link : links) {
			auto *item =
				new QListWidgetItem(QStringLiteral("%1 -> %2").arg(link.mainSceneName, link.targetSceneName), linkList);
			item->setData(Qt::UserRole, link.mainSceneUuid);
			item->setToolTip(item->text());
		}

		RefreshButtons();
	}

private:
	QString SelectedSceneId() const
	{
		if (auto *item = sceneList->currentItem()) {
			const QString sceneId = item->data(Qt::UserRole).toString();
			return sceneId.isEmpty() ? item->text() : sceneId;
		}
		return {};
	}

	QString SelectedSceneName() const
	{
		return sceneList->currentItem() ? sceneList->currentItem()->text() : QString();
	}

	void RefreshButtons()
	{
		const bool hasScene = sceneList->currentItem() != nullptr;
		duplicateButton->setEnabled(hasScene);
		renameButton->setEnabled(hasScene);
		removeButton->setEnabled(hasScene);
		linkButton->setEnabled(hasScene);
		openProjectorButton->setEnabled(hasScene);
		clearLinkButton->setEnabled(linkList->currentItem() != nullptr);
	}

	SwitcherWorkspaceDock *workspace = nullptr;
	QListWidget *sceneList = nullptr;
	QListWidget *linkList = nullptr;
	QPushButton *addButton = nullptr;
	QPushButton *duplicateButton = nullptr;
	QPushButton *renameButton = nullptr;
	QPushButton *removeButton = nullptr;
	QPushButton *linkButton = nullptr;
	QPushButton *clearLinkButton = nullptr;
	QPushButton *openWindowButton = nullptr;
	QPushButton *openProjectorButton = nullptr;
	bool refreshing = false;
};

class SwitchVerticalSourcesDockWidget : public QWidget {
public:
	explicit SwitchVerticalSourcesDockWidget(SwitcherWorkspaceDock *workspace_, QWidget *parent = nullptr)
		: QWidget(parent),
		  workspace(workspace_),
		  sourceTree(new QTreeWidget(this)),
		  propertiesButton(new QPushButton(QStringLiteral("Properties"), this)),
		  filtersButton(new QPushButton(QStringLiteral("Filters"), this)),
		  renameButton(new QPushButton(QStringLiteral("Rename"), this)),
		  removeButton(new QPushButton(QStringLiteral("Remove"), this)),
		  moveUpButton(new QPushButton(QStringLiteral("Move Up"), this)),
		  moveDownButton(new QPushButton(QStringLiteral("Move Down"), this))
	{
		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(12, 12, 12, 12);
		layout->setSpacing(10);

		ConfigureVerticalSourceTree(sourceTree);
		sourceTree->setSelectionMode(QAbstractItemView::SingleSelection);
		layout->addWidget(sourceTree, 1);

		auto *buttonGrid = new QGridLayout;
		buttonGrid->setContentsMargins(0, 0, 0, 0);
		buttonGrid->setHorizontalSpacing(8);
		buttonGrid->setVerticalSpacing(8);
		buttonGrid->addWidget(propertiesButton, 0, 0);
		buttonGrid->addWidget(filtersButton, 0, 1);
		buttonGrid->addWidget(renameButton, 1, 0);
		buttonGrid->addWidget(removeButton, 1, 1);
		buttonGrid->addWidget(moveUpButton, 2, 0);
		buttonGrid->addWidget(moveDownButton, 2, 1);
		layout->addLayout(buttonGrid);

		connect(sourceTree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item, int column) {
			if (refreshing || !workspace || !item)
				return;
			const int itemId = item->data(0, kSourceItemIdRole).toInt();
			if (itemId <= 0)
				return;
			if (column == 1)
				workspace->SetCanvasSourceVisible(workspace->VerticalCanvasId(), itemId,
								  item->checkState(1) == Qt::Checked);
			if (column == 2)
				workspace->SetCanvasSourceLocked(workspace->VerticalCanvasId(), itemId,
								 item->checkState(2) == Qt::Checked);
		});
		connect(sourceTree, &QTreeWidget::itemSelectionChanged, this, [this]() { RefreshButtons(); });
		connect(sourceTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
			if (!workspace || !item)
				return;
			workspace->OpenCanvasSourceProperties(workspace->VerticalCanvasId(),
							      item->data(0, kSourceItemIdRole).toInt());
		});
		connect(propertiesButton, &QPushButton::clicked, this, [this]() {
			if (workspace)
				workspace->OpenCanvasSourceProperties(workspace->VerticalCanvasId(), SelectedItemId());
		});
		connect(filtersButton, &QPushButton::clicked, this, [this]() {
			if (workspace)
				workspace->OpenCanvasSourceFilters(workspace->VerticalCanvasId(), SelectedItemId());
		});
		connect(renameButton, &QPushButton::clicked, this, [this]() {
			if (!workspace)
				return;
			const int itemId = SelectedItemId();
			if (itemId <= 0)
				return;
			bool ok = false;
			const QString name = QInputDialog::getText(this, QStringLiteral("Rename Vertical Source"),
								  QStringLiteral("Source Name"), QLineEdit::Normal,
								  SelectedItemName(), &ok);
			if (ok)
				workspace->RenameCanvasSource(workspace->VerticalCanvasId(), itemId, name.trimmed());
		});
		connect(removeButton, &QPushButton::clicked, this, [this]() {
			if (workspace)
				workspace->DeleteCanvasSource(workspace->VerticalCanvasId(), SelectedItemId());
		});
		connect(moveUpButton, &QPushButton::clicked, this, [this]() {
			if (workspace)
				workspace->MoveCanvasSource(workspace->VerticalCanvasId(), SelectedItemId(), OBS_ORDER_MOVE_UP);
		});
		connect(moveDownButton, &QPushButton::clicked, this, [this]() {
			if (workspace)
				workspace->MoveCanvasSource(workspace->VerticalCanvasId(), SelectedItemId(), OBS_ORDER_MOVE_DOWN);
		});

		Refresh();
	}

	void Refresh()
	{
		if (!workspace)
			return;

		QScopedValueRollback<bool> guard(refreshing, true);
		PopulateVerticalSourceTree(sourceTree, workspace->CanvasSources(workspace->VerticalCanvasId()));
		RefreshButtons();
	}

private:
	int SelectedItemId() const
	{
		if (auto *item = sourceTree->currentItem())
			return item->data(0, kSourceItemIdRole).toInt();
		return 0;
	}

	QString SelectedItemName() const
	{
		if (auto *item = sourceTree->currentItem())
			return item->data(0, kSourceItemNameRole).toString();
		return {};
	}

	void RefreshButtons()
	{
		const bool hasItem = SelectedItemId() > 0;
		propertiesButton->setEnabled(hasItem);
		filtersButton->setEnabled(hasItem);
		renameButton->setEnabled(hasItem);
		removeButton->setEnabled(hasItem);
		moveUpButton->setEnabled(hasItem);
		moveDownButton->setEnabled(hasItem);
	}

	SwitcherWorkspaceDock *workspace = nullptr;
	QTreeWidget *sourceTree = nullptr;
	QPushButton *propertiesButton = nullptr;
	QPushButton *filtersButton = nullptr;
	QPushButton *renameButton = nullptr;
	QPushButton *removeButton = nullptr;
	QPushButton *moveUpButton = nullptr;
	QPushButton *moveDownButton = nullptr;
	bool refreshing = false;
};

class SwitchVerticalTransitionsDockWidget : public QWidget {
public:
	explicit SwitchVerticalTransitionsDockWidget(SwitcherWorkspaceDock *workspace_, QWidget *parent = nullptr)
		: QWidget(parent),
		  workspace(workspace_),
		  defaultTransitionCombo(new QComboBox(this)),
		  defaultDurationSpin(new QSpinBox(this)),
		  sceneCombo(new QComboBox(this)),
		  overrideTransitionCombo(new QComboBox(this)),
		  overrideDurationSpin(new QSpinBox(this)),
		  applyButton(new QPushButton(QStringLiteral("Apply Override"), this)),
		  clearButton(new QPushButton(QStringLiteral("Use Default"), this))
	{
		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(12, 12, 12, 12);
		layout->setSpacing(12);

		defaultDurationSpin->setRange(50, 20000);
		defaultDurationSpin->setSingleStep(50);
		defaultDurationSpin->setSuffix(QStringLiteral(" ms"));
		overrideDurationSpin->setRange(0, 20000);
		overrideDurationSpin->setSingleStep(50);
		overrideDurationSpin->setSuffix(QStringLiteral(" ms"));

		auto *defaultForm = new QFormLayout;
		defaultForm->setLabelAlignment(Qt::AlignLeft);
		defaultForm->addRow(QStringLiteral("Default Transition"), defaultTransitionCombo);
		defaultForm->addRow(QStringLiteral("Default Duration"), defaultDurationSpin);
		layout->addLayout(defaultForm);

		auto *overrideGroup = new QGroupBox(QStringLiteral("Scene Override"), this);
		auto *overrideLayout = new QFormLayout(overrideGroup);
		overrideLayout->setLabelAlignment(Qt::AlignLeft);
		overrideLayout->addRow(QStringLiteral("Scene"), sceneCombo);
		overrideLayout->addRow(QStringLiteral("Transition"), overrideTransitionCombo);
		overrideLayout->addRow(QStringLiteral("Duration"), overrideDurationSpin);
		auto *buttonWidget = new QWidget(overrideGroup);
		auto *buttonRow = new QHBoxLayout;
		buttonRow->setContentsMargins(0, 0, 0, 0);
		buttonRow->setSpacing(8);
		buttonRow->addWidget(applyButton);
		buttonRow->addWidget(clearButton);
		buttonWidget->setLayout(buttonRow);
		overrideLayout->addRow(QString(), buttonWidget);
		layout->addWidget(overrideGroup);
		layout->addStretch(1);

		connect(defaultTransitionCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() {
			if (!refreshing && workspace && workspace->CanvasManager())
				workspace->CanvasManager()->SetDefaultTransition(defaultTransitionCombo->currentData().toString());
		});
		connect(defaultDurationSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
			if (!refreshing && workspace && workspace->CanvasManager())
				workspace->CanvasManager()->SetDefaultTransitionDuration(value);
		});
		connect(sceneCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]() { LoadSelectedSceneOverride(); });
		connect(applyButton, &QPushButton::clicked, this, [this]() {
			if (!workspace || !workspace->CanvasManager())
				return;
			const QString sceneId = sceneCombo->currentData().toString();
			if (!sceneId.isEmpty())
				workspace->CanvasManager()->SetSceneTransition(sceneId, overrideTransitionCombo->currentData().toString(),
									      overrideDurationSpin->value());
		});
		connect(clearButton, &QPushButton::clicked, this, [this]() {
			if (!workspace || !workspace->CanvasManager())
				return;
			const QString sceneId = sceneCombo->currentData().toString();
			if (!sceneId.isEmpty())
				workspace->CanvasManager()->SetSceneTransition(sceneId, QString(), 0);
		});

		Refresh();
	}

	void Refresh()
	{
		if (!workspace || !workspace->CanvasManager())
			return;

		QScopedValueRollback<bool> guard(refreshing, true);
		auto *manager = workspace->CanvasManager();
		const auto transitions = manager->Transitions();
		const auto scenes = workspace->CanvasScenes(workspace->VerticalCanvasId());

		{
			QSignalBlocker blocker(defaultTransitionCombo);
			defaultTransitionCombo->clear();
			overrideTransitionCombo->clear();
			for (const auto &transition : transitions) {
				defaultTransitionCombo->addItem(transition.name, transition.name);
				overrideTransitionCombo->addItem(transition.name, transition.name);
			}
			defaultTransitionCombo->setCurrentIndex(
				std::max(0, defaultTransitionCombo->findData(manager->DefaultTransitionName())));
		}
		defaultDurationSpin->setValue(manager->DefaultTransitionDuration());

		{
			QSignalBlocker blocker(sceneCombo);
			const QString currentSceneId = sceneCombo->currentData().toString();
			sceneCombo->clear();
			for (const auto &scene : scenes)
				sceneCombo->addItem(scene.name, scene.uuid);
			if (!currentSceneId.isEmpty())
				sceneCombo->setCurrentIndex(std::max(0, sceneCombo->findData(currentSceneId)));
			if (sceneCombo->count() > 0 && sceneCombo->currentIndex() < 0)
				sceneCombo->setCurrentIndex(0);
		}
		LoadSelectedSceneOverride();
	}

private:
	void LoadSelectedSceneOverride()
	{
		if (!workspace || !workspace->CanvasManager())
			return;

		QScopedValueRollback<bool> guard(refreshing, true);
		const QString sceneId = sceneCombo->currentData().toString();
		const QString transitionName = workspace->CanvasManager()->SceneTransitionName(sceneId);
		overrideTransitionCombo->setCurrentIndex(
			std::max(0, overrideTransitionCombo->findData(transitionName.isEmpty()
									     ? workspace->CanvasManager()->DefaultTransitionName()
									     : transitionName)));
		const int duration = workspace->CanvasManager()->SceneTransitionDuration(sceneId);
		overrideDurationSpin->setValue(duration > 0 ? duration : workspace->CanvasManager()->DefaultTransitionDuration());
		const bool hasScene = !sceneId.isEmpty();
		overrideTransitionCombo->setEnabled(hasScene);
		overrideDurationSpin->setEnabled(hasScene);
		applyButton->setEnabled(hasScene);
		clearButton->setEnabled(hasScene);
	}

	SwitcherWorkspaceDock *workspace = nullptr;
	QComboBox *defaultTransitionCombo = nullptr;
	QSpinBox *defaultDurationSpin = nullptr;
	QComboBox *sceneCombo = nullptr;
	QComboBox *overrideTransitionCombo = nullptr;
	QSpinBox *overrideDurationSpin = nullptr;
	QPushButton *applyButton = nullptr;
	QPushButton *clearButton = nullptr;
	bool refreshing = false;
};

class SwitchVerticalSettingsDockWidget : public QWidget {
public:
	explicit SwitchVerticalSettingsDockWidget(SwitcherWorkspaceDock *workspace_, QWidget *parent = nullptr)
		: QWidget(parent),
		  workspace(workspace_),
		  canvasNameEdit(new QLineEdit(this)),
		  presetCombo(new QComboBox(this)),
		  linkedSyncCheck(new QCheckBox(QStringLiteral("Sync linked scenes from the current program scene"), this)),
		  statusLabel(new QLabel(this)),
		  summaryLabel(new QLabel(this)),
		  configureButton(new QPushButton(QStringLiteral("Configure Vertical Output"), this)),
		  recordButton(new QPushButton(this)),
		  pauseButton(new QPushButton(this)),
		  splitButton(new QPushButton(QStringLiteral("Split Recording"), this)),
		  chapterButton(new QPushButton(QStringLiteral("Add Chapter"), this)),
		  streamButton(new QPushButton(this)),
		  replayButton(new QPushButton(this)),
		  saveReplayButton(new QPushButton(QStringLiteral("Save Replay"), this)),
		  virtualCamButton(new QPushButton(this))
	{
		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(12, 12, 12, 12);
		layout->setSpacing(12);

		presetCombo->addItem(QStringLiteral("9:16 (1080x1920)"), QSize(1080, 1920));
		presetCombo->addItem(QStringLiteral("9:16 (720x1280)"), QSize(720, 1280));
		presetCombo->addItem(QStringLiteral("1:1 (1080x1080)"), QSize(1080, 1080));
		presetCombo->addItem(QStringLiteral("16:9 (1920x1080)"), QSize(1920, 1080));

		auto *form = new QFormLayout;
		form->setLabelAlignment(Qt::AlignLeft);
		form->addRow(QStringLiteral("Name"), canvasNameEdit);
		form->addRow(QStringLiteral("Preset"), presetCombo);
		layout->addLayout(form);
		layout->addWidget(linkedSyncCheck);

		statusLabel->setWordWrap(true);
		summaryLabel->setWordWrap(true);
		summaryLabel->setTextFormat(Qt::RichText);
		layout->addWidget(statusLabel);
		layout->addWidget(summaryLabel);
		layout->addWidget(configureButton);

		auto *outputGrid = new QGridLayout;
		outputGrid->setContentsMargins(0, 0, 0, 0);
		outputGrid->setHorizontalSpacing(8);
		outputGrid->setVerticalSpacing(8);
		outputGrid->addWidget(recordButton, 0, 0);
		outputGrid->addWidget(pauseButton, 0, 1);
		outputGrid->addWidget(splitButton, 0, 2);
		outputGrid->addWidget(chapterButton, 1, 0);
		outputGrid->addWidget(streamButton, 1, 1);
		outputGrid->addWidget(replayButton, 1, 2);
		outputGrid->addWidget(saveReplayButton, 2, 0);
		outputGrid->addWidget(virtualCamButton, 2, 1, 1, 2);
		layout->addLayout(outputGrid);
		layout->addStretch(1);

		connect(canvasNameEdit, &QLineEdit::editingFinished, this, [this]() {
			if (!refreshing && workspace && workspace->CanvasManager())
				workspace->CanvasManager()->SetCanvasName(workspace->VerticalCanvasId(), canvasNameEdit->text().trimmed());
		});
		connect(presetCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
			if (refreshing || !workspace || !workspace->CanvasManager() || index < 0)
				return;
			workspace->CanvasManager()->SetVerticalPreset(presetCombo->itemData(index).toSize(),
								     presetCombo->itemText(index));
		});
		connect(linkedSyncCheck, &QCheckBox::checkStateChanged, this, [this](Qt::CheckState state) {
			if (!refreshing && workspace && workspace->CanvasManager())
				workspace->CanvasManager()->SetCanvasLinkedSync(workspace->VerticalCanvasId(), state == Qt::Checked);
		});
		connect(configureButton, &QPushButton::clicked, this, [this]() {
			if (workspace)
				QMetaObject::invokeMethod(workspace, "OpenVerticalOutputSettings", Qt::QueuedConnection);
		});
		connect(recordButton, &QPushButton::clicked, this, [this]() {
			if (workspace)
				workspace->PerformOutputAction(QStringLiteral("toggle_recording"));
		});
		connect(pauseButton, &QPushButton::clicked, this, [this]() {
			if (workspace)
				workspace->PerformOutputAction(QStringLiteral("toggle_recording_pause"));
		});
		connect(splitButton, &QPushButton::clicked, this, [this]() {
			if (workspace)
				workspace->PerformOutputAction(QStringLiteral("split_recording"));
		});
		connect(chapterButton, &QPushButton::clicked, this, [this]() {
			if (workspace)
				workspace->PerformOutputAction(QStringLiteral("add_recording_chapter"));
		});
		connect(streamButton, &QPushButton::clicked, this, [this]() {
			if (workspace)
				workspace->PerformOutputAction(QStringLiteral("toggle_streaming"));
		});
		connect(replayButton, &QPushButton::clicked, this, [this]() {
			if (workspace)
				workspace->PerformOutputAction(QStringLiteral("toggle_replay"));
		});
		connect(saveReplayButton, &QPushButton::clicked, this, [this]() {
			if (workspace)
				workspace->PerformOutputAction(QStringLiteral("save_replay"));
		});
		connect(virtualCamButton, &QPushButton::clicked, this, [this]() {
			if (workspace)
				workspace->PerformOutputAction(QStringLiteral("toggle_virtual_camera"));
		});

		Refresh();
	}

	void Refresh()
	{
		if (!workspace || !workspace->CanvasManager())
			return;

		QScopedValueRollback<bool> guard(refreshing, true);
		auto *manager = workspace->CanvasManager();
		const auto descriptor = workspace->CanvasDescriptorForId(workspace->VerticalCanvasId());
		const auto settings = manager->OutputSettings(workspace->VerticalCanvasId());
		canvasNameEdit->setText(descriptor.name);
		linkedSyncCheck->setChecked(descriptor.linkedSceneSync);

		const int presetIndex = std::max(0, presetCombo->findData(descriptor.size));
		presetCombo->setCurrentIndex(presetIndex);

		QStringList liveStates;
		if (obs_frontend_streaming_active())
			liveStates.push_back(QStringLiteral("Live"));
		if (obs_frontend_recording_active())
			liveStates.push_back(obs_frontend_recording_paused() ? QStringLiteral("Paused") : QStringLiteral("Recording"));
		if (obs_frontend_replay_buffer_active())
			liveStates.push_back(QStringLiteral("Replay"));
		if (obs_frontend_virtualcam_active())
			liveStates.push_back(QStringLiteral("Virtual Cam"));
		if (liveStates.isEmpty())
			liveStates.push_back(QStringLiteral("Idle"));
		statusLabel->setText(QStringLiteral("%1 | %2").arg(descriptor.activeSceneName.isEmpty()
									   ? QStringLiteral("No vertical scene selected")
									   : descriptor.activeSceneName,
								   liveStates.join(QStringLiteral(" | "))));

		const QString audioTracks = settings.audioTrackMask == 0
						 ? QStringLiteral("none")
						 : QString::number(settings.audioTrackMask, 16).toUpper();
		summaryLabel->setText(
			QStringLiteral("<b>Streaming</b><br/>%1 | %2 kbps | delay %3 ms%4<br/><br/>"
				       "<b>Recording</b><br/>%5 | %6 kbps | path %7<br/><br/>"
				       "<b>Replay</b><br/>%8 | %9 sec | path %10<br/><br/>"
				       "<b>Audio</b><br/>%11 kbps | track mask 0x%12")
				.arg(settings.followMainStreaming ? QStringLiteral("Follow Main")
								 : settings.streamEncoderId.isEmpty()
									   ? QStringLiteral("Custom")
									   : settings.streamEncoderId)
				.arg(settings.streamingVideoBitrateKbps)
				.arg(settings.streamDelayMs)
				.arg(settings.streamDelayPreserve ? QStringLiteral(" preserved") : QString())
				.arg(settings.followMainRecording ? QStringLiteral("Follow Main")
								 : settings.recordEncoderId.isEmpty()
									   ? QStringLiteral("Custom")
									   : settings.recordEncoderId)
				.arg(settings.recordingVideoBitrateKbps)
				.arg(settings.recordingPath.isEmpty() ? QStringLiteral("Default") : settings.recordingPath)
				.arg(settings.followMainReplay ? QStringLiteral("Follow Main")
							      : settings.replayAlwaysOn ? QStringLiteral("Always On")
										: QStringLiteral("Manual"))
				.arg(settings.replayDurationSeconds)
				.arg(settings.replayPath.isEmpty() ? QStringLiteral("Default") : settings.replayPath)
				.arg(settings.audioBitrateKbps)
				.arg(audioTracks));

		recordButton->setText(obs_frontend_recording_active() ? QStringLiteral("Stop Recording")
							     : QStringLiteral("Start Recording"));
		pauseButton->setText(obs_frontend_recording_paused() ? QStringLiteral("Resume Recording")
							    : QStringLiteral("Pause Recording"));
		streamButton->setText(obs_frontend_streaming_active() ? QStringLiteral("Stop Streaming")
							    : QStringLiteral("Start Streaming"));
		replayButton->setText(obs_frontend_replay_buffer_active() ? QStringLiteral("Stop Replay Buffer")
							      : QStringLiteral("Start Replay Buffer"));
		virtualCamButton->setText(obs_frontend_virtualcam_active() ? QStringLiteral("Stop Virtual Camera")
								   : QStringLiteral("Start Virtual Camera"));
		pauseButton->setEnabled(obs_frontend_recording_active());
		splitButton->setEnabled(obs_frontend_recording_active() && !obs_frontend_recording_paused());
		chapterButton->setEnabled(obs_frontend_recording_active() && !obs_frontend_recording_paused());
		saveReplayButton->setEnabled(obs_frontend_replay_buffer_active());
	}

private:
	SwitcherWorkspaceDock *workspace = nullptr;
	QLineEdit *canvasNameEdit = nullptr;
	QComboBox *presetCombo = nullptr;
	QCheckBox *linkedSyncCheck = nullptr;
	QLabel *statusLabel = nullptr;
	QLabel *summaryLabel = nullptr;
	QPushButton *configureButton = nullptr;
	QPushButton *recordButton = nullptr;
	QPushButton *pauseButton = nullptr;
	QPushButton *splitButton = nullptr;
	QPushButton *chapterButton = nullptr;
	QPushButton *streamButton = nullptr;
	QPushButton *replayButton = nullptr;
	QPushButton *saveReplayButton = nullptr;
	QPushButton *virtualCamButton = nullptr;
	bool refreshing = false;
};

SwitcherWorkspacePreview::SwitcherWorkspacePreview(QWidget *parent) : QWidget(parent)
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
}

SwitcherWorkspacePreview::~SwitcherWorkspacePreview()
{
	DestroyPreviewDisplay();
}

void SwitcherWorkspacePreview::DrawPreview(void *data, uint32_t cx, uint32_t cy)
{
	auto *preview = static_cast<SwitcherWorkspacePreview *>(data);
	if (!preview || !preview->source)
		return;

	uint32_t sourceCX = obs_source_get_width(preview->source);
	if (sourceCX <= 0)
		sourceCX = 1;
	uint32_t sourceCY = obs_source_get_height(preview->source);
	if (sourceCY <= 0)
		sourceCY = 1;

	int x = 0;
	int y = 0;
	float scale = 1.0f;
	GetScaleAndCenterPos(sourceCX, sourceCY, cx, cy, x, y, scale);

	gs_viewport_push();
	gs_projection_push();
	const bool previous = gs_set_linear_srgb(true);

	gs_ortho(0.0f, float(sourceCX), 0.0f, float(sourceCY), -100.0f, 100.0f);
	gs_set_viewport(x, y, int(scale * float(sourceCX)), int(scale * float(sourceCY)));
	obs_source_video_render(preview->source);

	gs_set_linear_srgb(previous);
	gs_projection_pop();
	gs_viewport_pop();
}

void SwitcherWorkspacePreview::SetSource(const OBSSource &source_)
{
	if (source == source_)
		return;

	if (previewActive && source)
		obs_source_dec_showing(source);

	source = source_;
	const QCursor cursor = source && obs_source_is_scene(source) ? QCursor(Qt::PointingHandCursor) : QCursor(Qt::ArrowCursor);
	setCursor(cursor);
	if (display)
		display->setCursor(cursor);

	if (previewActive && source)
		obs_source_inc_showing(source);

	UpdatePreviewLifecycle();
}

OBSSource SwitcherWorkspacePreview::GetSource() const
{
	return source;
}

void SwitcherWorkspacePreview::SetPreviewActive(bool active)
{
	if (previewConfigured == active)
		return;

	previewConfigured = active;
	UpdatePreviewLifecycle();
}

void SwitcherWorkspacePreview::RefreshActiveState()
{
}

void SwitcherWorkspacePreview::changeEvent(QEvent *event)
{
	QWidget::changeEvent(event);

	if (event && event->type() == QEvent::WindowStateChange)
		UpdatePreviewLifecycle();
}

void SwitcherWorkspacePreview::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	UpdatePreviewLifecycle();
}

void SwitcherWorkspacePreview::hideEvent(QHideEvent *event)
{
	DeactivatePreview();
	QWidget::hideEvent(event);
}

bool SwitcherWorkspacePreview::eventFilter(QObject *watched, QEvent *event)
{
	if (watched != display || !event)
		return QWidget::eventFilter(watched, event);

	switch (event->type()) {
	case QEvent::ContextMenu:
		return HandleContextMenuEvent(static_cast<QContextMenuEvent *>(event));
	case QEvent::MouseButtonRelease:
	case QEvent::MouseButtonDblClick:
		return HandleMouseButtonEvent(static_cast<QMouseEvent *>(event));
	default:
		return QWidget::eventFilter(watched, event);
	}
}

bool SwitcherWorkspacePreview::HandleMouseButtonEvent(QMouseEvent *event)
{
	if (!event)
		return false;

#ifdef __APPLE__
	const bool controlClickContext =
		event->button() == Qt::LeftButton && event->modifiers().testFlag(Qt::ControlModifier);
#else
	const bool controlClickContext = false;
#endif

	if (event->button() == Qt::RightButton || controlClickContext) {
		if (event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::MouseButtonDblClick)
			emit ContextMenuRequested(event->globalPosition().toPoint());
		return true;
	}

	if (!source || !obs_source_is_scene(source) || event->button() != Qt::LeftButton)
		return false;

	if (event->type() == QEvent::MouseButtonDblClick && obs_frontend_preview_program_mode_active()) {
		auto *userConfig = obs_frontend_get_user_config();
		if (!userConfig || config_get_bool(userConfig, "BasicWindow", "TransitionOnDoubleClick"))
			obs_frontend_set_current_scene(source);
		return true;
	}

	if (event->type() != QEvent::MouseButtonRelease)
		return false;

	if (obs_frontend_preview_program_mode_active())
		obs_frontend_set_current_preview_scene(source);
	else
		obs_frontend_set_current_scene(source);

	return true;
}

bool SwitcherWorkspacePreview::HandleContextMenuEvent(QContextMenuEvent *event)
{
	if (!event)
		return false;

	emit ContextMenuRequested(event->globalPos());
	return true;
}

bool SwitcherWorkspacePreview::ShouldActivatePreview() const
{
	if (!previewConfigured || !source || !isVisible())
		return false;

	if (auto *topLevel = window(); topLevel && topLevel->isMinimized())
		return false;

	return true;
}

void SwitcherWorkspacePreview::ActivatePreview()
{
	if (previewActive || !source)
		return;

	if (!display) {
		display = new OBSQTDisplay(this);
		display->setObjectName(QStringLiteral("switcherWorkspacePreview"));
		display->setMinimumSize(QSize(24, 24));
		display->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
		display->setMouseTracking(true);
		display->setFocusPolicy(Qt::NoFocus);
		display->installEventFilter(this);
		display->hide();
		layout()->addWidget(display);

		connect(display, &OBSQTDisplay::DisplayCreated, this, [this]() {
			if (previewActive && display && display->GetDisplay() && !drawCallbackInstalled) {
				obs_display_add_draw_callback(display->GetDisplay(), DrawPreview, this);
				drawCallbackInstalled = true;
			}
		});
	}

	display->setCursor(source && obs_source_is_scene(source) ? Qt::PointingHandCursor : Qt::ArrowCursor);
	previewActive = true;
	obs_source_inc_showing(source);
	display->show();
	display->CreateDisplay();
	if (display->GetDisplay()) {
		obs_display_set_enabled(display->GetDisplay(), true);
		if (!drawCallbackInstalled) {
			obs_display_add_draw_callback(display->GetDisplay(), DrawPreview, this);
			drawCallbackInstalled = true;
		}
	}
	QTimer::singleShot(0, this, [this]() {
		if (previewActive && display) {
			display->CreateDisplay();
			if (display->GetDisplay() && !drawCallbackInstalled) {
				obs_display_set_enabled(display->GetDisplay(), true);
				obs_display_add_draw_callback(display->GetDisplay(), DrawPreview, this);
				drawCallbackInstalled = true;
			}
		}
	});
}

void SwitcherWorkspacePreview::DeactivatePreview()
{
	if (!display)
		return;

	const bool wasActive = previewActive;
	previewActive = false;

	if (drawCallbackInstalled) {
		if (auto *obsDisplay = display->GetDisplay())
			obs_display_remove_draw_callback(obsDisplay, DrawPreview, this);
		drawCallbackInstalled = false;
	}
	if (auto *obsDisplay = display->GetDisplay())
		obs_display_set_enabled(obsDisplay, false);

	display->hide();
	if (wasActive && source)
		obs_source_dec_showing(source);
}

void SwitcherWorkspacePreview::DestroyPreviewDisplay()
{
	if (!display)
		return;

	DeactivatePreview();
	display->removeEventFilter(this);
	display->DestroyDisplay();
	delete display;
	display = nullptr;
}

void SwitcherWorkspacePreview::UpdatePreviewLifecycle()
{
	if (ShouldActivatePreview())
		ActivatePreview();
	else
		DeactivatePreview();
}

SwitcherWorkspaceSlot::SwitcherWorkspaceSlot(int slotIndex_, QWidget *parent)
	: QFrame(parent),
		  slotIndex(slotIndex_),
		  previewWidget(new SwitcherWorkspacePreview(this)),
		  contentStack(new QStackedWidget(this)),
		  emptyLabel(new QLabel(this)),
		  titleLabel(new QLabel(this)),
	  contextMenu(new QMenu(this))
{
	setObjectName(QStringLiteral("switcherWorkspaceSlot"));
	setAttribute(Qt::WA_StyledBackground, true);
	setAttribute(Qt::WA_Hover, true);
	setContextMenuPolicy(Qt::DefaultContextMenu);
	setMinimumSize(180, 120);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	emptyLabel->setAlignment(Qt::AlignCenter);
	emptyLabel->setWordWrap(true);
	emptyLabel->setMargin(28);
	emptyLabel->setObjectName(QStringLiteral("switcherWorkspaceSlotEmpty"));
	emptyLabel->setText(QStringLiteral("%1\n%2")
				    .arg(QT_UTF8(obs_module_text("SwitcherEmptySlot")),
					 QT_UTF8(obs_module_text("SwitcherEmptySlotHint"))));
	contentStack->setObjectName(QStringLiteral("switcherWorkspaceSlotContent"));

	contentStack->addWidget(emptyLabel);
	contentStack->addWidget(previewWidget);

	layout->addWidget(contentStack, 1);

	titleLabel->setObjectName(QStringLiteral("switcherWorkspaceSlotTitle"));
	titleLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);

	editAction = contextMenu->addAction(QT_UTF8(obs_module_text("SwitcherEditView")));
	advancedAction = contextMenu->addAction(QT_UTF8(obs_module_text("SwitcherAdvancedSettings")));
	contextMenu->addSeparator();
	detachAction = contextMenu->addAction(QT_UTF8(obs_module_text("OpenDetachedView")));
	verticalDetachAction = contextMenu->addAction(QT_UTF8(obs_module_text("OpenVerticalView")));
	connect(editAction, &QAction::triggered, this, [this] { emit ConfigureRequested(slotIndex); });
	connect(detachAction, &QAction::triggered, this, [this] { emit DetachRequested(slotIndex); });
	connect(verticalDetachAction, &QAction::triggered, this, [this] { emit VerticalDetachRequested(slotIndex); });
	connect(advancedAction, &QAction::triggered, this, [this] { emit AdvancedRequested(slotIndex); });
	connect(previewWidget, &SwitcherWorkspacePreview::ContextMenuRequested, this, &SwitcherWorkspaceSlot::ShowContextMenu);

	RefreshPresentation();
	RefreshSelectionState();
}

void SwitcherWorkspaceSlot::SetSource(const OBSSource &source)
{
	previewWidget->SetSource(source);
	previewWidget->SetPreviewActive(previewActive && source);
	RefreshPresentation();
	RefreshActiveState();
}

OBSSource SwitcherWorkspaceSlot::GetSource()
{
	return previewWidget->GetSource();
}

void SwitcherWorkspaceSlot::SetCustomTitle(const QString &title)
{
	customTitle = title.trimmed();
	RefreshPresentation();
}

QString SwitcherWorkspaceSlot::GetCustomTitle() const
{
	return customTitle;
}

QString SwitcherWorkspaceSlot::GetEffectiveTitle() const
{
	if (!customTitle.isEmpty())
		return customTitle;
	if (auto source = previewWidget->GetSource())
		return QT_UTF8(obs_source_get_name(source));
	return DefaultTitle();
}

void SwitcherWorkspaceSlot::SetPreviewActive(bool active)
{
	if (previewActive == active)
		return;

	previewActive = active;
	previewWidget->SetPreviewActive(previewActive && previewWidget->GetSource());
}

void SwitcherWorkspaceSlot::SetSelected(bool active)
{
	if (selected == active)
		return;

	selected = active;
	RefreshSelectionState();
}

void SwitcherWorkspaceSlot::RefreshActiveState()
{
	previewWidget->RefreshActiveState();
}

void SwitcherWorkspaceSlot::ClearIfMatches(obs_source_t *source)
{
	if (previewWidget->GetSource().Get() == source)
		SetSource(nullptr);
}

void SwitcherWorkspaceSlot::RefreshPresentation()
{
	const bool hasSource = previewWidget->GetSource();
	contentStack->setCurrentWidget(hasSource ? static_cast<QWidget *>(previewWidget) : static_cast<QWidget *>(emptyLabel));
	contentStack->setAttribute(Qt::WA_TransparentForMouseEvents, !hasSource);
	emptyLabel->setAttribute(Qt::WA_TransparentForMouseEvents, !hasSource);
	RefreshTitleLabel();
}

void SwitcherWorkspaceSlot::resizeEvent(QResizeEvent *event)
{
	QFrame::resizeEvent(event);
	RefreshTitleLabel();
	titleLabel->raise();
}

void SwitcherWorkspaceSlot::RefreshTitleLabel()
{
	const QString title = GetEffectiveTitle();
	const int horizontalMargin = 16;
	const int maxWidth = std::max(112, width() - (horizontalMargin * 2));

	titleLabel->setText(fontMetrics().elidedText(title, Qt::ElideRight, maxWidth));
	titleLabel->setToolTip(title);
	titleLabel->adjustSize();
	titleLabel->resize(std::min(maxWidth, titleLabel->sizeHint().width() + 20), 28);
	titleLabel->move(horizontalMargin, horizontalMargin);
}

void SwitcherWorkspaceSlot::RefreshSelectionState()
{
	titleLabel->setProperty("selected", selected);
	style()->unpolish(titleLabel);
	style()->polish(titleLabel);
	titleLabel->update();
}

void SwitcherWorkspaceSlot::ShowContextMenu(const QPoint &globalPos)
{
	const bool hasSource = previewWidget->GetSource();
	detachAction->setEnabled(hasSource);
	verticalDetachAction->setEnabled(hasSource);
	contextMenu->exec(globalPos);
}

void SwitcherWorkspaceSlot::contextMenuEvent(QContextMenuEvent *event)
{
	if (!previewWidget->GetSource() && event) {
		ShowContextMenu(event->globalPos());
		event->accept();
		return;
	}

	QFrame::contextMenuEvent(event);
}

void SwitcherWorkspaceSlot::mouseReleaseEvent(QMouseEvent *event)
{
	if (event) {
#ifdef __APPLE__
		const bool controlClickContext =
			event->button() == Qt::LeftButton && event->modifiers().testFlag(Qt::ControlModifier);
#else
		const bool controlClickContext = false;
#endif
		if (event->button() == Qt::RightButton || controlClickContext) {
			ShowContextMenu(event->globalPosition().toPoint());
			event->accept();
			return;
		}
	}

	QFrame::mouseReleaseEvent(event);
}

QString SwitcherWorkspaceSlot::DefaultTitle() const
{
	return QString("%1 %2").arg(QT_UTF8(obs_module_text("SwitcherSlot"))).arg(slotIndex + 1);
}

SwitcherWorkspaceDock::SwitcherWorkspaceDock(QMainWindow *parent)
	: QWidget(parent, Qt::Window),
		  modeList(new QListWidget(this)),
		  modeStack(new QStackedWidget(this)),
		  workspaceModePage(new QWidget(modeStack)),
		  verticalModePage(new QWidget(modeStack)),
		  motionModePage(new QWidget(modeStack)),
		  automationModePage(new QWidget(modeStack)),
		  remoteModePage(new QWidget(modeStack)),
		  contentSplitter(new QSplitter(Qt::Horizontal, this)),
		  scrollArea(new QScrollArea(contentSplitter)),
		  gridContainer(new QWidget(scrollArea)),
		  gridLayout(new QGridLayout(gridContainer)),
		  inspectorFrame(new QFrame(contentSplitter)),
		  inspectorModeButton(new QToolButton(inspectorFrame)),
		  inspectorTitleLabel(new QLabel(inspectorFrame)),
		  inspectorCloseButton(new QToolButton(inspectorFrame)),
		  inspectorStack(new QStackedWidget(inspectorFrame)),
		  workspacePage(new QWidget(inspectorStack)),
		  slotPage(new QWidget(inspectorStack)),
		  workspaceSettingsSection(new QWidget(workspacePage)),
		  slotEditorSection(new QWidget(slotPage)),
		  slotOutputSection(new QWidget(slotPage)),
		  remoteSettingsSection(new QWidget(remoteModePage)),
		  slotList(new QListWidget(workspacePage)),
		  layoutCombo(new QComboBox(workspacePage)),
		  sceneCombo(new QComboBox(slotPage)),
		  titleEdit(new QLineEdit(slotPage)),
		  clearSlotButton(new QPushButton(QT_UTF8(obs_module_text("Clear")), slotPage)),
		  detachSlotButton(new QPushButton(QT_UTF8(obs_module_text("OpenDetachedView")), slotPage)),
		  verticalDetachSlotButton(new QPushButton(QT_UTF8(obs_module_text("OpenVerticalView")), slotPage)),
		  slotOutputStatusLabel(new QLabel(slotOutputSection)),
		  slotProjectorButton(new QPushButton(slotOutputSection)),
		  slotRecordButton(new QPushButton(slotOutputSection)),
		  slotPauseRecordButton(new QPushButton(slotOutputSection)),
		  slotSplitRecordButton(new QPushButton(slotOutputSection)),
		  slotReplayButton(new QPushButton(slotOutputSection)),
		  slotReplaySaveButton(new QPushButton(slotOutputSection)),
		  slotStreamButton(new QPushButton(slotOutputSection)),
		  slotVirtualCamButton(new QPushButton(slotOutputSection)),
		  remoteEnabledCheckBox(new QCheckBox(QT_UTF8(obs_module_text("SwitcherRemoteEnable")), remoteModePage)),
		  remoteAutoStartCheckBox(new QCheckBox(QT_UTF8(obs_module_text("SwitcherRemoteAutoStart")), remoteModePage)),
		  remoteResolutionCombo(new QComboBox(remoteModePage)),
		  remoteFpsCombo(new QComboBox(remoteModePage)),
		  remoteUrlEdit(new QLineEdit(remoteModePage)),
		  remoteStatusLabel(new QLabel(remoteModePage)),
		  remoteCopyButton(new QPushButton(QT_UTF8(obs_module_text("SwitcherRemoteCopyUrl")), remoteModePage)),
		  remoteRegenerateButton(new QPushButton(QT_UTF8(obs_module_text("SwitcherRemoteRegenerateToken")),
							 remoteModePage)),
		  remoteRestartButton(new QPushButton(QT_UTF8(obs_module_text("SwitcherRemoteRestart")), remoteModePage))
{
	gWorkspaceDock = this;
	canvasManager = new SwitchCanvasManager(this);
	motionManager = new SwitchMotionManager(this);
	automationEngine = new SwitchAutomationEngine(canvasManager, motionManager, this);
	setObjectName(QStringLiteral("switcherWorkspaceRoot"));
	setAttribute(Qt::WA_StyledBackground, true);
	for (auto *page : {workspaceModePage, verticalModePage, motionModePage, automationModePage, remoteModePage}) {
		page->setObjectName(QStringLiteral("switcherModePage"));
		page->setAttribute(Qt::WA_StyledBackground, true);
	}
	setWindowTitle(QT_UTF8(obs_module_text("Switcher")));
	setWindowFlag(Qt::WindowMinMaxButtonsHint, true);
	setMinimumSize(1120, 720);
	resize(1400, 860);

	auto *rootLayout = new QHBoxLayout(this);
	rootLayout->setContentsMargins(18, 18, 18, 18);
	rootLayout->setSpacing(16);

	auto *modeRail = new QFrame(this);
	modeRail->setObjectName(QStringLiteral("switcherModeRail"));
	modeRail->setAttribute(Qt::WA_StyledBackground, true);
	modeRail->setFixedWidth(220);
	auto *modeRailLayout = new QVBoxLayout(modeRail);
	modeRailLayout->setContentsMargins(8, 8, 8, 8);
	modeRailLayout->setSpacing(0);

	auto *modeTitle = new QLabel(QStringLiteral("Switch"), modeRail);
	modeTitle->setObjectName(QStringLiteral("switcherModeTitle"));
	modeTitle->setVisible(false);
	modeRailLayout->addWidget(modeTitle);

	modeList->setObjectName(QStringLiteral("switcherModeList"));
	modeList->setFrameShape(QFrame::NoFrame);
	modeList->setIconSize(QSize(24, 24));
	modeList->setUniformItemSizes(true);
	modeList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	modeList->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	modeList->setSelectionMode(QAbstractItemView::SingleSelection);
	modeList->setMouseTracking(true);
	modeList->setSpacing(0);
	modeList->setItemDelegate(new SwitchModeItemDelegate(modeList));

	const auto addModeItem = [this](const QString &title, const QString &glyph) {
		auto *item = new QListWidgetItem(title, modeList);
		item->setData(kSwitchModeGlyphRole, glyph);
		item->setSizeHint(QSize(0, 52));
	};
	addModeItem(QStringLiteral("Workspace"), QStringLiteral("workspace"));
	addModeItem(QStringLiteral("Vertical"), QStringLiteral("vertical"));
	addModeItem(QStringLiteral("Motion"), QStringLiteral("motion"));
	addModeItem(QStringLiteral("Automation"), QStringLiteral("automation"));
	addModeItem(QT_UTF8(obs_module_text("SwitcherRemote")), QStringLiteral("remote"));
	modeList->setFixedHeight(modeList->count() * 52 + 2 * modeList->frameWidth());
	modeRailLayout->addWidget(modeList);
	modeRailLayout->addStretch(1);

	modeStack->setObjectName(QStringLiteral("switcherModeStack"));
	rootLayout->addWidget(modeRail, 0);
	rootLayout->addWidget(modeStack, 1);

	modeStack->addWidget(workspaceModePage);
	modeStack->addWidget(verticalModePage);
	modeStack->addWidget(motionModePage);
	modeStack->addWidget(automationModePage);
	modeStack->addWidget(remoteModePage);

	auto *workspaceModeLayout = new QVBoxLayout(workspaceModePage);
	workspaceModeLayout->setContentsMargins(0, 0, 0, 0);
	workspaceModeLayout->setSpacing(0);

	contentSplitter->setChildrenCollapsible(false);
	contentSplitter->setHandleWidth(1);
	contentSplitter->setOpaqueResize(false);
	contentSplitter->addWidget(scrollArea);
	contentSplitter->addWidget(inspectorFrame);
	contentSplitter->setStretchFactor(0, 1);
	contentSplitter->setStretchFactor(1, 0);
	workspaceModeLayout->addWidget(contentSplitter, 1);

	scrollArea->setWidgetResizable(true);
	scrollArea->setObjectName(QStringLiteral("switcherWorkspaceScrollArea"));
	scrollArea->setFrameShape(QFrame::NoFrame);
	scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	scrollArea->setWidget(gridContainer);
	gridContainer->setObjectName(QStringLiteral("switcherWorkspaceGridPage"));
	gridContainer->setAttribute(Qt::WA_StyledBackground, true);

	gridLayout->setContentsMargins(22, 22, 22, 22);
	gridLayout->setSpacing(18);

	inspectorFrame->setObjectName(QStringLiteral("switcherWorkspaceInspector"));
	inspectorFrame->setAttribute(Qt::WA_StyledBackground, true);
	inspectorFrame->setMinimumWidth(420);
	inspectorFrame->setMaximumWidth(560);
	inspectorFrame->hide();

	auto *inspectorLayout = new QVBoxLayout(inspectorFrame);
	inspectorLayout->setContentsMargins(0, 0, 0, 0);
	inspectorLayout->setSpacing(0);

	auto *inspectorHeader = new QWidget(inspectorFrame);
	inspectorHeader->setObjectName(QStringLiteral("switcherWorkspaceInspectorHeader"));
	inspectorHeader->setAttribute(Qt::WA_StyledBackground, true);
	auto *inspectorHeaderLayout = new QHBoxLayout(inspectorHeader);
	inspectorHeaderLayout->setContentsMargins(18, 18, 18, 12);
	inspectorHeaderLayout->setSpacing(8);

	inspectorModeButton->setObjectName(QStringLiteral("switcherWorkspaceInspectorModeButton"));
	inspectorModeButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	inspectorModeButton->setCursor(Qt::PointingHandCursor);
	inspectorModeButton->setAutoRaise(false);
	inspectorHeaderLayout->addWidget(inspectorModeButton);

	inspectorTitleLabel->setObjectName(QStringLiteral("switcherWorkspaceSettingsTitle"));
	inspectorHeaderLayout->addWidget(inspectorTitleLabel);
	inspectorHeaderLayout->addStretch(1);

	inspectorCloseButton->setObjectName(QStringLiteral("switcherWorkspaceInspectorCloseButton"));
	inspectorCloseButton->setCursor(Qt::PointingHandCursor);
	inspectorCloseButton->setAutoRaise(false);
	inspectorCloseButton->setFixedSize(32, 32);
	inspectorCloseButton->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton));
	inspectorHeaderLayout->addWidget(inspectorCloseButton);
	inspectorLayout->addWidget(inspectorHeader);

	inspectorStack->setObjectName(QStringLiteral("switcherWorkspaceInspectorStack"));
	inspectorLayout->addWidget(inspectorStack, 1);
	inspectorStack->addWidget(workspacePage);
	inspectorStack->addWidget(slotPage);

	for (int index = 0; index < kMaxSwitcherSlots; index++) {
		auto *slot = new SwitcherWorkspaceSlot(index, gridContainer);
		connect(slot, &SwitcherWorkspaceSlot::ConfigureRequested, this, &SwitcherWorkspaceDock::OpenSlotSettings);
		connect(slot, &SwitcherWorkspaceSlot::DetachRequested, this, &SwitcherWorkspaceDock::OpenSlotAsDock);
		connect(slot, &SwitcherWorkspaceSlot::VerticalDetachRequested, this,
			&SwitcherWorkspaceDock::OpenSlotAsVerticalDock);
		connect(slot, &SwitcherWorkspaceSlot::AdvancedRequested, this, &SwitcherWorkspaceDock::OpenAdvancedSettings);
		slotWidgets.push_back(slot);
	}

	layoutCombo->addItem(QT_UTF8(obs_module_text("SwitcherLayout2x2")), 4);
	layoutCombo->addItem(QT_UTF8(obs_module_text("SwitcherLayout3x3")), 9);
	layoutCombo->addItem(QT_UTF8(obs_module_text("SwitcherLayout4x4")), 16);
	layoutCombo->addItem(QT_UTF8(obs_module_text("SwitcherLayout5x5")), 25);

	slotList->setMaximumHeight(220);
	slotList->setAlternatingRowColors(true);

	remoteResolutionCombo->addItem(QT_UTF8(obs_module_text("SwitcherRemote720p")), QSize(1280, 720));
	remoteResolutionCombo->addItem(QT_UTF8(obs_module_text("SwitcherRemote1080p")), QSize(1920, 1080));

	remoteFpsCombo->addItem(QStringLiteral("5"), 5);
	remoteFpsCombo->addItem(QStringLiteral("10"), 10);
	remoteFpsCombo->addItem(QStringLiteral("15"), 15);
	remoteFpsCombo->addItem(QStringLiteral("30"), 30);

	remoteUrlEdit->setReadOnly(true);
	remoteStatusLabel->setWordWrap(true);
	SetMinimumControlHeight({layoutCombo, sceneCombo, titleEdit, clearSlotButton, detachSlotButton,
				 verticalDetachSlotButton, slotProjectorButton, slotRecordButton,
				 slotPauseRecordButton, slotSplitRecordButton, slotReplayButton, slotReplaySaveButton,
				 slotStreamButton, slotVirtualCamButton, remoteResolutionCombo, remoteFpsCombo, remoteUrlEdit,
				 remoteCopyButton, remoteRegenerateButton, remoteRestartButton});

	auto *workspacePageLayout = new QVBoxLayout(workspacePage);
	workspacePageLayout->setContentsMargins(18, 0, 18, 18);
	workspacePageLayout->setSpacing(14);

	auto *workspaceSettingsLayout = new QVBoxLayout(workspaceSettingsSection);
	workspaceSettingsLayout->setContentsMargins(0, 0, 0, 0);
	workspaceSettingsLayout->setSpacing(14);
	auto *workspaceSettingsTitle = new QLabel(QT_UTF8(obs_module_text("SwitcherAdvancedSettings")),
						 workspaceSettingsSection);
	workspaceSettingsTitle->setObjectName(QStringLiteral("switcherWorkspaceSettingsSectionTitle"));
	workspaceSettingsLayout->addWidget(workspaceSettingsTitle);

	auto *layoutForm = new QFormLayout;
	ConfigureFormLayout(layoutForm);
	layoutForm->addRow(QT_UTF8(obs_module_text("SwitcherLayout")), layoutCombo);
	workspaceSettingsLayout->addLayout(layoutForm);
	auto *slotListTitle = new QLabel(QT_UTF8(obs_module_text("SwitcherSlots")), workspaceSettingsSection);
	slotListTitle->setObjectName(QStringLiteral("switcherWorkspaceSettingsSectionTitle"));
	workspaceSettingsLayout->addWidget(slotListTitle);
	workspaceSettingsLayout->addWidget(slotList);
	workspacePageLayout->addWidget(workspaceSettingsSection);

	auto *slotPageLayout = new QVBoxLayout(slotPage);
	slotPageLayout->setContentsMargins(18, 0, 18, 18);
	slotPageLayout->setSpacing(14);

	auto *slotEditorLayout = new QVBoxLayout(slotEditorSection);
	slotEditorLayout->setContentsMargins(0, 0, 0, 0);
	slotEditorLayout->setSpacing(12);
	auto *slotEditorTitle = new QLabel(QT_UTF8(obs_module_text("SwitcherEditView")), slotEditorSection);
	slotEditorTitle->setObjectName(QStringLiteral("switcherWorkspaceSettingsSectionTitle"));
	slotEditorLayout->addWidget(slotEditorTitle);

	auto *editorForm = new QFormLayout;
	ConfigureFormLayout(editorForm);
	editorForm->addRow(QT_UTF8(obs_module_text("Source")), sceneCombo);
	editorForm->addRow(QT_UTF8(obs_module_text("Title")), titleEdit);
	slotEditorLayout->addLayout(editorForm);
	auto *slotActionLayout = new QHBoxLayout;
	slotActionLayout->setContentsMargins(0, 0, 0, 0);
	slotActionLayout->setSpacing(8);
	slotActionLayout->addWidget(clearSlotButton);
	slotActionLayout->addWidget(detachSlotButton);
	slotActionLayout->addWidget(verticalDetachSlotButton);
	slotEditorLayout->addLayout(slotActionLayout);
	slotPageLayout->addWidget(slotEditorSection);

	auto *slotOutputLayout = new QVBoxLayout(slotOutputSection);
	slotOutputLayout->setContentsMargins(0, 0, 0, 0);
	slotOutputLayout->setSpacing(10);
	auto *slotOutputTitle = new QLabel(QT_UTF8(obs_module_text("SwitcherViewOutputs")), slotOutputSection);
	slotOutputTitle->setObjectName(QStringLiteral("switcherWorkspaceSettingsSectionTitle"));
	slotOutputLayout->addWidget(slotOutputTitle);
	slotOutputStatusLabel->setWordWrap(true);
	slotOutputLayout->addWidget(slotOutputStatusLabel);

	auto *slotOutputGrid = new QGridLayout;
	slotOutputGrid->setContentsMargins(0, 0, 0, 0);
	slotOutputGrid->setHorizontalSpacing(8);
	slotOutputGrid->setVerticalSpacing(8);
	slotOutputGrid->addWidget(slotProjectorButton, 0, 0, 1, 2);
	slotOutputGrid->addWidget(slotRecordButton, 1, 0);
	slotOutputGrid->addWidget(slotPauseRecordButton, 1, 1);
	slotOutputGrid->addWidget(slotSplitRecordButton, 2, 0);
	slotOutputGrid->addWidget(slotStreamButton, 2, 1);
	slotOutputGrid->addWidget(slotReplayButton, 3, 0);
	slotOutputGrid->addWidget(slotReplaySaveButton, 3, 1);
	slotOutputGrid->addWidget(slotVirtualCamButton, 4, 0, 1, 2);
	slotOutputLayout->addLayout(slotOutputGrid);
	slotPageLayout->addWidget(slotOutputSection);
	slotPageLayout->addStretch(1);

	auto *remoteSettingsLayout = new QVBoxLayout(remoteSettingsSection);
	remoteSettingsLayout->setContentsMargins(0, 0, 0, 0);
	remoteSettingsLayout->setSpacing(12);
	auto *remoteLabel = new QLabel(QT_UTF8(obs_module_text("SwitcherRemote")), remoteSettingsSection);
	remoteLabel->setObjectName(QStringLiteral("switcherWorkspaceSettingsSectionTitle"));
	remoteSettingsLayout->addWidget(remoteLabel);
	remoteSettingsLayout->addWidget(remoteEnabledCheckBox);
	remoteSettingsLayout->addWidget(remoteAutoStartCheckBox);

	auto *remoteForm = new QFormLayout;
	ConfigureFormLayout(remoteForm);
	remoteForm->addRow(QT_UTF8(obs_module_text("SwitcherRemoteResolution")), remoteResolutionCombo);
	remoteForm->addRow(QT_UTF8(obs_module_text("SwitcherRemoteFps")), remoteFpsCombo);
	remoteForm->addRow(QT_UTF8(obs_module_text("SwitcherRemoteUrl")), remoteUrlEdit);
	remoteForm->addRow(QT_UTF8(obs_module_text("SwitcherRemoteStatus")), remoteStatusLabel);
	remoteSettingsLayout->addLayout(remoteForm);
	remoteSettingsLayout->addWidget(remoteCopyButton);
	remoteSettingsLayout->addWidget(remoteRegenerateButton);
	remoteSettingsLayout->addWidget(remoteRestartButton);
	workspacePageLayout->addStretch(1);

	auto *remoteModeLayout = new QVBoxLayout(remoteModePage);
	remoteModeLayout->setContentsMargins(18, 18, 18, 18);
	remoteModeLayout->setSpacing(14);
	remoteModeLayout->addWidget(remoteSettingsSection);
	remoteModeLayout->addStretch(1);

	verticalCanvasNameEdit = new QLineEdit(verticalModePage);
	verticalPresetCombo = new QComboBox(verticalModePage);
	verticalLinkedSyncCheckBox = new QCheckBox(QStringLiteral("Sync linked scenes from the current program scene"), verticalModePage);
	verticalTransitionCombo = new QComboBox(verticalModePage);
	verticalTransitionDurationSpin = new QSpinBox(verticalModePage);
	verticalSurfaceTabs = new QTabWidget(verticalModePage);
	verticalCanvasStatusLabel = new QLabel(verticalModePage);
	verticalSceneList = new QListWidget(verticalModePage);
	verticalSourceTree = new QTreeWidget(verticalModePage);
	verticalSceneAddButton = new QPushButton(QStringLiteral("Add Vertical Scene"), verticalModePage);
	verticalSceneRemoveButton = new QPushButton(QStringLiteral("Remove Vertical Scene"), verticalModePage);
	verticalSceneMenuButton = new QPushButton(QStringLiteral("Scene Menu"), verticalModePage);
	verticalSourcePropertiesButton = new QPushButton(QStringLiteral("Properties"), verticalModePage);
	verticalSourceFiltersButton = new QPushButton(QStringLiteral("Filters"), verticalModePage);
	verticalSourceMenuButton = new QPushButton(QStringLiteral("Source Menu"), verticalModePage);
	verticalSceneDockButton = new QPushButton(QStringLiteral("Dock Vertical To OBS"), verticalModePage);
	verticalScenesDockButton = new QPushButton(QStringLiteral("Open Scenes Dock"), verticalModePage);
	verticalSourcesDockButton = new QPushButton(QStringLiteral("Open Sources Dock"), verticalModePage);
	verticalTransitionsDockButton = new QPushButton(QStringLiteral("Open Transitions Dock"), verticalModePage);
	verticalSettingsDockButton = new QPushButton(QStringLiteral("Open Settings Dock"), verticalModePage);
	verticalSceneOpenWindowButton = new QPushButton(QStringLiteral("Open Vertical Window"), verticalModePage);
	verticalSceneOpenProjectorButton = new QPushButton(QStringLiteral("Open Vertical Projector"), verticalModePage);
	verticalLinkCurrentSceneButton = new QPushButton(QStringLiteral("Link Current Program Scene"), verticalModePage);
	verticalClearLinkButton = new QPushButton(QStringLiteral("Clear Selected Link"), verticalModePage);
	verticalSceneLinksList = new QListWidget(verticalModePage);
	verticalSceneOverrideCombo = new QComboBox(verticalModePage);
	verticalSceneOverrideDurationSpin = new QSpinBox(verticalModePage);
	verticalApplySceneTransitionButton = new QPushButton(QStringLiteral("Apply Override"), verticalModePage);
	verticalClearSceneTransitionButton = new QPushButton(QStringLiteral("Use Default Transition"), verticalModePage);
	verticalOutputSettingsSummaryLabel = new QLabel(verticalModePage);
	verticalConfigureOutputsButton = new QPushButton(QStringLiteral("Edit Vertical Output Profile"), verticalModePage);
	verticalOutputStatusLabel = new QLabel(verticalModePage);
	verticalRecordButton = new QPushButton(verticalModePage);
	verticalPauseRecordButton = new QPushButton(verticalModePage);
	verticalSplitRecordButton = new QPushButton(verticalModePage);
	verticalChapterButton = new QPushButton(verticalModePage);
	verticalReplayButton = new QPushButton(verticalModePage);
	verticalReplaySaveButton = new QPushButton(verticalModePage);
	verticalStreamButton = new QPushButton(verticalModePage);
	verticalVirtualCamButton = new QPushButton(verticalModePage);
	verticalCanvasPreview = new SwitchCanvasPreview(canvasManager, verticalModePage);
	verticalCanvasPreview->SetCanvasId(canvasManager->VerticalCanvasId());
	verticalCanvasStatusLabel->setObjectName(QStringLiteral("switcherVerticalPreviewStatus"));
	verticalOutputStatusLabel->setObjectName(QStringLiteral("switcherVerticalPreviewStatus"));
	verticalCanvasStatusLabel->setWordWrap(true);
	verticalOutputStatusLabel->setWordWrap(true);
	verticalSceneList->setAlternatingRowColors(true);
	verticalSceneList->setObjectName(QStringLiteral("switcherVerticalSceneList"));
	verticalSceneList->setContextMenuPolicy(Qt::CustomContextMenu);
	verticalSceneLinksList->setAlternatingRowColors(true);
	verticalSceneLinksList->setObjectName(QStringLiteral("switcherVerticalLinkList"));
	verticalSceneLinksList->setSelectionMode(QAbstractItemView::SingleSelection);
	verticalTransitionDurationSpin->setRange(50, 20000);
	verticalTransitionDurationSpin->setSingleStep(50);
	verticalTransitionDurationSpin->setSuffix(QStringLiteral(" ms"));
	verticalSceneOverrideDurationSpin->setRange(50, 20000);
	verticalSceneOverrideDurationSpin->setSingleStep(50);
	verticalSceneOverrideDurationSpin->setSuffix(QStringLiteral(" ms"));
	ConfigureVerticalSourceTree(verticalSourceTree);
	verticalSourceTree->setContextMenuPolicy(Qt::CustomContextMenu);
	verticalSourceTree->setSelectionMode(QAbstractItemView::SingleSelection);
	verticalPresetCombo->addItem(QStringLiteral("9:16 (1080x1920)"), QSize(1080, 1920));
	verticalPresetCombo->addItem(QStringLiteral("9:16 (720x1280)"), QSize(720, 1280));
	verticalPresetCombo->addItem(QStringLiteral("1:1 (1080x1080)"), QSize(1080, 1080));
	verticalPresetCombo->addItem(QStringLiteral("16:9 (1920x1080)"), QSize(1920, 1080));
	SetMinimumControlHeight({verticalCanvasNameEdit, verticalPresetCombo, verticalTransitionCombo,
				 verticalTransitionDurationSpin, verticalSceneOverrideCombo,
				 verticalSceneOverrideDurationSpin, verticalSceneAddButton, verticalSceneRemoveButton,
				 verticalSceneMenuButton, verticalSourcePropertiesButton, verticalSourceFiltersButton,
				 verticalSourceMenuButton, verticalLinkCurrentSceneButton, verticalClearLinkButton,
				 verticalApplySceneTransitionButton, verticalClearSceneTransitionButton,
				 verticalConfigureOutputsButton});

	auto *verticalLayout = new QHBoxLayout(verticalModePage);
	verticalLayout->setContentsMargins(18, 18, 18, 18);
	verticalLayout->setSpacing(16);

	auto makeVerticalPanel = [](QWidget *parent, const QString &title, QLabel **titleLabel = nullptr) {
		auto *panel = new QFrame(parent);
		panel->setObjectName(QStringLiteral("switcherVerticalPanel"));
		auto *panelLayout = new QVBoxLayout(panel);
		panelLayout->setContentsMargins(12, 12, 12, 12);
		panelLayout->setSpacing(10);
		if (!title.isEmpty()) {
			auto *label = new QLabel(title, panel);
			label->setObjectName(QStringLiteral("switcherVerticalPanelTitle"));
			panelLayout->addWidget(label);
			if (titleLabel)
				*titleLabel = label;
		} else if (titleLabel) {
			*titleLabel = nullptr;
		}
		return std::pair<QFrame *, QVBoxLayout *>(panel, panelLayout);
	};

	auto *verticalPreviewColumn = new QWidget(verticalModePage);
	auto *verticalPreviewColumnLayout = new QVBoxLayout(verticalPreviewColumn);
	verticalPreviewColumnLayout->setContentsMargins(0, 0, 0, 0);
	verticalPreviewColumnLayout->setSpacing(20);

	auto [previewCard, previewCardLayout] = makeVerticalPanel(verticalPreviewColumn, QStringLiteral("Vertical Canvas"));
	verticalCanvasStatusLabel->setObjectName(QStringLiteral("switcherVerticalPreviewStatus"));
	verticalCanvasPreview->setObjectName(QStringLiteral("switcherVerticalCanvasPreview"));
	verticalCanvasPreview->setMinimumWidth(360);
	previewCard->setObjectName(QStringLiteral("switcherVerticalPreviewCard"));
	previewCardLayout->addWidget(verticalCanvasStatusLabel);
	previewCardLayout->addWidget(verticalCanvasPreview, 1);

	for (auto *button : {verticalSceneDockButton, verticalScenesDockButton, verticalSourcesDockButton,
			     verticalTransitionsDockButton, verticalSettingsDockButton, verticalSceneOpenWindowButton,
			     verticalSceneOpenProjectorButton, verticalRecordButton, verticalPauseRecordButton,
			     verticalSplitRecordButton, verticalChapterButton, verticalStreamButton, verticalReplayButton,
			     verticalReplaySaveButton, verticalVirtualCamButton}) {
		button->setObjectName(QStringLiteral("switcherVerticalToolbarButton"));
		button->setMinimumHeight(34);
	}

	auto *verticalWindowGrid = new QGridLayout;
	verticalWindowGrid->setContentsMargins(0, 0, 0, 0);
	verticalWindowGrid->setHorizontalSpacing(8);
	verticalWindowGrid->setVerticalSpacing(8);
	verticalWindowGrid->addWidget(verticalSceneDockButton, 0, 0);
	verticalWindowGrid->addWidget(verticalSceneOpenWindowButton, 0, 1);
	verticalWindowGrid->addWidget(verticalSceneOpenProjectorButton, 0, 2);
	verticalWindowGrid->addWidget(verticalScenesDockButton, 1, 0);
	verticalWindowGrid->addWidget(verticalSourcesDockButton, 1, 1);
	verticalWindowGrid->addWidget(verticalTransitionsDockButton, 1, 2);
	verticalWindowGrid->addWidget(verticalSettingsDockButton, 2, 0, 1, 3);
	previewCardLayout->addLayout(verticalWindowGrid);

	auto [outputCard, outputCardLayout] = makeVerticalPanel(verticalPreviewColumn, QString());
	outputCardLayout->setContentsMargins(12, 12, 12, 12);
	outputCard->setMinimumHeight(240);
	verticalOutputStatusLabel->setMinimumHeight(34);

	auto *verticalOutputGrid = new QGridLayout;
	verticalOutputGrid->setContentsMargins(0, 0, 0, 0);
	verticalOutputGrid->setHorizontalSpacing(8);
	verticalOutputGrid->setVerticalSpacing(8);
	verticalOutputGrid->addWidget(verticalRecordButton, 0, 0);
	verticalOutputGrid->addWidget(verticalPauseRecordButton, 0, 1);
	verticalOutputGrid->addWidget(verticalSplitRecordButton, 1, 0);
	verticalOutputGrid->addWidget(verticalChapterButton, 1, 1);
	verticalOutputGrid->addWidget(verticalStreamButton, 2, 0);
	verticalOutputGrid->addWidget(verticalReplayButton, 2, 1);
	verticalOutputGrid->addWidget(verticalReplaySaveButton, 3, 0);
	verticalOutputGrid->addWidget(verticalVirtualCamButton, 3, 1);
	outputCardLayout->addLayout(verticalOutputGrid);
	outputCardLayout->addWidget(verticalOutputStatusLabel);
	verticalPreviewColumnLayout->addWidget(previewCard, 1);

	auto *verticalRail = new QWidget(verticalModePage);
	verticalRail->setMinimumWidth(360);
	auto *verticalRailLayout = new QVBoxLayout(verticalRail);
	verticalRailLayout->setContentsMargins(0, 0, 0, 0);
	verticalRailLayout->setSpacing(12);
	verticalRailLayout->addWidget(outputCard, 0);
	verticalSurfaceTabs->setObjectName(QStringLiteral("switcherVerticalSurfaceTabs"));

	auto *scenesTab = new QWidget(verticalSurfaceTabs);
	auto *scenesTabLayout = new QVBoxLayout(scenesTab);
	scenesTabLayout->setContentsMargins(0, 0, 0, 0);
	scenesTabLayout->setSpacing(12);
	auto *verticalSceneButtons = new QHBoxLayout;
	verticalSceneButtons->setContentsMargins(0, 0, 0, 0);
	verticalSceneButtons->setSpacing(8);
	verticalSceneButtons->addWidget(verticalSceneAddButton);
	verticalSceneButtons->addWidget(verticalSceneRemoveButton);
	verticalSceneButtons->addWidget(verticalSceneMenuButton);
	scenesTabLayout->addLayout(verticalSceneButtons);
	scenesTabLayout->addWidget(verticalSceneList, 1);
	auto *linkedScenesGroup = new QGroupBox(QStringLiteral("Linked Scenes"), scenesTab);
	auto *linkedScenesLayout = new QVBoxLayout(linkedScenesGroup);
	linkedScenesLayout->setContentsMargins(12, 12, 12, 12);
	linkedScenesLayout->setSpacing(8);
	linkedScenesLayout->addWidget(verticalLinkCurrentSceneButton);
	linkedScenesLayout->addWidget(verticalSceneLinksList, 1);
	linkedScenesLayout->addWidget(verticalClearLinkButton);
	scenesTabLayout->addWidget(linkedScenesGroup, 0);
	verticalSurfaceTabs->addTab(scenesTab, QStringLiteral("Scenes"));

	auto *sourcesTab = new QWidget(verticalSurfaceTabs);
	auto *sourcesTabLayout = new QVBoxLayout(sourcesTab);
	sourcesTabLayout->setContentsMargins(0, 0, 0, 0);
	sourcesTabLayout->setSpacing(12);
	sourcesTabLayout->addWidget(verticalSourceTree, 1);
	auto *verticalSourceButtons = new QHBoxLayout;
	verticalSourceButtons->setContentsMargins(0, 0, 0, 0);
	verticalSourceButtons->setSpacing(8);
	verticalSourceButtons->addWidget(verticalSourcePropertiesButton);
	verticalSourceButtons->addWidget(verticalSourceFiltersButton);
	verticalSourceButtons->addWidget(verticalSourceMenuButton);
	sourcesTabLayout->addLayout(verticalSourceButtons);
	verticalSurfaceTabs->addTab(sourcesTab, QStringLiteral("Sources"));

	auto *transitionsTab = new QWidget(verticalSurfaceTabs);
	auto *transitionsTabLayout = new QVBoxLayout(transitionsTab);
	transitionsTabLayout->setContentsMargins(0, 0, 0, 0);
	transitionsTabLayout->setSpacing(12);
	auto *verticalCanvasForm = new QFormLayout;
	ConfigureFormLayout(verticalCanvasForm);
	verticalCanvasForm->addRow(QStringLiteral("Name"), verticalCanvasNameEdit);
	verticalCanvasForm->addRow(QStringLiteral("Preset"), verticalPresetCombo);
	verticalCanvasForm->addRow(QStringLiteral("Default Transition"), verticalTransitionCombo);
	verticalCanvasForm->addRow(QStringLiteral("Default Duration"), verticalTransitionDurationSpin);
	transitionsTabLayout->addLayout(verticalCanvasForm);
	transitionsTabLayout->addWidget(verticalLinkedSyncCheckBox);
	auto *sceneOverrideGroup = new QGroupBox(QStringLiteral("Selected Scene Override"), transitionsTab);
	auto *sceneOverrideLayout = new QFormLayout(sceneOverrideGroup);
	ConfigureFormLayout(sceneOverrideLayout);
	sceneOverrideLayout->addRow(QStringLiteral("Transition"), verticalSceneOverrideCombo);
	sceneOverrideLayout->addRow(QStringLiteral("Duration"), verticalSceneOverrideDurationSpin);
	auto *overrideButtonsWidget = new QWidget(sceneOverrideGroup);
	auto *overrideButtons = new QHBoxLayout;
	overrideButtons->setContentsMargins(0, 0, 0, 0);
	overrideButtons->setSpacing(8);
	overrideButtons->addWidget(verticalApplySceneTransitionButton);
	overrideButtons->addWidget(verticalClearSceneTransitionButton);
	overrideButtonsWidget->setLayout(overrideButtons);
	sceneOverrideLayout->addRow(QString(), overrideButtonsWidget);
	transitionsTabLayout->addWidget(sceneOverrideGroup);
	transitionsTabLayout->addStretch(1);
	verticalSurfaceTabs->addTab(transitionsTab, QStringLiteral("Transitions"));

	auto *settingsTab = new QWidget(verticalSurfaceTabs);
	auto *settingsTabLayout = new QVBoxLayout(settingsTab);
	settingsTabLayout->setContentsMargins(0, 0, 0, 0);
	settingsTabLayout->setSpacing(12);
	verticalOutputSettingsSummaryLabel->setWordWrap(true);
	verticalOutputSettingsSummaryLabel->setTextFormat(Qt::RichText);
	auto *settingsIntro = new QLabel(
		QStringLiteral("Switch saves a vertical output profile here. Current shortcut buttons still control OBS main outputs until the isolated vertical output backend is enabled."),
		settingsTab);
	settingsIntro->setWordWrap(true);
	settingsTabLayout->addWidget(settingsIntro);
	settingsTabLayout->addWidget(verticalOutputSettingsSummaryLabel);
	settingsTabLayout->addWidget(verticalConfigureOutputsButton);
	settingsTabLayout->addStretch(1);
	verticalSurfaceTabs->addTab(settingsTab, QStringLiteral("Settings"));

	for (auto *button : {verticalSceneAddButton, verticalSceneRemoveButton, verticalSceneMenuButton,
			     verticalSourcePropertiesButton, verticalSourceFiltersButton, verticalSourceMenuButton,
			     verticalClearLinkButton, verticalLinkCurrentSceneButton, verticalConfigureOutputsButton,
			     verticalApplySceneTransitionButton, verticalClearSceneTransitionButton})
		button->setObjectName(QStringLiteral("switcherVerticalToolbarButton"));
	for (auto *button : {verticalSceneAddButton, verticalSceneRemoveButton, verticalSceneMenuButton,
			     verticalSourcePropertiesButton, verticalSourceFiltersButton, verticalSourceMenuButton,
			     verticalClearLinkButton, verticalLinkCurrentSceneButton, verticalConfigureOutputsButton,
			     verticalApplySceneTransitionButton, verticalClearSceneTransitionButton})
		button->setMinimumHeight(34);

	verticalRailLayout->addWidget(verticalSurfaceTabs, 1);

	verticalLayout->addWidget(verticalPreviewColumn, 1);
	verticalLayout->addWidget(verticalRail, 0);

	motionProfileList = new QListWidget(motionModePage);
	motionAddButton = new QPushButton(QStringLiteral("Add Profile"), motionModePage);
	motionDeleteButton = new QPushButton(QStringLiteral("Delete Profile"), motionModePage);
	motionShotList = new QListWidget(motionModePage);
	motionShotAddButton = new QPushButton(QStringLiteral("Add Shot"), motionModePage);
	motionShotDeleteButton = new QPushButton(QStringLiteral("Delete"), motionModePage);
	motionShotDuplicateButton = new QPushButton(QStringLiteral("Duplicate"), motionModePage);
	motionShotBindButton = new QPushButton(QStringLiteral("Bind Item"), motionModePage);
	motionWorkstationStatusLabel = new QLabel(motionModePage);
	motionShotModeHintLabel = new QLabel(motionModePage);
	motionShotNameEdit = new QLineEdit(motionModePage);
	motionShotEnabledCheckBox = new QCheckBox(QStringLiteral("Enable shot"), motionModePage);
	motionShotSceneCombo = new QComboBox(motionModePage);
	motionShotItemCombo = new QComboBox(motionModePage);
	motionShotModeCombo = new QComboBox(motionModePage);
	motionShotPlaybackCombo = new QComboBox(motionModePage);
	motionShotPresetCombo = new QComboBox(motionModePage);
	motionShotDurationSpin = new QSpinBox(motionModePage);
	motionShotDurationSlider = new QSlider(Qt::Horizontal, motionModePage);
	motionShotEasingCombo = new QComboBox(motionModePage);
	motionShotLoopModeCombo = new QComboBox(motionModePage);
	motionShotStartPanXSpin = new QDoubleSpinBox(motionModePage);
	motionShotStartPanXSlider = new QSlider(Qt::Horizontal, motionModePage);
	motionShotStartPanYSpin = new QDoubleSpinBox(motionModePage);
	motionShotStartPanYSlider = new QSlider(Qt::Horizontal, motionModePage);
	motionShotStartZoomSpin = new QDoubleSpinBox(motionModePage);
	motionShotStartZoomSlider = new QSlider(Qt::Horizontal, motionModePage);
	motionShotEndPanXSpin = new QDoubleSpinBox(motionModePage);
	motionShotEndPanXSlider = new QSlider(Qt::Horizontal, motionModePage);
	motionShotEndPanYSpin = new QDoubleSpinBox(motionModePage);
	motionShotEndPanYSlider = new QSlider(Qt::Horizontal, motionModePage);
	motionShotEndZoomSpin = new QDoubleSpinBox(motionModePage);
	motionShotEndZoomSlider = new QSlider(Qt::Horizontal, motionModePage);
	motionShotMaxZoomSpin = new QDoubleSpinBox(motionModePage);
	motionShotMaxZoomSlider = new QSlider(Qt::Horizontal, motionModePage);
	motionNameEdit = new QLineEdit(motionModePage);
	motionEnabledCheckBox = new QCheckBox(QStringLiteral("Enable auto-framing"), motionModePage);
	motionConfidenceSpin = new QDoubleSpinBox(motionModePage);
	motionConfidenceSlider = new QSlider(Qt::Horizontal, motionModePage);
	motionMaxZoomSpin = new QDoubleSpinBox(motionModePage);
	motionMaxZoomSlider = new QSlider(Qt::Horizontal, motionModePage);
	motionFramingMarginSpin = new QDoubleSpinBox(motionModePage);
	motionFramingMarginSlider = new QSlider(Qt::Horizontal, motionModePage);
	motionDeadZoneSpin = new QDoubleSpinBox(motionModePage);
	motionDeadZoneSlider = new QSlider(Qt::Horizontal, motionModePage);
	motionSmoothingSpin = new QDoubleSpinBox(motionModePage);
	motionSmoothingSlider = new QSlider(Qt::Horizontal, motionModePage);
	motionHoldSpin = new QSpinBox(motionModePage);
	motionHoldSlider = new QSlider(Qt::Horizontal, motionModePage);
	motionBackendCombo = new QComboBox(motionModePage);
	motionSubjectModeCombo = new QComboBox(motionModePage);
	motionFramingModeCombo = new QComboBox(motionModePage);
	motionPresetCombo = new QComboBox(motionModePage);
	motionTrackerHighSpin = new QDoubleSpinBox(motionModePage);
	motionTrackerHighSlider = new QSlider(Qt::Horizontal, motionModePage);
	motionTrackerLowSpin = new QDoubleSpinBox(motionModePage);
	motionTrackerLowSlider = new QSlider(Qt::Horizontal, motionModePage);
	motionNewTrackSpin = new QDoubleSpinBox(motionModePage);
	motionNewTrackSlider = new QSlider(Qt::Horizontal, motionModePage);
	motionTrackBufferSpin = new QSpinBox(motionModePage);
	motionAutoSwitchSpin = new QSpinBox(motionModePage);
	motionAutoSwitchSlider = new QSlider(Qt::Horizontal, motionModePage);
	motionPanResponsivenessSpin = new QDoubleSpinBox(motionModePage);
	motionPanResponsivenessSlider = new QSlider(Qt::Horizontal, motionModePage);
	motionTiltResponsivenessSpin = new QDoubleSpinBox(motionModePage);
	motionTiltResponsivenessSlider = new QSlider(Qt::Horizontal, motionModePage);
	motionZoomResponsivenessSpin = new QDoubleSpinBox(motionModePage);
	motionZoomResponsivenessSlider = new QSlider(Qt::Horizontal, motionModePage);
	motionMaxPanSpeedSpin = new QDoubleSpinBox(motionModePage);
	motionMaxPanSpeedSlider = new QSlider(Qt::Horizontal, motionModePage);
	motionMaxTiltSpeedSpin = new QDoubleSpinBox(motionModePage);
	motionMaxTiltSpeedSlider = new QSlider(Qt::Horizontal, motionModePage);
	motionMaxZoomSpeedSpin = new QDoubleSpinBox(motionModePage);
	motionMaxZoomSpeedSlider = new QSlider(Qt::Horizontal, motionModePage);
	motionDebugOverlayCheckBox = new QCheckBox(QStringLiteral("Debug overlay"), motionModePage);
	motionModelPathEdit = new QLineEdit(motionModePage);
	motionSourceCombo = new QComboBox(motionModePage);
	motionBindButton = new QPushButton(QStringLiteral("Bind Source"), motionModePage);
	motionUnbindButton = new QPushButton(QStringLiteral("Unbind Selected"), motionModePage);
	motionBindingList = new QListWidget(motionModePage);
	motionRuntimeStatusLabel = new QLabel(motionModePage);
	motionTargetStatusLabel = new QLabel(motionModePage);
	motionTrackList = new QListWidget(motionModePage);
	motionScenePreview = new SwitcherWorkspacePreview(motionModePage);
	motionLockCurrentButton = new QPushButton(QStringLiteral("Lock"), motionModePage);
	motionCyclePreviousButton = new QPushButton(QStringLiteral("Prev"), motionModePage);
	motionCycleNextButton = new QPushButton(QStringLiteral("Next"), motionModePage);
	motionClearLockButton = new QPushButton(QStringLiteral("Auto"), motionModePage);
	motionProfileList->setAlternatingRowColors(true);
	motionShotList->setAlternatingRowColors(true);
	motionBindingList->setAlternatingRowColors(true);
	motionTrackList->setAlternatingRowColors(true);
	motionShotSceneCombo->setToolTip(QStringLiteral("Choose the OBS scene this Motion shot controls."));
	motionShotItemCombo->setToolTip(QStringLiteral("Choose the exact scene item Motion should move. This is the normal workstation binding."));
	motionShotBindButton->setToolTip(QStringLiteral("Bind the selected scene item to this Motion shot."));
	motionShotPresetCombo->setToolTip(QStringLiteral("Apply an editable two-point camera move."));
	motionShotStartPanXSpin->setToolTip(QStringLiteral("Horizontal position at the start of the loop."));
	motionShotStartPanYSpin->setToolTip(QStringLiteral("Vertical position at the start of the loop."));
	motionShotStartZoomSpin->setToolTip(QStringLiteral("Zoom at the start of the loop."));
	motionShotEndPanXSpin->setToolTip(QStringLiteral("Horizontal position at the end of the loop."));
	motionShotEndPanYSpin->setToolTip(QStringLiteral("Vertical position at the end of the loop."));
	motionShotEndZoomSpin->setToolTip(QStringLiteral("Zoom at the end of the loop."));
	motionShotMaxZoomSpin->setToolTip(QStringLiteral("Highest zoom Motion may use while clamping away black edges."));
	motionTrackList->setToolTip(QStringLiteral("Click a visible track to lock it as the Motion target."));
	motionTrackList->setMinimumHeight(0);
	motionTrackList->setMaximumHeight(76);
	motionTrackList->setVisible(false);
	motionLockCurrentButton->setToolTip(QStringLiteral("Lock the current tracked subject."));
	motionCyclePreviousButton->setToolTip(QStringLiteral("Lock the previous visible track."));
	motionCycleNextButton->setToolTip(QStringLiteral("Lock the next visible track."));
	motionClearLockButton->setToolTip(QStringLiteral("Clear the lock and return to automatic target selection."));
	motionRuntimeStatusLabel->setWordWrap(true);
	motionRuntimeStatusLabel->setTextFormat(Qt::RichText);
	motionTargetStatusLabel->setWordWrap(true);
	motionTargetStatusLabel->setTextFormat(Qt::RichText);
	motionScenePreview->setObjectName(QStringLiteral("switcherMotionScenePreview"));
	motionScenePreview->setMinimumHeight(180);
	motionScenePreview->setMaximumHeight(220);
	motionScenePreview->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	motionWorkstationStatusLabel->setWordWrap(true);
	motionWorkstationStatusLabel->setTextFormat(Qt::RichText);
	motionWorkstationStatusLabel->setObjectName(QStringLiteral("switcherSettingsHint"));
	motionShotModeHintLabel->setWordWrap(true);
	motionShotModeHintLabel->setTextFormat(Qt::PlainText);
	motionShotModeHintLabel->setObjectName(QStringLiteral("switcherSettingsHint"));
	motionModelPathEdit->setReadOnly(true);
	motionShotModeCombo->addItem(QStringLiteral("AI Auto Frame"), QStringLiteral("ai_auto_frame"));
	motionShotModeCombo->addItem(QStringLiteral("Keyframe Loop"), QStringLiteral("keyframe_loop"));
	motionShotModeCombo->addItem(QStringLiteral("Hybrid"), QStringLiteral("hybrid"));
	motionShotPlaybackCombo->addItem(QStringLiteral("Free Run"), QStringLiteral("free_run"));
	motionShotPlaybackCombo->addItem(QStringLiteral("Restart On Program"), QStringLiteral("restart_on_program"));
	motionShotPlaybackCombo->addItem(QStringLiteral("Pause When Hidden"), QStringLiteral("pause_when_hidden"));
	motionShotPlaybackCombo->addItem(QStringLiteral("Cue In Preview"), QStringLiteral("cue_in_preview"));
	motionShotPresetCombo->addItem(QStringLiteral("Custom"), QStringLiteral("custom"));
	for (const auto &preset : SwitchMotionShotPresets())
		motionShotPresetCombo->addItem(preset.name, preset.id);
	motionShotEasingCombo->addItem(QStringLiteral("Smoothstep"), QStringLiteral("smoothstep"));
	motionShotEasingCombo->addItem(QStringLiteral("Ease In Out"), QStringLiteral("ease_in_out"));
	motionShotEasingCombo->addItem(QStringLiteral("Linear"), QStringLiteral("linear"));
	motionShotLoopModeCombo->addItem(QStringLiteral("Ping Pong"), QStringLiteral("ping_pong"));
	motionShotLoopModeCombo->addItem(QStringLiteral("Restart"), QStringLiteral("restart"));
	motionShotDurationSpin->setRange(250, 120000);
	motionShotDurationSpin->setSingleStep(250);
	motionShotDurationSpin->setSuffix(QStringLiteral(" ms"));
	for (auto *spin : {motionShotStartPanXSpin, motionShotStartPanYSpin, motionShotEndPanXSpin, motionShotEndPanYSpin}) {
		spin->setRange(-0.5, 0.5);
		spin->setSingleStep(0.005);
		spin->setDecimals(3);
	}
	for (auto *spin : {motionShotStartZoomSpin, motionShotEndZoomSpin, motionShotMaxZoomSpin}) {
		spin->setRange(1.0, 4.0);
		spin->setSingleStep(0.05);
		spin->setDecimals(2);
	}

	motionConfidenceSpin->setRange(0.01, 0.99);
	motionConfidenceSpin->setSingleStep(0.01);
	motionConfidenceSpin->setDecimals(2);
	motionMaxZoomSpin->setRange(1.0, 4.0);
	motionMaxZoomSpin->setSingleStep(0.05);
	motionMaxZoomSpin->setDecimals(2);
	motionFramingMarginSpin->setRange(0.0, 0.75);
	motionFramingMarginSpin->setSingleStep(0.01);
	motionFramingMarginSpin->setDecimals(2);
	motionDeadZoneSpin->setRange(0.0, 0.5);
	motionDeadZoneSpin->setSingleStep(0.01);
	motionDeadZoneSpin->setDecimals(2);
	motionSmoothingSpin->setRange(0.01, 1.0);
	motionSmoothingSpin->setSingleStep(0.01);
	motionSmoothingSpin->setDecimals(2);
	motionHoldSpin->setRange(0, 5000);
	motionHoldSpin->setSingleStep(50);
	motionHoldSpin->setSuffix(QStringLiteral(" ms"));
	for (auto *spin : {motionTrackerHighSpin, motionTrackerLowSpin, motionNewTrackSpin}) {
		spin->setRange(0.01, 0.99);
		spin->setSingleStep(0.01);
		spin->setDecimals(2);
	}
	motionTrackBufferSpin->setRange(1, 180);
	motionTrackBufferSpin->setSingleStep(1);
	motionAutoSwitchSpin->setRange(0, 3000);
	motionAutoSwitchSpin->setSingleStep(50);
	motionAutoSwitchSpin->setSuffix(QStringLiteral(" ms"));
	for (auto *spin : {motionPanResponsivenessSpin, motionTiltResponsivenessSpin, motionZoomResponsivenessSpin}) {
		spin->setRange(0.05, 1.5);
		spin->setSingleStep(0.01);
		spin->setDecimals(2);
	}
	for (auto *spin : {motionMaxPanSpeedSpin, motionMaxTiltSpeedSpin, motionMaxZoomSpeedSpin}) {
		spin->setRange(0.05, 4.0);
		spin->setSingleStep(0.01);
		spin->setDecimals(2);
	}
	motionBackendCombo->addItem(QStringLiteral("Auto"), QStringLiteral("auto"));
#ifdef __APPLE__
	motionBackendCombo->addItem(QStringLiteral("CoreML"), QStringLiteral("coreml"));
#elif defined(_WIN32)
	motionBackendCombo->addItem(QStringLiteral("DirectML"), QStringLiteral("directml"));
#endif
	motionBackendCombo->addItem(QStringLiteral("CPU Diagnostics"), QStringLiteral("cpu"));
	motionSubjectModeCombo->addItem(QStringLiteral("Auto"), QStringLiteral("auto"));
	motionSubjectModeCombo->addItem(QStringLiteral("Locked"), QStringLiteral("locked"));
	motionSubjectModeCombo->addItem(QStringLiteral("Hold"), QStringLiteral("hold"));
	motionSubjectModeCombo->addItem(QStringLiteral("Off"), QStringLiteral("off"));
	motionFramingModeCombo->addItem(QStringLiteral("Face / Headroom"), QStringLiteral("face_headroom"));
	motionFramingModeCombo->addItem(QStringLiteral("Upper Body"), QStringLiteral("upper_body"));
	motionFramingModeCombo->addItem(QStringLiteral("Full Body"), QStringLiteral("full_body"));
	motionFramingModeCombo->addItem(QStringLiteral("Group"), QStringLiteral("group"));
	motionPresetCombo->addItem(QStringLiteral("Gimbal Smooth"), QStringLiteral("gimbal"));
	motionPresetCombo->addItem(QStringLiteral("Subtle"), QStringLiteral("subtle"));
	motionPresetCombo->addItem(QStringLiteral("Presenter"), QStringLiteral("presenter"));
	motionPresetCombo->addItem(QStringLiteral("Tight Face"), QStringLiteral("tight_face"));
	motionPresetCombo->addItem(QStringLiteral("Wide Room"), QStringLiteral("wide_room"));
	motionPresetCombo->addItem(QStringLiteral("Fast Follow"), QStringLiteral("fast_follow"));
	SetMinimumControlHeight({motionAddButton, motionDeleteButton, motionNameEdit, motionConfidenceSpin,
				 motionShotAddButton, motionShotDeleteButton, motionShotDuplicateButton,
				 motionShotBindButton, motionShotNameEdit, motionShotSceneCombo, motionShotItemCombo,
				 motionShotModeCombo, motionShotPlaybackCombo, motionShotPresetCombo,
				 motionShotDurationSpin, motionShotEasingCombo, motionShotLoopModeCombo,
				 motionShotStartPanXSpin, motionShotStartPanYSpin, motionShotStartZoomSpin,
				 motionShotEndPanXSpin, motionShotEndPanYSpin, motionShotEndZoomSpin,
				 motionShotMaxZoomSpin,
				 motionMaxZoomSpin, motionFramingMarginSpin, motionDeadZoneSpin, motionSmoothingSpin,
				 motionHoldSpin, motionBackendCombo, motionSubjectModeCombo, motionFramingModeCombo,
				 motionPresetCombo, motionTrackerHighSpin, motionTrackerLowSpin, motionNewTrackSpin,
				 motionTrackBufferSpin, motionAutoSwitchSpin, motionPanResponsivenessSpin,
				 motionTiltResponsivenessSpin, motionZoomResponsivenessSpin, motionMaxPanSpeedSpin,
				 motionMaxTiltSpeedSpin, motionMaxZoomSpeedSpin, motionModelPathEdit, motionSourceCombo,
				 motionBindButton, motionUnbindButton, motionLockCurrentButton, motionCyclePreviousButton,
				 motionCycleNextButton, motionClearLockButton});
	SetMinimumControlWidth({motionConfidenceSpin, motionMaxZoomSpin, motionFramingMarginSpin,
				motionDeadZoneSpin, motionSmoothingSpin, motionHoldSpin,
				motionShotDurationSpin, motionShotStartPanXSpin, motionShotStartPanYSpin,
				motionShotStartZoomSpin, motionShotEndPanXSpin, motionShotEndPanYSpin,
				motionShotEndZoomSpin, motionShotMaxZoomSpin,
				motionTrackerHighSpin, motionTrackerLowSpin, motionNewTrackSpin,
				motionTrackBufferSpin, motionAutoSwitchSpin, motionPanResponsivenessSpin,
				motionTiltResponsivenessSpin, motionZoomResponsivenessSpin, motionMaxPanSpeedSpin,
				motionMaxTiltSpeedSpin, motionMaxZoomSpeedSpin},
			       128);
	SetMinimumControlWidth({motionBackendCombo, motionSubjectModeCombo, motionFramingModeCombo,
				motionPresetCombo, motionShotSceneCombo, motionShotItemCombo,
				motionShotModeCombo, motionShotPlaybackCombo, motionShotPresetCombo,
				motionShotEasingCombo, motionShotLoopModeCombo},
			       180);

	auto *motionLayout = new QHBoxLayout(motionModePage);
	motionLayout->setContentsMargins(18, 18, 18, 18);
	motionLayout->setSpacing(16);
	auto *motionSplitter = new QSplitter(Qt::Horizontal, motionModePage);
	motionSplitter->setChildrenCollapsible(false);
	motionSplitter->setHandleWidth(1);
	motionSplitter->setOpaqueResize(false);
	motionLayout->addWidget(motionSplitter, 1);

	auto *motionShotsPane = new QWidget(motionSplitter);
	motionShotsPane->setObjectName(QStringLiteral("switcherSettingsPane"));
	motionShotsPane->setAttribute(Qt::WA_StyledBackground, true);
	motionShotsPane->setMinimumWidth(280);
	auto *motionShotsLayout = new QVBoxLayout(motionShotsPane);
	motionShotsLayout->setContentsMargins(12, 12, 12, 12);
	motionShotsLayout->setSpacing(10);
	auto *motionShotsTitle = new QLabel(QStringLiteral("Shots"), motionShotsPane);
	motionShotsTitle->setObjectName(QStringLiteral("switcherWorkspaceSettingsSectionTitle"));
	motionShotsLayout->addWidget(motionShotsTitle);
	motionShotsLayout->addWidget(motionShotList, 1);
	auto *motionShotButtons = new QHBoxLayout;
	motionShotButtons->setContentsMargins(0, 0, 0, 0);
	motionShotButtons->setSpacing(8);
	motionShotButtons->addWidget(motionShotAddButton);
	motionShotButtons->addWidget(motionShotDeleteButton);
	motionShotsLayout->addLayout(motionShotButtons);
	auto *motionShotMoreButtons = new QHBoxLayout;
	motionShotMoreButtons->setContentsMargins(0, 0, 0, 0);
	motionShotMoreButtons->setSpacing(8);
	motionShotMoreButtons->addWidget(motionShotDuplicateButton);
	motionShotMoreButtons->addWidget(motionShotBindButton);
	motionShotsLayout->addLayout(motionShotMoreButtons);

	auto *motionEditorPane = new QWidget(motionSplitter);
	motionEditorPane->setObjectName(QStringLiteral("switcherSettingsPane"));
	motionEditorPane->setAttribute(Qt::WA_StyledBackground, true);
	motionEditorPane->setMinimumWidth(540);
	auto *motionEditorLayout = new QVBoxLayout(motionEditorPane);
	motionEditorLayout->setContentsMargins(12, 12, 12, 12);
	motionEditorLayout->setSpacing(10);
	auto *motionEditorTitle = new QLabel(QStringLiteral("Motion Workstation"), motionEditorPane);
	motionEditorTitle->setObjectName(QStringLiteral("switcherWorkspaceSettingsSectionTitle"));
	motionEditorLayout->addWidget(motionEditorTitle);
	motionEditorLayout->addWidget(motionWorkstationStatusLabel);

	const auto makeMotionSection = [](QWidget *parent, const QString &title) {
		auto *section = new QFrame(parent);
		section->setObjectName(QStringLiteral("switcherSettingsSection"));
		section->setAttribute(Qt::WA_StyledBackground, true);
		auto *sectionLayout = new QVBoxLayout(section);
		sectionLayout->setContentsMargins(12, 12, 12, 12);
		sectionLayout->setSpacing(10);
		auto *sectionTitle = new QLabel(title, section);
		sectionTitle->setObjectName(QStringLiteral("switcherWorkspaceSettingsSectionTitle"));
		sectionLayout->addWidget(sectionTitle);
		return std::pair<QFrame *, QVBoxLayout *>(section, sectionLayout);
	};
	auto *motionPreviewTargetStrip = new QWidget(motionEditorPane);
	auto *motionPreviewTargetLayout = new QHBoxLayout(motionPreviewTargetStrip);
	motionPreviewTargetLayout->setContentsMargins(0, 0, 0, 0);
	motionPreviewTargetLayout->setSpacing(12);
	auto [motionPreviewSection, motionPreviewSectionLayout] =
		makeMotionSection(motionPreviewTargetStrip, QStringLiteral("Live Scene Preview"));
	motionPreviewSectionLayout->addWidget(motionScenePreview);
	motionPreviewTargetLayout->addWidget(motionPreviewSection, 3);
	auto [motionLiveSection, motionLiveSectionLayout] =
		makeMotionSection(motionPreviewTargetStrip, QStringLiteral("Target"));
	motionLiveSection->setMinimumWidth(210);
	motionLiveSectionLayout->addWidget(motionTargetStatusLabel);
	auto *motionTargetButtons = new QGridLayout;
	motionTargetButtons->setContentsMargins(0, 0, 0, 0);
	motionTargetButtons->setSpacing(8);
	motionTargetButtons->addWidget(motionLockCurrentButton, 0, 0);
	motionTargetButtons->addWidget(motionClearLockButton, 0, 1);
	motionTargetButtons->addWidget(motionCyclePreviousButton, 1, 0);
	motionTargetButtons->addWidget(motionCycleNextButton, 1, 1);
	motionLiveSectionLayout->addLayout(motionTargetButtons);
	motionLiveSectionLayout->addWidget(motionTrackList);
	motionLiveSectionLayout->addStretch(1);
	motionPreviewTargetLayout->addWidget(motionLiveSection, 2);
	motionEditorLayout->addWidget(motionPreviewTargetStrip);

	auto *motionControlsScroll = new QScrollArea(motionEditorPane);
	motionControlsScroll->setObjectName(QStringLiteral("switcherSettingsScrollArea"));
	motionControlsScroll->setFrameShape(QFrame::NoFrame);
	motionControlsScroll->setWidgetResizable(true);
	auto *motionControlsPane = new QWidget(motionControlsScroll);
	motionControlsPane->setObjectName(QStringLiteral("switcherSettingsPane"));
	motionControlsPane->setAttribute(Qt::WA_StyledBackground, true);
	auto *motionControlsLayout = new QVBoxLayout(motionControlsPane);
	motionControlsLayout->setContentsMargins(0, 0, 0, 0);
	motionControlsLayout->setSpacing(12);
	const auto configureSlider = [](QSlider *slider) {
		slider->setObjectName(QStringLiteral("switcherMotionSlider"));
		slider->setMinimumWidth(170);
		slider->setCursor(Qt::PointingHandCursor);
	};
	const auto makeDoubleSliderRow = [this, configureSlider](QWidget *parent, QDoubleSpinBox *spin, QSlider *slider) {
		configureSlider(slider);
		slider->setRange(0, 1000);
		slider->setSingleStep(1);
		slider->setPageStep(50);
		spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
		spin->setMinimumWidth(88);
		spin->setMaximumWidth(110);
		const auto spinToSlider = [spin, slider]() {
			const double span = spin->maximum() - spin->minimum();
			const double normalized = span <= 0.0 ? 0.0 : (spin->value() - spin->minimum()) / span;
			QSignalBlocker blocker(slider);
			slider->setValue(static_cast<int>(std::lround(normalized * 1000.0)));
		};
		const auto sliderToSpin = [spin, slider]() {
			const double normalized = static_cast<double>(slider->value()) / 1000.0;
			const double value = spin->minimum() + (spin->maximum() - spin->minimum()) * normalized;
			spin->setValue(value);
		};
		QObject::connect(slider, &QSlider::valueChanged, this, sliderToSpin);
		QObject::connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, spinToSlider);
		spinToSlider();
		auto *row = new QWidget(parent);
		auto *layout = new QHBoxLayout(row);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->setSpacing(10);
		layout->addWidget(slider, 1);
		layout->addWidget(spin);
		return row;
	};
	const auto makeIntSliderRow = [this, configureSlider](QWidget *parent, QSpinBox *spin, QSlider *slider) {
		configureSlider(slider);
		slider->setRange(spin->minimum(), spin->maximum());
		slider->setSingleStep(std::max(1, spin->singleStep()));
		slider->setPageStep(std::max(1, spin->singleStep() * 10));
		spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
		spin->setMinimumWidth(96);
		spin->setMaximumWidth(120);
		const auto spinToSlider = [spin, slider]() {
			QSignalBlocker blocker(slider);
			slider->setValue(spin->value());
		};
		const auto sliderToSpin = [spin, slider]() {
			spin->setValue(slider->value());
		};
		QObject::connect(slider, &QSlider::valueChanged, this, sliderToSpin);
		QObject::connect(spin, qOverload<int>(&QSpinBox::valueChanged), this, spinToSlider);
		spinToSlider();
		auto *row = new QWidget(parent);
		auto *layout = new QHBoxLayout(row);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->setSpacing(10);
		layout->addWidget(slider, 1);
		layout->addWidget(spin);
		return row;
	};

	auto [motionShotSection, motionShotSectionLayout] =
		makeMotionSection(motionControlsPane, QStringLiteral("Shot Setup"));
	auto *motionShotForm = new QFormLayout;
	ConfigureFormLayout(motionShotForm);
	motionShotSectionLayout->addWidget(motionShotModeHintLabel);
	motionShotForm->addRow(QStringLiteral("Name"), motionShotNameEdit);
	motionShotForm->addRow(QStringLiteral("Scene"), motionShotSceneCombo);
	motionShotForm->addRow(QStringLiteral("Scene Item"), motionShotItemCombo);
	motionShotForm->addRow(QStringLiteral("Mode"), motionShotModeCombo);
	motionShotForm->addRow(QStringLiteral("Playback"), motionShotPlaybackCombo);
	motionShotSectionLayout->addLayout(motionShotForm);
	motionShotSectionLayout->addWidget(motionShotEnabledCheckBox);
	motionControlsLayout->addWidget(motionShotSection);

	auto [motionPositionSectionFrame, motionPositionSectionLayout] =
		makeMotionSection(motionControlsPane, QStringLiteral("Position And Zoom"));
	motionPositionSection = motionPositionSectionFrame;
	auto *motionPositionForm = new QFormLayout;
	ConfigureFormLayout(motionPositionForm);
	motionPositionForm->addRow(QStringLiteral("Loop Preset"), motionShotPresetCombo);
	motionPositionForm->addRow(QStringLiteral("Duration"), makeIntSliderRow(motionControlsPane, motionShotDurationSpin, motionShotDurationSlider));
	motionPositionForm->addRow(QStringLiteral("Easing"), motionShotEasingCombo);
	motionPositionForm->addRow(QStringLiteral("Loop"), motionShotLoopModeCombo);
	motionPositionForm->addRow(QStringLiteral("Start Pan X"), makeDoubleSliderRow(motionControlsPane, motionShotStartPanXSpin, motionShotStartPanXSlider));
	motionPositionForm->addRow(QStringLiteral("Start Pan Y"), makeDoubleSliderRow(motionControlsPane, motionShotStartPanYSpin, motionShotStartPanYSlider));
	motionPositionForm->addRow(QStringLiteral("Start Zoom"), makeDoubleSliderRow(motionControlsPane, motionShotStartZoomSpin, motionShotStartZoomSlider));
	motionPositionForm->addRow(QStringLiteral("End Pan X"), makeDoubleSliderRow(motionControlsPane, motionShotEndPanXSpin, motionShotEndPanXSlider));
	motionPositionForm->addRow(QStringLiteral("End Pan Y"), makeDoubleSliderRow(motionControlsPane, motionShotEndPanYSpin, motionShotEndPanYSlider));
	motionPositionForm->addRow(QStringLiteral("End Zoom"), makeDoubleSliderRow(motionControlsPane, motionShotEndZoomSpin, motionShotEndZoomSlider));
	motionPositionForm->addRow(QStringLiteral("Max Zoom"), makeDoubleSliderRow(motionControlsPane, motionShotMaxZoomSpin, motionShotMaxZoomSlider));
	motionPositionSectionLayout->addLayout(motionPositionForm);
	motionControlsLayout->addWidget(motionPositionSection);

	auto [motionProfileSectionFrame, motionProfileSectionLayout] =
		makeMotionSection(motionControlsPane, QStringLiteral("AI Engine"));
	motionProfileSection = motionProfileSectionFrame;
	auto *motionProfileForm = new QFormLayout;
	ConfigureFormLayout(motionProfileForm);
	motionProfileForm->addRow(QStringLiteral("Profile"), motionNameEdit);
	motionProfileForm->addRow(QStringLiteral("Motion Preset"), motionPresetCombo);
	motionProfileForm->addRow(QStringLiteral("Backend"), motionBackendCombo);
	motionProfileForm->addRow(QStringLiteral("Model"), motionModelPathEdit);
	motionProfileSectionLayout->addLayout(motionProfileForm);
	motionProfileSectionLayout->addWidget(motionRuntimeStatusLabel);
	motionProfileSectionLayout->addWidget(motionEnabledCheckBox);
	motionProfileSectionLayout->addWidget(motionDebugOverlayCheckBox);

	auto [motionFramingSectionFrame, motionFramingSectionLayout] =
		makeMotionSection(motionControlsPane, QStringLiteral("AI Target And Framing"));
	motionFramingSection = motionFramingSectionFrame;
	auto *motionFramingForm = new QFormLayout;
	ConfigureFormLayout(motionFramingForm);
	motionFramingForm->addRow(QStringLiteral("Subject"), motionSubjectModeCombo);
	motionFramingForm->addRow(QStringLiteral("Framing"), motionFramingModeCombo);
	motionFramingForm->addRow(QStringLiteral("Confidence"), makeDoubleSliderRow(motionControlsPane, motionConfidenceSpin, motionConfidenceSlider));
	motionFramingForm->addRow(QStringLiteral("Max Zoom"), makeDoubleSliderRow(motionControlsPane, motionMaxZoomSpin, motionMaxZoomSlider));
	motionFramingForm->addRow(QStringLiteral("Framing Margin"), makeDoubleSliderRow(motionControlsPane, motionFramingMarginSpin, motionFramingMarginSlider));
	motionFramingForm->addRow(QStringLiteral("Dead Zone"), makeDoubleSliderRow(motionControlsPane, motionDeadZoneSpin, motionDeadZoneSlider));
	motionFramingForm->addRow(QStringLiteral("Lost Hold"), makeIntSliderRow(motionControlsPane, motionHoldSpin, motionHoldSlider));
	motionFramingSectionLayout->addLayout(motionFramingForm);
	motionControlsLayout->addWidget(motionFramingSection);

	auto [motionTrackingSectionFrame, motionTrackingSectionLayout] =
		makeMotionSection(motionControlsPane, QStringLiteral("Advanced Tracking IDs"));
	motionTrackingSection = motionTrackingSectionFrame;
	auto *motionTrackingForm = new QFormLayout;
	ConfigureFormLayout(motionTrackingForm);
	motionTrackingForm->addRow(QStringLiteral("High Track"), makeDoubleSliderRow(motionControlsPane, motionTrackerHighSpin, motionTrackerHighSlider));
	motionTrackingForm->addRow(QStringLiteral("Low Track"), makeDoubleSliderRow(motionControlsPane, motionTrackerLowSpin, motionTrackerLowSlider));
	motionTrackingForm->addRow(QStringLiteral("New Track"), makeDoubleSliderRow(motionControlsPane, motionNewTrackSpin, motionNewTrackSlider));
	motionTrackingForm->addRow(QStringLiteral("Track Buffer"), motionTrackBufferSpin);
	motionTrackingForm->addRow(QStringLiteral("Auto Switch"), makeIntSliderRow(motionControlsPane, motionAutoSwitchSpin, motionAutoSwitchSlider));
	motionTrackingSectionLayout->addLayout(motionTrackingForm);

	auto [motionControllerSectionFrame, motionControllerSectionLayout] =
		makeMotionSection(motionControlsPane, QStringLiteral("Gimbal Motion"));
	motionControllerSection = motionControllerSectionFrame;
	auto *motionControllerForm = new QFormLayout;
	ConfigureFormLayout(motionControllerForm);
	motionControllerForm->addRow(QStringLiteral("Smoothing"), makeDoubleSliderRow(motionControlsPane, motionSmoothingSpin, motionSmoothingSlider));
	motionControllerForm->addRow(QStringLiteral("Pan Response"), makeDoubleSliderRow(motionControlsPane, motionPanResponsivenessSpin, motionPanResponsivenessSlider));
	motionControllerForm->addRow(QStringLiteral("Tilt Response"), makeDoubleSliderRow(motionControlsPane, motionTiltResponsivenessSpin, motionTiltResponsivenessSlider));
	motionControllerForm->addRow(QStringLiteral("Zoom Response"), makeDoubleSliderRow(motionControlsPane, motionZoomResponsivenessSpin, motionZoomResponsivenessSlider));
	motionControllerForm->addRow(QStringLiteral("Max Pan Speed"), makeDoubleSliderRow(motionControlsPane, motionMaxPanSpeedSpin, motionMaxPanSpeedSlider));
	motionControllerForm->addRow(QStringLiteral("Max Tilt Speed"), makeDoubleSliderRow(motionControlsPane, motionMaxTiltSpeedSpin, motionMaxTiltSpeedSlider));
	motionControllerForm->addRow(QStringLiteral("Max Zoom Speed"), makeDoubleSliderRow(motionControlsPane, motionMaxZoomSpeedSpin, motionMaxZoomSpeedSlider));
	motionControllerSectionLayout->addLayout(motionControllerForm);
	motionControlsLayout->addWidget(motionControllerSection);
	motionControlsLayout->addWidget(motionProfileSection);
	motionControlsLayout->addWidget(motionTrackingSection);

	motionControlsLayout->addStretch(1);
	motionControlsScroll->setWidget(motionControlsPane);
	motionEditorLayout->addWidget(motionControlsScroll, 1);

	auto *motionAdvancedPane = new QWidget(motionSplitter);
	motionAdvancedPane->setObjectName(QStringLiteral("switcherSettingsPane"));
	motionAdvancedPane->setAttribute(Qt::WA_StyledBackground, true);
	motionAdvancedPane->setMinimumWidth(280);
	auto *motionAdvancedLayout = new QVBoxLayout(motionAdvancedPane);
	motionAdvancedLayout->setContentsMargins(12, 12, 12, 12);
	motionAdvancedLayout->setSpacing(10);
	auto *motionProfilesTitle = new QLabel(QStringLiteral("AI Profiles"), motionAdvancedPane);
	motionProfilesTitle->setObjectName(QStringLiteral("switcherWorkspaceSettingsSectionTitle"));
	motionAdvancedLayout->addWidget(motionProfilesTitle);
	motionAdvancedLayout->addWidget(motionProfileList, 1);
	auto *motionProfileButtons = new QHBoxLayout;
	motionProfileButtons->setContentsMargins(0, 0, 0, 0);
	motionProfileButtons->setSpacing(8);
	motionProfileButtons->addWidget(motionAddButton);
	motionProfileButtons->addWidget(motionDeleteButton);
	motionAdvancedLayout->addLayout(motionProfileButtons);
	auto *motionBindingTitle = new QLabel(QStringLiteral("Legacy Source Filters"), motionAdvancedPane);
	motionBindingTitle->setObjectName(QStringLiteral("switcherWorkspaceSettingsSectionTitle"));
	auto *motionBindingHint = new QLabel(
		QStringLiteral("Use shots for normal work. Source filters are kept for older scenes and troubleshooting."),
		motionAdvancedPane);
	motionBindingHint->setWordWrap(true);
	motionBindingHint->setObjectName(QStringLiteral("switcherSettingsHint"));
	motionAdvancedLayout->addWidget(motionBindingTitle);
	motionAdvancedLayout->addWidget(motionBindingHint);
	motionAdvancedLayout->addWidget(motionSourceCombo);
	motionAdvancedLayout->addWidget(motionBindButton);
	motionAdvancedLayout->addWidget(motionBindingList, 1);
	motionAdvancedLayout->addWidget(motionUnbindButton);
	motionSplitter->setStretchFactor(0, 0);
	motionSplitter->setStretchFactor(1, 1);
	motionSplitter->setStretchFactor(2, 0);
	motionSplitter->setSizes({300, 760, 300});

	macroList = new QListWidget(automationModePage);
	macroAddButton = new QPushButton(QStringLiteral("Add Macro"), automationModePage);
	macroDeleteButton = new QPushButton(QStringLiteral("Delete Macro"), automationModePage);
	macroRunButton = new QPushButton(QStringLiteral("Run Macro"), automationModePage);
	macroDuplicateButton = new QPushButton(QStringLiteral("Duplicate"), automationModePage);
	macroExportButton = new QPushButton(QStringLiteral("Export Macro"), automationModePage);
	macroImportButton = new QPushButton(QStringLiteral("Import Macro"), automationModePage);
	macroNameEdit = new QLineEdit(automationModePage);
	macroEnabledCheckBox = new QCheckBox(QStringLiteral("Enabled"), automationModePage);
	macroPausedCheckBox = new QCheckBox(QStringLiteral("Paused"), automationModePage);
	macroRunModeCombo = new QComboBox(automationModePage);
	macroTriggerTypeCombo = new QComboBox(automationModePage);
	macroIntervalSpin = new QSpinBox(automationModePage);
	macroTriggerConnectionCombo = new QComboBox(automationModePage);
	macroTriggerSceneCombo = new QComboBox(automationModePage);
	macroTriggerStateCombo = new QComboBox(automationModePage);
	macroTriggerKeyEdit = new QLineEdit(automationModePage);
	macroTriggerValueEdit = new QLineEdit(automationModePage);
	macroActionTypeCombo = new QComboBox(automationModePage);
	macroActionConnectionCombo = new QComboBox(automationModePage);
	macroActionSceneCombo = new QComboBox(automationModePage);
	macroActionDelaySpin = new QSpinBox(automationModePage);
	macroActionKeyEdit = new QLineEdit(automationModePage);
	macroActionValueEdit = new QLineEdit(automationModePage);
	macroLastResultLabel = new QLabel(automationModePage);
	automationSurfaceTabs = new QTabWidget(automationModePage);
	variableList = new QListWidget(automationModePage);
	variableKeyEdit = new QLineEdit(automationModePage);
	variableValueEdit = new QLineEdit(automationModePage);
	variableSetButton = new QPushButton(QStringLiteral("Set Variable"), automationModePage);
	variableRemoveButton = new QPushButton(QStringLiteral("Remove Variable"), automationModePage);
	queueList = new QListWidget(automationModePage);
	queueClearButton = new QPushButton(QStringLiteral("Clear Queue"), automationModePage);
	connectionList = new QListWidget(automationModePage);
	connectionAddButton = new QPushButton(QStringLiteral("Add OSC Connection"), automationModePage);
	connectionRemoveButton = new QPushButton(QStringLiteral("Remove Connection"), automationModePage);
	connectionNameEdit = new QLineEdit(automationModePage);
	connectionModeCombo = new QComboBox(automationModePage);
	connectionRemoteHostEdit = new QLineEdit(automationModePage);
	connectionRemotePortSpin = new QSpinBox(automationModePage);
	connectionListenHostEdit = new QLineEdit(automationModePage);
	connectionListenPortSpin = new QSpinBox(automationModePage);
	connectionDetailsLabel = new QLabel(automationModePage);
	connectionTestButton = new QPushButton(QStringLiteral("Test Connection"), automationModePage);
	eventLogList = new QListWidget(automationModePage);
	automationStatusSummaryLabel = new QLabel(automationModePage);
	automationExportButton = new QPushButton(QStringLiteral("Export Automation"), automationModePage);
	automationImportButton = new QPushButton(QStringLiteral("Import Automation"), automationModePage);
	macroList->setAlternatingRowColors(true);
	variableList->setAlternatingRowColors(true);
	queueList->setAlternatingRowColors(true);
	connectionList->setAlternatingRowColors(true);
	eventLogList->setAlternatingRowColors(true);
	macroLastResultLabel->setWordWrap(true);
	connectionDetailsLabel->setWordWrap(true);
	connectionDetailsLabel->setTextFormat(Qt::RichText);
	automationStatusSummaryLabel->setWordWrap(true);
	automationStatusSummaryLabel->setTextFormat(Qt::RichText);
	macroIntervalSpin->setRange(250, 3600000);
	macroIntervalSpin->setSingleStep(250);
	macroActionDelaySpin->setRange(0, 600000);
	macroActionDelaySpin->setSingleStep(250);
	connectionRemotePortSpin->setRange(1, 65535);
	connectionListenPortSpin->setRange(1, 65535);
	macroRunModeCombo->addItem(QStringLiteral("On Change"), QStringLiteral("on_change"));
	macroRunModeCombo->addItem(QStringLiteral("On Match"), QStringLiteral("on_match"));
	macroRunModeCombo->addItem(QStringLiteral("Repeat"), QStringLiteral("repeat"));
	connectionModeCombo->addItem(QStringLiteral("Send + Receive"), QStringLiteral("duplex"));
	connectionModeCombo->addItem(QStringLiteral("Receive Only"), QStringLiteral("receive"));
	connectionModeCombo->addItem(QStringLiteral("Send Only"), QStringLiteral("send"));
	for (const auto &condition : SwitchConditionFactory::Types())
		macroTriggerTypeCombo->addItem(condition.name, condition.id);
	macroTriggerStateCombo->addItem(QStringLiteral("Active"), true);
	macroTriggerStateCombo->addItem(QStringLiteral("Inactive"), false);
	for (const auto &action : SwitchActionFactory::Types())
		macroActionTypeCombo->addItem(action.name, action.id);
	SetMinimumControlHeight({macroAddButton, macroDeleteButton, macroRunButton, macroDuplicateButton,
				 macroExportButton, macroImportButton, macroNameEdit, macroRunModeCombo,
				 macroTriggerTypeCombo, macroIntervalSpin, macroTriggerConnectionCombo,
				 macroTriggerSceneCombo, macroTriggerStateCombo, macroTriggerKeyEdit,
				 macroTriggerValueEdit, macroActionTypeCombo, macroActionConnectionCombo,
				 macroActionSceneCombo, macroActionDelaySpin, macroActionKeyEdit, macroActionValueEdit,
				 variableKeyEdit, variableValueEdit, variableSetButton, variableRemoveButton,
				 queueClearButton, connectionAddButton, connectionRemoveButton, connectionNameEdit,
				 connectionModeCombo, connectionRemoteHostEdit, connectionRemotePortSpin,
				 connectionListenHostEdit, connectionListenPortSpin, connectionTestButton,
				 automationExportButton, automationImportButton});

	auto *automationLayout = new QHBoxLayout(automationModePage);
	automationLayout->setContentsMargins(18, 18, 18, 18);
	automationLayout->setSpacing(16);
	auto *automationSplitter = new QSplitter(Qt::Horizontal, automationModePage);
	automationSplitter->setChildrenCollapsible(false);
	automationSplitter->setHandleWidth(1);
	automationSplitter->setOpaqueResize(false);
	automationLayout->addWidget(automationSplitter, 1);

	auto *macroListPane = new QWidget(automationSplitter);
	macroListPane->setObjectName(QStringLiteral("switcherSettingsPane"));
	macroListPane->setAttribute(Qt::WA_StyledBackground, true);
	macroListPane->setMinimumWidth(300);
	auto *macroListLayout = new QVBoxLayout(macroListPane);
	macroListLayout->setContentsMargins(12, 12, 12, 12);
	macroListLayout->setSpacing(10);
	auto *macroListTitle = new QLabel(QStringLiteral("Macros"), macroListPane);
	macroListTitle->setObjectName(QStringLiteral("switcherWorkspaceSettingsSectionTitle"));
	macroListLayout->addWidget(macroListTitle);
	macroListLayout->addWidget(macroList, 1);
	auto *macroButtons = new QHBoxLayout;
	macroButtons->setContentsMargins(0, 0, 0, 0);
	macroButtons->setSpacing(8);
	macroButtons->addWidget(macroAddButton);
	macroButtons->addWidget(macroDeleteButton);
	macroButtons->addWidget(macroRunButton);
	macroListLayout->addLayout(macroButtons);
	auto *macroImportExportButtons = new QHBoxLayout;
	macroImportExportButtons->setContentsMargins(0, 0, 0, 0);
	macroImportExportButtons->setSpacing(8);
	macroImportExportButtons->addWidget(macroDuplicateButton);
	macroImportExportButtons->addWidget(macroExportButton);
	macroImportExportButtons->addWidget(macroImportButton);
	macroListLayout->addLayout(macroImportExportButtons);

	auto *macroEditorPane = new QWidget(automationSplitter);
	macroEditorPane->setObjectName(QStringLiteral("switcherSettingsPane"));
	macroEditorPane->setAttribute(Qt::WA_StyledBackground, true);
	macroEditorPane->setMinimumWidth(560);
	auto *macroEditorLayout = new QVBoxLayout(macroEditorPane);
	macroEditorLayout->setContentsMargins(12, 12, 12, 12);
	macroEditorLayout->setSpacing(12);
	auto *macroEditorTitle = new QLabel(QStringLiteral("Editor"), macroEditorPane);
	macroEditorTitle->setObjectName(QStringLiteral("switcherWorkspaceSettingsSectionTitle"));
	macroEditorLayout->addWidget(macroEditorTitle);
	auto *macroEditorForm = new QFormLayout;
	ConfigureFormLayout(macroEditorForm);
	macroEditorForm->addRow(QStringLiteral("Name"), macroNameEdit);
	macroEditorForm->addRow(QStringLiteral("Run Mode"), macroRunModeCombo);
	macroEditorForm->addRow(QStringLiteral("Trigger"), macroTriggerTypeCombo);
	macroEditorForm->addRow(QStringLiteral("Interval (ms)"), macroIntervalSpin);
	macroEditorForm->addRow(QStringLiteral("Trigger Connection"), macroTriggerConnectionCombo);
	macroEditorForm->addRow(QStringLiteral("Trigger Scene"), macroTriggerSceneCombo);
	macroEditorForm->addRow(QStringLiteral("Trigger State"), macroTriggerStateCombo);
	macroEditorForm->addRow(QStringLiteral("Trigger Key"), macroTriggerKeyEdit);
	macroEditorForm->addRow(QStringLiteral("Trigger Value"), macroTriggerValueEdit);
	macroEditorForm->addRow(QStringLiteral("Action"), macroActionTypeCombo);
	macroEditorForm->addRow(QStringLiteral("Action Connection"), macroActionConnectionCombo);
	macroEditorForm->addRow(QStringLiteral("Action Scene"), macroActionSceneCombo);
	macroEditorForm->addRow(QStringLiteral("Delay (ms)"), macroActionDelaySpin);
	macroEditorForm->addRow(QStringLiteral("Key"), macroActionKeyEdit);
	macroEditorForm->addRow(QStringLiteral("Value / URL"), macroActionValueEdit);
	auto *macroToggles = new QHBoxLayout;
	macroToggles->setContentsMargins(0, 0, 0, 0);
	macroToggles->setSpacing(12);
	macroToggles->addWidget(macroEnabledCheckBox);
	macroToggles->addWidget(macroPausedCheckBox);
	macroToggles->addStretch(1);

	auto *editorTab = new QWidget(automationSurfaceTabs);
	auto *editorTabLayout = new QVBoxLayout(editorTab);
	editorTabLayout->setContentsMargins(0, 0, 0, 0);
	editorTabLayout->setSpacing(12);
	editorTabLayout->addLayout(macroEditorForm);
	editorTabLayout->addLayout(macroToggles);
	editorTabLayout->addWidget(macroLastResultLabel);
	editorTabLayout->addStretch(1);
	automationSurfaceTabs->addTab(editorTab, QStringLiteral("Editor"));

	auto *variablesTab = new QWidget(automationSurfaceTabs);
	auto *variablesLayout = new QVBoxLayout(variablesTab);
	variablesLayout->setContentsMargins(0, 0, 0, 0);
	variablesLayout->setSpacing(12);
	variablesLayout->addWidget(variableList, 1);
	auto *variableForm = new QFormLayout;
	ConfigureFormLayout(variableForm);
	variableForm->addRow(QStringLiteral("Key"), variableKeyEdit);
	variableForm->addRow(QStringLiteral("Value"), variableValueEdit);
	variablesLayout->addLayout(variableForm);
	auto *variableButtons = new QHBoxLayout;
	variableButtons->setContentsMargins(0, 0, 0, 0);
	variableButtons->setSpacing(8);
	variableButtons->addWidget(variableSetButton);
	variableButtons->addWidget(variableRemoveButton);
	variablesLayout->addLayout(variableButtons);
	automationSurfaceTabs->addTab(variablesTab, QStringLiteral("Variables"));

	auto *queuesTab = new QWidget(automationSurfaceTabs);
	auto *queuesLayout = new QVBoxLayout(queuesTab);
	queuesLayout->setContentsMargins(0, 0, 0, 0);
	queuesLayout->setSpacing(12);
	auto *queuesIntro = new QLabel(QStringLiteral("Action queues keep ordered or random macro actions moving without embedding queue state in individual macros."),
		queuesTab);
	queuesIntro->setWordWrap(true);
	queuesLayout->addWidget(queuesIntro);
	queuesLayout->addWidget(queueList, 1);
	queuesLayout->addWidget(queueClearButton);
	automationSurfaceTabs->addTab(queuesTab, QStringLiteral("Queues"));

	auto *connectionsTab = new QWidget(automationSurfaceTabs);
	auto *connectionsLayout = new QVBoxLayout(connectionsTab);
	connectionsLayout->setContentsMargins(0, 0, 0, 0);
	connectionsLayout->setSpacing(12);
	auto *connectionsIntro =
		new QLabel(QStringLiteral("Connections define reusable OSC endpoints for receiving and sending automation messages."),
			   connectionsTab);
	connectionsIntro->setWordWrap(true);
	connectionsLayout->addWidget(connectionsIntro);
	connectionsLayout->addWidget(connectionList, 1);
	auto *connectionButtons = new QHBoxLayout;
	connectionButtons->setContentsMargins(0, 0, 0, 0);
	connectionButtons->setSpacing(8);
	connectionButtons->addWidget(connectionAddButton);
	connectionButtons->addWidget(connectionRemoveButton);
	connectionButtons->addWidget(connectionTestButton);
	connectionsLayout->addLayout(connectionButtons);
	auto *connectionForm = new QFormLayout;
	ConfigureFormLayout(connectionForm);
	connectionForm->addRow(QStringLiteral("Name"), connectionNameEdit);
	connectionForm->addRow(QStringLiteral("Mode"), connectionModeCombo);
	connectionForm->addRow(QStringLiteral("Remote Host"), connectionRemoteHostEdit);
	connectionForm->addRow(QStringLiteral("Remote Port"), connectionRemotePortSpin);
	connectionForm->addRow(QStringLiteral("Listen Host"), connectionListenHostEdit);
	connectionForm->addRow(QStringLiteral("Listen Port"), connectionListenPortSpin);
	connectionsLayout->addLayout(connectionForm);
	connectionsLayout->addWidget(connectionDetailsLabel);
	automationSurfaceTabs->addTab(connectionsTab, QStringLiteral("Connections"));

	auto *eventLogTab = new QWidget(automationSurfaceTabs);
	auto *eventLogLayout = new QVBoxLayout(eventLogTab);
	eventLogLayout->setContentsMargins(0, 0, 0, 0);
	eventLogLayout->setSpacing(12);
	eventLogLayout->addWidget(eventLogList, 1);
	automationSurfaceTabs->addTab(eventLogTab, QStringLiteral("Event Log"));

	auto *automationSettingsTab = new QWidget(automationSurfaceTabs);
	auto *settingsLayout = new QVBoxLayout(automationSettingsTab);
	settingsLayout->setContentsMargins(0, 0, 0, 0);
	settingsLayout->setSpacing(12);
	settingsLayout->addWidget(automationStatusSummaryLabel);
	auto *automationImportExportButtons = new QHBoxLayout;
	automationImportExportButtons->setContentsMargins(0, 0, 0, 0);
	automationImportExportButtons->setSpacing(8);
	automationImportExportButtons->addWidget(automationExportButton);
	automationImportExportButtons->addWidget(automationImportButton);
	settingsLayout->addLayout(automationImportExportButtons);
	settingsLayout->addStretch(1);
	automationSurfaceTabs->addTab(automationSettingsTab, QStringLiteral("Settings"));

	macroEditorLayout->addWidget(automationSurfaceTabs, 1);

	automationSplitter->addWidget(macroListPane);
	automationSplitter->addWidget(macroEditorPane);
	automationSplitter->setStretchFactor(0, 0);
	automationSplitter->setStretchFactor(1, 1);
	automationSplitter->setSizes({360, 820});

	connect(layoutCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &SwitcherWorkspaceDock::LayoutChanged);
	connect(slotList, &QListWidget::currentRowChanged, this, &SwitcherWorkspaceDock::SelectedSlotChanged);
	connect(slotList, &QListWidget::itemActivated, this, [this](QListWidgetItem *) { OpenSelectedSlotInspector(); });
	connect(sceneCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &SwitcherWorkspaceDock::SelectedSceneChanged);
	connect(titleEdit, &QLineEdit::textChanged, this, &SwitcherWorkspaceDock::SelectedTitleChanged);
	connect(clearSlotButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::ClearSelectedSlot);
	connect(detachSlotButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::OpenSelectedSlotAsDock);
	connect(verticalDetachSlotButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::OpenSelectedSlotAsVerticalDock);
	connect(slotProjectorButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::OpenSelectedSlotProjector);
	connect(slotRecordButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::ToggleSelectedSlotRecording);
	connect(slotPauseRecordButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::ToggleSelectedSlotRecordingPause);
	connect(slotSplitRecordButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::SplitSelectedSlotRecording);
	connect(slotReplayButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::ToggleSelectedSlotReplayBuffer);
	connect(slotReplaySaveButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::SaveSelectedSlotReplayBuffer);
	connect(slotStreamButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::ToggleSelectedSlotStreaming);
	connect(slotVirtualCamButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::ToggleSelectedSlotVirtualCam);
	connect(remoteEnabledCheckBox, &QCheckBox::checkStateChanged, this, &SwitcherWorkspaceDock::RemoteEnabledChanged);
	connect(remoteAutoStartCheckBox, &QCheckBox::checkStateChanged, this, &SwitcherWorkspaceDock::RemoteAutoStartChanged);
	connect(remoteResolutionCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
		&SwitcherWorkspaceDock::RemoteResolutionChanged);
	connect(remoteFpsCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &SwitcherWorkspaceDock::RemoteFpsChanged);
	connect(remoteCopyButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::CopyRemoteUrl);
	connect(remoteRegenerateButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::RegenerateRemoteToken);
	connect(remoteRestartButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::RestartRemote);
	connect(inspectorModeButton, &QToolButton::clicked, this, [this] { ShowSettingsPanel(SettingsPanelMode::Workspace); });
	connect(inspectorCloseButton, &QToolButton::clicked, this, &SwitcherWorkspaceDock::HideSettingsPanel);
	connect(SwitcherRemoteManager::Instance(), &SwitcherRemoteManager::StateChanged, this,
		&SwitcherWorkspaceDock::RemoteStateUpdated);
	connect(modeList, &QListWidget::currentRowChanged, this, &SwitcherWorkspaceDock::ModeChanged);
	connect(verticalCanvasNameEdit, &QLineEdit::textEdited, this, &SwitcherWorkspaceDock::VerticalCanvasNameChanged);
	connect(verticalPresetCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &SwitcherWorkspaceDock::VerticalPresetChanged);
	connect(verticalTransitionCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
		&SwitcherWorkspaceDock::VerticalTransitionChanged);
	connect(verticalTransitionDurationSpin, qOverload<int>(&QSpinBox::valueChanged), this,
		&SwitcherWorkspaceDock::VerticalTransitionDurationChanged);
	connect(verticalSceneList, &QListWidget::currentRowChanged, this, &SwitcherWorkspaceDock::VerticalSceneSelectionChanged);
	connect(verticalSceneList, &QListWidget::customContextMenuRequested, this,
		&SwitcherWorkspaceDock::ShowVerticalSceneContextMenu);
	connect(verticalSceneAddButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::AddVerticalScene);
	connect(verticalSceneRemoveButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::RemoveSelectedVerticalScene);
	connect(verticalSceneMenuButton, &QPushButton::clicked, this, [this]() {
		QPoint anchor;
		if (auto *item = verticalSceneList->currentItem())
			anchor = verticalSceneList->visualItemRect(item).center();
		else
			anchor = QPoint(verticalSceneList->viewport()->width() / 2, verticalSceneList->viewport()->height() / 2);
		ShowVerticalSceneContextMenu(anchor);
	});
	connect(verticalSceneDockButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::OpenVerticalCanvasDock);
	connect(verticalScenesDockButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::OpenVerticalScenesDock);
	connect(verticalSourcesDockButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::OpenVerticalSourcesDock);
	connect(verticalTransitionsDockButton, &QPushButton::clicked, this,
		&SwitcherWorkspaceDock::OpenVerticalTransitionsDock);
	connect(verticalSettingsDockButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::OpenVerticalSettingsDock);
	connect(verticalSceneOpenWindowButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::OpenVerticalCanvasWindow);
	connect(verticalSceneOpenProjectorButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::OpenVerticalCanvasProjector);
	connect(verticalLinkCurrentSceneButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::LinkCurrentProgramSceneToSelectedVerticalScene);
	connect(verticalClearLinkButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::ClearSelectedVerticalLink);
	connect(verticalLinkedSyncCheckBox, &QCheckBox::checkStateChanged, this, &SwitcherWorkspaceDock::VerticalLinkedSyncChanged);
	connect(verticalConfigureOutputsButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::OpenVerticalOutputSettings);
	connect(verticalApplySceneTransitionButton, &QPushButton::clicked, this,
		[this]() { ApplySelectedSceneTransitionOverride(); });
	connect(verticalClearSceneTransitionButton, &QPushButton::clicked, this,
		[this]() { ClearSelectedSceneTransitionOverride(); });
	connect(verticalSourceTree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item, int column) {
		if (loadingVerticalUi)
			return;
		if (!item)
			return;
		const int itemId = item->data(0, kSourceItemIdRole).toInt();
		if (itemId <= 0)
			return;
		if (column == 1)
			SetCanvasSourceVisible(canvasManager->VerticalCanvasId(), itemId, item->checkState(1) == Qt::Checked);
		if (column == 2)
			SetCanvasSourceLocked(canvasManager->VerticalCanvasId(), itemId, item->checkState(2) == Qt::Checked);
	});
	connect(verticalSourceTree, &QTreeWidget::customContextMenuRequested, this,
		&SwitcherWorkspaceDock::ShowVerticalSourceContextMenu);
	connect(verticalSourceTree, &QTreeWidget::itemSelectionChanged, this, [this]() {
		const int itemId = SelectedVerticalSourceItemId();
		const bool hasSource = itemId > 0;
		verticalSourcePropertiesButton->setEnabled(hasSource);
		verticalSourceFiltersButton->setEnabled(hasSource);
		verticalSourceMenuButton->setEnabled(hasSource);
	});
	connect(verticalSourceTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
		if (!item)
			return;
		OpenVerticalSourceProperties(item->data(0, kSourceItemIdRole).toInt());
	});
	connect(verticalSourcePropertiesButton, &QPushButton::clicked, this,
		[this]() { OpenVerticalSourceProperties(SelectedVerticalSourceItemId()); });
	connect(verticalSourceFiltersButton, &QPushButton::clicked, this,
		[this]() { OpenVerticalSourceFilters(SelectedVerticalSourceItemId()); });
	connect(verticalSourceMenuButton, &QPushButton::clicked, this, [this]() {
		QPoint anchor;
		if (auto *item = verticalSourceTree->currentItem())
			anchor = verticalSourceTree->visualItemRect(item).center();
		else
			anchor = QPoint(verticalSourceTree->viewport()->width() / 2, verticalSourceTree->viewport()->height() / 2);
		ShowVerticalSourceContextMenu(anchor);
	});
	connect(verticalSceneLinksList, &QListWidget::currentRowChanged, this,
		[this]() { verticalClearLinkButton->setEnabled(verticalSceneLinksList->currentItem() != nullptr); });
	connect(verticalRecordButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::ToggleSelectedSlotRecording);
	connect(verticalPauseRecordButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::ToggleSelectedSlotRecordingPause);
	connect(verticalSplitRecordButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::SplitSelectedSlotRecording);
	connect(verticalChapterButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::AddRecordingChapter);
	connect(verticalReplayButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::ToggleSelectedSlotReplayBuffer);
	connect(verticalReplaySaveButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::SaveSelectedSlotReplayBuffer);
	connect(verticalStreamButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::ToggleSelectedSlotStreaming);
	connect(verticalVirtualCamButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::ToggleSelectedSlotVirtualCam);
	connect(motionShotList, &QListWidget::currentRowChanged, this, &SwitcherWorkspaceDock::MotionShotSelectionChanged);
	connect(motionShotAddButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::AddMotionShot);
	connect(motionShotDeleteButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::DeleteSelectedMotionShot);
	connect(motionShotDuplicateButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::DuplicateSelectedMotionShot);
	connect(motionShotBindButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::BindSelectedMotionShot);
	connect(motionShotSceneCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
		&SwitcherWorkspaceDock::MotionShotSceneChanged);
	connect(motionShotItemCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
		&SwitcherWorkspaceDock::MotionShotItemChanged);
	connect(motionProfileList, &QListWidget::currentRowChanged, this, &SwitcherWorkspaceDock::MotionSelectionChanged);
	connect(motionAddButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::AddMotionProfile);
	connect(motionDeleteButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::DeleteSelectedMotionProfile);
	connect(motionNameEdit, &QLineEdit::textEdited, this, &SwitcherWorkspaceDock::MotionNameChanged);
	connect(motionEnabledCheckBox, &QCheckBox::checkStateChanged, this, &SwitcherWorkspaceDock::MotionEnabledChanged);
	connect(motionConfidenceSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
		&SwitcherWorkspaceDock::MotionConfidenceChanged);
	connect(motionMaxZoomSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
		&SwitcherWorkspaceDock::MotionMaxZoomChanged);
	connect(motionFramingMarginSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
		&SwitcherWorkspaceDock::MotionFramingMarginChanged);
	connect(motionDeadZoneSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
		&SwitcherWorkspaceDock::MotionDeadZoneChanged);
	connect(motionSmoothingSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
		&SwitcherWorkspaceDock::MotionSmoothingChanged);
	connect(motionHoldSpin, qOverload<int>(&QSpinBox::valueChanged), this, &SwitcherWorkspaceDock::MotionHoldChanged);
	auto updateSelectedMotionProfile = [this](auto updater) {
		if (loadingMotionUi || !motionManager || !motionProfileList || !motionProfileList->currentItem())
			return;
		auto profile = MotionProfileDefinition(motionProfileList->currentItem()->data(Qt::UserRole).toString());
		if (profile.id.isEmpty())
			return;
		updater(profile);
		UpsertMotionProfile(profile);
	};
	auto updateSelectedMotionShot = [this](auto updater) {
		if (loadingMotionUi || !motionManager || !motionShotList || !motionShotList->currentItem())
			return;
		auto shot = MotionShotDefinition(motionShotList->currentItem()->data(Qt::UserRole).toString());
		if (shot.id.isEmpty())
			return;
		updater(shot);
		UpsertMotionShot(shot);
		RefreshMotionShots();
		RefreshMotionShotEditor();
	};
	connect(motionShotNameEdit, &QLineEdit::textEdited, this, [=](const QString &value) {
		updateSelectedMotionShot([value](SwitchMotionShot &shot) {
			shot.name = value.trimmed().isEmpty() ? QStringLiteral("Motion Shot") : value.trimmed();
		});
	});
	connect(motionShotEnabledCheckBox, &QCheckBox::checkStateChanged, this, [=](Qt::CheckState state) {
		updateSelectedMotionShot([state](SwitchMotionShot &shot) { shot.enabled = state == Qt::Checked; });
	});
	connect(motionShotModeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [=](int) {
		const QString mode = motionShotModeCombo->currentData().toString();
		updateSelectedMotionShot([mode](SwitchMotionShot &shot) { shot.shotMode = mode; });
		if ((mode == QStringLiteral("ai_auto_frame") || mode == QStringLiteral("hybrid")) &&
		    motionManager && motionShotList && motionShotList->currentItem()) {
			const auto shot = motionManager->ShotById(motionShotList->currentItem()->data(Qt::UserRole).toString());
			if (!shot.profileId.isEmpty())
				motionManager->SetProfileEnabled(shot.profileId, true);
		}
	});
	connect(motionShotPlaybackCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [=](int) {
		updateSelectedMotionShot([this](SwitchMotionShot &shot) { shot.playbackMode = motionShotPlaybackCombo->currentData().toString(); });
	});
	connect(motionShotPresetCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [=](int index) {
		if (loadingMotionUi || index <= 0)
			return;
		const QString presetId = motionShotPresetCombo->currentData().toString();
		updateSelectedMotionShot([presetId](SwitchMotionShot &shot) {
			for (const auto &preset : SwitchMotionShotPresets()) {
				if (preset.id != presetId)
					continue;
				const auto presetShot = preset.shot;
				shot.startPanX = presetShot.startPanX;
				shot.startPanY = presetShot.startPanY;
				shot.startZoom = presetShot.startZoom;
				shot.endPanX = presetShot.endPanX;
				shot.endPanY = presetShot.endPanY;
				shot.endZoom = presetShot.endZoom;
				shot.durationMs = presetShot.durationMs;
				shot.easing = presetShot.easing;
				shot.loopMode = presetShot.loopMode;
				shot.shotMode = QStringLiteral("keyframe_loop");
				break;
			}
		});
	});
	connect(motionShotDurationSpin, qOverload<int>(&QSpinBox::valueChanged), this,
		[=](int value) { updateSelectedMotionShot([value](SwitchMotionShot &shot) { shot.durationMs = value; }); });
	connect(motionShotEasingCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [=](int) {
		updateSelectedMotionShot([this](SwitchMotionShot &shot) { shot.easing = motionShotEasingCombo->currentData().toString(); });
	});
	connect(motionShotLoopModeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [=](int) {
		updateSelectedMotionShot([this](SwitchMotionShot &shot) { shot.loopMode = motionShotLoopModeCombo->currentData().toString(); });
	});
	connect(motionShotStartPanXSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
		[=](double value) { updateSelectedMotionShot([value](SwitchMotionShot &shot) { shot.startPanX = static_cast<float>(value); }); });
	connect(motionShotStartPanYSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
		[=](double value) { updateSelectedMotionShot([value](SwitchMotionShot &shot) { shot.startPanY = static_cast<float>(value); }); });
	connect(motionShotStartZoomSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
		[=](double value) { updateSelectedMotionShot([value](SwitchMotionShot &shot) { shot.startZoom = static_cast<float>(value); }); });
	connect(motionShotEndPanXSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
		[=](double value) { updateSelectedMotionShot([value](SwitchMotionShot &shot) { shot.endPanX = static_cast<float>(value); }); });
	connect(motionShotEndPanYSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
		[=](double value) { updateSelectedMotionShot([value](SwitchMotionShot &shot) { shot.endPanY = static_cast<float>(value); }); });
	connect(motionShotEndZoomSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
		[=](double value) { updateSelectedMotionShot([value](SwitchMotionShot &shot) { shot.endZoom = static_cast<float>(value); }); });
	connect(motionShotMaxZoomSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
		[=](double value) { updateSelectedMotionShot([value](SwitchMotionShot &shot) { shot.maxZoom = static_cast<float>(value); }); });
	connect(motionSubjectModeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [=](int) {
		updateSelectedMotionProfile([this](SwitchMotionProfile &profile) {
			profile.subjectMode = motionSubjectModeCombo->currentData().toString();
			if (profile.subjectMode != QStringLiteral("locked"))
				profile.lockedTrackId = -1;
		});
	});
	connect(motionFramingModeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [=](int) {
		updateSelectedMotionProfile([this](SwitchMotionProfile &profile) {
			profile.framingMode = motionFramingModeCombo->currentData().toString();
		});
	});
	connect(motionPresetCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [=](int) {
		updateSelectedMotionProfile([this](SwitchMotionProfile &profile) {
			const QString preset = motionPresetCombo->currentData().toString();
			if (preset == QStringLiteral("subtle")) {
				profile.maxZoom = 1.55f;
				profile.panResponsiveness = 0.42f;
				profile.tiltResponsiveness = 0.40f;
				profile.zoomResponsiveness = 0.34f;
				profile.maxPanSpeed = 0.42f;
				profile.maxTiltSpeed = 0.38f;
				profile.maxZoomSpeed = 0.70f;
			} else if (preset == QStringLiteral("presenter")) {
				profile.maxZoom = 2.0f;
				profile.framingMode = QStringLiteral("upper_body");
				profile.panResponsiveness = 0.62f;
				profile.tiltResponsiveness = 0.58f;
				profile.zoomResponsiveness = 0.48f;
				profile.maxPanSpeed = 0.75f;
				profile.maxTiltSpeed = 0.65f;
				profile.maxZoomSpeed = 1.05f;
			} else if (preset == QStringLiteral("tight_face")) {
				profile.maxZoom = 2.6f;
				profile.framingMode = QStringLiteral("face_headroom");
				profile.panResponsiveness = 0.70f;
				profile.tiltResponsiveness = 0.66f;
				profile.zoomResponsiveness = 0.50f;
				profile.maxPanSpeed = 0.82f;
				profile.maxTiltSpeed = 0.75f;
				profile.maxZoomSpeed = 1.10f;
			} else if (preset == QStringLiteral("wide_room")) {
				profile.maxZoom = 1.35f;
				profile.framingMode = QStringLiteral("group");
				profile.panResponsiveness = 0.35f;
				profile.tiltResponsiveness = 0.32f;
				profile.zoomResponsiveness = 0.28f;
				profile.maxPanSpeed = 0.35f;
				profile.maxTiltSpeed = 0.32f;
				profile.maxZoomSpeed = 0.55f;
			} else if (preset == QStringLiteral("fast_follow")) {
				profile.maxZoom = 2.2f;
				profile.panResponsiveness = 0.95f;
				profile.tiltResponsiveness = 0.90f;
				profile.zoomResponsiveness = 0.74f;
				profile.maxPanSpeed = 1.45f;
				profile.maxTiltSpeed = 1.25f;
				profile.maxZoomSpeed = 1.90f;
			} else {
				profile.maxZoom = 2.2f;
				profile.panResponsiveness = 0.64f;
				profile.tiltResponsiveness = 0.62f;
				profile.zoomResponsiveness = 0.54f;
				profile.maxPanSpeed = 0.85f;
				profile.maxTiltSpeed = 0.75f;
				profile.maxZoomSpeed = 1.25f;
			}
		});
		RefreshMotionEditor();
	});
	connect(motionTrackerHighSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [=](double value) {
		updateSelectedMotionProfile([value](SwitchMotionProfile &profile) { profile.trackerHighThreshold = static_cast<float>(value); });
	});
	connect(motionTrackerLowSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [=](double value) {
		updateSelectedMotionProfile([value](SwitchMotionProfile &profile) { profile.trackerLowThreshold = static_cast<float>(value); });
	});
	connect(motionNewTrackSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [=](double value) {
		updateSelectedMotionProfile([value](SwitchMotionProfile &profile) { profile.newTrackThreshold = static_cast<float>(value); });
	});
	connect(motionTrackBufferSpin, qOverload<int>(&QSpinBox::valueChanged), this, [=](int value) {
		updateSelectedMotionProfile([value](SwitchMotionProfile &profile) { profile.trackBufferFrames = value; });
	});
	connect(motionAutoSwitchSpin, qOverload<int>(&QSpinBox::valueChanged), this, [=](int value) {
		updateSelectedMotionProfile([value](SwitchMotionProfile &profile) { profile.autoSwitchMs = value; });
	});
	connect(motionPanResponsivenessSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [=](double value) {
		updateSelectedMotionProfile([value](SwitchMotionProfile &profile) { profile.panResponsiveness = static_cast<float>(value); });
	});
	connect(motionTiltResponsivenessSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [=](double value) {
		updateSelectedMotionProfile([value](SwitchMotionProfile &profile) { profile.tiltResponsiveness = static_cast<float>(value); });
	});
	connect(motionZoomResponsivenessSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [=](double value) {
		updateSelectedMotionProfile([value](SwitchMotionProfile &profile) { profile.zoomResponsiveness = static_cast<float>(value); });
	});
	connect(motionMaxPanSpeedSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [=](double value) {
		updateSelectedMotionProfile([value](SwitchMotionProfile &profile) { profile.maxPanSpeed = static_cast<float>(value); });
	});
	connect(motionMaxTiltSpeedSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [=](double value) {
		updateSelectedMotionProfile([value](SwitchMotionProfile &profile) { profile.maxTiltSpeed = static_cast<float>(value); });
	});
	connect(motionMaxZoomSpeedSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [=](double value) {
		updateSelectedMotionProfile([value](SwitchMotionProfile &profile) { profile.maxZoomSpeed = static_cast<float>(value); });
	});
	connect(motionDebugOverlayCheckBox, &QCheckBox::checkStateChanged, this, [=](Qt::CheckState state) {
		updateSelectedMotionProfile([state](SwitchMotionProfile &profile) { profile.debugOverlay = state == Qt::Checked; });
	});
	connect(motionLockCurrentButton, &QPushButton::clicked, this, [=]() {
		if (!motionManager)
			return;
		const auto runtime = motionManager->RuntimeState();
		if (runtime.targetTrackId < 0)
			return;
		updateSelectedMotionProfile([runtime](SwitchMotionProfile &profile) {
			profile.subjectMode = QStringLiteral("locked");
			profile.lockedTrackId = runtime.targetTrackId;
		});
		RefreshMotionEditor();
	});
	connect(motionCyclePreviousButton, &QPushButton::clicked, this, [=]() {
		if (!motionManager)
			return;
		const auto runtime = motionManager->RuntimeState();
		QVector<int> ids;
		for (const auto &track : runtime.tracks) {
			if (track.state == QStringLiteral("active") || track.state == QStringLiteral("new") ||
			    track.state == QStringLiteral("recovered"))
				ids.push_back(track.trackId);
		}
		if (ids.isEmpty())
			return;
		std::sort(ids.begin(), ids.end());
		const int index = ids.indexOf(runtime.targetTrackId);
		const int targetId = index < 0 ? ids.back() : ids[(index - 1 + ids.size()) % ids.size()];
		updateSelectedMotionProfile([targetId](SwitchMotionProfile &profile) {
			profile.subjectMode = QStringLiteral("locked");
			profile.lockedTrackId = targetId;
		});
		RefreshMotionEditor();
	});
	connect(motionCycleNextButton, &QPushButton::clicked, this, [=]() {
		if (!motionManager)
			return;
		const auto runtime = motionManager->RuntimeState();
		QVector<int> ids;
		for (const auto &track : runtime.tracks) {
			if (track.state == QStringLiteral("active") || track.state == QStringLiteral("new") ||
			    track.state == QStringLiteral("recovered"))
				ids.push_back(track.trackId);
		}
		if (ids.isEmpty())
			return;
		std::sort(ids.begin(), ids.end());
		const int index = ids.indexOf(runtime.targetTrackId);
		const int targetId = index < 0 ? ids.front() : ids[(index + 1) % ids.size()];
		updateSelectedMotionProfile([targetId](SwitchMotionProfile &profile) {
			profile.subjectMode = QStringLiteral("locked");
			profile.lockedTrackId = targetId;
		});
		RefreshMotionEditor();
	});
	connect(motionClearLockButton, &QPushButton::clicked, this, [=]() {
		updateSelectedMotionProfile([](SwitchMotionProfile &profile) {
			profile.subjectMode = QStringLiteral("auto");
			profile.lockedTrackId = -1;
		});
		RefreshMotionEditor();
	});
	connect(motionTrackList, &QListWidget::itemClicked, this, [=](QListWidgetItem *item) {
		if (!item)
			return;
		const int targetId = item->data(Qt::UserRole).toInt();
		if (targetId < 0)
			return;
		updateSelectedMotionProfile([targetId](SwitchMotionProfile &profile) {
			profile.subjectMode = QStringLiteral("locked");
			profile.lockedTrackId = targetId;
		});
		RefreshMotionEditor();
	});
	connect(motionBackendCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
		if (loadingMotionUi || !motionManager)
			return;
		auto profile = MotionProfileDefinition(motionProfileList->currentItem()
							       ? motionProfileList->currentItem()->data(Qt::UserRole).toString()
							       : QString());
		if (profile.id.isEmpty())
			return;
		profile.backend = motionBackendCombo->currentData().toString();
		UpsertMotionProfile(profile);
	});
	connect(motionBindButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::BindSelectedMotionSource);
	connect(motionUnbindButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::UnbindSelectedMotionSource);
	connect(motionBindingList, &QListWidget::currentRowChanged, this,
		[this]() { motionUnbindButton->setEnabled(motionBindingList->currentItem() != nullptr); });
	connect(macroList, &QListWidget::currentRowChanged, this, &SwitcherWorkspaceDock::MacroSelectionChanged);
	connect(macroAddButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::AddMacro);
	connect(macroDeleteButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::DeleteSelectedMacro);
	connect(macroRunButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::RunSelectedMacro);
	connect(macroDuplicateButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::DuplicateSelectedMacro);
	connect(macroExportButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::ExportSelectedMacro);
	connect(macroImportButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::ImportMacro);
	connect(macroNameEdit, &QLineEdit::textEdited, this, &SwitcherWorkspaceDock::MacroNameChanged);
	connect(macroEnabledCheckBox, &QCheckBox::checkStateChanged, this, &SwitcherWorkspaceDock::MacroEnabledChanged);
	connect(macroPausedCheckBox, &QCheckBox::checkStateChanged, this, &SwitcherWorkspaceDock::MacroPausedChanged);
	connect(macroRunModeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &SwitcherWorkspaceDock::MacroRunModeChanged);
	connect(macroTriggerTypeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &SwitcherWorkspaceDock::MacroTriggerTypeChanged);
	connect(macroIntervalSpin, qOverload<int>(&QSpinBox::valueChanged), this, &SwitcherWorkspaceDock::MacroIntervalChanged);
	connect(macroTriggerConnectionCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
		&SwitcherWorkspaceDock::MacroTriggerConnectionChanged);
	connect(macroTriggerSceneCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &SwitcherWorkspaceDock::MacroTriggerSceneChanged);
	connect(macroTriggerStateCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &SwitcherWorkspaceDock::MacroTriggerStateChanged);
	connect(macroTriggerKeyEdit, &QLineEdit::textEdited, this, &SwitcherWorkspaceDock::MacroTriggerKeyChanged);
	connect(macroTriggerValueEdit, &QLineEdit::textEdited, this, &SwitcherWorkspaceDock::MacroTriggerValueChanged);
	connect(macroActionTypeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &SwitcherWorkspaceDock::MacroActionTypeChanged);
	connect(macroActionConnectionCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
		&SwitcherWorkspaceDock::MacroActionConnectionChanged);
	connect(macroActionSceneCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &SwitcherWorkspaceDock::MacroActionSceneChanged);
	connect(macroActionDelaySpin, qOverload<int>(&QSpinBox::valueChanged), this, &SwitcherWorkspaceDock::MacroActionDelayChanged);
	connect(macroActionKeyEdit, &QLineEdit::textEdited, this, &SwitcherWorkspaceDock::MacroActionKeyChanged);
	connect(macroActionValueEdit, &QLineEdit::textEdited, this, &SwitcherWorkspaceDock::MacroActionValueChanged);
	connect(variableSetButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::SetAutomationVariableFromEditor);
	connect(variableRemoveButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::RemoveSelectedVariable);
	connect(variableList, &QListWidget::currentRowChanged, this,
		[this]() { variableRemoveButton->setEnabled(variableList->currentItem() != nullptr); });
	connect(queueList, &QListWidget::currentRowChanged, this,
		[this]() { queueClearButton->setEnabled(queueList->currentItem() != nullptr); });
	connect(queueClearButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::ClearSelectedQueue);
	connect(connectionAddButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::AddOscConnection);
	connect(connectionRemoveButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::RemoveSelectedConnection);
	connect(connectionList, &QListWidget::currentRowChanged, this, &SwitcherWorkspaceDock::ConnectionSelectionChanged);
	connect(connectionNameEdit, &QLineEdit::textEdited, this, &SwitcherWorkspaceDock::ConnectionNameChanged);
	connect(connectionModeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
		&SwitcherWorkspaceDock::ConnectionModeChanged);
	connect(connectionRemoteHostEdit, &QLineEdit::textEdited, this, &SwitcherWorkspaceDock::ConnectionRemoteHostChanged);
	connect(connectionRemotePortSpin, qOverload<int>(&QSpinBox::valueChanged), this,
		&SwitcherWorkspaceDock::ConnectionRemotePortChanged);
	connect(connectionListenHostEdit, &QLineEdit::textEdited, this, &SwitcherWorkspaceDock::ConnectionListenHostChanged);
	connect(connectionListenPortSpin, qOverload<int>(&QSpinBox::valueChanged), this,
		&SwitcherWorkspaceDock::ConnectionListenPortChanged);
	connect(connectionTestButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::TestSelectedConnection);
	connect(automationExportButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::ExportAutomationDocument);
	connect(automationImportButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::ImportAutomationDocument);
	connect(canvasManager, &SwitchCanvasManager::StateChanged, this,
		[this]() {
			if (frontendShuttingDown) {
				suppressNextVerticalRefresh = false;
				return;
			}
			if (suppressNextVerticalRefresh) {
				suppressNextVerticalRefresh = false;
			} else {
				ScheduleVerticalRefresh();
			}
			RefreshVerticalDockSurfaces();
			EmitCanvasVendorStateChanged();
			emit WorkspaceStateChanged();
		},
		Qt::QueuedConnection);
	connect(motionManager, &SwitchMotionManager::StateChanged, this,
		[this]() {
			if (frontendShuttingDown)
				return;
			ScheduleMotionRuntimeRefresh();
			emit WorkspaceStateChanged();
		},
		Qt::QueuedConnection);
	connect(motionManager, &SwitchMotionManager::ProfileChanged, this, [this](const QString &profileId) {
		if (frontendShuttingDown)
			return;
		UNUSED_PARAMETER(profileId);
	}, Qt::QueuedConnection);
	connect(motionManager, &SwitchMotionManager::ShotChanged, this, [this](const QString &shotId) {
		if (frontendShuttingDown)
			return;
		RefreshMotionShots();
		RefreshMotionShotEditor();
		obs_data_t *data = BuildMotionState();
		obs_data_set_string(data, "shotId", shotId.toUtf8().constData());
		SwitcherEmitVendorEvent("Motion.ShotChanged", data);
		obs_data_release(data);
	}, Qt::QueuedConnection);
	connect(motionManager, &SwitchMotionManager::ActiveShotChanged, this, [this](const QString &shotId) {
		if (frontendShuttingDown)
			return;
		obs_data_t *data = BuildMotionState();
		obs_data_set_string(data, "shotId", shotId.toUtf8().constData());
		SwitcherEmitVendorEvent("Motion.ActiveShotChanged", data);
		obs_data_release(data);
		ScheduleMotionRuntimeRefresh();
	}, Qt::QueuedConnection);
	connect(motionManager, &SwitchMotionManager::ShotRuntimeError, this, [this](const QString &message) {
		if (frontendShuttingDown)
			return;
		obs_data_t *data = BuildMotionState();
		obs_data_set_string(data, "message", message.toUtf8().constData());
		SwitcherEmitVendorEvent("Motion.ShotRuntimeError", data);
		obs_data_release(data);
	}, Qt::QueuedConnection);
	connect(motionManager, &SwitchMotionManager::TargetChanged, this, [this](bool active, float confidence,
									 int trackId, const QString &sourceUuid) {
		if (frontendShuttingDown)
			return;
		obs_data_t *data = BuildMotionState();
		obs_data_set_bool(data, "targetActive", active);
		obs_data_set_double(data, "targetConfidence", confidence);
		obs_data_set_int(data, "trackId", trackId);
		obs_data_set_string(data, "sourceUuid", sourceUuid.toUtf8().constData());
		SwitcherEmitVendorEvent("Motion.TargetChanged", data);
		obs_data_release(data);
		if (automationEngine) {
			automationEngine->SetVariable(QStringLiteral("motion.targetActive"), active ? QStringLiteral("true") : QStringLiteral("false"));
			automationEngine->SetVariable(QStringLiteral("motion.targetConfidence"), QString::number(confidence, 'f', 3));
			automationEngine->SetVariable(QStringLiteral("motion.targetTrackId"), QString::number(trackId));
		}
	});
	connect(motionManager, &SwitchMotionManager::TracksChanged, this, [this]() {
		if (frontendShuttingDown)
			return;
		obs_data_t *data = BuildMotionState();
		SwitcherEmitVendorEvent("Motion.TracksChanged", data);
		obs_data_release(data);
		ScheduleMotionRuntimeRefresh();
	});
	connect(motionManager, &SwitchMotionManager::RuntimeStatsChanged, this, [this]() {
		if (frontendShuttingDown)
			return;
		ScheduleMotionRuntimeRefresh();
	});
	connect(motionManager, &SwitchMotionManager::TargetLost, this, [this](int trackId, const QString &sourceUuid) {
		if (frontendShuttingDown)
			return;
		obs_data_t *data = BuildMotionState();
		obs_data_set_int(data, "trackId", trackId);
		obs_data_set_string(data, "sourceUuid", sourceUuid.toUtf8().constData());
		SwitcherEmitVendorEvent("Motion.TargetLost", data);
		obs_data_release(data);
	});
	connect(motionManager, &SwitchMotionManager::TargetReacquired, this, [this](int trackId, const QString &sourceUuid) {
		if (frontendShuttingDown)
			return;
		obs_data_t *data = BuildMotionState();
		obs_data_set_int(data, "trackId", trackId);
		obs_data_set_string(data, "sourceUuid", sourceUuid.toUtf8().constData());
		SwitcherEmitVendorEvent("Motion.TargetReacquired", data);
		obs_data_release(data);
	});
	connect(motionManager, &SwitchMotionManager::RuntimeError, this, [this](const QString &message) {
		if (frontendShuttingDown)
			return;
		obs_data_t *data = BuildMotionState();
		obs_data_set_string(data, "message", message.toUtf8().constData());
		SwitcherEmitVendorEvent("Motion.RuntimeError", data);
		obs_data_release(data);
	});
	connect(automationEngine, &SwitchAutomationEngine::StateChanged, this,
		[this]() {
			if (frontendShuttingDown)
				return;
			RefreshAutomationPage();
			EmitAutomationVendorStateChanged();
			emit WorkspaceStateChanged();
		},
		Qt::QueuedConnection);
	connect(automationEngine, &SwitchAutomationEngine::MacroTriggered, this,
		[this](const QString &macroId, bool success, const QString &message) {
			macroLastResultLabel->setText(message);
			EmitAutomationVendorStateChanged(macroId, success, message);
		});
	connect(automationEngine, &SwitchAutomationEngine::MacroPaused, this, [this](const QString &macroId, bool paused) {
		if (frontendShuttingDown)
			return;
		obs_data_t *data = BuildAutomationState();
		obs_data_set_string(data, "macroId", macroId.toUtf8().constData());
		obs_data_set_bool(data, "paused", paused);
		SwitcherEmitVendorEvent("Automation.MacroPaused", data);
		obs_data_release(data);
	});
	connect(automationEngine, &SwitchAutomationEngine::VariableChanged, this,
		[this](const QString &key, const QString &value) {
			if (frontendShuttingDown)
				return;
			obs_data_t *data = BuildAutomationState();
			obs_data_set_string(data, "key", key.toUtf8().constData());
			obs_data_set_string(data, "value", value.toUtf8().constData());
			SwitcherEmitVendorEvent("Automation.VariableChanged", data);
			obs_data_release(data);
		});
	connect(automationEngine, &SwitchAutomationEngine::QueueChanged, this, [this](const QString &queueId) {
		if (frontendShuttingDown)
			return;
		obs_data_t *data = BuildAutomationState();
		obs_data_set_string(data, "queueId", queueId.toUtf8().constData());
		SwitcherEmitVendorEvent("Automation.QueueChanged", data);
		obs_data_release(data);
	});
	connect(automationEngine, &SwitchAutomationEngine::ConnectionChanged, this,
		[this](const QString &connectionId) {
			if (frontendShuttingDown)
				return;
			obs_data_t *data = BuildAutomationState();
			obs_data_set_string(data, "connectionId", connectionId.toUtf8().constData());
			SwitcherEmitVendorEvent("Automation.ConnectionChanged", data);
			obs_data_release(data);
		});
	connect(automationEngine, &SwitchAutomationEngine::ErrorRaised, this,
		[this](const QString &scope, const QString &message) {
			if (frontendShuttingDown)
				return;
			obs_data_t *data = BuildAutomationState();
			obs_data_set_string(data, "scope", scope.toUtf8().constData());
			obs_data_set_string(data, "message", message.toUtf8().constData());
			SwitcherEmitVendorEvent("Automation.Error", data);
			obs_data_release(data);
		});

	RefreshGrid();
	RefreshSlotList();
	RefreshSceneOptions();
	RefreshSelectedSlotEditor();
	RemoteStateUpdated();
	RefreshVerticalPage();
	RefreshMotionPage();
	RefreshAutomationPage();
	RefreshSettingsPanelMode();
	UpdateInspectorVisibility(false);
	modeList->setCurrentRow(0);
	modeStack->setCurrentWidget(workspaceModePage);
	ApplyTheme();
}

SwitcherWorkspaceDock::~SwitcherWorkspaceDock()
{
	RemoveQuickOutputControls();
	if (!frontendShuttingDown)
		UnregisterVerticalObsDocks();
	if (gWorkspaceDock == this)
		gWorkspaceDock = nullptr;
}

void SwitcherWorkspaceDock::InstallQuickOutputControls()
{
	if (quickOutputContainer && quickSwitchButton) {
		UpdateQuickOutputControls();
		return;
	}
	if (quickOutputContainer || quickSwitchButton)
		RemoveQuickOutputControls();

	auto *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!mainWindow)
		return;

	auto *controlsFrame = mainWindow->findChild<QWidget *>(QStringLiteral("controlsFrame"));
	if (!controlsFrame || !controlsFrame->layout()) {
		QTimer::singleShot(500, this, [this]() {
			if (!frontendShuttingDown)
				InstallQuickOutputControls();
		});
		return;
	}

	auto *container = new QWidget(controlsFrame);
	container->setObjectName(QStringLiteral("switchQuickOutputControls"));
	auto *containerLayout = new QVBoxLayout(container);
	containerLayout->setContentsMargins(0, 0, 0, 0);
	containerLayout->setSpacing(0);

	const auto makeRow = [this, containerLayout, container](const QString &label, QPushButton **actionButton,
								QPushButton **configButton, bool multiview) {
		auto *row = new QWidget(container);
		row->setObjectName(QStringLiteral("switchQuickOutputRow"));
		auto *rowLayout = new QHBoxLayout(row);
		rowLayout->setContentsMargins(0, 0, 0, 0);
		rowLayout->setSpacing(0);

		auto *action = new QPushButton(label, row);
		action->setObjectName(QStringLiteral("switchQuickOutputButton"));
		action->setCursor(Qt::PointingHandCursor);

		auto *config = new QPushButton(row);
		config->setObjectName(QStringLiteral("switchQuickOutputConfigButton"));
		config->setIcon(SettingsButtonIcon(config));
		config->setProperty("class", QStringLiteral("icon-gear"));
		config->setCursor(Qt::PointingHandCursor);
		config->setFixedWidth(42);

		rowLayout->addWidget(action, 1);
		rowLayout->addWidget(config, 0);
		containerLayout->addWidget(row);

		connect(action, &QPushButton::clicked, this, [this, multiview]() { OpenQuickOutputProjector(multiview); });
		connect(config, &QPushButton::clicked, this, [this, multiview]() { ShowQuickOutputMonitorMenu(multiview); });

		*actionButton = action;
		*configButton = config;
	};

	makeRow(QStringLiteral("Multi View"), &quickMultiviewButton, &quickMultiviewConfigButton, true);
	makeRow(QStringLiteral("Program Out"), &quickProgramButton, &quickProgramConfigButton, false);

	QLayout *controlsLayout = controlsFrame->layout();
	const int modeSwitchIndex =
		LayoutIndexContainingWidget(controlsLayout, controlsFrame->findChild<QWidget *>(QStringLiteral("modeSwitch")));
	if (auto *boxLayout = qobject_cast<QBoxLayout *>(controlsLayout)) {
		boxLayout->insertWidget(modeSwitchIndex >= 0 ? modeSwitchIndex : boxLayout->count(), container);

		auto *switchButton = new QPushButton(QStringLiteral("Switch"), controlsFrame);
		switchButton->setObjectName(QStringLiteral("switchControlsButton"));
		switchButton->setCursor(Qt::PointingHandCursor);
		switchButton->setToolTip(QStringLiteral("Open Switch"));
		connect(switchButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::OpenWorkspaceWindow);

		const int settingsIndex = LayoutIndexContainingWidget(controlsLayout,
								      controlsFrame->findChild<QWidget *>(
									      QStringLiteral("settingsButton")));
		boxLayout->insertWidget(settingsIndex >= 0 ? settingsIndex + 1 : boxLayout->count(), switchButton);
		quickSwitchButton = switchButton;
	} else {
		controlsLayout->addWidget(container);
		auto *switchButton = new QPushButton(QStringLiteral("Switch"), controlsFrame);
		switchButton->setObjectName(QStringLiteral("switchControlsButton"));
		switchButton->setCursor(Qt::PointingHandCursor);
		switchButton->setToolTip(QStringLiteral("Open Switch"));
		connect(switchButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::OpenWorkspaceWindow);
		controlsLayout->addWidget(switchButton);
		quickSwitchButton = switchButton;
	}

	quickOutputContainer = container;
	UpdateQuickOutputControls();
}

void SwitcherWorkspaceDock::RemoveQuickOutputControls()
{
	if (!quickOutputContainer && !quickSwitchButton)
		return;

	if (quickOutputContainer)
		delete quickOutputContainer.data();
	quickOutputContainer = nullptr;
	quickMultiviewButton = nullptr;
	quickMultiviewConfigButton = nullptr;
	quickProgramButton = nullptr;
	quickProgramConfigButton = nullptr;
	quickOutputActive = QuickOutputActive::None;
	if (quickSwitchButton)
		delete quickSwitchButton.data();
	quickSwitchButton = nullptr;
}

void SwitcherWorkspaceDock::UpdateQuickOutputControls()
{
	const int multiviewMonitor = EffectiveQuickOutputMonitor(quickMultiviewMonitor);
	const int programMonitor = EffectiveQuickOutputMonitor(quickProgramMonitor);
	const QString multiviewTarget = MonitorLabel(multiviewMonitor);
	const QString programTarget = MonitorLabel(programMonitor);
	const bool multiviewActive = quickOutputActive == QuickOutputActive::Multiview;
	const bool programActive = quickOutputActive == QuickOutputActive::Program;

	if (quickMultiviewButton) {
		quickMultiviewButton->setText(QStringLiteral("Multi View"));
		quickMultiviewButton->setToolTip(multiviewActive
							 ? QStringLiteral("OBS Multi View is displaying on %1").arg(multiviewTarget)
							 : QStringLiteral("Open OBS Multi View on %1").arg(multiviewTarget));
	}
	if (quickMultiviewConfigButton)
		quickMultiviewConfigButton->setToolTip(multiviewActive
							       ? QStringLiteral("Multi View is displaying on %1")
									 .arg(multiviewTarget)
							       : QStringLiteral("Choose Multi View output display"));
	if (quickProgramButton) {
		quickProgramButton->setText(QStringLiteral("Program Out"));
		quickProgramButton->setToolTip(programActive
						       ? QStringLiteral("OBS Program output is displaying on %1")
								 .arg(programTarget)
						       : QStringLiteral("Open OBS Program output on %1").arg(programTarget));
	}
	if (quickProgramConfigButton)
		quickProgramConfigButton->setToolTip(programActive
							     ? QStringLiteral("Program output is displaying on %1")
								       .arg(programTarget)
							     : QStringLiteral("Choose Program output display"));
	if (quickSwitchButton)
		quickSwitchButton->setToolTip(QStringLiteral("Open Switch"));

	ApplyQuickOutputActiveStyle(quickMultiviewButton, multiviewActive, true);
	ApplyQuickOutputActiveStyle(quickMultiviewConfigButton, multiviewActive, true);
	ApplyQuickOutputActiveStyle(quickProgramButton, programActive, false);
	ApplyQuickOutputActiveStyle(quickProgramConfigButton, programActive, false);
}

void SwitcherWorkspaceDock::OpenQuickOutputProjector(bool multiview)
{
	const int monitor = EffectiveQuickOutputMonitor(multiview ? quickMultiviewMonitor : quickProgramMonitor);
	obs_frontend_open_projector(multiview ? "Multiview" : "StudioProgram", monitor, nullptr, nullptr);
	quickOutputActive = multiview ? QuickOutputActive::Multiview : QuickOutputActive::Program;
	UpdateQuickOutputControls();
}

void SwitcherWorkspaceDock::ShowQuickOutputMonitorMenu(bool multiview)
{
	auto *anchor = multiview ? quickMultiviewConfigButton : quickProgramConfigButton;
	if (!anchor)
		return;

	const int activeMonitor = EffectiveQuickOutputMonitor(multiview ? quickMultiviewMonitor : quickProgramMonitor);
	const QString title = multiview ? QStringLiteral("Multi View Output") : QStringLiteral("Program Output");

	QStringList options;
	std::vector<int> monitors;
	options << QStringLiteral("Windowed projector");
	monitors.push_back(-1);

	const auto screens = QGuiApplication::screens();
	for (int index = 0; index < screens.size(); index++) {
		options << MonitorLabel(index);
		monitors.push_back(index);
	}

	int currentIndex = 0;
	for (int index = 0; index < static_cast<int>(monitors.size()); index++) {
		if (monitors[index] == activeMonitor) {
			currentIndex = index;
			break;
		}
	}

	bool accepted = false;
	const QString selected =
		QInputDialog::getItem(anchor->window(), title, QStringLiteral("Output Display"), options, currentIndex,
				      false, &accepted);
	if (!accepted)
		return;

	const int selectedIndex = options.indexOf(selected);
	if (selectedIndex < 0 || selectedIndex >= static_cast<int>(monitors.size()))
		return;

	if (multiview)
		quickMultiviewMonitor = monitors[selectedIndex];
	else
		quickProgramMonitor = monitors[selectedIndex];
	UpdateQuickOutputControls();
	emit WorkspaceStateChanged();
}

void SwitcherWorkspaceDock::OpenWorkspaceWindow()
{
	const bool verticalDockVisible = verticalObsDockContainer && verticalObsDockContainer->isVisible();
	if (!workspacePlacementInitialized)
		ApplyDefaultWindowGeometry();

	if (verticalDockVisible) {
		restoreVerticalObsDockOnHide = true;
		if (verticalObsDockWidget)
			verticalObsDockWidget->SetPreviewRenderingEnabled(false);
		verticalObsDockContainer->hide();
	}

	if (isMinimized())
		showNormal();
	else
		show();

	raise();
	activateWindow();
}

obs_data_t *SwitcherWorkspaceDock::SaveContentState() const
{
	obs_data_t *data = obs_data_create();
	obs_data_set_int(data, "visible_slots", VisibleSlotCount());

	obs_data_array_t *slotArray = obs_data_array_create();
	for (const auto &slot : slotWidgets) {
		obs_data_t *slotData = obs_data_create();
		if (auto source = slot->GetSource()) {
			const char *sourceUuid = obs_source_get_uuid(source);
			if (sourceUuid && strlen(sourceUuid) > 0)
				obs_data_set_string(slotData, "source_uuid", sourceUuid);
		}
		if (!slot->GetCustomTitle().isEmpty())
			obs_data_set_string(slotData, "title", QT_TO_UTF8(slot->GetCustomTitle()));
		obs_data_array_push_back(slotArray, slotData);
		obs_data_release(slotData);
	}
	obs_data_set_array(data, "slots", slotArray);
	obs_data_array_release(slotArray);
	return data;
}

void SwitcherWorkspaceDock::LoadContentState(obs_data_t *data, bool emitSignal)
{
	loadingState = true;

	for (const auto &slot : slotWidgets) {
		slot->SetCustomTitle(QString());
		slot->SetSource(nullptr);
	}

	int layoutIndex = layoutCombo->findData(4);
	if (data)
		layoutIndex = layoutCombo->findData(NormalizeVisibleSlotCount(static_cast<int>(obs_data_get_int(data, "visible_slots"))));
	if (layoutIndex < 0)
		layoutIndex = 0;
	layoutCombo->setCurrentIndex(layoutIndex);

	if (data) {
		obs_data_array_t *slotArray = obs_data_get_array(data, "slots");
		if (slotArray) {
			const size_t count = std::min(obs_data_array_count(slotArray), slotWidgets.size());
			for (size_t index = 0; index < count; index++) {
				obs_data_t *slotData = obs_data_array_item(slotArray, index);
				slotWidgets[index]->SetCustomTitle(QT_UTF8(obs_data_get_string(slotData, "title")));

				obs_source_t *source = ResolveStoredSource(slotData);
				slotWidgets[index]->SetSource(source);
				if (source)
					obs_source_release(source);

				obs_data_release(slotData);
			}
			obs_data_array_release(slotArray);
		}
	}

	loadingState = false;

	RefreshGrid();
	RefreshSlotList();
	RefreshSceneOptions();
	RefreshSelectedSlotEditor();
	ApplyPreviewState(isVisible());
	if (emitSignal)
		emit WorkspaceStateChanged();
}

void SwitcherWorkspaceDock::RegisterUndoRedoAction(const char *name, obs_data_t *before, obs_data_t *after, bool repeatable)
{
	if (applyingUndoRedo || !before || !after)
		return;

	const char *beforeJson = obs_data_get_json(before);
	const char *afterJson = obs_data_get_json(after);
	if (!beforeJson || !afterJson || strcmp(beforeJson, afterJson) == 0)
		return;

	obs_frontend_add_undo_redo_action(name, ApplyWorkspaceUndoState, ApplyWorkspaceUndoState, beforeJson, afterJson,
					  repeatable);
}

void SwitcherWorkspaceDock::ApplySerializedContentState(const char *json)
{
	if (!json || !strlen(json))
		return;

	obs_data_t *data = obs_data_create_from_json(json);
	if (!data)
		return;

	applyingUndoRedo = true;
	LoadContentState(data, true);
	applyingUndoRedo = false;
	obs_data_release(data);
}

obs_data_t *SwitcherWorkspaceDock::SaveState()
{
	blog(LOG_INFO, "[Switch] Workspace SaveState begin");
	obs_data_t *data = SaveContentState();
	obs_data_set_bool(data, "visible", isVisible());
	obs_data_set_bool(data, "inspector_visible", inspectorFrame->isVisible());
	obs_data_set_int(data, "inspector_width", inspectorFrame->isVisible() ? inspectorFrame->width() : inspectorWidth);
	obs_data_set_int(data, "mode_index", modeList ? modeList->currentRow() : 0);
	obs_data_set_int(data, "quick_multiview_monitor", quickMultiviewMonitor);
	obs_data_set_int(data, "quick_program_monitor", quickProgramMonitor);
	if (workspacePlacementInitialized)
		obs_data_set_string(data, "geometry", saveGeometry().toBase64().constData());

	obs_data_t *remoteData = SwitcherRemoteManager::Instance()->SaveState();
	obs_data_set_obj(data, kRemoteStateKey, remoteData);
	obs_data_release(remoteData);

	obs_data_t *canvasData = canvasManager ? canvasManager->SaveState() : obs_data_create();
	obs_data_set_obj(data, kCanvasStateKey, canvasData);
	obs_data_release(canvasData);

	obs_data_t *automationData = automationEngine ? automationEngine->SaveState() : obs_data_create();
	obs_data_set_obj(data, kAutomationStateKey, automationData);
	obs_data_release(automationData);

	obs_data_t *motionData = motionManager ? motionManager->SaveState() : obs_data_create();
	obs_data_set_obj(data, kMotionStateKey, motionData);
	obs_data_release(motionData);

	blog(LOG_INFO, "[Switch] Workspace SaveState end");
	return data;
}

void SwitcherWorkspaceDock::LoadState(obs_data_t *data)
{
	frontendShuttingDown = false;
	LoadContentState(data, false);

	workspacePlacementInitialized = false;
	int modeIndex = 0;
	if (data) {
		const char *geometry = obs_data_get_string(data, "geometry");
		if (geometry && strlen(geometry) > 0)
			workspacePlacementInitialized = restoreGeometry(QByteArray::fromBase64(QByteArray(geometry)));

		inspectorWidth = std::max(360, static_cast<int>(obs_data_get_int(data, "inspector_width")));
		HideSettingsPanel();

		if (obs_data_get_bool(data, "visible"))
			OpenWorkspaceWindow();
		else
			hide();

		modeIndex = std::clamp(static_cast<int>(obs_data_get_int(data, "mode_index")), 0, 4);
		quickMultiviewMonitor = obs_data_has_user_value(data, "quick_multiview_monitor")
						  ? ClampedMonitorIndex(static_cast<int>(
							    obs_data_get_int(data, "quick_multiview_monitor")))
						  : -2;
		quickProgramMonitor = obs_data_has_user_value(data, "quick_program_monitor")
						? ClampedMonitorIndex(
							  static_cast<int>(obs_data_get_int(data, "quick_program_monitor")))
						: -2;
	} else {
		HideSettingsPanel();
		modeIndex = 0;
		quickMultiviewMonitor = -2;
		quickProgramMonitor = -2;
	}

	obs_data_t *remoteData = data ? obs_data_get_obj(data, kRemoteStateKey) : nullptr;
	SwitcherRemoteManager::Instance()->LoadState(remoteData);
	if (remoteData)
		obs_data_release(remoteData);

	obs_data_t *canvasData = data ? obs_data_get_obj(data, kCanvasStateKey) : nullptr;
	if (canvasManager)
		canvasManager->LoadState(canvasData);
	if (canvasData)
		obs_data_release(canvasData);

	obs_data_t *automationData = data ? obs_data_get_obj(data, kAutomationStateKey) : nullptr;
	if (automationEngine)
		automationEngine->LoadState(automationData);
	if (automationData)
		obs_data_release(automationData);

	obs_data_t *motionData = data ? obs_data_get_obj(data, kMotionStateKey) : nullptr;
	if (motionManager)
		motionManager->LoadState(motionData);
	if (motionData)
		obs_data_release(motionData);

	{
		QSignalBlocker modeBlocker(modeList);
		modeList->setCurrentRow(modeIndex);
	}
	ModeChanged(modeIndex);
	UpdateQuickOutputControls();

	RemoteStateUpdated();
	RefreshVerticalPage();
	RefreshMotionPage();
	RefreshAutomationPage();
	emit WorkspaceStateChanged();
}

void SwitcherWorkspaceDock::UnregisterVerticalObsDocks()
{
	if (verticalObsDockContainer || verticalObsDockWidget)
		obs_frontend_remove_dock(kSwitchVerticalDockId);
	if (verticalScenesObsDockContainer || verticalScenesObsDockWidget)
		obs_frontend_remove_dock(kSwitchVerticalScenesDockId);
	if (verticalSourcesObsDockContainer || verticalSourcesObsDockWidget)
		obs_frontend_remove_dock(kSwitchVerticalSourcesDockId);
	if (verticalTransitionsObsDockContainer || verticalTransitionsObsDockWidget)
		obs_frontend_remove_dock(kSwitchVerticalTransitionsDockId);
	if (verticalSettingsObsDockContainer || verticalSettingsObsDockWidget)
		obs_frontend_remove_dock(kSwitchVerticalSettingsDockId);

	verticalObsDockContainer = nullptr;
	verticalObsDockWidget = nullptr;
	restoreVerticalObsDockOnHide = false;
	verticalScenesObsDockContainer = nullptr;
	verticalScenesObsDockWidget = nullptr;
	verticalSourcesObsDockContainer = nullptr;
	verticalSourcesObsDockWidget = nullptr;
	verticalTransitionsObsDockContainer = nullptr;
	verticalTransitionsObsDockWidget = nullptr;
	verticalSettingsObsDockContainer = nullptr;
	verticalSettingsObsDockWidget = nullptr;
}

void SwitcherWorkspaceDock::HandleSourceRemoved(obs_source_t *source)
{
	if (canvasManager)
		canvasManager->HandleSourceRemoved(source);
	if (motionManager)
		motionManager->HandleSourceRemoved(source);

	if (frontendShuttingDown)
		return;

	for (const auto &slot : slotWidgets)
		slot->ClearIfMatches(source);

	RefreshSlotList();
	RefreshSceneOptions();
	RefreshSelectedSlotEditor();
	ScheduleVerticalRefresh();
	RefreshMotionPage();
	RefreshAutomationPage();
	emit WorkspaceStateChanged();
}

void SwitcherWorkspaceDock::HandleFrontendEvent(enum obs_frontend_event event)
{
	if (canvasManager)
		canvasManager->HandleFrontendEvent(event);
	if (automationEngine)
		automationEngine->HandleFrontendEvent(event);
	if (motionManager)
		motionManager->HandleFrontendEvent(event);

	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		frontendFinishedLoading = true;
		frontendShuttingDown = false;
		frontendExiting = false;
		if (modeList->currentRow() == 1 && canvasManager)
			canvasManager->EnsureVerticalCanvas();
		if (motionManager)
			motionManager->ApplyBindings();
		InstallQuickOutputControls();
		EnsureVerticalObsDock(false);
		EnsureVerticalScenesObsDock(false);
		EnsureVerticalSourcesObsDock(false);
		EnsureVerticalTransitionsObsDock(false);
		EnsureVerticalSettingsObsDock(false);
		ScheduleVerticalRefresh();
		RefreshMotionPage();
		RefreshAutomationPage();
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STREAMING_STARTING:
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
	case OBS_FRONTEND_EVENT_RECORDING_STARTING:
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
	case OBS_FRONTEND_EVENT_RECORDING_STOPPING:
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
	case OBS_FRONTEND_EVENT_RECORDING_PAUSED:
	case OBS_FRONTEND_EVENT_RECORDING_UNPAUSED:
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTING:
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED:
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPING:
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPED:
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED:
	case OBS_FRONTEND_EVENT_VIRTUALCAM_STARTED:
	case OBS_FRONTEND_EVENT_VIRTUALCAM_STOPPED:
			for (int index = 0; index < VisibleSlotCount(); index++)
				slotWidgets[index]->RefreshActiveState();
		RefreshSlotList();
		RefreshOutputActions();
		ScheduleVerticalRefresh();
		RefreshMotionPage();
		RefreshAutomationPage();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
	case OBS_FRONTEND_EVENT_EXIT:
		frontendShuttingDown = true;
		if (event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN || event == OBS_FRONTEND_EVENT_EXIT) {
			frontendExiting = true;
			if (motionManager && event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN) {
				motionManager->SetBindingsQuiesced(true);
			}
		}
		if (motionManager && event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING) {
			motionManager->DetachFilters();
		}
		verticalRefreshPending = false;
		suppressNextVerticalRefresh = false;
		HideSettingsPanel();
		ClearSceneCollectionState(true, frontendExiting);
		ApplyPreviewState(false);
		RemoveQuickOutputControls();
		UnregisterVerticalObsDocks();
		break;
	default:
		break;
	}
	if (!frontendShuttingDown)
		emit WorkspaceStateChanged();
}

void SwitcherWorkspaceDock::ClearSceneCollectionState(bool duringFrontendShutdown, bool preserveMotionState)
{
	loadingState = true;
	for (const auto &slot : slotWidgets) {
		slot->SetCustomTitle(QString());
		slot->SetSource(nullptr);
	}
	if (motionScenePreview) {
		motionScenePreview->SetPreviewActive(false);
		motionScenePreview->SetSource(nullptr);
	}
	const int layoutIndex = layoutCombo->findData(4);
	if (layoutIndex >= 0)
		layoutCombo->setCurrentIndex(layoutIndex);
	loadingState = false;

	if (canvasManager) {
		if (duringFrontendShutdown)
			canvasManager->ReleaseRuntimeReferencesForShutdown();
		else
			canvasManager->Reset();
	}
	if (automationEngine)
		automationEngine->Reset();
	if (motionManager) {
		if (duringFrontendShutdown && preserveMotionState) {
			/* Keep Motion state persisted, but remove runtime filters before OBS clears sources. */
			motionManager->DetachFilters();
		} else if (duringFrontendShutdown) {
			motionManager->Reset(false);
		} else if (preserveMotionState) {
			motionManager->DetachFilters();
		} else {
			motionManager->Reset();
		}
	}

	RefreshGrid();
	RefreshSlotList();
	RefreshSceneOptions();
	RefreshSelectedSlotEditor();
	if (duringFrontendShutdown) {
		if (verticalSceneList)
			verticalSceneList->clear();
		if (verticalSourceTree)
			verticalSourceTree->clear();
		if (verticalSceneLinksList)
			verticalSceneLinksList->clear();
		if (verticalCanvasStatusLabel)
			verticalCanvasStatusLabel->setText(QStringLiteral("Vertical canvas unavailable during shutdown."));
		if (verticalOutputStatusLabel)
			verticalOutputStatusLabel->setText(QStringLiteral("OBS Output Shortcuts - Main OBS: Idle"));
		return;
	}

	ScheduleVerticalRefresh();
	RefreshMotionPage();
	RefreshAutomationPage();
	emit WorkspaceStateChanged();
}

void SwitcherWorkspaceDock::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	RefreshSceneOptions();
	RefreshSlotList();
	RefreshSelectedSlotEditor();
	ScheduleVerticalRefresh();
	RefreshAutomationPage();
	if (verticalObsDockWidget && modeStack->currentWidget() == verticalModePage)
		verticalObsDockWidget->SetPreviewRenderingEnabled(false);
	ApplyPreviewState(true);
	for (int index = 0; index < VisibleSlotCount(); index++)
		slotWidgets[index]->RefreshActiveState();
}

void SwitcherWorkspaceDock::hideEvent(QHideEvent *event)
{
	ApplyPreviewState(false);
	QWidget::hideEvent(event);
	if (restoreVerticalObsDockOnHide && verticalObsDockContainer) {
		restoreVerticalObsDockOnHide = false;
		verticalObsDockContainer->setFloating(false);
		verticalObsDockContainer->show();
		verticalObsDockContainer->raise();
	}
	if (verticalObsDockContainer && verticalObsDockContainer->isVisible() && verticalObsDockWidget) {
		QPointer<SwitchVerticalDockWidget> dockWidget(verticalObsDockWidget);
		QTimer::singleShot(0, this, [dockWidget]() {
			if (dockWidget)
				dockWidget->SetPreviewRenderingEnabled(true);
		});
	}
	RefreshVerticalDockSurfaces();
}

void SwitcherWorkspaceDock::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
}

void SwitcherWorkspaceDock::changeEvent(QEvent *event)
{
	QWidget::changeEvent(event);

	switch (event->type()) {
	case QEvent::PaletteChange:
	case QEvent::StyleChange:
	case QEvent::ApplicationPaletteChange:
		ApplyTheme();
		break;
	case QEvent::WindowStateChange:
		ApplyPreviewState(isVisible() && !isMinimized());
		break;
	default:
		break;
	}
}

void SwitcherWorkspaceDock::LayoutChanged()
{
	if (loadingState)
		return;

	obs_data_t *before = SaveContentState();
	RefreshGrid();
	RefreshSlotList();
	RefreshSceneOptions();
	RefreshSelectedSlotEditor();
	obs_data_t *after = SaveContentState();
	RegisterUndoRedoAction(obs_module_text("SwitcherUndoLayout"), before, after);
	obs_data_release(before);
	obs_data_release(after);
	emit WorkspaceStateChanged();
}

void SwitcherWorkspaceDock::SelectedSlotChanged()
{
	RefreshSelectionHighlights();
	RefreshSceneOptions();
	RefreshSelectedSlotEditor();
	if (inspectorFrame->isVisible()) {
		RefreshSettingsPanelMode();
	}
}

void SwitcherWorkspaceDock::SelectedSceneChanged()
{
	if (loadingState)
		return;

	auto *slot = CurrentSlot();
	if (!slot)
		return;

	obs_data_t *before = SaveContentState();
	const auto sourceUuid = sceneCombo->currentData().toByteArray();
	obs_source_t *source = sourceUuid.isEmpty() ? nullptr : obs_get_source_by_uuid(sourceUuid.constData());
	slot->SetSource(source);
	if (source)
		obs_source_release(source);

	RefreshSlotList();
	RefreshSelectedSlotEditor();
	if (inspectorFrame->isVisible())
		RefreshSettingsPanelMode();
	obs_data_t *after = SaveContentState();
	RegisterUndoRedoAction(obs_module_text("SwitcherUndoAssignScene"), before, after);
	obs_data_release(before);
	obs_data_release(after);
	emit WorkspaceStateChanged();
}

void SwitcherWorkspaceDock::SelectedTitleChanged(const QString &title)
{
	if (loadingState)
		return;

	auto *slot = CurrentSlot();
	if (!slot)
		return;

	obs_data_t *before = SaveContentState();
	slot->SetCustomTitle(title);
	RefreshSlotList();
	obs_data_t *after = SaveContentState();
	RegisterUndoRedoAction(obs_module_text("SwitcherUndoRenameView"), before, after, true);
	obs_data_release(before);
	obs_data_release(after);
	if (inspectorFrame->isVisible())
		RefreshSettingsPanelMode();
	emit WorkspaceStateChanged();
}

void SwitcherWorkspaceDock::ClearSelectedSlot()
{
	auto *slot = CurrentSlot();
	if (!slot)
		return;

	obs_data_t *before = SaveContentState();
	loadingState = true;
	slot->SetCustomTitle(QString());
	slot->SetSource(nullptr);
	loadingState = false;

	RefreshSlotList();
	RefreshSceneOptions();
	RefreshSelectedSlotEditor();
	if (inspectorFrame->isVisible())
		RefreshSettingsPanelMode();
	obs_data_t *after = SaveContentState();
	RegisterUndoRedoAction(obs_module_text("SwitcherUndoClearView"), before, after);
	obs_data_release(before);
	obs_data_release(after);
	emit WorkspaceStateChanged();
}

void SwitcherWorkspaceDock::OpenSelectedSlotAsDock()
{
	auto *slot = CurrentSlot();
	if (!slot)
		return;

	auto source = slot->GetSource();
	if (!source)
		return;

	const auto title = slot->GetEffectiveTitle();
	auto *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());

	SwitcherDockRegistrationOptions options;
	options.dockId = SwitcherDock::CreateDockId();
	options.preview = true;
	options.switchScene = true;
	options.showActive = true;
	options.visible = true;
	options.applyPlacement = true;
	options.dockArea = Qt::RightDockWidgetArea;
	options.floating = true;

	CreateRegisteredSwitcherDock(title, source, mainWindow, options);
}

void SwitcherWorkspaceDock::OpenSelectedSlotAsVerticalDock()
{
	auto *slot = CurrentSlot();
	if (!slot)
		return;

	auto source = slot->GetSource();
	if (!source)
		return;

	const auto title = QString("%1 (%2)").arg(slot->GetEffectiveTitle(), QT_UTF8(obs_module_text("SwitcherVerticalFormat")));
	auto *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());

	SwitcherDockRegistrationOptions options;
	options.dockId = SwitcherDock::CreateDockId();
	options.preview = true;
	options.switchScene = true;
	options.showActive = true;
	options.visible = true;
	options.applyPlacement = true;
	options.dockArea = Qt::RightDockWidgetArea;
	options.floating = true;
	options.floatingSize = QSize(540, 960);

	CreateRegisteredSwitcherDock(title, source, mainWindow, options);
}

void SwitcherWorkspaceDock::OpenSelectedSlotProjector()
{
	auto *slot = CurrentSlot();
	if (!slot || !slot->GetSource())
		return;

	auto source = slot->GetSource();
	const char *name = obs_source_get_name(source);
	if (!name || !*name)
		return;

	obs_frontend_open_projector(obs_source_is_scene(source) ? "Scene" : "Source", -1, nullptr, name);
}

void SwitcherWorkspaceDock::ToggleSelectedSlotRecording()
{
	PerformOutputAction(QStringLiteral("toggle_recording"));
}

void SwitcherWorkspaceDock::ToggleSelectedSlotRecordingPause()
{
	PerformOutputAction(QStringLiteral("toggle_recording_pause"));
}

void SwitcherWorkspaceDock::SplitSelectedSlotRecording()
{
	PerformOutputAction(QStringLiteral("split_recording"));
}

void SwitcherWorkspaceDock::ToggleSelectedSlotReplayBuffer()
{
	PerformOutputAction(QStringLiteral("toggle_replay"));
}

void SwitcherWorkspaceDock::SaveSelectedSlotReplayBuffer()
{
	PerformOutputAction(QStringLiteral("save_replay"));
}

void SwitcherWorkspaceDock::ToggleSelectedSlotStreaming()
{
	PerformOutputAction(QStringLiteral("toggle_streaming"));
}

void SwitcherWorkspaceDock::ToggleSelectedSlotVirtualCam()
{
	PerformOutputAction(QStringLiteral("toggle_virtual_camera"));
}

void SwitcherWorkspaceDock::OpenSlotAsDock(int slotIndex)
{
	if (slotIndex < 0 || slotIndex >= VisibleSlotCount())
		return;

	if (slotList->currentRow() != slotIndex)
		slotList->setCurrentRow(slotIndex);

	OpenSelectedSlotAsDock();
}

void SwitcherWorkspaceDock::OpenSlotAsVerticalDock(int slotIndex)
{
	if (slotIndex < 0 || slotIndex >= VisibleSlotCount())
		return;

	if (slotList->currentRow() != slotIndex)
		slotList->setCurrentRow(slotIndex);

	OpenSelectedSlotAsVerticalDock();
}

void SwitcherWorkspaceDock::RemoteStateUpdated()
{
	RefreshRemoteControls();
}

void SwitcherWorkspaceDock::RemoteEnabledChanged(Qt::CheckState state)
{
	SwitcherRemoteManager::Instance()->SetEnabled(state == Qt::CheckState::Checked);
}

void SwitcherWorkspaceDock::RemoteAutoStartChanged(Qt::CheckState state)
{
	SwitcherRemoteManager::Instance()->SetAutoStart(state == Qt::CheckState::Checked);
}

void SwitcherWorkspaceDock::RemoteResolutionChanged(int)
{
	SwitcherRemoteManager::Instance()->SetRenderSize(remoteResolutionCombo->currentData().toSize());
}

void SwitcherWorkspaceDock::RemoteFpsChanged(int)
{
	SwitcherRemoteManager::Instance()->SetTargetFps(remoteFpsCombo->currentData().toInt());
}

void SwitcherWorkspaceDock::CopyRemoteUrl()
{
	QGuiApplication::clipboard()->setText(SwitcherRemoteManager::Instance()->Url());
}

void SwitcherWorkspaceDock::RegenerateRemoteToken()
{
	SwitcherRemoteManager::Instance()->RegenerateToken();
}

void SwitcherWorkspaceDock::RestartRemote()
{
	SwitcherRemoteManager::Instance()->Restart();
}

bool SwitcherWorkspaceDock::AddSceneOption(void *data, obs_source_t *source)
{
	const char *sourceName = obs_source_get_name(source);
	const char *sourceUuid = obs_source_get_uuid(source);
	auto *combo = static_cast<QComboBox *>(data);
	combo->addItem(QT_UTF8(sourceName), sourceUuid ? QByteArray(sourceUuid) : QByteArray());
	return true;
}

void SwitcherWorkspaceDock::ApplyPreviewState(bool enabled)
{
	previewsEnabled = enabled;
	const bool workspaceModeVisible =
		enabled && modeStack && modeStack->currentWidget() == workspaceModePage;
	const int visibleSlots = VisibleSlotCount();
	for (size_t index = 0; index < slotWidgets.size(); index++)
		slotWidgets[index]->SetPreviewActive(workspaceModeVisible && static_cast<int>(index) < visibleSlots);
	if (motionScenePreview) {
		const bool motionModeVisible =
			enabled && modeStack && modeStack->currentWidget() == motionModePage;
		motionScenePreview->SetPreviewActive(motionModeVisible && motionScenePreview->GetSource());
	}
}

SwitcherWorkspaceSlot *SwitcherWorkspaceDock::CurrentSlot() const
{
	const int index = SelectedSlotIndex();
	if (index < 0 || index >= VisibleSlotCount())
		return nullptr;
	return slotWidgets[index];
}

int SwitcherWorkspaceDock::SelectedSlotIndex() const
{
	return slotList->currentRow();
}

void SwitcherWorkspaceDock::RefreshGrid()
{
	while (auto *item = gridLayout->takeAt(0)) {
		if (item->widget())
			item->widget()->hide();
		delete item;
	}

	const int visibleSlots = VisibleSlotCount();
	const int columns = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(visibleSlots)))));

	for (int index = 0; index < visibleSlots; index++) {
		slotWidgets[index]->show();
		gridLayout->addWidget(slotWidgets[index], index / columns, index % columns);
	}

	for (int index = visibleSlots; index < static_cast<int>(slotWidgets.size()); index++)
		slotWidgets[index]->hide();

	ApplyPreviewState(isVisible() && previewsEnabled);
	RefreshSelectionHighlights();
}

void SwitcherWorkspaceDock::RefreshSceneOptions()
{
	auto *slot = CurrentSlot();
	const QByteArray selectedSourceUuid =
		slot && slot->GetSource() ? QByteArray(obs_source_get_uuid(slot->GetSource())) : QByteArray();
	const QString selectedSourceName =
		slot && slot->GetSource() ? QT_UTF8(obs_source_get_name(slot->GetSource())) : QString();

	QSignalBlocker blocker(sceneCombo);
	sceneCombo->clear();
	sceneCombo->addItem(QT_UTF8(obs_module_text("SwitcherNoScene")), QByteArray(""));

	struct obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t index = 0; index < scenes.sources.num; index++)
		AddSceneOption(sceneCombo, scenes.sources.array[index]);
	obs_frontend_source_list_free(&scenes);

	if (!selectedSourceUuid.isEmpty()) {
		int index = sceneCombo->findData(selectedSourceUuid);
		if (index < 0) {
			sceneCombo->addItem(selectedSourceName, selectedSourceUuid);
			index = sceneCombo->count() - 1;
		}
		sceneCombo->setCurrentIndex(index);
	} else {
		sceneCombo->setCurrentIndex(0);
	}
}

void SwitcherWorkspaceDock::RefreshSelectedSlotEditor()
{
	auto *slot = CurrentSlot();
	const bool hasSlot = slot != nullptr;

	QSignalBlocker titleBlocker(titleEdit);
	titleEdit->setText(hasSlot ? slot->GetCustomTitle() : QString());

	sceneCombo->setEnabled(hasSlot);
	titleEdit->setEnabled(hasSlot);
	clearSlotButton->setEnabled(hasSlot && (slot->GetSource() || !slot->GetCustomTitle().isEmpty()));
	detachSlotButton->setEnabled(hasSlot && slot->GetSource());
	verticalDetachSlotButton->setEnabled(hasSlot && slot->GetSource());
	RefreshOutputActions();
}

void SwitcherWorkspaceDock::RefreshOutputActions()
{
	auto *slot = CurrentSlot();
	const bool hasSlot = slot != nullptr;
	const bool hasSource = hasSlot && slot->GetSource();
	const bool frontendReady = FrontendApisAvailable();

	const bool recordingActive = frontendReady && obs_frontend_recording_active();
	const bool recordingPaused = frontendReady && obs_frontend_recording_paused();
	const bool replayActive = frontendReady && obs_frontend_replay_buffer_active();
	const bool streamingActive = frontendReady && obs_frontend_streaming_active();
	const bool virtualCamActive = frontendReady && obs_frontend_virtualcam_active();

	slotOutputSection->setEnabled(hasSlot);
	slotProjectorButton->setText(QT_UTF8(obs_module_text("SwitcherOpenProjector")));
	slotProjectorButton->setEnabled(hasSource);
	slotRecordButton->setText(
		QT_UTF8(obs_module_text(recordingActive ? "SwitcherStopRecording" : "SwitcherStartRecording")));
	slotPauseRecordButton->setText(
		QT_UTF8(obs_module_text(recordingPaused ? "SwitcherResumeRecording" : "SwitcherPauseRecording")));
	slotPauseRecordButton->setEnabled(hasSlot && recordingActive);
	slotSplitRecordButton->setText(QT_UTF8(obs_module_text("SwitcherSplitRecording")));
	slotSplitRecordButton->setEnabled(hasSlot && recordingActive && !recordingPaused);
	slotReplayButton->setText(QT_UTF8(obs_module_text(replayActive ? "SwitcherStopReplay" : "SwitcherStartReplay")));
	slotReplaySaveButton->setText(QT_UTF8(obs_module_text("SwitcherSaveReplay")));
	slotReplaySaveButton->setEnabled(hasSlot && replayActive);
	slotStreamButton->setText(QT_UTF8(obs_module_text(streamingActive ? "SwitcherStopStreaming" : "SwitcherStartStreaming")));
	slotVirtualCamButton->setText(
		QT_UTF8(obs_module_text(virtualCamActive ? "SwitcherStopVirtualCamera" : "SwitcherStartVirtualCamera")));

	slotRecordButton->setEnabled(hasSlot);
	slotReplayButton->setEnabled(hasSlot);
	slotStreamButton->setEnabled(hasSlot);
	slotVirtualCamButton->setEnabled(hasSlot);

	std::vector<QString> statuses;
	if (streamingActive)
		statuses.emplace_back(QT_UTF8(obs_module_text("Streaming")));
	if (recordingPaused)
		statuses.emplace_back(QT_UTF8(obs_module_text("RecordingPaused")));
	else if (recordingActive)
		statuses.emplace_back(QT_UTF8(obs_module_text("Recording")));
	if (replayActive)
		statuses.emplace_back(QT_UTF8(obs_module_text("SwitcherReplayBuffer")));
	if (virtualCamActive)
		statuses.emplace_back(QT_UTF8(obs_module_text("SwitcherVirtualCamera")));

	QString statusText;
	for (size_t index = 0; index < statuses.size(); index++) {
		if (index > 0)
			statusText += QStringLiteral(" | ");
		statusText += statuses[index];
	}
	if (statusText.isEmpty())
		statusText = QT_UTF8(obs_module_text("SwitcherIdle"));

	slotOutputStatusLabel->setText(QStringLiteral("Main OBS: %1").arg(statusText));
}

void SwitcherWorkspaceDock::RefreshSlotList()
{
	const int currentRow = slotList->currentRow();
	const int visibleSlots = VisibleSlotCount();

	QSignalBlocker blocker(slotList);
	slotList->clear();

	for (int index = 0; index < visibleSlots; index++) {
		auto title = slotWidgets[index]->GetEffectiveTitle();
		if (!slotWidgets[index]->GetSource())
			title += QString(" (%1)").arg(QT_UTF8(obs_module_text("SwitcherNoScene")));
		slotList->addItem(QString("%1. %2").arg(index + 1).arg(title));
	}

	if (visibleSlots > 0)
		slotList->setCurrentRow(std::clamp(currentRow, 0, visibleSlots - 1));

	RefreshSelectionHighlights();
}

void SwitcherWorkspaceDock::RefreshRemoteControls()
{
	auto *remote = SwitcherRemoteManager::Instance();

	QSignalBlocker enabledBlocker(remoteEnabledCheckBox);
	QSignalBlocker autoStartBlocker(remoteAutoStartCheckBox);
	QSignalBlocker resolutionBlocker(remoteResolutionCombo);
	QSignalBlocker fpsBlocker(remoteFpsCombo);

	remoteEnabledCheckBox->setChecked(remote->Enabled());
	remoteAutoStartCheckBox->setChecked(remote->AutoStart());
	remoteAutoStartCheckBox->setEnabled(remote->Enabled());

	const int resolutionIndex = remoteResolutionCombo->findData(remote->RenderSize());
	if (resolutionIndex >= 0)
		remoteResolutionCombo->setCurrentIndex(resolutionIndex);

	const int fpsIndex = remoteFpsCombo->findData(remote->TargetFps());
	if (fpsIndex >= 0)
		remoteFpsCombo->setCurrentIndex(fpsIndex);

	remoteResolutionCombo->setEnabled(remote->Enabled());
	remoteFpsCombo->setEnabled(remote->Enabled());
	remoteUrlEdit->setText(remote->Url());
	remoteStatusLabel->setText(remote->StatusText());
	remoteCopyButton->setEnabled(remote->Enabled() && !remote->Url().isEmpty());
	remoteRegenerateButton->setEnabled(remote->Enabled());
	remoteRestartButton->setEnabled(remote->Enabled());
}

void SwitcherWorkspaceDock::RefreshSelectionHighlights()
{
	const bool editingSlots = inspectorFrame->isVisible() && settingsPanelMode == SettingsPanelMode::SlotInspector;
	const int visibleSlots = VisibleSlotCount();
	const int selectedIndex = editingSlots ? SelectedSlotIndex() : -1;

	for (int index = 0; index < static_cast<int>(slotWidgets.size()); index++) {
		auto *slot = slotWidgets[static_cast<size_t>(index)];
		const bool slotVisible = index < visibleSlots;
		slot->SetSelected(slotVisible && index == selectedIndex);
	}
}

void SwitcherWorkspaceDock::UpdateInspectorVisibility(bool visible)
{
	if (!contentSplitter || !inspectorFrame)
		return;

	if (!visible) {
		if (inspectorFrame->isVisible())
			inspectorWidth = std::max(inspectorFrame->minimumWidth(), inspectorFrame->width());
		inspectorFrame->hide();
		contentSplitter->setSizes({1, 0});
		RefreshSelectionHighlights();
		return;
	}

	inspectorFrame->show();
	const int targetWidth = std::clamp(inspectorWidth, inspectorFrame->minimumWidth(), inspectorFrame->maximumWidth());
	const int totalWidth = std::max(width(), minimumWidth());
	contentSplitter->setSizes({std::max(320, totalWidth - targetWidth), targetWidth});
	RefreshSelectionHighlights();
}

void SwitcherWorkspaceDock::ApplyDefaultWindowGeometry()
{
	QScreen *screen = nullptr;
	if (windowHandle())
		screen = windowHandle()->screen();
	if (!screen)
		screen = QGuiApplication::screenAt(mapToGlobal(rect().center()));
	if (!screen)
		screen = QGuiApplication::primaryScreen();
	if (!screen)
		return;

	const QRect available = screen->availableGeometry();
	const int horizontalInset = std::max(available.width() / 14, 56);
	const int verticalInset = std::max(available.height() / 12, 48);
	QRect target = available.adjusted(horizontalInset, verticalInset, -horizontalInset, -verticalInset);
	const QSize minimum = minimumSize().boundedTo(available.size());

	if (target.width() < minimum.width() || target.height() < minimum.height()) {
		target.setSize(target.size().expandedTo(minimum).boundedTo(available.size()));
		target.moveCenter(available.center());
	}

	if (target.left() < available.left())
		target.moveLeft(available.left());
	if (target.top() < available.top())
		target.moveTop(available.top());
	if (target.right() > available.right())
		target.moveRight(available.right());
	if (target.bottom() > available.bottom())
		target.moveBottom(available.bottom());

	setGeometry(target);
	workspacePlacementInitialized = true;
}

void SwitcherWorkspaceDock::OpenSlotSettings(int slotIndex)
{
	if (slotIndex < 0 || slotIndex >= VisibleSlotCount())
		return;

	if (slotList->currentRow() != slotIndex)
		slotList->setCurrentRow(slotIndex);

	ShowSettingsPanel(SettingsPanelMode::SlotInspector);
}

void SwitcherWorkspaceDock::OpenAdvancedSettings(int slotIndex)
{
	if (slotIndex < 0 || slotIndex >= VisibleSlotCount())
		return;

	if (slotList->currentRow() != slotIndex)
		slotList->setCurrentRow(slotIndex);

	ShowSettingsPanel(SettingsPanelMode::Workspace);
}

void SwitcherWorkspaceDock::OpenSelectedSlotInspector()
{
	OpenSlotSettings(SelectedSlotIndex());
}

QString SwitcherWorkspaceDock::CurrentSlotLabel() const
{
	if (auto *slot = CurrentSlot())
		return slot->GetEffectiveTitle();
	return QString("%1 1").arg(QT_UTF8(obs_module_text("SwitcherSlot")));
}

void SwitcherWorkspaceDock::RefreshSettingsPanelMode()
{
	const bool slotInspector = settingsPanelMode == SettingsPanelMode::SlotInspector;
	inspectorTitleLabel->setText(slotInspector ? CurrentSlotLabel() : QT_UTF8(obs_module_text("SwitcherAdvancedSettings")));
	inspectorStack->setCurrentWidget(slotInspector ? slotPage : workspacePage);
	inspectorModeButton->setVisible(slotInspector);
	inspectorModeButton->setText(QT_UTF8(obs_module_text("SwitcherAdvanced")));
	inspectorModeButton->setToolTip(QT_UTF8(obs_module_text("SwitcherOpenAdvancedSettings")));
	inspectorModeButton->setIcon(style()->standardIcon(QStyle::SP_ArrowLeft));
}

void SwitcherWorkspaceDock::ShowSettingsPanel(SettingsPanelMode mode)
{
	settingsPanelMode = mode;
	RefreshSceneOptions();
	RefreshSelectedSlotEditor();
	RefreshRemoteControls();
	RefreshSettingsPanelMode();
	UpdateInspectorVisibility(true);
	RefreshSelectionHighlights();
}

void SwitcherWorkspaceDock::HideSettingsPanel()
{
	settingsPanelMode = SettingsPanelMode::Workspace;
	UpdateInspectorVisibility(false);
	RefreshSelectionHighlights();
}

void SwitcherWorkspaceDock::ApplyTheme()
{
	if (applyingTheme)
		return;

	QScopedValueRollback<bool> themeGuard(applyingTheme, true);

	const QPalette pal = palette();
	const QColor window = pal.color(QPalette::Window);
	const QColor base = pal.color(QPalette::Base);
	const QColor button = pal.color(QPalette::Button);
	const QColor text = pal.color(QPalette::Text);
	const QColor buttonText = pal.color(QPalette::ButtonText);
	const QColor windowText = pal.color(QPalette::WindowText);
	const QColor disabledText = pal.color(QPalette::Disabled, QPalette::Text);
	const QColor mid = pal.color(QPalette::Mid);
	const QColor highlight = pal.color(QPalette::Highlight);
	const QColor black(0, 0, 0);

	const QColor rootBackground = Blend(base, black, 0.08);
	const QColor pageBackground = Blend(base, black, 0.16);
	const QColor railBackground = Blend(button, window, 0.06);
	const QColor surfaceBackground = button;
	const QColor cardBackground = button.lighter(106);
	const QColor slotBackground = cardBackground;
	const QColor slotBorder = WithAlpha(button.lighter(135), 230);
	const QColor mutedText = WithAlpha(disabledText.isValid() ? disabledText : text, 220);
	const QColor slotChipBackground = Blend(slotBackground, window, 0.22);
	const QColor slotChipBorder = WithAlpha(mid, 148);
	const QColor slotChipText = WithAlpha(windowText, 235);
	const QColor hoverFill = WithAlpha(highlight, 28);
	const QColor hoverBorder = WithAlpha(highlight, 128);
	const QColor selectedFill = WithAlpha(highlight, 42);
	const QColor selectedBorder = WithAlpha(highlight, 220);
	const QColor popupBackground = surfaceBackground;
	const QColor popupCardBackground = cardBackground;
	const QColor popupFieldBackground = Blend(button, base, 0.16);
	const QColor popupBorder = WithAlpha(button.lighter(110), 170);
	const QColor popupButtonHover = Blend(button, highlight, 0.16);
	const QColor popupButtonPressed = Blend(button, highlight, 0.24);
	const QColor disabledFieldBackground = Blend(popupFieldBackground, button, 0.18);
	const QColor disabledButtonBackground = Blend(button, pageBackground, 0.28);
	const QColor disabledBorder = WithAlpha(mid, 135);
	const QColor disabledReadableText = WithAlpha(disabledText.isValid() ? disabledText : text, 210);
	const QColor sectionBackground = Blend(cardBackground, pageBackground, 0.28);

	QString styleSheet = QStringLiteral(
		"#switcherWorkspaceRoot {"
		"  background-color: @ROOT@;"
		"}"
		"#switcherModeRail {"
		"  background-color: @RAIL_BG@;"
		"  border: 1px solid @PANEL_BORDER@;"
		"  border-radius: 6px;"
		"}"
		"#switcherModeTitle {"
		"  color: @WINDOW_TEXT@;"
		"  font-size: 16px;"
		"  font-weight: 700;"
		"  padding: 8px 10px 4px 10px;"
		"}"
		"#switcherModeList {"
		"  background-color: transparent;"
		"  border: 0px;"
		"  outline: none;"
		"}"
			"#switcherModeList::item {"
			"  color: @WINDOW_TEXT@;"
			"  border: 1px solid transparent;"
			"  border-radius: 4px;"
			"  padding: 10px 12px;"
			"}"
		"#switcherModeList::item:hover {"
		"  background-color: @HOVER_FILL@;"
		"  border-color: @HOVER_BORDER@;"
		"}"
		"#switcherModeList::item:selected {"
		"  background-color: @SELECT_BORDER@;"
		"  color: @BUTTON_TEXT@;"
		"  border-color: @SELECT_BORDER@;"
		"}"
		"#switcherModeStack {"
		"  background-color: @PAGE_BG@;"
		"  border-radius: 6px;"
		"}"
		"#switcherModePage,"
		"#switcherWorkspaceGridPage {"
		"  background-color: @PAGE_BG@;"
		"  border-radius: 6px;"
		"}"
		"#switcherWorkspaceScrollArea,"
		"#switcherSettingsScrollArea {"
		"  background-color: @PAGE_BG@;"
		"  border: 0px;"
		"}"
		"#switcherSettingsPane {"
		"  background-color: @CARD_BG@;"
		"  border: 1px solid @PANEL_BORDER@;"
		"  border-radius: 6px;"
		"}"
		"#switcherSettingsSection {"
		"  background-color: @SECTION_BG@;"
		"  border: 1px solid @PANEL_BORDER@;"
		"  border-radius: 6px;"
		"}"
		"#switcherWorkspaceRoot QSplitter::handle {"
		"  background-color: transparent;"
		"}"
		"#switcherWorkspaceSlot {"
		"  background-color: @SLOT_BG@;"
		"  border: 1px solid @SLOT_BORDER@;"
		"  border-radius: 6px;"
		"}"
		"#switcherWorkspaceSlotContent {"
		"  background-color: transparent;"
		"  border: 0px;"
		"}"
		"#switcherWorkspaceSlotEmpty {"
		"  color: @MUTED_TEXT@;"
		"  font-size: 15px;"
		"  font-weight: 600;"
		"}"
		"#switcherWorkspaceSlotTitle {"
		"  background-color: transparent;"
		"  color: @WINDOW_TEXT@;"
		"  border: 0px;"
		"  border-radius: 0px;"
		"  padding: 0 6px;"
		"  font-size: 11px;"
		"  font-weight: 600;"
		"}"
		"#switcherWorkspaceSlotTitle[selected=\"true\"] {"
		"  color: @BUTTON_TEXT@;"
		"}"
		"#switcherWorkspaceInspector {"
		"  background-color: @CARD_BG@;"
		"  border: 1px solid @PANEL_BORDER@;"
		"  border-radius: 6px;"
		"}"
		"#switcherWorkspaceInspectorHeader {"
		"  background-color: @CARD_BG@;"
		"  border-bottom: 1px solid @PANEL_BORDER@;"
		"  border-top-left-radius: 6px;"
		"  border-top-right-radius: 6px;"
		"}"
		"#switcherModePage QLabel,"
		"#switcherModePage QCheckBox {"
		"  color: @WINDOW_TEXT@;"
		"}"
		"#switcherModePage QGroupBox {"
		"  background-color: @CARD_BG@;"
		"  color: @WINDOW_TEXT@;"
		"  border: 1px solid @PANEL_BORDER@;"
		"  border-radius: 6px;"
		"  margin-top: 12px;"
		"  padding: 12px;"
		"}"
		"#switcherModePage QGroupBox::title {"
		"  subcontrol-origin: margin;"
		"  left: 10px;"
		"  padding: 0px 4px;"
		"}"
		"#switcherModePage QListWidget,"
		"#switcherModePage QTreeWidget,"
		"#switcherModePage QComboBox,"
		"#switcherModePage QLineEdit,"
		"#switcherModePage QSpinBox,"
		"#switcherModePage QDoubleSpinBox {"
		"  background-color: @FIELD_BG@;"
		"  color: @FIELD_TEXT@;"
		"  border: 1px solid @PANEL_BORDER@;"
		"  border-radius: 5px;"
		"  padding: 7px 10px;"
		"}"
		"#switcherModePage QComboBox:disabled,"
		"#switcherModePage QLineEdit:disabled,"
		"#switcherModePage QSpinBox:disabled,"
		"#switcherModePage QDoubleSpinBox:disabled {"
		"  background-color: @DISABLED_FIELD_BG@;"
		"  color: @DISABLED_TEXT@;"
		"  border-color: @DISABLED_BORDER@;"
		"}"
		"#switcherModePage QListWidget,"
		"#switcherModePage QTreeWidget {"
		"  outline: none;"
		"}"
		"#switcherModePage QSlider#switcherMotionSlider {"
		"  min-height: 28px;"
		"}"
		"#switcherModePage QSlider#switcherMotionSlider::groove:horizontal {"
		"  background-color: @FIELD_BG@;"
		"  border: 1px solid @PANEL_BORDER@;"
		"  border-radius: 3px;"
		"  height: 6px;"
		"}"
		"#switcherModePage QSlider#switcherMotionSlider::sub-page:horizontal {"
		"  background-color: @SELECT_BORDER@;"
		"  border-radius: 3px;"
		"}"
		"#switcherModePage QSlider#switcherMotionSlider::handle:horizontal {"
		"  background-color: @BUTTON_TEXT@;"
		"  border: 1px solid @PANEL_BORDER@;"
		"  border-radius: 8px;"
		"  width: 16px;"
		"  margin: -6px 0px;"
		"}"
		"#switcherModePage QSlider#switcherMotionSlider:disabled::groove:horizontal {"
		"  background-color: @DISABLED_FIELD_BG@;"
		"  border-color: @DISABLED_BORDER@;"
		"}"
		"#switcherModePage QSlider#switcherMotionSlider:disabled::handle:horizontal {"
		"  background-color: @DISABLED_TEXT@;"
		"  border-color: @DISABLED_BORDER@;"
		"}"
		"#switcherModePage QListWidget::item,"
		"#switcherModePage QTreeWidget::item {"
		"  border-radius: 4px;"
		"  padding: 7px 10px;"
		"}"
		"#switcherModePage QListWidget::item:selected,"
		"#switcherModePage QTreeWidget::item:selected {"
		"  background-color: @SELECT_FILL@;"
		"  color: @FIELD_TEXT@;"
		"}"
		"#switcherModePage QPushButton,"
		"#switcherModePage QToolButton {"
		"  background-color: @BUTTON_BG@;"
		"  color: @BUTTON_TEXT@;"
		"  border: 1px solid @PANEL_BORDER@;"
		"  border-radius: 4px;"
		"  padding: 8px 12px;"
		"  font-weight: 600;"
		"}"
		"#switcherModePage QPushButton:hover,"
		"#switcherModePage QToolButton:hover {"
		"  background-color: @BUTTON_HOVER@;"
		"  border-color: @HOVER_BORDER@;"
		"}"
		"#switcherModePage QPushButton:pressed,"
		"#switcherModePage QToolButton:pressed {"
		"  background-color: @BUTTON_PRESSED@;"
		"}"
		"#switcherModePage QPushButton:disabled,"
		"#switcherModePage QToolButton:disabled {"
		"  background-color: @DISABLED_BUTTON_BG@;"
		"  color: @DISABLED_TEXT@;"
		"  border-color: @DISABLED_BORDER@;"
		"}"
		"#switcherModePage QCheckBox:disabled {"
		"  color: @DISABLED_TEXT@;"
		"}"
		"#switcherModePage QTabWidget::pane {"
		"  background-color: @CARD_BG@;"
		"  border: 1px solid @PANEL_BORDER@;"
		"  border-radius: 6px;"
		"  top: -1px;"
		"}"
		"#switcherModePage QTabBar::tab {"
		"  background-color: @BUTTON_BG@;"
		"  color: @BUTTON_TEXT@;"
		"  border: 1px solid @PANEL_BORDER@;"
		"  border-bottom: 0px;"
		"  border-top-left-radius: 4px;"
		"  border-top-right-radius: 4px;"
		"  padding: 7px 12px;"
		"}"
		"#switcherModePage QTabBar::tab:selected {"
		"  background-color: @CARD_BG@;"
		"  border-color: @PANEL_BORDER@;"
		"}"
		"#switcherWorkspaceInspector QLabel,"
		"#switcherWorkspaceInspector QCheckBox {"
		"  color: @WINDOW_TEXT@;"
		"}"
		"#switcherWorkspaceSettingsTitle,"
		"#switcherWorkspaceSettingsSectionTitle {"
		"  color: @WINDOW_TEXT@;"
		"  font-weight: 700;"
		"}"
		"#switcherWorkspaceSettingsTitle {"
		"  font-size: 16px;"
		"}"
			"#switcherWorkspaceSettingsSectionTitle {"
			"  font-size: 12px;"
			"}"
		"#switcherWorkspaceInspector QListWidget,"
		"#switcherWorkspaceInspector QComboBox,"
		"#switcherWorkspaceInspector QLineEdit {"
		"  background-color: @FIELD_BG@;"
		"  color: @FIELD_TEXT@;"
		"  border: 1px solid @PANEL_BORDER@;"
		"  border-radius: 5px;"
		"  padding: 8px 10px;"
		"}"
		"#switcherWorkspaceInspector QComboBox:disabled,"
		"#switcherWorkspaceInspector QLineEdit:disabled {"
		"  background-color: @DISABLED_FIELD_BG@;"
		"  color: @DISABLED_TEXT@;"
		"  border-color: @DISABLED_BORDER@;"
		"}"
		"#switcherWorkspaceInspector QListWidget {"
		"  outline: none;"
		"  background-color: @CARD_BG@;"
		"}"
		"#switcherWorkspaceInspector QListWidget::item {"
		"  border-radius: 4px;"
		"  padding: 8px 10px;"
		"}"
		"#switcherWorkspaceInspector QListWidget::item:selected {"
		"  background-color: @SELECT_FILL@;"
		"  color: @FIELD_TEXT@;"
		"}"
			"#switcherWorkspaceInspector QPushButton,"
			"#switcherWorkspaceInspector QToolButton {"
			"  background-color: @BUTTON_BG@;"
			"  color: @BUTTON_TEXT@;"
			"  border: 1px solid @PANEL_BORDER@;"
			"  border-radius: 4px;"
			"  padding: 8px 12px;"
			"  font-weight: 600;"
			"}"
			"#switcherWorkspaceInspector QPushButton:hover,"
			"#switcherWorkspaceInspector QToolButton:hover {"
			"  background-color: @BUTTON_HOVER@;"
			"  border-color: @HOVER_BORDER@;"
			"}"
			"#switcherWorkspaceInspector QPushButton:pressed,"
			"#switcherWorkspaceInspector QToolButton:pressed {"
			"  background-color: @BUTTON_PRESSED@;"
			"}"
			"#switcherWorkspaceInspector QPushButton:disabled,"
			"#switcherWorkspaceInspector QToolButton:disabled {"
			"  background-color: @DISABLED_BUTTON_BG@;"
			"  color: @DISABLED_TEXT@;"
			"  border-color: @DISABLED_BORDER@;"
			"}"
			"#switcherVerticalPanel,"
			"#switcherVerticalPreviewCard {"
			"  background-color: @CARD_BG@;"
			"  border: 1px solid @PANEL_BORDER@;"
			"  border-radius: 6px;"
			"}"
			"#switcherVerticalPanelTitle {"
			"  color: @WINDOW_TEXT@;"
			"  font-size: 12px;"
			"  font-weight: 700;"
			"}"
			"#switcherVerticalPreviewStatus {"
			"  color: @WINDOW_TEXT@;"
			"  font-size: 12px;"
			"  font-weight: 600;"
			"}"
			"#switcherVerticalHelpText {"
			"  color: @MUTED_TEXT@;"
			"  font-size: 12px;"
			"}"
			"#switcherVerticalSceneList,"
			"#switcherVerticalLinkList,"
			"#switcherVerticalSourceTree {"
			"  background-color: @FIELD_BG@;"
			"  color: @FIELD_TEXT@;"
			"  border: 1px solid @PANEL_BORDER@;"
			"  border-radius: 5px;"
			"  outline: none;"
			"}"
			"#switcherVerticalSceneList::item,"
			"#switcherVerticalLinkList::item {"
			"  border-radius: 4px;"
			"  padding: 8px 10px;"
			"}"
			"#switcherVerticalSceneList::item:selected,"
			"#switcherVerticalLinkList::item:selected {"
			"  background-color: @SELECT_FILL@;"
			"  color: @FIELD_TEXT@;"
			"}"
			"#switcherVerticalSourceTree::item {"
			"  padding: 6px 8px;"
			"}"
			"#switcherVerticalSourceTree::item:selected {"
			"  background-color: @SELECT_FILL@;"
			"  color: @FIELD_TEXT@;"
			"}"
			"#switcherVerticalSourceTree QHeaderView::section {"
			"  background-color: @BUTTON_BG@;"
			"  color: @BUTTON_TEXT@;"
			"  border: 0px;"
			"  border-bottom: 1px solid @PANEL_BORDER@;"
			"  padding: 6px 8px;"
			"}"
			"#switcherVerticalToolbarButton {"
			"  background-color: @BUTTON_BG@;"
			"  color: @BUTTON_TEXT@;"
			"  border: 1px solid @PANEL_BORDER@;"
			"  border-radius: 4px;"
			"  padding: 8px 10px;"
			"  font-weight: 600;"
			"}"
			"#switcherVerticalToolbarButton:hover {"
			"  background-color: @BUTTON_HOVER@;"
			"  border-color: @HOVER_BORDER@;"
			"}"
			"#switcherVerticalToolbarButton:pressed {"
			"  background-color: @BUTTON_PRESSED@;"
			"}"
			"#switcherVerticalToolbarButton:disabled {"
			"  background-color: @DISABLED_BUTTON_BG@;"
			"  color: @DISABLED_TEXT@;"
			"  border-color: @DISABLED_BORDER@;"
			"}"
			"#switcherVerticalDockMenuButton {"
			"  background-color: @BUTTON_BG@;"
			"  color: @BUTTON_TEXT@;"
			"  border: 1px solid @PANEL_BORDER@;"
			"  border-radius: 4px;"
			"  padding: 6px 10px;"
			"  font-weight: 600;"
			"}"
			"#switcherVerticalDockMenuButton:hover {"
			"  background-color: @BUTTON_HOVER@;"
			"  border-color: @HOVER_BORDER@;"
			"}"
			"#switcherVerticalDockMenuButton::menu-indicator {"
			"  width: 10px;"
			"}"
			"#switcherWorkspaceInspectorCloseButton {"
			"  padding: 0px;"
			"  min-width: 32px;"
			"  max-width: 32px;"
		"}");

	styleSheet.replace(QStringLiteral("@ROOT@"), CssColor(rootBackground));
	styleSheet.replace(QStringLiteral("@PAGE_BG@"), CssColor(pageBackground));
	styleSheet.replace(QStringLiteral("@RAIL_BG@"), CssColor(railBackground));
	styleSheet.replace(QStringLiteral("@SLOT_BG@"), CssColor(slotBackground));
	styleSheet.replace(QStringLiteral("@SLOT_BORDER@"), CssColor(slotBorder));
	styleSheet.replace(QStringLiteral("@WINDOW_TEXT@"), CssColor(windowText));
	styleSheet.replace(QStringLiteral("@MUTED_TEXT@"), CssColor(mutedText));
	styleSheet.replace(QStringLiteral("@HOVER_FILL@"), CssColor(hoverFill));
	styleSheet.replace(QStringLiteral("@HOVER_BORDER@"), CssColor(hoverBorder));
	styleSheet.replace(QStringLiteral("@SELECT_FILL@"), CssColor(selectedFill));
	styleSheet.replace(QStringLiteral("@SELECT_BORDER@"), CssColor(selectedBorder));
	styleSheet.replace(QStringLiteral("@PANEL_BG@"), CssColor(popupBackground));
	styleSheet.replace(QStringLiteral("@PANEL_BORDER@"), CssColor(popupBorder));
	styleSheet.replace(QStringLiteral("@FIELD_BG@"), CssColor(popupFieldBackground));
	styleSheet.replace(QStringLiteral("@CARD_BG@"), CssColor(popupCardBackground));
	styleSheet.replace(QStringLiteral("@FIELD_TEXT@"), CssColor(text));
	styleSheet.replace(QStringLiteral("@BUTTON_BG@"), CssColor(button));
	styleSheet.replace(QStringLiteral("@BUTTON_TEXT@"), CssColor(buttonText));
	styleSheet.replace(QStringLiteral("@BUTTON_HOVER@"), CssColor(popupButtonHover));
	styleSheet.replace(QStringLiteral("@BUTTON_PRESSED@"), CssColor(popupButtonPressed));
	styleSheet.replace(QStringLiteral("@DISABLED_TEXT@"), CssColor(disabledReadableText));
	styleSheet.replace(QStringLiteral("@DISABLED_FIELD_BG@"), CssColor(disabledFieldBackground));
	styleSheet.replace(QStringLiteral("@DISABLED_BUTTON_BG@"), CssColor(disabledButtonBackground));
	styleSheet.replace(QStringLiteral("@DISABLED_BORDER@"), CssColor(disabledBorder));
	styleSheet.replace(QStringLiteral("@SECTION_BG@"), CssColor(sectionBackground));
	styleSheet.replace(QStringLiteral("@CHIP_BG@"), CssColor(slotChipBackground));
	styleSheet.replace(QStringLiteral("@CHIP_TEXT@"), CssColor(slotChipText));
	styleSheet.replace(QStringLiteral("@CHIP_BORDER@"), CssColor(slotChipBorder));

	if (appliedThemeStyleSheet != styleSheet) {
		appliedThemeStyleSheet = styleSheet;
		setStyleSheet(styleSheet);
		if (verticalObsDockWidget)
			verticalObsDockWidget->setStyleSheet(styleSheet);
		if (verticalScenesObsDockWidget)
			verticalScenesObsDockWidget->setStyleSheet(styleSheet);
		if (verticalSourcesObsDockWidget)
			verticalSourcesObsDockWidget->setStyleSheet(styleSheet);
		if (verticalTransitionsObsDockWidget)
			verticalTransitionsObsDockWidget->setStyleSheet(styleSheet);
		if (verticalSettingsObsDockWidget)
			verticalSettingsObsDockWidget->setStyleSheet(styleSheet);
	}
}

OBSSource SwitcherWorkspaceDock::SlotSource(int index) const
{
	if (index < 0 || index >= VisibleSlotCount())
		return nullptr;
	return slotWidgets[static_cast<size_t>(index)]->GetSource();
}

QString SwitcherWorkspaceDock::SlotTitle(int index) const
{
	if (index < 0 || index >= VisibleSlotCount())
		return QString();
	return slotWidgets[static_cast<size_t>(index)]->GetEffectiveTitle();
}

int SwitcherWorkspaceDock::VisibleSlotCount() const
{
	return NormalizeVisibleSlotCount(layoutCombo->currentData().toInt());
}

bool SwitcherWorkspaceDock::FrontendApisAvailable() const
{
	return frontendFinishedLoading && !frontendShuttingDown;
}

obs_data_t *SwitcherWorkspaceDock::BuildCanvasState() const
{
	return canvasManager ? canvasManager->BuildStateData() : obs_data_create();
}

obs_data_t *SwitcherWorkspaceDock::BuildAutomationState() const
{
	return automationEngine ? automationEngine->BuildStateData() : obs_data_create();
}

obs_data_t *SwitcherWorkspaceDock::BuildMotionState() const
{
	return motionManager ? motionManager->BuildStateData() : obs_data_create();
}

SwitchCanvasManager *SwitcherWorkspaceDock::CanvasManager() const
{
	return canvasManager;
}

SwitchCanvasDescriptor SwitcherWorkspaceDock::CanvasDescriptorForId(const QString &canvasId) const
{
	return canvasManager ? canvasManager->CanvasDescriptor(canvasId) : SwitchCanvasDescriptor{};
}

QVector<SwitchCanvasSceneDescriptor> SwitcherWorkspaceDock::CanvasScenes(const QString &canvasId) const
{
	return canvasManager ? canvasManager->ScenesForCanvas(canvasId) : QVector<SwitchCanvasSceneDescriptor>{};
}

QVector<SwitchCanvasSourceDescriptor> SwitcherWorkspaceDock::CanvasSources(const QString &canvasId) const
{
	if (!canvasManager || canvasId != canvasManager->VerticalCanvasId())
		return {};

	const auto descriptor = canvasManager->CanvasDescriptor(canvasId);
	OBSSourceAutoRelease sceneSource = ResolveSceneSourceByDescriptor(canvasManager, canvasId, descriptor);
	if (!sceneSource)
		return {};

	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (!scene)
		return {};

	QVector<SwitchCanvasSourceDescriptor> sources;
	CanvasSourceCollector collector{&sources, 0};
	obs_scene_enum_items(scene, CollectCanvasSourceItems, &collector);
	return sources;
}

QString SwitcherWorkspaceDock::VerticalCanvasId() const
{
	return canvasManager ? canvasManager->VerticalCanvasId() : QStringLiteral("vertical");
}

bool SwitcherWorkspaceDock::SwitchCanvasScene(const QString &canvasId, const QString &sceneIdOrName)
{
	return canvasManager && canvasManager->SetCanvasActiveScene(canvasId, sceneIdOrName);
}

bool SwitcherWorkspaceDock::CreateCanvasScene(const QString &canvasId, const QString &baseName, QString *createdName)
{
	if (!canvasManager || canvasId != canvasManager->VerticalCanvasId())
		return false;

	return canvasManager->CreateVerticalScene(baseName, createdName);
}

bool SwitcherWorkspaceDock::DuplicateCanvasScene(const QString &canvasId, const QString &sceneIdOrName,
						 const QString &baseName, QString *createdName)
{
	if (!canvasManager || canvasId != canvasManager->VerticalCanvasId() || sceneIdOrName.isEmpty())
		return false;

	return canvasManager->DuplicateVerticalScene(sceneIdOrName, baseName, createdName);
}

bool SwitcherWorkspaceDock::DeleteCanvasScene(const QString &canvasId, const QString &sceneIdOrName)
{
	if (!canvasManager || canvasId != canvasManager->VerticalCanvasId() || sceneIdOrName.isEmpty())
		return false;

	QString sceneName;
	for (const auto &scene : canvasManager->ScenesForCanvas(canvasId)) {
		if (scene.uuid == sceneIdOrName || scene.name == sceneIdOrName) {
			sceneName = scene.name;
			break;
		}
	}

	return !sceneName.isEmpty() && canvasManager->RemoveVerticalScene(sceneName);
}

bool SwitcherWorkspaceDock::RenameCanvasScene(const QString &canvasId, const QString &sceneIdOrName, const QString &name)
{
	if (!canvasManager || canvasId != canvasManager->VerticalCanvasId() || sceneIdOrName.isEmpty())
		return false;

	return canvasManager->RenameVerticalScene(sceneIdOrName, name);
}

bool SwitcherWorkspaceDock::SetCanvasSourceVisible(const QString &canvasId, int itemId, bool visible)
{
	if (!canvasManager || canvasId != canvasManager->VerticalCanvasId() || itemId <= 0)
		return false;

	const auto descriptor = canvasManager->CanvasDescriptor(canvasId);
	OBSSourceAutoRelease sceneSource = ResolveSceneSourceByDescriptor(canvasManager, canvasId, descriptor);
	if (!sceneSource)
		return false;

	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (!scene)
		return false;

	obs_sceneitem_t *item = obs_scene_find_sceneitem_by_id(scene, itemId);
	if (!item)
		return false;

	obs_sceneitem_set_visible(item, visible);
	ScheduleVerticalRefresh();
	return true;
}

bool SwitcherWorkspaceDock::SetCanvasSourceLocked(const QString &canvasId, int itemId, bool locked)
{
	if (!canvasManager || canvasId != canvasManager->VerticalCanvasId() || itemId <= 0)
		return false;

	const auto descriptor = canvasManager->CanvasDescriptor(canvasId);
	OBSSourceAutoRelease sceneSource = ResolveSceneSourceByDescriptor(canvasManager, canvasId, descriptor);
	if (!sceneSource)
		return false;

	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (!scene)
		return false;

	obs_sceneitem_t *item = obs_scene_find_sceneitem_by_id(scene, itemId);
	if (!item)
		return false;

	obs_sceneitem_set_locked(item, locked);
	ScheduleVerticalRefresh();
	return true;
}

bool SwitcherWorkspaceDock::RenameCanvasSource(const QString &canvasId, int itemId, const QString &name)
{
	return canvasId == VerticalCanvasId() && RenameVerticalSource(itemId, name);
}

bool SwitcherWorkspaceDock::DeleteCanvasSource(const QString &canvasId, int itemId)
{
	return canvasId == VerticalCanvasId() && RemoveVerticalSource(itemId);
}

bool SwitcherWorkspaceDock::MoveCanvasSource(const QString &canvasId, int itemId, obs_order_movement movement)
{
	return canvasId == VerticalCanvasId() && MoveVerticalSource(itemId, movement);
}

bool SwitcherWorkspaceDock::OpenCanvasSourceProperties(const QString &canvasId, int itemId)
{
	if (canvasId != VerticalCanvasId() || itemId <= 0)
		return false;
	OpenVerticalSourceProperties(itemId);
	return true;
}

bool SwitcherWorkspaceDock::OpenCanvasSourceFilters(const QString &canvasId, int itemId)
{
	if (canvasId != VerticalCanvasId() || itemId <= 0)
		return false;
	OpenVerticalSourceFilters(itemId);
	return true;
}

bool SwitcherWorkspaceDock::SetCanvasLink(const QString &mainSceneUuid, const QString &targetSceneIdOrName)
{
	if (!canvasManager || mainSceneUuid.isEmpty() || targetSceneIdOrName.isEmpty())
		return false;

	OBSSourceAutoRelease mainScene = obs_get_source_by_uuid(mainSceneUuid.toUtf8().constData());
	if (!mainScene)
		return false;

	QString targetUuid;
	QString targetName;
	for (const auto &scene : canvasManager->ScenesForCanvas(canvasManager->VerticalCanvasId())) {
		if (scene.uuid == targetSceneIdOrName || scene.name == targetSceneIdOrName) {
			targetUuid = scene.uuid;
			targetName = scene.name;
			break;
		}
	}

	if (targetName.isEmpty())
		return false;

	return canvasManager->SetLinkedScene(mainSceneUuid, QString::fromUtf8(obs_source_get_name(mainScene)), targetUuid, targetName);
}

bool SwitcherWorkspaceDock::OpenCanvasWindow(const QString &canvasId)
{
	return canvasManager && canvasManager->OpenPreviewWindow(canvasId);
}

bool SwitcherWorkspaceDock::OpenCanvasProjector(const QString &canvasId)
{
	return canvasManager && canvasManager->OpenProjector(canvasId);
}

bool SwitcherWorkspaceDock::EnsureVerticalObsDock(bool visible)
{
	auto *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!mainWindow)
		return false;

	if (!verticalObsDockWidget) {
		auto *dockWidget = new SwitchVerticalDockWidget(this, mainWindow);
		verticalObsDockWidget = dockWidget;
		if (!appliedThemeStyleSheet.isEmpty())
			dockWidget->setStyleSheet(appliedThemeStyleSheet);
		verticalObsDockContainer =
			RegisterObsDockWidget(mainWindow, dockWidget, kSwitchVerticalDockId, "Switch Vertical");
		if (!verticalObsDockContainer) {
			delete dockWidget;
			verticalObsDockWidget = nullptr;
			return false;
		}
		connect(verticalObsDockContainer, &QObject::destroyed, this, [this]() {
			verticalObsDockContainer = nullptr;
			verticalObsDockWidget = nullptr;
		});
		connect(dockWidget, &QObject::destroyed, this, [this]() { verticalObsDockWidget = nullptr; });
		connect(verticalObsDockContainer, &QDockWidget::visibilityChanged, this, [this](bool dockVisible) {
			if (verticalObsDockWidget)
				verticalObsDockWidget->SetPreviewRenderingEnabled(dockVisible && !isVisible());
			if (verticalCanvasPreview) {
				verticalCanvasPreview->SetRenderingEnabled(!dockVisible);
				if (!dockVisible)
					verticalCanvasPreview->Refresh();
			}
		});
	}

	if (!verticalObsDockWidget || !verticalObsDockContainer)
		return false;

	verticalObsDockWidget->SetPreviewRenderingEnabled(visible && !isVisible());
	RefreshVerticalObsDock();
	if (visible) {
		if (verticalCanvasPreview)
			verticalCanvasPreview->SetRenderingEnabled(false);
		verticalObsDockWidget->SetPreviewRenderingEnabled(!isVisible());
		verticalObsDockContainer->setFloating(false);
		verticalObsDockContainer->show();
		verticalObsDockContainer->raise();
	} else {
		verticalObsDockWidget->SetPreviewRenderingEnabled(false);
		if (verticalCanvasPreview)
			verticalCanvasPreview->SetRenderingEnabled(true);
		verticalObsDockContainer->hide();
	}

	return true;
}

bool SwitcherWorkspaceDock::EnsureVerticalScenesObsDock(bool visible)
{
	auto *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!mainWindow)
		return false;

	if (!verticalScenesObsDockWidget) {
		auto *dockWidget = new SwitchVerticalScenesDockWidget(this, mainWindow);
		verticalScenesObsDockWidget = dockWidget;
		if (!appliedThemeStyleSheet.isEmpty())
			dockWidget->setStyleSheet(appliedThemeStyleSheet);
		verticalScenesObsDockContainer =
			RegisterObsDockWidget(mainWindow, dockWidget, kSwitchVerticalScenesDockId, "Switch Vertical Scenes");
		if (!verticalScenesObsDockContainer) {
			delete dockWidget;
			verticalScenesObsDockWidget = nullptr;
			return false;
		}
		connect(verticalScenesObsDockContainer, &QObject::destroyed, this, [this]() {
			verticalScenesObsDockContainer = nullptr;
			verticalScenesObsDockWidget = nullptr;
		});
		connect(dockWidget, &QObject::destroyed, this, [this]() { verticalScenesObsDockWidget = nullptr; });
	}

	if (!verticalScenesObsDockWidget || !verticalScenesObsDockContainer)
		return false;

	verticalScenesObsDockWidget->Refresh();
	if (visible) {
		verticalScenesObsDockContainer->setFloating(false);
		verticalScenesObsDockContainer->show();
		verticalScenesObsDockContainer->raise();
	} else {
		verticalScenesObsDockContainer->hide();
	}

	return true;
}

bool SwitcherWorkspaceDock::EnsureVerticalSourcesObsDock(bool visible)
{
	auto *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!mainWindow)
		return false;

	if (!verticalSourcesObsDockWidget) {
		auto *dockWidget = new SwitchVerticalSourcesDockWidget(this, mainWindow);
		verticalSourcesObsDockWidget = dockWidget;
		if (!appliedThemeStyleSheet.isEmpty())
			dockWidget->setStyleSheet(appliedThemeStyleSheet);
		verticalSourcesObsDockContainer =
			RegisterObsDockWidget(mainWindow, dockWidget, kSwitchVerticalSourcesDockId, "Switch Vertical Sources");
		if (!verticalSourcesObsDockContainer) {
			delete dockWidget;
			verticalSourcesObsDockWidget = nullptr;
			return false;
		}
		connect(verticalSourcesObsDockContainer, &QObject::destroyed, this, [this]() {
			verticalSourcesObsDockContainer = nullptr;
			verticalSourcesObsDockWidget = nullptr;
		});
		connect(dockWidget, &QObject::destroyed, this, [this]() { verticalSourcesObsDockWidget = nullptr; });
	}

	if (!verticalSourcesObsDockWidget || !verticalSourcesObsDockContainer)
		return false;

	verticalSourcesObsDockWidget->Refresh();
	if (visible) {
		verticalSourcesObsDockContainer->setFloating(false);
		verticalSourcesObsDockContainer->show();
		verticalSourcesObsDockContainer->raise();
	} else {
		verticalSourcesObsDockContainer->hide();
	}

	return true;
}

bool SwitcherWorkspaceDock::EnsureVerticalTransitionsObsDock(bool visible)
{
	auto *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!mainWindow)
		return false;

	if (!verticalTransitionsObsDockWidget) {
		auto *dockWidget = new SwitchVerticalTransitionsDockWidget(this, mainWindow);
		verticalTransitionsObsDockWidget = dockWidget;
		if (!appliedThemeStyleSheet.isEmpty())
			dockWidget->setStyleSheet(appliedThemeStyleSheet);
		verticalTransitionsObsDockContainer = RegisterObsDockWidget(mainWindow, dockWidget,
									 kSwitchVerticalTransitionsDockId,
									 "Switch Vertical Transitions");
		if (!verticalTransitionsObsDockContainer) {
			delete dockWidget;
			verticalTransitionsObsDockWidget = nullptr;
			return false;
		}
		connect(verticalTransitionsObsDockContainer, &QObject::destroyed, this, [this]() {
			verticalTransitionsObsDockContainer = nullptr;
			verticalTransitionsObsDockWidget = nullptr;
		});
		connect(dockWidget, &QObject::destroyed, this, [this]() { verticalTransitionsObsDockWidget = nullptr; });
	}

	if (!verticalTransitionsObsDockWidget || !verticalTransitionsObsDockContainer)
		return false;

	verticalTransitionsObsDockWidget->Refresh();
	if (visible) {
		verticalTransitionsObsDockContainer->setFloating(false);
		verticalTransitionsObsDockContainer->show();
		verticalTransitionsObsDockContainer->raise();
	} else {
		verticalTransitionsObsDockContainer->hide();
	}

	return true;
}

bool SwitcherWorkspaceDock::EnsureVerticalSettingsObsDock(bool visible)
{
	auto *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!mainWindow)
		return false;

	if (!verticalSettingsObsDockWidget) {
		auto *dockWidget = new SwitchVerticalSettingsDockWidget(this, mainWindow);
		verticalSettingsObsDockWidget = dockWidget;
		if (!appliedThemeStyleSheet.isEmpty())
			dockWidget->setStyleSheet(appliedThemeStyleSheet);
		verticalSettingsObsDockContainer =
			RegisterObsDockWidget(mainWindow, dockWidget, kSwitchVerticalSettingsDockId, "Switch Vertical Settings");
		if (!verticalSettingsObsDockContainer) {
			delete dockWidget;
			verticalSettingsObsDockWidget = nullptr;
			return false;
		}
		connect(verticalSettingsObsDockContainer, &QObject::destroyed, this, [this]() {
			verticalSettingsObsDockContainer = nullptr;
			verticalSettingsObsDockWidget = nullptr;
		});
		connect(dockWidget, &QObject::destroyed, this, [this]() { verticalSettingsObsDockWidget = nullptr; });
	}

	if (!verticalSettingsObsDockWidget || !verticalSettingsObsDockContainer)
		return false;

	verticalSettingsObsDockWidget->Refresh();
	if (visible) {
		verticalSettingsObsDockContainer->setFloating(false);
		verticalSettingsObsDockContainer->show();
		verticalSettingsObsDockContainer->raise();
	} else {
		verticalSettingsObsDockContainer->hide();
	}

	return true;
}

bool SwitcherWorkspaceDock::PerformOutputAction(const QString &action, const QString &value)
{
	if (!FrontendApisAvailable())
		return false;

	if (action == QStringLiteral("toggle_recording")) {
		if (obs_frontend_recording_active())
			obs_frontend_recording_stop();
		else
			obs_frontend_recording_start();
		return true;
	}
	if (action == QStringLiteral("toggle_recording_pause")) {
		if (!obs_frontend_recording_active())
			return false;
		obs_frontend_recording_pause(!obs_frontend_recording_paused());
		return true;
	}
	if (action == QStringLiteral("split_recording"))
		return obs_frontend_recording_split_file();
	if (action == QStringLiteral("add_recording_chapter")) {
		const QByteArray chapterName = value.trimmed().toUtf8();
		return obs_frontend_recording_add_chapter(chapterName.isEmpty() ? nullptr : chapterName.constData());
	}
	if (action == QStringLiteral("toggle_replay")) {
		if (obs_frontend_replay_buffer_active())
			obs_frontend_replay_buffer_stop();
		else
			obs_frontend_replay_buffer_start();
		return true;
	}
	if (action == QStringLiteral("save_replay")) {
		if (!obs_frontend_replay_buffer_active())
			return false;
		obs_frontend_replay_buffer_save();
		return true;
	}
	if (action == QStringLiteral("toggle_streaming")) {
		if (obs_frontend_streaming_active())
			obs_frontend_streaming_stop();
		else
			obs_frontend_streaming_start();
		return true;
	}
	if (action == QStringLiteral("toggle_virtual_camera")) {
		if (obs_frontend_virtualcam_active())
			obs_frontend_stop_virtualcam();
		else
			obs_frontend_start_virtualcam();
		return true;
	}

	return false;
}

QString SwitcherWorkspaceDock::CreateAutomationMacro(const QString &name)
{
	return automationEngine ? automationEngine->CreateMacro(name) : QString();
}

bool SwitcherWorkspaceDock::DeleteAutomationMacro(const QString &macroId)
{
	return automationEngine && automationEngine->DeleteMacro(macroId);
}

bool SwitcherWorkspaceDock::UpdateAutomationMacro(const SwitchMacroDescriptor &macro)
{
	return automationEngine && automationEngine->UpdateMacro(macro);
}

bool SwitcherWorkspaceDock::UpsertAutomationMacro(const SwitchAutomationMacro &macro, QString *effectiveId)
{
	return automationEngine && automationEngine->UpsertMacroDefinition(macro, effectiveId);
}

SwitchAutomationMacro SwitcherWorkspaceDock::AutomationMacroDefinition(const QString &macroId) const
{
	return automationEngine ? automationEngine->MacroDefinitionById(macroId) : SwitchAutomationMacro{};
}

bool SwitcherWorkspaceDock::SetAutomationMacroEnabled(const QString &macroId, bool enabled)
{
	return automationEngine && automationEngine->SetMacroEnabled(macroId, enabled);
}

bool SwitcherWorkspaceDock::SetAutomationMacroPaused(const QString &macroId, bool paused)
{
	return automationEngine && automationEngine->SetMacroPaused(macroId, paused);
}

bool SwitcherWorkspaceDock::TriggerAutomationMacro(const QString &macroId, QString *message)
{
	return automationEngine && automationEngine->TriggerMacro(macroId, message);
}

bool SwitcherWorkspaceDock::SetAutomationVariable(const QString &key, const QString &value)
{
	return automationEngine && automationEngine->SetVariable(key, value);
}

bool SwitcherWorkspaceDock::DeleteAutomationVariable(const QString &key)
{
	return automationEngine && automationEngine->RemoveVariable(key);
}

bool SwitcherWorkspaceDock::ClearAutomationQueue(const QString &queueId)
{
	return automationEngine && automationEngine->ClearQueue(queueId);
}

SwitchAutomationConnection SwitcherWorkspaceDock::AutomationConnection(const QString &connectionId) const
{
	return automationEngine ? automationEngine->ConnectionById(connectionId) : SwitchAutomationConnection{};
}

bool SwitcherWorkspaceDock::TestAutomationConnection(const QString &connectionId, QString *message)
{
	return automationEngine && automationEngine->TestConnection(connectionId, message);
}

SwitchMotionProfile SwitcherWorkspaceDock::MotionProfileDefinition(const QString &profileId) const
{
	return motionManager ? motionManager->ProfileById(profileId) : SwitchMotionProfile{};
}

bool SwitcherWorkspaceDock::UpsertMotionProfile(const SwitchMotionProfile &profile, QString *effectiveId)
{
	return motionManager && motionManager->UpsertProfile(profile, effectiveId);
}

bool SwitcherWorkspaceDock::DeleteMotionProfile(const QString &profileId)
{
	return motionManager && motionManager->DeleteProfile(profileId);
}

bool SwitcherWorkspaceDock::SetMotionEnabled(const QString &profileId, bool enabled)
{
	return motionManager && motionManager->SetProfileEnabled(profileId, enabled);
}

bool SwitcherWorkspaceDock::BindMotionSource(const QString &sourceUuid, const QString &sourceName,
					     const QString &profileId, QString *message)
{
	return motionManager && motionManager->BindSource(sourceUuid, sourceName, profileId, message);
}

bool SwitcherWorkspaceDock::UnbindMotionSource(const QString &sourceUuid, QString *message)
{
	return motionManager && motionManager->UnbindSource(sourceUuid, message);
}

SwitchMotionShot SwitcherWorkspaceDock::MotionShotDefinition(const QString &shotId) const
{
	return motionManager ? motionManager->ShotById(shotId) : SwitchMotionShot{};
}

bool SwitcherWorkspaceDock::UpsertMotionShot(const SwitchMotionShot &shot, QString *effectiveId)
{
	return motionManager && motionManager->UpsertShot(shot, effectiveId);
}

bool SwitcherWorkspaceDock::DeleteMotionShot(const QString &shotId)
{
	return motionManager && motionManager->DeleteShot(shotId);
}

bool SwitcherWorkspaceDock::SetMotionShotEnabled(const QString &shotId, bool enabled)
{
	return motionManager && motionManager->SetShotEnabled(shotId, enabled);
}

bool SwitcherWorkspaceDock::BindMotionSceneItem(const SwitchMotionShot &shot, QString *effectiveId, QString *message)
{
	return motionManager && motionManager->BindSceneItem(shot, effectiveId, message);
}

void SwitcherWorkspaceDock::RefreshMotionPage()
{
	if (frontendShuttingDown || !motionManager)
		return;

	RefreshMotionProfiles();
	RefreshMotionSources();
	RefreshMotionShots();
	RefreshMotionShotSceneItems();
	RefreshMotionBindings();
	RefreshMotionEditor();
	RefreshMotionShotEditor();
	ScheduleMotionRuntimeRefresh();
}

void SwitcherWorkspaceDock::ScheduleMotionRuntimeRefresh()
{
	if (frontendShuttingDown)
		return;
	if (modeStack && modeStack->currentWidget() == motionModePage)
		RefreshMotionRuntimeStatus();
}

void SwitcherWorkspaceDock::RefreshMotionProfiles()
{
	if (!motionManager || !motionProfileList)
		return;

	const QString selectedId = motionProfileList->currentItem()
					   ? motionProfileList->currentItem()->data(Qt::UserRole).toString()
					   : QString();
	QSignalBlocker blocker(motionProfileList);
	motionProfileList->clear();
	for (const auto &profile : motionManager->Profiles()) {
		auto *item = new QListWidgetItem(profile.enabled ? QStringLiteral("%1 [on]").arg(profile.name) : profile.name);
		item->setData(Qt::UserRole, profile.id);
		motionProfileList->addItem(item);
		if (profile.id == selectedId)
			motionProfileList->setCurrentItem(item);
	}
	if (!motionProfileList->currentItem() && motionProfileList->count() > 0)
		motionProfileList->setCurrentRow(0);
}

void SwitcherWorkspaceDock::RefreshMotionEditor()
{
	if (!motionManager || !motionProfileList)
		return;

	QScopedValueRollback<bool> guard(loadingMotionUi, true);
	const QString profileId = motionProfileList->currentItem()
					  ? motionProfileList->currentItem()->data(Qt::UserRole).toString()
					  : QString();
	const auto profile = motionManager->ProfileById(profileId);
	const bool hasProfile = !profile.id.isEmpty();
	const QString selectedShotId = motionShotList && motionShotList->currentItem()
					       ? motionShotList->currentItem()->data(Qt::UserRole).toString()
					       : QString();
	const auto selectedShot = motionManager->ShotById(selectedShotId);
	const bool selectedShotUsesAi =
		selectedShot.id.isEmpty() || selectedShot.shotMode != QStringLiteral("keyframe_loop");

	for (auto *widget : {static_cast<QWidget *>(motionNameEdit), static_cast<QWidget *>(motionEnabledCheckBox),
			     static_cast<QWidget *>(motionConfidenceSpin), static_cast<QWidget *>(motionMaxZoomSpin),
			     static_cast<QWidget *>(motionFramingMarginSpin), static_cast<QWidget *>(motionDeadZoneSpin),
			     static_cast<QWidget *>(motionSmoothingSpin), static_cast<QWidget *>(motionHoldSpin),
			     static_cast<QWidget *>(motionConfidenceSlider), static_cast<QWidget *>(motionMaxZoomSlider),
			     static_cast<QWidget *>(motionFramingMarginSlider), static_cast<QWidget *>(motionDeadZoneSlider),
			     static_cast<QWidget *>(motionSmoothingSlider), static_cast<QWidget *>(motionHoldSlider),
			     static_cast<QWidget *>(motionBackendCombo), static_cast<QWidget *>(motionSubjectModeCombo),
			     static_cast<QWidget *>(motionFramingModeCombo), static_cast<QWidget *>(motionPresetCombo),
			     static_cast<QWidget *>(motionTrackerHighSpin), static_cast<QWidget *>(motionTrackerLowSpin),
			     static_cast<QWidget *>(motionNewTrackSpin), static_cast<QWidget *>(motionTrackBufferSpin),
			     static_cast<QWidget *>(motionAutoSwitchSpin), static_cast<QWidget *>(motionPanResponsivenessSpin),
			     static_cast<QWidget *>(motionTrackerHighSlider), static_cast<QWidget *>(motionTrackerLowSlider),
			     static_cast<QWidget *>(motionNewTrackSlider), static_cast<QWidget *>(motionAutoSwitchSlider),
			     static_cast<QWidget *>(motionTiltResponsivenessSpin), static_cast<QWidget *>(motionZoomResponsivenessSpin),
			     static_cast<QWidget *>(motionMaxPanSpeedSpin), static_cast<QWidget *>(motionMaxTiltSpeedSpin),
			     static_cast<QWidget *>(motionMaxZoomSpeedSpin), static_cast<QWidget *>(motionDebugOverlayCheckBox),
			     static_cast<QWidget *>(motionPanResponsivenessSlider),
			     static_cast<QWidget *>(motionTiltResponsivenessSlider),
			     static_cast<QWidget *>(motionZoomResponsivenessSlider),
			     static_cast<QWidget *>(motionMaxPanSpeedSlider), static_cast<QWidget *>(motionMaxTiltSpeedSlider),
			     static_cast<QWidget *>(motionMaxZoomSpeedSlider),
			     static_cast<QWidget *>(motionTrackList), static_cast<QWidget *>(motionTargetStatusLabel),
			     static_cast<QWidget *>(motionLockCurrentButton), static_cast<QWidget *>(motionCyclePreviousButton),
			     static_cast<QWidget *>(motionCycleNextButton), static_cast<QWidget *>(motionClearLockButton),
			     static_cast<QWidget *>(motionBindButton)})
		widget->setEnabled(hasProfile);
	motionDeleteButton->setEnabled(hasProfile && motionManager->Profiles().size() > 1);
	for (auto *widget : {static_cast<QWidget *>(motionEnabledCheckBox),
			     static_cast<QWidget *>(motionConfidenceSpin),
			     static_cast<QWidget *>(motionMaxZoomSpin),
			     static_cast<QWidget *>(motionFramingMarginSpin),
			     static_cast<QWidget *>(motionDeadZoneSpin),
			     static_cast<QWidget *>(motionSmoothingSpin),
			     static_cast<QWidget *>(motionHoldSpin),
			     static_cast<QWidget *>(motionConfidenceSlider),
			     static_cast<QWidget *>(motionMaxZoomSlider),
			     static_cast<QWidget *>(motionFramingMarginSlider),
			     static_cast<QWidget *>(motionDeadZoneSlider),
			     static_cast<QWidget *>(motionSmoothingSlider),
			     static_cast<QWidget *>(motionHoldSlider),
			     static_cast<QWidget *>(motionBackendCombo),
			     static_cast<QWidget *>(motionSubjectModeCombo),
			     static_cast<QWidget *>(motionFramingModeCombo),
			     static_cast<QWidget *>(motionPresetCombo),
			     static_cast<QWidget *>(motionTrackerHighSpin),
			     static_cast<QWidget *>(motionTrackerLowSpin),
			     static_cast<QWidget *>(motionNewTrackSpin),
			     static_cast<QWidget *>(motionTrackBufferSpin),
			     static_cast<QWidget *>(motionAutoSwitchSpin),
			     static_cast<QWidget *>(motionTrackerHighSlider),
			     static_cast<QWidget *>(motionTrackerLowSlider),
			     static_cast<QWidget *>(motionNewTrackSlider),
			     static_cast<QWidget *>(motionAutoSwitchSlider),
			     static_cast<QWidget *>(motionPanResponsivenessSpin),
			     static_cast<QWidget *>(motionTiltResponsivenessSpin),
			     static_cast<QWidget *>(motionZoomResponsivenessSpin),
			     static_cast<QWidget *>(motionMaxPanSpeedSpin),
			     static_cast<QWidget *>(motionMaxTiltSpeedSpin),
			     static_cast<QWidget *>(motionMaxZoomSpeedSpin),
			     static_cast<QWidget *>(motionPanResponsivenessSlider),
			     static_cast<QWidget *>(motionTiltResponsivenessSlider),
			     static_cast<QWidget *>(motionZoomResponsivenessSlider),
			     static_cast<QWidget *>(motionMaxPanSpeedSlider),
			     static_cast<QWidget *>(motionMaxTiltSpeedSlider),
			     static_cast<QWidget *>(motionMaxZoomSpeedSlider),
			     static_cast<QWidget *>(motionDebugOverlayCheckBox),
			     static_cast<QWidget *>(motionTrackList),
			     static_cast<QWidget *>(motionTargetStatusLabel),
			     static_cast<QWidget *>(motionLockCurrentButton),
			     static_cast<QWidget *>(motionCyclePreviousButton),
			     static_cast<QWidget *>(motionCycleNextButton),
			     static_cast<QWidget *>(motionClearLockButton),
			     static_cast<QWidget *>(motionBindButton)})
		widget->setEnabled(hasProfile && selectedShotUsesAi);

	if (hasProfile) {
		motionNameEdit->setText(profile.name);
		motionEnabledCheckBox->setChecked(profile.enabled);
		motionConfidenceSpin->setValue(profile.confidenceThreshold);
		motionMaxZoomSpin->setValue(profile.maxZoom);
		motionFramingMarginSpin->setValue(profile.framingMargin);
		motionDeadZoneSpin->setValue(profile.deadZone);
		motionSmoothingSpin->setValue(profile.smoothing);
		motionHoldSpin->setValue(profile.holdMs);
		const int backendIndex = motionBackendCombo->findData(profile.backend);
		motionBackendCombo->setCurrentIndex(backendIndex >= 0 ? backendIndex : 0);
		const int subjectIndex = motionSubjectModeCombo->findData(profile.subjectMode);
		motionSubjectModeCombo->setCurrentIndex(subjectIndex >= 0 ? subjectIndex : 0);
		const int framingIndex = motionFramingModeCombo->findData(profile.framingMode);
		motionFramingModeCombo->setCurrentIndex(framingIndex >= 0 ? framingIndex : 1);
		motionTrackerHighSpin->setValue(profile.trackerHighThreshold);
		motionTrackerLowSpin->setValue(profile.trackerLowThreshold);
		motionNewTrackSpin->setValue(profile.newTrackThreshold);
		motionTrackBufferSpin->setValue(profile.trackBufferFrames);
		motionAutoSwitchSpin->setValue(profile.autoSwitchMs);
		motionPanResponsivenessSpin->setValue(profile.panResponsiveness);
		motionTiltResponsivenessSpin->setValue(profile.tiltResponsiveness);
		motionZoomResponsivenessSpin->setValue(profile.zoomResponsiveness);
		motionMaxPanSpeedSpin->setValue(profile.maxPanSpeed);
		motionMaxTiltSpeedSpin->setValue(profile.maxTiltSpeed);
		motionMaxZoomSpeedSpin->setValue(profile.maxZoomSpeed);
		motionDebugOverlayCheckBox->setChecked(profile.debugOverlay);
			QString modelPath = profile.modelPath.trimmed();
			if (modelPath.isEmpty())
				modelPath = motionManager->DefaultModelPath();
			if (modelPath.isEmpty()) {
				const auto runtime = motionManager->RuntimeState();
				modelPath = runtime.modelPath.trimmed();
			}
			const bool modelExists = !modelPath.isEmpty() && QFileInfo::exists(modelPath);
		motionModelPathEdit->setText(
			modelPath.isEmpty()
				? QStringLiteral("Installer model missing")
				: QStringLiteral("%1%2")
					  .arg(QFileInfo(modelPath).fileName(), modelExists ? QString() : QStringLiteral(" (missing)")));
		motionModelPathEdit->setToolTip(modelExists ? modelPath
							     : QStringLiteral("The installer must download yolo26-nano.onnx into the plugin data path."));
	} else {
		motionNameEdit->clear();
		motionEnabledCheckBox->setChecked(false);
		motionModelPathEdit->clear();
		motionModelPathEdit->setToolTip(QString());
	}

	ScheduleMotionRuntimeRefresh();
}

void SwitcherWorkspaceDock::RefreshMotionSources()
{
	if (!motionSourceCombo)
		return;

	const QString selectedUuid = motionSourceCombo->currentData().toString();
	QSignalBlocker blocker(motionSourceCombo);
	motionSourceCombo->clear();
	obs_enum_sources(AddMotionSourceOption, motionSourceCombo);
	const int index = motionSourceCombo->findData(selectedUuid);
	if (index >= 0)
		motionSourceCombo->setCurrentIndex(index);
}

void SwitcherWorkspaceDock::RefreshMotionBindings()
{
	if (!motionManager || !motionBindingList)
		return;

	const QString selectedUuid = motionBindingList->currentItem()
					     ? motionBindingList->currentItem()->data(Qt::UserRole).toString()
					     : QString();
	QSignalBlocker blocker(motionBindingList);
	motionBindingList->clear();
	for (const auto &binding : motionManager->Bindings()) {
		const auto profile = motionManager->ProfileById(binding.profileId);
		auto *item = new QListWidgetItem(QStringLiteral("%1 -> %2")
							 .arg(binding.sourceName.isEmpty() ? binding.sourceUuid : binding.sourceName,
							      profile.name.isEmpty() ? binding.profileId : profile.name));
		item->setData(Qt::UserRole, binding.sourceUuid);
		motionBindingList->addItem(item);
		if (binding.sourceUuid == selectedUuid)
			motionBindingList->setCurrentItem(item);
	}
	motionUnbindButton->setEnabled(motionBindingList->currentItem() != nullptr);
}

void SwitcherWorkspaceDock::RefreshMotionShots()
{
	if (!motionManager || !motionShotList)
		return;

	const QString selectedId = motionShotList->currentItem()
					   ? motionShotList->currentItem()->data(Qt::UserRole).toString()
					   : QString();
	QSignalBlocker blocker(motionShotList);
	motionShotList->clear();
	for (const auto &shot : motionManager->Shots()) {
		const QString sceneLabel = shot.sceneName.isEmpty() ? QStringLiteral("Unbound") : shot.sceneName;
		const QString sourceLabel = shot.sourceName.isEmpty() ? QStringLiteral("Scene Item") : shot.sourceName;
		auto *item = new QListWidgetItem(QStringLiteral("%1%2 - %3 - %4")
							 .arg(shot.enabled ? QString() : QStringLiteral("[off] "),
							      sceneLabel,
							      sourceLabel,
							      shot.shotMode == QStringLiteral("ai_auto_frame")
								      ? QStringLiteral("AI Auto Frame")
								      : (shot.shotMode == QStringLiteral("hybrid")
										 ? QStringLiteral("Hybrid")
										 : QStringLiteral("Keyframe Loop"))));
		item->setData(Qt::UserRole, shot.id);
		motionShotList->addItem(item);
		if (shot.id == selectedId)
			motionShotList->setCurrentItem(item);
	}
	if (!motionShotList->currentItem() && motionShotList->count() > 0)
		motionShotList->setCurrentRow(0);
	const bool hasShot = motionShotList->currentItem() != nullptr;
	motionShotDeleteButton->setEnabled(hasShot);
	motionShotDuplicateButton->setEnabled(hasShot);
}

void SwitcherWorkspaceDock::RefreshMotionShotSceneItems()
{
	if (!motionShotSceneCombo || !motionShotItemCombo)
		return;

	const QString selectedShotId = motionShotList && motionShotList->currentItem()
					       ? motionShotList->currentItem()->data(Qt::UserRole).toString()
					       : QString();
	const auto shot = motionManager ? motionManager->ShotById(selectedShotId) : SwitchMotionShot{};
	const QString selectedSceneUuid = !shot.sceneUuid.isEmpty() ? shot.sceneUuid : motionShotSceneCombo->currentData().toString();
	const qint64 selectedItemId =
		shot.sceneItemId >= 0 ? shot.sceneItemId : motionShotItemCombo->currentData(kMotionShotItemIdRole).toLongLong();

	QSignalBlocker sceneBlocker(motionShotSceneCombo);
	motionShotSceneCombo->clear();
	struct obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t index = 0; index < scenes.sources.num; index++) {
		obs_source_t *source = scenes.sources.array[index];
		const char *name = obs_source_get_name(source);
		const char *uuid = obs_source_get_uuid(source);
		if (!name || !uuid || !*uuid)
			continue;
		motionShotSceneCombo->addItem(QString::fromUtf8(name), QString::fromUtf8(uuid));
		const int row = motionShotSceneCombo->count() - 1;
		motionShotSceneCombo->setItemData(row, QString::fromUtf8(uuid), kMotionShotSceneUuidRole);
		motionShotSceneCombo->setItemData(row, QString::fromUtf8(name), kMotionShotSceneNameRole);
	}
	obs_frontend_source_list_free(&scenes);
	if (!shot.sceneUuid.isEmpty() && motionShotSceneCombo->findData(shot.sceneUuid) < 0) {
		motionShotSceneCombo->addItem(shot.sceneName.isEmpty() ? shot.sceneUuid : shot.sceneName, shot.sceneUuid);
		const int row = motionShotSceneCombo->count() - 1;
		motionShotSceneCombo->setItemData(row, shot.sceneUuid, kMotionShotSceneUuidRole);
		motionShotSceneCombo->setItemData(row, shot.sceneName, kMotionShotSceneNameRole);
	}
	const int sceneIndex = motionShotSceneCombo->findData(selectedSceneUuid);
	if (sceneIndex >= 0)
		motionShotSceneCombo->setCurrentIndex(sceneIndex);

	QSignalBlocker itemBlocker(motionShotItemCombo);
	motionShotItemCombo->clear();
	const QString sceneUuid = motionShotSceneCombo->currentData().toString();
	if (!sceneUuid.isEmpty()) {
		obs_source_t *sceneSource = obs_get_source_by_uuid(sceneUuid.toUtf8().constData());
		obs_scene_t *scene = sceneSource ? obs_scene_from_source(sceneSource) : nullptr;
		if (scene) {
			MotionSceneItemCollector collector{motionShotItemCombo, 0};
			obs_scene_enum_items(scene, AddMotionSceneItemOption, &collector);
		}
		if (sceneSource)
			obs_source_release(sceneSource);
	}

	for (int row = 0; row < motionShotItemCombo->count(); row++) {
		if (motionShotItemCombo->itemData(row, kMotionShotItemIdRole).toLongLong() == selectedItemId) {
			motionShotItemCombo->setCurrentIndex(row);
			break;
		}
	}
	if (motionShotItemCombo->currentIndex() < 0 && motionShotItemCombo->count() > 0)
		motionShotItemCombo->setCurrentIndex(0);
	if (motionShotBindButton)
		motionShotBindButton->setEnabled(motionShotSceneCombo->currentIndex() >= 0 &&
						 motionShotItemCombo->currentIndex() >= 0);
}

void SwitcherWorkspaceDock::RefreshMotionShotEditor()
{
	if (!motionManager || !motionShotList)
		return;

	QScopedValueRollback<bool> guard(loadingMotionUi, true);
	const QString shotId = motionShotList->currentItem()
				       ? motionShotList->currentItem()->data(Qt::UserRole).toString()
				       : QString();
	const auto shot = motionManager->ShotById(shotId);
	const bool hasShot = !shot.id.isEmpty();
	const auto setMotionModeSectionVisibility = [this](bool usesAi, bool usesLoop, bool visible) {
		if (motionPositionSection)
			motionPositionSection->setVisible(visible && usesLoop);
		if (motionFramingSection)
			motionFramingSection->setVisible(visible && usesAi);
		if (motionControllerSection)
			motionControllerSection->setVisible(visible && usesAi);
		if (motionProfileSection)
			motionProfileSection->setVisible(visible && usesAi);
		if (motionTrackingSection)
			motionTrackingSection->setVisible(visible && usesAi);
	};
	for (auto *widget : {static_cast<QWidget *>(motionShotNameEdit),
			     static_cast<QWidget *>(motionShotEnabledCheckBox),
			     static_cast<QWidget *>(motionWorkstationStatusLabel),
			     static_cast<QWidget *>(motionShotModeHintLabel),
			     static_cast<QWidget *>(motionShotSceneCombo),
			     static_cast<QWidget *>(motionShotItemCombo),
			     static_cast<QWidget *>(motionShotModeCombo),
			     static_cast<QWidget *>(motionShotPlaybackCombo),
			     static_cast<QWidget *>(motionShotPresetCombo),
			     static_cast<QWidget *>(motionShotDurationSpin),
			     static_cast<QWidget *>(motionShotDurationSlider),
			     static_cast<QWidget *>(motionShotEasingCombo),
			     static_cast<QWidget *>(motionShotLoopModeCombo),
			     static_cast<QWidget *>(motionShotStartPanXSpin),
			     static_cast<QWidget *>(motionShotStartPanXSlider),
			     static_cast<QWidget *>(motionShotStartPanYSpin),
			     static_cast<QWidget *>(motionShotStartPanYSlider),
			     static_cast<QWidget *>(motionShotStartZoomSpin),
			     static_cast<QWidget *>(motionShotStartZoomSlider),
			     static_cast<QWidget *>(motionShotEndPanXSpin),
			     static_cast<QWidget *>(motionShotEndPanXSlider),
			     static_cast<QWidget *>(motionShotEndPanYSpin),
			     static_cast<QWidget *>(motionShotEndPanYSlider),
			     static_cast<QWidget *>(motionShotEndZoomSpin),
			     static_cast<QWidget *>(motionShotEndZoomSlider),
			     static_cast<QWidget *>(motionShotMaxZoomSpin),
			     static_cast<QWidget *>(motionShotMaxZoomSlider),
			     static_cast<QWidget *>(motionShotBindButton)})
		widget->setEnabled(hasShot);
	motionShotDeleteButton->setEnabled(hasShot);
	motionShotDuplicateButton->setEnabled(hasShot);
	if (!hasShot) {
		setMotionModeSectionVisibility(false, false, false);
		motionWorkstationStatusLabel->setText(QStringLiteral(
			"<b>No shot selected.</b> Add a shot, bind a scene item, then edit target or position here."));
		if (motionScenePreview) {
			motionScenePreview->SetPreviewActive(false);
			motionScenePreview->SetSource(nullptr);
		}
		motionShotModeHintLabel->clear();
		motionShotNameEdit->clear();
		motionShotEnabledCheckBox->setChecked(false);
		return;
	}

	const bool usesAi = shot.shotMode == QStringLiteral("ai_auto_frame") || shot.shotMode == QStringLiteral("hybrid");
	const bool usesLoop = shot.shotMode == QStringLiteral("keyframe_loop") || shot.shotMode == QStringLiteral("hybrid");
	setMotionModeSectionVisibility(usesAi, usesLoop, true);
	const QString modeLabel = shot.shotMode == QStringLiteral("ai_auto_frame")
					  ? QStringLiteral("AI Auto Frame")
					  : (shot.shotMode == QStringLiteral("hybrid") ? QStringLiteral("Hybrid")
										 : QStringLiteral("Keyframe Loop"));
	const QString bindingLabel =
		shot.sceneName.isEmpty() || shot.sourceName.isEmpty()
			? QStringLiteral("not bound")
			: QStringLiteral("%1 / %2").arg(shot.sceneName.toHtmlEscaped(), shot.sourceName.toHtmlEscaped());
	motionWorkstationStatusLabel->setText(
		QStringLiteral("<b>%1:</b> %2<br><b>Bound:</b> %3 | final scene-item crop")
			.arg(modeLabel.toHtmlEscaped(),
			     shot.name.toHtmlEscaped(),
			     bindingLabel));
	if (motionScenePreview) {
		obs_source_t *sceneSource =
			shot.sceneUuid.isEmpty() ? nullptr : obs_get_source_by_uuid(shot.sceneUuid.toUtf8().constData());
		motionScenePreview->SetSource(sceneSource ? OBSSource(sceneSource) : OBSSource());
		const bool motionVisible =
			isVisible() && !isMinimized() && modeStack && modeStack->currentWidget() == motionModePage;
		motionScenePreview->SetPreviewActive(motionVisible && sceneSource);
		if (sceneSource)
			obs_source_release(sceneSource);
	}
	if (shot.shotMode == QStringLiteral("ai_auto_frame")) {
		motionShotModeHintLabel->setText(QStringLiteral(
			"AI controls below drive this shot. The source filter samples frames; the final crop is applied to this scene item."));
	} else if (shot.shotMode == QStringLiteral("hybrid")) {
		motionShotModeHintLabel->setText(QStringLiteral(
			"AI tracking and the keyframe loop are both active. The final scene-item crop is clamped to avoid black edges."));
	} else {
		motionShotModeHintLabel->setText(QStringLiteral(
			"Keyframe motion controls below drive this shot. AI tracking controls are inactive until the mode is AI or Hybrid."));
	}

	motionShotNameEdit->setText(shot.name);
	motionShotEnabledCheckBox->setChecked(shot.enabled);
	const int modeIndex = motionShotModeCombo->findData(shot.shotMode);
	motionShotModeCombo->setCurrentIndex(modeIndex >= 0 ? modeIndex : 1);
	const int playbackIndex = motionShotPlaybackCombo->findData(shot.playbackMode);
	motionShotPlaybackCombo->setCurrentIndex(playbackIndex >= 0 ? playbackIndex : 0);
	const int easingIndex = motionShotEasingCombo->findData(shot.easing);
	motionShotEasingCombo->setCurrentIndex(easingIndex >= 0 ? easingIndex : 0);
	const int loopIndex = motionShotLoopModeCombo->findData(shot.loopMode);
	motionShotLoopModeCombo->setCurrentIndex(loopIndex >= 0 ? loopIndex : 0);
	motionShotPresetCombo->setCurrentIndex(0);
	motionShotDurationSpin->setValue(shot.durationMs);
	motionShotStartPanXSpin->setValue(shot.startPanX);
	motionShotStartPanYSpin->setValue(shot.startPanY);
	motionShotStartZoomSpin->setValue(shot.startZoom);
	motionShotEndPanXSpin->setValue(shot.endPanX);
	motionShotEndPanYSpin->setValue(shot.endPanY);
	motionShotEndZoomSpin->setValue(shot.endZoom);
	motionShotMaxZoomSpin->setValue(shot.maxZoom);
	for (auto *widget : {static_cast<QWidget *>(motionShotPresetCombo),
			     static_cast<QWidget *>(motionShotDurationSpin),
			     static_cast<QWidget *>(motionShotDurationSlider),
			     static_cast<QWidget *>(motionShotEasingCombo),
			     static_cast<QWidget *>(motionShotLoopModeCombo),
			     static_cast<QWidget *>(motionShotStartPanXSpin),
			     static_cast<QWidget *>(motionShotStartPanXSlider),
			     static_cast<QWidget *>(motionShotStartPanYSpin),
			     static_cast<QWidget *>(motionShotStartPanYSlider),
			     static_cast<QWidget *>(motionShotStartZoomSpin),
			     static_cast<QWidget *>(motionShotStartZoomSlider),
			     static_cast<QWidget *>(motionShotEndPanXSpin),
			     static_cast<QWidget *>(motionShotEndPanXSlider),
			     static_cast<QWidget *>(motionShotEndPanYSpin),
			     static_cast<QWidget *>(motionShotEndPanYSlider),
			     static_cast<QWidget *>(motionShotEndZoomSpin),
			     static_cast<QWidget *>(motionShotEndZoomSlider)})
		widget->setEnabled(hasShot && usesLoop);
	motionShotMaxZoomSpin->setEnabled(hasShot);
	motionShotMaxZoomSlider->setEnabled(hasShot);
	if (usesAi && motionProfileList) {
		const int rowCount = motionProfileList->count();
		for (int row = 0; row < rowCount; row++) {
			if (motionProfileList->item(row)->data(Qt::UserRole).toString() == shot.profileId) {
				QSignalBlocker profileBlocker(motionProfileList);
				motionProfileList->setCurrentRow(row);
				break;
			}
		}
	}
	RefreshMotionShotSceneItems();
	RefreshMotionEditor();
}

void SwitcherWorkspaceDock::RefreshMotionRuntimeStatus()
{
	if (frontendShuttingDown || !motionManager || !motionRuntimeStatusLabel)
		return;
	if (modeStack && modeStack->currentWidget() != motionModePage)
		return;

	const auto runtime = motionManager->RuntimeState();
	const QString shotStatus =
		runtime.activeShotId.isEmpty()
			? QStringLiteral("<b>Active Shot:</b> none")
			: QStringLiteral("<b>Active Shot:</b> %1<br><b>Scene:</b> %2<br><b>Phase:</b> %3 ms<br><b>Shot PTZ:</b> pan %4 / tilt %5 / zoom %6")
				  .arg(runtime.activeShotName.toHtmlEscaped(),
				       runtime.activeSceneName.toHtmlEscaped(),
				       QString::number(runtime.activeShotPhaseMs),
				       QString::number(runtime.activeShotCamX, 'f', 3),
				       QString::number(runtime.activeShotCamY, 'f', 3),
				       QString::number(runtime.activeShotZoom, 'f', 2));
	motionRuntimeStatusLabel->setText(
		QStringLiteral("<b>Status:</b> %1<br><b>Backend:</b> %2<br><b>Provider:</b> %3<br><b>Model:</b> %4<br><b>Timing:</b> pre %5 ms / infer %6 ms / track %7 ms<br><b>Dropped:</b> %8<br>%9<br>%10")
			.arg(runtime.status,
			     runtime.backend.isEmpty() ? QStringLiteral("auto") : runtime.backend,
			     runtime.providerStatus.isEmpty() ? QStringLiteral("pending") : runtime.providerStatus.toHtmlEscaped(),
			     runtime.modelAvailable ? QStringLiteral("available") : QStringLiteral("missing"),
			     QString::number(runtime.preprocessingMs, 'f', 2),
			     QString::number(runtime.inferenceMs, 'f', 2),
			     QString::number(runtime.trackingMs, 'f', 2),
			     QString::number(runtime.droppedFrames),
			     runtime.message.toHtmlEscaped(),
			     shotStatus));
	if (motionTargetStatusLabel) {
		motionTargetStatusLabel->setText(
			runtime.targetActive
				? QStringLiteral("<b>Target:</b> #%1 %2 confidence %3")
					  .arg(runtime.targetTrackId)
					  .arg(runtime.targetState.isEmpty() ? QStringLiteral("active") : runtime.targetState)
					  .arg(runtime.targetConfidence, 0, 'f', 2)
				: QStringLiteral("<b>Target:</b> none"));
	}
	if (motionTrackList) {
		const int selectedTrackId = motionTrackList->currentItem()
						    ? motionTrackList->currentItem()->data(Qt::UserRole).toInt()
						    : -1;
		QSignalBlocker blocker(motionTrackList);
		const int trackCount = static_cast<int>(runtime.tracks.size());
		motionTrackList->setVisible(trackCount > 0);
		motionTrackList->setMaximumHeight(trackCount > 0 ? 76 : 0);
		while (motionTrackList->count() < trackCount)
			motionTrackList->addItem(new QListWidgetItem);

		QListWidgetItem *selectedItem = nullptr;
		for (int row = 0; row < motionTrackList->count(); row++) {
			auto *item = motionTrackList->item(row);
			if (!item)
				continue;
			if (row >= trackCount) {
				item->setHidden(true);
				item->setData(Qt::UserRole, -1);
				item->setText(QString());
				continue;
			}

			const auto &track = runtime.tracks[row];
			QString text = QStringLiteral("#%1  %2  conf %3  age %4  missed %5")
					       .arg(track.trackId)
					       .arg(track.state)
					       .arg(track.confidence, 0, 'f', 2)
					       .arg(track.ageFrames)
					       .arg(track.missedFrames);
			if (track.trackId == runtime.targetTrackId)
				text = QStringLiteral(">> %1").arg(text);
			item->setHidden(false);
			item->setData(Qt::UserRole, track.trackId);
			item->setText(text);
			if (track.trackId == selectedTrackId)
				selectedItem = item;
		}
		if (selectedItem)
			motionTrackList->setCurrentItem(selectedItem);
		else
			motionTrackList->clearSelection();
	}
	const QString selectedShotId = motionShotList && motionShotList->currentItem()
					       ? motionShotList->currentItem()->data(Qt::UserRole).toString()
					       : QString();
	const auto selectedShot = motionManager->ShotById(selectedShotId);
	const bool selectedShotUsesAi =
		selectedShot.id.isEmpty() || selectedShot.shotMode != QStringLiteral("keyframe_loop");
	const bool hasRuntimeTarget = runtime.targetTrackId >= 0;
	if (motionLockCurrentButton)
		motionLockCurrentButton->setEnabled(selectedShotUsesAi && hasRuntimeTarget);
	const bool hasTracks = !runtime.tracks.isEmpty();
	if (motionCyclePreviousButton)
		motionCyclePreviousButton->setEnabled(selectedShotUsesAi && hasTracks);
	if (motionCycleNextButton)
		motionCycleNextButton->setEnabled(selectedShotUsesAi && hasTracks);
}

void SwitcherWorkspaceDock::MotionSelectionChanged()
{
	RefreshMotionEditor();
}

void SwitcherWorkspaceDock::MotionNameChanged(const QString &name)
{
	if (loadingMotionUi || !motionProfileList || !motionProfileList->currentItem())
		return;
	auto profile = MotionProfileDefinition(motionProfileList->currentItem()
						       ? motionProfileList->currentItem()->data(Qt::UserRole).toString()
						       : QString());
	if (profile.id.isEmpty())
		return;
	profile.name = name.trimmed().isEmpty() ? QStringLiteral("Motion Profile") : name.trimmed();
	UpsertMotionProfile(profile);
}

void SwitcherWorkspaceDock::MotionEnabledChanged(Qt::CheckState state)
{
	if (loadingMotionUi || !motionProfileList || !motionProfileList->currentItem())
		return;
	const QString profileId = motionProfileList->currentItem()
					  ? motionProfileList->currentItem()->data(Qt::UserRole).toString()
					  : QString();
	SetMotionEnabled(profileId, state == Qt::Checked);
}

void SwitcherWorkspaceDock::MotionConfidenceChanged(double value)
{
	if (loadingMotionUi || !motionProfileList || !motionProfileList->currentItem())
		return;
	auto profile = MotionProfileDefinition(motionProfileList->currentItem()->data(Qt::UserRole).toString());
	if (profile.id.isEmpty())
		return;
	profile.confidenceThreshold = static_cast<float>(value);
	UpsertMotionProfile(profile);
}

void SwitcherWorkspaceDock::MotionMaxZoomChanged(double value)
{
	if (loadingMotionUi || !motionProfileList || !motionProfileList->currentItem())
		return;
	auto profile = MotionProfileDefinition(motionProfileList->currentItem()->data(Qt::UserRole).toString());
	if (profile.id.isEmpty())
		return;
	profile.maxZoom = static_cast<float>(value);
	UpsertMotionProfile(profile);
}

void SwitcherWorkspaceDock::MotionFramingMarginChanged(double value)
{
	if (loadingMotionUi || !motionProfileList || !motionProfileList->currentItem())
		return;
	auto profile = MotionProfileDefinition(motionProfileList->currentItem()->data(Qt::UserRole).toString());
	if (profile.id.isEmpty())
		return;
	profile.framingMargin = static_cast<float>(value);
	UpsertMotionProfile(profile);
}

void SwitcherWorkspaceDock::MotionDeadZoneChanged(double value)
{
	if (loadingMotionUi || !motionProfileList || !motionProfileList->currentItem())
		return;
	auto profile = MotionProfileDefinition(motionProfileList->currentItem()->data(Qt::UserRole).toString());
	if (profile.id.isEmpty())
		return;
	profile.deadZone = static_cast<float>(value);
	UpsertMotionProfile(profile);
}

void SwitcherWorkspaceDock::MotionSmoothingChanged(double value)
{
	if (loadingMotionUi || !motionProfileList || !motionProfileList->currentItem())
		return;
	auto profile = MotionProfileDefinition(motionProfileList->currentItem()->data(Qt::UserRole).toString());
	if (profile.id.isEmpty())
		return;
	profile.smoothing = static_cast<float>(value);
	UpsertMotionProfile(profile);
}

void SwitcherWorkspaceDock::MotionHoldChanged(int value)
{
	if (loadingMotionUi || !motionProfileList || !motionProfileList->currentItem())
		return;
	auto profile = MotionProfileDefinition(motionProfileList->currentItem()->data(Qt::UserRole).toString());
	if (profile.id.isEmpty())
		return;
	profile.holdMs = value;
	UpsertMotionProfile(profile);
}

void SwitcherWorkspaceDock::AddMotionProfile()
{
	if (!motionManager)
		return;
	auto profile = SwitchDefaultMotionProfile(motionManager->DefaultModelPath());
	profile.id = SwitchCreateMotionId(QStringLiteral("motion-profile"));
	profile.name = QStringLiteral("Motion Profile %1").arg(motionManager->Profiles().size() + 1);
	QString effectiveId;
	if (motionManager->UpsertProfile(profile, &effectiveId)) {
		RefreshMotionProfiles();
		const int rowCount = motionProfileList->count();
		for (int row = 0; row < rowCount; row++) {
			if (motionProfileList->item(row)->data(Qt::UserRole).toString() == effectiveId) {
				motionProfileList->setCurrentRow(row);
				break;
			}
		}
	}
}

void SwitcherWorkspaceDock::DeleteSelectedMotionProfile()
{
	if (!motionManager || !motionProfileList || !motionProfileList->currentItem())
		return;
	if (motionManager->DeleteProfile(motionProfileList->currentItem()->data(Qt::UserRole).toString())) {
		RefreshMotionProfiles();
		RefreshMotionEditor();
		RefreshMotionBindings();
	}
}

void SwitcherWorkspaceDock::BindSelectedMotionSource()
{
	if (!motionManager || !motionProfileList || !motionProfileList->currentItem())
		return;
	const QString sourceUuid = motionSourceCombo->currentData().toString();
	const QString sourceName = motionSourceCombo->currentText();
	const QString profileId = motionProfileList->currentItem()->data(Qt::UserRole).toString();
	QString message;
	if (motionManager->BindSource(sourceUuid, sourceName, profileId, &message)) {
		RefreshMotionBindings();
		RefreshMotionRuntimeStatus();
	} else if (!message.isEmpty()) {
		motionRuntimeStatusLabel->setText(message.toHtmlEscaped());
	}
}

void SwitcherWorkspaceDock::UnbindSelectedMotionSource()
{
	if (!motionManager || !motionBindingList || !motionBindingList->currentItem())
		return;
	QString message;
	if (motionManager->UnbindSource(motionBindingList->currentItem()->data(Qt::UserRole).toString(), &message)) {
		RefreshMotionBindings();
		RefreshMotionRuntimeStatus();
	} else if (!message.isEmpty()) {
		motionRuntimeStatusLabel->setText(message.toHtmlEscaped());
	}
}

void SwitcherWorkspaceDock::MotionShotSelectionChanged()
{
	RefreshMotionShotSceneItems();
	RefreshMotionShotEditor();
}

void SwitcherWorkspaceDock::MotionShotSceneChanged(int)
{
	if (loadingMotionUi)
		return;
	RefreshMotionShotSceneItems();
}

void SwitcherWorkspaceDock::MotionShotItemChanged(int)
{
	if (motionShotBindButton)
		motionShotBindButton->setEnabled(motionShotSceneCombo && motionShotSceneCombo->currentIndex() >= 0 &&
						 motionShotItemCombo && motionShotItemCombo->currentIndex() >= 0);
}

void SwitcherWorkspaceDock::AddMotionShot()
{
	if (!motionManager)
		return;
	auto shot = SwitchDefaultMotionShot();
	const MotionShotBindingCandidate programBinding = CurrentProgramMotionShotBinding();
	shot.name = QStringLiteral("Motion Shot %1").arg(motionManager->Shots().size() + 1);
	shot.profileId = motionProfileList && motionProfileList->currentItem()
				 ? motionProfileList->currentItem()->data(Qt::UserRole).toString()
				 : motionManager->DefaultProfileId();
	if (!programBinding.sceneUuid.isEmpty()) {
		shot.sceneUuid = programBinding.sceneUuid;
		shot.sceneName = programBinding.sceneName;
	}
	if (programBinding.sceneItemId >= 0) {
		shot.sceneItemId = programBinding.sceneItemId;
		shot.sourceUuid = programBinding.sourceUuid;
		shot.sourceName = programBinding.sourceName;
	}
	if (shot.sceneUuid.isEmpty() && motionShotSceneCombo && motionShotSceneCombo->currentIndex() >= 0) {
		shot.sceneUuid = motionShotSceneCombo->currentData().toString();
		shot.sceneName = motionShotSceneCombo->currentText();
	}
	if (shot.sceneItemId < 0 && motionShotItemCombo && motionShotItemCombo->currentIndex() >= 0) {
		shot.sceneItemId = motionShotItemCombo->currentData(kMotionShotItemIdRole).toLongLong();
		shot.sourceUuid = motionShotItemCombo->currentData(kMotionShotSourceUuidRole).toString();
		shot.sourceName = motionShotItemCombo->currentData(kMotionShotSourceNameRole).toString();
	}
	if (!shot.sceneName.isEmpty() && !shot.sourceName.isEmpty())
		shot.name = QStringLiteral("%1 - %2").arg(shot.sceneName, shot.sourceName);
	QString effectiveId;
	if (motionManager->UpsertShot(shot, &effectiveId)) {
		RefreshMotionShots();
		for (int row = 0; row < motionShotList->count(); row++) {
			if (motionShotList->item(row)->data(Qt::UserRole).toString() == effectiveId) {
				motionShotList->setCurrentRow(row);
				break;
			}
		}
		RefreshMotionShotEditor();
	}
}

void SwitcherWorkspaceDock::DeleteSelectedMotionShot()
{
	if (!motionManager || !motionShotList || !motionShotList->currentItem())
		return;
	if (motionManager->DeleteShot(motionShotList->currentItem()->data(Qt::UserRole).toString())) {
		RefreshMotionShots();
		RefreshMotionShotEditor();
	}
}

void SwitcherWorkspaceDock::DuplicateSelectedMotionShot()
{
	if (!motionManager || !motionShotList || !motionShotList->currentItem())
		return;
	auto shot = motionManager->ShotById(motionShotList->currentItem()->data(Qt::UserRole).toString());
	if (shot.id.isEmpty())
		return;
	shot.id = SwitchCreateMotionId(QStringLiteral("motion-shot"));
	shot.name = QStringLiteral("%1 Copy").arg(shot.name);
	QString effectiveId;
	if (motionManager->UpsertShot(shot, &effectiveId)) {
		RefreshMotionShots();
		for (int row = 0; row < motionShotList->count(); row++) {
			if (motionShotList->item(row)->data(Qt::UserRole).toString() == effectiveId) {
				motionShotList->setCurrentRow(row);
				break;
			}
		}
		RefreshMotionShotEditor();
	}
}

void SwitcherWorkspaceDock::BindSelectedMotionShot()
{
	if (!motionManager || !motionShotList || !motionShotList->currentItem() ||
	    !motionShotSceneCombo || !motionShotItemCombo)
		return;
	auto shot = motionManager->ShotById(motionShotList->currentItem()->data(Qt::UserRole).toString());
	if (shot.id.isEmpty())
		return;
	shot.sceneUuid = motionShotSceneCombo->currentData().toString();
	shot.sceneName = motionShotSceneCombo->currentText().trimmed();
	shot.sceneItemId = motionShotItemCombo->currentData(kMotionShotItemIdRole).toLongLong();
	shot.sourceUuid = motionShotItemCombo->currentData(kMotionShotSourceUuidRole).toString();
	shot.sourceName = motionShotItemCombo->currentData(kMotionShotSourceNameRole).toString().trimmed();
	if (motionProfileList && motionProfileList->currentItem())
		shot.profileId = motionProfileList->currentItem()->data(Qt::UserRole).toString();
	QString message;
	QString effectiveId;
	if (motionManager->BindSceneItem(shot, &effectiveId, &message)) {
		RefreshMotionShots();
		RefreshMotionShotEditor();
	} else if (!message.isEmpty()) {
		motionRuntimeStatusLabel->setText(message.toHtmlEscaped());
	}
}

void SwitcherWorkspaceDock::ModeChanged(int index)
{
	switch (index) {
	case 1:
		if (canvasManager && frontendFinishedLoading)
			canvasManager->EnsureVerticalCanvas();
		modeStack->setCurrentWidget(verticalModePage);
		ScheduleVerticalRefresh();
		break;
	case 2:
		modeStack->setCurrentWidget(motionModePage);
		RefreshMotionShotEditor();
		ScheduleMotionRuntimeRefresh();
		break;
	case 3:
		modeStack->setCurrentWidget(automationModePage);
		RefreshAutomationPage();
		break;
	case 4:
		modeStack->setCurrentWidget(remoteModePage);
		RefreshRemoteControls();
		break;
	case 0:
	default:
		modeStack->setCurrentWidget(workspaceModePage);
		break;
	}

	ApplyPreviewState(isVisible() && !isMinimized());
	if (verticalCanvasPreview && modeStack->currentWidget() != verticalModePage)
		verticalCanvasPreview->SetRenderingEnabled(false);
}

void SwitcherWorkspaceDock::ScheduleVerticalRefresh()
{
	if (frontendShuttingDown)
		return;
	if (verticalRefreshPending)
		return;

	verticalRefreshPending = true;
	QPointer<SwitcherWorkspaceDock> self(this);
	QTimer::singleShot(0, this, [self]() {
		if (!self)
			return;
		self->verticalRefreshPending = false;
		if (self->frontendShuttingDown)
			return;
		self->RefreshVerticalPage();
	});
}

void SwitcherWorkspaceDock::VerticalCanvasNameChanged(const QString &name)
{
	if (loadingVerticalUi || !canvasManager)
		return;
	canvasManager->SetCanvasName(canvasManager->VerticalCanvasId(), name);
}

void SwitcherWorkspaceDock::VerticalPresetChanged(int)
{
	if (loadingVerticalUi || !canvasManager)
		return;

	const QSize size = verticalPresetCombo->currentData().toSize();
	canvasManager->SetVerticalPreset(size, verticalPresetCombo->currentText());
}

void SwitcherWorkspaceDock::VerticalSceneSelectionChanged()
{
	if (loadingVerticalUi || !canvasManager)
		return;

	auto *item = verticalSceneList->currentItem();
	if (!item)
		return;

	const QString sceneId = item->data(Qt::UserRole).toString();
	const QString sceneName = item->data(Qt::UserRole + 1).toString();
	const auto descriptor = canvasManager->CanvasDescriptor(canvasManager->VerticalCanvasId());
	if (descriptor.activeSceneUuid == sceneId || descriptor.activeSceneName == sceneName)
		return;

	suppressNextVerticalRefresh = true;
	if (!canvasManager->SetCanvasActiveScene(canvasManager->VerticalCanvasId(), sceneId.isEmpty() ? sceneName : sceneId)) {
		suppressNextVerticalRefresh = false;
		return;
	}

	ApplyVerticalSceneSelectionUi(sceneName);
}

void SwitcherWorkspaceDock::AddVerticalScene()
{
	if (!canvasManager)
		return;

	const QString canvasId = canvasManager->VerticalCanvasId();
	QPointer<SwitcherWorkspaceDock> self(this);
	QTimer::singleShot(0, this, [self, canvasId]() {
		if (!self || !self->canvasManager)
			return;
		blog(LOG_INFO, "[Switch] AddVerticalScene timer fired");
		QString createdName;
		if (!self->CreateCanvasScene(canvasId, QStringLiteral("Vertical Scene"), &createdName) || createdName.isEmpty())
			return;
		self->AppendVerticalSceneItem(createdName, true);
	});
}

void SwitcherWorkspaceDock::RemoveSelectedVerticalScene()
{
	if (!canvasManager || !verticalSceneList->currentItem())
		return;

	DeleteCanvasScene(canvasManager->VerticalCanvasId(), verticalSceneList->currentItem()->data(Qt::UserRole + 1).toString());
}

void SwitcherWorkspaceDock::OpenVerticalCanvasWindow()
{
	if (!canvasManager)
		return;

	OpenCanvasWindow(canvasManager->VerticalCanvasId());
}

void SwitcherWorkspaceDock::OpenVerticalCanvasProjector()
{
	if (!canvasManager)
		return;

	OpenCanvasProjector(canvasManager->VerticalCanvasId());
}

void SwitcherWorkspaceDock::OpenVerticalCanvasDock()
{
	EnsureVerticalObsDock(true);
}

void SwitcherWorkspaceDock::OpenVerticalScenesDock()
{
	EnsureVerticalScenesObsDock(true);
}

void SwitcherWorkspaceDock::OpenVerticalSourcesDock()
{
	EnsureVerticalSourcesObsDock(true);
}

void SwitcherWorkspaceDock::OpenVerticalTransitionsDock()
{
	EnsureVerticalTransitionsObsDock(true);
}

void SwitcherWorkspaceDock::OpenVerticalSettingsDock()
{
	EnsureVerticalSettingsObsDock(true);
}

void SwitcherWorkspaceDock::LinkCurrentProgramSceneToSelectedVerticalScene()
{
	if (!canvasManager || !verticalSceneList->currentItem())
		return;

	OBSSourceAutoRelease currentScene = obs_frontend_get_current_scene();
	if (!currentScene)
		return;

	const QString targetUuid = verticalSceneList->currentItem()->data(Qt::UserRole).toString();
	const QString targetName = verticalSceneList->currentItem()->data(Qt::UserRole + 1).toString();
	canvasManager->SetLinkedScene(QString::fromUtf8(obs_source_get_uuid(currentScene)),
				      QString::fromUtf8(obs_source_get_name(currentScene)), targetUuid, targetName);
}

void SwitcherWorkspaceDock::ClearSelectedVerticalLink()
{
	if (!canvasManager || !verticalSceneLinksList)
		return;

	auto *item = verticalSceneLinksList->currentItem();
	if (!item)
		return;

	const QString mainSceneUuid = item->data(Qt::UserRole).toString();
	if (mainSceneUuid.isEmpty())
		return;

	canvasManager->ClearLinkedScene(mainSceneUuid);
}

void SwitcherWorkspaceDock::VerticalLinkedSyncChanged(Qt::CheckState state)
{
	if (loadingVerticalUi || !canvasManager)
		return;

	canvasManager->SetCanvasLinkedSync(canvasManager->VerticalCanvasId(), state == Qt::Checked);
}

void SwitcherWorkspaceDock::VerticalTransitionChanged(int)
{
	if (loadingVerticalUi || !canvasManager)
		return;
	canvasManager->SetDefaultTransition(verticalTransitionCombo->currentText());
}

void SwitcherWorkspaceDock::VerticalTransitionDurationChanged(int value)
{
	if (loadingVerticalUi || !canvasManager)
		return;
	canvasManager->SetDefaultTransitionDuration(value);
}

void SwitcherWorkspaceDock::OpenVerticalOutputSettings()
{
	if (!canvasManager)
		return;

	SwitchVerticalOutputSettingsDialog dialog(this, this);
	if (dialog.exec() != QDialog::Accepted)
		return;

	const auto settings = dialog.Settings();
	canvasManager->SetOutputSettings(canvasManager->VerticalCanvasId(), settings);
	if (!settings.followMainReplay && settings.replayAlwaysOn && !obs_frontend_replay_buffer_active())
		PerformOutputAction(QStringLiteral("toggle_replay"));
}

void SwitcherWorkspaceDock::AppendVerticalSceneItem(const QString &sceneName, bool selectItem)
{
	if (!verticalSceneList || sceneName.isEmpty())
		return;

	for (int index = 0; index < verticalSceneList->count(); index++) {
		auto *existingItem = verticalSceneList->item(index);
		if (!existingItem)
			continue;
		if (existingItem->data(Qt::UserRole + 1).toString().compare(sceneName, Qt::CaseInsensitive) != 0)
			continue;
		if (selectItem) {
			QSignalBlocker blocker(verticalSceneList);
			verticalSceneList->setCurrentItem(existingItem);
		}
		verticalSceneRemoveButton->setEnabled(verticalSceneList->currentItem() != nullptr);
		verticalSceneMenuButton->setEnabled(verticalSceneList->currentItem() != nullptr);
		verticalLinkCurrentSceneButton->setEnabled(verticalSceneList->currentItem() != nullptr);
		verticalSceneOverrideCombo->setEnabled(verticalSceneList->currentItem() != nullptr);
		verticalSceneOverrideDurationSpin->setEnabled(verticalSceneList->currentItem() != nullptr);
		verticalApplySceneTransitionButton->setEnabled(verticalSceneList->currentItem() != nullptr);
		verticalClearSceneTransitionButton->setEnabled(verticalSceneList->currentItem() != nullptr);
		return;
	}

	QSignalBlocker blocker(verticalSceneList);
	auto *item = new QListWidgetItem(sceneName, verticalSceneList);
	item->setData(Qt::UserRole, sceneName);
	item->setData(Qt::UserRole + 1, sceneName);
	if (selectItem)
		verticalSceneList->setCurrentItem(item);

	verticalSceneRemoveButton->setEnabled(verticalSceneList->currentItem() != nullptr);
	verticalSceneMenuButton->setEnabled(verticalSceneList->currentItem() != nullptr);
	verticalLinkCurrentSceneButton->setEnabled(verticalSceneList->currentItem() != nullptr);
	verticalSceneOverrideCombo->setEnabled(verticalSceneList->currentItem() != nullptr);
	verticalSceneOverrideDurationSpin->setEnabled(verticalSceneList->currentItem() != nullptr);
	verticalApplySceneTransitionButton->setEnabled(verticalSceneList->currentItem() != nullptr);
	verticalClearSceneTransitionButton->setEnabled(verticalSceneList->currentItem() != nullptr);
}

void SwitcherWorkspaceDock::ApplyVerticalSceneSelectionUi(const QString &sceneName)
{
	if (!verticalSceneList || !canvasManager)
		return;

	const auto descriptor = canvasManager->CanvasDescriptor(canvasManager->VerticalCanvasId());
	const QString aspectLabel = descriptor.aspectPreset.isEmpty()
					    ? QStringLiteral("%1x%2").arg(descriptor.size.width()).arg(descriptor.size.height())
					    : descriptor.aspectPreset;

	QSignalBlocker blocker(verticalSceneList);
	for (int index = 0; index < verticalSceneList->count(); index++) {
		auto *item = verticalSceneList->item(index);
		if (!item)
			continue;
		const QString itemName = item->data(Qt::UserRole + 1).toString();
		const bool active = itemName == sceneName;
		item->setText(active ? QStringLiteral("%1  [Live]").arg(itemName) : itemName);
		if (active)
			verticalSceneList->setCurrentItem(item);
	}

	verticalCanvasStatusLabel->setText(
		QStringLiteral("Native canvas active at %1.\nCurrent vertical scene: %2.")
			.arg(aspectLabel, sceneName.isEmpty() ? QStringLiteral("No vertical scene selected") : sceneName));
}

QString SwitcherWorkspaceDock::SelectedVerticalSceneId() const
{
	auto *item = verticalSceneList ? verticalSceneList->currentItem() : nullptr;
	return item ? item->data(Qt::UserRole).toString() : QString();
}

QString SwitcherWorkspaceDock::SelectedVerticalSceneName() const
{
	auto *item = verticalSceneList ? verticalSceneList->currentItem() : nullptr;
	return item ? item->data(Qt::UserRole + 1).toString() : QString();
}

int SwitcherWorkspaceDock::SelectedVerticalSourceItemId() const
{
	auto *item = verticalSourceTree ? verticalSourceTree->currentItem() : nullptr;
	return item ? item->data(0, kSourceItemIdRole).toInt() : 0;
}

bool SwitcherWorkspaceDock::RenameVerticalSource(int itemId, const QString &name)
{
	if (!canvasManager || itemId <= 0)
		return false;

	const QString trimmedName = name.trimmed();
	if (trimmedName.isEmpty())
		return false;

	const auto descriptor = canvasManager->CanvasDescriptor(canvasManager->VerticalCanvasId());
	OBSSourceAutoRelease sceneSource =
		ResolveSceneSourceByDescriptor(canvasManager, canvasManager->VerticalCanvasId(), descriptor);
	if (!sceneSource)
		return false;

	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (!scene)
		return false;

	obs_sceneitem_t *item = obs_scene_find_sceneitem_by_id(scene, itemId);
	if (!item)
		return false;

	obs_source_t *source = obs_source_get_ref(obs_sceneitem_get_source(item));
	if (!source)
		return false;

	obs_source_t *existing = obs_get_source_by_name(trimmedName.toUtf8().constData());
	if (existing && existing != source) {
		obs_source_release(existing);
		obs_source_release(source);
		return false;
	}
	if (existing)
		obs_source_release(existing);

	obs_source_set_name(source, trimmedName.toUtf8().constData());
	obs_source_release(source);
	ScheduleVerticalRefresh();
	return true;
}

bool SwitcherWorkspaceDock::RemoveVerticalSource(int itemId)
{
	if (!canvasManager || itemId <= 0)
		return false;

	const auto descriptor = canvasManager->CanvasDescriptor(canvasManager->VerticalCanvasId());
	OBSSourceAutoRelease sceneSource =
		ResolveSceneSourceByDescriptor(canvasManager, canvasManager->VerticalCanvasId(), descriptor);
	if (!sceneSource)
		return false;

	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (!scene)
		return false;

	obs_sceneitem_t *item = obs_scene_find_sceneitem_by_id(scene, itemId);
	if (!item)
		return false;

	obs_sceneitem_remove(item);
	ScheduleVerticalRefresh();
	return true;
}

bool SwitcherWorkspaceDock::MoveVerticalSource(int itemId, obs_order_movement movement)
{
	if (!canvasManager || itemId <= 0)
		return false;

	const auto descriptor = canvasManager->CanvasDescriptor(canvasManager->VerticalCanvasId());
	OBSSourceAutoRelease sceneSource =
		ResolveSceneSourceByDescriptor(canvasManager, canvasManager->VerticalCanvasId(), descriptor);
	if (!sceneSource)
		return false;

	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (!scene)
		return false;

	obs_sceneitem_t *item = obs_scene_find_sceneitem_by_id(scene, itemId);
	if (!item)
		return false;

	obs_sceneitem_set_order(item, movement);
	ScheduleVerticalRefresh();
	return true;
}

void SwitcherWorkspaceDock::OpenVerticalSourceProperties(int itemId)
{
	if (!canvasManager || itemId <= 0)
		return;

	const auto descriptor = canvasManager->CanvasDescriptor(canvasManager->VerticalCanvasId());
	OBSSourceAutoRelease sceneSource =
		ResolveSceneSourceByDescriptor(canvasManager, canvasManager->VerticalCanvasId(), descriptor);
	if (!sceneSource)
		return;

	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (!scene)
		return;

	obs_sceneitem_t *item = obs_scene_find_sceneitem_by_id(scene, itemId);
	if (!item)
		return;

	if (obs_source_t *source = obs_source_get_ref(obs_sceneitem_get_source(item))) {
		obs_frontend_open_source_properties(source);
		obs_source_release(source);
	}
}

void SwitcherWorkspaceDock::OpenVerticalSourceFilters(int itemId)
{
	if (!canvasManager || itemId <= 0)
		return;

	const auto descriptor = canvasManager->CanvasDescriptor(canvasManager->VerticalCanvasId());
	OBSSourceAutoRelease sceneSource =
		ResolveSceneSourceByDescriptor(canvasManager, canvasManager->VerticalCanvasId(), descriptor);
	if (!sceneSource)
		return;

	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (!scene)
		return;

	obs_sceneitem_t *item = obs_scene_find_sceneitem_by_id(scene, itemId);
	if (!item)
		return;

	if (obs_source_t *source = obs_source_get_ref(obs_sceneitem_get_source(item))) {
		obs_frontend_open_source_filters(source);
		obs_source_release(source);
	}
}

void SwitcherWorkspaceDock::ApplySelectedSceneTransitionOverride()
{
	if (!canvasManager)
		return;

	const QString sceneId = SelectedVerticalSceneId();
	if (sceneId.isEmpty())
		return;

	const QString transitionName =
		verticalSceneOverrideCombo->currentIndex() <= 0 ? QString() : verticalSceneOverrideCombo->currentText();
	canvasManager->SetSceneTransition(sceneId, transitionName, verticalSceneOverrideDurationSpin->value());
}

void SwitcherWorkspaceDock::ClearSelectedSceneTransitionOverride()
{
	if (!canvasManager)
		return;

	const QString sceneId = SelectedVerticalSceneId();
	if (sceneId.isEmpty())
		return;

	canvasManager->SetSceneTransition(sceneId, QString(), verticalSceneOverrideDurationSpin->value());
}

void SwitcherWorkspaceDock::ShowVerticalSceneContextMenu(const QPoint &pos)
{
	if (!canvasManager || !verticalSceneList)
		return;

	if (auto *clickedItem = verticalSceneList->itemAt(pos))
		verticalSceneList->setCurrentItem(clickedItem);

	auto *sceneItem = verticalSceneList->currentItem();
	QMenu menu(this);

	auto *gridAction = menu.addAction(QStringLiteral("Grid Mode"));
	gridAction->setCheckable(true);
	gridAction->setChecked(verticalSceneList->viewMode() == QListView::IconMode);
	connect(gridAction, &QAction::triggered, this, [this](bool checked) {
		if (checked) {
			verticalSceneList->setResizeMode(QListView::Adjust);
			verticalSceneList->setViewMode(QListView::IconMode);
			verticalSceneList->setUniformItemSizes(true);
			verticalSceneList->setStyleSheet(QStringLiteral("*{padding: 0; margin: 0;}"));
		} else {
			verticalSceneList->setViewMode(QListView::ListMode);
			verticalSceneList->setResizeMode(QListView::Fixed);
			verticalSceneList->setUniformItemSizes(false);
			verticalSceneList->setStyleSheet(QString());
		}
	});
	menu.addAction(QStringLiteral("Add Vertical Scene"), this, &SwitcherWorkspaceDock::AddVerticalScene);

	if (!sceneItem) {
		menu.exec(verticalSceneList->viewport()->mapToGlobal(pos));
		return;
	}

	const QString sceneId = SelectedVerticalSceneId();
	const QString sceneName = SelectedVerticalSceneName();
	menu.addSeparator();
	menu.addAction(QStringLiteral("Duplicate"), this, [this, sceneId, sceneName] {
		QString createdName;
		if (!canvasManager->DuplicateVerticalScene(sceneId, sceneName, &createdName) || createdName.isEmpty())
			return;
		AppendVerticalSceneItem(createdName, true);
	});
	menu.addAction(QStringLiteral("Rename"), this, [this, sceneId, sceneName] {
		bool accepted = false;
		const QString newName =
			QInputDialog::getText(this, QStringLiteral("Rename Vertical Scene"), QStringLiteral("Scene name:"),
					      QLineEdit::Normal, sceneName, &accepted);
		if (accepted && !newName.trimmed().isEmpty())
			canvasManager->RenameVerticalScene(sceneId, newName.trimmed());
	});
	menu.addAction(QStringLiteral("Remove"), this, &SwitcherWorkspaceDock::RemoveSelectedVerticalScene);
	menu.addSeparator();
	menu.addAction(QStringLiteral("Open Projector"), this, [this, sceneId] {
		canvasManager->SetCanvasActiveScene(canvasManager->VerticalCanvasId(), sceneId);
		OpenVerticalCanvasProjector();
	});
	menu.addAction(QStringLiteral("Screenshot (Scene)"), this, [sceneId] {
		if (obs_source_t *source = obs_get_source_by_uuid(sceneId.toUtf8().constData())) {
			obs_frontend_take_source_screenshot(source);
			obs_source_release(source);
		}
	});
	menu.addAction(QStringLiteral("Filters"), this, [sceneId] {
		if (obs_source_t *source = obs_get_source_by_uuid(sceneId.toUtf8().constData())) {
			obs_frontend_open_source_filters(source);
			obs_source_release(source);
		}
	});

	auto *transitionMenu = menu.addMenu(QStringLiteral("Transition Override"));
	const QString overrideName = canvasManager->SceneTransitionName(sceneId);
	const int overrideDuration =
		std::max(50, canvasManager->SceneTransitionDuration(sceneId) > 0 ? canvasManager->SceneTransitionDuration(sceneId)
									 : canvasManager->DefaultTransitionDuration());
	auto *noneAction = transitionMenu->addAction(QStringLiteral("Use Default"));
	noneAction->setCheckable(true);
	noneAction->setChecked(overrideName.isEmpty());
	connect(noneAction, &QAction::triggered, this, [this, sceneId, overrideDuration] {
		canvasManager->SetSceneTransition(sceneId, QString(), overrideDuration);
	});
	for (const auto &transition : canvasManager->Transitions()) {
		auto *action = transitionMenu->addAction(transition.name);
		action->setCheckable(true);
		action->setChecked(transition.name == overrideName);
		connect(action, &QAction::triggered, this, [this, sceneId, transition, overrideDuration] {
			canvasManager->SetSceneTransition(sceneId, transition.name, overrideDuration);
		});
	}
	auto *durationSpin = new QSpinBox(transitionMenu);
	durationSpin->setRange(50, 20000);
	durationSpin->setSingleStep(50);
	durationSpin->setSuffix(QStringLiteral(" ms"));
	durationSpin->setValue(overrideDuration);
	connect(durationSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this, sceneId, overrideName](int value) {
		canvasManager->SetSceneTransition(sceneId, overrideName, value);
	});
	auto *durationAction = new QWidgetAction(transitionMenu);
	durationAction->setDefaultWidget(durationSpin);
	transitionMenu->addSeparator();
	transitionMenu->addAction(durationAction);

	auto *linkedScenesMenu = menu.addMenu(QStringLiteral("Linked Scenes"));
	connect(linkedScenesMenu, &QMenu::aboutToShow, this, [this, linkedScenesMenu, sceneId, sceneName] {
		linkedScenesMenu->clear();
		const auto links = canvasManager->Links();
		struct obs_frontend_source_list scenes = {};
		obs_frontend_get_scenes(&scenes);
		for (size_t index = 0; index < scenes.sources.num; index++) {
			obs_source_t *source = scenes.sources.array[index];
			if (!source)
				continue;
			const QString mainSceneUuid = QString::fromUtf8(obs_source_get_uuid(source));
			const QString mainSceneName = QString::fromUtf8(obs_source_get_name(source));
			auto *checkBox = new QCheckBox(mainSceneName, linkedScenesMenu);
			const bool linked = std::any_of(links.cbegin(), links.cend(), [&](const SwitchCanvasLink &link) {
				return link.mainSceneUuid == mainSceneUuid &&
				       (link.targetSceneUuid == sceneId || link.targetSceneName == sceneName);
			});
			checkBox->setChecked(linked);
			connect(checkBox, &QCheckBox::checkStateChanged, this,
				[this, mainSceneUuid, mainSceneName, sceneId, sceneName, checkBox](Qt::CheckState) {
					if (checkBox->isChecked())
						canvasManager->SetLinkedScene(mainSceneUuid, mainSceneName, sceneId, sceneName);
					else
						canvasManager->ClearLinkedScene(mainSceneUuid);
				});
			auto *widgetAction = new QWidgetAction(linkedScenesMenu);
			widgetAction->setDefaultWidget(checkBox);
			linkedScenesMenu->addAction(widgetAction);
		}
		obs_frontend_source_list_free(&scenes);
	});

	menu.addAction(QStringLiteral("Show on Main Canvas"), this, [sceneId] {
		if (obs_source_t *source = obs_get_source_by_uuid(sceneId.toUtf8().constData())) {
			if (obs_frontend_preview_program_mode_active())
				obs_frontend_set_current_preview_scene(source);
			else
				obs_frontend_set_current_scene(source);
			obs_source_release(source);
		}
	});
	menu.exec(verticalSceneList->viewport()->mapToGlobal(pos));
}

void SwitcherWorkspaceDock::ShowVerticalSourceContextMenu(const QPoint &pos)
{
	if (!canvasManager || !verticalSourceTree)
		return;

	if (auto *clickedItem = verticalSourceTree->itemAt(pos))
		verticalSourceTree->setCurrentItem(clickedItem);

	auto *item = verticalSourceTree->currentItem();
	const int itemId = item ? item->data(0, kSourceItemIdRole).toInt() : 0;
	if (itemId <= 0)
		return;

	QString sourceName = item->text(0).trimmed();
	const auto descriptor = canvasManager->CanvasDescriptor(canvasManager->VerticalCanvasId());
	if (OBSSource sceneSource =
		    ResolveSceneSourceByDescriptor(canvasManager, canvasManager->VerticalCanvasId(), descriptor)) {
		if (obs_scene_t *scene = obs_scene_from_source(sceneSource)) {
			if (obs_sceneitem_t *sceneItem = obs_scene_find_sceneitem_by_id(scene, itemId)) {
				if (obs_source_t *source = obs_source_get_ref(obs_sceneitem_get_source(sceneItem))) {
					sourceName = QString::fromUtf8(obs_source_get_name(source));
					obs_source_release(source);
				}
			}
		}
	}
	QMenu menu(this);
	auto *visibleAction = menu.addAction(QStringLiteral("Visible"));
	visibleAction->setCheckable(true);
	visibleAction->setChecked(item->checkState(1) == Qt::Checked);
	connect(visibleAction, &QAction::triggered, this,
		[this, itemId](bool checked) { SetCanvasSourceVisible(canvasManager->VerticalCanvasId(), itemId, checked); });
	auto *lockedAction = menu.addAction(QStringLiteral("Locked"));
	lockedAction->setCheckable(true);
	lockedAction->setChecked(item->checkState(2) == Qt::Checked);
	connect(lockedAction, &QAction::triggered, this,
		[this, itemId](bool checked) { SetCanvasSourceLocked(canvasManager->VerticalCanvasId(), itemId, checked); });
	menu.addSeparator();
	menu.addAction(QStringLiteral("Properties"), this, [this, itemId] { OpenVerticalSourceProperties(itemId); });
	menu.addAction(QStringLiteral("Filters"), this, [this, itemId] { OpenVerticalSourceFilters(itemId); });
	menu.addAction(QStringLiteral("Rename"), this, [this, itemId, sourceName] {
		bool accepted = false;
		const QString newName =
			QInputDialog::getText(this, QStringLiteral("Rename Source"), QStringLiteral("Source name:"), QLineEdit::Normal,
					      sourceName, &accepted);
		if (accepted)
			RenameVerticalSource(itemId, newName);
	});
	menu.addAction(QStringLiteral("Remove"), this, [this, itemId] { RemoveVerticalSource(itemId); });
	auto *orderMenu = menu.addMenu(QStringLiteral("Order"));
	orderMenu->addAction(QStringLiteral("Move Up"), this, [this, itemId] { MoveVerticalSource(itemId, OBS_ORDER_MOVE_UP); });
	orderMenu->addAction(QStringLiteral("Move Down"), this,
			     [this, itemId] { MoveVerticalSource(itemId, OBS_ORDER_MOVE_DOWN); });
	orderMenu->addAction(QStringLiteral("Move To Top"), this,
			     [this, itemId] { MoveVerticalSource(itemId, OBS_ORDER_MOVE_TOP); });
	orderMenu->addAction(QStringLiteral("Move To Bottom"), this,
			     [this, itemId] { MoveVerticalSource(itemId, OBS_ORDER_MOVE_BOTTOM); });
	menu.exec(verticalSourceTree->viewport()->mapToGlobal(pos));
}

void SwitcherWorkspaceDock::AddRecordingChapter()
{
	PerformOutputAction(QStringLiteral("add_recording_chapter"));
}

QString SwitcherWorkspaceDock::SelectedMacroId() const
{
	auto *item = macroList ? macroList->currentItem() : nullptr;
	return item ? item->data(Qt::UserRole).toString() : QString();
}

QString SwitcherWorkspaceDock::SelectedQueueId() const
{
	auto *item = queueList ? queueList->currentItem() : nullptr;
	return item ? item->data(Qt::UserRole).toString() : QString();
}

QString SwitcherWorkspaceDock::SelectedConnectionId() const
{
	auto *item = connectionList ? connectionList->currentItem() : nullptr;
	return item ? item->data(Qt::UserRole).toString() : QString();
}

void SwitcherWorkspaceDock::AddMacro()
{
	if (!automationEngine)
		return;

	const QString id = automationEngine->CreateMacro(QStringLiteral("Macro"));
	RefreshAutomationPage();
	for (int row = 0; row < macroList->count(); row++) {
		auto *item = macroList->item(row);
		if (item && item->data(Qt::UserRole).toString() == id) {
			macroList->setCurrentRow(row);
			break;
		}
	}
}

void SwitcherWorkspaceDock::DeleteSelectedMacro()
{
	const QString macroId = SelectedMacroId();
	if (!macroId.isEmpty() && automationEngine)
		automationEngine->DeleteMacro(macroId);
}

void SwitcherWorkspaceDock::RunSelectedMacro()
{
	const QString macroId = SelectedMacroId();
	if (macroId.isEmpty() || !automationEngine)
		return;

	QString message;
	if (!automationEngine->TriggerMacro(macroId, &message))
		message = QStringLiteral("Unable to run the selected macro");
	macroLastResultLabel->setText(message);
}

void SwitcherWorkspaceDock::DuplicateSelectedMacro()
{
	if (!automationEngine)
		return;

	const QString macroId = SelectedMacroId();
	if (macroId.isEmpty())
		return;

	auto macro = automationEngine->MacroDefinitionById(macroId);
	if (macro.id.isEmpty())
		return;

	macro.id.clear();
	macro.name = QStringLiteral("%1 Copy").arg(macro.name.isEmpty() ? QStringLiteral("Macro") : macro.name);
	QString effectiveId;
	if (!automationEngine->UpsertMacroDefinition(macro, &effectiveId))
		return;

	RefreshAutomationPage();
	for (int row = 0; row < macroList->count(); row++) {
		auto *item = macroList->item(row);
		if (item && item->data(Qt::UserRole).toString() == effectiveId) {
			macroList->setCurrentRow(row);
			break;
		}
	}
}

void SwitcherWorkspaceDock::ExportSelectedMacro()
{
	if (!automationEngine)
		return;

	const QString macroId = SelectedMacroId();
	if (macroId.isEmpty())
		return;

	const auto macro = automationEngine->MacroDefinitionById(macroId);
	if (macro.id.isEmpty())
		return;

	QString suggested = macro.name.trimmed();
	if (suggested.isEmpty())
		suggested = QStringLiteral("macro");
	suggested.replace('/', '-');
	suggested.replace(':', '-');

	const QString path = QFileDialog::getSaveFileName(
		this, QStringLiteral("Export Macro"), QDir::home().filePath(suggested + QStringLiteral(".switch-macro.json")),
		QStringLiteral("Switch Macro (*.switch-macro.json);;JSON (*.json)"));
	if (path.isEmpty())
		return;

	obs_data_t *data = SwitchAutomationMacroToObsData(macro);
	const char *json = obs_data_get_json(data);
	QFile file(path);
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
		file.write(json ? json : "{}");
	file.close();
	obs_data_release(data);
}

void SwitcherWorkspaceDock::ImportMacro()
{
	if (!automationEngine)
		return;

	const QString path = QFileDialog::getOpenFileName(
		this, QStringLiteral("Import Macro"), QDir::homePath(),
		QStringLiteral("Switch Macro (*.switch-macro.json *.json);;All Files (*)"));
	if (path.isEmpty())
		return;

	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return;

	const QByteArray contents = file.readAll();
	file.close();
	obs_data_t *data = obs_data_create_from_json(contents.constData());
	if (!data)
		return;

	auto macro = SwitchAutomationMacroFromObsData(data);
	obs_data_release(data);
	macro.id.clear();

	QString effectiveId;
	if (!automationEngine->UpsertMacroDefinition(macro, &effectiveId))
		return;

	RefreshAutomationPage();
	for (int row = 0; row < macroList->count(); row++) {
		auto *item = macroList->item(row);
		if (item && item->data(Qt::UserRole).toString() == effectiveId) {
			macroList->setCurrentRow(row);
			break;
		}
	}
}

void SwitcherWorkspaceDock::MacroSelectionChanged()
{
	RefreshMacroEditor();
}

void SwitcherWorkspaceDock::MacroNameChanged(const QString &)
{
	UpdateSelectedMacroFromEditor();
}

void SwitcherWorkspaceDock::MacroEnabledChanged(Qt::CheckState)
{
	UpdateSelectedMacroFromEditor();
}

void SwitcherWorkspaceDock::MacroPausedChanged(Qt::CheckState)
{
	UpdateSelectedMacroFromEditor();
}

void SwitcherWorkspaceDock::MacroRunModeChanged(int)
{
	UpdateSelectedMacroFromEditor();
	RefreshMacroEditor();
}

void SwitcherWorkspaceDock::MacroTriggerTypeChanged(int)
{
	UpdateSelectedMacroFromEditor();
	RefreshMacroEditor();
}

void SwitcherWorkspaceDock::MacroIntervalChanged(int)
{
	UpdateSelectedMacroFromEditor();
}

void SwitcherWorkspaceDock::MacroTriggerConnectionChanged(int)
{
	UpdateSelectedMacroFromEditor();
}

void SwitcherWorkspaceDock::MacroTriggerSceneChanged(int)
{
	UpdateSelectedMacroFromEditor();
}

void SwitcherWorkspaceDock::MacroTriggerStateChanged(int)
{
	UpdateSelectedMacroFromEditor();
}

void SwitcherWorkspaceDock::MacroTriggerKeyChanged(const QString &)
{
	UpdateSelectedMacroFromEditor();
}

void SwitcherWorkspaceDock::MacroTriggerValueChanged(const QString &)
{
	UpdateSelectedMacroFromEditor();
}

void SwitcherWorkspaceDock::MacroActionTypeChanged(int)
{
	UpdateSelectedMacroFromEditor();
	RefreshMacroEditor();
}

void SwitcherWorkspaceDock::MacroActionConnectionChanged(int)
{
	UpdateSelectedMacroFromEditor();
}

void SwitcherWorkspaceDock::MacroActionSceneChanged(int)
{
	UpdateSelectedMacroFromEditor();
}

void SwitcherWorkspaceDock::MacroActionDelayChanged(int)
{
	UpdateSelectedMacroFromEditor();
}

void SwitcherWorkspaceDock::MacroActionKeyChanged(const QString &)
{
	UpdateSelectedMacroFromEditor();
}

void SwitcherWorkspaceDock::MacroActionValueChanged(const QString &)
{
	UpdateSelectedMacroFromEditor();
}

void SwitcherWorkspaceDock::SetAutomationVariableFromEditor()
{
	if (!automationEngine)
		return;

	if (automationEngine->SetVariable(variableKeyEdit->text(), variableValueEdit->text())) {
		variableKeyEdit->clear();
		variableValueEdit->clear();
	}
}

void SwitcherWorkspaceDock::RemoveSelectedVariable()
{
	auto *item = variableList->currentItem();
	if (!item || !automationEngine)
		return;

	automationEngine->RemoveVariable(item->data(Qt::UserRole).toString());
}

void SwitcherWorkspaceDock::ClearSelectedQueue()
{
	if (!automationEngine)
		return;

	const QString queueId = SelectedQueueId();
	if (queueId.isEmpty())
		return;

	automationEngine->ClearQueue(queueId);
}

void SwitcherWorkspaceDock::AddOscConnection()
{
	if (!automationEngine)
		return;

	SwitchAutomationConnection connection;
	connection.typeId = QStringLiteral("osc");
	connection.name = QStringLiteral("OSC Connection");
	connection.config.insert(QStringLiteral("mode"), QStringLiteral("duplex"));
	connection.config.insert(QStringLiteral("remoteHost"), QStringLiteral("127.0.0.1"));
	connection.config.insert(QStringLiteral("remotePort"), 9000);
	connection.config.insert(QStringLiteral("listenHost"), QStringLiteral("0.0.0.0"));
	connection.config.insert(QStringLiteral("listenPort"), 9000);

	QString effectiveId;
	if (!automationEngine->UpsertConnection(connection, &effectiveId))
		return;

	RefreshAutomationPage();
	for (int row = 0; row < connectionList->count(); ++row) {
		auto *item = connectionList->item(row);
		if (item && item->data(Qt::UserRole).toString() == effectiveId) {
			connectionList->setCurrentRow(row);
			break;
		}
	}
}

void SwitcherWorkspaceDock::RemoveSelectedConnection()
{
	if (!automationEngine)
		return;

	const QString connectionId = SelectedConnectionId();
	if (connectionId.isEmpty())
		return;

	automationEngine->DeleteConnection(connectionId);
}

void SwitcherWorkspaceDock::ConnectionSelectionChanged()
{
	RefreshAutomationConnections();
}

void SwitcherWorkspaceDock::ConnectionNameChanged(const QString &)
{
	UpdateSelectedConnectionFromEditor();
}

void SwitcherWorkspaceDock::ConnectionModeChanged(int)
{
	UpdateSelectedConnectionFromEditor();
	RefreshAutomationConnectionEditor();
}

void SwitcherWorkspaceDock::ConnectionRemoteHostChanged(const QString &)
{
	UpdateSelectedConnectionFromEditor();
}

void SwitcherWorkspaceDock::ConnectionRemotePortChanged(int)
{
	UpdateSelectedConnectionFromEditor();
}

void SwitcherWorkspaceDock::ConnectionListenHostChanged(const QString &)
{
	UpdateSelectedConnectionFromEditor();
}

void SwitcherWorkspaceDock::ConnectionListenPortChanged(int)
{
	UpdateSelectedConnectionFromEditor();
}

void SwitcherWorkspaceDock::TestSelectedConnection()
{
	if (!automationEngine)
		return;

	const QString connectionId = SelectedConnectionId();
	if (connectionId.isEmpty())
		return;

	QString message;
	const bool success = automationEngine->TestConnection(connectionId, &message);
	macroLastResultLabel->setText(message.isEmpty()
					      ? (success ? QStringLiteral("Connection test passed")
							 : QStringLiteral("Connection test failed"))
					      : message);
}

void SwitcherWorkspaceDock::ExportAutomationDocument()
{
	if (!automationEngine)
		return;

	const QString path = QFileDialog::getSaveFileName(
		this, QStringLiteral("Export Automation"), QDir::home().filePath(QStringLiteral("switch-automation.json")),
		QStringLiteral("Switch Automation (*.switch-automation.json);;JSON (*.json)"));
	if (path.isEmpty())
		return;

	obs_data_t *data = automationEngine->SaveState();
	const char *json = obs_data_get_json(data);
	QFile file(path);
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
		file.write(json ? json : "{}");
	file.close();
	obs_data_release(data);
}

void SwitcherWorkspaceDock::ImportAutomationDocument()
{
	if (!automationEngine)
		return;

	const QString path = QFileDialog::getOpenFileName(
		this, QStringLiteral("Import Automation"), QDir::homePath(),
		QStringLiteral("Switch Automation (*.switch-automation.json *.json);;All Files (*)"));
	if (path.isEmpty())
		return;

	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return;

	const QByteArray contents = file.readAll();
	file.close();
	obs_data_t *data = obs_data_create_from_json(contents.constData());
	if (!data)
		return;

	automationEngine->LoadState(data);
	obs_data_release(data);
	if (FrontendApisAvailable())
		automationEngine->SetLifecycleState(SwitchAutomationLifecycleState::Running);
	RefreshAutomationPage();
	emit WorkspaceStateChanged();
}

void SwitcherWorkspaceDock::RefreshVerticalPage()
{
	if (!canvasManager)
		return;
	const bool frontendReady = FrontendApisAvailable();

	QSignalBlocker nameBlocker(verticalCanvasNameEdit);
	QSignalBlocker presetBlocker(verticalPresetCombo);
	QSignalBlocker linkedBlocker(verticalLinkedSyncCheckBox);
	QSignalBlocker transitionBlocker(verticalTransitionCombo);
	QSignalBlocker transitionDurationBlocker(verticalTransitionDurationSpin);
	QSignalBlocker sceneBlocker(verticalSceneList);
	QSignalBlocker linkBlocker(verticalSceneLinksList);
	QSignalBlocker sceneOverrideBlocker(verticalSceneOverrideCombo);
	QSignalBlocker sceneOverrideDurationBlocker(verticalSceneOverrideDurationSpin);
	QScopedValueRollback<bool> guard(loadingVerticalUi, true);

	const auto descriptor = canvasManager->CanvasDescriptor(canvasManager->VerticalCanvasId());
	verticalCanvasNameEdit->setText(descriptor.name);
	const int presetIndex = verticalPresetCombo->findData(descriptor.size);
	if (presetIndex >= 0)
		verticalPresetCombo->setCurrentIndex(presetIndex);
	verticalLinkedSyncCheckBox->setChecked(descriptor.linkedSceneSync);
	verticalTransitionCombo->clear();
	verticalSceneOverrideCombo->clear();
	verticalSceneOverrideCombo->addItem(QStringLiteral("Use Default"));
	for (const auto &transition : canvasManager->Transitions())
		verticalTransitionCombo->addItem(transition.name);
	for (const auto &transition : canvasManager->Transitions())
		verticalSceneOverrideCombo->addItem(transition.name);
	const int transitionIndex = verticalTransitionCombo->findText(canvasManager->DefaultTransitionName());
	if (transitionIndex >= 0)
		verticalTransitionCombo->setCurrentIndex(transitionIndex);
	else if (verticalTransitionCombo->count() > 0)
		verticalTransitionCombo->setCurrentIndex(0);
	verticalTransitionDurationSpin->setValue(canvasManager->DefaultTransitionDuration());

	const QString currentSceneId = descriptor.activeSceneUuid;
	verticalSceneList->clear();
	int selectedRow = -1;
	int activeRow = -1;
	const auto scenes = canvasManager->ScenesForCanvas(canvasManager->VerticalCanvasId());
	const auto sources = CanvasSources(canvasManager->VerticalCanvasId());
	for (int index = 0; index < scenes.size(); index++) {
		const bool active = scenes[index].uuid == currentSceneId || scenes[index].name == descriptor.activeSceneName;
		auto *item = new QListWidgetItem(active ? QStringLiteral("%1  [Live]").arg(scenes[index].name) : scenes[index].name,
						 verticalSceneList);
		item->setData(Qt::UserRole, scenes[index].uuid);
		item->setData(Qt::UserRole + 1, scenes[index].name);
		if (active)
			activeRow = index;
	}
	if (activeRow >= 0)
		selectedRow = activeRow;
	else if (verticalSceneList->count() > 0)
		selectedRow = 0;
	if (selectedRow >= 0)
		verticalSceneList->setCurrentRow(selectedRow);

	verticalSceneLinksList->clear();
	for (const auto &link : canvasManager->Links()) {
		auto *item = new QListWidgetItem(QStringLiteral("%1 -> %2").arg(link.mainSceneName, link.targetSceneName),
						 verticalSceneLinksList);
		item->setData(Qt::UserRole, link.mainSceneUuid);
		item->setData(Qt::UserRole + 1, link.targetSceneUuid);
	}
	if (verticalSceneLinksList->count() > 0)
		verticalSceneLinksList->setCurrentRow(0);

	QStringList statuses;
	if (frontendReady && obs_frontend_streaming_active())
		statuses.push_back(QT_UTF8(obs_module_text("Streaming")));
	if (frontendReady && obs_frontend_recording_paused())
		statuses.push_back(QT_UTF8(obs_module_text("RecordingPaused")));
	else if (frontendReady && obs_frontend_recording_active())
		statuses.push_back(QT_UTF8(obs_module_text("Recording")));
	if (frontendReady && obs_frontend_replay_buffer_active())
		statuses.push_back(QT_UTF8(obs_module_text("SwitcherReplayBuffer")));
	if (frontendReady && obs_frontend_virtualcam_active())
		statuses.push_back(QT_UTF8(obs_module_text("SwitcherVirtualCamera")));
	if (statuses.isEmpty())
		statuses.push_back(QT_UTF8(obs_module_text("SwitcherIdle")));

	const QString activeSceneName = descriptor.activeSceneName.isEmpty() ? QStringLiteral("No vertical scene selected")
								    : descriptor.activeSceneName;
	verticalCanvasStatusLabel->setText(
		QStringLiteral("Native canvas active at %1.\nCurrent vertical scene: %2.")
			.arg(descriptor.aspectPreset.isEmpty() ? QStringLiteral("%1x%2").arg(descriptor.size.width()).arg(descriptor.size.height())
							       : descriptor.aspectPreset,
			     activeSceneName));
	verticalOutputStatusLabel->setText(
		QStringLiteral("OBS Output Shortcuts - Main OBS: %1").arg(statuses.join(QStringLiteral(" | "))));
	PopulateVerticalSourceTree(verticalSourceTree, sources);
	RefreshVerticalSettingsSummary();

	const QString selectedSceneId = SelectedVerticalSceneId();
	const QString overrideName = selectedSceneId.isEmpty() ? QString() : canvasManager->SceneTransitionName(selectedSceneId);
	const int overrideDuration = selectedSceneId.isEmpty()
					    ? canvasManager->DefaultTransitionDuration()
					    : std::max(50, canvasManager->SceneTransitionDuration(selectedSceneId) > 0
									 ? canvasManager->SceneTransitionDuration(selectedSceneId)
									 : canvasManager->DefaultTransitionDuration());
	const int overrideIndex =
		overrideName.isEmpty() ? 0 : std::max(0, verticalSceneOverrideCombo->findText(overrideName));
	verticalSceneOverrideCombo->setCurrentIndex(overrideIndex);
	verticalSceneOverrideDurationSpin->setValue(overrideDuration);

	verticalSceneOpenWindowButton->setText(QStringLiteral("Open Window"));
	verticalSceneOpenProjectorButton->setText(QStringLiteral("Open Projector"));
	verticalSceneDockButton->setText(QStringLiteral("Dock To OBS"));
	verticalRecordButton->setText(
		frontendReady && obs_frontend_recording_active() ? QStringLiteral("Stop Recording")
							 : QStringLiteral("Start Recording"));
	verticalPauseRecordButton->setText(frontendReady && obs_frontend_recording_paused() ? QStringLiteral("Resume")
										  : QStringLiteral("Pause"));
	verticalSplitRecordButton->setText(QStringLiteral("Split File"));
	verticalChapterButton->setText(QStringLiteral("Add Marker"));
	verticalReplayButton->setText(frontendReady && obs_frontend_replay_buffer_active() ? QStringLiteral("Stop Replay")
										    : QStringLiteral("Start Replay"));
	verticalReplaySaveButton->setText(QStringLiteral("Save Replay"));
	verticalStreamButton->setText(
		frontendReady && obs_frontend_streaming_active() ? QStringLiteral("Stop Streaming")
							 : QStringLiteral("Start Streaming"));
	verticalVirtualCamButton->setText(
		frontendReady && obs_frontend_virtualcam_active() ? QStringLiteral("Stop Virtual Camera")
								  : QStringLiteral("Start Virtual Camera"));

	verticalPauseRecordButton->setEnabled(frontendReady && obs_frontend_recording_active());
	verticalSplitRecordButton->setEnabled(frontendReady && obs_frontend_recording_active() &&
					      !obs_frontend_recording_paused());
	verticalChapterButton->setEnabled(frontendReady && obs_frontend_recording_active() &&
					  !obs_frontend_recording_paused());
	verticalReplaySaveButton->setEnabled(frontendReady && obs_frontend_replay_buffer_active());
	verticalSceneRemoveButton->setEnabled(verticalSceneList->currentItem() != nullptr);
	verticalSceneMenuButton->setEnabled(verticalSceneList->currentItem() != nullptr);
	verticalSceneOpenProjectorButton->setEnabled(!descriptor.activeSceneName.isEmpty());
	verticalLinkCurrentSceneButton->setEnabled(verticalSceneList->currentItem() != nullptr);
	verticalClearLinkButton->setEnabled(verticalSceneLinksList->currentItem() != nullptr);
	verticalSceneOverrideCombo->setEnabled(verticalSceneList->currentItem() != nullptr);
	verticalSceneOverrideDurationSpin->setEnabled(verticalSceneList->currentItem() != nullptr);
	verticalApplySceneTransitionButton->setEnabled(verticalSceneList->currentItem() != nullptr);
	verticalClearSceneTransitionButton->setEnabled(verticalSceneList->currentItem() != nullptr);
	const int sourceItemId = SelectedVerticalSourceItemId();
	verticalSourcePropertiesButton->setEnabled(sourceItemId > 0);
	verticalSourceFiltersButton->setEnabled(sourceItemId > 0);
	verticalSourceMenuButton->setEnabled(sourceItemId > 0);

	const bool verticalDockPreviewVisible = verticalObsDockContainer && verticalObsDockContainer->isVisible();
	if (verticalCanvasPreview)
		verticalCanvasPreview->SetRenderingEnabled(!verticalDockPreviewVisible);
	if (frontendReady && modeStack->currentWidget() == verticalModePage && verticalCanvasPreview &&
	    !verticalDockPreviewVisible && canvasManager->CanRenderCanvas())
		verticalCanvasPreview->Refresh();

	if (frontendReady && canvasManager->CanRenderCanvas() && !isVisible())
		RefreshVerticalDockSurfaces();
}

void SwitcherWorkspaceDock::RefreshVerticalSettingsSummary()
{
	if (!canvasManager || !verticalOutputSettingsSummaryLabel)
		return;

	const auto settings = canvasManager->OutputSettings(canvasManager->VerticalCanvasId());
	const auto encoders = AvailableVideoEncoders();
	const QString streamingEncoder =
		settings.followMainStreaming ? QStringLiteral("Match main OBS")
					     : EncoderNameForId(encoders, settings.streamEncoderId);
	const QString recordingEncoder =
		settings.followMainRecording ? QStringLiteral("Match main OBS")
					     : EncoderNameForId(encoders, settings.recordEncoderId);
	const QString delaySummary = settings.followMainStreaming
					 ? QStringLiteral("Main OBS")
					 : (settings.streamDelayEnabled ? QStringLiteral("%1 ms%2")
									 .arg(settings.streamDelayMs)
									 .arg(settings.streamDelayPreserve
										      ? QStringLiteral(" preserved")
										      : QString())
								 : QStringLiteral("Off"));
	const QString replaySummary = settings.followMainReplay
					  ? QStringLiteral("Main OBS")
					  : QStringLiteral("%1s to %2%3")
						    .arg(settings.replayDurationSeconds)
						    .arg(settings.replayPath.isEmpty() ? QStringLiteral("default folder")
										       : settings.replayPath)
						    .arg(settings.replayAlwaysOn ? QStringLiteral(" (armed)") : QString());
	const QString recordingSplitSummary =
		settings.recordingSplitEnabled ? QStringLiteral("Every %1 min").arg(settings.recordingSplitMinutes)
					       : QStringLiteral("Manual only");
	verticalOutputSettingsSummaryLabel->setText(
		QStringLiteral("<b>Status</b><br/>Saved profile; isolated vertical output runtime is not active yet.<br/><br/>"
			       "<b>Streaming</b><br/>%1, %2 kbps, delay %3<br/><br/>"
			       "<b>Recording</b><br/>%4, %5 kbps<br/>Path: %6<br/>File split: %7<br/><br/>"
			       "<b>Replay / Backtrack</b><br/>%8<br/><br/>"
			       "<b>Audio</b><br/>%9 kbps on tracks %10<br/><br/>"
			       "<b>Virtual Camera</b><br/>%11")
			.arg(streamingEncoder,
			     QString::number(settings.streamingVideoBitrateKbps),
			     delaySummary,
			     recordingEncoder,
			     QString::number(settings.recordingVideoBitrateKbps),
			     settings.recordingPath.isEmpty() ? QStringLiteral("default folder") : settings.recordingPath,
			     recordingSplitSummary,
			     replaySummary,
			     QString::number(settings.audioBitrateKbps),
			     AudioTracksSummary(settings.audioTrackMask),
			     settings.followMainVirtualCamera ? QStringLiteral("Main OBS")
							 : QStringLiteral("Switch profile")));
}

void SwitcherWorkspaceDock::RefreshVerticalObsDock()
{
	if (frontendShuttingDown)
		return;
	if (verticalObsDockWidget)
		verticalObsDockWidget->Refresh();
}

void SwitcherWorkspaceDock::RefreshVerticalDockSurfaces()
{
	if (frontendShuttingDown)
		return;
	if (verticalObsDockWidget)
		verticalObsDockWidget->Refresh(!isVisible());
	if (verticalScenesObsDockWidget)
		verticalScenesObsDockWidget->Refresh();
	if (verticalSourcesObsDockWidget)
		verticalSourcesObsDockWidget->Refresh();
	if (verticalTransitionsObsDockWidget)
		verticalTransitionsObsDockWidget->Refresh();
	if (verticalSettingsObsDockWidget)
		verticalSettingsObsDockWidget->Refresh();
}

void SwitcherWorkspaceDock::RefreshMacroList()
{
	if (!automationEngine)
		return;

	const QString selectedId = SelectedMacroId();
	QSignalBlocker blocker(macroList);
	macroList->clear();

	for (const auto &macro : automationEngine->Macros()) {
		QString title = macro.name;
		if (!macro.enabled)
			title += QStringLiteral(" [Disabled]");
		else if (macro.paused)
			title += QStringLiteral(" [Paused]");

		auto *item = new QListWidgetItem(title, macroList);
		item->setData(Qt::UserRole, macro.id);
		if (macro.id == selectedId)
			macroList->setCurrentItem(item);
	}

	if (!selectedId.isEmpty() || macroList->count() == 0)
		return;

	macroList->setCurrentRow(0);
}

void SwitcherWorkspaceDock::RefreshAutomationVariables()
{
	if (!automationEngine)
		return;

	QSignalBlocker blocker(variableList);
	variableList->clear();
	const auto vars = automationEngine->Variables();
	for (auto it = vars.begin(); it != vars.end(); ++it) {
		auto *item = new QListWidgetItem(QStringLiteral("%1 = %2").arg(it.key(), it.value()), variableList);
		item->setData(Qt::UserRole, it.key());
	}

	variableRemoveButton->setEnabled(variableList->currentItem() != nullptr);
}

void SwitcherWorkspaceDock::RefreshAutomationQueues()
{
	if (!automationEngine)
		return;

	const QString selectedId = SelectedQueueId();
	QSignalBlocker blocker(queueList);
	queueList->clear();
	for (const auto &queue : automationEngine->Queues()) {
		QString label = queue.name.isEmpty() ? queue.id : queue.name;
		label += QStringLiteral(" [%1]").arg(queue.mode);
		if (!queue.actionSegmentIds.isEmpty())
			label += QStringLiteral(" (%1 items)").arg(queue.actionSegmentIds.size());
		auto *item = new QListWidgetItem(label, queueList);
		item->setData(Qt::UserRole, queue.id);
		if (queue.id == selectedId)
			queueList->setCurrentItem(item);
	}

	if (queueList->currentItem() == nullptr && queueList->count() > 0)
		queueList->setCurrentRow(0);
	queueClearButton->setEnabled(queueList->currentItem() != nullptr);
}

void SwitcherWorkspaceDock::RefreshAutomationConnections()
{
	if (!automationEngine)
		return;

	const QString selectedId = SelectedConnectionId();
	QSignalBlocker blocker(connectionList);
	connectionList->clear();
	for (const auto &connection : automationEngine->Connections()) {
		QString label = connection.name.isEmpty() ? connection.id : connection.name;
		label += QStringLiteral(" [%1]").arg(connection.typeId);
		auto *item = new QListWidgetItem(label, connectionList);
		item->setData(Qt::UserRole, connection.id);
		if (connection.id == selectedId)
			connectionList->setCurrentItem(item);
	}

	if (connectionList->currentItem() == nullptr && connectionList->count() > 0)
		connectionList->setCurrentRow(0);

	RefreshAutomationConnectionEditor();

	const QString connectionId = SelectedConnectionId();
	const auto connection = automationEngine->ConnectionById(connectionId);
	if (connection.id.isEmpty()) {
		connectionDetailsLabel->setText(QStringLiteral("Select a connection to inspect its configuration."));
		connectionRemoveButton->setEnabled(false);
		connectionTestButton->setEnabled(false);
		return;
	}

	QStringList lines;
	lines << QStringLiteral("<b>%1</b>").arg(connection.name.isEmpty() ? connection.id : connection.name);
	lines << QStringLiteral("Type: %1").arg(connection.typeId);
	if (!connection.status.isEmpty())
		lines << QStringLiteral("Status: %1").arg(connection.status);
	if (!connection.config.isEmpty()) {
		QStringList metadataLines;
		for (auto it = connection.config.constBegin(); it != connection.config.constEnd(); ++it)
			metadataLines << QStringLiteral("%1=%2").arg(it.key().toHtmlEscaped(), it.value().toString().toHtmlEscaped());
		lines << QStringLiteral("Config: %1").arg(metadataLines.join(QStringLiteral(", ")));
	}

	connectionDetailsLabel->setText(lines.join(QStringLiteral("<br/>")));
	connectionRemoveButton->setEnabled(true);
	connectionTestButton->setEnabled(true);
}

void SwitcherWorkspaceDock::RefreshAutomationConnectionEditor()
{
	const QString connectionId = SelectedConnectionId();
	const bool hasConnection = !connectionId.isEmpty() && automationEngine;
	QScopedValueRollback<bool> guard(loadingAutomationUi, true);

	connectionNameEdit->setEnabled(hasConnection);
	connectionModeCombo->setEnabled(hasConnection);
	connectionRemoteHostEdit->setEnabled(hasConnection);
	connectionRemotePortSpin->setEnabled(hasConnection);
	connectionListenHostEdit->setEnabled(hasConnection);
	connectionListenPortSpin->setEnabled(hasConnection);
	connectionRemoveButton->setEnabled(hasConnection);

	if (!hasConnection) {
		connectionNameEdit->clear();
		connectionRemoteHostEdit->clear();
		connectionRemotePortSpin->setValue(1);
		connectionListenHostEdit->clear();
		connectionListenPortSpin->setValue(1);
		return;
	}

	const auto connection = automationEngine->ConnectionById(connectionId);
	const bool oscConnection = connection.typeId == QStringLiteral("osc");
	connectionNameEdit->setText(connection.name);
	connectionModeCombo->setCurrentIndex(std::max(0, connectionModeCombo->findData(
						     connection.config.value(QStringLiteral("mode")).toString().trimmed().isEmpty()
							     ? QStringLiteral("duplex")
							     : connection.config.value(QStringLiteral("mode")).toString())));
	connectionRemoteHostEdit->setText(connection.config.value(QStringLiteral("remoteHost")).toString());
	connectionRemotePortSpin->setValue(std::max(1, connection.config.value(QStringLiteral("remotePort")).toInt()));
	connectionListenHostEdit->setText(connection.config.value(QStringLiteral("listenHost")).toString());
	connectionListenPortSpin->setValue(std::max(1, connection.config.value(QStringLiteral("listenPort")).toInt()));

	connectionModeCombo->setEnabled(hasConnection && oscConnection);
	connectionRemoteHostEdit->setEnabled(hasConnection && oscConnection);
	connectionRemotePortSpin->setEnabled(hasConnection && oscConnection);
	connectionListenHostEdit->setEnabled(hasConnection && oscConnection);
	connectionListenPortSpin->setEnabled(hasConnection && oscConnection);
}

void SwitcherWorkspaceDock::RefreshAutomationEventLog()
{
	if (!automationEngine)
		return;

	QSignalBlocker blocker(eventLogList);
	eventLogList->clear();
	for (const auto &event : automationEngine->EventLog()) {
		const QString timestamp =
			QDateTime::fromMSecsSinceEpoch(event.timestampMs).toString(QStringLiteral("HH:mm:ss"));
		QString summary = QStringLiteral("[%1] %2").arg(event.level.toUpper(), event.scope);
		if (!event.macroId.isEmpty())
			summary += QStringLiteral(" (%1)").arg(event.macroId);
		summary += QStringLiteral(" - %1").arg(event.message);
		auto *item = new QListWidgetItem(QStringLiteral("%1  %2").arg(timestamp, summary), eventLogList);
		item->setToolTip(summary);
	}
	if (eventLogList->count() > 0)
		eventLogList->scrollToBottom();
}

void SwitcherWorkspaceDock::RefreshAutomationStatus()
{
	if (!automationEngine)
		return;

	const auto lifecycle = automationEngine->LifecycleState();
	QString lifecycleName = QStringLiteral("Loading");
	switch (lifecycle) {
	case SwitchAutomationLifecycleState::Loading:
		lifecycleName = QStringLiteral("Loading");
		break;
	case SwitchAutomationLifecycleState::Running:
		lifecycleName = QStringLiteral("Running");
		break;
	case SwitchAutomationLifecycleState::SceneCollectionSwitch:
		lifecycleName = QStringLiteral("Scene Collection Switch");
		break;
	case SwitchAutomationLifecycleState::ShuttingDown:
		lifecycleName = QStringLiteral("Shutting Down");
		break;
	}

	automationStatusSummaryLabel->setText(
		QStringLiteral("<b>Lifecycle</b><br/>%1<br/><br/>"
			       "<b>Document</b><br/>%2 macro(s), %3 variable(s), %4 queue(s), %5 connection(s)<br/><br/>"
			       "<b>Recent Activity</b><br/>%6 event(s) retained")
			.arg(lifecycleName)
			.arg(automationEngine->Document().macros.size())
			.arg(automationEngine->Variables().size())
			.arg(automationEngine->Queues().size())
			.arg(automationEngine->Connections().size())
			.arg(automationEngine->EventLog().size()));
}

void SwitcherWorkspaceDock::RefreshMacroEditor()
{
	const QString macroId = SelectedMacroId();
	const bool hasMacro = !macroId.isEmpty() && automationEngine;
	QScopedValueRollback<bool> guard(loadingAutomationUi, true);

	macroNameEdit->setEnabled(hasMacro);
	macroEnabledCheckBox->setEnabled(hasMacro);
	macroPausedCheckBox->setEnabled(hasMacro);
	macroRunModeCombo->setEnabled(hasMacro);
	macroTriggerTypeCombo->setEnabled(hasMacro);
	macroIntervalSpin->setEnabled(hasMacro);
	macroTriggerConnectionCombo->setEnabled(hasMacro);
	macroTriggerSceneCombo->setEnabled(hasMacro);
	macroTriggerStateCombo->setEnabled(hasMacro);
	macroTriggerKeyEdit->setEnabled(hasMacro);
	macroTriggerValueEdit->setEnabled(hasMacro);
	macroActionTypeCombo->setEnabled(hasMacro);
	macroActionConnectionCombo->setEnabled(hasMacro);
	macroActionSceneCombo->setEnabled(hasMacro);
	macroActionDelaySpin->setEnabled(hasMacro);
	macroActionKeyEdit->setEnabled(hasMacro);
	macroActionValueEdit->setEnabled(hasMacro);
	macroDeleteButton->setEnabled(hasMacro);
	macroRunButton->setEnabled(hasMacro);
	macroDuplicateButton->setEnabled(hasMacro);
	macroExportButton->setEnabled(hasMacro);

	if (!hasMacro) {
		macroNameEdit->clear();
		macroEnabledCheckBox->setChecked(false);
		macroPausedCheckBox->setChecked(false);
		macroLastResultLabel->setText(QStringLiteral("Select a macro to edit it."));
		return;
	}

	const auto macro = automationEngine->MacroById(macroId);
	macroNameEdit->setText(macro.name);
	macroEnabledCheckBox->setChecked(macro.enabled);
	macroPausedCheckBox->setChecked(macro.paused);
	macroRunModeCombo->setCurrentIndex(std::max(0, macroRunModeCombo->findData(macro.runMode)));
	macroTriggerTypeCombo->setCurrentIndex(std::max(0, macroTriggerTypeCombo->findData(macro.triggerType)));
	macroIntervalSpin->setValue(std::max(250, macro.intervalMs));
	macroTriggerKeyEdit->setText(macro.triggerValueKey);
	macroTriggerValueEdit->setText(macro.triggerValue);
	macroTriggerStateCombo->setCurrentIndex(std::max(0, macroTriggerStateCombo->findData(macro.desiredState)));
	macroActionTypeCombo->setCurrentIndex(std::max(0, macroActionTypeCombo->findData(macro.actionType)));
	macroActionDelaySpin->setValue(std::max(0, macro.actionDelayMs));
	macroActionKeyEdit->setText(macro.actionValueKey);
	macroActionValueEdit->setText(macro.actionValue);

	QSignalBlocker triggerConnectionBlocker(macroTriggerConnectionCombo);
	QSignalBlocker triggerSceneBlocker(macroTriggerSceneCombo);
	QSignalBlocker actionConnectionBlocker(macroActionConnectionCombo);
	QSignalBlocker actionSceneBlocker(macroActionSceneCombo);
	macroTriggerConnectionCombo->clear();
	macroTriggerSceneCombo->clear();
	macroActionConnectionCombo->clear();
	macroActionSceneCombo->clear();

	for (const auto &connection : automationEngine->Connections()) {
		const QString title = connection.name.isEmpty() ? connection.id : connection.name;
		macroTriggerConnectionCombo->addItem(QStringLiteral("%1 [%2]").arg(title, connection.typeId), connection.id);
		macroActionConnectionCombo->addItem(QStringLiteral("%1 [%2]").arg(title, connection.typeId), connection.id);
	}

	struct obs_frontend_source_list sceneList = {};
	obs_frontend_get_scenes(&sceneList);
	for (size_t index = 0; index < sceneList.sources.num; index++) {
		obs_source_t *source = sceneList.sources.array[index];
		if (!source)
			continue;

		const QString uuid = QString::fromUtf8(obs_source_get_uuid(source));
		const QString name = QString::fromUtf8(obs_source_get_name(source));
		macroTriggerSceneCombo->addItem(name, uuid);
		if (macro.actionType == QStringLiteral("switch_program_scene") ||
		    macro.actionType == QStringLiteral("switch_preview_scene"))
			macroActionSceneCombo->addItem(name, uuid);
	}
	obs_frontend_source_list_free(&sceneList);

	if (macro.actionType == QStringLiteral("switch_vertical_scene")) {
		for (const auto &scene : canvasManager->ScenesForCanvas(canvasManager->VerticalCanvasId()))
			macroActionSceneCombo->addItem(scene.name, scene.uuid);
	}

	if (macroTriggerSceneCombo->count() > 0) {
		const QString triggerSceneId = !macro.triggerSceneUuid.isEmpty() ? macro.triggerSceneUuid : macro.triggerSceneName;
		macroTriggerSceneCombo->setCurrentIndex(std::max(0, macroTriggerSceneCombo->findData(triggerSceneId)));
	}
	if (macroTriggerConnectionCombo->count() > 0) {
		const QString triggerConnectionId =
			!macro.triggerConnectionId.isEmpty() ? macro.triggerConnectionId : macro.triggerConnectionName;
		macroTriggerConnectionCombo->setCurrentIndex(
			std::max(0, macroTriggerConnectionCombo->findData(triggerConnectionId)));
	}
	if (macroActionSceneCombo->count() > 0) {
		const QString actionSceneId = !macro.actionSceneUuid.isEmpty() ? macro.actionSceneUuid : macro.actionSceneName;
		macroActionSceneCombo->setCurrentIndex(std::max(0, macroActionSceneCombo->findData(actionSceneId)));
	}
	if (macroActionConnectionCombo->count() > 0) {
		const QString actionConnectionId =
			!macro.actionConnectionId.isEmpty() ? macro.actionConnectionId : macro.actionConnectionName;
		macroActionConnectionCombo->setCurrentIndex(
			std::max(0, macroActionConnectionCombo->findData(actionConnectionId)));
	}

	const bool sceneTrigger = macro.triggerType == QStringLiteral("program_scene");
	const bool stateTrigger = macro.triggerType.endsWith(QStringLiteral("_state"));
	const bool oscTrigger = macro.triggerType == QStringLiteral("osc_receive");
	const bool timerTrigger =
		macro.triggerType == QStringLiteral("timer") || macro.runMode == QStringLiteral("repeat") || oscTrigger;
	const bool sceneAction = macro.actionType.startsWith(QStringLiteral("switch_")) &&
				 macro.actionType != QStringLiteral("open_vertical_window");
	const bool variableAction = macro.actionType == QStringLiteral("set_variable") || macro.actionType == QStringLiteral("remove_variable");
	const bool triggerConnection = oscTrigger;
	const bool triggerKey = oscTrigger;
	const bool triggerValue = oscTrigger;
	const bool actionConnection = macro.actionType == QStringLiteral("osc");
	const bool valueAction = variableAction || macro.actionType == QStringLiteral("http_get") ||
				 macro.actionType == QStringLiteral("add_recording_chapter") ||
				 macro.actionType == QStringLiteral("http_post") || macro.actionType == QStringLiteral("osc");
	const bool actionKey = variableAction || macro.actionType == QStringLiteral("copy_variable") ||
			       macro.actionType == QStringLiteral("osc");
	macroTriggerSceneCombo->setEnabled(hasMacro && sceneTrigger);
	macroTriggerConnectionCombo->setEnabled(hasMacro && triggerConnection);
	macroTriggerStateCombo->setEnabled(hasMacro && (stateTrigger || oscTrigger));
	macroIntervalSpin->setEnabled(hasMacro && timerTrigger);
	macroTriggerKeyEdit->setEnabled(hasMacro && triggerKey);
	macroTriggerValueEdit->setEnabled(hasMacro && triggerValue);
	macroActionConnectionCombo->setEnabled(hasMacro && actionConnection);
	macroActionSceneCombo->setEnabled(hasMacro && sceneAction);
	macroActionKeyEdit->setEnabled(hasMacro && actionKey);
	macroActionValueEdit->setEnabled(hasMacro && valueAction);
	macroTriggerKeyEdit->setPlaceholderText(oscTrigger ? QStringLiteral("/switch/live") : QString());
	macroTriggerValueEdit->setPlaceholderText(oscTrigger ? QStringLiteral("[1, \"live\", true]") : QString());
	macroActionKeyEdit->setPlaceholderText(actionConnection ? QStringLiteral("/switch/live") : QString());
	macroActionValueEdit->setPlaceholderText(actionConnection ? QStringLiteral("[1, \"live\", true]") : QString());
}

void SwitcherWorkspaceDock::RefreshAutomationPage()
{
	RefreshMacroList();
	RefreshMacroEditor();
	RefreshAutomationVariables();
	RefreshAutomationQueues();
	RefreshAutomationConnections();
	RefreshAutomationEventLog();
	RefreshAutomationStatus();
}

void SwitcherWorkspaceDock::UpdateSelectedMacroFromEditor()
{
	if (loadingAutomationUi || !automationEngine)
		return;

	const QString macroId = SelectedMacroId();
	if (macroId.isEmpty())
		return;

	auto macro = automationEngine->MacroById(macroId);
	if (macro.id.isEmpty())
		return;

	macro.name = macroNameEdit->text().trimmed().isEmpty() ? QStringLiteral("Macro") : macroNameEdit->text().trimmed();
	macro.enabled = macroEnabledCheckBox->isChecked();
	macro.paused = macroPausedCheckBox->isChecked();
	macro.runMode = macroRunModeCombo->currentData().toString();
	macro.triggerType = macroTriggerTypeCombo->currentData().toString();
	macro.intervalMs = macroIntervalSpin->value();
	macro.triggerConnectionId = macroTriggerConnectionCombo->currentData().toString();
	macro.triggerConnectionName = macroTriggerConnectionCombo->currentText();
	macro.desiredState = macroTriggerStateCombo->currentData().toBool();
	macro.triggerSceneUuid = macroTriggerSceneCombo->currentData().toString();
	macro.triggerSceneName = macroTriggerSceneCombo->currentText();
	macro.triggerValueKey = macroTriggerKeyEdit->text();
	macro.triggerValue = macroTriggerValueEdit->text();
	macro.actionType = macroActionTypeCombo->currentData().toString();
	macro.actionConnectionId = macroActionConnectionCombo->currentData().toString();
	macro.actionConnectionName = macroActionConnectionCombo->currentText();
	macro.actionSceneUuid = macroActionSceneCombo->currentData().toString();
	macro.actionSceneName = macroActionSceneCombo->currentText();
	macro.actionDelayMs = macroActionDelaySpin->value();
	macro.actionValueKey = macroActionKeyEdit->text();
	macro.actionValue = macroActionValueEdit->text();
	automationEngine->UpdateMacro(macro);
}

void SwitcherWorkspaceDock::UpdateSelectedConnectionFromEditor()
{
	if (loadingAutomationUi || !automationEngine)
		return;

	const QString connectionId = SelectedConnectionId();
	if (connectionId.isEmpty())
		return;

	auto connection = automationEngine->ConnectionById(connectionId);
	if (connection.id.isEmpty())
		return;

	connection.name = connectionNameEdit->text().trimmed().isEmpty() ? QStringLiteral("OSC Connection")
								 : connectionNameEdit->text().trimmed();
	connection.config.insert(QStringLiteral("mode"), connectionModeCombo->currentData().toString());
	connection.config.insert(QStringLiteral("remoteHost"), connectionRemoteHostEdit->text().trimmed());
	connection.config.insert(QStringLiteral("remotePort"), connectionRemotePortSpin->value());
	connection.config.insert(QStringLiteral("listenHost"), connectionListenHostEdit->text().trimmed());
	connection.config.insert(QStringLiteral("listenPort"), connectionListenPortSpin->value());
	automationEngine->UpsertConnection(connection, nullptr);
}

void SwitcherWorkspaceDock::EmitCanvasVendorStateChanged() const
{
	if (frontendShuttingDown)
		return;

	obs_data_t *data = BuildCanvasState();
	SwitcherEmitVendorEvent("CanvasStateChanged", data);
	SwitcherEmitVendorEvent("Canvas.SceneChanged", data);
	SwitcherEmitVendorEvent("Canvas.LinkedSceneChanged", data);
	SwitcherEmitVendorEvent("Canvas.OutputStateChanged", data);
	obs_data_release(data);
}

void SwitcherWorkspaceDock::EmitAutomationVendorStateChanged(const QString &macroId, bool success,
							     const QString &message) const
{
	if (frontendShuttingDown)
		return;

	obs_data_t *data = BuildAutomationState();
	if (!macroId.isEmpty())
		obs_data_set_string(data, "macroId", macroId.toUtf8().constData());
	obs_data_set_bool(data, "success", success);
	if (!message.isEmpty())
		obs_data_set_string(data, "message", message.toUtf8().constData());
	SwitcherEmitVendorEvent(macroId.isEmpty() ? "Automation.StateChanged" : "Automation.MacroTriggered", data);
	SwitcherEmitVendorEvent(macroId.isEmpty() ? "AutomationStateChanged" : "AutomationMacroTriggered", data);
	obs_data_release(data);
}

void SwitcherWorkspaceDock::EmitMotionVendorStateChanged(const QString &profileId) const
{
	if (frontendShuttingDown)
		return;

	obs_data_t *data = BuildMotionState();
	if (!profileId.isEmpty())
		obs_data_set_string(data, "profileId", profileId.toUtf8().constData());
	SwitcherEmitVendorEvent(profileId.isEmpty() ? "Motion.StateChanged" : "Motion.ProfileChanged", data);
	obs_data_release(data);
}
