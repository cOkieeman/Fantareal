#include "extensions/extensioninstaller.h"
#include "extensions/githubextensionsource.h"
#include "extensions/extensionmanifest.h"
#include "extensions/extensionregistry.h"
#include "extensions/extensionwebhost.h"
#include "fantarealbridge.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVariantMap>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlScheme>
#include <QWebChannel>
#include <QWebEnginePage>
#include <QWebEngineProfile>

#include <iostream>

namespace {

int fail(const QString& message) {
    std::cerr << message.toStdString() << '\n';
    return 1;
}

bool writeBytes(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}

bool writeJson(const QString& path, const QJsonObject& value) {
    return writeBytes(path, QJsonDocument(value).toJson(QJsonDocument::Indented));
}

bool createFixture(const QString& root) {
    const QJsonObject manifest {
        { QStringLiteral("schemaVersion"), 1 },
        { QStringLiteral("id"), QStringLiteral("com.fantareal.web-host-test") },
        { QStringLiteral("name"), QStringLiteral("Web Host Test") },
        { QStringLiteral("description"), QStringLiteral("Focused Web Host fixture") },
        { QStringLiteral("version"), QStringLiteral("1.0.0") },
        { QStringLiteral("publisher"), QStringLiteral("Fantareal") },
        { QStringLiteral("compatibility"), QJsonObject {
                                                 { QStringLiteral("hostApi"), QStringLiteral(">=1.0.0 <2.0.0") },
                                                 { QStringLiteral("python"), QStringLiteral(">=3.11") },
                                                 { QStringLiteral("platforms"), QJsonArray { QStringLiteral("windows") } },
                                             } },
        { QStringLiteral("entrypoints"), QJsonObject {
                                             { QStringLiteral("page"), QJsonObject {
                                                                           { QStringLiteral("type"), QStringLiteral("web") },
                                                                           { QStringLiteral("path"), QStringLiteral("web/index.html") },
                                                                           { QStringLiteral("bridge"), QStringLiteral("fantareal.extension.v1") },
                                                                       } },
                                             { QStringLiteral("service"), QJsonObject {
                                                                              { QStringLiteral("type"), QStringLiteral("python") },
                                                                              { QStringLiteral("module"), QStringLiteral("fixture.service") },
                                                                              { QStringLiteral("protocol"), QStringLiteral("jsonrpc-2.0-stdio") },
                                                                              { QStringLiteral("lockfile"), QStringLiteral("uv.lock") },
                                                                          } },
                                         } },
        { QStringLiteral("contributes"), QJsonObject {
                                                { QStringLiteral("artifacts"), QJsonArray {
                                                                                     QJsonObject {
                                                                                         { QStringLiteral("kind"), QStringLiteral("fantareal.role-card") },
                                                                                         { QStringLiteral("mediaTypes"), QJsonArray { QStringLiteral("application/json") } },
                                                                                     },
                                                                                     QJsonObject {
                                                                                         { QStringLiteral("kind"), QStringLiteral("fantareal.worldbook") },
                                                                                         { QStringLiteral("mediaTypes"), QJsonArray { QStringLiteral("application/json") } },
                                                                                     },
                                                                                 } },
                                            } },
        { QStringLiteral("permissions"), QJsonArray {
                                             QStringLiteral("files.user-selected.read"),
                                             QStringLiteral("storage.workspace"),
                                             QStringLiteral("artifacts.publish"),
                                         } },
        { QStringLiteral("limits"), QJsonObject {} },
    };
    return writeJson(QDir(root).absoluteFilePath(QStringLiteral("fantareal-extension.json")), manifest)
        && writeBytes(QDir(root).absoluteFilePath(QStringLiteral("web/index.html")),
            QByteArrayLiteral("<!doctype html><html><head><link rel=\"stylesheet\" href=\"style.css\"></head><body>ok</body></html>"))
        && writeBytes(QDir(root).absoluteFilePath(QStringLiteral("web/style.css")), QByteArrayLiteral("body{color:#fff}"))
        && writeBytes(QDir(root).absoluteFilePath(QStringLiteral("uv.lock")), QByteArrayLiteral("version = 1\n"))
        && writeBytes(QDir(root).absoluteFilePath(QStringLiteral("src/fixture/service.py")), QByteArrayLiteral("def main(): pass\n"));
}

bool createFantarealFixture(const QString& rootPath) {
    const QDir root(rootPath);
    const QStringList directories {
        QStringLiteral("data/auto_saga"),
        QStringLiteral("data/database"),
        QStringLiteral("data/logs"),
        QStringLiteral("cards"),
    };
    for (const QString& directory : directories) {
        if (!root.mkpath(directory)) {
            return false;
        }
    }
    const QJsonObject emptyObject;
    const QStringList emptyObjectFiles {
        QStringLiteral("data/settings.json"),
        QStringLiteral("data/route_forwarding.json"),
        QStringLiteral("data/worldbook_runtime_state.json"),
        QStringLiteral("data/creative_workshop_state.json"),
        QStringLiteral("data/persona.json"),
        QStringLiteral("data/user_profile.json"),
        QStringLiteral("data/auto_saga/state.json"),
        QStringLiteral("cards/template_single_role_card.json"),
        QStringLiteral("cards/template_multi_role_card.json"),
    };
    for (const QString& path : emptyObjectFiles) {
        if (!writeJson(root.absoluteFilePath(path), emptyObject)) {
            return false;
        }
    }
    if (!writeJson(root.absoluteFilePath(QStringLiteral("data/current_role_card.json")),
            QJsonObject { { QStringLiteral("raw"), QJsonObject { { QStringLiteral("name"), QStringLiteral("Before Router") } } } })
        || !writeJson(root.absoluteFilePath(QStringLiteral("data/worldbook.json")),
            QJsonObject {
                { QStringLiteral("settings"), QJsonObject { { QStringLiteral("enabled"), true } } },
                { QStringLiteral("entries"), QJsonArray {} },
            })
        || !writeJson(root.absoluteFilePath(QStringLiteral("data/preset.json")), QJsonObject {
                                                                                      { QStringLiteral("presets"), QJsonArray {} },
                                                                                  })
        || !writeBytes(root.absoluteFilePath(QStringLiteral("data/conversations.json")), QByteArrayLiteral("[]\n"))) {
        return false;
    }
    QFile(root.absoluteFilePath(QStringLiteral("data/database/database.db"))).open(QIODevice::WriteOnly);
    QFile(root.absoluteFilePath(QStringLiteral("data/logs/fantareal.log"))).open(QIODevice::WriteOnly);
    return true;
}

QJsonObject readJsonObject(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).object();
}

