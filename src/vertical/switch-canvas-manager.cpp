#include "switch-canvas-manager.hpp"

#include <algorithm>

#include <QHBoxLayout>
#include <QLabel>
#include <QScopedValueRollback>
#include <QStandardPaths>
#include <QShowEvent>
#include <QHideEvent>
#include <QTimer>
#include <QVBoxLayout>

#include <obs-module.h>

#include "qt-display.hpp"

#ifndef QT_TO_UTF8
#define QT_TO_UTF8(str) str.toUtf8().constData()
#endif

namespace {
constexpr auto kMainCanvasId = "main";
constexpr auto kVerticalCanvasId = "vertical";
constexpr auto kDefaultVerticalCanvasName = "Switch Vertical";
constexpr auto kDefaultVerticalSceneName = "Vertical Scene";
constexpr auto kSceneTransitionKey = "transition";
constexpr auto kSceneTransitionDurationKey = "transition_duration";
constexpr auto kDefaultRecordingFilenamePattern = "Switch Vertical %CCYY-%MM-%DD %hh-%mm-%ss";

struct CanvasSceneLookup {
	QString sceneIdOrName;
	obs_source_t *source = nullptr;
};

struct SceneDescriptorsCollector {
	QVector<SwitchCanvasSceneDescriptor> *scenes = nullptr;
};

void GetScaleAndCenterPos(int baseCX, int baseCY, int windowCX, int windowCY, int &x, int &y, float &scale)
{
	int newCX = 0;
	int newCY = 0;

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

bool CollectCanvasSceneById(void *param, obs_source_t *source)
{
	auto *lookup = static_cast<CanvasSceneLookup *>(param);
	if (!lookup || !source)
		return true;
	if (obs_source_removed(source))
		return true;

	const QString uuid = QString::fromUtf8(obs_source_get_uuid(source));
	const QString name = QString::fromUtf8(obs_source_get_name(source));
	if (lookup->sceneIdOrName == uuid || lookup->sceneIdOrName == name) {
		if (lookup->source)
			obs_source_release(lookup->source);
		lookup->source = obs_source_get_ref(source);
		return false;
	}

	return true;
}

bool CollectCanvasScenes(void *param, obs_source_t *source)
{
	auto *collector = static_cast<SceneDescriptorsCollector *>(param);
	if (!collector || !collector->scenes || !source)
		return true;
	if (obs_source_removed(source))
		return true;

	collector->scenes->push_back(
		{QString::fromUtf8(obs_source_get_uuid(source)), QString::fromUtf8(obs_source_get_name(source))});
	return true;
}

QString PresetLabelForSize(const QSize &size)
{
	if (size == QSize(1080, 1920))
		return QStringLiteral("9:16 (1080x1920)");
	if (size == QSize(720, 1280))
		return QStringLiteral("9:16 (720x1280)");
	if (size == QSize(1080, 1080))
		return QStringLiteral("1:1 (1080x1080)");
	if (size == QSize(1920, 1080))
		return QStringLiteral("16:9 (1920x1080)");
	return QStringLiteral("%1x%2").arg(size.width()).arg(size.height());
}

obs_source_t *ResolveSceneOutputSource(obs_source_t *source)
{
	if (!source)
		return nullptr;
	if (obs_source_get_type(source) != OBS_SOURCE_TYPE_TRANSITION)
		return source;

	obs_source_t *active = obs_transition_get_active_source(source);
	if (active)
		return active;
	return obs_transition_get_source(source, OBS_TRANSITION_SOURCE_A);
}

bool CountSingleSceneItem(obs_scene_t *scene, obs_sceneitem_t *item, void *param)
{
	UNUSED_PARAMETER(scene);
	UNUSED_PARAMETER(item);
	auto *hasItems = static_cast<bool *>(param);
	if (hasItems)
		*hasItems = true;
	return false;
}

bool SceneSourceHasItems(obs_source_t *source)
{
	if (!source || !obs_source_is_scene(source))
		return false;

	obs_scene_t *scene = obs_scene_from_source(source);
	if (!scene)
		return false;

	bool hasItems = false;
	obs_scene_enum_items(scene, CountSingleSceneItem, &hasItems);
	return hasItems;
}

class SwitchCanvasWindow : public QWidget {
public:
	SwitchCanvasWindow(SwitchCanvasManager *manager, const QString &canvasId, const QString &title, bool projectorMode)
		: QWidget(nullptr, projectorMode ? Qt::Window | Qt::WindowStaysOnTopHint : Qt::Window),
		  preview(new SwitchCanvasPreview(manager, this))
	{
		setAttribute(Qt::WA_DeleteOnClose, true);
		setWindowTitle(title);
		setStyleSheet(QStringLiteral("background-color: black;"));
		resize(projectorMode ? QSize(720, 1280) : QSize(540, 960));

		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(projectorMode ? 0 : 12, projectorMode ? 0 : 12, projectorMode ? 0 : 12,
					   projectorMode ? 0 : 12);
		layout->setSpacing(projectorMode ? 0 : 8);

		if (!projectorMode) {
			auto *label = new QLabel(title, this);
			label->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 600; color: white;"));
			layout->addWidget(label);
		}

		preview->SetCanvasId(canvasId);
		layout->addWidget(preview, 1);
	}

private:
	SwitchCanvasPreview *preview = nullptr;
};

class PreviewRenderGuard {
public:
	explicit PreviewRenderGuard(SwitchCanvasManager *manager_) : manager(manager_)
	{
		active = manager && manager->TryBeginPreviewRender();
	}

	~PreviewRenderGuard()
	{
		if (active && manager)
			manager->EndPreviewRender();
	}

	bool IsActive() const { return active; }

private:
	SwitchCanvasManager *manager = nullptr;
	bool active = false;
};
} // namespace

SwitchCanvasPreview::SwitchCanvasPreview(SwitchCanvasManager *manager_, QWidget *parent) : QWidget(parent), manager(manager_)
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
}

SwitchCanvasPreview::~SwitchCanvasPreview()
{
	DestroyDisplayWidget();
}

void SwitchCanvasPreview::SetCanvasId(const QString &canvasId_)
{
	if (canvasId == canvasId_)
		return;

	canvasId = canvasId_;
	if (isVisible())
		Refresh();
}

void SwitchCanvasPreview::SetRenderingEnabled(bool enabled)
{
	if (renderingEnabled == enabled)
		return;

	renderingEnabled = enabled;
	if (renderingEnabled)
		Refresh();
	else
		DestroyDisplayWidget();
}

void SwitchCanvasPreview::Refresh()
{
	if (!renderingEnabled || !isVisible())
		return;

	if (!display) {
		display = new OBSQTDisplay(this);
		display->SetDisplayBackgroundColor(Qt::black);
		display->setMinimumSize(QSize(180, 320));

		auto *layout = qobject_cast<QVBoxLayout *>(this->layout());
		if (layout)
			layout->addWidget(display);

		connect(display, &OBSQTDisplay::DisplayCreated, this, [this]() { Activate(); });
	}

	display->show();
	display->CreateDisplay();
	QTimer::singleShot(0, this, [this]() {
		if (renderingEnabled && display)
			display->CreateDisplay();
	});
	Activate();
	update();
	if (display)
		display->update();
}

void SwitchCanvasPreview::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	Refresh();
}

void SwitchCanvasPreview::hideEvent(QHideEvent *event)
{
	Deactivate();
	QWidget::hideEvent(event);
}

void SwitchCanvasPreview::Activate()
{
	if (!renderingEnabled || !display || drawCallbackInstalled || !isVisible() || canvasId.isEmpty())
		return;
	if (!manager || !manager->CanvasById(canvasId))
		return;
	if (!display->GetDisplay())
		return;

	obs_display_set_enabled(display->GetDisplay(), true);
	obs_display_add_draw_callback(display->GetDisplay(), DrawCanvas, this);
	drawCallbackInstalled = true;
}

void SwitchCanvasPreview::Deactivate()
{
	if (!display)
		return;
	if (drawCallbackInstalled) {
		if (auto *obsDisplay = display->GetDisplay())
			obs_display_remove_draw_callback(obsDisplay, DrawCanvas, this);
		drawCallbackInstalled = false;
	}
	if (auto *obsDisplay = display->GetDisplay())
		obs_display_set_enabled(obsDisplay, false);
	display->hide();
}

void SwitchCanvasPreview::DestroyDisplayWidget()
{
	if (!display)
		return;

	Deactivate();
	display->DestroyDisplay();
	delete display;
	display = nullptr;
}

void SwitchCanvasPreview::DrawCanvas(void *data, uint32_t cx, uint32_t cy)
{
	auto *preview = static_cast<SwitchCanvasPreview *>(data);
	if (!preview || !preview->manager || preview->canvasId.isEmpty())
		return;
	if (!preview->manager->CanRenderCanvas())
		return;

	PreviewRenderGuard renderGuard(preview->manager);
	if (!renderGuard.IsActive())
		return;

	obs_canvas_t *canvas = preview->manager->CanvasById(preview->canvasId);
	if (!canvas)
		return;

	const QSize size = preview->manager->CanvasSize(preview->canvasId);
	uint32_t sourceCX = std::max(1, size.width());
	uint32_t sourceCY = std::max(1, size.height());

	int x = 0;
	int y = 0;
	float scale = 1.0f;
	GetScaleAndCenterPos(static_cast<int>(sourceCX), static_cast<int>(sourceCY), static_cast<int>(cx), static_cast<int>(cy), x,
			     y, scale);

	gs_viewport_push();
	gs_projection_push();
	const bool previous = gs_set_linear_srgb(true);

	gs_ortho(0.0f, float(sourceCX), 0.0f, float(sourceCY), -100.0f, 100.0f);
	gs_set_viewport(x, y, int(scale * float(sourceCX)), int(scale * float(sourceCY)));
	obs_canvas_render(canvas);

	gs_set_linear_srgb(previous);
	gs_projection_pop();
	gs_viewport_pop();
}

