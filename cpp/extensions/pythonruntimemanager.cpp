#include "pythonruntimemanager.h"

#include "extensionmanifest.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QUuid>
#include <private/qzipreader_p.h>

namespace {

constexpr auto kUvVersion = "0.11.3";
constexpr auto kPythonVersion = "3.11.15";
constexpr auto kPythonDistribution = "cpython-3.11.15-windows-x86_64-none";
constexpr auto kUvArchiveUrl =
    "https://github.com/astral-sh/uv/releases/download/0.11.3/uv-x86_64-pc-windows-msvc.zip";
constexpr auto kUvArchiveSha256 = "ae681c0aaec7cc96af184648cb88d73f8393ed60fa5880abdd6bdb910f9b227c";
constexpr auto kUvExecutableSha256 = "07876908e19cf9a875a01d3b702c89b25ded154cc736079feeb4ef78a8f4ca64";
constexpr qint64 kUvExecutableBytes = 67'044'352;
constexpr qint64 kMaxUvArchiveBytes = 32LL * 1024 * 1024;
constexpr int kMaxRedirects = 5;

QString sha256File(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        hash.addData(file.read(1024 * 1024));
    }
    return QString::fromLatin1(hash.result().toHex());
}

bool allowedDownloadUrl(const QUrl& url) {
    if (url.scheme() != QStringLiteral("https") || url.userInfo().size() > 0 || url.port(-1) != -1) {
        return false;
    }
    const QString host = url.host().toLower();
    return host == QStringLiteral("github.com")
        || host == QStringLiteral("release-assets.githubusercontent.com")
        || host == QStringLiteral("objects.githubusercontent.com");
}

} // namespace

PythonRuntimeManager::PythonRuntimeManager(QString extensionRoot, QObject* parent)
    : QObject(parent)
    , extensionRoot_(QDir::cleanPath(QFileInfo(extensionRoot).absoluteFilePath())) {
    processTimer_.setSingleShot(true);
    connect(&processTimer_, &QTimer::timeout, this, [this]() {
        if (stage_ == Stage::InstallingPython) {
            process_.kill();
            failCurrent(QStringLiteral("python_install_timeout"), QStringLiteral("Python runtime 安装超时"));
        } else if (stage_ == Stage::SyncingEnvironment) {
            process_.kill();
            failCurrent(QStringLiteral("environment_sync_timeout"), QStringLiteral("插件 Python 环境安装超时"));
        }
    });
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
        this, &PythonRuntimeManager::handleProcessFinished);
    connect(&process_, &QProcess::errorOccurred, this, &PythonRuntimeManager::handleProcessError);
}

QString PythonRuntimeManager::ensureEnvironment(
    const QString& extensionId,
    const QString& packageRoot,
    const QString& packageDigest,
    const QString& lockfileRelativePath) {
    Request request;
    request.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    request.extensionId = extensionId;
    request.packageRoot = QDir::cleanPath(QFileInfo(packageRoot).absoluteFilePath());
    request.packageDigest = packageDigest.toLower();
    request.lockfileRelativePath = lockfileRelativePath;
    const QString environmentKey = QString::fromLatin1(
        QCryptographicHash::hash(
            request.extensionId.toUtf8() + QByteArrayLiteral(":") + request.packageDigest.toUtf8(),
            QCryptographicHash::Sha256)
            .toHex()
            .left(32));
    request.environmentRoot = QDir(runtimesRoot()).absoluteFilePath(
        QStringLiteral("envs/%1").arg(environmentKey));
    queue_.enqueue(request);
    QTimer::singleShot(0, this, &PythonRuntimeManager::startNext);
    return request.id;
}

void PythonRuntimeManager::cancel(const QString& requestId) {
    for (qsizetype i = queue_.size() - 1; i >= 0; --i) {
        if (queue_.at(i).id == requestId) {
            queue_.removeAt(i);
        }
    }
    if (current_.id != requestId) {
        return;
    }
    if (reply_) {
        reply_->abort();
    }
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
    }
    failCurrent(QStringLiteral("runtime_request_cancelled"), QStringLiteral("runtime 请求已取消"));
}

QString PythonRuntimeManager::uvVersion() { return QLatin1String(kUvVersion); }
QString PythonRuntimeManager::pythonVersion() { return QLatin1String(kPythonVersion); }
QString PythonRuntimeManager::uvArchiveUrl() { return QLatin1String(kUvArchiveUrl); }
QString PythonRuntimeManager::uvArchiveSha256() { return QLatin1String(kUvArchiveSha256); }

