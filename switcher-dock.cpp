#include "switcher-dock.hpp"
#include <obs-module.h>
#include "switch-ai-tracker.hpp"
#include <QEnterEvent>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QPushButton>
#include <QWidgetAction>
#include <QWindow>
#include <QScreen>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QSplitter>
#include <QFont>
#include <QFontDialog>
#include <QColorDialog>
#include <QEvent>
#include <QHideEvent>
#include <QPalette>
#include <QTimer>
#include <QShowEvent>
#include <QUuid>
#include <QAbstractButton>
#include <QEventLoop>

#include <algorithm>
#include <initializer_list>

#include <cstdio>

#include "media-control.hpp"
#include "obs-websocket-api.h"
#include "switcher-workspace.hpp"
#include "switcher-remote-manager.hpp"
#include "version.h"
#include "graphics/matrix4.h"
#include "util/config-file.h"
#include <QContextMenuEvent>

#ifndef QT_UTF8
#define QT_UTF8(str) QString::fromUtf8(str)
#endif
#ifndef QT_TO_UTF8
#define QT_TO_UTF8(str) str.toUtf8().constData()
#endif

#define ACTIVE_NONE 0
#define ACTIVE_PREVIEW 1
#define ACTIVE_PROGRAM 2
#define ACTIVE_DOWNSTREAM_KEYER 3
#define ACTIVE_STREAMING 4
#define ACTIVE_RECORDING 5
#define ACTIVE_RECORDING_AND_STREAMING 6
#define ACTIVE_RECORDING_PAUSED 7

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("Fix It & Post");
OBS_MODULE_USE_DEFAULT_LOCALE("switch", "en-US")

namespace {
constexpr auto kSwitcherStateKey = "switch";
constexpr auto kSwitcherWorkspaceDockId = "switch-workspace";
constexpr int kMinimumObsMajor = 32;
constexpr int kMinimumObsMinor = 1;
constexpr int kMinimumObsPatch = 2;
bool frontendShutdownStarted = false;
bool frontendCallbacksRegistered = false;
bool sourceRemoveSignalRegistered = false;

QEvent::Type DockClosedEventType()
{
	return static_cast<QEvent::Type>(QEvent::User + QEvent::Close);
}

const QByteArray &LegacyStateKey()
{
	static const auto key = QByteArrayLiteral("source") + QByteArrayLiteral("-") + QByteArrayLiteral("dock");
	return key;
}

const QByteArray &LegacyWorkspaceDockId()
{
	static const auto key =
		QByteArrayLiteral("source") + QByteArrayLiteral("-") + QByteArrayLiteral("dock") + QByteArrayLiteral("-") +
		QByteArrayLiteral("switch");
	return key;
}

bool SupportsCurrentObsVersion()
{
	unsigned int major = 0;
	unsigned int minor = 0;
	unsigned int patch = 0;
	const char *versionString = obs_get_version_string();
	if (!versionString)
		return false;

	const int parsed = std::sscanf(versionString, "%u.%u.%u", &major, &minor, &patch);
	if (parsed < 2)
		return false;
	if (parsed < 3)
		patch = 0;

	if (major != static_cast<unsigned int>(kMinimumObsMajor))
		return major > static_cast<unsigned int>(kMinimumObsMajor);
	if (minor != static_cast<unsigned int>(kMinimumObsMinor))
		return minor > static_cast<unsigned int>(kMinimumObsMinor);
	return patch >= static_cast<unsigned int>(kMinimumObsPatch);
}

obs_source_t *ResolveStoredSource(obs_data_t *data)
{
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

QString ActiveFrameStyleSheet(const QWidget *widget, int active)
{
	if (!widget)
		return QString();

	const QPalette pal = widget->palette();
	const QColor surface = Blend(pal.color(QPalette::Window), pal.color(QPalette::Base), 0.18);
	const QColor accent = pal.color(QPalette::Highlight);
	const QColor good = Blend(accent, QColor(40, 180, 120), 0.28);
	const QColor warning = Blend(accent, QColor(214, 140, 58), 0.42);
	const QColor danger = Blend(accent, QColor(214, 82, 82), 0.58);
	const QColor info = Blend(accent, QColor(66, 165, 245), 0.5);

	QColor statusColor;
	int borderAlpha = 0;
	int fillAlpha = 0;

	switch (active) {
	case ACTIVE_PREVIEW:
		statusColor = accent;
		borderAlpha = 118;
		fillAlpha = 18;
		break;
	case ACTIVE_PROGRAM:
		statusColor = good;
		borderAlpha = 156;
		fillAlpha = 28;
		break;
	case ACTIVE_DOWNSTREAM_KEYER:
		statusColor = warning;
		borderAlpha = 144;
		fillAlpha = 22;
		break;
	case ACTIVE_STREAMING:
		statusColor = info;
		borderAlpha = 150;
		fillAlpha = 24;
		break;
	case ACTIVE_RECORDING:
		statusColor = danger;
		borderAlpha = 168;
		fillAlpha = 28;
		break;
	case ACTIVE_RECORDING_AND_STREAMING:
		statusColor = Blend(danger, info, 0.4);
		borderAlpha = 170;
		fillAlpha = 30;
		break;
	case ACTIVE_RECORDING_PAUSED:
		statusColor = warning;
		borderAlpha = 164;
		fillAlpha = 28;
		break;
	default:
		return QStringLiteral("QFrame{background-color: transparent; border: 1px solid transparent; border-radius: 14px;}");
	}

	const QColor border = WithAlpha(statusColor, borderAlpha);
	const QColor fill = WithAlpha(Blend(statusColor, surface, 0.82), fillAlpha);
	return QStringLiteral("QFrame{background-color: %1; border: 1px solid %2; border-radius: 14px;}")
		.arg(CssColor(fill), CssColor(border));
}

void RefreshDetachedDockActiveStates()
{
	for (const auto &dock : switcher_docks)
		QMetaObject::invokeMethod(dock, "ActiveChanged", Qt::QueuedConnection);
}

void ApplyDockRegistrationOptions(SwitcherDock *dock, const SwitcherDockRegistrationOptions &options)
{
	if (options.preview)
		dock->EnablePreview();
	if (options.volMeter)
		dock->EnableVolMeter();
	if (options.volControls)
		dock->EnableVolControls();
	if (options.mediaControls)
		dock->EnableMediaControls();
	if (options.switchScene)
		dock->EnableSwitchScene();
	if (options.showActive)
		dock->EnableShowActive();
	if (options.properties)
		dock->EnableProperties();
	if (options.filters)
		dock->EnableFilters();
	if (options.textInput)
		dock->EnableTextInput();
	if (options.sceneItems)
		dock->EnableSceneItems();
}
} // namespace

static SwitcherWorkspaceDock *switcher_dock = nullptr;
static obs_websocket_vendor switcher_vendor = nullptr;
using VendorRequestHandler = void (*)(obs_data_t *, obs_data_t *, void *);

struct VendorRequestRegistration {
	const char *name;
	VendorRequestHandler handler;
};

static const char *SwitchFrontendEventName(enum obs_frontend_event event)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
		return "SCENE_COLLECTION_CHANGING";
	case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
		return "SCRIPTING_SHUTDOWN";
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
		return "SCENE_COLLECTION_CLEANUP";
	case OBS_FRONTEND_EVENT_EXIT:
		return "EXIT";
	default:
		return "OTHER";
	}
}

static void FlushQtDeferredDeletesForSwitchShutdown()
{
	if (!QCoreApplication::instance())
		return;

	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	QCoreApplication::sendPostedEvents(nullptr);
	QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

static void DestroyWorkspaceDockForFrontendShutdown()
{
	if (!switcher_dock)
		return;

	blog(LOG_INFO, "[Switch] quiescing workspace UI for frontend shutdown");
	SwitcherRemoteManager::Instance()->SetWorkspace(nullptr);
	switcher_dock->setEnabled(false);
	switcher_dock->hide();
}

void SwitcherEmitVendorEvent(const char *eventName, obs_data_t *data)
{
	if (switcher_vendor && data)
		obs_websocket_vendor_emit_event(switcher_vendor, eventName, data);
}

static void vendor_get_remote_state(obs_data_t *, obs_data_t *response_data, void *)
{
	obs_data_t *state = SwitcherRemoteManager::Instance()->BuildRemoteStateData();
	if (state) {
		obs_data_apply(response_data, state);
		obs_data_release(state);
	}
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_list_slots(obs_data_t *, obs_data_t *response_data, void *)
{
	vendor_get_remote_state(nullptr, response_data, nullptr);
}

static void vendor_select_preview_slot(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	const bool success =
		SwitcherRemoteManager::Instance()->SelectPreviewSlot(static_cast<int>(obs_data_get_int(request_data, "slotIndex")));
	obs_data_set_bool(response_data, "success", success);
}

static void vendor_cut(obs_data_t *, obs_data_t *response_data, void *)
{
	obs_data_set_bool(response_data, "success", SwitcherRemoteManager::Instance()->Cut());
}

static void vendor_auto(obs_data_t *, obs_data_t *response_data, void *)
{
	obs_data_set_bool(response_data, "success", SwitcherRemoteManager::Instance()->AutoTransition());
}

static void vendor_get_selected_slot_state(obs_data_t *, obs_data_t *response_data, void *)
{
	vendor_get_remote_state(nullptr, response_data, nullptr);
}

static void vendor_set_slot_visibility(obs_data_t *, obs_data_t *response_data, void *)
{
	obs_data_set_bool(response_data, "success", false);
	obs_data_set_string(response_data, "message", "Slot visibility is not supported for the workspace grid");
}

static void vendor_set_slot_mute(obs_data_t *, obs_data_t *response_data, void *)
{
	obs_data_set_bool(response_data, "success", false);
	obs_data_set_string(response_data, "message", "Slot mute is not supported for the workspace grid");
}

static void vendor_restart_remote(obs_data_t *, obs_data_t *response_data, void *)
{
	SwitcherRemoteManager::Instance()->Restart();
	obs_data_set_bool(response_data, "success", true);
}

static obs_data_array_t *vendor_string_array(std::initializer_list<const char *> values)
{
	obs_data_array_t *array = obs_data_array_create();
	for (const char *value : values) {
		obs_data_t *entry = obs_data_create();
		obs_data_set_string(entry, "id", value);
		obs_data_array_push_back(array, entry);
		obs_data_release(entry);
	}
	return array;
}

static void vendor_set_string_array(obs_data_t *data, const char *name, std::initializer_list<const char *> values)
{
	obs_data_array_t *array = vendor_string_array(values);
	obs_data_set_array(data, name, array);
	obs_data_array_release(array);
}

static void vendor_get_switch_capabilities(obs_data_t *, obs_data_t *response_data, void *)
{
	obs_data_set_bool(response_data, "success", true);
	obs_data_set_string(response_data, "name", "Switch");
	obs_data_set_string(response_data, "version", PROJECT_VERSION);
	obs_data_set_string(response_data, "obsVersion", obs_get_version_string());
	obs_data_set_bool(response_data, "workspaceAvailable", switcher_dock != nullptr);

	vendor_set_string_array(response_data, "modes", {"workspace", "vertical", "motion", "automation"});
	vendor_set_string_array(response_data, "vendorNamespaces",
				{"Switch", "Workspace", "Canvas", "Output", "Motion", "Automation"});

	obs_data_t *motion = obs_data_create();
	obs_data_set_string(motion, "filterId", "switch_ai_tracker");
	obs_data_set_string(motion, "filterName", "Switch Motion");
	obs_data_set_string(motion, "modelOutputParser", "yolo26-end-to-end-nx6");
	obs_data_set_string(motion, "modelDistribution", "installer-download");
	obs_data_set_string(motion, "modelManifest", "models/manifest.json");
	obs_data_set_bool(motion, "renderPathNonBlocking", true);
	vendor_set_string_array(motion, "shotModes", {"ai_auto_frame", "keyframe_loop", "hybrid"});
	vendor_set_string_array(motion, "shotPlaybackModes",
				{"free_run", "restart_on_program", "pause_when_hidden", "cue_in_preview"});
#if defined(__APPLE__)
	vendor_set_string_array(motion, "backends", {"auto", "coreml", "cpu"});
	obs_data_set_string(motion, "autoBackend", "coreml");
#elif defined(_WIN32)
	vendor_set_string_array(motion, "backends", {"auto", "directml", "cpu"});
	obs_data_set_string(motion, "autoBackend", "directml");
#else
	vendor_set_string_array(motion, "backends", {"auto", "cpu"});
	obs_data_set_string(motion, "autoBackend", "cpu");
#endif
	obs_data_set_obj(response_data, "motion", motion);
	obs_data_release(motion);

	obs_data_t *vertical = obs_data_create();
	obs_data_set_bool(vertical, "isolatedOutputBackend", false);
	obs_data_set_string(vertical, "outputControlScope", "obs-global-frontend");
	obs_data_set_bool(vertical, "verticalCanvasState", switcher_dock != nullptr);
	obs_data_set_obj(response_data, "vertical", vertical);
	obs_data_release(vertical);

	obs_data_t *automation = obs_data_create();
	obs_data_set_bool(automation, "macros", true);
	obs_data_set_bool(automation, "variables", true);
	obs_data_set_bool(automation, "queues", true);
	obs_data_set_bool(automation, "connections", true);
	obs_data_set_obj(response_data, "automation", automation);
	obs_data_release(automation);
}

static bool vendor_require_workspace(obs_data_t *response_data)
{
	if (switcher_dock)
		return true;

	obs_data_set_bool(response_data, "success", false);
	obs_data_set_string(response_data, "message", "Switch workspace is not available");
	return false;
}

static QString vendor_canvas_id_from_request(obs_data_t *request_data)
{
	const QString canvasId = QString::fromUtf8(obs_data_get_string(request_data, "canvasId"));
	return canvasId.isEmpty() && switcher_dock ? switcher_dock->VerticalCanvasId() : canvasId;
}

static void vendor_set_canvas_descriptor(obs_data_t *canvasData, const SwitchCanvasDescriptor &descriptor)
{
	obs_data_set_string(canvasData, "id", descriptor.id.toUtf8().constData());
	obs_data_set_string(canvasData, "name", descriptor.name.toUtf8().constData());
	obs_data_set_string(canvasData, "active_scene_uuid", descriptor.activeSceneUuid.toUtf8().constData());
	obs_data_set_string(canvasData, "active_scene_name", descriptor.activeSceneName.toUtf8().constData());
	obs_data_set_int(canvasData, "width", descriptor.size.width());
	obs_data_set_int(canvasData, "height", descriptor.size.height());
	obs_data_set_string(canvasData, "aspect_preset", descriptor.aspectPreset.toUtf8().constData());
	obs_data_set_bool(canvasData, "linked_scene_sync", descriptor.linkedSceneSync);
	obs_data_set_bool(canvasData, "follow_main_streaming", descriptor.followMainStreaming);
	obs_data_set_bool(canvasData, "follow_main_recording", descriptor.followMainRecording);
	obs_data_set_bool(canvasData, "follow_main_replay", descriptor.followMainReplay);
	obs_data_set_bool(canvasData, "follow_main_virtual_camera", descriptor.followMainVirtualCamera);
}

static bool vendor_output_type_supported(const QString &type)
{
	return type == QStringLiteral("recording") || type == QStringLiteral("streaming") ||
	       type == QStringLiteral("replay") || type == QStringLiteral("replay_buffer") ||
	       type == QStringLiteral("virtual_camera") || type == QStringLiteral("virtualcam");
}

static bool vendor_output_type_active(obs_data_t *state, const QString &type)
{
	if (!state)
		return false;
	if (type == QStringLiteral("recording"))
		return obs_data_get_bool(state, "recording_active");
	if (type == QStringLiteral("streaming"))
		return obs_data_get_bool(state, "streaming_active");
	if (type == QStringLiteral("replay") || type == QStringLiteral("replay_buffer"))
		return obs_data_get_bool(state, "replay_active");
	if (type == QStringLiteral("virtual_camera") || type == QStringLiteral("virtualcam"))
		return obs_data_get_bool(state, "virtual_camera_active");
	return false;
}

static QString vendor_toggle_action_for_output_type(const QString &type)
{
	if (type == QStringLiteral("recording"))
		return QStringLiteral("toggle_recording");
	if (type == QStringLiteral("streaming"))
		return QStringLiteral("toggle_streaming");
	if (type == QStringLiteral("replay") || type == QStringLiteral("replay_buffer"))
		return QStringLiteral("toggle_replay");
	if (type == QStringLiteral("virtual_camera") || type == QStringLiteral("virtualcam"))
		return QStringLiteral("toggle_virtual_camera");
	return {};
}

static void vendor_add_macro_to_response(obs_data_t *response_data, const SwitchAutomationMacro &macro)
{
	obs_data_t *macroData = SwitchAutomationMacroToObsData(macro);
	obs_data_set_obj(response_data, "macro", macroData);
	obs_data_release(macroData);
	obs_data_set_string(response_data, "macroId", macro.id.toUtf8().constData());
}

static void vendor_list_canvases(obs_data_t *, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	obs_data_t *state = switcher_dock->BuildCanvasState();
	obs_data_apply(response_data, state);
	obs_data_release(state);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_get_canvas(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString canvasId = vendor_canvas_id_from_request(request_data);
	const auto descriptor = switcher_dock->CanvasDescriptorForId(canvasId);
	if (descriptor.id.isEmpty()) {
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "message", "Canvas not found");
		return;
	}

	obs_data_t *canvasData = obs_data_create();
	vendor_set_canvas_descriptor(canvasData, descriptor);
	obs_data_set_obj(response_data, "canvas", canvasData);
	obs_data_release(canvasData);

	obs_data_array_t *sceneArray = obs_data_array_create();
	for (const auto &scene : switcher_dock->CanvasScenes(canvasId)) {
		obs_data_t *entry = obs_data_create();
		obs_data_set_string(entry, "uuid", scene.uuid.toUtf8().constData());
		obs_data_set_string(entry, "name", scene.name.toUtf8().constData());
		obs_data_array_push_back(sceneArray, entry);
		obs_data_release(entry);
	}
	obs_data_set_array(response_data, "scenes", sceneArray);
	obs_data_array_release(sceneArray);

	obs_data_array_t *sourceArray = obs_data_array_create();
	for (const auto &source : switcher_dock->CanvasSources(canvasId)) {
		obs_data_t *entry = obs_data_create();
		obs_data_set_int(entry, "id", source.id);
		obs_data_set_string(entry, "name", source.name.toUtf8().constData());
		obs_data_set_bool(entry, "visible", source.visible);
		obs_data_set_bool(entry, "locked", source.locked);
		obs_data_set_int(entry, "depth", source.depth);
		obs_data_set_bool(entry, "group", source.isGroup);
		obs_data_array_push_back(sourceArray, entry);
		obs_data_release(entry);
	}
	obs_data_set_array(response_data, "sources", sourceArray);
	obs_data_array_release(sourceArray);

	obs_data_array_t *linkArray = obs_data_array_create();
	if (auto *manager = switcher_dock->CanvasManager()) {
		for (const auto &link : manager->Links()) {
			obs_data_t *entry = obs_data_create();
			obs_data_set_string(entry, "main_scene_uuid", link.mainSceneUuid.toUtf8().constData());
			obs_data_set_string(entry, "main_scene_name", link.mainSceneName.toUtf8().constData());
			obs_data_set_string(entry, "target_scene_uuid", link.targetSceneUuid.toUtf8().constData());
			obs_data_set_string(entry, "target_scene_name", link.targetSceneName.toUtf8().constData());
			obs_data_array_push_back(linkArray, entry);
			obs_data_release(entry);
		}
	}
	obs_data_set_array(response_data, "links", linkArray);
	obs_data_array_release(linkArray);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_switch_canvas_scene(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString canvasId = QString::fromUtf8(obs_data_get_string(request_data, "canvasId"));
	const QString sceneId = QString::fromUtf8(obs_data_get_string(request_data, "sceneId"));
	const QString sceneName = QString::fromUtf8(obs_data_get_string(request_data, "sceneName"));
	const bool success = switcher_dock->SwitchCanvasScene(canvasId, !sceneId.isEmpty() ? sceneId : sceneName);
	obs_data_set_bool(response_data, "success", success);
}

static void vendor_create_canvas_scene(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString canvasId = QString::fromUtf8(obs_data_get_string(request_data, "canvasId"));
	const QString baseName = QString::fromUtf8(obs_data_get_string(request_data, "baseName"));
	QString createdName;
	const bool success = switcher_dock->CreateCanvasScene(canvasId, baseName, &createdName);
	obs_data_set_bool(response_data, "success", success);
	if (success && !createdName.isEmpty())
		obs_data_set_string(response_data, "sceneName", createdName.toUtf8().constData());
}

static void vendor_delete_canvas_scene(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString canvasId = QString::fromUtf8(obs_data_get_string(request_data, "canvasId"));
	const QString sceneId = QString::fromUtf8(obs_data_get_string(request_data, "sceneId"));
	const QString sceneName = QString::fromUtf8(obs_data_get_string(request_data, "sceneName"));
	const bool success = switcher_dock->DeleteCanvasScene(canvasId, !sceneId.isEmpty() ? sceneId : sceneName);
	obs_data_set_bool(response_data, "success", success);
}

static void vendor_set_canvas_link(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString mainSceneUuid = QString::fromUtf8(obs_data_get_string(request_data, "mainSceneUuid"));
	const QString targetSceneId = QString::fromUtf8(obs_data_get_string(request_data, "targetSceneId"));
	const QString targetSceneName = QString::fromUtf8(obs_data_get_string(request_data, "targetSceneName"));
	const bool success =
		switcher_dock->SetCanvasLink(mainSceneUuid, !targetSceneId.isEmpty() ? targetSceneId : targetSceneName);
	obs_data_set_bool(response_data, "success", success);
}

static void vendor_set_linked_scene(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	vendor_set_canvas_link(request_data, response_data, nullptr);
}

static void vendor_open_canvas_window(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString canvasId = QString::fromUtf8(obs_data_get_string(request_data, "canvasId"));
	obs_data_set_bool(response_data, "success", switcher_dock->OpenCanvasWindow(canvasId));
}

static void vendor_open_canvas_projector(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString canvasId = QString::fromUtf8(obs_data_get_string(request_data, "canvasId"));
	obs_data_set_bool(response_data, "success", switcher_dock->OpenCanvasProjector(canvasId));
}

static void vendor_perform_output_action(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString action = QString::fromUtf8(obs_data_get_string(request_data, "action"));
	const QString value = QString::fromUtf8(obs_data_get_string(request_data, "value"));
	obs_data_set_bool(response_data, "success", switcher_dock->PerformOutputAction(action, value));
}

static void vendor_get_canvas_output_state(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	UNUSED_PARAMETER(request_data);
	if (!vendor_require_workspace(response_data))
		return;

	obs_data_t *state = switcher_dock->BuildCanvasState();
	obs_data_set_bool(response_data, "streaming_active", obs_data_get_bool(state, "streaming_active"));
	obs_data_set_bool(response_data, "recording_active", obs_data_get_bool(state, "recording_active"));
	obs_data_set_bool(response_data, "recording_paused", obs_data_get_bool(state, "recording_paused"));
	obs_data_set_bool(response_data, "replay_active", obs_data_get_bool(state, "replay_active"));
	obs_data_set_bool(response_data, "virtual_camera_active", obs_data_get_bool(state, "virtual_camera_active"));
	if (obs_data_t *output = obs_data_get_obj(state, "vertical_output")) {
		obs_data_set_obj(response_data, "vertical_output", output);
		obs_data_release(output);
	}
	obs_data_release(state);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_output_start(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString type = QString::fromUtf8(obs_data_get_string(request_data, "type")).trimmed().toLower();
	if (!vendor_output_type_supported(type)) {
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "message", "Unsupported output type");
		return;
	}

	obs_data_t *state = switcher_dock->BuildCanvasState();
	const bool active = vendor_output_type_active(state, type);
	obs_data_release(state);
	const bool success = active || switcher_dock->PerformOutputAction(vendor_toggle_action_for_output_type(type));
	obs_data_set_bool(response_data, "success", success);
}

static void vendor_output_stop(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString type = QString::fromUtf8(obs_data_get_string(request_data, "type")).trimmed().toLower();
	if (!vendor_output_type_supported(type)) {
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "message", "Unsupported output type");
		return;
	}

	obs_data_t *state = switcher_dock->BuildCanvasState();
	const bool active = vendor_output_type_active(state, type);
	obs_data_release(state);
	const bool success = !active || switcher_dock->PerformOutputAction(vendor_toggle_action_for_output_type(type));
	obs_data_set_bool(response_data, "success", success);
}

static void vendor_output_pause(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString type = QString::fromUtf8(obs_data_get_string(request_data, "type")).trimmed().toLower();
	if (!type.isEmpty() && type != QStringLiteral("recording")) {
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "message", "Pause is only supported for recording");
		return;
	}

	const bool targetPaused = !obs_data_has_user_value(request_data, "paused") || obs_data_get_bool(request_data, "paused");
	obs_data_t *state = switcher_dock->BuildCanvasState();
	const bool recordingActive = obs_data_get_bool(state, "recording_active");
	const bool recordingPaused = obs_data_get_bool(state, "recording_paused");
	obs_data_release(state);
	if (!recordingActive) {
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "message", "Recording is not active");
		return;
	}
	const bool success = (recordingPaused == targetPaused) ||
			     switcher_dock->PerformOutputAction(QStringLiteral("toggle_recording_pause"));
	obs_data_set_bool(response_data, "success", success);
}

