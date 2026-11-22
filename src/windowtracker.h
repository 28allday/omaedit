#pragma once
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <vector>

// Translation tracking using a padded, normalized luminance template.
// The caller supplies clip-relative decoded frames in either direction.
class WindowTracker {
  public:
    bool start(const QImage &frame, const QRectF &window);
    bool next(const QImage &frame, QPointF *centre, double *confidence = nullptr);
    QString error() const { return m_error; }

  private:
    struct Sample {
        int x, y;
        double value;
    };
    std::vector<Sample> m_samples;
    QSize m_size, m_patch;
    QPoint m_position;
    QPointF m_offset;
    double m_variance = 0;
    QString m_error;
};
