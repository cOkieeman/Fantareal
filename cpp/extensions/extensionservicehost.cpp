#include "extensionservicehost.h"

#include "extensionmanifest.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTimer>

namespace {

constexpr qsizetype kMaxRequestBytes = 1024 * 1024;
constexpr qsizetype kMaxResponseBytes = 8 * 1024 * 1024;
constexpr qsizetype kMaxStderrBufferBytes = 64 * 1024;
constexpr qint64 kMaxLogBytes = 1024 * 1024;

QString processErrorCode(QProcess::ProcessError error) {
    switch (error) {
    case QProcess::FailedToStart:
        return QStringLiteral("service_start_failed");
    case QProcess::Crashed:
        return QStringLiteral("service_crashed");
    case QProcess::Timedout:
        return QStringLiteral("service_process_timeout");
    case QProcess::WriteError:
        return QStringLiteral("service_write_failed");
    case QProcess::ReadError:
        return QStringLiteral("service_read_failed");
    case QProcess::UnknownError:
        return QStringLiteral("service_process_error");
    }
    return QStringLiteral("service_process_error");
}

} // namespace

ExtensionServiceHost::ExtensionServiceHost(QString extensionRoot, QObject* parent)
    : QObject(parent)
    , extensionRoot_(QDir::cleanPath(QFileInfo(extensionRoot).absoluteFilePath()))
    , runtimeManager_(extensionRoot_) {
    connect(&runtimeManager_, &PythonRuntimeManager::environmentReady,
        this, &ExtensionServiceHost::handleEnvironmentReady);
    connect(&runtimeManager_, &PythonRuntimeManager::environmentFailed,
        this, &ExtensionServiceHost::handleEnvironmentFailed);
    connect(&process_, &QProcess::started, this, &ExtensionServiceHost::handleStarted);
    connect(&process_, &QProcess::readyReadStandardOutput, this, &ExtensionServiceHost::handleStdout);
    connect(&process_, &QProcess::readyReadStandardError, this, &ExtensionServiceHost::handleStderr);
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
        this, &ExtensionServiceHost::handleFinished);
    connect(&process_, &QProcess::errorOccurred, this, &ExtensionServiceHost::handleProcessError);
}

ExtensionServiceHost::ExtensionServiceHost(
    QString extensionRoot,
    QString trustedTestProgram,
    QStringList trustedTestArguments,
    int testRequestTimeoutMs,
    QObject* parent)
    : ExtensionServiceHost(std::move(extensionRoot), parent) {
    trustedTestProgram_ = std::move(trustedTestProgram);
    trustedTestArguments_ = std::move(trustedTestArguments);
    requestTimeoutMs_ = qBound(100, testRequestTimeoutMs, 60'000);
}

ExtensionServiceHost::~ExtensionServiceHost() {
    QObject::disconnect(&runtimeManager_, nullptr, this, nullptr);
    QObject::disconnect(&process_, nullptr, this, nullptr);
    if (!runtimeRequestId_.isEmpty()) {
        runtimeManager_.cancel(runtimeRequestId_);
    }
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
        process_.waitForFinished(2000);
    }
}

