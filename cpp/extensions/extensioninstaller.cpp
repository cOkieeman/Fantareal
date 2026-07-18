#include "extensioninstaller.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileInfoList>
#include <QSaveFile>
#include <QSet>
#include <QRegularExpression>
#include <QUuid>

#include <algorithm>
#include <functional>

namespace {

constexpr int kMaxPackageFiles = 4000;
constexpr qint64 kMaxPackageBytes = 256LL * 1024 * 1024;
constexpr qint64 kMaxSingleFileBytes = 64LL * 1024 * 1024;

QString packageRelativePath(
    const ExtensionManifest& manifest,
    const QString& digest,
    const QString& resolvedCommit) {
    const QString sourceVersion = resolvedCommit.isEmpty()
        ? QStringLiteral("local")
        : resolvedCommit.left(12);
    return QStringLiteral("packages/%1/%2-%3-%4")
        .arg(manifest.id, manifest.version, sourceVersion, digest.left(12));
}

bool ensureDirectory(const QString& path, QString* error) {
    if (QDir().mkpath(path)) {
        return true;
    }
    if (error) {
        *error = QStringLiteral("无法创建目录：%1").arg(path);
    }
    return false;
}

} // namespace

ExtensionInstaller::ExtensionInstaller(ExtensionRegistry& registry)
    : registry_(registry) {
}

QVariantMap ExtensionInstaller::installFromDirectory(const QString& sourceDirectory) {
    const QFileInfo sourceInfo(sourceDirectory);
    return installPreparedDirectory(
        sourceDirectory,
        QStringLiteral("local-directory"),
        QDir::toNativeSeparators(sourceInfo.absoluteFilePath()),
        {});
}

QVariantMap ExtensionInstaller::installFromGitHubDirectory(
    const QString& sourceDirectory,
    const QString& sourceUrl,
    const QString& resolvedCommit) {
    static const QRegularExpression shaPattern(QStringLiteral("^[0-9a-f]{40}$"));
    if (!shaPattern.match(resolvedCommit).hasMatch()) {
        return result(false, QStringLiteral("GitHub resolved commit 必须是 40 位小写 SHA"),
            QStringLiteral("github_commit_invalid"));
    }
    return installPreparedDirectory(
        sourceDirectory,
        QStringLiteral("github"),
        sourceUrl,
        resolvedCommit);
}

