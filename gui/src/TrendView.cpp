// TrendView.cpp -- see TrendView.h.
#include "TrendView.h"
#include "Palette.h"

#include <QPainter>

#include <cmath>

namespace {
constexpr double kCriterion = 0.5;   // mmHg: wave.c calls a run CONVERGED below this
constexpr double kFloor = 0.05;      // bottom of the log axis
}  // namespace

TrendView::TrendView(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(140);
}

void TrendView::setBeats(const QVector<double> &sbp, double current)
{
    const bool have = std::isfinite(current);
    if (sbp == sbp_ && have == haveCurrent_ && (!have || current == current_)) return;
    sbp_ = sbp;
    haveCurrent_ = have;
    current_ = have ? current : 0;
    update();
}

void TrendView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), Pal::bg());

    const QRectF head(0, 0, width(), 22);
    p.setFont(Pal::font(14, true));
    p.setPen(Pal::text());
    p.drawText(head, Qt::AlignLeft | Qt::AlignVCenter, tr("수렴 · 심박마다 수축기압 변화"));
    p.setFont(Pal::font(12));
    p.setPen(Pal::muted());
    p.drawText(head, Qt::AlignRight | Qt::AlignVCenter,
               tr("mmHg · 기준 %1").arg(kCriterion, 0, 'f', 1));

    // One bar per beat after the first: |SBP(k) - SBP(k-1)|. The last bar is
    // the beat in progress when its peak is already final.
    struct Bar { double v; int beat; bool open; };
    QVector<Bar> bars;
    for (int k = 1; k < sbp_.size(); ++k)
        if (std::isfinite(sbp_[k]) && std::isfinite(sbp_[k - 1]))
            bars.append({std::fabs(sbp_[k] - sbp_[k - 1]), k + 1, false});
    if (haveCurrent_ && !sbp_.isEmpty() && std::isfinite(sbp_.last()))
        bars.append({std::fabs(current_ - sbp_.last()), int(sbp_.size()) + 1, true});

    const QRectF plot(40, 32, width() - 48, height() - 32 - 22);
    if (bars.isEmpty()) {
        p.setPen(Pal::muted());
        p.drawText(plot, Qt::AlignCenter, tr("두 번째 심박이 끝나면 나타납니다"));
        return;
    }

    double top = 10;
    for (const Bar &b : bars) top = qMax(top, b.v * 1.6);
    const double l0 = std::log10(kFloor), l1 = std::log10(top);
    auto Y = [&](double v) {
        const double lv = std::log10(qMax(v, kFloor));
        return plot.bottom() - (lv - l0) / (l1 - l0) * plot.height();
    };

    // Decades on the log axis.
    p.setFont(Pal::font(11));
    for (double v = 0.1; v <= top * 1.001; v *= 10) {
        p.setPen(QPen(Pal::grid(), 1));
        p.drawLine(QPointF(plot.left(), Y(v)), QPointF(plot.right(), Y(v)));
        p.setPen(Pal::muted());
        p.drawText(QRectF(0, Y(v) - 8, plot.left() - 6, 16), Qt::AlignRight | Qt::AlignVCenter,
                   v < 1 ? QString::number(v, 'f', 1) : QString::number(v, 'f', 0));
    }

    const int n = bars.size();
    const double slot = plot.width() / n;
    const double bw = qMin(30.0, slot * 0.62);
    for (int k = 0; k < n; ++k) {
        const Bar &b = bars[k];
        const double cx = plot.left() + slot * (k + 0.5);
        const QRectF r(cx - bw / 2, Y(b.v), bw, plot.bottom() - Y(b.v));
        if (b.open) {
            p.fillRect(r, Pal::accentSoft());
            p.setPen(QPen(Pal::accent(), 1.5));
            p.setBrush(Qt::NoBrush);
            p.drawRect(r.adjusted(0.75, 0.75, -0.75, 0));
        } else {
            p.fillRect(r, Pal::accent());
        }
        if (n <= 14) {
            p.setPen(Pal::text());
            // Never above the plot: the tallest bar's label would reach the title.
            const double ly = qMax(r.top() - 16, plot.top() - 6);
            p.drawText(QRectF(cx - 30, ly, 60, 14), Qt::AlignCenter,
                       b.v < 10 ? QString::number(b.v, 'f', 2) : QString::number(b.v, 'f', 1));
        }
        p.setPen(Pal::muted());
        p.drawText(QRectF(cx - 20, plot.bottom() + 4, 40, 16), Qt::AlignCenter,
                   QString::number(b.beat));
    }

    // The criterion, over the bars so it stays readable where they cross it.
    p.setPen(QPen(Pal::text(), 1.2, Qt::DashLine));
    p.drawLine(QPointF(plot.left(), Y(kCriterion)), QPointF(plot.right(), Y(kCriterion)));
    p.setPen(QPen(Pal::line(), 1));
    p.drawLine(plot.bottomLeft(), plot.bottomRight());
    p.setPen(Pal::muted());
    p.drawText(QRectF(0, plot.bottom() + 4, plot.left() - 6, 16), Qt::AlignRight | Qt::AlignVCenter,
               tr("심박"));
}
