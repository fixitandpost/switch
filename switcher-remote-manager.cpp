#include "switcher-remote-manager.hpp"

#include <QDateTime>
#include <QDataStream>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <obs-module.h>
#include <util/platform.h>

#include "switcher-workspace.hpp"

namespace {
#ifdef _WIN32
constexpr auto kRemoteHelperRelativePath = "remote/switch-remote-helper.exe";
#else
constexpr auto kRemoteHelperRelativePath = "remote/switch-remote-helper";
#endif
constexpr qint64 kRemoteSocketHighWaterMarkBytes = 4 * 1024 * 1024;
constexpr qint64 kRemoteSocketLowWaterMarkBytes = 1 * 1024 * 1024;
constexpr auto kRemoteBackpressureDetail = "Remote helper is backlogged, dropping frames";

struct SourceTreeSearch {
	obs_source_t *target = nullptr;
	bool found = false;
};

QSize NormalizeRemoteSize(const QSize &size)
{
	if (size.width() >= 1920 || size.height() >= 1080)
		return QSize(1920, 1080);
	return QSize(1280, 720);
}

int NormalizeRemoteFps(int fps)
{
	return std::clamp(fps, 1, 30);
}

QString GenerateRemoteToken()
{
	char *uuid = os_generate_uuid();
	if (!uuid)
		return QStringLiteral("switch");

	QString token = QString::fromUtf8(uuid);
	bfree(uuid);
	token.remove('{').remove('}').remove('-');
	return token.left(20);
}

void LogHelperOutput(const QByteArray &data, int level)
{
	const QList<QByteArray> lines = data.split('\n');
	for (const auto &line : lines) {
		const QByteArray trimmed = line.trimmed();
		if (!trimmed.isEmpty())
			blog(level, "[Switch Remote] %s", trimmed.constData());
	}
}
} // namespace

extern void SwitcherEmitVendorEvent(const char *eventName, obs_data_t *data);

SwitcherRemoteManager *SwitcherRemoteManager::Instance()
{
	static auto *instance = new SwitcherRemoteManager();
	return instance;
}

SwitcherRemoteManager::SwitcherRemoteManager(QObject *parent) : QObject(parent), renderTimer(new QTimer(this))
{
	renderTimer->setTimerType(Qt::PreciseTimer);
	connect(renderTimer, &QTimer::timeout, this, &SwitcherRemoteManager::RenderAndSendFrame);
	token = GenerateRemoteToken();
}

void SwitcherRemoteManager::SetWorkspace(SwitcherWorkspaceDock *workspaceDock)
{
	if (workspace == workspaceDock)
		return;

	if (workspace)
		disconnect(workspace, nullptr, this, nullptr);

	workspace = workspaceDock;
	if (workspace)
		connect(workspace, &SwitcherWorkspaceDock::WorkspaceStateChanged, this, &SwitcherRemoteManager::WorkspaceStateChanged);

	WorkspaceStateChanged();
	if (enabled && autoStart)
		QMetaObject::invokeMethod(this, &SwitcherRemoteManager::StartIfNeeded, Qt::QueuedConnection);
}

obs_data_t *SwitcherRemoteManager::SaveState() const
{
	obs_data_t *data = obs_data_create();
	obs_data_set_bool(data, "enabled", enabled);
	obs_data_set_bool(data, "autostart", autoStart);
	obs_data_set_string(data, "token", token.toUtf8().constData());
	obs_data_set_int(data, "render_width", renderSize.width());
	obs_data_set_int(data, "render_height", renderSize.height());
	obs_data_set_int(data, "fps", targetFps);
	return data;
}