QVariantMap ExtensionInstaller::installPreparedDirectory(
    const QString& sourceDirectory,
    const QString& sourceType,
    const QString& sourceUrl,
    const QString& resolvedCommit) {
    const QFileInfo sourceInfo(sourceDirectory);
    if (!sourceInfo.exists() || !sourceInfo.isDir() || sourceInfo.isSymLink() || sourceInfo.isJunction()) {
        return result(false, QStringLiteral("本地插件目录不存在或不是普通目录"), QStringLiteral("source_invalid"));
    }

    QString error;
    if (!registry_.load(&error)) {
        return result(false, error, QStringLiteral("registry_load_failed"));
    }

    const QString transactionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString transactionRoot = QDir(registry_.rootPath()).absoluteFilePath(
        QStringLiteral("staging/install-%1").arg(transactionId));
    const QString stagedPackage = QDir(transactionRoot).absoluteFilePath(QStringLiteral("package"));
    if (!ensureDirectory(stagedPackage, &error)) {
        return result(false, error, QStringLiteral("staging_create_failed"));
    }

    CopyState copyState;
    if (!copyDirectorySecure(
            sourceInfo.canonicalFilePath(), sourceInfo.canonicalFilePath(), stagedPackage, &copyState, &error)) {
        removePathWithinRoot(transactionRoot, nullptr);
        return result(false, error, QStringLiteral("package_copy_failed"));
    }

    const ExtensionManifestResult manifestResult = ExtensionManifestParser::loadFromDirectory(stagedPackage);
    if (!manifestResult.ok) {
        removePathWithinRoot(transactionRoot, nullptr);
        return result(false, manifestResult.message, manifestResult.code);
    }

    QString digest;
    if (!computePackageDigest(stagedPackage, &digest, &error)) {
        removePathWithinRoot(transactionRoot, nullptr);
        return result(false, error, QStringLiteral("package_digest_failed"));
    }

    const QString relativePackagePath = packageRelativePath(manifestResult.manifest, digest, resolvedCommit);
    const QString destinationPath = QDir(registry_.rootPath()).absoluteFilePath(relativePackagePath);
    const bool destinationExisted = QFileInfo::exists(destinationPath);
    if (!destinationExisted) {
        if (!ensureDirectory(QFileInfo(destinationPath).absolutePath(), &error)) {
            removePathWithinRoot(transactionRoot, nullptr);
            return result(false, error, QStringLiteral("package_parent_failed"));
        }
        if (!QDir().rename(stagedPackage, destinationPath)) {
            removePathWithinRoot(transactionRoot, nullptr);
            return result(false, QStringLiteral("无法原子安装插件 package"), QStringLiteral("package_activate_failed"));
        }
    }
    removePathWithinRoot(transactionRoot, nullptr);

    QStringList retiredPackagePaths;
    if (!registry_.upsertPackage(
            manifestResult.manifest,
            relativePackagePath,
            digest,
            sourceType,
            sourceUrl,
            resolvedCommit,
            &retiredPackagePaths,
            &error)) {
        if (!destinationExisted) {
            removePathWithinRoot(destinationPath, nullptr);
        }
        return result(false, error, QStringLiteral("registry_update_failed"));
    }

    QVariantMap response = result(true, QStringLiteral("插件已安装"));
    response.insert(QStringLiteral("id"), manifestResult.manifest.id);
    response.insert(QStringLiteral("name"), manifestResult.manifest.name);
    response.insert(QStringLiteral("version"), manifestResult.manifest.version);
    response.insert(QStringLiteral("digest"), digest);
    response.insert(QStringLiteral("packagePath"), relativePackagePath);
    response.insert(QStringLiteral("fileCount"), copyState.fileCount);
    response.insert(QStringLiteral("totalBytes"), copyState.totalBytes);
    response.insert(QStringLiteral("alreadyInstalled"), destinationExisted);
    response.insert(QStringLiteral("sourceType"), sourceType);
    response.insert(QStringLiteral("sourceUrl"), sourceUrl);
    response.insert(QStringLiteral("resolvedCommit"), resolvedCommit);
    QStringList cleanupWarnings;
    for (const QString& retiredPath : retiredPackagePaths) {
        const QString retiredAbsolutePath = QDir(registry_.rootPath()).absoluteFilePath(retiredPath);
        QString cleanupError;
        if (!removePathWithinRoot(retiredAbsolutePath, &cleanupError)) {
            cleanupWarnings.append(cleanupError);
        }
    }
    if (!cleanupWarnings.isEmpty()) {
        response.insert(QStringLiteral("warning"), true);
        response.insert(QStringLiteral("message"),
            QStringLiteral("插件已安装，但旧 package 清理失败：%1").arg(cleanupWarnings.join(QStringLiteral("；"))));
    }
    return response;
}

QVariantMap ExtensionInstaller::setEnabled(const QString& extensionId, bool enabled) {
    QString error;
    if (!registry_.load(&error)) {
        return result(false, error, QStringLiteral("registry_load_failed"));
    }
    if (!registry_.setEnabled(extensionId, enabled, &error)) {
        return result(false, error, QStringLiteral("registry_update_failed"));
    }
    QVariantMap response = result(true, enabled ? QStringLiteral("插件已启用") : QStringLiteral("插件已停用"));
    response.insert(QStringLiteral("id"), extensionId);
    response.insert(QStringLiteral("enabled"), enabled);
    return response;
}

