#include "extensions/extensioninstaller.h"
#include "extensions/extensionmanifest.h"
#include "extensions/extensionregistry.h"
#include "extensions/githubextensionsource.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>
#include <QVariantMap>
#include <QtCore/private/qzipwriter_p.h>

#include <iostream>

namespace {

bool fail(const QString& message) {
    std::cerr << message.toStdString() << '\n';
    return false;
}

bool writeBytes(const QString& path, const QByteArray& payload) {
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        return false;
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(payload) == payload.size();
}

bool writeJson(const QString& path, const QJsonObject& object) {
    return writeBytes(path, QJsonDocument(object).toJson(QJsonDocument::Indented));
}

QJsonObject baseManifest(const QString& id, const QString& name) {
#ifdef Q_OS_WIN
    const QString platform = QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    const QString platform = QStringLiteral("macos");
#else
    const QString platform = QStringLiteral("linux");
#endif
    return {
        { QStringLiteral("schemaVersion"), 1 },
        { QStringLiteral("id"), id },
        { QStringLiteral("name"), name },
        { QStringLiteral("description"), QStringLiteral("Extension core test fixture") },
        { QStringLiteral("version"), QStringLiteral("1.2.3") },
        { QStringLiteral("publisher"), QStringLiteral("Fantareal Tests") },
        { QStringLiteral("compatibility"), QJsonObject {
                                                  { QStringLiteral("hostApi"), QStringLiteral(">=1.0.0 <2.0.0") },
                                                  { QStringLiteral("platforms"), QJsonArray { platform } },
                                              } },
        { QStringLiteral("permissions"), QJsonArray {
                                                QStringLiteral("files.user-selected.read"),
                                                QStringLiteral("storage.workspace"),
                                                QStringLiteral("artifacts.publish"),
                                            } },
    };
}

bool makeWebPlugin(const QDir& root, const QString& id = QStringLiteral("com.fantareal.web-test")) {
    if (!writeBytes(root.absoluteFilePath(QStringLiteral("web/index.html")), QByteArrayLiteral("<!doctype html><title>Test</title>"))) {
        return false;
    }
    QJsonObject manifest = baseManifest(id, QStringLiteral("Web Test"));
    manifest.insert(QStringLiteral("entrypoints"), QJsonObject {
                                                        { QStringLiteral("page"), QJsonObject {
                                                                                      { QStringLiteral("type"), QStringLiteral("web") },
                                                                                      { QStringLiteral("path"), QStringLiteral("web/index.html") },
                                                                                      { QStringLiteral("bridge"), QStringLiteral("fantareal.extension.v1") },
                                                                                  } },
                                                    });
    manifest.insert(QStringLiteral("contributes"), QJsonObject {
                                                         { QStringLiteral("artifacts"), QJsonArray {
                                                                                              QJsonObject {
                                                                                                  { QStringLiteral("kind"), QStringLiteral("fantareal.role-card") },
                                                                                                  { QStringLiteral("mediaTypes"), QJsonArray { QStringLiteral("application/json") } },
                                                                                              },
                                                                                          } },
                                                     });
    return writeJson(root.absoluteFilePath(QStringLiteral("fantareal-extension.json")), manifest);
}

bool makeServicePlugin(const QDir& root) {
    if (!makeWebPlugin(root, QStringLiteral("com.fantareal.service-test"))
        || !writeBytes(root.absoluteFilePath(QStringLiteral("uv.lock")), QByteArrayLiteral("version = 1\n"))
        || !writeBytes(
            root.absoluteFilePath(QStringLiteral("src/fantareal_service_test/service.py")),
            QByteArrayLiteral("def main():\n    return 0\n"))) {
        return false;
    }
    QFile manifestFile(root.absoluteFilePath(QStringLiteral("fantareal-extension.json")));
    if (!manifestFile.open(QIODevice::ReadOnly)) {
        return false;
    }
    QJsonObject manifest = QJsonDocument::fromJson(manifestFile.readAll()).object();
    QJsonObject entrypoints = manifest.value(QStringLiteral("entrypoints")).toObject();
    entrypoints.insert(QStringLiteral("service"), QJsonObject {
                                                         { QStringLiteral("type"), QStringLiteral("python") },
                                                         { QStringLiteral("module"), QStringLiteral("fantareal_service_test.service") },
                                                         { QStringLiteral("protocol"), QStringLiteral("jsonrpc-2.0-stdio") },
                                                         { QStringLiteral("lockfile"), QStringLiteral("uv.lock") },
                                                     });
    manifest.insert(QStringLiteral("entrypoints"), entrypoints);
    return writeJson(root.absoluteFilePath(QStringLiteral("fantareal-extension.json")), manifest);
}

QJsonObject readJson(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).object();
}