void SwitcherRemoteManager::LoadState(obs_data_t *data)
{
	const bool wasEnabled = enabled;

	enabled = data ? obs_data_get_bool(data, "enabled") : false;
	autoStart = data ? obs_data_get_bool(data, "autostart") : false;

	QString savedToken = data ? QString::fromUtf8(obs_data_get_string(data, "token")) : QString();
	if (!savedToken.isEmpty())
		token = savedToken;
	if (token.isEmpty())
		token = GenerateRemoteToken();

	const int width = data ? static_cast<int>(obs_data_get_int(data, "render_width")) : renderSize.width();
	const int height = data ? static_cast<int>(obs_data_get_int(data, "render_height")) : renderSize.height();
	renderSize = NormalizeRemoteSize(QSize(width, height));
	targetFps = NormalizeRemoteFps(data ? static_cast<int>(obs_data_get_int(data, "fps")) : targetFps);

	if (!enabled) {
		StopHelper(false);
		UpdateStatus(Status::Disabled);
	} else if (autoStart) {
		QMetaObject::invokeMethod(this, &SwitcherRemoteManager::StartIfNeeded, Qt::QueuedConnection);
	} else if (!wasEnabled) {
		UpdateStatus(Status::Disabled, QStringLiteral("Remote enabled but not started"));
	}

	UpdateRenderTimer();
	emit StateChanged();
}

void SwitcherRemoteManager::HandleFrontendEvent(enum obs_frontend_event event)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		frontendShuttingDown = false;
		SyncSelectedSlotFromObs();
		SendStateSnapshot("PreviewProgramChanged");
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
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
		if (frontendShuttingDown)
			break;
			SyncSelectedSlotFromObs();
			SendStateSnapshot("PreviewProgramChanged");
			break;
		case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
			frontendShuttingDown = true;
			selectedSlotIndex = -1;
			Shutdown();
			break;
		case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
			frontendShuttingDown = true;
			selectedSlotIndex = -1;
			break;
	case OBS_FRONTEND_EVENT_EXIT:
		frontendShuttingDown = true;
		Shutdown();
		break;
	default:
		break;
	}
}

void SwitcherRemoteManager::HandleSourceRemoved(obs_source_t *source)
{
	if (!workspace || !source)
		return;

	const OBSSource selected = SelectedSource();
	if (selected && selected.Get() == source)
		selectedSlotIndex = -1;

	SendStateSnapshot("SlotsChanged");
}

void SwitcherRemoteManager::Shutdown()
{
	renderTimer->stop();
	StopHelper(true);
	if (server) {
		server->close();
		server->deleteLater();
		server = nullptr;
		controlPort = 0;
	}
	renderer.Reset();
}

void SwitcherRemoteManager::SetEnabled(bool value)
{
	if (enabled == value)
		return;

	enabled = value;
	if (!enabled) {
		StopHelper(false);
		UpdateStatus(Status::Disabled);
	} else {
		autoStart = true;
		StartHelper();
	}

	emit StateChanged();
}

void SwitcherRemoteManager::SetAutoStart(bool value)
{
	if (autoStart == value)
		return;

	autoStart = value;
	if (enabled && autoStart)
		StartIfNeeded();

	emit StateChanged();
}

void SwitcherRemoteManager::SetRenderSize(const QSize &size)
{
	const QSize normalized = NormalizeRemoteSize(size);
	if (renderSize == normalized)
		return;

	renderSize = normalized;
	renderer.Reset();
	SendStateSnapshot("LayoutChanged");
}

void SwitcherRemoteManager::SetTargetFps(int fps)
{
	const int normalized = NormalizeRemoteFps(fps);
	if (targetFps == normalized)
		return;

	targetFps = normalized;
	UpdateRenderTimer();
	SendStateSnapshot("HealthChanged");
}

void SwitcherRemoteManager::RegenerateToken()
{
	token = GenerateRemoteToken();
	if (enabled)
		Restart();
	else
		emit StateChanged();
}

QString SwitcherRemoteManager::Url() const
{
	if (!enabled)
		return QString();
	return QStringLiteral("http://%1:%2/?token=%3").arg(DetectLanAddress()).arg(httpPort).arg(token);
}

QString SwitcherRemoteManager::StatusText() const
{
	switch (status) {
	case Status::Disabled:
		return enabled ? QStringLiteral("Remote ready to start") : QStringLiteral("Remote disabled");
	case Status::Starting:
		return statusDetail.isEmpty() ? QStringLiteral("Starting remote helper") : statusDetail;
	case Status::Running:
		return statusDetail.isEmpty() ? QStringLiteral("Remote available on LAN") : statusDetail;
	case Status::Error:
		return statusDetail.isEmpty() ? QStringLiteral("Remote error") : statusDetail;
	}

	return QStringLiteral("Remote disabled");
}

