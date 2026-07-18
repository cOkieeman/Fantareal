#pragma once

#include "extensioninstaller.h"
#include "githubextensionsource.h"

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

class ExtensionManager : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList extensions READ extensions NOTIFY extensionsChanged)
    Q_PROPERTY(int extensionCount READ extensionCount NOTIFY extensionsChanged)
    Q_PROPERTY(QString rootPath READ rootPath CONSTANT)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

public:
    explicit ExtensionManager(QObject* parent = nullptr);
    explicit ExtensionManager(const QString& extensionRoot, QObject* parent = nullptr);

    QVariantList extensions() const;
    int extensionCount() const;
    QString rootPath() const;
    QString lastError() const;
    bool busy() const;

    Q_INVOKABLE QVariantMap refresh();
    Q_INVOKABLE QVariantMap installFromLocalDirectory(const QString& sourcePath);
    Q_INVOKABLE QVariantMap installFromGitHub(const QString& repositoryUrl);
    Q_INVOKABLE QVariantMap setExtensionEnabled(const QString& extensionId, bool enabled);
    Q_INVOKABLE QVariantMap rollbackExtension(const QString& extensionId);
    Q_INVOKABLE QVariantMap uninstallExtension(const QString& extensionId);

    static QString defaultExtensionRoot();

signals:
    void extensionsChanged();
    void lastErrorChanged();
    void busyChanged();
    void operationFinished(const QVariantMap& result);

private:
    void setLastError(const QString& error);
    void publishResult(const QVariantMap& result);

    ExtensionRegistry registry_;
    ExtensionInstaller installer_;
    GitHubExtensionSource githubSource_;
    QVariantList extensions_;
    QString lastError_;
};
