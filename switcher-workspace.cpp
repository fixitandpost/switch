#include "switcher-workspace.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <QApplication>
#include <QAbstractItemView>
#include <QAction>
#include <QComboBox>
#include <QCheckBox>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QFormLayout>
#include <QGuiApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
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
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QToolButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>

#include <obs-module.h>

#include "switcher-settings.hpp"
#include "switcher-remote-manager.hpp"
#include "util/config-file.h"

#ifndef QT_UTF8
#define QT_UTF8(str) QString::fromUtf8(str)
#endif
#ifndef QT_TO_UTF8
#define QT_TO_UTF8(str) str.toUtf8().constData()
#endif

namespace {
constexpr int kMaxSwitcherSlots = 25;
constexpr auto kRemoteStateKey = "remote";
SwitcherWorkspaceDock *gWorkspaceDock = nullptr;

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
} // namespace

SwitcherWorkspacePreview::SwitcherWorkspacePreview(QWidget *parent) : QWidget(parent)
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
}

SwitcherWorkspacePreview::~SwitcherWorkspacePreview()
{
	DeactivatePreview();
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

	if (event->button() == Qt::RightButton || controlClickContext)
		return true;

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
			if (previewActive && display && display->GetDisplay())
				obs_display_add_draw_callback(display->GetDisplay(), DrawPreview, this);
		});
	}

	display->setCursor(source && obs_source_is_scene(source) ? Qt::PointingHandCursor : Qt::ArrowCursor);
	previewActive = true;
	obs_source_inc_showing(source);
	display->show();
	display->CreateDisplay();
	QTimer::singleShot(0, this, [this]() {
		if (previewActive && display)
			display->CreateDisplay(true);
	});
}

void SwitcherWorkspacePreview::DeactivatePreview()
{
	if (!display)
		return;

	const bool wasActive = previewActive;
	previewActive = false;

	if (auto *obsDisplay = display->GetDisplay())
		obs_display_remove_draw_callback(obsDisplay, DrawPreview, this);

	display->hide();
	display->DestroyDisplay();
	if (wasActive && source)
		obs_source_dec_showing(source);
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

	contentStack->addWidget(emptyLabel);
	contentStack->addWidget(previewWidget);

	layout->addWidget(contentStack, 1);

	titleLabel->setObjectName(QStringLiteral("switcherWorkspaceSlotTitle"));
	titleLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);

	auto *editAction = contextMenu->addAction(QT_UTF8(obs_module_text("SwitcherEditView")));
	auto *detachAction = contextMenu->addAction(QT_UTF8(obs_module_text("OpenDetachedView")));
	auto *advancedAction = contextMenu->addAction(QT_UTF8(obs_module_text("SwitcherAdvancedSettings")));
	connect(editAction, &QAction::triggered, this, [this] { emit ConfigureRequested(slotIndex); });
	connect(detachAction, &QAction::triggered, this, [this] { emit DetachRequested(slotIndex); });
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

QString SwitcherWorkspaceSlot::DefaultTitle() const
{
	return QString("%1 %2").arg(QT_UTF8(obs_module_text("SwitcherSlot"))).arg(slotIndex + 1);
}