bool SwitcherRemoteManager::SelectPreviewSlot(int slotIndex)
{
	if (!workspace || slotIndex < 0 || slotIndex >= workspace->VisibleSlotCount())
		return false;

	const OBSSource source = workspace->SlotSource(slotIndex);
	if (!source)
		return false;

	selectedSlotIndex = slotIndex;
	if (obs_source_is_scene(source) && obs_frontend_preview_program_mode_active())
		obs_frontend_set_current_preview_scene(source);

	SendStateSnapshot("PreviewProgramChanged");
	return true;
}

bool SwitcherRemoteManager::Cut()
{
	return TakeSelectedSlot(false);
}

bool SwitcherRemoteManager::AutoTransition()
{
	return TakeSelectedSlot(true);
}

obs_data_t *SwitcherRemoteManager::BuildRemoteStateData() const
{
	const QByteArray json = QJsonDocument(BuildStateJson()).toJson(QJsonDocument::Compact);
	return obs_data_create_from_json(json.constData());
}

void SwitcherRemoteManager::WorkspaceStateChanged()
{
	if (workspace && selectedSlotIndex >= workspace->VisibleSlotCount())
		selectedSlotIndex = workspace->VisibleSlotCount() > 0 ? workspace->VisibleSlotCount() - 1 : -1;
	if (workspace && selectedSlotIndex >= 0 && !workspace->SlotSource(selectedSlotIndex))
		selectedSlotIndex = -1;

	SendStateSnapshot("SlotsChanged");
}

void SwitcherRemoteManager::StartIfNeeded()
{
	if (!enabled || !autoStart)
		return;
	if (helperProcess && helperProcess->state() != QProcess::NotRunning)
		return;

	StartHelper();
}

void SwitcherRemoteManager::Stop()
{
	StopHelper(true);
}

void SwitcherRemoteManager::RenderAndSendFrame()
{
	if (!enabled || !socket || !workspace)
		return;
	if (socket->state() != QAbstractSocket::ConnectedState)
		return;

	const qint64 queuedBytes = socket->bytesToWrite();
	if (queuedBytes >= kRemoteSocketHighWaterMarkBytes) {
		if (!frameBackpressureActive) {
			frameBackpressureActive = true;
			UpdateStatus(Status::Running, QString::fromLatin1(kRemoteBackpressureDetail));
		}
		return;
	}

	if (frameBackpressureActive && queuedBytes <= kRemoteSocketLowWaterMarkBytes) {
		frameBackpressureActive = false;
		UpdateStatus(Status::Running, QStringLiteral("Remote available at %1").arg(Url()));
	}

	std::vector<SwitcherRemoteRenderSlot> renderSlots;
	const int visibleSlots = workspace->VisibleSlotCount();
	renderSlots.reserve(static_cast<size_t>(visibleSlots));
	for (int index = 0; index < visibleSlots; index++)
		renderSlots.push_back({index, workspace->SlotSource(index), workspace->SlotTitle(index)});

	obs_source_t *previewSource = nullptr;
	if (obs_frontend_preview_program_mode_active())
		previewSource = obs_frontend_get_current_preview_scene();
	obs_source_t *programSource = obs_frontend_get_current_scene();

	QString error;
	const QByteArray frame = renderer.RenderJpeg(renderSlots, selectedSlotIndex, renderSize,
						     obs_frontend_preview_program_mode_active(), previewSource, programSource, &error);

	if (previewSource)
		obs_source_release(previewSource);
	if (programSource)
		obs_source_release(programSource);

	if (frame.isEmpty()) {
		if (!error.isEmpty())
			UpdateStatus(Status::Error, error);
		return;
	}

	QJsonObject message{
		{"type", QStringLiteral("frame")},
		{"format", QStringLiteral("image/jpeg")},
		{"width", renderSize.width()},
		{"height", renderSize.height()},
		{"timestampMs", static_cast<qint64>(QDateTime::currentMSecsSinceEpoch())},
	};
	SendMessage(message, frame);
}

