#pragma once
// FieldView.h -- the vessels as a grid of cells, grouped by region.
//
// Fill is the current pressure on a single blue ramp and the number is the
// same pressure in mmHg. A bar along the bottom of a cell means the wave
// front is passing through that vessel now. Click a cell to plot it.

#include "Protocol.h"

#include <QPair>
#include <QRectF>
#include <QVector>
#include <QWidget>

class FieldView : public QWidget {
    Q_OBJECT
public:
    explicit FieldView(QWidget *parent = nullptr);

    void setInit(const StreamInit &init);
    void setTick(const StreamTick &tick);
    void setSelected(int index);
    int selected() const { return selected_; }

signals:
    void selectedChanged(int index);

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    bool event(QEvent *e) override;

private:
    struct Cell { int index; QRectF rect; };
    struct Group { QString title; QRectF rect; };

    void layoutCells();
    int cellAt(const QPointF &p) const;
    QString label(int i) const;

    StreamInit init_;
    StreamTick tick_;
    bool haveTick_ = false;
    QVector<Cell> cells_;
    QVector<Group> groups_;
    double legendTop_ = 0;
    int selected_ = 0;

    // The colour range follows the data but moves slowly: taken over the last
    // few seconds of ticks, so a cell does not change colour just because the
    // extremes elsewhere in the tree moved.
    double lo_ = 60, hi_ = 140;
    QVector<QPair<double, double>> recent_;
};
