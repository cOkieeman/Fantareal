#include "extensionmanifest.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>
#include <QVersionNumber>

namespace {

constexpr qint64 kMaxManifestBytes = 1024 * 1024;

ExtensionManifestResult failure(const QString& code, const QString& message) {
    return { false, {}, code, message };
}

bool hasOnlyKeys(const QJsonObject& object, const QSet<QString>& allowed, QString* unexpectedKey) {
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!allowed.contains(it.key())) {
            if (unexpectedKey) {
                *unexpectedKey = it.key();
            }
            return false;
        }
    }
    return true;
}

bool requiredString(
    const QJsonObject& object,
    const QString& key,
    QString* value,
    QString* error,
    int maxLength = 0) {
    const QJsonValue raw = object.value(key);
    if (!raw.isString() || raw.toString().trimmed().isEmpty()) {
        if (error) {
            *error = QStringLiteral("%1 必须是非空字符串").arg(key);
        }
        return false;
    }
    const QString parsed = raw.toString().trimmed();
    if (maxLength > 0 && parsed.size() > maxLength) {
        if (error) {
            *error = QStringLiteral("%1 超过最大长度 %2").arg(key).arg(maxLength);
        }
        return false;
    }
    if (value) {
        *value = parsed;
    }
    return true;
}

bool optionalString(
    const QJsonObject& object,
    const QString& key,
    QString* value,
    QString* error,
    int maxLength = 0) {
    if (!object.contains(key)) {
        return true;
    }
    if (!object.value(key).isString()) {
        if (error) {
            *error = QStringLiteral("%1 必须是字符串").arg(key);
        }
        return false;
    }
    const QString parsed = object.value(key).toString().trimmed();
    if (maxLength > 0 && parsed.size() > maxLength) {
        if (error) {
            *error = QStringLiteral("%1 超过最大长度 %2").arg(key).arg(maxLength);
        }
        return false;
    }
    if (value) {
        *value = parsed;
    }
    return true;
}

QString canonicalOrAbsolute(const QString& path) {
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}

bool validateEntrypointFile(const QString& packageRoot, const QString& relativePath, QString* error) {
    QString normalized;
    if (!ExtensionManifestParser::isSafeRelativePath(relativePath, &normalized)) {
        if (error) {
            *error = QStringLiteral("entrypoint 路径不安全：%1").arg(relativePath);
        }
        return false;
    }

    const QString absolutePath = QDir(packageRoot).absoluteFilePath(normalized);
    const QFileInfo info(absolutePath);
    if (!info.exists() || !info.isFile() || info.isSymLink() || info.isJunction()) {
        if (error) {
            *error = QStringLiteral("entrypoint 文件不存在或不是普通文件：%1").arg(normalized);
        }
        return false;
    }
    if (!ExtensionManifestParser::pathIsWithin(packageRoot, info.absoluteFilePath())) {
        if (error) {
            *error = QStringLiteral("entrypoint 文件离开 package root：%1").arg(normalized);
        }
        return false;
    }
    return true;
}

bool hostApiRangeMatches(const QString& range, QString* error) {
    const QVersionNumber current(1, 0, 0);
    const QStringList constraints = range.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (constraints.isEmpty()) {
        if (error) {
            *error = QStringLiteral("hostApi 约束为空");
        }
        return false;
    }
    for (const QString& constraint : constraints) {
        static const QRegularExpression pattern(QStringLiteral("^(>=|<=|>|<|=)?([0-9]+(?:\\.[0-9]+){0,2})$"));
        const QRegularExpressionMatch match = pattern.match(constraint);
        if (!match.hasMatch()) {
            if (error) {
                *error = QStringLiteral("hostApi 约束格式无效：%1").arg(constraint);
            }
            return false;
        }
        const QString operation = match.captured(1);
        const QVersionNumber requested = QVersionNumber::fromString(match.captured(2));
        const int comparison = QVersionNumber::compare(current, requested);
        const bool matches = operation == QStringLiteral(">=") ? comparison >= 0
            : operation == QStringLiteral("<=")                ? comparison <= 0
            : operation == QStringLiteral(">")                 ? comparison > 0
            : operation == QStringLiteral("<")                 ? comparison < 0
                                                                  : comparison == 0;
        if (!matches) {
            if (error) {
                *error = QStringLiteral("插件要求 hostApi %1，当前为 1.0.0").arg(range);
            }
            return false;
        }
    }
    return true;
}

