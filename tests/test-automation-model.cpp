#define CATCH_CONFIG_MAIN
#include <catch_amalgamated.hpp>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QtNetwork/qudpsocket.h>

#include <cmath>
#include <functional>
#include <iterator>

#include "switch-automation-engine.hpp"
#include "switch-automation-model.hpp"
#include "switch-motion-manager.hpp"
#include "switch-osc.hpp"

namespace {
QCoreApplication *EnsureCoreApplication()
{
	if (auto *app = QCoreApplication::instance())
		return app;

	static int argc = 1;
	static char appName[] = "switch-automation-tests";
	static char *argv[] = {appName, nullptr};
	static QCoreApplication *app = new QCoreApplication(argc, argv);
	return app;
}

quint16 ReserveUdpPort()
{
	EnsureCoreApplication();
	QUdpSocket socket;
	REQUIRE(socket.bind(QHostAddress::LocalHost, 0));
	const quint16 port = socket.localPort();
	socket.close();
	return port;
}

bool WaitForCondition(const std::function<bool()> &condition, int timeoutMs = 2500)
{
	EnsureCoreApplication();
	QElapsedTimer timer;
	timer.start();
	while (timer.elapsed() < timeoutMs) {
		QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
		if (condition())
			return true;
	}
	QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
	return condition();
}

bool EventLogContains(const QVector<SwitchAutomationEventRecord> &events, const QString &needle)
{
	for (const auto &event : events) {
		if (event.message.contains(needle))
			return true;
	}
	return false;
}
} // namespace

TEST_CASE("legacy automation macros migrate into document segments", "[automation][migration]")
{
	obs_data_t *root = obs_data_create();
	obs_data_array_t *macros = obs_data_array_create();
	obs_data_t *legacy = obs_data_create();
	obs_data_set_string(legacy, "id", "macro-1");
	obs_data_set_string(legacy, "name", "Legacy Timer");
	obs_data_set_string(legacy, "run_mode", "repeat");
	obs_data_set_int(legacy, "interval_ms", 2500);
	obs_data_set_string(legacy, "trigger_type", "timer");
	obs_data_set_bool(legacy, "desired_state", true);
	obs_data_set_string(legacy, "action_type", "set_variable");
	obs_data_set_string(legacy, "action_value_key", "mode");
	obs_data_set_string(legacy, "action_value", "live");
	obs_data_array_push_back(macros, legacy);
	obs_data_set_array(root, "macros", macros);

	const auto document = SwitchAutomationDocumentFromObsData(root);
	REQUIRE(document.settingsVersion == 2);
	REQUIRE(document.macros.size() == 1);

	const auto &macro = document.macros.front();
	CHECK(macro.id == "macro-1");
	CHECK(macro.name == "Legacy Timer");
	CHECK(macro.runPolicy == "repeat");
	CHECK(macro.repeatIntervalMs == 2500);
	REQUIRE(macro.conditions.size() == 1);
	CHECK(macro.conditions.front().typeId == "timer");
	CHECK(macro.conditions.front().config.value("intervalMs").toInt() == 2500);
	REQUIRE(macro.actions.size() == 1);
	CHECK(macro.actions.front().typeId == "set_variable");
	CHECK(macro.actions.front().config.value("valueKey").toString() == "mode");
	CHECK(macro.actions.front().config.value("value").toString() == "live");

	obs_data_release(legacy);
	obs_data_array_release(macros);
	obs_data_release(root);
}

TEST_CASE("automation document round-trips full macro state", "[automation][serialization]")
{
	SwitchAutomationDocument document;
	document.engineState = "running";
	document.variables.insert("profile", "vertical");

	SwitchAutomationMacro macro;
	macro.id = "macro-2";
	macro.name = "Composite Macro";
	macro.group = "Production";
	macro.enabled = true;
	macro.paused = false;
	macro.runPolicy = "on_match";
	macro.startupPolicy = "run_immediately";
	macro.repeatIntervalMs = 8000;
	macro.hotkeys.insert("primary", "Shift+F1");
	macro.dockConfig.insert("registerDock", true);
	macro.properties.insert("lastPath", "/tmp/out.mp4");
	macro.lastConditionMatched = true;
	macro.lastExecutionMs = 42;

	SwitchAutomationSegment conditionA;
	conditionA.id = "condition-a";
	conditionA.typeId = "scene";
	conditionA.label = "Program Scene";
	conditionA.logicOp = "and";
	conditionA.config.insert("sceneName", "Cam 1");
	macro.conditions.push_back(conditionA);

	SwitchAutomationSegment conditionB;
	conditionB.id = "condition-b";
	conditionB.typeId = "variable";
	conditionB.label = "Variable Check";
	conditionB.logicOp = "or";
	conditionB.config.insert("valueKey", "profile");
	conditionB.config.insert("value", "vertical");
	macro.conditions.push_back(conditionB);

	SwitchAutomationSegment actionA;
	actionA.id = "action-a";
	actionA.typeId = "wait";
	actionA.durationMs = 1500;
	actionA.config.insert("durationMs", 1500);
	macro.actions.push_back(actionA);

	SwitchAutomationSegment actionB;
	actionB.id = "action-b";
	actionB.typeId = "http_post";
	actionB.config.insert("url", "https://example.com/hook");
	actionB.config.insert("value", "{\"ok\":true}");
	actionB.config.insert("contentType", "application/json");
	macro.actions.push_back(actionB);

	document.macros.push_back(macro);

	SwitchAutomationQueue queue;
	queue.id = "queue-1";
	queue.name = "Scene Queue";
	queue.mode = "sequence";
	queue.actionSegmentIds = {"action-a", "action-b"};
	queue.nextIndex = 1;
	document.queues.push_back(queue);

	SwitchAutomationConnection connection;
	connection.id = "connection-1";
	connection.typeId = "http";
	connection.name = "Webhook";
	connection.status = "idle";
	connection.config.insert("url", "https://example.com/hook");
	document.connections.push_back(connection);

	SwitchAutomationDockPreset dock;
	dock.id = "dock-1";
	dock.name = "Macro Dock";
	dock.macroId = macro.id;
	dock.registerDock = true;
	dock.hasStatusLabel = true;
	document.dockPresets.push_back(dock);

	obs_data_t *saved = SwitchAutomationDocumentToObsData(document);
	const auto loaded = SwitchAutomationDocumentFromObsData(saved);
	obs_data_release(saved);

	REQUIRE(loaded.macros.size() == 1);
	const auto &loadedMacro = loaded.macros.front();
	CHECK(loaded.engineState == "running");
	CHECK(loaded.variables.value("profile") == "vertical");
	CHECK(loadedMacro.name == macro.name);
	CHECK(loadedMacro.group == macro.group);
	CHECK(loadedMacro.runPolicy == "on_match");
	CHECK(loadedMacro.startupPolicy == "run_immediately");
	CHECK(loadedMacro.hotkeys.value("primary").toString() == "Shift+F1");
	CHECK(loadedMacro.dockConfig.value("registerDock").toBool());
	CHECK(loadedMacro.properties.value("lastPath") == "/tmp/out.mp4");
	REQUIRE(loadedMacro.conditions.size() == 2);
	CHECK(loadedMacro.conditions[1].logicOp == "or");
	REQUIRE(loadedMacro.actions.size() == 2);
	CHECK(loadedMacro.actions[1].typeId == "http_post");
	REQUIRE(loaded.queues.size() == 1);
	CHECK(loaded.queues.front().actionSegmentIds.size() == 2);
	REQUIRE(loaded.connections.size() == 1);
	CHECK(loaded.connections.front().config.value("url").toString() == "https://example.com/hook");
	REQUIRE(loaded.dockPresets.size() == 1);
	CHECK(loaded.dockPresets.front().registerDock);
}

