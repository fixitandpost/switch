#include "switch-motion-manager.hpp"

#include <QDateTime>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QtGlobal>

#include <util/platform.h>

#include <algorithm>
#include <cstring>
#include <cmath>
#include <limits>
#include <mutex>
#include <utility>

namespace {
constexpr auto kMotionFilterId = "switch_ai_tracker";
constexpr auto kMotionFilterName = "Switch Motion";
constexpr auto kMotionStateVersion = 3;
QPointer<SwitchMotionManager> gRuntimeStateManager;
std::mutex gRuntimeStateManagerMutex;

float ClampFloat(float value, float minimum, float maximum)
{
	return std::max(minimum, std::min(maximum, value));
}

float MaxCameraOffsetForZoom(float zoom)
{
	zoom = std::max(1.0f, zoom);
	return (zoom - 1.0f) / (2.0f * zoom);
}

float ClampCameraOffset(float offset, float zoom)
{
	const float maximum = MaxCameraOffsetForZoom(zoom);
	return ClampFloat(offset, -maximum, maximum);
}

float DetectionWidth(const SwitchMotionDetection &detection)
{
	return std::max(0.0f, detection.x2 - detection.x1);
}

float DetectionHeight(const SwitchMotionDetection &detection)
{
	return std::max(0.0f, detection.y2 - detection.y1);
}

float DetectionCenterX(const SwitchMotionDetection &detection)
{
	return (detection.x1 + detection.x2) * 0.5f;
}

float DetectionCenterY(const SwitchMotionDetection &detection)
{
	return (detection.y1 + detection.y2) * 0.5f;
}

float DetectionArea(const SwitchMotionDetection &detection)
{
	return DetectionWidth(detection) * DetectionHeight(detection);
}

float DetectionIou(const SwitchMotionDetection &left, const SwitchMotionDetection &right)
{
	const float x1 = std::max(left.x1, right.x1);
	const float y1 = std::max(left.y1, right.y1);
	const float x2 = std::min(left.x2, right.x2);
	const float y2 = std::min(left.y2, right.y2);
	const float intersection = std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
	if (intersection <= 0.0f)
		return 0.0f;
	const float unionArea = DetectionArea(left) + DetectionArea(right) - intersection;
	return unionArea > 0.0f ? intersection / unionArea : 0.0f;
}

float DetectionCenterDistance(const SwitchMotionDetection &left, const SwitchMotionDetection &right, const QSize &frameSize)
{
	const float frameWidth = static_cast<float>(std::max(1, frameSize.width()));
	const float frameHeight = static_cast<float>(std::max(1, frameSize.height()));
	const float dx = (DetectionCenterX(left) - DetectionCenterX(right)) / frameWidth;
	const float dy = (DetectionCenterY(left) - DetectionCenterY(right)) / frameHeight;
	return std::sqrt(dx * dx + dy * dy);
}

bool IsVisibleTrack(const SwitchMotionTrack &track)
{
	return track.state == QStringLiteral("active") || track.state == QStringLiteral("new") ||
	       track.state == QStringLiteral("recovered");
}

QString NormalizedSubjectMode(QString mode)
{
	mode = mode.trimmed().toLower();
	if (mode == QStringLiteral("locked") || mode == QStringLiteral("hold") || mode == QStringLiteral("off"))
		return mode;
	return QStringLiteral("auto");
}

QString NormalizedFramingMode(QString mode)
{
	mode = mode.trimmed().toLower();
	if (mode == QStringLiteral("face") || mode == QStringLiteral("face_headroom") ||
	    mode == QStringLiteral("headroom"))
		return QStringLiteral("face_headroom");
	if (mode == QStringLiteral("full") || mode == QStringLiteral("full_body"))
		return QStringLiteral("full_body");
	if (mode == QStringLiteral("group"))
		return QStringLiteral("group");
	return QStringLiteral("upper_body");
}

QString NormalizedShotMode(QString mode)
{
	mode = mode.trimmed().toLower();
	if (mode == QStringLiteral("ai_auto_frame") || mode == QStringLiteral("hybrid"))
		return mode;
	return QStringLiteral("keyframe_loop");
}

QString NormalizedPlaybackMode(QString mode)
{
	mode = mode.trimmed().toLower();
	if (mode == QStringLiteral("restart_on_program") || mode == QStringLiteral("pause_when_hidden") ||
	    mode == QStringLiteral("cue_in_preview"))
		return mode;
	return QStringLiteral("free_run");
}

QString NormalizedEasing(QString easing)
{
	easing = easing.trimmed().toLower();
	if (easing == QStringLiteral("linear") || easing == QStringLiteral("ease_in_out"))
		return easing;
	return QStringLiteral("smoothstep");
}

QString NormalizedLoopMode(QString loopMode)
{
	loopMode = loopMode.trimmed().toLower();
	if (loopMode == QStringLiteral("restart"))
		return loopMode;
	return QStringLiteral("ping_pong");
}

bool ShotUsesAiRuntime(const SwitchMotionShot &shot)
{
	const QString mode = NormalizedShotMode(shot.shotMode);
	return mode == QStringLiteral("ai_auto_frame") || mode == QStringLiteral("hybrid");
}

bool SourceHasAiShot(const QVector<SwitchMotionShot> &shots, const QString &sourceUuid)
{
	for (const auto &shot : shots) {
		if (shot.enabled && shot.sourceUuid == sourceUuid && ShotUsesAiRuntime(shot))
			return true;
	}
	return false;
}

bool SourceHasShot(const QVector<SwitchMotionShot> &shots, const QString &sourceUuid)
{
	for (const auto &shot : shots) {
		if (shot.enabled && shot.sourceUuid == sourceUuid)
			return true;
	}
	return false;
}

QString ShotTransformKey(const SwitchMotionShot &shot)
{
	return QStringLiteral("%1:%2").arg(shot.sceneUuid, QString::number(shot.sceneItemId));
}

bool SourceAllowsContinuousSceneItemTransform(obs_source_t *source)
{
	UNUSED_PARAMETER(source);
	return true;
}

QString InstallerManagedModelMissingMessage()
{
	return QStringLiteral("Motion model is not installed. Run the installer model download step or set a profile model path.");
}

float SmoothStep(float value)
{
	value = ClampFloat(value, 0.0f, 1.0f);
	return value * value * (3.0f - 2.0f * value);
}

float ApplySoftDeadZone(float offset, float deadZone)
{
	const float magnitude = std::abs(offset);
	if (deadZone <= 0.0001f || magnitude <= deadZone)
		return deadZone <= 0.0001f ? offset : 0.0f;

	const float softBand = std::max(0.02f, deadZone * 0.75f);
	if (magnitude >= deadZone + softBand)
		return offset;

	const float blend = SmoothStep((magnitude - deadZone) / softBand);
	return std::copysign(magnitude * blend, offset);
}

float MotionDeltaSeconds(qint64 nowMs, qint64 previousMs)
{
	if (previousMs <= 0 || nowMs <= previousMs)
		return 1.0f / 30.0f;
	return ClampFloat(static_cast<float>(nowMs - previousMs) / 1000.0f, 1.0f / 240.0f, 0.12f);
}

float MotionResponseAlpha(float smoothing, float deltaSeconds)
{
	const float response = 2.4f + ClampFloat(smoothing, 0.01f, 1.0f) * 9.6f;
	return ClampFloat(1.0f - std::exp(-response * deltaSeconds), 0.0f, 1.0f);
}

float MotionTargetAlpha(float smoothing, float deltaSeconds)
{
	const float response = 2.0f + ClampFloat(smoothing, 0.01f, 1.0f) * 5.4f;
	return ClampFloat(1.0f - std::exp(-response * deltaSeconds), 0.0f, 0.72f);
}

float MoveToward(float current, float target, float maximumDelta)
{
	const float delta = target - current;
	if (std::abs(delta) <= maximumDelta)
		return target;
	return current + std::copysign(maximumDelta, delta);
}

float EaseToward(float current, float target, float alpha, float maximumDelta)
{
	const float eased = current + (target - current) * alpha;
	return MoveToward(current, eased, maximumDelta);
}

struct MotionAssignment {
	int trackIndex = -1;
	int detectionIndex = -1;
	float score = 0.0f;
};

float MatchScore(const SwitchMotionTrack &track, const SwitchMotionDetection &detection, const QSize &frameSize,
		 float matchThreshold)
{
	const float iou = DetectionIou(track.smoothedBox, detection);
	if (iou >= matchThreshold)
		return 1.0f + iou + detection.confidence * 0.1f;

	const float distance = DetectionCenterDistance(track.smoothedBox, detection, frameSize);
	const float trackSpan = std::max(DetectionWidth(track.smoothedBox) / std::max(1, frameSize.width()),
					 DetectionHeight(track.smoothedBox) / std::max(1, frameSize.height()));
	const float distanceLimit = std::clamp(trackSpan * 0.9f + 0.04f, 0.08f, 0.22f);
	if (distance > distanceLimit)
		return -std::numeric_limits<float>::infinity();

	const float sizeRatio = DetectionArea(track.smoothedBox) > 0.0f && DetectionArea(detection) > 0.0f
					? std::min(DetectionArea(track.smoothedBox), DetectionArea(detection)) /
						  std::max(DetectionArea(track.smoothedBox), DetectionArea(detection))
					: 0.0f;
	if (sizeRatio < 0.18f)
		return -std::numeric_limits<float>::infinity();

	return 0.35f + (distanceLimit - distance) / distanceLimit * 0.45f + sizeRatio * 0.20f +
	       detection.confidence * 0.1f;
}

void SolveAssignmentsRecursive(const QVector<QVector<float>> &scores, int row, QVector<bool> &usedDetections,
			       QVector<MotionAssignment> &current, float currentScore, QVector<MotionAssignment> &best,
			       float &bestScore)
{
	if (row >= scores.size()) {
		if (currentScore > bestScore) {
			bestScore = currentScore;
			best = current;
		}
		return;
	}

	SolveAssignmentsRecursive(scores, row + 1, usedDetections, current, currentScore, best, bestScore);
	for (int col = 0; col < scores[row].size(); col++) {
		const float score = scores[row][col];
		if (usedDetections[col] || !std::isfinite(score))
			continue;
		usedDetections[col] = true;
		current.push_back({row, col, score});
		SolveAssignmentsRecursive(scores, row + 1, usedDetections, current, currentScore + score, best, bestScore);
		current.pop_back();
		usedDetections[col] = false;
	}
}

QVector<MotionAssignment> SolveAssignments(const QVector<SwitchMotionTrack> &tracks,
					   const QVector<SwitchMotionDetection> &detections,
					   const QSize &frameSize,
					   float matchThreshold)
{
	QVector<MotionAssignment> empty;
	if (tracks.isEmpty() || detections.isEmpty())
		return empty;

	QVector<QVector<float>> scores;
	scores.reserve(tracks.size());
	for (const auto &track : tracks) {
		QVector<float> row;
		row.reserve(detections.size());
		for (const auto &detection : detections)
			row.push_back(MatchScore(track, detection, frameSize, matchThreshold));
		scores.push_back(row);
	}

	if (tracks.size() > 10 || detections.size() > 10) {
		QVector<MotionAssignment> candidates;
		for (int row = 0; row < scores.size(); row++) {
			for (int col = 0; col < scores[row].size(); col++) {
				if (std::isfinite(scores[row][col]))
					candidates.push_back({row, col, scores[row][col]});
			}
		}
		std::sort(candidates.begin(), candidates.end(), [](const auto &left, const auto &right) {
			if (left.score != right.score)
				return left.score > right.score;
			if (left.trackIndex != right.trackIndex)
				return left.trackIndex < right.trackIndex;
			return left.detectionIndex < right.detectionIndex;
		});
		QVector<bool> usedTracks(tracks.size(), false);
		QVector<bool> usedDetections(detections.size(), false);
		QVector<MotionAssignment> result;
		for (const auto &candidate : candidates) {
			if (usedTracks[candidate.trackIndex] || usedDetections[candidate.detectionIndex])
				continue;
			usedTracks[candidate.trackIndex] = true;
			usedDetections[candidate.detectionIndex] = true;
			result.push_back(candidate);
		}
		std::sort(result.begin(), result.end(), [](const auto &left, const auto &right) {
			return left.trackIndex < right.trackIndex;
		});
		return result;
	}

	QVector<bool> usedDetections(detections.size(), false);
	QVector<MotionAssignment> current;
	QVector<MotionAssignment> best;
	float bestScore = -std::numeric_limits<float>::infinity();
	SolveAssignmentsRecursive(scores, 0, usedDetections, current, 0.0f, best, bestScore);
	std::sort(best.begin(), best.end(), [](const auto &left, const auto &right) {
		if (left.trackIndex != right.trackIndex)
			return left.trackIndex < right.trackIndex;
		return left.detectionIndex < right.detectionIndex;
	});
	return best;
}

void UpdateTrackFromDetection(SwitchMotionTrack *track, const SwitchMotionDetection &detection, qint64 nowMs,
			      const QString &sourceUuid)
{
	if (!track)
		return;

	const float previousCenterX = DetectionCenterX(track->smoothedBox);
	const float previousCenterY = DetectionCenterY(track->smoothedBox);
	const bool hadBox = DetectionArea(track->smoothedBox) > 0.0f && track->ageFrames > 0;
	const float alpha = track->missedFrames > 0 ? 0.55f : 0.38f;
	if (hadBox) {
		track->smoothedBox.x1 += (detection.x1 - track->smoothedBox.x1) * alpha;
		track->smoothedBox.y1 += (detection.y1 - track->smoothedBox.y1) * alpha;
		track->smoothedBox.x2 += (detection.x2 - track->smoothedBox.x2) * alpha;
		track->smoothedBox.y2 += (detection.y2 - track->smoothedBox.y2) * alpha;
	} else {
		track->smoothedBox = detection;
	}

	const float deltaSeconds = MotionDeltaSeconds(nowMs, track->lastSeenMs);
	if (hadBox && deltaSeconds > 0.0f) {
		track->velocityX = (DetectionCenterX(track->smoothedBox) - previousCenterX) / deltaSeconds;
		track->velocityY = (DetectionCenterY(track->smoothedBox) - previousCenterY) / deltaSeconds;
	}
	track->bbox = detection;
	track->confidence = detection.confidence;
	track->ageFrames++;
	track->missedFrames = 0;
	track->lastSeenMs = nowMs;
	track->sourceUuid = sourceUuid;
	track->state = track->ageFrames <= 1 ? QStringLiteral("new") : QStringLiteral("active");
}

QString StringValue(obs_data_t *data, const char *key)
{
	return data ? QString::fromUtf8(obs_data_get_string(data, key)) : QString();
}

float FloatValue(obs_data_t *data, const char *key, float fallback)
{
	if (!data || !obs_data_has_user_value(data, key))
		return fallback;
	return static_cast<float>(obs_data_get_double(data, key));
}

int IntValue(obs_data_t *data, const char *key, int fallback)
{
	if (!data || !obs_data_has_user_value(data, key))
		return fallback;
	return static_cast<int>(obs_data_get_int(data, key));
}

qint64 Int64Value(obs_data_t *data, const char *key, qint64 fallback)
{
	if (!data || !obs_data_has_user_value(data, key))
		return fallback;
	return static_cast<qint64>(obs_data_get_int(data, key));
}

bool BoolValue(obs_data_t *data, const char *key, bool fallback)
{
	if (!data || !obs_data_has_user_value(data, key))
		return fallback;
	return obs_data_get_bool(data, key);
}

void SetProfileSettings(obs_data_t *settings, const SwitchMotionProfile &profile, bool transformEnabled = true)
{
	obs_data_set_string(settings, "profile_id", profile.id.toUtf8().constData());
	obs_data_set_bool(settings, "motion_enabled", profile.enabled);
	obs_data_set_bool(settings, "transform_enabled", transformEnabled);
	obs_data_set_int(settings, "transform_settings_version", 2);
	obs_data_set_double(settings, "confidence_threshold", profile.confidenceThreshold);
	obs_data_set_int(settings, "target_class_id", profile.targetClassId);
	obs_data_set_double(settings, "max_zoom", profile.maxZoom);
	obs_data_set_double(settings, "framing_margin", profile.framingMargin);
	obs_data_set_double(settings, "dead_zone", profile.deadZone);
	obs_data_set_double(settings, "smoothing", profile.smoothing);
	obs_data_set_int(settings, "hold_ms", profile.holdMs);
	obs_data_set_string(settings, "backend", profile.backend.toUtf8().constData());
	obs_data_set_string(settings, "model_path", profile.modelPath.toUtf8().constData());
	obs_data_set_string(settings, "subject_mode", profile.subjectMode.toUtf8().constData());
	obs_data_set_string(settings, "framing_mode", profile.framingMode.toUtf8().constData());
	obs_data_set_double(settings, "tracker_high_threshold", profile.trackerHighThreshold);
	obs_data_set_double(settings, "tracker_low_threshold", profile.trackerLowThreshold);
	obs_data_set_double(settings, "new_track_threshold", profile.newTrackThreshold);
	obs_data_set_int(settings, "track_buffer_frames", profile.trackBufferFrames);
	obs_data_set_int(settings, "locked_track_grace_frames", profile.lockedTrackGraceFrames);
	obs_data_set_int(settings, "auto_switch_ms", profile.autoSwitchMs);
	obs_data_set_double(settings, "pan_responsiveness", profile.panResponsiveness);
	obs_data_set_double(settings, "tilt_responsiveness", profile.tiltResponsiveness);
	obs_data_set_double(settings, "zoom_responsiveness", profile.zoomResponsiveness);
	obs_data_set_double(settings, "max_pan_speed", profile.maxPanSpeed);
	obs_data_set_double(settings, "max_tilt_speed", profile.maxTiltSpeed);
	obs_data_set_double(settings, "max_zoom_speed", profile.maxZoomSpeed);
	obs_data_set_bool(settings, "debug_overlay", profile.debugOverlay);
	obs_data_set_int(settings, "locked_track_id", profile.lockedTrackId);
}

bool ApplyBindingNow(const SwitchMotionBinding &binding, const SwitchMotionProfile &profile, bool transformEnabled,
		     QString *message)
{
	obs_source_t *source = obs_get_source_by_uuid(binding.sourceUuid.toUtf8().constData());
	if (!source) {
		if (message)
			*message = QStringLiteral("Unable to find OBS source for Motion binding");
		return false;
	}

	obs_data_t *settings = obs_data_create();
	SetProfileSettings(settings, profile, transformEnabled);

	obs_source_t *filter = obs_source_get_filter_by_name(source, kMotionFilterName);
	if (filter) {
		obs_source_update(filter, settings);
		obs_source_release(filter);
	} else {
		filter = obs_source_create_private(kMotionFilterId, kMotionFilterName, settings);
		if (!filter) {
			obs_data_release(settings);
			obs_source_release(source);
			if (message)
				*message = QStringLiteral("Unable to create Switch Motion filter");
			return false;
		}
		obs_source_filter_add(source, filter);
		obs_source_release(filter);
	}

	obs_data_release(settings);
	obs_source_release(source);
	return true;
}

bool RemoveMotionFilterFromSource(obs_source_t *source)
{
	if (!source)
		return false;

	obs_source_t *filter = obs_source_get_filter_by_name(source, kMotionFilterName);
	if (!filter)
		return false;

	obs_source_filter_remove(source, filter);
	obs_source_release(filter);
	return true;
}

bool RemoveBindingNow(const QString &sourceUuid, QString *message)
{
	obs_source_t *source = obs_get_source_by_uuid(sourceUuid.toUtf8().constData());
	if (!source) {
		if (message)
			*message = QStringLiteral("Unable to find OBS source for Motion binding");
		return false;
	}

	RemoveMotionFilterFromSource(source);
	obs_source_release(source);
	return true;
}

bool RemoveMotionFilterFromEnum(void *, obs_source_t *source)
{
	RemoveMotionFilterFromSource(source);
	return true;
}

bool NearlyEqual(float left, float right)
{
	return std::abs(left - right) < 0.0001f;
}

bool ProfilesEquivalent(const SwitchMotionProfile &left, const SwitchMotionProfile &right)
{
	return left.id == right.id &&
	       left.name == right.name &&
	       left.enabled == right.enabled &&
	       NearlyEqual(left.confidenceThreshold, right.confidenceThreshold) &&
	       left.targetClassId == right.targetClassId &&
	       NearlyEqual(left.maxZoom, right.maxZoom) &&
	       NearlyEqual(left.framingMargin, right.framingMargin) &&
	       NearlyEqual(left.deadZone, right.deadZone) &&
	       NearlyEqual(left.smoothing, right.smoothing) &&
	       left.holdMs == right.holdMs &&
	       left.backend == right.backend &&
	       left.modelPath == right.modelPath &&
	       left.subjectMode == right.subjectMode &&
	       left.framingMode == right.framingMode &&
	       NearlyEqual(left.trackerHighThreshold, right.trackerHighThreshold) &&
	       NearlyEqual(left.trackerLowThreshold, right.trackerLowThreshold) &&
	       NearlyEqual(left.newTrackThreshold, right.newTrackThreshold) &&
	       left.trackBufferFrames == right.trackBufferFrames &&
	       left.lockedTrackGraceFrames == right.lockedTrackGraceFrames &&
	       left.autoSwitchMs == right.autoSwitchMs &&
	       NearlyEqual(left.panResponsiveness, right.panResponsiveness) &&
	       NearlyEqual(left.tiltResponsiveness, right.tiltResponsiveness) &&
	       NearlyEqual(left.zoomResponsiveness, right.zoomResponsiveness) &&
	       NearlyEqual(left.maxPanSpeed, right.maxPanSpeed) &&
	       NearlyEqual(left.maxTiltSpeed, right.maxTiltSpeed) &&
	       NearlyEqual(left.maxZoomSpeed, right.maxZoomSpeed) &&
	       left.debugOverlay == right.debugOverlay &&
	       left.lockedTrackId == right.lockedTrackId;
}
} // namespace

