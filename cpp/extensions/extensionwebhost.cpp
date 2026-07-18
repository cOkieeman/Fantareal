#include "extensionwebhost.h"

#include "extensionartifactrouter.h"
#include "extensionmanifest.h"
#include "extensionregistry.h"
#include "extensionservicehost.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>
#include <QWebEngineDownloadRequest>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlScheme>
#include <QWebEngineUrlSchemeHandler>


namespace {

constexpr auto kScheme = "fantareal-extension";
constexpr auto kBridgeObjectName = "fantarealExtensionBridge";
constexpr auto kBootstrapScriptName = "fantareal-extension-host-v1";
constexpr qint64 kMaxSelectedFileBytes = 64LL * 1024 * 1024;
constexpr qint64 kMaxHtmlBytes = 4LL * 1024 * 1024;

QVariantMap failure(const QString& code, const QString& message) {
    return {
        { QStringLiteral("ok"), false },
        { QStringLiteral("code"), code },
        { QStringLiteral("message"), message },
    };
}

QString javascriptString(const QString& value) {
    const QByteArray encoded = QJsonDocument(QJsonArray { value }).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(encoded.mid(1, encoded.size() - 2));
}

QString normalizedSuffix(const QString& value) {
    const QString suffix = value.trimmed().toLower();
    return suffix.startsWith(QLatin1Char('.')) ? suffix : QStringLiteral(".") + suffix;
}

QString cspMetaTag() {
    return QStringLiteral(
        "<meta http-equiv=\"Content-Security-Policy\" content=\""
        "default-src 'self'; base-uri 'none'; object-src 'none'; frame-src 'none'; "
        "child-src 'none'; connect-src 'none'; form-action 'none'; media-src 'none'; "
        "worker-src 'none'; img-src 'self' data: blob:; font-src 'self' data:; "
        "style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'\">");
}

QByteArray injectCspIntoHtml(QByteArray html) {
    if (html.size() > kMaxHtmlBytes) {
        return {};
    }
    const QByteArray tag = cspMetaTag().toUtf8();
    static const QRegularExpression headPattern(QStringLiteral("<head(?:\\s[^>]*)?>"),
        QRegularExpression::CaseInsensitiveOption);
    const QString text = QString::fromUtf8(html);
    const QRegularExpressionMatch match = headPattern.match(text);
    if (match.hasMatch()) {
        const qsizetype byteOffset = text.left(match.capturedEnd()).toUtf8().size();
        html.insert(byteOffset, tag);
        return html;
    }
    html.prepend(QByteArrayLiteral("<head>") + tag + QByteArrayLiteral("</head>"));
    return html;
}

bool workspaceTreeContainsLink(const QString& path) {
    const QDir directory(path);
    const QFileInfoList entries = directory.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo& entry : entries) {
        if (entry.isSymLink() || entry.isJunction()) {
            return true;
        }
        if (entry.isDir() && workspaceTreeContainsLink(entry.absoluteFilePath())) {
            return true;
        }
    }
    return false;
}

class ExtensionRequestInterceptor final : public QWebEngineUrlRequestInterceptor {
public:
    explicit ExtensionRequestInterceptor(ExtensionWebHost* host)
        : QWebEngineUrlRequestInterceptor(host)
        , host_(host) {
    }

    void interceptRequest(QWebEngineUrlRequestInfo& info) override {
        if (!host_->isAllowedRequest(info.requestUrl(), info.firstPartyUrl(), static_cast<int>(info.resourceType()))) {
            info.block(true);
        }
    }

private:
    ExtensionWebHost* host_;
};

class ExtensionSchemeHandler final : public QWebEngineUrlSchemeHandler {
public:
    explicit ExtensionSchemeHandler(ExtensionWebHost* host)
        : QWebEngineUrlSchemeHandler(host)
        , host_(host) {
    }