SwitchCanvasManager::SwitchCanvasManager(QObject *parent) : QObject(parent)
{
	EnsureDefaultState();
}

SwitchCanvasManager::~SwitchCanvasManager()
{
	RemoveRuntimeCanvases();
	ReleaseTransitions();
}

bool SwitchCanvasManager::TryBeginPreviewRender()
{
	return !previewRenderActive.test_and_set(std::memory_order_acquire);
}

void SwitchCanvasManager::EndPreviewRender()
{
	previewRenderActive.clear(std::memory_order_release);
}

QString SwitchCanvasManager::VerticalCanvasId() const
{
	return QString::fromUtf8(kVerticalCanvasId);
}

QString SwitchCanvasManager::MainCanvasId() const
{
	return QString::fromUtf8(kMainCanvasId);
}

QString SwitchCanvasManager::DefaultTransitionName() const
{
	return defaultTransitionName;
}

int SwitchCanvasManager::DefaultTransitionDuration() const
{
	return std::max(50, defaultTransitionDurationMs);
}

void SwitchCanvasManager::EnsureDefaultState()
{
	if (FindCanvasIndex(MainCanvasId()) < 0) {
		obs_video_info ovi = {};
		obs_get_video_info(&ovi);
		canvases.push_back({MainCanvasId(), QStringLiteral("Program"), QSize(int(ovi.base_width), int(ovi.base_height)),
				    PresetLabelForSize(QSize(int(ovi.base_width), int(ovi.base_height))), QString(), QString(), false,
				    true, true, true, true});
	}

	if (FindCanvasIndex(VerticalCanvasId()) < 0) {
		canvases.push_back({VerticalCanvasId(),
				    QString::fromUtf8(kDefaultVerticalCanvasName),
				    QSize(1080, 1920),
				    QStringLiteral("9:16 (1080x1920)"),
				    QString(),
				    QString(),
				    true,
				    true,
				    true,
				    true,
				    true});
	}

	if (verticalSceneNames.isEmpty())
		verticalSceneNames.push_back(QString::fromUtf8(kDefaultVerticalSceneName));

	const QString defaultMoviesPath = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
	if (verticalOutputSettings.recordingPath.trimmed().isEmpty())
		verticalOutputSettings.recordingPath = defaultMoviesPath;
	if (verticalOutputSettings.recordingFilenamePattern.trimmed().isEmpty())
		verticalOutputSettings.recordingFilenamePattern = QString::fromUtf8(kDefaultRecordingFilenamePattern);
	if (verticalOutputSettings.replayPath.trimmed().isEmpty())
		verticalOutputSettings.replayPath = defaultMoviesPath;
	verticalOutputSettings.streamingVideoBitrateKbps = std::max(500, verticalOutputSettings.streamingVideoBitrateKbps);
	verticalOutputSettings.recordingVideoBitrateKbps = std::max(1000, verticalOutputSettings.recordingVideoBitrateKbps);
	verticalOutputSettings.audioBitrateKbps = std::max(64, verticalOutputSettings.audioBitrateKbps);
	verticalOutputSettings.streamDelayMs = std::max(0, verticalOutputSettings.streamDelayMs);
	verticalOutputSettings.recordingSplitMinutes = std::clamp(verticalOutputSettings.recordingSplitMinutes, 1, 24 * 60);
	verticalOutputSettings.replayDurationSeconds = std::max(5, verticalOutputSettings.replayDurationSeconds);
	if (verticalOutputSettings.audioTrackMask == 0)
		verticalOutputSettings.audioTrackMask = 0x1;

	const int verticalIndex = FindCanvasIndex(VerticalCanvasId());
	if (verticalIndex >= 0) {
		canvases[verticalIndex].followMainStreaming = verticalOutputSettings.followMainStreaming;
		canvases[verticalIndex].followMainRecording = verticalOutputSettings.followMainRecording;
		canvases[verticalIndex].followMainReplay = verticalOutputSettings.followMainReplay;
		canvases[verticalIndex].followMainVirtualCamera = verticalOutputSettings.followMainVirtualCamera;
	}
}

int SwitchCanvasManager::FindCanvasIndex(const QString &canvasId) const
{
	for (int index = 0; index < canvases.size(); index++) {
		if (canvases[index].id == canvasId)
			return index;
	}
	return -1;
}

void SwitchCanvasManager::SuspendCanvasRendering()
{
	suppressCanvasRender = true;
	renderResumeGeneration++;
}

void SwitchCanvasManager::ScheduleCanvasRenderingResume(int delayMs)
{
	const int generation = renderResumeGeneration;
	QPointer<SwitchCanvasManager> self(this);
	QTimer::singleShot(std::max(0, delayMs), this, [self, generation]() {
		if (!self || generation != self->renderResumeGeneration)
			return;
		self->suppressCanvasRender = false;
		blog(LOG_INFO, "[Switch] Canvas render resume generation=%d", generation);
	});
}

QVector<SwitchCanvasDescriptor> SwitchCanvasManager::Canvases() const
{
	QVector<SwitchCanvasDescriptor> result = canvases;
	const int mainIndex = FindCanvasIndex(MainCanvasId());
	if (mainIndex >= 0) {
		obs_video_info ovi = {};
		obs_get_video_info(&ovi);
		result[mainIndex].size = QSize(int(ovi.base_width), int(ovi.base_height));
		result[mainIndex].aspectPreset = PresetLabelForSize(result[mainIndex].size);

		OBSSourceAutoRelease currentScene = obs_frontend_get_current_scene();
		if (currentScene) {
			result[mainIndex].activeSceneUuid = QString::fromUtf8(obs_source_get_uuid(currentScene));
			result[mainIndex].activeSceneName = QString::fromUtf8(obs_source_get_name(currentScene));
		}
	}

	const int verticalIndex = FindCanvasIndex(VerticalCanvasId());
	if (verticalIndex >= 0) {
		UpdateVerticalActiveSceneFromRuntime();
		result[verticalIndex] = canvases[verticalIndex];
	}

	return result;
}

SwitchCanvasDescriptor SwitchCanvasManager::CanvasDescriptor(const QString &canvasId) const
{
	const QVector<SwitchCanvasDescriptor> descriptors = Canvases();
	for (const auto &descriptor : descriptors) {
		if (descriptor.id == canvasId)
			return descriptor;
	}
	return {};
}

SwitchCanvasOutputSettings SwitchCanvasManager::OutputSettings(const QString &canvasId) const
{
	if (canvasId == VerticalCanvasId())
		return verticalOutputSettings;
	return {};
}

QSize SwitchCanvasManager::CanvasSize(const QString &canvasId) const
{
	const auto descriptor = CanvasDescriptor(canvasId);
	return descriptor.size.isValid() ? descriptor.size : QSize(1920, 1080);
}

obs_source_t *SwitchCanvasManager::CanvasSceneSource(const QString &canvasId, const QString &sceneUuidOrName) const
{
	if (sceneUuidOrName.isEmpty())
		return nullptr;

	if (canvasId == MainCanvasId())
		return ResolveFrontendScene(sceneUuidOrName);

	if (canvasId != VerticalCanvasId())
		return nullptr;
	if (suppressCanvasRender)
		return nullptr;

	if (!verticalCanvas || obs_canvas_removed(verticalCanvas))
		return nullptr;

	return ResolveCanvasScene(verticalCanvas, sceneUuidOrName);
}

obs_canvas_t *SwitchCanvasManager::AcquireCanvasByName(const QString &name) const
{
	obs_frontend_canvas_list canvasList = {};
	obs_frontend_get_canvases(&canvasList);

	obs_canvas_t *result = nullptr;
	for (size_t index = 0; index < canvasList.canvases.num; index++) {
		obs_canvas_t *candidate = canvasList.canvases.array[index];
		if (!candidate || obs_canvas_removed(candidate))
			continue;
		if (QString::fromUtf8(obs_canvas_get_name(candidate)) == name) {
			result = obs_canvas_get_ref(candidate);
			break;
		}
	}

	obs_frontend_canvas_list_free(&canvasList);
	return result;
}

void SwitchCanvasManager::ReleaseTransitions()
{
	if (transitionSource) {
		obs_weak_source_release(transitionSource);
		transitionSource = nullptr;
	}
	transitions.clear();
	activeTransitionName.clear();
	defaultTransitionName.clear();
}

void SwitchCanvasManager::EnsureTransitions()
{
	if (!transitions.isEmpty())
		return;

	size_t index = 0;
	const char *id = nullptr;
	while (obs_enum_transition_types(index++, &id)) {
		const char *displayName = obs_source_get_display_name(id);
		if (!displayName || !*displayName)
			continue;
		OBSSourceAutoRelease transition = obs_source_create_private(id, displayName, nullptr);
		if (transition)
			transitions.push_back(transition.Get());
	}

	if (defaultTransitionName.isEmpty()) {
		const char *fadeName = obs_source_get_display_name("fade_transition");
		if (fadeName && *fadeName)
			defaultTransitionName = QString::fromUtf8(fadeName);
	}

	if (activeTransitionName.isEmpty())
		activeTransitionName = defaultTransitionName;
}