QVariantMap ExtensionInstaller::rollback(const QString& extensionId) {
    QString error;
    if (!registry_.load(&error)) {
        return result(false, error, QStringLiteral("registry_load_failed"));
    }
    QString activatedPath;
    if (!registry_.activateRollbackPackage(extensionId, &activatedPath, &error)) {
        return result(false, error, QStringLiteral("rollback_unavailable"));
    }
    QVariantMap response = result(true, QStringLiteral("插件已切换到上一个 package"));
    response.insert(QStringLiteral("id"), extensionId);
    response.insert(QStringLiteral("packagePath"), activatedPath);
    return response;
}

QVariantMap ExtensionInstaller::uninstall(const QString& extensionId) {
    QString error;
    if (!registry_.load(&error)) {
        return result(false, error, QStringLiteral("registry_load_failed"));
    }
    if (registry_.packagePaths(extensionId).isEmpty()) {
        return result(false, QStringLiteral("找不到插件：%1").arg(extensionId), QStringLiteral("extension_not_found"));
    }

    const QString packageIdRoot = QDir(registry_.rootPath()).absoluteFilePath(
        QStringLiteral("packages/%1").arg(extensionId));
    const QString transactionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString stagedRemoval = QDir(registry_.rootPath()).absoluteFilePath(
        QStringLiteral("staging/uninstall-%1").arg(transactionId));
    bool packageMoved = false;
    if (QFileInfo::exists(packageIdRoot)) {
        if (!ExtensionManifestParser::pathIsWithin(
                QDir(registry_.rootPath()).absoluteFilePath(QStringLiteral("packages")), packageIdRoot)) {
            return result(false, QStringLiteral("拒绝删除 extension packages 根目录之外的路径"), QStringLiteral("unsafe_delete"));
        }
        if (!QDir().rename(packageIdRoot, stagedRemoval)) {
            return result(false, QStringLiteral("无法隔离待卸载插件 package"), QStringLiteral("uninstall_stage_failed"));
        }
        packageMoved = true;
    }

    if (!registry_.removeExtension(extensionId, &error)) {
        if (packageMoved) {
            QDir().rename(stagedRemoval, packageIdRoot);
        }
        return result(false, error, QStringLiteral("registry_update_failed"));
    }
    if (packageMoved && !removePathWithinRoot(stagedRemoval, &error)) {
        QVariantMap response = result(
            true,
            QStringLiteral("插件已从 registry 卸载，但残留 package 清理失败：%1").arg(error));
        response.insert(QStringLiteral("warning"), true);
        response.insert(QStringLiteral("id"), extensionId);
        return response;
    }

    QVariantMap response = result(true, QStringLiteral("插件已卸载"));
    response.insert(QStringLiteral("id"), extensionId);
    return response;
}

bool ExtensionInstaller::copyDirectorySecure(
    const QString& sourceRoot,
    const QString& sourceDirectory,
    const QString& destinationDirectory,
    CopyState* state,
    QString* error) const {
    const QDir source(sourceDirectory);
    const QFileInfoList entries = source.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
        QDir::DirsFirst | QDir::Name);
    for (const QFileInfo& info : entries) {
        if (shouldIgnoreName(info.fileName())) {
            continue;
        }
        if (info.isSymLink() || info.isJunction()
            || !ExtensionManifestParser::pathIsWithin(sourceRoot, info.absoluteFilePath())) {
            if (error) {
                *error = QStringLiteral("插件 package 包含 symlink、junction 或越界路径：%1").arg(info.fileName());
            }
            return false;
        }

        const QString destinationPath = QDir(destinationDirectory).absoluteFilePath(info.fileName());
        if (info.isDir()) {
            if (!ensureDirectory(destinationPath, error)
                || !copyDirectorySecure(sourceRoot, info.absoluteFilePath(), destinationPath, state, error)) {
                return false;
            }
            continue;
        }
        if (!info.isFile()) {
            if (error) {
                *error = QStringLiteral("插件 package 包含不支持的文件类型：%1").arg(info.fileName());
            }
            return false;
        }
        if (info.size() < 0 || info.size() > kMaxSingleFileBytes) {
            if (error) {
                *error = QStringLiteral("插件文件超过 64 MiB 限制：%1").arg(info.fileName());
            }
            return false;
        }
        ++state->fileCount;
        state->totalBytes += info.size();
        if (state->fileCount > kMaxPackageFiles || state->totalBytes > kMaxPackageBytes) {
            if (error) {
                *error = QStringLiteral("插件 package 超过文件数量或 256 MiB 大小限制");
            }
            return false;
        }
        if (!QFile::copy(info.absoluteFilePath(), destinationPath)) {
            if (error) {
                *error = QStringLiteral("复制插件文件失败：%1").arg(info.fileName());
            }
            return false;
        }
    }
    return true;
}