void SwitchMotionPublishRuntimeState(const SwitchMotionRuntimeState &state)
{
	QPointer<SwitchMotionManager> manager;
	{
		std::lock_guard<std::mutex> lock(gRuntimeStateManagerMutex);
		manager = gRuntimeStateManager;
	}
	if (!manager)
		return;
	QMetaObject::invokeMethod(manager, [manager, state]() {
		if (manager)
			manager->SetRuntimeState(state);
	}, Qt::QueuedConnection);
}

void SwitchMotionPublishRuntimeError(const QString &message)
{
	QPointer<SwitchMotionManager> manager;
	{
		std::lock_guard<std::mutex> lock(gRuntimeStateManagerMutex);
		manager = gRuntimeStateManager;
	}
	if (!manager)
		return;
	QMetaObject::invokeMethod(manager, [manager, message]() {
		if (manager)
			manager->RaiseRuntimeError(message);
	}, Qt::QueuedConnection);
}

void SwitchMotionPublishFilterProfileUpdate(const SwitchMotionProfile &profile, const QString &sourceUuid)
{
	QPointer<SwitchMotionManager> manager;
	{
		std::lock_guard<std::mutex> lock(gRuntimeStateManagerMutex);
		manager = gRuntimeStateManager;
	}
	if (!manager)
		return;
	QMetaObject::invokeMethod(manager, [manager, profile, sourceUuid]() {
		if (manager)
			manager->SyncProfileFromFilter(profile, sourceUuid);
	}, Qt::QueuedConnection);
}

QString SwitchCreateMotionId(const QString &prefix)
{
	return QStringLiteral("%1-%2").arg(prefix, QString::number(QDateTime::currentMSecsSinceEpoch(), 36));
}

SwitchMotionProfile SwitchDefaultMotionProfile(const QString &modelPath)
{
	SwitchMotionProfile profile;
	profile.id = QStringLiteral("motion-default");
	profile.name = QStringLiteral("Default Auto Frame");
	profile.enabled = false;
	profile.maxZoom = 2.1f;
	profile.deadZone = 0.045f;
	profile.smoothing = 0.52f;
	profile.holdMs = 1200;
	profile.subjectMode = QStringLiteral("auto");
	profile.framingMode = QStringLiteral("upper_body");
	profile.trackerHighThreshold = 0.55f;
	profile.trackerLowThreshold = 0.20f;
	profile.newTrackThreshold = 0.65f;
	profile.trackBufferFrames = 45;
	profile.lockedTrackGraceFrames = 90;
	profile.autoSwitchMs = 500;
	profile.panResponsiveness = 0.64f;
	profile.tiltResponsiveness = 0.62f;
	profile.zoomResponsiveness = 0.54f;
	profile.maxPanSpeed = 0.85f;
	profile.maxTiltSpeed = 0.75f;
	profile.maxZoomSpeed = 1.25f;
	profile.modelPath = modelPath;
	return profile;
}

SwitchMotionShot SwitchDefaultMotionShot()
{
	SwitchMotionShot shot;
	shot.id = SwitchCreateMotionId(QStringLiteral("motion-shot"));
	shot.name = QStringLiteral("Motion Shot");
	shot.enabled = true;
	shot.shotMode = QStringLiteral("keyframe_loop");
	shot.playbackMode = QStringLiteral("free_run");
	shot.durationMs = 18000;
	shot.easing = QStringLiteral("smoothstep");
	shot.loopMode = QStringLiteral("ping_pong");
	shot.startPanX = -0.025f;
	shot.endPanX = 0.025f;
	shot.startZoom = 1.08f;
	shot.endZoom = 1.18f;
	shot.maxZoom = 2.2f;
	return shot;
}

QVector<SwitchMotionShotPreset> SwitchMotionShotPresets()
{
	auto makePreset = [](const QString &id, const QString &name, float startPanX, float startPanY,
			     float startZoom, float endPanX, float endPanY, float endZoom, int durationMs,
			     const QString &easing = QStringLiteral("smoothstep"),
			     const QString &loopMode = QStringLiteral("ping_pong")) {
		SwitchMotionShot shot = SwitchDefaultMotionShot();
		shot.name = name;
		shot.shotMode = QStringLiteral("keyframe_loop");
		shot.startPanX = startPanX;
		shot.startPanY = startPanY;
		shot.startZoom = startZoom;
		shot.endPanX = endPanX;
		shot.endPanY = endPanY;
		shot.endZoom = endZoom;
		shot.durationMs = durationMs;
		shot.easing = easing;
		shot.loopMode = loopMode;
		return SwitchMotionShotPreset{id, name, shot};
	};

	return {
		makePreset(QStringLiteral("static_hold"), QStringLiteral("Static Hold"), 0.0f, 0.0f, 1.0f,
			   0.0f, 0.0f, 1.0f, 12000, QStringLiteral("linear"), QStringLiteral("restart")),
		makePreset(QStringLiteral("slow_push_in"), QStringLiteral("Slow Push In"), 0.0f, 0.0f, 1.0f,
			   0.0f, 0.0f, 1.28f, 22000),
		makePreset(QStringLiteral("slow_pull_out"), QStringLiteral("Slow Pull Out"), 0.0f, 0.0f, 1.28f,
			   0.0f, 0.0f, 1.0f, 22000),
		makePreset(QStringLiteral("left_to_right"), QStringLiteral("Left To Right"), -0.11f, 0.0f, 1.34f,
			   0.11f, 0.0f, 1.34f, 24000),
		makePreset(QStringLiteral("right_to_left"), QStringLiteral("Right To Left"), 0.11f, 0.0f, 1.34f,
			   -0.11f, 0.0f, 1.34f, 24000),
		makePreset(QStringLiteral("subtle_float"), QStringLiteral("Subtle Float"), -0.025f, -0.012f, 1.10f,
			   0.025f, 0.012f, 1.16f, 18000),
		makePreset(QStringLiteral("wide_room_drift"), QStringLiteral("Wide Room Drift"), -0.08f, 0.0f, 1.22f,
			   0.08f, 0.0f, 1.22f, 30000),
		makePreset(QStringLiteral("presenter_push"), QStringLiteral("Presenter Push"), -0.03f, -0.015f,
			   1.05f, 0.035f, -0.035f, 1.36f, 26000),
	};
}

obs_data_t *SwitchMotionProfileToObsData(const SwitchMotionProfile &profile)
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "id", profile.id.toUtf8().constData());
	obs_data_set_string(data, "name", profile.name.toUtf8().constData());
	obs_data_set_bool(data, "enabled", profile.enabled);
	obs_data_set_double(data, "confidenceThreshold", profile.confidenceThreshold);
	obs_data_set_int(data, "targetClassId", profile.targetClassId);
	obs_data_set_double(data, "maxZoom", profile.maxZoom);
	obs_data_set_double(data, "framingMargin", profile.framingMargin);
	obs_data_set_double(data, "deadZone", profile.deadZone);
	obs_data_set_double(data, "smoothing", profile.smoothing);
	obs_data_set_int(data, "holdMs", profile.holdMs);
	obs_data_set_string(data, "backend", profile.backend.toUtf8().constData());
	obs_data_set_string(data, "modelPath", profile.modelPath.toUtf8().constData());
	obs_data_set_string(data, "subjectMode", profile.subjectMode.toUtf8().constData());
	obs_data_set_string(data, "framingMode", profile.framingMode.toUtf8().constData());
	obs_data_set_double(data, "trackerHighThreshold", profile.trackerHighThreshold);
	obs_data_set_double(data, "trackerLowThreshold", profile.trackerLowThreshold);
	obs_data_set_double(data, "newTrackThreshold", profile.newTrackThreshold);
	obs_data_set_int(data, "trackBufferFrames", profile.trackBufferFrames);
	obs_data_set_int(data, "lockedTrackGraceFrames", profile.lockedTrackGraceFrames);
	obs_data_set_int(data, "autoSwitchMs", profile.autoSwitchMs);
	obs_data_set_double(data, "panResponsiveness", profile.panResponsiveness);
	obs_data_set_double(data, "tiltResponsiveness", profile.tiltResponsiveness);
	obs_data_set_double(data, "zoomResponsiveness", profile.zoomResponsiveness);
	obs_data_set_double(data, "maxPanSpeed", profile.maxPanSpeed);
	obs_data_set_double(data, "maxTiltSpeed", profile.maxTiltSpeed);
	obs_data_set_double(data, "maxZoomSpeed", profile.maxZoomSpeed);
	obs_data_set_bool(data, "debugOverlay", profile.debugOverlay);
	obs_data_set_int(data, "lockedTrackId", profile.lockedTrackId);
	return data;
}

