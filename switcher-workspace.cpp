#include "switcher-workspace.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <QComboBox>
#include <QCheckBox>
#include <QClipboard>
#include <QDialog>
#include <QFormLayout>
#include <QGuiApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScreen>
#include <QShowEvent>
#include <QHideEvent>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <obs-module.h>

#include "switcher-settings.hpp"
#include "switcher-remote-manager.hpp"

#ifndef QT_UTF8
#define QT_UTF8(str) QString::fromUtf8(str)
#endif
#ifndef QT_TO_UTF8
#define QT_TO_UTF8(str) str.toUtf8().constData()
#endif

namespace {
constexpr int kMaxSwitcherSlots = 25;
constexpr auto kRemoteStateKey = "remote";
constexpr int kChromeMargin = 18;

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

int NextDetachedDockId()
{
	static int nextId = 0;
	return ++nextId;
}
} // namespace

SwitcherWorkspaceSlot::SwitcherWorkspaceSlot(int slotIndex_, QWidget *parent)
	: QFrame(parent),
	  slotIndex(slotIndex_),
	  dock(new SwitcherDock(QStringLiteral("switcher-workspace-slot-%1").arg(slotIndex_ + 1), false, this)),
	  titleLabel(new QLabel(this)),
	  contentStack(new QStackedWidget(this)),
	  emptyLabel(new QLabel(QT_UTF8(obs_module_text("SwitcherEmptySlot")), this)),
	  selectionButton(new QPushButton(this))
{
	setObjectName(QStringLiteral("switcherWorkspaceSlot"));
	setAttribute(Qt::WA_Hover, true);
	setMinimumSize(180, 120);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(10, 10, 10, 10);
	layout->setSpacing(10);

	titleLabel->setAlignment(Qt::AlignCenter);
	titleLabel->setWordWrap(true);
	titleLabel->setObjectName(QStringLiteral("switcherWorkspaceSlotTitle"));

	emptyLabel->setAlignment(Qt::AlignCenter);
	emptyLabel->setWordWrap(true);
	emptyLabel->setObjectName(QStringLiteral("switcherWorkspaceSlotEmpty"));

	dock->setHandleWidth(0);

	contentStack->addWidget(emptyLabel);
	contentStack->addWidget(dock);

	layout->addWidget(titleLabel);
	layout->addWidget(contentStack, 1);

	selectionButton->setObjectName(QStringLiteral("switcherWorkspaceSlotHitTarget"));
	selectionButton->setCursor(Qt::PointingHandCursor);
	selectionButton->setFlat(true);
	selectionButton->setFocusPolicy(Qt::NoFocus);
	selectionButton->setToolTip(GetEffectiveTitle());
	connect(selectionButton, &QPushButton::clicked, this, [this] { emit Clicked(slotIndex); });

	RefreshPresentation();
	RefreshSelectionState();
}

void SwitcherWorkspaceSlot::SetSource(const OBSSource &source)
{
	dock->SetSource(source);
	if (source) {
		dock->EnableShowActive();
		dock->EnableSwitchScene();
		if (previewActive)
			dock->EnablePreview();
	} else {
		dock->DisableSwitchScene();
		dock->DisablePreview();
		dock->DisableShowActive();
	}
	RefreshPresentation();
	RefreshActiveState();
}

OBSSource SwitcherWorkspaceSlot::GetSource()
{
	return dock->GetSource();
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
	if (auto source = dock->GetSource())
		return QT_UTF8(obs_source_get_name(source));
	return DefaultTitle();
}

void SwitcherWorkspaceSlot::SetPreviewActive(bool active)
{
	if (previewActive == active)
		return;

	previewActive = active;
	if (!dock->GetSource()) {
		dock->DisablePreview();
		return;
	}

	if (previewActive)
		dock->EnablePreview();
	else
		dock->DisablePreview();
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
	QMetaObject::invokeMethod(dock, "ActiveChanged", Qt::QueuedConnection);
}

void SwitcherWorkspaceSlot::ClearIfMatches(obs_source_t *source)
{
	if (dock->GetSource().Get() == source)
		SetSource(nullptr);
}

void SwitcherWorkspaceSlot::RefreshPresentation()
{
	titleLabel->setText(GetEffectiveTitle());
	contentStack->setCurrentWidget(dock->GetSource() ? static_cast<QWidget *>(dock) : static_cast<QWidget *>(emptyLabel));
	selectionButton->setToolTip(GetEffectiveTitle());
}

