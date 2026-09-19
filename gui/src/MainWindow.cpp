// MainWindow.cpp -- see MainWindow.h.
#include "MainWindow.h"
#include "FieldView.h"
#include "Palette.h"
#include "TrendView.h"
#include "WaveView.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>

namespace {

QLabel *makeLabel(const QFont &f, const QColor &c, QWidget *parent)
{
    auto *l = new QLabel(parent);
    l->setFont(f);
    l->setStyleSheet(QStringLiteral("color:%1;").arg(Pal::css(c)));
    return l;
}

QWidget *hairline(QWidget *parent)
{
    auto *w = new QWidget(parent);
    w->setFixedHeight(1);
    w->setStyleSheet(QStringLiteral("background:%1;").arg(Pal::css(Pal::line())));
    return w;
}

// One of the four numbers: a caption over a large value with its unit.
QLabel *makeMetric(QWidget *parent, QGridLayout *grid, int row, int col, const QString &caption)
{
    auto *cap = makeLabel(Pal::font(12), Pal::muted(), parent);
    cap->setText(caption);
    auto *val = new QLabel(parent);
    val->setFont(Pal::font(24, true));
    val->setTextFormat(Qt::RichText);
    val->setText(QStringLiteral("–"));
    auto *box = new QVBoxLayout;
    box->setSpacing(2);
    box->addWidget(cap);
    box->addWidget(val);
    grid->addLayout(box, row, col);
    return val;
}

void setMetric(QLabel *l, const QString &value, const QString &unit)
{
    l->setText(QStringLiteral("<span style='color:%1'>%2</span>"
                              "<span style='color:%3;font-size:13px;font-weight:400'>&nbsp;%4</span>")
                   .arg(Pal::css(Pal::text()), value.toHtmlEscaped(),
                        Pal::css(Pal::muted()), unit));
}

QString fmt(double v, int prec)
{
    return std::isfinite(v) ? QString::number(v, 'f', prec) : QStringLiteral("–");
}

// The verdict as the window shows it. The monitor's own word stays in the
// tooltip, so the badge can always be matched against the terminal and logs.
QString verdictText(const QString &v)
{
    if (v == QLatin1String("CONVERGED"))  return QObject::tr("수렴 완료");
    if (v == QLatin1String("CONVERGING")) return QObject::tr("수렴 중");
    if (v == QLatin1String("FILLING"))    return QObject::tr("채우는 중");
    if (v == QLatin1String("OFF TARGET")) return QObject::tr("목표 이탈");
    if (v == QLatin1String("STALLED"))    return QObject::tr("멈춤");
    return QObject::tr("대기");
}

}  // namespace