TEST_CASE("automation macros round-trip as standalone objects", "[automation][serialization]")
{
	SwitchAutomationMacro macro;
	macro.id = "macro-standalone";
	macro.name = "Standalone";

	SwitchAutomationSegment condition;
	condition.id = "condition-standalone";
	condition.typeId = "file";
	condition.config.insert("path", "/tmp/example.txt");
	macro.conditions.push_back(condition);

	SwitchAutomationSegment action;
	action.id = "action-standalone";
	action.typeId = "remove_variable";
	action.config.insert("valueKey", "profile");
	macro.actions.push_back(action);

	obs_data_t *data = SwitchAutomationMacroToObsData(macro);
	const auto loaded = SwitchAutomationMacroFromObsData(data);
	obs_data_release(data);

	CHECK(loaded.id == macro.id);
	CHECK(loaded.conditions.front().typeId == "file");
	CHECK(loaded.actions.front().typeId == "remove_variable");
}

TEST_CASE("automation templates resolve document and macro variables", "[automation][variables]")
{
	QHash<QString, QString> variables;
	variables.insert("profile", "vertical");
	variables.insert("scene", "Gameplay");

	QHash<QString, QString> properties;
	properties.insert("outputPath", "/tmp/out.mp4");
	properties.insert("macroName", "Recorder");

	const QString resolved = SwitchResolveAutomationTemplate(
		"Profile=${profile}; Scene=${scene}; Path=${macro.outputPath}; Macro=${macro.macroName}", variables,
		properties);

	CHECK(resolved == "Profile=vertical; Scene=Gameplay; Path=/tmp/out.mp4; Macro=Recorder");
}

TEST_CASE("motion parses YOLO26 end-to-end detections", "[motion][yolo26]")
{
	const float output[] = {
		10.0f, 20.0f, 110.0f, 220.0f, 0.30f, 0.0f,
		40.0f, 50.0f, 180.0f, 300.0f, 0.88f, 0.0f,
		15.0f, 25.0f, 90.0f, 120.0f, 0.92f, 2.0f,
	};
	const auto detections = SwitchParseYolo26Detections(output, std::size(output), {1, 3, 6}, 0.45f, 0);
	REQUIRE(detections.size() == 1);
	CHECK(detections.front().x1 == 40.0f);
	CHECK(detections.front().confidence == 0.88f);

	SwitchMotionDetection selected;
	REQUIRE(SwitchSelectPrimaryDetection(detections, &selected));
	CHECK(selected.x2 == 180.0f);
}

TEST_CASE("motion parses transposed YOLO26 end-to-end detections", "[motion][yolo26]")
{
	const float output[] = {
		10.0f, 40.0f,
		20.0f, 50.0f,
		110.0f, 180.0f,
		220.0f, 300.0f,
		0.30f, 0.88f,
		0.0f, 0.0f,
	};
	const auto detections = SwitchParseYolo26Detections(output, std::size(output), {1, 6, 2}, 0.45f, 0);
	REQUIRE(detections.size() == 1);
	CHECK(detections.front().x1 == 40.0f);
	CHECK(detections.front().y1 == 50.0f);
	CHECK(detections.front().x2 == 180.0f);
	CHECK(detections.front().y2 == 300.0f);
	CHECK(detections.front().confidence == 0.88f);
}

TEST_CASE("motion tracker keeps stable IDs for two people across frames", "[motion][tracking]")
{
	SwitchMotionProfile profile = SwitchDefaultMotionProfile();
	SwitchMotionTracker tracker;

	QVector<SwitchMotionDetection> frameA;
	frameA.push_back({100.0f, 200.0f, 220.0f, 520.0f, 0.92f, 0});
	frameA.push_back({620.0f, 210.0f, 760.0f, 530.0f, 0.88f, 0});
	auto tracks = tracker.Update(frameA, profile, QSize(1000, 1000), 1000, "source-a");
	REQUIRE(tracks.size() == 2);
	const int leftId = tracks[0].trackId;
	const int rightId = tracks[1].trackId;

	QVector<SwitchMotionDetection> frameB;
	frameB.push_back({118.0f, 202.0f, 238.0f, 522.0f, 0.90f, 0});
	frameB.push_back({600.0f, 212.0f, 742.0f, 532.0f, 0.91f, 0});
	tracks = tracker.Update(frameB, profile, QSize(1000, 1000), 1100, "source-a");
	REQUIRE(tracks.size() == 2);
	CHECK(tracks[0].trackId == leftId);
	CHECK(tracks[1].trackId == rightId);
	CHECK(tracks[0].state == "active");
	CHECK(tracks[1].state == "active");
}