void SwitcherRemoteManager::AcceptHelperConnection()
{
	if (!server)
		return;

	if (socket) {
		socket->disconnect(this);
		socket->deleteLater();
		socket = nullptr;
	}

	socket = server->nextPendingConnection();
	if (!socket)
		return;

	connect(socket, &QTcpSocket::readyRead, this, &SwitcherRemoteManager::ReadHelperMessage);
	connect(socket, &QTcpSocket::disconnected, this, &SwitcherRemoteManager::HelperDisconnected);

	frameBackpressureActive = false;
	UpdateStatus(Status::Running, QStringLiteral("Remote available at %1").arg(Url()));
	UpdateRenderTimer();
	SendStateSnapshot();
}

void SwitcherRemoteManager::ReadHelperMessage()
{
	if (!socket)
		return;

	incomingBuffer.append(socket->readAll());
	while (incomingBuffer.size() >= 8) {
		QByteArray header = incomingBuffer.left(8);
		QDataStream sizeStream(&header, QIODevice::ReadOnly);
		sizeStream.setByteOrder(QDataStream::BigEndian);

		quint32 jsonSize = 0;
		quint32 payloadSize = 0;
		sizeStream >> jsonSize >> payloadSize;

		const int totalSize = 8 + static_cast<int>(jsonSize) + static_cast<int>(payloadSize);
		if (incomingBuffer.size() < totalSize)
			return;

		const QByteArray jsonBytes = incomingBuffer.mid(8, static_cast<int>(jsonSize));
		incomingBuffer.remove(0, totalSize);

		const QJsonDocument document = QJsonDocument::fromJson(jsonBytes);
		if (!document.isObject())
			continue;

		ProcessCommand(document.object());
	}
}

void SwitcherRemoteManager::HelperDisconnected()
{
	if (socket) {
		socket->deleteLater();
		socket = nullptr;
	}

	frameBackpressureActive = false;
	UpdateRenderTimer();
	if (enabled && helperProcess && helperProcess->state() != QProcess::NotRunning)
		UpdateStatus(Status::Starting, QStringLiteral("Remote helper disconnected, waiting for reconnect"));
}

void SwitcherRemoteManager::HelperProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
	UNUSED_PARAMETER(exitCode);
	if (helperProcess) {
		helperProcess->deleteLater();
		helperProcess = nullptr;
	}

	if (socket) {
		socket->deleteLater();
		socket = nullptr;
	}

	UpdateRenderTimer();

	if (!enabled || helperStopRequested) {
		helperStopRequested = false;
		if (!enabled)
			UpdateStatus(Status::Disabled);
		return;
	}

	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	while (!restartTimestamps.isEmpty() && now - restartTimestamps.front() > 60000)
		restartTimestamps.removeFirst();
	restartTimestamps.push_back(now);

	if (restartTimestamps.size() > 3) {
		UpdateStatus(Status::Error, QStringLiteral("Remote helper crashed repeatedly"));
		return;
	}

	const QString detail = exitStatus == QProcess::CrashExit ? QStringLiteral("Remote helper crashed, restarting")
								       : QStringLiteral("Remote helper exited, restarting");
	UpdateStatus(Status::Starting, detail);
	QTimer::singleShot(1000, this, &SwitcherRemoteManager::StartHelper);
}

void SwitcherRemoteManager::HelperProcessReadyReadStdout()
{
	if (helperProcess)
		LogHelperOutput(helperProcess->readAllStandardOutput(), LOG_INFO);
}

void SwitcherRemoteManager::HelperProcessReadyReadStderr()
{
	if (helperProcess)
		LogHelperOutput(helperProcess->readAllStandardError(), LOG_WARNING);
}

void SwitcherRemoteManager::HelperProcessError(QProcess::ProcessError error)
{
	UNUSED_PARAMETER(error);
	if (helperStopRequested)
		return;
	UpdateStatus(Status::Error, QStringLiteral("Remote helper failed to start"));
}

void SwitcherRemoteManager::EnsureServer()
{
	if (server)
		return;

	server = new QTcpServer(this);
	if (!server->listen(QHostAddress::LocalHost, 0)) {
		UpdateStatus(Status::Error, QStringLiteral("Unable to open local remote control socket"));
		server->deleteLater();
		server = nullptr;
		return;
	}

	controlPort = server->serverPort();
	connect(server, &QTcpServer::newConnection, this, &SwitcherRemoteManager::AcceptHelperConnection);
}

