#include "switch-ai-tracker.hpp"

#include <coreml_provider_factory.h>
#include <onnxruntime_cxx_api.h>
#include <obs-properties.h>
#include <util/platform.h>

#include <QFileInfo>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {
constexpr auto kProfileId = "profile_id";
constexpr auto kMotionEnabled = "motion_enabled";
constexpr auto kTransformEnabled = "transform_enabled";
constexpr auto kTransformSettingsVersion = "transform_settings_version";
constexpr int kCurrentTransformSettingsVersion = 2;
constexpr auto kConfidenceThreshold = "confidence_threshold";
constexpr auto kTargetClassId = "target_class_id";
constexpr auto kMaxZoom = "max_zoom";
constexpr auto kFramingMargin = "framing_margin";
constexpr auto kDeadZone = "dead_zone";
constexpr auto kSmoothing = "smoothing";
constexpr auto kHoldMs = "hold_ms";
constexpr auto kBackend = "backend";
constexpr auto kModelPath = "model_path";
constexpr auto kSubjectMode = "subject_mode";
constexpr auto kFramingMode = "framing_mode";
constexpr auto kTrackerHighThreshold = "tracker_high_threshold";
constexpr auto kTrackerLowThreshold = "tracker_low_threshold";
constexpr auto kNewTrackThreshold = "new_track_threshold";
constexpr auto kTrackBufferFrames = "track_buffer_frames";
constexpr auto kLockedTrackGraceFrames = "locked_track_grace_frames";
constexpr auto kAutoSwitchMs = "auto_switch_ms";
constexpr auto kPanResponsiveness = "pan_responsiveness";
constexpr auto kTiltResponsiveness = "tilt_responsiveness";
constexpr auto kZoomResponsiveness = "zoom_responsiveness";
constexpr auto kMaxPanSpeed = "max_pan_speed";
constexpr auto kMaxTiltSpeed = "max_tilt_speed";
constexpr auto kMaxZoomSpeed = "max_zoom_speed";
constexpr auto kDebugOverlay = "debug_overlay";
constexpr auto kLockedTrackId = "locked_track_id";
constexpr int kInputWidth = 640;
constexpr int kInputHeight = 640;
constexpr uint64_t kInferenceIntervalNs = 100000000ULL;
constexpr uint64_t kFrameLogIntervalNs = 2000000000ULL;
constexpr uint64_t kDetectionLogIntervalNs = 2000000000ULL;
constexpr uint64_t kRuntimePublishIntervalNs = 250000000ULL;
constexpr uint64_t kInferenceWatchdogTimeoutNs = 1500000000ULL;
constexpr uint64_t kInferenceWatchdogLogIntervalNs = 5000000000ULL;
std::atomic_bool gFrontendQuiescing{false};
std::mutex gFilterRegistryMutex;
std::vector<tracker_filter_data *> gFilterRegistry;

QString NormalizeMotionBackend(QString backend)
{
	backend = backend.trimmed().toLower();
	if (backend == QStringLiteral("auto") || backend == QStringLiteral("cpu"))
		return backend;
#if defined(__APPLE__)
	if (backend == QStringLiteral("coreml"))
		return backend;
#elif defined(_WIN32)
	if (backend == QStringLiteral("directml"))
		return backend;
#endif
	return QStringLiteral("auto");
}

QString NormalizeSubjectMode(QString mode)
{
	mode = mode.trimmed().toLower();
	if (mode == QStringLiteral("auto") || mode == QStringLiteral("locked") ||
	    mode == QStringLiteral("hold") || mode == QStringLiteral("off"))
		return mode;
	return QStringLiteral("auto");
}

QString NormalizeFramingMode(QString mode)
{
	mode = mode.trimmed().toLower();
	if (mode == QStringLiteral("face_headroom") || mode == QStringLiteral("upper_body") ||
	    mode == QStringLiteral("full_body") || mode == QStringLiteral("group"))
		return mode;
	return QStringLiteral("upper_body");
}

float MaxCameraOffsetForZoom(float zoom)
{
	zoom = std::max(1.0f, zoom);
	return (zoom - 1.0f) / (2.0f * zoom);
}

float ClampCameraOffset(float offset, float zoom)
{
	const float maximum = MaxCameraOffsetForZoom(zoom);
	return std::clamp(offset, -maximum, maximum);
}

float MotionDetectionArea(const SwitchMotionDetection &detection)
{
	return std::max(0.0f, detection.x2 - detection.x1) * std::max(0.0f, detection.y2 - detection.y1);
}

float MotionDetectionCenterX(const SwitchMotionDetection &detection)
{
	return (detection.x1 + detection.x2) * 0.5f;
}

float MotionDetectionCenterY(const SwitchMotionDetection &detection)
{
	return (detection.y1 + detection.y2) * 0.5f;
}

float MotionDetectionIou(const SwitchMotionDetection &left, const SwitchMotionDetection &right)
{
	const float x1 = std::max(left.x1, right.x1);
	const float y1 = std::max(left.y1, right.y1);
	const float x2 = std::min(left.x2, right.x2);
	const float y2 = std::min(left.y2, right.y2);
	const float intersection = std::max(0.0f, x2 - x1) * std::max(0.0f, y2 - y1);
	if (intersection <= 0.0f)
		return 0.0f;
	const float unionArea = MotionDetectionArea(left) + MotionDetectionArea(right) - intersection;
	return unionArea > 0.0f ? intersection / unionArea : 0.0f;
}

bool MotionDetectionsOverlapAsDuplicate(const SwitchMotionDetection &left,
					 const SwitchMotionDetection &right,
					 const SwitchMotionFrameSample &sample)
{
	if (left.classId != right.classId)
		return false;
	if (MotionDetectionIou(left, right) >= 0.68f)
		return true;

	const float frameWidth = static_cast<float>(std::max<uint32_t>(1, sample.width));
	const float frameHeight = static_cast<float>(std::max<uint32_t>(1, sample.height));
	const float dx = (MotionDetectionCenterX(left) - MotionDetectionCenterX(right)) / frameWidth;
	const float dy = (MotionDetectionCenterY(left) - MotionDetectionCenterY(right)) / frameHeight;
	const float centerDistance = std::sqrt(dx * dx + dy * dy);
	const float leftArea = MotionDetectionArea(left);
	const float rightArea = MotionDetectionArea(right);
	const float areaRatio = leftArea > 0.0f && rightArea > 0.0f
					? std::min(leftArea, rightArea) / std::max(leftArea, rightArea)
					: 0.0f;
	return centerDistance <= 0.045f && areaRatio >= 0.48f;
}

QVector<SwitchMotionDetection> SuppressDuplicateDetections(QVector<SwitchMotionDetection> detections,
							   const SwitchMotionFrameSample &sample)
{
	std::sort(detections.begin(), detections.end(), [](const auto &left, const auto &right) {
		if (left.confidence != right.confidence)
			return left.confidence > right.confidence;
		return MotionDetectionArea(left) > MotionDetectionArea(right);
	});

	QVector<SwitchMotionDetection> result;
	result.reserve(detections.size());
	for (const auto &detection : detections) {
		bool duplicate = false;
		for (const auto &accepted : result) {
			if (MotionDetectionsOverlapAsDuplicate(detection, accepted, sample)) {
				duplicate = true;
				break;
			}
		}
		if (!duplicate)
			result.push_back(detection);
	}
	return result;
}

void RenderMotionFilterTarget(obs_source_t *target, obs_source_t *parent)
{
	if (!target)
		return;

	if (target == parent) {
		const uint32_t flags = obs_source_get_output_flags(target);
		if ((flags & OBS_SOURCE_CUSTOM_DRAW) == 0 && (flags & OBS_SOURCE_ASYNC) == 0) {
			obs_source_default_render(target);
			return;
		}
	}

	obs_source_video_render(target);
}

void RegisterFilter(tracker_filter_data *filter)
{
	if (!filter)
		return;

	std::lock_guard<std::mutex> lock(gFilterRegistryMutex);
	if (std::find(gFilterRegistry.begin(), gFilterRegistry.end(), filter) == gFilterRegistry.end())
		gFilterRegistry.push_back(filter);
}

void UnregisterFilter(tracker_filter_data *filter)
{
	std::lock_guard<std::mutex> lock(gFilterRegistryMutex);
	gFilterRegistry.erase(std::remove(gFilterRegistry.begin(), gFilterRegistry.end(), filter), gFilterRegistry.end());
}

void StopInferenceWorker(tracker_filter_data *filter)
{
	if (!filter)
		return;

	void *runOptions = nullptr;
	{
		std::lock_guard<std::mutex> lock(filter->runOptionsMutex);
		if (filter->activeRunOptions && !filter->activeRunTerminateRequested) {
			filter->activeRunTerminateRequested = true;
			filter->activeRunIntentionalTerminate = true;
			runOptions = filter->activeRunOptions;
		}
	}
	if (runOptions) {
		const OrtApi &api = Ort::GetApi();
		if (OrtStatus *status = api.RunOptionsSetTerminate(static_cast<OrtRunOptions *>(runOptions))) {
			blog(LOG_WARNING, "[Switch Motion] failed to terminate inference worker: %s",
			     api.GetErrorMessage(status));
			api.ReleaseStatus(status);
		}
	}

	{
		std::lock_guard<std::mutex> lock(filter->inferenceMutex);
		filter->stopInference = true;
		filter->hasPendingFrame = false;
		filter->pendingFrame = SwitchMotionFrameSample{};
	}
	filter->inferenceCv.notify_all();
	if (filter->inferenceThread.joinable() && filter->inferenceThread.get_id() != std::this_thread::get_id())
		filter->inferenceThread.join();
}

