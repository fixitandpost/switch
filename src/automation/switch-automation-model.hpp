#pragma once

#include <QHash>
#include <QVariantMap>
#include <QVector>
#include <QString>
#include <QStringList>

#include <obs-data.h>

struct SwitchMacroDescriptor {
	QString id;
	QString name;
	QString group;
	bool enabled = true;
	bool paused = false;
	QString runMode = QStringLiteral("on_change");
	QString triggerType = QStringLiteral("manual");
	int intervalMs = 5000;
	bool desiredState = true;
	QString triggerConnectionId;
	QString triggerConnectionName;
	QString triggerSceneUuid;
	QString triggerSceneName;
	QString triggerValueKey;
	QString triggerValue;
	QString actionType = QStringLiteral("switch_program_scene");
	QString actionConnectionId;
	QString actionConnectionName;
	QString actionSceneUuid;
	QString actionSceneName;
	QString actionValueKey;
	QString actionValue;
	int actionDelayMs = 0;
	bool lastConditionMatched = false;
	qint64 lastExecutionMs = 0;
};

struct SwitchAutomationSegment {
	QString id;
	QString typeId;
	QString label;
	bool enabled = true;
	bool collapsed = false;
	QString logicOp = QStringLiteral("and");
	int durationMs = 0;
	QVariantMap config;
	bool lastResult = false;
};

struct SwitchAutomationQueue {
	QString id;
	QString name;
	QString mode = QStringLiteral("sequence");
	QVector<QString> actionSegmentIds;
	int nextIndex = 0;
	bool enabled = true;
};

struct SwitchAutomationConnection {
	QString id;
	QString typeId;
	QString name;
	bool enabled = true;
	bool available = true;
	QString status;
	QVariantMap config;
};

struct SwitchAutomationDockPreset {
	QString id;
	QString name;
	QString macroId;
	bool registerDock = false;
	bool hasRunButton = true;
	bool hasPauseButton = true;
	bool hasStatusLabel = false;
	bool highlightIfExecuted = true;
};

struct SwitchAutomationMacro {
	QString id;
	QString name;
	QString group;
	bool enabled = true;
	bool paused = false;
	QString runPolicy = QStringLiteral("on_change");
	QString startupPolicy = QStringLiteral("wait_for_change");
	int repeatIntervalMs = 5000;
	QVariantMap hotkeys;
	QVariantMap dockConfig;
	QHash<QString, QString> properties;
	QVector<SwitchAutomationSegment> conditions;
	QVector<SwitchAutomationSegment> actions;
	bool lastConditionMatched = false;
	qint64 lastExecutionMs = 0;
};

struct SwitchAutomationDocument {
	int settingsVersion = 2;
	QVector<SwitchAutomationMacro> macros;
	QHash<QString, QString> variables;
	QVector<SwitchAutomationQueue> queues;
	QVector<SwitchAutomationConnection> connections;
	QVector<SwitchAutomationDockPreset> dockPresets;
	QString engineState = QStringLiteral("loading");
};

QString SwitchCreateAutomationId(const QString &prefix);
QString SwitchResolveAutomationTemplate(const QString &value, const QHash<QString, QString> &variables,
					const QHash<QString, QString> &properties = {});
bool SwitchShouldRunAutomationMacro(const SwitchAutomationMacro &macro, bool conditionMatched, qint64 now);
int SwitchAdvanceAutomationQueue(SwitchAutomationQueue &queue, uint32_t randomValue = 0);
QStringList SwitchValidateAutomationConnection(const SwitchAutomationConnection &connection);
obs_data_t *SwitchAutomationMacroToObsData(const SwitchAutomationMacro &macro);
SwitchAutomationMacro SwitchAutomationMacroFromObsData(obs_data_t *data);
obs_data_t *SwitchAutomationDocumentToObsData(const SwitchAutomationDocument &document);
SwitchAutomationDocument SwitchAutomationDocumentFromObsData(obs_data_t *data);