QString currentPlatformName() {
#ifdef Q_OS_WIN
    return QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#else
    return QStringLiteral("linux");
#endif
}

} // namespace

QVariantMap ExtensionManifest::toVariantMap() const {
    QVariantMap result;
    result.insert(QStringLiteral("schemaVersion"), schemaVersion);
    result.insert(QStringLiteral("id"), id);
    result.insert(QStringLiteral("name"), name);
    result.insert(QStringLiteral("description"), description);
    result.insert(QStringLiteral("version"), version);
    result.insert(QStringLiteral("publisher"), publisher);
    result.insert(QStringLiteral("hostApi"), hostApi);
    result.insert(QStringLiteral("python"), pythonRange);
    result.insert(QStringLiteral("platforms"), platforms);
    result.insert(QStringLiteral("permissions"), permissions);
    result.insert(QStringLiteral("hasPage"), hasPage);
    result.insert(QStringLiteral("pagePath"), pagePath);
    result.insert(QStringLiteral("pageBridge"), pageBridge);
    result.insert(QStringLiteral("hasService"), hasService);
    result.insert(QStringLiteral("serviceModule"), serviceModule);
    result.insert(QStringLiteral("serviceProtocol"), serviceProtocol);
    result.insert(QStringLiteral("serviceLockfile"), serviceLockfile);
    QVariantList artifactDeclarations;
    for (auto it = artifactMediaTypes.constBegin(); it != artifactMediaTypes.constEnd(); ++it) {
        artifactDeclarations.append(QVariantMap {
            { QStringLiteral("kind"), it.key() },
            { QStringLiteral("mediaTypes"), it.value() },
        });
    }
    result.insert(QStringLiteral("artifacts"), artifactDeclarations);
    return result;
}

ExtensionManifestResult ExtensionManifestParser::loadFromDirectory(const QString& packageRoot) {
    const QFileInfo rootInfo(packageRoot);
    if (!rootInfo.exists() || !rootInfo.isDir() || rootInfo.isSymLink() || rootInfo.isJunction()) {
        return failure(QStringLiteral("package_root_invalid"), QStringLiteral("插件 package root 不存在或不是普通目录"));
    }

    QFile manifestFile(QDir(packageRoot).absoluteFilePath(QStringLiteral("fantareal-extension.json")));
    if (!manifestFile.open(QIODevice::ReadOnly)) {
        return failure(QStringLiteral("manifest_missing"), QStringLiteral("缺少 fantareal-extension.json"));
    }
    if (manifestFile.size() <= 0 || manifestFile.size() > kMaxManifestBytes) {
        return failure(QStringLiteral("manifest_size_invalid"), QStringLiteral("manifest 为空或超过 1 MiB"));
    }
    return parse(manifestFile.readAll(), packageRoot);
}