SwitchMotionProfile SwitchMotionProfileFromObsData(obs_data_t *data, const QString &fallbackModelPath)
{
	SwitchMotionProfile profile = SwitchDefaultMotionProfile(fallbackModelPath);
	if (!data)
		return profile;

	profile.id = StringValue(data, "id").trimmed();
	profile.name = StringValue(data, "name").trimmed();
	if (profile.id.isEmpty())
		profile.id = SwitchCreateMotionId(QStringLiteral("motion-profile"));
	if (profile.name.isEmpty())
		profile.name = QStringLiteral("Motion Profile");

	profile.enabled = BoolValue(data, "enabled", profile.enabled);
	profile.confidenceThreshold = ClampFloat(FloatValue(data, "confidenceThreshold", profile.confidenceThreshold), 0.01f, 0.99f);
	profile.targetClassId = IntValue(data, "targetClassId", profile.targetClassId);
	profile.maxZoom = ClampFloat(FloatValue(data, "maxZoom", profile.maxZoom), 1.0f, 4.0f);
	profile.framingMargin = ClampFloat(FloatValue(data, "framingMargin", profile.framingMargin), 0.0f, 0.75f);
	profile.deadZone = ClampFloat(FloatValue(data, "deadZone", profile.deadZone), 0.0f, 0.5f);
	profile.smoothing = ClampFloat(FloatValue(data, "smoothing", profile.smoothing), 0.01f, 1.0f);
	profile.holdMs = std::max(0, IntValue(data, "holdMs", profile.holdMs));
	const bool legacyDefault =
		(std::abs(profile.maxZoom - 1.35f) < 0.0001f || std::abs(profile.maxZoom - 1.75f) < 0.0001f) &&
		std::abs(profile.framingMargin - 0.18f) < 0.0001f &&
		std::abs(profile.deadZone - 0.08f) < 0.0001f &&
		(std::abs(profile.smoothing - 0.18f) < 0.0001f || std::abs(profile.smoothing - 0.34f) < 0.0001f) &&
		profile.holdMs == 700;
	if (legacyDefault) {
		profile.maxZoom = 2.1f;
		profile.deadZone = 0.045f;
		profile.smoothing = 0.52f;
		profile.holdMs = 1200;
	}
	profile.backend = StringValue(data, "backend").trimmed();
	if (profile.backend.isEmpty())
		profile.backend = QStringLiteral("auto");
	profile.modelPath = StringValue(data, "modelPath").trimmed();
	if (profile.modelPath.isEmpty())
		profile.modelPath = fallbackModelPath;
	profile.subjectMode = NormalizedSubjectMode(StringValue(data, "subjectMode"));
	profile.framingMode = NormalizedFramingMode(StringValue(data, "framingMode"));
	profile.trackerHighThreshold = ClampFloat(FloatValue(data, "trackerHighThreshold", profile.trackerHighThreshold), 0.01f, 0.99f);
	profile.trackerLowThreshold = ClampFloat(FloatValue(data, "trackerLowThreshold", profile.trackerLowThreshold), 0.01f,
						profile.trackerHighThreshold);
	profile.newTrackThreshold = ClampFloat(FloatValue(data, "newTrackThreshold", profile.newTrackThreshold),
					      profile.trackerHighThreshold, 0.99f);
	profile.trackBufferFrames = std::max(1, IntValue(data, "trackBufferFrames", profile.trackBufferFrames));
	profile.lockedTrackGraceFrames = std::max(profile.trackBufferFrames,
						 IntValue(data, "lockedTrackGraceFrames", profile.lockedTrackGraceFrames));
	profile.autoSwitchMs = std::max(0, IntValue(data, "autoSwitchMs", profile.autoSwitchMs));
	profile.panResponsiveness = ClampFloat(FloatValue(data, "panResponsiveness", profile.panResponsiveness), 0.05f, 1.5f);
	profile.tiltResponsiveness = ClampFloat(FloatValue(data, "tiltResponsiveness", profile.tiltResponsiveness), 0.05f, 1.5f);
	profile.zoomResponsiveness = ClampFloat(FloatValue(data, "zoomResponsiveness", profile.zoomResponsiveness), 0.05f, 1.5f);
	profile.maxPanSpeed = ClampFloat(FloatValue(data, "maxPanSpeed", profile.maxPanSpeed), 0.05f, 3.0f);
	profile.maxTiltSpeed = ClampFloat(FloatValue(data, "maxTiltSpeed", profile.maxTiltSpeed), 0.05f, 3.0f);
	profile.maxZoomSpeed = ClampFloat(FloatValue(data, "maxZoomSpeed", profile.maxZoomSpeed), 0.05f, 4.0f);
	profile.debugOverlay = BoolValue(data, "debugOverlay", profile.debugOverlay);
	profile.lockedTrackId = IntValue(data, "lockedTrackId", profile.lockedTrackId);
	return profile;
}

obs_data_t *SwitchMotionBindingToObsData(const SwitchMotionBinding &binding)
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "sourceUuid", binding.sourceUuid.toUtf8().constData());
	obs_data_set_string(data, "sourceName", binding.sourceName.toUtf8().constData());
	obs_data_set_string(data, "profileId", binding.profileId.toUtf8().constData());
	obs_data_set_bool(data, "enabled", binding.enabled);
	return data;
}

SwitchMotionBinding SwitchMotionBindingFromObsData(obs_data_t *data)
{
	SwitchMotionBinding binding;
	if (!data)
		return binding;
	binding.sourceUuid = StringValue(data, "sourceUuid");
	binding.sourceName = StringValue(data, "sourceName");
	binding.profileId = StringValue(data, "profileId");
	binding.enabled = BoolValue(data, "enabled", true);
	return binding;
}

obs_data_t *SwitchMotionShotToObsData(const SwitchMotionShot &shot)
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "id", shot.id.toUtf8().constData());
	obs_data_set_string(data, "name", shot.name.toUtf8().constData());
	obs_data_set_bool(data, "enabled", shot.enabled);
	obs_data_set_string(data, "sceneUuid", shot.sceneUuid.toUtf8().constData());
	obs_data_set_string(data, "sceneName", shot.sceneName.toUtf8().constData());
	obs_data_set_int(data, "sceneItemId", static_cast<long long>(shot.sceneItemId));
	obs_data_set_string(data, "sourceUuid", shot.sourceUuid.toUtf8().constData());
	obs_data_set_string(data, "sourceName", shot.sourceName.toUtf8().constData());
	obs_data_set_string(data, "profileId", shot.profileId.toUtf8().constData());
	obs_data_set_string(data, "shotMode", shot.shotMode.toUtf8().constData());
	obs_data_set_string(data, "playbackMode", shot.playbackMode.toUtf8().constData());
	obs_data_set_int(data, "phaseAnchorMs", static_cast<long long>(shot.phaseAnchorMs));
	obs_data_set_int(data, "pausedPhaseMs", static_cast<long long>(shot.pausedPhaseMs));
	obs_data_set_int(data, "lastProgramMs", static_cast<long long>(shot.lastProgramMs));
	obs_data_set_double(data, "startPanX", shot.startPanX);
	obs_data_set_double(data, "startPanY", shot.startPanY);
	obs_data_set_double(data, "startZoom", shot.startZoom);
	obs_data_set_double(data, "endPanX", shot.endPanX);
	obs_data_set_double(data, "endPanY", shot.endPanY);
	obs_data_set_double(data, "endZoom", shot.endZoom);
	obs_data_set_int(data, "durationMs", shot.durationMs);
	obs_data_set_string(data, "easing", shot.easing.toUtf8().constData());
	obs_data_set_string(data, "loopMode", shot.loopMode.toUtf8().constData());
	obs_data_set_double(data, "maxZoom", shot.maxZoom);
	return data;
}

SwitchMotionShot SwitchMotionShotFromObsData(obs_data_t *data)
{
	SwitchMotionShot shot = SwitchDefaultMotionShot();
	if (!data)
		return shot;

	shot.id = StringValue(data, "id").trimmed();
	if (shot.id.isEmpty())
		shot.id = SwitchCreateMotionId(QStringLiteral("motion-shot"));
	shot.name = StringValue(data, "name").trimmed();
	if (shot.name.isEmpty())
		shot.name = QStringLiteral("Motion Shot");
	shot.enabled = BoolValue(data, "enabled", shot.enabled);
	shot.sceneUuid = StringValue(data, "sceneUuid");
	shot.sceneName = StringValue(data, "sceneName");
	shot.sceneItemId = Int64Value(data, "sceneItemId", shot.sceneItemId);
	shot.sourceUuid = StringValue(data, "sourceUuid");
	shot.sourceName = StringValue(data, "sourceName");
	shot.profileId = StringValue(data, "profileId");
	shot.shotMode = NormalizedShotMode(StringValue(data, "shotMode"));
	shot.playbackMode = NormalizedPlaybackMode(StringValue(data, "playbackMode"));
	shot.phaseAnchorMs = Int64Value(data, "phaseAnchorMs", shot.phaseAnchorMs);
	shot.pausedPhaseMs = Int64Value(data, "pausedPhaseMs", shot.pausedPhaseMs);
	shot.lastProgramMs = Int64Value(data, "lastProgramMs", shot.lastProgramMs);
	shot.startPanX = ClampFloat(FloatValue(data, "startPanX", shot.startPanX), -0.5f, 0.5f);
	shot.startPanY = ClampFloat(FloatValue(data, "startPanY", shot.startPanY), -0.5f, 0.5f);
	shot.startZoom = ClampFloat(FloatValue(data, "startZoom", shot.startZoom), 1.0f, 4.0f);
	shot.endPanX = ClampFloat(FloatValue(data, "endPanX", shot.endPanX), -0.5f, 0.5f);
	shot.endPanY = ClampFloat(FloatValue(data, "endPanY", shot.endPanY), -0.5f, 0.5f);
	shot.endZoom = ClampFloat(FloatValue(data, "endZoom", shot.endZoom), 1.0f, 4.0f);
	shot.durationMs = std::max(250, IntValue(data, "durationMs", shot.durationMs));
	shot.easing = NormalizedEasing(StringValue(data, "easing"));
	shot.loopMode = NormalizedLoopMode(StringValue(data, "loopMode"));
	shot.maxZoom = ClampFloat(FloatValue(data, "maxZoom", shot.maxZoom), 1.0f, 4.0f);
	return shot;
}

obs_data_array_t *SwitchMotionShotsToObsArray(const QVector<SwitchMotionShot> &shots)
{
	obs_data_array_t *array = obs_data_array_create();
	for (const auto &shot : shots) {
		obs_data_t *entry = SwitchMotionShotToObsData(shot);
		obs_data_array_push_back(array, entry);
		obs_data_release(entry);
	}
	return array;
}

obs_data_t *SwitchMotionShotPresetToObsData(const SwitchMotionShotPreset &preset)
{
	obs_data_t *data = SwitchMotionShotToObsData(preset.shot);
	obs_data_set_string(data, "presetId", preset.id.toUtf8().constData());
	obs_data_set_string(data, "presetName", preset.name.toUtf8().constData());
	return data;
}

obs_data_array_t *SwitchMotionShotPresetsToObsArray()
{
	obs_data_array_t *array = obs_data_array_create();
	for (const auto &preset : SwitchMotionShotPresets()) {
		obs_data_t *entry = SwitchMotionShotPresetToObsData(preset);
		obs_data_array_push_back(array, entry);
		obs_data_release(entry);
	}
	return array;
}

obs_data_t *SwitchMotionTrackToObsData(const SwitchMotionTrack &track)
{
	obs_data_t *data = obs_data_create();
	obs_data_set_int(data, "trackId", track.trackId);
	obs_data_set_string(data, "state", track.state.toUtf8().constData());
	obs_data_set_string(data, "sourceUuid", track.sourceUuid.toUtf8().constData());
	obs_data_set_double(data, "confidence", track.confidence);
	obs_data_set_int(data, "ageFrames", track.ageFrames);
	obs_data_set_int(data, "missedFrames", track.missedFrames);
	obs_data_set_int(data, "lastSeenMs", track.lastSeenMs);
	obs_data_set_double(data, "velocityX", track.velocityX);
	obs_data_set_double(data, "velocityY", track.velocityY);

	obs_data_t *bbox = obs_data_create();
	obs_data_set_double(bbox, "x1", track.bbox.x1);
	obs_data_set_double(bbox, "y1", track.bbox.y1);
	obs_data_set_double(bbox, "x2", track.bbox.x2);
	obs_data_set_double(bbox, "y2", track.bbox.y2);
	obs_data_set_double(bbox, "confidence", track.bbox.confidence);
	obs_data_set_int(bbox, "classId", track.bbox.classId);
	obs_data_set_obj(data, "bbox", bbox);
	obs_data_release(bbox);

	obs_data_t *smoothed = obs_data_create();
	obs_data_set_double(smoothed, "x1", track.smoothedBox.x1);
	obs_data_set_double(smoothed, "y1", track.smoothedBox.y1);
	obs_data_set_double(smoothed, "x2", track.smoothedBox.x2);
	obs_data_set_double(smoothed, "y2", track.smoothedBox.y2);
	obs_data_set_double(smoothed, "confidence", track.smoothedBox.confidence);
	obs_data_set_int(smoothed, "classId", track.smoothedBox.classId);
	obs_data_set_obj(data, "smoothedBox", smoothed);
	obs_data_release(smoothed);
	return data;
}

obs_data_array_t *SwitchMotionTracksToObsArray(const QVector<SwitchMotionTrack> &tracks)
{
	obs_data_array_t *array = obs_data_array_create();
	for (const auto &track : tracks) {
		obs_data_t *entry = SwitchMotionTrackToObsData(track);
		obs_data_array_push_back(array, entry);
		obs_data_release(entry);
	}
	return array;
}

obs_data_t *SwitchMotionRuntimeStateToObsData(const SwitchMotionRuntimeState &state)
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "status", state.status.toUtf8().constData());
	obs_data_set_string(data, "backend", state.backend.toUtf8().constData());
	obs_data_set_string(data, "modelPath", state.modelPath.toUtf8().constData());
	obs_data_set_string(data, "message", state.message.toUtf8().constData());
	obs_data_set_string(data, "providerStatus", state.providerStatus.toUtf8().constData());
	obs_data_set_bool(data, "modelAvailable", state.modelAvailable);
	obs_data_set_bool(data, "targetActive", state.targetActive);
	obs_data_set_double(data, "targetConfidence", state.targetConfidence);
	obs_data_set_int(data, "targetTrackId", state.targetTrackId);
	obs_data_set_string(data, "targetState", state.targetState.toUtf8().constData());
	obs_data_set_string(data, "sourceUuid", state.sourceUuid.toUtf8().constData());
	obs_data_set_double(data, "camX", state.camX);
	obs_data_set_double(data, "camY", state.camY);
	obs_data_set_double(data, "zoom", state.zoom);
	obs_data_set_double(data, "preprocessingMs", state.preprocessingMs);
	obs_data_set_double(data, "inferenceMs", state.inferenceMs);
	obs_data_set_double(data, "trackingMs", state.trackingMs);
	obs_data_set_int(data, "droppedFrames", static_cast<long long>(state.droppedFrames));
	obs_data_set_int(data, "activeTrackCount", state.activeTrackCount);
	obs_data_set_string(data, "activeShotId", state.activeShotId.toUtf8().constData());
	obs_data_set_string(data, "activeShotName", state.activeShotName.toUtf8().constData());
	obs_data_set_string(data, "activeSceneName", state.activeSceneName.toUtf8().constData());
	obs_data_set_string(data, "activeShotMode", state.activeShotMode.toUtf8().constData());
	obs_data_set_string(data, "activeShotPlaybackMode", state.activeShotPlaybackMode.toUtf8().constData());
	obs_data_set_int(data, "activeShotPhaseMs", static_cast<long long>(state.activeShotPhaseMs));
	obs_data_set_double(data, "activeShotCamX", state.activeShotCamX);
	obs_data_set_double(data, "activeShotCamY", state.activeShotCamY);
	obs_data_set_double(data, "activeShotZoom", state.activeShotZoom);
	obs_data_array_t *tracks = SwitchMotionTracksToObsArray(state.tracks);
	obs_data_set_array(data, "tracks", tracks);
	obs_data_array_release(tracks);

	obs_data_t *target = obs_data_create();
	obs_data_set_bool(target, "active", state.targetActive);
	obs_data_set_int(target, "trackId", state.targetTrackId);
	obs_data_set_string(target, "sourceUuid", state.sourceUuid.toUtf8().constData());
	obs_data_set_string(target, "state", state.targetState.toUtf8().constData());
	obs_data_set_double(target, "confidence", state.targetConfidence);
	for (const auto &track : state.tracks) {
		if (track.trackId != state.targetTrackId)
			continue;
		obs_data_t *trackData = SwitchMotionTrackToObsData(track);
		obs_data_set_obj(target, "track", trackData);
		obs_data_release(trackData);
		break;
	}
	obs_data_set_obj(data, "target", target);
	obs_data_release(target);
	return data;
}

