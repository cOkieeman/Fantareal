#pragma once

#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <functional>

struct ExtensionArtifactPublishResult {
    bool ok{};
    QVariantMap result;
    QString code;
    QString message;
};

class ExtensionArtifactRouter final {
public:
    using Importer = std::function<QVariantMap(const QString& absolutePath)>;

    void setImporters(Importer roleCardImporter, Importer worldbookImporter);
    ExtensionArtifactPublishResult publish(
        const QString& extensionId,
        const QString& sessionId,
        const QString& workspaceRoot,
        const QMap<QString, QStringList>& declaredArtifacts,
        const QVariantMap& artifact);
    void closeSession(const QString& extensionId, const QString& sessionId);

private:
    Importer roleCardImporter_;
    Importer worldbookImporter_;
    QSet<QString> publishedArtifacts_;
};
