#pragma once

#include <QDockWidget>
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
class QPushButton;
class QCheckBox;
class QDialog;
class QScrollArea;
class QStackedWidget;
class QToolButton;
class QResizeEvent;

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

signals:
	void Clicked(int slotIndex);

protected:
	void resizeEvent(QResizeEvent *event) override;

private:
	void RefreshPresentation();
	void RefreshSelectionState();
	QString DefaultTitle() const;

	int slotIndex;
	QString customTitle;
	SwitcherDock *dock;
	QLabel *titleLabel;
	QStackedWidget *contentStack;
	QLabel *emptyLabel;
	QPushButton *selectionButton;
	bool previewActive = false;
	bool selected = false;
};

class SwitcherWorkspaceDock : public QDockWidget {
	Q_OBJECT

public:
	explicit SwitcherWorkspaceDock(QMainWindow *parent = nullptr);

	obs_data_t *SaveState();
	void LoadState(obs_data_t *data);
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

private slots:
	void LayoutChanged();
	void SelectedSlotChanged();
	void SelectSlot(int slotIndex);
	void SelectedSceneChanged();
	void SelectedTitleChanged(const QString &title);
	void ClearSelectedSlot();
	void OpenSelectedSlotAsDock();
	void OpenLegacyDockManager();
	void ToggleSettingsPopup();
	void RemoteStateUpdated();
	void RemoteEnabledChanged(Qt::CheckState state);
	void RemoteAutoStartChanged(Qt::CheckState state);
	void RemoteResolutionChanged(int index);
	void RemoteFpsChanged(int index);
	void CopyRemoteUrl();
	void RegenerateRemoteToken();
	void RestartRemote();

private:
	static bool AddSceneOption(void *data, obs_source_t *source);

	void ApplyPreviewState(bool enabled);
	SwitcherWorkspaceSlot *CurrentSlot() const;
	int SelectedSlotIndex() const;
	void RefreshGrid();
	void RefreshSceneOptions();
	void RefreshSelectedSlotEditor();
	void RefreshSlotList();
	void RefreshRemoteControls();
	void RefreshSelectionHighlights();
	void PositionChrome();
	void PositionSettingsPopup();

	QScrollArea *scrollArea;
	QWidget *gridContainer;
	QGridLayout *gridLayout;
	QToolButton *settingsButton;
	QDialog *settingsPopup;
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

signals:
	void WorkspaceStateChanged();
};
