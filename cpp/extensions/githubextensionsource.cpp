#include "githubextensionsource.h"

#include "extensioninstaller.h"
#include "extensionmanifest.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUrl>
#include <QUuid>
#include <QtCore/private/qzipreader_p.h>

namespace {

constexpr int kMaxRedirects = 3;
constexpr qint64 kMaxApiPayloadBytes = 1024 * 1024;
constexpr qint64 kMaxArchiveBytes = 64LL * 1024 * 1024;
constexpr int kMaxPackageFiles = 4000;
constexpr qint64 kMaxPackageBytes = 256LL * 1024 * 1024;
constexpr qint64 kMaxSingleFileBytes = 64LL * 1024 * 1024;

bool isValidOwner(const QString& owner) {
    static const QRegularExpression pattern(
        QStringLiteral("^[A-Za-z0-9](?:[A-Za-z0-9-]{0,37}[A-Za-z0-9])?$"));
    return owner.size() <= 39 && pattern.match(owner).hasMatch() && !owner.contains(QStringLiteral("--"));
}

bool isValidRepository(const QString& repository) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_.-]+$"));
    return !repository.isEmpty() && repository.size() <= 100
        && repository != QStringLiteral(".") && repository != QStringLiteral("..")
        && pattern.match(repository).hasMatch();
}

bool isValidRef(const QString& ref) {
    if (ref.isEmpty() || ref.size() > 255 || ref.startsWith(QLatin1Char('/'))
        || ref.endsWith(QLatin1Char('/')) || ref.endsWith(QLatin1Char('.'))
        || ref.contains(QStringLiteral("..")) || ref.contains(QStringLiteral("//"))
        || ref.contains(QStringLiteral("@{")) || ref.contains(QLatin1Char('\\'))
        || ref.contains(QChar::Null)) {
        return false;
    }
    for (const QChar character : ref) {
        if (character.unicode() < 0x20 || character.unicode() == 0x7f
            || QStringLiteral(" ~^:?*[").contains(character)) {
            return false;
        }
    }
    return true;
}

QUrl apiUrl(const QString& path) {
    QUrl url;
    url.setScheme(QStringLiteral("https"));
    url.setHost(QStringLiteral("api.github.com"));
    url.setPath(path);
    return url;
}

bool safeArchiveRelativePath(const QString& path, QString* normalized) {
    if (path.isEmpty() || path.contains(QLatin1Char('\\')) || path.contains(QChar::Null)
        || path.startsWith(QLatin1Char('/')) || path.startsWith(QLatin1Char('\\'))
        || QRegularExpression(QStringLiteral("^[A-Za-z]:")).match(path).hasMatch()) {
        return false;
    }
    return ExtensionManifestParser::isSafeRelativePath(path, normalized);
}

QString networkFailureMessage(const QString& networkError, int statusCode) {
    if (statusCode == 403 || statusCode == 429) {
        return QStringLiteral("GitHub 请求被限流或拒绝（HTTP %1），请稍后重试").arg(statusCode);
    }
    if (statusCode == 404) {
        return QStringLiteral("GitHub 仓库、分支或 commit 不存在（HTTP 404）");
    }
    if (statusCode >= 400) {
        return QStringLiteral("GitHub 请求失败（HTTP %1）").arg(statusCode);
    }
    return QStringLiteral("GitHub 网络请求失败：%1").arg(networkError);
}

} // namespace

GitHubExtensionSource::GitHubExtensionSource(
    ExtensionInstaller& installer,
    QString extensionRoot,
    QObject* parent)
    : QObject(parent)
    , installer_(installer)
    , extensionRoot_(QDir::cleanPath(QFileInfo(extensionRoot).absoluteFilePath()))
    , network_(new QNetworkAccessManager(this)) {
}

bool GitHubExtensionSource::busy() const {
    return busy_;
}

