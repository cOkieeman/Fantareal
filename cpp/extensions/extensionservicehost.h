#pragma once

#include "pythonruntimemanager.h"

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QProcess>
#include <QQueue>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>

class ExtensionServiceHost final : public QObject {
    Q_OBJECT

public:
    explicit ExtensionServiceHost(QString extensionRoot, QObject* parent = nullptr);
    ExtensionServiceHost(
        QString extensionRoot,
        QString trustedTestProgram,
        QStringList trustedTestArguments,
        int testRequestTimeoutMs,
        QObject* parent = nullptr);
    ~ExtensionServiceHost() override;

    void invoke(
        const QString& hostRequestId,
        const QString& extensionId,
        const QString& sessionId,
        const QString& packageRoot,
        const QString& packageDigest,
        const QString& serviceModule,
        const QString& serviceLockfile,
        const QString& workspaceRoot,
        const QString& locale,
        const QString& method,
        const QVariantMap& params);
    void closeSession(const QString& extensionId, const QString& sessionId);

signals:
    void responseReady(
        const QString& hostRequestId,
        bool ok,
        const QVariant& result,
        const QString& errorCode,
        const QString& errorMessage);

private:
    enum class State {
        Idle,
        PreparingEnvironment,
        Starting,
        Initializing,
        HealthChecking,
        Ready,
        ShuttingDown,
    };

    enum class RpcKind {
        Initialize,
        Health,
        Invoke,
        Shutdown,
    };

    struct Invocation {
        QString hostRequestId;
        QString method;
        QVariantMap params;
    };

    struct PendingRpc {
        RpcKind kind;
        QString hostRequestId;
    };

    bool sessionMatches(const QString& extensionId, const QString& sessionId) const;
    bool validateInvocation(
        const QString& method,
        const QVariantMap& params,
        const QString& workspaceRoot,
        QString* code,
        QString* error) const;
    bool validateValuePaths(const QVariant& value, const QString& workspaceRoot) const;
    QVariant normalizeValuePaths(const QVariant& value, const QString& workspaceRoot) const;
    void beginSession(
        const QString& extensionId,
        const QString& sessionId,
        const QString& packageRoot,
        const QString& packageDigest,
        const QString& serviceModule,
        const QString& serviceLockfile,
        const QString& workspaceRoot,
        const QString& locale);
    void handleEnvironmentReady(const QString& requestId, const QString& pythonExecutable);
    void handleEnvironmentFailed(const QString& requestId, const QString& code, const QString& error);
    void startProcess(const QString& pythonExecutable);
    void handleStarted();
    void handleStdout();
    void handleStderr();
    void handleFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void handleProcessError(QProcess::ProcessError error);
    void handleLine(const QByteArray& line);
    qint64 sendRpc(RpcKind kind, const QString& method, const QJsonObject& params, const QString& hostRequestId = {});
    void handleRpcResult(qint64 id, const QJsonValue& result, const QJsonObject& error);
    void armTimeout(qint64 id, int timeoutMs);
    void flushInvocations();
    void failInvocation(const Invocation& invocation, const QString& code, const QString& error);
    void failAll(const QString& code, const QString& error);
    void protocolFailure(const QString& code, const QString& error);
    void abortCurrentSession();
    void stopProcess(bool graceful);
    void clearSession();
    void appendServiceLog(const QByteArray& payload);

    QString extensionRoot_;
    PythonRuntimeManager runtimeManager_;
    QProcess process_;
    State state_ = State::Idle;

    QString extensionId_;
    QString sessionId_;
    QString packageRoot_;
    QString packageDigest_;
    QString serviceModule_;
    QString serviceLockfile_;
    QString workspaceRoot_;
    QString locale_;
    QString runtimeRequestId_;

    QQueue<Invocation> invocations_;
    QHash<qint64, PendingRpc> pending_;
    qint64 nextRpcId_ = 1;
    QByteArray stdoutBuffer_;
    QByteArray stderrBuffer_;
    bool stopping_ = false;

    QString trustedTestProgram_;
    QStringList trustedTestArguments_;
    int requestTimeoutMs_ = 60'000;
};
