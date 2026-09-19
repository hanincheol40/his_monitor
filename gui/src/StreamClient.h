#pragma once
// StreamClient.h -- runs `his_monitor --stream` and turns its lines into
// signals.
//
// The monitor stays a separate process on purpose. It is the part with the
// threads, the file tailing and the verdict -- all tested, all run under the
// sanitizers in CI -- and the GUI reimplements none of it; it draws what the
// monitor says. With --ssh the same monitor runs on the server and the lines
// come back through the ssh channel unchanged.

#include "Protocol.h"

#include <QObject>
#include <QProcess>
#include <QStringList>

// Single-quote a word for a POSIX shell, for the command sent over ssh.
QString shellQuote(const QString &s);

class StreamClient : public QObject {
    Q_OBJECT
public:
    struct Options {
        QString monitor;          // his_monitor: local path, or name on the host
        QString base;             // simulation base name, as his_monitor takes it
        QStringList monitorArgs;  // passed through unchanged
        QString sshHost;          // empty: run locally
        QString remoteDir;        // working directory on the host, with --ssh
    };

    explicit StreamClient(QObject *parent = nullptr);
    ~StreamClient() override;

    void start(const Options &o);
    void stop();
    qint64 monitorPid() const { return proc_.processId(); }
    int parseErrors() const { return parseErrors_; }

signals:
    void initReceived(const StreamInit &init);
    void tickReceived(const StreamTick &tick);
    void finished(int code, const QString &verdict, const QString &note);
    void failed(const QString &why);

private:
    void onStdout();
    void onStderr();
    void onFinished(int code, QProcess::ExitStatus status);
    void onError(QProcess::ProcessError e);
    void handleLine(const QByteArray &line);

    QProcess proc_;
    QByteArray buf_;
    QString program_;
    StreamInit init_;
    bool haveInit_ = false;
    bool sawExit_ = false;
    StreamExit exit_;
    QString lastStderr_;
    int parseErrors_ = 0;
};