obs_source_t *SwitchCanvasManager::GetTransition(const QString &transitionName) const
{
	if (transitionName.isEmpty())
		return nullptr;
	for (const auto &transition : transitions) {
		if (!transition)
			continue;
		if (QString::fromUtf8(obs_source_get_name(transition)) == transitionName)
			return transition;
	}
	return nullptr;
}

bool SwitchCanvasManager::ActivateTransition(const QString &transitionName)
{
	if (!EnsureVerticalCanvas())
		return false;
	EnsureTransitions();

	obs_source_t *newTransition = GetTransition(transitionName);
	if (!newTransition)
		return false;

	obs_transition_set_size(newTransition, uint32_t(std::max(2, CanvasSize(VerticalCanvasId()).width())),
				uint32_t(std::max(2, CanvasSize(VerticalCanvasId()).height())));

	obs_source_t *oldSource = transitionSource ? obs_weak_source_get_source(transitionSource) : nullptr;
	if (oldSource && oldSource == newTransition) {
		obs_source_release(oldSource);
		activeTransitionName = transitionName;
		return true;
	}

	if (!oldSource || obs_source_get_type(oldSource) != OBS_SOURCE_TYPE_TRANSITION) {
		if (oldSource) {
			obs_transition_set(newTransition, oldSource);
			obs_source_release(oldSource);
		}
		if (transitionSource)
			obs_weak_source_release(transitionSource);
		transitionSource = obs_source_get_weak_source(newTransition);
		obs_canvas_set_channel(verticalCanvas, 0, newTransition);
		obs_source_inc_showing(newTransition);
		obs_source_inc_active(newTransition);
		activeTransitionName = transitionName;
		return true;
	}

	if (oldSource != newTransition) {
		obs_source_inc_showing(newTransition);
		obs_source_inc_active(newTransition);
		obs_transition_swap_begin(newTransition, oldSource);
		if (transitionSource)
			obs_weak_source_release(transitionSource);
		transitionSource = obs_source_get_weak_source(newTransition);
		obs_canvas_set_channel(verticalCanvas, 0, newTransition);
		obs_transition_swap_end(newTransition, oldSource);
		obs_source_dec_showing(oldSource);
		obs_source_dec_active(oldSource);
	}
	obs_source_release(oldSource);
	activeTransitionName = transitionName;
	return true;
}

bool SwitchCanvasManager::EnsureVerticalCanvas()
{
	EnsureDefaultState();
	EnsureTransitions();

	const int canvasIndex = FindCanvasIndex(VerticalCanvasId());
	if (canvasIndex < 0)
		return false;
	if (ensuringVerticalCanvas)
		return verticalCanvas && !obs_canvas_removed(verticalCanvas);
	QScopedValueRollback<bool> canvasGuard(ensuringVerticalCanvas, true);

	auto &descriptor = canvases[canvasIndex];
	if (verticalCanvas && !obs_canvas_removed(verticalCanvas)) {
		EnsureVerticalSceneList();
		return true;
	}

	if (verticalCanvas) {
		obs_canvas_release(verticalCanvas);
		verticalCanvas = nullptr;
	}

	verticalCanvas = AcquireCanvasByName(descriptor.name);
	if (!verticalCanvas)
		verticalCanvas = obs_frontend_add_canvas(QT_TO_UTF8(descriptor.name), nullptr, PROGRAM);
	if (!verticalCanvas) {
		blog(LOG_WARNING, "[Switch] Failed to create vertical canvas '%s'", descriptor.name.toUtf8().constData());
		return false;
	}

	obs_canvas_set_name(verticalCanvas, QT_TO_UTF8(descriptor.name));

	obs_video_info ovi = {};
	obs_get_video_info(&ovi);
	ovi.base_width = uint32_t(std::max(2, descriptor.size.width()));
	ovi.base_height = uint32_t(std::max(2, descriptor.size.height()));
	ovi.output_width = ovi.base_width;
	ovi.output_height = ovi.base_height;
	obs_canvas_reset_video(verticalCanvas, &ovi);

	if (!defaultTransitionName.isEmpty())
		ActivateTransition(defaultTransitionName);

	EnsureVerticalSceneList();
	return true;
}

void SwitchCanvasManager::EnsureVerticalSceneList()
{
	if (ensuringVerticalSceneList)
		return;
	QScopedValueRollback<bool> sceneGuard(ensuringVerticalSceneList, true);

	if (!verticalCanvas && !EnsureVerticalCanvas())
		return;

	const QVector<SwitchCanvasSceneDescriptor> existingScenes = ScenesForCanvas(VerticalCanvasId());
	QStringList desiredScenes = verticalSceneNames;
	desiredScenes.erase(std::remove_if(desiredScenes.begin(), desiredScenes.end(),
					   [&](const QString &sceneName) {
						   return removedVerticalSceneNames.contains(sceneName, Qt::CaseInsensitive);
					   }),
			    desiredScenes.end());
	if (desiredScenes.isEmpty() && existingScenes.isEmpty())
		desiredScenes.push_back(QString::fromUtf8(kDefaultVerticalSceneName));

	for (const auto &sceneName : desiredScenes) {
		bool found = false;
		for (const auto &existing : existingScenes) {
			if (existing.name == sceneName) {
				found = true;
				break;
			}
		}
		if (!found) {
			if (obs_scene_t *scene = obs_canvas_scene_create(verticalCanvas, QT_TO_UTF8(sceneName)))
				obs_scene_release(scene);
		}
	}

	RebuildVerticalSceneCache();
	UpdateVerticalActiveSceneFromRuntime();

	if (canvases[FindCanvasIndex(VerticalCanvasId())].activeSceneName.isEmpty() && !verticalSceneNames.isEmpty())
		SetCanvasActiveScene(VerticalCanvasId(), verticalSceneNames.front());
}

void SwitchCanvasManager::RebuildVerticalSceneCache()
{
	QStringList nextSceneNames = verticalSceneNames;
	nextSceneNames.erase(std::remove_if(nextSceneNames.begin(), nextSceneNames.end(),
					    [&](const QString &sceneName) {
						    return removedVerticalSceneNames.contains(sceneName, Qt::CaseInsensitive);
					    }),
			     nextSceneNames.end());

	if (verticalCanvas && !obs_canvas_removed(verticalCanvas)) {
		QVector<SwitchCanvasSceneDescriptor> liveScenes;
		SceneDescriptorsCollector collector{&liveScenes};
		obs_canvas_enum_scenes(verticalCanvas, CollectCanvasScenes, &collector);
		for (const auto &scene : liveScenes) {
			if (removedVerticalSceneNames.contains(scene.name, Qt::CaseInsensitive))
				continue;
			if (!nextSceneNames.contains(scene.name, Qt::CaseInsensitive))
				nextSceneNames.push_back(scene.name);
		}
	}

	verticalSceneNames = nextSceneNames;
	if (verticalSceneNames.isEmpty())
		verticalSceneNames.push_back(QString::fromUtf8(kDefaultVerticalSceneName));
}

QVector<SwitchCanvasSceneDescriptor> SwitchCanvasManager::ScenesForCanvas(const QString &canvasId) const
{
	QVector<SwitchCanvasSceneDescriptor> scenes;

	if (canvasId == MainCanvasId()) {
		struct obs_frontend_source_list sourceList = {};
		obs_frontend_get_scenes(&sourceList);
		for (size_t index = 0; index < sourceList.sources.num; index++) {
			obs_source_t *source = sourceList.sources.array[index];
			if (!source)
				continue;
			scenes.push_back(
				{QString::fromUtf8(obs_source_get_uuid(source)), QString::fromUtf8(obs_source_get_name(source))});
		}
		obs_frontend_source_list_free(&sourceList);
		return scenes;
	}

	if (canvasId == VerticalCanvasId() && !ensuringVerticalSceneList) {
		const int verticalIndex = FindCanvasIndex(VerticalCanvasId());
		const QString activeSceneUuid = verticalIndex >= 0 ? canvases[verticalIndex].activeSceneUuid : QString();
		const QString activeSceneName = verticalIndex >= 0 ? canvases[verticalIndex].activeSceneName : QString();
		for (const auto &sceneName : verticalSceneNames) {
			if (removedVerticalSceneNames.contains(sceneName, Qt::CaseInsensitive))
				continue;
			QString sceneUuid = sceneName == activeSceneName ? activeSceneUuid : QString();
			if (sceneUuid.isEmpty() && verticalCanvas && !obs_canvas_removed(verticalCanvas) && !suppressCanvasRender) {
				OBSSourceAutoRelease sceneSource =
					obs_canvas_get_source_by_name(verticalCanvas, sceneName.toUtf8().constData());
				if (sceneSource && !obs_source_removed(sceneSource))
					sceneUuid = QString::fromUtf8(obs_source_get_uuid(sceneSource));
			}
			scenes.push_back({sceneUuid, sceneName});
		}
		if (scenes.isEmpty())
			scenes.push_back({QString(), QString::fromUtf8(kDefaultVerticalSceneName)});
		return scenes;
	}

	obs_canvas_t *canvas = CanvasById(canvasId);
	if (!canvas)
		return scenes;

	SceneDescriptorsCollector collector{&scenes};
	obs_canvas_enum_scenes(canvas, CollectCanvasScenes, &collector);
	if (canvasId == VerticalCanvasId()) {
		scenes.erase(std::remove_if(scenes.begin(), scenes.end(),
					    [&](const SwitchCanvasSceneDescriptor &scene) {
						    return removedVerticalSceneNames.contains(scene.name, Qt::CaseInsensitive);
					    }),
			     scenes.end());
	}

	if (scenes.isEmpty() && canvasId == VerticalCanvasId()) {
		for (const auto &sceneName : verticalSceneNames) {
			if (removedVerticalSceneNames.contains(sceneName, Qt::CaseInsensitive))
				continue;
			OBSSourceAutoRelease sceneSource =
				obs_canvas_get_source_by_name(canvas, sceneName.toUtf8().constData());
			if (!sceneSource || obs_source_removed(sceneSource))
				continue;
			scenes.push_back(
				{QString::fromUtf8(obs_source_get_uuid(sceneSource)), QString::fromUtf8(obs_source_get_name(sceneSource))});
		}
	}

	return scenes;
}