ExtensionManifestResult ExtensionManifestParser::parse(const QByteArray& payload, const QString& packageRoot) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return failure(
            QStringLiteral("manifest_json_invalid"),
            QStringLiteral("manifest JSON 无效：%1").arg(parseError.errorString()));
    }

    const QJsonObject root = document.object();
    QString unexpectedKey;
    if (!hasOnlyKeys(root,
            { QStringLiteral("$schema"), QStringLiteral("schemaVersion"), QStringLiteral("id"),
                QStringLiteral("name"), QStringLiteral("description"), QStringLiteral("version"),
                QStringLiteral("publisher"), QStringLiteral("compatibility"), QStringLiteral("entrypoints"),
                QStringLiteral("contributes"), QStringLiteral("permissions"), QStringLiteral("limits") },
            &unexpectedKey)) {
        return failure(
            QStringLiteral("manifest_unknown_property"),
            QStringLiteral("manifest 包含未知字段：%1").arg(unexpectedKey));
    }

    if (!root.value(QStringLiteral("schemaVersion")).isDouble()
        || root.value(QStringLiteral("schemaVersion")).toInt() != 1) {
        return failure(QStringLiteral("schema_version_unsupported"), QStringLiteral("仅支持 schemaVersion 1"));
    }

    ExtensionManifest manifest;
    manifest.schemaVersion = 1;
    manifest.raw = root;
    QString error;
    if (!requiredString(root, QStringLiteral("id"), &manifest.id, &error, 160)
        || !requiredString(root, QStringLiteral("name"), &manifest.name, &error, 80)
        || !requiredString(root, QStringLiteral("version"), &manifest.version, &error, 80)
        || !optionalString(root, QStringLiteral("description"), &manifest.description, &error, 500)
        || !optionalString(root, QStringLiteral("publisher"), &manifest.publisher, &error, 80)) {
        return failure(QStringLiteral("manifest_field_invalid"), error);
    }

    static const QRegularExpression idPattern(QStringLiteral("^[a-z0-9]+(?:[.-][a-z0-9]+)+$"));
    static const QRegularExpression versionPattern(
        QStringLiteral("^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)(?:-[0-9A-Za-z.-]+)?$"));
    if (!idPattern.match(manifest.id).hasMatch()) {
        return failure(QStringLiteral("extension_id_invalid"), QStringLiteral("extension id 格式无效"));
    }
    if (!versionPattern.match(manifest.version).hasMatch()) {
        return failure(QStringLiteral("extension_version_invalid"), QStringLiteral("extension version 必须是 SemVer"));
    }

    if (!root.value(QStringLiteral("compatibility")).isObject()) {
        return failure(QStringLiteral("compatibility_invalid"), QStringLiteral("compatibility 必须是 object"));
    }
    const QJsonObject compatibility = root.value(QStringLiteral("compatibility")).toObject();
    if (!hasOnlyKeys(compatibility,
            { QStringLiteral("hostApi"), QStringLiteral("python"), QStringLiteral("platforms") },
            &unexpectedKey)) {
        return failure(
            QStringLiteral("compatibility_unknown_property"),
            QStringLiteral("compatibility 包含未知字段：%1").arg(unexpectedKey));
    }
    if (!requiredString(compatibility, QStringLiteral("hostApi"), &manifest.hostApi, &error, 120)
        || !optionalString(compatibility, QStringLiteral("python"), &manifest.pythonRange, &error, 120)) {
        return failure(QStringLiteral("compatibility_invalid"), error);
    }
    if (!hostApiRangeMatches(manifest.hostApi, &error)) {
        return failure(QStringLiteral("host_api_incompatible"), error);
    }
    if (compatibility.contains(QStringLiteral("platforms"))) {
        const QJsonValue platforms = compatibility.value(QStringLiteral("platforms"));
        if (!platforms.isArray() || platforms.toArray().isEmpty()) {
            return failure(QStringLiteral("platforms_invalid"), QStringLiteral("platforms 必须是非空 array"));
        }
        QSet<QString> seenPlatforms;
        for (const QJsonValue& value : platforms.toArray()) {
            const QString platform = value.toString();
            if (!value.isString()
                || !QSet<QString>{ QStringLiteral("windows"), QStringLiteral("linux"), QStringLiteral("macos") }.contains(platform)
                || seenPlatforms.contains(platform)) {
                return failure(QStringLiteral("platforms_invalid"), QStringLiteral("platforms 包含无效或重复值"));
            }
            seenPlatforms.insert(platform);
            manifest.platforms.append(platform);
        }
        if (!manifest.platforms.contains(currentPlatformName())) {
            return failure(
                QStringLiteral("platform_incompatible"),
                QStringLiteral("插件不支持当前平台：%1").arg(currentPlatformName()));
        }
    }

    if (!root.value(QStringLiteral("entrypoints")).isObject()) {
        return failure(QStringLiteral("entrypoints_invalid"), QStringLiteral("entrypoints 必须是 object"));
    }
    const QJsonObject entrypoints = root.value(QStringLiteral("entrypoints")).toObject();
    if (!hasOnlyKeys(entrypoints, { QStringLiteral("page"), QStringLiteral("service") }, &unexpectedKey)) {
        return failure(
            QStringLiteral("entrypoints_unknown_property"),
            QStringLiteral("entrypoints 包含未知字段：%1").arg(unexpectedKey));
    }
    manifest.hasPage = entrypoints.value(QStringLiteral("page")).isObject();
    manifest.hasService = entrypoints.value(QStringLiteral("service")).isObject();
    if (!manifest.hasPage && !manifest.hasService) {
        return failure(QStringLiteral("entrypoint_missing"), QStringLiteral("page 和 service 至少需要声明一个"));
    }

    if (manifest.hasPage) {
        const QJsonObject page = entrypoints.value(QStringLiteral("page")).toObject();
        if (!hasOnlyKeys(page,
                { QStringLiteral("type"), QStringLiteral("path"), QStringLiteral("bridge") },
                &unexpectedKey)
            || !requiredString(page, QStringLiteral("path"), &manifest.pagePath, &error, 500)
            || !requiredString(page, QStringLiteral("bridge"), &manifest.pageBridge, &error, 120)) {
            return failure(
                QStringLiteral("page_entrypoint_invalid"),
                error.isEmpty() ? QStringLiteral("page 包含未知字段：%1").arg(unexpectedKey) : error);
        }
        if (page.value(QStringLiteral("type")).toString() != QStringLiteral("web")
            || manifest.pageBridge != QStringLiteral("fantareal.extension.v1")) {
            return failure(QStringLiteral("page_entrypoint_invalid"), QStringLiteral("page type 或 bridge 不受支持"));
        }
        if (!validateEntrypointFile(packageRoot, manifest.pagePath, &error)) {
            return failure(QStringLiteral("page_entrypoint_invalid"), error);
        }
    }

    if (manifest.hasService) {
        const QJsonObject service = entrypoints.value(QStringLiteral("service")).toObject();
        if (!hasOnlyKeys(service,
                { QStringLiteral("type"), QStringLiteral("module"), QStringLiteral("protocol"), QStringLiteral("lockfile") },
                &unexpectedKey)
            || !requiredString(service, QStringLiteral("module"), &manifest.serviceModule, &error, 240)
            || !requiredString(service, QStringLiteral("protocol"), &manifest.serviceProtocol, &error, 120)
            || !requiredString(service, QStringLiteral("lockfile"), &manifest.serviceLockfile, &error, 500)) {
            return failure(
                QStringLiteral("service_entrypoint_invalid"),
                error.isEmpty() ? QStringLiteral("service 包含未知字段：%1").arg(unexpectedKey) : error);
        }
        static const QRegularExpression modulePattern(QStringLiteral("^[A-Za-z_][A-Za-z0-9_.]+$"));
        if (service.value(QStringLiteral("type")).toString() != QStringLiteral("python")
            || manifest.serviceProtocol != QStringLiteral("jsonrpc-2.0-stdio")
            || manifest.serviceLockfile != QStringLiteral("uv.lock")
            || !modulePattern.match(manifest.serviceModule).hasMatch()) {
            return failure(QStringLiteral("service_entrypoint_invalid"), QStringLiteral("service 声明不受支持"));
        }
        if (!validateEntrypointFile(packageRoot, manifest.serviceLockfile, &error)) {
            return failure(QStringLiteral("service_entrypoint_invalid"), error);
        }
        const QString modulePath = manifest.serviceModule;
        QString moduleRelative = modulePath;
        moduleRelative.replace(QLatin1Char('.'), QLatin1Char('/'));
        moduleRelative += QStringLiteral(".py");
        const QString sourceModule = QStringLiteral("src/") + moduleRelative;
        if (!QFileInfo::exists(QDir(packageRoot).absoluteFilePath(sourceModule))
            && !QFileInfo::exists(QDir(packageRoot).absoluteFilePath(moduleRelative))) {
            return failure(
                QStringLiteral("service_module_missing"),
                QStringLiteral("找不到 service module：%1").arg(manifest.serviceModule));
        }
    }

    const QJsonValue contributesValue = root.value(QStringLiteral("contributes"));
    if (!contributesValue.isUndefined()) {
        if (!contributesValue.isObject()) {
            return failure(QStringLiteral("contributes_invalid"), QStringLiteral("contributes 必须是 object"));
        }
        const QJsonObject contributes = contributesValue.toObject();
        if (!hasOnlyKeys(contributes,
                { QStringLiteral("pages"), QStringLiteral("commands"), QStringLiteral("artifacts") },
                &unexpectedKey)) {
            return failure(
                QStringLiteral("contributes_unknown_property"),
                QStringLiteral("contributes 包含未知字段：%1").arg(unexpectedKey));
        }
        const QJsonValue artifactsValue = contributes.value(QStringLiteral("artifacts"));
        if (!artifactsValue.isUndefined()) {
            if (!artifactsValue.isArray() || artifactsValue.toArray().isEmpty()) {
                return failure(QStringLiteral("artifact_declarations_invalid"),
                    QStringLiteral("contributes.artifacts 必须是非空 array"));
            }
            static const QRegularExpression kindPattern(
                QStringLiteral("^[a-z0-9]+(?:[.-][a-z0-9]+)+$"));
            static const QRegularExpression mediaTypePattern(
                QStringLiteral("^[a-z0-9][a-z0-9!#$&^_.+-]*/[a-z0-9][a-z0-9!#$&^_.+-]*$"));
            for (const QJsonValue& declarationValue : artifactsValue.toArray()) {
                if (!declarationValue.isObject()) {
                    return failure(QStringLiteral("artifact_declaration_invalid"),
                        QStringLiteral("artifact declaration 必须是 object"));
                }
                const QJsonObject declaration = declarationValue.toObject();
                if (!hasOnlyKeys(declaration,
                        { QStringLiteral("kind"), QStringLiteral("mediaTypes") }, &unexpectedKey)) {
                    return failure(QStringLiteral("artifact_declaration_unknown_property"),
                        QStringLiteral("artifact declaration 包含未知字段：%1").arg(unexpectedKey));
                }
                QString kind;
                if (!requiredString(declaration, QStringLiteral("kind"), &kind, &error, 160)
                    || !kindPattern.match(kind).hasMatch()) {
                    return failure(QStringLiteral("artifact_kind_invalid"),
                        error.isEmpty() ? QStringLiteral("artifact kind 格式无效") : error);
                }
                if (manifest.artifactMediaTypes.contains(kind)) {
                    return failure(QStringLiteral("artifact_kind_duplicate"),
                        QStringLiteral("artifact kind 重复声明：%1").arg(kind));
                }
                const QJsonValue mediaTypesValue = declaration.value(QStringLiteral("mediaTypes"));
                if (!mediaTypesValue.isArray() || mediaTypesValue.toArray().isEmpty()) {
                    return failure(QStringLiteral("artifact_media_types_invalid"),
                        QStringLiteral("artifact mediaTypes 必须是非空 array"));
                }
                QStringList mediaTypes;
                for (const QJsonValue& mediaTypeValue : mediaTypesValue.toArray()) {
                    const QString mediaType = mediaTypeValue.toString().trimmed().toLower();
                    if (!mediaTypeValue.isString() || !mediaTypePattern.match(mediaType).hasMatch()
                        || mediaTypes.contains(mediaType)) {
                        return failure(QStringLiteral("artifact_media_types_invalid"),
                            QStringLiteral("artifact mediaTypes 包含无效或重复值"));
                    }
                    mediaTypes.append(mediaType);
                }
                manifest.artifactMediaTypes.insert(kind, mediaTypes);
            }
        }
    }

    if (!root.value(QStringLiteral("permissions")).isArray()) {
        return failure(QStringLiteral("permissions_invalid"), QStringLiteral("permissions 必须是 array"));
    }
    const QSet<QString> allowedPermissions = {
        QStringLiteral("files.user-selected.read"),
        QStringLiteral("storage.workspace"),
        QStringLiteral("artifacts.publish"),
    };
    QSet<QString> seenPermissions;
    for (const QJsonValue& value : root.value(QStringLiteral("permissions")).toArray()) {
        const QString permission = value.toString();
        if (!value.isString() || !allowedPermissions.contains(permission) || seenPermissions.contains(permission)) {
            return failure(QStringLiteral("permissions_invalid"), QStringLiteral("permissions 包含无效或重复值"));
        }
        seenPermissions.insert(permission);
        manifest.permissions.append(permission);
    }

    return { true, manifest, {}, {} };
}

