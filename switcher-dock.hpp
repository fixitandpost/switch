#pragma once

#include <qboxlayout.h>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QDockWidget>
#include <QPointer>
#include <QSlider>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <memory>

#include "obs.h"
#include <obs-frontend-api.h>
#include <QSplitter>

#include "media-control.hpp"
#include "obs.hpp"
#include "qt-display.hpp"
#include "volume-meter.hpp"

#define SHOW_PREVIEW 1
#define SHOW_AUDIO 2
#define SHOW_VOLUME 4
#define SHOW_MUTE 8
#define SHOW_MEDIA 16
#define SHOW_ALL 31

typedef std::function<bool(QObject *, QEvent *)> EventFilterFunc;

class OBSEventFilter : public QObject {
	Q_OBJECT
public:
	OBSEventFilter(EventFilterFunc filter_) : filter(filter_) {}

protected:
	bool eventFilter(QObject *obj, QEvent *event) { return filter(obj, event); }

public:
	EventFilterFunc filter;
};

class LockedCheckBox : public QCheckBox {
	Q_OBJECT

public:
	LockedCheckBox();
	explicit LockedCheckBox(QWidget *parent);
};

class VisibilityCheckBox : public QCheckBox {
	Q_OBJECT

public:
	VisibilityCheckBox();
	explicit VisibilityCheckBox(QWidget *parent);
};

class MuteCheckBox : public QCheckBox {
	Q_OBJECT
public:
	MuteCheckBox();
	explicit MuteCheckBox(QWidget *parent);
};

class SliderIgnoreScroll : public QSlider {
	Q_OBJECT

public:
	SliderIgnoreScroll(QWidget *parent = nullptr);
	SliderIgnoreScroll(Qt::Orientation orientation, QWidget *parent = nullptr);

protected:
	virtual void wheelEvent(QWheelEvent *event) override;
};

class QMainWindow;

struct SwitcherDockRegistrationOptions {
	bool selected = false;
	QString dockId;
	bool preview = false;
	bool volMeter = false;
	bool volControls = false;
	bool mediaControls = false;
	bool switchScene = false;
	bool showActive = false;
	bool properties = false;
	bool filters = false;
	bool textInput = false;
	bool sceneItems = false;
	bool visible = true;
	bool applyPlacement = false;
	Qt::DockWidgetArea dockArea = Qt::LeftDockWidgetArea;
	bool floating = false;
};

class SwitcherDock : public QSplitter {
	Q_OBJECT

private:
	OBSSource source = nullptr;
	std::unique_ptr<OBSEventFilter> eventFilter;
	QAction *action = nullptr;
	float zoom = 1.0f;
	float scrollX = 0.5f;
	float scrollY = 0.5f;
	int scrollingFromX = 0;
	int scrollingFromY = 0;
	int lastMouseX = 0;
	int lastMouseY = 0;
	bool hasLastMousePosition = false;
	bool selected;
	bool workspaceContextMenuEnabled = false;
	QPointer<QDockWidget> wrapperDockWidget = nullptr;
	bool wrapperDockVisible = true;

	OBSQTDisplay *preview = nullptr;
	bool previewConfigured = false;
	bool previewActive = false;
	VolumeMeter *volMeter = nullptr;
	QWidget *volMeterWidget = nullptr;
	obs_volmeter_t *obs_volmeter = nullptr;
	LockedCheckBox *locked = nullptr;
	SliderIgnoreScroll *slider = nullptr;
	MuteCheckBox *mute = nullptr;
	MediaControl *mediaControl = nullptr;
	bool showTimeDecimals = true;
	bool showTimeRemaining = true;
	QWidget *volControl = nullptr;
	bool switch_scene_enabled = false;
	QFrame *activeFrame = nullptr;
	QLabel *activeLabel = nullptr;
	QWidget *sceneItems = nullptr;
	QScrollArea *sceneItemsScrollArea = nullptr;
	QPushButton *propertiesButton = nullptr;
	QPushButton *filtersButton = nullptr;
	QPlainTextEdit *textInput = nullptr;
	QTimer *textInputTimer = nullptr;
	obs_data_t *textInputCustomStyle = nullptr;

	OBSSignal visibleSignal;
	OBSSignal addSignal;
	OBSSignal removeSignal;
	OBSSignal reorderSignal;
	OBSSignal refreshSignal;
	OBSSignal muteSignal;
	OBSSignal volumeSignal;
	OBSSignal activateSignal;
	OBSSignal deactivateSignal;

	static void DrawPreview(void *data, uint32_t cx, uint32_t cy);

