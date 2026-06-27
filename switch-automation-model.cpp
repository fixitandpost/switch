#include "switch-automation-model.hpp"

#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUuid>

#include <algorithm>

namespace {
constexpr int kAutomationSettingsVersion = 2;

QJsonObject VariantMapToJsonObject(const QVariantMap &map)
{
	return QJsonObject::fromVariantMap(map);
}

QVariantMap JsonObjectToVariantMap(const QString &json)
{
	if (json.trimmed().isEmpty())
		return {};

	const auto document = QJsonDocument::fromJson(json.toUtf8());
	if (!document.isObject())
		return {};
	return document.object().toVariantMap();
}

QString VariantMapToJson(const QVariantMap &map)
{
	if (map.isEmpty())
		return {};
	return QString::fromUtf8(QJsonDocument(VariantMapToJsonObject(map)).toJson(QJsonDocument::Compact));
}

obs_data_t *HashToObsData(const QHash<QString, QString> &values)
{
	obs_data_t *data = obs_data_create();
	for (auto it = values.begin(); it != values.end(); ++it)
		obs_data_set_string(data, it.key().toUtf8().constData(), it.value().toUtf8().constData());
	return data;
}

QHash<QString, QString> HashFromObsData(obs_data_t *data)
{
	QHash<QString, QString> values;
	if (!data)
		return values;

	obs_data_item_t *item = obs_data_first(data);
	while (item) {
		const QString key = QString::fromUtf8(obs_data_item_get_name(item));
		if (!key.isEmpty())
			values.insert(key, QString::fromUtf8(obs_data_item_get_string(item)));
		if (!obs_data_item_next(&item))
			break;
	}
	if (item)
		obs_data_item_release(&item);
	return values;
}

SwitchAutomationSegment LoadSegment(obs_data_t *data)
{
	SwitchAutomationSegment segment;
	if (!data)
		return segment;

	segment.id = QString::fromUtf8(obs_data_get_string(data, "id"));
	segment.typeId = QString::fromUtf8(obs_data_get_string(data, "type_id"));
	segment.label = QString::fromUtf8(obs_data_get_string(data, "label"));
	segment.enabled = obs_data_get_bool(data, "enabled");
	segment.collapsed = obs_data_get_bool(data, "collapsed");
	segment.logicOp = QString::fromUtf8(obs_data_get_string(data, "logic"));
	if (segment.logicOp.isEmpty())
		segment.logicOp = QStringLiteral("and");
	segment.durationMs = int(obs_data_get_int(data, "duration_ms"));
	segment.lastResult = obs_data_get_bool(data, "last_result");
	segment.config = JsonObjectToVariantMap(QString::fromUtf8(obs_data_get_string(data, "config_json")));
	if (segment.id.isEmpty())
		segment.id = SwitchCreateAutomationId(QStringLiteral("segment"));
	return segment;
}

void SaveSegment(obs_data_array_t *array, const SwitchAutomationSegment &segment)
{
	obs_data_t *entry = obs_data_create();
	obs_data_set_string(entry, "id", segment.id.toUtf8().constData());
	obs_data_set_string(entry, "type_id", segment.typeId.toUtf8().constData());
	obs_data_set_string(entry, "label", segment.label.toUtf8().constData());
	obs_data_set_bool(entry, "enabled", segment.enabled);
	obs_data_set_bool(entry, "collapsed", segment.collapsed);
	obs_data_set_string(entry, "logic", segment.logicOp.toUtf8().constData());
	obs_data_set_int(entry, "duration_ms", segment.durationMs);
	obs_data_set_bool(entry, "last_result", segment.lastResult);
	obs_data_set_string(entry, "config_json", VariantMapToJson(segment.config).toUtf8().constData());
	obs_data_array_push_back(array, entry);
	obs_data_release(entry);
}

SwitchAutomationMacro LegacyMacroFromObsData(obs_data_t *data)
{
	SwitchAutomationMacro macro;
	if (!data)
		return macro;

	macro.id = QString::fromUtf8(obs_data_get_string(data, "id"));
	macro.name = QString::fromUtf8(obs_data_get_string(data, "name"));
	macro.group = QString::fromUtf8(obs_data_get_string(data, "group"));
	macro.enabled = obs_data_has_user_value(data, "enabled") ? obs_data_get_bool(data, "enabled") : true;
	macro.paused = obs_data_get_bool(data, "paused");
	macro.runPolicy = QString::fromUtf8(obs_data_get_string(data, "run_mode"));
	if (macro.runPolicy.isEmpty())
		macro.runPolicy = QStringLiteral("on_change");
	macro.repeatIntervalMs = int(obs_data_get_int(data, "interval_ms"));
	if (macro.repeatIntervalMs <= 0)
		macro.repeatIntervalMs = 5000;
	macro.lastConditionMatched = obs_data_get_bool(data, "last_condition_matched");
	macro.lastExecutionMs = obs_data_get_int(data, "last_execution_ms");

	SwitchAutomationSegment condition;
	condition.id = SwitchCreateAutomationId(QStringLiteral("condition"));
	condition.typeId = QString::fromUtf8(obs_data_get_string(data, "trigger_type"));
	if (condition.typeId.isEmpty())
		condition.typeId = QStringLiteral("manual");
	condition.config.insert(QStringLiteral("desiredState"), obs_data_get_bool(data, "desired_state"));
	condition.config.insert(QStringLiteral("intervalMs"), int(obs_data_get_int(data, "interval_ms")));
	condition.config.insert(QStringLiteral("sceneUuid"), QString::fromUtf8(obs_data_get_string(data, "trigger_scene_uuid")));
	condition.config.insert(QStringLiteral("sceneName"), QString::fromUtf8(obs_data_get_string(data, "trigger_scene_name")));
	macro.conditions.push_back(condition);

	SwitchAutomationSegment action;
	action.id = SwitchCreateAutomationId(QStringLiteral("action"));
	action.typeId = QString::fromUtf8(obs_data_get_string(data, "action_type"));
	if (action.typeId.isEmpty())
		action.typeId = QStringLiteral("switch_program_scene");
	action.config.insert(QStringLiteral("sceneUuid"), QString::fromUtf8(obs_data_get_string(data, "action_scene_uuid")));
	action.config.insert(QStringLiteral("sceneName"), QString::fromUtf8(obs_data_get_string(data, "action_scene_name")));
	action.config.insert(QStringLiteral("valueKey"), QString::fromUtf8(obs_data_get_string(data, "action_value_key")));
	action.config.insert(QStringLiteral("value"), QString::fromUtf8(obs_data_get_string(data, "action_value")));
	action.config.insert(QStringLiteral("delayMs"), int(obs_data_get_int(data, "action_delay_ms")));
	macro.actions.push_back(action);

	if (macro.id.isEmpty())
		macro.id = SwitchCreateAutomationId(QStringLiteral("macro"));
	if (macro.name.trimmed().isEmpty())
		macro.name = QStringLiteral("Macro");
	return macro;
}

SwitchAutomationMacro LoadMacro(obs_data_t *data)
{
	SwitchAutomationMacro macro;
	if (!data)
		return macro;

	if (!obs_data_has_user_value(data, "conditions") && obs_data_has_user_value(data, "trigger_type"))
		return LegacyMacroFromObsData(data);

	macro.id = QString::fromUtf8(obs_data_get_string(data, "id"));
	macro.name = QString::fromUtf8(obs_data_get_string(data, "name"));
	macro.group = QString::fromUtf8(obs_data_get_string(data, "group"));
	macro.enabled = obs_data_has_user_value(data, "enabled") ? obs_data_get_bool(data, "enabled") : true;
	macro.paused = obs_data_get_bool(data, "paused");
	macro.runPolicy = QString::fromUtf8(obs_data_get_string(data, "run_policy"));
	if (macro.runPolicy.isEmpty())
		macro.runPolicy = QStringLiteral("on_change");
	macro.startupPolicy = QString::fromUtf8(obs_data_get_string(data, "startup_policy"));
	if (macro.startupPolicy.isEmpty())
		macro.startupPolicy = QStringLiteral("wait_for_change");
	macro.repeatIntervalMs = int(obs_data_get_int(data, "repeat_interval_ms"));
	if (macro.repeatIntervalMs <= 0)
		macro.repeatIntervalMs = 5000;
	macro.hotkeys = JsonObjectToVariantMap(QString::fromUtf8(obs_data_get_string(data, "hotkeys_json")));
	macro.dockConfig = JsonObjectToVariantMap(QString::fromUtf8(obs_data_get_string(data, "dock_config_json")));
	macro.lastConditionMatched = obs_data_get_bool(data, "last_condition_matched");
	macro.lastExecutionMs = obs_data_get_int(data, "last_execution_ms");

	if (obs_data_t *properties = obs_data_get_obj(data, "properties")) {
		macro.properties = HashFromObsData(properties);
		obs_data_release(properties);
	}

	if (obs_data_array_t *conditions = obs_data_get_array(data, "conditions")) {
		const size_t count = obs_data_array_count(conditions);
		for (size_t index = 0; index < count; ++index) {
			obs_data_t *entry = obs_data_array_item(conditions, index);
			macro.conditions.push_back(LoadSegment(entry));
			obs_data_release(entry);
		}
		obs_data_array_release(conditions);
	}

	if (obs_data_array_t *actions = obs_data_get_array(data, "actions")) {
		const size_t count = obs_data_array_count(actions);
		for (size_t index = 0; index < count; ++index) {
			obs_data_t *entry = obs_data_array_item(actions, index);
			macro.actions.push_back(LoadSegment(entry));
			obs_data_release(entry);
		}
		obs_data_array_release(actions);
	}

	if (macro.id.isEmpty())
		macro.id = SwitchCreateAutomationId(QStringLiteral("macro"));
	if (macro.name.trimmed().isEmpty())
		macro.name = QStringLiteral("Macro");
	if (macro.conditions.isEmpty()) {
		SwitchAutomationSegment segment;
		segment.id = SwitchCreateAutomationId(QStringLiteral("condition"));
		segment.typeId = QStringLiteral("manual");
		macro.conditions.push_back(segment);
	}
	if (macro.actions.isEmpty()) {
		SwitchAutomationSegment segment;
		segment.id = SwitchCreateAutomationId(QStringLiteral("action"));
		segment.typeId = QStringLiteral("switch_program_scene");
		macro.actions.push_back(segment);
	}
	return macro;
}

void SaveMacro(obs_data_array_t *array, const SwitchAutomationMacro &macro)
{
	obs_data_t *entry = SwitchAutomationMacroToObsData(macro);
	obs_data_array_push_back(array, entry);
	obs_data_release(entry);
}

SwitchAutomationQueue LoadQueue(obs_data_t *data)
{
	SwitchAutomationQueue queue;
	if (!data)
		return queue;

	queue.id = QString::fromUtf8(obs_data_get_string(data, "id"));
	queue.name = QString::fromUtf8(obs_data_get_string(data, "name"));
	queue.mode = QString::fromUtf8(obs_data_get_string(data, "mode"));
	if (queue.mode.isEmpty())
		queue.mode = QStringLiteral("sequence");
	queue.nextIndex = int(obs_data_get_int(data, "next_index"));
	queue.enabled = obs_data_has_user_value(data, "enabled") ? obs_data_get_bool(data, "enabled") : true;

	if (obs_data_array_t *items = obs_data_get_array(data, "action_segment_ids")) {
		const size_t count = obs_data_array_count(items);
		for (size_t index = 0; index < count; ++index) {
			obs_data_t *entry = obs_data_array_item(items, index);
			queue.actionSegmentIds.push_back(QString::fromUtf8(obs_data_get_string(entry, "id")));
			obs_data_release(entry);
		}
		obs_data_array_release(items);
	}

	if (queue.id.isEmpty())
		queue.id = SwitchCreateAutomationId(QStringLiteral("queue"));
	return queue;
}

void SaveQueue(obs_data_array_t *array, const SwitchAutomationQueue &queue)
{
	obs_data_t *entry = obs_data_create();
	obs_data_set_string(entry, "id", queue.id.toUtf8().constData());
	obs_data_set_string(entry, "name", queue.name.toUtf8().constData());
	obs_data_set_string(entry, "mode", queue.mode.toUtf8().constData());
	obs_data_set_int(entry, "next_index", queue.nextIndex);
	obs_data_set_bool(entry, "enabled", queue.enabled);

	obs_data_array_t *items = obs_data_array_create();
	for (const auto &id : queue.actionSegmentIds) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "id", id.toUtf8().constData());
		obs_data_array_push_back(items, item);
		obs_data_release(item);
	}
	obs_data_set_array(entry, "action_segment_ids", items);
	obs_data_array_release(items);

	obs_data_array_push_back(array, entry);
	obs_data_release(entry);
}