    void requestStarted(QWebEngineUrlRequestJob* job) override {
        if (job->requestMethod() != QByteArrayLiteral("GET")) {
            job->fail(QWebEngineUrlRequestJob::RequestDenied);
            return;
        }
        QByteArray mimeType;
        bool injectCsp = false;
        const QString path = host_->resolveResourcePath(job->requestUrl(), &mimeType, &injectCsp);
        if (path.isEmpty()) {
            job->fail(QWebEngineUrlRequestJob::UrlNotFound);
            return;
        }

        auto* file = new QFile(path, job);
        if (!file->open(QIODevice::ReadOnly)) {
            job->fail(QWebEngineUrlRequestJob::RequestFailed);
            return;
        }
        if (!injectCsp) {
            job->reply(mimeType, file);
            return;
        }
        const QByteArray html = injectCspIntoHtml(file->readAll());
        file->close();
        if (html.isEmpty()) {
            job->fail(QWebEngineUrlRequestJob::RequestFailed);
            return;
        }
        auto* buffer = new QBuffer(job);
        buffer->setData(html);
        buffer->open(QIODevice::ReadOnly);
        job->reply(QByteArrayLiteral("text/html; charset=utf-8"), buffer);
    }

private:
    ExtensionWebHost* host_;
};

} // namespace

ExtensionWebHost::ExtensionWebHost(QObject* parent)
    : ExtensionWebHost(QDir::cleanPath(qEnvironmentVariable("FANTAREAL_EXTENSION_ROOT")), parent) {
}

ExtensionWebHost::ExtensionWebHost(const QString& extensionRoot, QObject* parent)
    : QObject(parent)
    , extensionRoot_(extensionRoot.trimmed().isEmpty()
              ? QDir::cleanPath(QDir::home().absoluteFilePath(QStringLiteral(".fantareal/extensions")))
              : QDir::cleanPath(QFileInfo(extensionRoot).absoluteFilePath())) {
    artifactRouter_ = new ExtensionArtifactRouter;
    auto* host = new ExtensionServiceHost(extensionRoot_, this);
    serviceHost_ = host;
    connect(host, &ExtensionServiceHost::responseReady, this,
        [this](const QString& requestId, bool ok, const QVariant& result,
            const QString& errorCode, const QString& errorMessage) {
            if (!ok) {
                setLastError(errorMessage);
            }
            queueResponse(requestId, ok, result, errorCode, errorMessage);
        });
    configureProfile();
}

ExtensionWebHost::~ExtensionWebHost() {
    QWebEngineProfile* profile = QWebEngineProfile::defaultProfile();
    profile->setUrlRequestInterceptor(nullptr);
    if (schemeHandler_) {
        profile->removeUrlSchemeHandler(static_cast<QWebEngineUrlSchemeHandler*>(schemeHandler_));
    }
    for (const QWebEngineScript& script : profile->scripts()->find(QLatin1String(kBootstrapScriptName))) {
        profile->scripts()->remove(script);
    }
    delete artifactRouter_;
}

void ExtensionWebHost::registerUrlScheme() {
    QWebEngineUrlScheme scheme { QByteArray(kScheme) };
    scheme.setSyntax(QWebEngineUrlScheme::Syntax::HostAndPort);
    scheme.setDefaultPort(443);
    scheme.setFlags(QWebEngineUrlScheme::SecureScheme | QWebEngineUrlScheme::LocalScheme);
    QWebEngineUrlScheme::registerScheme(scheme);
}

bool ExtensionWebHost::active() const {
    return !sessionId_.isEmpty();
}

QString ExtensionWebHost::extensionId() const {
    return extensionId_;
}

QString ExtensionWebHost::sessionId() const {
    return sessionId_;
}

QString ExtensionWebHost::title() const {
    return title_;
}

QUrl ExtensionWebHost::pageUrl() const {
    return pageUrl_;
}

QString ExtensionWebHost::lastError() const {
    return lastError_;
}

void ExtensionWebHost::setArtifactImporters(
    ArtifactImporter roleCardImporter,
    ArtifactImporter worldbookImporter) {
    artifactRouter_->setImporters(std::move(roleCardImporter), std::move(worldbookImporter));
}