QVector<SwitchCanvasTransitionDescriptor> SwitchCanvasManager::Transitions() const
{
	const_cast<SwitchCanvasManager *>(this)->EnsureTransitions();
	QVector<SwitchCanvasTransitionDescriptor> result;
	for (const auto &transition : transitions) {
		if (!transition)
			continue;
		result.push_back({QString::fromUtf8(obs_source_get_name(transition)),
				  obs_is_source_configurable(obs_source_get_unversioned_id(transition))});
	}
	return result;
}

QVector<SwitchCanvasLink> SwitchCanvasManager::Links() const
{
	return links;
}

obs_source_t *SwitchCanvasManager::ResolveFrontendScene(const QString &sceneUuidOrName) const
{
	if (sceneUuidOrName.isEmpty())
		return nullptr;

	if (auto *source = obs_get_source_by_uuid(sceneUuidOrName.toUtf8().constData()))
		return source;
	return obs_get_source_by_name(sceneUuidOrName.toUtf8().constData());
}

obs_source_t *SwitchCanvasManager::ResolveCanvasScene(obs_canvas_t *canvas, const QString &sceneUuidOrName) const
{
	if (!canvas || sceneUuidOrName.isEmpty())
		return nullptr;

	if (auto *source = obs_get_source_by_uuid(sceneUuidOrName.toUtf8().constData()))
		return source;
	if (auto *source = obs_canvas_get_source_by_name(canvas, sceneUuidOrName.toUtf8().constData()))
		return source;

	CanvasSceneLookup lookup{sceneUuidOrName, nullptr};
	obs_canvas_enum_scenes(canvas, CollectCanvasSceneById, &lookup);
	if (lookup.source)
		return lookup.source;
	return nullptr;
}

bool SwitchCanvasManager::SetCanvasName(const QString &canvasId, const QString &name)
{
	const int index = FindCanvasIndex(canvasId);
	if (index < 0 || name.trimmed().isEmpty())
		return false;

	canvases[index].name = name.trimmed();
	if (canvasId == VerticalCanvasId() && verticalCanvas)
		obs_canvas_set_name(verticalCanvas, QT_TO_UTF8(canvases[index].name));

	emit StateChanged();
	return true;
}

bool SwitchCanvasManager::SetVerticalPreset(const QSize &size, const QString &preset)
{
	const int index = FindCanvasIndex(VerticalCanvasId());
	if (index < 0 || !size.isValid())
		return false;
	SuspendCanvasRendering();

	auto &descriptor = canvases[index];
	descriptor.size = size;
	descriptor.aspectPreset = preset;

	if (verticalCanvas) {
		obs_video_info ovi = {};
		obs_get_video_info(&ovi);
		ovi.base_width = uint32_t(std::max(2, size.width()));
		ovi.base_height = uint32_t(std::max(2, size.height()));
		ovi.output_width = ovi.base_width;
		ovi.output_height = ovi.base_height;
		obs_canvas_reset_video(verticalCanvas, &ovi);
	}

	emit StateChanged();
	ScheduleCanvasRenderingResume();
	return true;
}

bool SwitchCanvasManager::SetCanvasLinkedSync(const QString &canvasId, bool enabled)
{
	const int index = FindCanvasIndex(canvasId);
	if (index < 0)
		return false;

	canvases[index].linkedSceneSync = enabled;
	if (enabled && canvasId == VerticalCanvasId())
		SyncLinkedSceneFromProgram();

	emit StateChanged();
	return true;
}

bool SwitchCanvasManager::SetOutputSettings(const QString &canvasId, const SwitchCanvasOutputSettings &settings)
{
	if (canvasId != VerticalCanvasId())
		return false;

	verticalOutputSettings = settings;
	verticalOutputSettings.streamingVideoBitrateKbps = std::max(500, verticalOutputSettings.streamingVideoBitrateKbps);
	verticalOutputSettings.recordingVideoBitrateKbps = std::max(1000, verticalOutputSettings.recordingVideoBitrateKbps);
	verticalOutputSettings.audioBitrateKbps = std::max(64, verticalOutputSettings.audioBitrateKbps);
	verticalOutputSettings.streamDelayMs = std::max(0, verticalOutputSettings.streamDelayMs);
	verticalOutputSettings.recordingSplitMinutes = std::clamp(verticalOutputSettings.recordingSplitMinutes, 1, 24 * 60);
	verticalOutputSettings.replayDurationSeconds = std::max(5, verticalOutputSettings.replayDurationSeconds);
	if (verticalOutputSettings.audioTrackMask == 0)
		verticalOutputSettings.audioTrackMask = 0x1;

	const int index = FindCanvasIndex(VerticalCanvasId());
	if (index >= 0) {
		canvases[index].followMainStreaming = verticalOutputSettings.followMainStreaming;
		canvases[index].followMainRecording = verticalOutputSettings.followMainRecording;
		canvases[index].followMainReplay = verticalOutputSettings.followMainReplay;
		canvases[index].followMainVirtualCamera = verticalOutputSettings.followMainVirtualCamera;
	}

	emit StateChanged();
	return true;
}

bool SwitchCanvasManager::SetCanvasActiveScene(const QString &canvasId, const QString &sceneUuidOrName)
{
	const int index = FindCanvasIndex(canvasId);
	if (index < 0)
		return false;

	if (canvasId == MainCanvasId()) {
		OBSSourceAutoRelease source = ResolveFrontendScene(sceneUuidOrName);
		if (!source)
			return false;
		obs_frontend_set_current_scene(source);
		canvases[index].activeSceneUuid = QString::fromUtf8(obs_source_get_uuid(source));
		canvases[index].activeSceneName = QString::fromUtf8(obs_source_get_name(source));
		emit StateChanged();
		return true;
	}

	if (canvasId != VerticalCanvasId())
		return false;
	if (!verticalCanvas && !EnsureVerticalCanvas())
		return false;
	if (!verticalCanvas || obs_canvas_removed(verticalCanvas))
		return false;
	EnsureTransitions();
	SuspendCanvasRendering();

	obs_source_t *sceneSource = ResolveCanvasScene(verticalCanvas, sceneUuidOrName);
	if (!sceneSource) {
		ScheduleCanvasRenderingResume();
		return false;
	}

	if (transitionSource) {
		if (obs_source_t *oldSource = obs_weak_source_get_source(transitionSource)) {
			if (obs_source_get_type(oldSource) == OBS_SOURCE_TYPE_TRANSITION) {
				obs_canvas_set_channel(verticalCanvas, 0, nullptr);
				obs_source_dec_showing(oldSource);
				obs_source_dec_active(oldSource);
			}
			obs_source_release(oldSource);
		}
		obs_weak_source_release(transitionSource);
		transitionSource = nullptr;
		activeTransitionName.clear();
	}

	// Scene switching stays on the safest direct-canvas path until the dedicated
	// transition runtime is stabilized for production use.
	obs_canvas_set_channel(verticalCanvas, 0, sceneSource);
	canvases[index].activeSceneUuid = QString::fromUtf8(obs_source_get_uuid(sceneSource));
	canvases[index].activeSceneName = QString::fromUtf8(obs_source_get_name(sceneSource));
	obs_source_release(sceneSource);
	ScheduleCanvasRenderingResume(250);
	emit StateChanged();
	return true;
}

QString SwitchCanvasManager::NextVerticalSceneName(const QString &baseName) const
{
	const QString fallback = QString::fromUtf8(kDefaultVerticalSceneName);
	QString candidate = baseName.trimmed().isEmpty() ? fallback : baseName.trimmed();
	QStringList existing = verticalSceneNames;
	int suffix = 2;
	while (existing.contains(candidate, Qt::CaseInsensitive))
		candidate = QStringLiteral("%1 %2").arg(baseName.trimmed().isEmpty() ? fallback : baseName.trimmed()).arg(suffix++);
	return candidate;
}

