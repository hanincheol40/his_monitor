// FieldView.cpp -- see FieldView.h.
#include "FieldView.h"
#include "Palette.h"

#include <QFontMetricsF>
#include <QHelpEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>
#include <QtNumeric>

#include <cmath>

namespace {
const char *const kGroupName[4] = {"대동맥 · 내장", "머리 · 목", "팔 · 손", "골반 · 다리"};
constexpr double kGap = 4, kLabelH = 20, kGroupGap = 8, kLegendH = 26;
constexpr double kMinCellW = 90;
}  // namespace

FieldView::FieldView(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(false);
    setMinimumHeight(360);
}

void FieldView::setInit(const StreamInit &init)
{
    init_ = init;
    haveTick_ = false;
    recent_.clear();
    selected_ = 0;
    layoutCells();
    update();
}

void FieldView::setTick(const StreamTick &t)
{
    tick_ = t;
    haveTick_ = true;
    double mn = qInf(), mx = -qInf();
    for (double v : t.P)
        if (std::isfinite(v)) { mn = qMin(mn, v); mx = qMax(mx, v); }
    if (std::isfinite(mn)) {
        recent_.append({mn, mx});
        const int keep = qMax(4, 4000 / qMax(1, init_.periodMs));   // about 4 s
        while (recent_.size() > keep) recent_.removeFirst();
        double a = qInf(), b = -qInf();
        for (const auto &r : recent_) { a = qMin(a, r.first); b = qMax(b, r.second); }
        lo_ = std::floor(a / 10.0) * 10.0;
        hi_ = std::ceil(b / 10.0) * 10.0;
        if (hi_ - lo_ < 20) hi_ = lo_ + 20;
    }
    update();
}

void FieldView::setSelected(int index)
{
    if (index == selected_ || index < 0 || index >= init_.ndom) return;
    selected_ = index;
    update();
    emit selectedChanged(index);
}

void FieldView::resizeEvent(QResizeEvent *)
{
    layoutCells();
}

// Lay the cells out to fill the widget: as many columns as fit a readable
// name, and a row height that makes all four groups fit the height.
void FieldView::layoutCells()
{
    cells_.clear();
    groups_.clear();
    if (init_.ndom <= 0) return;

    const double W = width(), H = height();
    const int cols = qBound(4, int((W + kGap) / (kMinCellW + kGap)), 12);

    QVector<QVector<int>> members(4);
    for (int i = 0; i < init_.ndom; ++i) {
        int r = init_.region.value(i);
        if (r < 0 || r > 3) r = 0;
        members[r] << i;
    }
    int rows = 0, used = 0;
    for (const auto &m : members)
        if (!m.isEmpty()) { rows += (m.size() + cols - 1) / cols; ++used; }

    const double avail = H - kLegendH - used * (kLabelH + kGroupGap);
    const double cellH = qBound(16.0, avail / qMax(1, rows) - kGap, 30.0);
    const double cellW = (W - (cols - 1) * kGap) / cols;

    double y = 0;
    for (int g = 0; g < 4; ++g) {
        const QVector<int> &m = members[g];
        if (m.isEmpty()) continue;
        groups_.append({QString::fromUtf8(kGroupName[g]) + QStringLiteral("  %1").arg(m.size()),
                        QRectF(0, y, W, kLabelH)});
        y += kLabelH;
        for (int k = 0; k < m.size(); ++k)
            cells_.append({m[k], QRectF((k % cols) * (cellW + kGap),
                                        y + (k / cols) * (cellH + kGap), cellW, cellH)});
        y += ((m.size() + cols - 1) / cols) * (cellH + kGap) - kGap + kGroupGap;
    }
    legendTop_ = y;
}

QString FieldView::label(int i) const
{
    const QString s = init_.shortName.value(i);
    if (!s.isEmpty()) return s;
    const QString n = init_.name.value(i);
    if (!n.isEmpty()) return n;
    return QStringLiteral("seg %1").arg(i + 1, 3, 10, QLatin1Char('0'));
}

int FieldView::cellAt(const QPointF &p) const
{
    for (const Cell &c : cells_)
        if (c.rect.contains(p)) return c.index;
    return -1;
}

