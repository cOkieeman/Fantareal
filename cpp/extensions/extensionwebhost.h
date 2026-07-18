#pragma once

#include <QObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariant>
#include <QVariantMap>

#include <functional>

class ExtensionArtifactRouter;
class ExtensionServiceHost;

class ExtensionWebHost final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY sessionChanged)
    Q_PROPERTY(QString extensionId READ extensionId NOTIFY sessionChanged)
    Q_PROPERTY(QString sessionId READ sessionId NOTIFY sessionChanged)
    Q_PROPERTY(QString title READ title NOTIFY sessionChanged)
    Q_PROPERTY(QUrl pageUrl READ pageUrl NOTIFY sessionChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    using ArtifactImporter = std::function<QVariantMap(const QString& absolutePath)>;

    explicit ExtensionWebHost(QObject* parent = nullptr);
    explicit ExtensionWebHost(const QString& extensionRoot, QObject* parent = nullptr);
    ~ExtensionWebHost() override;

    static void registerUrlScheme();

    bool active() const;
    QString extensionId() const;
    QString sessionId() const;
    QString title() const;
    QUrl pageUrl() const;
    QString lastError() const;

    void setArtifactImporters(ArtifactImporter roleCardImporter, ArtifactImporter worldbookImporter);

    Q_INVOKABLE QVariantMap openExtension(const QString& extensionId);
    Q_INVOKABLE void closeSession(const QString& sessionId = {});
    Q_INVOKABLE QString request(
        const QString& extensionId,
        const QString& sessionId,
        const QString& method,
        const QVariantMap& params = {});
    Q_INVOKABLE void completeFileSelection(const QUrl& selectedFile);
    Q_INVOKABLE void cancelFileSelection();
    Q_INVOKABLE bool isAllowedNavigation(const QUrl& url) const;
    Q_INVOKABLE void reportPageLoadFailure(const QString& sessionId, const QString& message);

    bool isAllowedRequest(const QUrl& url, const QUrl& firstPartyUrl, int resourceType) const;
    QString resolveResourcePath(const QUrl& url, QByteArray* mimeType, bool* injectCsp) const;

signals:
    void sessionChanged();
    void lastErrorChanged();
    void responseReady(
        const QString& requestId,
        bool ok,
        const QVariant& result,
        const QString& errorCode,
        const QString& errorMessage);
    void fileSelectionRequested(const QStringList& nameFilters);
    void closeRequested();

private:
    void configureProfile();
    bool requestMatchesSession(const QString& extensionId, const QString& sessionId) const;
    void queueResponse(
        const QString& requestId,
        bool ok,
        const QVariant& result = {},
        const QString& errorCode = {},
        const QString& errorMessage = {});
    void setLastError(const QString& error);
    QStringList fileFiltersForAccepts(const QVariant& accepts) const;
    bool selectedFileMatchesAccepts(const QString& path) const;
    QString bootstrapSource(QString* error) const;
    void installBootstrapScript(QString* error);
    void scheduleWorkspaceCleanup(const QString& workspacePath);
    void clearSessionState();

    QString extensionRoot_;
    QString extensionId_;
    QString sessionId_;
    QString title_;
    QString originHost_;
    QString packageRoot_;
    QString packageDigest_;
    QString workspaceRoot_;
    QString pageRelativePath_;
    QString serviceModule_;
    QString serviceLockfile_;
    QStringList permissions_;
    QMap<QString, QStringList> artifactMediaTypes_;
    bool hasService_ = false;
    QUrl pageUrl_;
    QString lastError_;
    QString pendingFileRequestId_;
    QStringList pendingAccepts_;
    QObject* requestInterceptor_ = nullptr;
    QObject* schemeHandler_ = nullptr;
    ExtensionArtifactRouter* artifactRouter_ = nullptr;
    ExtensionServiceHost* serviceHost_ = nullptr;
};