MainWindow::MainWindow(const Options &o, QWidget *parent)
    : QWidget(parent), opt_(o), client_(new StreamClient(this))
{
    setWindowTitle(QStringLiteral("his_monitor"));
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, Pal::bg());
    setPalette(pal);
    resize(1280, 720);
    setMinimumSize(1100, 640);

    // ---- header: what is running, how far along, and the verdict
    auto *title = makeLabel(Pal::font(20, true), Pal::text(), this);
    title->setText(QStringLiteral("his_monitor"));
    baseLabel_ = makeLabel(Pal::font(14), Pal::muted(), this);
    simLabel_  = makeLabel(Pal::font(14), Pal::text(), this);
    progress_  = new QProgressBar(this);
    progress_->setRange(0, 1000);
    progress_->setTextVisible(false);
    progress_->setFixedSize(180, 8);
    progress_->setStyleSheet(QStringLiteral(
        "QProgressBar{border:none;background:%1;border-radius:4px;}"
        "QProgressBar::chunk{background:%2;border-radius:4px;}")
        .arg(Pal::css(Pal::grid()), Pal::css(Pal::accent())));
    cycleLabel_ = makeLabel(Pal::font(14), Pal::text(), this);
    etaLabel_   = makeLabel(Pal::font(14), Pal::muted(), this);
    verdictLabel_ = new QLabel(this);
    verdictLabel_->setFont(Pal::font(14, true));
    verdictLabel_->setAlignment(Qt::AlignCenter);
    verdictLabel_->setMinimumWidth(104);
    stopButton_ = new QPushButton(tr("솔버 중단"), this);
    stopButton_->setFont(Pal::font(13));
    stopButton_->setCursor(Qt::PointingHandCursor);
    stopButton_->setEnabled(false);
    stopButton_->setStyleSheet(QStringLiteral(
        "QPushButton{color:%1;background:%2;border:1px solid %3;border-radius:6px;padding:6px 14px;}"
        "QPushButton:hover{color:%4;border-color:%4;}"
        "QPushButton:disabled{color:%3;}")
        .arg(Pal::css(Pal::text()), Pal::css(Pal::bg()), Pal::css(Pal::line()),
             Pal::css(Pal::fault())));
    noteLabel_ = makeLabel(Pal::font(13), Pal::muted(), this);

    auto *row1 = new QHBoxLayout;
    row1->setSpacing(12);
    row1->addWidget(title);
    row1->addWidget(baseLabel_);
    row1->addStretch(1);
    row1->addWidget(simLabel_);
    row1->addWidget(progress_, 0, Qt::AlignVCenter);
    row1->addSpacing(6);
    row1->addWidget(cycleLabel_);
    row1->addSpacing(6);
    row1->addWidget(etaLabel_);
    row1->addSpacing(14);
    row1->addWidget(verdictLabel_);
    row1->addWidget(stopButton_);

    // ---- body: the field on the left, numbers and plots on the right
    fieldTitle_ = makeLabel(Pal::font(14, true), Pal::text(), this);
    fieldTitle_->setText(tr("혈관 · 현재 압력"));
    field_ = new FieldView(this);
    auto *left = new QVBoxLayout;
    left->setSpacing(6);
    left->addWidget(fieldTitle_);
    left->addWidget(field_, 1);

    auto *right = new QWidget(this);
    right->setFixedWidth(400);
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(16);
    grid->setVerticalSpacing(10);
    mBp_   = makeMetric(right, grid, 0, 0, tr("대동맥 수축기 / 이완기"));
    mPp_   = makeMetric(right, grid, 0, 1, tr("맥압"));
    mDsbp_ = makeMetric(right, grid, 1, 0, tr("심박 간 수축기압 변화"));
    mPwv_  = makeMetric(right, grid, 1, 1, tr("경동맥–대퇴 맥파 속도"));
    wave_  = new WaveView(right);
    trend_ = new TrendView(right);
    auto *rv = new QVBoxLayout(right);
    rv->setContentsMargins(0, 0, 0, 0);
    rv->setSpacing(14);
    rv->addLayout(grid);
    rv->addWidget(hairline(right));
    rv->addWidget(wave_, 3);
    rv->addWidget(trend_, 2);

    auto *body = new QHBoxLayout;
    body->setSpacing(28);
    body->addLayout(left, 1);
    body->addWidget(right);

    // ---- status line: what the monitor itself is doing
    statusLeft_  = makeLabel(Pal::font(12), Pal::muted(), this);
    statusRight_ = makeLabel(Pal::font(12), Pal::muted(), this);
    auto *status = new QHBoxLayout;
    status->addWidget(statusLeft_, 1);
    status->addWidget(statusRight_);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(24, 18, 24, 14);
    root->setSpacing(10);
    root->addLayout(row1);
    root->addWidget(noteLabel_);
    root->addWidget(hairline(this));
    root->addSpacing(4);
    root->addLayout(body, 1);
    root->addWidget(hairline(this));
    root->addLayout(status);

    setVerdict(QString());
    simLabel_->setText(tr("연결 중…"));
    const QString where = opt_.stream.sshHost.isEmpty() ? tr("로컬") : opt_.stream.sshHost;
    statusRight_->setText(tr("his_monitor --stream · %1").arg(where));

    connect(client_, &StreamClient::initReceived, this, &MainWindow::onInit);
    connect(client_, &StreamClient::tickReceived, this, &MainWindow::onTick);
    connect(client_, &StreamClient::finished, this, &MainWindow::onFinished);
    connect(client_, &StreamClient::failed, this, &MainWindow::onFailed);
    connect(field_, &FieldView::selectedChanged, wave_, &WaveView::setSelected);
    connect(stopButton_, &QPushButton::clicked, this, &MainWindow::onStopClicked);

    if (!opt_.captureDir.isEmpty()) {
        auto *t = new QTimer(this);
        connect(t, &QTimer::timeout, this, &MainWindow::captureFrame);
        t->start(int(qMax(0.5, opt_.captureEvery) * 1000));
    }
    if (opt_.quitAfter > 0)
        QTimer::singleShot(int(opt_.quitAfter * 1000), this, [this] { close(); });

    client_->start(opt_.stream);
}