QVector<SwitchMotionDetection> SwitchParseYolo26Detections(const float *values, size_t valueCount,
							    const QVector<int64_t> &shape,
							    float confidenceThreshold,
							    int targetClassId)
{
	QVector<SwitchMotionDetection> detections;
	if (!values || shape.size() != 3 || shape[0] < 1)
		return detections;

	if (shape[2] == 6 && shape[1] > 0) {
		const size_t rows = static_cast<size_t>(shape[1]);
		const size_t needed = rows * 6;
		if (valueCount < needed)
			return detections;

		for (size_t row = 0; row < rows; row++) {
			const float *entry = values + row * 6;
			const float confidence = entry[4];
			const int classId = static_cast<int>(std::lround(entry[5]));
			if (confidence < confidenceThreshold)
				continue;
			if (targetClassId >= 0 && classId != targetClassId)
				continue;

			SwitchMotionDetection detection;
			detection.x1 = entry[0];
			detection.y1 = entry[1];
			detection.x2 = entry[2];
			detection.y2 = entry[3];
			detection.confidence = confidence;
			detection.classId = classId;
			detections.push_back(detection);
		}
		return detections;
	}

	if (shape[1] != 6 || shape[2] <= 0)
		return detections;

	const size_t rows = static_cast<size_t>(shape[2]);
	const size_t needed = rows * 6;
	if (valueCount < needed)
		return detections;
	for (size_t row = 0; row < rows; row++) {
		const float confidence = values[4 * rows + row];
		const int classId = static_cast<int>(std::lround(values[5 * rows + row]));
		if (confidence < confidenceThreshold)
			continue;
		if (targetClassId >= 0 && classId != targetClassId)
			continue;

		SwitchMotionDetection detection;
		detection.x1 = values[row];
		detection.y1 = values[rows + row];
		detection.x2 = values[2 * rows + row];
		detection.y2 = values[3 * rows + row];
		detection.confidence = confidence;
		detection.classId = classId;
		detections.push_back(detection);
	}
	return detections;
}

bool SwitchSelectPrimaryDetection(const QVector<SwitchMotionDetection> &detections, SwitchMotionDetection *selected)
{
	if (detections.isEmpty() || !selected)
		return false;

	auto best = std::max_element(detections.cbegin(), detections.cend(), [](const auto &a, const auto &b) {
		return a.confidence < b.confidence;
	});
	*selected = *best;
	return true;
}

bool SwitchSelectPrimaryDetection(const QVector<SwitchMotionDetection> &detections,
				  const SwitchMotionCameraState &previous,
				  const QSize &frameSize,
				  SwitchMotionDetection *selected)
{
	if (detections.isEmpty() || !selected)
		return false;
	if (!previous.targetActive || frameSize.width() <= 0 || frameSize.height() <= 0)
		return SwitchSelectPrimaryDetection(detections, selected);

	const bool hasExplicitTarget =
		std::abs(previous.targetCamX) > 0.0001f || std::abs(previous.targetCamY) > 0.0001f ||
		previous.targetZoom > 1.0001f;
	const float stableCamX = hasExplicitTarget ? previous.targetCamX : previous.camX;
	const float stableCamY = hasExplicitTarget ? previous.targetCamY : previous.camY;
	const float targetCenterX = ClampFloat(0.5f + stableCamX, 0.0f, 1.0f);
	const float targetCenterY = ClampFloat(0.5f + stableCamY, 0.0f, 1.0f);
	const float frameWidth = static_cast<float>(frameSize.width());
	const float frameHeight = static_cast<float>(frameSize.height());

	auto best = detections.cbegin();
	float bestScore = -std::numeric_limits<float>::infinity();
	float bestDistance = std::numeric_limits<float>::infinity();
	for (auto it = detections.cbegin(); it != detections.cend(); ++it) {
		const float centerX = ((it->x1 + it->x2) * 0.5f) / frameWidth;
		const float centerY = ((it->y1 + it->y2) * 0.5f) / frameHeight;
		const float dx = centerX - targetCenterX;
		const float dy = centerY - targetCenterY;
		const float distance = std::sqrt(dx * dx + dy * dy);
		const float score = it->confidence - distance * 0.75f;
		if (score > bestScore) {
			best = it;
			bestScore = score;
			bestDistance = distance;
		}
	}

	if (best->confidence < 0.65f && bestDistance > 0.18f)
		return false;

	*selected = *best;
	return true;
}

SwitchMotionDetection SwitchMotionFramingDetection(const SwitchMotionDetection &detection,
						   const QString &framingMode,
						   const QSize &frameSize)
{
	if (detection.classId != 0 || frameSize.width() <= 0 || frameSize.height() <= 0)
		return detection;

	const QString mode = NormalizedFramingMode(framingMode);
	if (mode == QStringLiteral("full_body") || mode == QStringLiteral("group"))
		return detection;

	const float frameWidth = static_cast<float>(frameSize.width());
	const float frameHeight = static_cast<float>(frameSize.height());
	const float boxWidth = std::max(1.0f, detection.x2 - detection.x1);
	const float boxHeight = std::max(1.0f, detection.y2 - detection.y1);
	const float centerX = (detection.x1 + detection.x2) * 0.5f;
	const float minTargetWidth = std::min(boxHeight * (mode == QStringLiteral("face_headroom") ? 0.28f : 0.52f),
					      frameWidth);
	const float widthScale = mode == QStringLiteral("face_headroom") ? 0.54f : 1.18f;
	const float topOffset = mode == QStringLiteral("face_headroom") ? 0.00f : -0.08f;
	const float bottomOffset = mode == QStringLiteral("face_headroom") ? 0.42f : 0.76f;
	const float targetWidth = std::clamp(boxWidth * widthScale, minTargetWidth, frameWidth);

	SwitchMotionDetection framing = detection;
	framing.x1 = std::clamp(centerX - targetWidth * 0.5f, 0.0f, frameWidth);
	framing.x2 = std::clamp(centerX + targetWidth * 0.5f, 0.0f, frameWidth);
	framing.y1 = std::clamp(detection.y1 + boxHeight * topOffset, 0.0f, std::max(0.0f, frameHeight - 1.0f));
	framing.y2 = std::clamp(detection.y1 + boxHeight * bottomOffset, framing.y1 + 1.0f, frameHeight);
	return framing;
}

SwitchMotionDetection SwitchMotionGroupDetection(const QVector<SwitchMotionTrack> &tracks)
{
	SwitchMotionDetection group;
	bool hasTrack = false;
	for (const auto &track : tracks) {
		if (!IsVisibleTrack(track))
			continue;
		const auto &box = DetectionArea(track.smoothedBox) > 0.0f ? track.smoothedBox : track.bbox;
		if (!hasTrack) {
			group = box;
			hasTrack = true;
		} else {
			group.x1 = std::min(group.x1, box.x1);
			group.y1 = std::min(group.y1, box.y1);
			group.x2 = std::max(group.x2, box.x2);
			group.y2 = std::max(group.y2, box.y2);
			group.confidence = std::max(group.confidence, track.confidence);
		}
	}
	if (!hasTrack)
		return {};
	group.classId = 0;
	return group;
}

qint64 SwitchMotionShotPhaseMs(const SwitchMotionShot &shot, qint64 nowMs)
{
	const int duration = std::max(1, shot.durationMs);
	if (shot.playbackMode == QStringLiteral("pause_when_hidden") && shot.phaseAnchorMs <= 0)
		return std::clamp<qint64>(shot.pausedPhaseMs, 0, duration - 1);
	if (shot.phaseAnchorMs <= 0)
		return 0;
	qint64 elapsed = nowMs - shot.phaseAnchorMs;
	if (elapsed < 0)
		elapsed = 0;
	return elapsed % duration;
}

SwitchMotionCameraState SwitchEvaluateMotionShotLoop(const SwitchMotionShot &shot, const SwitchMotionProfile &profile,
						     qint64 phaseMs)
{
	SwitchMotionCameraState state;
	const int duration = std::max(1, shot.durationMs);
	float t = ClampFloat(static_cast<float>(phaseMs) / static_cast<float>(duration), 0.0f, 1.0f);
	if (NormalizedLoopMode(shot.loopMode) == QStringLiteral("ping_pong")) {
		t = t <= 0.5f ? t * 2.0f : (1.0f - t) * 2.0f;
	}

	const QString easing = NormalizedEasing(shot.easing);
	if (easing == QStringLiteral("smoothstep")) {
		t = SmoothStep(t);
	} else if (easing == QStringLiteral("ease_in_out")) {
		constexpr float kPi = 3.14159265358979323846f;
		t = 0.5f - std::cos(ClampFloat(t, 0.0f, 1.0f) * kPi) * 0.5f;
	}

	const auto lerp = [t](float start, float end) { return start + (end - start) * t; };
	const float maxZoom = ClampFloat(shot.maxZoom > 0.0f ? shot.maxZoom : profile.maxZoom, 1.0f, 4.0f);
	state.zoom = ClampFloat(lerp(shot.startZoom, shot.endZoom), 1.0f, maxZoom);
	state.camX = lerp(shot.startPanX, shot.endPanX);
	state.camY = lerp(shot.startPanY, shot.endPanY);

	const float requiredZoomForPanX = 1.0f / std::max(0.01f, 1.0f - 2.0f * std::abs(state.camX));
	const float requiredZoomForPanY = 1.0f / std::max(0.01f, 1.0f - 2.0f * std::abs(state.camY));
	state.zoom = ClampFloat(std::max({state.zoom, requiredZoomForPanX, requiredZoomForPanY}), 1.0f, maxZoom);
	state.camX = ClampCameraOffset(state.camX, state.zoom);
	state.camY = ClampCameraOffset(state.camY, state.zoom);
	state.targetZoom = state.zoom;
	state.targetCamX = state.camX;
	state.targetCamY = state.camY;
	state.targetActive = true;
	return state;
}

SwitchMotionCameraState SwitchComposeMotionCameraState(const SwitchMotionProfile &profile,
						       const SwitchMotionCameraState &aiState,
						       const SwitchMotionShot &shot,
						       qint64 nowMs)
{
	const QString mode = NormalizedShotMode(shot.shotMode);
	const qint64 phase = SwitchMotionShotPhaseMs(shot, nowMs);
	const auto loop = SwitchEvaluateMotionShotLoop(shot, profile, phase);
	const float maxZoom = ClampFloat(shot.maxZoom > 0.0f ? shot.maxZoom : profile.maxZoom, 1.0f, 4.0f);

	SwitchMotionCameraState result;
	if (mode == QStringLiteral("ai_auto_frame")) {
		result = aiState;
		if (result.zoom <= 0.0f)
			result.zoom = 1.0f;
	} else if (mode == QStringLiteral("hybrid")) {
		result = aiState;
		if (result.zoom <= 0.0f)
			result.zoom = 1.0f;
		result.camX += loop.camX;
		result.camY += loop.camY;
		result.zoom *= loop.zoom;
		result.targetActive = aiState.targetActive;
		result.targetConfidence = aiState.targetConfidence;
		result.targetTrackId = aiState.targetTrackId;
	} else {
		result = loop;
	}

	result.zoom = ClampFloat(result.zoom, 1.0f, maxZoom);
	result.camX = ClampCameraOffset(result.camX, result.zoom);
	result.camY = ClampCameraOffset(result.camY, result.zoom);
	result.targetCamX = ClampCameraOffset(result.targetCamX, result.zoom);
	result.targetCamY = ClampCameraOffset(result.targetCamY, result.zoom);
	result.targetZoom = ClampFloat(result.targetZoom <= 0.0f ? result.zoom : result.targetZoom, 1.0f, maxZoom);
	return result;
}

bool SwitchMotionSelectTargetTrack(const QVector<SwitchMotionTrack> &tracks,
				   const SwitchMotionProfile &profile,
				   const SwitchMotionCameraState &previousCamera,
				   const QSize &frameSize,
				   qint64 nowMs,
				   int *candidateTrackId,
				   qint64 *candidateSinceMs,
				   SwitchMotionTrack *selected)
{
	if (!selected || tracks.isEmpty())
		return false;

	const QString subjectMode = NormalizedSubjectMode(profile.subjectMode);
	const auto findTrack = [&tracks](int trackId, int maxMissedFrames) -> const SwitchMotionTrack * {
		if (trackId < 0)
			return nullptr;
		for (const auto &track : tracks) {
			if (track.trackId == trackId && track.missedFrames <= maxMissedFrames)
				return &track;
		}
		return nullptr;
	};

	if (subjectMode == QStringLiteral("off"))
		return false;

	if (subjectMode == QStringLiteral("locked")) {
		if (const auto *track = findTrack(profile.lockedTrackId, std::max(profile.lockedTrackGraceFrames, profile.trackBufferFrames))) {
			*selected = *track;
			return IsVisibleTrack(*track);
		}
	}

	if (subjectMode == QStringLiteral("hold") && previousCamera.targetTrackId >= 0) {
		if (const auto *track = findTrack(previousCamera.targetTrackId, profile.trackBufferFrames)) {
			*selected = *track;
			return IsVisibleTrack(*track);
		}
		return false;
	}

	const float frameWidth = static_cast<float>(std::max(1, frameSize.width()));
	const float frameHeight = static_cast<float>(std::max(1, frameSize.height()));
	int bestTrackId = -1;
	float bestScore = -std::numeric_limits<float>::infinity();
	for (const auto &track : tracks) {
		if (!IsVisibleTrack(track))
			continue;
		const auto &box = DetectionArea(track.smoothedBox) > 0.0f ? track.smoothedBox : track.bbox;
		const float centerX = DetectionCenterX(box) / frameWidth;
		const float centerY = DetectionCenterY(box) / frameHeight;
		const float dx = centerX - 0.5f;
		const float dy = centerY - 0.5f;
		const float centerScore = 1.0f - std::min(1.0f, std::sqrt(dx * dx + dy * dy) * 1.8f);
		const float boxArea = std::min(1.0f, DetectionArea(box) / (frameWidth * frameHeight));
		const float ageScore = std::min(1.0f, static_cast<float>(track.ageFrames) / 30.0f);
		const float continuity = previousCamera.targetTrackId == track.trackId ? 0.55f : 0.0f;
		const float missedPenalty = static_cast<float>(track.missedFrames) * 0.08f;
		const float score = track.confidence * 1.2f + centerScore * 0.35f + boxArea * 0.28f +
				    ageScore * 0.20f + continuity - missedPenalty;
		if (score > bestScore ||
		    (std::abs(score - bestScore) < 0.0001f && (bestTrackId < 0 || track.trackId < bestTrackId))) {
			bestScore = score;
			bestTrackId = track.trackId;
		}
	}

	if (bestTrackId < 0)
		return false;

	const int previousTarget = previousCamera.targetTrackId;
	const bool targetChanged = previousTarget >= 0 && previousTarget != bestTrackId;
	if (targetChanged && profile.autoSwitchMs > 0) {
		if (candidateTrackId && candidateSinceMs) {
			if (*candidateTrackId != bestTrackId) {
				*candidateTrackId = bestTrackId;
				*candidateSinceMs = nowMs;
				if (const auto *track = findTrack(previousTarget, profile.trackBufferFrames)) {
					*selected = *track;
					return IsVisibleTrack(*track);
				}
			} else if (nowMs - *candidateSinceMs < profile.autoSwitchMs) {
				if (const auto *track = findTrack(previousTarget, profile.trackBufferFrames)) {
					*selected = *track;
					return IsVisibleTrack(*track);
				}
			}
		}
	}

	if (candidateTrackId && *candidateTrackId == bestTrackId)
		*candidateTrackId = -1;
	if (candidateSinceMs)
		*candidateSinceMs = 0;
	if (const auto *track = findTrack(bestTrackId, profile.trackBufferFrames)) {
		*selected = *track;
		return true;
	}
	return false;
}

