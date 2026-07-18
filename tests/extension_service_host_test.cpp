#include "extensions/extensionservicehost.h"
#include "extensions/pythonruntimemanager.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <QVariantMap>

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

int fail(const QString& message) {
    std::cerr << message.toStdString() << '\n';
    return 1;
}

QJsonObject rpcResult(qint64 id, const QJsonValue& result) {
    return {
        { QStringLiteral("jsonrpc"), QStringLiteral("2.0") },
        { QStringLiteral("id"), id },
        { QStringLiteral("result"), result },
    };
}

void writeRpc(const QJsonObject& object) {
    std::cout << QJsonDocument(object).toJson(QJsonDocument::Compact).constData() << std::endl;
}

int runMockService() {
    std::string line;
    while (std::getline(std::cin, line)) {
        const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(line));
        const QJsonObject request = document.object();
        const qint64 id = static_cast<qint64>(request.value(QStringLiteral("id")).toDouble());
        const QString method = request.value(QStringLiteral("method")).toString();
        if (method == QStringLiteral("fixture.invalid")) {
            std::cout << "not-json" << std::endl;
            continue;
        }
        if (method == QStringLiteral("fixture.crash")) {
            std::quick_exit(17);
        }
        if (method == QStringLiteral("fixture.timeout")) {
            continue;
        }
        if (method == QStringLiteral("fixture.rpcError")) {
            writeRpc({
                { QStringLiteral("jsonrpc"), QStringLiteral("2.0") },
                { QStringLiteral("id"), id },
                { QStringLiteral("error"), QJsonObject {
                                                      { QStringLiteral("code"), -32000 },
                                                      { QStringLiteral("message"), QStringLiteral("fixture error") },
                                                  } },
            });
            continue;
        }
        const QJsonObject response = rpcResult(id,
            method == QStringLiteral("fixture.echo")
                ? request.value(QStringLiteral("params"))
                : QJsonObject { { QStringLiteral("ok"), true } });
        writeRpc(response);
        if (method == QStringLiteral("fixture.duplicate")) {
            writeRpc(response);
        }
        if (method == QStringLiteral("extension.shutdown")) {
            return 0;
        }
    }
    return 0;
}

struct Response {
    bool received = false;
    bool ok = false;
    QVariant result;
    QString code;
    QString message;
};