void ExtensionServiceHost::invoke(
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
    const QVariantMap& params) {
    QString code;
    QString error;
    const QString requestedWorkspace = QDir::cleanPath(QFileInfo(workspaceRoot).absoluteFilePath());
    const QString workspacesRoot = QDir(extensionRoot_).absoluteFilePath(QStringLiteral("workspaces"));
    const QFileInfo workspaceInfo(requestedWorkspace);
    if (!workspaceInfo.isDir() || workspaceInfo.isSymLink() || workspaceInfo.isJunction()
        || !ExtensionManifestParser::pathIsWithin(workspacesRoot, requestedWorkspace)) {
        emit responseReady(hostRequestId, false, {}, QStringLiteral("service_workspace_unsafe"),
            QStringLiteral("service session workspace 路径不安全"));
        return;
    }
    if (!validateInvocation(method, params, requestedWorkspace, &code, &error)) {
        emit responseReady(hostRequestId, false, {}, code, error);
        return;
    }

    if (sessionMatches(extensionId, sessionId)
        && (packageRoot_ != QDir::cleanPath(QFileInfo(packageRoot).absoluteFilePath())
            || packageDigest_ != packageDigest || serviceModule_ != serviceModule
            || serviceLockfile_ != serviceLockfile || workspaceRoot_ != requestedWorkspace)) {
        emit responseReady(hostRequestId, false, {}, QStringLiteral("service_session_mismatch"),
            QStringLiteral("service session descriptor 与已启动进程不一致"));
        return;
    }
    if (!sessionMatches(extensionId, sessionId)) {
        if (state_ != State::Idle) {
            failAll(QStringLiteral("service_session_replaced"), QStringLiteral("service session 已被替换"));
            abortCurrentSession();
        }
        beginSession(extensionId, sessionId, packageRoot, packageDigest, serviceModule,
            serviceLockfile, workspaceRoot, locale);
    }

    const QVariantMap normalizedParams = normalizeValuePaths(params, requestedWorkspace).toMap();
    Invocation invocation { hostRequestId, method, normalizedParams };
    if (state_ == State::Ready) {
        const qint64 id = sendRpc(
            RpcKind::Invoke, method, QJsonObject::fromVariantMap(normalizedParams), hostRequestId);
        if (id < 0) {
            failInvocation(invocation, QStringLiteral("service_write_failed"), QStringLiteral("无法写入 service 请求"));
        }
        return;
    }
    invocations_.enqueue(invocation);
}

void ExtensionServiceHost::closeSession(const QString& extensionId, const QString& sessionId) {
    if (!sessionMatches(extensionId, sessionId)) {
        return;
    }
    if (!runtimeRequestId_.isEmpty()) {
        runtimeManager_.cancel(runtimeRequestId_);
        runtimeRequestId_.clear();
    }
    failAll(QStringLiteral("session_closed"), QStringLiteral("插件 session 已关闭"));
    stopProcess(true);
}

bool ExtensionServiceHost::sessionMatches(const QString& extensionId, const QString& sessionId) const {
    return !extensionId_.isEmpty() && extensionId_ == extensionId && sessionId_ == sessionId;
}

bool ExtensionServiceHost::validateInvocation(
    const QString& method,
    const QVariantMap& params,
    const QString& workspaceRoot,
    QString* code,
    QString* error) const {
    static const QRegularExpression methodPattern(QStringLiteral("^[A-Za-z_][A-Za-z0-9_.]{0,119}$"));
    if (!methodPattern.match(method).hasMatch() || method.startsWith(QStringLiteral("extension."))) {
        *code = QStringLiteral("service_method_denied");
        *error = QStringLiteral("service method 无效或属于 host 保留命名空间");
        return false;
    }
    const QByteArray encoded = QJsonDocument(QJsonObject::fromVariantMap(params)).toJson(QJsonDocument::Compact);
    if (encoded.size() > kMaxRequestBytes || !validateValuePaths(params, workspaceRoot)) {
        *code = encoded.size() > kMaxRequestBytes
            ? QStringLiteral("service_request_too_large")
            : QStringLiteral("service_path_outside_workspace");
        *error = encoded.size() > kMaxRequestBytes
            ? QStringLiteral("service 请求超过 1 MiB")
            : QStringLiteral("service 参数包含 session workspace 外的绝对路径");
        return false;
    }
    return true;
}

