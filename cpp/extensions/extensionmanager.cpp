#include "extensionmanager.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUrl>

namespace {

QVariantMap failure(const QString& code, const QString& message) {
    return {
        { QStringLiteral("ok"), false },
        { QStringLiteral("code"), code },
        { QStringLiteral("message"), message },
    };
}

QString localPathFromInput(const QString& input) {
    const QUrl url(input);
    if (url.isValid() && url.isLocalFile()) {
        return QDir::cleanPath(url.toLocalFile());
    }
    return QDir::cleanPath(QDir::fromNativeSeparators(input.trimmed()));
}

} // namespace

ExtensionManager::ExtensionManager(QObject* parent)
    : ExtensionManager(defaultExtensionRoot(), parent) {
}

ExtensionManager::ExtensionManager(const QString& extensionRoot, QObject* parent)
    : QObject(parent)
    , registry_(extensionRoot)
    , installer_(registry_)
    , githubSource_(installer_, registry_.rootPath(), this) {
    connect(&githubSource_, &GitHubExtensionSource::busyChanged, this, &ExtensionManager::busyChanged);
    connect(&githubSource_, &GitHubExtensionSource::finished, this, [this](const QVariantMap& result) {
        publishResult(result);
        if (result.value(QStringLiteral("ok")).toBool()) {
            refresh();
        }
        emit operationFinished(result);
    });
    refresh();
}

QVariantList ExtensionManager::extensions() const {
    return extensions_;
}

int ExtensionManager::extensionCount() const {
    return extensions_.size();
}

QString ExtensionManager::rootPath() const {
    return registry_.rootPath();
}

QString ExtensionManager::lastError() const {
    return lastError_;
}

bool ExtensionManager::busy() const {
    return githubSource_.busy();
}

QVariantMap ExtensionManager::refresh() {
    QString error;
    if (!registry_.load(&error)) {
        const QVariantMap response = failure(QStringLiteral("registry_load_failed"), error);
        publishResult(response);
        return response;
    }
    const QVariantList refreshed = registry_.extensions();
    const bool changed = refreshed != extensions_;
    extensions_ = refreshed;
    setLastError({});
    if (changed) {
        emit extensionsChanged();
    }
    QVariantMap response {
        { QStringLiteral("ok"), true },
        { QStringLiteral("message"), QStringLiteral("插件列表已刷新") },
        { QStringLiteral("count"), extensions_.size() },
    };
    if (!registry_.lastRecoveryPath().isEmpty()) {
        response.insert(QStringLiteral("warning"), true);
        response.insert(QStringLiteral("recoveryPath"), registry_.lastRecoveryPath());
        response.insert(QStringLiteral("message"), QStringLiteral("损坏的 registry 已隔离并重建"));
    }
    return response;
}

QVariantMap ExtensionManager::installFromLocalDirectory(const QString& sourcePath) {
    if (busy()) {
        const QVariantMap response = failure(QStringLiteral("operation_busy"), QStringLiteral("GitHub 插件操作进行中"));
        publishResult(response);
        return response;
    }
    const QString localPath = localPathFromInput(sourcePath);
    if (localPath.trimmed().isEmpty()) {
        const QVariantMap response = failure(QStringLiteral("source_empty"), QStringLiteral("请选择插件目录"));
        publishResult(response);
        return response;
    }
    const QVariantMap response = installer_.installFromDirectory(localPath);
    publishResult(response);
    if (response.value(QStringLiteral("ok")).toBool()) {
        refresh();
    }
    return response;
}

QVariantMap ExtensionManager::installFromGitHub(const QString& repositoryUrl) {
    const QVariantMap response = githubSource_.install(repositoryUrl);
    publishResult(response);
    return response;
}

QVariantMap ExtensionManager::setExtensionEnabled(const QString& extensionId, bool enabled) {
    if (busy()) {
        const QVariantMap response = failure(QStringLiteral("operation_busy"), QStringLiteral("GitHub 插件操作进行中"));
        publishResult(response);
        return response;
    }
    const QVariantMap response = installer_.setEnabled(extensionId, enabled);
    publishResult(response);
    if (response.value(QStringLiteral("ok")).toBool()) {
        refresh();
    }
    return response;
}

QVariantMap ExtensionManager::rollbackExtension(const QString& extensionId) {
    if (busy()) {
        const QVariantMap response = failure(QStringLiteral("operation_busy"), QStringLiteral("GitHub 插件操作进行中"));
        publishResult(response);
        return response;
    }
    const QVariantMap response = installer_.rollback(extensionId);
    publishResult(response);
    if (response.value(QStringLiteral("ok")).toBool()) {
        refresh();
    }
    return response;
}

QVariantMap ExtensionManager::uninstallExtension(const QString& extensionId) {
    if (busy()) {
        const QVariantMap response = failure(QStringLiteral("operation_busy"), QStringLiteral("GitHub 插件操作进行中"));
        publishResult(response);
        return response;
    }
    const QVariantMap response = installer_.uninstall(extensionId);
    publishResult(response);
    if (response.value(QStringLiteral("ok")).toBool()) {
        refresh();
    }
    return response;
}

QString ExtensionManager::defaultExtensionRoot() {
    const QString overrideRoot = qEnvironmentVariable("FANTAREAL_EXTENSION_ROOT").trimmed();
    if (!overrideRoot.isEmpty()) {
        return QDir::cleanPath(QFileInfo(overrideRoot).absoluteFilePath());
    }
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .absoluteFilePath(QStringLiteral("extensions"));
}

void ExtensionManager::setLastError(const QString& error) {
    if (lastError_ == error) {
        return;
    }
    lastError_ = error;
    emit lastErrorChanged();
}

void ExtensionManager::publishResult(const QVariantMap& result) {
    if (result.value(QStringLiteral("ok")).toBool()) {
        setLastError({});
    } else {
        setLastError(result.value(QStringLiteral("message")).toString());
    }
}