SwitchMotionCameraState SwitchUpdateMotionCameraState(const SwitchMotionProfile &profile,
						      const SwitchMotionDetection *detection,
						      const QSize &frameSize, qint64 nowMs,
						      SwitchMotionCameraState previous)
{
	SwitchMotionCameraState next = previous;
	const float smoothing = ClampFloat(profile.smoothing, 0.01f, 1.0f);
	const float deltaSeconds = MotionDeltaSeconds(nowMs, previous.lastUpdateMs);
	const float frameWidth = std::max(1, frameSize.width());
	const float frameHeight = std::max(1, frameSize.height());
	const float deadZone = ClampFloat(profile.deadZone, 0.0f, 0.5f);
	const QString framingMode = NormalizedFramingMode(profile.framingMode);
	const float framingMaxZoom = framingMode == QStringLiteral("face_headroom")
					     ? profile.maxZoom
					     : (framingMode == QStringLiteral("upper_body") ? 1.35f : 1.20f);
	const float effectiveMaxZoom = ClampFloat(std::min(profile.maxZoom, framingMaxZoom), 1.0f, 4.0f);

	float targetX = previous.targetCamX;
	float targetY = previous.targetCamY;
	float targetZoom = previous.targetZoom <= 0.0f ? 1.0f : previous.targetZoom;
	bool targetActive = false;
	float confidence = 0.0f;
	int targetTrackId = detection ? previous.targetTrackId : previous.targetTrackId;

	if (detection) {
		const float centerX = ((detection->x1 + detection->x2) * 0.5f) / frameWidth;
		const float centerY = ((detection->y1 + detection->y2) * 0.5f) / frameHeight;
		const float boxWidth = std::max(1.0f, detection->x2 - detection->x1) / frameWidth;
		const float boxHeight = std::max(1.0f, detection->y2 - detection->y1) / frameHeight;
		const float desiredSpan = std::max(boxWidth, boxHeight) * (1.0f + profile.framingMargin);

		targetX = ApplySoftDeadZone(centerX - 0.5f, deadZone);
		targetY = ApplySoftDeadZone(centerY - 0.5f, deadZone);
		targetZoom = ClampFloat(desiredSpan > 0.0f ? 0.62f / desiredSpan : 1.0f, 1.0f, effectiveMaxZoom);
		targetX = ClampCameraOffset(targetX, targetZoom);
		targetY = ClampCameraOffset(targetY, targetZoom);
		targetActive = true;
		confidence = detection->confidence;
		next.lastTargetMs = nowMs;
	} else if (previous.targetActive && nowMs - previous.lastTargetMs <= profile.holdMs) {
		targetX = previous.targetCamX;
		targetY = previous.targetCamY;
		targetZoom = previous.targetZoom;
		targetActive = true;
		confidence = previous.targetConfidence;
	} else {
		targetX = 0.0f;
		targetY = 0.0f;
		targetZoom = 1.0f;
		targetTrackId = -1;
	}

	targetZoom = ClampFloat(targetZoom, 1.0f, effectiveMaxZoom);
	targetX = ClampCameraOffset(targetX, targetZoom);
	targetY = ClampCameraOffset(targetY, targetZoom);

	if (detection && previous.targetActive && previous.lastUpdateMs > 0) {
		const float targetAlpha = MotionTargetAlpha(smoothing, deltaSeconds);
		targetX = previous.targetCamX + (targetX - previous.targetCamX) * targetAlpha;
		targetY = previous.targetCamY + (targetY - previous.targetCamY) * targetAlpha;
		targetZoom = previous.targetZoom + (targetZoom - previous.targetZoom) * targetAlpha;
		targetZoom = ClampFloat(targetZoom, 1.0f, effectiveMaxZoom);
		targetX = ClampCameraOffset(targetX, targetZoom);
		targetY = ClampCameraOffset(targetY, targetZoom);
	}

	const auto springAxis = [deltaSeconds](float current, float target, float velocity, float responsiveness,
					       float maxSpeed, float slowFactor) {
		responsiveness = ClampFloat(responsiveness, 0.05f, 1.5f);
		maxSpeed = ClampFloat(maxSpeed, 0.02f, 4.0f) * slowFactor;
		const float stiffness = 18.0f + responsiveness * 34.0f;
		const float damping = 2.0f * std::sqrt(stiffness);
		const float acceleration = (target - current) * stiffness - velocity * damping;
		const float maxAccel = maxSpeed * (7.0f + responsiveness * 9.0f);
		velocity += ClampFloat(acceleration, -maxAccel, maxAccel) * deltaSeconds;
		velocity = ClampFloat(velocity, -maxSpeed, maxSpeed);
		current += velocity * deltaSeconds;
		if (std::abs(target - current) < 0.00035f && std::abs(velocity) < 0.0015f) {
			current = target;
			velocity = 0.0f;
		}
		return std::pair<float, float>{current, velocity};
	};

	const float neutralSlowFactor = targetActive ? 1.0f : 0.35f;
	const auto pan = springAxis(previous.camX, targetX, previous.panVelocity,
				   profile.panResponsiveness * (0.65f + smoothing * 0.70f),
				   profile.maxPanSpeed, neutralSlowFactor);
	const auto tilt = springAxis(previous.camY, targetY, previous.tiltVelocity,
				    profile.tiltResponsiveness * (0.65f + smoothing * 0.70f),
				    profile.maxTiltSpeed, neutralSlowFactor);
	const float zoomSpeed = targetZoom >= previous.zoom ? profile.maxZoomSpeed : profile.maxZoomSpeed * 0.72f;
	const auto zoom = springAxis(previous.zoom, targetZoom, previous.zoomVelocity,
				    profile.zoomResponsiveness * (0.65f + smoothing * 0.70f), zoomSpeed,
				    neutralSlowFactor);

	next.targetCamX = targetX;
	next.targetCamY = targetY;
	next.targetZoom = targetZoom;
	next.camX = pan.first;
	next.camY = tilt.first;
	next.zoom = zoom.first;
	next.panVelocity = pan.second;
	next.tiltVelocity = tilt.second;
	next.zoomVelocity = zoom.second;
	next.zoom = ClampFloat(next.zoom, 1.0f, effectiveMaxZoom);
	next.camX = ClampCameraOffset(next.camX, next.zoom);
	next.camY = ClampCameraOffset(next.camY, next.zoom);
	next.targetActive = targetActive;
	next.targetConfidence = confidence;
	next.targetTrackId = targetActive ? targetTrackId : -1;
	next.lastUpdateMs = nowMs;
	return next;
}

void SwitchMotionTracker::Reset()
{
	tracks.clear();
	nextTrackId = 1;
	currentTargetTrackId = -1;
}

QVector<SwitchMotionTrack> SwitchMotionTracker::Update(const QVector<SwitchMotionDetection> &detections,
						       const SwitchMotionProfile &profile,
						       const QSize &frameSize,
						       qint64 nowMs,
						       const QString &sourceUuid)
{
	QVector<SwitchMotionDetection> highDetections;
	QVector<SwitchMotionDetection> lowDetections;
	highDetections.reserve(detections.size());
	lowDetections.reserve(detections.size());

	const float highThreshold = ClampFloat(profile.trackerHighThreshold, 0.01f, 0.99f);
	const float lowThreshold = ClampFloat(profile.trackerLowThreshold, 0.01f, highThreshold);
	const float newTrackThreshold = ClampFloat(profile.newTrackThreshold, highThreshold, 0.99f);
	for (const auto &detection : detections) {
		if (profile.targetClassId >= 0 && detection.classId != profile.targetClassId)
			continue;
		if (detection.confidence >= highThreshold)
			highDetections.push_back(detection);
		else if (detection.confidence >= lowThreshold)
			lowDetections.push_back(detection);
	}

	QVector<bool> trackMatched(tracks.size(), false);
	QVector<bool> highMatched(highDetections.size(), false);
	QVector<bool> lowMatched(lowDetections.size(), false);
	constexpr float kMatchThreshold = 0.25f;

	const auto highAssignments = SolveAssignments(tracks, highDetections, frameSize, kMatchThreshold);
	for (const auto &assignment : highAssignments) {
		if (assignment.trackIndex < 0 || assignment.trackIndex >= tracks.size() || assignment.detectionIndex < 0 ||
		    assignment.detectionIndex >= highDetections.size())
			continue;
		UpdateTrackFromDetection(&tracks[assignment.trackIndex], highDetections[assignment.detectionIndex], nowMs,
					 sourceUuid);
		trackMatched[assignment.trackIndex] = true;
		highMatched[assignment.detectionIndex] = true;
	}

	QVector<SwitchMotionTrack> unmatchedTracks;
	QVector<int> unmatchedTrackIndexes;
	for (int index = 0; index < tracks.size(); index++) {
		if (trackMatched[index])
			continue;
		unmatchedTracks.push_back(tracks[index]);
		unmatchedTrackIndexes.push_back(index);
	}

	const auto lowAssignments = SolveAssignments(unmatchedTracks, lowDetections, frameSize, kMatchThreshold);
	for (const auto &assignment : lowAssignments) {
		if (assignment.trackIndex < 0 || assignment.trackIndex >= unmatchedTrackIndexes.size() ||
		    assignment.detectionIndex < 0 || assignment.detectionIndex >= lowDetections.size())
			continue;
		const int trackIndex = unmatchedTrackIndexes[assignment.trackIndex];
		UpdateTrackFromDetection(&tracks[trackIndex], lowDetections[assignment.detectionIndex], nowMs, sourceUuid);
		tracks[trackIndex].state = QStringLiteral("recovered");
		trackMatched[trackIndex] = true;
		lowMatched[assignment.detectionIndex] = true;
	}

	for (int index = 0; index < tracks.size(); index++) {
		if (trackMatched[index])
			continue;
		tracks[index].missedFrames++;
		tracks[index].confidence = std::max(0.0f, tracks[index].confidence * 0.86f);
		tracks[index].state = QStringLiteral("lost");
	}

	for (int index = 0; index < highDetections.size(); index++) {
		if (highMatched[index] || highDetections[index].confidence < newTrackThreshold)
			continue;
		SwitchMotionTrack track;
		track.trackId = nextTrackId++;
		UpdateTrackFromDetection(&track, highDetections[index], nowMs, sourceUuid);
		tracks.push_back(track);
	}

	const int maxBuffer = std::max(1, profile.trackBufferFrames);
	const int lockedGrace = std::max(maxBuffer, profile.lockedTrackGraceFrames);
	tracks.erase(std::remove_if(tracks.begin(), tracks.end(), [&](const SwitchMotionTrack &track) {
			     const int allowedMisses = track.trackId == profile.lockedTrackId ? lockedGrace : maxBuffer;
			     return track.missedFrames > allowedMisses;
		     }),
		     tracks.end());
	std::sort(tracks.begin(), tracks.end(), [](const auto &left, const auto &right) {
		return left.trackId < right.trackId;
	});
	return tracks;
}

QVector<SwitchMotionTrack> SwitchMotionTracker::Tracks() const
{
	return tracks;
}

int SwitchMotionTracker::CycleTarget(int direction) const
{
	QVector<int> activeIds;
	for (const auto &track : tracks) {
		if (IsVisibleTrack(track))
			activeIds.push_back(track.trackId);
	}
	if (activeIds.isEmpty())
		return -1;
	std::sort(activeIds.begin(), activeIds.end());
	const int step = direction < 0 ? -1 : 1;
	const int currentIndex = activeIds.indexOf(currentTargetTrackId);
	if (currentIndex < 0)
		return step > 0 ? activeIds.front() : activeIds.back();
	const int nextIndex = (currentIndex + step + activeIds.size()) % activeIds.size();
	return activeIds[nextIndex];
}

SwitchMotionManager::SwitchMotionManager(QObject *parent)
	: QObject(parent)
{
	{
		std::lock_guard<std::mutex> lock(gRuntimeStateManagerMutex);
		gRuntimeStateManager = this;
	}
	runtimeState.modelPath = ResolveDefaultModelPath();
	runtimeState.modelAvailable = QFileInfo::exists(runtimeState.modelPath);
	runtimeState.status = runtimeState.modelAvailable ? QStringLiteral("ready") : QStringLiteral("model_missing");
	runtimeState.message = runtimeState.modelAvailable ? QString() : InstallerManagedModelMissingMessage();
	runtimeState.backend = QStringLiteral("auto");
	EnsureDefaultProfile();
	shotTimer = new QTimer(this);
	shotTimer->setInterval(33);
	connect(shotTimer, &QTimer::timeout, this, [this]() { TickMotionShots(); });
}

SwitchMotionManager::~SwitchMotionManager()
{
	std::lock_guard<std::mutex> lock(gRuntimeStateManagerMutex);
	if (gRuntimeStateManager == this)
		gRuntimeStateManager.clear();
}

obs_data_t *SwitchMotionManager::SaveState() const
{
	obs_data_t *data = obs_data_create();
	obs_data_set_int(data, "settingsVersion", kMotionStateVersion);

	obs_data_array_t *profileArray = obs_data_array_create();
	for (const auto &profile : profiles) {
		obs_data_t *entry = SwitchMotionProfileToObsData(profile);
		obs_data_array_push_back(profileArray, entry);
		obs_data_release(entry);
	}
	obs_data_set_array(data, "profiles", profileArray);
	obs_data_array_release(profileArray);

	obs_data_array_t *bindingArray = obs_data_array_create();
	for (const auto &binding : bindings) {
		obs_data_t *entry = SwitchMotionBindingToObsData(binding);
		obs_data_array_push_back(bindingArray, entry);
		obs_data_release(entry);
	}
	obs_data_set_array(data, "bindings", bindingArray);
	obs_data_array_release(bindingArray);

	obs_data_array_t *shotArray = obs_data_array_create();
	for (const auto &shot : shots) {
		obs_data_t *entry = SwitchMotionShotToObsData(shot);
		obs_data_array_push_back(shotArray, entry);
		obs_data_release(entry);
	}
	obs_data_set_array(data, "shots", shotArray);
	obs_data_array_release(shotArray);
	return data;
}