void SwitcherWorkspaceSlot::resizeEvent(QResizeEvent *event)
{
	QFrame::resizeEvent(event);
	selectionButton->setGeometry(rect());
	selectionButton->raise();
}

void SwitcherWorkspaceSlot::RefreshSelectionState()
{
	selectionButton->setProperty("selected", selected);
	style()->unpolish(selectionButton);
	style()->polish(selectionButton);
	selectionButton->update();
}

QString SwitcherWorkspaceSlot::DefaultTitle() const
{
	return QString("%1 %2").arg(QT_UTF8(obs_module_text("SwitcherSlot"))).arg(slotIndex + 1);
}

SwitcherWorkspaceDock::SwitcherWorkspaceDock(QMainWindow *parent)
	: QDockWidget(parent),
	  scrollArea(new QScrollArea(this)),
	  gridContainer(new QWidget(scrollArea)),
	  gridLayout(new QGridLayout(gridContainer)),
	  settingsButton(new QToolButton(scrollArea->viewport())),
	  settingsPopup(new QDialog(this, Qt::Popup | Qt::FramelessWindowHint)),
	  slotList(new QListWidget(settingsPopup)),
	  layoutCombo(new QComboBox(settingsPopup)),
	  sceneCombo(new QComboBox(settingsPopup)),
	  titleEdit(new QLineEdit(settingsPopup)),
	  clearSlotButton(new QPushButton(QT_UTF8(obs_module_text("Clear")), settingsPopup)),
	  detachSlotButton(new QPushButton(QT_UTF8(obs_module_text("OpenDetachedView")), settingsPopup)),
	  legacyManagerButton(new QPushButton(QT_UTF8(obs_module_text("OpenLegacyDockManager")), settingsPopup)),
	  remoteEnabledCheckBox(new QCheckBox(QT_UTF8(obs_module_text("SwitcherRemoteEnable")), settingsPopup)),
	  remoteAutoStartCheckBox(new QCheckBox(QT_UTF8(obs_module_text("SwitcherRemoteAutoStart")), settingsPopup)),
	  remoteResolutionCombo(new QComboBox(settingsPopup)),
	  remoteFpsCombo(new QComboBox(settingsPopup)),
	  remoteUrlEdit(new QLineEdit(settingsPopup)),
	  remoteStatusLabel(new QLabel(settingsPopup)),
	  remoteCopyButton(new QPushButton(QT_UTF8(obs_module_text("SwitcherRemoteCopyUrl")), settingsPopup)),
	  remoteRegenerateButton(new QPushButton(QT_UTF8(obs_module_text("SwitcherRemoteRegenerateToken")), settingsPopup)),
	  remoteRestartButton(new QPushButton(QT_UTF8(obs_module_text("SwitcherRemoteRestart")), settingsPopup))
{
	setWindowTitle(QT_UTF8(obs_module_text("Switcher")));
	setAllowedAreas(Qt::AllDockWidgetAreas);

	auto *root = new QWidget(this);
	root->setObjectName(QStringLiteral("switcherWorkspaceRoot"));
	root->setAttribute(Qt::WA_StyledBackground, true);
	root->setStyleSheet(
		"#switcherWorkspaceRoot {"
		"  background: qlineargradient(x1:0, y1:0, x2:1, y2:1,"
		"    stop:0 rgba(2, 6, 23, 255),"
		"    stop:0.45 rgba(15, 23, 42, 255),"
		"    stop:1 rgba(30, 41, 59, 255));"
		"}"
		"#switcherWorkspaceSlot {"
		"  background-color: rgba(15, 23, 42, 228);"
		"  border: 1px solid rgba(148, 163, 184, 52);"
		"  border-radius: 20px;"
		"}"
		"#switcherWorkspaceSlotTitle {"
		"  color: rgba(248, 250, 252, 242);"
		"  font-weight: 700;"
		"  font-size: 13px;"
		"}"
		"#switcherWorkspaceSlotEmpty {"
		"  color: rgba(148, 163, 184, 220);"
		"  font-size: 15px;"
		"}"
		"#switcherWorkspaceSlotHitTarget {"
		"  background: transparent;"
		"  border: 2px solid transparent;"
		"  border-radius: 20px;"
		"}"
		"#switcherWorkspaceSlotHitTarget:hover {"
		"  background-color: rgba(248, 250, 252, 12);"
		"  border-color: rgba(125, 211, 252, 100);"
		"}"
		"#switcherWorkspaceSlotHitTarget[selected=\"true\"] {"
		"  background-color: rgba(248, 250, 252, 18);"
		"  border-color: rgba(248, 250, 252, 220);"
		"}"
		"#switcherWorkspaceSettingsSurface {"
		"  background-color: rgba(2, 6, 23, 228);"
		"  border: 1px solid rgba(148, 163, 184, 58);"
		"  border-radius: 22px;"
		"}"
		"#switcherWorkspaceSettingsSurface QLabel {"
		"  color: rgba(226, 232, 240, 238);"
		"}"
		"#switcherWorkspaceSettingsSurface QListWidget,"
		"#switcherWorkspaceSettingsSurface QComboBox,"
		"#switcherWorkspaceSettingsSurface QLineEdit {"
		"  background-color: rgba(15, 23, 42, 232);"
		"  color: rgba(248, 250, 252, 245);"
		"  border: 1px solid rgba(148, 163, 184, 70);"
		"  border-radius: 12px;"
		"  padding: 8px 10px;"
		"}"
		"#switcherWorkspaceSettingsSurface QPushButton,"
		"#switcherWorkspaceSettingsSurface QCheckBox {"
		"  color: rgba(248, 250, 252, 245);"
		"}"
		"#switcherWorkspaceSettingsSurface QPushButton {"
		"  background-color: rgba(30, 41, 59, 236);"
		"  border: 1px solid rgba(148, 163, 184, 74);"
		"  border-radius: 12px;"
		"  padding: 9px 12px;"
		"}"
		"#switcherWorkspaceSettingsSurface QPushButton:hover {"
		"  border-color: rgba(125, 211, 252, 120);"
		"}"
		"#switcherWorkspaceSettingsButton {"
		"  background-color: rgba(2, 6, 23, 232);"
		"  border: 1px solid rgba(148, 163, 184, 58);"
		"  border-radius: 16px;"
		"  color: rgba(248, 250, 252, 248);"
		"  font-size: 22px;"
		"  font-weight: 700;"
		"}"
		"#switcherWorkspaceSettingsButton:hover {"
		"  background-color: rgba(15, 23, 42, 244);"
		"  border-color: rgba(125, 211, 252, 120);"
		"}"
		"#switcherWorkspaceSettingsButton:pressed {"
		"  background-color: rgba(30, 41, 59, 248);"
		"}");

	auto *rootLayout = new QVBoxLayout(root);
	rootLayout->setContentsMargins(0, 0, 0, 0);
	rootLayout->addWidget(scrollArea);

	scrollArea->setWidgetResizable(true);
	scrollArea->setFrameShape(QFrame::NoFrame);
	scrollArea->setWidget(gridContainer);

	gridLayout->setContentsMargins(18, 18, 18, 18);
	gridLayout->setSpacing(16);

	settingsButton->setObjectName(QStringLiteral("switcherWorkspaceSettingsButton"));
	settingsButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
	settingsButton->setText(QStringLiteral("⚙"));
	settingsButton->setToolTip(QT_UTF8(obs_module_text("SwitcherOpenSettings")));
	settingsButton->setCursor(Qt::PointingHandCursor);
	settingsButton->setAutoRaise(false);
	settingsButton->setFixedSize(52, 52);

	for (int index = 0; index < kMaxSwitcherSlots; index++) {
		auto *slot = new SwitcherWorkspaceSlot(index, gridContainer);
		connect(slot, &SwitcherWorkspaceSlot::Clicked, this, &SwitcherWorkspaceDock::SelectSlot);
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

	auto *settingsRootLayout = new QVBoxLayout(settingsPopup);
	settingsRootLayout->setContentsMargins(0, 0, 0, 0);
	settingsRootLayout->setSpacing(0);

	auto *settingsSurface = new QFrame(settingsPopup);
	settingsSurface->setObjectName(QStringLiteral("switcherWorkspaceSettingsSurface"));
	settingsSurface->setAttribute(Qt::WA_StyledBackground, true);
	settingsRootLayout->addWidget(settingsSurface);

	auto *settingsLayout = new QVBoxLayout(settingsSurface);
	settingsLayout->setContentsMargins(18, 18, 18, 18);
	settingsLayout->setSpacing(14);

	auto *settingsTitle = new QLabel(QT_UTF8(obs_module_text("SwitcherSettings")), settingsSurface);
	settingsTitle->setStyleSheet("color: rgba(248, 250, 252, 245); font-size: 17px; font-weight: 700;");
	settingsLayout->addWidget(settingsTitle);

	auto *layoutForm = new QFormLayout;
	layoutForm->setLabelAlignment(Qt::AlignLeft);
	layoutForm->addRow(QT_UTF8(obs_module_text("SwitcherLayout")), layoutCombo);
	settingsLayout->addLayout(layoutForm);

	auto *slotsLabel = new QLabel(QT_UTF8(obs_module_text("SwitcherSlots")), settingsSurface);
	slotsLabel->setStyleSheet("color: rgba(248, 250, 252, 240); font-weight: 700;");
	settingsLayout->addWidget(slotsLabel);
	settingsLayout->addWidget(slotList);

	auto *editorForm = new QFormLayout;
	editorForm->setLabelAlignment(Qt::AlignLeft);
	editorForm->addRow(QT_UTF8(obs_module_text("Source")), sceneCombo);
	editorForm->addRow(QT_UTF8(obs_module_text("Title")), titleEdit);
	settingsLayout->addLayout(editorForm);

	settingsLayout->addWidget(clearSlotButton);
	settingsLayout->addWidget(detachSlotButton);
	settingsLayout->addWidget(legacyManagerButton);

	auto *remoteForm = new QFormLayout;
	remoteForm->setLabelAlignment(Qt::AlignLeft);
	auto *remoteLabel = new QLabel(QT_UTF8(obs_module_text("SwitcherRemote")), settingsSurface);
	remoteLabel->setStyleSheet("color: rgba(248, 250, 252, 240); font-weight: 700;");
	settingsLayout->addWidget(remoteLabel);
	settingsLayout->addWidget(remoteEnabledCheckBox);
	settingsLayout->addWidget(remoteAutoStartCheckBox);
	remoteForm->addRow(QT_UTF8(obs_module_text("SwitcherRemoteResolution")), remoteResolutionCombo);
	remoteForm->addRow(QT_UTF8(obs_module_text("SwitcherRemoteFps")), remoteFpsCombo);
	remoteForm->addRow(QT_UTF8(obs_module_text("SwitcherRemoteUrl")), remoteUrlEdit);
	remoteForm->addRow(QT_UTF8(obs_module_text("SwitcherRemoteStatus")), remoteStatusLabel);
	settingsLayout->addLayout(remoteForm);
	settingsLayout->addWidget(remoteCopyButton);
	settingsLayout->addWidget(remoteRegenerateButton);
	settingsLayout->addWidget(remoteRestartButton);
	settingsLayout->addStretch(1);

	setWidget(root);
	resize(1280, 820);
	settingsPopup->setAttribute(Qt::WA_StyledBackground, true);
	settingsPopup->setMinimumWidth(360);
	settingsPopup->setMaximumWidth(440);

	connect(layoutCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &SwitcherWorkspaceDock::LayoutChanged);
	connect(slotList, &QListWidget::currentRowChanged, this, &SwitcherWorkspaceDock::SelectedSlotChanged);
	connect(sceneCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &SwitcherWorkspaceDock::SelectedSceneChanged);
	connect(titleEdit, &QLineEdit::textChanged, this, &SwitcherWorkspaceDock::SelectedTitleChanged);
	connect(clearSlotButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::ClearSelectedSlot);
	connect(detachSlotButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::OpenSelectedSlotAsDock);
	connect(legacyManagerButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::OpenLegacyDockManager);
	connect(remoteEnabledCheckBox, &QCheckBox::checkStateChanged, this, &SwitcherWorkspaceDock::RemoteEnabledChanged);
	connect(remoteAutoStartCheckBox, &QCheckBox::checkStateChanged, this, &SwitcherWorkspaceDock::RemoteAutoStartChanged);
	connect(remoteResolutionCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
		&SwitcherWorkspaceDock::RemoteResolutionChanged);
	connect(remoteFpsCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &SwitcherWorkspaceDock::RemoteFpsChanged);
	connect(remoteCopyButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::CopyRemoteUrl);
	connect(remoteRegenerateButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::RegenerateRemoteToken);
	connect(remoteRestartButton, &QPushButton::clicked, this, &SwitcherWorkspaceDock::RestartRemote);
	connect(settingsButton, &QToolButton::clicked, this, &SwitcherWorkspaceDock::ToggleSettingsPopup);
	connect(SwitcherRemoteManager::Instance(), &SwitcherRemoteManager::StateChanged, this,
		&SwitcherWorkspaceDock::RemoteStateUpdated);

	RefreshGrid();
	RefreshSlotList();
	RefreshSceneOptions();
	RefreshSelectedSlotEditor();
	RemoteStateUpdated();
	PositionChrome();
}

obs_data_t *SwitcherWorkspaceDock::SaveState()
{
	obs_data_t *data = obs_data_create();
	obs_data_set_int(data, "visible_slots", VisibleSlotCount());
	obs_data_set_bool(data, "visible", isVisible());
	obs_data_set_bool(data, "floating", isFloating());
	obs_data_set_int(data, "dockarea",
			 static_cast<int>(static_cast<QMainWindow *>(obs_frontend_get_main_window())->dockWidgetArea(this)));
	obs_data_set_string(data, "geometry", saveGeometry().toBase64().constData());

	obs_data_array_t *slotArray = obs_data_array_create();
	for (const auto &slot : slotWidgets) {
		obs_data_t *slotData = obs_data_create();
		if (auto source = slot->GetSource())
			obs_data_set_string(slotData, "source_name", obs_source_get_name(source));
		if (!slot->GetCustomTitle().isEmpty())
			obs_data_set_string(slotData, "title", QT_TO_UTF8(slot->GetCustomTitle()));
		obs_data_array_push_back(slotArray, slotData);
		obs_data_release(slotData);
	}
	obs_data_set_array(data, "slots", slotArray);
	obs_data_array_release(slotArray);

	obs_data_t *remoteData = SwitcherRemoteManager::Instance()->SaveState();
	obs_data_set_obj(data, kRemoteStateKey, remoteData);
	obs_data_release(remoteData);

	return data;
}

void SwitcherWorkspaceDock::LoadState(obs_data_t *data)
{
	loadingState = true;

	for (const auto &slot : slotWidgets) {
		slot->SetCustomTitle(QString());
		slot->SetSource(nullptr);
	}

	int layoutIndex = layoutCombo->findData(4);
	if (data) {
		layoutIndex = layoutCombo->findData(NormalizeVisibleSlotCount(static_cast<int>(obs_data_get_int(data, "visible_slots"))));
	}
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

				const char *sourceName = obs_data_get_string(slotData, "source_name");
				if (sourceName && strlen(sourceName) > 0) {
					obs_source_t *source = obs_get_source_by_name(sourceName);
					slotWidgets[index]->SetSource(source);
					if (source)
						obs_source_release(source);
				}

				obs_data_release(slotData);
			}
			obs_data_array_release(slotArray);
		}

		auto *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		const auto dockArea = static_cast<Qt::DockWidgetArea>(obs_data_get_int(data, "dockarea"));
		if (dockArea != Qt::NoDockWidgetArea && mainWindow->dockWidgetArea(this) != dockArea)
			mainWindow->addDockWidget(dockArea, this);

		const bool floating = obs_data_get_bool(data, "floating");
		if (isFloating() != floating)
			setFloating(floating);

		const char *geometry = obs_data_get_string(data, "geometry");
		if (geometry && strlen(geometry) > 0)
			restoreGeometry(QByteArray::fromBase64(QByteArray(geometry)));

		if (obs_data_get_bool(data, "visible"))
			show();
		else
			hide();
	}

	loadingState = false;

	obs_data_t *remoteData = data ? obs_data_get_obj(data, kRemoteStateKey) : nullptr;
	SwitcherRemoteManager::Instance()->LoadState(remoteData);
	if (remoteData)
		obs_data_release(remoteData);

	RefreshGrid();
	RefreshSlotList();
	RefreshSceneOptions();
	RefreshSelectedSlotEditor();
	ApplyPreviewState(isVisible());
	RemoteStateUpdated();
	emit WorkspaceStateChanged();
}