void ClearPendingInferenceFrame(tracker_filter_data *filter)
{
	if (!filter)
		return;

	std::lock_guard<std::mutex> lock(filter->inferenceMutex);
	filter->hasPendingFrame = false;
	filter->pendingFrame = SwitchMotionFrameSample{};
}

void RequestInferenceTerminate(tracker_filter_data *filter)
{
	if (!filter)
		return;

	void *runOptions = nullptr;
	{
		std::lock_guard<std::mutex> lock(filter->runOptionsMutex);
		if (!filter->activeRunOptions || filter->activeRunTerminateRequested)
			return;
		filter->activeRunTerminateRequested = true;
		filter->activeRunIntentionalTerminate = true;
		runOptions = filter->activeRunOptions;
	}

	const OrtApi &api = Ort::GetApi();
	if (OrtStatus *status = api.RunOptionsSetTerminate(static_cast<OrtRunOptions *>(runOptions))) {
		blog(LOG_WARNING, "[Switch Motion] failed to terminate inference during source deactivate: %s",
		     api.GetErrorMessage(status));
		api.ReleaseStatus(status);
	}
}

void StopAllInferenceWorkers()
{
	std::vector<tracker_filter_data *> filters;
	{
		std::lock_guard<std::mutex> lock(gFilterRegistryMutex);
		filters = gFilterRegistry;
	}

	for (auto *filter : filters)
		StopInferenceWorker(filter);
}

std::vector<tracker_filter_data *> SnapshotRegisteredFilters()
{
	std::lock_guard<std::mutex> lock(gFilterRegistryMutex);
	return gFilterRegistry;
}

struct PreparedInput {
	std::vector<float> tensor;
	float scale = 1.0f;
	float padX = 0.0f;
	float padY = 0.0f;
};

std::string ShapeToString(const std::vector<int64_t> &shape)
{
	std::string result = "[";
	for (size_t index = 0; index < shape.size(); index++) {
		if (index > 0)
			result += ",";
		result += std::to_string(shape[index]);
	}
	result += "]";
	return result;
}

void BeginInferenceRun(tracker_filter_data *filter, Ort::RunOptions &runOptions);
bool EndInferenceRun(tracker_filter_data *filter, bool *intentionalTerminate);
bool InferenceRunWasTerminated(tracker_filter_data *filter);
bool InferenceRunTerminationWasIntentional(tracker_filter_data *filter);

class ActiveInferenceRun {
public:
	ActiveInferenceRun(tracker_filter_data *filter, Ort::RunOptions &runOptions, bool *terminated,
			   bool *intentionalTerminate);
	~ActiveInferenceRun();

private:
	tracker_filter_data *filter = nullptr;
	bool *terminated = nullptr;
	bool *intentionalTerminate = nullptr;
};

class MotionInferenceRuntime {
public:
	bool EnsureLoaded(const SwitchMotionProfile &profile, QString *message)
	{
		const QString requestedBackend = profile.backend.isEmpty() ? QStringLiteral("auto") : profile.backend;
		const QString selectedBackend = SelectBackend(requestedBackend, profile.modelPath);
		if (session && modelPath == profile.modelPath && backend == selectedBackend)
			return true;

		session.reset();
		inputName.clear();
		outputName.clear();
		modelPath = profile.modelPath;
		backend = selectedBackend;

		if (modelPath.isEmpty()) {
			if (message)
				*message = QStringLiteral("Motion model path is empty");
			return false;
		}
		if (!QFileInfo::exists(modelPath)) {
			if (message)
				*message = QStringLiteral("Motion model is not installed: %1").arg(modelPath);
			return false;
		}

		try {
			Ort::SessionOptions options;
			options.SetIntraOpNumThreads(1);
			options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
			options.DisableMemPattern();

#if defined(__APPLE__)
			if (selectedBackend == QStringLiteral("coreml")) {
				uint32_t flags = COREML_FLAG_ONLY_ALLOW_STATIC_INPUT_SHAPES;
				if (CoreMlCpuOnlyEnabled())
					flags |= COREML_FLAG_USE_CPU_ONLY;
				if (OrtStatus *status = OrtSessionOptionsAppendExecutionProvider_CoreML(options, flags)) {
					const OrtApi &api = Ort::GetApi();
					if (message) {
						*message = QStringLiteral("CoreML provider unavailable: %1")
								   .arg(QString::fromUtf8(api.GetErrorMessage(status)));
					}
					api.ReleaseStatus(status);
					return false;
				}
			}
#endif

			session = std::make_unique<Ort::Session>(OrtEnv(), modelPath.toUtf8().constData(), options);

			Ort::AllocatorWithDefaultOptions allocator;
			auto inputNameAllocated = session->GetInputNameAllocated(0, allocator);
			auto outputNameAllocated = session->GetOutputNameAllocated(0, allocator);
			inputName = inputNameAllocated.get();
			outputName = outputNameAllocated.get();

			if (message && message->isEmpty())
				*message = QStringLiteral("Motion runtime ready (%1)").arg(backend);
			return true;
		} catch (const Ort::Exception &ex) {
			if (message)
				*message = QStringLiteral("Motion runtime error: %1").arg(QString::fromUtf8(ex.what()));
			session.reset();
			return false;
		}
	}

	QString Backend() const { return backend; }

	QString ProviderStatus() const
	{
		if (backend == QStringLiteral("coreml")) {
			return CoreMlCpuOnlyEnabled()
				       ? QStringLiteral("CoreML EP static-shape CPU diagnostics mode")
				       : QStringLiteral("CoreML EP static-shape provider; Apple chooses CPU/GPU/ANE placement");
		}
		if (backend == QStringLiteral("directml"))
			return QStringLiteral("DirectML EP requested; sequential execution required on Windows");
		if (backend == QStringLiteral("cpu"))
			return QStringLiteral("CPU diagnostics provider");
		return backend.isEmpty() ? QStringLiteral("unloaded") : backend;
	}

	bool Run(tracker_filter_data *filter, const PreparedInput &input, QVector<SwitchMotionDetection> *detections,
		 QString *message)
	{
		if (!session) {
			if (message)
				*message = QStringLiteral("Motion runtime is not loaded");
			return false;
		}

		bool terminatedByWatchdog = false;
		bool intentionallyTerminated = false;
		try {
			std::array<int64_t, 4> inputShape = {1, 3, kInputHeight, kInputWidth};
			auto memoryInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
			Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
				memoryInfo, const_cast<float *>(input.tensor.data()), input.tensor.size(), inputShape.data(),
				inputShape.size());
			const char *inputNames[] = {inputName.c_str()};
			const char *outputNames[] = {outputName.c_str()};
			Ort::RunOptions runOptions;
			ActiveInferenceRun activeRun(filter, runOptions, &terminatedByWatchdog, &intentionallyTerminated);
			auto outputs = session->Run(runOptions, inputNames, &inputTensor, 1, outputNames, 1);
			if (outputs.empty() || !outputs.front().IsTensor()) {
				if (message)
					*message = QStringLiteral("Motion model returned no tensor output");
				return false;
			}

			const auto shape = outputs.front().GetTensorTypeAndShapeInfo().GetShape();
			if (!outputShapeLogged) {
				blog(LOG_INFO, "[Switch Motion] model output '%s' shape %s", outputName.c_str(),
				     ShapeToString(shape).c_str());
				outputShapeLogged = true;
			}
			const QVector<int64_t> qtShape(shape.cbegin(), shape.cend());
			const float *values = outputs.front().GetTensorData<float>();
			const size_t valueCount = outputs.front().GetTensorTypeAndShapeInfo().GetElementCount();
			*detections = SwitchParseYolo26Detections(values, valueCount, qtShape, 0.0f, -1);
			if (detections->isEmpty() && shape.size() == 3 && shape[1] != 6 && shape[2] != 6 && message) {
				*message = QStringLiteral("Motion model output shape %1 is not YOLO26 end-to-end [1,N,6]")
						   .arg(QString::fromStdString(ShapeToString(shape)));
			}
			return true;
		} catch (const Ort::Exception &ex) {
			const bool terminated = terminatedByWatchdog || InferenceRunWasTerminated(filter);
			if (terminated)
				session.reset();
			if (message) {
				if (terminated &&
				    (intentionallyTerminated || InferenceRunTerminationWasIntentional(filter) ||
				     gFrontendQuiescing.load() || !filter->sourceActive.load())) {
					message->clear();
				} else {
					*message = terminated
							   ? QStringLiteral("Motion inference timed out and was restarted")
							   : QStringLiteral("Motion inference error: %1").arg(QString::fromUtf8(ex.what()));
				}
			}
			return false;
		}
	}

private:
	static Ort::Env &OrtEnv()
	{
		static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "switch-motion");
		return env;
	}

	static QString SelectBackend(const QString &requestedBackend, const QString &path)
	{
		UNUSED_PARAMETER(path);
#if defined(__APPLE__)
		if (requestedBackend == QStringLiteral("auto"))
			return QStringLiteral("coreml");
#elif defined(_WIN32)
		if (requestedBackend == QStringLiteral("auto"))
			return QStringLiteral("directml");
#endif
		return requestedBackend == QStringLiteral("auto") ? QStringLiteral("cpu") : requestedBackend;
	}

	static bool CoreMlCpuOnlyEnabled()
	{
		const char *value = getenv("SWITCH_MOTION_COREML_CPU_ONLY");
		return value && QString::fromUtf8(value).trimmed() == QStringLiteral("1");
	}

	std::unique_ptr<Ort::Session> session;
	QString modelPath;
	QString backend;
	std::string inputName;
	std::string outputName;
	bool outputShapeLogged = false;
};