void PythonRuntimeManager::startNext() {
    if (stage_ != Stage::Idle || !current_.id.isEmpty() || queue_.isEmpty()) {
        return;
    }
    current_ = queue_.dequeue();
    QString code;
    QString error;
    if (!validateRequest(&current_, &code, &error)) {
        failCurrent(code, error);
        return;
    }
    if (environmentIsReady(current_)) {
        completeCurrent(QDir(current_.environmentRoot).absoluteFilePath(QStringLiteral("Scripts/python.exe")));
        return;
    }
    ensureUv();
}

bool PythonRuntimeManager::validateRequest(Request* request, QString* code, QString* error) const {
    static const QRegularExpression idPattern(QStringLiteral("^[a-z0-9][a-z0-9._-]{1,79}$"));
    static const QRegularExpression digestPattern(QStringLiteral("^[0-9a-f]{64}$"));
    if (!idPattern.match(request->extensionId).hasMatch()
        || !digestPattern.match(request->packageDigest).hasMatch()) {
        *code = QStringLiteral("runtime_request_invalid");
        *error = QStringLiteral("extension id 或 package digest 无效");
        return false;
    }
    const QString packagesRoot = QDir(extensionRoot_).absoluteFilePath(QStringLiteral("packages"));
    const QFileInfo packageInfo(request->packageRoot);
    if (!packageInfo.isDir() || packageInfo.isSymLink() || packageInfo.isJunction()
        || !ExtensionManifestParser::pathIsWithin(packagesRoot, request->packageRoot)) {
        *code = QStringLiteral("runtime_package_unsafe");
        *error = QStringLiteral("Python package 路径不安全");
        return false;
    }
    QString normalizedLockfile;
    if (!ExtensionManifestParser::isSafeRelativePath(request->lockfileRelativePath, &normalizedLockfile)) {
        *code = QStringLiteral("runtime_lockfile_unsafe");
        *error = QStringLiteral("Python lockfile 路径不安全");
        return false;
    }
    request->lockfileRelativePath = normalizedLockfile;
    if (normalizedLockfile != QStringLiteral("uv.lock")) {
        *code = QStringLiteral("runtime_lockfile_unsupported");
        *error = QStringLiteral("Extension Platform v1 仅支持根目录 uv.lock");
        return false;
    }
    const QFileInfo lockfile(QDir(request->packageRoot).absoluteFilePath(normalizedLockfile));
    const QFileInfo project(QDir(request->packageRoot).absoluteFilePath(QStringLiteral("pyproject.toml")));
    if (!lockfile.isFile() || lockfile.isSymLink() || lockfile.isJunction()
        || !project.isFile() || project.isSymLink() || project.isJunction()) {
        *code = QStringLiteral("runtime_project_invalid");
        *error = QStringLiteral("插件缺少安全的 pyproject.toml 或 lockfile");
        return false;
    }
    return true;
}

void PythonRuntimeManager::ensureUv() {
    const QFileInfo uvInfo(uvExecutable());
    if (uvInfo.isFile() && !uvInfo.isSymLink() && !uvInfo.isJunction()
        && uvInfo.size() == kUvExecutableBytes
        && sha256File(uvExecutable()) == QLatin1String(kUvExecutableSha256)) {
        ensurePython();
        return;
    }
    emit runtimeStatusChanged(QStringLiteral("downloading_uv"), QStringLiteral("正在下载固定版本 uv 0.11.3"));
    startUvDownload(QUrl(QLatin1String(kUvArchiveUrl)), 0);
}