SwitchAutomationConnection LoadConnection(obs_data_t *data)
{
	SwitchAutomationConnection connection;
	if (!data)
		return connection;

	connection.id = QString::fromUtf8(obs_data_get_string(data, "id"));
	connection.typeId = QString::fromUtf8(obs_data_get_string(data, "type_id"));
	connection.name = QString::fromUtf8(obs_data_get_string(data, "name"));
	connection.enabled = obs_data_has_user_value(data, "enabled") ? obs_data_get_bool(data, "enabled") : true;
	connection.available = !obs_data_has_user_value(data, "available") || obs_data_get_bool(data, "available");
	connection.status = QString::fromUtf8(obs_data_get_string(data, "status"));
	connection.config = JsonObjectToVariantMap(QString::fromUtf8(obs_data_get_string(data, "config_json")));

	if (connection.id.isEmpty())
		connection.id = SwitchCreateAutomationId(QStringLiteral("connection"));
	return connection;
}

void SaveConnection(obs_data_array_t *array, const SwitchAutomationConnection &connection)
{
	obs_data_t *entry = obs_data_create();
	obs_data_set_string(entry, "id", connection.id.toUtf8().constData());
	obs_data_set_string(entry, "type_id", connection.typeId.toUtf8().constData());
	obs_data_set_string(entry, "name", connection.name.toUtf8().constData());
	obs_data_set_bool(entry, "enabled", connection.enabled);
	obs_data_set_bool(entry, "available", connection.available);
	obs_data_set_string(entry, "status", connection.status.toUtf8().constData());
	obs_data_set_string(entry, "config_json", VariantMapToJson(connection.config).toUtf8().constData());
	obs_data_array_push_back(array, entry);
	obs_data_release(entry);
}