SwitchMotionProfile ProfileFromSettings(obs_data_t *settings)
{
	SwitchMotionProfile profile = SwitchDefaultMotionProfile();
	profile.id = QString::fromUtf8(obs_data_get_string(settings, kProfileId));
	if (profile.id.isEmpty())
		profile.id = QStringLiteral("motion-filter");
	profile.enabled = obs_data_get_bool(settings, kMotionEnabled);
	profile.confidenceThreshold = static_cast<float>(obs_data_get_double(settings, kConfidenceThreshold));
	profile.targetClassId = static_cast<int>(obs_data_get_int(settings, kTargetClassId));
	profile.maxZoom = static_cast<float>(obs_data_get_double(settings, kMaxZoom));
	profile.framingMargin = static_cast<float>(obs_data_get_double(settings, kFramingMargin));
	profile.deadZone = static_cast<float>(obs_data_get_double(settings, kDeadZone));
	profile.smoothing = static_cast<float>(obs_data_get_double(settings, kSmoothing));
	profile.holdMs = static_cast<int>(obs_data_get_int(settings, kHoldMs));
	profile.backend = NormalizeMotionBackend(QString::fromUtf8(obs_data_get_string(settings, kBackend)));
	if (profile.backend.isEmpty())
		profile.backend = QStringLiteral("auto");
	profile.modelPath = QString::fromUtf8(obs_data_get_string(settings, kModelPath));
	profile.subjectMode = NormalizeSubjectMode(QString::fromUtf8(obs_data_get_string(settings, kSubjectMode)));
	if (profile.subjectMode.isEmpty())
		profile.subjectMode = QStringLiteral("auto");
	profile.framingMode = NormalizeFramingMode(QString::fromUtf8(obs_data_get_string(settings, kFramingMode)));
	if (profile.framingMode.isEmpty())
		profile.framingMode = QStringLiteral("upper_body");
	profile.trackerHighThreshold = static_cast<float>(obs_data_get_double(settings, kTrackerHighThreshold));
	profile.trackerLowThreshold = static_cast<float>(obs_data_get_double(settings, kTrackerLowThreshold));
	profile.newTrackThreshold = static_cast<float>(obs_data_get_double(settings, kNewTrackThreshold));
	profile.trackBufferFrames = static_cast<int>(obs_data_get_int(settings, kTrackBufferFrames));
	profile.lockedTrackGraceFrames = static_cast<int>(obs_data_get_int(settings, kLockedTrackGraceFrames));
	profile.autoSwitchMs = static_cast<int>(obs_data_get_int(settings, kAutoSwitchMs));
	profile.panResponsiveness = static_cast<float>(obs_data_get_double(settings, kPanResponsiveness));
	profile.tiltResponsiveness = static_cast<float>(obs_data_get_double(settings, kTiltResponsiveness));
	profile.zoomResponsiveness = static_cast<float>(obs_data_get_double(settings, kZoomResponsiveness));
	profile.maxPanSpeed = static_cast<float>(obs_data_get_double(settings, kMaxPanSpeed));
	profile.maxTiltSpeed = static_cast<float>(obs_data_get_double(settings, kMaxTiltSpeed));
	profile.maxZoomSpeed = static_cast<float>(obs_data_get_double(settings, kMaxZoomSpeed));
	profile.debugOverlay = obs_data_get_bool(settings, kDebugOverlay);
	profile.lockedTrackId = static_cast<int>(obs_data_get_int(settings, kLockedTrackId));
	return profile;
}

void PublishCameraState(tracker_filter_data *filter, const SwitchMotionCameraState &state)
{
	filter->currentCamX.store(state.camX);
	filter->currentCamY.store(state.camY);
	filter->currentZoom.store(state.zoom);
	filter->targetActive.store(state.targetActive);
	filter->targetConfidence.store(state.targetConfidence);
	filter->currentTargetTrackId.store(state.targetTrackId);
}

void PublishRuntimeState(tracker_filter_data *filter, const SwitchMotionProfile &profile,
			 const SwitchMotionCameraState &cameraState,
			 const QVector<SwitchMotionTrack> &tracks,
			 const MotionInferenceRuntime &runtime,
			 const QString &message,
			 double preprocessingMs,
			 double inferenceMs,
			 double trackingMs)
{
	if (!filter)
		return;
	if (gFrontendQuiescing.load())
		return;

	const uint64_t nowNs = os_gettime_ns();
	uint64_t previousPublishNs = filter->lastRuntimePublishNs.load();
	if (previousPublishNs != 0 && nowNs - previousPublishNs < kRuntimePublishIntervalNs)
		return;
	if (!filter->lastRuntimePublishNs.compare_exchange_strong(previousPublishNs, nowNs))
		return;

	SwitchMotionRuntimeState state;
	state.status = profile.enabled ? QStringLiteral("running") : QStringLiteral("idle");
	state.backend = runtime.Backend().isEmpty() ? profile.backend : runtime.Backend();
	state.modelPath = profile.modelPath;
	state.message = message;
	state.providerStatus = runtime.ProviderStatus();
	state.modelAvailable = !profile.modelPath.isEmpty() && QFileInfo::exists(profile.modelPath);
	state.targetActive = cameraState.targetActive;
	state.targetConfidence = cameraState.targetConfidence;
	state.targetTrackId = cameraState.targetTrackId;
	state.sourceUuid = filter->sourceUuid;
	state.camX = cameraState.camX;
	state.camY = cameraState.camY;
	state.zoom = cameraState.zoom;
	state.preprocessingMs = preprocessingMs;
	state.inferenceMs = inferenceMs;
	state.trackingMs = trackingMs;
	state.droppedFrames = filter->droppedFrames.load();
	state.tracks = tracks;
	for (const auto &track : tracks) {
		if (track.state == QStringLiteral("active") || track.state == QStringLiteral("new") ||
		    track.state == QStringLiteral("recovered"))
			state.activeTrackCount++;
		if (track.trackId == state.targetTrackId)
			state.targetState = track.state;
	}
	SwitchMotionPublishRuntimeState(state);
}

void BeginInferenceRun(tracker_filter_data *filter, Ort::RunOptions &runOptions)
{
	if (!filter)
		return;

	std::lock_guard<std::mutex> lock(filter->runOptionsMutex);
	filter->activeRunOptions = runOptions;
	filter->activeRunStartNs = os_gettime_ns();
	filter->activeRunTerminateRequested = false;
	filter->activeRunIntentionalTerminate = false;
}

bool EndInferenceRun(tracker_filter_data *filter, bool *intentionalTerminate)
{
	if (!filter)
		return false;

	std::lock_guard<std::mutex> lock(filter->runOptionsMutex);
	const bool wasTerminated = filter->activeRunTerminateRequested;
	if (intentionalTerminate)
		*intentionalTerminate = filter->activeRunIntentionalTerminate;
	filter->activeRunOptions = nullptr;
	filter->activeRunStartNs = 0;
	filter->activeRunTerminateRequested = false;
	filter->activeRunIntentionalTerminate = false;
	return wasTerminated;
}

bool InferenceRunWasTerminated(tracker_filter_data *filter)
{
	if (!filter)
		return false;

	std::lock_guard<std::mutex> lock(filter->runOptionsMutex);
	return filter->activeRunTerminateRequested;
}

bool InferenceRunTerminationWasIntentional(tracker_filter_data *filter)
{
	if (!filter)
		return false;

	std::lock_guard<std::mutex> lock(filter->runOptionsMutex);
	return filter->activeRunIntentionalTerminate;
}

ActiveInferenceRun::ActiveInferenceRun(tracker_filter_data *filter_, Ort::RunOptions &runOptions_, bool *terminated_,
				       bool *intentionalTerminate_)
	: filter(filter_), terminated(terminated_), intentionalTerminate(intentionalTerminate_)
{
	BeginInferenceRun(filter, runOptions_);
}

ActiveInferenceRun::~ActiveInferenceRun()
{
	if (terminated)
		*terminated = EndInferenceRun(filter, intentionalTerminate);
	else
		EndInferenceRun(filter, intentionalTerminate);
}

void WatchdogInferenceRun(tracker_filter_data *filter)
{
	if (!filter)
		return;

	const uint64_t nowNs = os_gettime_ns();
	void *runOptions = nullptr;
	uint64_t runStartNs = 0;
	{
		std::lock_guard<std::mutex> lock(filter->runOptionsMutex);
		if (!filter->activeRunOptions || filter->activeRunStartNs == 0 || filter->activeRunTerminateRequested)
			return;
		if (nowNs - filter->activeRunStartNs < kInferenceWatchdogTimeoutNs)
			return;

		runOptions = filter->activeRunOptions;
		runStartNs = filter->activeRunStartNs;
		filter->activeRunTerminateRequested = true;
		filter->activeRunIntentionalTerminate = false;
	}

	const OrtApi &api = Ort::GetApi();
	if (OrtStatus *status = api.RunOptionsSetTerminate(static_cast<OrtRunOptions *>(runOptions))) {
		uint64_t previousLogNs = filter->lastWatchdogLogNs.load();
		if (previousLogNs == 0 || nowNs - previousLogNs >= kInferenceWatchdogLogIntervalNs) {
			if (filter->lastWatchdogLogNs.compare_exchange_strong(previousLogNs, nowNs)) {
				blog(LOG_WARNING, "[Switch Motion] failed to terminate stalled inference: %s",
				     api.GetErrorMessage(status));
			}
		}
		api.ReleaseStatus(status);
		return;
	}

	uint64_t previousLogNs = filter->lastWatchdogLogNs.load();
	if (previousLogNs == 0 || nowNs - previousLogNs >= kInferenceWatchdogLogIntervalNs) {
		if (filter->lastWatchdogLogNs.compare_exchange_strong(previousLogNs, nowNs)) {
			blog(LOG_WARNING, "[Switch Motion] terminated stalled inference after %.2f ms",
			     double(nowNs - runStartNs) / 1000000.0);
		}
	}
}