void PythonRuntimeManager::startUvDownload(const QUrl& url, int redirectCount) {
    if (!allowedDownloadUrl(url) || redirectCount > kMaxRedirects) {
        failCurrent(QStringLiteral("uv_download_redirect_denied"), QStringLiteral("uv 下载重定向不安全"));
        return;
    }
    resetDownload();
    stage_ = Stage::DownloadingUv;
    redirectCount_ = redirectCount;
    const QString stagingRoot = QDir(runtimesRoot()).absoluteFilePath(QStringLiteral("staging"));
    if (!QDir().mkpath(stagingRoot)) {
        failCurrent(QStringLiteral("runtime_staging_failed"), QStringLiteral("无法创建 runtime staging 目录"));
        return;
    }
    downloadArchivePath_ = QDir(stagingRoot).absoluteFilePath(
        QStringLiteral("uv-%1.zip").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    downloadFile_ = std::make_unique<QSaveFile>(downloadArchivePath_);
    if (!downloadFile_->open(QIODevice::WriteOnly)) {
        failCurrent(QStringLiteral("uv_download_open_failed"), QStringLiteral("无法创建 uv 下载临时文件"));
        return;
    }
    downloadHash_ = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Fantareal-ExtensionHost/1.0"));
    request.setTransferTimeout(60'000);
    reply_ = network_.get(request);
    connect(reply_, &QNetworkReply::readyRead, this, &PythonRuntimeManager::handleDownloadReadyRead);
    connect(reply_, &QNetworkReply::finished, this, &PythonRuntimeManager::handleDownloadFinished);
}

void PythonRuntimeManager::handleDownloadReadyRead() {
    if (!reply_ || !downloadFile_) {
        return;
    }
    const QByteArray chunk = reply_->readAll();
    downloadedBytes_ += chunk.size();
    if (downloadedBytes_ > kMaxUvArchiveBytes || downloadFile_->write(chunk) != chunk.size()) {
        reply_->abort();
        failCurrent(QStringLiteral("uv_download_limit"), QStringLiteral("uv 下载超过 32 MiB 或写入失败"));
        return;
    }
    downloadHash_->addData(chunk);
}

void PythonRuntimeManager::handleDownloadFinished() {
    if (!reply_) {
        return;
    }
    QNetworkReply* finishedReply = reply_;
    const QByteArray tail = finishedReply->readAll();
    if (!tail.isEmpty() && downloadFile_ && downloadHash_) {
        downloadedBytes_ += tail.size();
        if (downloadedBytes_ > kMaxUvArchiveBytes || downloadFile_->write(tail) != tail.size()) {
            reply_ = nullptr;
            finishedReply->deleteLater();
            failCurrent(QStringLiteral("uv_download_limit"), QStringLiteral("uv 下载超过 32 MiB 或写入失败"));
            return;
        }
        downloadHash_->addData(tail);
    }
    const QUrl redirect = finishedReply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
    const QNetworkReply::NetworkError networkError = finishedReply->error();
    const QUrl baseUrl = finishedReply->url();
    reply_ = nullptr;
    finishedReply->deleteLater();

    if (redirect.isValid()) {
        const QUrl target = baseUrl.resolved(redirect);
        resetDownload();
        startUvDownload(target, redirectCount_ + 1);
        return;
    }
    if (networkError != QNetworkReply::NoError) {
        failCurrent(QStringLiteral("uv_download_failed"), QStringLiteral("uv 下载失败"));
        return;
    }
    if (!downloadFile_ || !downloadHash_) {
        return;
    }
    if (downloadedBytes_ <= 0
        || QString::fromLatin1(downloadHash_->result().toHex()) != QLatin1String(kUvArchiveSha256)) {
        failCurrent(QStringLiteral("uv_checksum_mismatch"), QStringLiteral("uv archive SHA-256 校验失败"));
        return;
    }
    if (!downloadFile_->commit()) {
        failCurrent(QStringLiteral("uv_download_commit_failed"), QStringLiteral("无法原子保存 uv archive"));
        return;
    }
    const QString archivePath = downloadArchivePath_;
    downloadFile_.reset();
    downloadHash_.reset();
    QString error;
    if (!installUvFromArchive(archivePath, &error)) {
        QFile::remove(archivePath);
        failCurrent(QStringLiteral("uv_archive_invalid"), error);
        return;
    }
    QFile::remove(archivePath);
    ensurePython();
}

bool PythonRuntimeManager::installUvFromArchive(const QString& archivePath, QString* error) {
    QZipReader reader(archivePath);
    if (!reader.exists() || !reader.isReadable() || reader.status() != QZipReader::NoError) {
        *error = QStringLiteral("无法读取 uv archive");
        return false;
    }
    QZipReader::FileInfo uvEntry;
    bool found = false;
    for (const QZipReader::FileInfo& entry : reader.fileInfoList()) {
        if (entry.filePath == QStringLiteral("uv.exe") && entry.isFile && !entry.isSymLink) {
            uvEntry = entry;
            found = true;
            break;
        }
    }
    if (!found || uvEntry.size != kUvExecutableBytes) {
        *error = QStringLiteral("uv archive 缺少预期的 uv.exe");
        return false;
    }
    const QByteArray payload = reader.fileData(uvEntry.filePath);
    reader.close();
    if (payload.size() != kUvExecutableBytes
        || QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex())
            != QLatin1String(kUvExecutableSha256)) {
        *error = QStringLiteral("uv.exe 内容校验失败");
        return false;
    }
    if (!QDir().mkpath(QFileInfo(uvExecutable()).absolutePath())) {
        *error = QStringLiteral("无法创建 uv runtime 目录");
        return false;
    }
    QSaveFile output(uvExecutable());
    if (!output.open(QIODevice::WriteOnly) || output.write(payload) != payload.size() || !output.commit()) {
        *error = QStringLiteral("无法原子安装 uv.exe");
        return false;
    }
    return true;
}

void PythonRuntimeManager::ensurePython() {
    const QFileInfo pythonInfo(pythonExecutable());
    if (pythonInfo.isFile() && !pythonInfo.isSymLink() && !pythonInfo.isJunction()) {
        syncEnvironment();
        return;
    }
    emit runtimeStatusChanged(QStringLiteral("installing_python"), QStringLiteral("正在安装 managed Python 3.11.15"));
    startProcess(Stage::InstallingPython,
        { QStringLiteral("python"), QStringLiteral("install"), QLatin1String(kPythonVersion),
            QStringLiteral("--install-dir"), pythonInstallRoot(), QStringLiteral("--no-bin"),
            QStringLiteral("--no-registry") },
        5 * 60'000);
}

void PythonRuntimeManager::syncEnvironment() {
    if (environmentIsReady(current_)) {
        completeCurrent(QDir(current_.environmentRoot).absoluteFilePath(QStringLiteral("Scripts/python.exe")));
        return;
    }
    if (!QDir().mkpath(QFileInfo(current_.environmentRoot).absolutePath())) {
        failCurrent(QStringLiteral("environment_parent_failed"), QStringLiteral("无法创建插件环境父目录"));
        return;
    }
    emit runtimeStatusChanged(QStringLiteral("syncing_environment"),
        QStringLiteral("正在安装插件 %1 的锁定依赖").arg(current_.extensionId));
    startProcess(Stage::SyncingEnvironment,
        { QStringLiteral("sync"), QStringLiteral("--frozen"), QStringLiteral("--no-dev"),
            QStringLiteral("--no-editable"), QStringLiteral("--python"), pythonExecutable(),
            QStringLiteral("--no-python-downloads"), QStringLiteral("--project"), current_.packageRoot },
        10 * 60'000);
}

void PythonRuntimeManager::startProcess(Stage stage, const QStringList& arguments, int timeoutMs) {
    resetProcess();
    stage_ = stage;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.remove(QStringLiteral("PYTHONHOME"));
    environment.remove(QStringLiteral("PYTHONPATH"));
    environment.insert(QStringLiteral("UV_CACHE_DIR"), QDir(runtimesRoot()).absoluteFilePath(QStringLiteral("cache")));
    environment.insert(QStringLiteral("UV_NO_PROGRESS"), QStringLiteral("1"));
    if (stage == Stage::SyncingEnvironment) {
        environment.insert(QStringLiteral("UV_PYTHON_DOWNLOADS"), QStringLiteral("never"));
        environment.insert(QStringLiteral("UV_PROJECT_ENVIRONMENT"), current_.environmentRoot);
    }
    process_.setProcessEnvironment(environment);
    process_.setWorkingDirectory(current_.packageRoot);
    process_.setProgram(uvExecutable());
    process_.setArguments(arguments);
    process_.start();
    processTimer_.start(timeoutMs);
}

void PythonRuntimeManager::handleProcessFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    if (handlingProcessFailure_ || (stage_ != Stage::InstallingPython && stage_ != Stage::SyncingEnvironment)) {
        return;
    }
    processTimer_.stop();
    const QString diagnostics = QString::fromUtf8(process_.readAllStandardError()).right(2000).trimmed();
    if (exitStatus != QProcess::NormalExit || exitCode != 0) {
        const QString code = stage_ == Stage::InstallingPython
            ? QStringLiteral("python_install_failed")
            : QStringLiteral("environment_sync_failed");
        failCurrent(code, diagnostics.isEmpty() ? QStringLiteral("uv 子进程执行失败") : diagnostics);
        return;
    }
    if (stage_ == Stage::InstallingPython) {
        if (!QFileInfo::exists(pythonExecutable())) {
            failCurrent(QStringLiteral("python_install_incomplete"), QStringLiteral("uv 未生成预期 Python executable"));
            return;
        }
        stage_ = Stage::Idle;
        syncEnvironment();
        return;
    }
    const QString environmentPython = QDir(current_.environmentRoot).absoluteFilePath(QStringLiteral("Scripts/python.exe"));
    if (!QFileInfo::exists(environmentPython)) {
        failCurrent(QStringLiteral("environment_sync_incomplete"), QStringLiteral("插件环境缺少 Python executable"));
        return;
    }
    QString error;
    if (!writeEnvironmentMarker(current_, &error)) {
        failCurrent(QStringLiteral("environment_marker_failed"), error);
        return;
    }
    completeCurrent(environmentPython);
}

void PythonRuntimeManager::handleProcessError(QProcess::ProcessError error) {
    if (handlingProcessFailure_ || error == QProcess::Crashed) {
        return;
    }
    handlingProcessFailure_ = true;
    const QString code = stage_ == Stage::InstallingPython
        ? QStringLiteral("python_install_start_failed")
        : QStringLiteral("environment_sync_start_failed");
    const QString message = process_.errorString();
    handlingProcessFailure_ = false;
    failCurrent(code, message);
}

void PythonRuntimeManager::failCurrent(const QString& code, const QString& error) {
    if (current_.id.isEmpty()) {
        return;
    }
    const QString requestId = current_.id;
    resetDownload();
    resetProcess();
    stage_ = Stage::Idle;
    current_ = {};
    emit environmentFailed(requestId, code, error.left(4000));
    QTimer::singleShot(0, this, &PythonRuntimeManager::startNext);
}

void PythonRuntimeManager::completeCurrent(const QString& pythonExecutablePath) {
    const QString requestId = current_.id;
    resetDownload();
    resetProcess();
    stage_ = Stage::Idle;
    current_ = {};
    emit runtimeStatusChanged(QStringLiteral("ready"), QStringLiteral("Python 插件环境已就绪"));
    emit environmentReady(requestId, QDir::cleanPath(pythonExecutablePath));
    QTimer::singleShot(0, this, &PythonRuntimeManager::startNext);
}

void PythonRuntimeManager::resetDownload() {
    if (reply_) {
        reply_->disconnect(this);
        reply_->abort();
        reply_->deleteLater();
        reply_ = nullptr;
    }
    if (downloadFile_) {
        downloadFile_->cancelWriting();
    }
    downloadFile_.reset();
    downloadHash_.reset();
    downloadedBytes_ = 0;
}

void PythonRuntimeManager::resetProcess() {
    processTimer_.stop();
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
        process_.waitForFinished(1000);
    }
    process_.setProgram({});
    process_.setArguments({});
}

QString PythonRuntimeManager::runtimesRoot() const {
    return QDir(extensionRoot_).absoluteFilePath(QStringLiteral("runtimes"));
}

QString PythonRuntimeManager::uvExecutable() const {
    return QDir(runtimesRoot()).absoluteFilePath(
        QStringLiteral("uv/%1/uv.exe").arg(QLatin1String(kUvVersion)));
}

QString PythonRuntimeManager::pythonInstallRoot() const {
    return QDir(runtimesRoot()).absoluteFilePath(QStringLiteral("python"));
}

QString PythonRuntimeManager::pythonExecutable() const {
    return QDir(pythonInstallRoot()).absoluteFilePath(
        QStringLiteral("%1/python.exe").arg(QLatin1String(kPythonDistribution)));
}

QString PythonRuntimeManager::markerPath(const Request& request) const {
    return QDir(request.environmentRoot).absoluteFilePath(QStringLiteral(".fantareal-runtime.json"));
}

bool PythonRuntimeManager::environmentIsReady(const Request& request) const {
    QFile marker(markerPath(request));
    if (!marker.open(QIODevice::ReadOnly)) {
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(marker.readAll(), &parseError);
    const QJsonObject root = document.object();
    const QFileInfo environmentPython(QDir(request.environmentRoot).absoluteFilePath(QStringLiteral("Scripts/python.exe")));
    return parseError.error == QJsonParseError::NoError && document.isObject()
        && root.value(QStringLiteral("extensionId")).toString() == request.extensionId
        && root.value(QStringLiteral("packageDigest")).toString() == request.packageDigest
        && root.value(QStringLiteral("uvVersion")).toString() == QLatin1String(kUvVersion)
        && root.value(QStringLiteral("pythonVersion")).toString() == QLatin1String(kPythonVersion)
        && QFileInfo::exists(pythonExecutable())
        && environmentPython.isFile() && !environmentPython.isSymLink() && !environmentPython.isJunction();
}

bool PythonRuntimeManager::writeEnvironmentMarker(const Request& request, QString* error) const {
    QSaveFile marker(markerPath(request));
    const QJsonObject root {
        { QStringLiteral("schemaVersion"), 1 },
        { QStringLiteral("extensionId"), request.extensionId },
        { QStringLiteral("packageDigest"), request.packageDigest },
        { QStringLiteral("uvVersion"), QLatin1String(kUvVersion) },
        { QStringLiteral("pythonVersion"), QLatin1String(kPythonVersion) },
    };
    const QByteArray payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (!marker.open(QIODevice::WriteOnly) || marker.write(payload) != payload.size() || !marker.commit()) {
        *error = QStringLiteral("无法原子保存插件环境 marker");
        return false;
    }
    return true;
}