bool ExtensionManifestParser::isSafeRelativePath(const QString& path, QString* normalizedPath) {
    const QString forward = QDir::fromNativeSeparators(path.trimmed());
    if (forward.isEmpty() || QDir::isAbsolutePath(forward) || forward.contains(QLatin1Char(':'))
        || forward.contains(QChar::Null)) {
        return false;
    }
    const QString cleaned = QDir::cleanPath(forward);
    if (cleaned.isEmpty() || cleaned == QStringLiteral(".") || cleaned == QStringLiteral("..")
        || cleaned.startsWith(QStringLiteral("../")) || cleaned.startsWith(QLatin1Char('/'))) {
        return false;
    }
    if (normalizedPath) {
        *normalizedPath = cleaned;
    }
    return true;
}

bool ExtensionManifestParser::pathIsWithin(const QString& rootPath, const QString& candidatePath) {
    const QString root = canonicalOrAbsolute(rootPath);
    const QString candidate = canonicalOrAbsolute(candidatePath);
    if (root.isEmpty() || candidate.isEmpty()) {
        return false;
    }
#ifdef Q_OS_WIN
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity sensitivity = Qt::CaseSensitive;
#endif
    return candidate.compare(root, sensitivity) == 0
        || candidate.startsWith(root + QLatin1Char('/'), sensitivity);
}