Response invokeAndWait(
    ExtensionServiceHost& host,
    const QString& requestId,
    const QString& extensionId,
    const QString& sessionId,
    const QString& packageRoot,
    const QString& workspaceRoot,
    const QString& method,
    const QVariantMap& params = {},
    int timeoutMs = 3000) {
    Response response;
    const QMetaObject::Connection connection = QObject::connect(
        &host, &ExtensionServiceHost::responseReady, &host,
        [&](const QString& id, bool ok, const QVariant& result, const QString& code, const QString& message) {
            if (id == requestId) {
                response = { true, ok, result, code, message };
            }
        });
    host.invoke(requestId, extensionId, sessionId, packageRoot,
        QString(64, QLatin1Char('a')), QStringLiteral("fixture.service"), QStringLiteral("uv.lock"),
        workspaceRoot, QStringLiteral("zh-CN"), method, params);
    QElapsedTimer timer;
    timer.start();
    while (!response.received && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    QObject::disconnect(connection);
    return response;
}

ExtensionServiceHost makeHost(const QString& root, int timeoutMs = 250) {
    return ExtensionServiceHost(root, QCoreApplication::applicationFilePath(),
        { QStringLiteral("--mock-service") }, timeoutMs);
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    if (QCoreApplication::arguments().contains(QStringLiteral("--mock-service"))) {
        return runMockService();
    }

    if (PythonRuntimeManager::uvVersion() != QStringLiteral("0.11.3")
        || PythonRuntimeManager::pythonVersion() != QStringLiteral("3.11.15")
        || PythonRuntimeManager::uvArchiveSha256().size() != 64) {
        return fail(QStringLiteral("runtime manifest constants are not pinned"));
    }

    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        return fail(QStringLiteral("temporary root unavailable"));
    }
    const QString extensionId = QStringLiteral("com.fantareal.service-test");
    const QString sessionId = QStringLiteral("session-1");
    const QString packageRoot = QDir(temporary.path()).absoluteFilePath(QStringLiteral("packages/test/package"));
    const QString workspaceRoot = QDir(temporary.path()).absoluteFilePath(
        QStringLiteral("workspaces/%1/%2").arg(extensionId, sessionId));
    if (!QDir().mkpath(packageRoot) || !QDir().mkpath(workspaceRoot)) {
        return fail(QStringLiteral("fixture layout unavailable"));
    }

    {
        ExtensionServiceHost host = makeHost(temporary.path());
        const QString inputPath = QDir(workspaceRoot).absoluteFilePath(QStringLiteral("input/card.png"));
        const Response echo = invokeAndWait(host, QStringLiteral("echo"), extensionId, sessionId,
            packageRoot, workspaceRoot, QStringLiteral("fixture.echo"),
            { { QStringLiteral("input"), inputPath }, { QStringLiteral("mode"), QStringLiteral("role_card") } });
        if (!echo.received || !echo.ok
            || echo.result.toMap().value(QStringLiteral("input")).toString() != QStringLiteral("input/card.png")) {
            return fail(QStringLiteral("initialize/health/invoke flow failed"));
        }
        const QString secondInputPath = QDir(workspaceRoot).absoluteFilePath(QStringLiteral("input/second.json"));
        const Response secondEcho = invokeAndWait(host, QStringLiteral("second-echo"), extensionId, sessionId,
            packageRoot, workspaceRoot, QStringLiteral("fixture.echo"),
            { { QStringLiteral("input"), secondInputPath } });
        if (!secondEcho.received || !secondEcho.ok
            || secondEcho.result.toMap().value(QStringLiteral("input")).toString()
                != QStringLiteral("input/second.json")) {
            return fail(QStringLiteral("ready service invocation did not normalize workspace path"));
        }
        const Response outside = invokeAndWait(host, QStringLiteral("outside"), extensionId, sessionId,
            packageRoot, workspaceRoot, QStringLiteral("fixture.echo"),
            { { QStringLiteral("input"), QDir(temporary.path()).absoluteFilePath(QStringLiteral("outside.png")) } });
        if (!outside.received || outside.ok || outside.code != QStringLiteral("service_path_outside_workspace")) {
            return fail(QStringLiteral("absolute path escaped the session workspace"));
        }
        const Response rpcError = invokeAndWait(host, QStringLiteral("rpc-error"), extensionId, sessionId,
            packageRoot, workspaceRoot, QStringLiteral("fixture.rpcError"));
        if (!rpcError.received || rpcError.ok || rpcError.code != QStringLiteral("service_rpc_error")) {
            return fail(QStringLiteral("JSON-RPC error was not isolated"));
        }
        const Response oversized = invokeAndWait(host, QStringLiteral("oversized"), extensionId, sessionId,
            packageRoot, workspaceRoot, QStringLiteral("fixture.echo"),
            { { QStringLiteral("payload"), QString(1024 * 1024 + 1, QLatin1Char('x')) } });
        if (!oversized.received || oversized.ok || oversized.code != QStringLiteral("service_request_too_large")) {
            return fail(QStringLiteral("oversized request was not rejected"));
        }
        host.closeSession(extensionId, sessionId);
    }

    {
        const QString duplicateSession = QStringLiteral("duplicate-session");
        const QString duplicateWorkspace = QDir(temporary.path()).absoluteFilePath(
            QStringLiteral("workspaces/%1/%2").arg(extensionId, duplicateSession));
        QDir().mkpath(duplicateWorkspace);
        ExtensionServiceHost host = makeHost(temporary.path());
        const Response duplicate = invokeAndWait(host, QStringLiteral("duplicate"), extensionId,
            duplicateSession, packageRoot, duplicateWorkspace, QStringLiteral("fixture.duplicate"));
        if (!duplicate.received || !duplicate.ok) {
            return fail(QStringLiteral("first duplicate response was not delivered"));
        }
        QElapsedTimer recoveryDelay;
        recoveryDelay.start();
        while (recoveryDelay.elapsed() < 100) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        const Response recovered = invokeAndWait(host, QStringLiteral("duplicate-recovery"), extensionId,
            duplicateSession, packageRoot, duplicateWorkspace, QStringLiteral("fixture.echo"),
            { { QStringLiteral("value"), 7 } });
        if (!recovered.received || !recovered.ok
            || recovered.result.toMap().value(QStringLiteral("value")).toInt() != 7) {
            return fail(QStringLiteral("service did not recover after duplicate response"));
        }
    }

    {
        ExtensionServiceHost host = makeHost(temporary.path());
        const Response invalid = invokeAndWait(host, QStringLiteral("invalid"), extensionId, QStringLiteral("invalid-session"),
            packageRoot, QDir(temporary.path()).absoluteFilePath(QStringLiteral("workspaces/%1/invalid-session").arg(extensionId)),
            QStringLiteral("fixture.invalid"));
        if (!invalid.received || invalid.ok || invalid.code != QStringLiteral("service_workspace_unsafe")) {
            return fail(QStringLiteral("missing workspace was not rejected before service start"));
        }
    }

    const auto runFailureCase = [&](const QString& name, const QString& method, const QStringList& acceptedCodes) {
        const QString testSession = name + QStringLiteral("-session");
        const QString testWorkspace = QDir(temporary.path()).absoluteFilePath(
            QStringLiteral("workspaces/%1/%2").arg(extensionId, testSession));
        QDir().mkpath(testWorkspace);
        ExtensionServiceHost host = makeHost(temporary.path());
        const Response response = invokeAndWait(host, name, extensionId, testSession, packageRoot,
            testWorkspace, method, {}, 3000);
        return response.received && !response.ok && acceptedCodes.contains(response.code);
    };
    if (!runFailureCase(QStringLiteral("invalid-json"), QStringLiteral("fixture.invalid"),
            { QStringLiteral("service_invalid_json") })) {
        return fail(QStringLiteral("invalid JSON did not terminate the service"));
    }
    if (!runFailureCase(QStringLiteral("timeout"), QStringLiteral("fixture.timeout"),
            { QStringLiteral("service_timeout") })) {
        return fail(QStringLiteral("service timeout was not reported"));
    }
    if (!runFailureCase(QStringLiteral("crash"), QStringLiteral("fixture.crash"),
            { QStringLiteral("service_crashed"), QStringLiteral("service_exited") })) {
        return fail(QStringLiteral("service crash was not isolated"));
    }

    {
        const QString cancelSession = QStringLiteral("cancel-session");
        const QString cancelWorkspace = QDir(temporary.path()).absoluteFilePath(
            QStringLiteral("workspaces/%1/%2").arg(extensionId, cancelSession));
        QDir().mkpath(cancelWorkspace);
        ExtensionServiceHost host = makeHost(temporary.path(), 2000);
        Response cancelled;
        QObject::connect(&host, &ExtensionServiceHost::responseReady, &host,
            [&](const QString& id, bool ok, const QVariant& result, const QString& code, const QString& message) {
                if (id == QStringLiteral("cancel")) {
                    cancelled = { true, ok, result, code, message };
                }
            });
        host.invoke(QStringLiteral("cancel"), extensionId, cancelSession, packageRoot,
            QString(64, QLatin1Char('a')), QStringLiteral("fixture.service"), QStringLiteral("uv.lock"),
            cancelWorkspace, QStringLiteral("zh-CN"), QStringLiteral("fixture.timeout"), {});
        QElapsedTimer started;
        started.start();
        while (started.elapsed() < 100) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        host.closeSession(extensionId, cancelSession);
        QCoreApplication::processEvents();
        if (!cancelled.received || cancelled.ok || cancelled.code != QStringLiteral("session_closed")) {
            return fail(QStringLiteral("session cancellation did not fail pending invocation"));
        }
    }

    return 0;
}