static void vendor_output_split(obs_data_t *, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	obs_data_set_bool(response_data, "success", switcher_dock->PerformOutputAction(QStringLiteral("split_recording")));
}

static void vendor_output_chapter(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	const QString value = QString::fromUtf8(obs_data_get_string(request_data, "value"));
	obs_data_set_bool(response_data, "success",
			 switcher_dock->PerformOutputAction(QStringLiteral("add_recording_chapter"), value));
}

static void vendor_output_save_replay(obs_data_t *, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	obs_data_set_bool(response_data, "success", switcher_dock->PerformOutputAction(QStringLiteral("save_replay")));
}

static void vendor_list_macros(obs_data_t *, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	obs_data_t *state = switcher_dock->BuildAutomationState();
	obs_data_apply(response_data, state);
	obs_data_release(state);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_create_macro(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString name = QString::fromUtf8(obs_data_get_string(request_data, "name"));
	const QString macroId = switcher_dock->CreateAutomationMacro(name);
	obs_data_set_bool(response_data, "success", !macroId.isEmpty());
	if (!macroId.isEmpty())
		obs_data_set_string(response_data, "macroId", macroId.toUtf8().constData());
}

static void vendor_delete_macro(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString macroId = QString::fromUtf8(obs_data_get_string(request_data, "macroId"));
	obs_data_set_bool(response_data, "success", switcher_dock->DeleteAutomationMacro(macroId));
}

static void vendor_update_macro(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	SwitchMacroDescriptor macro;
	macro.id = QString::fromUtf8(obs_data_get_string(request_data, "macroId"));
	macro.name = QString::fromUtf8(obs_data_get_string(request_data, "name"));
	macro.group = QString::fromUtf8(obs_data_get_string(request_data, "group"));
	macro.enabled = obs_data_get_bool(request_data, "enabled");
	macro.paused = obs_data_get_bool(request_data, "paused");
	macro.runMode = QString::fromUtf8(obs_data_get_string(request_data, "runMode"));
	macro.triggerType = QString::fromUtf8(obs_data_get_string(request_data, "triggerType"));
	macro.intervalMs = int(obs_data_get_int(request_data, "intervalMs"));
	macro.desiredState = obs_data_get_bool(request_data, "desiredState");
	macro.triggerConnectionId = QString::fromUtf8(obs_data_get_string(request_data, "triggerConnectionId"));
	macro.triggerConnectionName = QString::fromUtf8(obs_data_get_string(request_data, "triggerConnectionName"));
	macro.triggerSceneUuid = QString::fromUtf8(obs_data_get_string(request_data, "triggerSceneUuid"));
	macro.triggerSceneName = QString::fromUtf8(obs_data_get_string(request_data, "triggerSceneName"));
	macro.triggerValueKey = QString::fromUtf8(obs_data_get_string(request_data, "triggerValueKey"));
	macro.triggerValue = QString::fromUtf8(obs_data_get_string(request_data, "triggerValue"));
	macro.actionType = QString::fromUtf8(obs_data_get_string(request_data, "actionType"));
	macro.actionConnectionId = QString::fromUtf8(obs_data_get_string(request_data, "actionConnectionId"));
	macro.actionConnectionName = QString::fromUtf8(obs_data_get_string(request_data, "actionConnectionName"));
	macro.actionSceneUuid = QString::fromUtf8(obs_data_get_string(request_data, "actionSceneUuid"));
	macro.actionSceneName = QString::fromUtf8(obs_data_get_string(request_data, "actionSceneName"));
	macro.actionValueKey = QString::fromUtf8(obs_data_get_string(request_data, "actionValueKey"));
	macro.actionValue = QString::fromUtf8(obs_data_get_string(request_data, "actionValue"));
	macro.actionDelayMs = int(obs_data_get_int(request_data, "actionDelayMs"));

	obs_data_set_bool(response_data, "success", switcher_dock->UpdateAutomationMacro(macro));
}

static void vendor_set_macro_enabled(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString macroId = QString::fromUtf8(obs_data_get_string(request_data, "macroId"));
	const bool enabled = obs_data_get_bool(request_data, "enabled");
	obs_data_set_bool(response_data, "success", switcher_dock->SetAutomationMacroEnabled(macroId, enabled));
}

static void vendor_set_macro_paused(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString macroId = QString::fromUtf8(obs_data_get_string(request_data, "macroId"));
	const bool paused = obs_data_get_bool(request_data, "paused");
	obs_data_set_bool(response_data, "success", switcher_dock->SetAutomationMacroPaused(macroId, paused));
}

static void vendor_trigger_macro(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString macroId = QString::fromUtf8(obs_data_get_string(request_data, "macroId"));
	QString message;
	const bool success = switcher_dock->TriggerAutomationMacro(macroId, &message);
	obs_data_set_bool(response_data, "success", success);
	if (!message.isEmpty())
		obs_data_set_string(response_data, "message", message.toUtf8().constData());
}

static void vendor_set_variable(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString key = QString::fromUtf8(obs_data_get_string(request_data, "key"));
	const QString value = QString::fromUtf8(obs_data_get_string(request_data, "value"));
	obs_data_set_bool(response_data, "success", switcher_dock->SetAutomationVariable(key, value));
}

static void vendor_delete_variable(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString key = QString::fromUtf8(obs_data_get_string(request_data, "key"));
	obs_data_set_bool(response_data, "success", switcher_dock->DeleteAutomationVariable(key));
}

static void vendor_get_macro(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString macroId = QString::fromUtf8(obs_data_get_string(request_data, "macroId"));
	const auto macro = switcher_dock->AutomationMacroDefinition(macroId);
	const bool success = !macro.id.isEmpty();
	obs_data_set_bool(response_data, "success", success);
	if (!success) {
		obs_data_set_string(response_data, "message", "Automation macro not found");
		return;
	}

	vendor_add_macro_to_response(response_data, macro);
}

static void vendor_upsert_macro(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	QString effectiveId;
	bool success = false;

	if (obs_data_t *macroData = obs_data_get_obj(request_data, "macro")) {
		auto macro = SwitchAutomationMacroFromObsData(macroData);
		obs_data_release(macroData);
		success = switcher_dock->UpsertAutomationMacro(macro, &effectiveId);
	} else {
		SwitchMacroDescriptor macro;
		macro.id = QString::fromUtf8(obs_data_get_string(request_data, "macroId"));
		if (macro.id.isEmpty()) {
			const QString requestedName = QString::fromUtf8(obs_data_get_string(request_data, "name"));
			macro.id = switcher_dock->CreateAutomationMacro(requestedName);
		}
		macro.name = QString::fromUtf8(obs_data_get_string(request_data, "name"));
		macro.group = QString::fromUtf8(obs_data_get_string(request_data, "group"));
		macro.enabled = !obs_data_has_user_value(request_data, "enabled") || obs_data_get_bool(request_data, "enabled");
		macro.paused = obs_data_get_bool(request_data, "paused");
		macro.runMode = QString::fromUtf8(obs_data_get_string(request_data, "runMode"));
		macro.triggerType = QString::fromUtf8(obs_data_get_string(request_data, "triggerType"));
		macro.intervalMs = int(obs_data_get_int(request_data, "intervalMs"));
		macro.desiredState = !obs_data_has_user_value(request_data, "desiredState") ||
				     obs_data_get_bool(request_data, "desiredState");
		macro.triggerConnectionId = QString::fromUtf8(obs_data_get_string(request_data, "triggerConnectionId"));
		macro.triggerConnectionName = QString::fromUtf8(obs_data_get_string(request_data, "triggerConnectionName"));
		macro.triggerSceneUuid = QString::fromUtf8(obs_data_get_string(request_data, "triggerSceneUuid"));
		macro.triggerSceneName = QString::fromUtf8(obs_data_get_string(request_data, "triggerSceneName"));
		macro.triggerValueKey = QString::fromUtf8(obs_data_get_string(request_data, "triggerValueKey"));
		macro.triggerValue = QString::fromUtf8(obs_data_get_string(request_data, "triggerValue"));
		macro.actionType = QString::fromUtf8(obs_data_get_string(request_data, "actionType"));
		macro.actionConnectionId = QString::fromUtf8(obs_data_get_string(request_data, "actionConnectionId"));
		macro.actionConnectionName = QString::fromUtf8(obs_data_get_string(request_data, "actionConnectionName"));
		macro.actionSceneUuid = QString::fromUtf8(obs_data_get_string(request_data, "actionSceneUuid"));
		macro.actionSceneName = QString::fromUtf8(obs_data_get_string(request_data, "actionSceneName"));
		macro.actionValueKey = QString::fromUtf8(obs_data_get_string(request_data, "actionValueKey"));
		macro.actionValue = QString::fromUtf8(obs_data_get_string(request_data, "actionValue"));
		macro.actionDelayMs = int(obs_data_get_int(request_data, "actionDelayMs"));
		effectiveId = macro.id;
		success = !macro.id.isEmpty() && switcher_dock->UpdateAutomationMacro(macro);
	}

	obs_data_set_bool(response_data, "success", success);
	if (!success) {
		obs_data_set_string(response_data, "message", "Unable to upsert automation macro");
		return;
	}

	const auto macro = switcher_dock->AutomationMacroDefinition(effectiveId);
	if (!macro.id.isEmpty())
		vendor_add_macro_to_response(response_data, macro);
}

static void vendor_list_variables(obs_data_t *, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	obs_data_t *state = switcher_dock->BuildAutomationState();
	if (obs_data_t *variables = obs_data_get_obj(state, "variables")) {
		obs_data_set_obj(response_data, "variables", variables);
		obs_data_release(variables);
	}
	obs_data_release(state);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_list_queues(obs_data_t *, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	obs_data_t *state = switcher_dock->BuildAutomationState();
	if (obs_data_array_t *queues = obs_data_get_array(state, "queues")) {
		obs_data_set_array(response_data, "queues", queues);
		obs_data_array_release(queues);
	}
	obs_data_release(state);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_clear_queue(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString queueId = QString::fromUtf8(obs_data_get_string(request_data, "queueId"));
	obs_data_set_bool(response_data, "success", switcher_dock->ClearAutomationQueue(queueId));
}

static void vendor_get_automation_status(obs_data_t *, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	obs_data_t *state = switcher_dock->BuildAutomationState();
	obs_data_apply(response_data, state);
	obs_data_release(state);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_list_connections(obs_data_t *, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	obs_data_t *state = switcher_dock->BuildAutomationState();
	if (obs_data_array_t *connections = obs_data_get_array(state, "connections")) {
		obs_data_set_array(response_data, "connections", connections);
		obs_data_array_release(connections);
	}
	obs_data_release(state);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_test_connection(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	const QString connectionId = QString::fromUtf8(obs_data_get_string(request_data, "connectionId"));
	QString message;
	const bool success = switcher_dock->TestAutomationConnection(connectionId, &message);
	obs_data_set_bool(response_data, "success", success);
	if (!message.isEmpty())
		obs_data_set_string(response_data, "message", message.toUtf8().constData());

	const auto connection = switcher_dock->AutomationConnection(connectionId);
	if (!connection.id.isEmpty()) {
		obs_data_t *state = switcher_dock->BuildAutomationState();
		if (obs_data_array_t *connections = obs_data_get_array(state, "connections")) {
			obs_data_set_array(response_data, "connections", connections);
			obs_data_array_release(connections);
		}
		obs_data_release(state);
	}
}

static void vendor_get_motion_state(obs_data_t *, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	obs_data_t *state = switcher_dock->BuildMotionState();
	obs_data_set_obj(response_data, "state", state);
	obs_data_release(state);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_list_motion_profiles(obs_data_t *, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	obs_data_t *state = switcher_dock->BuildMotionState();
	if (obs_data_array_t *profiles = obs_data_get_array(state, "profiles")) {
		obs_data_set_array(response_data, "profiles", profiles);
		obs_data_array_release(profiles);
	}
	obs_data_release(state);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_get_motion_profile(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	const QString profileId = QString::fromUtf8(obs_data_get_string(request_data, "profileId"));
	const auto profile = switcher_dock->MotionProfileDefinition(profileId);
	if (profile.id.isEmpty()) {
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "message", "Motion profile not found");
		return;
	}
	obs_data_t *profileData = SwitchMotionProfileToObsData(profile);
	obs_data_set_obj(response_data, "profile", profileData);
	obs_data_release(profileData);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_upsert_motion_profile(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	SwitchMotionProfile profile;
	if (obs_data_t *profileData = obs_data_get_obj(request_data, "profile")) {
		profile = SwitchMotionProfileFromObsData(profileData);
		obs_data_release(profileData);
	} else {
		const QString requestedId = QString::fromUtf8(obs_data_get_string(request_data, "profileId"));
		profile = requestedId.isEmpty() ? SwitchDefaultMotionProfile() : switcher_dock->MotionProfileDefinition(requestedId);
		if (profile.id.isEmpty())
			profile = SwitchDefaultMotionProfile();
		profile.id = requestedId.isEmpty() ? profile.id : requestedId;
		profile.name = QString::fromUtf8(obs_data_get_string(request_data, "name"));
		if (profile.name.isEmpty())
			profile.name = QStringLiteral("Motion Profile");
		if (obs_data_has_user_value(request_data, "enabled"))
			profile.enabled = obs_data_get_bool(request_data, "enabled");
		if (obs_data_has_user_value(request_data, "confidenceThreshold"))
			profile.confidenceThreshold = static_cast<float>(obs_data_get_double(request_data, "confidenceThreshold"));
		if (obs_data_has_user_value(request_data, "targetClassId"))
			profile.targetClassId = static_cast<int>(obs_data_get_int(request_data, "targetClassId"));
		if (obs_data_has_user_value(request_data, "maxZoom"))
			profile.maxZoom = static_cast<float>(obs_data_get_double(request_data, "maxZoom"));
		if (obs_data_has_user_value(request_data, "framingMargin"))
			profile.framingMargin = static_cast<float>(obs_data_get_double(request_data, "framingMargin"));
		if (obs_data_has_user_value(request_data, "deadZone"))
			profile.deadZone = static_cast<float>(obs_data_get_double(request_data, "deadZone"));
		if (obs_data_has_user_value(request_data, "smoothing"))
			profile.smoothing = static_cast<float>(obs_data_get_double(request_data, "smoothing"));
		if (obs_data_has_user_value(request_data, "holdMs"))
			profile.holdMs = static_cast<int>(obs_data_get_int(request_data, "holdMs"));
		if (obs_data_has_user_value(request_data, "backend"))
			profile.backend = QString::fromUtf8(obs_data_get_string(request_data, "backend"));
		if (obs_data_has_user_value(request_data, "modelPath"))
			profile.modelPath = QString::fromUtf8(obs_data_get_string(request_data, "modelPath"));
		if (obs_data_has_user_value(request_data, "subjectMode"))
			profile.subjectMode = QString::fromUtf8(obs_data_get_string(request_data, "subjectMode"));
		if (obs_data_has_user_value(request_data, "framingMode"))
			profile.framingMode = QString::fromUtf8(obs_data_get_string(request_data, "framingMode"));
		if (obs_data_has_user_value(request_data, "trackerHighThreshold"))
			profile.trackerHighThreshold = static_cast<float>(obs_data_get_double(request_data, "trackerHighThreshold"));
		if (obs_data_has_user_value(request_data, "trackerLowThreshold"))
			profile.trackerLowThreshold = static_cast<float>(obs_data_get_double(request_data, "trackerLowThreshold"));
		if (obs_data_has_user_value(request_data, "newTrackThreshold"))
			profile.newTrackThreshold = static_cast<float>(obs_data_get_double(request_data, "newTrackThreshold"));
		if (obs_data_has_user_value(request_data, "trackBufferFrames"))
			profile.trackBufferFrames = static_cast<int>(obs_data_get_int(request_data, "trackBufferFrames"));
		if (obs_data_has_user_value(request_data, "autoSwitchMs"))
			profile.autoSwitchMs = static_cast<int>(obs_data_get_int(request_data, "autoSwitchMs"));
		if (obs_data_has_user_value(request_data, "panResponsiveness"))
			profile.panResponsiveness = static_cast<float>(obs_data_get_double(request_data, "panResponsiveness"));
		if (obs_data_has_user_value(request_data, "tiltResponsiveness"))
			profile.tiltResponsiveness = static_cast<float>(obs_data_get_double(request_data, "tiltResponsiveness"));
		if (obs_data_has_user_value(request_data, "zoomResponsiveness"))
			profile.zoomResponsiveness = static_cast<float>(obs_data_get_double(request_data, "zoomResponsiveness"));
		if (obs_data_has_user_value(request_data, "maxPanSpeed"))
			profile.maxPanSpeed = static_cast<float>(obs_data_get_double(request_data, "maxPanSpeed"));
		if (obs_data_has_user_value(request_data, "maxTiltSpeed"))
			profile.maxTiltSpeed = static_cast<float>(obs_data_get_double(request_data, "maxTiltSpeed"));
		if (obs_data_has_user_value(request_data, "maxZoomSpeed"))
			profile.maxZoomSpeed = static_cast<float>(obs_data_get_double(request_data, "maxZoomSpeed"));
		if (obs_data_has_user_value(request_data, "debugOverlay"))
			profile.debugOverlay = obs_data_get_bool(request_data, "debugOverlay");
		if (obs_data_has_user_value(request_data, "lockedTrackId"))
			profile.lockedTrackId = static_cast<int>(obs_data_get_int(request_data, "lockedTrackId"));
	}

	QString effectiveId;
	const bool success = switcher_dock->UpsertMotionProfile(profile, &effectiveId);
	obs_data_set_bool(response_data, "success", success);
	obs_data_set_string(response_data, "profileId", effectiveId.toUtf8().constData());
	if (success) {
		obs_data_t *profileData = SwitchMotionProfileToObsData(switcher_dock->MotionProfileDefinition(effectiveId));
		obs_data_set_obj(response_data, "profile", profileData);
		obs_data_release(profileData);
	}
}

static void vendor_delete_motion_profile(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	const QString profileId = QString::fromUtf8(obs_data_get_string(request_data, "profileId"));
	obs_data_set_bool(response_data, "success", switcher_dock->DeleteMotionProfile(profileId));
}

static void vendor_set_motion_enabled(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	const QString profileId = QString::fromUtf8(obs_data_get_string(request_data, "profileId"));
	const bool enabled = obs_data_get_bool(request_data, "enabled");
	obs_data_set_bool(response_data, "success", switcher_dock->SetMotionEnabled(profileId, enabled));
}

static void vendor_bind_motion_source(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	const QString sourceUuid = QString::fromUtf8(obs_data_get_string(request_data, "sourceUuid"));
	const QString sourceName = QString::fromUtf8(obs_data_get_string(request_data, "sourceName"));
	const QString profileId = QString::fromUtf8(obs_data_get_string(request_data, "profileId"));
	QString message;
	const bool success = switcher_dock->BindMotionSource(sourceUuid, sourceName, profileId, &message);
	obs_data_set_bool(response_data, "success", success);
	if (!message.isEmpty())
		obs_data_set_string(response_data, "message", message.toUtf8().constData());
}

static void vendor_unbind_motion_source(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	const QString sourceUuid = QString::fromUtf8(obs_data_get_string(request_data, "sourceUuid"));
	QString message;
	const bool success = switcher_dock->UnbindMotionSource(sourceUuid, &message);
	obs_data_set_bool(response_data, "success", success);
	if (!message.isEmpty())
		obs_data_set_string(response_data, "message", message.toUtf8().constData());
}

static void vendor_list_motion_shots(obs_data_t *, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	obs_data_t *state = switcher_dock->BuildMotionState();
	if (obs_data_array_t *shots = obs_data_get_array(state, "shots")) {
		obs_data_set_array(response_data, "shots", shots);
		obs_data_array_release(shots);
	}
	obs_data_release(state);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_get_motion_shot(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	const QString shotId = QString::fromUtf8(obs_data_get_string(request_data, "shotId"));
	const auto shot = switcher_dock->MotionShotDefinition(shotId);
	if (shot.id.isEmpty()) {
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "message", "Motion shot not found");
		return;
	}
	obs_data_t *shotData = SwitchMotionShotToObsData(shot);
	obs_data_set_obj(response_data, "shot", shotData);
	obs_data_release(shotData);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_upsert_motion_shot(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;

	SwitchMotionShot shot;
	if (obs_data_t *shotData = obs_data_get_obj(request_data, "shot")) {
		shot = SwitchMotionShotFromObsData(shotData);
		obs_data_release(shotData);
	} else {
		const QString requestedId = QString::fromUtf8(obs_data_get_string(request_data, "shotId"));
		shot = requestedId.isEmpty() ? SwitchDefaultMotionShot() : switcher_dock->MotionShotDefinition(requestedId);
		if (shot.id.isEmpty())
			shot = SwitchDefaultMotionShot();
		if (!requestedId.isEmpty())
			shot.id = requestedId;
		if (obs_data_has_user_value(request_data, "name"))
			shot.name = QString::fromUtf8(obs_data_get_string(request_data, "name"));
		if (obs_data_has_user_value(request_data, "enabled"))
			shot.enabled = obs_data_get_bool(request_data, "enabled");
		if (obs_data_has_user_value(request_data, "sceneUuid"))
			shot.sceneUuid = QString::fromUtf8(obs_data_get_string(request_data, "sceneUuid"));
		if (obs_data_has_user_value(request_data, "sceneName"))
			shot.sceneName = QString::fromUtf8(obs_data_get_string(request_data, "sceneName"));
		if (obs_data_has_user_value(request_data, "sceneItemId"))
			shot.sceneItemId = static_cast<int64_t>(obs_data_get_int(request_data, "sceneItemId"));
		if (obs_data_has_user_value(request_data, "sourceUuid"))
			shot.sourceUuid = QString::fromUtf8(obs_data_get_string(request_data, "sourceUuid"));
		if (obs_data_has_user_value(request_data, "sourceName"))
			shot.sourceName = QString::fromUtf8(obs_data_get_string(request_data, "sourceName"));
		if (obs_data_has_user_value(request_data, "profileId"))
			shot.profileId = QString::fromUtf8(obs_data_get_string(request_data, "profileId"));
		if (obs_data_has_user_value(request_data, "shotMode"))
			shot.shotMode = QString::fromUtf8(obs_data_get_string(request_data, "shotMode"));
		if (obs_data_has_user_value(request_data, "playbackMode"))
			shot.playbackMode = QString::fromUtf8(obs_data_get_string(request_data, "playbackMode"));
		if (obs_data_has_user_value(request_data, "startPanX"))
			shot.startPanX = static_cast<float>(obs_data_get_double(request_data, "startPanX"));
		if (obs_data_has_user_value(request_data, "startPanY"))
			shot.startPanY = static_cast<float>(obs_data_get_double(request_data, "startPanY"));
		if (obs_data_has_user_value(request_data, "startZoom"))
			shot.startZoom = static_cast<float>(obs_data_get_double(request_data, "startZoom"));
		if (obs_data_has_user_value(request_data, "endPanX"))
			shot.endPanX = static_cast<float>(obs_data_get_double(request_data, "endPanX"));
		if (obs_data_has_user_value(request_data, "endPanY"))
			shot.endPanY = static_cast<float>(obs_data_get_double(request_data, "endPanY"));
		if (obs_data_has_user_value(request_data, "endZoom"))
			shot.endZoom = static_cast<float>(obs_data_get_double(request_data, "endZoom"));
		if (obs_data_has_user_value(request_data, "durationMs"))
			shot.durationMs = static_cast<int>(obs_data_get_int(request_data, "durationMs"));
		if (obs_data_has_user_value(request_data, "easing"))
			shot.easing = QString::fromUtf8(obs_data_get_string(request_data, "easing"));
		if (obs_data_has_user_value(request_data, "loopMode"))
			shot.loopMode = QString::fromUtf8(obs_data_get_string(request_data, "loopMode"));
		if (obs_data_has_user_value(request_data, "maxZoom"))
			shot.maxZoom = static_cast<float>(obs_data_get_double(request_data, "maxZoom"));
	}

	QString effectiveId;
	const bool success = switcher_dock->UpsertMotionShot(shot, &effectiveId);
	obs_data_set_bool(response_data, "success", success);
	obs_data_set_string(response_data, "shotId", effectiveId.toUtf8().constData());
	if (success) {
		obs_data_t *shotData = SwitchMotionShotToObsData(switcher_dock->MotionShotDefinition(effectiveId));
		obs_data_set_obj(response_data, "shot", shotData);
		obs_data_release(shotData);
	}
}

static void vendor_delete_motion_shot(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	const QString shotId = QString::fromUtf8(obs_data_get_string(request_data, "shotId"));
	obs_data_set_bool(response_data, "success", switcher_dock->DeleteMotionShot(shotId));
}

static void vendor_bind_motion_scene_item(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	const QString shotId = QString::fromUtf8(obs_data_get_string(request_data, "shotId"));
	SwitchMotionShot shot = shotId.isEmpty() ? SwitchDefaultMotionShot() : switcher_dock->MotionShotDefinition(shotId);
	if (!shotId.isEmpty() && shot.id.isEmpty()) {
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "message", "Motion shot not found");
		return;
	}
	if (!shotId.isEmpty())
		shot.id = shotId;
	shot.sceneUuid = QString::fromUtf8(obs_data_get_string(request_data, "sceneUuid"));
	shot.sceneName = QString::fromUtf8(obs_data_get_string(request_data, "sceneName"));
	shot.sceneItemId = static_cast<int64_t>(obs_data_get_int(request_data, "sceneItemId"));
	shot.sourceUuid = QString::fromUtf8(obs_data_get_string(request_data, "sourceUuid"));
	shot.sourceName = QString::fromUtf8(obs_data_get_string(request_data, "sourceName"));
	if (obs_data_has_user_value(request_data, "profileId"))
		shot.profileId = QString::fromUtf8(obs_data_get_string(request_data, "profileId"));
	if (obs_data_has_user_value(request_data, "shotMode"))
		shot.shotMode = QString::fromUtf8(obs_data_get_string(request_data, "shotMode"));
	if (obs_data_has_user_value(request_data, "playbackMode"))
		shot.playbackMode = QString::fromUtf8(obs_data_get_string(request_data, "playbackMode"));
	QString effectiveId;
	QString message;
	const bool success = switcher_dock->BindMotionSceneItem(shot, &effectiveId, &message);
	obs_data_set_bool(response_data, "success", success);
	obs_data_set_string(response_data, "shotId", effectiveId.toUtf8().constData());
	if (!message.isEmpty())
		obs_data_set_string(response_data, "message", message.toUtf8().constData());
}

static void vendor_set_motion_shot_enabled(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	const QString shotId = QString::fromUtf8(obs_data_get_string(request_data, "shotId"));
	const bool enabled = obs_data_get_bool(request_data, "enabled");
	obs_data_set_bool(response_data, "success", switcher_dock->SetMotionShotEnabled(shotId, enabled));
}

static void vendor_set_motion_shot_playback(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	auto shot = switcher_dock->MotionShotDefinition(QString::fromUtf8(obs_data_get_string(request_data, "shotId")));
	if (shot.id.isEmpty()) {
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "message", "Motion shot not found");
		return;
	}
	shot.playbackMode = QString::fromUtf8(obs_data_get_string(request_data, "playbackMode"));
	QString effectiveId;
	const bool success = switcher_dock->UpsertMotionShot(shot, &effectiveId);
	obs_data_set_bool(response_data, "success", success);
	obs_data_set_string(response_data, "shotId", effectiveId.toUtf8().constData());
}

static void vendor_get_active_motion_shot(obs_data_t *, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	obs_data_t *state = switcher_dock->BuildMotionState();
	if (obs_data_t *runtime = obs_data_get_obj(state, "runtime")) {
		obs_data_set_string(response_data, "shotId", obs_data_get_string(runtime, "activeShotId"));
		obs_data_set_string(response_data, "shotName", obs_data_get_string(runtime, "activeShotName"));
		obs_data_set_string(response_data, "sceneName", obs_data_get_string(runtime, "activeSceneName"));
		obs_data_set_string(response_data, "shotMode", obs_data_get_string(runtime, "activeShotMode"));
		obs_data_set_string(response_data, "playbackMode", obs_data_get_string(runtime, "activeShotPlaybackMode"));
		obs_data_set_int(response_data, "phaseMs", obs_data_get_int(runtime, "activeShotPhaseMs"));
		obs_data_set_double(response_data, "camX", obs_data_get_double(runtime, "activeShotCamX"));
		obs_data_set_double(response_data, "camY", obs_data_get_double(runtime, "activeShotCamY"));
		obs_data_set_double(response_data, "zoom", obs_data_get_double(runtime, "activeShotZoom"));
		obs_data_release(runtime);
	}
	obs_data_release(state);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_list_motion_shot_presets(obs_data_t *, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	obs_data_array_t *presets = SwitchMotionShotPresetsToObsArray();
	obs_data_set_array(response_data, "presets", presets);
	obs_data_array_release(presets);
	obs_data_set_bool(response_data, "success", true);
}

static QString vendor_motion_profile_id(obs_data_t *request_data)
{
	QString profileId = QString::fromUtf8(obs_data_get_string(request_data, "profileId"));
	if (!profileId.isEmpty())
		return profileId;
	obs_data_t *state = switcher_dock ? switcher_dock->BuildMotionState() : nullptr;
	if (!state)
		return {};
	if (obs_data_array_t *profiles = obs_data_get_array(state, "profiles")) {
		if (obs_data_array_count(profiles) > 0) {
			obs_data_t *profile = obs_data_array_item(profiles, 0);
			profileId = QString::fromUtf8(obs_data_get_string(profile, "id"));
			obs_data_release(profile);
		}
		obs_data_array_release(profiles);
	}
	obs_data_release(state);
	return profileId;
}

static void vendor_list_motion_tracks(obs_data_t *, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	obs_data_t *state = switcher_dock->BuildMotionState();
	if (obs_data_array_t *tracks = obs_data_get_array(state, "tracks")) {
		obs_data_set_array(response_data, "tracks", tracks);
		obs_data_array_release(tracks);
	}
	obs_data_release(state);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_get_motion_target(obs_data_t *, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	obs_data_t *state = switcher_dock->BuildMotionState();
	if (obs_data_t *target = obs_data_get_obj(state, "target")) {
		obs_data_set_obj(response_data, "target", target);
		obs_data_release(target);
	}
	obs_data_release(state);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_get_motion_runtime_stats(obs_data_t *, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	obs_data_t *state = switcher_dock->BuildMotionState();
	if (obs_data_t *runtime = obs_data_get_obj(state, "runtime")) {
		obs_data_set_obj(response_data, "runtime", runtime);
		obs_data_release(runtime);
	}
	obs_data_release(state);
	obs_data_set_bool(response_data, "success", true);
}

static void vendor_set_motion_target_track(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	const QString profileId = vendor_motion_profile_id(request_data);
	auto profile = switcher_dock->MotionProfileDefinition(profileId);
	if (profile.id.isEmpty()) {
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "message", "Motion profile not found");
		return;
	}
	profile.subjectMode = QStringLiteral("locked");
	profile.lockedTrackId = static_cast<int>(obs_data_get_int(request_data, "trackId"));
	QString effectiveId;
	const bool success = switcher_dock->UpsertMotionProfile(profile, &effectiveId);
	obs_data_set_bool(response_data, "success", success);
	obs_data_set_string(response_data, "profileId", effectiveId.toUtf8().constData());
	obs_data_set_int(response_data, "trackId", profile.lockedTrackId);
}

static void vendor_clear_motion_target_track(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	const QString profileId = vendor_motion_profile_id(request_data);
	auto profile = switcher_dock->MotionProfileDefinition(profileId);
	if (profile.id.isEmpty()) {
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "message", "Motion profile not found");
		return;
	}
	profile.subjectMode = QStringLiteral("auto");
	profile.lockedTrackId = -1;
	QString effectiveId;
	const bool success = switcher_dock->UpsertMotionProfile(profile, &effectiveId);
	obs_data_set_bool(response_data, "success", success);
	obs_data_set_string(response_data, "profileId", effectiveId.toUtf8().constData());
}

static void vendor_cycle_motion_target(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	obs_data_t *state = switcher_dock->BuildMotionState();
	obs_data_t *runtime = obs_data_get_obj(state, "runtime");
	obs_data_array_t *tracks = runtime ? obs_data_get_array(runtime, "tracks") : nullptr;
	QVector<int> activeIds;
	if (tracks) {
		const size_t count = obs_data_array_count(tracks);
		for (size_t index = 0; index < count; index++) {
			obs_data_t *track = obs_data_array_item(tracks, index);
			const QString trackState = QString::fromUtf8(obs_data_get_string(track, "state"));
			if (trackState == QStringLiteral("active") || trackState == QStringLiteral("new") ||
			    trackState == QStringLiteral("recovered"))
				activeIds.push_back(static_cast<int>(obs_data_get_int(track, "trackId")));
			obs_data_release(track);
		}
		obs_data_array_release(tracks);
	}
	const int currentTrackId = runtime ? static_cast<int>(obs_data_get_int(runtime, "targetTrackId")) : -1;
	if (runtime)
		obs_data_release(runtime);
	obs_data_release(state);
	if (activeIds.isEmpty()) {
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "message", "No active Motion tracks");
		return;
	}
	std::sort(activeIds.begin(), activeIds.end());
	const QString direction = QString::fromUtf8(obs_data_get_string(request_data, "direction"));
	const int step = direction == QStringLiteral("previous") || direction == QStringLiteral("prev") ? -1 : 1;
	const int currentIndex = activeIds.indexOf(currentTrackId);
	const int targetId = currentIndex < 0 ? (step > 0 ? activeIds.front() : activeIds.back())
					      : activeIds[(currentIndex + step + activeIds.size()) % activeIds.size()];
	obs_data_set_int(request_data, "trackId", targetId);
	vendor_set_motion_target_track(request_data, response_data, nullptr);
}

static void vendor_set_motion_subject_mode(obs_data_t *request_data, obs_data_t *response_data, void *)
{
	if (!vendor_require_workspace(response_data))
		return;
	const QString profileId = vendor_motion_profile_id(request_data);
	auto profile = switcher_dock->MotionProfileDefinition(profileId);
	if (profile.id.isEmpty()) {
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "message", "Motion profile not found");
		return;
	}
	const QString mode = QString::fromUtf8(obs_data_get_string(request_data, "subjectMode"));
	profile.subjectMode = mode.isEmpty() ? QStringLiteral("auto") : mode;
	if (profile.subjectMode != QStringLiteral("locked"))
		profile.lockedTrackId = -1;
	QString effectiveId;
	const bool success = switcher_dock->UpsertMotionProfile(profile, &effectiveId);
	obs_data_set_bool(response_data, "success", success);
	obs_data_set_string(response_data, "profileId", effectiveId.toUtf8().constData());
	obs_data_set_string(response_data, "subjectMode", profile.subjectMode.toUtf8().constData());
}

constexpr VendorRequestRegistration kVendorRequests[] = {
	{"GetCapabilities", vendor_get_switch_capabilities},
	{"Switch.GetCapabilities", vendor_get_switch_capabilities},
	{"GetRemoteState", vendor_get_remote_state},
	{"ListSlots", vendor_list_slots},
	{"SelectPreviewSlot", vendor_select_preview_slot},
	{"Cut", vendor_cut},
	{"Auto", vendor_auto},
	{"GetSelectedSlotState", vendor_get_selected_slot_state},
	{"SetSlotVisibility", vendor_set_slot_visibility},
	{"SetSlotMute", vendor_set_slot_mute},
	{"RestartRemote", vendor_restart_remote},
	{"Workspace.GetState", vendor_get_remote_state},
	{"Workspace.ListSlots", vendor_list_slots},
	{"Workspace.SelectPreviewSlot", vendor_select_preview_slot},
	{"Workspace.Cut", vendor_cut},
	{"Workspace.Auto", vendor_auto},
	{"Workspace.GetSelectedSlotState", vendor_get_selected_slot_state},
	{"Workspace.SetSlotVisibility", vendor_set_slot_visibility},
	{"Workspace.SetSlotMute", vendor_set_slot_mute},
	{"Workspace.RestartRemote", vendor_restart_remote},
	{"ListCanvases", vendor_list_canvases},
	{"SwitchCanvasScene", vendor_switch_canvas_scene},
	{"CreateCanvasScene", vendor_create_canvas_scene},
	{"DeleteCanvasScene", vendor_delete_canvas_scene},
	{"SetCanvasLink", vendor_set_canvas_link},
	{"OpenCanvasWindow", vendor_open_canvas_window},
	{"OpenCanvasProjector", vendor_open_canvas_projector},
	{"PerformOutputAction", vendor_perform_output_action},
	{"Canvas.List", vendor_list_canvases},
	{"Canvas.Get", vendor_get_canvas},
	{"Canvas.SwitchScene", vendor_switch_canvas_scene},
	{"Canvas.CreateScene", vendor_create_canvas_scene},
	{"Canvas.DeleteScene", vendor_delete_canvas_scene},
	{"Canvas.SetLink", vendor_set_canvas_link},
	{"Canvas.SetLinkedScene", vendor_set_linked_scene},
	{"Canvas.OpenWindow", vendor_open_canvas_window},
	{"Canvas.OpenProjector", vendor_open_canvas_projector},
	{"Canvas.GetOutputState", vendor_get_canvas_output_state},
	{"Output.PerformAction", vendor_perform_output_action},
	{"Output.Start", vendor_output_start},
	{"Output.Stop", vendor_output_stop},
	{"Output.Pause", vendor_output_pause},
	{"Output.Split", vendor_output_split},
	{"Output.Chapter", vendor_output_chapter},
	{"Output.SaveReplay", vendor_output_save_replay},
	{"ListMacros", vendor_list_macros},
	{"CreateMacro", vendor_create_macro},
	{"DeleteMacro", vendor_delete_macro},
	{"UpdateMacro", vendor_update_macro},
	{"SetMacroEnabled", vendor_set_macro_enabled},
	{"SetMacroPaused", vendor_set_macro_paused},
	{"TriggerMacro", vendor_trigger_macro},
	{"SetVariable", vendor_set_variable},
	{"DeleteVariable", vendor_delete_variable},
	{"Automation.ListMacros", vendor_list_macros},
	{"Automation.GetMacro", vendor_get_macro},
	{"Automation.UpsertMacro", vendor_upsert_macro},
	{"Automation.DeleteMacro", vendor_delete_macro},
	{"Automation.RunMacro", vendor_trigger_macro},
	{"Automation.SetPaused", vendor_set_macro_paused},
	{"Automation.SetEnabled", vendor_set_macro_enabled},
	{"Automation.ListVariables", vendor_list_variables},
	{"Automation.SetVariable", vendor_set_variable},
	{"Automation.DeleteVariable", vendor_delete_variable},
	{"Automation.ListQueues", vendor_list_queues},
	{"Automation.ClearQueue", vendor_clear_queue},
	{"Automation.GetStatus", vendor_get_automation_status},
	{"Automation.ListConnections", vendor_list_connections},
	{"Automation.TestConnection", vendor_test_connection},
	{"Motion.GetState", vendor_get_motion_state},
	{"Motion.ListProfiles", vendor_list_motion_profiles},
	{"Motion.GetProfile", vendor_get_motion_profile},
	{"Motion.UpsertProfile", vendor_upsert_motion_profile},
	{"Motion.DeleteProfile", vendor_delete_motion_profile},
	{"Motion.SetEnabled", vendor_set_motion_enabled},
	{"Motion.BindSource", vendor_bind_motion_source},
	{"Motion.UnbindSource", vendor_unbind_motion_source},
	{"Motion.ListShots", vendor_list_motion_shots},
	{"Motion.GetShot", vendor_get_motion_shot},
	{"Motion.UpsertShot", vendor_upsert_motion_shot},
	{"Motion.DeleteShot", vendor_delete_motion_shot},
	{"Motion.BindSceneItem", vendor_bind_motion_scene_item},
	{"Motion.SetShotEnabled", vendor_set_motion_shot_enabled},
	{"Motion.SetShotPlayback", vendor_set_motion_shot_playback},
	{"Motion.GetActiveShot", vendor_get_active_motion_shot},
	{"Motion.ListShotPresets", vendor_list_motion_shot_presets},
	{"Motion.ListTracks", vendor_list_motion_tracks},
	{"Motion.GetTarget", vendor_get_motion_target},
	{"Motion.SetTargetTrack", vendor_set_motion_target_track},
	{"Motion.ClearTargetTrack", vendor_clear_motion_target_track},
	{"Motion.CycleTarget", vendor_cycle_motion_target},
	{"Motion.SetSubjectMode", vendor_set_motion_subject_mode},
	{"Motion.GetRuntimeStats", vendor_get_motion_runtime_stats},
};

static void frontend_save_load(obs_data_t *save_data, bool saving, void *)
{
	const auto main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (saving) {
		blog(LOG_INFO, "[Switch] frontend_save_load begin saving");
		obs_data_t *obj = obs_data_create();
		obs_data_array_t *docks = obs_data_array_create();
		for (const auto &it : switcher_docks) {
			auto *dockWidget = it->ParentDockWidget();
			if (!dockWidget) {
				blog(LOG_WARNING, "[Switch] Skipping persistence for dock '%s' without a parent QDockWidget",
				     QT_TO_UTF8(it->DockId()));
				continue;
			}

				auto *ownerWindow = it->OwningMainWindow();
				obs_data_t *dock = obs_data_create();
				if (!it->GetSelected() && it->GetSource()) {
					const char *sourceUuid = obs_source_get_uuid(it->GetSource());
					if (sourceUuid && strlen(sourceUuid) > 0)
						obs_data_set_string(dock, "source_uuid", sourceUuid);
				}
				obs_data_set_bool(dock, "selected", it->GetSelected());
				obs_data_set_string(dock, "dock_id", QT_TO_UTF8(it->DockId()));
				obs_data_set_string(dock, "title", QT_TO_UTF8(it->windowTitle()));
				obs_data_set_bool(dock, "preview", it->PreviewEnabled());
				obs_data_set_bool(dock, "volmeter", it->VolMeterEnabled());
				obs_data_set_bool(dock, "volcontrols", it->VolControlsEnabled());
			obs_data_set_bool(dock, "mediacontrols", it->MediaControlsEnabled());
			obs_data_set_bool(dock, "showtimedecimals", it->GetShowMs());
			obs_data_set_bool(dock, "showtimedremaining", it->GetShowTimeRemaining());
			obs_data_set_bool(dock, "switchscene", it->SwitchSceneEnabled());
			obs_data_set_bool(dock, "showactive", it->ShowActiveEnabled());
			obs_data_set_bool(dock, "sceneitems", it->SceneItemsEnabled());
			obs_data_set_bool(dock, "properties", it->PropertiesEnabled());
			obs_data_set_bool(dock, "filters", it->FiltersEnabled());
			obs_data_set_bool(dock, "textinput", it->TextInputEnabled());
			auto st = it->GetCustomTextInputStyle();
			if (st)
				obs_data_set_obj(dock, "textinputstyle", st);
			obs_data_set_string(dock, "geometry", dockWidget->saveGeometry().toBase64().constData());
			obs_data_set_string(dock, "split", it->saveSplitState().toBase64().constData());
				obs_data_set_string(dock, "window", "");
				obs_data_set_int(dock, "dockarea",
						 (ownerWindow ? ownerWindow : main_window)->dockWidgetArea(dockWidget));
				obs_data_set_bool(dock, "hidden", dockWidget->isHidden());
				obs_data_set_bool(dock, "floating", dockWidget->isFloating());
				obs_data_set_double(dock, "zoom", it->GetZoom());
			obs_data_set_double(dock, "scrollx", it->GetScrollX());
			obs_data_set_double(dock, "scrolly", it->GetScrollY());
			obs_data_array_push_back(docks, dock);
			obs_data_release(dock);
			}
			obs_data_set_array(obj, "docks", docks);
			obs_data_array_release(docks);
			obs_data_set_bool(obj, "corner_tl", main_window->corner(Qt::TopLeftCorner) == Qt::LeftDockWidgetArea);
		obs_data_set_bool(obj, "corner_tr", main_window->corner(Qt::TopRightCorner) == Qt::RightDockWidgetArea);
		obs_data_set_bool(obj, "corner_br", main_window->corner(Qt::BottomRightCorner) == Qt::RightDockWidgetArea);
		obs_data_set_bool(obj, "corner_bl", main_window->corner(Qt::BottomLeftCorner) == Qt::LeftDockWidgetArea);
		if (switcher_dock) {
			blog(LOG_INFO, "[Switch] frontend_save_load before workspace SaveState");
			obs_data_t *switcher = switcher_dock->SaveState();
			blog(LOG_INFO, "[Switch] frontend_save_load after workspace SaveState");
			obs_data_set_obj(obj, "switch", switcher);
			obs_data_release(switcher);
		}
		obs_data_set_obj(save_data, kSwitcherStateKey, obj);

		obs_data_release(obj);
		blog(LOG_INFO, "[Switch] frontend_save_load end saving");
	} else {
		for (const auto &it : switcher_docks) {
			it->close();
			it->deleteLater();
		}
		switcher_docks.clear();

		obs_data_t *obj = obs_data_get_obj(save_data, kSwitcherStateKey);
		if (!obj)
			obj = obs_data_get_obj(save_data, LegacyStateKey().constData());
		if (obj) {
			main_window->setCorner(Qt::TopLeftCorner, obs_data_get_bool(obj, "corner_tl") ? Qt::LeftDockWidgetArea
												      : Qt::TopDockWidgetArea);
			main_window->setCorner(Qt::TopRightCorner, obs_data_get_bool(obj, "corner_tr") ? Qt::RightDockWidgetArea
												       : Qt::TopDockWidgetArea);
			main_window->setCorner(Qt::BottomRightCorner, obs_data_get_bool(obj, "corner_br")
									      ? Qt::RightDockWidgetArea
									      : Qt::BottomDockWidgetArea);
			main_window->setCorner(Qt::BottomLeftCorner, obs_data_get_bool(obj, "corner_bl")
									     ? Qt::LeftDockWidgetArea
									     : Qt::BottomDockWidgetArea);
			obs_frontend_push_ui_translation(obs_module_get_string);
			obs_data_array_t *docks = obs_data_get_array(obj, "docks");
			if (docks) {
				size_t count = obs_data_array_count(docks);
				for (size_t i = 0; i < count; i++) {
					obs_data_t *dock = obs_data_array_item(docks, i);
					const bool selected = obs_data_get_bool(dock, "selected");
					OBSSourceAutoRelease resolvedSource =
						selected ? nullptr : ResolveStoredSource(dock);
					OBSSource source = resolvedSource.Get();
					if (!selected && !source) {
						obs_data_release(dock);
						continue;
					}

						auto *window = main_window;

						QString title = QT_UTF8(obs_data_get_string(dock, "title"));
						if (title.isEmpty() && source)
							title = QT_UTF8(obs_source_get_name(source));
						if (title.isEmpty())
							title = QStringLiteral("Switch Dock %1").arg(i + 1);

						QString dockId = QT_UTF8(obs_data_get_string(dock, "dock_id"));
						if (dockId.isEmpty())
							dockId = SwitcherDock::CreateDockId();

						SwitcherDockRegistrationOptions options;
						options.selected = selected;
						options.dockId = dockId;
						options.preview = obs_data_get_bool(dock, "preview");
						options.volMeter = obs_data_get_bool(dock, "volmeter");
						options.volControls = obs_data_get_bool(dock, "volcontrols");
						options.mediaControls = obs_data_get_bool(dock, "mediacontrols");
						options.switchScene = obs_data_get_bool(dock, "switchscene");
						options.showActive = obs_data_get_bool(dock, "showactive");
						options.properties = obs_data_get_bool(dock, "properties");
						options.filters = obs_data_get_bool(dock, "filters");
						options.textInput = obs_data_get_bool(dock, "textinput");
						options.sceneItems = obs_data_get_bool(dock, "sceneitems");
						options.visible = !obs_data_get_bool(dock, "hidden");
						options.applyPlacement = true;
						options.dockArea = static_cast<Qt::DockWidgetArea>(obs_data_get_int(dock, "dockarea"));
						options.floating = obs_data_get_bool(dock, "floating");

						auto *tmp = CreateRegisteredSwitcherDock(title, source, window, options);
						if (!tmp) {
							obs_data_release(dock);
							continue;
						}

						tmp->SetShowMs(obs_data_get_bool(dock, "showtimedecimals"));
						tmp->SetShowTimeRemaining(obs_data_get_bool(dock, "showtimedremaining"));
						auto st = obs_data_get_obj(dock, "textinputstyle");
						tmp->SetCustomTextInputStyle(st);
						obs_data_release(st);

						auto *d = tmp->ParentDockWidget();
						if (!d) {
							obs_data_release(dock);
							continue;
						}

						const char *geometry = obs_data_get_string(dock, "geometry");
						if (geometry && strlen(geometry))
							d->restoreGeometry(QByteArray::fromBase64(QByteArray(geometry)));
						const char *split = obs_data_get_string(dock, "split");
						if (split && strlen(split))
							tmp->restoreSplitState(QByteArray::fromBase64(QByteArray(split)));

						tmp->SetZoom(obs_data_get_double(dock, "zoom"));
					tmp->SetScrollX(obs_data_get_double(dock, "scrollx"));
					tmp->SetScrollY(obs_data_get_double(dock, "scrolly"));
					obs_data_release(dock);
				}
				obs_data_array_release(docks);
			}
				obs_frontend_pop_ui_translation();
			obs_data_t *switcher = obs_data_get_obj(obj, "switch");
			if (switcher_dock)
				switcher_dock->LoadState(switcher);
			if (switcher)
				obs_data_release(switcher);
			obs_data_release(obj);
			} else if (switcher_dock) {
				switcher_dock->ClearSceneCollectionState();
			}
		}
}

static void item_select(void *p, calldata_t *calldata)
{
	UNUSED_PARAMETER(p);
	auto item = (obs_sceneitem_t *)calldata_ptr(calldata, "item");
	auto source = obs_sceneitem_get_source(item);
	for (const auto &it : switcher_docks) {
		if (!it->GetSelected())
			continue;
		it->SetSource(source);
	}
}

static OBSSource previous_scene = nullptr;
static OBSSignal previousSceneItemSelect;
static OBSSignal previousSceneRemove;
static OBSSignal previousSceneDestroy;

bool get_selected_source(obs_scene_t *obs_scene, obs_sceneitem_t *item, void *p)
{
	UNUSED_PARAMETER(obs_scene);
	if (!obs_sceneitem_selected(item))
		return true;
	auto source = obs_sceneitem_get_source(item);
	*(obs_source_t **)p = source;
	return false;
}

void update_selected_source()
{
	if (!previous_scene)
		return;
	auto scene = obs_scene_from_source(previous_scene);
	if (!scene)
		return;
	obs_source_t *selected_source = nullptr;
	obs_scene_enum_items(scene, get_selected_source, &selected_source);
	if (!selected_source)
		return;
	for (const auto &it : switcher_docks) {
		if (!it->GetSelected())
			continue;
		it->SetSource(selected_source);
	}
}

void set_previous_scene_empty(void *p, calldata_t *calldata)
{
	UNUSED_PARAMETER(p);
	UNUSED_PARAMETER(calldata);
	if (!previous_scene)
		return;
	previousSceneItemSelect.Disconnect();
	previousSceneRemove.Disconnect();
	previousSceneDestroy.Disconnect();
	previous_scene = nullptr;
}

void attach_previous_scene(obs_source_t *scene)
{
	if (!scene)
		return;

	previous_scene = scene;
	auto *sh = obs_source_get_signal_handler(previous_scene);
	if (!sh)
		return;

	previousSceneItemSelect.Connect(sh, "item_select", item_select, nullptr);
	previousSceneRemove.Connect(sh, "remove", set_previous_scene_empty, nullptr);
	previousSceneDestroy.Connect(sh, "destroy", set_previous_scene_empty, nullptr);
}

static void frontend_event(enum obs_frontend_event event, void *)
{
	bool forwardedToWorkspace = false;
	if (event == OBS_FRONTEND_EVENT_SCENE_CHANGED || event == OBS_FRONTEND_EVENT_TRANSITION_STOPPED ||
	    event == OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED) {
		switch_ai_tracker_interrupt_all_inference();
	}

	if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING ||
	    event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN ||
	    event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP ||
	    event == OBS_FRONTEND_EVENT_EXIT) {
		blog(LOG_INFO, "[Switch] frontend_event %s", SwitchFrontendEventName(event));
	}

	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		frontendShutdownStarted = false;
		switch_ai_tracker_set_frontend_quiescing(false);
	} else if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING) {
		switch_ai_tracker_set_frontend_quiescing(true);
		set_previous_scene_empty(nullptr, nullptr);
		for (const auto &it : switcher_docks)
			it->SetSource(nullptr);
		if (switcher_dock) {
			switcher_dock->HandleFrontendEvent(event);
			forwardedToWorkspace = true;
		}
	} else if (event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN ||
		   event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP ||
		   event == OBS_FRONTEND_EVENT_EXIT) {
		frontendShutdownStarted = true;
		switch_ai_tracker_set_frontend_quiescing(true);
		set_previous_scene_empty(nullptr, nullptr);
		for (const auto &it : switcher_docks) {
			it->SetSource(nullptr);
			obs_frontend_remove_dock(it->objectName().toUtf8().constData());
		}
		switcher_docks.clear();
		if (switcher_dock) {
			switcher_dock->HandleFrontendEvent(event);
			forwardedToWorkspace = true;
			if (event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN)
				DestroyWorkspaceDockForFrontendShutdown();
		}
	} else if (event == OBS_FRONTEND_EVENT_SCENE_CHANGED || event == OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED ||
		   event == OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED || event == OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED) {
		if (previous_scene) {
			set_previous_scene_empty(nullptr, nullptr);
		}
		auto *preview = obs_frontend_get_current_preview_scene();
		if (preview) {
			attach_previous_scene(preview);
			obs_source_release(preview);
		} else {
			auto scene = obs_frontend_get_current_scene();
			if (scene) {
				attach_previous_scene(scene);
				obs_source_release(scene);
			}
		}
		update_selected_source();
		RefreshDetachedDockActiveStates();
		if (switcher_dock) {
			switcher_dock->HandleFrontendEvent(event);
			forwardedToWorkspace = true;
		}
	} else if (event == OBS_FRONTEND_EVENT_STREAMING_STARTING || event == OBS_FRONTEND_EVENT_STREAMING_STARTED ||
		   event == OBS_FRONTEND_EVENT_STREAMING_STOPPING || event == OBS_FRONTEND_EVENT_STREAMING_STOPPED ||
		   event == OBS_FRONTEND_EVENT_RECORDING_STARTING || event == OBS_FRONTEND_EVENT_RECORDING_STARTED ||
		   event == OBS_FRONTEND_EVENT_RECORDING_STOPPING || event == OBS_FRONTEND_EVENT_RECORDING_STOPPED ||
		   event == OBS_FRONTEND_EVENT_RECORDING_PAUSED || event == OBS_FRONTEND_EVENT_RECORDING_UNPAUSED) {
		RefreshDetachedDockActiveStates();
		if (switcher_dock) {
			switcher_dock->HandleFrontendEvent(event);
			forwardedToWorkspace = true;
		}
	}
	if (!forwardedToWorkspace && switcher_dock)
		switcher_dock->HandleFrontendEvent(event);
	SwitcherRemoteManager::Instance()->HandleFrontendEvent(event);
}

static void source_remove(void *data, calldata_t *call_data)
{
	UNUSED_PARAMETER(data);
	obs_source_t *source = static_cast<obs_source_t *>(calldata_ptr(call_data, "source"));
	for (auto it = switcher_docks.begin(); it != switcher_docks.end();) {
		if ((*it)->GetSource().Get() == source) {
			obs_frontend_remove_dock((*it)->objectName().toUtf8().constData());
			it = switcher_docks.erase(it);
		} else {
			++it;
		}
	}
	if (switcher_dock)
		switcher_dock->HandleSourceRemoved(source);
	SwitcherRemoteManager::Instance()->HandleSourceRemoved(source);
}

bool obs_module_load()
{
	if (!SupportsCurrentObsVersion()) {
		blog(LOG_ERROR, "[Switch] OBS Studio %d.%d.%d or newer is required. Detected %s.", kMinimumObsMajor,
		     kMinimumObsMinor, kMinimumObsPatch, obs_get_version_string());
		return false;
	}

	blog(LOG_INFO, "[Switch] loaded version %s", PROJECT_VERSION);

	switch_ai_tracker_set_frontend_quiescing(false);
	obs_register_source(&switch_ai_tracker_info);

	obs_frontend_add_save_callback(frontend_save_load, nullptr);
	obs_frontend_add_event_callback(frontend_event, nullptr);
	frontendCallbacksRegistered = true;
	signal_handler_connect(obs_get_signal_handler(), "source_remove", source_remove, nullptr);
	sourceRemoveSignalRegistered = true;

	auto *remoteManager = SwitcherRemoteManager::Instance();
	// OBS display widgets must not share OBS's main-window hierarchy on macOS.
	switcher_dock = new SwitcherWorkspaceDock();
	switcher_dock->hide();
	QObject::connect(switcher_dock, &QObject::destroyed, [](QObject *destroyed) {
		if (switcher_dock == destroyed)
			switcher_dock = nullptr;
		SwitcherRemoteManager::Instance()->SetWorkspace(nullptr);
	});
	remoteManager->SetWorkspace(switcher_dock);

	const auto action = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(obs_module_text("Switcher")));

	auto cb = [] {
		obs_frontend_push_ui_translation(obs_module_get_string);

		if (switcher_dock)
			switcher_dock->OpenWorkspaceWindow();
		obs_frontend_pop_ui_translation();
	};

	QAction::connect(action, &QAction::triggered, cb);
	return true;
}

void obs_module_post_load(void)
{
	switcher_vendor = obs_websocket_register_vendor("Switch");
	if (!switcher_vendor)
		return;

	for (const auto &request : kVendorRequests)
		obs_websocket_vendor_register_request(switcher_vendor, request.name, request.handler, nullptr);
}

void obs_module_unload()
{
	switch_ai_tracker_set_frontend_quiescing(true);
	const bool frontendApiAvailable = !frontendShutdownStarted && obs_frontend_get_main_window() != nullptr;
	if (frontendCallbacksRegistered && frontendApiAvailable && !frontendShutdownStarted) {
		obs_frontend_remove_save_callback(frontend_save_load, nullptr);
		obs_frontend_remove_event_callback(frontend_event, nullptr);
		frontendCallbacksRegistered = false;
	}
	if (sourceRemoveSignalRegistered) {
		signal_handler_disconnect(obs_get_signal_handler(), "source_remove", source_remove, nullptr);
		sourceRemoveSignalRegistered = false;
	}
	set_previous_scene_empty(nullptr, nullptr);
	SwitcherRemoteManager::Instance()->Shutdown();
	SwitcherRemoteManager::Instance()->SetWorkspace(nullptr);
	if (switcher_dock) {
		if (frontendShutdownStarted) {
			switcher_dock->hide();
			SwitcherRemoteManager::Instance()->SetWorkspace(nullptr);
			switcher_dock = nullptr;
		} else {
			if (frontendApiAvailable && !frontendShutdownStarted) {
				obs_frontend_remove_dock(kSwitcherWorkspaceDockId);
				obs_frontend_remove_dock(LegacyWorkspaceDockId().constData());
				obs_frontend_remove_dock("switch-vertical-canvas");
				obs_frontend_remove_dock("switch-vertical-scenes");
				obs_frontend_remove_dock("switch-vertical-sources");
				obs_frontend_remove_dock("switch-vertical-transitions");
				obs_frontend_remove_dock("switch-vertical-settings");
			}
			switcher_dock->close();
			delete switcher_dock;
			switcher_dock = nullptr;
			FlushQtDeferredDeletesForSwitchShutdown();
		}
	}

	if (switcher_vendor) {
		for (const auto &request : kVendorRequests)
			obs_websocket_vendor_unregister_request(switcher_vendor, request.name);
		switcher_vendor = nullptr;
	}
	FlushQtDeferredDeletesForSwitchShutdown();
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Description");
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return obs_module_text("Switcher");
}

#define LOG_OFFSET_DB 6.0f
#define LOG_RANGE_DB 96.0f
/* equals -log10f(LOG_OFFSET_DB) */
#define LOG_OFFSET_VAL -0.77815125038364363f
/* equals -log10f(-LOG_RANGE_DB + LOG_OFFSET_DB) */
#define LOG_RANGE_VAL -2.00860017176191756f

SwitcherDock::SwitcherDock(QString name, bool selected_, QWidget *parent)
	: QSplitter(parent),
	  eventFilter(BuildEventFilter()),
	  selected(selected_)
{

	setWindowTitle(name);
	setObjectName(name);

	setOrientation(Qt::Vertical);
	setChildrenCollapsible(false);
}

SwitcherDock::~SwitcherDock()
{
	DisableFilters();
	DisableProperties();
	DisableSceneItems();
	DisableShowActive();
	DisableVolMeter();
	DisableVolControls();
	DisableMediaControls();
	DisablePreview();
	obs_data_release(textInputCustomStyle);
}

void SwitcherDock::changeEvent(QEvent *event)
{
	QSplitter::changeEvent(event);

	switch (event->type()) {
	case QEvent::PaletteChange:
	case QEvent::StyleChange:
	case QEvent::ApplicationPaletteChange:
		ActiveChanged();
		break;
	default:
		break;
	}
}

bool SwitcherDock::event(QEvent *event)
{
	return QSplitter::event(event);
}

void SwitcherDock::showEvent(QShowEvent *event)
{
	QSplitter::showEvent(event);
}

void SwitcherDock::hideEvent(QHideEvent *event)
{
	QSplitter::hideEvent(event);
}

QString SwitcherDock::CreateDockId()
{
	return QStringLiteral("switcher-dock-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

void SwitcherDock::SetDockId(const QString &id)
{
	if (!id.isEmpty())
		setObjectName(id);
}

QDockWidget *SwitcherDock::ParentDockWidget() const
{
	for (QWidget *parent = parentWidget(); parent; parent = parent->parentWidget()) {
		if (auto *dockWidget = qobject_cast<QDockWidget *>(parent))
			return dockWidget;
	}

	return nullptr;
}

QMainWindow *SwitcherDock::OwningMainWindow() const
{
	for (QWidget *parent = parentWidget(); parent; parent = parent->parentWidget()) {
		if (auto *mainWindow = qobject_cast<QMainWindow *>(parent))
			return mainWindow;
	}

	return nullptr;
}

void SwitcherDock::ApplyDockWidgetFeatures(QDockWidget *dockWidget)
{
	if (!dockWidget)
		return;

	const auto features = dockWidget->features();
	const bool floating = dockWidget->isFloating();
	const auto desired =
		floating ? (features | QDockWidget::DockWidgetClosable) : (features & ~QDockWidget::DockWidgetClosable);
	if (features != desired)
		dockWidget->setFeatures(desired);

	if (!floating) {
		if (auto *closeButton =
			    dockWidget->findChild<QAbstractButton *>(QStringLiteral("qt_dockwidget_closebutton"))) {
			closeButton->hide();
			closeButton->setEnabled(false);
		}
	}
}

void SwitcherDock::BindDockWidgetLifecycle(QDockWidget *dockWidget)
{
	if (wrapperDockWidget == dockWidget) {
		wrapperDockVisible = !dockWidget || dockWidget->isVisible();
		ApplyDockWidgetFeatures(dockWidget);
		return;
	}

	wrapperDockWidget = dockWidget;
	wrapperDockVisible = !dockWidget || dockWidget->isVisible();
	if (!dockWidget)
		return;

	ApplyDockWidgetFeatures(dockWidget);
	connect(dockWidget, &QDockWidget::visibilityChanged, this, &SwitcherDock::DockWidgetVisibilityChanged,
		Qt::UniqueConnection);
	connect(dockWidget, &QDockWidget::featuresChanged, this,
		[this, dockWidget](QDockWidget::DockWidgetFeatures) { ApplyDockWidgetFeatures(dockWidget); });
	connect(dockWidget, &QDockWidget::topLevelChanged, this,
		[this, dockWidget](bool) { QTimer::singleShot(0, dockWidget, [this, dockWidget]() { ApplyDockWidgetFeatures(dockWidget); }); });
	connect(dockWidget, &QObject::destroyed, this, [this]() {
		wrapperDockWidget = nullptr;
		wrapperDockVisible = false;
		DeactivatePreview();
	});
	QTimer::singleShot(0, dockWidget, [this, dockWidget]() { ApplyDockWidgetFeatures(dockWidget); });
}

SwitcherDock *CreateRegisteredSwitcherDock(const QString &title, const OBSSource &source, QMainWindow *mainWindow,
					   const SwitcherDockRegistrationOptions &options)
{
	if (!mainWindow)
		mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());

	auto *dock = new SwitcherDock(title, options.selected, mainWindow);
	const QString dockId = options.dockId.isEmpty() ? SwitcherDock::CreateDockId() : options.dockId;
	dock->SetDockId(dockId);
	dock->SetSource(source);
	ApplyDockRegistrationOptions(dock, options);

	const auto dockIdUtf8 = dockId.toUtf8();
	const auto dockTitleUtf8 = title.toUtf8();
	if (!obs_frontend_add_dock_by_id(dockIdUtf8.constData(), dockTitleUtf8.constData(), dock)) {
		blog(LOG_WARNING, "[Switch] Failed to add dock '%s' because the id was rejected", dockIdUtf8.constData());
		delete dock;
		return nullptr;
	}

	switcher_docks.push_back(dock);
	auto *dockWidget = dock->ParentDockWidget();
	if (!dockWidget) {
		blog(LOG_WARNING, "[Switch] Failed to locate parent dock widget for dock '%s'", dockIdUtf8.constData());
		obs_frontend_remove_dock(dockIdUtf8.constData());
		switcher_docks.pop_back();
		delete dock;
		return nullptr;
	}
	dock->BindDockWidgetLifecycle(dockWidget);

	if (options.applyPlacement && mainWindow) {
		if (options.dockArea != Qt::NoDockWidgetArea)
			mainWindow->addDockWidget(options.dockArea, dockWidget);
		if (dockWidget->isFloating() != options.floating)
			dockWidget->setFloating(options.floating);
	}

	if (options.visible)
		dockWidget->show();
	else
		dockWidget->hide();

	if (options.floating && options.floatingSize.isValid()) {
		const QSize targetSize = options.floatingSize;
		QTimer::singleShot(0, dockWidget, [dockWidget, targetSize]() {
			if (dockWidget)
				dockWidget->resize(targetSize);
		});
	}

	return dock;
}

static inline void GetScaleAndCenterPos(int baseCX, int baseCY, int windowCX, int windowCY, int &x, int &y, float &scale)
{
	int newCX, newCY;

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

void SwitcherDock::DrawPreview(void *data, uint32_t cx, uint32_t cy)
{
	SwitcherDock *window = static_cast<SwitcherDock *>(data);

	if (!window->source)
		return;

	uint32_t sourceCX = obs_source_get_width(window->source);
	if (sourceCX <= 0)
		sourceCX = 1;
	uint32_t sourceCY = obs_source_get_height(window->source);
	if (sourceCY <= 0)
		sourceCY = 1;

	int x, y;
	float scale;

	GetScaleAndCenterPos(sourceCX, sourceCY, cx, cy, x, y, scale);
	auto newCX = scale * float(sourceCX);
	auto newCY = scale * float(sourceCY);

	auto extraCx = (window->zoom - 1.0f) * newCX;
	auto extraCy = (window->zoom - 1.0f) * newCY;
	int newCx = newCX * window->zoom;
	int newCy = newCY * window->zoom;
	x -= extraCx * window->scrollX;
	y -= extraCy * window->scrollY;
	gs_viewport_push();
	gs_projection_push();
	const bool previous = gs_set_linear_srgb(true);

	gs_ortho(0.0f, float(sourceCX), 0.0f, float(sourceCY), -100.0f, 100.0f);
	gs_set_viewport(x, y, newCx, newCy);
	obs_source_video_render(window->source);

	gs_set_linear_srgb(previous);
	gs_projection_pop();
	gs_viewport_pop();
}

void SwitcherDock::OBSVolumeLevel(void *data, const float magnitude[MAX_AUDIO_CHANNELS], const float peak[MAX_AUDIO_CHANNELS],
				const float inputPeak[MAX_AUDIO_CHANNELS])
{
	SwitcherDock *dockWidget = static_cast<SwitcherDock *>(data);
	if (dockWidget->volMeter)
		dockWidget->volMeter->setLevels(magnitude, peak, inputPeak);
}

void SwitcherDock::OBSVolume(void *data, calldata_t *call_data)
{
	obs_source_t *source;
	calldata_get_ptr(call_data, "source", &source);
	double volume;
	calldata_get_float(call_data, "volume", &volume);
	SwitcherDock *dockWidget = static_cast<SwitcherDock *>(data);
	QMetaObject::invokeMethod(dockWidget, "SetOutputVolume", Qt::QueuedConnection, Q_ARG(double, volume));
}

void SwitcherDock::OBSMute(void *data, calldata_t *call_data)
{
	obs_source_t *source;
	calldata_get_ptr(call_data, "source", &source);
	bool muted = calldata_bool(call_data, "muted");
	SwitcherDock *dockWidget = static_cast<SwitcherDock *>(data);
	QMetaObject::invokeMethod(dockWidget, "SetMute", Qt::QueuedConnection, Q_ARG(bool, muted));
}

void SwitcherDock::OBSActiveChanged(void *data, calldata_t *call_data)
{
	UNUSED_PARAMETER(call_data);
	SwitcherDock *dockWidget = static_cast<SwitcherDock *>(data);
	QMetaObject::invokeMethod(dockWidget, "ActiveChanged", Qt::QueuedConnection);
}

void SwitcherDock::UpdateVolumeSignals()
{
	muteSignal.Disconnect();
	volumeSignal.Disconnect();

	if (!source || !volControl || !volControl->isVisibleTo(this))
		return;

	if (auto *sh = obs_source_get_signal_handler(source)) {
		muteSignal.Connect(sh, "mute", OBSMute, this);
		volumeSignal.Connect(sh, "volume", OBSVolume, this);
	}
}

void SwitcherDock::UpdateActiveSignals()
{
	activateSignal.Disconnect();
	deactivateSignal.Disconnect();

	if (!source || !ShowActiveEnabled())
		return;

	if (auto *sh = obs_source_get_signal_handler(source)) {
		activateSignal.Connect(sh, "activate", OBSActiveChanged, this);
		deactivateSignal.Connect(sh, "deactivate", OBSActiveChanged, this);
	}
}

void SwitcherDock::LockVolumeControl(bool lock)
{
	slider->setEnabled(!lock);
	mute->setEnabled(!lock);
}

void SwitcherDock::MuteVolumeControl(bool mute)
{
	if (source && obs_source_muted(source) != mute)
		obs_source_set_muted(source, mute);
}

void SwitcherDock::SetOutputVolume(double volume)
{
	float db = obs_mul_to_db(volume);
	float def;
	if (db >= 0.0f)
		def = 1.0f;
	else if (db <= -96.0f)
		def = 0.0f;
	else
		def = (-log10f(-db + LOG_OFFSET_DB) - LOG_RANGE_VAL) / (LOG_OFFSET_VAL - LOG_RANGE_VAL);

	int val = def * 10000.0;
	slider->setValue(val);
}

void SwitcherDock::SetMute(bool muted)
{
	mute->setChecked(muted);
}

void SwitcherDock::ActiveChanged()
{
	int active = ACTIVE_NONE;
	if (!source) {
		SetActive(active);
		return;
	}

	if (auto *preview = obs_frontend_get_current_preview_scene()) {
		if (source == preview) {
			active = ACTIVE_PREVIEW;
		}
		std::pair<obs_source_t *, int> t = std::pair<obs_source_t *, int>(source, active);
		obs_source_enum_active_tree(
			preview,
			[](obs_source_t *parent, obs_source_t *child, void *param) {
				UNUSED_PARAMETER(parent);
				auto m = (std::pair<obs_source_t *, int> *)param;
				if (m->first == child) {
					m->second = ACTIVE_PREVIEW;
				}
			},
			&t);
		active = t.second;
		obs_source_release(preview);
	}
	for (uint32_t channel = 1; channel < MAX_CHANNELS; channel++) {
		auto dsk = obs_get_output_source(channel);
		if (!dsk)
			continue;
		if (source == dsk) {
			active = ACTIVE_DOWNSTREAM_KEYER;
		}
		std::pair<obs_source_t *, int> t = std::pair<obs_source_t *, int>(source, active);
		obs_source_enum_active_tree(
			dsk,
			[](obs_source_t *parent, obs_source_t *child, void *param) {
				UNUSED_PARAMETER(parent);
				auto m = static_cast<std::pair<obs_source_t *, int> *>(param);
				if (m->first == child) {
					m->second = ACTIVE_DOWNSTREAM_KEYER;
				}
			},
			&t);
		active = t.second;

		obs_source_release(dsk);
	}
	if (auto *program = obs_frontend_get_current_scene()) {
		if (source == program) {
			active = ACTIVE_PROGRAM;
		}
		std::pair<obs_source_t *, int> t = std::pair<obs_source_t *, int>(source, active);
		obs_source_enum_active_tree(
			program,
			[](obs_source_t *parent, obs_source_t *child, void *param) {
				UNUSED_PARAMETER(parent);
				auto m = static_cast<std::pair<obs_source_t *, int> *>(param);
				if (m->first == child) {
					m->second = ACTIVE_PROGRAM;
				}
			},
			&t);
		active = t.second;
		obs_source_release(program);
	}
	if (active == ACTIVE_PROGRAM) {
		if (obs_frontend_streaming_active()) {
			if (obs_frontend_recording_active() && !obs_frontend_recording_paused()) {
				active = ACTIVE_RECORDING_AND_STREAMING;
			} else {
				active = ACTIVE_STREAMING;
			}
		} else if (obs_frontend_recording_active()) {
			if (obs_frontend_recording_paused()) {
				active = ACTIVE_RECORDING_PAUSED;
			} else {
				active = ACTIVE_RECORDING;
			}
		}
	}
	SetActive(active);
}

void SwitcherDock::SetActive(int active)
{
	if (activeFrame)
		activeFrame->setStyleSheet(ActiveFrameStyleSheet(activeFrame, active));

	if (activeLabel) {
		if (active == ACTIVE_STREAMING) {
			activeLabel->setProperty("themeID", "error");
			activeLabel->setProperty("class", "text-danger");
			activeLabel->setText(QT_UTF8(obs_module_text("Streaming")));
		} else if (active == ACTIVE_RECORDING) {
			activeLabel->setProperty("themeID", "error");
			activeLabel->setProperty("class", "text-danger");
			activeLabel->setText(QT_UTF8(obs_module_text("Recording")));
		} else if (active == ACTIVE_RECORDING_AND_STREAMING) {
			activeLabel->setProperty("themeID", "error");
			activeLabel->setProperty("class", "text-danger");
			activeLabel->setText(QT_UTF8(obs_module_text("StreamingAndRecording")));
		} else if (active == ACTIVE_RECORDING_PAUSED) {
			activeLabel->setProperty("themeID", "warning");
			activeLabel->setProperty("class", "text-warning");
			activeLabel->setText(QT_UTF8(obs_module_text("RecordingPaused")));
		} else if (active == ACTIVE_PROGRAM) {
			activeLabel->setProperty("themeID", obs_frontend_preview_program_mode_active() ? "error" : "good");
			activeLabel->setProperty("class",
						 obs_frontend_preview_program_mode_active() ? "text-danger" : "text-success");
			activeLabel->setText(QT_UTF8(obs_module_text("Active")));
		} else if (active == ACTIVE_PREVIEW) {
			activeLabel->setProperty("themeID", "good");
			activeLabel->setProperty("class", "text-success");
			activeLabel->setText(QT_UTF8(obs_module_text("Preview")));
		} else if (active == ACTIVE_DOWNSTREAM_KEYER) {
			activeLabel->setProperty("themeID", "warning");
			activeLabel->setProperty("class", "text-warning");
			activeLabel->setText(QT_UTF8(obs_module_text("DownstreamKeyer")));
		} else {
			activeLabel->setText(QT_UTF8(obs_module_text("NotActive")));
			activeLabel->setProperty("themeID", "");
			activeLabel->setProperty("class", "");
		}

		/* force style sheet recalculation */
		QString qss = activeLabel->styleSheet();
		activeLabel->setStyleSheet("/* */");
		activeLabel->setStyleSheet(qss);
	}
}

void SwitcherDock::SliderChanged(int vol)
{
	float def = (float)vol / 10000.0f;
	float db;
	if (def >= 1.0f)
		db = 0.0f;
	else if (def <= 0.0f)
		db = -INFINITY;
	else
		db = -(LOG_RANGE_DB + LOG_OFFSET_DB) * powf((LOG_RANGE_DB + LOG_OFFSET_DB) / LOG_OFFSET_DB, -def) + LOG_OFFSET_DB;
	const float mul = obs_db_to_mul(db);
	if (source)
		obs_source_set_volume(source, mul);
}

bool SwitcherDock::GetSourceRelativeXY(int mouseX, int mouseY, int &relX, int &relY)
{
	float pixelRatio = devicePixelRatioF();

	int mouseXscaled = (int)roundf(mouseX * pixelRatio);
	int mouseYscaled = (int)roundf(mouseY * pixelRatio);

	QSize size = preview->size() * preview->devicePixelRatioF();

	uint32_t sourceCX = source ? obs_source_get_width(source) : 1;
	if (sourceCX <= 0)
		sourceCX = 1;
	uint32_t sourceCY = source ? obs_source_get_height(source) : 1;
	if (sourceCY <= 0)
		sourceCY = 1;

	int x, y;
	float scale;

	GetScaleAndCenterPos(sourceCX, sourceCY, size.width(), size.height(), x, y, scale);

	auto newCX = scale * float(sourceCX);
	auto newCY = scale * float(sourceCY);

	auto extraCx = (zoom - 1.0f) * newCX;
	auto extraCy = (zoom - 1.0f) * newCY;

	scale *= zoom;

	if (x > 0) {
		relX = int(float(mouseXscaled - x + extraCx * scrollX) / scale);
		relY = int(float(mouseYscaled + extraCy * scrollY) / scale);
	} else {
		relX = int(float(mouseXscaled + extraCx * scrollX) / scale);
		relY = int(float(mouseYscaled - y + extraCy * scrollY) / scale);
	}

	// Confirm mouse is inside the source
	if (relX < 0 || relX > int(sourceCX))
		return false;
	if (relY < 0 || relY > int(sourceCY))
		return false;

	return true;
}

OBSEventFilter *SwitcherDock::BuildEventFilter()
{
	return new OBSEventFilter([this](QObject *obj, QEvent *event) {
		UNUSED_PARAMETER(obj);

	switch (event->type()) {
	case QEvent::ContextMenu:
		return this->HandleContextMenuEvent(static_cast<QContextMenuEvent *>(event));
	case QEvent::MouseButtonPress:
	case QEvent::MouseButtonRelease:
	case QEvent::MouseButtonDblClick:
			return this->HandleMouseClickEvent(static_cast<QMouseEvent *>(event));
		case QEvent::MouseMove:
			return this->HandleMouseMoveEvent(static_cast<QMouseEvent *>(event));
		case QEvent::Enter:
		case QEvent::Leave:
			return this->HandleMouseBoundaryEvent(event);

		case QEvent::Wheel:
			return this->HandleMouseWheelEvent(static_cast<QWheelEvent *>(event));
		case QEvent::FocusIn:
		case QEvent::FocusOut:
			return this->HandleFocusEvent(static_cast<QFocusEvent *>(event));
		case QEvent::KeyPress:
		case QEvent::KeyRelease:
			return this->HandleKeyEvent(static_cast<QKeyEvent *>(event));
		default:
			return false;
		}
	});
}

static int TranslateQtKeyboardEventModifiers(Qt::KeyboardModifiers modifiers, bool mouseEvent)
{
	int obsModifiers = INTERACT_NONE;

	if (modifiers.testFlag(Qt::ShiftModifier))
		obsModifiers |= INTERACT_SHIFT_KEY;
	if (modifiers.testFlag(Qt::AltModifier))
		obsModifiers |= INTERACT_ALT_KEY;
#ifdef __APPLE__
	// Mac: Meta = Control, Control = Command
	if (modifiers.testFlag(Qt::ControlModifier))
		obsModifiers |= INTERACT_COMMAND_KEY;
	if (modifiers.testFlag(Qt::MetaModifier))
		obsModifiers |= INTERACT_CONTROL_KEY;
#else
	// Handle windows key? Can a browser even trap that key?
	if (modifiers.testFlag(Qt::ControlModifier))
		obsModifiers |= INTERACT_CONTROL_KEY;
#endif

	if (!mouseEvent) {
		if (modifiers.testFlag(Qt::KeypadModifier))
			obsModifiers |= INTERACT_IS_KEY_PAD;
	}

	return obsModifiers;
}

static int TranslateQtKeyboardEventModifiers(QInputEvent *event, bool mouseEvent)
{
	return TranslateQtKeyboardEventModifiers(event ? event->modifiers() : Qt::NoModifier, mouseEvent);
}

static int TranslateQtMouseEventModifiers(Qt::MouseButtons buttons, Qt::KeyboardModifiers keyboardModifiers)
{
	int modifiers = TranslateQtKeyboardEventModifiers(keyboardModifiers, true);

	if (buttons.testFlag(Qt::LeftButton))
		modifiers |= INTERACT_MOUSE_LEFT;
	if (buttons.testFlag(Qt::MiddleButton))
		modifiers |= INTERACT_MOUSE_MIDDLE;
	if (buttons.testFlag(Qt::RightButton))
		modifiers |= INTERACT_MOUSE_RIGHT;

	return modifiers;
}

static int TranslateQtMouseEventModifiers(QMouseEvent *event)
{
	return TranslateQtMouseEventModifiers(event ? event->buttons() : Qt::NoButton,
					      event ? event->modifiers() : Qt::NoModifier);
}

static bool CloseFloat(float a, float b, float epsilon = 0.01)
{
	using std::abs;
	return abs(a - b) <= epsilon;
}

struct click_event {
	int32_t x;
	int32_t y;
	uint32_t modifiers;
	int32_t button;
	bool mouseUp;
	uint32_t clickCount;
};

static bool HandleSceneMouseClickEvent(obs_scene_t *scene, obs_sceneitem_t *item, void *data)
{
	UNUSED_PARAMETER(scene);
	auto click_event = static_cast<struct click_event *>(data);

	matrix4 transform;
	matrix4 invTransform;
	vec3 transformedPos;
	vec3 pos3;
	vec3 pos3_;

	vec3_set(&pos3, click_event->x, click_event->y, 0.0f);

	obs_sceneitem_get_box_transform(item, &transform);

	matrix4_inv(&invTransform, &transform);
	vec3_transform(&transformedPos, &pos3, &invTransform);
	vec3_transform(&pos3_, &transformedPos, &transform);

	if (click_event->mouseUp || (CloseFloat(pos3.x, pos3_.x) && CloseFloat(pos3.y, pos3_.y) && transformedPos.x >= 0.0f &&
				     transformedPos.x <= 1.0f && transformedPos.y >= 0.0f && transformedPos.y <= 1.0f)) {
		auto source = obs_sceneitem_get_source(item);
		obs_mouse_event mouseEvent{};
		mouseEvent.x = transformedPos.x * obs_source_get_base_width(source);
		mouseEvent.y = transformedPos.y * obs_source_get_base_height(source);
		mouseEvent.modifiers = click_event->modifiers;
		obs_source_send_mouse_click(source, &mouseEvent, click_event->button, click_event->mouseUp,
					    click_event->clickCount);
	}
	return true;
}

bool SwitcherDock::HandleMouseClickEvent(QMouseEvent *event)
{
#ifdef __APPLE__
	const bool controlClickContext =
		event && event->button() == Qt::LeftButton && event->modifiers().testFlag(Qt::ControlModifier);
#else
	const bool controlClickContext = false;
#endif
	if (workspaceContextMenuEnabled && event && (event->button() == Qt::RightButton || controlClickContext))
		return true;

	const bool mouseUp = event->type() == QEvent::MouseButtonRelease;
	if (!mouseUp && event->button() == Qt::LeftButton && event->modifiers().testFlag(Qt::ControlModifier)) {
		scrollingFromX = event->pos().x();
		scrollingFromY = event->pos().y();
	}
	uint32_t clickCount = 1;
	if (event->type() == QEvent::MouseButtonDblClick)
		clickCount = 2;

	struct obs_mouse_event mouseEvent = {};

	mouseEvent.modifiers = TranslateQtMouseEventModifiers(event);

	int32_t button = 0;

	switch (event->button()) {
	case Qt::LeftButton:
		button = MOUSE_LEFT;
		break;
	case Qt::MiddleButton:
		button = MOUSE_MIDDLE;
		break;
	case Qt::RightButton:
		button = MOUSE_RIGHT;
		break;
	default:
		blog(LOG_WARNING, "unknown button type %d", event->button());
		return false;
	}

	// Why doesn't this work?
	//if (event->flags().testFlag(Qt::MouseEventCreatedDoubleClick))
	//	clickCount = 2;

	const bool insideSource = GetSourceRelativeXY(event->pos().x(), event->pos().y(), mouseEvent.x, mouseEvent.y);
	if (insideSource) {
		lastMouseX = mouseEvent.x;
		lastMouseY = mouseEvent.y;
		hasLastMousePosition = true;
	}

	if (source && (mouseUp || insideSource))
		obs_source_send_mouse_click(source, &mouseEvent, button, mouseUp, clickCount);

	if (switch_scene_enabled && source && obs_source_is_scene(source)) {
		if (mouseUp) {
			if (obs_frontend_preview_program_mode_active()) {
				obs_frontend_set_current_preview_scene(source);
			} else {
				obs_frontend_set_current_scene(source);
			}
		} else if (clickCount == 2 && obs_frontend_preview_program_mode_active()) {
			auto *userConfig = obs_frontend_get_user_config();
			if (!userConfig || config_get_bool(userConfig, "BasicWindow", "TransitionOnDoubleClick"))
				obs_frontend_set_current_scene(source);
		}
	} else {
		if (obs_scene_t *scene = obs_scene_from_source(source)) {
			if (mouseUp || insideSource) {
				click_event ce{mouseEvent.x, mouseEvent.y, mouseEvent.modifiers, button, mouseUp, clickCount};
				obs_scene_enum_items(scene, HandleSceneMouseClickEvent, &ce);
			}
		}
	}

	return true;
}

bool SwitcherDock::HandleContextMenuEvent(QContextMenuEvent *event)
{
	if (!workspaceContextMenuEnabled || !event)
		return false;

	emit ContextMenuRequested(event->globalPos());
	return true;
}

struct move_event {
	int32_t x;
	int32_t y;
	uint32_t modifiers;
	bool mouseLeave;
};

static bool HandleSceneMouseMoveEvent(obs_scene_t *scene, obs_sceneitem_t *item, void *data)
{
	UNUSED_PARAMETER(scene);
	auto move_event = static_cast<struct move_event *>(data);

	matrix4 transform{};
	matrix4 invTransform{};
	vec3 transformedPos{};
	vec3 pos3{};
	vec3 pos3_{};

	vec3_set(&pos3, move_event->x, move_event->y, 0.0f);

	obs_sceneitem_get_box_transform(item, &transform);

	matrix4_inv(&invTransform, &transform);
	vec3_transform(&transformedPos, &pos3, &invTransform);
	vec3_transform(&pos3_, &transformedPos, &transform);

	if (CloseFloat(pos3.x, pos3_.x) && CloseFloat(pos3.y, pos3_.y) && transformedPos.x >= 0.0f && transformedPos.x <= 1.0f &&
	    transformedPos.y >= 0.0f && transformedPos.y <= 1.0f) {
		auto source = obs_sceneitem_get_source(item);
		obs_mouse_event mouseEvent{};
		mouseEvent.x = transformedPos.x * obs_source_get_base_width(source);
		mouseEvent.y = transformedPos.y * obs_source_get_base_height(source);
		mouseEvent.modifiers = move_event->modifiers;
		obs_source_send_mouse_move(source, &mouseEvent, move_event->mouseLeave);
	}
	return true;
}

bool SwitcherDock::HandleMouseMoveEvent(QMouseEvent *event)
{
	if (!event)
		return false;
	if (workspaceContextMenuEnabled && event->modifiers().testFlag(Qt::ControlModifier))
		return true;
	if (!source)
		return true;
	if (event->buttons() == Qt::LeftButton && event->modifiers().testFlag(Qt::ControlModifier)) {

		QSize size = preview->size() * preview->devicePixelRatioF();
		scrollX -= float(event->pos().x() - scrollingFromX) / size.width();
		scrollY -= float(event->pos().y() - scrollingFromY) / size.height();
		if (scrollX < 0.0f)
			scrollX = 0.0;
		if (scrollX > 1.0f)
			scrollX = 1.0f;
		if (scrollY < 0.0f)
			scrollY = 0.0;
		if (scrollY > 1.0f)
			scrollY = 1.0f;
		scrollingFromX = event->pos().x();
		scrollingFromY = event->pos().y();
		return true;
	}

	struct obs_mouse_event mouseEvent = {};
	mouseEvent.modifiers = TranslateQtMouseEventModifiers(event);

	const bool insideSource = GetSourceRelativeXY(event->pos().x(), event->pos().y(), mouseEvent.x, mouseEvent.y);
	const bool hadLastMousePosition = hasLastMousePosition;
	const bool mouseLeave = !insideSource;

	if (insideSource) {
		lastMouseX = mouseEvent.x;
		lastMouseY = mouseEvent.y;
		hasLastMousePosition = true;
	} else if (hadLastMousePosition) {
		mouseEvent.x = lastMouseX;
		mouseEvent.y = lastMouseY;
	}

	obs_source_send_mouse_move(source, &mouseEvent, mouseLeave);
	if (!switch_scene_enabled) {
		if (obs_scene_t *scene = obs_scene_from_source(source)) {
			move_event ce{mouseEvent.x, mouseEvent.y, mouseEvent.modifiers, mouseLeave};
			if (insideSource || hadLastMousePosition)
				obs_scene_enum_items(scene, HandleSceneMouseMoveEvent, &ce);
		}
	}

	if (mouseLeave)
		hasLastMousePosition = false;

	return true;
}

bool SwitcherDock::HandleMouseBoundaryEvent(QEvent *event)
{
	if (!event)
		return false;
	if (!source)
		return true;

	if (event->type() == QEvent::Enter) {
		auto *enterEvent = static_cast<QEnterEvent *>(event);
		obs_mouse_event mouseEvent = {};
		mouseEvent.modifiers =
			TranslateQtMouseEventModifiers(QGuiApplication::mouseButtons(), QGuiApplication::keyboardModifiers());

		const QPointF position = enterEvent->position();
		if (!GetSourceRelativeXY(static_cast<int>(position.x()), static_cast<int>(position.y()), mouseEvent.x,
					 mouseEvent.y))
			return true;

		lastMouseX = mouseEvent.x;
		lastMouseY = mouseEvent.y;
		hasLastMousePosition = true;
		obs_source_send_mouse_move(source, &mouseEvent, false);

		if (!switch_scene_enabled) {
			if (obs_scene_t *scene = obs_scene_from_source(source)) {
				move_event ce{mouseEvent.x, mouseEvent.y, mouseEvent.modifiers, false};
				obs_scene_enum_items(scene, HandleSceneMouseMoveEvent, &ce);
			}
		}
		return true;
	}

	if (event->type() != QEvent::Leave)
		return false;

	obs_mouse_event mouseEvent = {};
	mouseEvent.modifiers =
		TranslateQtMouseEventModifiers(QGuiApplication::mouseButtons(), QGuiApplication::keyboardModifiers());
	if (hasLastMousePosition) {
		mouseEvent.x = lastMouseX;
		mouseEvent.y = lastMouseY;
	}

	obs_source_send_mouse_move(source, &mouseEvent, true);
	if (!switch_scene_enabled && hasLastMousePosition) {
		if (obs_scene_t *scene = obs_scene_from_source(source)) {
			move_event ce{lastMouseX, lastMouseY, mouseEvent.modifiers, true};
			obs_scene_enum_items(scene, HandleSceneMouseMoveEvent, &ce);
		}
	}

	hasLastMousePosition = false;
	return true;
}

struct wheel_event {
	int32_t x;
	int32_t y;
	uint32_t modifiers;
	int xDelta;
	int yDelta;
};

static bool HandleSceneMouseWheelEvent(obs_scene_t *scene, obs_sceneitem_t *item, void *data)
{
	UNUSED_PARAMETER(scene);
	auto wheel_event = static_cast<struct wheel_event *>(data);

	matrix4 transform{};
	matrix4 invTransform{};
	vec3 transformedPos{};
	vec3 pos3{};
	vec3 pos3_{};

	vec3_set(&pos3, wheel_event->x, wheel_event->y, 0.0f);

	obs_sceneitem_get_box_transform(item, &transform);

	matrix4_inv(&invTransform, &transform);
	vec3_transform(&transformedPos, &pos3, &invTransform);
	vec3_transform(&pos3_, &transformedPos, &transform);

	if (CloseFloat(pos3.x, pos3_.x) && CloseFloat(pos3.y, pos3_.y) && transformedPos.x >= 0.0f && transformedPos.x <= 1.0f &&
	    transformedPos.y >= 0.0f && transformedPos.y <= 1.0f) {
		auto source = obs_sceneitem_get_source(item);
		obs_mouse_event mouseEvent{};
		mouseEvent.x = transformedPos.x * obs_source_get_base_width(source);
		mouseEvent.y = transformedPos.y * obs_source_get_base_height(source);
		mouseEvent.modifiers = wheel_event->modifiers;
		obs_source_send_mouse_wheel(source, &mouseEvent, wheel_event->xDelta, wheel_event->yDelta);
	}
	return true;
}

bool SwitcherDock::HandleMouseWheelEvent(QWheelEvent *event)
{
	if (!source)
		return true;
	struct obs_mouse_event mouseEvent = {};

	mouseEvent.modifiers = TranslateQtKeyboardEventModifiers(event, true);

	int xDelta = 0;
	int yDelta = 0;

	const QPoint angleDelta = event->angleDelta();
	if (!event->pixelDelta().isNull()) {
		if (angleDelta.x())
			xDelta = event->pixelDelta().x();
		else
			yDelta = event->pixelDelta().y();
	} else {
		if (angleDelta.x())
			xDelta = angleDelta.x();
		else
			yDelta = angleDelta.y();
	}

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
	const QPointF position = event->position();
	const int x = position.x();
	const int y = position.y();
#else
	const int x = event->pos().x();
	const int y = event->pos().y();
#endif

	const bool insideSource = GetSourceRelativeXY(x, y, mouseEvent.x, mouseEvent.y);
	if ((QGuiApplication::keyboardModifiers() & Qt::ControlModifier) && yDelta != 0) {
		const auto factor = 1.0f + (0.0008f * yDelta);

		zoom *= factor;
		if (zoom < 1.0f)
			zoom = 1.0f;
		if (zoom > 100.0f)
			zoom = 100.0f;

	} else if (insideSource) {
		lastMouseX = mouseEvent.x;
		lastMouseY = mouseEvent.y;
		hasLastMousePosition = true;
		obs_source_send_mouse_wheel(source, &mouseEvent, xDelta, yDelta);
		if (switch_scene_enabled) {

		} else if (obs_scene_t *scene = obs_scene_from_source(source)) {
			wheel_event ce{mouseEvent.x, mouseEvent.y, mouseEvent.modifiers, xDelta, yDelta};
			obs_scene_enum_items(scene, HandleSceneMouseWheelEvent, &ce);
		}
	}

	return true;
}

bool SwitcherDock::HandleFocusEvent(QFocusEvent *event)
{
	bool focus = event->type() == QEvent::FocusIn;

	if (source)
		obs_source_send_focus(source, focus);

	return true;
}
struct key_event {
	struct obs_key_event keyEvent;
	bool keyUp;
};

bool HandleSceneKeyEvent(obs_scene_t *scene, obs_sceneitem_t *item, void *data)
{
	UNUSED_PARAMETER(scene);
	auto key_event = static_cast<struct key_event *>(data);
	const auto source = obs_sceneitem_get_source(item);
	obs_source_send_key_click(source, &key_event->keyEvent, key_event->keyUp);
	return true;
}

bool SwitcherDock::HandleKeyEvent(QKeyEvent *event)
{
	if (!source)
		return true;
	struct obs_key_event keyEvent;

	QByteArray text = event->text().toUtf8();
	keyEvent.modifiers = TranslateQtKeyboardEventModifiers(event, false);
	keyEvent.text = text.data();
	keyEvent.native_modifiers = event->nativeModifiers();
	keyEvent.native_scancode = event->nativeScanCode();
	keyEvent.native_vkey = event->nativeVirtualKey();

	bool keyUp = event->type() == QEvent::KeyRelease;

	obs_source_send_key_click(source, &keyEvent, keyUp);
	if (!switch_scene_enabled) {
		if (obs_scene_t *scene = obs_scene_from_source(source)) {
			key_event ce{keyEvent, keyUp};
			obs_scene_enum_items(scene, HandleSceneKeyEvent, &ce);
		}
	}

	return true;
}

bool SwitcherDock::ShouldActivatePreview() const
{
	return previewConfigured && source;
}

void SwitcherDock::ActivatePreview()
{
	if (previewActive || !source)
		return;

	if (!preview) {
		preview = new OBSQTDisplay(this);
		preview->setObjectName(QStringLiteral("preview"));
		preview->setMinimumSize(QSize(24, 24));
		QSizePolicy sizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
		sizePolicy.setHorizontalStretch(0);
		sizePolicy.setVerticalStretch(0);
		sizePolicy.setHeightForWidth(preview->sizePolicy().hasHeightForWidth());
		preview->setSizePolicy(sizePolicy);
		preview->setMouseTracking(true);
		preview->setFocusPolicy(Qt::StrongFocus);
		preview->installEventFilter(eventFilter.get());

		auto addDrawCallback = [this]() {
			if (previewActive && preview && preview->GetDisplay())
				obs_display_add_draw_callback(preview->GetDisplay(), DrawPreview, this);
		};
		connect(preview, &OBSQTDisplay::DisplayCreated, addDrawCallback);
	}

	if (activeLabel && activeLabel->isVisibleTo(this)) {
		activeLabel->setVisible(false);
		if (!activeFrame) {
			activeFrame = new QFrame;
			auto *layout = new QHBoxLayout();
			layout->setContentsMargins(0, 0, 0, 0);
			activeFrame->setLayout(layout);
		}

		if (indexOf(activeFrame) < 0) {
			const int previewIndex = indexOf(preview);
			if (previewIndex >= 0)
				replaceWidget(previewIndex, activeFrame);
			else
				addWidget(activeFrame);
		}

		activeFrame->layout()->addWidget(preview);
		activeFrame->setVisible(true);
		ActiveChanged();
	} else if (activeFrame) {
		const int frameIndex = indexOf(activeFrame);
		if (frameIndex >= 0)
			replaceWidget(frameIndex, preview);
	}

	if (indexOf(preview) < 0 && (!activeFrame || !activeFrame->isVisibleTo(this)))
		addWidget(preview);

	previewActive = true;
	obs_source_inc_showing(source);
	preview->setVisible(true);
	preview->show();
	preview->CreateDisplay();
	QTimer::singleShot(0, this, [this]() {
		if (previewActive && preview)
			preview->CreateDisplay(true);
	});
}

void SwitcherDock::DeactivatePreview()
{
	if (!preview)
		return;

	const bool wasActive = previewActive;
	previewActive = false;

	if (auto *display = preview->GetDisplay())
		obs_display_remove_draw_callback(display, DrawPreview, this);

	preview->setVisible(false);
	preview->DestroyDisplay();
	if (activeFrame && activeFrame->isVisibleTo(this)) {
		activeFrame->setVisible(false);
		EnableShowActive();
	}

	if (wasActive && source)
		obs_source_dec_showing(source);
}

void SwitcherDock::UpdatePreviewLifecycle()
{
	if (ShouldActivatePreview())
		ActivatePreview();
	else
		DeactivatePreview();
}

void SwitcherDock::DockWidgetVisibilityChanged(bool visible)
{
	wrapperDockVisible = visible;
}

void SwitcherDock::EnablePreview()
{
	previewConfigured = true;
	UpdatePreviewLifecycle();
}

void SwitcherDock::DisablePreview()
{
	previewConfigured = false;
	UpdatePreviewLifecycle();
}

bool SwitcherDock::PreviewEnabled()
{
	return previewConfigured;
}

void SwitcherDock::EnableVolMeter()
{
	if (obs_volmeter != nullptr)
		return;

	obs_volmeter = obs_volmeter_create(OBS_FADER_LOG);
	if (source)
		obs_volmeter_attach_source(obs_volmeter, source);

	volMeter = new VolumeMeter(nullptr, obs_volmeter);
	volMeter->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);

	obs_volmeter_add_callback(obs_volmeter, OBSVolumeLevel, this);

	if (volMeterWidget) {
		volMeterWidget->layout()->addWidget(volMeter);
		volMeterWidget->setVisible(true);
		return;
	}
	const auto volMeterLayout = new QVBoxLayout;
	volMeterWidget = new QWidget;
	volMeterWidget->setLayout(volMeterLayout);
	volMeterWidget->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
	volMeterLayout->addWidget(volMeter);
	addWidget(volMeterWidget);
}

void SwitcherDock::DisableVolMeter()
{
	if (!obs_volmeter)
		return;

	volMeterWidget->setVisible(false);

	obs_volmeter_remove_callback(obs_volmeter, OBSVolumeLevel, this);

	auto layout = volMeterWidget->layout();
	while (auto i = layout->itemAt(0)) {
		layout->removeItem(i);
	}
	volMeter->deleteLater();
	volMeter = nullptr;

	obs_volmeter_destroy(obs_volmeter);
	obs_volmeter = nullptr;
}

bool SwitcherDock::VolMeterEnabled()
{
	return obs_volmeter != nullptr;
}

void SwitcherDock::UpdateVolControls()
{
	if (!volControl)
		return;
	bool lock = false;
	if (obs_data_t *settings = source ? obs_source_get_private_settings(source) : nullptr) {
		lock = obs_data_get_bool(settings, "volume_locked");
		obs_data_release(settings);
	}
	locked->setChecked(lock);
	mute->setEnabled(!lock);
	mute->setChecked(source ? obs_source_muted(source) : false);
	slider->setEnabled(!lock);
	float mul = source ? obs_source_get_volume(source) : 0.0f;
	float db = obs_mul_to_db(mul);
	float def;
	if (db >= 0.0f)
		def = 1.0f;
	else if (db <= -96.0f)
		def = 0.0f;
	else
		def = (-log10f(-db + LOG_OFFSET_DB) - LOG_RANGE_VAL) / (LOG_OFFSET_VAL - LOG_RANGE_VAL);
	slider->setValue(def * 10000.0f);
}

void SwitcherDock::EnableVolControls()
{
	if (volControl != nullptr) {
		volControl->setVisible(true);
		UpdateVolumeSignals();
		return;
	}

	volControl = new QWidget;
	volControl->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
	auto *audioLayout = new QHBoxLayout(this);

	locked = new LockedCheckBox();
	locked->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
	locked->setFixedSize(16, 16);

	locked->setStyleSheet("background: none");

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
	connect(locked, &QCheckBox::checkStateChanged, this, &SwitcherDock::LockVolumeControl, Qt::DirectConnection);
#else
	connect(locked, &QCheckBox::stateChanged, this, &SwitcherDock::LockVolumeControl, Qt::DirectConnection);
#endif

	slider = new SliderIgnoreScroll(Qt::Horizontal);
	slider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	slider->setMinimum(0);
	slider->setMaximum(10000);
	slider->setToolTip(QT_UTF8(obs_module_text("VolumeOutput")));

	connect(slider, SIGNAL(valueChanged(int)), this, SLOT(SliderChanged(int)));

	mute = new MuteCheckBox();

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
	connect(mute, &QCheckBox::checkStateChanged, this, &SwitcherDock::MuteVolumeControl, Qt::DirectConnection);
#else
	connect(mute, &QCheckBox::stateChanged, this, &SwitcherDock::MuteVolumeControl, Qt::DirectConnection);
#endif

	audioLayout->addWidget(locked);
	audioLayout->addWidget(slider);
	audioLayout->addWidget(mute);

	volControl->setLayout(audioLayout);
	addWidget(volControl);

	UpdateVolControls();
	UpdateVolumeSignals();
}

void SwitcherDock::DisableVolControls()
{
	if (!volControl)
		return;
	muteSignal.Disconnect();
	volumeSignal.Disconnect();
	volControl->setVisible(false);
}
bool SwitcherDock::VolControlsEnabled()
{
	return volControl != nullptr && volControl->isVisibleTo(this);
}

void SwitcherDock::EnableMediaControls()
{
	if (mediaControl != nullptr) {
		mediaControl->SetSource(OBSGetWeakRef(source));
		mediaControl->setVisible(true);
		return;
	}
	mediaControl = new MediaControl(OBSGetWeakRef(source), showTimeDecimals, showTimeRemaining);
	mediaControl->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
	addWidget(mediaControl);
}

void SwitcherDock::DisableMediaControls()
{
	if (!mediaControl)
		return;
	showTimeDecimals = mediaControl->GetShowMs();
	showTimeRemaining = mediaControl->GetShowTimeRemaining();
	mediaControl->SetSource(nullptr);
	mediaControl->setVisible(false);
}

bool SwitcherDock::MediaControlsEnabled()
{
	return mediaControl != nullptr && mediaControl->isVisibleTo(this);
}

void SwitcherDock::EnableSwitchScene()
{
	if (!source || !obs_source_is_scene(source))
		return;
	switch_scene_enabled = true;
}

void SwitcherDock::DisableSwitchScene()
{
	switch_scene_enabled = false;
}

bool SwitcherDock::SwitchSceneEnabled()
{
	return switch_scene_enabled;
}

void SwitcherDock::EnableShowActive()
{
	if (preview && preview->isVisibleTo(this)) {
		if (activeFrame) {
			replaceWidget(indexOf(preview), activeFrame);
			activeFrame->setVisible(true);
			activeFrame->layout()->addWidget(preview);
			ActiveChanged();
			UpdateActiveSignals();
			return;
		}
		activeFrame = new QFrame;
		auto l = new QHBoxLayout();
		l->setContentsMargins(0, 0, 0, 0);
		activeFrame->setLayout(l);
		replaceWidget(indexOf(preview), activeFrame);
		l->addWidget(preview);
		ActiveChanged();
		UpdateActiveSignals();
		return;
	}
	if (activeLabel) {
		activeLabel->setVisible(true);
		UpdateActiveSignals();
		return;
	}

	activeLabel = new QLabel;
	activeLabel->setAlignment(Qt::AlignCenter);
	activeLabel->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
	ActiveChanged();
	addWidget(activeLabel);
	UpdateActiveSignals();
}

void SwitcherDock::DisableShowActive()
{
	activateSignal.Disconnect();
	deactivateSignal.Disconnect();
	if (activeLabel) {
		activeLabel->setVisible(false);
	}
	if (activeFrame) {
		if (preview && preview->isVisibleTo(this) && activeFrame->isVisibleTo(this)) {
			replaceWidget(indexOf(activeFrame), preview);
		}
		activeFrame->setVisible(false);
	}
}
bool SwitcherDock::ShowActiveEnabled()
{
	return (activeLabel != nullptr && activeLabel->isVisibleTo(this)) ||
	       (activeFrame != nullptr && activeFrame->isVisibleTo(this));
}

void SwitcherDock::EnableSceneItems()
{
	if (!source)
		return;
	obs_scene_t *scene = obs_scene_from_source(source);
	if (!scene)
		scene = obs_group_from_source(source);
	if (!scene)
		return;
	if (!sceneItems) {
		sceneItemsScrollArea = new QScrollArea;
		sceneItemsScrollArea->setObjectName(QString::fromUtf8("vScrollArea"));
		sceneItemsScrollArea->setFrameShape(QFrame::StyledPanel);
		sceneItemsScrollArea->setFrameShadow(QFrame::Sunken);
		sceneItemsScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
		sceneItemsScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		sceneItemsScrollArea->setWidgetResizable(true);
		sceneItemsScrollArea->setContentsMargins(0, 0, 0, 0);

		auto layout = new QGridLayout;
		sceneItems = new QWidget;
		sceneItems->setContentsMargins(0, 0, 0, 0);
		sceneItems->setObjectName(QStringLiteral("contextContainer"));
		sceneItems->setLayout(layout);
		layout->setColumnStretch(0, 1);
		sceneItems->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Maximum);

		sceneItemsScrollArea->setWidget(sceneItems);
		addWidget(sceneItemsScrollArea);

	} else {
		sceneItems->setVisible(true);
		sceneItemsScrollArea->setVisible(true);
	}
	int count = 0;
	obs_scene_enum_items(scene, GetSceneItemCount, &count);
	sceneItems->layout()->setProperty("sceneItemCount", count);
	sceneItems->layout()->setProperty("row", 0);
	sceneItems->layout()->setProperty("indent", 0);
	obs_scene_enum_items(scene, AddSceneItem, sceneItems->layout());

	auto itemVisible = [](void *data, calldata_t *cd) {
		const auto dock = static_cast<SwitcherDock *>(data);
		const auto curItem = static_cast<obs_sceneitem_t *>(calldata_ptr(cd, "item"));

		const int id = (int)obs_sceneitem_get_id(curItem);
		QMetaObject::invokeMethod(dock, "VisibilityChanged", Qt::QueuedConnection, Q_ARG(int, id));
	};

	auto refreshItems = [](void *data, calldata_t *cd) {
		UNUSED_PARAMETER(cd);
		const auto dock = static_cast<SwitcherDock *>(data);
		QMetaObject::invokeMethod(dock, "RefreshItems", Qt::QueuedConnection);
	};

	signal_handler_t *signal = obs_source_get_signal_handler(source);

	addSignal.Connect(signal, "item_add", refreshItems, this);
	removeSignal.Connect(signal, "item_remove", refreshItems, this);
	reorderSignal.Connect(signal, "reorder", refreshItems, this);
	refreshSignal.Connect(signal, "refresh", refreshItems, this);
	visibleSignal.Connect(signal, "item_visible", itemVisible, this);
}

void SwitcherDock::VisibilityChanged(int id)
{
	auto layout = dynamic_cast<QGridLayout *>(sceneItems->layout());
	auto count = layout->rowCount();

	for (int i = 0; i < count; i++) {
		QLayoutItem *item = layout->itemAtPosition(i, 3);
		if (!item)
			continue;
		QWidget *w = item->widget();
		if (!w)
			continue;
		if (id != w->property("id").toInt())
			continue;
		auto scene = obs_scene_from_source(source);
		if (!scene)
			scene = obs_group_from_source(source);
		obs_sceneitem_t *sceneitem = obs_scene_find_sceneitem_by_id(scene, id);

		auto checkBox = dynamic_cast<QCheckBox *>(w);
		checkBox->setChecked(obs_sceneitem_visible(sceneitem));
	}
}

void SwitcherDock::RefreshItems()
{
	DisableSceneItems();
	EnableSceneItems();
}

bool SwitcherDock::GetSceneItemCount(obs_scene_t *scene, obs_sceneitem_t *item, void *data)
{
	UNUSED_PARAMETER(scene);
	int *count = (int *)data;
	if (obs_sceneitem_is_group(item))
		obs_scene_enum_items(obs_group_from_source(obs_sceneitem_get_source(item)), GetSceneItemCount, count);
	(*count)++;
	return true;
}

bool SwitcherDock::AddSceneItem(obs_scene_t *scene, obs_sceneitem_t *item, void *data)
{
	UNUSED_PARAMETER(scene);
	QGridLayout *layout = static_cast<QGridLayout *>(data);
	int indent = layout->property("indent").toInt();
	if (obs_sceneitem_is_group(item)) {
		indent++;
		layout->setProperty("indent", indent);
		obs_scene_enum_items(obs_group_from_source(obs_sceneitem_get_source(item)), AddSceneItem, data);
		indent--;
		layout->setProperty("indent", indent);
	}

	auto source = obs_sceneitem_get_source(item);
	int sceneItemCount = layout->property("sceneItemCount").toInt();
	int row = layout->property("row").toInt();
	row++;
	layout->setProperty("row", row);
	row = sceneItemCount - row;
	auto name = QString(indent * 4, ' ') + QT_UTF8(obs_source_get_name(source));
	auto label = new QLabel(name);
	layout->addWidget(label, row, 0, Qt::AlignLeft | Qt::AlignVCenter);

	if (obs_is_source_configurable(obs_source_get_id(source))) {
		auto prop = new QPushButton();
		prop->setObjectName(QStringLiteral("sourcePropertiesButton"));
		prop->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
		prop->setFixedSize(16, 16);
		prop->setFlat(true);
		layout->addWidget(prop, row, 1, Qt::AlignCenter);

		auto openProps = [source]() {
			obs_frontend_open_source_properties(source);
		};
		connect(prop, &QAbstractButton::clicked, openProps);
	}

	auto filters = new QPushButton();
	filters->setObjectName(QStringLiteral("sourceFiltersButton"));
	filters->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
	filters->setFixedSize(16, 16);
	filters->setFlat(true);
	layout->addWidget(filters, row, 2, Qt::AlignCenter);

	auto openFilters = [source]() {
		obs_frontend_open_source_filters(source);
	};

	connect(filters, &QAbstractButton::clicked, openFilters);

	auto vis = new VisibilityCheckBox();
	vis->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum);
	vis->setFixedSize(16, 16);
	vis->setChecked(obs_sceneitem_visible(item));
	vis->setStyleSheet("background: none");
	vis->setProperty("id", (int)obs_sceneitem_get_id(item));
	layout->addWidget(vis, row, 3, Qt::AlignCenter);

	auto setItemVisible = [item](bool val) {
		obs_sceneitem_set_visible(item, val);
	};

	connect(vis, &QAbstractButton::clicked, setItemVisible);
	return true;
}