TEST_CASE("motion tracker recovers IDs through low-confidence detections", "[motion][tracking]")
{
	SwitchMotionProfile profile = SwitchDefaultMotionProfile();
	SwitchMotionTracker tracker;

	auto tracks = tracker.Update({{300.0f, 200.0f, 460.0f, 580.0f, 0.90f, 0}}, profile, QSize(1000, 1000), 1000,
				     "source-a");
	REQUIRE(tracks.size() == 1);
	const int trackId = tracks.front().trackId;

	tracks = tracker.Update({{310.0f, 202.0f, 470.0f, 582.0f, 0.28f, 0}}, profile, QSize(1000, 1000), 1100,
				"source-a");
	REQUIRE(tracks.size() == 1);
	CHECK(tracks.front().trackId == trackId);
	CHECK(tracks.front().state == "recovered");
	CHECK(tracks.front().missedFrames == 0);
}

TEST_CASE("motion locked subject survives brief occlusion", "[motion][tracking]")
{
	SwitchMotionProfile profile = SwitchDefaultMotionProfile();
	SwitchMotionTracker tracker;
	auto tracks = tracker.Update({{300.0f, 200.0f, 460.0f, 580.0f, 0.90f, 0}}, profile, QSize(1000, 1000), 1000,
				     "source-a");
	REQUIRE(tracks.size() == 1);
	profile.subjectMode = "locked";
	profile.lockedTrackId = tracks.front().trackId;

	for (int frame = 0; frame < 20; frame++)
		tracks = tracker.Update({}, profile, QSize(1000, 1000), 1100 + frame * 33, "source-a");

	SwitchMotionTrack selected;
	SwitchMotionCameraState previous;
	previous.targetTrackId = profile.lockedTrackId;
	REQUIRE_FALSE(SwitchMotionSelectTargetTrack(tracks, profile, previous, QSize(1000, 1000), 1800, nullptr, nullptr,
						   &selected));
	REQUIRE_FALSE(tracks.isEmpty());
	CHECK(tracks.front().trackId == profile.lockedTrackId);
	CHECK(tracks.front().missedFrames == 20);
}

TEST_CASE("motion auto target does not switch from a single-frame spike", "[motion][tracking]")
{
	SwitchMotionProfile profile = SwitchDefaultMotionProfile();
	profile.autoSwitchMs = 500;
	SwitchMotionTracker tracker;
	auto tracks = tracker.Update({{420.0f, 200.0f, 560.0f, 580.0f, 0.90f, 0}}, profile, QSize(1000, 1000), 1000,
				     "source-a");
	REQUIRE(tracks.size() == 1);
	const int firstId = tracks.front().trackId;

	tracks = tracker.Update({{420.0f, 200.0f, 560.0f, 580.0f, 0.82f, 0},
				 {100.0f, 180.0f, 310.0f, 650.0f, 0.99f, 0}},
				profile, QSize(1000, 1000), 1100, "source-a");
	SwitchMotionCameraState previous;
	previous.targetTrackId = firstId;
	previous.targetActive = true;
	int candidateId = -1;
	qint64 candidateSinceMs = 0;
	SwitchMotionTrack selected;
	REQUIRE(SwitchMotionSelectTargetTrack(tracks, profile, previous, QSize(1000, 1000), 1100, &candidateId,
					      &candidateSinceMs, &selected));
	CHECK(selected.trackId == firstId);
}

TEST_CASE("motion target selection favors continuity over small confidence jumps", "[motion][tracking]")
{
	QVector<SwitchMotionDetection> detections;
	SwitchMotionDetection nearby;
	nearby.x1 = 555.0f;
	nearby.y1 = 400.0f;
	nearby.x2 = 635.0f;
	nearby.y2 = 560.0f;
	nearby.confidence = 0.78f;
	nearby.classId = 0;
	detections.push_back(nearby);

	SwitchMotionDetection jump;
	jump.x1 = 70.0f;
	jump.y1 = 390.0f;
	jump.x2 = 150.0f;
	jump.y2 = 560.0f;
	jump.confidence = 0.84f;
	jump.classId = 0;
	detections.push_back(jump);

	SwitchMotionCameraState previous;
	previous.targetActive = true;
	previous.camX = 0.1f;
	previous.camY = 0.0f;

	SwitchMotionDetection selected;
	REQUIRE(SwitchSelectPrimaryDetection(detections, previous, QSize(1000, 1000), &selected));
	CHECK(selected.x1 == Catch::Approx(nearby.x1));
	CHECK(selected.confidence == Catch::Approx(nearby.confidence));
}

TEST_CASE("motion target selection holds through low-confidence far outliers", "[motion][tracking]")
{
	QVector<SwitchMotionDetection> detections;
	SwitchMotionDetection outlier;
	outlier.x1 = 20.0f;
	outlier.y1 = 390.0f;
	outlier.x2 = 140.0f;
	outlier.y2 = 560.0f;
	outlier.confidence = 0.54f;
	outlier.classId = 0;
	detections.push_back(outlier);

	SwitchMotionCameraState previous;
	previous.targetActive = true;
	previous.targetCamX = 0.18f;
	previous.targetCamY = 0.0f;
	previous.targetZoom = 2.0f;
	previous.targetConfidence = 0.94f;

	SwitchMotionDetection selected;
	CHECK_FALSE(SwitchSelectPrimaryDetection(detections, previous, QSize(1000, 1000), &selected));
}

TEST_CASE("motion camera state smooths target and returns toward neutral", "[motion][camera]")
{
	SwitchMotionProfile profile = SwitchDefaultMotionProfile();
	profile.enabled = true;
	profile.smoothing = 0.5f;
	profile.maxZoom = 2.0f;
	profile.deadZone = 0.0f;
	profile.holdMs = 100;

	SwitchMotionDetection detection;
	detection.x1 = 700.0f;
	detection.y1 = 420.0f;
	detection.x2 = 780.0f;
	detection.y2 = 580.0f;
	detection.confidence = 0.9f;
	detection.classId = 0;

	auto state = SwitchUpdateMotionCameraState(profile, &detection, QSize(1000, 1000), 1000, {});
	CHECK(state.targetActive);
	CHECK(state.zoom > 1.0f);
	CHECK(state.camX > 0.0f);

	state = SwitchUpdateMotionCameraState(profile, nullptr, QSize(1000, 1000), 1200, state);
	CHECK_FALSE(state.targetActive);
	CHECK(state.zoom >= 1.0f);
	CHECK(state.camX < 0.25f);
}

