#pragma once

#include <QCryptographicHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QProcess>
#include <QQueue>
#include <QSaveFile>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <memory>

class QNetworkReply;

class PythonRuntimeManager final : public QObject {
    Q_OBJECT

public:
    explicit PythonRuntimeManager(QString extensionRoot, QObject* parent = nullptr);

    QString ensureEnvironment(
        const QString& extensionId,
        const QString& packageRoot,
        const QString& packageDigest,
        const QString& lockfileRelativePath);
    void cancel(const QString& requestId);

    static QString uvVersion();
    static QString pythonVersion();
    static QString uvArchiveUrl();
    static QString uvArchiveSha256();

signals:
    void environmentReady(const QString& requestId, const QString& pythonExecutable);
    void environmentFailed(
        const QString& requestId,
        const QString& errorCode,
        const QString& errorMessage);
    void runtimeStatusChanged(const QString& status, const QString& detail);

private:
    struct Request {
        QString id;
        QString extensionId;
        QString packageRoot;
        QString packageDigest;
        QString lockfileRelativePath;
        QString environmentRoot;
    };

    enum class Stage {
        Idle,
        DownloadingUv,
        InstallingPython,
        SyncingEnvironment,
    };

    void startNext();
    bool validateRequest(Request* request, QString* code, QString* error) const;
    void ensureUv();
    void startUvDownload(const QUrl& url, int redirectCount);
    void handleDownloadReadyRead();
    void handleDownloadFinished();
    bool installUvFromArchive(const QString& archivePath, QString* error);
    void ensurePython();
    void syncEnvironment();
    void startProcess(Stage stage, const QStringList& arguments, int timeoutMs);
    void handleProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void handleProcessError(QProcess::ProcessError error);
    void failCurrent(const QString& code, const QString& error);
    void completeCurrent(const QString& pythonExecutable);
    void resetDownload();
    void resetProcess();

    QString runtimesRoot() const;
    QString uvExecutable() const;
    QString pythonInstallRoot() const;
    QString pythonExecutable() const;
    QString markerPath(const Request& request) const;
    bool environmentIsReady(const Request& request) const;
    bool writeEnvironmentMarker(const Request& request, QString* error) const;

    QString extensionRoot_;
    QQueue<Request> queue_;
    Request current_;
    Stage stage_ = Stage::Idle;

    QNetworkAccessManager network_;
    QNetworkReply* reply_ = nullptr;
    std::unique_ptr<QSaveFile> downloadFile_;
    std::unique_ptr<QCryptographicHash> downloadHash_;
    QString downloadArchivePath_;
    qint64 downloadedBytes_ = 0;
    int redirectCount_ = 0;

    QProcess process_;
    QTimer processTimer_;
    bool handlingProcessFailure_ = false;
};