void SwitcherRemoteManager::StartHelper()
{
	if (!enabled)
		return;

	EnsureServer();
	if (!server)
		return;
	if (helperProcess) {
		if (helperProcess->state() != QProcess::NotRunning)
			return;
		helperProcess->deleteLater();
		helperProcess = nullptr;
	}

	const QString helperPath = HelperPath();
	if (helperPath.isEmpty() || !QFileInfo::exists(helperPath)) {
		UpdateStatus(Status::Error, QStringLiteral("Bundled remote helper is missing"));
		return;
	}

	helperStopRequested = false;
	helperProcess = new QProcess(this);
	helperProcess->setProgram(helperPath);
	helperProcess->setArguments({
		QStringLiteral("--control-port"),
		QString::number(controlPort),
		QStringLiteral("--http-port"),
		QString::number(httpPort),
		QStringLiteral("--token"),
		token,
	});

	connect(helperProcess, &QProcess::finished, this, &SwitcherRemoteManager::HelperProcessFinished);
	connect(helperProcess, &QProcess::readyReadStandardOutput, this, &SwitcherRemoteManager::HelperProcessReadyReadStdout);
	connect(helperProcess, &QProcess::readyReadStandardError, this, &SwitcherRemoteManager::HelperProcessReadyReadStderr);
	connect(helperProcess, &QProcess::errorOccurred, this, &SwitcherRemoteManager::HelperProcessError);

	UpdateStatus(Status::Starting, QStringLiteral("Starting remote helper"));
	helperProcess->start();
}

void SwitcherRemoteManager::StopHelper(bool keepEnabled)
{
	helperStopRequested = true;
	renderTimer->stop();
	frameBackpressureActive = false;

	if (socket) {
		socket->disconnect(this);
		socket->close();
		socket->deleteLater();
		socket = nullptr;
	}

	if (helperProcess) {
		helperProcess->terminate();
		if (!helperProcess->waitForFinished(3000))
			helperProcess->kill();
	}

	if (!keepEnabled)
		enabled = false;
}

void SwitcherRemoteManager::UpdateStatus(Status nextStatus, const QString &detail)
{
	status = nextStatus;
	statusDetail = detail;
	emit StateChanged();
}

void SwitcherRemoteManager::UpdateRenderTimer()
{
	if (!enabled || !socket || targetFps <= 0) {
		renderTimer->stop();
		return;
	}

	renderTimer->start(std::max(1, 1000 / targetFps));
}

void SwitcherRemoteManager::SendStateSnapshot(const char *eventName)
{
	const QJsonObject state = BuildStateJson();

	if (socket) {
		QJsonObject message{
			{"type", QStringLiteral("state")},
			{"event", QString::fromUtf8(eventName)},
			{"state", state},
		};
		SendMessage(message);
	}

	obs_data_t *data = obs_data_create_from_json(QJsonDocument(state).toJson(QJsonDocument::Compact).constData());
	SwitcherEmitVendorEvent(eventName, data);
	obs_data_release(data);
	emit StateChanged();
}

void SwitcherRemoteManager::SendMessage(const QJsonObject &message, const QByteArray &payload)
{
	if (!socket)
		return;
	if (socket->state() != QAbstractSocket::ConnectedState)
		return;

	const QString type = message.value(QStringLiteral("type")).toString();
	const bool isFrameMessage = type == QLatin1String("frame");
	const qint64 queuedBytes = socket->bytesToWrite();

	if (isFrameMessage && queuedBytes >= kRemoteSocketHighWaterMarkBytes) {
		if (!frameBackpressureActive) {
			frameBackpressureActive = true;
			UpdateStatus(Status::Running, QString::fromLatin1(kRemoteBackpressureDetail));
		}
		return;
	}

	if (frameBackpressureActive && queuedBytes <= kRemoteSocketLowWaterMarkBytes) {
		frameBackpressureActive = false;
		UpdateStatus(Status::Running, QStringLiteral("Remote available at %1").arg(Url()));
	}

	const QByteArray json = QJsonDocument(message).toJson(QJsonDocument::Compact);
	QByteArray packet;
	packet.reserve(8 + json.size() + payload.size());

	QDataStream stream(&packet, QIODevice::WriteOnly);
	stream.setByteOrder(QDataStream::BigEndian);
	stream << static_cast<quint32>(json.size()) << static_cast<quint32>(payload.size());
	packet.append(json);
	packet.append(payload);

	socket->write(packet);
	socket->flush();
}