bool writeSyntheticTavernCard(const QString& path) {
    const QJsonObject card {
        { QStringLiteral("spec"), QStringLiteral("chara_card_v2") },
        { QStringLiteral("spec_version"), QStringLiteral("2.0") },
        { QStringLiteral("data"), QJsonObject {
                                          { QStringLiteral("name"), QStringLiteral("Phase 6 Hero") },
                                          { QStringLiteral("description"), QStringLiteral("Synthetic public test card used to verify the complete Phase 6 role-card conversion and native import path.") },
                                          { QStringLiteral("personality"), QStringLiteral("Reliable") },
                                          { QStringLiteral("scenario"), QStringLiteral("Artifact Router smoke") },
                                          { QStringLiteral("first_mes"), QStringLiteral("Hello from Phase 6") },
                                      } },
    };
    const QString encoded = QString::fromLatin1(
        QJsonDocument(card).toJson(QJsonDocument::Compact).toBase64());
    QImage image(64, 96, QImage::Format_RGB32);
    image.fill(qRgb(42, 80, 72));
    QImageWriter writer(path, QByteArrayLiteral("png"));
    writer.setText(QStringLiteral("chara"), encoded);
    return writer.write(image);
}

struct HostResponse {
    bool received = false;
    bool ok = false;
    QVariant result;
    QString code;
    QString message;
};