void SwitchMotionManager::LoadState(obs_data_t *data)
{
	SetBindingsQuiesced(false);
	profiles.clear();
	bindings.clear();
	RestoreAllShotTransforms();
	shots.clear();
	activeShotId.clear();

	if (data) {
		if (obs_data_array_t *profileArray = obs_data_get_array(data, "profiles")) {
			const size_t count = obs_data_array_count(profileArray);
			for (size_t index = 0; index < count; index++) {
				obs_data_t *entry = obs_data_array_item(profileArray, index);
				profiles.push_back(SwitchMotionProfileFromObsData(entry, DefaultModelPath()));
				obs_data_release(entry);
			}
			obs_data_array_release(profileArray);
		}
		if (obs_data_array_t *bindingArray = obs_data_get_array(data, "bindings")) {
			const size_t count = obs_data_array_count(bindingArray);
			for (size_t index = 0; index < count; index++) {
				obs_data_t *entry = obs_data_array_item(bindingArray, index);
				const auto binding = SwitchMotionBindingFromObsData(entry);
				if (!binding.sourceUuid.isEmpty())
					bindings.push_back(binding);
				obs_data_release(entry);
			}
			obs_data_array_release(bindingArray);
		}
		if (obs_data_array_t *shotArray = obs_data_get_array(data, "shots")) {
			const size_t count = obs_data_array_count(shotArray);
			for (size_t index = 0; index < count; index++) {
				obs_data_t *entry = obs_data_array_item(shotArray, index);
				const auto shot = SwitchMotionShotFromObsData(entry);
				if (!shot.id.isEmpty())
					shots.push_back(shot);
				obs_data_release(entry);
			}
			obs_data_array_release(shotArray);
		}
	}

	EnsureDefaultProfile();
	ApplyBindings();
	ApplyShotSamplers();
	StartShotTimer();
	emit StateChanged();
}

void SwitchMotionManager::Reset(bool detachFilters)
{
	if (detachFilters)
		DetachFilters();
	else
		SetBindingsQuiesced(false);
	profiles.clear();
	bindings.clear();
	RestoreAllShotTransforms();
	shots.clear();
	activeShotId.clear();
	{
		std::lock_guard<std::mutex> lock(runtimeStateMutex);
		runtimeState = {};
		runtimeState.modelPath = ResolveDefaultModelPath();
		runtimeState.modelAvailable = QFileInfo::exists(runtimeState.modelPath);
		runtimeState.status = runtimeState.modelAvailable ? QStringLiteral("ready") : QStringLiteral("model_missing");
		runtimeState.message = runtimeState.modelAvailable ? QString() : InstallerManagedModelMissingMessage();
		runtimeState.backend = QStringLiteral("auto");
	}
	EnsureDefaultProfile();
	StopShotTimer();
	emit StateChanged();
}

obs_data_t *SwitchMotionManager::BuildStateData() const
{
	obs_data_t *data = SaveState();
	const auto runtimeCopy = RuntimeState();
	obs_data_t *runtime = SwitchMotionRuntimeStateToObsData(runtimeCopy);
	obs_data_set_obj(data, "runtime", runtime);
	if (obs_data_array_t *tracks = obs_data_get_array(runtime, "tracks")) {
		obs_data_set_array(data, "tracks", tracks);
		obs_data_array_release(tracks);
	}
	if (obs_data_t *target = obs_data_get_obj(runtime, "target")) {
		obs_data_set_obj(data, "target", target);
		obs_data_release(target);
	}
	obs_data_release(runtime);
	obs_data_array_t *presets = SwitchMotionShotPresetsToObsArray();
	obs_data_set_array(data, "shotPresets", presets);
	obs_data_array_release(presets);
	return data;
}

QVector<SwitchMotionProfile> SwitchMotionManager::Profiles() const
{
	return profiles;
}

QVector<SwitchMotionBinding> SwitchMotionManager::Bindings() const
{
	return bindings;
}

QVector<SwitchMotionShot> SwitchMotionManager::Shots() const
{
	return shots;
}

SwitchMotionProfile SwitchMotionManager::ProfileById(const QString &profileId) const
{
	const int index = FindProfileIndex(profileId);
	return index >= 0 ? profiles[index] : SwitchMotionProfile{};
}

SwitchMotionShot SwitchMotionManager::ShotById(const QString &shotId) const
{
	const int index = FindShotIndex(shotId);
	return index >= 0 ? shots[index] : SwitchMotionShot{};
}

SwitchMotionRuntimeState SwitchMotionManager::RuntimeState() const
{
	std::lock_guard<std::mutex> lock(runtimeStateMutex);
	return runtimeState;
}

QString SwitchMotionManager::DefaultProfileId() const
{
	return profiles.isEmpty() ? QString() : profiles.front().id;
}

QString SwitchMotionManager::DefaultModelPath() const
{
	return ResolveDefaultModelPath();
}

QString SwitchMotionManager::ActiveShotId() const
{
	return activeShotId;
}

bool SwitchMotionManager::UpsertProfile(const SwitchMotionProfile &profile, QString *effectiveId)
{
	if (bindingsQuiesced)
		return false;

	SwitchMotionProfile normalized = profile;
	if (normalized.id.trimmed().isEmpty())
		normalized.id = SwitchCreateMotionId(QStringLiteral("motion-profile"));
	if (normalized.name.trimmed().isEmpty())
		normalized.name = QStringLiteral("Motion Profile");
	if (normalized.modelPath.trimmed().isEmpty())
		normalized.modelPath = DefaultModelPath();
	normalized.subjectMode = NormalizedSubjectMode(normalized.subjectMode);
	normalized.framingMode = NormalizedFramingMode(normalized.framingMode);
	normalized.confidenceThreshold = ClampFloat(normalized.confidenceThreshold, 0.01f, 0.99f);
	normalized.maxZoom = ClampFloat(normalized.maxZoom, 1.0f, 4.0f);
	normalized.framingMargin = ClampFloat(normalized.framingMargin, 0.0f, 0.75f);
	normalized.deadZone = ClampFloat(normalized.deadZone, 0.0f, 0.5f);
	normalized.smoothing = ClampFloat(normalized.smoothing, 0.01f, 1.0f);
	normalized.holdMs = std::max(0, normalized.holdMs);
	normalized.trackerHighThreshold = ClampFloat(normalized.trackerHighThreshold, 0.01f, 0.99f);
	normalized.trackerLowThreshold = ClampFloat(normalized.trackerLowThreshold, 0.01f, normalized.trackerHighThreshold);
	normalized.newTrackThreshold = ClampFloat(normalized.newTrackThreshold, normalized.trackerHighThreshold, 0.99f);
	normalized.trackBufferFrames = std::max(1, normalized.trackBufferFrames);
	normalized.lockedTrackGraceFrames = std::max(normalized.trackBufferFrames, normalized.lockedTrackGraceFrames);
	normalized.autoSwitchMs = std::max(0, normalized.autoSwitchMs);
	normalized.panResponsiveness = ClampFloat(normalized.panResponsiveness, 0.05f, 1.5f);
	normalized.tiltResponsiveness = ClampFloat(normalized.tiltResponsiveness, 0.05f, 1.5f);
	normalized.zoomResponsiveness = ClampFloat(normalized.zoomResponsiveness, 0.05f, 1.5f);
	normalized.maxPanSpeed = ClampFloat(normalized.maxPanSpeed, 0.05f, 3.0f);
	normalized.maxTiltSpeed = ClampFloat(normalized.maxTiltSpeed, 0.05f, 3.0f);
	normalized.maxZoomSpeed = ClampFloat(normalized.maxZoomSpeed, 0.05f, 4.0f);
	if (normalized.subjectMode != QStringLiteral("locked"))
		normalized.lockedTrackId = -1;

	const int index = FindProfileIndex(normalized.id);
	if (index >= 0)
		profiles[index] = normalized;
	else
		profiles.push_back(normalized);

	QSet<QString> sourcesToRefresh;
	for (const auto &binding : bindings) {
		if (binding.profileId == normalized.id)
			sourcesToRefresh.insert(binding.sourceUuid);
	}
	for (const auto &shot : shots) {
		if (shot.profileId == normalized.id && !shot.sourceUuid.isEmpty())
			sourcesToRefresh.insert(shot.sourceUuid);
	}
	for (const auto &sourceUuid : sourcesToRefresh)
		RefreshSourceFilterForUuid(sourceUuid);

	if (effectiveId)
		*effectiveId = normalized.id;
	emit ProfileChanged(normalized.id);
	emit StateChanged();
	return true;
}

bool SwitchMotionManager::DeleteProfile(const QString &profileId)
{
	if (bindingsQuiesced)
		return false;

	const int index = FindProfileIndex(profileId);
	if (index < 0 || profiles.size() <= 1)
		return false;

	profiles.removeAt(index);
	QSet<QString> sourcesToRefresh;
	for (auto &binding : bindings) {
		if (binding.profileId == profileId) {
			binding.profileId = DefaultProfileId();
			sourcesToRefresh.insert(binding.sourceUuid);
		}
	}
	for (auto &shot : shots) {
		if (shot.profileId == profileId) {
			shot.profileId = DefaultProfileId();
			if (!shot.sourceUuid.isEmpty())
				sourcesToRefresh.insert(shot.sourceUuid);
		}
	}
	for (const auto &sourceUuid : sourcesToRefresh)
		RefreshSourceFilterForUuid(sourceUuid);
	emit ProfileChanged(profileId);
	emit StateChanged();
	return true;
}

bool SwitchMotionManager::SetProfileEnabled(const QString &profileId, bool enabled)
{
	if (bindingsQuiesced)
		return false;

	const int index = FindProfileIndex(profileId);
	if (index < 0)
		return false;
	profiles[index].enabled = enabled;
	QSet<QString> sourcesToRefresh;
	for (const auto &binding : bindings) {
		if (binding.profileId == profileId)
			sourcesToRefresh.insert(binding.sourceUuid);
	}
	for (const auto &shot : shots) {
		if (shot.profileId == profileId && !shot.sourceUuid.isEmpty())
			sourcesToRefresh.insert(shot.sourceUuid);
	}
	for (const auto &sourceUuid : sourcesToRefresh)
		RefreshSourceFilterForUuid(sourceUuid);
	emit ProfileChanged(profileId);
	emit StateChanged();
	return true;
}

bool SwitchMotionManager::BindSource(const QString &sourceUuid, const QString &sourceName, const QString &profileId,
				     QString *message)
{
	if (bindingsQuiesced) {
		if (message)
			*message = QStringLiteral("Motion bindings are quiesced during OBS scene teardown");
		return false;
	}

	if (sourceUuid.trimmed().isEmpty()) {
		if (message)
			*message = QStringLiteral("sourceUuid is required");
		return false;
	}

	const SwitchMotionProfile profile = ProfileById(profileId);
	if (profile.id.isEmpty()) {
		if (message)
			*message = QStringLiteral("Motion profile not found");
		return false;
	}

	SwitchMotionBinding binding;
	binding.sourceUuid = sourceUuid;
	binding.sourceName = sourceName;
	binding.profileId = profile.id;
	binding.enabled = true;

	const int index = FindBindingIndex(sourceUuid);
	if (index >= 0)
		bindings[index] = binding;
	else
		bindings.push_back(binding);

	RefreshSourceFilterForUuid(sourceUuid);
	emit StateChanged();
	return true;
}

void SwitchMotionManager::ApplyBindings()
{
	if (bindingsQuiesced)
		return;

	for (const auto &binding : bindings) {
		if (SourceHasShot(shots, binding.sourceUuid)) {
			RefreshSourceFilterForUuid(binding.sourceUuid);
			continue;
		}
		const auto profile = ProfileById(binding.profileId);
		if (!profile.id.isEmpty())
			ApplyBinding(binding, profile, nullptr);
	}
	ApplyShotSamplers();
}

void SwitchMotionManager::DetachFilters()
{
	SetBindingsQuiesced(true);
	StopShotTimer();
	RestoreAllShotTransforms();

	const auto currentBindings = bindings;
	for (const auto &binding : currentBindings) {
		QString message;
		RemoveBindingNow(binding.sourceUuid, &message);
	}
	obs_enum_sources(RemoveMotionFilterFromEnum, nullptr);

	runtimeState.targetActive = false;
	runtimeState.targetConfidence = 0.0f;
	runtimeState.targetTrackId = -1;
	runtimeState.camX = 0.0f;
	runtimeState.camY = 0.0f;
	runtimeState.zoom = 1.0f;
	runtimeState.tracks.clear();
	runtimeState.activeTrackCount = 0;
}

void SwitchMotionManager::SetBindingsQuiesced(bool quiesced)
{
	if (bindingsQuiesced == quiesced)
		return;
	bindingsQuiesced = quiesced;
	bindingGeneration++;
}

bool SwitchMotionManager::UnbindSource(const QString &sourceUuid, QString *message)
{
	const int index = FindBindingIndex(sourceUuid);
	if (index < 0) {
		if (message)
			*message = QStringLiteral("Motion binding not found");
		return false;
	}

	const QString uuid = bindings[index].sourceUuid;
	bindings.removeAt(index);
	auto *self = this;
	QTimer::singleShot(25, self, [self, uuid]() {
		if (!self)
			return;
		self->RefreshSourceFilterForUuid(uuid);
		QString removeMessage;
		if (!RemoveBindingNow(uuid, &removeMessage) && !removeMessage.isEmpty()) {
			blog(LOG_WARNING, "[Switch Motion] %s", removeMessage.toUtf8().constData());
			self->RaiseRuntimeError(removeMessage);
		}
		self->RefreshSourceFilterForUuid(uuid);
	});
	emit StateChanged();
	return true;
}