TEST_CASE("motion camera state clamps pan to avoid black frame edges", "[motion][camera]")
{
	SwitchMotionProfile profile = SwitchDefaultMotionProfile();
	profile.enabled = true;
	profile.smoothing = 1.0f;
	profile.maxZoom = 1.75f;
	profile.deadZone = 0.0f;

	SwitchMotionDetection detection;
	detection.x1 = 930.0f;
	detection.y1 = 460.0f;
	detection.x2 = 990.0f;
	detection.y2 = 540.0f;
	detection.confidence = 0.95f;
	detection.classId = 0;

	SwitchMotionCameraState state;
	for (int frame = 0; frame < 60; frame++) {
		state = SwitchUpdateMotionCameraState(profile, &detection, QSize(1000, 1000), 1000 + frame * 33, state);
		const float maxSafeOffset = (state.zoom - 1.0f) / (2.0f * state.zoom);
		CHECK(std::abs(state.camX) <= maxSafeOffset + 0.0001f);
		CHECK(std::abs(state.camY) <= maxSafeOffset + 0.0001f);
	}
	const float maxSafeOffset = (state.zoom - 1.0f) / (2.0f * state.zoom);
	CHECK(state.zoom <= profile.maxZoom);
	CHECK(state.zoom > 1.0f);
	CHECK(std::abs(state.camX) <= maxSafeOffset + 0.0001f);
	CHECK(std::abs(state.camY) <= maxSafeOffset + 0.0001f);
}

TEST_CASE("motion camera state eases continuously between inference updates", "[motion][camera]")
{
	SwitchMotionProfile profile = SwitchDefaultMotionProfile();
	profile.enabled = true;
	profile.smoothing = 0.52f;
	profile.maxZoom = 2.1f;
	profile.deadZone = 0.0f;

	SwitchMotionDetection detection;
	detection.x1 = 690.0f;
	detection.y1 = 260.0f;
	detection.x2 = 850.0f;
	detection.y2 = 540.0f;
	detection.confidence = 0.93f;
	detection.classId = 0;

	auto state = SwitchUpdateMotionCameraState(profile, &detection, QSize(1000, 1000), 1000, {});
	const float firstCamX = state.camX;
	const float firstZoom = state.zoom;
	CHECK(firstCamX > 0.0f);
	CHECK(firstZoom > 1.0f);

	state = SwitchUpdateMotionCameraState(profile, nullptr, QSize(1000, 1000), 1033, state);
	CHECK(state.targetActive);
	CHECK(state.camX > firstCamX);
	CHECK(state.zoom > firstZoom);
	CHECK(state.camX - firstCamX < 0.1f);
	CHECK(state.zoom - firstZoom < 0.35f);
}

TEST_CASE("motion camera state damps abrupt detection jumps", "[motion][camera]")
{
	SwitchMotionProfile profile = SwitchDefaultMotionProfile();
	profile.enabled = true;
	profile.smoothing = 0.52f;
	profile.maxZoom = 2.1f;
	profile.deadZone = 0.0f;

	SwitchMotionDetection left;
	left.x1 = 50.0f;
	left.y1 = 350.0f;
	left.x2 = 150.0f;
	left.y2 = 650.0f;
	left.confidence = 0.92f;
	left.classId = 0;

	SwitchMotionDetection right = left;
	right.x1 = 850.0f;
	right.x2 = 950.0f;

	auto state = SwitchUpdateMotionCameraState(profile, &left, QSize(1000, 1000), 1000, {});
	const float firstTargetX = state.targetCamX;
	const float firstCamX = state.camX;
	REQUIRE(firstTargetX < -0.10f);
	REQUIRE(firstTargetX > -0.20f);

	state = SwitchUpdateMotionCameraState(profile, &right, QSize(1000, 1000), 1033, state);
	CHECK(state.targetCamX < -0.05f);
	CHECK(state.camX - firstCamX < 0.08f);
}

TEST_CASE("motion camera controller respects velocity and zoom caps", "[motion][camera]")
{
	SwitchMotionProfile profile = SwitchDefaultMotionProfile();
	profile.enabled = true;
	profile.deadZone = 0.0f;
	profile.maxZoom = 2.2f;
	profile.maxPanSpeed = 0.35f;
	profile.maxZoomSpeed = 0.50f;

	SwitchMotionDetection detection;
	detection.x1 = 860.0f;
	detection.y1 = 220.0f;
	detection.x2 = 960.0f;
	detection.y2 = 520.0f;
	detection.confidence = 0.95f;
	detection.classId = 0;

	SwitchMotionCameraState state;
	for (int frame = 0; frame < 10; frame++) {
		const auto previous = state;
		state = SwitchUpdateMotionCameraState(profile, &detection, QSize(1000, 1000), 1000 + frame * 33, state);
		CHECK(std::abs(state.panVelocity) <= profile.maxPanSpeed + 0.001f);
		CHECK(std::abs(state.zoomVelocity) <= profile.maxZoomSpeed + 0.001f);
		CHECK(state.zoom <= profile.maxZoom);
		if (frame > 0)
			CHECK(std::abs(state.camX - previous.camX) <= profile.maxPanSpeed * 0.04f + 0.001f);
	}
}

TEST_CASE("motion shot loop interpolation is deterministic", "[motion][shots]")
{
	SwitchMotionProfile profile = SwitchDefaultMotionProfile();
	SwitchMotionShot shot = SwitchDefaultMotionShot();
	shot.startPanX = -0.10f;
	shot.startPanY = 0.02f;
	shot.startZoom = 1.0f;
	shot.endPanX = 0.10f;
	shot.endPanY = -0.02f;
	shot.endZoom = 1.4f;
	shot.durationMs = 10000;
	shot.loopMode = "restart";
	shot.easing = "linear";

	const auto state = SwitchEvaluateMotionShotLoop(shot, profile, 5000);
	CHECK(state.camX == Catch::Approx(0.0f).margin(0.0001f));
	CHECK(state.camY == Catch::Approx(0.0f).margin(0.0001f));
	CHECK(state.zoom == Catch::Approx(1.2f).margin(0.0001f));
}

