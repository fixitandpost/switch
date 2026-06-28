#pragma once

#include <QPointer>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtWidgets/qwidget.h>

#include <atomic>

#include <obs-frontend-api.h>
#include "obs.hpp"

class OBSQTDisplay;
class SwitchCanvasManager;

struct SwitchCanvasDescriptor {
	QString id;
	QString name;
	QSize size;
	QString aspectPreset;
	QString activeSceneUuid;
	QString activeSceneName;
	bool linkedSceneSync = false;
	bool followMainStreaming = true;
	bool followMainRecording = true;
	bool followMainReplay = true;
	bool followMainVirtualCamera = true;
};

struct SwitchCanvasSceneDescriptor {
	QString uuid;
	QString name;
};

struct SwitchCanvasSourceDescriptor {
	int id = 0;
	QString name;
	bool visible = true;
	bool locked = false;
	int depth = 0;
	bool isGroup = false;
};

struct SwitchCanvasTransitionDescriptor {
	QString name;
	bool configurable = false;
};

struct SwitchCanvasLink {
	QString mainSceneUuid;
	QString mainSceneName;
	QString targetSceneUuid;
	QString targetSceneName;
};

struct SwitchCanvasOutputSettings {
	bool followMainStreaming = true;
	bool followMainRecording = true;
	bool followMainReplay = true;
	bool followMainVirtualCamera = true;
	QString streamEncoderId;
	QString recordEncoderId;
	int streamingVideoBitrateKbps = 6000;
	int recordingVideoBitrateKbps = 12000;
	int audioBitrateKbps = 160;
	bool streamDelayEnabled = false;
	int streamDelayMs = 0;
	bool streamDelayPreserve = false;
	QString recordingPath;
	QString recordingFilenamePattern;
	bool recordingSplitEnabled = false;
	int recordingSplitMinutes = 15;
	QString replayPath;
	int replayDurationSeconds = 30;
	bool replayAlwaysOn = false;
	uint32_t audioTrackMask = 0x1;
};

class SwitchCanvasPreview : public QWidget {
public:
	explicit SwitchCanvasPreview(SwitchCanvasManager *manager, QWidget *parent = nullptr);
	~SwitchCanvasPreview() override;

	void SetCanvasId(const QString &canvasId);
	QString CanvasId() const { return canvasId; }
	void SetRenderingEnabled(bool enabled);
	void Refresh();

protected:
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;

private:
	static void DrawCanvas(void *data, uint32_t cx, uint32_t cy);
	void Activate();
	void Deactivate();
	void DestroyDisplayWidget();

	SwitchCanvasManager *manager = nullptr;
	OBSQTDisplay *display = nullptr;
	QString canvasId;
	bool drawCallbackInstalled = false;
	bool renderingEnabled = true;
};

class SwitchCanvasManager : public QObject {
	Q_OBJECT

public:
	explicit SwitchCanvasManager(QObject *parent = nullptr);
	~SwitchCanvasManager() override;

	obs_data_t *SaveState() const;
	void LoadState(obs_data_t *data);
	void Reset();
	void ReleaseRuntimeReferencesForShutdown();

	QVector<SwitchCanvasDescriptor> Canvases() const;
	SwitchCanvasDescriptor CanvasDescriptor(const QString &canvasId) const;
	QVector<SwitchCanvasSceneDescriptor> ScenesForCanvas(const QString &canvasId) const;
	QVector<SwitchCanvasTransitionDescriptor> Transitions() const;
	QVector<SwitchCanvasLink> Links() const;
	SwitchCanvasOutputSettings OutputSettings(const QString &canvasId) const;
	QSize CanvasSize(const QString &canvasId) const;
	obs_source_t *CanvasSceneSource(const QString &canvasId, const QString &sceneUuidOrName) const;
	QString VerticalCanvasId() const;
	QString MainCanvasId() const;
	QString DefaultTransitionName() const;
	int DefaultTransitionDuration() const;