QVariantMap ExtensionWebHost::openExtension(const QString& extensionId) {
    closeSession();

    ExtensionRegistry registry(extensionRoot_);
    QString error;
    if (!registry.load(&error)) {
        setLastError(error);
        return failure(QStringLiteral("registry_load_failed"), error);
    }

    QVariantMap entry;
    for (const QVariant& value : registry.extensions()) {
        const QVariantMap candidate = value.toMap();
        if (candidate.value(QStringLiteral("id")).toString() == extensionId) {
            entry = candidate;
            break;
        }
    }
    if (entry.isEmpty()) {
        const QString message = QStringLiteral("找不到插件：%1").arg(extensionId);
        setLastError(message);
        return failure(QStringLiteral("extension_not_found"), message);
    }
    if (!entry.value(QStringLiteral("enabled")).toBool()) {
        const QString message = QStringLiteral("插件已停用，不能打开页面");
        setLastError(message);
        return failure(QStringLiteral("extension_disabled"), message);
    }
    if (!entry.value(QStringLiteral("hasPage")).toBool()) {
        const QString message = QStringLiteral("插件没有声明 Web page 入口");
        setLastError(message);
        return failure(QStringLiteral("page_entrypoint_missing"), message);
    }

    const QString packagePath = entry.value(QStringLiteral("packagePath")).toString();
    const QString packagesRoot = QDir(extensionRoot_).absoluteFilePath(QStringLiteral("packages"));
    const QString candidateRoot = QDir(extensionRoot_).absoluteFilePath(packagePath);
    const QFileInfo candidateInfo(candidateRoot);
    const QString canonicalPackageRoot = candidateInfo.canonicalFilePath();
    if (!candidateInfo.isDir() || candidateInfo.isSymLink() || candidateInfo.isJunction()
        || canonicalPackageRoot.isEmpty()
        || !ExtensionManifestParser::pathIsWithin(packagesRoot, canonicalPackageRoot)) {
        const QString message = QStringLiteral("active package 路径不安全或不存在");
        setLastError(message);
        return failure(QStringLiteral("package_path_unsafe"), message);
    }

    const ExtensionManifestResult manifestResult = ExtensionManifestParser::loadFromDirectory(canonicalPackageRoot);
    if (!manifestResult.ok || manifestResult.manifest.id != extensionId || !manifestResult.manifest.hasPage) {
        const QString message = manifestResult.ok
            ? QStringLiteral("active package manifest 与 registry 不一致")
            : manifestResult.message;
        setLastError(message);
        return failure(QStringLiteral("active_manifest_invalid"), message);
    }

    extensionId_ = manifestResult.manifest.id;
    title_ = manifestResult.manifest.name;
    packageRoot_ = canonicalPackageRoot;
    packageDigest_ = entry.value(QStringLiteral("digest")).toString();
    pageRelativePath_ = manifestResult.manifest.pagePath;
    serviceModule_ = manifestResult.manifest.serviceModule;
    serviceLockfile_ = manifestResult.manifest.serviceLockfile;
    permissions_ = manifestResult.manifest.permissions;
    artifactMediaTypes_ = manifestResult.manifest.artifactMediaTypes;
    hasService_ = manifestResult.manifest.hasService;
    sessionId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    originHost_ = QString::fromLatin1(
        QCryptographicHash::hash(extensionId_.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
    workspaceRoot_ = QDir(extensionRoot_).absoluteFilePath(
        QStringLiteral("workspaces/%1/%2").arg(extensionId_, sessionId_));
    if (!QDir().mkpath(QDir(workspaceRoot_).absoluteFilePath(QStringLiteral("input")))) {
        const QString message = QStringLiteral("无法创建插件 session workspace");
        clearSessionState();
        setLastError(message);
        emit sessionChanged();
        return failure(QStringLiteral("workspace_create_failed"), message);
    }

    pageUrl_.setScheme(QLatin1String(kScheme));
    pageUrl_.setHost(originHost_);
    pageUrl_.setPath(QStringLiteral("/package/") + pageRelativePath_);
    installBootstrapScript(&error);
    if (!error.isEmpty()) {
        clearSessionState();
        setLastError(error);
        emit sessionChanged();
        return failure(QStringLiteral("webchannel_bootstrap_failed"), error);
    }

    setLastError({});
    emit sessionChanged();
    return {
        { QStringLiteral("ok"), true },
        { QStringLiteral("message"), QStringLiteral("插件页面 session 已创建") },
        { QStringLiteral("extensionId"), extensionId_ },
        { QStringLiteral("sessionId"), sessionId_ },
        { QStringLiteral("title"), title_ },
        { QStringLiteral("pageUrl"), pageUrl_ },
    };
}

void ExtensionWebHost::closeSession(const QString& sessionId) {
    if (!sessionId.isEmpty() && sessionId != sessionId_) {
        return;
    }
    if (!active()) {
        return;
    }
    const QString closingExtensionId = extensionId_;
    const QString closingSessionId = sessionId_;
    const QString closingWorkspaceRoot = workspaceRoot_;
    serviceHost_->closeSession(closingExtensionId, closingSessionId);
    artifactRouter_->closeSession(closingExtensionId, closingSessionId);
    if (!pendingFileRequestId_.isEmpty()) {
        queueResponse(pendingFileRequestId_, false, {}, QStringLiteral("session_closed"),
            QStringLiteral("插件页面已关闭"));
    }
    clearSessionState();
    setLastError({});
    emit sessionChanged();
    scheduleWorkspaceCleanup(closingWorkspaceRoot);
}

QString ExtensionWebHost::request(
    const QString& extensionId,
    const QString& sessionId,
    const QString& method,
    const QVariantMap& params) {
    const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!requestMatchesSession(extensionId, sessionId)) {
        queueResponse(requestId, false, {}, QStringLiteral("session_invalid"),
            QStringLiteral("extension id 或 session id 已失效"));
        return requestId;
    }

    if (method == QStringLiteral("context.get")) {
        queueResponse(requestId, true, QVariantMap {
                                           { QStringLiteral("hostApi"), QStringLiteral("1.0.0") },
                                           { QStringLiteral("extensionId"), extensionId_ },
                                           { QStringLiteral("sessionId"), sessionId_ },
                                           { QStringLiteral("title"), title_ },
                                           { QStringLiteral("permissions"), permissions_ },
                                           { QStringLiteral("hasService"), hasService_ },
                                           { QStringLiteral("artifactKinds"), artifactMediaTypes_.keys() },
                                       });
        return requestId;
    }
    if (method == QStringLiteral("host.lastError")) {
        queueResponse(requestId, true, lastError_);
        return requestId;
    }
    if (method == QStringLiteral("files.pick")) {
        if (!permissions_.contains(QStringLiteral("files.user-selected.read"))) {
            queueResponse(requestId, false, {}, QStringLiteral("permission_denied"),
                QStringLiteral("插件未声明 files.user-selected.read 权限"));
            return requestId;
        }
        if (!pendingFileRequestId_.isEmpty()) {
            queueResponse(requestId, false, {}, QStringLiteral("file_picker_busy"),
                QStringLiteral("已有文件选择请求正在进行"));
            return requestId;
        }
        pendingFileRequestId_ = requestId;
        pendingAccepts_.clear();
        for (const QVariant& value : params.value(QStringLiteral("accept")).toList()) {
            const QString accept = value.toString().trimmed().toLower();
            if (!accept.isEmpty()) {
                pendingAccepts_.append(accept);
            }
        }
        const QStringList filters = fileFiltersForAccepts(params.value(QStringLiteral("accept")));
        QTimer::singleShot(0, this, [this, filters]() { emit fileSelectionRequested(filters); });
        return requestId;
    }
    if (method == QStringLiteral("service.invoke")) {
        if (!hasService_) {
            queueResponse(requestId, false, {}, QStringLiteral("service_not_declared"),
                QStringLiteral("插件没有声明 service 入口"));
        } else {
            const QString serviceMethod = params.value(QStringLiteral("method")).toString();
            const QVariant serviceParams = params.value(QStringLiteral("params"));
            if (serviceMethod.isEmpty()
                || (serviceParams.isValid() && serviceParams.metaType().id() != QMetaType::QVariantMap)) {
                queueResponse(requestId, false, {}, QStringLiteral("service_request_invalid"),
                    QStringLiteral("service.invoke 需要 method string 和 params object"));
            } else {
                serviceHost_->invoke(
                    requestId, extensionId_, sessionId_, packageRoot_, packageDigest_, serviceModule_,
                    serviceLockfile_, workspaceRoot_, QStringLiteral("zh-CN"), serviceMethod,
                    serviceParams.toMap());
            }
        }
        return requestId;
    }
    if (method == QStringLiteral("artifacts.publish")) {
        if (!permissions_.contains(QStringLiteral("artifacts.publish"))) {
            queueResponse(requestId, false, {}, QStringLiteral("permission_denied"),
                QStringLiteral("插件未声明 artifacts.publish 权限"));
        } else if (params.value(QStringLiteral("artifact")).metaType().id() != QMetaType::QVariantMap) {
            queueResponse(requestId, false, {}, QStringLiteral("artifact_envelope_invalid"),
                QStringLiteral("artifacts.publish 需要 artifact object"));
        } else {
            const ExtensionArtifactPublishResult published = artifactRouter_->publish(
                extensionId_, sessionId_, workspaceRoot_, artifactMediaTypes_,
                params.value(QStringLiteral("artifact")).toMap());
            if (!published.ok) {
                setLastError(published.message);
            } else {
                setLastError({});
            }
            queueResponse(requestId, published.ok, published.result, published.code, published.message);
        }
        return requestId;
    }
    if (method == QStringLiteral("host.close") || method == QStringLiteral("host.cancel")) {
        queueResponse(requestId, true, true);
        QTimer::singleShot(0, this, [this]() { emit closeRequested(); });
        return requestId;
    }

    queueResponse(requestId, false, {}, QStringLiteral("method_not_allowed"),
        QStringLiteral("不允许的 Extension Host 方法：%1").arg(method.left(120)));
    return requestId;
}

void ExtensionWebHost::completeFileSelection(const QUrl& selectedFile) {
    if (pendingFileRequestId_.isEmpty()) {
        return;
    }
    const QString requestId = pendingFileRequestId_;
    pendingFileRequestId_.clear();

    const QString sourcePath = selectedFile.isLocalFile() ? selectedFile.toLocalFile() : QString();
    const QFileInfo sourceInfo(sourcePath);
    if (sourcePath.isEmpty() || !sourceInfo.isFile() || sourceInfo.isSymLink() || sourceInfo.isJunction()) {
        queueResponse(requestId, false, {}, QStringLiteral("selected_file_invalid"),
            QStringLiteral("所选路径不是普通本地文件"));
        pendingAccepts_.clear();
        return;
    }
    if (sourceInfo.size() < 0 || sourceInfo.size() > kMaxSelectedFileBytes) {
        queueResponse(requestId, false, {}, QStringLiteral("selected_file_too_large"),
            QStringLiteral("所选文件超过 64 MiB 限制"));
        pendingAccepts_.clear();
        return;
    }
    if (!selectedFileMatchesAccepts(sourcePath)) {
        queueResponse(requestId, false, {}, QStringLiteral("selected_file_type_denied"),
            QStringLiteral("所选文件类型不符合插件请求"));
        pendingAccepts_.clear();
        return;
    }

    QString safeName = sourceInfo.fileName();
    safeName.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
    if (safeName.isEmpty()) {
        safeName = QStringLiteral("input.bin");
    }
    const QString relativePath = QStringLiteral("input/%1-%2")
                                     .arg(QUuid::createUuid().toString(QUuid::WithoutBraces), safeName);
    const QString destinationPath = QDir(workspaceRoot_).absoluteFilePath(relativePath);
    if (!ExtensionManifestParser::pathIsWithin(workspaceRoot_, destinationPath)
        || !QFile::copy(sourceInfo.absoluteFilePath(), destinationPath)) {
        queueResponse(requestId, false, {}, QStringLiteral("selected_file_copy_failed"),
            QStringLiteral("无法把所选文件复制到 session workspace"));
        pendingAccepts_.clear();
        return;
    }

    QUrl previewUrl;
    previewUrl.setScheme(QLatin1String(kScheme));
    previewUrl.setHost(originHost_);
    previewUrl.setPath(QStringLiteral("/workspace/") + relativePath);
    QVariantMap result {
        { QStringLiteral("name"), sourceInfo.fileName() },
        { QStringLiteral("path"), QDir::toNativeSeparators(destinationPath) },
        { QStringLiteral("size"), sourceInfo.size() },
    };
    if (sourceInfo.suffix().compare(QStringLiteral("png"), Qt::CaseInsensitive) == 0) {
        result.insert(QStringLiteral("previewUrl"), previewUrl.toString());
    }
    pendingAccepts_.clear();
    queueResponse(requestId, true, result);
}

void ExtensionWebHost::cancelFileSelection() {
    if (pendingFileRequestId_.isEmpty()) {
        return;
    }
    const QString requestId = pendingFileRequestId_;
    pendingFileRequestId_.clear();
    pendingAccepts_.clear();
    queueResponse(requestId, true, QVariant());
}

bool ExtensionWebHost::isAllowedNavigation(const QUrl& url) const {
    if (url == QUrl(QStringLiteral("about:blank"))) {
        return true;
    }
    return active() && url.scheme() == QLatin1String(kScheme) && url.host() == originHost_
        && !resolveResourcePath(url, nullptr, nullptr).isEmpty();
}

void ExtensionWebHost::reportPageLoadFailure(const QString& sessionId, const QString& message) {
    if (active() && sessionId == sessionId_) {
        setLastError(QStringLiteral("插件页面加载失败：%1").arg(message.left(500)));
    }
}

bool ExtensionWebHost::isAllowedRequest(const QUrl& url, const QUrl& firstPartyUrl, int resourceType) const {
    if (url == QUrl(QStringLiteral("about:blank"))) {
        return true;
    }
    if (active() && url.scheme() == QLatin1String(kScheme) && url.host() == originHost_) {
        return !resolveResourcePath(url, nullptr, nullptr).isEmpty();
    }
    const bool embeddedData = url.scheme() == QStringLiteral("data") || url.scheme() == QStringLiteral("blob");
    const auto type = static_cast<QWebEngineUrlRequestInfo::ResourceType>(resourceType);
    return embeddedData && active() && firstPartyUrl.scheme() == QLatin1String(kScheme)
        && firstPartyUrl.host() == originHost_
        && type != QWebEngineUrlRequestInfo::ResourceTypeMainFrame
        && type != QWebEngineUrlRequestInfo::ResourceTypeSubFrame;
}

QString ExtensionWebHost::resolveResourcePath(const QUrl& url, QByteArray* mimeType, bool* injectCsp) const {
    if (!active() || url.scheme() != QLatin1String(kScheme) || url.host() != originHost_
        || url.hasQuery() || url.hasFragment()) {
        return {};
    }
    QString root;
    QString relativePath;
    const QString decodedPath = url.path(QUrl::FullyDecoded);
    if (decodedPath.startsWith(QStringLiteral("/package/"))) {
        root = packageRoot_;
        relativePath = decodedPath.mid(QStringLiteral("/package/").size());
    } else if (decodedPath.startsWith(QStringLiteral("/workspace/"))) {
        root = workspaceRoot_;
        relativePath = decodedPath.mid(QStringLiteral("/workspace/").size());
    } else {
        return {};
    }
    QString normalized;
    if (!ExtensionManifestParser::isSafeRelativePath(relativePath, &normalized)) {
        return {};
    }
    const QString candidate = QDir(root).absoluteFilePath(normalized);
    const QFileInfo info(candidate);
    if (!info.isFile() || info.isSymLink() || info.isJunction()
        || !ExtensionManifestParser::pathIsWithin(root, candidate)) {
        return {};
    }
    if (mimeType) {
        *mimeType = QMimeDatabase().mimeTypeForFile(info).name().toUtf8();
    }
    if (injectCsp) {
        *injectCsp = info.suffix().compare(QStringLiteral("html"), Qt::CaseInsensitive) == 0
            || info.suffix().compare(QStringLiteral("htm"), Qt::CaseInsensitive) == 0;
    }
    return info.canonicalFilePath();
}

void ExtensionWebHost::configureProfile() {
    QDir().mkpath(QDir(extensionRoot_).absoluteFilePath(QStringLiteral("browser/profile")));
    QDir().mkpath(QDir(extensionRoot_).absoluteFilePath(QStringLiteral("browser/cache")));
    QWebEngineProfile* profile = QWebEngineProfile::defaultProfile();
    profile->setPersistentStoragePath(QDir(extensionRoot_).absoluteFilePath(QStringLiteral("browser/profile")));
    profile->setCachePath(QDir(extensionRoot_).absoluteFilePath(QStringLiteral("browser/cache")));
    profile->setHttpCacheType(QWebEngineProfile::NoCache);
    profile->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
    profile->setSpellCheckEnabled(false);
    profile->setPushServiceEnabled(false);
    profile->setHttpAcceptLanguage(QStringLiteral("zh-CN,en-US;q=0.8"));

    QWebEngineSettings* settings = profile->settings();
    settings->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
    settings->setAttribute(QWebEngineSettings::LocalStorageEnabled, true);
    settings->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
    settings->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
    settings->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
    settings->setAttribute(QWebEngineSettings::JavascriptCanAccessClipboard, false);
    settings->setAttribute(QWebEngineSettings::PluginsEnabled, false);
    settings->setAttribute(QWebEngineSettings::FullScreenSupportEnabled, false);
    settings->setAttribute(QWebEngineSettings::ScreenCaptureEnabled, false);
    settings->setAttribute(QWebEngineSettings::AllowRunningInsecureContent, false);
    settings->setAttribute(QWebEngineSettings::PdfViewerEnabled, false);
    settings->setUnknownUrlSchemePolicy(QWebEngineSettings::DisallowUnknownUrlSchemes);

    requestInterceptor_ = new ExtensionRequestInterceptor(this);
    schemeHandler_ = new ExtensionSchemeHandler(this);
    profile->setUrlRequestInterceptor(static_cast<QWebEngineUrlRequestInterceptor*>(requestInterceptor_));
    profile->installUrlSchemeHandler(QByteArray(kScheme),
        static_cast<QWebEngineUrlSchemeHandler*>(schemeHandler_));
    connect(profile, &QWebEngineProfile::downloadRequested, this,
        [](QWebEngineDownloadRequest* download) { download->cancel(); });
}

bool ExtensionWebHost::requestMatchesSession(
    const QString& extensionId,
    const QString& sessionId) const {
    return active() && extensionId == extensionId_ && sessionId == sessionId_;
}

void ExtensionWebHost::queueResponse(
    const QString& requestId,
    bool ok,
    const QVariant& result,
    const QString& errorCode,
    const QString& errorMessage) {
    QTimer::singleShot(0, this, [this, requestId, ok, result, errorCode, errorMessage]() {
        emit responseReady(requestId, ok, result, errorCode, errorMessage);
    });
}

void ExtensionWebHost::setLastError(const QString& error) {
    if (lastError_ == error) {
        return;
    }
    lastError_ = error;
    emit lastErrorChanged();
}

QStringList ExtensionWebHost::fileFiltersForAccepts(const QVariant& accepts) const {
    QStringList suffixes;
    for (const QVariant& value : accepts.toList()) {
        const QString accept = value.toString().trimmed().toLower();
        if (accept == QStringLiteral("image/png")) {
            suffixes.append(QStringLiteral("*.png"));
        } else if (accept == QStringLiteral("application/json")) {
            suffixes.append({ QStringLiteral("*.json"), QStringLiteral("*.jsonc") });
        } else if (accept.startsWith(QLatin1Char('.'))
            && QRegularExpression(QStringLiteral("^\\.[a-z0-9]{1,12}$")).match(accept).hasMatch()) {
            suffixes.append(QStringLiteral("*") + accept);
        }
    }
    suffixes.removeDuplicates();
    if (suffixes.isEmpty()) {
        return { QStringLiteral("支持的输入文件 (*.png *.json *.jsonc)") };
    }
    return { QStringLiteral("插件请求的文件 (%1)").arg(suffixes.join(QLatin1Char(' '))) };
}

bool ExtensionWebHost::selectedFileMatchesAccepts(const QString& path) const {
    if (pendingAccepts_.isEmpty()) {
        return true;
    }
    const QString suffix = normalizedSuffix(QFileInfo(path).suffix());
    for (const QString& accept : pendingAccepts_) {
        if (accept == QStringLiteral("image/png") && suffix == QStringLiteral(".png")) {
            return true;
        }
        if (accept == QStringLiteral("application/json")
            && (suffix == QStringLiteral(".json") || suffix == QStringLiteral(".jsonc"))) {
            return true;
        }
        if (accept.startsWith(QLatin1Char('.')) && suffix == accept) {
            return true;
        }
    }
    return false;
}

QString ExtensionWebHost::bootstrapSource(QString* error) const {
    QFile webChannelScript(QStringLiteral(":/qtwebchannel/qwebchannel.js"));
    if (!webChannelScript.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("无法读取 Qt WebChannel bootstrap resource");
        }
        return {};
    }
    const QString bridge = QString::fromUtf8(webChannelScript.readAll());
    const QString bootstrap = QStringLiteral(R"JS(
(() => {
  "use strict";
  const extensionId = %1;
  const sessionId = %2;
  const pending = new Map();
  const ready = new Promise((resolve, reject) => {
    if (!window.qt || !qt.webChannelTransport) {
      reject(new Error("Fantareal WebChannel transport unavailable"));
      return;
    }
    new QWebChannel(qt.webChannelTransport, (channel) => {
      const bridge = channel.objects.%3;
      if (!bridge) {
        reject(new Error("Fantareal Extension Host bridge unavailable"));
        return;
      }
      bridge.responseReady.connect((requestId, ok, result, errorCode, errorMessage) => {
        const waiter = pending.get(requestId);
        if (!waiter) return;
        pending.delete(requestId);
        if (ok) waiter.resolve(result);
        else {
          const error = new Error(errorMessage || errorCode || "Extension Host request failed");
          error.code = errorCode || "extension_host_error";
          waiter.reject(error);
        }
      });
      resolve(bridge);
    });
  });
  const call = (method, params = {}) => ready.then((bridge) => new Promise((resolve, reject) => {
    const requestId = bridge.request(extensionId, sessionId, method, params);
    if (!requestId) {
      reject(new Error("Extension Host did not create a request"));
      return;
    }
    pending.set(requestId, { resolve, reject });
  }));
  const host = Object.freeze({
    getContext: () => call("context.get"),
    getLastError: () => call("host.lastError"),
    pickInput: (options = {}) => call("files.pick", options),
    invoke: (method, params = {}) => call("service.invoke", { method, params }),
    publishArtifact: (artifact) => call("artifacts.publish", { artifact }),
    close: () => call("host.close"),
    cancel: () => call("host.cancel")
  });
  Object.defineProperty(window, "fantarealExtension", {
    value: host,
    writable: false,
    configurable: false,
    enumerable: true
  });
})();
)JS")
                                  .arg(javascriptString(extensionId_), javascriptString(sessionId_),
                                      QLatin1String(kBridgeObjectName));
    return bridge + QLatin1Char('\n') + bootstrap;
}