int ClampByte(int value)
{
	return std::max(0, std::min(255, value));
}

uint8_t PlaneValue(const SwitchMotionFrameSample &sample, int plane, int x, int y)
{
	if (plane < 0 || plane >= MAX_AV_PLANES || sample.planes[plane].empty() || sample.linesize[plane] == 0)
		return 0;
	const uint32_t row = static_cast<uint32_t>(std::max(0, y));
	const uint32_t col = static_cast<uint32_t>(std::max(0, x));
	const size_t offset = size_t(row) * sample.linesize[plane] + col;
	return offset < sample.planes[plane].size() ? sample.planes[plane][offset] : 0;
}

void YuvToRgb(uint8_t yValue, uint8_t uValue, uint8_t vValue, bool fullRange, uint8_t *r, uint8_t *g, uint8_t *b);

uint16_t PlaneValue16(const SwitchMotionFrameSample &sample, int plane, int x, int y)
{
	if (plane < 0 || plane >= MAX_AV_PLANES || sample.planes[plane].empty() || sample.linesize[plane] < 2)
		return 0;
	const uint32_t row = static_cast<uint32_t>(std::max(0, y));
	const uint32_t col = static_cast<uint32_t>(std::max(0, x));
	const size_t offset = size_t(row) * sample.linesize[plane] + size_t(col) * 2;
	if (offset + 1 >= sample.planes[plane].size())
		return 0;
	return uint16_t(sample.planes[plane][offset]) | (uint16_t(sample.planes[plane][offset + 1]) << 8);
}

uint8_t VideoWordToByte(uint16_t value)
{
	if (value > 4095)
		return static_cast<uint8_t>(ClampByte(int(value >> 8)));
	return static_cast<uint8_t>(ClampByte(int(value >> 2)));
}

void Yuv16ToRgb(uint16_t yValue, uint16_t uValue, uint16_t vValue, bool fullRange, uint8_t *r, uint8_t *g, uint8_t *b)
{
	YuvToRgb(VideoWordToByte(yValue), VideoWordToByte(uValue), VideoWordToByte(vValue), fullRange, r, g, b);
}

void YuvToRgb(uint8_t yValue, uint8_t uValue, uint8_t vValue, bool fullRange, uint8_t *r, uint8_t *g, uint8_t *b)
{
	if (fullRange) {
		const int y = yValue;
		const int u = int(uValue) - 128;
		const int v = int(vValue) - 128;
		*r = static_cast<uint8_t>(ClampByte(int(std::round(y + 1.402f * v))));
		*g = static_cast<uint8_t>(ClampByte(int(std::round(y - 0.344136f * u - 0.714136f * v))));
		*b = static_cast<uint8_t>(ClampByte(int(std::round(y + 1.772f * u))));
		return;
	}

	const int c = std::max(0, int(yValue) - 16);
	const int d = int(uValue) - 128;
	const int e = int(vValue) - 128;
	*r = static_cast<uint8_t>(ClampByte((298 * c + 409 * e + 128) >> 8));
	*g = static_cast<uint8_t>(ClampByte((298 * c - 100 * d - 208 * e + 128) >> 8));
	*b = static_cast<uint8_t>(ClampByte((298 * c + 516 * d + 128) >> 8));
}

bool SampleRgb(const SwitchMotionFrameSample &sample, int x, int y, uint8_t *r, uint8_t *g, uint8_t *b)
{
	if (sample.width == 0 || sample.height == 0)
		return false;

	x = std::max(0, std::min(int(sample.width) - 1, x));
	y = std::max(0, std::min(int(sample.height) - 1, y));
	if (sample.flip)
		y = int(sample.height) - 1 - y;

	switch (sample.format) {
	case VIDEO_FORMAT_BGRA:
	case VIDEO_FORMAT_BGRX: {
		if (sample.planes[0].empty() || sample.linesize[0] == 0)
			return false;
		const size_t offset = size_t(y) * sample.linesize[0] + size_t(x) * 4;
		if (offset + 2 >= sample.planes[0].size())
			return false;
		*b = sample.planes[0][offset + 0];
		*g = sample.planes[0][offset + 1];
		*r = sample.planes[0][offset + 2];
		return true;
	}
	case VIDEO_FORMAT_RGBA: {
		if (sample.planes[0].empty() || sample.linesize[0] == 0)
			return false;
		const size_t offset = size_t(y) * sample.linesize[0] + size_t(x) * 4;
		if (offset + 2 >= sample.planes[0].size())
			return false;
		*r = sample.planes[0][offset + 0];
		*g = sample.planes[0][offset + 1];
		*b = sample.planes[0][offset + 2];
		return true;
	}
	case VIDEO_FORMAT_BGR3: {
		if (sample.planes[0].empty() || sample.linesize[0] == 0)
			return false;
		const size_t offset = size_t(y) * sample.linesize[0] + size_t(x) * 3;
		if (offset + 2 >= sample.planes[0].size())
			return false;
		*b = sample.planes[0][offset + 0];
		*g = sample.planes[0][offset + 1];
		*r = sample.planes[0][offset + 2];
		return true;
	}
	case VIDEO_FORMAT_Y800: {
		const uint8_t value = PlaneValue(sample, 0, x, y);
		*r = value;
		*g = value;
		*b = value;
		return true;
	}
	case VIDEO_FORMAT_NV12: {
		const uint8_t yValue = PlaneValue(sample, 0, x, y);
		const size_t uvOffset = size_t(y / 2) * sample.linesize[1] + size_t(x / 2) * 2;
		if (sample.planes[1].empty() || uvOffset + 1 >= sample.planes[1].size())
			return false;
		YuvToRgb(yValue, sample.planes[1][uvOffset + 0], sample.planes[1][uvOffset + 1], sample.fullRange, r, g, b);
		return true;
	}
	case VIDEO_FORMAT_I420: {
		const uint8_t yValue = PlaneValue(sample, 0, x, y);
		YuvToRgb(yValue, PlaneValue(sample, 1, x / 2, y / 2), PlaneValue(sample, 2, x / 2, y / 2),
			 sample.fullRange, r, g, b);
		return true;
	}
	case VIDEO_FORMAT_I444: {
		const uint8_t yValue = PlaneValue(sample, 0, x, y);
		YuvToRgb(yValue, PlaneValue(sample, 1, x, y), PlaneValue(sample, 2, x, y), sample.fullRange, r, g, b);
		return true;
	}
	case VIDEO_FORMAT_I422: {
		const uint8_t yValue = PlaneValue(sample, 0, x, y);
		YuvToRgb(yValue, PlaneValue(sample, 1, x / 2, y), PlaneValue(sample, 2, x / 2, y), sample.fullRange, r, g,
			 b);
		return true;
	}
	case VIDEO_FORMAT_I010: {
		const uint16_t yValue = PlaneValue16(sample, 0, x, y);
		Yuv16ToRgb(yValue, PlaneValue16(sample, 1, x / 2, y / 2), PlaneValue16(sample, 2, x / 2, y / 2),
			   sample.fullRange, r, g, b);
		return true;
	}
	case VIDEO_FORMAT_I210: {
		const uint16_t yValue = PlaneValue16(sample, 0, x, y);
		Yuv16ToRgb(yValue, PlaneValue16(sample, 1, x / 2, y), PlaneValue16(sample, 2, x / 2, y),
			   sample.fullRange, r, g, b);
		return true;
	}
	case VIDEO_FORMAT_I412: {
		const uint16_t yValue = PlaneValue16(sample, 0, x, y);
		Yuv16ToRgb(yValue, PlaneValue16(sample, 1, x, y), PlaneValue16(sample, 2, x, y), sample.fullRange, r, g,
			   b);
		return true;
	}
	case VIDEO_FORMAT_P010: {
		const uint16_t yValue = PlaneValue16(sample, 0, x, y);
		const size_t uvOffset = size_t(y / 2) * sample.linesize[1] + size_t(x / 2) * 4;
		if (sample.planes[1].empty() || uvOffset + 3 >= sample.planes[1].size())
			return false;
		const uint16_t uValue = uint16_t(sample.planes[1][uvOffset]) |
					(uint16_t(sample.planes[1][uvOffset + 1]) << 8);
		const uint16_t vValue = uint16_t(sample.planes[1][uvOffset + 2]) |
					(uint16_t(sample.planes[1][uvOffset + 3]) << 8);
		Yuv16ToRgb(yValue, uValue, vValue, sample.fullRange, r, g, b);
		return true;
	}
	case VIDEO_FORMAT_P216:
	case VIDEO_FORMAT_P416: {
		const uint16_t yValue = PlaneValue16(sample, 0, x, y);
		const int chromaX = sample.format == VIDEO_FORMAT_P216 ? x / 2 : x;
		const size_t uvOffset = size_t(y) * sample.linesize[1] + size_t(chromaX) * 4;
		if (sample.planes[1].empty() || uvOffset + 3 >= sample.planes[1].size())
			return false;
		const uint16_t uValue = uint16_t(sample.planes[1][uvOffset]) |
					(uint16_t(sample.planes[1][uvOffset + 1]) << 8);
		const uint16_t vValue = uint16_t(sample.planes[1][uvOffset + 2]) |
					(uint16_t(sample.planes[1][uvOffset + 3]) << 8);
		Yuv16ToRgb(yValue, uValue, vValue, sample.fullRange, r, g, b);
		return true;
	}
	case VIDEO_FORMAT_YUY2:
	case VIDEO_FORMAT_YVYU:
	case VIDEO_FORMAT_UYVY: {
		if (sample.planes[0].empty() || sample.linesize[0] == 0)
			return false;
		const size_t offset = size_t(y) * sample.linesize[0] + size_t(x / 2) * 4;
		if (offset + 3 >= sample.planes[0].size())
			return false;
		uint8_t yValue = 0;
		uint8_t uValue = 128;
		uint8_t vValue = 128;
		const bool secondPixel = (x % 2) != 0;
		if (sample.format == VIDEO_FORMAT_YUY2) {
			yValue = sample.planes[0][offset + (secondPixel ? 2 : 0)];
			uValue = sample.planes[0][offset + 1];
			vValue = sample.planes[0][offset + 3];
		} else if (sample.format == VIDEO_FORMAT_YVYU) {
			yValue = sample.planes[0][offset + (secondPixel ? 2 : 0)];
			vValue = sample.planes[0][offset + 1];
			uValue = sample.planes[0][offset + 3];
		} else {
			uValue = sample.planes[0][offset + 0];
			yValue = sample.planes[0][offset + (secondPixel ? 3 : 1)];
			vValue = sample.planes[0][offset + 2];
		}
		YuvToRgb(yValue, uValue, vValue, sample.fullRange, r, g, b);
		return true;
	}
	default:
		return false;
	}
}