void SwitcherDock::DisableSceneItems()
{
	if (!sceneItems)
		return;

	sceneItemsScrollArea->setVisible(false);
	sceneItems->setVisible(false);
	const auto layout = sceneItems->layout();
	for (auto i = layout->count() - 1; i >= 0; i--) {
		auto item = layout->takeAt(i);
		layout->removeWidget(item->widget());
		delete item->widget();
	}
	visibleSignal.Disconnect();
	addSignal.Disconnect();
	removeSignal.Disconnect();
	reorderSignal.Disconnect();
	refreshSignal.Disconnect();
}
bool SwitcherDock::SceneItemsEnabled()
{
	return sceneItems != nullptr && sceneItems->isVisibleTo(this);
}

void SwitcherDock::EnableProperties()
{
	if (propertiesButton) {
		propertiesButton->setVisible(true);
		return;
	}

	propertiesButton = new QPushButton;
	propertiesButton->setObjectName(QStringLiteral("sourcePropertiesButton"));
	propertiesButton->setText(QT_UTF8(obs_module_text("Properties")));
	addWidget(propertiesButton);
	auto openProps = [this]() {
		if (!source)
			return;
		obs_frontend_open_source_properties(source);
	};
	connect(propertiesButton, &QAbstractButton::clicked, openProps);
}