void SwitcherRemoteManager::ProcessCommand(const QJsonObject &message)
{
	const QString type = message.value(QStringLiteral("type")).toString();
	if (type != QStringLiteral("command"))
		return;

	const QString command = message.value(QStringLiteral("command")).toString();
	if (command == QStringLiteral("select_preview_slot")) {
		SelectPreviewSlot(message.value(QStringLiteral("slotIndex")).toInt(-1));
	} else if (command == QStringLiteral("cut")) {
		Cut();
	} else if (command == QStringLiteral("auto")) {
		AutoTransition();
	} else if (command == QStringLiteral("restart_remote")) {
		Restart();
	}
}

bool SwitcherRemoteManager::TakeSelectedSlot(bool triggerAuto)
{
	const OBSSource source = SelectedSource();
	if (!source || !obs_source_is_scene(source))
		return false;

	if (obs_frontend_preview_program_mode_active()) {
		obs_frontend_set_current_preview_scene(source);
		if (triggerAuto)
			obs_frontend_preview_program_trigger_transition();
		else
			obs_frontend_set_current_scene(source);
	} else {
		obs_frontend_set_current_scene(source);
	}

	SendStateSnapshot("PreviewProgramChanged");
	return true;
}

void SwitcherRemoteManager::SyncSelectedSlotFromObs()
{
	if (!workspace)
		return;

	if (!obs_frontend_preview_program_mode_active())
		return;

	obs_source_t *previewSource = obs_frontend_get_current_preview_scene();
	if (!previewSource)
		return;

	for (int index = 0; index < workspace->VisibleSlotCount(); index++) {
		const OBSSource slotSource = workspace->SlotSource(index);
		if (slotSource && SourceInTree(previewSource, slotSource)) {
			selectedSlotIndex = index;
			break;
		}
	}

	obs_source_release(previewSource);
}

bool SwitcherRemoteManager::SourceInTree(obs_source_t *root, obs_source_t *target) const
{
	if (!root || !target)
		return false;
	if (root == target)
		return true;

	SourceTreeSearch search{target, false};
	obs_source_enum_active_tree(
		root,
		[](obs_source_t *parent, obs_source_t *child, void *data) {
			UNUSED_PARAMETER(parent);
			auto *search = static_cast<SourceTreeSearch *>(data);
			if (search->target == child)
				search->found = true;
		},
		&search);

	return search.found;
}

QString SwitcherRemoteManager::DetectLanAddress() const
{
	const auto addresses = QNetworkInterface::allAddresses();
	for (const auto &address : addresses) {
		if (address.protocol() != QAbstractSocket::IPv4Protocol || address.isLoopback())
			continue;

		const quint32 value = address.toIPv4Address();
		const bool privateA = (value & 0xff000000U) == 0x0a000000U;
		const bool privateB = (value & 0xfff00000U) == 0xac100000U;
		const bool privateC = (value & 0xffff0000U) == 0xc0a80000U;
		if (privateA || privateB || privateC)
			return address.toString();
	}

	return QStringLiteral("127.0.0.1");
}

QString SwitcherRemoteManager::HelperPath() const
{
	char *path = obs_module_file(kRemoteHelperRelativePath);
	if (!path)
		return QString();

	QString helperPath = QString::fromUtf8(path);
	bfree(path);
	return helperPath;
}

