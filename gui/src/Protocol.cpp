// Protocol.cpp -- see Protocol.h.
#include "Protocol.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QtNumeric>

#include <utility>

namespace {

// JSON null -- a value the monitor could not compute -- becomes NaN, never 0.
// A zero pressure would be drawn as a real one; a NaN is shown as "no data".
double num(const QJsonValue &v)
{
    return v.isDouble() ? v.toDouble() : qQNaN();
}

LineKind invalid(QString *err, const QString &why)
{
    if (err) *err = why;
    return LineKind::Invalid;
}

}  // namespace

LineKind parseLine(const QByteArray &line, const StreamInit *init,
                   StreamInit *initOut, StreamTick *tickOut,
                   StreamExit *exitOut, QString *err)
{
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject())
        return invalid(err, QStringLiteral("not a JSON object: %1").arg(pe.errorString()));

    const QJsonObject o = doc.object();
    const QString type = o.value(QStringLiteral("type")).toString();

    if (type == QLatin1String("init")) {
        StreamInit in;
        in.base     = o.value(QStringLiteral("base")).toString();
        in.ndom     = o.value(QStringLiteral("ndom")).toInt();
        in.tfinal   = num(o.value(QStringLiteral("tfinal")));
        in.periodMs = o.value(QStringLiteral("period_ms")).toInt();
        in.histHz   = o.value(QStringLiteral("hist_hz")).toInt();
        const QJsonArray reg = o.value(QStringLiteral("region")).toArray();
        const QJsonArray nm  = o.value(QStringLiteral("name")).toArray();
        const QJsonArray sh  = o.value(QStringLiteral("short")).toArray();
        if (in.ndom <= 0 || reg.size() != in.ndom || nm.size() != in.ndom ||
            sh.size() != in.ndom)
            return invalid(err, QStringLiteral("init: region/name/short are not one per domain"));
        for (int i = 0; i < in.ndom; ++i) {
            in.region << reg[i].toInt();
            in.name << nm[i].toString();
            in.shortName << sh[i].toString();
        }
        if (initOut) *initOut = std::move(in);
        return LineKind::Init;
    }

    if (type == QLatin1String("tick")) {
        if (!init) return invalid(err, QStringLiteral("tick before init"));
        const int n = init->ndom;
        const QJsonArray P    = o.value(QStringLiteral("P")).toArray();
        const QJsonArray arr  = o.value(QStringLiteral("arrival_ms")).toArray();
        const QJsonArray fr   = o.value(QStringLiteral("front")).toArray();
        const QJsonArray hist = o.value(QStringLiteral("hist")).toArray();
        if (P.size() != n || arr.size() != n || fr.size() != n || hist.size() != n)
            return invalid(err, QStringLiteral("tick: arrays are not one per domain "
                                               "(P has %1, want %2)").arg(P.size()).arg(n));
        StreamTick t;
        t.t          = num(o.value(QStringLiteral("t")));
        t.wall       = num(o.value(QStringLiteral("wall")));
        t.eta        = num(o.value(QStringLiteral("eta")));
        t.verdict    = o.value(QStringLiteral("verdict")).toString();
        t.note       = o.value(QStringLiteral("note")).toString();
        t.cycle      = o.value(QStringLiteral("cycle")).toInt();
        t.sbp        = num(o.value(QStringLiteral("sbp")));
        t.dbp        = num(o.value(QStringLiteral("dbp")));
        t.dsbp       = num(o.value(QStringLiteral("dsbp")));
        t.pwv        = num(o.value(QStringLiteral("pwv")));
        t.umax       = num(o.value(QStringLiteral("umax")));
        t.umaxDom    = o.value(QStringLiteral("umax_dom")).toInt();
        t.threads    = o.value(QStringLiteral("threads")).toInt();
        t.mb         = num(o.value(QStringLiteral("mb")));
        t.tickMs     = num(o.value(QStringLiteral("tick_ms")));
        t.tickMeanMs = num(o.value(QStringLiteral("tick_mean_ms")));
        t.tickMaxMs  = num(o.value(QStringLiteral("tick_max_ms")));
        t.jitterMs   = num(o.value(QStringLiteral("jitter_ms")));
        t.late       = qint64(o.value(QStringLiteral("late")).toDouble());
        t.ticks      = qint64(o.value(QStringLiteral("ticks")).toDouble());
        t.ioerrDom   = o.value(QStringLiteral("ioerr_dom")).toInt();
        for (const QJsonValue &v : o.value(QStringLiteral("sbp_beats")).toArray())
            t.sbpBeats << num(v);
        t.sbpCurrent = num(o.value(QStringLiteral("sbp_current")));

        t.P.resize(n);
        t.arrivalMs.resize(n);
        t.front.resize(n);
        t.hist.resize(n);
        for (int i = 0; i < n; ++i) {
            t.P[i] = num(P[i]);
            t.arrivalMs[i] = arr[i].toInt();
            t.front[i] = fr[i].toInt() != 0;
            const QJsonArray h = hist[i].toArray();
            if (h.size() % 2)
                return invalid(err, QStringLiteral("tick: hist[%1] is not (t, P) pairs").arg(i));
            t.hist[i].reserve(h.size() / 2);
            for (int k = 0; k + 1 < h.size(); k += 2)
                t.hist[i].append(QPointF(h[k].toDouble(), h[k + 1].toDouble()));
        }
        if (tickOut) *tickOut = std::move(t);
        return LineKind::Tick;
    }

    if (type == QLatin1String("exit")) {
        StreamExit x;
        x.code    = o.value(QStringLiteral("code")).toInt();
        x.verdict = o.value(QStringLiteral("verdict")).toString();
        x.note    = o.value(QStringLiteral("note")).toString();
        if (exitOut) *exitOut = std::move(x);
        return LineKind::Exit;
    }

    return invalid(err, QStringLiteral("unknown line type '%1'").arg(type));
}