bool ExtensionServiceHost::validateValuePaths(const QVariant& value, const QString& workspaceRoot) const {
    if (value.metaType().id() == QMetaType::QString) {
        const QString text = value.toString();
        if (!QDir::isAbsolutePath(text)) {
            return true;
        }
        return !workspaceRoot.isEmpty() && ExtensionManifestParser::pathIsWithin(workspaceRoot, text);
    }
    if (value.metaType().id() == QMetaType::QVariantMap) {
        const QVariantMap map = value.toMap();
        for (auto it = map.cbegin(); it != map.cend(); ++it) {
            if (!validateValuePaths(it.value(), workspaceRoot)) {
                return false;
            }
        }
    } else if (value.metaType().id() == QMetaType::QVariantList || value.metaType().id() == QMetaType::QStringList) {
        for (const QVariant& item : value.toList()) {
            if (!validateValuePaths(item, workspaceRoot)) {
                return false;
            }
        }
    }
    return true;
}

QVariant ExtensionServiceHost::normalizeValuePaths(const QVariant& value, const QString& workspaceRoot) const {
    if (value.metaType().id() == QMetaType::QString) {
        const QString text = value.toString();
        if (QDir::isAbsolutePath(text)) {
            return QDir::fromNativeSeparators(QDir(workspaceRoot).relativeFilePath(text));
        }
        return value;
    }
    if (value.metaType().id() == QMetaType::QVariantMap) {
        QVariantMap normalized;
        const QVariantMap map = value.toMap();
        for (auto it = map.cbegin(); it != map.cend(); ++it) {
            normalized.insert(it.key(), normalizeValuePaths(it.value(), workspaceRoot));
        }
        return normalized;
    }
    if (value.metaType().id() == QMetaType::QVariantList || value.metaType().id() == QMetaType::QStringList) {
        QVariantList normalized;
        for (const QVariant& item : value.toList()) {
            normalized.append(normalizeValuePaths(item, workspaceRoot));
        }
        return normalized;
    }
    return value;
}

void ExtensionServiceHost::beginSession(
    const QString& extensionId,
    const QString& sessionId,
    const QString& packageRoot,
    const QString& packageDigest,
    const QString& serviceModule,
    const QString& serviceLockfile,
    const QString& workspaceRoot,
    const QString& locale) {
    extensionId_ = extensionId;
    sessionId_ = sessionId;
    packageRoot_ = QDir::cleanPath(QFileInfo(packageRoot).absoluteFilePath());
    packageDigest_ = packageDigest;
    serviceModule_ = serviceModule;
    serviceLockfile_ = serviceLockfile;
    workspaceRoot_ = QDir::cleanPath(QFileInfo(workspaceRoot).absoluteFilePath());
    locale_ = locale.isEmpty() ? QStringLiteral("zh-CN") : locale.left(32);
    state_ = State::PreparingEnvironment;

    if (!trustedTestProgram_.isEmpty()) {
        QTimer::singleShot(0, this, [this]() { startProcess(trustedTestProgram_); });
        return;
    }
    runtimeRequestId_ = runtimeManager_.ensureEnvironment(
        extensionId_, packageRoot_, packageDigest_, serviceLockfile_);
}

void ExtensionServiceHost::handleEnvironmentReady(
    const QString& requestId,
    const QString& pythonExecutable) {
    if (state_ != State::PreparingEnvironment || requestId != runtimeRequestId_) {
        return;
    }
    runtimeRequestId_.clear();
    startProcess(pythonExecutable);
}

void ExtensionServiceHost::handleEnvironmentFailed(
    const QString& requestId,
    const QString& code,
    const QString& error) {
    if (state_ != State::PreparingEnvironment || requestId != runtimeRequestId_) {
        return;
    }
    runtimeRequestId_.clear();
    failAll(code, error);
    clearSession();
}