SwitchAutomationDockPreset LoadDockPreset(obs_data_t *data)
{
	SwitchAutomationDockPreset preset;
	if (!data)
		return preset;

	preset.id = QString::fromUtf8(obs_data_get_string(data, "id"));
	preset.name = QString::fromUtf8(obs_data_get_string(data, "name"));
	preset.macroId = QString::fromUtf8(obs_data_get_string(data, "macro_id"));
	preset.registerDock = obs_data_get_bool(data, "register_dock");
	preset.hasRunButton = !obs_data_has_user_value(data, "has_run_button") || obs_data_get_bool(data, "has_run_button");
	preset.hasPauseButton =
		!obs_data_has_user_value(data, "has_pause_button") || obs_data_get_bool(data, "has_pause_button");
	preset.hasStatusLabel = obs_data_get_bool(data, "has_status_label");
	preset.highlightIfExecuted = !obs_data_has_user_value(data, "highlight_if_executed") ||
				     obs_data_get_bool(data, "highlight_if_executed");

	if (preset.id.isEmpty())
		preset.id = SwitchCreateAutomationId(QStringLiteral("dock"));
	return preset;
}

void SaveDockPreset(obs_data_array_t *array, const SwitchAutomationDockPreset &preset)
{
	obs_data_t *entry = obs_data_create();
	obs_data_set_string(entry, "id", preset.id.toUtf8().constData());
	obs_data_set_string(entry, "name", preset.name.toUtf8().constData());
	obs_data_set_string(entry, "macro_id", preset.macroId.toUtf8().constData());
	obs_data_set_bool(entry, "register_dock", preset.registerDock);
	obs_data_set_bool(entry, "has_run_button", preset.hasRunButton);
	obs_data_set_bool(entry, "has_pause_button", preset.hasPauseButton);
	obs_data_set_bool(entry, "has_status_label", preset.hasStatusLabel);
	obs_data_set_bool(entry, "highlight_if_executed", preset.highlightIfExecuted);
	obs_data_array_push_back(array, entry);
	obs_data_release(entry);
}
} // namespace