TEST_CASE("motion shot free run resumes mid loop after hidden time", "[motion][shots]")
{
	SwitchMotionShot shot = SwitchDefaultMotionShot();
	shot.playbackMode = "free_run";
	shot.phaseAnchorMs = 1000;
	shot.durationMs = 10000;

	CHECK(SwitchMotionShotPhaseMs(shot, 3500) == 2500);
	CHECK(SwitchMotionShotPhaseMs(shot, 13500) == 2500);
}

TEST_CASE("motion shot playback restart and pause phases are deterministic", "[motion][shots]")
{
	SwitchMotionShot restart = SwitchDefaultMotionShot();
	restart.playbackMode = "restart_on_program";
	restart.phaseAnchorMs = 5000;
	restart.durationMs = 10000;
	CHECK(SwitchMotionShotPhaseMs(restart, 5000) == 0);
	CHECK(SwitchMotionShotPhaseMs(restart, 7250) == 2250);

	SwitchMotionShot paused = SwitchDefaultMotionShot();
	paused.playbackMode = "pause_when_hidden";
	paused.phaseAnchorMs = 0;
	paused.pausedPhaseMs = 4200;
	paused.durationMs = 10000;
	CHECK(SwitchMotionShotPhaseMs(paused, 999999) == 4200);
}

TEST_CASE("motion shot ping pong loop reverses direction", "[motion][shots]")
{
	SwitchMotionProfile profile = SwitchDefaultMotionProfile();
	SwitchMotionShot shot = SwitchDefaultMotionShot();
	shot.startPanX = -0.10f;
	shot.endPanX = 0.10f;
	shot.startZoom = 1.2f;
	shot.endZoom = 1.2f;
	shot.durationMs = 10000;
	shot.loopMode = "ping_pong";
	shot.easing = "linear";

	const auto firstHalf = SwitchEvaluateMotionShotLoop(shot, profile, 2500);
	const auto secondHalf = SwitchEvaluateMotionShotLoop(shot, profile, 7500);
	CHECK(firstHalf.camX == Catch::Approx(0.0f).margin(0.0001f));
	CHECK(secondHalf.camX == Catch::Approx(0.0f).margin(0.0001f));
	const auto peak = SwitchEvaluateMotionShotLoop(shot, profile, 5000);
	CHECK(peak.camX == Catch::Approx(0.10f).margin(0.0001f));
}

TEST_CASE("motion shot clamps pan and raises zoom to avoid black edges", "[motion][shots]")
{
	SwitchMotionProfile profile = SwitchDefaultMotionProfile();
	profile.maxZoom = 2.0f;
	SwitchMotionShot shot = SwitchDefaultMotionShot();
	shot.startPanX = 0.30f;
	shot.endPanX = 0.30f;
	shot.startPanY = -0.25f;
	shot.endPanY = -0.25f;
	shot.startZoom = 1.0f;
	shot.endZoom = 1.0f;
	shot.maxZoom = 2.0f;
	shot.loopMode = "restart";

	const auto state = SwitchEvaluateMotionShotLoop(shot, profile, 0);
	const float maxOffset = (state.zoom - 1.0f) / (2.0f * state.zoom);
	CHECK(state.zoom > 1.0f);
	CHECK(state.zoom <= shot.maxZoom);
	CHECK(std::abs(state.camX) <= maxOffset + 0.0001f);
	CHECK(std::abs(state.camY) <= maxOffset + 0.0001f);
}

TEST_CASE("motion shots can share one source with independent scene item IDs", "[motion][shots]")
{
	SwitchMotionShot cam4 = SwitchDefaultMotionShot();
	cam4.id = "shot-cam4";
	cam4.sceneUuid = "scene-cam4";
	cam4.sceneItemId = 4;
	cam4.sourceUuid = "media-2";
	cam4.shotMode = "keyframe_loop";

	SwitchMotionShot cam5 = SwitchDefaultMotionShot();
	cam5.id = "shot-cam5";
	cam5.sceneUuid = "scene-cam5";
	cam5.sceneItemId = 5;
	cam5.sourceUuid = "media-2";
	cam5.shotMode = "ai_auto_frame";

	CHECK(cam4.sourceUuid == cam5.sourceUuid);
	CHECK(cam4.sceneUuid != cam5.sceneUuid);
	CHECK(cam4.sceneItemId != cam5.sceneItemId);
	CHECK(cam4.shotMode == "keyframe_loop");
	CHECK(cam5.shotMode == "ai_auto_frame");
}

TEST_CASE("motion shot serialization round-trips through OBS data", "[motion][serialization]")
{
	SwitchMotionShot shot = SwitchDefaultMotionShot();
	shot.id = "shot-1";
	shot.name = "Cam 4 Slow Push";
	shot.sceneUuid = "scene-4";
	shot.sceneName = "Cam 4";
	shot.sceneItemId = 44;
	shot.sourceUuid = "source-media-2";
	shot.sourceName = "Media 2";
	shot.profileId = "motion-default";
	shot.shotMode = "hybrid";
	shot.playbackMode = "free_run";
	shot.phaseAnchorMs = 1710000000000LL;
	shot.pausedPhaseMs = 3456;
	shot.lastProgramMs = 1710000003000LL;
	shot.startPanX = -0.04f;
	shot.startPanY = 0.02f;
	shot.startZoom = 1.05f;
	shot.endPanX = 0.06f;
	shot.endPanY = -0.03f;
	shot.endZoom = 1.32f;
	shot.durationMs = 26000;
	shot.easing = "ease_in_out";
	shot.loopMode = "ping_pong";
	shot.maxZoom = 2.4f;

	obs_data_t *data = SwitchMotionShotToObsData(shot);
	const auto loaded = SwitchMotionShotFromObsData(data);
	obs_data_release(data);

	CHECK(loaded.id == shot.id);
	CHECK(loaded.name == shot.name);
	CHECK(loaded.sceneUuid == shot.sceneUuid);
	CHECK(loaded.sceneName == shot.sceneName);
	CHECK(loaded.sceneItemId == shot.sceneItemId);
	CHECK(loaded.sourceUuid == shot.sourceUuid);
	CHECK(loaded.sourceName == shot.sourceName);
	CHECK(loaded.profileId == shot.profileId);
	CHECK(loaded.shotMode == shot.shotMode);
	CHECK(loaded.playbackMode == shot.playbackMode);
	CHECK(loaded.phaseAnchorMs == shot.phaseAnchorMs);
	CHECK(loaded.pausedPhaseMs == shot.pausedPhaseMs);
	CHECK(loaded.lastProgramMs == shot.lastProgramMs);
	CHECK(loaded.startPanX == Catch::Approx(shot.startPanX));
	CHECK(loaded.endPanX == Catch::Approx(shot.endPanX));
	CHECK(loaded.endZoom == Catch::Approx(shot.endZoom));
	CHECK(loaded.durationMs == shot.durationMs);
	CHECK(loaded.easing == shot.easing);
	CHECK(loaded.loopMode == shot.loopMode);
	CHECK(loaded.maxZoom == Catch::Approx(shot.maxZoom));
}