void ExtensionServiceHost::startProcess(const QString& pythonExecutable) {
    if (state_ != State::PreparingEnvironment) {
        return;
    }
    state_ = State::Starting;
    stopping_ = false;
    stdoutBuffer_.clear();
    stderrBuffer_.clear();
    pending_.clear();
    nextRpcId_ = 1;

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("PYTHONHOME"));
    environment.remove(QStringLiteral("PYTHONPATH"));
    environment.remove(QStringLiteral("VIRTUAL_ENV"));
    environment.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    environment.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    environment.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
    environment.insert(QStringLiteral("PYTHONNOUSERSITE"), QStringLiteral("1"));
    process_.setProcessEnvironment(environment);
    process_.setProcessChannelMode(QProcess::SeparateChannels);
    process_.setWorkingDirectory(packageRoot_);

    if (!trustedTestProgram_.isEmpty()) {
        process_.setProgram(trustedTestProgram_);
        process_.setArguments(trustedTestArguments_);
    } else {
        process_.setProgram(pythonExecutable);
        process_.setArguments({ QStringLiteral("-I"), QStringLiteral("-X"), QStringLiteral("utf8"),
            QStringLiteral("-m"), serviceModule_ });
    }
    process_.start();
    QTimer::singleShot(10'000, this, [this]() {
        if (state_ == State::Starting) {
            protocolFailure(QStringLiteral("service_start_timeout"), QStringLiteral("service 启动超时"));
        }
    });
}

void ExtensionServiceHost::handleStarted() {
    if (state_ != State::Starting) {
        return;
    }
    state_ = State::Initializing;
    const qint64 id = sendRpc(RpcKind::Initialize, QStringLiteral("extension.initialize"),
        QJsonObject {
            { QStringLiteral("workspace"), QDir::toNativeSeparators(workspaceRoot_) },
            { QStringLiteral("locale"), locale_ },
        });
    if (id < 0) {
        protocolFailure(QStringLiteral("service_initialize_write_failed"), QStringLiteral("无法发送 service initialize"));
    }
}

void ExtensionServiceHost::handleStdout() {
    stdoutBuffer_.append(process_.readAllStandardOutput());
    if (stdoutBuffer_.size() > kMaxResponseBytes && !stdoutBuffer_.contains('\n')) {
        protocolFailure(QStringLiteral("service_message_too_large"), QStringLiteral("service stdout 消息超过 8 MiB"));
        return;
    }
    qsizetype newline = -1;
    while ((newline = stdoutBuffer_.indexOf('\n')) >= 0) {
        QByteArray line = stdoutBuffer_.left(newline);
        stdoutBuffer_.remove(0, newline + 1);
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        if (line.size() > kMaxResponseBytes) {
            protocolFailure(QStringLiteral("service_message_too_large"), QStringLiteral("service stdout 消息超过 8 MiB"));
            return;
        }
        if (!line.trimmed().isEmpty()) {
            handleLine(line);
        }
        if (state_ == State::Idle || stopping_) {
            return;
        }
    }
}

void ExtensionServiceHost::handleStderr() {
    const QByteArray payload = process_.readAllStandardError();
    stderrBuffer_.append(payload);
    if (stderrBuffer_.size() > kMaxStderrBufferBytes) {
        stderrBuffer_ = stderrBuffer_.right(kMaxStderrBufferBytes);
    }
    appendServiceLog(payload);
}

void ExtensionServiceHost::handleFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    handleStderr();
    if (stopping_) {
        clearSession();
        return;
    }
    const QString detail = QStringLiteral("service 进程退出（code=%1, status=%2）%3")
                               .arg(exitCode)
                               .arg(static_cast<int>(exitStatus))
                               .arg(stderrBuffer_.isEmpty()
                                       ? QString()
                                       : QStringLiteral(": ") + QString::fromUtf8(stderrBuffer_).right(1000));
    failAll(exitStatus == QProcess::CrashExit
            ? QStringLiteral("service_crashed")
            : QStringLiteral("service_exited"),
        detail);
    clearSession();
}

void ExtensionServiceHost::handleProcessError(QProcess::ProcessError error) {
    if (stopping_) {
        return;
    }
    failAll(processErrorCode(error), process_.errorString());
    stopProcess(false);
}