void SwitcherWorkspaceDock::HandleSourceRemoved(obs_source_t *source)
{
	for (const auto &slot : slotWidgets)
		slot->ClearIfMatches(source);

	RefreshSlotList();
	RefreshSceneOptions();
	RefreshSelectedSlotEditor();
	emit WorkspaceStateChanged();
}

void SwitcherWorkspaceDock::HandleFrontendEvent(enum obs_frontend_event event)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
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
			for (int index = 0; index < VisibleSlotCount(); index++)
				slotWidgets[index]->RefreshActiveState();
		RefreshSlotList();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
	case OBS_FRONTEND_EVENT_EXIT:
		ClearSceneCollectionState();
		ApplyPreviewState(false);
		break;
	default:
		break;
	}
	emit WorkspaceStateChanged();
}

void SwitcherWorkspaceDock::ClearSceneCollectionState()
{
	loadingState = true;
	for (const auto &slot : slotWidgets) {
		slot->SetCustomTitle(QString());
		slot->SetSource(nullptr);
	}
	const int layoutIndex = layoutCombo->findData(4);
	if (layoutIndex >= 0)
		layoutCombo->setCurrentIndex(layoutIndex);
	loadingState = false;

	RefreshGrid();
	RefreshSlotList();
	RefreshSceneOptions();
	RefreshSelectedSlotEditor();
	emit WorkspaceStateChanged();
}