void ExtensionWebHost::installBootstrapScript(QString* error) {
    QWebEngineScriptCollection* scripts = QWebEngineProfile::defaultProfile()->scripts();
    for (const QWebEngineScript& script : scripts->find(QLatin1String(kBootstrapScriptName))) {
        scripts->remove(script);
    }
    const QString source = bootstrapSource(error);
    if (source.isEmpty()) {
        return;
    }
    QWebEngineScript script;
    script.setName(QLatin1String(kBootstrapScriptName));
    script.setInjectionPoint(QWebEngineScript::DocumentCreation);
    script.setWorldId(QWebEngineScript::MainWorld);
    script.setRunsOnSubFrames(false);
    script.setSourceCode(source);
    scripts->insert(script);
}

void ExtensionWebHost::scheduleWorkspaceCleanup(const QString& workspacePath) {
    if (workspacePath.isEmpty()) {
        return;
    }
    const QString workspacesRoot = QDir(extensionRoot_).absoluteFilePath(QStringLiteral("workspaces"));
    const QFileInfo workspaceInfo(workspacePath);
    if (!workspaceInfo.isDir() || workspaceInfo.isSymLink() || workspaceInfo.isJunction()
        || !ExtensionManifestParser::pathIsWithin(workspacesRoot, workspaceInfo.absoluteFilePath())) {
        setLastError(QStringLiteral("已拒绝清理不安全的 session workspace"));
        return;
    }
    QTimer::singleShot(1500, this, [this, workspacePath]() {
        QDir workspace(workspacePath);
        if (workspace.exists() && workspaceTreeContainsLink(workspacePath)) {
            setLastError(QStringLiteral("session workspace 含链接，已拒绝自动清理"));
        } else if (workspace.exists() && !workspace.removeRecursively()) {
            setLastError(QStringLiteral("session workspace 临时文件清理失败，将在后续重试"));
        }
    });
}

void ExtensionWebHost::clearSessionState() {
    extensionId_.clear();
    sessionId_.clear();
    title_.clear();
    originHost_.clear();
    packageRoot_.clear();
    packageDigest_.clear();
    workspaceRoot_.clear();
    pageRelativePath_.clear();
    serviceModule_.clear();
    serviceLockfile_.clear();
    permissions_.clear();
    artifactMediaTypes_.clear();
    hasService_ = false;
    pageUrl_ = {};
    pendingFileRequestId_.clear();
    pendingAccepts_.clear();
}
