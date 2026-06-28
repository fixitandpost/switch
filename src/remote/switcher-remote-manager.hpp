#pragma once

#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QJsonObject>
#include <QSize>
#include <QString>
#include <QVector>

#include <obs-frontend-api.h>

#include "switcher-remote-renderer.hpp"

class QTcpServer;
class QTcpSocket;
class QTimer;
class SwitcherWorkspaceDock;

class SwitcherRemoteManager : public QObject {
	Q_OBJECT

public:
	enum class Status {
		Disabled,
		Starting,
		Running,
		Error,
	};

	static SwitcherRemoteManager *Instance();

	void SetWorkspace(SwitcherWorkspaceDock *workspaceDock);

	obs_data_t *SaveState() const;
	void LoadState(obs_data_t *data);

	void HandleFrontendEvent(enum obs_frontend_event event);
	void HandleSourceRemoved(obs_source_t *source);
	void Shutdown();

	bool Enabled() const { return enabled; }
	void SetEnabled(bool value);

	bool AutoStart() const { return autoStart; }
	void SetAutoStart(bool value);

	QSize RenderSize() const { return renderSize; }
	void SetRenderSize(const QSize &size);

	int TargetFps() const { return targetFps; }
	void SetTargetFps(int fps);

	QString Token() const { return token; }
	void RegenerateToken();

	QString Url() const;
	QString StatusText() const;

	int SelectedSlotIndex() const { return selectedSlotIndex; }
	bool SelectPreviewSlot(int slotIndex);
	bool Cut();
	bool AutoTransition();
	void Restart();

	obs_data_t *BuildRemoteStateData() const;

signals:
	void StateChanged();

public slots:
	void WorkspaceStateChanged();

private slots:
	void StartIfNeeded();
	void Stop();
	void RenderAndSendFrame();
	void AcceptHelperConnection();
	void ReadHelperMessage();
	void HelperDisconnected();
	void HelperProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
	void HelperProcessReadyReadStdout();
	void HelperProcessReadyReadStderr();
	void HelperProcessError(QProcess::ProcessError error);

private:
	explicit SwitcherRemoteManager(QObject *parent = nullptr);

	void EnsureServer();
	void StartHelper();
	void StopHelper(bool keepEnabled);
	void UpdateStatus(Status nextStatus, const QString &detail = {});
	void UpdateRenderTimer();
	void SendStateSnapshot(const char *eventName = "RemoteStateChanged");
	void SendMessage(const QJsonObject &message, const QByteArray &payload = {});
	void ProcessCommand(const QJsonObject &message);
	bool TakeSelectedSlot(bool triggerAuto);
	void SyncSelectedSlotFromObs();
	bool SourceInTree(obs_source_t *root, obs_source_t *target) const;
	QString DetectLanAddress() const;
	QString HelperPath() const;
	QJsonObject BuildStateJson() const;
	OBSSource SelectedSource() const;
	int ResolveProgramSlotIndex() const;
	int ResolvePreviewSlotIndex() const;

	QPointer<SwitcherWorkspaceDock> workspace;
	QTcpServer *server = nullptr;
	QTcpSocket *socket = nullptr;
	QProcess *helperProcess = nullptr;
	QTimer *renderTimer = nullptr;
	QByteArray incomingBuffer;
	SwitcherRemoteRenderer renderer;
	QVector<qint64> restartTimestamps;
	bool enabled = false;
	bool autoStart = false;
	bool helperStopRequested = false;
	QString token;
	QSize renderSize = QSize(1280, 720);
	int targetFps = 10;
	int httpPort = 8899;
	int selectedSlotIndex = -1;
	bool frontendShuttingDown = false;
	bool frameBackpressureActive = false;
	Status status = Status::Disabled;
	QString statusDetail;
	quint16 controlPort = 0;
};