void ExtensionServiceHost::handleLine(const QByteArray& line) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        protocolFailure(QStringLiteral("service_invalid_json"), QStringLiteral("service stdout 包含非法 JSON-RPC"));
        return;
    }
    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("jsonrpc")).toString() != QStringLiteral("2.0")
        || !object.value(QStringLiteral("id")).isDouble()) {
        protocolFailure(QStringLiteral("service_invalid_response"), QStringLiteral("service 响应缺少 jsonrpc 2.0 或 numeric id"));
        return;
    }
    const qint64 id = static_cast<qint64>(object.value(QStringLiteral("id")).toDouble(-1));
    const bool hasResult = object.contains(QStringLiteral("result"));
    const bool hasError = object.value(QStringLiteral("error")).isObject();
    if (id < 1 || hasResult == hasError || (hasError && object.value(QStringLiteral("error")).toObject().isEmpty())
        || !pending_.contains(id)) {
        protocolFailure(QStringLiteral("service_response_unmatched"), QStringLiteral("service 响应 id 未匹配、重复或 envelope 无效"));
        return;
    }
    handleRpcResult(id, object.value(QStringLiteral("result")), object.value(QStringLiteral("error")).toObject());
}

qint64 ExtensionServiceHost::sendRpc(
    RpcKind kind,
    const QString& method,
    const QJsonObject& params,
    const QString& hostRequestId) {
    if (process_.state() != QProcess::Running) {
        return -1;
    }
    const qint64 id = nextRpcId_++;
    const QJsonObject request {
        { QStringLiteral("jsonrpc"), QStringLiteral("2.0") },
        { QStringLiteral("id"), id },
        { QStringLiteral("method"), method },
        { QStringLiteral("params"), params },
    };
    QByteArray payload = QJsonDocument(request).toJson(QJsonDocument::Compact);
    payload.append('\n');
    if (payload.size() > kMaxRequestBytes || process_.write(payload) != payload.size()) {
        return -1;
    }
    pending_.insert(id, PendingRpc { kind, hostRequestId });
    const int timeout = kind == RpcKind::Invoke ? requestTimeoutMs_
        : kind == RpcKind::Shutdown          ? 2'000
                                              : 10'000;
    armTimeout(id, timeout);
    return id;
}

void ExtensionServiceHost::handleRpcResult(
    qint64 id,
    const QJsonValue& result,
    const QJsonObject& error) {
    const PendingRpc pending = pending_.take(id);
    if (!error.isEmpty()) {
        const QString message = error.value(QStringLiteral("message")).toString(QStringLiteral("service JSON-RPC error"));
        if (pending.kind == RpcKind::Invoke) {
            emit responseReady(pending.hostRequestId, false, {}, QStringLiteral("service_rpc_error"), message.left(2000));
            return;
        }
        protocolFailure(QStringLiteral("service_handshake_failed"), message.left(2000));
        return;
    }

    if (pending.kind == RpcKind::Initialize) {
        state_ = State::HealthChecking;
        if (sendRpc(RpcKind::Health, QStringLiteral("extension.health"), {}) < 0) {
            protocolFailure(QStringLiteral("service_health_write_failed"), QStringLiteral("无法发送 service health"));
        }
        return;
    }
    if (pending.kind == RpcKind::Health) {
        state_ = State::Ready;
        flushInvocations();
        return;
    }
    if (pending.kind == RpcKind::Invoke) {
        emit responseReady(pending.hostRequestId, true, result.toVariant(), {}, {});
        return;
    }
    if (pending.kind == RpcKind::Shutdown) {
        stopProcess(false);
    }
}

void ExtensionServiceHost::armTimeout(qint64 id, int timeoutMs) {
    QTimer::singleShot(timeoutMs, this, [this, id]() {
        if (!pending_.contains(id)) {
            return;
        }
        const PendingRpc pending = pending_.take(id);
        if (pending.kind == RpcKind::Invoke) {
            emit responseReady(pending.hostRequestId, false, {}, QStringLiteral("service_timeout"),
                QStringLiteral("service 请求超时"));
        }
        protocolFailure(QStringLiteral("service_timeout"), QStringLiteral("service JSON-RPC 请求超时"));
    });
}