void SwitcherWorkspaceDock::showEvent(QShowEvent *event)
{
	QDockWidget::showEvent(event);
	RefreshSceneOptions();
	RefreshSlotList();
	RefreshSelectedSlotEditor();
	ApplyPreviewState(true);
	for (int index = 0; index < VisibleSlotCount(); index++)
		slotWidgets[index]->RefreshActiveState();
	PositionChrome();
}

void SwitcherWorkspaceDock::hideEvent(QHideEvent *event)
{
	ApplyPreviewState(false);
	if (settingsPopup->isVisible())
		settingsPopup->hide();
	QDockWidget::hideEvent(event);
}

void SwitcherWorkspaceDock::resizeEvent(QResizeEvent *event)
{
	QDockWidget::resizeEvent(event);
	PositionChrome();
}

void SwitcherWorkspaceDock::LayoutChanged()
{
	if (loadingState)
		return;
	RefreshGrid();
	RefreshSlotList();
	RefreshSceneOptions();
	RefreshSelectedSlotEditor();
	emit WorkspaceStateChanged();
}

void SwitcherWorkspaceDock::SelectedSlotChanged()
{
	RefreshSelectionHighlights();
	RefreshSceneOptions();
	RefreshSelectedSlotEditor();
}

void SwitcherWorkspaceDock::SelectSlot(int slotIndex)
{
	if (slotIndex < 0 || slotIndex >= VisibleSlotCount())
		return;

	slotList->setCurrentRow(slotIndex);
}