int PlaneCount(enum video_format format)
{
	switch (format) {
	case VIDEO_FORMAT_I420:
	case VIDEO_FORMAT_I444:
	case VIDEO_FORMAT_I422:
	case VIDEO_FORMAT_I010:
	case VIDEO_FORMAT_I210:
	case VIDEO_FORMAT_I412:
		return 3;
	case VIDEO_FORMAT_NV12:
	case VIDEO_FORMAT_P010:
	case VIDEO_FORMAT_P216:
	case VIDEO_FORMAT_P416:
		return 2;
	case VIDEO_FORMAT_BGRA:
	case VIDEO_FORMAT_BGRX:
	case VIDEO_FORMAT_RGBA:
	case VIDEO_FORMAT_BGR3:
	case VIDEO_FORMAT_Y800:
	case VIDEO_FORMAT_YUY2:
	case VIDEO_FORMAT_YVYU:
	case VIDEO_FORMAT_UYVY:
		return 1;
	default:
		return 0;
	}
}

uint32_t PlaneRows(enum video_format format, int plane, uint32_t height)
{
	switch (format) {
	case VIDEO_FORMAT_NV12:
	case VIDEO_FORMAT_P010:
		return plane == 0 ? height : (height + 1) / 2;
	case VIDEO_FORMAT_I420:
	case VIDEO_FORMAT_I010:
		return plane == 0 ? height : (height + 1) / 2;
	case VIDEO_FORMAT_I422:
	case VIDEO_FORMAT_I210:
	case VIDEO_FORMAT_I412:
	case VIDEO_FORMAT_P216:
	case VIDEO_FORMAT_P416:
		return height;
	default:
		return height;
	}
}

bool CopyFrameSample(struct obs_source_frame *frame, SwitchMotionFrameSample *sample)
{
	if (!frame || !sample || frame->width == 0 || frame->height == 0)
		return false;

	const int planeCount = PlaneCount(frame->format);
	if (planeCount <= 0)
		return false;

	SwitchMotionFrameSample next;
	next.format = frame->format;
	next.width = frame->width;
	next.height = frame->height;
	next.timestamp = frame->timestamp;
	next.fullRange = frame->full_range;
	next.flip = frame->flip;

	for (int plane = 0; plane < planeCount; plane++) {
		if (!frame->data[plane] || frame->linesize[plane] == 0)
			return false;
		next.linesize[plane] = frame->linesize[plane];
		const uint32_t rows = PlaneRows(frame->format, plane, frame->height);
		const size_t bytes = size_t(frame->linesize[plane]) * rows;
		next.planes[plane].assign(frame->data[plane], frame->data[plane] + bytes);
	}

	*sample = std::move(next);
	return true;
}

bool SameScaleInfo(const video_scale_info &left, const video_scale_info &right)
{
	return left.format == right.format && left.width == right.width && left.height == right.height &&
	       left.range == right.range && left.colorspace == right.colorspace;
}

bool ScaleFrameSampleToBgrx(tracker_filter_data *filter, struct obs_source_frame *frame, SwitchMotionFrameSample *sample)
{
	if (!filter || !frame || !sample || frame->width == 0 || frame->height == 0)
		return false;

	const video_scale_info srcInfo = {
		frame->format,
		frame->width,
		frame->height,
		frame->full_range ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL,
		VIDEO_CS_DEFAULT,
	};
	const video_scale_info dstInfo = {
		VIDEO_FORMAT_BGRX,
		frame->width,
		frame->height,
		VIDEO_RANGE_FULL,
		VIDEO_CS_DEFAULT,
	};

	if (!filter->scaler || !SameScaleInfo(filter->scalerSrcInfo, srcInfo) ||
	    !SameScaleInfo(filter->scalerDstInfo, dstInfo)) {
		if (filter->scaler) {
			video_scaler_destroy(filter->scaler);
			filter->scaler = nullptr;
		}

		const int result = video_scaler_create(&filter->scaler, &dstInfo, &srcInfo, VIDEO_SCALE_FAST_BILINEAR);
		if (result != VIDEO_SCALER_SUCCESS) {
			blog(LOG_WARNING, "[Switch Motion] video_scaler_create failed for %s %ux%u: %d",
			     get_video_format_name(frame->format), frame->width, frame->height, result);
			return false;
		}

		filter->scalerSrcInfo = srcInfo;
		filter->scalerDstInfo = dstInfo;
		filter->scalerBuffer.assign(size_t(dstInfo.width) * dstInfo.height * 4, 0);
	}

	struct obs_source_frame converted = {};
	converted.data[0] = filter->scalerBuffer.data();
	converted.linesize[0] = dstInfo.width * 4;
	converted.width = dstInfo.width;
	converted.height = dstInfo.height;
	converted.format = dstInfo.format;
	converted.timestamp = frame->timestamp;
	converted.full_range = true;
	converted.flip = frame->flip;

	if (!video_scaler_scale(filter->scaler, converted.data, converted.linesize, frame->data, frame->linesize)) {
		blog(LOG_WARNING, "[Switch Motion] video_scaler_scale failed for %s %ux%u",
		     get_video_format_name(frame->format), frame->width, frame->height);
		return false;
	}

	return CopyFrameSample(&converted, sample);
}

void LogSampleFailure(tracker_filter_data *filter, const obs_source_frame *frame)
{
	if (!filter || !frame)
		return;

	const uint64_t nowNs = os_gettime_ns();
	uint64_t previousLogNs = filter->lastSampleFailureLogNs.load();
	if (previousLogNs != 0 && nowNs - previousLogNs < kFrameLogIntervalNs)
		return;
	if (!filter->lastSampleFailureLogNs.compare_exchange_strong(previousLogNs, nowNs))
		return;

	blog(LOG_WARNING,
	     "[Switch Motion] unable to sample frame format=%s (%d) %ux%u data0=%s linesize0=%u",
	     get_video_format_name(frame->format), static_cast<int>(frame->format), frame->width, frame->height,
	     frame->data[0] ? "yes" : "no", frame->linesize[0]);
}

void QueueInferenceFrame(tracker_filter_data *filter, const SwitchMotionProfile &profile, obs_source_frame *frame)
{
	if (!filter || !frame || !profile.enabled || gFrontendQuiescing.load())
		return;

	const uint64_t nowNs = os_gettime_ns();
	uint64_t previousNs = filter->lastQueuedNs.load();
	if (previousNs != 0 && nowNs - previousNs < kInferenceIntervalNs)
		return;
	if (!filter->lastQueuedNs.compare_exchange_strong(previousNs, nowNs))
		return;

	SwitchMotionFrameSample sample;
	if (!CopyFrameSample(frame, &sample) && !ScaleFrameSampleToBgrx(filter, frame, &sample)) {
		LogSampleFailure(filter, frame);
		return;
	}

	uint64_t previousFrameLogNs = filter->lastFrameLogNs.load();
	if (previousFrameLogNs == 0 || nowNs - previousFrameLogNs >= kFrameLogIntervalNs) {
		if (filter->lastFrameLogNs.compare_exchange_strong(previousFrameLogNs, nowNs)) {
			blog(LOG_INFO, "[Switch Motion] frame input %ux%u format=%d", sample.width, sample.height,
			     static_cast<int>(sample.format));
		}
	}

	std::unique_lock<std::mutex> lock(filter->inferenceMutex, std::try_to_lock);
	if (!lock.owns_lock()) {
		filter->droppedFrames.fetch_add(1);
		return;
	}
	if (filter->hasPendingFrame)
		filter->droppedFrames.fetch_add(1);
	filter->pendingFrame = std::move(sample);
	filter->hasPendingFrame = true;
	lock.unlock();
	filter->inferenceCv.notify_one();
}

bool PrepareInput(const SwitchMotionFrameSample &sample, PreparedInput *prepared)
{
	if (!prepared || sample.width == 0 || sample.height == 0)
		return false;

	prepared->tensor.assign(size_t(3) * kInputWidth * kInputHeight, 114.0f / 255.0f);
	const float scale = std::min(float(kInputWidth) / float(sample.width), float(kInputHeight) / float(sample.height));
	const int resizedWidth = std::max(1, int(std::round(float(sample.width) * scale)));
	const int resizedHeight = std::max(1, int(std::round(float(sample.height) * scale)));
	const int padX = (kInputWidth - resizedWidth) / 2;
	const int padY = (kInputHeight - resizedHeight) / 2;
	prepared->scale = scale;
	prepared->padX = float(padX);
	prepared->padY = float(padY);

	const size_t planeSize = size_t(kInputWidth) * kInputHeight;
	for (int y = 0; y < resizedHeight; y++) {
		const int srcY = std::min(int(sample.height) - 1, int(float(y) / scale));
		for (int x = 0; x < resizedWidth; x++) {
			const int srcX = std::min(int(sample.width) - 1, int(float(x) / scale));
			uint8_t r = 0;
			uint8_t g = 0;
			uint8_t b = 0;
			if (!SampleRgb(sample, srcX, srcY, &r, &g, &b))
				return false;
			const size_t dst = size_t(y + padY) * kInputWidth + size_t(x + padX);
			prepared->tensor[dst] = float(r) / 255.0f;
			prepared->tensor[planeSize + dst] = float(g) / 255.0f;
			prepared->tensor[planeSize * 2 + dst] = float(b) / 255.0f;
		}
	}
	return true;
}