QString SwitchCreateAutomationId(const QString &prefix)
{
	return QStringLiteral("%1-%2").arg(prefix, QUuid::createUuid().toString(QUuid::WithoutBraces));
}

QString SwitchResolveAutomationTemplate(const QString &value, const QHash<QString, QString> &variables,
					const QHash<QString, QString> &properties)
{
	QString resolved = value;
	for (auto it = variables.begin(); it != variables.end(); ++it)
		resolved.replace(QStringLiteral("${%1}").arg(it.key()), it.value());
	for (auto it = properties.begin(); it != properties.end(); ++it)
		resolved.replace(QStringLiteral("${macro.%1}").arg(it.key()), it.value());
	return resolved;
}

bool SwitchShouldRunAutomationMacro(const SwitchAutomationMacro &macro, bool conditionMatched, qint64 now)
{
	if (!conditionMatched)
		return false;

	if (macro.runPolicy == QStringLiteral("on_match"))
		return true;
	if (macro.runPolicy == QStringLiteral("repeat"))
		return (now - macro.lastExecutionMs) >= std::max(250, macro.repeatIntervalMs);
	return conditionMatched && !macro.lastConditionMatched;
}

int SwitchAdvanceAutomationQueue(SwitchAutomationQueue &queue, uint32_t randomValue)
{
	if (!queue.enabled || queue.actionSegmentIds.isEmpty())
		return -1;

	const int count = queue.actionSegmentIds.size();
	if (queue.mode == QStringLiteral("random")) {
		const int index = int(randomValue % uint32_t(count));
		queue.nextIndex = (index + 1) % count;
		return index;
	}

	const int index = std::clamp(queue.nextIndex, 0, count - 1);
	queue.nextIndex = (index + 1) % count;
	return index;
}