void SwitcherWorkspaceDock::SelectedSceneChanged()
{
	if (loadingState)
		return;

	auto *slot = CurrentSlot();
	if (!slot)
		return;

	const auto sourceName = sceneCombo->currentData().toByteArray();
	obs_source_t *source = sourceName.isEmpty() ? nullptr : obs_get_source_by_name(sourceName.constData());
	slot->SetSource(source);
	if (source)
		obs_source_release(source);

	RefreshSlotList();
	RefreshSelectedSlotEditor();
	emit WorkspaceStateChanged();
}

void SwitcherWorkspaceDock::SelectedTitleChanged(const QString &title)
{
	if (loadingState)
		return;

	auto *slot = CurrentSlot();
	if (!slot)
		return;

	slot->SetCustomTitle(title);
	RefreshSlotList();
	emit WorkspaceStateChanged();
}

void SwitcherWorkspaceDock::ClearSelectedSlot()
{
	auto *slot = CurrentSlot();
	if (!slot)
		return;

	loadingState = true;
	slot->SetCustomTitle(QString());
	slot->SetSource(nullptr);
	loadingState = false;

	RefreshSlotList();
	RefreshSceneOptions();
	RefreshSelectedSlotEditor();
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
	const auto id = QString("switcher-detached-%1").arg(NextDetachedDockId());
	auto *tmp = new SwitcherDock(title, false, static_cast<QMainWindow *>(obs_frontend_get_main_window()));
	tmp->SetSource(source);
	tmp->EnablePreview();
	tmp->EnableSwitchScene();
	tmp->EnableShowActive();

	const auto dockId = id.toUtf8();
	const auto dockTitle = title.toUtf8();
	if (!obs_frontend_add_dock_by_id(dockId.constData(), dockTitle.constData(), tmp)) {
		delete tmp;
		return;
	}

	switcher_docks.push_back(tmp);
	if (auto *dockWidget = qobject_cast<QDockWidget *>(tmp->parentWidget())) {
		dockWidget->show();
		dockWidget->raise();
		dockWidget->setFloating(true);
	}
}

