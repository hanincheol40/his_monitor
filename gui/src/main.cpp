// main.cpp -- his_gui: a Qt window on `his_monitor --stream`.
//
//   his_gui <base> [his_monitor options] [gui options]
//
// Anything that is not a GUI option is handed to his_monitor unchanged, so
// the monitor's options work here exactly as they do in the terminal.

#include "MainWindow.h"
#include "Protocol.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

#include <cstdio>
#include <cstring>

namespace {

void usage()
{
    std::fputs(
        "his_gui - a Qt window on his_monitor --stream\n"
        "\n"
        "usage: his_gui <base> [his_monitor options] [options]\n"
        "  <base>                simulation base name, as his_monitor takes it\n"
        "                        (e.g. sim_1, or /path/to/sim_1)\n"
        "\n"
        "  --monitor <path>      his_monitor to run. Default: next to this program,\n"
        "                        then ../his_monitor, ../../his_monitor, then PATH\n"
        "  --ssh <host>          run the monitor on <host> over ssh (key auth);\n"
        "                        the window stays here\n"
        "  --dir <path>          working directory on that host, with --ssh\n"
        "  --capture <dir>       save the window as a PNG every --every seconds\n"
        "  --every <s>           capture interval, default 10\n"
        "  --scale <n>           capture pixel scale, default 2 (crisp on slides)\n"
        "  --quit-after <s>      close after this many seconds\n"
        "  --check-stream <file> parse a saved --stream capture with the window's\n"
        "                        own parser, report, and exit 0 if it is sound.\n"
        "                        Needs no display\n"
        "\n"
        "Everything else goes to his_monitor: --names, --from-start, --period,\n"
        "--threads, --pwv-target, --tol, --umax, --stall, --abort-on-fail ...\n",
        stdout);
}

// his_monitor options that take a value, so the value is not mistaken for
// the base name when the options come first.
bool monitorTakesValue(const QString &a)
{
    static const QStringList v{QStringLiteral("--names"), QStringLiteral("--period"),
                               QStringLiteral("--threads"), QStringLiteral("--pwv-target"),
                               QStringLiteral("--tol"), QStringLiteral("--umax"),
                               QStringLiteral("--stall")};
    return v.contains(a);
}

QString findMonitor()
{
    const QString here = QCoreApplication::applicationDirPath();
    for (const QString &c : {here + QStringLiteral("/his_monitor"),
                             here + QStringLiteral("/../his_monitor"),
                             here + QStringLiteral("/../../his_monitor")}) {
        const QFileInfo fi(c);
        if (fi.isFile() && fi.isExecutable()) return fi.canonicalFilePath();
    }
    const QString inPath = QStandardPaths::findExecutable(QStringLiteral("his_monitor"));
    return inPath.isEmpty() ? QStringLiteral("his_monitor") : inPath;
}

// The same parser the window uses, over a saved stream. CI runs this against
// real monitor output, so a change on either side of the protocol that the
// other does not follow fails the build instead of drawing nonsense.
int checkStream(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "his_gui: cannot read %s\n", qPrintable(path));
        return 1;
    }
    StreamInit init;
    bool haveInit = false;
    int nInit = 0, nTick = 0, nExit = 0, nBad = 0, lineNo = 0;
    long samples = 0;
    while (!f.atEnd()) {
        const QByteArray line = f.readLine().trimmed();
        ++lineNo;
        if (line.isEmpty()) continue;
        StreamInit in;
        StreamTick t;
        StreamExit x;
        QString err;
        switch (parseLine(line, haveInit ? &init : nullptr, &in, &t, &x, &err)) {
        case LineKind::Init: init = in; haveInit = true; ++nInit; break;
        case LineKind::Tick:
            ++nTick;
            for (const auto &h : t.hist) samples += h.size();
            break;
        case LineKind::Exit: ++nExit; break;
        case LineKind::Invalid:
            ++nBad;
            std::fprintf(stderr, "  line %d: %s\n", lineNo, qPrintable(err));
            break;
        }
    }
    std::printf("%s: init %d, tick %d, exit %d, invalid %d; %d domains, %ld waveform samples\n",
                qPrintable(path), nInit, nTick, nExit, nBad, init.ndom, samples);
    return (nInit == 1 && nTick > 0 && nBad == 0) ? 0 : 1;
}

}  // namespace

int main(int argc, char **argv)
{
    // --check-stream needs no display: decide before any QApplication exists.
    for (int i = 1; i + 1 < argc; ++i) {
        if (!std::strcmp(argv[i], "--check-stream")) {
            QCoreApplication app(argc, argv);
            return checkStream(QString::fromLocal8Bit(argv[i + 1]));
        }
    }
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], "-h") || !std::strcmp(argv[i], "--help")) { usage(); return 0; }

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("his_gui"));

    MainWindow::Options o;
    StreamClient::Options &s = o.stream;
    const QStringList a = app.arguments();
    for (int i = 1; i < a.size(); ++i) {
        const QString &x = a[i];
        const bool hasNext = i + 1 < a.size();
        if      (x == QLatin1String("--monitor")    && hasNext) s.monitor = a[++i];
        else if (x == QLatin1String("--ssh")        && hasNext) s.sshHost = a[++i];
        else if (x == QLatin1String("--dir")        && hasNext) s.remoteDir = a[++i];
        else if (x == QLatin1String("--capture")    && hasNext) o.captureDir = a[++i];
        else if (x == QLatin1String("--every")      && hasNext) o.captureEvery = a[++i].toDouble();
        else if (x == QLatin1String("--scale")      && hasNext) o.captureScale = a[++i].toDouble();
        else if (x == QLatin1String("--quit-after") && hasNext) o.quitAfter = a[++i].toDouble();
        else if (monitorTakesValue(x) && hasNext) s.monitorArgs << x << a[++i];
        else if (s.base.isEmpty() && !x.startsWith(QLatin1Char('-'))) s.base = x;
        else s.monitorArgs << x;
    }
    if (s.base.isEmpty()) {
        usage();
        return 1;
    }
    if (s.monitor.isEmpty())
        s.monitor = s.sshHost.isEmpty() ? findMonitor() : QStringLiteral("his_monitor");

    MainWindow w(o);
    w.show();
    return app.exec();
}