QStringList SwitchValidateAutomationConnection(const SwitchAutomationConnection &connection)
{
	QStringList errors;
	if (connection.typeId.trimmed().isEmpty())
		errors.push_back(QStringLiteral("Connection type is required"));

	if (connection.typeId == QStringLiteral("http")) {
		const QString urlValue = connection.config.value(QStringLiteral("url")).toString().trimmed();
		if (urlValue.isEmpty()) {
			errors.push_back(QStringLiteral("HTTP connections require a URL"));
		} else {
			const QUrl url(urlValue);
			if (!url.isValid() || url.scheme().isEmpty() || url.host().isEmpty())
				errors.push_back(QStringLiteral("HTTP connection URL must include a scheme and host"));
		}
	}

	if (connection.typeId == QStringLiteral("mqtt")) {
		const QString host = connection.config.value(QStringLiteral("host")).toString().trimmed();
		const int port = connection.config.value(QStringLiteral("port")).toInt();
		if (host.isEmpty())
			errors.push_back(QStringLiteral("MQTT connections require a host"));
		if (port <= 0)
			errors.push_back(QStringLiteral("MQTT connections require a valid port"));
	}

	if (connection.typeId == QStringLiteral("twitch")) {
		const QString token = connection.config.value(QStringLiteral("token")).toString().trimmed();
		const QString channelId = connection.config.value(QStringLiteral("channelId")).toString().trimmed();
		if (token.isEmpty())
			errors.push_back(QStringLiteral("Twitch connections require an access token"));
		if (channelId.isEmpty())
			errors.push_back(QStringLiteral("Twitch connections require a channel ID"));
	}

	if (connection.typeId == QStringLiteral("websocket")) {
		const QString urlValue = connection.config.value(QStringLiteral("url")).toString().trimmed();
		if (!urlValue.isEmpty()) {
			const QUrl url(urlValue);
			if (!url.isValid() || url.scheme().isEmpty() || url.host().isEmpty())
				errors.push_back(QStringLiteral("WebSocket connection URL must include a scheme and host"));
		} else if (connection.config.value(QStringLiteral("port")).toInt() <= 0) {
			errors.push_back(QStringLiteral("WebSocket connections require a URL or valid port"));
		}
	}

	if (connection.typeId == QStringLiteral("file")) {
		const QString path = connection.config.value(QStringLiteral("path")).toString().trimmed();
		if (path.isEmpty())
			errors.push_back(QStringLiteral("File connections require a path"));
		else if (!QFileInfo::exists(path))
			errors.push_back(QStringLiteral("Configured file path does not exist"));
	}

	if (connection.typeId == QStringLiteral("osc")) {
		const QString mode = connection.config.value(QStringLiteral("mode")).toString().trimmed().isEmpty()
					     ? QStringLiteral("duplex")
					     : connection.config.value(QStringLiteral("mode")).toString().trimmed();
		const QString remoteHost = connection.config.value(QStringLiteral("remoteHost")).toString().trimmed();
		const int remotePort = connection.config.value(QStringLiteral("remotePort")).toInt();
		const int listenPort = connection.config.value(QStringLiteral("listenPort")).toInt();

		if (mode == QStringLiteral("send") || mode == QStringLiteral("duplex")) {
			if (remoteHost.isEmpty())
				errors.push_back(QStringLiteral("OSC send connections require a remote host"));
			if (remotePort <= 0 || remotePort > 65535)
				errors.push_back(QStringLiteral("OSC send connections require a valid remote port"));
		}

		if (mode == QStringLiteral("receive") || mode == QStringLiteral("duplex")) {
			if (listenPort <= 0 || listenPort > 65535)
				errors.push_back(QStringLiteral("OSC receive connections require a valid listen port"));
		}
	}

	return errors;
}