bool testGitHubUrlParser() {
    const GitHubRepositorySpec root = GitHubExtensionSource::parseRepositoryUrl(
        QStringLiteral("https://github.com/cOkieeman/Fantareal-tavern-card-converter"));
    if (!root.ok || root.owner != QStringLiteral("cOkieeman")
        || root.repository != QStringLiteral("Fantareal-tavern-card-converter") || !root.ref.isEmpty()) {
        return fail(QStringLiteral("GitHub repository root URL should be accepted"));
    }

    const GitHubRepositorySpec branch = GitHubExtensionSource::parseRepositoryUrl(
        QStringLiteral("https://github.com/cOkieeman/Fantareal-tavern-card-converter/tree/feature/test"));
    if (!branch.ok || branch.ref != QStringLiteral("feature/test")) {
        return fail(QStringLiteral("GitHub tree URL should preserve slash-containing ref"));
    }

    const QStringList rejected = {
        QStringLiteral("http://github.com/owner/repo"),
        QStringLiteral("https://github.example/owner/repo"),
        QStringLiteral("https://github.com/owner/repo/issues"),
        QStringLiteral("https://github.com/owner/repo?ref=main"),
        QStringLiteral("https://user@github.com/owner/repo"),
        QStringLiteral("https://github.com/owner/repo/tree/bad..ref"),
    };
    for (const QString& url : rejected) {
        if (GitHubExtensionSource::parseRepositoryUrl(url).ok) {
            return fail(QStringLiteral("unsafe or unsupported GitHub URL should be rejected: %1").arg(url));
        }
    }
    return true;
}