QJsonObject SwitcherRemoteManager::BuildStateJson() const
{
	const bool frontendReady = !frontendShuttingDown;
	QJsonObject root{
		{"enabled", enabled},
		{"autoStart", autoStart},
		{"status", StatusText()},
		{"statusCode", status == Status::Running ? QStringLiteral("running")
					 : status == Status::Starting  ? QStringLiteral("starting")
					 : status == Status::Error     ? QStringLiteral("error")
								       : QStringLiteral("disabled")},
		{"url", Url()},
		{"httpPort", httpPort},
		{"token", token},
		{"selectedSlotIndex", selectedSlotIndex},
		{"previewProgramMode", frontendReady ? obs_frontend_preview_program_mode_active() : false},
		{"transitionDurationMs", frontendReady ? obs_frontend_get_transition_duration() : 0},
		{"targetFps", targetFps},
		{"renderWidth", renderSize.width()},
		{"renderHeight", renderSize.height()},
		{"helperConnected", socket != nullptr},
	};

	if (!workspace)
		return root;

	const int visibleSlots = workspace->VisibleSlotCount();
	root.insert(QStringLiteral("visibleSlots"), visibleSlots);
	root.insert(QStringLiteral("programSlotIndex"), frontendReady ? ResolveProgramSlotIndex() : -1);
	root.insert(QStringLiteral("previewSlotIndex"), frontendReady ? ResolvePreviewSlotIndex() : -1);

	const auto layout = BuildSwitcherRemoteTileLayout(visibleSlots, renderSize);
	QJsonArray slotEntries;

	obs_source_t *previewSource = nullptr;
	if (frontendReady && obs_frontend_preview_program_mode_active())
		previewSource = obs_frontend_get_current_preview_scene();
	obs_source_t *programSource = frontendReady ? obs_frontend_get_current_scene() : nullptr;

	for (int index = 0; index < visibleSlots; index++) {
		const OBSSource source = workspace->SlotSource(index);
		const bool hasSource = source != nullptr;
		const bool preview =
			frontendReady && hasSource &&
			((obs_frontend_preview_program_mode_active() && SourceInTree(previewSource, source)) ||
			 (!obs_frontend_preview_program_mode_active() && index == selectedSlotIndex));
		const bool program = frontendReady && hasSource && SourceInTree(programSource, source);

		QJsonObject slot{
			{"index", index},
			{"title", workspace->SlotTitle(index)},
			{"hasSource", hasSource},
			{"selected", index == selectedSlotIndex},
			{"preview", preview},
			{"program", program},
			{"canSwitch", hasSource && obs_source_is_scene(source)},
		};

		if (hasSource)
			slot.insert(QStringLiteral("sourceName"), QString::fromUtf8(obs_source_get_name(source)));

		if (index < static_cast<int>(layout.size())) {
			const auto &rect = layout[static_cast<size_t>(index)].normalizedRect;
			slot.insert(QStringLiteral("rect"),
				    QJsonObject{
					    {"x", rect.x()},
					    {"y", rect.y()},
					    {"width", rect.width()},
					    {"height", rect.height()},
				    });
		}

			slotEntries.push_back(slot);
	}

	if (previewSource)
		obs_source_release(previewSource);
	if (programSource)
		obs_source_release(programSource);

	root.insert(QStringLiteral("slots"), slotEntries);
	return root;
}

OBSSource SwitcherRemoteManager::SelectedSource() const
{
	if (!workspace || selectedSlotIndex < 0 || selectedSlotIndex >= workspace->VisibleSlotCount())
		return nullptr;
	return workspace->SlotSource(selectedSlotIndex);
}

int SwitcherRemoteManager::ResolveProgramSlotIndex() const
{
	if (!workspace)
		return -1;

	obs_source_t *programSource = obs_frontend_get_current_scene();
	if (!programSource)
		return -1;

	int result = -1;
	for (int index = 0; index < workspace->VisibleSlotCount(); index++) {
		const OBSSource source = workspace->SlotSource(index);
		if (source && SourceInTree(programSource, source)) {
			result = index;
			break;
		}
	}

	obs_source_release(programSource);
	return result;
}

int SwitcherRemoteManager::ResolvePreviewSlotIndex() const
{
	if (!workspace)
		return -1;
	if (!obs_frontend_preview_program_mode_active())
		return selectedSlotIndex;

	obs_source_t *previewSource = obs_frontend_get_current_preview_scene();
	if (!previewSource)
		return -1;

	int result = -1;
	for (int index = 0; index < workspace->VisibleSlotCount(); index++) {
		const OBSSource source = workspace->SlotSource(index);
		if (source && SourceInTree(previewSource, source)) {
			result = index;
			break;
		}
	}

	obs_source_release(previewSource);
	return result;
}

void SwitcherRemoteManager::Restart()
{
	if (!enabled) {
		emit StateChanged();
		return;
	}

	StopHelper(true);
	QTimer::singleShot(100, this, &SwitcherRemoteManager::StartHelper);
}