bool SwitchCanvasManager::CreateVerticalScene(const QString &baseName, QString *createdName)
{
	if (!EnsureVerticalCanvas())
		return false;
	SuspendCanvasRendering();

	const QString sceneName = NextVerticalSceneName(baseName);
	blog(LOG_INFO, "[Switch] CreateVerticalScene begin name='%s'", sceneName.toUtf8().constData());
	obs_scene_t *scene = obs_canvas_scene_create(verticalCanvas, QT_TO_UTF8(sceneName));
	if (!scene) {
		blog(LOG_WARNING, "[Switch] Failed to create vertical scene '%s'", sceneName.toUtf8().constData());
		ScheduleCanvasRenderingResume();
		return false;
	}
	blog(LOG_INFO, "[Switch] CreateVerticalScene created name='%s' ptr=%p", sceneName.toUtf8().constData(), scene);
	obs_scene_release(scene);

	removedVerticalSceneNames.removeAll(sceneName);
	if (!verticalSceneNames.contains(sceneName, Qt::CaseInsensitive))
		verticalSceneNames.push_back(sceneName);
	if (createdName)
		*createdName = sceneName;
	blog(LOG_INFO, "[Switch] CreateVerticalScene emit StateChanged name='%s' total=%d",
	     sceneName.toUtf8().constData(), int(verticalSceneNames.size()));
	ScheduleCanvasRenderingResume(350);
	blog(LOG_INFO, "[Switch] CreateVerticalScene end name='%s'", sceneName.toUtf8().constData());
	return true;
}

bool SwitchCanvasManager::DuplicateVerticalScene(const QString &sceneUuidOrName, const QString &baseName, QString *createdName)
{
	if (!EnsureVerticalCanvas())
		return false;
	SuspendCanvasRendering();

	obs_source_t *sceneSource = ResolveCanvasScene(verticalCanvas, sceneUuidOrName);
	if (!sceneSource) {
		ScheduleCanvasRenderingResume();
		return false;
	}

	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (!scene) {
		obs_source_release(sceneSource);
		ScheduleCanvasRenderingResume();
		return false;
	}

	const QString sceneName = NextVerticalSceneName(baseName);
	obs_scene_t *duplicateScene = obs_scene_duplicate(scene, QT_TO_UTF8(sceneName), OBS_SCENE_DUP_REFS);
	obs_source_release(sceneSource);
	if (!duplicateScene) {
		ScheduleCanvasRenderingResume();
		return false;
	}
	obs_source_t *duplicateSource = obs_scene_get_source(duplicateScene);
	obs_scene_release(duplicateScene);
	if (!duplicateSource) {
		ScheduleCanvasRenderingResume();
		return false;
	}
	obs_source_release(duplicateSource);

	removedVerticalSceneNames.removeAll(sceneName);
	if (!verticalSceneNames.contains(sceneName, Qt::CaseInsensitive))
		verticalSceneNames.push_back(sceneName);
	if (createdName)
		*createdName = sceneName;
	ScheduleCanvasRenderingResume(350);
	return true;
}

bool SwitchCanvasManager::RenameVerticalScene(const QString &sceneUuidOrName, const QString &name)
{
	if (!EnsureVerticalCanvas())
		return false;
	SuspendCanvasRendering();

	const QString trimmedName = name.trimmed();
	if (trimmedName.isEmpty()) {
		ScheduleCanvasRenderingResume();
		return false;
	}

	obs_source_t *existing = ResolveCanvasScene(verticalCanvas, trimmedName);
	if (existing) {
		obs_source_release(existing);
		if (sceneUuidOrName != trimmedName) {
			ScheduleCanvasRenderingResume();
			return false;
		}
	}

	obs_source_t *sceneSource = ResolveCanvasScene(verticalCanvas, sceneUuidOrName);
	if (!sceneSource) {
		ScheduleCanvasRenderingResume();
		return false;
	}

	const QString previousName = QString::fromUtf8(obs_source_get_name(sceneSource));
	obs_source_set_name(sceneSource, QT_TO_UTF8(trimmedName));
	obs_source_release(sceneSource);
	removedVerticalSceneNames.removeAll(trimmedName);
	for (QString &sceneName : verticalSceneNames) {
		if (sceneName == previousName)
			sceneName = trimmedName;
	}

	for (auto &link : links) {
		if (link.targetSceneName == previousName)
			link.targetSceneName = trimmedName;
	}

	const int index = FindCanvasIndex(VerticalCanvasId());
	if (index >= 0 && canvases[index].activeSceneName == previousName)
		canvases[index].activeSceneName = trimmedName;

	RebuildVerticalSceneCache();
	emit StateChanged();
	ScheduleCanvasRenderingResume();
	return true;
}

bool SwitchCanvasManager::CanRemoveVerticalScene(const QString &sceneUuidOrName, QString *reason) const
{
	auto setReason = [reason](const QString &message) {
		if (reason)
			*reason = message;
	};

	if (sceneUuidOrName.trimmed().isEmpty()) {
		setReason(QStringLiteral("No vertical scene was selected."));
		return false;
	}

	const auto scenes = ScenesForCanvas(VerticalCanvasId());
	const qsizetype sceneCount = std::max(scenes.size(), verticalSceneNames.size());
	if (sceneCount <= 1) {
		setReason(QStringLiteral("Switch keeps one vertical scene available so the vertical canvas cannot go blank."));
		return false;
	}

	QString targetUuid;
	QString targetName;
	for (const auto &scene : scenes) {
		if (scene.uuid == sceneUuidOrName || scene.name == sceneUuidOrName) {
			targetUuid = scene.uuid;
			targetName = scene.name;
			break;
		}
	}

	if (targetName.isEmpty()) {
		for (const auto &sceneName : verticalSceneNames) {
			if (sceneName == sceneUuidOrName) {
				targetName = sceneName;
				break;
			}
		}
		if (targetName.isEmpty()) {
			setReason(QStringLiteral("The selected vertical scene no longer exists."));
			return false;
		}
	}

	const auto descriptor = CanvasDescriptor(VerticalCanvasId());
	const bool activeByUuid = !targetUuid.isEmpty() && descriptor.activeSceneUuid == targetUuid;
	const bool activeByName = descriptor.activeSceneName == targetName;
	if (activeByUuid || activeByName) {
		setReason(QStringLiteral("The live vertical scene cannot be removed while it is displayed."));
		return false;
	}

	setReason(QString());
	return true;
}

bool SwitchCanvasManager::RemoveVerticalScene(const QString &sceneName)
{
	if (!EnsureVerticalCanvas() || sceneName.isEmpty())
		return false;

	QString refusalReason;
	if (!CanRemoveVerticalScene(sceneName, &refusalReason)) {
		if (refusalReason == QStringLiteral("The selected vertical scene no longer exists.")) {
			if (!removedVerticalSceneNames.contains(sceneName, Qt::CaseInsensitive))
				removedVerticalSceneNames.push_back(sceneName);
			verticalSceneNames.erase(std::remove_if(verticalSceneNames.begin(), verticalSceneNames.end(),
								[&](const QString &name) {
									return name.compare(sceneName, Qt::CaseInsensitive) == 0;
								}),
						verticalSceneNames.end());
			links.erase(std::remove_if(links.begin(), links.end(),
						   [&](const SwitchCanvasLink &link) {
							   return link.targetSceneName == sceneName;
						   }),
				    links.end());
			RebuildVerticalSceneCache();
			emit StateChanged();
			return true;
		}
		blog(LOG_WARNING, "[Switch] Refused to remove vertical scene '%s': %s", sceneName.toUtf8().constData(),
		     refusalReason.toUtf8().constData());
		return false;
	}

	SuspendCanvasRendering();

	auto rememberRemovedScene = [&](const QString &removedName) {
		if (removedName.trimmed().isEmpty())
			return;
		if (!removedVerticalSceneNames.contains(removedName, Qt::CaseInsensitive))
			removedVerticalSceneNames.push_back(removedName);
	};

	auto removeCachedScene = [&]() {
		rememberRemovedScene(sceneName);
		const qsizetype beforeSize = verticalSceneNames.size();
		verticalSceneNames.erase(std::remove_if(verticalSceneNames.begin(), verticalSceneNames.end(),
							[&](const QString &name) {
								return name.compare(sceneName, Qt::CaseInsensitive) == 0;
							}),
					verticalSceneNames.end());
		if (verticalSceneNames.size() == beforeSize)
			return false;

		links.erase(std::remove_if(links.begin(), links.end(),
					   [&](const SwitchCanvasLink &link) {
						   return link.targetSceneName == sceneName;
					   }),
			    links.end());
		RebuildVerticalSceneCache();
		emit StateChanged();
		ScheduleCanvasRenderingResume(350);
		return true;
	};

	obs_source_t *sceneSource = ResolveCanvasScene(verticalCanvas, sceneName);
	if (!sceneSource) {
		if (removeCachedScene())
			return true;
		ScheduleCanvasRenderingResume();
		return false;
	}

	const QString resolvedName = QString::fromUtf8(obs_source_get_name(sceneSource));
	const QString resolvedUuid = QString::fromUtf8(obs_source_get_uuid(sceneSource));
	rememberRemovedScene(resolvedName);
	rememberRemovedScene(sceneName);
	obs_scene_t *scene = obs_scene_from_source(sceneSource);
	if (!scene) {
		obs_source_release(sceneSource);
		if (removeCachedScene())
			return true;
		ScheduleCanvasRenderingResume();
		return false;
	}

	obs_canvas_scene_remove(scene);
	obs_source_remove(sceneSource);
	obs_source_release(sceneSource);
	verticalSceneNames.erase(std::remove_if(verticalSceneNames.begin(), verticalSceneNames.end(),
						[&](const QString &name) {
							return name.compare(resolvedName, Qt::CaseInsensitive) == 0 ||
							       name.compare(sceneName, Qt::CaseInsensitive) == 0;
						}),
				 verticalSceneNames.end());
	links.erase(std::remove_if(links.begin(), links.end(),
				   [&](const SwitchCanvasLink &link) {
					   return link.targetSceneUuid == resolvedUuid || link.targetSceneName == resolvedName ||
						  link.targetSceneName == sceneName;
				   }),
		    links.end());
	RebuildVerticalSceneCache();

	const int index = FindCanvasIndex(VerticalCanvasId());
	if (index >= 0 && (canvases[index].activeSceneName == resolvedName || canvases[index].activeSceneUuid == resolvedUuid)) {
		canvases[index].activeSceneName.clear();
		canvases[index].activeSceneUuid.clear();
		if (!verticalSceneNames.isEmpty())
			SetCanvasActiveScene(VerticalCanvasId(), verticalSceneNames.front());
	}

	emit StateChanged();
	ScheduleCanvasRenderingResume(350);
	return true;
}