SwitcherWorkspaceDock::SwitcherWorkspaceDock(QMainWindow *parent)
	: QWidget(parent, Qt::Window),
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
	  remoteSettingsSection(new QWidget(workspacePage)),
	  slotList(new QListWidget(workspacePage)),
	  layoutCombo(new QComboBox(workspacePage)),
	  sceneCombo(new QComboBox(slotPage)),
	  titleEdit(new QLineEdit(slotPage)),
	  clearSlotButton(new QPushButton(QT_UTF8(obs_module_text("Clear")), slotPage)),
	  detachSlotButton(new QPushButton(QT_UTF8(obs_module_text("OpenDetachedView")), slotPage)),
	  legacyManagerButton(new QPushButton(QT_UTF8(obs_module_text("OpenLegacyDockManager")), workspacePage)),
	  remoteEnabledCheckBox(new QCheckBox(QT_UTF8(obs_module_text("SwitcherRemoteEnable")), workspacePage)),
	  remoteAutoStartCheckBox(new QCheckBox(QT_UTF8(obs_module_text("SwitcherRemoteAutoStart")), workspacePage)),
	  remoteResolutionCombo(new QComboBox(workspacePage)),
	  remoteFpsCombo(new QComboBox(workspacePage)),
	  remoteUrlEdit(new QLineEdit(workspacePage)),
	  remoteStatusLabel(new QLabel(workspacePage)),
	  remoteCopyButton(new QPushButton(QT_UTF8(obs_module_text("SwitcherRemoteCopyUrl")), workspacePage)),
	  remoteRegenerateButton(new QPushButton(QT_UTF8(obs_module_text("SwitcherRemoteRegenerateToken")), workspacePage)),
	  remoteRestartButton(new QPushButton(QT_UTF8(obs_module_text("SwitcherRemoteRestart")), workspacePage))
{
	gWorkspaceDock = this;
	setObjectName(QStringLiteral("switcherWorkspaceRoot"));
	setAttribute(Qt::WA_StyledBackground, true);
	setWindowTitle(QT_UTF8(obs_module_text("Switcher")));
	setWindowFlag(Qt::WindowMinMaxButtonsHint, true);
	setMinimumSize(1120, 720);
	resize(1400, 860);

	auto *rootLayout = new QVBoxLayout(this);
	rootLayout->setContentsMargins(0, 0, 0, 0);
	rootLayout->setSpacing(0);

	contentSplitter->setChildrenCollapsible(false);
	contentSplitter->setHandleWidth(6);
	contentSplitter->setOpaqueResize(false);
	contentSplitter->addWidget(scrollArea);
	contentSplitter->addWidget(inspectorFrame);
	contentSplitter->setStretchFactor(0, 1);
	contentSplitter->setStretchFactor(1, 0);
	rootLayout->addWidget(contentSplitter, 1);

	scrollArea->setWidgetResizable(true);
	scrollArea->setFrameShape(QFrame::NoFrame);
	scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	scrollArea->setWidget(gridContainer);

	gridLayout->setContentsMargins(22, 22, 22, 22);
	gridLayout->setSpacing(18);

	inspectorFrame->setObjectName(QStringLiteral("switcherWorkspaceInspector"));
	inspectorFrame->setAttribute(Qt::WA_StyledBackground, true);
	inspectorFrame->setMinimumWidth(320);
	inspectorFrame->setMaximumWidth(460);
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
		connect(slot, &SwitcherWorkspaceSlot::AdvancedRequested, this, &SwitcherWorkspaceDock::OpenAdvancedSettings);
		slotWidgets.push_back(slot);
	}

	layoutCombo->addItem(QT_UTF8(obs_module_text("SwitcherLayout2x2")), 4);
	layoutCombo->addItem(QT_UTF8(obs_module_text("SwitcherLayout3x3")), 9);
	layoutCombo->addItem(QT_UTF8(obs_module_text("SwitcherLayout4x4")), 16);
	layoutCombo->addItem(QT_UTF8(obs_module_text("SwitcherLayout5x5")), 25);

	slotList->setMaximumHeight(220);
	slotList->setAlternatingRowColors(true);
	slotList->hide();

	remoteResolutionCombo->addItem(QT_UTF8(obs_module_text("SwitcherRemote720p")), QSize(1280, 720));
	remoteResolutionCombo->addItem(QT_UTF8(obs_module_text("SwitcherRemote1080p")), QSize(1920, 1080));

	remoteFpsCombo->addItem(QStringLiteral("5"), 5);
	remoteFpsCombo->addItem(QStringLiteral("10"), 10);
	remoteFpsCombo->addItem(QStringLiteral("15"), 15);
	remoteFpsCombo->addItem(QStringLiteral("30"), 30);

	remoteUrlEdit->setReadOnly(true);
	remoteStatusLabel->setWordWrap(true);

	auto *workspacePageLayout = new QVBoxLayout(workspacePage);
	workspacePageLayout->setContentsMargins(18, 0, 18, 18);
	workspacePageLayout->setSpacing(14);

	auto *workspaceSettingsLayout = new QVBoxLayout(workspaceSettingsSection);
	workspaceSettingsLayout->setContentsMargins(0, 0, 0, 0);
	workspaceSettingsLayout->setSpacing(14);

	auto *layoutForm = new QFormLayout;
	layoutForm->setLabelAlignment(Qt::AlignLeft);
	layoutForm->addRow(QT_UTF8(obs_module_text("SwitcherLayout")), layoutCombo);
	workspaceSettingsLayout->addLayout(layoutForm);
	workspaceSettingsLayout->addWidget(legacyManagerButton);
	workspacePageLayout->addWidget(workspaceSettingsSection);

	auto *slotPageLayout = new QVBoxLayout(slotPage);
	slotPageLayout->setContentsMargins(18, 0, 18, 18);
	slotPageLayout->setSpacing(14);

	auto *slotEditorLayout = new QVBoxLayout(slotEditorSection);
	slotEditorLayout->setContentsMargins(0, 0, 0, 0);
	slotEditorLayout->setSpacing(12);

	auto *editorForm = new QFormLayout;
	editorForm->setLabelAlignment(Qt::AlignLeft);
	editorForm->addRow(QT_UTF8(obs_module_text("Source")), sceneCombo);
	editorForm->addRow(QT_UTF8(obs_module_text("Title")), titleEdit);
	slotEditorLayout->addLayout(editorForm);
	slotEditorLayout->addWidget(clearSlotButton);
	slotEditorLayout->addWidget(detachSlotButton);
	slotPageLayout->addWidget(slotEditorSection);
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
	remoteForm->setLabelAlignment(Qt::AlignLeft);
	remoteForm->addRow(QT_UTF8(obs_module_text("SwitcherRemoteResolution")), remoteResolutionCombo);
	remoteForm->addRow(QT_UTF8(obs_module_text("SwitcherRemoteFps")), remoteFpsCombo);
	remoteForm->addRow(QT_UTF8(obs_module_text("SwitcherRemoteUrl")), remoteUrlEdit);
	remoteForm->addRow(QT_UTF8(obs_module_text("SwitcherRemoteStatus")), remoteStatusLabel);
	remoteSettingsLayout->addLayout(remoteForm);
	remoteSettingsLayout->addWidget(remoteCopyButton);
	remoteSettingsLayout->addWidget(remoteRegenerateButton);
	remoteSettingsLayout->addWidget(remoteRestartButton);
	workspacePageLayout->addWidget(remoteSettingsSection);
	workspacePageLayout->addStretch(1);

	connect(layoutCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &SwitcherWorkspaceDock::LayoutChanged);
	connect(slotList, &QListWidget::currentRowChanged, this, &SwitcherWorkspaceDock::SelectedSlotChanged);
	connect(slotList, &QListWidget::itemActivated, this, [this](QListWidgetItem *) { OpenSelectedSlotInspector(); });
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
	connect(inspectorModeButton, &QToolButton::clicked, this, [this] { ShowSettingsPanel(SettingsPanelMode::Workspace); });
	connect(inspectorCloseButton, &QToolButton::clicked, this, &SwitcherWorkspaceDock::HideSettingsPanel);
	connect(SwitcherRemoteManager::Instance(), &SwitcherRemoteManager::StateChanged, this,
		&SwitcherWorkspaceDock::RemoteStateUpdated);

	RefreshGrid();
	RefreshSlotList();
	RefreshSceneOptions();
	RefreshSelectedSlotEditor();
	RemoteStateUpdated();
	RefreshSettingsPanelMode();
	UpdateInspectorVisibility(false);
	ApplyTheme();
}