// client_ is a child object; its destructor stops the monitor.
MainWindow::~MainWindow() = default;

void MainWindow::closeEvent(QCloseEvent *e)
{
    client_->stop();
    e->accept();
}

void MainWindow::onInit(const StreamInit &init)
{
    init_ = init;
    const QString stem = QFileInfo(init.base).fileName();
    baseLabel_->setText(stem);
    setWindowTitle(QStringLiteral("his_monitor · %1").arg(stem));
    fieldTitle_->setText(tr("혈관 %1개 · 현재 압력").arg(init.ndom));
    field_->setInit(init);
    wave_->setInit(init);
    stopButton_->setEnabled(true);
}

void MainWindow::onTick(const StreamTick &t)
{
    simLabel_->setText(tr("시뮬레이션 %1 / %2 s").arg(fmt(t.t, 3), fmt(init_.tfinal, 3)));
    progress_->setValue(init_.tfinal > 0
                            ? int(1000 * qBound(0.0, t.t / init_.tfinal, 1.0)) : 0);
    cycleLabel_->setText(tr("심박 %1").arg(t.cycle));
    etaLabel_->setText(t.eta > 0 ? tr("남은 시간 %1 s").arg(t.eta, 0, 'f', 0) : QString());
    setVerdict(t.verdict);
    noteLabel_->setText(t.note);

    setMetric(mBp_, fmt(t.sbp, 1) + QStringLiteral(" / ") + fmt(t.dbp, 1), QStringLiteral("mmHg"));
    setMetric(mPp_, fmt(t.sbp - t.dbp, 1), QStringLiteral("mmHg"));
    setMetric(mDsbp_, std::isfinite(t.dsbp) ? QString::asprintf("%+.2f", t.dsbp)
                                            : QStringLiteral("–"), QStringLiteral("mmHg"));
    setMetric(mPwv_, t.pwv > 0 ? fmt(t.pwv, 2) : QStringLiteral("–"), QStringLiteral("m/s"));

    field_->setTick(t);
    wave_->addTick(t);
    trend_->setBeats(t.sbpBeats, t.sbpCurrent);

    statusLeft_->setText(tr("도메인 %1 · 리더 스레드 %2 · %3 MB 읽음 · 최대 유속 %4 m/s · "
                            "틱 최대 %5 ms · 지터 %6 ms · 데드라인 초과 %7 / %8")
                             .arg(init_.ndom).arg(t.threads).arg(fmt(t.mb, 1))
                             .arg(fmt(t.umax, 2)).arg(fmt(t.tickMaxMs, 1))
                             .arg(fmt(t.jitterMs, 1)).arg(t.late).arg(t.ticks));
}

void MainWindow::setVerdict(const QString &v)
{
    if (v == lastVerdict_) return;
    lastVerdict_ = v;
    QString bg = Pal::css(Pal::bg()), fg = Pal::css(Pal::muted()), border = Pal::css(Pal::line());
    if (v == QLatin1String("CONVERGED")) {
        bg = border = Pal::css(Pal::accent());
        fg = QStringLiteral("#ffffff");
    } else if (v == QLatin1String("CONVERGING")) {
        bg = Pal::css(Pal::accentSoft());
        fg = border = Pal::css(Pal::accent());
    } else if (v == QLatin1String("OFF TARGET") || v == QLatin1String("STALLED")) {
        bg = border = Pal::css(Pal::fault());
        fg = QStringLiteral("#ffffff");
    }
    verdictLabel_->setText(verdictText(v));
    verdictLabel_->setToolTip(v);
    verdictLabel_->setStyleSheet(QStringLiteral(
        "QLabel{background:%1;color:%2;border:1px solid %3;border-radius:14px;padding:4px 14px;}")
        .arg(bg, fg, border));
}

