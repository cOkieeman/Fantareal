#include "extensionartifactrouter.h"

#include "extensionmanifest.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaType>
#include <QRegularExpression>

namespace {

constexpr qint64 kMaxArtifactBytes = 64LL * 1024 * 1024;
constexpr qsizetype kMaxEnvelopeBytes = 64 * 1024;

ExtensionArtifactPublishResult rejected(const QString& code, const QString& message) {
    return { false, {}, code, message };
}

bool requiredEnvelopeString(
    const QVariantMap& artifact,
    const QString& key,
    QString* value,
    int maxLength) {
    const QVariant raw = artifact.value(key);
    if (raw.metaType().id() != QMetaType::QString) {
        return false;
    }
    const QString parsed = raw.toString().trimmed();
    if (parsed.isEmpty() || parsed.size() > maxLength) {
        return false;
    }
    *value = parsed;
    return true;
}

QString publishKey(
    const QString& extensionId,
    const QString& sessionId,
    const QString& kind,
    const QString& canonicalPath) {
    return extensionId + QChar::Null + sessionId + QChar::Null + kind + QChar::Null + canonicalPath;
}

QString sessionPrefix(const QString& extensionId, const QString& sessionId) {
    return extensionId + QChar::Null + sessionId + QChar::Null;
}

} // namespace

void ExtensionArtifactRouter::setImporters(Importer roleCardImporter, Importer worldbookImporter) {
    roleCardImporter_ = std::move(roleCardImporter);
    worldbookImporter_ = std::move(worldbookImporter);
}

