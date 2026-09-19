#pragma once
// TrendView.h -- how much the aortic root's systolic pressure changed from
// each beat to the next. The run has converged when the bars drop under the
// 0.5 mmHg line, which is the monitor's own CONVERGED criterion. Log scale,
// because the first change is tens of mmHg and the last is tenths.

#include <QVector>
#include <QWidget>

class TrendView : public QWidget {
    Q_OBJECT
public:
    explicit TrendView(QWidget *parent = nullptr);
    // Systolic of each completed beat, and of the beat in progress once its
    // peak is final (NaN before that). The last one is drawn hollow: it is
    // what the verdict is judging now, but no later foot has closed it yet.
    void setBeats(const QVector<double> &sbp, double current);

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QVector<double> sbp_;
    double current_ = 0;
    bool haveCurrent_ = false;
};
