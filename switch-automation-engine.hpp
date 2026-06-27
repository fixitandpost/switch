#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>

#include <obs-frontend-api.h>

#include "switch-automation-model.hpp"
#include "switch-osc.hpp"

class QNetworkAccessManager;
class QTimer;
class QUdpSocket;
class SwitchCanvasManager;
class SwitchMotionManager;

enum class SwitchAutomationLifecycleState {
	Loading,
	Running,
	SceneCollectionSwitch,
	ShuttingDown,
};

struct SwitchConditionFactoryInfo {
	QString id;
	QString name;
	QString moduleId;
	QString category;
	bool available = true;
	QString unavailableReason;
};

struct SwitchActionFactoryInfo {
	QString id;
	QString name;
	QString moduleId;
	QString category;
	bool available = true;
	QString unavailableReason;
};

struct SwitchAutomationEventRecord {
	qint64 timestampMs = 0;
	QString level;
	QString scope;
	QString message;
	QString macroId;
	bool success = true;
};

class SwitchConditionFactory {
public:
	static QVector<SwitchConditionFactoryInfo> Types(bool includeUnavailable = true);
	static QString NameForId(const QString &id);
	static bool IsAvailable(const QString &id);
};

class SwitchActionFactory {
public:
	static QVector<SwitchActionFactoryInfo> Types(bool includeUnavailable = true);
	static QString NameForId(const QString &id);
	static bool IsAvailable(const QString &id);
};

class SwitchAutomationEngine : public QObject {
	Q_OBJECT

public:
	explicit SwitchAutomationEngine(SwitchCanvasManager *canvasManager, QObject *parent = nullptr);
	explicit SwitchAutomationEngine(SwitchCanvasManager *canvasManager, SwitchMotionManager *motionManager,
					QObject *parent = nullptr);
	~SwitchAutomationEngine() override;

	obs_data_t *SaveState() const;
	void LoadState(obs_data_t *data);
	void Reset();

	const SwitchAutomationDocument &Document() const;
	QVector<SwitchMacroDescriptor> Macros() const;
	SwitchMacroDescriptor MacroById(const QString &macroId) const;
	SwitchAutomationMacro MacroDefinitionById(const QString &macroId) const;
	QHash<QString, QString> Variables() const;
	QVector<SwitchAutomationQueue> Queues() const;
	QVector<SwitchAutomationConnection> Connections() const;
	QVector<SwitchAutomationEventRecord> EventLog() const;
	SwitchAutomationLifecycleState LifecycleState() const;

	QString CreateMacro(const QString &suggestedName = QString());
	bool DeleteMacro(const QString &macroId);
	bool UpdateMacro(const SwitchMacroDescriptor &macro);
	bool UpsertMacroDefinition(const SwitchAutomationMacro &macro, QString *effectiveId = nullptr);
	bool UpsertConnection(const SwitchAutomationConnection &connection, QString *effectiveId = nullptr);
	bool DeleteConnection(const QString &connectionId);
	SwitchAutomationConnection ConnectionById(const QString &connectionId) const;
	bool TestConnection(const QString &connectionId, QString *message = nullptr);
	bool SetMacroPaused(const QString &macroId, bool paused);
	bool SetMacroEnabled(const QString &macroId, bool enabled);
	bool TriggerMacro(const QString &macroId, QString *message = nullptr);

	bool SetVariable(const QString &key, const QString &value);
	bool RemoveVariable(const QString &key);
	bool ClearQueue(const QString &queueId);
	bool SetLifecycleState(SwitchAutomationLifecycleState state);

	obs_data_t *BuildStateData() const;
	void HandleFrontendEvent(enum obs_frontend_event event);

signals:
	void StateChanged();
	void MacroTriggered(const QString &macroId, bool success, const QString &message);
	void MacroPaused(const QString &macroId, bool paused);
	void VariableChanged(const QString &key, const QString &value);
	void QueueChanged(const QString &queueId);
	void ConnectionChanged(const QString &connectionId);
	void ErrorRaised(const QString &scope, const QString &message);

private slots:
	void EvaluateAll();

private:
	struct PendingMacroRun {
		QString runId;
		QString macroId;
		QVector<SwitchAutomationSegment> actions;
		int nextActionIndex = 0;
		QHash<QString, QString> properties;
		QString lastMessage;
	};

	struct OscConnectionRuntime {
		QUdpSocket *socket = nullptr;
		quint64 sequence = 0;
		qint64 lastReceivedMs = 0;
		QString address;
		QVector<SwitchOscArgument> arguments;
		QString senderHost;
		quint16 senderPort = 0;
		QString lastError;
	};

	QString NextMacroName(const QString &suggestedName) const;
	int FindMacroIndex(const QString &macroId) const;
	int FindConnectionIndex(const QString &connectionId) const;
	QString ResolveTemplate(const QString &value, const QHash<QString, QString> &properties = {}) const;
	SwitchMacroDescriptor LegacyDescriptorFromMacro(const SwitchAutomationMacro &macro) const;
	void ApplyLegacyDescriptorToMacro(SwitchAutomationMacro &macro, const SwitchMacroDescriptor &descriptor) const;
	bool EvaluateSegmentCondition(const SwitchAutomationSegment &segment, const SwitchAutomationMacro &macro, qint64 now);
	bool EvaluateMacroCondition(SwitchAutomationMacro &macro, qint64 now);
	bool ShouldRunMacro(const SwitchAutomationMacro &macro, bool conditionMatched, qint64 now) const;
	bool BeginMacroRun(SwitchAutomationMacro &macro, QString *message = nullptr);
	void ContinuePendingRun(const QString &runId);
	bool ExecuteActionSegment(const SwitchAutomationMacro &macro, const SwitchAutomationSegment &segment,
				  QHash<QString, QString> &properties, QString *message, int *nextDelayMs, bool *continueExecution);
	void AppendEvent(const QString &level, const QString &scope, const QString &message, const QString &macroId = {},
			 bool success = true);
	void ClearPendingRuns();
	void ReconfigureOscConnections();
	void StopOscConnections();
	void StartOscConnection(const SwitchAutomationConnection &connection);
	void HandleOscReadyRead(const QString &connectionId);
	QHash<QString, QString> BuildOscTriggerProperties(const QString &connectionId) const;
	QString ResolveOscConnectionId(const QVariantMap &config) const;

	SwitchCanvasManager *canvasManager = nullptr;
	SwitchMotionManager *motionManager = nullptr;
	QTimer *evaluationTimer = nullptr;
	QNetworkAccessManager *networkManager = nullptr;
	SwitchAutomationDocument document;
	QVector<SwitchAutomationEventRecord> eventLog;
	QHash<QString, PendingMacroRun> pendingRuns;
	QHash<QString, OscConnectionRuntime> oscRuntime;
	QHash<QString, quint64> oscConditionLastSeen;
	QHash<QString, QHash<QString, QString>> pendingTriggerProperties;
	SwitchAutomationLifecycleState lifecycleState = SwitchAutomationLifecycleState::Loading;
	bool frontendCallbacksReady = false;
};