QVariantMap GitHubExtensionSource::install(const QString& repositoryUrl) {
    if (busy_) {
        return result(false, QStringLiteral("已有 GitHub 插件操作正在进行"), QStringLiteral("operation_busy"));
    }

    spec_ = parseRepositoryUrl(repositoryUrl);
    if (!spec_.ok) {
        return result(false, spec_.message, spec_.code);
    }

    resolvedCommit_.clear();
    receivedBytes_ = 0;
    redirectCount_ = 0;
    setBusy(true);
    if (spec_.ref.isEmpty()) {
        startRepositoryRequest();
    } else {
        startCommitRequest(spec_.ref);
    }

    QVariantMap response = result(true, QStringLiteral("正在解析 GitHub 仓库版本"));
    response.insert(QStringLiteral("pending"), true);
    response.insert(QStringLiteral("sourceUrl"), spec_.sourceUrl);
    return response;
}

GitHubRepositorySpec GitHubExtensionSource::parseRepositoryUrl(const QString& repositoryUrl) {
    GitHubRepositorySpec spec;
    const QString input = repositoryUrl.trimmed();
    const QUrl url(input, QUrl::StrictMode);
    if (!url.isValid() || url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0
        || url.host().compare(QStringLiteral("github.com"), Qt::CaseInsensitive) != 0
        || !url.userInfo().isEmpty() || !url.query().isEmpty() || !url.fragment().isEmpty()
        || (url.port(-1) != -1 && url.port(-1) != 443)) {
        spec.code = QStringLiteral("github_url_invalid");
        spec.message = QStringLiteral("仅支持公开 https://github.com/<owner>/<repo> URL");
        return spec;
    }

    QStringList parts = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.size() < 2) {
        spec.code = QStringLiteral("github_url_invalid");
        spec.message = QStringLiteral("GitHub URL 缺少 owner 或 repo");
        return spec;
    }
    spec.owner = parts.at(0);
    spec.repository = parts.at(1);
    if (spec.repository.endsWith(QStringLiteral(".git"), Qt::CaseInsensitive)) {
        spec.repository.chop(4);
    }
    if (!isValidOwner(spec.owner) || !isValidRepository(spec.repository)) {
        spec.code = QStringLiteral("github_repository_invalid");
        spec.message = QStringLiteral("GitHub owner 或 repo 名称无效");
        return spec;
    }

    if (parts.size() > 2) {
        if (parts.size() < 4 || parts.at(2) != QStringLiteral("tree")) {
            spec.code = QStringLiteral("github_url_unsupported");
            spec.message = QStringLiteral("仅支持仓库根 URL 或 /tree/<ref> URL");
            return spec;
        }
        spec.ref = parts.mid(3).join(QLatin1Char('/'));
        if (!isValidRef(spec.ref)) {
            spec.code = QStringLiteral("github_ref_invalid");
            spec.message = QStringLiteral("GitHub ref 无效");
            return spec;
        }
    }

    QUrl canonical;
    canonical.setScheme(QStringLiteral("https"));
    canonical.setHost(QStringLiteral("github.com"));
    QString canonicalPath = QStringLiteral("/%1/%2").arg(spec.owner, spec.repository);
    if (!spec.ref.isEmpty()) {
        canonicalPath += QStringLiteral("/tree/%1").arg(spec.ref);
    }
    canonical.setPath(canonicalPath);
    spec.sourceUrl = canonical.toString(QUrl::FullyEncoded);
    spec.ok = true;
    return spec;
}