ExtensionArtifactPublishResult ExtensionArtifactRouter::publish(
    const QString& extensionId,
    const QString& sessionId,
    const QString& workspaceRoot,
    const QMap<QString, QStringList>& declaredArtifacts,
    const QVariantMap& artifact) {
    if (extensionId.trimmed().isEmpty() || sessionId.trimmed().isEmpty()) {
        return rejected(QStringLiteral("artifact_session_invalid"),
            QStringLiteral("artifact 缺少有效的 extension/session 上下文"));
    }
    const QByteArray envelope = QJsonDocument(QJsonObject::fromVariantMap(artifact)).toJson(QJsonDocument::Compact);
    if (artifact.isEmpty() || envelope.isEmpty() || envelope.size() > kMaxEnvelopeBytes) {
        return rejected(QStringLiteral("artifact_envelope_invalid"),
            QStringLiteral("artifact envelope 为空、无效或超过 64 KiB"));
    }

    static const QSet<QString> allowedKeys {
        QStringLiteral("id"),
        QStringLiteral("kind"),
        QStringLiteral("mediaType"),
        QStringLiteral("path"),
        QStringLiteral("suggestedName"),
        QStringLiteral("metadata"),
    };
    for (auto it = artifact.constBegin(); it != artifact.constEnd(); ++it) {
        if (!allowedKeys.contains(it.key())) {
            return rejected(QStringLiteral("artifact_envelope_invalid"),
                QStringLiteral("artifact envelope 包含未知字段：%1").arg(it.key()));
        }
    }

    QString kind;
    QString mediaType;
    QString relativePath;
    QString suggestedName;
    if (!requiredEnvelopeString(artifact, QStringLiteral("kind"), &kind, 160)
        || !requiredEnvelopeString(artifact, QStringLiteral("mediaType"), &mediaType, 160)
        || !requiredEnvelopeString(artifact, QStringLiteral("path"), &relativePath, 1024)
        || !requiredEnvelopeString(artifact, QStringLiteral("suggestedName"), &suggestedName, 180)) {
        return rejected(QStringLiteral("artifact_envelope_invalid"),
            QStringLiteral("artifact 必须包含有效的 kind、mediaType、path 和 suggestedName"));
    }
    mediaType = mediaType.toLower();
    if (artifact.contains(QStringLiteral("id"))) {
        QString artifactId;
        if (!requiredEnvelopeString(artifact, QStringLiteral("id"), &artifactId, 160)) {
            return rejected(QStringLiteral("artifact_envelope_invalid"),
                QStringLiteral("artifact id 必须是非空短字符串"));
        }
    }
    if (artifact.contains(QStringLiteral("metadata"))
        && artifact.value(QStringLiteral("metadata")).metaType().id() != QMetaType::QVariantMap) {
        return rejected(QStringLiteral("artifact_envelope_invalid"),
            QStringLiteral("artifact metadata 必须是 object"));
    }
    static const QRegularExpression unsafeSuggestedName(QStringLiteral("[\\\\/\\x00-\\x1f]"));
    if (unsafeSuggestedName.match(suggestedName).hasMatch()) {
        return rejected(QStringLiteral("artifact_suggested_name_invalid"),
            QStringLiteral("artifact suggestedName 不能包含路径分隔符或控制字符"));
    }

    const auto declaration = declaredArtifacts.constFind(kind);
    if (declaration == declaredArtifacts.constEnd()) {
        return rejected(QStringLiteral("artifact_kind_not_declared"),
            QStringLiteral("插件 manifest 未声明 artifact kind：%1").arg(kind));
    }
    if (!declaration.value().contains(mediaType)) {
        return rejected(QStringLiteral("artifact_media_type_not_declared"),
            QStringLiteral("插件 manifest 未为 %1 声明 mediaType：%2").arg(kind, mediaType));
    }
    if (kind != QStringLiteral("fantareal.role-card")
        && kind != QStringLiteral("fantareal.worldbook")) {
        return rejected(QStringLiteral("artifact_kind_unsupported"),
            QStringLiteral("Fantareal 当前不支持导入 artifact kind：%1").arg(kind));
    }
    if (mediaType != QStringLiteral("application/json")) {
        return rejected(QStringLiteral("artifact_media_type_unsupported"),
            QStringLiteral("角色卡与世界书 artifact 必须使用 application/json"));
    }

    QString normalizedPath;
    if (!ExtensionManifestParser::isSafeRelativePath(relativePath, &normalizedPath)) {
        return rejected(QStringLiteral("artifact_path_unsafe"),
            QStringLiteral("artifact path 必须是 session workspace 内的安全相对路径"));
    }
    const QFileInfo workspaceInfo(workspaceRoot);
    const QString canonicalWorkspace = workspaceInfo.canonicalFilePath();
    const QString candidatePath = QDir(workspaceRoot).absoluteFilePath(normalizedPath);
    const QFileInfo candidateInfo(candidatePath);
    const QString canonicalArtifact = candidateInfo.canonicalFilePath();
    if (!workspaceInfo.isDir() || workspaceInfo.isSymLink() || workspaceInfo.isJunction()
        || canonicalWorkspace.isEmpty() || !candidateInfo.isFile() || candidateInfo.isSymLink()
        || candidateInfo.isJunction() || canonicalArtifact.isEmpty()
        || !ExtensionManifestParser::pathIsWithin(canonicalWorkspace, canonicalArtifact)) {
        return rejected(QStringLiteral("artifact_path_unsafe"),
            QStringLiteral("artifact 文件不存在、不是普通文件或离开 session workspace"));
    }
    if (candidateInfo.size() <= 0 || candidateInfo.size() > kMaxArtifactBytes) {
        return rejected(QStringLiteral("artifact_size_invalid"),
            QStringLiteral("artifact 文件为空或超过 64 MiB"));
    }
    if (candidateInfo.suffix().compare(QStringLiteral("json"), Qt::CaseInsensitive) != 0) {
        return rejected(QStringLiteral("artifact_file_type_invalid"),
            QStringLiteral("角色卡与世界书 artifact 必须是 .json 文件"));
    }

    const QString key = publishKey(extensionId, sessionId, kind, canonicalArtifact);
    if (publishedArtifacts_.contains(key)) {
        return rejected(QStringLiteral("artifact_already_published"),
            QStringLiteral("该 artifact 已在当前 session 中成功导入"));
    }

    const Importer& importer = kind == QStringLiteral("fantareal.role-card")
        ? roleCardImporter_
        : worldbookImporter_;
    if (!importer) {
        return rejected(QStringLiteral("artifact_importer_unavailable"),
            QStringLiteral("Fantareal 原生 importer 当前不可用"));
    }
    const QVariantMap nativeResult = importer(canonicalArtifact);
    if (!nativeResult.value(QStringLiteral("ok")).toBool()) {
        const QString message = nativeResult.value(QStringLiteral("message")).toString().trimmed();
        return rejected(QStringLiteral("native_import_failed"),
            message.isEmpty() ? QStringLiteral("Fantareal 原生导入失败") : message);
    }

    publishedArtifacts_.insert(key);
    QVariantMap result {
        { QStringLiteral("ok"), true },
        { QStringLiteral("kind"), kind },
        { QStringLiteral("suggestedName"), suggestedName },
        { QStringLiteral("extensionId"), extensionId },
        { QStringLiteral("sessionId"), sessionId },
        { QStringLiteral("message"), nativeResult.value(QStringLiteral("message")) },
    };
    if (nativeResult.contains(QStringLiteral("entryCount"))) {
        result.insert(QStringLiteral("entryCount"), nativeResult.value(QStringLiteral("entryCount")));
    }
    if (nativeResult.contains(QStringLiteral("sourcePath"))) {
        result.insert(QStringLiteral("sourcePath"), nativeResult.value(QStringLiteral("sourcePath")));
    }
    return { true, result, {}, {} };
}

void ExtensionArtifactRouter::closeSession(const QString& extensionId, const QString& sessionId) {
    const QString prefix = sessionPrefix(extensionId, sessionId);
    for (auto it = publishedArtifacts_.begin(); it != publishedArtifacts_.end();) {
        if (it->startsWith(prefix)) {
            it = publishedArtifacts_.erase(it);
        } else {
            ++it;
        }
    }
}