QVector<SwitchMotionDetection> MapDetectionsToFrame(const QVector<SwitchMotionDetection> &detections,
						    const PreparedInput &input,
						    const SwitchMotionFrameSample &sample,
						    const SwitchMotionProfile &profile)
{
	QVector<SwitchMotionDetection> mapped;
	for (auto detection : detections) {
		const float trackerThreshold = std::min(profile.confidenceThreshold, profile.trackerLowThreshold);
		if (detection.confidence < trackerThreshold)
			continue;
		if (profile.targetClassId >= 0 && detection.classId != profile.targetClassId)
			continue;
		detection.x1 = std::clamp((detection.x1 - input.padX) / input.scale, 0.0f, float(sample.width));
		detection.y1 = std::clamp((detection.y1 - input.padY) / input.scale, 0.0f, float(sample.height));
		detection.x2 = std::clamp((detection.x2 - input.padX) / input.scale, 0.0f, float(sample.width));
		detection.y2 = std::clamp((detection.y2 - input.padY) / input.scale, 0.0f, float(sample.height));
		if (detection.x2 > detection.x1 && detection.y2 > detection.y1)
			mapped.push_back(detection);
	}
	return SuppressDuplicateDetections(std::move(mapped), sample);
}

SwitchMotionDetection FramingDetectionForPrimarySubject(const SwitchMotionDetection &detection, const QSize &frameSize)
{
	if (detection.classId != 0 || frameSize.width() <= 0 || frameSize.height() <= 0)
		return detection;

	const float frameWidth = static_cast<float>(frameSize.width());
	const float frameHeight = static_cast<float>(frameSize.height());
	const float boxWidth = std::max(1.0f, detection.x2 - detection.x1);
	const float boxHeight = std::max(1.0f, detection.y2 - detection.y1);
	const float centerX = (detection.x1 + detection.x2) * 0.5f;
	const float minTargetWidth = std::min(boxHeight * 0.28f, frameWidth);
	const float targetWidth = std::clamp(boxWidth * 0.58f, minTargetWidth, frameWidth);
	const float targetTop = detection.y1 + boxHeight * 0.04f;
	const float targetBottom = detection.y1 + boxHeight * 0.48f;

	SwitchMotionDetection framing = detection;
	framing.x1 = std::clamp(centerX - targetWidth * 0.5f, 0.0f, frameWidth);
	framing.x2 = std::clamp(centerX + targetWidth * 0.5f, 0.0f, frameWidth);
	framing.y1 = std::clamp(targetTop, 0.0f, std::max(0.0f, frameHeight - 1.0f));
	framing.y2 = std::clamp(targetBottom, framing.y1 + 1.0f, frameHeight);
	return framing;
}

void InferenceLoop(tracker_filter_data *filter)
{
	MotionInferenceRuntime runtime;
	while (true) {
		@autoreleasepool {
		SwitchMotionFrameSample frame;
		{
			std::unique_lock<std::mutex> lock(filter->inferenceMutex);
			filter->inferenceCv.wait(lock, [filter]() { return filter->stopInference || filter->hasPendingFrame; });
			if (filter->stopInference)
				return;
			frame = std::move(filter->pendingFrame);
			filter->pendingFrame = SwitchMotionFrameSample{};
			filter->hasPendingFrame = false;
		}

		if (gFrontendQuiescing.load())
			continue;

		SwitchMotionProfile profile;
		{
			std::lock_guard<std::mutex> lock(filter->stateMutex);
			profile = filter->profile;
		}

		if (!profile.enabled)
			continue;

		QString runtimeMessage;
		if (!runtime.EnsureLoaded(profile, &runtimeMessage)) {
			if (!filter->runtimeErrorLogged) {
				blog(LOG_ERROR, "[Switch Motion] %s", runtimeMessage.toUtf8().constData());
				SwitchMotionPublishRuntimeError(runtimeMessage);
				filter->runtimeErrorLogged = true;
			}
			continue;
		}
		filter->inferenceReady = true;
		filter->runtimeErrorLogged = false;

		const uint64_t preprocessStartNs = os_gettime_ns();
		PreparedInput input;
		if (!PrepareInput(frame, &input))
			continue;
		const double preprocessingMs = double(os_gettime_ns() - preprocessStartNs) / 1000000.0;

		const uint64_t inferenceStartNs = os_gettime_ns();
		QVector<SwitchMotionDetection> detections;
		if (!runtime.Run(filter, input, &detections, &runtimeMessage)) {
			if (runtimeMessage.isEmpty())
				continue;
			if (!filter->runtimeErrorLogged) {
				blog(LOG_ERROR, "[Switch Motion] %s", runtimeMessage.toUtf8().constData());
				SwitchMotionPublishRuntimeError(runtimeMessage);
				filter->runtimeErrorLogged = true;
			}
			continue;
		}
		const double inferenceMs = double(os_gettime_ns() - inferenceStartNs) / 1000000.0;
		if (gFrontendQuiescing.load())
			continue;

		const uint64_t trackingStartNs = os_gettime_ns();
		SwitchMotionProfile activeProfile;
		SwitchMotionCameraState currentState;
		{
			std::lock_guard<std::mutex> lock(filter->stateMutex);
			activeProfile = filter->profile;
			currentState = filter->cameraState;
		}
		if (!activeProfile.enabled)
			continue;

		const auto mappedDetections = MapDetectionsToFrame(detections, input, frame, activeProfile);
		const QSize frameSize(int(frame.width), int(frame.height));
		const qint64 nowMs = static_cast<qint64>(os_gettime_ns() / 1000000ULL);
		QVector<SwitchMotionTrack> tracks;
		SwitchMotionTrack selectedTrack;
		bool hasTarget = false;
		SwitchMotionDetection framingTarget;
		SwitchMotionCameraState next;
		{
			std::lock_guard<std::mutex> lock(filter->stateMutex);
			tracks = filter->tracker.Update(mappedDetections, activeProfile, frameSize, nowMs, filter->sourceUuid);
			hasTarget = SwitchMotionSelectTargetTrack(tracks, activeProfile, currentState, frameSize, nowMs,
								  &filter->autoCandidateTrackId,
								  &filter->autoCandidateSinceMs,
								  &selectedTrack);
			if (activeProfile.framingMode == QStringLiteral("group") && activeProfile.subjectMode != QStringLiteral("locked")) {
				const auto group = SwitchMotionGroupDetection(tracks);
				if (MotionDetectionArea(group) > 0.0f) {
					framingTarget = group;
					hasTarget = true;
					currentState.targetTrackId = -2;
				}
			} else if (hasTarget) {
				auto trackBox = selectedTrack.smoothedBox;
				if (MotionDetectionArea(trackBox) <= 0.0f)
					trackBox = selectedTrack.bbox;
				const float lookAheadMs = 180.0f;
				trackBox.x1 += selectedTrack.velocityX * (lookAheadMs / 1000.0f);
				trackBox.x2 += selectedTrack.velocityX * (lookAheadMs / 1000.0f);
				trackBox.y1 += selectedTrack.velocityY * (lookAheadMs / 1000.0f);
				trackBox.y2 += selectedTrack.velocityY * (lookAheadMs / 1000.0f);
				trackBox.x1 = std::clamp(trackBox.x1, 0.0f, float(frame.width));
				trackBox.x2 = std::clamp(trackBox.x2, trackBox.x1 + 1.0f, float(frame.width));
				trackBox.y1 = std::clamp(trackBox.y1, 0.0f, float(frame.height));
				trackBox.y2 = std::clamp(trackBox.y2, trackBox.y1 + 1.0f, float(frame.height));
				framingTarget = SwitchMotionFramingDetection(trackBox, activeProfile.framingMode, frameSize);
				currentState.targetTrackId = selectedTrack.trackId;
			}
			next = SwitchUpdateMotionCameraState(activeProfile, hasTarget ? &framingTarget : nullptr, frameSize,
							    nowMs, currentState);
			if (hasTarget && activeProfile.framingMode != QStringLiteral("group"))
				next.targetTrackId = selectedTrack.trackId;
			else if (hasTarget)
				next.targetTrackId = -2;
			filter->tracker.SetCurrentTargetTrackId(next.targetTrackId);
			filter->cameraState = next;
			PublishCameraState(filter, filter->cameraState);
		}
		const double trackingMs = double(os_gettime_ns() - trackingStartNs) / 1000000.0;
		const uint64_t nowNs = os_gettime_ns();
		uint64_t previousLogNs = filter->lastDetectionLogNs.load();
		if (previousLogNs == 0 || nowNs - previousLogNs >= kDetectionLogIntervalNs) {
			if (filter->lastDetectionLogNs.compare_exchange_strong(previousLogNs, nowNs)) {
				if (hasTarget) {
					blog(LOG_INFO,
					     "[Switch Motion] detections=%lld tracks=%lld target=%d conf=%.2f focus=(%.0f,%.0f %.0fx%.0f) cam=(%.3f,%.3f) zoom=%.2f pre=%.2f infer=%.2f track=%.2f",
					     static_cast<long long>(mappedDetections.size()), static_cast<long long>(tracks.size()),
					     next.targetTrackId, next.targetConfidence, framingTarget.x1, framingTarget.y1,
					     framingTarget.x2 - framingTarget.x1, framingTarget.y2 - framingTarget.y1,
					     next.camX, next.camY, next.zoom, preprocessingMs, inferenceMs, trackingMs);
				} else {
					blog(LOG_INFO,
					     "[Switch Motion] detections=%lld tracks=%lld no target cam=(%.3f,%.3f) zoom=%.2f pre=%.2f infer=%.2f track=%.2f",
					     static_cast<long long>(mappedDetections.size()), static_cast<long long>(tracks.size()),
					     next.camX, next.camY, next.zoom, preprocessingMs, inferenceMs, trackingMs);
				}
			}
		}

		PublishRuntimeState(filter, activeProfile, next, tracks, runtime, runtimeMessage, preprocessingMs,
				    inferenceMs, trackingMs);
		}
	}
}
} // namespace