SwitcherWorkspaceDock::~SwitcherWorkspaceDock()
{
	if (gWorkspaceDock == this)
		gWorkspaceDock = nullptr;
}

void SwitcherWorkspaceDock::OpenWorkspaceWindow()
{
	if (!workspacePlacementInitialized)
		ApplyDefaultWindowGeometry();

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
	obs_data_t *data = SaveContentState();
	obs_data_set_bool(data, "visible", isVisible());
	obs_data_set_bool(data, "inspector_visible", inspectorFrame->isVisible());
	obs_data_set_int(data, "inspector_width", inspectorFrame->isVisible() ? inspectorFrame->width() : inspectorWidth);
	if (workspacePlacementInitialized)
		obs_data_set_string(data, "geometry", saveGeometry().toBase64().constData());

	obs_data_t *remoteData = SwitcherRemoteManager::Instance()->SaveState();
	obs_data_set_obj(data, kRemoteStateKey, remoteData);
	obs_data_release(remoteData);

	return data;
}

void SwitcherWorkspaceDock::LoadState(obs_data_t *data)
{
	LoadContentState(data, false);

	workspacePlacementInitialized = false;
	if (data) {
		const char *geometry = obs_data_get_string(data, "geometry");
		if (geometry && strlen(geometry) > 0)
			workspacePlacementInitialized = restoreGeometry(QByteArray::fromBase64(QByteArray(geometry)));

		inspectorWidth = std::max(320, static_cast<int>(obs_data_get_int(data, "inspector_width")));
		if (obs_data_get_bool(data, "inspector_visible"))
			ShowSettingsPanel(SettingsPanelMode::Workspace);
		else
			HideSettingsPanel();

		if (obs_data_get_bool(data, "visible"))
			OpenWorkspaceWindow();
		else
			hide();
	} else {
		HideSettingsPanel();
	}

	obs_data_t *remoteData = data ? obs_data_get_obj(data, kRemoteStateKey) : nullptr;
	SwitcherRemoteManager::Instance()->LoadState(remoteData);
	if (remoteData)
		obs_data_release(remoteData);

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
		HideSettingsPanel();
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
	QWidget::showEvent(event);
	RefreshSceneOptions();
	RefreshSlotList();
	RefreshSelectedSlotEditor();
	ApplyPreviewState(true);
	for (int index = 0; index < VisibleSlotCount(); index++)
		slotWidgets[index]->RefreshActiveState();
}