TEST_CASE("motion profiles serialize through OBS data", "[motion][serialization]")
{
	SwitchMotionProfile profile = SwitchDefaultMotionProfile("/tmp/yolo26n.onnx");
	profile.id = "motion-profile-1";
	profile.name = "Presenter";
	profile.enabled = true;
	profile.confidenceThreshold = 0.62f;
	profile.maxZoom = 1.75f;
	profile.holdMs = 900;
	profile.subjectMode = "locked";
	profile.framingMode = "face_headroom";
	profile.trackerHighThreshold = 0.57f;
	profile.trackerLowThreshold = 0.24f;
	profile.newTrackThreshold = 0.70f;
	profile.trackBufferFrames = 60;
	profile.autoSwitchMs = 650;
	profile.maxPanSpeed = 0.55f;
	profile.debugOverlay = true;
	profile.lockedTrackId = 12;

	obs_data_t *data = SwitchMotionProfileToObsData(profile);
	const auto loaded = SwitchMotionProfileFromObsData(data);
	obs_data_release(data);

	CHECK(loaded.id == profile.id);
	CHECK(loaded.name == profile.name);
	CHECK(loaded.enabled);
	CHECK(loaded.confidenceThreshold == Catch::Approx(0.62f));
	CHECK(loaded.maxZoom == Catch::Approx(1.75f));
	CHECK(loaded.holdMs == 900);
	CHECK(loaded.modelPath == "/tmp/yolo26n.onnx");
	CHECK(loaded.subjectMode == "locked");
	CHECK(loaded.framingMode == "face_headroom");
	CHECK(loaded.trackerHighThreshold == Catch::Approx(0.57f));
	CHECK(loaded.trackerLowThreshold == Catch::Approx(0.24f));
	CHECK(loaded.newTrackThreshold == Catch::Approx(0.70f));
	CHECK(loaded.trackBufferFrames == 60);
	CHECK(loaded.autoSwitchMs == 650);
	CHECK(loaded.maxPanSpeed == Catch::Approx(0.55f));
	CHECK(loaded.debugOverlay);
	CHECK(loaded.lockedTrackId == 12);
}

TEST_CASE("motion runtime state serializes tracks and target", "[motion][serialization]")
{
	SwitchMotionRuntimeState state;
	state.status = "running";
	state.backend = "coreml";
	state.providerStatus = "CoreML EP loaded with static-shape provider flag";
	state.targetActive = true;
	state.targetConfidence = 0.91f;
	state.targetTrackId = 7;
	state.sourceUuid = "source-a";
	state.inferenceMs = 12.5;
	state.activeTrackCount = 1;
	state.activeShotId = "shot-cam4";
	state.activeShotName = "Cam 4 Slow Push";
	state.activeSceneName = "Cam 4";
	state.activeShotMode = "keyframe_loop";
	state.activeShotPlaybackMode = "free_run";
	state.activeShotPhaseMs = 4200;
	state.activeShotCamX = 0.035f;
	state.activeShotCamY = -0.010f;
	state.activeShotZoom = 1.22f;
	SwitchMotionTrack track;
	track.trackId = 7;
	track.state = "active";
	track.confidence = 0.91f;
	track.bbox = {10.0f, 20.0f, 110.0f, 220.0f, 0.91f, 0};
	track.smoothedBox = track.bbox;
	state.tracks.push_back(track);

	obs_data_t *data = SwitchMotionRuntimeStateToObsData(state);
	CHECK(obs_data_get_int(data, "targetTrackId") == 7);
	CHECK(obs_data_get_int(data, "activeTrackCount") == 1);
	CHECK(QString::fromUtf8(obs_data_get_string(data, "activeShotId")) == "shot-cam4");
	CHECK(QString::fromUtf8(obs_data_get_string(data, "activeShotName")) == "Cam 4 Slow Push");
	CHECK(obs_data_get_int(data, "activeShotPhaseMs") == 4200);
	CHECK(obs_data_get_double(data, "activeShotZoom") == Catch::Approx(1.22));
	REQUIRE(obs_data_has_user_value(data, "target"));
	obs_data_t *target = obs_data_get_obj(data, "target");
	REQUIRE(target != nullptr);
	CHECK(obs_data_get_int(target, "trackId") == 7);
	obs_data_release(target);
	obs_data_array_t *tracks = obs_data_get_array(data, "tracks");
	REQUIRE(tracks != nullptr);
	CHECK(obs_data_array_count(tracks) == 1);
	obs_data_array_release(tracks);
	obs_data_release(data);
}

TEST_CASE("motion legacy default profiles migrate to stronger zoom and smoother tracking", "[motion][serialization]")
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "id", "motion-profile-legacy");
	obs_data_set_string(data, "name", "Motion Profile 2");
	obs_data_set_double(data, "confidenceThreshold", 0.45);
	obs_data_set_int(data, "targetClassId", 0);
	obs_data_set_double(data, "maxZoom", 1.35);
	obs_data_set_double(data, "framingMargin", 0.18);
	obs_data_set_double(data, "deadZone", 0.08);
	obs_data_set_double(data, "smoothing", 0.18);
	obs_data_set_int(data, "holdMs", 700);

	const auto loaded = SwitchMotionProfileFromObsData(data, "/tmp/yolo26n.onnx");
	obs_data_release(data);

	CHECK(loaded.maxZoom == Catch::Approx(2.1f));
	CHECK(loaded.deadZone == Catch::Approx(0.045f));
	CHECK(loaded.smoothing == Catch::Approx(0.52f));
	CHECK(loaded.holdMs == 1200);
}

