#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <media-io/video-scaler.h>
#include <obs-module.h>

#include "switch-motion-manager.hpp"

struct SwitchMotionFrameSample {
	enum video_format format = VIDEO_FORMAT_NONE;
	uint32_t width = 0;
	uint32_t height = 0;
	uint64_t timestamp = 0;
	bool fullRange = false;
	bool flip = false;
	uint32_t linesize[MAX_AV_PLANES] = {};
	std::vector<uint8_t> planes[MAX_AV_PLANES];
};

struct tracker_filter_data {
	obs_source_t *context = nullptr;
	SwitchMotionProfile profile;
	SwitchMotionCameraState cameraState;
	SwitchMotionTracker tracker;
	QString sourceUuid;
	QString sourceName;
	int autoCandidateTrackId = -1;
	qint64 autoCandidateSinceMs = 0;
	std::mutex stateMutex;
	std::mutex inferenceMutex;
	std::condition_variable inferenceCv;
	std::thread inferenceThread;
	video_scaler_t *scaler = nullptr;
	video_scale_info scalerSrcInfo = {};
	video_scale_info scalerDstInfo = {};
	std::vector<uint8_t> scalerBuffer;
	std::mutex sourceMutex;
	obs_weak_source_t *parentWeak = nullptr;
	std::mutex runOptionsMutex;
	void *activeRunOptions = nullptr;
	uint64_t activeRunStartNs = 0;
	bool activeRunTerminateRequested = false;
	bool activeRunIntentionalTerminate = false;
	SwitchMotionFrameSample pendingFrame;
	std::atomic<float> currentCamX{0.0f};
	std::atomic<float> currentCamY{0.0f};
	std::atomic<float> currentZoom{1.0f};
	std::atomic<bool> targetActive{false};
	std::atomic<float> targetConfidence{0.0f};
	std::atomic<int> currentTargetTrackId{-1};
	std::atomic<uint64_t> droppedFrames{0};
	std::atomic<uint64_t> lastQueuedNs{0};
	std::atomic<uint64_t> lastFrameLogNs{0};
	std::atomic<uint64_t> lastDetectionLogNs{0};
	std::atomic<uint64_t> lastRuntimePublishNs{0};
	std::atomic<uint64_t> lastWatchdogLogNs{0};
	std::atomic<uint64_t> lastSampleFailureLogNs{0};
	std::atomic<bool> sourceActive{false};
	bool stopInference = false;
	bool hasPendingFrame = false;
	bool inferenceReady = false;
	bool runtimeErrorLogged = false;
	bool runtimeWarningLogged = false;
	bool transformEnabled = false;
};

extern struct obs_source_info switch_ai_tracker_info;

void switch_ai_tracker_set_frontend_quiescing(bool quiescing);
void switch_ai_tracker_interrupt_all_inference();