void SwitcherWorkspaceDock::hideEvent(QHideEvent *event)
{
	ApplyPreviewState(false);
	QWidget::hideEvent(event);
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

void SwitcherWorkspaceDock::OpenSlotAsDock(int slotIndex)
{
	if (slotIndex < 0 || slotIndex >= VisibleSlotCount())
		return;

	if (slotList->currentRow() != slotIndex)
		slotList->setCurrentRow(slotIndex);

	OpenSelectedSlotAsDock();
}

void SwitcherWorkspaceDock::OpenLegacyDockManager()
{
	const auto dialog = new SwitcherSettingsDialog(static_cast<QMainWindow *>(obs_frontend_get_main_window()));
	dialog->setAttribute(Qt::WA_DeleteOnClose, true);
	dialog->show();
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
	const QColor alternate = pal.color(QPalette::AlternateBase);
	const QColor button = pal.color(QPalette::Button);
	const QColor text = pal.color(QPalette::Text);
	const QColor buttonText = pal.color(QPalette::ButtonText);
	const QColor windowText = pal.color(QPalette::WindowText);
	const QColor disabledText = pal.color(QPalette::Disabled, QPalette::Text);
	const QColor mid = pal.color(QPalette::Mid);
	const QColor highlight = pal.color(QPalette::Highlight);

	const QColor rootBackground = Blend(window, base, 0.08);
	const QColor slotBackground = Blend(base, window, 0.18);
	const QColor slotBorder = WithAlpha(mid, 110);
	const QColor mutedText = WithAlpha(disabledText.isValid() ? disabledText : text, 220);
	const QColor slotChipBackground = Blend(slotBackground, window, 0.22);
	const QColor slotChipBorder = WithAlpha(mid, 148);
	const QColor slotChipText = WithAlpha(windowText, 235);
	const QColor hoverFill = WithAlpha(highlight, 28);
	const QColor hoverBorder = WithAlpha(highlight, 128);
	const QColor selectedFill = WithAlpha(highlight, 42);
	const QColor selectedBorder = WithAlpha(highlight, 220);
	const QColor popupBackground = Blend(window, base, 0.28);
	const QColor popupCardBackground = Blend(popupBackground, alternate, 0.08);
	const QColor popupFieldBackground = Blend(base, alternate, 0.18);
	const QColor popupBorder = WithAlpha(mid, 128);
	const QColor popupButtonHover = Blend(button, highlight, 0.16);
	const QColor popupButtonPressed = Blend(button, highlight, 0.24);

	QString styleSheet = QStringLiteral(
		"#switcherWorkspaceRoot {"
		"  background-color: @ROOT@;"
		"}"
		"#switcherWorkspaceSlot {"
		"  background-color: @SLOT_BG@;"
		"  border: 1px solid @SLOT_BORDER@;"
		"  border-radius: 12px;"
		"}"
		"#switcherWorkspaceSlotEmpty {"
		"  color: @MUTED_TEXT@;"
		"  font-size: 15px;"
		"  font-weight: 600;"
		"}"
		"#switcherWorkspaceSlotTitle {"
		"  background-color: @CHIP_BG@;"
		"  color: @CHIP_TEXT@;"
		"  border: 1px solid @CHIP_BORDER@;"
		"  border-radius: 10px;"
		"  padding: 0 10px;"
		"  font-size: 11px;"
		"  font-weight: 600;"
		"}"
		"#switcherWorkspaceSlotTitle[selected=\"true\"] {"
		"  background-color: @SELECT_FILL@;"
		"  border-color: @SELECT_BORDER@;"
		"}"
		"#switcherWorkspaceInspector {"
		"  background-color: @PANEL_BG@;"
		"  border-left: 1px solid @PANEL_BORDER@;"
		"}"
		"#switcherWorkspaceInspectorHeader {"
		"  background-color: @PANEL_BG@;"
		"  border-bottom: 1px solid @PANEL_BORDER@;"
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
		"  font-size: 11px;"
		"}"
		"#switcherWorkspaceInspector QListWidget,"
		"#switcherWorkspaceInspector QComboBox,"
		"#switcherWorkspaceInspector QLineEdit {"
		"  background-color: @FIELD_BG@;"
		"  color: @FIELD_TEXT@;"
		"  border: 1px solid @PANEL_BORDER@;"
		"  border-radius: 8px;"
		"  padding: 8px 10px;"
		"}"
		"#switcherWorkspaceInspector QListWidget {"
		"  outline: none;"
		"  background-color: @CARD_BG@;"
		"}"
		"#switcherWorkspaceInspector QListWidget::item {"
		"  border-radius: 8px;"
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
			"  border-radius: 10px;"
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
			"#switcherWorkspaceInspectorCloseButton {"
			"  padding: 0px;"
			"  min-width: 32px;"
			"  max-width: 32px;"
		"}");

	styleSheet.replace(QStringLiteral("@ROOT@"), CssColor(rootBackground));
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
	styleSheet.replace(QStringLiteral("@CHIP_BG@"), CssColor(slotChipBackground));
	styleSheet.replace(QStringLiteral("@CHIP_TEXT@"), CssColor(slotChipText));
	styleSheet.replace(QStringLiteral("@CHIP_BORDER@"), CssColor(slotChipBorder));

	if (appliedThemeStyleSheet != styleSheet) {
		appliedThemeStyleSheet = styleSheet;
		setStyleSheet(styleSheet);
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
