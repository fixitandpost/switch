#pragma once

#include <QFrame>
#include <QWidget>
#include <vector>

#include <obs-frontend-api.h>

#include "switcher-dock.hpp"

class QComboBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QMenu;
class QPushButton;
class QCheckBox;
class QContextMenuEvent;
class QEvent;
class QHideEvent;
class QMainWindow;
class QMouseEvent;
class QScrollArea;
class QShowEvent;
class QSplitter;
class QStackedWidget;
class QToolButton;
class QResizeEvent;
class QObject;

class SwitcherWorkspacePreview : public QWidget {
	Q_OBJECT

public:
	explicit SwitcherWorkspacePreview(QWidget *parent = nullptr);
	~SwitcherWorkspacePreview() override;

	void SetSource(const OBSSource &source);
	OBSSource GetSource() const;
	void SetPreviewActive(bool active);
	void RefreshActiveState();

signals:
	void ContextMenuRequested(const QPoint &globalPos);

protected:
	void changeEvent(QEvent *event) override;
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	static void DrawPreview(void *data, uint32_t cx, uint32_t cy);
	bool HandleMouseButtonEvent(QMouseEvent *event);
	bool HandleContextMenuEvent(QContextMenuEvent *event);
	bool ShouldActivatePreview() const;
	void ActivatePreview();
	void DeactivatePreview();
	void UpdatePreviewLifecycle();

	OBSQTDisplay *display = nullptr;
	OBSSource source = nullptr;
	bool previewConfigured = false;
	bool previewActive = false;
};

class SwitcherWorkspaceSlot : public QFrame {
	Q_OBJECT

public:
	explicit SwitcherWorkspaceSlot(int slotIndex, QWidget *parent = nullptr);

	void SetSource(const OBSSource &source);
	OBSSource GetSource();

	void SetCustomTitle(const QString &title);
	QString GetCustomTitle() const;
	QString GetEffectiveTitle() const;

	void SetPreviewActive(bool active);
	void SetSelected(bool active);
	void RefreshActiveState();
	void ClearIfMatches(obs_source_t *source);
	void ShowContextMenu(const QPoint &globalPos);

signals:
	void ConfigureRequested(int slotIndex);
	void AdvancedRequested(int slotIndex);
	void DetachRequested(int slotIndex);

protected:
	void contextMenuEvent(QContextMenuEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;

private:
	void RefreshPresentation();
	void RefreshTitleLabel();
	void RefreshSelectionState();
	QString DefaultTitle() const;

	int slotIndex;
	QString customTitle;
	SwitcherWorkspacePreview *previewWidget;
	QStackedWidget *contentStack;
	QLabel *emptyLabel;
	QLabel *titleLabel;
	QMenu *contextMenu;
	bool previewActive = false;
	bool selected = false;
};

class SwitcherWorkspaceDock : public QWidget {
	Q_OBJECT

public:
	explicit SwitcherWorkspaceDock(QMainWindow *parent = nullptr);
	~SwitcherWorkspaceDock() override;

	void OpenWorkspaceWindow();
	obs_data_t *SaveState();
	void LoadState(obs_data_t *data);
	void ApplySerializedContentState(const char *json);
	void HandleSourceRemoved(obs_source_t *source);
	void HandleFrontendEvent(enum obs_frontend_event event);
	void ClearSceneCollectionState();
	int VisibleSlotCount() const;
	OBSSource SlotSource(int index) const;
	QString SlotTitle(int index) const;

protected:
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void changeEvent(QEvent *event) override;

private slots:
	void LayoutChanged();
	void SelectedSlotChanged();
	void SelectedSceneChanged();
	void SelectedTitleChanged(const QString &title);
	void ClearSelectedSlot();
	void OpenSelectedSlotAsDock();
	void OpenSlotAsDock(int slotIndex);
	void OpenSelectedSlotInspector();
	void OpenLegacyDockManager();
	void RemoteStateUpdated();
	void RemoteEnabledChanged(Qt::CheckState state);
	void RemoteAutoStartChanged(Qt::CheckState state);
	void RemoteResolutionChanged(int index);
	void RemoteFpsChanged(int index);
	void CopyRemoteUrl();
	void RegenerateRemoteToken();
	void RestartRemote();

private:
	enum class SettingsPanelMode { Workspace, SlotInspector };

	static bool AddSceneOption(void *data, obs_source_t *source);

	obs_data_t *SaveContentState() const;
	void LoadContentState(obs_data_t *data, bool emitSignal);
	void RegisterUndoRedoAction(const char *name, obs_data_t *before, obs_data_t *after, bool repeatable = false);
	void ApplyPreviewState(bool enabled);
	void ApplyTheme();
	SwitcherWorkspaceSlot *CurrentSlot() const;
	QString CurrentSlotLabel() const;
	int SelectedSlotIndex() const;
	void RefreshGrid();
	void RefreshSceneOptions();
	void RefreshSettingsPanelMode();
	void RefreshSelectedSlotEditor();
	void RefreshSlotList();
	void RefreshRemoteControls();
	void RefreshSelectionHighlights();
	void UpdateInspectorVisibility(bool visible);
	void ApplyDefaultWindowGeometry();
	void OpenSlotSettings(int slotIndex);
	void OpenAdvancedSettings(int slotIndex);
	void ShowSettingsPanel(SettingsPanelMode mode);
	void HideSettingsPanel();

	QSplitter *contentSplitter;
	QScrollArea *scrollArea;
	QWidget *gridContainer;
	QGridLayout *gridLayout;
	QFrame *inspectorFrame;
	QToolButton *inspectorModeButton;
	QLabel *inspectorTitleLabel;
	QToolButton *inspectorCloseButton;
	QStackedWidget *inspectorStack;
	QWidget *workspacePage;
	QWidget *slotPage;
	QWidget *workspaceSettingsSection;
	QWidget *slotEditorSection;
	QWidget *remoteSettingsSection;
	QListWidget *slotList;
	QComboBox *layoutCombo;
	QComboBox *sceneCombo;
	QLineEdit *titleEdit;
	QPushButton *clearSlotButton;
	QPushButton *detachSlotButton;
	QPushButton *legacyManagerButton;
	QCheckBox *remoteEnabledCheckBox;
	QCheckBox *remoteAutoStartCheckBox;
	QComboBox *remoteResolutionCombo;
	QComboBox *remoteFpsCombo;
	QLineEdit *remoteUrlEdit;
	QLabel *remoteStatusLabel;
	QPushButton *remoteCopyButton;
	QPushButton *remoteRegenerateButton;
	QPushButton *remoteRestartButton;
	std::vector<SwitcherWorkspaceSlot *> slotWidgets;
	bool previewsEnabled = false;
	bool loadingState = false;
	bool applyingUndoRedo = false;
	bool applyingTheme = false;
	bool workspacePlacementInitialized = false;
	int inspectorWidth = 380;
	SettingsPanelMode settingsPanelMode = SettingsPanelMode::Workspace;
	QString appliedThemeStyleSheet;

signals:
	void WorkspaceStateChanged();
};