QVariantMap GitHubExtensionSource::extractArchive(
    const QString& archivePath,
    const QString& destinationRoot) {
    const QFileInfo archiveInfo(archivePath);
    if (!archiveInfo.exists() || !archiveInfo.isFile() || archiveInfo.size() > kMaxArchiveBytes) {
        return result(false, QStringLiteral("GitHub archive 不存在或超过 64 MiB"), QStringLiteral("archive_invalid"));
    }
    if (QFileInfo::exists(destinationRoot) && !QDir(destinationRoot).isEmpty()) {
        return result(false, QStringLiteral("archive 解包目标目录必须为空"), QStringLiteral("archive_destination_not_empty"));
    }
    if (!QDir().mkpath(destinationRoot)) {
        return result(false, QStringLiteral("无法创建 archive 解包目录"), QStringLiteral("archive_destination_failed"));
    }

    QZipReader reader(archivePath);
    if (!reader.exists() || !reader.isReadable() || reader.status() != QZipReader::NoError) {
        return result(false, QStringLiteral("无法读取 GitHub ZIP archive"), QStringLiteral("archive_read_failed"));
    }

    const QList<QZipReader::FileInfo> entries = reader.fileInfoList();
    if (entries.isEmpty() || entries.size() > kMaxPackageFiles + 1) {
        return result(false, QStringLiteral("GitHub archive 为空或文件数量超限"), QStringLiteral("archive_entry_limit"));
    }

    QString commonRoot;
    QSet<QString> seenPaths;
    int fileCount = 0;
    qint64 totalBytes = 0;
    for (const QZipReader::FileInfo& entry : entries) {
        QString archiveEntry = entry.filePath;
        while (archiveEntry.endsWith(QLatin1Char('/'))) {
            archiveEntry.chop(1);
        }
        QString normalizedEntry;
        if (!safeArchiveRelativePath(archiveEntry, &normalizedEntry) || entry.isSymLink
            || (!entry.isFile && !entry.isDir)) {
            return result(false, QStringLiteral("archive 包含不安全路径或文件类型：%1").arg(entry.filePath),
                QStringLiteral("archive_entry_unsafe"));
        }

        const QString topLevel = normalizedEntry.section(QLatin1Char('/'), 0, 0);
        if (commonRoot.isEmpty()) {
            commonRoot = topLevel;
        } else if (commonRoot != topLevel) {
            return result(false, QStringLiteral("GitHub archive 必须只有一个顶层目录"),
                QStringLiteral("archive_root_invalid"));
        }

        const QString relative = normalizedEntry.mid(commonRoot.size()).remove(0, 1);
        if (relative.isEmpty()) {
            if (!entry.isDir) {
                return result(false, QStringLiteral("archive 顶层 entry 必须是目录"),
                    QStringLiteral("archive_root_invalid"));
            }
            continue;
        }

        QString normalizedRelative;
        if (!safeArchiveRelativePath(relative, &normalizedRelative)) {
            return result(false, QStringLiteral("archive entry 逃逸顶层目录：%1").arg(entry.filePath),
                QStringLiteral("archive_entry_unsafe"));
        }
        const QString collisionKey = normalizedRelative.toCaseFolded();
        if (seenPaths.contains(collisionKey)) {
            return result(false, QStringLiteral("archive 包含重复或大小写冲突路径：%1").arg(normalizedRelative),
                QStringLiteral("archive_entry_duplicate"));
        }
        seenPaths.insert(collisionKey);

        const QString destinationPath = QDir(destinationRoot).absoluteFilePath(normalizedRelative);
        if (!ExtensionManifestParser::pathIsWithin(destinationRoot, destinationPath)) {
            return result(false, QStringLiteral("archive entry 超出 staging"), QStringLiteral("archive_entry_unsafe"));
        }
        if (entry.isDir) {
            if (!QDir().mkpath(destinationPath)) {
                return result(false, QStringLiteral("无法创建 archive 子目录：%1").arg(normalizedRelative),
                    QStringLiteral("archive_extract_failed"));
            }
            continue;
        }

        if (entry.size < 0 || entry.size > kMaxSingleFileBytes) {
            return result(false, QStringLiteral("archive 文件超过 64 MiB：%1").arg(normalizedRelative),
                QStringLiteral("archive_file_limit"));
        }
        ++fileCount;
        totalBytes += entry.size;
        if (fileCount > kMaxPackageFiles || totalBytes > kMaxPackageBytes) {
            return result(false, QStringLiteral("archive 解包后超过文件数量或 256 MiB 限制"),
                QStringLiteral("archive_package_limit"));
        }

        const QByteArray payload = reader.fileData(entry.filePath);
        if (reader.status() != QZipReader::NoError || payload.size() != entry.size) {
            return result(false, QStringLiteral("archive 文件读取或大小校验失败：%1").arg(normalizedRelative),
                QStringLiteral("archive_data_invalid"));
        }
        if (!QDir().mkpath(QFileInfo(destinationPath).absolutePath())) {
            return result(false, QStringLiteral("无法创建 archive 文件父目录"),
                QStringLiteral("archive_extract_failed"));
        }
        QSaveFile output(destinationPath);
        if (!output.open(QIODevice::WriteOnly) || output.write(payload) != payload.size() || !output.commit()) {
            return result(false, QStringLiteral("无法写入 archive 文件：%1").arg(normalizedRelative),
                QStringLiteral("archive_extract_failed"));
        }
    }
    reader.close();

    QVariantMap response = result(true, QStringLiteral("GitHub archive 已安全解包"));
    response.insert(QStringLiteral("packageRoot"), QDir::cleanPath(destinationRoot));
    response.insert(QStringLiteral("fileCount"), fileCount);
    response.insert(QStringLiteral("totalBytes"), totalBytes);
    return response;
}

