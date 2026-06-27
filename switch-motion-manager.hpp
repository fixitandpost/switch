#pragma once

#include <QHash>
#include <QObject>
#include <QSize>
#include <QString>
#include <QVector>

#include <cstdint>
#include <mutex>

#include <obs-frontend-api.h>
#include <obs-module.h>

class QTimer;

struct SwitchMotionDetection {
	float x1 = 0.0f;
	float y1 = 0.0f;
	float x2 = 0.0f;
	float y2 = 0.0f;
	float confidence = 0.0f;
	int classId = -1;
};

struct SwitchMotionProfile {
	QString id;
	QString name;
	bool enabled = false;
	float confidenceThreshold = 0.45f;
	int targetClassId = 0;
	float maxZoom = 2.1f;
	float framingMargin = 0.18f;
	float deadZone = 0.045f;
	float smoothing = 0.52f;
	int holdMs = 1200;
	QString backend = QStringLiteral("auto");
	QString modelPath;
	QString subjectMode = QStringLiteral("auto");
	QString framingMode = QStringLiteral("upper_body");
	float trackerHighThreshold = 0.55f;
	float trackerLowThreshold = 0.20f;
	float newTrackThreshold = 0.65f;
	int trackBufferFrames = 45;
	int lockedTrackGraceFrames = 90;
	int autoSwitchMs = 500;
	float panResponsiveness = 0.64f;
	float tiltResponsiveness = 0.62f;
	float zoomResponsiveness = 0.54f;
	float maxPanSpeed = 0.85f;
	float maxTiltSpeed = 0.75f;
	float maxZoomSpeed = 1.25f;
	bool debugOverlay = false;
	int lockedTrackId = -1;
};

struct SwitchMotionBinding {
	QString sourceUuid;
	QString sourceName;
	QString profileId;
	bool enabled = true;
};

struct SwitchMotionShot {
	QString id;
	QString name;
	bool enabled = true;
	QString sceneUuid;
	QString sceneName;
	int64_t sceneItemId = -1;
	QString sourceUuid;
	QString sourceName;
	QString profileId;
	QString shotMode = QStringLiteral("keyframe_loop");
	QString playbackMode = QStringLiteral("free_run");
	qint64 phaseAnchorMs = 0;
	qint64 pausedPhaseMs = 0;
	qint64 lastProgramMs = 0;
	float startPanX = 0.0f;
	float startPanY = 0.0f;
	float startZoom = 1.0f;
	float endPanX = 0.0f;
	float endPanY = 0.0f;
	float endZoom = 1.18f;
	int durationMs = 18000;
	QString easing = QStringLiteral("smoothstep");
	QString loopMode = QStringLiteral("ping_pong");
	float maxZoom = 2.2f;
};

struct SwitchMotionShotPreset {
	QString id;
	QString name;
	SwitchMotionShot shot;
};

struct SwitchMotionCameraState {
	float camX = 0.0f;
	float camY = 0.0f;
	float zoom = 1.0f;
	float panVelocity = 0.0f;
	float tiltVelocity = 0.0f;
	float zoomVelocity = 0.0f;
	float targetCamX = 0.0f;
	float targetCamY = 0.0f;
	float targetZoom = 1.0f;
	bool targetActive = false;
	float targetConfidence = 0.0f;
	int targetTrackId = -1;
	qint64 lastTargetMs = 0;
	qint64 lastUpdateMs = 0;
};

struct SwitchMotionTrack {
	int trackId = -1;
	SwitchMotionDetection bbox;
	SwitchMotionDetection smoothedBox;
	float velocityX = 0.0f;
	float velocityY = 0.0f;
	float confidence = 0.0f;
	int ageFrames = 0;
	int missedFrames = 0;
	QString state = QStringLiteral("new");
	qint64 lastSeenMs = 0;
	QString sourceUuid;
};

struct SwitchMotionRuntimeState {
	QString status = QStringLiteral("idle");
	QString backend;
	QString modelPath;
	QString message;
	QString providerStatus;
	bool modelAvailable = false;
	bool targetActive = false;
	float targetConfidence = 0.0f;
	int targetTrackId = -1;
	QString targetState;
	QString sourceUuid;
	float camX = 0.0f;
	float camY = 0.0f;
	float zoom = 1.0f;
	double preprocessingMs = 0.0;
	double inferenceMs = 0.0;
	double trackingMs = 0.0;
	uint64_t droppedFrames = 0;
	int activeTrackCount = 0;
	QVector<SwitchMotionTrack> tracks;
	QString activeShotId;
	QString activeShotName;
	QString activeSceneName;
	QString activeShotMode;
	QString activeShotPlaybackMode;
	qint64 activeShotPhaseMs = 0;
	float activeShotCamX = 0.0f;
	float activeShotCamY = 0.0f;
	float activeShotZoom = 1.0f;
};

