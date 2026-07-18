#pragma once

#include "extensionregistry.h"

#include <QString>
#include <QVariantMap>

class ExtensionInstaller {
public:
    explicit ExtensionInstaller(ExtensionRegistry& registry);

    QVariantMap installFromDirectory(const QString& sourceDirectory);
    QVariantMap installFromGitHubDirectory(
        const QString& sourceDirectory,
        const QString& sourceUrl,
        const QString& resolvedCommit);
    QVariantMap setEnabled(const QString& extensionId, bool enabled);
    QVariantMap rollback(const QString& extensionId);
    QVariantMap uninstall(const QString& extensionId);

private:
    struct CopyState {
        int fileCount{};
        qint64 totalBytes{};
    };

    bool copyDirectorySecure(
        const QString& sourceRoot,
        const QString& sourceDirectory,
        const QString& destinationDirectory,
        CopyState* state,
        QString* error) const;
    bool computePackageDigest(const QString& packageRoot, QString* digest, QString* error) const;
    bool removePathWithinRoot(const QString& path, QString* error) const;
    QVariantMap installPreparedDirectory(
        const QString& sourceDirectory,
        const QString& sourceType,
        const QString& sourceUrl,
        const QString& resolvedCommit);
    static bool shouldIgnoreName(const QString& name);
    static QVariantMap result(bool ok, const QString& message, const QString& code = {});

    ExtensionRegistry& registry_;
};
