// StreamClient.cpp -- see StreamClient.h.
#include "StreamClient.h"

QString shellQuote(const QString &s)
{
    QString q = s;
    q.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + q + QLatin1Char('\'');
}

StreamClient::StreamClient(QObject *parent) : QObject(parent)
{
    connect(&proc_, &QProcess::readyReadStandardOutput, this, &StreamClient::onStdout);
    connect(&proc_, &QProcess::readyReadStandardError, this, &StreamClient::onStderr);
    connect(&proc_, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &StreamClient::onFinished);
    connect(&proc_, &QProcess::errorOccurred, this, &StreamClient::onError);
}

StreamClient::~StreamClient()
{
    // Nothing may be emitted into a window that is already half destroyed.
    disconnect(&proc_, nullptr, this, nullptr);
    stop();
}

void StreamClient::start(const Options &o)
{
    QStringList margs;
    margs << o.base << o.monitorArgs << QStringLiteral("--stream");

    if (o.sshHost.isEmpty()) {
        program_ = o.monitor;
        proc_.start(program_, margs);
        return;
    }
    QStringList words{QStringLiteral("exec"), shellQuote(o.monitor)};
    for (const QString &a : margs) words << shellQuote(a);
    QString cmd = words.join(QLatin1Char(' '));
    if (!o.remoteDir.isEmpty())
        cmd = QStringLiteral("cd ") + shellQuote(o.remoteDir) + QStringLiteral(" && ") + cmd;
    // -T: no terminal, so nothing but the monitor's own bytes comes back.
    // BatchMode: fail at once rather than hang on a password prompt that no
    // one can see from inside a GUI.
    program_ = QStringLiteral("ssh");
    proc_.start(program_, {QStringLiteral("-T"), QStringLiteral("-o"),
                           QStringLiteral("BatchMode=yes"), o.sshHost, cmd});
}

void StreamClient::stop()
{
    if (proc_.state() == QProcess::NotRunning) return;
    // SIGTERM: the monitor writes its exit record and joins its threads.
    // Over ssh, closing the channel closes the monitor's stdout, and its next
    // write fails -- it treats that as its reader going away and stops.
    proc_.terminate();
    if (!proc_.waitForFinished(2000)) {
        proc_.kill();
        proc_.waitForFinished(1000);
    }
}

void StreamClient::onStdout()
{
    buf_.append(proc_.readAllStandardOutput());
    int nl;
    while ((nl = buf_.indexOf('\n')) >= 0) {
        const QByteArray line = buf_.left(nl);
        buf_.remove(0, nl + 1);
        if (!line.trimmed().isEmpty()) handleLine(line);
    }
}

void StreamClient::onStderr()
{
    const QList<QByteArray> lines = proc_.readAllStandardError().split('\n');
    for (const QByteArray &l : lines)
        if (!l.trimmed().isEmpty()) lastStderr_ = QString::fromLocal8Bit(l.trimmed());
}

void StreamClient::handleLine(const QByteArray &line)
{
    StreamInit in;
    StreamTick t;
    StreamExit x;
    QString err;
    switch (parseLine(line, haveInit_ ? &init_ : nullptr, &in, &t, &x, &err)) {
    case LineKind::Init:
        init_ = in;
        haveInit_ = true;
        emit initReceived(init_);
        break;
    case LineKind::Tick:
        emit tickReceived(t);
        break;
    case LineKind::Exit:
        exit_ = x;
        sawExit_ = true;
        break;
    case LineKind::Invalid:
        ++parseErrors_;
        break;
    }
}

void StreamClient::onFinished(int code, QProcess::ExitStatus status)
{
    onStdout();                                  // whatever is still buffered
    if (sawExit_)
        emit finished(exit_.code, exit_.verdict, exit_.note);
    else if (status == QProcess::CrashExit)
        emit failed(tr("his_monitor가 비정상 종료했습니다. %1").arg(lastStderr_));
    else if (!haveInit_)
        emit failed(lastStderr_.isEmpty()
                        ? tr("his_monitor가 아무것도 보내지 않고 종료했습니다 (코드 %1)").arg(code)
                        : lastStderr_);
    else
        emit finished(code, QString(), lastStderr_);
}

void StreamClient::onError(QProcess::ProcessError e)
{
    if (e == QProcess::FailedToStart)
        emit failed(tr("%1 을(를) 실행하지 못했습니다: %2").arg(program_, proc_.errorString()));
}