	bool EnsureVerticalCanvas();
	bool SetCanvasName(const QString &canvasId, const QString &name);
	bool SetVerticalPreset(const QSize &size, const QString &preset);
	bool SetCanvasLinkedSync(const QString &canvasId, bool enabled);
	bool SetOutputSettings(const QString &canvasId, const SwitchCanvasOutputSettings &settings);
	bool SetCanvasActiveScene(const QString &canvasId, const QString &sceneUuidOrName);
	bool CreateVerticalScene(const QString &baseName, QString *createdName = nullptr);
	bool DuplicateVerticalScene(const QString &sceneUuidOrName, const QString &baseName, QString *createdName = nullptr);
	bool RenameVerticalScene(const QString &sceneUuidOrName, const QString &name);
	bool CanRemoveVerticalScene(const QString &sceneUuidOrName, QString *reason = nullptr) const;
	bool RemoveVerticalScene(const QString &sceneName);
	bool SetLinkedScene(const QString &mainSceneUuid, const QString &mainSceneName, const QString &targetSceneUuid,
			    const QString &targetSceneName);
	bool ClearLinkedScene(const QString &mainSceneUuid);
	bool OpenPreviewWindow(const QString &canvasId);
	bool OpenProjector(const QString &canvasId);
	bool SetDefaultTransition(const QString &transitionName);
	bool SetDefaultTransitionDuration(int durationMs);
	QString SceneTransitionName(const QString &sceneUuidOrName) const;
	int SceneTransitionDuration(const QString &sceneUuidOrName) const;
	bool SetSceneTransition(const QString &sceneUuidOrName, const QString &transitionName, int durationMs);
	obs_data_t *BuildStateData() const;

	void SyncLinkedSceneFromProgram();
	void HandleFrontendEvent(enum obs_frontend_event event);
	void HandleSourceRemoved(obs_source_t *source);

	obs_canvas_t *CanvasById(const QString &canvasId) const;
	bool CanRenderCanvas() const { return !suppressCanvasRender; }
	bool TryBeginPreviewRender();
	void EndPreviewRender();

signals:
	void StateChanged();

private:
	void EnsureDefaultState();
	void EnsureTransitions();
	void EnsureVerticalSceneList();
	void RemoveRuntimeCanvases();
	void ReleaseTransitions();
	obs_canvas_t *AcquireCanvasByName(const QString &name) const;
	obs_source_t *ResolveFrontendScene(const QString &sceneUuidOrName) const;
	obs_source_t *ResolveCanvasScene(obs_canvas_t *canvas, const QString &sceneUuidOrName) const;
	obs_source_t *GetTransition(const QString &transitionName) const;
	bool ActivateTransition(const QString &transitionName);
	void UpdateVerticalActiveSceneFromRuntime() const;
	void RebuildVerticalSceneCache();
	QString NextVerticalSceneName(const QString &baseName) const;
	int FindCanvasIndex(const QString &canvasId) const;
	void SuspendCanvasRendering();
	void ScheduleCanvasRenderingResume(int delayMs = 75);
	void UpdateCachedOutputState();

	QVector<SwitchCanvasDescriptor> canvases;
	QVector<SwitchCanvasLink> links;
	QStringList verticalSceneNames;
	SwitchCanvasOutputSettings verticalOutputSettings;
	obs_canvas_t *verticalCanvas = nullptr;
	QVector<OBSSource> transitions;
	obs_weak_source_t *transitionSource = nullptr;
	QString defaultTransitionName;
	QString activeTransitionName;
	int defaultTransitionDurationMs = 300;
	QVector<QPointer<QWidget>> previewWindows;
	bool ensuringVerticalCanvas = false;
	bool ensuringVerticalSceneList = false;
	bool suppressCanvasRender = false;
	bool frontendActive = false;
	int renderResumeGeneration = 0;
	bool cachedStreamingActive = false;
	bool cachedRecordingActive = false;
	bool cachedRecordingPaused = false;
	bool cachedReplayActive = false;
	bool cachedVirtualCameraActive = false;
	std::atomic_flag previewRenderActive = ATOMIC_FLAG_INIT;
};