bool ExtensionInstaller::computePackageDigest(const QString& packageRoot, QString* digest, QString* error) const {
    QStringList relativeFiles;
    const QDir root(packageRoot);
    std::function<bool(const QString&)> collect = [&](const QString& directoryPath) {
        const QDir directory(directoryPath);
        const QFileInfoList entries = directory.entryInfoList(
            QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
            QDir::DirsFirst | QDir::Name);
        for (const QFileInfo& info : entries) {
            if (info.isSymLink() || info.isJunction()) {
                if (error) {
                    *error = QStringLiteral("digest 阶段发现 symlink：%1").arg(info.absoluteFilePath());
                }
                return false;
            }
            if (info.isDir()) {
                if (!collect(info.absoluteFilePath())) {
                    return false;
                }
            } else if (info.isFile()) {
                relativeFiles.append(QDir::fromNativeSeparators(root.relativeFilePath(info.absoluteFilePath())));
            }
        }
        return true;
    };
    if (!collect(packageRoot)) {
        return false;
    }
    std::sort(relativeFiles.begin(), relativeFiles.end());

    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const QString& relativePath : relativeFiles) {
        hash.addData(relativePath.toUtf8());
        hash.addData("\0", 1);
        QFile file(root.absoluteFilePath(relativePath));
        if (!file.open(QIODevice::ReadOnly)) {
            if (error) {
                *error = QStringLiteral("无法读取插件文件以计算 digest：%1").arg(relativePath);
            }
            return false;
        }
        while (!file.atEnd()) {
            const QByteArray chunk = file.read(1024 * 1024);
            if (chunk.isEmpty() && file.error() != QFile::NoError) {
                if (error) {
                    *error = QStringLiteral("读取插件文件失败：%1").arg(relativePath);
                }
                return false;
            }
            hash.addData(chunk);
        }
        hash.addData("\0", 1);
    }
    *digest = QString::fromLatin1(hash.result().toHex());
    return true;
}

bool ExtensionInstaller::removePathWithinRoot(const QString& path, QString* error) const {
    if (!QFileInfo::exists(path)) {
        return true;
    }
    if (!ExtensionManifestParser::pathIsWithin(registry_.rootPath(), path)
        || QDir::cleanPath(path) == QDir::cleanPath(registry_.rootPath())) {
        if (error) {
            *error = QStringLiteral("拒绝删除 extension 根目录之外或根目录本身");
        }
        return false;
    }
    QDir directory(path);
    if (!directory.removeRecursively()) {
        if (error) {
            *error = QStringLiteral("无法清理目录：%1").arg(path);
        }
        return false;
    }
    return true;
}

bool ExtensionInstaller::shouldIgnoreName(const QString& name) {
    static const QSet<QString> ignored = {
        QStringLiteral(".git"),
        QStringLiteral(".venv"),
        QStringLiteral("__pycache__"),
        QStringLiteral(".pytest_cache"),
        QStringLiteral(".ruff_cache"),
    };
    return ignored.contains(name);
}

QVariantMap ExtensionInstaller::result(bool ok, const QString& message, const QString& code) {
    QVariantMap response;
    response.insert(QStringLiteral("ok"), ok);
    response.insert(QStringLiteral("message"), message);
    if (!code.isEmpty()) {
        response.insert(QStringLiteral("code"), code);
    }
    return response;
}