TEST_CASE("motion early v1 default profiles migrate to smoother gimbal settings", "[motion][serialization]")
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "id", "motion-profile-v1");
	obs_data_set_string(data, "name", "Default Auto Frame");
	obs_data_set_double(data, "confidenceThreshold", 0.45);
	obs_data_set_int(data, "targetClassId", 0);
	obs_data_set_double(data, "maxZoom", 1.75);
	obs_data_set_double(data, "framingMargin", 0.18);
	obs_data_set_double(data, "deadZone", 0.08);
	obs_data_set_double(data, "smoothing", 0.34);
	obs_data_set_int(data, "holdMs", 700);

	const auto loaded = SwitchMotionProfileFromObsData(data, "/tmp/yolo26n.onnx");
	obs_data_release(data);

	CHECK(loaded.maxZoom == Catch::Approx(2.1f));
	CHECK(loaded.deadZone == Catch::Approx(0.045f));
	CHECK(loaded.smoothing == Catch::Approx(0.52f));
	CHECK(loaded.holdMs == 1200);
}

TEST_CASE("automation run policies follow expected semantics", "[automation][scheduler]")
{
	SwitchAutomationMacro macro;
	macro.repeatIntervalMs = 5000;

	macro.runPolicy = "on_change";
	macro.lastConditionMatched = false;
	CHECK(SwitchShouldRunAutomationMacro(macro, true, 1000));
	macro.lastConditionMatched = true;
	CHECK_FALSE(SwitchShouldRunAutomationMacro(macro, true, 2000));

	macro.runPolicy = "on_match";
	CHECK(SwitchShouldRunAutomationMacro(macro, true, 3000));
	CHECK_FALSE(SwitchShouldRunAutomationMacro(macro, false, 3000));

	macro.runPolicy = "repeat";
	macro.lastExecutionMs = 1000;
	CHECK_FALSE(SwitchShouldRunAutomationMacro(macro, true, 5500));
	CHECK(SwitchShouldRunAutomationMacro(macro, true, 6500));
}

TEST_CASE("automation queues advance in sequence and random modes", "[automation][queue]")
{
	SwitchAutomationQueue queue;
	queue.id = "queue";
	queue.actionSegmentIds = {"a", "b", "c"};
	queue.mode = "sequence";

	CHECK(SwitchAdvanceAutomationQueue(queue) == 0);
	CHECK(queue.nextIndex == 1);
	CHECK(SwitchAdvanceAutomationQueue(queue) == 1);
	CHECK(queue.nextIndex == 2);
	CHECK(SwitchAdvanceAutomationQueue(queue) == 2);
	CHECK(queue.nextIndex == 0);

	queue.mode = "random";
	CHECK(SwitchAdvanceAutomationQueue(queue, 7) == 1);
	CHECK(queue.nextIndex == 2);
}

TEST_CASE("automation connection validation catches missing required fields", "[automation][connections]")
{
	SwitchAutomationConnection http;
	http.typeId = "http";
	http.config.insert("url", "https://example.com/hook");
	CHECK(SwitchValidateAutomationConnection(http).isEmpty());

	http.config.insert("url", "not-a-url");
	CHECK_FALSE(SwitchValidateAutomationConnection(http).isEmpty());

	SwitchAutomationConnection mqtt;
	mqtt.typeId = "mqtt";
	mqtt.config.insert("host", "broker.example.com");
	mqtt.config.insert("port", 1883);
	CHECK(SwitchValidateAutomationConnection(mqtt).isEmpty());

	mqtt.config.insert("port", 0);
	CHECK_FALSE(SwitchValidateAutomationConnection(mqtt).isEmpty());

	SwitchAutomationConnection twitch;
	twitch.typeId = "twitch";
	twitch.config.insert("token", "secret");
	twitch.config.insert("channelId", "1234");
	CHECK(SwitchValidateAutomationConnection(twitch).isEmpty());

	SwitchAutomationConnection file;
	file.typeId = "file";
	QTemporaryDir temporaryDir;
	REQUIRE(temporaryDir.isValid());
	file.config.insert("path", temporaryDir.path());
	CHECK(SwitchValidateAutomationConnection(file).isEmpty());

	SwitchAutomationConnection osc;
	osc.typeId = "osc";
	osc.config.insert("mode", "duplex");
	osc.config.insert("remoteHost", "127.0.0.1");
	osc.config.insert("remotePort", 9000);
	osc.config.insert("listenPort", 9001);
	CHECK(SwitchValidateAutomationConnection(osc).isEmpty());

	osc.config.insert("remotePort", 0);
	CHECK_FALSE(SwitchValidateAutomationConnection(osc).isEmpty());
}

TEST_CASE("osc arguments and packets round-trip", "[automation][osc]")
{
	QVector<SwitchOscArgument> arguments;
	QString error;
	REQUIRE(SwitchParseOscArguments(
		QStringLiteral("[1,{\"type\":\"float\",\"value\":0.5},\"live\",true,null,{\"type\":\"blob\",\"value\":\"0xDEADBEEF\"}]"),
		&arguments, &error));
	REQUIRE(arguments.size() == 6);
	CHECK(arguments[0].type == SwitchOscArgumentType::Int);
	CHECK(arguments[1].type == SwitchOscArgumentType::Float);
	CHECK(arguments[2].type == SwitchOscArgumentType::String);
	CHECK(arguments[3].type == SwitchOscArgumentType::True);
	CHECK(arguments[4].type == SwitchOscArgumentType::Null);
	CHECK(arguments[5].type == SwitchOscArgumentType::Blob);

	SwitchOscMessage message;
	message.address = QStringLiteral("/switch/live");
	message.arguments = arguments;

	QByteArray packet;
	REQUIRE(SwitchBuildOscPacket(message, &packet, &error));
	REQUIRE((packet.size() % 4) == 0);

	SwitchOscMessage decoded;
	REQUIRE(SwitchParseOscPacket(packet, &decoded, &error));
	CHECK(decoded.address == message.address);
	CHECK(SwitchOscArgumentsEqual(decoded.arguments, message.arguments));
	CHECK(SwitchOscAddressPatternMatches(QStringLiteral("/switch/*"), decoded.address));
}