bool SwitchCanvasManager::SetLinkedScene(const QString &mainSceneUuid, const QString &mainSceneName, const QString &targetSceneUuid,
					 const QString &targetSceneName)
{
	if (mainSceneUuid.isEmpty() || targetSceneName.isEmpty())
		return false;

	for (auto &link : links) {
		if (link.mainSceneUuid == mainSceneUuid) {
			link.mainSceneName = mainSceneName;
			link.targetSceneUuid = targetSceneUuid;
			link.targetSceneName = targetSceneName;
			emit StateChanged();
			return true;
		}
	}

	links.push_back({mainSceneUuid, mainSceneName, targetSceneUuid, targetSceneName});
	emit StateChanged();
	return true;
}

bool SwitchCanvasManager::ClearLinkedScene(const QString &mainSceneUuid)
{
	if (mainSceneUuid.isEmpty())
		return false;

	const auto beforeSize = links.size();
	links.erase(std::remove_if(links.begin(), links.end(),
				   [&](const SwitchCanvasLink &link) { return link.mainSceneUuid == mainSceneUuid; }),
		    links.end());
	if (links.size() == beforeSize)
		return false;

	emit StateChanged();
	return true;
}

void SwitchCanvasManager::UpdateVerticalActiveSceneFromRuntime() const
{
	const int index = FindCanvasIndex(VerticalCanvasId());
	if (index < 0 || !verticalCanvas)
		return;

	if (obs_source_t *source = obs_canvas_get_channel(verticalCanvas, 0)) {
		obs_source_t *sceneSource = ResolveSceneOutputSource(source);
		if (sceneSource) {
			const_cast<SwitchCanvasDescriptor &>(canvases[index]).activeSceneUuid =
				QString::fromUtf8(obs_source_get_uuid(sceneSource));
			const_cast<SwitchCanvasDescriptor &>(canvases[index]).activeSceneName =
				QString::fromUtf8(obs_source_get_name(sceneSource));
			if (sceneSource != source)
				obs_source_release(sceneSource);
		}
		obs_source_release(source);
	}
}

void SwitchCanvasManager::SyncLinkedSceneFromProgram()
{
	const int verticalIndex = FindCanvasIndex(VerticalCanvasId());
	if (verticalIndex < 0 || !canvases[verticalIndex].linkedSceneSync)
		return;

	OBSSourceAutoRelease currentScene = obs_frontend_get_current_scene();
	if (!currentScene)
		return;

	const QString currentUuid = QString::fromUtf8(obs_source_get_uuid(currentScene));
	for (const auto &link : links) {
		if (link.mainSceneUuid == currentUuid) {
			SetCanvasActiveScene(VerticalCanvasId(), !link.targetSceneUuid.isEmpty() ? link.targetSceneUuid : link.targetSceneName);
			return;
		}
	}
}

bool SwitchCanvasManager::OpenPreviewWindow(const QString &canvasId)
{
	if (canvasId == VerticalCanvasId() && !EnsureVerticalCanvas())
		return false;

	const auto descriptor = CanvasDescriptor(canvasId);
	if (descriptor.id.isEmpty())
		return false;

	auto *window = new SwitchCanvasWindow(this, canvasId, descriptor.name, false);
	previewWindows.push_back(window);
	connect(window, &QObject::destroyed, [this, window]() { previewWindows.removeAll(window); });
	window->show();
	window->raise();
	window->activateWindow();
	return true;
}

bool SwitchCanvasManager::OpenProjector(const QString &canvasId)
{
	const auto descriptor = CanvasDescriptor(canvasId);
	if (descriptor.activeSceneName.isEmpty())
		return false;

	auto *window = new SwitchCanvasWindow(this, canvasId, QStringLiteral("Projector - Scene: %1").arg(descriptor.activeSceneName),
					      true);
	previewWindows.push_back(window);
	connect(window, &QObject::destroyed, [this, window]() { previewWindows.removeAll(window); });
	window->show();
	window->raise();
	window->activateWindow();
	return true;
}

bool SwitchCanvasManager::SetDefaultTransition(const QString &transitionName)
{
	EnsureTransitions();
	if (!GetTransition(transitionName))
		return false;

	defaultTransitionName = transitionName;
	if (!ActivateTransition(transitionName))
		return false;
	emit StateChanged();
	return true;
}

bool SwitchCanvasManager::SetDefaultTransitionDuration(int durationMs)
{
	defaultTransitionDurationMs = std::max(50, durationMs);
	emit StateChanged();
	return true;
}

QString SwitchCanvasManager::SceneTransitionName(const QString &sceneUuidOrName) const
{
	if (!verticalCanvas)
		return {};

	obs_source_t *sceneSource = ResolveCanvasScene(verticalCanvas, sceneUuidOrName);
	if (!sceneSource)
		return {};

	QString value;
	if (OBSDataAutoRelease privateSettings = obs_source_get_private_settings(sceneSource))
		value = QString::fromUtf8(obs_data_get_string(privateSettings, kSceneTransitionKey));
	obs_source_release(sceneSource);
	return value;
}

int SwitchCanvasManager::SceneTransitionDuration(const QString &sceneUuidOrName) const
{
	if (!verticalCanvas)
		return 0;

	obs_source_t *sceneSource = ResolveCanvasScene(verticalCanvas, sceneUuidOrName);
	if (!sceneSource)
		return 0;

	int value = 0;
	if (OBSDataAutoRelease privateSettings = obs_source_get_private_settings(sceneSource))
		value = int(obs_data_get_int(privateSettings, kSceneTransitionDurationKey));
	obs_source_release(sceneSource);
	return value;
}

bool SwitchCanvasManager::SetSceneTransition(const QString &sceneUuidOrName, const QString &transitionName, int durationMs)
{
	if (!EnsureVerticalCanvas())
		return false;
	if (!transitionName.isEmpty() && !GetTransition(transitionName))
		return false;

	obs_source_t *sceneSource = ResolveCanvasScene(verticalCanvas, sceneUuidOrName);
	if (!sceneSource)
		return false;

	if (OBSDataAutoRelease privateSettings = obs_source_get_private_settings(sceneSource)) {
		obs_data_set_string(privateSettings, kSceneTransitionKey, QT_TO_UTF8(transitionName.trimmed()));
		obs_data_set_int(privateSettings, kSceneTransitionDurationKey, std::max(0, durationMs));
	}
	obs_source_release(sceneSource);
	emit StateChanged();
	return true;
}

