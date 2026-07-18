#pragma once

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariantMap>

struct ExtensionManifest {
    int schemaVersion{};
    QString id;
    QString name;
    QString description;
    QString version;
    QString publisher;
    QString hostApi;
    QString pythonRange;
    QStringList platforms;
    QStringList permissions;

    bool hasPage{};
    QString pagePath;
    QString pageBridge;

    bool hasService{};
    QString serviceModule;
    QString serviceProtocol;
    QString serviceLockfile;

    QMap<QString, QStringList> artifactMediaTypes;

    QJsonObject raw;

    QVariantMap toVariantMap() const;
};

struct ExtensionManifestResult {
    bool ok{};
    ExtensionManifest manifest;
    QString code;
    QString message;
};

class ExtensionManifestParser {
public:
    static ExtensionManifestResult loadFromDirectory(const QString& packageRoot);
    static ExtensionManifestResult parse(const QByteArray& payload, const QString& packageRoot);
    static bool isSafeRelativePath(const QString& path, QString* normalizedPath = nullptr);
    static bool pathIsWithin(const QString& rootPath, const QString& candidatePath);
};