obs_data_t *SwitchAutomationMacroToObsData(const SwitchAutomationMacro &macro)
{
	obs_data_t *entry = obs_data_create();
	obs_data_set_string(entry, "id", macro.id.toUtf8().constData());
	obs_data_set_string(entry, "name", macro.name.toUtf8().constData());
	obs_data_set_string(entry, "group", macro.group.toUtf8().constData());
	obs_data_set_bool(entry, "enabled", macro.enabled);
	obs_data_set_bool(entry, "paused", macro.paused);
	obs_data_set_string(entry, "run_policy", macro.runPolicy.toUtf8().constData());
	obs_data_set_string(entry, "startup_policy", macro.startupPolicy.toUtf8().constData());
	obs_data_set_int(entry, "repeat_interval_ms", macro.repeatIntervalMs);
	obs_data_set_string(entry, "hotkeys_json", VariantMapToJson(macro.hotkeys).toUtf8().constData());
	obs_data_set_string(entry, "dock_config_json", VariantMapToJson(macro.dockConfig).toUtf8().constData());
	obs_data_set_bool(entry, "last_condition_matched", macro.lastConditionMatched);
	obs_data_set_int(entry, "last_execution_ms", macro.lastExecutionMs);

	obs_data_t *properties = HashToObsData(macro.properties);
	obs_data_set_obj(entry, "properties", properties);
	obs_data_release(properties);

	obs_data_array_t *conditions = obs_data_array_create();
	for (const auto &segment : macro.conditions)
		SaveSegment(conditions, segment);
	obs_data_set_array(entry, "conditions", conditions);
	obs_data_array_release(conditions);

	obs_data_array_t *actions = obs_data_array_create();
	for (const auto &segment : macro.actions)
		SaveSegment(actions, segment);
	obs_data_set_array(entry, "actions", actions);
	obs_data_array_release(actions);

	return entry;
}