void SwitcherDock::DisableProperties()
{
	if (!propertiesButton)
		return;
	propertiesButton->setVisible(false);
}

bool SwitcherDock::PropertiesEnabled()
{
	return propertiesButton != nullptr && propertiesButton->isVisible();
}

void SwitcherDock::EnableFilters()
{
	if (filtersButton) {
		filtersButton->setVisible(true);
		return;
	}
	filtersButton = new QPushButton;
	filtersButton->setObjectName(QStringLiteral("sourceFiltersButton"));
	filtersButton->setText(QT_UTF8(obs_module_text("Filters")));
	addWidget(filtersButton);
	auto openProps = [this]() {
		if (!source)
			return;
		obs_frontend_open_source_filters(source);
	};
	connect(filtersButton, &QAbstractButton::clicked, openProps);
}

void SwitcherDock::DisableFilters()
{
	if (!filtersButton)
		return;
	filtersButton->setVisible(false);
}

bool SwitcherDock::FiltersEnabled()
{
	return filtersButton != nullptr && filtersButton->isVisibleTo(this);
}

static inline QColor color_from_int(long long val)
{
	return QColor(val & 0xff, (val >> 8) & 0xff, (val >> 16) & 0xff, (val >> 24) & 0xff);
}

static inline long long color_to_int(QColor color)
{
	auto shift = [&](unsigned val, int shift) {
		return ((val & 0xff) << shift);
	};

	return shift(color.red(), 0) | shift(color.green(), 8) | shift(color.blue(), 16) | shift(color.alpha(), 24);
}