void MainWindow::onFinished(int code, const QString &verdict, const QString &note)
{
    stopButton_->setEnabled(false);
    if (!verdict.isEmpty()) setVerdict(verdict);
    noteLabel_->setText(tr("모니터 종료 (코드 %1)").arg(code) +
                        (note.isEmpty() ? QString() : QStringLiteral(" · ") + note));
    statusRight_->setText(tr("연결 끊김"));
}

void MainWindow::onFailed(const QString &why)
{
    stopButton_->setEnabled(false);
    noteLabel_->setStyleSheet(QStringLiteral("color:%1;").arg(Pal::css(Pal::fault())));
    noteLabel_->setText(why);
    simLabel_->setText(tr("연결 안 됨"));
    statusRight_->setText(tr("연결 끊김"));
}

// Run a short command where the monitor runs -- here, or on the ssh host.
QString MainWindow::runCommand(const QStringList &argv, int timeoutMs) const
{
    QProcess p;
    if (opt_.stream.sshHost.isEmpty()) {
        p.start(argv.first(), argv.mid(1));
    } else {
        QStringList q;
        for (const QString &a : argv) q << shellQuote(a);
        p.start(QStringLiteral("ssh"), {QStringLiteral("-T"), QStringLiteral("-o"),
                                        QStringLiteral("BatchMode=yes"), opt_.stream.sshHost,
                                        q.join(QLatin1Char(' '))});
    }
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(500);
        return QString();
    }
    return QString::fromLocal8Bit(p.readAllStandardOutput());
}

// Stop the solver -- not just the monitor. The solver is started as
// `oneDbio ... <base>.in`, so its command line names the input file, and
// nothing else that is running should. The processes found are shown by name
// and have to be confirmed before anything is signalled.
//
// The match accepts "<base>.in" and "<base> in". A running solver rewrites its
// own argument in place -- the '.' of the file name is overwritten with a NUL
// -- so ps and /proc show `-m 116art_25yo in`.
void MainWindow::onStopClicked()
{
    const QString stem = QFileInfo(opt_.stream.base).fileName();
    const QString pattern = QRegularExpression::escape(stem) + QStringLiteral("[. ]in( |$)");
    const bool local = opt_.stream.sshHost.isEmpty();

    QStringList pids;
    const QStringList lines = runCommand({QStringLiteral("pgrep"), QStringLiteral("-f"), pattern})
                                  .split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString pid = line.trimmed();
        if (pid.isEmpty()) continue;
        if (local && (pid.toLongLong() == QCoreApplication::applicationPid() ||
                      pid.toLongLong() == client_->monitorPid()))
            continue;
        pids << pid;
    }
    if (pids.isEmpty()) {
        QMessageBox::information(this, tr("솔버 중단"),
            tr("실행 중인 솔버를 찾지 못했습니다.\n명령줄에 %1.in 이 들어 있는 프로세스가 없습니다.")
                .arg(stem));
        return;
    }
    const QString who = runCommand({QStringLiteral("ps"), QStringLiteral("-o"),
                                    QStringLiteral("pid=,args="), QStringLiteral("-p"),
                                    pids.join(QLatin1Char(','))}).trimmed();
    if (QMessageBox::question(this, tr("솔버 중단"),
            tr("이 프로세스에 중단 신호(SIGTERM)를 보낼까요?\n\n%1")
                .arg(who.isEmpty() ? pids.join(QStringLiteral(", ")) : who),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    runCommand(QStringList{QStringLiteral("kill"), QStringLiteral("-TERM")} + pids);
    noteLabel_->setText(tr("솔버에 중단 신호를 보냈습니다 (PID %1). 모니터가 곧 멈춤을 보고합니다.")
                            .arg(pids.join(QStringLiteral(", "))));
}

// Render the window into a PNG at `captureScale` times its size -- crisp when
// it is scaled down onto a slide, and identical to what is on screen.
void MainWindow::captureFrame()
{
    if (!QDir().mkpath(opt_.captureDir)) return;
    const qreal s = opt_.captureScale > 0 ? opt_.captureScale : 1.0;
    QImage img(QSize(qRound(width() * s), qRound(height() * s)),
               QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(s);
    img.fill(Pal::bg());
    render(&img);
    img.save(QDir(opt_.captureDir).filePath(
        QStringLiteral("frame_%1.png").arg(++frames_, 3, 10, QLatin1Char('0'))));
}