	static void OBSVolumeLevel(void *data, const float magnitude[MAX_AUDIO_CHANNELS], const float peak[MAX_AUDIO_CHANNELS],
				   const float inputPeak[MAX_AUDIO_CHANNELS]);
	static void OBSVolume(void *data, calldata_t *calldata);
	static void OBSMute(void *data, calldata_t *calldata);
	static void OBSActiveChanged(void *, calldata_t *);
	static bool AddSceneItem(obs_scene_t *scene, obs_sceneitem_t *item, void *data);
	static bool GetSceneItemCount(obs_scene_t *scene, obs_sceneitem_t *item, void *data);
	bool GetSourceRelativeXY(int mouseX, int mouseY, int &x, int &y);

	bool HandleMouseClickEvent(QMouseEvent *event);
	bool HandleContextMenuEvent(QContextMenuEvent *event);
	bool HandleMouseMoveEvent(QMouseEvent *event);
	bool HandleMouseBoundaryEvent(QEvent *event);
	bool HandleMouseWheelEvent(QWheelEvent *event);
	bool HandleFocusEvent(QFocusEvent *event);
	bool HandleKeyEvent(QKeyEvent *event);

	OBSEventFilter *BuildEventFilter();
	void ApplyDockWidgetFeatures(QDockWidget *dockWidget);
	bool ShouldActivatePreview() const;
	void ActivatePreview();
	void DeactivatePreview();
	void UpdatePreviewLifecycle();
	void UpdateVolumeSignals();
	void UpdateActiveSignals();

private slots:
	void LockVolumeControl(bool lock);
	void MuteVolumeControl(bool mute);
	void SliderChanged(int vol);
	void SetOutputVolume(double volume);
	void SetMute(bool muted);
	void ActiveChanged();
	void VisibilityChanged(int id);
	void RefreshItems();
	void SetActive(int active);
	void DockWidgetVisibilityChanged(bool visible);

signals:
	void ContextMenuRequested(const QPoint &globalPos);

public:
	SwitcherDock(QString name, bool selected, QWidget *parent = nullptr);
	~SwitcherDock();
	static QString CreateDockId();
	void SetDockId(const QString &id);
	QString DockId() const { return objectName(); }
	QDockWidget *ParentDockWidget() const;
	QMainWindow *OwningMainWindow() const;
	void BindDockWidgetLifecycle(QDockWidget *dockWidget);
	void SetWorkspaceContextMenuEnabled(bool enabled) { workspaceContextMenuEnabled = enabled; }

protected:
	void changeEvent(QEvent *event) override;
	bool event(QEvent *event) override;
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;

public:
	void SetSource(const OBSSource source_);
	OBSSource GetSource();

	bool GetSelected() { return selected; }

	void setAction(QAction *action);

	void EnablePreview();
	void DisablePreview();
	bool PreviewEnabled();

	void EnableVolMeter();
	void DisableVolMeter();
	bool VolMeterEnabled();

	void EnableVolControls();
	void UpdateVolControls();
	void DisableVolControls();
	bool VolControlsEnabled();

	void EnableMediaControls();
	void DisableMediaControls();
	bool MediaControlsEnabled();
	bool GetShowMs() { return mediaControl ? mediaControl->GetShowMs() : showTimeDecimals; }
	void SetShowMs(bool b) { showTimeDecimals = b; }
	bool GetShowTimeRemaining() { return mediaControl ? mediaControl->GetShowTimeRemaining() : showTimeRemaining; }
	void SetShowTimeRemaining(bool b) { showTimeRemaining = b; }

	void EnableSwitchScene();
	void DisableSwitchScene();
	bool SwitchSceneEnabled();

	void EnableShowActive();
	void DisableShowActive();
	bool ShowActiveEnabled();

	void EnableSceneItems();
	void DisableSceneItems();
	bool SceneItemsEnabled();

	void EnableProperties();
	void DisableProperties();
	bool PropertiesEnabled();

	void EnableFilters();
	void DisableFilters();
	bool FiltersEnabled();

	void EnableTextInput();
	void DisableTextInput();
	bool TextInputEnabled();

	obs_data_t *GetCustomTextInputStyle();
	void SetCustomTextInputStyle(obs_data_t *);
	void ApplyCustomTextInputStyle();

	float GetZoom() const { return zoom; }
	void SetZoom(float zoom);
	float GetScrollX() { return scrollX; }
	void SetScrollX(float scroll);
	float GetScrollY() { return scrollY; }
	void SetScrollY(float scroll);

	QByteArray saveSplitState();
	bool restoreSplitState(const QByteArray &splitState);
};

inline std::list<SwitcherDock *> switcher_docks;

SwitcherDock *CreateRegisteredSwitcherDock(const QString &title, const OBSSource &source, QMainWindow *mainWindow,
					   const SwitcherDockRegistrationOptions &options);
