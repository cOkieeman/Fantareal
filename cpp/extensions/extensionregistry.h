#pragma once

#include "extensionmanifest.h"

#include <QJsonArray>
#include <QString>
#include <QStringList>
#include <QVariantList>

class ExtensionRegistry {
public:
    explicit ExtensionRegistry(QString extensionRoot);

    bool load(QString* error = nullptr);
    bool save(QString* error = nullptr) const;

    QVariantList extensions() const;
    QStringList packagePaths(const QString& extensionId) const;
    QString lastRecoveryPath() const;
    QString rootPath() const;
    QString registryPath() const;

    bool upsertPackage(
        const ExtensionManifest& manifest,
        const QString& packagePath,
        const QString& digest,
        const QString& sourceType,
        const QString& sourceUrl,
        const QString& resolvedCommit,
        QStringList* retiredPackagePaths = nullptr,
        QString* error = nullptr);
    bool setEnabled(const QString& extensionId, bool enabled, QString* error = nullptr);
    bool activateRollbackPackage(const QString& extensionId, QString* activatedPath, QString* error = nullptr);
    bool removeExtension(const QString& extensionId, QString* error = nullptr);

private:
    bool ensureLayout(QString* error = nullptr) const;
    bool recoverCorruptRegistry(const QString& reason, QString* error);
    int indexOf(const QString& extensionId) const;
    static QVariantMap toDisplayMap(const QJsonObject& entry);
    static bool validateRegistryDocument(const QJsonObject& root, QString* error);

    QString extensionRoot_;
    QString registryPath_;
    QJsonArray entries_;
    QString lastRecoveryPath_;
};