QString SwitchCreateMotionId(const QString &prefix);
SwitchMotionProfile SwitchDefaultMotionProfile(const QString &modelPath = QString());
SwitchMotionShot SwitchDefaultMotionShot();
QVector<SwitchMotionShotPreset> SwitchMotionShotPresets();
obs_data_t *SwitchMotionProfileToObsData(const SwitchMotionProfile &profile);
SwitchMotionProfile SwitchMotionProfileFromObsData(obs_data_t *data, const QString &fallbackModelPath = QString());
obs_data_t *SwitchMotionBindingToObsData(const SwitchMotionBinding &binding);
SwitchMotionBinding SwitchMotionBindingFromObsData(obs_data_t *data);
obs_data_t *SwitchMotionShotToObsData(const SwitchMotionShot &shot);
SwitchMotionShot SwitchMotionShotFromObsData(obs_data_t *data);
obs_data_array_t *SwitchMotionShotsToObsArray(const QVector<SwitchMotionShot> &shots);
obs_data_t *SwitchMotionShotPresetToObsData(const SwitchMotionShotPreset &preset);
obs_data_array_t *SwitchMotionShotPresetsToObsArray();
obs_data_t *SwitchMotionTrackToObsData(const SwitchMotionTrack &track);
obs_data_array_t *SwitchMotionTracksToObsArray(const QVector<SwitchMotionTrack> &tracks);
obs_data_t *SwitchMotionRuntimeStateToObsData(const SwitchMotionRuntimeState &state);
QVector<SwitchMotionDetection> SwitchParseYolo26Detections(const float *values, size_t valueCount,
							    const QVector<int64_t> &shape,
							    float confidenceThreshold,
							    int targetClassId);
bool SwitchSelectPrimaryDetection(const QVector<SwitchMotionDetection> &detections, SwitchMotionDetection *selected);
bool SwitchSelectPrimaryDetection(const QVector<SwitchMotionDetection> &detections,
				  const SwitchMotionCameraState &previous,
				  const QSize &frameSize,
				  SwitchMotionDetection *selected);
SwitchMotionCameraState SwitchUpdateMotionCameraState(const SwitchMotionProfile &profile,
						      const SwitchMotionDetection *detection,
						      const QSize &frameSize, qint64 nowMs,
						      SwitchMotionCameraState previous);
SwitchMotionDetection SwitchMotionFramingDetection(const SwitchMotionDetection &detection,
						   const QString &framingMode,
						   const QSize &frameSize);
SwitchMotionDetection SwitchMotionGroupDetection(const QVector<SwitchMotionTrack> &tracks);
qint64 SwitchMotionShotPhaseMs(const SwitchMotionShot &shot, qint64 nowMs);
SwitchMotionCameraState SwitchEvaluateMotionShotLoop(const SwitchMotionShot &shot, const SwitchMotionProfile &profile,
						     qint64 phaseMs);
SwitchMotionCameraState SwitchComposeMotionCameraState(const SwitchMotionProfile &profile,
						       const SwitchMotionCameraState &aiState,
						       const SwitchMotionShot &shot,
						       qint64 nowMs);
bool SwitchMotionSelectTargetTrack(const QVector<SwitchMotionTrack> &tracks,
				   const SwitchMotionProfile &profile,
				   const SwitchMotionCameraState &previousCamera,
				   const QSize &frameSize,
				   qint64 nowMs,
				   int *candidateTrackId,
				   qint64 *candidateSinceMs,
				   SwitchMotionTrack *selected);
void SwitchMotionPublishRuntimeState(const SwitchMotionRuntimeState &state);
void SwitchMotionPublishRuntimeError(const QString &message);
void SwitchMotionPublishFilterProfileUpdate(const SwitchMotionProfile &profile, const QString &sourceUuid);

class SwitchMotionTracker {
public:
	void Reset();
	QVector<SwitchMotionTrack> Update(const QVector<SwitchMotionDetection> &detections,
					  const SwitchMotionProfile &profile,
					  const QSize &frameSize,
					  qint64 nowMs,
					  const QString &sourceUuid);
	QVector<SwitchMotionTrack> Tracks() const;
	int CurrentTargetTrackId() const { return currentTargetTrackId; }
	void SetCurrentTargetTrackId(int trackId) { currentTargetTrackId = trackId; }
	int CycleTarget(int direction) const;

private:
	QVector<SwitchMotionTrack> tracks;
	int nextTrackId = 1;
	int currentTargetTrackId = -1;
};