void GitHubExtensionSource::startRepositoryRequest() {
    startRequest(apiUrl(QStringLiteral("/repos/%1/%2").arg(spec_.owner, spec_.repository)), Phase::Repository);
}

void GitHubExtensionSource::startCommitRequest(const QString& ref) {
    const QByteArray url = QByteArrayLiteral("https://api.github.com/repos/")
        + spec_.owner.toUtf8() + '/' + spec_.repository.toUtf8()
        + QByteArrayLiteral("/commits/") + QUrl::toPercentEncoding(ref);
    startRequest(QUrl::fromEncoded(url, QUrl::StrictMode), Phase::Commit);
}

void GitHubExtensionSource::startArchiveRequest() {
    stagingRoot_ = QDir(extensionRoot_).absoluteFilePath(
        QStringLiteral("staging/github-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    archivePath_ = QDir(stagingRoot_).absoluteFilePath(QStringLiteral("package.zip"));
    QString error;
    if (!prepareArchiveFile(&error)) {
        fail(QStringLiteral("archive_create_failed"), error);
        return;
    }

    QUrl url;
    url.setScheme(QStringLiteral("https"));
    url.setHost(QStringLiteral("codeload.github.com"));
    url.setPath(QStringLiteral("/%1/%2/zip/%3").arg(spec_.owner, spec_.repository, resolvedCommit_));
    startRequest(url, Phase::Archive);
}

void GitHubExtensionSource::startRequest(const QUrl& url, Phase phase, int redirectCount) {
    phase_ = phase;
    redirectCount_ = redirectCount;
    receivedBytes_ = 0;
    archiveWriteFailed_ = false;

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Fantareal-ExtensionHost/1.0"));
    request.setRawHeader("Accept", phase == Phase::Archive
            ? QByteArrayLiteral("application/zip")
            : QByteArrayLiteral("application/vnd.github+json"));
    request.setRawHeader("X-GitHub-Api-Version", QByteArrayLiteral("2022-11-28"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(phase == Phase::Archive ? 60000 : 15000);

    reply_ = network_->get(request);
    connect(reply_, &QNetworkReply::readyRead, this, &GitHubExtensionSource::handleReadyRead);
    connect(reply_, &QNetworkReply::finished, this, &GitHubExtensionSource::handleReplyFinished);
}

void GitHubExtensionSource::handleReadyRead() {
    if (!reply_ || phase_ != Phase::Archive || !archiveFile_) {
        return;
    }
    const QByteArray chunk = reply_->readAll();
    receivedBytes_ += chunk.size();
    if (receivedBytes_ > kMaxArchiveBytes) {
        reply_->abort();
        return;
    }
    if (archiveFile_->write(chunk) != chunk.size()) {
        archiveWriteFailed_ = true;
        reply_->abort();
    }
}

void GitHubExtensionSource::handleReplyFinished() {
    QNetworkReply* completedReply = reply_;
    if (!completedReply) {
        return;
    }
    if (phase_ == Phase::Archive) {
        handleReadyRead();
    }
    reply_ = nullptr;
    const int statusCode = completedReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QUrl redirectTarget = completedReply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
    const QNetworkReply::NetworkError networkError = completedReply->error();
    const QUrl completedUrl = completedReply->url();
    const QString completedError = completedReply->errorString();
    QByteArray payload;
    if (phase_ != Phase::Archive) {
        payload = completedReply->readAll();
        receivedBytes_ = payload.size();
    }
    completedReply->deleteLater();

    if (phase_ == Phase::Archive && receivedBytes_ > kMaxArchiveBytes) {
        fail(QStringLiteral("archive_size_invalid"), QStringLiteral("GitHub archive 超过 64 MiB"));
        return;
    }
    if (phase_ == Phase::Archive && archiveWriteFailed_) {
        fail(QStringLiteral("archive_write_failed"), QStringLiteral("写入 GitHub archive 失败"));
        return;
    }

    if (statusCode >= 300 && statusCode < 400 && !redirectTarget.isEmpty()) {
        if (redirectCount_ >= kMaxRedirects) {
            fail(QStringLiteral("redirect_limit"), QStringLiteral("GitHub 请求重定向次数超过限制"));
            return;
        }
        const QUrl resolvedTarget = completedUrl.resolved(redirectTarget);
        if (!redirectAllowed(resolvedTarget)) {
            fail(QStringLiteral("redirect_rejected"), QStringLiteral("GitHub 请求重定向到未允许的主机"));
            return;
        }
        if (phase_ == Phase::Archive) {
            QString error;
            if (!prepareArchiveFile(&error)) {
                fail(QStringLiteral("archive_create_failed"), error);
                return;
            }
        }
        startRequest(resolvedTarget, phase_, redirectCount_ + 1);
        return;
    }

    if (networkError != QNetworkReply::NoError || statusCode < 200 || statusCode >= 300) {
        fail(QStringLiteral("github_request_failed"), networkFailureMessage(completedError, statusCode));
        return;
    }
    if (phase_ != Phase::Archive && receivedBytes_ > kMaxApiPayloadBytes) {
        fail(QStringLiteral("github_response_too_large"), QStringLiteral("GitHub API 响应超过 1 MiB 限制"));
        return;
    }

    switch (phase_) {
    case Phase::Repository:
        handleRepositoryPayload(payload);
        break;
    case Phase::Commit:
        handleCommitPayload(payload);
        break;
    case Phase::Archive:
        handleArchiveComplete();
        break;
    case Phase::None:
        fail(QStringLiteral("operation_state_invalid"), QStringLiteral("GitHub 操作状态无效"));
        break;
    }
}

void GitHubExtensionSource::handleRepositoryPayload(const QByteArray& payload) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    const QString defaultBranch = document.object().value(QStringLiteral("default_branch")).toString();
    if (parseError.error != QJsonParseError::NoError || !document.isObject() || !isValidRef(defaultBranch)) {
        fail(QStringLiteral("github_repository_response_invalid"), QStringLiteral("GitHub 仓库元数据无效"));
        return;
    }
    startCommitRequest(defaultBranch);
}

void GitHubExtensionSource::handleCommitPayload(const QByteArray& payload) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    const QString commit = document.object().value(QStringLiteral("sha")).toString().toLower();
    static const QRegularExpression shaPattern(QStringLiteral("^[0-9a-f]{40}$"));
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
        || !shaPattern.match(commit).hasMatch()) {
        fail(QStringLiteral("github_commit_response_invalid"), QStringLiteral("GitHub commit 响应未包含 40 位 SHA"));
        return;
    }
    resolvedCommit_ = commit;
    startArchiveRequest();
}

void GitHubExtensionSource::handleArchiveComplete() {
    if (!archiveFile_) {
        fail(QStringLiteral("archive_state_invalid"), QStringLiteral("GitHub archive 文件状态无效"));
        return;
    }
    archiveFile_->flush();
    archiveFile_->close();
    delete archiveFile_;
    archiveFile_ = nullptr;
    if (receivedBytes_ <= 0 || receivedBytes_ > kMaxArchiveBytes) {
        fail(QStringLiteral("archive_size_invalid"), QStringLiteral("GitHub archive 为空或超过 64 MiB"));
        return;
    }

    const QString extractedRoot = QDir(stagingRoot_).absoluteFilePath(QStringLiteral("extracted"));
    const QVariantMap extraction = extractArchive(archivePath_, extractedRoot);
    if (!extraction.value(QStringLiteral("ok")).toBool()) {
        fail(extraction.value(QStringLiteral("code")).toString(), extraction.value(QStringLiteral("message")).toString());
        return;
    }

    QVariantMap installResult = installer_.installFromGitHubDirectory(
        extractedRoot, spec_.sourceUrl, resolvedCommit_);
    installResult.insert(QStringLiteral("sourceUrl"), spec_.sourceUrl);
    installResult.insert(QStringLiteral("resolvedCommit"), resolvedCommit_);
    complete(installResult);
}

void GitHubExtensionSource::fail(const QString& code, const QString& message) {
    if (reply_) {
        reply_->abort();
        reply_->deleteLater();
        reply_ = nullptr;
    }
    if (archiveFile_) {
        archiveFile_->close();
        delete archiveFile_;
        archiveFile_ = nullptr;
    }
    cleanupStaging();
    phase_ = Phase::None;
    setBusy(false);
    emit finished(result(false, message, code));
}

void GitHubExtensionSource::complete(const QVariantMap& resultValue) {
    cleanupStaging();
    phase_ = Phase::None;
    setBusy(false);
    emit finished(resultValue);
}

void GitHubExtensionSource::setBusy(bool busyValue) {
    if (busy_ == busyValue) {
        return;
    }
    busy_ = busyValue;
    emit busyChanged();
}

void GitHubExtensionSource::cleanupStaging() {
    if (stagingRoot_.isEmpty() || !QFileInfo::exists(stagingRoot_)) {
        stagingRoot_.clear();
        archivePath_.clear();
        return;
    }
    const QString stagingBase = QDir(extensionRoot_).absoluteFilePath(QStringLiteral("staging"));
    if (ExtensionManifestParser::pathIsWithin(stagingBase, stagingRoot_)
        && QDir::cleanPath(stagingRoot_) != QDir::cleanPath(stagingBase)) {
        QDir(stagingRoot_).removeRecursively();
    }
    stagingRoot_.clear();
    archivePath_.clear();
}

bool GitHubExtensionSource::prepareArchiveFile(QString* error) {
    if (archiveFile_) {
        archiveFile_->close();
        delete archiveFile_;
        archiveFile_ = nullptr;
    }
    if (!QDir().mkpath(stagingRoot_)) {
        if (error) {
            *error = QStringLiteral("无法创建 GitHub staging 目录");
        }
        return false;
    }
    archiveFile_ = new QFile(archivePath_, this);
    if (!archiveFile_->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) {
            *error = QStringLiteral("无法创建 GitHub archive：%1").arg(archiveFile_->errorString());
        }
        delete archiveFile_;
        archiveFile_ = nullptr;
        return false;
    }
    receivedBytes_ = 0;
    return true;
}

bool GitHubExtensionSource::redirectAllowed(const QUrl& target) const {
    if (!target.isValid() || target.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0
        || !target.userInfo().isEmpty() || (target.port(-1) != -1 && target.port(-1) != 443)) {
        return false;
    }
    const QString host = target.host().toLower();
    if (phase_ == Phase::Archive) {
        return host == QStringLiteral("codeload.github.com");
    }
    return host == QStringLiteral("api.github.com");
}

QVariantMap GitHubExtensionSource::result(bool ok, const QString& message, const QString& code) {
    QVariantMap response {
        { QStringLiteral("ok"), ok },
        { QStringLiteral("message"), message },
    };
    if (!code.isEmpty()) {
        response.insert(QStringLiteral("code"), code);
    }
    return response;
}