obs_data_t *SwitchCanvasManager::SaveState() const
{
	obs_data_t *root = obs_data_create();

	obs_data_array_t *canvasArray = obs_data_array_create();
	for (const auto &canvas : Canvases()) {
		obs_data_t *entry = obs_data_create();
		obs_data_set_string(entry, "id", canvas.id.toUtf8().constData());
		obs_data_set_string(entry, "name", canvas.name.toUtf8().constData());
		obs_data_set_int(entry, "width", canvas.size.width());
		obs_data_set_int(entry, "height", canvas.size.height());
		obs_data_set_string(entry, "aspect_preset", canvas.aspectPreset.toUtf8().constData());
		obs_data_set_string(entry, "active_scene_uuid", canvas.activeSceneUuid.toUtf8().constData());
		obs_data_set_string(entry, "active_scene_name", canvas.activeSceneName.toUtf8().constData());
		obs_data_set_bool(entry, "linked_scene_sync", canvas.linkedSceneSync);
		obs_data_set_bool(entry, "follow_main_streaming", canvas.followMainStreaming);
		obs_data_set_bool(entry, "follow_main_recording", canvas.followMainRecording);
		obs_data_set_bool(entry, "follow_main_replay", canvas.followMainReplay);
		obs_data_set_bool(entry, "follow_main_virtual_camera", canvas.followMainVirtualCamera);
		if (canvas.id == VerticalCanvasId()) {
			obs_data_set_string(entry, "default_transition_name", defaultTransitionName.toUtf8().constData());
			obs_data_set_int(entry, "default_transition_duration_ms", defaultTransitionDurationMs);
		}
		obs_data_array_push_back(canvasArray, entry);
		obs_data_release(entry);
	}
	obs_data_set_array(root, "canvases", canvasArray);
	obs_data_array_release(canvasArray);

	obs_data_array_t *sceneArray = obs_data_array_create();
	for (const auto &sceneName : verticalSceneNames) {
		obs_data_t *entry = obs_data_create();
		obs_data_set_string(entry, "name", sceneName.toUtf8().constData());
		obs_data_array_push_back(sceneArray, entry);
		obs_data_release(entry);
	}
	obs_data_set_array(root, "vertical_scenes", sceneArray);
	obs_data_array_release(sceneArray);

	obs_data_array_t *linkArray = obs_data_array_create();
	for (const auto &link : links) {
		obs_data_t *entry = obs_data_create();
		obs_data_set_string(entry, "main_scene_uuid", link.mainSceneUuid.toUtf8().constData());
		obs_data_set_string(entry, "main_scene_name", link.mainSceneName.toUtf8().constData());
		obs_data_set_string(entry, "target_scene_uuid", link.targetSceneUuid.toUtf8().constData());
		obs_data_set_string(entry, "target_scene_name", link.targetSceneName.toUtf8().constData());
		obs_data_array_push_back(linkArray, entry);
		obs_data_release(entry);
	}
	obs_data_set_array(root, "links", linkArray);
	obs_data_array_release(linkArray);

	obs_data_t *output = obs_data_create();
	obs_data_set_bool(output, "follow_main_streaming", verticalOutputSettings.followMainStreaming);
	obs_data_set_bool(output, "follow_main_recording", verticalOutputSettings.followMainRecording);
	obs_data_set_bool(output, "follow_main_replay", verticalOutputSettings.followMainReplay);
	obs_data_set_bool(output, "follow_main_virtual_camera", verticalOutputSettings.followMainVirtualCamera);
	obs_data_set_string(output, "stream_encoder_id", verticalOutputSettings.streamEncoderId.toUtf8().constData());
	obs_data_set_string(output, "record_encoder_id", verticalOutputSettings.recordEncoderId.toUtf8().constData());
	obs_data_set_int(output, "streaming_video_bitrate_kbps", verticalOutputSettings.streamingVideoBitrateKbps);
	obs_data_set_int(output, "recording_video_bitrate_kbps", verticalOutputSettings.recordingVideoBitrateKbps);
	obs_data_set_int(output, "audio_bitrate_kbps", verticalOutputSettings.audioBitrateKbps);
	obs_data_set_bool(output, "stream_delay_enabled", verticalOutputSettings.streamDelayEnabled);
	obs_data_set_int(output, "stream_delay_ms", verticalOutputSettings.streamDelayMs);
	obs_data_set_bool(output, "stream_delay_preserve", verticalOutputSettings.streamDelayPreserve);
	obs_data_set_string(output, "recording_path", verticalOutputSettings.recordingPath.toUtf8().constData());
	obs_data_set_string(output, "recording_filename_pattern",
			    verticalOutputSettings.recordingFilenamePattern.toUtf8().constData());
	obs_data_set_bool(output, "recording_split_enabled", verticalOutputSettings.recordingSplitEnabled);
	obs_data_set_int(output, "recording_split_minutes", verticalOutputSettings.recordingSplitMinutes);
	obs_data_set_string(output, "replay_path", verticalOutputSettings.replayPath.toUtf8().constData());
	obs_data_set_int(output, "replay_duration_seconds", verticalOutputSettings.replayDurationSeconds);
	obs_data_set_bool(output, "replay_always_on", verticalOutputSettings.replayAlwaysOn);
	obs_data_set_int(output, "audio_track_mask", int(verticalOutputSettings.audioTrackMask));
	obs_data_set_obj(root, "vertical_output", output);
	obs_data_release(output);

	return root;
}

void SwitchCanvasManager::LoadState(obs_data_t *data)
{
	RemoveRuntimeCanvases();
	canvases.clear();
	links.clear();
	verticalSceneNames.clear();
	verticalOutputSettings = {};

	if (data) {
		if (obs_data_array_t *canvasArray = obs_data_get_array(data, "canvases")) {
			const size_t count = obs_data_array_count(canvasArray);
			for (size_t index = 0; index < count; index++) {
				obs_data_t *entry = obs_data_array_item(canvasArray, index);
				canvases.push_back({QString::fromUtf8(obs_data_get_string(entry, "id")),
						    QString::fromUtf8(obs_data_get_string(entry, "name")),
						    QSize(int(obs_data_get_int(entry, "width")), int(obs_data_get_int(entry, "height"))),
						    QString::fromUtf8(obs_data_get_string(entry, "aspect_preset")),
						    QString::fromUtf8(obs_data_get_string(entry, "active_scene_uuid")),
						    QString::fromUtf8(obs_data_get_string(entry, "active_scene_name")),
						    obs_data_get_bool(entry, "linked_scene_sync"),
						    obs_data_get_bool(entry, "follow_main_streaming"),
						    obs_data_get_bool(entry, "follow_main_recording"),
						    obs_data_get_bool(entry, "follow_main_replay"),
						    obs_data_get_bool(entry, "follow_main_virtual_camera")});
				if (QString::fromUtf8(obs_data_get_string(entry, "id")) == VerticalCanvasId()) {
					defaultTransitionName = QString::fromUtf8(obs_data_get_string(entry, "default_transition_name"));
					defaultTransitionDurationMs = int(obs_data_get_int(entry, "default_transition_duration_ms"));
				}
				obs_data_release(entry);
			}
			obs_data_array_release(canvasArray);
		}

		if (obs_data_array_t *sceneArray = obs_data_get_array(data, "vertical_scenes")) {
			const size_t count = obs_data_array_count(sceneArray);
			for (size_t index = 0; index < count; index++) {
				obs_data_t *entry = obs_data_array_item(sceneArray, index);
				verticalSceneNames.push_back(QString::fromUtf8(obs_data_get_string(entry, "name")));
				obs_data_release(entry);
			}
			obs_data_array_release(sceneArray);
		}

		if (obs_data_array_t *linkArray = obs_data_get_array(data, "links")) {
			const size_t count = obs_data_array_count(linkArray);
			for (size_t index = 0; index < count; index++) {
				obs_data_t *entry = obs_data_array_item(linkArray, index);
				links.push_back({QString::fromUtf8(obs_data_get_string(entry, "main_scene_uuid")),
						 QString::fromUtf8(obs_data_get_string(entry, "main_scene_name")),
						 QString::fromUtf8(obs_data_get_string(entry, "target_scene_uuid")),
						 QString::fromUtf8(obs_data_get_string(entry, "target_scene_name"))});
				obs_data_release(entry);
			}
			obs_data_array_release(linkArray);
		}

		if (obs_data_t *output = obs_data_get_obj(data, "vertical_output")) {
			verticalOutputSettings.followMainStreaming = obs_data_get_bool(output, "follow_main_streaming");
			verticalOutputSettings.followMainRecording = obs_data_get_bool(output, "follow_main_recording");
			verticalOutputSettings.followMainReplay = obs_data_get_bool(output, "follow_main_replay");
			verticalOutputSettings.followMainVirtualCamera = obs_data_get_bool(output, "follow_main_virtual_camera");
			verticalOutputSettings.streamEncoderId =
				QString::fromUtf8(obs_data_get_string(output, "stream_encoder_id"));
			verticalOutputSettings.recordEncoderId =
				QString::fromUtf8(obs_data_get_string(output, "record_encoder_id"));
			verticalOutputSettings.streamingVideoBitrateKbps =
				int(obs_data_get_int(output, "streaming_video_bitrate_kbps"));
			verticalOutputSettings.recordingVideoBitrateKbps =
				int(obs_data_get_int(output, "recording_video_bitrate_kbps"));
			verticalOutputSettings.audioBitrateKbps = int(obs_data_get_int(output, "audio_bitrate_kbps"));
			verticalOutputSettings.streamDelayEnabled = obs_data_get_bool(output, "stream_delay_enabled");
			verticalOutputSettings.streamDelayMs = int(obs_data_get_int(output, "stream_delay_ms"));
			verticalOutputSettings.streamDelayPreserve = obs_data_get_bool(output, "stream_delay_preserve");
			verticalOutputSettings.recordingPath = QString::fromUtf8(obs_data_get_string(output, "recording_path"));
			verticalOutputSettings.recordingFilenamePattern =
				QString::fromUtf8(obs_data_get_string(output, "recording_filename_pattern"));
			verticalOutputSettings.recordingSplitEnabled =
				obs_data_get_bool(output, "recording_split_enabled");
			verticalOutputSettings.recordingSplitMinutes =
				int(obs_data_get_int(output, "recording_split_minutes"));
			verticalOutputSettings.replayPath = QString::fromUtf8(obs_data_get_string(output, "replay_path"));
			verticalOutputSettings.replayDurationSeconds =
				int(obs_data_get_int(output, "replay_duration_seconds"));
			verticalOutputSettings.replayAlwaysOn = obs_data_get_bool(output, "replay_always_on");
			verticalOutputSettings.audioTrackMask =
				uint32_t(std::max<int64_t>(1, obs_data_get_int(output, "audio_track_mask")));
			obs_data_release(output);
		}
	}

	EnsureDefaultState();
	if (defaultTransitionDurationMs <= 0)
		defaultTransitionDurationMs = 300;
	emit StateChanged();
}

void SwitchCanvasManager::Reset()
{
	LoadState(nullptr);
}

void SwitchCanvasManager::RemoveRuntimeCanvases()
{
	for (auto &window : previewWindows) {
		if (window)
			window->close();
	}
	previewWindows.clear();

	if (transitionSource) {
		if (obs_source_t *transition = obs_weak_source_get_source(transitionSource)) {
			obs_source_dec_showing(transition);
			obs_source_dec_active(transition);
			obs_source_release(transition);
		}
		obs_weak_source_release(transitionSource);
		transitionSource = nullptr;
	}
	activeTransitionName.clear();

	if (verticalCanvas) {
		obs_canvas_set_channel(verticalCanvas, 0, nullptr);
		obs_frontend_remove_canvas(verticalCanvas);
		obs_canvas_release(verticalCanvas);
		verticalCanvas = nullptr;
	}
}

