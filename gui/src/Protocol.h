#pragma once
// Protocol.h -- the `his_monitor --stream` line format, as the GUI reads it.
//
// One JSON object per line: "init" once, a "tick" per display period, then
// "exit". Parsing lives here and nowhere else, so the window and the
// --check-stream self-test read the stream through exactly the same code.

#include <QByteArray>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>

struct StreamInit {
    QString base;
    int ndom = 0;
    double tfinal = 0;
    int periodMs = 0;
    int histHz = 0;
    QVector<int> region;      // 0 aorta/visceral, 1 head/neck, 2 arm/hand, 3 pelvis/leg
    QStringList name;         // full artery name; empty without --names
    QStringList shortName;    // abbreviated to fit a cell
};

struct StreamTick {
    double t = 0, wall = 0, eta = 0;
    QString verdict, note;
    int cycle = 0;
    double sbp = 0, dbp = 0, dsbp = 0, pwv = 0, umax = 0;
    int umaxDom = 0, threads = 0, ioerrDom = 0;
    double mb = 0, tickMs = 0, tickMeanMs = 0, tickMaxMs = 0, jitterMs = 0;
    qint64 late = 0, ticks = 0;
    QVector<double> sbpBeats;           // systolic of each completed beat, root
    double sbpCurrent = 0;              // the beat in progress, once its peak is
                                        // final; NaN while it is still rising
    QVector<double> P;                  // mmHg; NaN where a domain has no sample
    QVector<int> arrivalMs;             // foot delay from the aortic root
    QVector<bool> front;                // on the wave front this frame
    QVector<QVector<QPointF>> hist;     // per domain: (t, P) since the last tick
};

struct StreamExit {
    int code = 0;
    QString verdict, note;
};

enum class LineKind { Init, Tick, Exit, Invalid };

// Parse one line. A tick is checked against the init that came before it,
// so pass that init (nullptr if none has arrived). `err` explains Invalid.
LineKind parseLine(const QByteArray &line, const StreamInit *init,
                   StreamInit *initOut, StreamTick *tickOut,
                   StreamExit *exitOut, QString *err);