void SwitcherDock::EnableTextInput()
{
	if (textInput) {
		textInput->setVisible(true);
		textInputTimer->start(1000);
		return;
	}

	textInput = new QPlainTextEdit;
	textInput->setObjectName(QStringLiteral("textInput"));
	ApplyCustomTextInputStyle();
	textInput->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(textInput, &QPlainTextEdit::customContextMenuRequested, [this]() {
		auto menu = textInput->createStandardContextMenu();
		menu->addSeparator();
		auto m = menu->addMenu(obs_module_text("CustomStyle"));
		m->addAction(QString::fromUtf8(obs_module_text("Font")), [this]() {
			QFontDialog::FontDialogOptions options;
			bool success = false;
#ifndef _WIN32
			options = QFontDialog::DontUseNativeDialog;
#endif
			QFont font;
			if (textInputCustomStyle) {
				auto fam = obs_data_get_string(textInputCustomStyle, "font-family");
				if (fam && strlen(fam))
					font.setFamily(fam);
				auto st = obs_data_get_int(textInputCustomStyle, "font-style");
				if (st)
					font.setStyle((QFont::Style)st);
				auto w = obs_data_get_int(textInputCustomStyle, "font-weight");
				if (w)
					font.setWeight((QFont::Weight)w);

				auto s = obs_data_get_int(textInputCustomStyle, "font-size");
				if (s)
					font.setPointSize(s);
			}
			font = QFontDialog::getFont(
				&success, font, this,
				QString::fromUtf8(obs_frontend_get_locale_string("Basic.PropertiesWindow.SelectFont.WindowTitle")),
				options);
			if (!success)
				return;
			if (!textInputCustomStyle)
				textInputCustomStyle = obs_data_create();
			obs_data_set_string(textInputCustomStyle, "font-family", font.family().toUtf8().constData());
			obs_data_set_int(textInputCustomStyle, "font-style", font.style());
			obs_data_set_int(textInputCustomStyle, "font-weight", font.weight());
			obs_data_set_int(textInputCustomStyle, "font-size", font.pointSize());
			ApplyCustomTextInputStyle();
		});
		m->addAction(QString::fromUtf8(obs_module_text("BackgroundColor")), [this]() {
			QColor color;
			if (textInputCustomStyle) {
				auto color_int = obs_data_get_int(textInputCustomStyle, "background-color");
				if (color_int)
					color = color_from_int(color_int);
			}
			QColorDialog::ColorDialogOptions options;
#ifdef __linux__
			// TODO: Revisit hang on Ubuntu with native dialog
			options |= QColorDialog::DontUseNativeDialog;
#endif
			color = QColorDialog::getColor(color, this, QString::fromUtf8(obs_module_text("BackgroundColor")), options);

			if (!color.isValid())
				return;
			color.setAlpha(255);
			if (!textInputCustomStyle)
				textInputCustomStyle = obs_data_create();
			obs_data_set_int(textInputCustomStyle, "background-color", color_to_int(color));
			ApplyCustomTextInputStyle();
		});
		m->addAction(QString::fromUtf8(obs_module_text("TextColor")), [this]() {
			QColor color;
			if (textInputCustomStyle) {
				auto color_int = obs_data_get_int(textInputCustomStyle, "text-color");
				if (color_int)
					color = color_from_int(color_int);
			}
			QColorDialog::ColorDialogOptions options;
#ifdef __linux__
			// TODO: Revisit hang on Ubuntu with native dialog
			options |= QColorDialog::DontUseNativeDialog;
#endif
			color = QColorDialog::getColor(color, this, QString::fromUtf8(obs_module_text("TextColor")), options);

			if (!color.isValid())
				return;
			color.setAlpha(255);
			if (!textInputCustomStyle)
				textInputCustomStyle = obs_data_create();
			obs_data_set_int(textInputCustomStyle, "text-color", color_to_int(color));
			ApplyCustomTextInputStyle();
		});
		m->addSeparator();
		m->addAction(QString::fromUtf8(obs_module_text("Clear")), [this]() {
			if (!textInputCustomStyle)
				return;
			obs_data_release(textInputCustomStyle);
			textInputCustomStyle = nullptr;
			ApplyCustomTextInputStyle();
		});
		menu->exec(QCursor::pos());
		delete menu;
	});

	if (auto *settings = source ? obs_source_get_settings(source) : nullptr) {
		textInput->setPlainText(QT_UTF8(obs_data_get_string(settings, "text")));
		obs_data_release(settings);
	}

	addWidget(textInput);
	auto changeText = [this]() {
		if (auto *settings = source ? obs_source_get_settings(source) : nullptr) {
			if (textInput->toPlainText() != QT_UTF8(obs_data_get_string(settings, "text"))) {
				obs_data_set_string(settings, "text", QT_TO_UTF8(textInput->toPlainText()));
				obs_source_update(source, nullptr);
			}
			obs_data_release(settings);
		}
	};
	connect(textInput, &QPlainTextEdit::textChanged, changeText);

	textInputTimer = new QTimer(this);
	connect(textInputTimer, &QTimer::timeout, this, [=]() {
		if (auto *settings = source ? obs_source_get_settings(source) : nullptr) {
			const auto text = QT_UTF8(obs_data_get_string(settings, "text"));
			if (textInput->toPlainText() != text) {
				textInput->setPlainText(text);
			}
			obs_data_release(settings);
		}
	});
	textInputTimer->start(1000);
}