bool SwitchMotionManager::UpsertShot(const SwitchMotionShot &shot, QString *effectiveId)
{
	if (bindingsQuiesced)
		return false;

	SwitchMotionShot normalized = shot;
	if (normalized.id.trimmed().isEmpty())
		normalized.id = SwitchCreateMotionId(QStringLiteral("motion-shot"));
	if (normalized.name.trimmed().isEmpty())
		normalized.name = QStringLiteral("Motion Shot");
	if (normalized.profileId.trimmed().isEmpty())
		normalized.profileId = DefaultProfileId();
	normalized.shotMode = NormalizedShotMode(normalized.shotMode);
	normalized.playbackMode = NormalizedPlaybackMode(normalized.playbackMode);
	normalized.easing = NormalizedEasing(normalized.easing);
	normalized.loopMode = NormalizedLoopMode(normalized.loopMode);
	normalized.durationMs = std::max(250, normalized.durationMs);
	normalized.startPanX = ClampFloat(normalized.startPanX, -0.5f, 0.5f);
	normalized.startPanY = ClampFloat(normalized.startPanY, -0.5f, 0.5f);
	normalized.endPanX = ClampFloat(normalized.endPanX, -0.5f, 0.5f);
	normalized.endPanY = ClampFloat(normalized.endPanY, -0.5f, 0.5f);
	normalized.startZoom = ClampFloat(normalized.startZoom, 1.0f, 4.0f);
	normalized.endZoom = ClampFloat(normalized.endZoom, 1.0f, 4.0f);
	normalized.maxZoom = ClampFloat(normalized.maxZoom, 1.0f, 4.0f);
	if (normalized.phaseAnchorMs <= 0)
		normalized.phaseAnchorMs = static_cast<qint64>(os_gettime_ns() / 1000000ULL);

	const int index = FindShotIndex(normalized.id);
	const QString previousSourceUuid = index >= 0 ? shots[index].sourceUuid : QString();
	if (index >= 0)
		shots[index] = normalized;
	else
		shots.push_back(normalized);

	if (effectiveId)
		*effectiveId = normalized.id;
	if (!previousSourceUuid.isEmpty() && previousSourceUuid != normalized.sourceUuid)
		RefreshSourceFilterForUuid(previousSourceUuid);
	RefreshSourceFilterForUuid(normalized.sourceUuid);
	StartShotTimer();
	emit ShotChanged(normalized.id);
	emit StateChanged();
	return true;
}

bool SwitchMotionManager::DeleteShot(const QString &shotId)
{
	if (bindingsQuiesced)
		return false;

	const int index = FindShotIndex(shotId);
	if (index < 0)
		return false;
	const auto shot = shots[index];
	const bool removedActiveShot = activeShotId == shotId;
	RestoreShotTransform(ShotTransformKey(shot));
	shots.removeAt(index);
	if (removedActiveShot)
		activeShotId.clear();
	RefreshSourceFilterForUuid(shot.sourceUuid);
	if (shots.isEmpty()) {
		StopShotTimer();
		ClearRuntimeForSource(shot.sourceUuid);
	} else if (removedActiveShot || !SourceHasShot(shots, shot.sourceUuid)) {
		ClearRuntimeForSource(shot.sourceUuid);
	}
	emit ShotChanged(shotId);
	emit StateChanged();
	return true;
}

bool SwitchMotionManager::SetShotEnabled(const QString &shotId, bool enabled)
{
	const int index = FindShotIndex(shotId);
	if (index < 0)
		return false;
	shots[index].enabled = enabled;
	const auto shot = shots[index];
	if (!enabled)
		RestoreShotTransform(ShotTransformKey(shot));
	RefreshSourceFilterForUuid(shot.sourceUuid);
	if (enabled)
		StartShotTimer();
	else if (!SourceHasShot(shots, shot.sourceUuid))
		ClearRuntimeForSource(shot.sourceUuid);
	if (!std::any_of(shots.cbegin(), shots.cend(), [](const SwitchMotionShot &candidate) { return candidate.enabled; }))
		StopShotTimer();
	emit ShotChanged(shotId);
	emit StateChanged();
	return true;
}

bool SwitchMotionManager::BindSceneItem(const SwitchMotionShot &shot, QString *effectiveId, QString *message)
{
	if (shot.sceneUuid.trimmed().isEmpty() || shot.sceneItemId < 0) {
		if (message)
			*message = QStringLiteral("sceneUuid and sceneItemId are required");
		return false;
	}
	if (shot.sourceUuid.trimmed().isEmpty()) {
		if (message)
			*message = QStringLiteral("sourceUuid is required");
		return false;
	}
	if (!shot.profileId.isEmpty() && ProfileById(shot.profileId).id.isEmpty()) {
		if (message)
			*message = QStringLiteral("Motion profile not found");
		return false;
	}
	return UpsertShot(shot, effectiveId);
}

bool SwitchMotionManager::SetShotPlayback(const QString &shotId, const QString &playbackMode)
{
	const int index = FindShotIndex(shotId);
	if (index < 0)
		return false;
	shots[index].playbackMode = NormalizedPlaybackMode(playbackMode);
	if (shots[index].playbackMode == QStringLiteral("restart_on_program"))
		shots[index].phaseAnchorMs = static_cast<qint64>(os_gettime_ns() / 1000000ULL);
	emit ShotChanged(shotId);
	emit StateChanged();
	return true;
}

void SwitchMotionManager::HandleSourceRemoved(obs_source_t *source)
{
	if (!source)
		return;
	const char *uuid = obs_source_get_uuid(source);
	if (!uuid || !*uuid)
		return;
	const int index = FindBindingIndex(QString::fromUtf8(uuid));
	if (index >= 0) {
		bindings.removeAt(index);
		emit StateChanged();
	}
	for (int shotIndex = shots.size() - 1; shotIndex >= 0; --shotIndex) {
		if (shots[shotIndex].sourceUuid == QString::fromUtf8(uuid)) {
			RestoreShotTransform(ShotTransformKey(shots[shotIndex]));
			shots.removeAt(shotIndex);
			emit StateChanged();
		}
	}
}

void SwitchMotionManager::SetRuntimeState(const SwitchMotionRuntimeState &state)
{
	SwitchMotionRuntimeState previous;
	SwitchMotionRuntimeState next;
	{
		std::lock_guard<std::mutex> lock(runtimeStateMutex);
		previous = runtimeState;
		runtimeState = state;
		runtimeState.activeShotId = previous.activeShotId;
		runtimeState.activeShotName = previous.activeShotName;
		runtimeState.activeSceneName = previous.activeSceneName;
		runtimeState.activeShotMode = previous.activeShotMode;
		runtimeState.activeShotPlaybackMode = previous.activeShotPlaybackMode;
		runtimeState.activeShotPhaseMs = previous.activeShotPhaseMs;
		runtimeState.activeShotCamX = previous.activeShotCamX;
		runtimeState.activeShotCamY = previous.activeShotCamY;
		runtimeState.activeShotZoom = previous.activeShotZoom;
		next = runtimeState;
	}
	EmitTargetIfChanged(previous, next);
	const bool tracksChanged = previous.tracks.size() != next.tracks.size() ||
				   previous.activeTrackCount != next.activeTrackCount;
	if (tracksChanged)
		emit TracksChanged();
	const bool stateChanged = previous.status != next.status || previous.backend != next.backend ||
				  previous.providerStatus != next.providerStatus ||
				  previous.modelPath != next.modelPath ||
				  previous.message != next.message ||
				  previous.targetActive != next.targetActive ||
				  previous.targetTrackId != next.targetTrackId ||
				  previous.activeShotId != next.activeShotId ||
				  previous.activeShotMode != next.activeShotMode ||
				  previous.activeShotPlaybackMode != next.activeShotPlaybackMode ||
				  previous.activeShotPhaseMs != next.activeShotPhaseMs || tracksChanged;
	if (stateChanged)
		emit StateChanged();
}

void SwitchMotionManager::RaiseRuntimeError(const QString &message)
{
	{
		std::lock_guard<std::mutex> lock(runtimeStateMutex);
		runtimeState.status = QStringLiteral("error");
		runtimeState.message = message;
	}
	emit RuntimeError(message);
	emit StateChanged();
}

void SwitchMotionManager::SyncProfileFromFilter(const SwitchMotionProfile &profile, const QString &sourceUuid)
{
	if (bindingsQuiesced || profile.id.trimmed().isEmpty())
		return;

	if (!sourceUuid.isEmpty() && SourceHasShot(shots, sourceUuid) && !SourceHasAiShot(shots, sourceUuid) &&
	    !profile.enabled) {
		return;
	}

	SwitchMotionProfile normalized = profile;
	if (normalized.name.trimmed().isEmpty())
		normalized.name = QStringLiteral("Motion Profile");
	if (normalized.modelPath.trimmed().isEmpty())
		normalized.modelPath = DefaultModelPath();
	normalized.subjectMode = NormalizedSubjectMode(normalized.subjectMode);
	normalized.framingMode = NormalizedFramingMode(normalized.framingMode);
	normalized.confidenceThreshold = ClampFloat(normalized.confidenceThreshold, 0.01f, 0.99f);
	normalized.maxZoom = ClampFloat(normalized.maxZoom, 1.0f, 4.0f);
	normalized.framingMargin = ClampFloat(normalized.framingMargin, 0.0f, 0.75f);
	normalized.deadZone = ClampFloat(normalized.deadZone, 0.0f, 0.5f);
	normalized.smoothing = ClampFloat(normalized.smoothing, 0.01f, 1.0f);
	normalized.holdMs = std::max(0, normalized.holdMs);
	normalized.trackerHighThreshold = ClampFloat(normalized.trackerHighThreshold, 0.01f, 0.99f);
	normalized.trackerLowThreshold = ClampFloat(normalized.trackerLowThreshold, 0.01f, normalized.trackerHighThreshold);
	normalized.newTrackThreshold = ClampFloat(normalized.newTrackThreshold, normalized.trackerHighThreshold, 0.99f);
	normalized.trackBufferFrames = std::max(1, normalized.trackBufferFrames);
	normalized.lockedTrackGraceFrames = std::max(normalized.trackBufferFrames, normalized.lockedTrackGraceFrames);
	normalized.autoSwitchMs = std::max(0, normalized.autoSwitchMs);
	normalized.panResponsiveness = ClampFloat(normalized.panResponsiveness, 0.05f, 1.5f);
	normalized.tiltResponsiveness = ClampFloat(normalized.tiltResponsiveness, 0.05f, 1.5f);
	normalized.zoomResponsiveness = ClampFloat(normalized.zoomResponsiveness, 0.05f, 1.5f);
	normalized.maxPanSpeed = ClampFloat(normalized.maxPanSpeed, 0.05f, 3.0f);
	normalized.maxTiltSpeed = ClampFloat(normalized.maxTiltSpeed, 0.05f, 3.0f);
	normalized.maxZoomSpeed = ClampFloat(normalized.maxZoomSpeed, 0.05f, 4.0f);
	if (normalized.subjectMode != QStringLiteral("locked"))
		normalized.lockedTrackId = -1;

	const int index = FindProfileIndex(normalized.id);
	if (index >= 0 && ProfilesEquivalent(profiles[index], normalized))
		return;

	if (index >= 0)
		profiles[index] = normalized;
	else
		profiles.push_back(normalized);

	QSet<QString> sourcesToRefresh;
	for (const auto &binding : bindings) {
		if (binding.profileId == normalized.id && binding.sourceUuid != sourceUuid)
			sourcesToRefresh.insert(binding.sourceUuid);
	}
	for (const auto &shot : shots) {
		if (shot.profileId == normalized.id && !shot.sourceUuid.isEmpty() && shot.sourceUuid != sourceUuid)
			sourcesToRefresh.insert(shot.sourceUuid);
	}
	for (const auto &uuid : sourcesToRefresh)
		RefreshSourceFilterForUuid(uuid);

	emit ProfileChanged(normalized.id);
	emit StateChanged();
}

int SwitchMotionManager::FindProfileIndex(const QString &profileId) const
{
	for (int index = 0; index < profiles.size(); index++) {
		if (profiles[index].id == profileId)
			return index;
	}
	return -1;
}

int SwitchMotionManager::FindBindingIndex(const QString &sourceUuid) const
{
	for (int index = 0; index < bindings.size(); index++) {
		if (bindings[index].sourceUuid == sourceUuid)
			return index;
	}
	return -1;
}

int SwitchMotionManager::FindShotIndex(const QString &shotId) const
{
	for (int index = 0; index < shots.size(); index++) {
		if (shots[index].id == shotId)
			return index;
	}
	return -1;
}

bool SwitchMotionManager::ApplyBinding(const SwitchMotionBinding &binding, const SwitchMotionProfile &profile,
				       QString *message) const
{
	if (bindingsQuiesced) {
		if (message)
			*message = QStringLiteral("Motion bindings are quiesced during OBS scene teardown");
		return false;
	}

	obs_source_t *source = obs_get_source_by_uuid(binding.sourceUuid.toUtf8().constData());
	if (!source) {
		if (message)
			*message = QStringLiteral("Unable to find OBS source for Motion binding");
		return false;
	}
	obs_source_release(source);

	auto *self = const_cast<SwitchMotionManager *>(this);
	const uint64_t generation = bindingGeneration;
	QTimer::singleShot(25, self, [self, binding, profile, generation]() {
		if (!self || self->bindingsQuiesced || self->bindingGeneration != generation)
			return;
		if (SourceHasShot(self->shots, binding.sourceUuid)) {
			self->RefreshSourceFilterForUuid(binding.sourceUuid);
			return;
		}

		QString applyMessage;
		if (!ApplyBindingNow(binding, profile, true, &applyMessage)) {
			if (applyMessage.isEmpty())
				applyMessage = QStringLiteral("Unable to apply Switch Motion filter");
			blog(LOG_WARNING, "[Switch Motion] %s", applyMessage.toUtf8().constData());
			if (self)
				self->RaiseRuntimeError(applyMessage);
		}
	});
	return true;
}

void SwitchMotionManager::ApplyShotSamplers()
{
	for (const auto &shot : shots) {
		if (shot.sourceUuid.isEmpty() || !shot.enabled)
			continue;
		RefreshSourceFilterForUuid(shot.sourceUuid);
	}
}

void SwitchMotionManager::RefreshSourceFilterForUuid(const QString &sourceUuid)
{
	if (sourceUuid.trimmed().isEmpty() || bindingsQuiesced)
		return;

	const SwitchMotionShot *fallbackShot = nullptr;
	for (const auto &shot : shots) {
		if (!shot.enabled || shot.sourceUuid != sourceUuid)
			continue;
		if (!fallbackShot)
			fallbackShot = &shot;
		if (!ShotUsesAiRuntime(shot))
			continue;
		auto profile = ProfileById(shot.profileId);
		if (profile.id.isEmpty())
			profile = SwitchDefaultMotionProfile(DefaultModelPath());
		profile.enabled = true;
		if (profile.modelPath.trimmed().isEmpty())
			profile.modelPath = DefaultModelPath();

		SwitchMotionBinding sampler;
		sampler.sourceUuid = shot.sourceUuid;
		sampler.sourceName = shot.sourceName;
		sampler.profileId = profile.id;
		sampler.enabled = true;
		ApplyBindingNow(sampler, profile, false, nullptr);
		return;
	}

	if (fallbackShot) {
		auto profile = ProfileById(fallbackShot->profileId);
		if (profile.id.isEmpty())
			profile = SwitchDefaultMotionProfile(DefaultModelPath());
		profile.enabled = false;
		if (profile.modelPath.trimmed().isEmpty())
			profile.modelPath = DefaultModelPath();

		SwitchMotionBinding sampler;
		sampler.sourceUuid = fallbackShot->sourceUuid;
		sampler.sourceName = fallbackShot->sourceName;
		sampler.profileId = profile.id;
		sampler.enabled = true;
		ApplyBindingNow(sampler, profile, false, nullptr);
		return;
	}

	const int bindingIndex = FindBindingIndex(sourceUuid);
	if (bindingIndex >= 0) {
		const auto profile = ProfileById(bindings[bindingIndex].profileId);
		if (!profile.id.isEmpty()) {
			const bool transformEnabled = !SourceHasShot(shots, sourceUuid);
			ApplyBindingNow(bindings[bindingIndex], profile, transformEnabled, nullptr);
			return;
		}
	}

	RemoveBindingNow(sourceUuid, nullptr);
}

