#pragma once
// MainWindow.h -- the window: a header with progress and verdict, the field of
// vessels on the left, four numbers / the waveform / convergence on the right.

#include "Protocol.h"
#include "StreamClient.h"

#include <QWidget>

class QLabel;
class QProgressBar;
class QPushButton;
class FieldView;
class WaveView;
class TrendView;

class MainWindow : public QWidget {
    Q_OBJECT
public:
    struct Options {
        StreamClient::Options stream;
        QString captureDir;        // save a PNG every captureEvery seconds
        double captureEvery = 10;
        double captureScale = 2;   // 2: crisp when scaled onto a slide
        double quitAfter = 0;      // seconds; 0 = run until closed
    };

    explicit MainWindow(const Options &o, QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *e) override;

private:
    void onInit(const StreamInit &init);
    void onTick(const StreamTick &tick);
    void onFinished(int code, const QString &verdict, const QString &note);
    void onFailed(const QString &why);
    void onStopClicked();
    void setVerdict(const QString &verdict);
    void captureFrame();
    QString runCommand(const QStringList &argv, int timeoutMs = 3000) const;

    Options opt_;
    StreamClient *client_;
    StreamInit init_;
    QString lastVerdict_ = QStringLiteral("?");
    int frames_ = 0;

    QLabel *baseLabel_, *simLabel_, *cycleLabel_, *etaLabel_, *verdictLabel_, *noteLabel_;
    QProgressBar *progress_;
    QPushButton *stopButton_;
    QLabel *fieldTitle_;
    QLabel *mBp_, *mPp_, *mDsbp_, *mPwv_;
    FieldView *field_;
    WaveView *wave_;
    TrendView *trend_;
    QLabel *statusLeft_, *statusRight_;
};
