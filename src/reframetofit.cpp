#include "reframetofit.h"

#include <algorithm>
#include <vector>
#include <cmath>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace ReframeToFit {

QPointF interest(const QImage &picture) {
    if (picture.isNull() || picture.width() < 4 || picture.height() < 4) return {0.5, 0.5};
    const QImage grey = picture.convertToFormat(QImage::Format_Grayscale8);
    const cv::Mat view(grey.height(), grey.width(), CV_8UC1, const_cast<uchar *>(grey.constBits()), size_t(grey.bytesPerLine()));
    // Centre against surround (a difference of Gaussians): what stands apart
    // from what is around it, as a subject does from its background. Small,
    // in the picture's own shape.
    constexpr int width = 96;
    const int height = std::max(8, int(std::lround(view.rows * double(width) / view.cols)));
    cv::Mat small, centre, surround;
    cv::resize(view, small, cv::Size(width, height), 0, 0, cv::INTER_AREA);
    small.convertTo(small, CV_32F, 1. / 255.);
    cv::GaussianBlur(small, centre, cv::Size(0, 0), 1.0);
    cv::GaussianBlur(small, surround, cv::Size(0, 0), width / 12.);
    cv::Mat standing = cv::abs(centre - surround);
    cv::GaussianBlur(standing, standing, cv::Size(0, 0), 2.0);
    // The strongest tenth decides, weighted by how far above that it is.
    std::vector<float> values(standing.begin<float>(), standing.end<float>());
    std::vector<float> sorted = values;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() * 9 / 10, sorted.end());
    const double floor = sorted[sorted.size() * 9 / 10];
    double total = 0, sx = 0, sy = 0;
    for (int y = 0; y < standing.rows; ++y) {
        const float *row = standing.ptr<float>(y);
        for (int x = 0; x < standing.cols; ++x) {
            const double w = std::max(0., row[x] - floor);
            total += w;
            sx += w * (x + .5);
            sy += w * (y + .5);
        }
    }
    if (!(total > 1e-6) || !std::isfinite(total)) return {0.5, 0.5};
    return {std::clamp(sx / total / standing.cols, 0., 1.), std::clamp(sy / total / standing.rows, 0., 1.)};
}

QList<QPointF> smooth(const QList<QPointF> &path, int window) {
    if (path.size() < 3 || window < 2) return path;
    const int half = window / 2;
    const auto median = [&](int i, bool horizontal) {
        QList<double> values;
        for (int k = std::max(0, i - half); k <= std::min(int(path.size()) - 1, i + half); ++k)
            values.append(horizontal ? path.at(k).x() : path.at(k).y());
        std::sort(values.begin(), values.end());
        return values.at(values.size() / 2);
    };
    QList<QPointF> steady;
    for (int i = 0; i < path.size(); ++i) steady.append({median(i, true), median(i, false)});
    QList<QPointF> out;
    for (int i = 0; i < steady.size(); ++i) {
        QPointF sum;
        int count = 0;
        for (int k = std::max(0, i - half); k <= std::min(int(steady.size()) - 1, i + half); ++k, ++count) sum += steady.at(k);
        out.append(sum / count);
    }
    return out;
}

Placement place(double sourceAspect, double frameAspect, QPointF focus) {
    Placement p;
    if (!(sourceAspect > 0) || !(frameAspect > 0)) return p;
    // The fitted picture, as fractions of the frame, centred in it.
    const double width = std::min(1., sourceAspect / frameAspect), height = std::min(1., frameAspect / sourceAspect);
    const double left = (1 - width) / 2, top = (1 - height) / 2;
    p.scale = std::max(1 / width, 1 / height); // fills the frame
    // Picture point (u, v) lands at scale * (left + u * width) + x: centre the
    // focus, but keep the picture covering the frame on both sides.
    const double x = .5 - p.scale * (left + focus.x() * width), y = .5 - p.scale * (top + focus.y() * height);
    // The two limits meet where the picture just covers the frame, and there
    // rounding can put the upper a hair under the lower (scale * width is
    // 1 - 1e-16 for 16:9 into 4:3): std::clamp aborts on that, so they are
    // put in order first.
    const auto within = [](double v, double a, double b) { return std::clamp(v, std::min(a, b), std::max(a, b)); };
    p.x = within(x, 1 - p.scale * (left + width), -p.scale * left);
    p.y = within(y, 1 - p.scale * (top + height), -p.scale * top);
    return p;
}

}  // namespace ReframeToFit