bool testArchiveExtraction(const QDir& fixtureRoot) {
    const QString archivePath = fixtureRoot.absoluteFilePath(QStringLiteral("safe.zip"));
    QZipWriter writer(archivePath);
    writer.addDirectory(QStringLiteral("repo-012345/"));
    writer.addFile(QStringLiteral("repo-012345/fantareal-extension.json"), QByteArrayLiteral("{}"));
    writer.addFile(QStringLiteral("repo-012345/web/index.html"), QByteArrayLiteral("<title>ok</title>"));
    writer.close();
    if (writer.status() != QZipWriter::NoError) {
        return fail(QStringLiteral("failed to write safe ZIP fixture"));
    }

    const QString extracted = fixtureRoot.absoluteFilePath(QStringLiteral("safe-extracted"));
    const QVariantMap extraction = GitHubExtensionSource::extractArchive(archivePath, extracted);
    if (!extraction.value(QStringLiteral("ok")).toBool()
        || extraction.value(QStringLiteral("fileCount")).toInt() != 2
        || !QFileInfo::exists(QDir(extracted).absoluteFilePath(QStringLiteral("web/index.html")))) {
        return fail(QStringLiteral("safe GitHub ZIP should extract: %1")
                .arg(extraction.value(QStringLiteral("message")).toString()));
    }

    const QString traversalArchive = fixtureRoot.absoluteFilePath(QStringLiteral("traversal.zip"));
    QZipWriter traversalWriter(traversalArchive);
    traversalWriter.addDirectory(QStringLiteral("repo-012345/"));
    traversalWriter.addFile(QStringLiteral("repo-012345/../escape.txt"), QByteArrayLiteral("escape"));
    traversalWriter.close();
    const QVariantMap traversal = GitHubExtensionSource::extractArchive(
        traversalArchive, fixtureRoot.absoluteFilePath(QStringLiteral("traversal-extracted")));
    if (traversal.value(QStringLiteral("ok")).toBool()
        || QFileInfo::exists(fixtureRoot.absoluteFilePath(QStringLiteral("escape.txt")))) {
        return fail(QStringLiteral("ZIP traversal entry should be rejected: ok=%1 code=%2 message=%3")
                .arg(traversal.value(QStringLiteral("ok")).toBool())
                .arg(traversal.value(QStringLiteral("code")).toString(),
                    traversal.value(QStringLiteral("message")).toString()));
    }

    const QString symlinkArchive = fixtureRoot.absoluteFilePath(QStringLiteral("symlink.zip"));
    QZipWriter symlinkWriter(symlinkArchive);
    symlinkWriter.addDirectory(QStringLiteral("repo-012345/"));
    symlinkWriter.addSymLink(QStringLiteral("repo-012345/link"), QStringLiteral("../outside"));
    symlinkWriter.close();
    const QVariantMap symlink = GitHubExtensionSource::extractArchive(
        symlinkArchive, fixtureRoot.absoluteFilePath(QStringLiteral("symlink-extracted")));
    if (symlink.value(QStringLiteral("ok")).toBool()
        || symlink.value(QStringLiteral("code")).toString() != QStringLiteral("archive_entry_unsafe")) {
        return fail(QStringLiteral("ZIP symlink entry should be rejected"));
    }
    return true;
}