void SwitchCanvasManager::ReleaseRuntimeReferencesForShutdown()
{
	SuspendCanvasRendering();
	for (auto &window : previewWindows) {
		if (window)
			window->close();
	}
	previewWindows.clear();

	if (transitionSource) {
		if (obs_source_t *transition = obs_weak_source_get_source(transitionSource)) {
			obs_source_dec_showing(transition);
			obs_source_dec_active(transition);
			obs_source_release(transition);
		}
		obs_weak_source_release(transitionSource);
		transitionSource = nullptr;
	}
	activeTransitionName.clear();

	if (verticalCanvas) {
		obs_canvas_set_channel(verticalCanvas, 0, nullptr);
		obs_canvas_release(verticalCanvas);
		verticalCanvas = nullptr;
	}

	ReleaseTransitions();
}

void SwitchCanvasManager::UpdateCachedOutputState()
{
	if (!frontendActive)
		return;

	cachedStreamingActive = obs_frontend_streaming_active();
	cachedRecordingActive = obs_frontend_recording_active();
	cachedRecordingPaused = obs_frontend_recording_paused();
	cachedReplayActive = obs_frontend_replay_buffer_active();
	cachedVirtualCameraActive = obs_frontend_virtualcam_active();
}

obs_canvas_t *SwitchCanvasManager::CanvasById(const QString &canvasId) const
{
	if (canvasId == VerticalCanvasId())
		return verticalCanvas;
	return nullptr;
}

obs_data_t *SwitchCanvasManager::BuildStateData() const
{
	obs_data_t *root = SaveState();
	const bool streamingActive = frontendActive ? obs_frontend_streaming_active() : cachedStreamingActive;
	const bool recordingActive = frontendActive ? obs_frontend_recording_active() : cachedRecordingActive;
	const bool recordingPaused = frontendActive ? obs_frontend_recording_paused() : cachedRecordingPaused;
	const bool replayActive = frontendActive ? obs_frontend_replay_buffer_active() : cachedReplayActive;
	const bool virtualCameraActive = frontendActive ? obs_frontend_virtualcam_active() : cachedVirtualCameraActive;
	obs_data_set_bool(root, "streaming_active", streamingActive);
	obs_data_set_bool(root, "recording_active", recordingActive);
	obs_data_set_bool(root, "recording_paused", recordingPaused);
	obs_data_set_bool(root, "replay_active", replayActive);
	obs_data_set_bool(root, "virtual_camera_active", virtualCameraActive);

	obs_data_array_t *canvasArray = obs_data_array_create();
	for (const auto &canvas : Canvases()) {
		obs_data_t *entry = obs_data_create();
		obs_data_set_string(entry, "id", canvas.id.toUtf8().constData());
		obs_data_set_string(entry, "name", canvas.name.toUtf8().constData());
		obs_data_set_string(entry, "active_scene_name", canvas.activeSceneName.toUtf8().constData());

		obs_data_array_t *sceneArray = obs_data_array_create();
		for (const auto &scene : ScenesForCanvas(canvas.id)) {
			obs_data_t *sceneEntry = obs_data_create();
			obs_data_set_string(sceneEntry, "uuid", scene.uuid.toUtf8().constData());
			obs_data_set_string(sceneEntry, "name", scene.name.toUtf8().constData());
			obs_data_array_push_back(sceneArray, sceneEntry);
			obs_data_release(sceneEntry);
		}
		obs_data_set_array(entry, "scenes", sceneArray);
		obs_data_array_release(sceneArray);

		obs_data_array_push_back(canvasArray, entry);
		obs_data_release(entry);
	}

	obs_data_set_array(root, "canvas_state", canvasArray);
	obs_data_array_release(canvasArray);

	obs_data_t *output = obs_data_create();
	obs_data_set_bool(output, "follow_main_streaming", verticalOutputSettings.followMainStreaming);
	obs_data_set_bool(output, "follow_main_recording", verticalOutputSettings.followMainRecording);
	obs_data_set_bool(output, "follow_main_replay", verticalOutputSettings.followMainReplay);
	obs_data_set_bool(output, "follow_main_virtual_camera", verticalOutputSettings.followMainVirtualCamera);
	obs_data_set_string(output, "stream_encoder_id", verticalOutputSettings.streamEncoderId.toUtf8().constData());
	obs_data_set_string(output, "record_encoder_id", verticalOutputSettings.recordEncoderId.toUtf8().constData());
	obs_data_set_int(output, "streaming_video_bitrate_kbps", verticalOutputSettings.streamingVideoBitrateKbps);
	obs_data_set_int(output, "recording_video_bitrate_kbps", verticalOutputSettings.recordingVideoBitrateKbps);
	obs_data_set_int(output, "audio_bitrate_kbps", verticalOutputSettings.audioBitrateKbps);
	obs_data_set_bool(output, "stream_delay_enabled", verticalOutputSettings.streamDelayEnabled);
	obs_data_set_int(output, "stream_delay_ms", verticalOutputSettings.streamDelayMs);
	obs_data_set_bool(output, "stream_delay_preserve", verticalOutputSettings.streamDelayPreserve);
	obs_data_set_string(output, "recording_path", verticalOutputSettings.recordingPath.toUtf8().constData());
	obs_data_set_string(output, "recording_filename_pattern",
			    verticalOutputSettings.recordingFilenamePattern.toUtf8().constData());
	obs_data_set_bool(output, "recording_split_enabled", verticalOutputSettings.recordingSplitEnabled);
	obs_data_set_int(output, "recording_split_minutes", verticalOutputSettings.recordingSplitMinutes);
	obs_data_set_string(output, "replay_path", verticalOutputSettings.replayPath.toUtf8().constData());
	obs_data_set_int(output, "replay_duration_seconds", verticalOutputSettings.replayDurationSeconds);
	obs_data_set_bool(output, "replay_always_on", verticalOutputSettings.replayAlwaysOn);
	obs_data_set_int(output, "audio_track_mask", int(verticalOutputSettings.audioTrackMask));
	obs_data_set_obj(root, "vertical_output", output);
	obs_data_release(output);
	return root;
}

void SwitchCanvasManager::HandleFrontendEvent(enum obs_frontend_event event)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		frontendActive = true;
		UpdateCachedOutputState();
		emit StateChanged();
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
	case OBS_FRONTEND_EVENT_CANVAS_ADDED:
	case OBS_FRONTEND_EVENT_CANVAS_REMOVED:
		if (suppressCanvasRender) {
			blog(LOG_INFO, "[Switch] Skipping frontend event %d during canvas mutation", int(event));
			break;
		}
		SyncLinkedSceneFromProgram();
		emit StateChanged();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTING:
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
	case OBS_FRONTEND_EVENT_RECORDING_STARTING:
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
	case OBS_FRONTEND_EVENT_RECORDING_STOPPING:
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
	case OBS_FRONTEND_EVENT_RECORDING_PAUSED:
	case OBS_FRONTEND_EVENT_RECORDING_UNPAUSED:
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTING:
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED:
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPING:
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPED:
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED:
	case OBS_FRONTEND_EVENT_VIRTUALCAM_STARTED:
	case OBS_FRONTEND_EVENT_VIRTUALCAM_STOPPED:
		UpdateCachedOutputState();
		break;
	case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
	case OBS_FRONTEND_EVENT_EXIT:
		frontendActive = false;
		ReleaseRuntimeReferencesForShutdown();
		break;
	default:
		break;
	}
}

void SwitchCanvasManager::HandleSourceRemoved(obs_source_t *source)
{
	if (!source)
		return;

	const QString sourceUuid = QString::fromUtf8(obs_source_get_uuid(source));
	const QString sourceName = QString::fromUtf8(obs_source_get_name(source));

	links.erase(std::remove_if(links.begin(), links.end(),
				   [&](const SwitchCanvasLink &link) {
					   return link.mainSceneUuid == sourceUuid || link.targetSceneUuid == sourceUuid ||
						  link.targetSceneName == sourceName;
				   }),
		    links.end());

	if (verticalSceneNames.contains(sourceName)) {
		if (!removedVerticalSceneNames.contains(sourceName, Qt::CaseInsensitive))
			removedVerticalSceneNames.push_back(sourceName);
		verticalSceneNames.removeAll(sourceName);
		RebuildVerticalSceneCache();
	}

	const int verticalIndex = FindCanvasIndex(VerticalCanvasId());
	if (verticalIndex >= 0 &&
	    ((!sourceUuid.isEmpty() && canvases[verticalIndex].activeSceneUuid == sourceUuid) ||
	     canvases[verticalIndex].activeSceneName == sourceName)) {
		canvases[verticalIndex].activeSceneUuid.clear();
		canvases[verticalIndex].activeSceneName.clear();
		QPointer<SwitchCanvasManager> self(this);
		QTimer::singleShot(0, this, [self]() {
			if (!self)
				return;
			if (!self->EnsureVerticalCanvas())
				return;
			self->EnsureVerticalSceneList();
			const auto scenes = self->ScenesForCanvas(self->VerticalCanvasId());
			if (!scenes.isEmpty())
				self->SetCanvasActiveScene(self->VerticalCanvasId(),
						       !scenes.front().uuid.isEmpty() ? scenes.front().uuid
										      : scenes.front().name);
		});
	}

	emit StateChanged();
}