void switch_ai_tracker_set_frontend_quiescing(bool quiescing)
{
	gFrontendQuiescing.store(quiescing);
	if (quiescing)
		StopAllInferenceWorkers();
}

void switch_ai_tracker_interrupt_all_inference()
{
	const auto filters = SnapshotRegisteredFilters();
	for (auto *filter : filters) {
		ClearPendingInferenceFrame(filter);
		RequestInferenceTerminate(filter);
	}
}

static const char *switch_ai_tracker_get_name(void *)
{
	return "Switch Motion";
}

static void switch_ai_tracker_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, kMotionEnabled, false);
	obs_data_set_default_bool(settings, kTransformEnabled, false);
	obs_data_set_default_double(settings, kConfidenceThreshold, 0.45);
	obs_data_set_default_int(settings, kTargetClassId, 0);
	obs_data_set_default_double(settings, kMaxZoom, 2.1);
	obs_data_set_default_double(settings, kFramingMargin, 0.18);
	obs_data_set_default_double(settings, kDeadZone, 0.045);
	obs_data_set_default_double(settings, kSmoothing, 0.52);
	obs_data_set_default_int(settings, kHoldMs, 1200);
	obs_data_set_default_string(settings, kBackend, "auto");
	obs_data_set_default_string(settings, kSubjectMode, "auto");
	obs_data_set_default_string(settings, kFramingMode, "upper_body");
	obs_data_set_default_double(settings, kTrackerHighThreshold, 0.55);
	obs_data_set_default_double(settings, kTrackerLowThreshold, 0.20);
	obs_data_set_default_double(settings, kNewTrackThreshold, 0.65);
	obs_data_set_default_int(settings, kTrackBufferFrames, 45);
	obs_data_set_default_int(settings, kLockedTrackGraceFrames, 90);
	obs_data_set_default_int(settings, kAutoSwitchMs, 500);
	obs_data_set_default_double(settings, kPanResponsiveness, 0.64);
	obs_data_set_default_double(settings, kTiltResponsiveness, 0.62);
	obs_data_set_default_double(settings, kZoomResponsiveness, 0.54);
	obs_data_set_default_double(settings, kMaxPanSpeed, 0.85);
	obs_data_set_default_double(settings, kMaxTiltSpeed, 0.75);
	obs_data_set_default_double(settings, kMaxZoomSpeed, 1.25);
	obs_data_set_default_bool(settings, kDebugOverlay, false);
	obs_data_set_default_int(settings, kLockedTrackId, -1);
}

