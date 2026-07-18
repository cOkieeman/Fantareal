#include "extensionregistry.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>
#include <QRegularExpression>

#include <algorithm>

namespace {

QString normalizedRelativePackagePath(const QString& path) {
    QString normalized;
    if (!ExtensionManifestParser::isSafeRelativePath(path, &normalized)
        || !normalized.startsWith(QStringLiteral("packages/"))) {
        return {};
    }
    return normalized;
}

QJsonArray stringListToJson(const QStringList& values) {
    QJsonArray result;
    for (const QString& value : values) {
        result.append(value);
    }
    return result;
}

QString timestampSuffix() {
    return QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddTHHmmsszzzZ"));
}

} // namespace

ExtensionRegistry::ExtensionRegistry(QString extensionRoot)
    : extensionRoot_(QDir::cleanPath(QFileInfo(extensionRoot).absoluteFilePath()))
    , registryPath_(QDir(extensionRoot_).absoluteFilePath(QStringLiteral("registry.json"))) {
}

bool ExtensionRegistry::load(QString* error) {
    lastRecoveryPath_.clear();
    if (!ensureLayout(error)) {
        return false;
    }
    if (!QFileInfo::exists(registryPath_)) {
        entries_ = {};
        return true;
    }

    QFile file(registryPath_);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("无法读取 extension registry：%1").arg(file.errorString());
        }
        return false;
    }

    const QByteArray payload = file.readAll();
    file.close();

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return recoverCorruptRegistry(
            QStringLiteral("registry JSON 无效：%1").arg(parseError.errorString()), error);
    }

    QString validationError;
    if (!validateRegistryDocument(document.object(), &validationError)) {
        return recoverCorruptRegistry(validationError, error);
    }

    entries_ = document.object().value(QStringLiteral("extensions")).toArray();
    return true;
}

bool ExtensionRegistry::save(QString* error) const {
    if (!ensureLayout(error)) {
        return false;
    }
    QJsonObject root;
    root.insert(QStringLiteral("schemaVersion"), 1);
    root.insert(QStringLiteral("extensions"), entries_);

    QSaveFile file(registryPath_);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = QStringLiteral("无法写入 extension registry：%1").arg(file.errorString());
        }
        return false;
    }
    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(payload) != payload.size()) {
        if (error) {
            *error = QStringLiteral("extension registry 写入不完整：%1").arg(file.errorString());
        }
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        if (error) {
            *error = QStringLiteral("extension registry 原子提交失败：%1").arg(file.errorString());
        }
        return false;
    }
    return true;
}

QVariantList ExtensionRegistry::extensions() const {
    QVariantList result;
    result.reserve(entries_.size());
    for (const QJsonValue& value : entries_) {
        result.append(toDisplayMap(value.toObject()));
    }
    std::sort(result.begin(), result.end(), [](const QVariant& left, const QVariant& right) {
        return left.toMap().value(QStringLiteral("name")).toString().localeAwareCompare(
                   right.toMap().value(QStringLiteral("name")).toString())
            < 0;
    });
    return result;
}

QStringList ExtensionRegistry::packagePaths(const QString& extensionId) const {
    const int index = indexOf(extensionId);
    if (index < 0) {
        return {};
    }
    QStringList paths;
    const QJsonArray packages = entries_.at(index).toObject().value(QStringLiteral("packages")).toArray();
    for (const QJsonValue& value : packages) {
        const QString path = value.toObject().value(QStringLiteral("path")).toString();
        if (!path.isEmpty()) {
            paths.append(path);
        }
    }
    return paths;
}

QString ExtensionRegistry::lastRecoveryPath() const {
    return lastRecoveryPath_;
}

QString ExtensionRegistry::rootPath() const {
    return extensionRoot_;
}

QString ExtensionRegistry::registryPath() const {
    return registryPath_;
}