void SwitcherDock::DisableTextInput()
{
	if (!textInput)
		return;
	textInputTimer->stop();

	textInput->setVisible(false);
	textInput = nullptr;
}

bool SwitcherDock::TextInputEnabled()
{
	return textInput != nullptr && textInput->isVisibleTo(this);
}

obs_data_t *SwitcherDock::GetCustomTextInputStyle()
{
	return textInputCustomStyle;
}

void SwitcherDock::SetCustomTextInputStyle(obs_data_t *ns)
{
	obs_data_release(textInputCustomStyle);
	obs_data_addref(ns);
	textInputCustomStyle = ns;
	ApplyCustomTextInputStyle();
}

void SwitcherDock::ApplyCustomTextInputStyle()
{
	if (!textInput)
		return;
	if (!textInputCustomStyle) {
		textInput->setStyleSheet("");
		return;
	}
	QString style = QString::fromUtf8("QPlainTextEdit { \n");
	auto fam = obs_data_get_string(textInputCustomStyle, "font-family");
	if (fam && strlen(fam))
		style += QString("font-family: \"%1\";\n").arg(QString::fromUtf8(fam));
	auto st = obs_data_get_int(textInputCustomStyle, "font-style");
	if (st == QFont::StyleItalic) {
		style += "font-style: italic;\n";
	}
	auto w = obs_data_get_int(textInputCustomStyle, "font-weight");
	if (w) {
		style += QString("font-weight: %1;\n").arg(w);
	}
	auto s = obs_data_get_int(textInputCustomStyle, "font-size");
	if (s)
		style += QString("font-size: %1px;\n").arg(s);
	auto color_int = obs_data_get_int(textInputCustomStyle, "background-color");
	if (color_int) {
		auto color = color_from_int(color_int);
		style += QString("background-color: %1;\n").arg(color.name(QColor::HexRgb));
	}
	color_int = obs_data_get_int(textInputCustomStyle, "text-color");
	if (color_int) {
		auto color = color_from_int(color_int);
		style += QString("color: %1;\n").arg(color.name(QColor::HexRgb));
	}
	style += QString::fromUtf8("}");
	textInput->setStyleSheet(style);
}