void FieldView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), Pal::bg());

    if (init_.ndom <= 0) {
        p.setPen(Pal::muted());
        p.setFont(Pal::font(14));
        p.drawText(rect(), Qt::AlignCenter, tr("his_monitor 연결을 기다리는 중"));
        return;
    }

    p.setFont(Pal::font(12, true));
    p.setPen(Pal::muted());
    for (const Group &g : groups_)
        p.drawText(g.rect.adjusted(0, 0, 0, -5), Qt::AlignLeft | Qt::AlignBottom, g.title);

    const QFont nameFont = Pal::font(10), valFont = Pal::font(11, true);
    const QFontMetricsF fmName(nameFont), fmVal(valFont);
    for (const Cell &c : cells_) {
        const int i = c.index;
        const double P = haveTick_ ? tick_.P.value(i, qQNaN()) : qQNaN();
        const bool has = std::isfinite(P);
        const QColor fill = has ? Pal::ramp((P - lo_) / (hi_ - lo_)) : Pal::empty();
        const QColor fg = has ? Pal::onFill(fill) : Pal::muted();

        QPainterPath box;
        box.addRoundedRect(c.rect, 3, 3);
        p.fillPath(box, fill);

        const QRectF in = c.rect.adjusted(5, 0, -5, 0);
        const QString val = has ? QString::number(P, 'f', 0) : QStringLiteral("–");
        p.setFont(valFont);
        p.setPen(fg);
        p.drawText(in, Qt::AlignRight | Qt::AlignVCenter, val);

        const double room = in.width() - fmVal.horizontalAdvance(val) - 4;
        p.setFont(nameFont);
        p.drawText(in, Qt::AlignLeft | Qt::AlignVCenter,
                   fmName.elidedText(label(i), Qt::ElideRight, room));

        if (has && tick_.front.value(i))
            p.fillRect(QRectF(c.rect.left() + 4, c.rect.bottom() - 3,
                              c.rect.width() - 8, 1.6), fg);

        if (i == selected_) {
            p.setPen(QPen(Pal::text(), 2));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(c.rect.adjusted(-1, -1, 1, 1), 4, 4);
        }
    }

    // Legend: the ramp with its range, and how to read a cell.
    const double ly = legendTop_ + 4;
    const QRectF bar(0, ly + 5, 160, 8);
    QLinearGradient grad(bar.topLeft(), bar.topRight());
    for (int k = 0; k <= 10; ++k) grad.setColorAt(k / 10.0, Pal::ramp(k / 10.0));
    p.fillRect(bar, grad);
    p.setFont(Pal::font(11));
    p.setPen(Pal::muted());
    p.drawText(QRectF(bar.right() + 8, ly, 160, 18), Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("%1 – %2 mmHg").arg(lo_, 0, 'f', 0).arg(hi_, 0, 'f', 0));
    p.drawText(QRectF(0, ly, width(), 18), Qt::AlignRight | Qt::AlignVCenter,
               tr("색 = 현재 압력   밑줄 = 파면 통과 중   칸을 누르면 오른쪽에 파형"));
}

void FieldView::mousePressEvent(QMouseEvent *e)
{
    const int i = cellAt(e->pos());
    if (i >= 0) setSelected(i);
}

bool FieldView::event(QEvent *e)
{
    if (e->type() == QEvent::ToolTip) {
        auto *he = static_cast<QHelpEvent *>(e);
        const int i = cellAt(he->pos());
        if (i < 0) {
            QToolTip::hideText();
            return true;
        }
        const QString full = init_.name.value(i).isEmpty() ? label(i) : init_.name.value(i);
        QString s = QStringLiteral("%1  %2").arg(i + 1, 3, 10, QLatin1Char('0')).arg(full);
        const double P = haveTick_ ? tick_.P.value(i, qQNaN()) : qQNaN();
        if (std::isfinite(P))
            s += tr("\n%1 mmHg · 대동맥에서 %2 ms 뒤 도달")
                     .arg(P, 0, 'f', 1).arg(tick_.arrivalMs.value(i));
        QToolTip::showText(he->globalPos(), s, this);
        return true;
    }
    return QWidget::event(e);
}