bool ExtensionRegistry::upsertPackage(
    const ExtensionManifest& manifest,
    const QString& packagePath,
    const QString& digest,
    const QString& sourceType,
    const QString& sourceUrl,
    const QString& resolvedCommit,
    QStringList* retiredPackagePaths,
    QString* error) {
    const QString normalizedPath = normalizedRelativePackagePath(packagePath);
    if (normalizedPath.isEmpty() || digest.trimmed().isEmpty() || sourceType.trimmed().isEmpty()) {
        if (error) {
            *error = QStringLiteral("registry package descriptor 无效");
        }
        return false;
    }

    QJsonObject package;
    package.insert(QStringLiteral("version"), manifest.version);
    package.insert(QStringLiteral("path"), normalizedPath);
    package.insert(QStringLiteral("digest"), digest);
    package.insert(QStringLiteral("sourceType"), sourceType);
    package.insert(QStringLiteral("sourceUrl"), sourceUrl);
    package.insert(QStringLiteral("resolvedCommit"), resolvedCommit);
    package.insert(QStringLiteral("installedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    package.insert(QStringLiteral("hasPage"), manifest.hasPage);
    package.insert(QStringLiteral("hasService"), manifest.hasService);

    const int existingIndex = indexOf(manifest.id);
    QJsonObject entry;
    QJsonArray packages;
    bool enabled = true;
    if (existingIndex >= 0) {
        entry = entries_.at(existingIndex).toObject();
        packages = entry.value(QStringLiteral("packages")).toArray();
        enabled = entry.value(QStringLiteral("enabled")).toBool(true);
    }

    bool packageExists = false;
    for (int i = 0; i < packages.size(); ++i) {
        if (packages.at(i).toObject().value(QStringLiteral("path")).toString() == normalizedPath) {
            packages.replace(i, package);
            packageExists = true;
            break;
        }
    }
    if (!packageExists) {
        packages.append(package);
    }

    entry.insert(QStringLiteral("id"), manifest.id);
    entry.insert(QStringLiteral("name"), manifest.name);
    entry.insert(QStringLiteral("description"), manifest.description);
    entry.insert(QStringLiteral("publisher"), manifest.publisher);
    entry.insert(QStringLiteral("hostApi"), manifest.hostApi);
    entry.insert(QStringLiteral("python"), manifest.pythonRange);
    entry.insert(QStringLiteral("permissions"), stringListToJson(manifest.permissions));
    entry.insert(QStringLiteral("enabled"), enabled);
    const QString previousActivePackage = entry.value(QStringLiteral("activePackage")).toString();
    if (!previousActivePackage.isEmpty() && previousActivePackage != normalizedPath) {
        entry.insert(QStringLiteral("rollbackPackage"), previousActivePackage);
    }
    const QString rollbackPackage = entry.value(QStringLiteral("rollbackPackage")).toString();
    QStringList retiredPaths;
    while (packages.size() > 3) {
        int removeIndex = -1;
        for (int i = 0; i < packages.size(); ++i) {
            const QString candidatePath = packages.at(i).toObject().value(QStringLiteral("path")).toString();
            if (candidatePath != normalizedPath && candidatePath != rollbackPackage) {
                removeIndex = i;
                break;
            }
        }
        if (removeIndex < 0) {
            break;
        }
        retiredPaths.append(packages.at(removeIndex).toObject().value(QStringLiteral("path")).toString());
        packages.removeAt(removeIndex);
    }
    entry.insert(QStringLiteral("activePackage"), normalizedPath);
    entry.insert(QStringLiteral("packages"), packages);

    const QJsonArray previous = entries_;
    if (existingIndex >= 0) {
        entries_.replace(existingIndex, entry);
    } else {
        entries_.append(entry);
    }
    if (!save(error)) {
        entries_ = previous;
        return false;
    }
    if (retiredPackagePaths) {
        *retiredPackagePaths = retiredPaths;
    }
    return true;
}

bool ExtensionRegistry::setEnabled(const QString& extensionId, bool enabled, QString* error) {
    const int index = indexOf(extensionId);
    if (index < 0) {
        if (error) {
            *error = QStringLiteral("找不到 extension：%1").arg(extensionId);
        }
        return false;
    }
    const QJsonArray previous = entries_;
    QJsonObject entry = entries_.at(index).toObject();
    entry.insert(QStringLiteral("enabled"), enabled);
    entries_.replace(index, entry);
    if (!save(error)) {
        entries_ = previous;
        return false;
    }
    return true;
}

bool ExtensionRegistry::activateRollbackPackage(
    const QString& extensionId,
    QString* activatedPath,
    QString* error) {
    const int index = indexOf(extensionId);
    if (index < 0) {
        if (error) {
            *error = QStringLiteral("找不到 extension：%1").arg(extensionId);
        }
        return false;
    }

    const QJsonArray previous = entries_;
    QJsonObject entry = entries_.at(index).toObject();
    const QString activePackage = entry.value(QStringLiteral("activePackage")).toString();
    const QString rollbackPackage = entry.value(QStringLiteral("rollbackPackage")).toString();
    if (rollbackPackage.isEmpty() || rollbackPackage == activePackage) {
        if (error) {
            *error = QStringLiteral("插件没有可回滚的 package");
        }
        return false;
    }

    bool rollbackExists = false;
    for (const QJsonValue& value : entry.value(QStringLiteral("packages")).toArray()) {
        rollbackExists = rollbackExists
            || value.toObject().value(QStringLiteral("path")).toString() == rollbackPackage;
    }
    if (!rollbackExists) {
        if (error) {
            *error = QStringLiteral("回滚 package 不存在");
        }
        return false;
    }

    entry.insert(QStringLiteral("activePackage"), rollbackPackage);
    entry.insert(QStringLiteral("rollbackPackage"), activePackage);
    entries_.replace(index, entry);
    if (!save(error)) {
        entries_ = previous;
        return false;
    }
    if (activatedPath) {
        *activatedPath = rollbackPackage;
    }
    return true;
}

bool ExtensionRegistry::removeExtension(const QString& extensionId, QString* error) {
    const int index = indexOf(extensionId);
    if (index < 0) {
        if (error) {
            *error = QStringLiteral("找不到 extension：%1").arg(extensionId);
        }
        return false;
    }
    const QJsonArray previous = entries_;
    entries_.removeAt(index);
    if (!save(error)) {
        entries_ = previous;
        return false;
    }
    return true;
}

bool ExtensionRegistry::ensureLayout(QString* error) const {
    QDir root(extensionRoot_);
    if (!root.exists() && !QDir().mkpath(extensionRoot_)) {
        if (error) {
            *error = QStringLiteral("无法创建 extension 根目录：%1").arg(extensionRoot_);
        }
        return false;
    }
    for (const QString& relative : { QStringLiteral("staging"), QStringLiteral("packages"),
             QStringLiteral("runtimes"), QStringLiteral("workspaces"), QStringLiteral("logs") }) {
        if (!root.mkpath(relative)) {
            if (error) {
                *error = QStringLiteral("无法创建 extension 目录：%1").arg(relative);
            }
            return false;
        }
    }
    return true;
}

bool ExtensionRegistry::recoverCorruptRegistry(const QString& reason, QString* error) {
    const QString recoveryPath = QDir(extensionRoot_).absoluteFilePath(
        QStringLiteral("registry.corrupt-%1.json").arg(timestampSuffix()));
    if (!QFile::rename(registryPath_, recoveryPath)) {
        if (error) {
            *error = QStringLiteral("%1；且无法隔离损坏 registry").arg(reason);
        }
        return false;
    }
    lastRecoveryPath_ = recoveryPath;
    entries_ = {};
    QString saveError;
    if (!save(&saveError)) {
        QFile::rename(recoveryPath, registryPath_);
        lastRecoveryPath_.clear();
        if (error) {
            *error = QStringLiteral("%1；重建 registry 失败：%2").arg(reason, saveError);
        }
        return false;
    }
    if (error) {
        *error = QStringLiteral("%1；已隔离到 %2 并重建空 registry").arg(reason, recoveryPath);
    }
    return true;
}

int ExtensionRegistry::indexOf(const QString& extensionId) const {
    for (int i = 0; i < entries_.size(); ++i) {
        if (entries_.at(i).toObject().value(QStringLiteral("id")).toString() == extensionId) {
            return i;
        }
    }
    return -1;
}

QVariantMap ExtensionRegistry::toDisplayMap(const QJsonObject& entry) {
    QVariantMap result;
    result.insert(QStringLiteral("id"), entry.value(QStringLiteral("id")).toString());
    result.insert(QStringLiteral("name"), entry.value(QStringLiteral("name")).toString());
    result.insert(QStringLiteral("description"), entry.value(QStringLiteral("description")).toString());
    result.insert(QStringLiteral("publisher"), entry.value(QStringLiteral("publisher")).toString());
    result.insert(QStringLiteral("hostApi"), entry.value(QStringLiteral("hostApi")).toString());
    result.insert(QStringLiteral("python"), entry.value(QStringLiteral("python")).toString());
    result.insert(QStringLiteral("enabled"), entry.value(QStringLiteral("enabled")).toBool());
    result.insert(QStringLiteral("permissions"), entry.value(QStringLiteral("permissions")).toArray().toVariantList());

    const QString activePath = entry.value(QStringLiteral("activePackage")).toString();
    const QJsonArray packages = entry.value(QStringLiteral("packages")).toArray();
    for (const QJsonValue& value : packages) {
        const QJsonObject package = value.toObject();
        if (package.value(QStringLiteral("path")).toString() != activePath) {
            continue;
        }
        result.insert(QStringLiteral("version"), package.value(QStringLiteral("version")).toString());
        result.insert(QStringLiteral("packagePath"), activePath);
        result.insert(QStringLiteral("digest"), package.value(QStringLiteral("digest")).toString());
        result.insert(QStringLiteral("sourceType"), package.value(QStringLiteral("sourceType")).toString());
        result.insert(QStringLiteral("sourceUrl"), package.value(QStringLiteral("sourceUrl")).toString());
        result.insert(QStringLiteral("resolvedCommit"), package.value(QStringLiteral("resolvedCommit")).toString());
        result.insert(QStringLiteral("installedAt"), package.value(QStringLiteral("installedAt")).toString());
        result.insert(QStringLiteral("hasPage"), package.value(QStringLiteral("hasPage")).toBool());
        result.insert(QStringLiteral("hasService"), package.value(QStringLiteral("hasService")).toBool());
        break;
    }
    result.insert(QStringLiteral("packageCount"), packages.size());
    const QString rollbackPath = entry.value(QStringLiteral("rollbackPackage")).toString();
    result.insert(QStringLiteral("canRollback"), !rollbackPath.isEmpty() && rollbackPath != activePath);
    return result;
}

bool ExtensionRegistry::validateRegistryDocument(const QJsonObject& root, QString* error) {
    if (root.value(QStringLiteral("schemaVersion")).toInt(-1) != 1
        || !root.value(QStringLiteral("extensions")).isArray()) {
        if (error) {
            *error = QStringLiteral("registry schema 无效");
        }
        return false;
    }
    QSet<QString> ids;
    for (const QJsonValue& value : root.value(QStringLiteral("extensions")).toArray()) {
        if (!value.isObject()) {
            if (error) {
                *error = QStringLiteral("registry extension entry 必须是 object");
            }
            return false;
        }
        const QJsonObject entry = value.toObject();
        const QString id = entry.value(QStringLiteral("id")).toString();
        const QString activePackage = entry.value(QStringLiteral("activePackage")).toString();
        const QString rollbackPackage = entry.value(QStringLiteral("rollbackPackage")).toString();
        if (id.isEmpty() || ids.contains(id) || normalizedRelativePackagePath(activePackage).isEmpty()
            || !entry.value(QStringLiteral("packages")).isArray()) {
            if (error) {
                *error = QStringLiteral("registry extension entry 无效");
            }
            return false;
        }
        ids.insert(id);
        bool activeFound = false;
        bool rollbackFound = rollbackPackage.isEmpty();
        for (const QJsonValue& packageValue : entry.value(QStringLiteral("packages")).toArray()) {
            if (!packageValue.isObject()) {
                if (error) {
                    *error = QStringLiteral("registry package entry 必须是 object");
                }
                return false;
            }
            const QString path = packageValue.toObject().value(QStringLiteral("path")).toString();
            if (normalizedRelativePackagePath(path).isEmpty()) {
                if (error) {
                    *error = QStringLiteral("registry package path 不安全");
                }
                return false;
            }
            activeFound = activeFound || path == activePackage;
            rollbackFound = rollbackFound || path == rollbackPackage;

            const QJsonObject package = packageValue.toObject();
            const QString sourceType = package.value(QStringLiteral("sourceType")).toString();
            const QString resolvedCommit = package.value(QStringLiteral("resolvedCommit")).toString();
            static const QRegularExpression shaPattern(QStringLiteral("^[0-9a-f]{40}$"));
            if (sourceType == QStringLiteral("github") && !shaPattern.match(resolvedCommit).hasMatch()) {
                if (error) {
                    *error = QStringLiteral("registry GitHub package commit 无效");
                }
                return false;
            }
        }
        if (!activeFound || !rollbackFound) {
            if (error) {
                *error = QStringLiteral("registry active 或 rollback package 不存在");
            }
            return false;
        }
    }
    return true;
}
