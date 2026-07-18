#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

class ExtensionInstaller;
class QFile;
class QNetworkAccessManager;
class QNetworkReply;
class QUrl;

struct GitHubRepositorySpec {
    bool ok{};
    QString owner;
    QString repository;
    QString ref;
    QString sourceUrl;
    QString code;
    QString message;
};

class GitHubExtensionSource : public QObject {
    Q_OBJECT

public:
    explicit GitHubExtensionSource(
        ExtensionInstaller& installer,
        QString extensionRoot,
        QObject* parent = nullptr);

    bool busy() const;
    QVariantMap install(const QString& repositoryUrl);

    static GitHubRepositorySpec parseRepositoryUrl(const QString& repositoryUrl);
    static QVariantMap extractArchive(
        const QString& archivePath,
        const QString& destinationRoot);

signals:
    void busyChanged();
    void finished(const QVariantMap& result);

private:
    enum class Phase {
        None,
        Repository,
        Commit,
        Archive,
    };

    void startRepositoryRequest();
    void startCommitRequest(const QString& ref);
    void startArchiveRequest();
    void startRequest(const QUrl& url, Phase phase, int redirectCount = 0);
    void handleReadyRead();
    void handleReplyFinished();
    void handleRepositoryPayload(const QByteArray& payload);
    void handleCommitPayload(const QByteArray& payload);
    void handleArchiveComplete();
    void fail(const QString& code, const QString& message);
    void complete(const QVariantMap& result);
    void setBusy(bool busy);
    void cleanupStaging();
    bool prepareArchiveFile(QString* error);
    bool redirectAllowed(const QUrl& target) const;
    static QVariantMap result(bool ok, const QString& message, const QString& code = {});

    ExtensionInstaller& installer_;
    QString extensionRoot_;
    QNetworkAccessManager* network_{};
    QNetworkReply* reply_{};
    QFile* archiveFile_{};
    GitHubRepositorySpec spec_;
    QString resolvedCommit_;
    QString stagingRoot_;
    QString archivePath_;
    Phase phase_{Phase::None};
    int redirectCount_{};
    qint64 receivedBytes_{};
    bool archiveWriteFailed_{};
    bool busy_{};
};