bool runOptionalSourceSmoke(const QStringList& arguments) {
    QString localSource;
    QString githubUrl;
    for (const QString& argument : arguments) {
        if (argument.startsWith(QStringLiteral("--local-source="))) {
            localSource = argument.mid(QStringLiteral("--local-source=").size());
        } else if (argument.startsWith(QStringLiteral("--github-url="))) {
            githubUrl = argument.mid(QStringLiteral("--github-url=").size());
        }
    }
    if (localSource.isEmpty() && githubUrl.isEmpty()) {
        return true;
    }

    QTemporaryDir smokeRoot;
    if (!smokeRoot.isValid()) {
        return fail(QStringLiteral("failed to create optional smoke root"));
    }
    ExtensionRegistry registry(QDir(smokeRoot.path()).absoluteFilePath(QStringLiteral("extensions")));
    QString error;
    if (!registry.load(&error)) {
        return fail(QStringLiteral("optional smoke registry failed: %1").arg(error));
    }
    ExtensionInstaller installer(registry);
    if (!localSource.isEmpty()) {
        const QVariantMap localResult = installer.installFromDirectory(localSource);
        if (!localResult.value(QStringLiteral("ok")).toBool()) {
            return fail(QStringLiteral("real local source smoke failed: %1")
                    .arg(localResult.value(QStringLiteral("message")).toString()));
        }
        std::cout << "LOCAL_SOURCE_SMOKE_COMMIT=none\n";
    }

    if (!githubUrl.isEmpty()) {
        GitHubExtensionSource source(installer, registry.rootPath());
        QVariantMap finalResult;
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(&source, &GitHubExtensionSource::finished, &loop, [&](const QVariantMap& result) {
            finalResult = result;
            loop.quit();
        });
        const QVariantMap accepted = source.install(githubUrl);
        if (!accepted.value(QStringLiteral("ok")).toBool()) {
            return fail(QStringLiteral("real GitHub source smoke was not accepted: %1")
                    .arg(accepted.value(QStringLiteral("message")).toString()));
        }
        timeout.start(120000);
        loop.exec();
        if (finalResult.isEmpty()) {
            return fail(QStringLiteral("real GitHub source smoke timed out"));
        }
        if (!finalResult.value(QStringLiteral("ok")).toBool()) {
            return fail(QStringLiteral("real GitHub source smoke failed: %1")
                    .arg(finalResult.value(QStringLiteral("message")).toString()));
        }
        const QString commit = finalResult.value(QStringLiteral("resolvedCommit")).toString();
        if (commit.size() != 40 || registry.extensions().isEmpty()
            || registry.extensions().first().toMap().value(QStringLiteral("resolvedCommit")).toString() != commit) {
            return fail(QStringLiteral("real GitHub smoke did not persist immutable commit"));
        }
        std::cout << "GITHUB_SOURCE_SMOKE_COMMIT=" << commit.toStdString() << '\n';
    }
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    QTemporaryDir fixture;
    if (!fixture.isValid()) {
        return fail(QStringLiteral("failed to create temporary fixture")) ? 0 : 1;
    }
    QDir fixtureRoot(fixture.path());
    if (!testGitHubUrlParser() || !testArchiveExtraction(fixtureRoot)) {
        return 1;
    }
    const QString webPluginPath = fixtureRoot.absoluteFilePath(QStringLiteral("web-plugin"));
    const QString servicePluginPath = fixtureRoot.absoluteFilePath(QStringLiteral("service-plugin"));
    if (!QDir().mkpath(webPluginPath) || !QDir().mkpath(servicePluginPath)
        || !makeWebPlugin(QDir(webPluginPath)) || !makeServicePlugin(QDir(servicePluginPath))) {
        return fail(QStringLiteral("failed to create extension fixtures")) ? 0 : 1;
    }

    const ExtensionManifestResult webManifest = ExtensionManifestParser::loadFromDirectory(webPluginPath);
    if (!webManifest.ok || !webManifest.manifest.hasPage || webManifest.manifest.hasService
        || webManifest.manifest.id != QStringLiteral("com.fantareal.web-test")
        || webManifest.manifest.artifactMediaTypes.value(QStringLiteral("fantareal.role-card"))
            != QStringList { QStringLiteral("application/json") }) {
        return fail(QStringLiteral("web-only manifest should be accepted: %1").arg(webManifest.message)) ? 0 : 1;
    }

    const ExtensionManifestResult serviceManifest = ExtensionManifestParser::loadFromDirectory(servicePluginPath);
    if (!serviceManifest.ok || !serviceManifest.manifest.hasPage || !serviceManifest.manifest.hasService) {
        return fail(QStringLiteral("page + service manifest should be accepted: %1").arg(serviceManifest.message)) ? 0 : 1;
    }

    QJsonObject missingEntrypoint = baseManifest(QStringLiteral("com.fantareal.invalid"), QStringLiteral("Invalid"));
    missingEntrypoint.insert(QStringLiteral("entrypoints"), QJsonObject {});
    const ExtensionManifestResult missingResult = ExtensionManifestParser::parse(
        QJsonDocument(missingEntrypoint).toJson(), webPluginPath);
    if (missingResult.ok || missingResult.code != QStringLiteral("entrypoint_missing")) {
        return fail(QStringLiteral("manifest without page/service should be rejected")) ? 0 : 1;
    }

    QJsonObject traversalManifest = baseManifest(QStringLiteral("com.fantareal.traversal"), QStringLiteral("Traversal"));
    traversalManifest.insert(QStringLiteral("entrypoints"), QJsonObject {
                                                              { QStringLiteral("page"), QJsonObject {
                                                                                            { QStringLiteral("type"), QStringLiteral("web") },
                                                                                            { QStringLiteral("path"), QStringLiteral("../outside.html") },
                                                                                            { QStringLiteral("bridge"), QStringLiteral("fantareal.extension.v1") },
                                                                                        } },
                                                          });
    const ExtensionManifestResult traversalResult = ExtensionManifestParser::parse(
        QJsonDocument(traversalManifest).toJson(), webPluginPath);
    if (traversalResult.ok || traversalResult.code != QStringLiteral("page_entrypoint_invalid")) {
        return fail(QStringLiteral("path traversal entrypoint should be rejected")) ? 0 : 1;
    }

    QJsonObject incompatibleManifest = baseManifest(
        QStringLiteral("com.fantareal.future-host"), QStringLiteral("Future Host"));
    QJsonObject incompatibleCompatibility = incompatibleManifest.value(QStringLiteral("compatibility")).toObject();
    incompatibleCompatibility.insert(QStringLiteral("hostApi"), QStringLiteral(">=2.0.0 <3.0.0"));
    incompatibleManifest.insert(QStringLiteral("compatibility"), incompatibleCompatibility);
    incompatibleManifest.insert(QStringLiteral("entrypoints"), webManifest.manifest.raw.value(QStringLiteral("entrypoints")));
    const ExtensionManifestResult incompatibleResult = ExtensionManifestParser::parse(
        QJsonDocument(incompatibleManifest).toJson(), webPluginPath);
    if (incompatibleResult.ok || incompatibleResult.code != QStringLiteral("host_api_incompatible")) {
        return fail(QStringLiteral("incompatible host API should be rejected")) ? 0 : 1;
    }

    QJsonObject duplicateArtifactManifest = webManifest.manifest.raw;
    QJsonObject duplicateContributes = duplicateArtifactManifest.value(QStringLiteral("contributes")).toObject();
    QJsonArray duplicateArtifacts = duplicateContributes.value(QStringLiteral("artifacts")).toArray();
    duplicateArtifacts.append(duplicateArtifacts.first());
    duplicateContributes.insert(QStringLiteral("artifacts"), duplicateArtifacts);
    duplicateArtifactManifest.insert(QStringLiteral("contributes"), duplicateContributes);
    const ExtensionManifestResult duplicateArtifactResult = ExtensionManifestParser::parse(
        QJsonDocument(duplicateArtifactManifest).toJson(), webPluginPath);
    if (duplicateArtifactResult.ok
        || duplicateArtifactResult.code != QStringLiteral("artifact_kind_duplicate")) {
        return fail(QStringLiteral("duplicate artifact declaration should be rejected")) ? 0 : 1;
    }

    if (!writeBytes(QDir(servicePluginPath).absoluteFilePath(QStringLiteral(".git/config")), QByteArrayLiteral("ignored"))) {
        return fail(QStringLiteral("failed to create ignored local source data")) ? 0 : 1;
    }

    const QString extensionRoot = fixtureRoot.absoluteFilePath(QStringLiteral("host/extensions"));
    ExtensionRegistry registry(extensionRoot);
    QString registryError;
    if (!registry.load(&registryError)) {
        return fail(QStringLiteral("empty registry should load: %1").arg(registryError)) ? 0 : 1;
    }
    ExtensionInstaller installer(registry);
    const QVariantMap installResult = installer.installFromDirectory(servicePluginPath);
    if (!installResult.value(QStringLiteral("ok")).toBool()) {
        return fail(QStringLiteral("local directory install failed: %1").arg(installResult.value(QStringLiteral("message")).toString())) ? 0 : 1;
    }
    const QString relativePackagePath = installResult.value(QStringLiteral("packagePath")).toString();
    const QString installedPackage = QDir(extensionRoot).absoluteFilePath(relativePackagePath);
    if (!QFileInfo::exists(QDir(installedPackage).absoluteFilePath(QStringLiteral("fantareal-extension.json")))
        || QFileInfo::exists(QDir(installedPackage).absoluteFilePath(QStringLiteral(".git/config")))
        || !relativePackagePath.startsWith(QStringLiteral("packages/com.fantareal.service-test/1.2.3-local-"))) {
        return fail(QStringLiteral("installed package layout or ignored-directory policy is wrong")) ? 0 : 1;
    }

    const QVariantList installedExtensions = registry.extensions();
    if (installedExtensions.size() != 1) {
        return fail(QStringLiteral("registry should contain one extension")) ? 0 : 1;
    }
    const QVariantMap installedEntry = installedExtensions.first().toMap();
    if (installedEntry.value(QStringLiteral("id")).toString() != QStringLiteral("com.fantareal.service-test")
        || !installedEntry.value(QStringLiteral("enabled")).toBool()
        || !installedEntry.value(QStringLiteral("hasPage")).toBool()
        || !installedEntry.value(QStringLiteral("hasService")).toBool()) {
        return fail(QStringLiteral("registry display entry is incomplete")) ? 0 : 1;
    }

    const QVariantMap duplicateResult = installer.installFromDirectory(servicePluginPath);
    if (!duplicateResult.value(QStringLiteral("ok")).toBool()
        || !duplicateResult.value(QStringLiteral("alreadyInstalled")).toBool()
        || registry.extensions().first().toMap().value(QStringLiteral("packageCount")).toInt() != 1) {
        return fail(QStringLiteral("same digest install should be idempotent")) ? 0 : 1;
    }

    const QVariantMap invalidCommitResult = installer.installFromGitHubDirectory(
        servicePluginPath,
        QStringLiteral("https://github.com/fantareal/service-test"),
        QStringLiteral("not-a-commit"));
    if (invalidCommitResult.value(QStringLiteral("ok")).toBool()
        || registry.extensions().first().toMap().value(QStringLiteral("packageCount")).toInt() != 1) {
        return fail(QStringLiteral("invalid GitHub commit must not change registry")) ? 0 : 1;
    }

    if (!writeBytes(QDir(servicePluginPath).absoluteFilePath(QStringLiteral("release.txt")), QByteArrayLiteral("remote"))) {
        return fail(QStringLiteral("failed to mutate remote package fixture")) ? 0 : 1;
    }
    const QString remoteCommit(40, QLatin1Char('1'));
    const QVariantMap remoteInstall = installer.installFromGitHubDirectory(
        servicePluginPath,
        QStringLiteral("https://github.com/fantareal/service-test"),
        remoteCommit);
    if (!remoteInstall.value(QStringLiteral("ok")).toBool()) {
        return fail(QStringLiteral("GitHub package simulation should install: %1")
                .arg(remoteInstall.value(QStringLiteral("message")).toString())) ? 0 : 1;
    }
    const QString remotePackagePath = remoteInstall.value(QStringLiteral("packagePath")).toString();
    const QVariantMap remoteEntry = registry.extensions().first().toMap();
    if (remoteEntry.value(QStringLiteral("sourceType")).toString() != QStringLiteral("github")
        || remoteEntry.value(QStringLiteral("resolvedCommit")).toString() != remoteCommit
        || !remoteEntry.value(QStringLiteral("canRollback")).toBool()
        || remoteEntry.value(QStringLiteral("packageCount")).toInt() != 2) {
        return fail(QStringLiteral("GitHub package metadata or rollback marker is incomplete")) ? 0 : 1;
    }
    const QVariantMap rollbackResult = installer.rollback(QStringLiteral("com.fantareal.service-test"));
    const QVariantMap rolledBackEntry = registry.extensions().first().toMap();
    if (!rollbackResult.value(QStringLiteral("ok")).toBool()
        || rolledBackEntry.value(QStringLiteral("sourceType")).toString() != QStringLiteral("local-directory")
        || !rolledBackEntry.value(QStringLiteral("canRollback")).toBool()) {
        return fail(QStringLiteral("rollback should restore previous local package")) ? 0 : 1;
    }

    const QString brokenUpdatePath = fixtureRoot.absoluteFilePath(QStringLiteral("broken-update"));
    if (!QDir().mkpath(brokenUpdatePath)
        || !writeJson(
            QDir(brokenUpdatePath).absoluteFilePath(QStringLiteral("fantareal-extension.json")),
            baseManifest(QStringLiteral("com.fantareal.service-test"), QStringLiteral("Broken Update")))) {
        return fail(QStringLiteral("failed to create broken update fixture")) ? 0 : 1;
    }
    const QVariantMap beforeBrokenUpdate = registry.extensions().first().toMap();
    const QVariantMap brokenUpdate = installer.installFromGitHubDirectory(
        brokenUpdatePath,
        QStringLiteral("https://github.com/fantareal/service-test"),
        QString(40, QLatin1Char('2')));
    const QVariantMap afterBrokenUpdate = registry.extensions().first().toMap();
    if (brokenUpdate.value(QStringLiteral("ok")).toBool()
        || afterBrokenUpdate.value(QStringLiteral("packagePath"))
            != beforeBrokenUpdate.value(QStringLiteral("packagePath"))
        || afterBrokenUpdate.value(QStringLiteral("packageCount"))
            != beforeBrokenUpdate.value(QStringLiteral("packageCount"))) {
        return fail(QStringLiteral("failed update must not change active package or package history")) ? 0 : 1;
    }

    const QVariantMap disableResult = installer.setEnabled(QStringLiteral("com.fantareal.service-test"), false);
    if (!disableResult.value(QStringLiteral("ok")).toBool()) {
        return fail(QStringLiteral("disable should succeed")) ? 0 : 1;
    }
    ExtensionRegistry reloadedRegistry(extensionRoot);
    if (!reloadedRegistry.load(&registryError) || reloadedRegistry.extensions().size() != 1
        || reloadedRegistry.extensions().first().toMap().value(QStringLiteral("enabled")).toBool()) {
        return fail(QStringLiteral("disabled state should survive registry reload")) ? 0 : 1;
    }

    const QVariantMap uninstallResult = installer.uninstall(QStringLiteral("com.fantareal.service-test"));
    if (!uninstallResult.value(QStringLiteral("ok")).toBool() || QFileInfo::exists(installedPackage)
        || QFileInfo::exists(QDir(extensionRoot).absoluteFilePath(remotePackagePath))) {
        return fail(QStringLiteral("uninstall should remove registry entry and package")) ? 0 : 1;
    }
    ExtensionRegistry emptyRegistry(extensionRoot);
    if (!emptyRegistry.load(&registryError) || !emptyRegistry.extensions().isEmpty()) {
        return fail(QStringLiteral("registry should be empty after uninstall")) ? 0 : 1;
    }

    const QString corruptRoot = fixtureRoot.absoluteFilePath(QStringLiteral("corrupt/extensions"));
    ExtensionRegistry corruptRegistry(corruptRoot);
    if (!corruptRegistry.load(&registryError)
        || !writeBytes(corruptRegistry.registryPath(), QByteArrayLiteral("{not-json"))
        || !corruptRegistry.load(&registryError)
        || corruptRegistry.lastRecoveryPath().isEmpty()
        || !QFileInfo::exists(corruptRegistry.lastRecoveryPath())) {
        return fail(QStringLiteral("corrupt registry should be isolated and rebuilt: %1").arg(registryError)) ? 0 : 1;
    }
    const QJsonObject recoveredRegistry = readJson(corruptRegistry.registryPath());
    if (recoveredRegistry.value(QStringLiteral("schemaVersion")).toInt() != 1
        || !recoveredRegistry.value(QStringLiteral("extensions")).toArray().isEmpty()) {
        return fail(QStringLiteral("recovered registry should be a valid empty document")) ? 0 : 1;
    }

    return runOptionalSourceSmoke(QCoreApplication::arguments()) ? 0 : 1;
}
