#pragma once
// WaveView.h -- the selected vessel's pressure against the aortic root's, over
// the last two seconds of simulated time. Drawn with QPainter: two lines and a
// grid need no chart library.

#include "Protocol.h"

#include <QVector>
#include <QWidget>

class WaveView : public QWidget {
    Q_OBJECT
public:
    explicit WaveView(QWidget *parent = nullptr);

    void setInit(const StreamInit &init);
    void addTick(const StreamTick &tick);
    void setSelected(int index);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QString title(int i) const;

    StreamInit init_;
    QVector<QVector<QPointF>> wave_;   // per domain, trimmed to the window
    QVector<int> arrival_;
    int selected_ = 0;
    double tnow_ = 0;
};