void SwitchMotionManager::StartShotTimer()
{
	if (!shotTimer || bindingsQuiesced)
		return;
	bool hasEnabledShot = false;
	for (const auto &shot : shots) {
		if (shot.enabled) {
			hasEnabledShot = true;
			break;
		}
	}
	if (hasEnabledShot && !shotTimer->isActive())
		shotTimer->start();
}

void SwitchMotionManager::StopShotTimer()
{
	if (shotTimer && shotTimer->isActive())
		shotTimer->stop();
}

void SwitchMotionManager::RestoreAllShotTransforms()
{
	const auto keys = shotBaseTransforms.keys();
	for (const auto &key : keys)
		RestoreShotTransform(key);
	shotBaseTransforms.clear();
	activeShotId.clear();
}

void SwitchMotionManager::RestoreShotTransform(const QString &shotKey)
{
	if (!shotBaseTransforms.contains(shotKey))
		return;
	const auto parts = shotKey.split(QLatin1Char(':'));
	if (parts.size() != 2) {
		shotBaseTransforms.remove(shotKey);
		return;
	}
	const QString sceneUuid = parts[0];
	bool ok = false;
	const int64_t itemId = parts[1].toLongLong(&ok);
	if (!ok) {
		shotBaseTransforms.remove(shotKey);
		return;
	}

	obs_source_t *sceneSource = obs_get_source_by_uuid(sceneUuid.toUtf8().constData());
	if (!sceneSource) {
		shotBaseTransforms.remove(shotKey);
		return;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	obs_sceneitem_t *item = scene ? obs_scene_find_sceneitem_by_id(scene, itemId) : nullptr;
	if (item)
		obs_sceneitem_set_info2(item, &shotBaseTransforms[shotKey]);
	obs_source_release(sceneSource);
	shotBaseTransforms.remove(shotKey);
}

void SwitchMotionManager::ClearRuntimeForSource(const QString &sourceUuid)
{
	SwitchMotionRuntimeState previous;
	SwitchMotionRuntimeState next;
	bool shouldEmit = false;
	{
		std::lock_guard<std::mutex> lock(runtimeStateMutex);
		if (!sourceUuid.isEmpty() && !runtimeState.sourceUuid.isEmpty() && runtimeState.sourceUuid != sourceUuid)
			return;

		previous = runtimeState;
		runtimeState.sourceUuid.clear();
		runtimeState.status = runtimeState.modelAvailable ? QStringLiteral("ready") : QStringLiteral("model_missing");
		runtimeState.message = runtimeState.modelAvailable ? QString() : InstallerManagedModelMissingMessage();
		runtimeState.targetActive = false;
		runtimeState.targetConfidence = 0.0f;
		runtimeState.targetTrackId = -1;
		runtimeState.targetState.clear();
		runtimeState.camX = 0.0f;
		runtimeState.camY = 0.0f;
		runtimeState.zoom = 1.0f;
		runtimeState.tracks.clear();
		runtimeState.activeTrackCount = 0;
		runtimeState.activeShotId.clear();
		runtimeState.activeShotName.clear();
		runtimeState.activeShotMode.clear();
		runtimeState.activeShotPlaybackMode.clear();
		runtimeState.activeShotPhaseMs = 0;
		runtimeState.activeShotCamX = 0.0f;
		runtimeState.activeShotCamY = 0.0f;
		runtimeState.activeShotZoom = 1.0f;
		next = runtimeState;
		shouldEmit = previous.targetActive || !previous.sourceUuid.isEmpty() || !previous.activeShotId.isEmpty() ||
			     !previous.tracks.isEmpty() || previous.activeTrackCount != 0;
	}

	if (shouldEmit) {
		EmitTargetIfChanged(previous, next);
		emit RuntimeStatsChanged();
	}
}

bool SwitchMotionManager::ApplyShotTransform(const SwitchMotionShot &shot, const SwitchMotionCameraState &state,
					    QString *message)
{
	if (shot.sceneUuid.isEmpty() || shot.sceneItemId < 0) {
		if (message)
			*message = QStringLiteral("Motion shot is missing a scene item binding");
		return false;
	}

	obs_source_t *sceneSource = obs_get_source_by_uuid(shot.sceneUuid.toUtf8().constData());
	if (!sceneSource) {
		if (message)
			*message = QStringLiteral("Motion shot scene is not available");
		return false;
	}
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	obs_sceneitem_t *item = scene ? obs_scene_find_sceneitem_by_id(scene, shot.sceneItemId) : nullptr;
	if (!item) {
		obs_source_release(sceneSource);
		if (message)
			*message = QStringLiteral("Motion shot scene item is not available");
		return false;
	}

	const QString key = ShotTransformKey(shot);
	if (!shotBaseTransforms.contains(key)) {
		obs_transform_info base = {};
		obs_sceneitem_get_info2(item, &base);
		shotBaseTransforms.insert(key, base);
	}
	const auto base = shotBaseTransforms.value(key);
	auto next = base;

	obs_source_t *itemSource = obs_sceneitem_get_source(item);
	if (!SourceAllowsContinuousSceneItemTransform(itemSource)) {
		obs_source_release(sceneSource);
		return true;
	}
	const float sourceWidth = static_cast<float>(std::max<uint32_t>(1, itemSource ? obs_source_get_width(itemSource) : 1));
	const float sourceHeight = static_cast<float>(std::max<uint32_t>(1, itemSource ? obs_source_get_height(itemSource) : 1));
	const float zoom = std::max(1.0f, state.zoom);
	const float camX = ClampCameraOffset(state.camX, zoom);
	const float camY = ClampCameraOffset(state.camY, zoom);

	const auto alignmentFraction = [](uint32_t alignment, bool horizontal) {
		if (horizontal) {
			if (alignment & OBS_ALIGN_RIGHT)
				return 1.0f;
			if (alignment & OBS_ALIGN_LEFT)
				return 0.0f;
			return 0.5f;
		}
		if (alignment & OBS_ALIGN_BOTTOM)
			return 1.0f;
		if (alignment & OBS_ALIGN_TOP)
			return 0.0f;
		return 0.5f;
	};

	const float anchorX = alignmentFraction(base.alignment, true);
	const float anchorY = alignmentFraction(base.alignment, false);
	const float localDeltaX = sourceWidth * base.scale.x *
				  ((0.5f - anchorX) - zoom * (0.5f + camX - anchorX));
	const float localDeltaY = sourceHeight * base.scale.y *
				  ((0.5f - anchorY) - zoom * (0.5f + camY - anchorY));
	const float radians = base.rot * 0.017453292519943295f;
	const float cosValue = std::cos(radians);
	const float sinValue = std::sin(radians);

	next.scale.x = base.scale.x * zoom;
	next.scale.y = base.scale.y * zoom;
	next.pos.x = base.pos.x + cosValue * localDeltaX - sinValue * localDeltaY;
	next.pos.y = base.pos.y + sinValue * localDeltaX + cosValue * localDeltaY;
	obs_sceneitem_set_info2(item, &next);
	obs_source_release(sceneSource);
	return true;
}

SwitchMotionShot *SwitchMotionManager::ShotForSceneUuid(const QString &sceneUuid)
{
	if (sceneUuid.isEmpty())
		return nullptr;
	for (auto &shot : shots) {
		if (shot.enabled && shot.sceneUuid == sceneUuid)
			return &shot;
	}
	return nullptr;
}

QString SwitchMotionManager::CurrentProgramSceneUuid(QString *sceneName) const
{
	obs_source_t *sceneSource = obs_frontend_get_current_scene();
	if (!sceneSource)
		return {};
	if (sceneName)
		*sceneName = QString::fromUtf8(obs_source_get_name(sceneSource));
	const char *uuid = obs_source_get_uuid(sceneSource);
	const QString result = uuid ? QString::fromUtf8(uuid) : QString();
	obs_source_release(sceneSource);
	return result;
}

QString SwitchMotionManager::CurrentPreviewSceneUuid() const
{
	obs_source_t *sceneSource = obs_frontend_get_current_preview_scene();
	if (!sceneSource)
		return {};
	const char *uuid = obs_source_get_uuid(sceneSource);
	const QString result = uuid ? QString::fromUtf8(uuid) : QString();
	obs_source_release(sceneSource);
	return result;
}

void SwitchMotionManager::TickMotionShots()
{
	if (bindingsQuiesced || shots.isEmpty())
		return;

	const qint64 nowMs = static_cast<qint64>(os_gettime_ns() / 1000000ULL);
	QString sceneName;
	const QString sceneUuid = CurrentProgramSceneUuid(&sceneName);
	if (sceneUuid != lastObservedProgramSceneUuid) {
		lastObservedProgramSceneUuid = sceneUuid;
		shotSceneSettlingUntilMs = nowMs + 500;
		return;
	}

	if (shotSceneSettlingUntilMs > nowMs)
		return;
	shotSceneSettlingUntilMs = 0;

	SwitchMotionShot *shot = ShotForSceneUuid(sceneUuid);
	const QString nextActiveShotId = shot ? shot->id : QString();

	if (activeShotId != nextActiveShotId) {
		if (!activeShotId.isEmpty()) {
			const int previousIndex = FindShotIndex(activeShotId);
			if (previousIndex >= 0) {
				if (shots[previousIndex].playbackMode == QStringLiteral("pause_when_hidden"))
					shots[previousIndex].pausedPhaseMs = SwitchMotionShotPhaseMs(shots[previousIndex], nowMs);
				emit ShotChanged(shots[previousIndex].id);
			}
		}

		activeShotId = nextActiveShotId;
		if (shot) {
			if (shot->playbackMode == QStringLiteral("restart_on_program")) {
				shot->phaseAnchorMs = nowMs;
				shot->pausedPhaseMs = 0;
			} else if (shot->playbackMode == QStringLiteral("pause_when_hidden")) {
				shot->phaseAnchorMs = nowMs - std::clamp<qint64>(shot->pausedPhaseMs, 0, shot->durationMs);
			} else if (shot->phaseAnchorMs <= 0) {
				shot->phaseAnchorMs = nowMs;
			}
			shot->lastProgramMs = nowMs;
			emit ShotChanged(shot->id);
		}
		emit ActiveShotChanged(activeShotId);
	}

	if (!shot) {
		std::lock_guard<std::mutex> lock(runtimeStateMutex);
		runtimeState.activeShotId.clear();
		runtimeState.activeShotName.clear();
		runtimeState.activeSceneName = sceneName;
		runtimeState.activeShotMode.clear();
		runtimeState.activeShotPlaybackMode.clear();
		runtimeState.activeShotPhaseMs = 0;
		runtimeState.activeShotCamX = 0.0f;
		runtimeState.activeShotCamY = 0.0f;
		runtimeState.activeShotZoom = 1.0f;
		return;
	}

	auto profile = ProfileById(shot->profileId);
	if (profile.id.isEmpty())
		profile = SwitchDefaultMotionProfile(DefaultModelPath());
	if (profile.modelPath.trimmed().isEmpty())
		profile.modelPath = DefaultModelPath();
	if (shot->maxZoom > 0.0f)
		profile.maxZoom = std::max(profile.maxZoom, shot->maxZoom);

	const auto runtimeCopy = RuntimeState();
	SwitchMotionCameraState aiState;
	if (runtimeCopy.sourceUuid == shot->sourceUuid) {
		aiState.camX = runtimeCopy.camX;
		aiState.camY = runtimeCopy.camY;
		aiState.zoom = runtimeCopy.zoom <= 0.0f ? 1.0f : runtimeCopy.zoom;
		aiState.targetActive = runtimeCopy.targetActive;
		aiState.targetConfidence = runtimeCopy.targetConfidence;
		aiState.targetTrackId = runtimeCopy.targetTrackId;
	} else {
		aiState.zoom = 1.0f;
	}

	const auto camera = SwitchComposeMotionCameraState(profile, aiState, *shot, nowMs);
	QString message;
	if (!ApplyShotTransform(*shot, camera, &message)) {
		if (!message.isEmpty()) {
			RaiseRuntimeError(message);
			emit ShotRuntimeError(message);
		}
		return;
	}

	const qint64 phaseMs = SwitchMotionShotPhaseMs(*shot, nowMs);
	bool shouldEmitRuntime = false;
	{
		std::lock_guard<std::mutex> lock(runtimeStateMutex);
		runtimeState.activeShotId = shot->id;
		runtimeState.activeShotName = shot->name;
		runtimeState.activeSceneName = sceneName;
		runtimeState.activeShotMode = shot->shotMode;
		runtimeState.activeShotPlaybackMode = shot->playbackMode;
		runtimeState.activeShotPhaseMs = phaseMs;
		runtimeState.activeShotCamX = camera.camX;
		runtimeState.activeShotCamY = camera.camY;
		runtimeState.activeShotZoom = camera.zoom;
		if (nowMs - lastShotRuntimeEmitMs >= 250) {
			lastShotRuntimeEmitMs = nowMs;
			shouldEmitRuntime = true;
		}
	}
	if (shouldEmitRuntime)
		emit RuntimeStatsChanged();
}

void SwitchMotionManager::HandleFrontendEvent(enum obs_frontend_event event)
{
	if (bindingsQuiesced)
		return;
	const qint64 nowMs = static_cast<qint64>(os_gettime_ns() / 1000000ULL);
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		ApplyBindings();
		StartShotTimer();
		break;
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
		if (obs_frontend_preview_program_mode_active()) {
			const QString previewSceneUuid = CurrentPreviewSceneUuid();
			for (auto &shot : shots) {
				if (shot.enabled && shot.sceneUuid == previewSceneUuid &&
				    shot.playbackMode == QStringLiteral("cue_in_preview")) {
					shot.phaseAnchorMs = nowMs - std::clamp<qint64>(shot.pausedPhaseMs, 0, shot.durationMs);
					emit ShotChanged(shot.id);
				}
			}
		}
		TickMotionShots();
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_TRANSITION_STOPPED:
		shotSceneSettlingUntilMs = nowMs + 500;
		StartShotTimer();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
	case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
	case OBS_FRONTEND_EVENT_EXIT:
		StopShotTimer();
		RestoreAllShotTransforms();
		break;
	default:
		break;
	}
}

QString SwitchMotionManager::ResolveDefaultModelPath() const
{
	if (obs_module_t *module = obs_get_module("switch")) {
		const char *dataPath = obs_get_module_data_path(module);
		if (dataPath) {
			const QString modelPath = QStringLiteral("%1/models/yolo26-nano.onnx").arg(QString::fromUtf8(dataPath));
			if (QFileInfo::exists(modelPath))
				return modelPath;
		}
	}

	return QString();
}

void SwitchMotionManager::EnsureDefaultProfile()
{
	if (!profiles.isEmpty())
		return;
	profiles.push_back(SwitchDefaultMotionProfile(DefaultModelPath()));
}

void SwitchMotionManager::EmitTargetIfChanged(const SwitchMotionRuntimeState &previous,
					      const SwitchMotionRuntimeState &next)
{
	if (previous.targetActive != next.targetActive ||
	    previous.targetTrackId != next.targetTrackId ||
	    previous.sourceUuid != next.sourceUuid) {
		emit TargetChanged(next.targetActive, next.targetConfidence, next.targetTrackId, next.sourceUuid);
		if (previous.targetActive && !next.targetActive)
			emit TargetLost(previous.targetTrackId, previous.sourceUuid);
		if (!previous.targetActive && next.targetActive)
			emit TargetReacquired(next.targetTrackId, next.sourceUuid);
	}
}