void SwitcherDock::SetSource(const OBSSource source_)
{
	if (source_ == source)
		return;
	if (previewActive && source)
		obs_source_dec_showing(source);

	if (obs_volmeter)
		obs_volmeter_detach_source(obs_volmeter);

	muteSignal.Disconnect();
	volumeSignal.Disconnect();
	activateSignal.Disconnect();
	deactivateSignal.Disconnect();

	source = source_;
	hasLastMousePosition = false;

	UpdateVolControls();
	UpdateVolumeSignals();
	UpdateActiveSignals();
	ActiveChanged();

	if (textInput && textInput->isVisibleTo(this)) {
		if (auto *settings = source ? obs_source_get_settings(source) : nullptr) {
			const auto text = QT_UTF8(obs_data_get_string(settings, "text"));
			if (textInput->toPlainText() != text) {
				textInput->setPlainText(text);
			}
			obs_data_release(settings);
			if (strncmp(obs_source_get_id(source), "text_", 5) == 0)
				textInput->setFocus();
		} else if (!textInput->toPlainText().isEmpty()) {
			textInput->setPlainText("");
		}
	}
	if (mediaControl && mediaControl->isVisibleTo(this)) {
		mediaControl->SetSource(OBSGetWeakRef(source));
	}

	if (!source)
		return UpdatePreviewLifecycle();

	if (obs_volmeter)
		obs_volmeter_attach_source(obs_volmeter, source);

	if (previewActive)
		obs_source_inc_showing(source);

	UpdatePreviewLifecycle();
}