class SwitchMotionManager : public QObject {
	Q_OBJECT

public:
	explicit SwitchMotionManager(QObject *parent = nullptr);
	~SwitchMotionManager() override;

	obs_data_t *SaveState() const;
	void LoadState(obs_data_t *data);
	void Reset(bool detachFilters = true);
	obs_data_t *BuildStateData() const;

	QVector<SwitchMotionProfile> Profiles() const;
	QVector<SwitchMotionBinding> Bindings() const;
	QVector<SwitchMotionShot> Shots() const;
	SwitchMotionProfile ProfileById(const QString &profileId) const;
	SwitchMotionShot ShotById(const QString &shotId) const;
	SwitchMotionRuntimeState RuntimeState() const;
	QString DefaultProfileId() const;
	QString DefaultModelPath() const;
	QString ActiveShotId() const;

	bool UpsertProfile(const SwitchMotionProfile &profile, QString *effectiveId = nullptr);
	bool DeleteProfile(const QString &profileId);
	bool SetProfileEnabled(const QString &profileId, bool enabled);
	bool BindSource(const QString &sourceUuid, const QString &sourceName, const QString &profileId, QString *message = nullptr);
	bool UnbindSource(const QString &sourceUuid, QString *message = nullptr);
	bool UpsertShot(const SwitchMotionShot &shot, QString *effectiveId = nullptr);
	bool DeleteShot(const QString &shotId);
	bool SetShotEnabled(const QString &shotId, bool enabled);
	bool BindSceneItem(const SwitchMotionShot &shot, QString *effectiveId = nullptr, QString *message = nullptr);
	bool SetShotPlayback(const QString &shotId, const QString &playbackMode);
	void ApplyBindings();
	void DetachFilters();
	void SetBindingsQuiesced(bool quiesced);
	void HandleSourceRemoved(obs_source_t *source);
	void HandleFrontendEvent(enum obs_frontend_event event);
	void SetRuntimeState(const SwitchMotionRuntimeState &state);
	void RaiseRuntimeError(const QString &message);
	void SyncProfileFromFilter(const SwitchMotionProfile &profile, const QString &sourceUuid);

signals:
	void StateChanged();
	void ProfileChanged(const QString &profileId);
	void TracksChanged();
	void TargetChanged(bool active, float confidence, int trackId, const QString &sourceUuid);
	void TargetLost(int trackId, const QString &sourceUuid);
	void TargetReacquired(int trackId, const QString &sourceUuid);
	void RuntimeStatsChanged();
	void RuntimeError(const QString &message);
	void ShotChanged(const QString &shotId);
	void ActiveShotChanged(const QString &shotId);
	void ShotRuntimeError(const QString &message);

private:
	int FindProfileIndex(const QString &profileId) const;
	int FindBindingIndex(const QString &sourceUuid) const;
	int FindShotIndex(const QString &shotId) const;
	bool ApplyBinding(const SwitchMotionBinding &binding, const SwitchMotionProfile &profile, QString *message) const;
	void ApplyShotSamplers();
	void RefreshSourceFilterForUuid(const QString &sourceUuid);
	void StartShotTimer();
	void StopShotTimer();
	void TickMotionShots();
	void RestoreAllShotTransforms();
	void RestoreShotTransform(const QString &shotKey);
	bool ApplyShotTransform(const SwitchMotionShot &shot, const SwitchMotionCameraState &state, QString *message);
	SwitchMotionShot *ShotForSceneUuid(const QString &sceneUuid);
	QString CurrentProgramSceneUuid(QString *sceneName = nullptr) const;
	QString CurrentPreviewSceneUuid() const;
	QString ResolveDefaultModelPath() const;
	void EnsureDefaultProfile();
	void EmitTargetIfChanged(const SwitchMotionRuntimeState &previous, const SwitchMotionRuntimeState &next);

	QVector<SwitchMotionProfile> profiles;
	QVector<SwitchMotionBinding> bindings;
	QVector<SwitchMotionShot> shots;
	SwitchMotionRuntimeState runtimeState;
	mutable std::mutex runtimeStateMutex;
	QTimer *shotTimer = nullptr;
	QHash<QString, obs_transform_info> shotBaseTransforms;
	QString activeShotId;
	QString lastObservedProgramSceneUuid;
	qint64 lastShotRuntimeEmitMs = 0;
	qint64 shotSceneSettlingUntilMs = 0;
	bool bindingsQuiesced = false;
	uint64_t bindingGeneration = 0;
};