static obs_properties_t *switch_ai_tracker_properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	auto *info = obs_properties_add_text(
		props, "switch_motion_managed_info",
		"Managed from Tools > Switch > Motion. For Motion Shots, this filter samples frames for AI; the final crop is applied to the bound scene item in the Motion workstation, so this preview may not match Program.",
		OBS_TEXT_INFO);
	obs_property_text_set_info_type(info, OBS_TEXT_INFO_NORMAL);

	obs_properties_add_bool(props, kMotionEnabled, "Enable Motion auto-framing");
	obs_properties_add_float_slider(props, kConfidenceThreshold, "Confidence threshold", 0.01, 0.99, 0.01);
	obs_properties_add_int(props, kTargetClassId, "Target class id", 0, 1000, 1);
	obs_properties_add_float_slider(props, kMaxZoom, "Maximum zoom", 1.0, 4.0, 0.05);
	obs_properties_add_float_slider(props, kFramingMargin, "Framing margin", 0.0, 0.75, 0.01);
	obs_properties_add_float_slider(props, kDeadZone, "Dead zone", 0.0, 0.5, 0.01);
	obs_properties_add_float_slider(props, kSmoothing, "Smoothing", 0.01, 1.0, 0.01);
	obs_properties_add_int(props, kHoldMs, "Lost-target hold (ms)", 0, 5000, 50);

	auto *subjectMode = obs_properties_add_list(props, kSubjectMode, "Subject mode", OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(subjectMode, "Auto", "auto");
	obs_property_list_add_string(subjectMode, "Locked", "locked");
	obs_property_list_add_string(subjectMode, "Hold", "hold");
	obs_property_list_add_string(subjectMode, "Off", "off");

	auto *framingMode = obs_properties_add_list(props, kFramingMode, "Framing mode", OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(framingMode, "Face / Headroom", "face_headroom");
	obs_property_list_add_string(framingMode, "Upper Body", "upper_body");
	obs_property_list_add_string(framingMode, "Full Body", "full_body");
	obs_property_list_add_string(framingMode, "Group", "group");

	obs_properties_add_float_slider(props, kTrackerHighThreshold, "Tracker high threshold", 0.01, 0.99, 0.01);
	obs_properties_add_float_slider(props, kTrackerLowThreshold, "Tracker recovery threshold", 0.01, 0.99, 0.01);
	obs_properties_add_float_slider(props, kNewTrackThreshold, "New-track threshold", 0.01, 0.99, 0.01);
	obs_properties_add_int(props, kTrackBufferFrames, "Lost-track buffer (frames)", 1, 180, 1);
	obs_properties_add_int(props, kLockedTrackGraceFrames, "Locked subject grace (frames)", 1, 300, 1);
	obs_properties_add_int(props, kAutoSwitchMs, "Auto switch hold (ms)", 0, 3000, 50);
	obs_properties_add_float_slider(props, kPanResponsiveness, "Pan responsiveness", 0.05, 1.5, 0.01);
	obs_properties_add_float_slider(props, kTiltResponsiveness, "Tilt responsiveness", 0.05, 1.5, 0.01);
	obs_properties_add_float_slider(props, kZoomResponsiveness, "Zoom responsiveness", 0.05, 1.5, 0.01);
	obs_properties_add_float_slider(props, kMaxPanSpeed, "Max pan speed", 0.05, 3.0, 0.01);
	obs_properties_add_float_slider(props, kMaxTiltSpeed, "Max tilt speed", 0.05, 3.0, 0.01);
	obs_properties_add_float_slider(props, kMaxZoomSpeed, "Max zoom speed", 0.05, 4.0, 0.01);
	obs_properties_add_bool(props, kDebugOverlay, "Debug overlay");
	obs_properties_add_int(props, kLockedTrackId, "Locked track id", -1, 1000000, 1);

	auto *backend = obs_properties_add_list(props, kBackend, "Backend", OBS_COMBO_TYPE_LIST,
						OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(backend, "Auto", "auto");
#if defined(__APPLE__)
	obs_property_list_add_string(backend, "CoreML", "coreml");
#elif defined(_WIN32)
	obs_property_list_add_string(backend, "DirectML", "directml");
#endif
	obs_property_list_add_string(backend, "CPU Diagnostics", "cpu");
	obs_properties_add_path(props, kModelPath, "Advanced model path", OBS_PATH_FILE,
				"ONNX/CoreML models (*.onnx *.mlmodel *.mlpackage)", nullptr);
	return props;
}

static void switch_ai_tracker_update(void *data, obs_data_t *settings)
{
	auto *filter = static_cast<tracker_filter_data *>(data);
	if (!filter)
		return;

	SwitchMotionProfile profileForSync;
	QString sourceUuidForSync;
	{
		std::lock_guard<std::mutex> lock(filter->stateMutex);
		const auto previousProfile = filter->profile;
		filter->profile = ProfileFromSettings(settings);
		filter->transformEnabled =
			obs_data_get_int(settings, kTransformSettingsVersion) >= kCurrentTransformSettingsVersion &&
			obs_data_get_bool(settings, kTransformEnabled);
		filter->profile.maxZoom = std::clamp(filter->profile.maxZoom, 1.0f, 4.0f);
		filter->profile.smoothing = std::clamp(filter->profile.smoothing, 0.01f, 1.0f);
		filter->profile.deadZone = std::clamp(filter->profile.deadZone, 0.0f, 0.5f);
		filter->profile.trackerHighThreshold = std::clamp(filter->profile.trackerHighThreshold, 0.01f, 0.99f);
		filter->profile.trackerLowThreshold =
			std::clamp(filter->profile.trackerLowThreshold, 0.01f, filter->profile.trackerHighThreshold);
		filter->profile.newTrackThreshold =
			std::clamp(filter->profile.newTrackThreshold, filter->profile.trackerHighThreshold, 0.99f);
		filter->profile.trackBufferFrames = std::max(1, filter->profile.trackBufferFrames);
		filter->profile.lockedTrackGraceFrames =
			std::max(filter->profile.trackBufferFrames, filter->profile.lockedTrackGraceFrames);
		filter->profile.autoSwitchMs = std::max(0, filter->profile.autoSwitchMs);
		filter->profile.panResponsiveness = std::clamp(filter->profile.panResponsiveness, 0.05f, 1.5f);
		filter->profile.tiltResponsiveness = std::clamp(filter->profile.tiltResponsiveness, 0.05f, 1.5f);
		filter->profile.zoomResponsiveness = std::clamp(filter->profile.zoomResponsiveness, 0.05f, 1.5f);
		filter->profile.maxPanSpeed = std::clamp(filter->profile.maxPanSpeed, 0.05f, 3.0f);
		filter->profile.maxTiltSpeed = std::clamp(filter->profile.maxTiltSpeed, 0.05f, 3.0f);
		filter->profile.maxZoomSpeed = std::clamp(filter->profile.maxZoomSpeed, 0.05f, 4.0f);
		filter->runtimeWarningLogged = false;
		if (previousProfile.id != filter->profile.id) {
			filter->tracker.Reset();
			filter->autoCandidateTrackId = -1;
			filter->autoCandidateSinceMs = 0;
		}
		if (!filter->profile.enabled) {
			filter->cameraState = SwitchMotionCameraState{};
			filter->tracker.Reset();
			filter->autoCandidateTrackId = -1;
			filter->autoCandidateSinceMs = 0;
			PublishCameraState(filter, filter->cameraState);
		}
		profileForSync = filter->profile;
		sourceUuidForSync = filter->sourceUuid;
	}
	SwitchMotionPublishFilterProfileUpdate(profileForSync, sourceUuidForSync);
}

static void *switch_ai_tracker_create(obs_data_t *settings, obs_source_t *context)
{
	auto *filter = new tracker_filter_data();
	filter->context = context;
	switch_ai_tracker_update(filter, settings);
	filter->inferenceThread = std::thread(InferenceLoop, filter);
	RegisterFilter(filter);
	return filter;
}

static void switch_ai_tracker_destroy(void *data)
{
	auto *filter = static_cast<tracker_filter_data *>(data);
	if (!filter)
		return;
	blog(LOG_INFO, "[Switch Motion] filter_destroy");
	UnregisterFilter(filter);
	StopInferenceWorker(filter);
	if (filter->scaler) {
		video_scaler_destroy(filter->scaler);
		filter->scaler = nullptr;
	}
	{
		std::lock_guard<std::mutex> lock(filter->sourceMutex);
		if (filter->parentWeak) {
			obs_weak_source_release(filter->parentWeak);
			filter->parentWeak = nullptr;
		}
	}
	delete filter;
}

static void switch_ai_tracker_filter_add(void *data, obs_source_t *parent)
{
	auto *filter = static_cast<tracker_filter_data *>(data);
	if (!filter)
		return;

	blog(LOG_INFO, "[Switch Motion] filter_add parent='%s' flags=0x%x",
	     parent ? obs_source_get_name(parent) : "(null)", parent ? obs_source_get_output_flags(parent) : 0);

	filter->sourceActive.store(true);
	filter->lastQueuedNs.store(0);

	std::lock_guard<std::mutex> lock(filter->sourceMutex);
	if (filter->parentWeak)
		obs_weak_source_release(filter->parentWeak);
	filter->parentWeak = parent ? obs_source_get_weak_source(parent) : nullptr;
	{
		std::lock_guard<std::mutex> stateLock(filter->stateMutex);
		filter->sourceUuid = parent && obs_source_get_uuid(parent) ? QString::fromUtf8(obs_source_get_uuid(parent)) : QString();
		filter->sourceName = parent && obs_source_get_name(parent) ? QString::fromUtf8(obs_source_get_name(parent)) : QString();
	}
}

static void switch_ai_tracker_filter_remove(void *data, obs_source_t *)
{
	auto *filter = static_cast<tracker_filter_data *>(data);
	if (!filter)
		return;

	blog(LOG_INFO, "[Switch Motion] filter_remove");
	filter->sourceActive.store(false);
	ClearPendingInferenceFrame(filter);
	RequestInferenceTerminate(filter);

	std::lock_guard<std::mutex> lock(filter->sourceMutex);
	if (filter->parentWeak) {
		obs_weak_source_release(filter->parentWeak);
		filter->parentWeak = nullptr;
	}
	{
		std::lock_guard<std::mutex> stateLock(filter->stateMutex);
		filter->sourceUuid.clear();
		filter->sourceName.clear();
		filter->tracker.Reset();
		filter->cameraState = SwitchMotionCameraState{};
		if (!gFrontendQuiescing.load())
			PublishCameraState(filter, filter->cameraState);
	}
}

static void switch_ai_tracker_activate(void *data)
{
	auto *filter = static_cast<tracker_filter_data *>(data);
	if (!filter)
		return;

	filter->sourceActive.store(true);
	filter->lastQueuedNs.store(0);
	blog(LOG_INFO, "[Switch Motion] filter_activate");
}

static void switch_ai_tracker_deactivate(void *data)
{
	auto *filter = static_cast<tracker_filter_data *>(data);
	if (!filter)
		return;

	blog(LOG_INFO, "[Switch Motion] filter_deactivate");
	filter->sourceActive.store(false);
	ClearPendingInferenceFrame(filter);
	RequestInferenceTerminate(filter);
}

static void switch_ai_tracker_video_tick(void *data, float)
{
	auto *filter = static_cast<tracker_filter_data *>(data);
	if (!filter)
		return;

	if (gFrontendQuiescing.load() || !filter->sourceActive.load())
		return;

	WatchdogInferenceRun(filter);

	SwitchMotionProfile profile;
	SwitchMotionCameraState previous;
	{
		std::lock_guard<std::mutex> lock(filter->stateMutex);
		profile = filter->profile;
		previous = filter->cameraState;
	}

	if (!profile.enabled)
		return;

	if (!filter->runtimeWarningLogged && !profile.modelPath.isEmpty()) {
		blog(LOG_INFO, "[Switch Motion] filter active for profile '%s' using %s", profile.id.toUtf8().constData(),
		     profile.modelPath.toUtf8().constData());
		filter->runtimeWarningLogged = true;
	}

	const qint64 nowMs = static_cast<qint64>(os_gettime_ns() / 1000000ULL);
	const QSize frameSize(1, 1);
	const auto next = SwitchUpdateMotionCameraState(profile, nullptr, frameSize, nowMs, previous);

	{
		std::lock_guard<std::mutex> lock(filter->stateMutex);
		filter->cameraState = next;
		PublishCameraState(filter, filter->cameraState);
	}
}

static struct obs_source_frame *switch_ai_tracker_filter_video(void *data, struct obs_source_frame *frame)
{
	auto *filter = static_cast<tracker_filter_data *>(data);
	if (!filter || !frame)
		return frame;
	if (!filter->sourceActive.load())
		return frame;

	SwitchMotionProfile profile;
	{
		std::lock_guard<std::mutex> lock(filter->stateMutex);
		profile = filter->profile;
	}
	if (!profile.enabled || gFrontendQuiescing.load())
		return frame;

	WatchdogInferenceRun(filter);
	QueueInferenceFrame(filter, profile, frame);
	return frame;
}

static void switch_ai_tracker_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	auto *filter = static_cast<tracker_filter_data *>(data);
	if (!filter)
		return;

	if (gFrontendQuiescing.load()) {
		obs_source_skip_video_filter(filter->context);
		return;
	}

	if (!filter->sourceActive.load()) {
		obs_source_skip_video_filter(filter->context);
		return;
	}

	const float zoom = std::max(1.0f, filter->currentZoom.load());
	const float camX = ClampCameraOffset(filter->currentCamX.load(), zoom);
	const float camY = ClampCameraOffset(filter->currentCamY.load(), zoom);

	obs_source_t *target = obs_filter_get_target(filter->context);
	obs_source_t *parent = obs_filter_get_parent(filter->context);
	if (!target || !parent) {
		obs_source_skip_video_filter(filter->context);
		return;
	}

	if (!filter->transformEnabled ||
	    (zoom <= 1.0001f && std::abs(camX) < 0.0001f && std::abs(camY) < 0.0001f)) {
		obs_source_skip_video_filter(filter->context);
		return;
	}

	const float width = static_cast<float>(std::max<uint32_t>(1, obs_source_get_width(target)));
	const float height = static_cast<float>(std::max<uint32_t>(1, obs_source_get_height(target)));
	const float cropWidth = width / zoom;
	const float cropHeight = height / zoom;
	const float minCenterX = cropWidth * 0.5f;
	const float maxCenterX = width - minCenterX;
	const float minCenterY = cropHeight * 0.5f;
	const float maxCenterY = height - minCenterY;
	const float cropCenterX = std::clamp(width * (0.5f + camX), minCenterX, maxCenterX);
	const float cropCenterY = std::clamp(height * (0.5f + camY), minCenterY, maxCenterY);

	gs_matrix_push();

	struct vec3 center;
	vec3_set(&center, width * 0.5f, height * 0.5f, 0.0f);
	struct vec3 scale;
	vec3_set(&scale, zoom, zoom, 1.0f);
	struct vec3 offset;
	vec3_set(&offset, -cropCenterX, -cropCenterY, 0.0f);

	gs_matrix_translate(&center);
	gs_matrix_scale(&scale);
	gs_matrix_translate(&offset);
	RenderMotionFilterTarget(target, parent);

	gs_matrix_pop();
}

struct obs_source_info switch_ai_tracker_info = {
	.id = "switch_ai_tracker",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = switch_ai_tracker_get_name,
	.create = switch_ai_tracker_create,
	.destroy = switch_ai_tracker_destroy,
	.get_defaults = switch_ai_tracker_defaults,
	.get_properties = switch_ai_tracker_properties,
	.update = switch_ai_tracker_update,
	.activate = switch_ai_tracker_activate,
	.deactivate = switch_ai_tracker_deactivate,
	.video_tick = switch_ai_tracker_video_tick,
	.video_render = switch_ai_tracker_video_render,
	.filter_video = switch_ai_tracker_filter_video,
	.filter_remove = switch_ai_tracker_filter_remove,
	.filter_add = switch_ai_tracker_filter_add,
};