OBSSource SwitcherDock::GetSource()
{
	return source;
}

void SwitcherDock::setAction(QAction *a)
{
	action = a;
}

void SwitcherDock::SetZoom(float zoom)
{
	if (zoom < 1.0f)
		return;
	this->zoom = zoom;
}

void SwitcherDock::SetScrollX(float scroll)
{
	if (scroll < 0.0f || scroll > 1.0f)
		return;
	scrollX = scroll;
}

void SwitcherDock::SetScrollY(float scroll)
{
	if (scroll < 0.0f || scroll > 1.0f)
		return;
	scrollY = scroll;
}

QByteArray SwitcherDock::saveSplitState()
{
	return saveState();
}

bool SwitcherDock::restoreSplitState(const QByteArray &splitState)
{
	return restoreState(splitState);
}

LockedCheckBox::LockedCheckBox()
{
	setProperty("lockCheckBox", true);
	setProperty("class", "indicator-lock");
}

LockedCheckBox::LockedCheckBox(QWidget *parent) : QCheckBox(parent)
{
	setProperty("lockCheckBox", true);
	setProperty("class", "indicator-lock");
}

VisibilityCheckBox::VisibilityCheckBox()
{
	setProperty("visibilityCheckBox", true);
	setProperty("class", "indicator-visibility");
}

VisibilityCheckBox::VisibilityCheckBox(QWidget *parent) : QCheckBox(parent)
{
	setProperty("visibilityCheckBox", true);
	setProperty("class", "indicator-visibility");
}

MuteCheckBox::MuteCheckBox()
{
	setProperty("muteCheckBox", true);
	setProperty("class", "indicator-mute");
}

MuteCheckBox::MuteCheckBox(QWidget *parent) : QCheckBox(parent)
{
	setProperty("muteCheckBox", true);
	setProperty("class", "indicator-mute");
}

SliderIgnoreScroll::SliderIgnoreScroll(QWidget *parent) : QSlider(parent)
{
	setFocusPolicy(Qt::StrongFocus);
}

SliderIgnoreScroll::SliderIgnoreScroll(Qt::Orientation orientation, QWidget *parent) : QSlider(parent)
{
	setFocusPolicy(Qt::StrongFocus);
	setOrientation(orientation);
}

void SliderIgnoreScroll::wheelEvent(QWheelEvent *event)
{
	if (!hasFocus())
		event->ignore();
	else
		QSlider::wheelEvent(event);
}