void ExtensionServiceHost::flushInvocations() {
    while (state_ == State::Ready && !invocations_.isEmpty()) {
        const Invocation invocation = invocations_.dequeue();
        if (sendRpc(RpcKind::Invoke, invocation.method, QJsonObject::fromVariantMap(invocation.params),
                invocation.hostRequestId)
            < 0) {
            failInvocation(invocation, QStringLiteral("service_write_failed"), QStringLiteral("无法写入 service 请求"));
            protocolFailure(QStringLiteral("service_write_failed"), QStringLiteral("service stdin 写入失败"));
            return;
        }
    }
}

void ExtensionServiceHost::failInvocation(
    const Invocation& invocation,
    const QString& code,
    const QString& error) {
    emit responseReady(invocation.hostRequestId, false, {}, code, error);
}

void ExtensionServiceHost::failAll(const QString& code, const QString& error) {
    while (!invocations_.isEmpty()) {
        failInvocation(invocations_.dequeue(), code, error.left(2000));
    }
    const QList<qint64> ids = pending_.keys();
    for (qint64 id : ids) {
        const PendingRpc pending = pending_.take(id);
        if (pending.kind == RpcKind::Invoke && !pending.hostRequestId.isEmpty()) {
            emit responseReady(pending.hostRequestId, false, {}, code, error.left(2000));
        }
    }
}

void ExtensionServiceHost::protocolFailure(const QString& code, const QString& error) {
    failAll(code, error);
    stopProcess(false);
}

void ExtensionServiceHost::abortCurrentSession() {
    if (!runtimeRequestId_.isEmpty()) {
        const QString requestId = runtimeRequestId_;
        runtimeRequestId_.clear();
        runtimeManager_.cancel(requestId);
    }
    stopping_ = true;
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
        process_.waitForFinished(2000);
    }
    clearSession();
}

void ExtensionServiceHost::stopProcess(bool graceful) {
    if (state_ == State::Idle && process_.state() == QProcess::NotRunning) {
        clearSession();
        return;
    }
    stopping_ = true;
    if (graceful && state_ == State::Ready && process_.state() == QProcess::Running) {
        state_ = State::ShuttingDown;
        if (sendRpc(RpcKind::Shutdown, QStringLiteral("extension.shutdown"), {}) >= 0) {
            QTimer::singleShot(2'500, this, [this]() {
                if (process_.state() != QProcess::NotRunning) {
                    process_.kill();
                }
            });
            return;
        }
    }
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
    } else {
        clearSession();
    }
}

void ExtensionServiceHost::clearSession() {
    state_ = State::Idle;
    extensionId_.clear();
    sessionId_.clear();
    packageRoot_.clear();
    packageDigest_.clear();
    serviceModule_.clear();
    serviceLockfile_.clear();
    workspaceRoot_.clear();
    locale_.clear();
    runtimeRequestId_.clear();
    invocations_.clear();
    pending_.clear();
    stdoutBuffer_.clear();
    stderrBuffer_.clear();
    stopping_ = false;
}

void ExtensionServiceHost::appendServiceLog(const QByteArray& payload) {
    if (payload.isEmpty() || extensionId_.isEmpty() || sessionId_.isEmpty()) {
        return;
    }
    const QString logDirectory = QDir(extensionRoot_).absoluteFilePath(
        QStringLiteral("logs/%1").arg(extensionId_));
    if (!QDir().mkpath(logDirectory)) {
        return;
    }
    QFile log(QDir(logDirectory).absoluteFilePath(sessionId_ + QStringLiteral(".log")));
    if (log.size() >= kMaxLogBytes || !log.open(QIODevice::WriteOnly | QIODevice::Append)) {
        return;
    }
    const QByteArray timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs).toUtf8();
    const QByteArray limited = payload.left(kMaxLogBytes - log.size());
    log.write(timestamp + QByteArrayLiteral(" stderr ") + limited);
    if (!limited.endsWith('\n')) {
        log.write("\n");
    }
}