TEST_CASE("automation engine receives OSC datagrams on configured connections", "[automation][osc]")
{
	EnsureCoreApplication();
	SwitchAutomationEngine engine(nullptr);

	SwitchAutomationConnection connection;
	connection.typeId = "osc";
	connection.name = "Receiver";
	connection.config.insert("mode", "receive");
	connection.config.insert("listenHost", "127.0.0.1");
	connection.config.insert("listenPort", int(ReserveUdpPort()));

	QString connectionId;
	REQUIRE(engine.UpsertConnection(connection, &connectionId));
	REQUIRE_FALSE(connectionId.isEmpty());
	REQUIRE(engine.SetLifecycleState(SwitchAutomationLifecycleState::Running));

	SwitchOscMessage message{"/switch/test",
				 {{SwitchOscArgumentType::Int, 1},
				  {SwitchOscArgumentType::String, "live"},
				  {SwitchOscArgumentType::True, {}}}};
	QByteArray packet;
	QString buildError;
	REQUIRE(SwitchBuildOscPacket(message, &packet, &buildError));

	QUdpSocket sender;
	REQUIRE(sender.writeDatagram(packet, QHostAddress::LocalHost,
				     quint16(connection.config.value("listenPort").toInt())) == packet.size());
	REQUIRE(WaitForCondition([&]() { return EventLogContains(engine.EventLog(), "Received OSC /switch/test"); }));
}

TEST_CASE("OSC receive conditions can trigger native automation actions", "[automation][osc]")
{
	EnsureCoreApplication();
	SwitchAutomationEngine engine(nullptr);

	SwitchAutomationConnection connection;
	connection.typeId = "osc";
	connection.name = "Trigger";
	connection.config.insert("mode", "receive");
	connection.config.insert("listenHost", "127.0.0.1");
	connection.config.insert("listenPort", int(ReserveUdpPort()));

	QString connectionId;
	REQUIRE(engine.UpsertConnection(connection, &connectionId));
	REQUIRE(engine.SetLifecycleState(SwitchAutomationLifecycleState::Running));

	const QString macroId = engine.CreateMacro("OSC Trigger");
	REQUIRE_FALSE(macroId.isEmpty());

	SwitchMacroDescriptor macro = engine.MacroById(macroId);
	REQUIRE(macro.id == macroId);
	macro.runMode = "on_match";
	macro.triggerType = "osc_receive";
	macro.intervalMs = 500;
	macro.desiredState = true;
	macro.triggerConnectionId = connectionId;
	macro.triggerValueKey = "/switch/trigger";
	macro.triggerValue = "[1, \"live\", true]";
	macro.actionType = "set_variable";
	macro.actionValueKey = "lastOscAddress";
	macro.actionValue = "${macro.oscAddress}";
	REQUIRE(engine.UpdateMacro(macro));

	SwitchOscMessage message{"/switch/trigger",
				 {{SwitchOscArgumentType::Int, 1},
				  {SwitchOscArgumentType::String, "live"},
				  {SwitchOscArgumentType::True, {}}}};
	QByteArray packet;
	QString buildError;
	REQUIRE(SwitchBuildOscPacket(message, &packet, &buildError));

	QUdpSocket sender;
	REQUIRE(sender.writeDatagram(packet, QHostAddress::LocalHost,
				     quint16(connection.config.value("listenPort").toInt())) == packet.size());
	REQUIRE(WaitForCondition([&]() { return engine.Variables().value("lastOscAddress") == "/switch/trigger"; }));
}

TEST_CASE("OSC send actions emit datagrams to configured endpoints", "[automation][osc]")
{
	EnsureCoreApplication();
	SwitchAutomationEngine engine(nullptr);

	QUdpSocket receiver;
	const quint16 listenPort = ReserveUdpPort();
	REQUIRE(receiver.bind(QHostAddress::LocalHost, listenPort));

	SwitchAutomationConnection connection;
	connection.typeId = "osc";
	connection.name = "Sender";
	connection.config.insert("mode", "send");
	connection.config.insert("remoteHost", "127.0.0.1");
	connection.config.insert("remotePort", int(listenPort));

	QString connectionId;
	REQUIRE(engine.UpsertConnection(connection, &connectionId));
	REQUIRE_FALSE(connectionId.isEmpty());
	REQUIRE(engine.SetLifecycleState(SwitchAutomationLifecycleState::Running));

	const QString macroId = engine.CreateMacro("OSC Send");
	REQUIRE_FALSE(macroId.isEmpty());

	SwitchMacroDescriptor macro = engine.MacroById(macroId);
	REQUIRE(macro.id == macroId);
	macro.actionType = "osc";
	macro.actionConnectionId = connectionId;
	macro.actionValueKey = "/switch/out";
	macro.actionValue = "[42, \"sent\", true]";
	REQUIRE(engine.UpdateMacro(macro));

	QString triggerMessage;
	REQUIRE(engine.TriggerMacro(macroId, &triggerMessage));
	REQUIRE(WaitForCondition([&]() { return receiver.hasPendingDatagrams(); }));

	QByteArray datagram(int(receiver.pendingDatagramSize()), Qt::Uninitialized);
	REQUIRE(receiver.readDatagram(datagram.data(), datagram.size()) == datagram.size());
	SwitchOscMessage decoded;
	QString parseError;
	REQUIRE(SwitchParseOscPacket(datagram, &decoded, &parseError));
	REQUIRE(decoded.address == "/switch/out");
	REQUIRE(decoded.arguments.size() == 3);
	REQUIRE(decoded.arguments[0].type == SwitchOscArgumentType::Int);
	REQUIRE(decoded.arguments[0].value.toInt() == 42);
	REQUIRE(decoded.arguments[1].type == SwitchOscArgumentType::String);
	REQUIRE(decoded.arguments[1].value.toString() == "sent");
	REQUIRE(decoded.arguments[2].type == SwitchOscArgumentType::True);
}