SwitchAutomationMacro SwitchAutomationMacroFromObsData(obs_data_t *data)
{
	return LoadMacro(data);
}

obs_data_t *SwitchAutomationDocumentToObsData(const SwitchAutomationDocument &document)
{
	obs_data_t *root = obs_data_create();
	obs_data_set_int(root, "settings_version", document.settingsVersion);
	obs_data_set_string(root, "engine_state", document.engineState.toUtf8().constData());

	obs_data_array_t *macros = obs_data_array_create();
	for (const auto &macro : document.macros)
		SaveMacro(macros, macro);
	obs_data_set_array(root, "macros", macros);
	obs_data_array_release(macros);

	obs_data_t *variables = HashToObsData(document.variables);
	obs_data_set_obj(root, "variables", variables);
	obs_data_release(variables);

	obs_data_array_t *queues = obs_data_array_create();
	for (const auto &queue : document.queues)
		SaveQueue(queues, queue);
	obs_data_set_array(root, "queues", queues);
	obs_data_array_release(queues);

	obs_data_array_t *connections = obs_data_array_create();
	for (const auto &connection : document.connections)
		SaveConnection(connections, connection);
	obs_data_set_array(root, "connections", connections);
	obs_data_array_release(connections);

	obs_data_array_t *dockPresets = obs_data_array_create();
	for (const auto &preset : document.dockPresets)
		SaveDockPreset(dockPresets, preset);
	obs_data_set_array(root, "dock_presets", dockPresets);
	obs_data_array_release(dockPresets);

	return root;
}

SwitchAutomationDocument SwitchAutomationDocumentFromObsData(obs_data_t *data)
{
	SwitchAutomationDocument document;
	document.settingsVersion = kAutomationSettingsVersion;

	if (!data)
		return document;

	const int settingsVersion = int(obs_data_get_int(data, "settings_version"));
	document.engineState = QString::fromUtf8(obs_data_get_string(data, "engine_state"));
	if (document.engineState.isEmpty())
		document.engineState = QStringLiteral("loading");

	if (obs_data_array_t *macros = obs_data_get_array(data, "macros")) {
		const size_t count = obs_data_array_count(macros);
		for (size_t index = 0; index < count; ++index) {
			obs_data_t *entry = obs_data_array_item(macros, index);
			document.macros.push_back(LoadMacro(entry));
			obs_data_release(entry);
		}
		obs_data_array_release(macros);
	}

	if (obs_data_t *variables = obs_data_get_obj(data, "variables")) {
		document.variables = HashFromObsData(variables);
		obs_data_release(variables);
	}

	if (settingsVersion >= kAutomationSettingsVersion) {
		if (obs_data_array_t *queues = obs_data_get_array(data, "queues")) {
			const size_t count = obs_data_array_count(queues);
			for (size_t index = 0; index < count; ++index) {
				obs_data_t *entry = obs_data_array_item(queues, index);
				document.queues.push_back(LoadQueue(entry));
				obs_data_release(entry);
			}
			obs_data_array_release(queues);
		}

		if (obs_data_array_t *connections = obs_data_get_array(data, "connections")) {
			const size_t count = obs_data_array_count(connections);
			for (size_t index = 0; index < count; ++index) {
				obs_data_t *entry = obs_data_array_item(connections, index);
				document.connections.push_back(LoadConnection(entry));
				obs_data_release(entry);
			}
			obs_data_array_release(connections);
		}

		if (obs_data_array_t *dockPresets = obs_data_get_array(data, "dock_presets")) {
			const size_t count = obs_data_array_count(dockPresets);
			for (size_t index = 0; index < count; ++index) {
				obs_data_t *entry = obs_data_array_item(dockPresets, index);
				document.dockPresets.push_back(LoadDockPreset(entry));
				obs_data_release(entry);
			}
			obs_data_array_release(dockPresets);
		}
	}

	document.settingsVersion = kAutomationSettingsVersion;
	return document;
}