void SwitcherWorkspaceDock::OpenLegacyDockManager()
{
	const auto dialog = new SwitcherSettingsDialog(static_cast<QMainWindow *>(obs_frontend_get_main_window()));
	dialog->setAttribute(Qt::WA_DeleteOnClose, true);
	dialog->show();
}

void SwitcherWorkspaceDock::ToggleSettingsPopup()
{
	if (settingsPopup->isVisible()) {
		settingsPopup->hide();
		return;
	}

	RefreshSceneOptions();
	RefreshSelectedSlotEditor();
	RefreshRemoteControls();
	PositionSettingsPopup();
	settingsPopup->show();
	settingsPopup->raise();
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
	auto *combo = static_cast<QComboBox *>(data);
	combo->addItem(QT_UTF8(sourceName), QByteArray(sourceName));
	return true;
}

void SwitcherWorkspaceDock::ApplyPreviewState(bool enabled)
{
	previewsEnabled = enabled;
	const int visibleSlots = VisibleSlotCount();
	for (size_t index = 0; index < slotWidgets.size(); index++)
		slotWidgets[index]->SetPreviewActive(enabled && static_cast<int>(index) < visibleSlots);
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
	PositionChrome();
}

void SwitcherWorkspaceDock::RefreshSceneOptions()
{
	auto *slot = CurrentSlot();
	const QString selectedSourceName =
		slot && slot->GetSource() ? QT_UTF8(obs_source_get_name(slot->GetSource())) : QString();

	QSignalBlocker blocker(sceneCombo);
	sceneCombo->clear();
	sceneCombo->addItem(QT_UTF8(obs_module_text("SwitcherNoScene")), QByteArray(""));
	obs_enum_scenes(AddSceneOption, sceneCombo);

	if (!selectedSourceName.isEmpty()) {
		const auto sourceData = selectedSourceName.toUtf8();
		int index = sceneCombo->findData(sourceData);
		if (index < 0) {
			sceneCombo->addItem(selectedSourceName, sourceData);
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
	const int selectedIndex = SelectedSlotIndex();
	for (int index = 0; index < static_cast<int>(slotWidgets.size()); index++)
		slotWidgets[static_cast<size_t>(index)]->SetSelected(index == selectedIndex && index < VisibleSlotCount());
}

void SwitcherWorkspaceDock::PositionChrome()
{
	if (!scrollArea || !scrollArea->viewport())
		return;

	auto *viewport = scrollArea->viewport();
	settingsButton->move(viewport->width() - settingsButton->width() - kChromeMargin, kChromeMargin);
	settingsButton->raise();

	if (settingsPopup->isVisible())
		PositionSettingsPopup();
}

void SwitcherWorkspaceDock::PositionSettingsPopup()
{
	settingsPopup->adjustSize();

	const QPoint desiredTopRight = settingsButton->mapToGlobal(QPoint(settingsButton->width(), settingsButton->height() + 10));
	QRect popupRect(QPoint(desiredTopRight.x() - settingsPopup->width(), desiredTopRight.y()), settingsPopup->size());

	QScreen *screen = QGuiApplication::screenAt(desiredTopRight);
	if (!screen)
		screen = QGuiApplication::primaryScreen();
	if (screen) {
		const QRect available = screen->availableGeometry();
		if (popupRect.right() > available.right())
			popupRect.moveRight(available.right() - 8);
		if (popupRect.left() < available.left())
			popupRect.moveLeft(available.left() + 8);
		if (popupRect.bottom() > available.bottom())
			popupRect.moveBottom(available.bottom() - 8);
		if (popupRect.top() < available.top())
			popupRect.moveTop(available.top() + 8);
	}

	settingsPopup->move(popupRect.topLeft());
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
