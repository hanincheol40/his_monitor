// WaveView.cpp -- see WaveView.h.
#include "WaveView.h"
#include "Palette.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace {
constexpr double kWindow = 2.0;      // seconds of simulated time on screen

// A round step that puts about `target` ticks across `range`.
double niceStep(double range, int target)
{
    const double raw = range / qMax(1, target);
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    for (double m : {1.0, 2.0, 2.5, 5.0, 10.0})
        if (m * mag >= raw) return m * mag;
    return 10.0 * mag;
}
}  // namespace

WaveView::WaveView(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(200);
}

void WaveView::setInit(const StreamInit &init)
{
    init_ = init;
    wave_ = QVector<QVector<QPointF>>(init.ndom);
    arrival_ = QVector<int>(init.ndom, 0);
    selected_ = 0;
    tnow_ = 0;
    update();
}

void WaveView::addTick(const StreamTick &t)
{
    if (wave_.size() != t.hist.size()) return;
    tnow_ = t.t;
    arrival_ = t.arrivalMs;
    for (int i = 0; i < wave_.size(); ++i) {
        QVector<QPointF> &w = wave_[i];
        const QVector<QPointF> &add = t.hist[i];
        // Time running backwards is a rerun into the same files: start over.
        if (!add.isEmpty() && !w.isEmpty() && add.first().x() < w.last().x() - 0.5)
            w.clear();
        w += add;
        const double cut = tnow_ - kWindow - 0.2;
        const auto keep = std::lower_bound(w.begin(), w.end(), cut,
            [](const QPointF &a, double x) { return a.x() < x; });
        w.erase(w.begin(), keep);
    }
    update();
}

void WaveView::setSelected(int index)
{
    selected_ = index;
    update();
}

QString WaveView::title(int i) const
{
    QString n = init_.shortName.value(i);
    if (n.isEmpty()) n = init_.name.value(i);
    const QString id = QStringLiteral("%1").arg(i + 1, 3, 10, QLatin1Char('0'));
    return n.isEmpty() ? id : id + QLatin1Char(' ') + n;
}

void WaveView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), Pal::bg());

    const int i = qBound(0, selected_, qMax(0, wave_.size() - 1));
    const QRectF head(0, 0, width(), 22);
    p.setFont(Pal::font(14, true));
    p.setPen(Pal::text());
    p.drawText(head, Qt::AlignLeft | Qt::AlignVCenter,
               tr("압력 파형 · %1").arg(wave_.isEmpty() ? QString() : title(i)));
    // The unit lives up here: on the axis it would collide with the top tick
    // label.
    p.setFont(Pal::font(12));
    p.setPen(Pal::muted());
    p.drawText(head, Qt::AlignRight | Qt::AlignVCenter,
               (i > 0 && arrival_.value(i) > 0)
                   ? tr("대동맥에서 %1 ms 뒤 도달 · mmHg").arg(arrival_.value(i))
                   : QStringLiteral("mmHg"));

    const QRectF plot(40, 32, width() - 48, height() - 32 - 22);
    const double t1 = tnow_, t0 = t1 - kWindow;
    const QVector<QPointF> none;
    const QVector<QPointF> &sel  = wave_.isEmpty() ? none : wave_[i];
    const QVector<QPointF> &root = wave_.isEmpty() ? none : wave_[0];

    double lo = 1e9, hi = -1e9;
    for (const auto *s : {&sel, &root})
        for (const QPointF &q : *s)
            if (q.x() >= t0 && std::isfinite(q.y())) { lo = qMin(lo, q.y()); hi = qMax(hi, q.y()); }
    if (lo > hi) {
        p.setFont(Pal::font(12));
        p.setPen(Pal::muted());
        p.drawText(plot, Qt::AlignCenter, tr("데이터를 기다리는 중"));
        return;
    }
    const double step = niceStep(qMax(1.0, hi - lo), 4);
    lo = std::floor(lo / step) * step;
    hi = std::ceil(hi / step) * step;
    if (hi <= lo) hi = lo + step;

    // A band along the top of the plot holds the legend, and the data is
    // scaled into the area below it, so a peak can never run through the key.
    const QRectF area = plot.adjusted(0, 18, 0, 0);
    auto X = [&](double t) { return plot.left() + (t - t0) / (t1 - t0) * plot.width(); };
    auto Y = [&](double v) { return area.bottom() - (v - lo) / (hi - lo) * area.height(); };

    // Grid and axis labels.
    p.setFont(Pal::font(11));
    for (double v = lo; v <= hi + 1e-9; v += step) {
        p.setPen(QPen(Pal::grid(), 1));
        p.drawLine(QPointF(plot.left(), Y(v)), QPointF(plot.right(), Y(v)));
        p.setPen(Pal::muted());
        p.drawText(QRectF(0, Y(v) - 8, plot.left() - 6, 16), Qt::AlignRight | Qt::AlignVCenter,
                   QString::number(v, 'f', 0));
    }
    for (double t = std::ceil(t0 / 0.5) * 0.5; t <= t1 + 1e-9; t += 0.5) {
        p.setPen(QPen(Pal::grid(), 1));
        p.drawLine(QPointF(X(t), plot.top()), QPointF(X(t), plot.bottom()));
        p.setPen(Pal::muted());
        // A label centred on the last tick would run off the right edge; pin
        // it to the edge instead.
        QRectF r(X(t) - 30, plot.bottom() + 4, 60, 16);
        Qt::Alignment al = Qt::AlignCenter;
        if (r.right() > width()) { r.moveRight(width()); al = Qt::AlignRight | Qt::AlignVCenter; }
        p.drawText(r, al, QStringLiteral("%1 s").arg(t, 0, 'f', 1));
    }
    p.setPen(QPen(Pal::line(), 1));
    p.drawLine(plot.bottomLeft(), plot.bottomRight());

    auto series = [&](const QVector<QPointF> &s, const QColor &c, double w) {
        QPainterPath path;
        bool first = true;
        for (const QPointF &q : s) {
            if (q.x() < t0 || !std::isfinite(q.y())) continue;
            const QPointF pt(X(q.x()), Y(q.y()));
            if (first) { path.moveTo(pt); first = false; } else path.lineTo(pt);
        }
        p.setPen(QPen(c, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    };
    p.save();
    p.setClipRect(area.adjusted(-1, -2, 1, 2));
    if (i != 0) series(root, Pal::muted(), 1.4);
    series(sel, Pal::accent(), 2.2);
    p.restore();

    // Legend, top right inside the plot.
    p.setFont(Pal::font(11));
    double lx = plot.right() - 8;
    auto key = [&](const QString &txt, const QColor &c) {
        const double w = QFontMetricsF(p.font()).horizontalAdvance(txt);
        lx -= w;
        p.setPen(Pal::muted());
        p.drawText(QRectF(lx, plot.top() + 2, w + 2, 16), Qt::AlignLeft | Qt::AlignVCenter, txt);
        lx -= 22;
        p.setPen(QPen(c, 2.2));
        p.drawLine(QPointF(lx, plot.top() + 10), QPointF(lx + 16, plot.top() + 10));
        lx -= 14;
    };
    key(i == 0 ? tr("대동맥 근위부") : tr("선택 혈관"), Pal::accent());
    if (i != 0) key(tr("대동맥 근위부"), Pal::muted());
}
