#include "switch-automation-engine.hpp"

#include <QDateTime>
#include <QFileInfo>
#include <QHostAddress>
#include <QHostInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTimer>
#include <QUdpSocket>
#include <QUrl>

#include <obs-module.h>

#include "switch-canvas-manager.hpp"
#include "switch-motion-manager.hpp"
#include "switch-osc.hpp"

namespace {
struct ConditionRegistration {
	SwitchConditionFactoryInfo info;
};

struct ActionRegistration {
	SwitchActionFactoryInfo info;
};

QString LifecycleStateName(SwitchAutomationLifecycleState state)
{
	switch (state) {
	case SwitchAutomationLifecycleState::Loading:
		return QStringLiteral("loading");
	case SwitchAutomationLifecycleState::Running:
		return QStringLiteral("running");
	case SwitchAutomationLifecycleState::SceneCollectionSwitch:
		return QStringLiteral("scene_collection_switch");
	case SwitchAutomationLifecycleState::ShuttingDown:
		return QStringLiteral("shutting_down");
	}
	return QStringLiteral("loading");
}

#define kUnavailableReason "Not implemented in this build yet"

QVector<ConditionRegistration> ConditionRegistry()
{
	return {
		{{QStringLiteral("manual"), QStringLiteral("Manual"), QStringLiteral("base"), QStringLiteral("Core"), true, {}}},
		{{QStringLiteral("timer"), QStringLiteral("Timer"), QStringLiteral("base"), QStringLiteral("Core"), true, {}}},
		{{QStringLiteral("program_scene"), QStringLiteral("Program Scene"), QStringLiteral("base"), QStringLiteral("Scene"), true, {}}},
		{{QStringLiteral("scene"), QStringLiteral("Scene"), QStringLiteral("base"), QStringLiteral("Scene"), true, {}}},
		{{QStringLiteral("recording_state"), QStringLiteral("Recording State"), QStringLiteral("base"), QStringLiteral("OBS"), true, {}}},
		{{QStringLiteral("streaming_state"), QStringLiteral("Streaming State"), QStringLiteral("base"), QStringLiteral("OBS"), true, {}}},
		{{QStringLiteral("replay_state"), QStringLiteral("Replay Buffer State"), QStringLiteral("base"), QStringLiteral("OBS"), true, {}}},
		{{QStringLiteral("virtual_camera_state"), QStringLiteral("Virtual Camera State"), QStringLiteral("base"), QStringLiteral("OBS"), true, {}}},
		{{QStringLiteral("variable"), QStringLiteral("Variable"), QStringLiteral("base"), QStringLiteral("Data"), true, {}}},
		{{QStringLiteral("macro_state"), QStringLiteral("Macro State"), QStringLiteral("base"), QStringLiteral("Data"), true, {}}},
		{{QStringLiteral("file"), QStringLiteral("File"), QStringLiteral("base"), QStringLiteral("System"), true, {}}},
		{{QStringLiteral("process"), QStringLiteral("Process"), QStringLiteral("base"), QStringLiteral("System"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("window"), QStringLiteral("Window"), QStringLiteral("base"), QStringLiteral("System"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("date"), QStringLiteral("Date and Time"), QStringLiteral("base"), QStringLiteral("System"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("audio"), QStringLiteral("Audio"), QStringLiteral("base"), QStringLiteral("OBS"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("clipboard"), QStringLiteral("Clipboard"), QStringLiteral("base"), QStringLiteral("System"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("cursor"), QStringLiteral("Cursor"), QStringLiteral("base"), QStringLiteral("System"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("display"), QStringLiteral("Display"), QStringLiteral("base"), QStringLiteral("System"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("filter"), QStringLiteral("Filter"), QStringLiteral("base"), QStringLiteral("OBS"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("folder"), QStringLiteral("Folder Watch"), QStringLiteral("base"), QStringLiteral("System"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("hotkey"), QStringLiteral("Hotkey"), QStringLiteral("base"), QStringLiteral("OBS"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("idle"), QStringLiteral("Idle"), QStringLiteral("base"), QStringLiteral("System"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("media"), QStringLiteral("Media"), QStringLiteral("base"), QStringLiteral("OBS"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("obs_stats"), QStringLiteral("OBS Statistics"), QStringLiteral("base"), QStringLiteral("OBS"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("plugin_state"), QStringLiteral("Plugin State"), QStringLiteral("base"), QStringLiteral("OBS"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("profile"), QStringLiteral("Profile"), QStringLiteral("base"), QStringLiteral("OBS"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("queue"), QStringLiteral("Queue"), QStringLiteral("base"), QStringLiteral("Data"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("run"), QStringLiteral("Run"), QStringLiteral("base"), QStringLiteral("System"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("scene_item_order"), QStringLiteral("Scene Item Order"), QStringLiteral("base"), QStringLiteral("OBS"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("scene_item_transform"), QStringLiteral("Scene Item Transform"), QStringLiteral("base"), QStringLiteral("OBS"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("scene_item_visibility"), QStringLiteral("Scene Item Visibility"), QStringLiteral("base"), QStringLiteral("OBS"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("slideshow"), QStringLiteral("Slide Show"), QStringLiteral("base"), QStringLiteral("OBS"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("source"), QStringLiteral("Source"), QStringLiteral("base"), QStringLiteral("OBS"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("studio_mode"), QStringLiteral("Studio Mode"), QStringLiteral("base"), QStringLiteral("OBS"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("transition"), QStringLiteral("Transition"), QStringLiteral("base"), QStringLiteral("OBS"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("websocket"), QStringLiteral("Websocket"), QStringLiteral("http"), QStringLiteral("Remote"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("http"), QStringLiteral("HTTP"), QStringLiteral("http"), QStringLiteral("Remote"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("osc_receive"), QStringLiteral("OSC Receive"), QStringLiteral("base"), QStringLiteral("Remote"), true, {}}},
		{{QStringLiteral("motion_target"), QStringLiteral("Motion Target"), QStringLiteral("base"), QStringLiteral("Motion"), true, {}}},
		{{QStringLiteral("midi"), QStringLiteral("MIDI"), QStringLiteral("midi"), QStringLiteral("External"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("mqtt"), QStringLiteral("MQTT"), QStringLiteral("mqtt"), QStringLiteral("External"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("twitch"), QStringLiteral("Twitch"), QStringLiteral("twitch"), QStringLiteral("External"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("usb"), QStringLiteral("USB"), QStringLiteral("usb"), QStringLiteral("External"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("video"), QStringLiteral("Video"), QStringLiteral("video"), QStringLiteral("External"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("openvr"), QStringLiteral("OpenVR"), QStringLiteral("openvr"), QStringLiteral("External"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("stream_deck"), QStringLiteral("Stream Deck"), QStringLiteral("stream-deck"), QStringLiteral("External"), false, QStringLiteral(kUnavailableReason)}},
	};
}

QVector<ActionRegistration> ActionRegistry()
{
	return {
		{{QStringLiteral("switch_program_scene"), QStringLiteral("Switch Program Scene"), QStringLiteral("base"), QStringLiteral("Scene"), true, {}}},
		{{QStringLiteral("switch_preview_scene"), QStringLiteral("Switch Preview Scene"), QStringLiteral("base"), QStringLiteral("Scene"), true, {}}},
		{{QStringLiteral("switch_scene"), QStringLiteral("Switch Scene"), QStringLiteral("base"), QStringLiteral("Scene"), true, {}}},
		{{QStringLiteral("switch_vertical_scene"), QStringLiteral("Switch Vertical Scene"), QStringLiteral("base"), QStringLiteral("Vertical"), true, {}}},
		{{QStringLiteral("open_vertical_window"), QStringLiteral("Open Vertical Window"), QStringLiteral("base"), QStringLiteral("Vertical"), true, {}}},
		{{QStringLiteral("open_vertical_projector"), QStringLiteral("Open Vertical Projector"), QStringLiteral("base"), QStringLiteral("Vertical"), true, {}}},
		{{QStringLiteral("start_recording"), QStringLiteral("Start Recording"), QStringLiteral("base"), QStringLiteral("OBS"), true, {}}},
		{{QStringLiteral("stop_recording"), QStringLiteral("Stop Recording"), QStringLiteral("base"), QStringLiteral("OBS"), true, {}}},
		{{QStringLiteral("pause_recording"), QStringLiteral("Pause Recording"), QStringLiteral("base"), QStringLiteral("OBS"), true, {}}},
		{{QStringLiteral("resume_recording"), QStringLiteral("Resume Recording"), QStringLiteral("base"), QStringLiteral("OBS"), true, {}}},
		{{QStringLiteral("split_recording"), QStringLiteral("Split Recording"), QStringLiteral("base"), QStringLiteral("OBS"), true, {}}},
		{{QStringLiteral("add_recording_chapter"), QStringLiteral("Add Recording Chapter"), QStringLiteral("base"), QStringLiteral("OBS"), true, {}}},
		{{QStringLiteral("start_streaming"), QStringLiteral("Start Streaming"), QStringLiteral("base"), QStringLiteral("OBS"), true, {}}},
		{{QStringLiteral("stop_streaming"), QStringLiteral("Stop Streaming"), QStringLiteral("base"), QStringLiteral("OBS"), true, {}}},
		{{QStringLiteral("start_replay"), QStringLiteral("Start Replay Buffer"), QStringLiteral("base"), QStringLiteral("OBS"), true, {}}},
		{{QStringLiteral("save_replay"), QStringLiteral("Save Replay"), QStringLiteral("base"), QStringLiteral("OBS"), true, {}}},
		{{QStringLiteral("stop_replay"), QStringLiteral("Stop Replay Buffer"), QStringLiteral("base"), QStringLiteral("OBS"), true, {}}},
		{{QStringLiteral("start_virtual_camera"), QStringLiteral("Start Virtual Camera"), QStringLiteral("base"), QStringLiteral("OBS"), true, {}}},
		{{QStringLiteral("stop_virtual_camera"), QStringLiteral("Stop Virtual Camera"), QStringLiteral("base"), QStringLiteral("OBS"), true, {}}},
		{{QStringLiteral("set_variable"), QStringLiteral("Set Variable"), QStringLiteral("base"), QStringLiteral("Data"), true, {}}},
		{{QStringLiteral("remove_variable"), QStringLiteral("Remove Variable"), QStringLiteral("base"), QStringLiteral("Data"), true, {}}},
		{{QStringLiteral("copy_variable"), QStringLiteral("Copy Variable"), QStringLiteral("base"), QStringLiteral("Data"), true, {}}},
		{{QStringLiteral("run_macro"), QStringLiteral("Run Macro"), QStringLiteral("base"), QStringLiteral("Data"), true, {}}},
		{{QStringLiteral("wait"), QStringLiteral("Wait"), QStringLiteral("base"), QStringLiteral("Flow"), true, {}}},
		{{QStringLiteral("http_get"), QStringLiteral("HTTP GET"), QStringLiteral("base"), QStringLiteral("Remote"), true, {}}},
		{{QStringLiteral("http_post"), QStringLiteral("HTTP POST"), QStringLiteral("base"), QStringLiteral("Remote"), true, {}}},
		{{QStringLiteral("queue"), QStringLiteral("Queue"), QStringLiteral("base"), QStringLiteral("Flow"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("random"), QStringLiteral("Random"), QStringLiteral("base"), QStringLiteral("Flow"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("sequence"), QStringLiteral("Sequence"), QStringLiteral("base"), QStringLiteral("Flow"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("projector"), QStringLiteral("Projector"), QStringLiteral("base"), QStringLiteral("OBS"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("scene_collection"), QStringLiteral("Scene Collection"), QStringLiteral("base"), QStringLiteral("OBS"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("source"), QStringLiteral("Source"), QStringLiteral("base"), QStringLiteral("OBS"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("scene_item_visibility"), QStringLiteral("Scene Item Visibility"), QStringLiteral("base"), QStringLiteral("OBS"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("scene_item_transform"), QStringLiteral("Scene Item Transform"), QStringLiteral("base"), QStringLiteral("OBS"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("scene_item_order"), QStringLiteral("Scene Item Order"), QStringLiteral("base"), QStringLiteral("OBS"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("scene_item_lock"), QStringLiteral("Scene Item Lock"), QStringLiteral("base"), QStringLiteral("OBS"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("filter"), QStringLiteral("Filter"), QStringLiteral("base"), QStringLiteral("OBS"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("profile"), QStringLiteral("Profile"), QStringLiteral("base"), QStringLiteral("OBS"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("transition"), QStringLiteral("Transition"), QStringLiteral("base"), QStringLiteral("OBS"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("studio_mode"), QStringLiteral("Studio Mode"), QStringLiteral("base"), QStringLiteral("OBS"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("window"), QStringLiteral("Window"), QStringLiteral("base"), QStringLiteral("System"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("run"), QStringLiteral("Run"), QStringLiteral("base"), QStringLiteral("System"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("audio"), QStringLiteral("Audio"), QStringLiteral("base"), QStringLiteral("OBS"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("clipboard"), QStringLiteral("Clipboard"), QStringLiteral("base"), QStringLiteral("System"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("file"), QStringLiteral("File"), QStringLiteral("base"), QStringLiteral("System"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("media"), QStringLiteral("Media"), QStringLiteral("base"), QStringLiteral("OBS"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("osc"), QStringLiteral("OSC Send"), QStringLiteral("base"), QStringLiteral("Remote"), true, {}}},
		{{QStringLiteral("motion_enable"), QStringLiteral("Enable Motion Profile"), QStringLiteral("base"), QStringLiteral("Motion"), true, {}}},
		{{QStringLiteral("motion_disable"), QStringLiteral("Disable Motion Profile"), QStringLiteral("base"), QStringLiteral("Motion"), true, {}}},
		{{QStringLiteral("plugin_state"), QStringLiteral("Plugin State"), QStringLiteral("base"), QStringLiteral("OBS"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("screenshot"), QStringLiteral("Screenshot"), QStringLiteral("base"), QStringLiteral("OBS"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("system_notification"), QStringLiteral("System Notification"), QStringLiteral("base"), QStringLiteral("System"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("timer"), QStringLiteral("Timer"), QStringLiteral("base"), QStringLiteral("Flow"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("websocket"), QStringLiteral("Websocket"), QStringLiteral("http"), QStringLiteral("Remote"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("midi"), QStringLiteral("MIDI"), QStringLiteral("midi"), QStringLiteral("External"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("mqtt"), QStringLiteral("MQTT"), QStringLiteral("mqtt"), QStringLiteral("External"), false, QStringLiteral(kUnavailableReason)}},
		{{QStringLiteral("twitch"), QStringLiteral("Twitch"), QStringLiteral("twitch"), QStringLiteral("External"), false, QStringLiteral(kUnavailableReason)}},
	};
}

template<typename TRegistration>
QString RegistryNameForId(const QVector<TRegistration> &items, const QString &id)
{
	for (const auto &item : items) {
		if (item.info.id == id)
			return item.info.name;
	}
	return id;
}

template<typename TRegistration>
bool RegistryIsAvailable(const QVector<TRegistration> &items, const QString &id)
{
	for (const auto &item : items) {
		if (item.info.id == id)
			return item.info.available;
	}
	return false;
}

QString StringFromConfig(const QVariantMap &config, const QString &key)
{
	return config.value(key).toString();
}

bool BoolFromConfig(const QVariantMap &config, const QString &key, bool defaultValue = false)
{
	return config.contains(key) ? config.value(key).toBool() : defaultValue;
}

int IntFromConfig(const QVariantMap &config, const QString &key, int defaultValue = 0)
{
	return config.contains(key) ? config.value(key).toInt() : defaultValue;
}

QString ConnectionModeFromConfig(const QVariantMap &config)
{
	const QString mode = StringFromConfig(config, QStringLiteral("mode")).trimmed().toLower();
	return mode.isEmpty() ? QStringLiteral("duplex") : mode;
}

bool ConnectionCanReceive(const QVariantMap &config)
{
	const QString mode = ConnectionModeFromConfig(config);
	return mode == QStringLiteral("receive") || mode == QStringLiteral("duplex");
}

bool ConnectionCanSend(const QVariantMap &config)
{
	const QString mode = ConnectionModeFromConfig(config);
	return mode == QStringLiteral("send") || mode == QStringLiteral("duplex");
}

QHostAddress ListenAddressFromConfig(const QVariantMap &config)
{
	const QString host = StringFromConfig(config, QStringLiteral("listenHost")).trimmed();
	if (host.isEmpty())
		return QHostAddress::AnyIPv4;
	const QHostAddress address(host);
	return address.isNull() ? QHostAddress::AnyIPv4 : address;
}

bool ResolveRemoteEndpoint(const QVariantMap &config, QHostAddress *address, quint16 *port, QString *error)
{
	if (!address || !port)
		return false;

	const QString host = StringFromConfig(config, QStringLiteral("remoteHost")).trimmed();
	const int portValue = IntFromConfig(config, QStringLiteral("remotePort"));
	if (host.isEmpty() || portValue <= 0 || portValue > 65535) {
		if (error)
			*error = QStringLiteral("OSC remote host and port are required");
		return false;
	}

	QHostAddress resolved(host);
	if (resolved.isNull()) {
		const auto info = QHostInfo::fromName(host);
		if (info.error() != QHostInfo::NoError || info.addresses().isEmpty()) {
			if (error)
				*error = QStringLiteral("Unable to resolve OSC host %1").arg(host);
			return false;
		}
		resolved = info.addresses().front();
	}

	*address = resolved;
	*port = quint16(portValue);
	return true;
}

bool FrontendStateValue(bool callbacksReady, bool (*getter)())
{
	return callbacksReady && getter ? getter() : false;
}
} // namespace

QVector<SwitchConditionFactoryInfo> SwitchConditionFactory::Types(bool includeUnavailable)
{
	QVector<SwitchConditionFactoryInfo> values;
	for (const auto &item : ConditionRegistry()) {
		if (!includeUnavailable && !item.info.available)
			continue;
		values.push_back(item.info);
	}
	return values;
}

QString SwitchConditionFactory::NameForId(const QString &id)
{
	return RegistryNameForId(ConditionRegistry(), id);
}

bool SwitchConditionFactory::IsAvailable(const QString &id)
{
	return RegistryIsAvailable(ConditionRegistry(), id);
}

QVector<SwitchActionFactoryInfo> SwitchActionFactory::Types(bool includeUnavailable)
{
	QVector<SwitchActionFactoryInfo> values;
	for (const auto &item : ActionRegistry()) {
		if (!includeUnavailable && !item.info.available)
			continue;
		values.push_back(item.info);
	}
	return values;
}

QString SwitchActionFactory::NameForId(const QString &id)
{
	return RegistryNameForId(ActionRegistry(), id);
}

bool SwitchActionFactory::IsAvailable(const QString &id)
{
	return RegistryIsAvailable(ActionRegistry(), id);
}

SwitchAutomationEngine::SwitchAutomationEngine(SwitchCanvasManager *canvasManager_, QObject *parent)
	: SwitchAutomationEngine(canvasManager_, nullptr, parent)
{
}

SwitchAutomationEngine::SwitchAutomationEngine(SwitchCanvasManager *canvasManager_, SwitchMotionManager *motionManager_,
					       QObject *parent)
	: QObject(parent),
	  canvasManager(canvasManager_),
	  motionManager(motionManager_),
	  evaluationTimer(new QTimer(this)),
	  networkManager(new QNetworkAccessManager(this))
{
	evaluationTimer->setInterval(300);
	connect(evaluationTimer, &QTimer::timeout, this, &SwitchAutomationEngine::EvaluateAll);
	connect(networkManager, &QNetworkAccessManager::finished, this, [this](QNetworkReply *reply) {
		if (!reply)
			return;

		const bool ok = reply->error() == QNetworkReply::NoError;
		AppendEvent(ok ? QStringLiteral("info") : QStringLiteral("error"),
			    QStringLiteral("network"),
			    ok ? QStringLiteral("HTTP request completed")
			       : QStringLiteral("HTTP request failed: %1").arg(reply->errorString()),
			    {}, ok);
		reply->deleteLater();
	});
	SetLifecycleState(SwitchAutomationLifecycleState::Loading);
}

SwitchAutomationEngine::~SwitchAutomationEngine()
{
	StopOscConnections();
}

const SwitchAutomationDocument &SwitchAutomationEngine::Document() const
{
	return document;
}

QVector<SwitchMacroDescriptor> SwitchAutomationEngine::Macros() const
{
	QVector<SwitchMacroDescriptor> values;
	values.reserve(document.macros.size());
	for (const auto &macro : document.macros)
		values.push_back(LegacyDescriptorFromMacro(macro));
	return values;
}

SwitchAutomationMacro SwitchAutomationEngine::MacroDefinitionById(const QString &macroId) const
{
	const int index = FindMacroIndex(macroId);
	return index >= 0 ? document.macros[index] : SwitchAutomationMacro{};
}

SwitchMacroDescriptor SwitchAutomationEngine::MacroById(const QString &macroId) const
{
	const int index = FindMacroIndex(macroId);
	return index >= 0 ? LegacyDescriptorFromMacro(document.macros[index]) : SwitchMacroDescriptor{};
}

QHash<QString, QString> SwitchAutomationEngine::Variables() const
{
	return document.variables;
}

QVector<SwitchAutomationQueue> SwitchAutomationEngine::Queues() const
{
	return document.queues;
}

QVector<SwitchAutomationConnection> SwitchAutomationEngine::Connections() const
{
	return document.connections;
}

QVector<SwitchAutomationEventRecord> SwitchAutomationEngine::EventLog() const
{
	return eventLog;
}

SwitchAutomationLifecycleState SwitchAutomationEngine::LifecycleState() const
{
	return lifecycleState;
}

QString SwitchAutomationEngine::NextMacroName(const QString &suggestedName) const
{
	const QString base = suggestedName.trimmed().isEmpty() ? QStringLiteral("Macro") : suggestedName.trimmed();
	QString candidate = base;
	int suffix = 2;
	while (std::any_of(document.macros.begin(), document.macros.end(),
			   [&](const SwitchAutomationMacro &macro) { return macro.name.compare(candidate, Qt::CaseInsensitive) == 0; })) {
		candidate = QStringLiteral("%1 %2").arg(base).arg(suffix++);
	}
	return candidate;
}

QString SwitchAutomationEngine::CreateMacro(const QString &suggestedName)
{
	SwitchAutomationMacro macro;
	macro.id = SwitchCreateAutomationId(QStringLiteral("macro"));
	macro.name = NextMacroName(suggestedName);
	SwitchAutomationSegment condition;
	condition.id = SwitchCreateAutomationId(QStringLiteral("condition"));
	condition.typeId = QStringLiteral("manual");
	SwitchAutomationSegment action;
	action.id = SwitchCreateAutomationId(QStringLiteral("action"));
	action.typeId = QStringLiteral("switch_program_scene");
	macro.conditions.push_back(condition);
	macro.actions.push_back(action);
	document.macros.push_back(macro);
	AppendEvent(QStringLiteral("info"), QStringLiteral("macro"), QStringLiteral("Created macro %1").arg(macro.name), macro.id);
	emit StateChanged();
	return macro.id;
}

int SwitchAutomationEngine::FindMacroIndex(const QString &macroId) const
{
	for (int index = 0; index < document.macros.size(); ++index) {
		if (document.macros[index].id == macroId)
			return index;
	}
	return -1;
}

int SwitchAutomationEngine::FindConnectionIndex(const QString &connectionId) const
{
	for (int index = 0; index < document.connections.size(); ++index) {
		if (document.connections[index].id == connectionId)
			return index;
	}
	return -1;
}

bool SwitchAutomationEngine::DeleteMacro(const QString &macroId)
{
	const int index = FindMacroIndex(macroId);
	if (index < 0)
		return false;

	AppendEvent(QStringLiteral("info"), QStringLiteral("macro"),
		    QStringLiteral("Deleted macro %1").arg(document.macros[index].name), macroId);
	document.macros.removeAt(index);
	document.dockPresets.erase(std::remove_if(document.dockPresets.begin(), document.dockPresets.end(),
						  [&](const SwitchAutomationDockPreset &preset) { return preset.macroId == macroId; }),
				   document.dockPresets.end());
	emit StateChanged();
	return true;
}

SwitchMacroDescriptor SwitchAutomationEngine::LegacyDescriptorFromMacro(const SwitchAutomationMacro &macro) const
{
	SwitchMacroDescriptor descriptor;
	descriptor.id = macro.id;
	descriptor.name = macro.name;
	descriptor.group = macro.group;
	descriptor.enabled = macro.enabled;
	descriptor.paused = macro.paused;
	descriptor.runMode = macro.runPolicy;
	descriptor.intervalMs = macro.repeatIntervalMs;
	descriptor.lastConditionMatched = macro.lastConditionMatched;
	descriptor.lastExecutionMs = macro.lastExecutionMs;

	if (!macro.conditions.isEmpty()) {
		const auto &condition = macro.conditions.front();
		descriptor.triggerType = condition.typeId;
		descriptor.desiredState = BoolFromConfig(condition.config, QStringLiteral("desiredState"), true);
		descriptor.triggerConnectionId = StringFromConfig(condition.config, QStringLiteral("connectionId"));
		descriptor.triggerConnectionName = StringFromConfig(condition.config, QStringLiteral("connectionName"));
		descriptor.triggerSceneUuid = StringFromConfig(condition.config, QStringLiteral("sceneUuid"));
		descriptor.triggerSceneName = StringFromConfig(condition.config, QStringLiteral("sceneName"));
		descriptor.triggerValueKey = StringFromConfig(condition.config, QStringLiteral("valueKey"));
		descriptor.triggerValue = StringFromConfig(condition.config, QStringLiteral("value"));
		if (const int intervalMs = IntFromConfig(condition.config, QStringLiteral("intervalMs")); intervalMs > 0)
			descriptor.intervalMs = intervalMs;
	}

	if (!macro.actions.isEmpty()) {
		const auto &action = macro.actions.front();
		descriptor.actionType = action.typeId;
		descriptor.actionConnectionId = StringFromConfig(action.config, QStringLiteral("connectionId"));
		descriptor.actionConnectionName = StringFromConfig(action.config, QStringLiteral("connectionName"));
		descriptor.actionSceneUuid = StringFromConfig(action.config, QStringLiteral("sceneUuid"));
		descriptor.actionSceneName = StringFromConfig(action.config, QStringLiteral("sceneName"));
		descriptor.actionValueKey = StringFromConfig(action.config, QStringLiteral("valueKey"));
		descriptor.actionValue = StringFromConfig(action.config, QStringLiteral("value"));
		descriptor.actionDelayMs = IntFromConfig(action.config, QStringLiteral("delayMs"));
	}

	return descriptor;
}

void SwitchAutomationEngine::ApplyLegacyDescriptorToMacro(SwitchAutomationMacro &macro, const SwitchMacroDescriptor &descriptor) const
{
	macro.id = descriptor.id;
	macro.name = descriptor.name.trimmed().isEmpty() ? QStringLiteral("Macro") : descriptor.name.trimmed();
	macro.group = descriptor.group;
	macro.enabled = descriptor.enabled;
	macro.paused = descriptor.paused;
	macro.runPolicy = descriptor.runMode.trimmed().isEmpty() ? QStringLiteral("on_change") : descriptor.runMode;
	macro.repeatIntervalMs = std::max(250, descriptor.intervalMs);
	macro.lastConditionMatched = descriptor.lastConditionMatched;
	macro.lastExecutionMs = descriptor.lastExecutionMs;

	if (macro.conditions.isEmpty()) {
		SwitchAutomationSegment segment;
		segment.id = SwitchCreateAutomationId(QStringLiteral("condition"));
		macro.conditions.push_back(segment);
	}
	if (macro.actions.isEmpty()) {
		SwitchAutomationSegment segment;
		segment.id = SwitchCreateAutomationId(QStringLiteral("action"));
		macro.actions.push_back(segment);
	}

	auto &condition = macro.conditions.front();
	condition.typeId = descriptor.triggerType.trimmed().isEmpty() ? QStringLiteral("manual") : descriptor.triggerType;
	condition.config.insert(QStringLiteral("desiredState"), descriptor.desiredState);
	condition.config.insert(QStringLiteral("connectionId"), descriptor.triggerConnectionId);
	condition.config.insert(QStringLiteral("connectionName"), descriptor.triggerConnectionName);
	condition.config.insert(QStringLiteral("sceneUuid"), descriptor.triggerSceneUuid);
	condition.config.insert(QStringLiteral("sceneName"), descriptor.triggerSceneName);
	condition.config.insert(QStringLiteral("valueKey"), descriptor.triggerValueKey);
	condition.config.insert(QStringLiteral("value"), descriptor.triggerValue);
	condition.config.insert(QStringLiteral("intervalMs"), descriptor.intervalMs);

	auto &action = macro.actions.front();
	action.typeId = descriptor.actionType.trimmed().isEmpty() ? QStringLiteral("switch_program_scene") : descriptor.actionType;
	action.config.insert(QStringLiteral("connectionId"), descriptor.actionConnectionId);
	action.config.insert(QStringLiteral("connectionName"), descriptor.actionConnectionName);
	action.config.insert(QStringLiteral("sceneUuid"), descriptor.actionSceneUuid);
	action.config.insert(QStringLiteral("sceneName"), descriptor.actionSceneName);
	action.config.insert(QStringLiteral("valueKey"), descriptor.actionValueKey);
	action.config.insert(QStringLiteral("value"), descriptor.actionValue);
	action.config.insert(QStringLiteral("delayMs"), descriptor.actionDelayMs);

	for (int index = 1; index < macro.conditions.size(); ++index)
		macro.conditions[index].logicOp = QStringLiteral("and");
}

bool SwitchAutomationEngine::UpdateMacro(const SwitchMacroDescriptor &descriptor)
{
	const int index = FindMacroIndex(descriptor.id);
	if (index < 0)
		return false;

	const qint64 lastExecutionMs = document.macros[index].lastExecutionMs;
	const bool lastConditionMatched = document.macros[index].lastConditionMatched;
	ApplyLegacyDescriptorToMacro(document.macros[index], descriptor);
	document.macros[index].lastExecutionMs = lastExecutionMs;
	document.macros[index].lastConditionMatched = lastConditionMatched;
	AppendEvent(QStringLiteral("info"), QStringLiteral("macro"),
		    QStringLiteral("Updated macro %1").arg(document.macros[index].name), descriptor.id);
	emit StateChanged();
	return true;
}

bool SwitchAutomationEngine::UpsertMacroDefinition(const SwitchAutomationMacro &macro, QString *effectiveId)
{
	SwitchAutomationMacro next = macro;
	if (next.id.trimmed().isEmpty())
		next.id = SwitchCreateAutomationId(QStringLiteral("macro"));
	if (next.name.trimmed().isEmpty())
		next.name = NextMacroName(QStringLiteral("Macro"));
	if (next.runPolicy.trimmed().isEmpty())
		next.runPolicy = QStringLiteral("on_change");
	if (next.startupPolicy.trimmed().isEmpty())
		next.startupPolicy = QStringLiteral("wait_for_change");
	if (next.repeatIntervalMs <= 0)
		next.repeatIntervalMs = 5000;

	for (auto &segment : next.conditions) {
		if (segment.id.trimmed().isEmpty())
			segment.id = SwitchCreateAutomationId(QStringLiteral("condition"));
		if (segment.typeId.trimmed().isEmpty())
			segment.typeId = QStringLiteral("manual");
		if (segment.logicOp.trimmed().isEmpty())
			segment.logicOp = QStringLiteral("and");
	}
	for (auto &segment : next.actions) {
		if (segment.id.trimmed().isEmpty())
			segment.id = SwitchCreateAutomationId(QStringLiteral("action"));
		if (segment.typeId.trimmed().isEmpty())
			segment.typeId = QStringLiteral("switch_program_scene");
	}

	if (next.conditions.isEmpty()) {
		SwitchAutomationSegment segment;
		segment.id = SwitchCreateAutomationId(QStringLiteral("condition"));
		segment.typeId = QStringLiteral("manual");
		next.conditions.push_back(segment);
	}
	if (next.actions.isEmpty()) {
		SwitchAutomationSegment segment;
		segment.id = SwitchCreateAutomationId(QStringLiteral("action"));
		segment.typeId = QStringLiteral("switch_program_scene");
		next.actions.push_back(segment);
	}

	const int index = FindMacroIndex(next.id);
	if (index >= 0) {
		next.lastConditionMatched = document.macros[index].lastConditionMatched;
		next.lastExecutionMs = document.macros[index].lastExecutionMs;
		document.macros[index] = next;
		AppendEvent(QStringLiteral("info"), QStringLiteral("macro"),
			    QStringLiteral("Updated macro %1").arg(next.name), next.id);
	} else {
		document.macros.push_back(next);
		AppendEvent(QStringLiteral("info"), QStringLiteral("macro"),
			    QStringLiteral("Created macro %1").arg(next.name), next.id);
	}

	if (effectiveId)
		*effectiveId = next.id;
	emit StateChanged();
	return true;
}

bool SwitchAutomationEngine::UpsertConnection(const SwitchAutomationConnection &connection, QString *effectiveId)
{
	SwitchAutomationConnection next = connection;
	if (next.id.trimmed().isEmpty())
		next.id = SwitchCreateAutomationId(QStringLiteral("connection"));
	if (next.typeId.trimmed().isEmpty())
		next.typeId = QStringLiteral("osc");
	if (next.name.trimmed().isEmpty())
		next.name = QStringLiteral("Connection");
	next.available = true;

	const int index = FindConnectionIndex(next.id);
	if (index >= 0) {
		document.connections[index] = next;
		AppendEvent(QStringLiteral("info"), QStringLiteral("connection"),
			    QStringLiteral("Updated connection %1").arg(next.name));
	} else {
		document.connections.push_back(next);
		AppendEvent(QStringLiteral("info"), QStringLiteral("connection"),
			    QStringLiteral("Created connection %1").arg(next.name));
	}

	ReconfigureOscConnections();
	emit ConnectionChanged(next.id);
	emit StateChanged();
	if (effectiveId)
		*effectiveId = next.id;
	return true;
}

bool SwitchAutomationEngine::DeleteConnection(const QString &connectionId)
{
	const int index = FindConnectionIndex(connectionId);
	if (index < 0)
		return false;

	const QString name = document.connections[index].name.isEmpty() ? document.connections[index].id : document.connections[index].name;
	document.connections.removeAt(index);
	if (oscRuntime.contains(connectionId) && oscRuntime[connectionId].socket) {
		oscRuntime[connectionId].socket->close();
		oscRuntime[connectionId].socket->deleteLater();
	}
	oscRuntime.remove(connectionId);
	AppendEvent(QStringLiteral("info"), QStringLiteral("connection"),
		    QStringLiteral("Deleted connection %1").arg(name));
	ReconfigureOscConnections();
	emit ConnectionChanged(connectionId);
	emit StateChanged();
	return true;
}

SwitchAutomationConnection SwitchAutomationEngine::ConnectionById(const QString &connectionId) const
{
	for (const auto &connection : document.connections) {
		if (connection.id == connectionId)
			return connection;
	}
	return {};
}

bool SwitchAutomationEngine::TestConnection(const QString &connectionId, QString *message)
{
	for (auto &connection : document.connections) {
		if (connection.id != connectionId)
			continue;

		const QStringList errors = SwitchValidateAutomationConnection(connection);
		if (!errors.isEmpty()) {
			connection.status = errors.join(QStringLiteral("; "));
			AppendEvent(QStringLiteral("error"), QStringLiteral("connection"),
				    QStringLiteral("Connection %1 failed validation").arg(connection.name.isEmpty() ? connection.id
													 : connection.name),
				    {}, false);
			emit ConnectionChanged(connection.id);
			emit StateChanged();
			if (message)
				*message = connection.status;
			return false;
		}

		if (connection.typeId == QStringLiteral("osc")) {
			QString status;
			if (ConnectionCanReceive(connection.config)) {
				if (const auto runtime = oscRuntime.value(connection.id); runtime.socket) {
					status = QStringLiteral("Listening on %1:%2")
							 .arg(runtime.socket->localAddress().toString())
							 .arg(runtime.socket->localPort());
				} else {
					QUdpSocket probe;
					const auto address = ListenAddressFromConfig(connection.config);
					const quint16 port = quint16(IntFromConfig(connection.config, QStringLiteral("listenPort")));
					if (!probe.bind(address, port)) {
						status = QStringLiteral("Unable to bind %1:%2: %3")
								 .arg(address.toString())
								 .arg(port)
								 .arg(probe.errorString());
						connection.status = status;
						AppendEvent(QStringLiteral("error"), QStringLiteral("connection"),
							    QStringLiteral("Connection %1 failed bind test")
								    .arg(connection.name.isEmpty() ? connection.id : connection.name),
							    {}, false);
						emit ConnectionChanged(connection.id);
						emit StateChanged();
						if (message)
							*message = status;
						return false;
					}
					status = QStringLiteral("OSC listener can bind to %1:%2")
							 .arg(probe.localAddress().toString())
							 .arg(probe.localPort());
				}
			}
			if (ConnectionCanSend(connection.config)) {
				QHostAddress address;
				quint16 port = 0;
				QString resolveError;
				if (!ResolveRemoteEndpoint(connection.config, &address, &port, &resolveError)) {
					connection.status = resolveError;
					AppendEvent(QStringLiteral("error"), QStringLiteral("connection"),
						    QStringLiteral("Connection %1 failed host resolution")
							    .arg(connection.name.isEmpty() ? connection.id : connection.name),
						    {}, false);
					emit ConnectionChanged(connection.id);
					emit StateChanged();
					if (message)
						*message = connection.status;
					return false;
				}
				const QString sendStatus =
					QStringLiteral("OSC send ready for %1:%2").arg(address.toString()).arg(port);
				status = status.isEmpty() ? sendStatus : QStringLiteral("%1; %2").arg(status, sendStatus);
			}
			connection.status = status.isEmpty() ? QStringLiteral("Configuration valid") : status;
		} else {
			connection.status = QStringLiteral("Configuration valid");
		}
		AppendEvent(QStringLiteral("info"), QStringLiteral("connection"),
			    QStringLiteral("Connection %1 validated").arg(connection.name.isEmpty() ? connection.id : connection.name));
		emit ConnectionChanged(connection.id);
		emit StateChanged();
		if (message)
			*message = connection.status;
		return true;
	}

	if (message)
		*message = QStringLiteral("Connection not found");
	return false;
}

bool SwitchAutomationEngine::SetMacroPaused(const QString &macroId, bool paused)
{
	const int index = FindMacroIndex(macroId);
	if (index < 0)
		return false;

	document.macros[index].paused = paused;
	AppendEvent(QStringLiteral("info"), QStringLiteral("macro"),
		    QStringLiteral("%1 macro %2").arg(paused ? QStringLiteral("Paused") : QStringLiteral("Resumed"),
						    document.macros[index].name),
		    macroId);
	emit MacroPaused(macroId, paused);
	emit StateChanged();
	return true;
}

bool SwitchAutomationEngine::SetMacroEnabled(const QString &macroId, bool enabled)
{
	const int index = FindMacroIndex(macroId);
	if (index < 0)
		return false;

	document.macros[index].enabled = enabled;
	AppendEvent(QStringLiteral("info"), QStringLiteral("macro"),
		    QStringLiteral("%1 macro %2").arg(enabled ? QStringLiteral("Enabled") : QStringLiteral("Disabled"),
						    document.macros[index].name),
		    macroId);
	emit StateChanged();
	return true;
}

QString SwitchAutomationEngine::ResolveTemplate(const QString &value, const QHash<QString, QString> &properties) const
{
	return SwitchResolveAutomationTemplate(value, document.variables, properties);
}

QString SwitchAutomationEngine::ResolveOscConnectionId(const QVariantMap &config) const
{
	const QString connectionId = StringFromConfig(config, QStringLiteral("connectionId")).trimmed();
	if (!connectionId.isEmpty())
		return connectionId;

	const QString connectionName = StringFromConfig(config, QStringLiteral("connectionName")).trimmed();
	if (connectionName.isEmpty())
		return {};

	for (const auto &connection : document.connections) {
		if (connection.name.compare(connectionName, Qt::CaseInsensitive) == 0)
			return connection.id;
	}
	return {};
}

QHash<QString, QString> SwitchAutomationEngine::BuildOscTriggerProperties(const QString &connectionId) const
{
	QHash<QString, QString> properties;
	const auto runtime = oscRuntime.value(connectionId);
	if (runtime.sequence == 0)
		return properties;

	properties.insert(QStringLiteral("oscConnectionId"), connectionId);
	for (const auto &connection : document.connections) {
		if (connection.id == connectionId) {
			properties.insert(QStringLiteral("oscConnectionName"), connection.name);
			break;
		}
	}
	properties.insert(QStringLiteral("oscAddress"), runtime.address);
	properties.insert(QStringLiteral("oscArguments"), SwitchOscArgumentsToSummaryString(runtime.arguments));
	properties.insert(QStringLiteral("oscArgumentsJson"), SwitchOscArgumentsToConfigString(runtime.arguments));
	properties.insert(QStringLiteral("oscSenderHost"), runtime.senderHost);
	properties.insert(QStringLiteral("oscSenderPort"), QString::number(runtime.senderPort));
	properties.insert(QStringLiteral("oscSequence"), QString::number(runtime.sequence));
	return properties;
}

void SwitchAutomationEngine::StopOscConnections()
{
	for (auto it = oscRuntime.begin(); it != oscRuntime.end(); ++it) {
		if (it->socket) {
			it->socket->close();
			it->socket->disconnect(this);
			delete it->socket;
			it->socket = nullptr;
		}
	}
	oscRuntime.clear();
}

void SwitchAutomationEngine::StartOscConnection(const SwitchAutomationConnection &connection)
{
	if (connection.typeId != QStringLiteral("osc") || !connection.enabled || !ConnectionCanReceive(connection.config))
		return;

	auto &runtime = oscRuntime[connection.id];
	if (runtime.socket) {
		runtime.socket->close();
		runtime.socket->disconnect(this);
		delete runtime.socket;
		runtime.socket = nullptr;
	}

	auto *socket = new QUdpSocket(this);
	const auto address = ListenAddressFromConfig(connection.config);
	const quint16 port = quint16(IntFromConfig(connection.config, QStringLiteral("listenPort")));
	if (!socket->bind(address, port)) {
		runtime.lastError = socket->errorString();
		AppendEvent(QStringLiteral("error"), QStringLiteral("osc"),
			    QStringLiteral("Failed to bind OSC listener %1 on %2:%3: %4")
				    .arg(connection.name.isEmpty() ? connection.id : connection.name)
				    .arg(address.toString())
				    .arg(port)
				    .arg(socket->errorString()),
			    {}, false);
		delete socket;
		return;
	}

	runtime.socket = socket;
	runtime.lastError.clear();
	connect(socket, &QUdpSocket::readyRead, this, [this, connectionId = connection.id]() { HandleOscReadyRead(connectionId); });
	AppendEvent(QStringLiteral("info"), QStringLiteral("osc"),
		    QStringLiteral("Listening for OSC on %1:%2")
			    .arg(socket->localAddress().toString())
			    .arg(socket->localPort()));
}

void SwitchAutomationEngine::ReconfigureOscConnections()
{
	StopOscConnections();
	if (lifecycleState != SwitchAutomationLifecycleState::Running)
		return;

	for (const auto &connection : document.connections)
		StartOscConnection(connection);
}

void SwitchAutomationEngine::HandleOscReadyRead(const QString &connectionId)
{
	auto it = oscRuntime.find(connectionId);
	if (it == oscRuntime.end() || !it->socket)
		return;

	auto &runtime = it.value();
	while (runtime.socket->hasPendingDatagrams()) {
		QByteArray datagram;
		datagram.resize(int(runtime.socket->pendingDatagramSize()));
		QHostAddress sender;
		quint16 senderPort = 0;
		const qint64 readSize =
			runtime.socket->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);
		if (readSize < 0)
			break;
		datagram.resize(int(readSize));

		SwitchOscMessage message;
		QString parseError;
		if (!SwitchParseOscPacket(datagram, &message, &parseError)) {
			runtime.lastError = parseError;
			AppendEvent(QStringLiteral("error"), QStringLiteral("osc"),
				    QStringLiteral("Discarded OSC packet on %1: %2").arg(connectionId, parseError),
				    {}, false);
			continue;
		}

		runtime.sequence += 1;
		runtime.lastReceivedMs = QDateTime::currentMSecsSinceEpoch();
		runtime.address = message.address;
		runtime.arguments = message.arguments;
		runtime.senderHost = sender.toString();
		runtime.senderPort = senderPort;
		runtime.lastError.clear();

		AppendEvent(QStringLiteral("info"), QStringLiteral("osc"),
			    QStringLiteral("Received OSC %1 from %2:%3")
				    .arg(SwitchOscMessageSummary(message), runtime.senderHost)
				    .arg(runtime.senderPort));
	}

	emit StateChanged();
}

bool SwitchAutomationEngine::EvaluateSegmentCondition(const SwitchAutomationSegment &segment,
						      const SwitchAutomationMacro &macro, qint64 now)
{
	if (!segment.enabled)
		return true;

	const QString typeId = segment.typeId;
	if (typeId == QStringLiteral("manual"))
		return false;

	if (typeId == QStringLiteral("timer")) {
		const int intervalMs = std::max(250, IntFromConfig(segment.config, QStringLiteral("intervalMs"),
						     macro.repeatIntervalMs > 0 ? macro.repeatIntervalMs : 5000));
		return (QDateTime::currentMSecsSinceEpoch() - macro.lastExecutionMs) >= intervalMs;
	}

	if (typeId == QStringLiteral("program_scene") || typeId == QStringLiteral("scene")) {
		if (!frontendCallbacksReady)
			return false;

		obs_source_t *currentScene = obs_frontend_get_current_scene();
		if (!currentScene)
			return false;

		const QString currentUuid = QString::fromUtf8(obs_source_get_uuid(currentScene));
		const QString currentName = QString::fromUtf8(obs_source_get_name(currentScene));
		const QString targetUuid = StringFromConfig(segment.config, QStringLiteral("sceneUuid"));
		const QString targetName = StringFromConfig(segment.config, QStringLiteral("sceneName"));
		obs_source_release(currentScene);

		return (!targetUuid.isEmpty() && currentUuid == targetUuid) || (!targetName.isEmpty() && currentName == targetName);
	}

	if (typeId == QStringLiteral("recording_state"))
		return FrontendStateValue(frontendCallbacksReady, obs_frontend_recording_active) ==
		       BoolFromConfig(segment.config, QStringLiteral("desiredState"), true);
	if (typeId == QStringLiteral("streaming_state"))
		return FrontendStateValue(frontendCallbacksReady, obs_frontend_streaming_active) ==
		       BoolFromConfig(segment.config, QStringLiteral("desiredState"), true);
	if (typeId == QStringLiteral("replay_state"))
		return FrontendStateValue(frontendCallbacksReady, obs_frontend_replay_buffer_active) ==
		       BoolFromConfig(segment.config, QStringLiteral("desiredState"), true);
	if (typeId == QStringLiteral("virtual_camera_state"))
		return FrontendStateValue(frontendCallbacksReady, obs_frontend_virtualcam_active) ==
		       BoolFromConfig(segment.config, QStringLiteral("desiredState"), true);

	if (typeId == QStringLiteral("variable")) {
		const QString key = StringFromConfig(segment.config, QStringLiteral("key")).trimmed();
		if (key.isEmpty())
			return false;

		const QString mode = StringFromConfig(segment.config, QStringLiteral("mode")).trimmed().isEmpty()
					     ? QStringLiteral("equals")
					     : StringFromConfig(segment.config, QStringLiteral("mode")).trimmed();
		const QString currentValue = document.variables.value(key);
		if (mode == QStringLiteral("exists"))
			return document.variables.contains(key);
		if (mode == QStringLiteral("contains"))
			return currentValue.contains(ResolveTemplate(StringFromConfig(segment.config, QStringLiteral("value"))));
		return currentValue == ResolveTemplate(StringFromConfig(segment.config, QStringLiteral("value")));
	}

	if (typeId == QStringLiteral("motion_target")) {
		const bool desiredState = BoolFromConfig(segment.config, QStringLiteral("desiredState"), true);
		return (document.variables.value(QStringLiteral("motion.targetActive")) == QStringLiteral("true")) == desiredState;
	}

	if (typeId == QStringLiteral("macro_state")) {
		const QString targetId = StringFromConfig(segment.config, QStringLiteral("macroId"));
		const QString targetName = StringFromConfig(segment.config, QStringLiteral("macroName"));
		const QString state = StringFromConfig(segment.config, QStringLiteral("state")).trimmed().isEmpty()
					      ? QStringLiteral("enabled")
					      : StringFromConfig(segment.config, QStringLiteral("state")).trimmed();
		for (const auto &candidate : document.macros) {
			if ((!targetId.isEmpty() && candidate.id == targetId) ||
			    (!targetName.isEmpty() && candidate.name == targetName)) {
				if (state == QStringLiteral("paused"))
					return candidate.paused;
				if (state == QStringLiteral("conditions_true"))
					return candidate.lastConditionMatched;
				if (state == QStringLiteral("ran"))
					return candidate.lastExecutionMs > 0;
				return candidate.enabled;
			}
		}
		return false;
	}

	if (typeId == QStringLiteral("file")) {
		const QString path = ResolveTemplate(StringFromConfig(segment.config, QStringLiteral("path")));
		if (path.isEmpty())
			return false;
		const QFileInfo info(path);
		const QString mode = StringFromConfig(segment.config, QStringLiteral("mode")).trimmed().isEmpty()
					     ? QStringLiteral("exists")
					     : StringFromConfig(segment.config, QStringLiteral("mode")).trimmed();
		return mode == QStringLiteral("missing") ? !info.exists() : info.exists();
	}

	if (typeId == QStringLiteral("osc_receive")) {
		const QString connectionId = ResolveOscConnectionId(segment.config);
		if (connectionId.isEmpty())
			return false;

		const auto runtime = oscRuntime.value(connectionId);
		const bool desiredState = BoolFromConfig(segment.config, QStringLiteral("desiredState"), true);
		const int intervalMs = std::max(250, IntFromConfig(segment.config, QStringLiteral("intervalMs"),
						     macro.repeatIntervalMs > 0 ? macro.repeatIntervalMs : 5000));
		const QString addressPattern = ResolveTemplate(StringFromConfig(segment.config, QStringLiteral("valueKey")),
							      macro.properties);
		const QString expectedArgsText = ResolveTemplate(StringFromConfig(segment.config, QStringLiteral("value")),
								macro.properties);

		bool matches = runtime.sequence > 0 && SwitchOscAddressPatternMatches(addressPattern, runtime.address);
		if (matches && !expectedArgsText.trimmed().isEmpty()) {
			QVector<SwitchOscArgument> expectedArguments;
			QString parseError;
			if (!SwitchParseOscArguments(expectedArgsText, &expectedArguments, &parseError)) {
				AppendEvent(QStringLiteral("error"), QStringLiteral("osc"),
					    QStringLiteral("Invalid OSC match arguments on macro %1: %2")
						    .arg(macro.name, parseError),
					    macro.id, false);
				return false;
			}
			matches = SwitchOscArgumentsEqual(runtime.arguments, expectedArguments);
		}

		const QString stateKey = QStringLiteral("%1:%2").arg(macro.id, segment.id);
		if (!desiredState)
			return !matches || (now - runtime.lastReceivedMs) >= intervalMs;

		if (!matches || oscConditionLastSeen.value(stateKey) == runtime.sequence)
			return false;

		oscConditionLastSeen.insert(stateKey, runtime.sequence);
		pendingTriggerProperties.insert(macro.id, BuildOscTriggerProperties(connectionId));
		return true;
	}

	return false;
}

bool SwitchAutomationEngine::EvaluateMacroCondition(SwitchAutomationMacro &macro, qint64 now)
{
	UNUSED_PARAMETER(now);
	if (macro.conditions.isEmpty())
		return false;

	bool matched = false;
	bool hasValue = false;
	for (const auto &segment : macro.conditions) {
		if (!segment.enabled)
			continue;
		const bool current = EvaluateSegmentCondition(segment, macro, now);
		if (!hasValue) {
			matched = current;
			hasValue = true;
			continue;
		}
		matched = segment.logicOp == QStringLiteral("or") ? (matched || current) : (matched && current);
	}

	return hasValue && matched;
}

bool SwitchAutomationEngine::ShouldRunMacro(const SwitchAutomationMacro &macro, bool conditionMatched, qint64 now) const
{
	return SwitchShouldRunAutomationMacro(macro, conditionMatched, now);
}

bool SwitchAutomationEngine::ExecuteActionSegment(const SwitchAutomationMacro &macro, const SwitchAutomationSegment &segment,
						  QHash<QString, QString> &properties, QString *message, int *nextDelayMs,
						  bool *continueExecution)
{
	UNUSED_PARAMETER(macro);
	if (nextDelayMs)
		*nextDelayMs = 0;
	if (continueExecution)
		*continueExecution = true;

	if (!segment.enabled)
		return true;

	const QString actionType = segment.typeId;
	const QString resolvedValue = ResolveTemplate(StringFromConfig(segment.config, QStringLiteral("value")), properties);
	const QString resolvedSceneUuid = ResolveTemplate(StringFromConfig(segment.config, QStringLiteral("sceneUuid")), properties);
	const QString resolvedSceneName = ResolveTemplate(StringFromConfig(segment.config, QStringLiteral("sceneName")), properties);

	if (actionType == QStringLiteral("wait")) {
		if (nextDelayMs)
			*nextDelayMs = std::max(0, IntFromConfig(segment.config, QStringLiteral("durationMs"), segment.durationMs));
		if (message)
			*message = QStringLiteral("Waited %1 ms").arg(nextDelayMs ? *nextDelayMs : 0);
		return true;
	}

	if (actionType == QStringLiteral("switch_program_scene") || actionType == QStringLiteral("switch_scene")) {
		if (!frontendCallbacksReady)
			return false;
		obs_source_t *source = nullptr;
		if (!resolvedSceneUuid.isEmpty())
			source = obs_get_source_by_uuid(resolvedSceneUuid.toUtf8().constData());
		if (!source && !resolvedSceneName.isEmpty())
			source = obs_get_source_by_name(resolvedSceneName.toUtf8().constData());
		if (!source)
			return false;
		obs_frontend_set_current_scene(source);
		if (message)
			*message = QStringLiteral("Switched program scene to %1").arg(QString::fromUtf8(obs_source_get_name(source)));
		obs_source_release(source);
	} else if (actionType == QStringLiteral("switch_preview_scene")) {
		if (!frontendCallbacksReady)
			return false;
		if (!obs_frontend_preview_program_mode_active())
			return false;
		obs_source_t *source = nullptr;
		if (!resolvedSceneUuid.isEmpty())
			source = obs_get_source_by_uuid(resolvedSceneUuid.toUtf8().constData());
		if (!source && !resolvedSceneName.isEmpty())
			source = obs_get_source_by_name(resolvedSceneName.toUtf8().constData());
		if (!source)
			return false;
		obs_frontend_set_current_preview_scene(source);
		if (message)
			*message = QStringLiteral("Switched preview scene to %1").arg(QString::fromUtf8(obs_source_get_name(source)));
		obs_source_release(source);
	} else if (actionType == QStringLiteral("switch_vertical_scene")) {
		if (!canvasManager)
			return false;
		const QString target = !resolvedSceneUuid.isEmpty() ? resolvedSceneUuid : resolvedSceneName;
		if (!canvasManager->SetCanvasActiveScene(canvasManager->VerticalCanvasId(), target))
			return false;
		if (message)
			*message = QStringLiteral("Switched vertical scene to %1").arg(target);
	} else if (actionType == QStringLiteral("open_vertical_window")) {
		if (!canvasManager || !canvasManager->OpenPreviewWindow(canvasManager->VerticalCanvasId()))
			return false;
		if (message)
			*message = QStringLiteral("Opened vertical window");
	} else if (actionType == QStringLiteral("open_vertical_projector")) {
		if (!canvasManager || !canvasManager->OpenProjector(canvasManager->VerticalCanvasId()))
			return false;
		if (message)
			*message = QStringLiteral("Opened vertical projector");
	} else if (actionType == QStringLiteral("start_recording")) {
		if (!frontendCallbacksReady)
			return false;
		if (!obs_frontend_recording_active())
			obs_frontend_recording_start();
	} else if (actionType == QStringLiteral("stop_recording")) {
		if (!frontendCallbacksReady)
			return false;
		if (obs_frontend_recording_active())
			obs_frontend_recording_stop();
	} else if (actionType == QStringLiteral("pause_recording")) {
		if (!frontendCallbacksReady)
			return false;
		if (!obs_frontend_recording_active())
			return false;
		obs_frontend_recording_pause(true);
	} else if (actionType == QStringLiteral("resume_recording")) {
		if (!frontendCallbacksReady)
			return false;
		if (!obs_frontend_recording_active())
			return false;
		obs_frontend_recording_pause(false);
	} else if (actionType == QStringLiteral("split_recording")) {
		if (!frontendCallbacksReady)
			return false;
		if (!obs_frontend_recording_split_file())
			return false;
	} else if (actionType == QStringLiteral("add_recording_chapter")) {
		if (!frontendCallbacksReady)
			return false;
		const QByteArray chapterName = resolvedValue.toUtf8();
		if (!obs_frontend_recording_add_chapter(chapterName.isEmpty() ? nullptr : chapterName.constData()))
			return false;
	} else if (actionType == QStringLiteral("start_streaming")) {
		if (!frontendCallbacksReady)
			return false;
		if (!obs_frontend_streaming_active())
			obs_frontend_streaming_start();
	} else if (actionType == QStringLiteral("stop_streaming")) {
		if (!frontendCallbacksReady)
			return false;
		if (obs_frontend_streaming_active())
			obs_frontend_streaming_stop();
	} else if (actionType == QStringLiteral("start_replay")) {
		if (!frontendCallbacksReady)
			return false;
		if (!obs_frontend_replay_buffer_active())
			obs_frontend_replay_buffer_start();
	} else if (actionType == QStringLiteral("save_replay")) {
		if (!frontendCallbacksReady)
			return false;
		if (!obs_frontend_replay_buffer_active())
			return false;
		obs_frontend_replay_buffer_save();
	} else if (actionType == QStringLiteral("stop_replay")) {
		if (!frontendCallbacksReady)
			return false;
		if (obs_frontend_replay_buffer_active())
			obs_frontend_replay_buffer_stop();
	} else if (actionType == QStringLiteral("start_virtual_camera")) {
		if (!frontendCallbacksReady)
			return false;
		if (!obs_frontend_virtualcam_active())
			obs_frontend_start_virtualcam();
	} else if (actionType == QStringLiteral("stop_virtual_camera")) {
		if (!frontendCallbacksReady)
			return false;
		if (obs_frontend_virtualcam_active())
			obs_frontend_stop_virtualcam();
	} else if (actionType == QStringLiteral("set_variable")) {
		const QString key = ResolveTemplate(StringFromConfig(segment.config, QStringLiteral("valueKey")), properties).trimmed();
		if (key.isEmpty())
			return false;
		document.variables.insert(key, resolvedValue);
		properties.insert(key, resolvedValue);
		emit VariableChanged(key, resolvedValue);
		if (message)
			*message = QStringLiteral("Set variable %1").arg(key);
	} else if (actionType == QStringLiteral("remove_variable")) {
		const QString key = ResolveTemplate(StringFromConfig(segment.config, QStringLiteral("valueKey")), properties).trimmed();
		if (key.isEmpty())
			return false;
		document.variables.remove(key);
		properties.remove(key);
		emit VariableChanged(key, QString());
		if (message)
			*message = QStringLiteral("Removed variable %1").arg(key);
	} else if (actionType == QStringLiteral("copy_variable")) {
		const QString fromKey = ResolveTemplate(StringFromConfig(segment.config, QStringLiteral("sourceKey")), properties).trimmed();
		const QString toKey = ResolveTemplate(StringFromConfig(segment.config, QStringLiteral("valueKey")), properties).trimmed();
		if (fromKey.isEmpty() || toKey.isEmpty())
			return false;
		const QString copiedValue = document.variables.value(fromKey);
		document.variables.insert(toKey, copiedValue);
		properties.insert(toKey, copiedValue);
		emit VariableChanged(toKey, copiedValue);
		if (message)
			*message = QStringLiteral("Copied variable %1 to %2").arg(fromKey, toKey);
	} else if (actionType == QStringLiteral("run_macro")) {
		const QString targetId = ResolveTemplate(StringFromConfig(segment.config, QStringLiteral("macroId")), properties).trimmed();
		const QString targetName = ResolveTemplate(StringFromConfig(segment.config, QStringLiteral("macroName")), properties).trimmed();
		QString selectedId = targetId;
		if (selectedId.isEmpty() && !targetName.isEmpty()) {
			for (const auto &candidate : document.macros) {
				if (candidate.name == targetName) {
					selectedId = candidate.id;
					break;
				}
			}
		}
		if (selectedId.isEmpty())
			return false;
		QString nestedMessage;
		if (!TriggerMacro(selectedId, &nestedMessage))
			return false;
		if (message)
			*message = nestedMessage.isEmpty() ? QStringLiteral("Triggered nested macro") : nestedMessage;
	} else if (actionType == QStringLiteral("motion_enable") || actionType == QStringLiteral("motion_disable")) {
		if (!motionManager)
			return false;
		const QString profileId = ResolveTemplate(StringFromConfig(segment.config, QStringLiteral("valueKey")), properties).trimmed();
		if (profileId.isEmpty())
			return false;
		const bool enable = actionType == QStringLiteral("motion_enable");
		if (!motionManager->SetProfileEnabled(profileId, enable))
			return false;
		if (message)
			*message = enable ? QStringLiteral("Enabled Motion profile %1").arg(profileId)
					  : QStringLiteral("Disabled Motion profile %1").arg(profileId);
	} else if (actionType == QStringLiteral("http_get")) {
		const QUrl url(resolvedValue);
		if (!url.isValid() || url.scheme().isEmpty())
			return false;
		networkManager->get(QNetworkRequest(url));
		if (message)
			*message = QStringLiteral("Triggered HTTP GET %1").arg(url.toString());
	} else if (actionType == QStringLiteral("http_post")) {
		const QUrl url(ResolveTemplate(StringFromConfig(segment.config, QStringLiteral("url")), properties));
		if (!url.isValid() || url.scheme().isEmpty())
			return false;
		const QByteArray body = resolvedValue.toUtf8();
		QNetworkRequest request(url);
		request.setHeader(QNetworkRequest::ContentTypeHeader,
				  StringFromConfig(segment.config, QStringLiteral("contentType")).trimmed().isEmpty()
					  ? QStringLiteral("application/json")
					  : StringFromConfig(segment.config, QStringLiteral("contentType")));
		networkManager->post(request, body);
		if (message)
			*message = QStringLiteral("Triggered HTTP POST %1").arg(url.toString());
	} else if (actionType == QStringLiteral("osc")) {
		const QString connectionId = ResolveOscConnectionId(segment.config);
		if (connectionId.isEmpty())
			return false;

		const auto connection = ConnectionById(connectionId);
		if (connection.id.isEmpty() || !connection.enabled || connection.typeId != QStringLiteral("osc") ||
		    !ConnectionCanSend(connection.config))
			return false;

		const QString address = ResolveTemplate(StringFromConfig(segment.config, QStringLiteral("valueKey")), properties).trimmed();
		if (address.isEmpty())
			return false;

		QVector<SwitchOscArgument> arguments;
		QString parseError;
		if (!SwitchParseOscArguments(resolvedValue, &arguments, &parseError)) {
			AppendEvent(QStringLiteral("error"), QStringLiteral("osc"),
				    QStringLiteral("Invalid OSC argument JSON for macro %1: %2").arg(macro.name, parseError),
				    macro.id, false);
			return false;
		}

		SwitchOscMessage oscMessage{address, arguments};
		QByteArray packet;
		QString buildError;
		if (!SwitchBuildOscPacket(oscMessage, &packet, &buildError)) {
			AppendEvent(QStringLiteral("error"), QStringLiteral("osc"),
				    QStringLiteral("Unable to build OSC packet for macro %1: %2").arg(macro.name, buildError),
				    macro.id, false);
			return false;
		}

		QHostAddress remoteAddress;
		quint16 remotePort = 0;
		QString resolveError;
		if (!ResolveRemoteEndpoint(connection.config, &remoteAddress, &remotePort, &resolveError)) {
			AppendEvent(QStringLiteral("error"), QStringLiteral("osc"),
				    QStringLiteral("Unable to resolve OSC destination for macro %1: %2").arg(macro.name, resolveError),
				    macro.id, false);
			return false;
		}

		QUdpSocket tempSocket;
		auto runtime = oscRuntime.value(connectionId);
		QUdpSocket *socket = runtime.socket ? runtime.socket : &tempSocket;
		const qint64 written = socket->writeDatagram(packet, remoteAddress, remotePort);
		if (written != packet.size()) {
			AppendEvent(QStringLiteral("error"), QStringLiteral("osc"),
				    QStringLiteral("Failed to send OSC %1 to %2:%3")
					    .arg(SwitchOscMessageSummary(oscMessage), remoteAddress.toString())
					    .arg(remotePort),
				    macro.id, false);
			return false;
		}

		AppendEvent(QStringLiteral("info"), QStringLiteral("osc"),
			    QStringLiteral("Sent OSC %1 to %2:%3")
				    .arg(SwitchOscMessageSummary(oscMessage), remoteAddress.toString())
				    .arg(remotePort),
			    macro.id);
		if (message)
			*message = QStringLiteral("Sent OSC %1").arg(oscMessage.address);
	} else {
		return false;
	}

	if (nextDelayMs && *nextDelayMs == 0)
		*nextDelayMs = std::max(0, IntFromConfig(segment.config, QStringLiteral("delayMs")));
	return true;
}

bool SwitchAutomationEngine::BeginMacroRun(SwitchAutomationMacro &macro, QString *message)
{
	if (lifecycleState != SwitchAutomationLifecycleState::Running)
		return false;
	if (macro.actions.isEmpty())
		return false;

	PendingMacroRun run;
	run.runId = SwitchCreateAutomationId(QStringLiteral("run"));
	run.macroId = macro.id;
	run.actions = macro.actions;
	run.nextActionIndex = 0;
	run.properties = macro.properties;
	run.properties.insert(QStringLiteral("macroName"), macro.name);
	if (pendingTriggerProperties.contains(macro.id)) {
		const auto triggerProperties = pendingTriggerProperties.take(macro.id);
		for (auto it = triggerProperties.begin(); it != triggerProperties.end(); ++it)
			run.properties.insert(it.key(), it.value());
	}
	run.properties.insert(QStringLiteral("streamingActive"),
			      FrontendStateValue(frontendCallbacksReady, obs_frontend_streaming_active)
				      ? QStringLiteral("true")
				      : QStringLiteral("false"));
	run.properties.insert(QStringLiteral("recordingActive"),
			      FrontendStateValue(frontendCallbacksReady, obs_frontend_recording_active) ? QStringLiteral("true")
												 : QStringLiteral("false"));
	run.properties.insert(QStringLiteral("replayActive"),
			      FrontendStateValue(frontendCallbacksReady, obs_frontend_replay_buffer_active)
				      ? QStringLiteral("true")
				      : QStringLiteral("false"));
	run.properties.insert(QStringLiteral("virtualCameraActive"),
			      FrontendStateValue(frontendCallbacksReady, obs_frontend_virtualcam_active)
				      ? QStringLiteral("true")
				      : QStringLiteral("false"));
	pendingRuns.insert(run.runId, run);
	if (message)
		*message = QStringLiteral("Scheduled macro %1").arg(macro.name);
	QTimer::singleShot(0, this, [this, runId = run.runId]() { ContinuePendingRun(runId); });
	return true;
}

void SwitchAutomationEngine::ContinuePendingRun(const QString &runId)
{
	if (!pendingRuns.contains(runId))
		return;
	if (lifecycleState != SwitchAutomationLifecycleState::Running) {
		pendingRuns.remove(runId);
		return;
	}

	PendingMacroRun run = pendingRuns.value(runId);
	const int macroIndex = FindMacroIndex(run.macroId);
	if (macroIndex < 0) {
		pendingRuns.remove(runId);
		return;
	}

	if (run.nextActionIndex >= run.actions.size()) {
		emit MacroTriggered(run.macroId, true,
				    run.lastMessage.isEmpty() ? QStringLiteral("Macro executed") : run.lastMessage);
		pendingRuns.remove(runId);
		emit StateChanged();
		return;
	}

	QString message;
	int nextDelayMs = 0;
	bool continueExecution = true;
	const auto &macro = document.macros[macroIndex];
	const auto &segment = run.actions[run.nextActionIndex];
	const bool success = ExecuteActionSegment(macro, segment, run.properties, &message, &nextDelayMs, &continueExecution);
	if (!success) {
		AppendEvent(QStringLiteral("error"), QStringLiteral("macro"),
			    QStringLiteral("Failed action %1 in macro %2")
				    .arg(SwitchActionFactory::NameForId(segment.typeId), macro.name),
			    run.macroId, false);
		emit MacroTriggered(run.macroId, false,
				    message.isEmpty() ? QStringLiteral("Macro action failed") : message);
		pendingRuns.remove(runId);
		emit StateChanged();
		return;
	}

	document.macros[macroIndex].properties = run.properties;
	run.lastMessage = message;
	run.nextActionIndex += 1;
	if (!continueExecution || run.nextActionIndex >= run.actions.size()) {
		emit MacroTriggered(run.macroId, true,
				    run.lastMessage.isEmpty() ? QStringLiteral("Macro executed") : run.lastMessage);
		pendingRuns.remove(runId);
		emit StateChanged();
		return;
	}

	pendingRuns.insert(runId, run);
	QTimer::singleShot(std::max(0, nextDelayMs), this, [this, runId]() { ContinuePendingRun(runId); });
}

bool SwitchAutomationEngine::TriggerMacro(const QString &macroId, QString *message)
{
	const int index = FindMacroIndex(macroId);
	if (index < 0)
		return false;

	auto &macro = document.macros[index];
	if (!macro.enabled || macro.paused)
		return false;

	const bool success = BeginMacroRun(macro, message);
	if (success) {
		macro.lastExecutionMs = QDateTime::currentMSecsSinceEpoch();
		AppendEvent(QStringLiteral("info"), QStringLiteral("macro"),
			    QStringLiteral("Triggered macro %1").arg(macro.name), macro.id);
		emit StateChanged();
	}
	return success;
}

bool SwitchAutomationEngine::SetVariable(const QString &key, const QString &value)
{
	const QString trimmed = key.trimmed();
	if (trimmed.isEmpty())
		return false;

	document.variables.insert(trimmed, value);
	AppendEvent(QStringLiteral("info"), QStringLiteral("variable"),
		    QStringLiteral("Set variable %1").arg(trimmed));
	emit VariableChanged(trimmed, value);
	emit StateChanged();
	return true;
}

bool SwitchAutomationEngine::RemoveVariable(const QString &key)
{
	const QString trimmed = key.trimmed();
	if (trimmed.isEmpty() || !document.variables.remove(trimmed))
		return false;

	AppendEvent(QStringLiteral("info"), QStringLiteral("variable"),
		    QStringLiteral("Removed variable %1").arg(trimmed));
	emit VariableChanged(trimmed, QString());
	emit StateChanged();
	return true;
}

bool SwitchAutomationEngine::ClearQueue(const QString &queueId)
{
	for (auto &queue : document.queues) {
		if (queue.id != queueId)
			continue;
		queue.nextIndex = 0;
		queue.actionSegmentIds.clear();
		emit QueueChanged(queueId);
		emit StateChanged();
		return true;
	}
	return false;
}

void SwitchAutomationEngine::ClearPendingRuns()
{
	pendingRuns.clear();
}

bool SwitchAutomationEngine::SetLifecycleState(SwitchAutomationLifecycleState state)
{
	if (lifecycleState == state)
		return true;

	lifecycleState = state;
	document.engineState = LifecycleStateName(state);
	if (state == SwitchAutomationLifecycleState::Running)
		evaluationTimer->start();
	else
		evaluationTimer->stop();

	if (state != SwitchAutomationLifecycleState::Running) {
		ClearPendingRuns();
		StopOscConnections();
	} else {
		ReconfigureOscConnections();
	}

	emit StateChanged();
	return true;
}

obs_data_t *SwitchAutomationEngine::SaveState() const
{
	return SwitchAutomationDocumentToObsData(document);
}

void SwitchAutomationEngine::LoadState(obs_data_t *data)
{
	SetLifecycleState(SwitchAutomationLifecycleState::Loading);
	ClearPendingRuns();
	document = SwitchAutomationDocumentFromObsData(data);
	oscConditionLastSeen.clear();
	pendingTriggerProperties.clear();
	eventLog.clear();
	AppendEvent(QStringLiteral("info"), QStringLiteral("engine"),
		    QStringLiteral("Loaded automation state with %1 macro(s)").arg(document.macros.size()));
	emit StateChanged();
}

void SwitchAutomationEngine::Reset()
{
	document = {};
	eventLog.clear();
	ClearPendingRuns();
	oscConditionLastSeen.clear();
	pendingTriggerProperties.clear();
	StopOscConnections();
	SetLifecycleState(SwitchAutomationLifecycleState::Loading);
	emit StateChanged();
}

void SwitchAutomationEngine::AppendEvent(const QString &level, const QString &scope, const QString &message,
					 const QString &macroId, bool success)
{
	eventLog.push_back({QDateTime::currentMSecsSinceEpoch(), level, scope, message, macroId, success});
	while (eventLog.size() > 50)
		eventLog.removeFirst();

	if (level == QStringLiteral("error"))
		emit ErrorRaised(scope, message);
}

obs_data_t *SwitchAutomationEngine::BuildStateData() const
{
	obs_data_t *root = SaveState();
	obs_data_set_bool(root, "streaming_active",
			  FrontendStateValue(frontendCallbacksReady, obs_frontend_streaming_active));
	obs_data_set_bool(root, "recording_active",
			  FrontendStateValue(frontendCallbacksReady, obs_frontend_recording_active));
	obs_data_set_bool(root, "replay_active",
			  FrontendStateValue(frontendCallbacksReady, obs_frontend_replay_buffer_active));
	obs_data_set_bool(root, "virtual_camera_active",
			  FrontendStateValue(frontendCallbacksReady, obs_frontend_virtualcam_active));
	obs_data_set_string(root, "lifecycle_state", LifecycleStateName(lifecycleState).toUtf8().constData());
	obs_data_set_int(root, "pending_run_count", pendingRuns.size());

	obs_data_array_t *eventArray = obs_data_array_create();
	for (const auto &event : eventLog) {
		obs_data_t *entry = obs_data_create();
		obs_data_set_int(entry, "timestamp_ms", event.timestampMs);
		obs_data_set_string(entry, "level", event.level.toUtf8().constData());
		obs_data_set_string(entry, "scope", event.scope.toUtf8().constData());
		obs_data_set_string(entry, "message", event.message.toUtf8().constData());
		obs_data_set_string(entry, "macro_id", event.macroId.toUtf8().constData());
		obs_data_set_bool(entry, "success", event.success);
		obs_data_array_push_back(eventArray, entry);
		obs_data_release(entry);
	}
	obs_data_set_array(root, "event_log", eventArray);
	obs_data_array_release(eventArray);

	return root;
}

void SwitchAutomationEngine::HandleFrontendEvent(enum obs_frontend_event event)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		frontendCallbacksReady = true;
		SetLifecycleState(SwitchAutomationLifecycleState::Running);
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
		frontendCallbacksReady = false;
		SetLifecycleState(SwitchAutomationLifecycleState::SceneCollectionSwitch);
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		frontendCallbacksReady = false;
		SetLifecycleState(SwitchAutomationLifecycleState::ShuttingDown);
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
	case OBS_FRONTEND_EVENT_RECORDING_PAUSED:
	case OBS_FRONTEND_EVENT_RECORDING_UNPAUSED:
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STARTED:
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_STOPPED:
	case OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED:
	case OBS_FRONTEND_EVENT_VIRTUALCAM_STARTED:
	case OBS_FRONTEND_EVENT_VIRTUALCAM_STOPPED:
		if (lifecycleState == SwitchAutomationLifecycleState::Running)
			EvaluateAll();
		emit StateChanged();
		break;
	default:
		break;
	}
}

void SwitchAutomationEngine::EvaluateAll()
{
	if (lifecycleState != SwitchAutomationLifecycleState::Running)
		return;

	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	for (auto &macro : document.macros) {
		if (!macro.enabled || macro.paused)
			continue;

		const bool matched = EvaluateMacroCondition(macro, now);
		const bool shouldRun = ShouldRunMacro(macro, matched, now);
		const bool lastMatched = macro.lastConditionMatched;
		macro.lastConditionMatched = matched;
		if (!shouldRun)
			continue;

		QString message;
		if (BeginMacroRun(macro, &message)) {
			macro.lastExecutionMs = now;
			AppendEvent(QStringLiteral("info"), QStringLiteral("macro"),
				    message.isEmpty() ? QStringLiteral("%1 executed").arg(macro.name) : message,
				    macro.id);
		} else if (matched != lastMatched) {
			AppendEvent(QStringLiteral("error"), QStringLiteral("macro"),
				    QStringLiteral("%1 failed to execute").arg(macro.name), macro.id, false);
			emit MacroTriggered(macro.id, false, QStringLiteral("%1 failed to execute").arg(macro.name));
		}
	}

	emit StateChanged();
}