HostResponse waitForResponse(ExtensionWebHost& host, const QString& requestId, int timeoutMs = 2000) {
    HostResponse response;
    const QMetaObject::Connection connection = QObject::connect(
        &host,
        &ExtensionWebHost::responseReady,
        &host,
        [&](const QString& id, bool ok, const QVariant& result, const QString& code, const QString& message) {
            if (id != requestId) {
                return;
            }
            response = { true, ok, result, code, message };
        });
    QElapsedTimer timer;
    timer.start();
    while (!response.received && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    QObject::disconnect(connection);
    return response;
}

bool runOptionalPackageSmoke(
    const QStringList& arguments,
    ExtensionRegistry& registry,
    ExtensionInstaller& installer,
    ExtensionWebHost& host,
    const QString& fantarealRoot) {
    QString githubUrl;
    QString localPackage;
    const bool serviceSmoke = arguments.contains(QStringLiteral("--service-smoke"));
    const bool artifactSmoke = arguments.contains(QStringLiteral("--artifact-smoke"));
    for (const QString& argument : arguments) {
        if (argument.startsWith(QStringLiteral("--github-url="))) {
            githubUrl = argument.mid(QStringLiteral("--github-url=").size());
        } else if (argument.startsWith(QStringLiteral("--local-package="))) {
            localPackage = argument.mid(QStringLiteral("--local-package=").size());
        }
    }
    if (githubUrl.isEmpty() && localPackage.isEmpty()) {
        return true;
    }

    QVariantMap installResult;
    if (!localPackage.isEmpty()) {
        installResult = installer.installFromDirectory(localPackage);
    } else {
        GitHubExtensionSource source(installer, registry.rootPath());
        QEventLoop installLoop;
        QTimer installTimeout;
        installTimeout.setSingleShot(true);
        QObject::connect(&installTimeout, &QTimer::timeout, &installLoop, &QEventLoop::quit);
        QObject::connect(&source, &GitHubExtensionSource::finished, &installLoop,
            [&](const QVariantMap& result) {
                installResult = result;
                installLoop.quit();
            });
        const QVariantMap accepted = source.install(githubUrl);
        if (!accepted.value(QStringLiteral("ok")).toBool()) {
            fail(QStringLiteral("GitHub Web smoke was not accepted: %1")
                    .arg(accepted.value(QStringLiteral("message")).toString()));
            return false;
        }
        installTimeout.start(120000);
        installLoop.exec();
    }
    if (!installResult.value(QStringLiteral("ok")).toBool()) {
        fail(QStringLiteral("Extension package smoke install failed: %1")
                .arg(installResult.value(QStringLiteral("message")).toString()));
        return false;
    }

    const QString extensionId = installResult.value(QStringLiteral("id")).toString();
    const QVariantMap openResult = host.openExtension(extensionId);
    if (!openResult.value(QStringLiteral("ok")).toBool()) {
        fail(QStringLiteral("GitHub Web smoke open failed: %1")
                .arg(openResult.value(QStringLiteral("message")).toString()));
        return false;
    }

    QWebChannel channel;
    channel.registerObject(QStringLiteral("fantarealExtensionBridge"), &host);
    QWebEnginePage page(QWebEngineProfile::defaultProfile());
    page.setWebChannel(&channel);
    bool loaded = false;
    bool loadCompleted = false;
    QEventLoop loadLoop;
    QTimer loadTimeout;
    loadTimeout.setSingleShot(true);
    QObject::connect(&loadTimeout, &QTimer::timeout, &loadLoop, &QEventLoop::quit);
    QObject::connect(&page, &QWebEnginePage::loadFinished, &loadLoop, [&](bool ok) {
        loaded = ok;
        loadCompleted = true;
        loadLoop.quit();
    });
    page.load(host.pageUrl());
    loadTimeout.start(20000);
    loadLoop.exec();
    if (!loadCompleted || !loaded) {
        fail(QStringLiteral("GitHub Web smoke page load failed"));
        return false;
    }

    QVariant javascriptResult;
    QEventLoop scriptLoop;
    QTimer scriptTimeout;
    scriptTimeout.setSingleShot(true);
    QObject::connect(&scriptTimeout, &QTimer::timeout, &scriptLoop, &QEventLoop::quit);
    page.runJavaScript(QStringLiteral(
                           "({title: document.title, host: typeof window.fantarealExtension === 'object', "
                           "pick: typeof window.fantarealExtension?.pickInput === 'function', "
                           "invoke: typeof window.fantarealExtension?.invoke === 'function'})"),
        [&](const QVariant& value) {
            javascriptResult = value;
            scriptLoop.quit();
        });
    scriptTimeout.start(5000);
    scriptLoop.exec();
    const QVariantMap javascript = javascriptResult.toMap();
    if (javascript.value(QStringLiteral("title")).toString() != QStringLiteral("酒馆卡转换器")
        || !javascript.value(QStringLiteral("host")).toBool()
        || !javascript.value(QStringLiteral("pick")).toBool()
        || !javascript.value(QStringLiteral("invoke")).toBool()) {
        fail(QStringLiteral("GitHub Web smoke bridge or page contract is incomplete"));
        return false;
    }
    if (serviceSmoke) {
        const QString worldbookPath = QDir(registry.rootPath()).absoluteFilePath(QStringLiteral("service-smoke-worldbook.json"));
        if (!writeBytes(worldbookPath,
                QByteArrayLiteral("{\"name\":\"Smoke World\",\"entries\":[{\"key\":[\"harbor\"],\"content\":\"A harbor.\"}]}"))) {
            fail(QStringLiteral("GitHub service smoke input creation failed"));
            return false;
        }
        const QString pickRequest = host.request(extensionId, host.sessionId(), QStringLiteral("files.pick"),
            { { QStringLiteral("accept"), QVariantList { QStringLiteral("application/json") } } });
        QCoreApplication::processEvents();
        host.completeFileSelection(QUrl::fromLocalFile(worldbookPath));
        const HostResponse picked = waitForResponse(host, pickRequest, 5000);
        const QString inputPath = picked.result.toMap().value(QStringLiteral("path")).toString();
        if (!picked.received || !picked.ok || inputPath.isEmpty()) {
            fail(QStringLiteral("GitHub service smoke input isolation failed: %1").arg(picked.message));
            return false;
        }

        const QString invokeRequest = host.request(extensionId, host.sessionId(), QStringLiteral("service.invoke"),
            {
                { QStringLiteral("method"), QStringLiteral("converter.convertWorldbook") },
                { QStringLiteral("params"), QVariantMap { { QStringLiteral("input"), inputPath } } },
            });
        const HostResponse invoked = waitForResponse(host, invokeRequest, 12 * 60 * 1000);
        const QVariantMap result = invoked.result.toMap();
        const QVariantList artifacts = result.value(QStringLiteral("artifacts")).toList();
        const QString workspaceRoot = QFileInfo(inputPath).absoluteDir().absolutePath() + QStringLiteral("/..");
        const QString artifactPath = artifacts.isEmpty()
            ? QString()
            : QDir(workspaceRoot).absoluteFilePath(artifacts.first().toMap().value(QStringLiteral("path")).toString());
        if (!invoked.received || !invoked.ok || result.value(QStringLiteral("entryCount")).toInt() != 1
            || artifacts.size() != 1
            || artifacts.first().toMap().value(QStringLiteral("kind")).toString() != QStringLiteral("fantareal.worldbook")
            || !QFileInfo::exists(artifactPath)) {
            fail(QStringLiteral("GitHub service smoke invoke failed [%1]: %2").arg(invoked.code, invoked.message));
            return false;
        }
        std::cout << "GITHUB_SERVICE_HOST_SMOKE=ok\n";
        if (artifactSmoke) {
            const QVariantMap artifact = artifacts.first().toMap();
            const QString publishRequest = host.request(extensionId, host.sessionId(),
                QStringLiteral("artifacts.publish"),
                { { QStringLiteral("artifact"), artifact } });
            const HostResponse published = waitForResponse(host, publishRequest, 30000);
            const QJsonObject importedWorldbook = readJsonObject(
                QDir(fantarealRoot).absoluteFilePath(QStringLiteral("data/worldbook.json")));
            if (!published.received || !published.ok
                || published.result.toMap().value(QStringLiteral("kind")).toString()
                    != QStringLiteral("fantareal.worldbook")
                || importedWorldbook.value(QStringLiteral("entries")).toArray().size() != 1) {
                fail(QStringLiteral("artifact publish/native import smoke failed [%1]: %2")
                        .arg(published.code, published.message));
                return false;
            }
            std::cout << "LOCAL_WORLDBOOK_ARTIFACT_IMPORT_SMOKE=ok\n";

            const QString cardPath = QDir(registry.rootPath()).absoluteFilePath(
                QStringLiteral("service-smoke-card.png"));
            if (!writeSyntheticTavernCard(cardPath)) {
                fail(QStringLiteral("synthetic Tavern PNG creation failed"));
                return false;
            }
            const QString cardPickRequest = host.request(extensionId, host.sessionId(),
                QStringLiteral("files.pick"),
                { { QStringLiteral("accept"), QVariantList { QStringLiteral("image/png") } } });
            QCoreApplication::processEvents();
            host.completeFileSelection(QUrl::fromLocalFile(cardPath));
            const HostResponse cardPicked = waitForResponse(host, cardPickRequest, 5000);
            const QString cardInputPath = cardPicked.result.toMap().value(QStringLiteral("path")).toString();
            if (!cardPicked.received || !cardPicked.ok || cardInputPath.isEmpty()) {
                fail(QStringLiteral("synthetic Tavern PNG isolation failed: %1").arg(cardPicked.message));
                return false;
            }
            const QString cardInvokeRequest = host.request(extensionId, host.sessionId(),
                QStringLiteral("service.invoke"),
                {
                    { QStringLiteral("method"), QStringLiteral("converter.convertCard") },
                    { QStringLiteral("params"), QVariantMap {
                                                           { QStringLiteral("input"), cardInputPath },
                                                           { QStringLiteral("mode"), QStringLiteral("role") },
                                                       } },
                });
            const HostResponse cardInvoked = waitForResponse(host, cardInvokeRequest, 12 * 60 * 1000);
            QVariantMap roleArtifact;
            for (const QVariant& value : cardInvoked.result.toMap().value(QStringLiteral("artifacts")).toList()) {
                const QVariantMap candidate = value.toMap();
                if (candidate.value(QStringLiteral("kind")).toString()
                    == QStringLiteral("fantareal.role-card")) {
                    roleArtifact = candidate;
                    break;
                }
            }
            if (!cardInvoked.received || !cardInvoked.ok || roleArtifact.isEmpty()) {
                fail(QStringLiteral("synthetic Tavern PNG conversion failed [%1]: %2")
                        .arg(cardInvoked.code, cardInvoked.message));
                return false;
            }
            const QString rolePublishRequest = host.request(extensionId, host.sessionId(),
                QStringLiteral("artifacts.publish"),
                { { QStringLiteral("artifact"), roleArtifact } });
            const HostResponse rolePublished = waitForResponse(host, rolePublishRequest, 30000);
            const QJsonObject importedRole = readJsonObject(
                QDir(fantarealRoot).absoluteFilePath(QStringLiteral("data/current_role_card.json")));
            if (!rolePublished.received || !rolePublished.ok
                || importedRole.value(QStringLiteral("raw")).toObject().value(QStringLiteral("name")).toString()
                    != QStringLiteral("Phase 6 Hero")) {
                fail(QStringLiteral("role-card artifact/native import smoke failed [%1]: %2")
                        .arg(rolePublished.code, rolePublished.message));
                return false;
            }
            std::cout << "LOCAL_ROLE_CARD_ARTIFACT_IMPORT_SMOKE=ok\n";
            std::cout << "LOCAL_ARTIFACT_NATIVE_IMPORT_SMOKE=ok\n";
        }
    }
    if (!githubUrl.isEmpty()) {
        std::cout << "GITHUB_WEB_HOST_SMOKE_COMMIT="
                  << installResult.value(QStringLiteral("resolvedCommit")).toString().toStdString() << '\n';
    } else {
        std::cout << "LOCAL_WEB_HOST_SMOKE=ok\n";
    }
    host.closeSession();
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    ExtensionWebHost::registerUrlScheme();
    if (QWebEngineUrlScheme::schemeByName(QByteArrayLiteral("fantareal-extension")).name().isEmpty()) {
        return fail(QStringLiteral("custom WebEngine scheme was not registered"));
    }
    QGuiApplication app(argc, argv);

    QTemporaryDir temporary;
    if (!temporary.isValid()) {
        return fail(QStringLiteral("failed to create temporary root"));
    }
    QString testRoot = temporary.path();
    for (const QString& argument : QCoreApplication::arguments()) {
        if (argument.startsWith(QStringLiteral("--test-root="))) {
            testRoot = QDir::cleanPath(QFileInfo(
                argument.mid(QStringLiteral("--test-root=").size())).absoluteFilePath());
        }
    }
    if (!QDir().mkpath(testRoot)) {
        return fail(QStringLiteral("failed to create selected test root"));
    }
    const QString sourceRoot = QDir(testRoot).absoluteFilePath(QStringLiteral("source"));
    const QString extensionRoot = QDir(testRoot).absoluteFilePath(QStringLiteral("extensions"));
    const QString fantarealRoot = QDir(testRoot).absoluteFilePath(QStringLiteral("fantareal-root"));
    if (!QDir().mkpath(sourceRoot) || !createFixture(sourceRoot)
        || !createFantarealFixture(fantarealRoot)) {
        return fail(QStringLiteral("failed to create Web Host fixture"));
    }
    qputenv("FANTAREAL_ROOT", QFile::encodeName(fantarealRoot));
    FantarealBridge bridge;

    ExtensionRegistry registry(extensionRoot);
    QString error;
    if (!registry.load(&error)) {
        return fail(QStringLiteral("failed to initialize registry: %1").arg(error));
    }
    ExtensionInstaller installer(registry);
    const QVariantMap installResult = installer.installFromDirectory(sourceRoot);
    if (!installResult.value(QStringLiteral("ok")).toBool()) {
        return fail(QStringLiteral("failed to install fixture: %1")
                .arg(installResult.value(QStringLiteral("message")).toString()));
    }

    ExtensionWebHost host(extensionRoot);
    host.setArtifactImporters(
        [&bridge](const QString& path) { return bridge.importRoleCardFile(path); },
        [&bridge](const QString& path) { return bridge.importWorldbookFile(path); });
    const QVariantMap openResult = host.openExtension(QStringLiteral("com.fantareal.web-host-test"));
    if (!openResult.value(QStringLiteral("ok")).toBool() || !host.active()
        || host.pageUrl().scheme() != QStringLiteral("fantareal-extension")) {
        return fail(QStringLiteral("failed to open isolated Web session: %1")
                .arg(openResult.value(QStringLiteral("message")).toString()));
    }

    QUrl styleUrl = host.pageUrl();
    styleUrl.setPath(QStringLiteral("/package/web/style.css"));
    if (!host.isAllowedNavigation(host.pageUrl()) || !host.isAllowedNavigation(styleUrl)
        || host.isAllowedNavigation(QUrl::fromLocalFile(QDir(testRoot).absoluteFilePath(QStringLiteral("outside.txt"))))
        || host.isAllowedNavigation(QUrl(QStringLiteral("https://example.com/")))) {
        return fail(QStringLiteral("navigation allowlist is incorrect"));
    }
    if (host.isAllowedRequest(QUrl(QStringLiteral("https://example.com/api")), host.pageUrl(),
            static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeXhr))
        || !host.isAllowedRequest(QUrl(QStringLiteral("data:image/png;base64,AA==")), host.pageUrl(),
            static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeImage))
        || host.isAllowedRequest(QUrl(QStringLiteral("data:text/html,test")), host.pageUrl(),
            static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeMainFrame))) {
        return fail(QStringLiteral("request interception policy is incorrect"));
    }

    const QString contextRequest = host.request(host.extensionId(), host.sessionId(), QStringLiteral("context.get"));
    const HostResponse context = waitForResponse(host, contextRequest);
    if (!context.received || !context.ok
        || context.result.toMap().value(QStringLiteral("extensionId")).toString() != host.extensionId()
        || context.result.toMap().value(QStringLiteral("artifactKinds")).toStringList().size() != 2) {
        return fail(QStringLiteral("context request did not return the active extension"));
    }

    const QString invalidRequest = host.request(host.extensionId(), QStringLiteral("wrong-session"),
        QStringLiteral("context.get"));
    const HostResponse invalid = waitForResponse(host, invalidRequest);
    if (!invalid.received || invalid.ok || invalid.code != QStringLiteral("session_invalid")) {
        return fail(QStringLiteral("invalid session was not rejected"));
    }

    bool pickerRequested = false;
    QObject::connect(&host, &ExtensionWebHost::fileSelectionRequested, &host,
        [&](const QStringList& filters) { pickerRequested = !filters.isEmpty(); });
    const QString fileRequest = host.request(host.extensionId(), host.sessionId(), QStringLiteral("files.pick"),
        { { QStringLiteral("accept"), QVariantList { QStringLiteral("image/png") } } });
    QCoreApplication::processEvents();
    if (!pickerRequested) {
        return fail(QStringLiteral("file picker signal was not emitted"));
    }
    const QString selectedPng = QDir(testRoot).absoluteFilePath(QStringLiteral("selected.png"));
    if (!writeBytes(selectedPng, QByteArrayLiteral("not-a-real-png"))) {
        return fail(QStringLiteral("failed to create selected file fixture"));
    }
    host.completeFileSelection(QUrl::fromLocalFile(selectedPng));
    const HostResponse fileResponse = waitForResponse(host, fileRequest);
    const QVariantMap selected = fileResponse.result.toMap();
    if (!fileResponse.received || !fileResponse.ok
        || !QFileInfo::exists(selected.value(QStringLiteral("path")).toString())
        || !host.isAllowedNavigation(QUrl(selected.value(QStringLiteral("previewUrl")).toString()))) {
        return fail(QStringLiteral("selected file was not isolated in the session workspace"));
    }

    const QString workspaceRoot = QDir::cleanPath(
        QFileInfo(selected.value(QStringLiteral("path")).toString()).absoluteDir().absoluteFilePath(QStringLiteral("..")));
    const QString roleArtifactPath = QDir(workspaceRoot).absoluteFilePath(QStringLiteral("output/router-role.json"));
    if (!writeJson(roleArtifactPath, QJsonObject {
                                         { QStringLiteral("name"), QStringLiteral("Router Hero") },
                                         { QStringLiteral("description"), QStringLiteral("Imported through Artifact Router") },
                                     })) {
        return fail(QStringLiteral("failed to create role artifact fixture"));
    }
    const QVariantMap roleArtifact {
        { QStringLiteral("id"), QStringLiteral("router-role") },
        { QStringLiteral("kind"), QStringLiteral("fantareal.role-card") },
        { QStringLiteral("mediaType"), QStringLiteral("application/json") },
        { QStringLiteral("path"), QStringLiteral("output/router-role.json") },
        { QStringLiteral("suggestedName"), QStringLiteral("Router Hero.json") },
        { QStringLiteral("metadata"), QVariantMap { { QStringLiteral("source"), QStringLiteral("focused-test") } } },
    };
    const QString publishRequest = host.request(host.extensionId(), host.sessionId(),
        QStringLiteral("artifacts.publish"), { { QStringLiteral("artifact"), roleArtifact } });
    const HostResponse published = waitForResponse(host, publishRequest);
    const QJsonObject currentRole = readJsonObject(
        QDir(fantarealRoot).absoluteFilePath(QStringLiteral("data/current_role_card.json")));
    if (!published.received || !published.ok
        || published.result.toMap().value(QStringLiteral("kind")).toString()
            != QStringLiteral("fantareal.role-card")
        || currentRole.value(QStringLiteral("raw")).toObject().value(QStringLiteral("name")).toString()
            != QStringLiteral("Router Hero")) {
        return fail(QStringLiteral("role artifact was not routed through native import"));
    }

    const QString duplicateRequest = host.request(host.extensionId(), host.sessionId(),
        QStringLiteral("artifacts.publish"), { { QStringLiteral("artifact"), roleArtifact } });
    const HostResponse duplicate = waitForResponse(host, duplicateRequest);
    if (!duplicate.received || duplicate.ok
        || duplicate.code != QStringLiteral("artifact_already_published")) {
        return fail(QStringLiteral("duplicate artifact publish was not rejected"));
    }

    QVariantMap undeclaredArtifact = roleArtifact;
    undeclaredArtifact.insert(QStringLiteral("kind"), QStringLiteral("image.png"));
    undeclaredArtifact.insert(QStringLiteral("mediaType"), QStringLiteral("image/png"));
    const QString undeclaredRequest = host.request(host.extensionId(), host.sessionId(),
        QStringLiteral("artifacts.publish"), { { QStringLiteral("artifact"), undeclaredArtifact } });
    const HostResponse undeclared = waitForResponse(host, undeclaredRequest);
    if (!undeclared.received || undeclared.ok
        || undeclared.code != QStringLiteral("artifact_kind_not_declared")) {
        return fail(QStringLiteral("undeclared artifact kind was not rejected"));
    }

    QVariantMap unsafeArtifact = roleArtifact;
    unsafeArtifact.insert(QStringLiteral("path"), selectedPng);
    const QString unsafeRequest = host.request(host.extensionId(), host.sessionId(),
        QStringLiteral("artifacts.publish"), { { QStringLiteral("artifact"), unsafeArtifact } });
    const HostResponse unsafe = waitForResponse(host, unsafeRequest);
    if (!unsafe.received || unsafe.ok || unsafe.code != QStringLiteral("artifact_path_unsafe")) {
        return fail(QStringLiteral("absolute artifact path was not rejected"));
    }

    const QString serviceRequest = host.request(host.extensionId(), host.sessionId(),
        QStringLiteral("service.invoke"), { { QStringLiteral("method"), QStringLiteral("extension.initialize") } });
    const HostResponse service = waitForResponse(host, serviceRequest);
    if (!service.received || service.ok || service.code != QStringLiteral("service_method_denied")) {
        return fail(QStringLiteral("reserved service method was not rejected"));
    }

    const QString closedSession = host.sessionId();
    host.closeSession(closedSession);
    if (host.active() || !host.sessionId().isEmpty()) {
        return fail(QStringLiteral("closed session remained active"));
    }
    const QString staleRequest = host.request(QStringLiteral("com.fantareal.web-host-test"), closedSession,
        QStringLiteral("context.get"));
    const HostResponse stale = waitForResponse(host, staleRequest);
    if (!stale.received || stale.ok || stale.code != QStringLiteral("session_invalid")) {
        return fail(QStringLiteral("stale session call was not rejected"));
    }
    QElapsedTimer cleanupTimer;
    cleanupTimer.start();
    while (QFileInfo::exists(workspaceRoot) && cleanupTimer.elapsed() < 4000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    if (QFileInfo::exists(workspaceRoot)) {
        return fail(QStringLiteral("closed session workspace was not cleaned"));
    }

    return runOptionalPackageSmoke(
        QCoreApplication::arguments(), registry, installer, host, fantarealRoot) ? 0 : 1;
}
