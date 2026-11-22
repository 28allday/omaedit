#include "waveformitem.h"

#include <QCoreApplication>
#include <QPainter>
#include <QPointer>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QThreadPool>
#include <cmath>

namespace {
// Past this many samples a pixel the finest peaks (256 samples a block) are
// wider than two pixels: the samples themselves are read instead.
constexpr double kSampleZoom = WavePeaks::kBlock / 2.;

// The samples a zoomed-in picture reads come on a thread of their own (one:
// a reader is not shared), through whatever the application has given to
// read them with (wavesamples.cpp).
WaveformItem::SampleReader sampleReader;
QThreadPool &samplePool() {
    static QThreadPool *pool = [] {
        auto *made = new QThreadPool;
        made->setMaxThreadCount(1);
        made->setThreadPriority(QThread::LowPriority);
        if (QCoreApplication::instance())
            QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, QCoreApplication::instance(), [made] {
                made->clear();
                made->waitForDone();
            });
        return made;
    }();
    return *pool;
}

static void registerWaveformItem() { qmlRegisterType<WaveformItem>("omaedit", 1, 0, "WaveformItem"); }
Q_COREAPP_STARTUP_FUNCTION(registerWaveformItem)
}  // namespace

WaveformItem::WaveformItem(QQuickItem *parent) : QQuickPaintedItem(parent) {
    m_ask.setSingleShot(true);
    m_ask.setInterval(40);
    connect(&m_ask, &QTimer::timeout, this, [this] {
        if (!m_data || m_asking || width() <= 0) return;
        const int rate = m_data->rate();
        const qint64 samples = m_data->samples();
        double a = position(m_firstFrame), b = position(m_lastFrame);
        if (a > b) std::swap(a, b);
        // A quarter of the view more on each side: a short scroll asks for nothing.
        const double margin = (b - a) / 4 + 2;
        const qint64 first = qBound<qint64>(0, qint64(std::floor(a - margin)), samples);
        const qint64 last = qBound<qint64>(first, qint64(std::ceil(b + margin)), samples);
        if (last <= first || last - first > qint64(kSampleZoom * 16384) || !sampleReader) return;
        m_asking = true;
        const quint64 ticket = ++m_asked;
        const WavePeaks::Recipe recipe = WavePeaks::Store::instance()->recipe(m_source.id);
        samplePool().start([read = sampleReader, guard = QPointer<WaveformItem>(this), id = m_source.id, recipe, first, count = int(last - first), rate,
                                              channels = m_data->channels(), ticket] {
            const std::shared_ptr<const Window> window = read(id, recipe, first, count, rate, channels);
            QMetaObject::invokeMethod(qApp, [guard, window, ticket] {
                if (!guard || guard->m_asked != ticket) return;
                guard->m_asking = false;
                if (window) guard->m_window = window;
                else guard->m_unread = true;  // (not asked for again until the sound is another: the peaks stay)
                guard->refresh();
            }, Qt::QueuedConnection);
        });
    });
    connect(WavePeaks::Store::instance(), &WavePeaks::Store::changed, this, [this](const QString &id) {
        if (id != m_source.id || m_source.kind == WavePeaks::Source::None) return;
        const auto data = WavePeaks::Store::instance()->find(id);
        if (data != m_data) {
            // Another file under the same name: what was read of the old one goes.
            m_data = data;
            m_window.reset();
            ++m_asked;
            m_asking = false;
            m_unread = false;
        }
        refresh();
    });
}

WaveformItem::~WaveformItem() = default;

void WaveformItem::setSampleReader(const SampleReader &reader) { sampleReader = reader; }

void WaveformItem::setEntry(const QVariantMap &entry) {
    if (m_entry == entry) return;
    m_entry = entry;
    resolve();
    emit sourceChanged();
}
void WaveformItem::setPath(const QString &path) {
    if (m_path == path) return;
    m_path = path;
    resolve();
    emit sourceChanged();
}
void WaveformItem::setFps(double fps) {
    if (m_fps == fps) return;
    m_fps = fps;
    resolve();
    emit sourceChanged();
}
void WaveformItem::setPending(const QVariantMap &pending) {
    if (m_pending == pending) return;
    m_pending = pending;
    resolve();
    emit sourceChanged();
}
void WaveformItem::setPreviewDb(double db) {
    if (m_previewDb == db || (std::isnan(m_previewDb) && std::isnan(db))) return;
    m_previewDb = db;
    resolve();
    emit sourceChanged();
}
void WaveformItem::setFirstFrame(double frame) {
    if (m_firstFrame == frame) return;
    m_firstFrame = frame;
    refresh();
    emit viewChanged();
}
void WaveformItem::setLastFrame(double frame) {
    if (m_lastFrame == frame) return;
    m_lastFrame = frame;
    refresh();
    emit viewChanged();
}
void WaveformItem::setRectified(bool on) {
    if (m_rectified == on) return;
    m_rectified = on;
    update();
    emit lookChanged();
}
void WaveformItem::setStereoHeight(double height) {
    if (m_stereoHeight == height) return;
    m_stereoHeight = height;
    update();
    emit lookChanged();
}
void WaveformItem::setLaneGap(double gap) {
    if (m_laneGap == gap) return;
    m_laneGap = gap;
    update();
    emit lookChanged();
}
void WaveformItem::setPeakColor(const QColor &colour) {
    if (m_peakColor == colour) return;
    m_peakColor = colour;
    update();
    emit lookChanged();
}
void WaveformItem::setBodyColor(const QColor &colour) {
    if (m_bodyColor == colour) return;
    m_bodyColor = colour;
    update();
    emit lookChanged();
}
void WaveformItem::setClipColor(const QColor &colour) {
    if (m_clipColor == colour) return;
    m_clipColor = colour;
    update();
    emit lookChanged();
}

void WaveformItem::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) {
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) refresh();
}

void WaveformItem::itemChange(ItemChange change, const ItemChangeData &value) {
    QQuickPaintedItem::itemChange(change, value);
    if (change == ItemVisibleHasChanged && value.boolValue) refresh();
}

// A clip out of view (its block not shown, its page not the one open) is not
// painted: its picture is made when it is next seen (itemChange asks for it).
QSGNode *WaveformItem::updatePaintNode(QSGNode *node, UpdatePaintNodeData *data) {
    if (!isVisible()) return node;
    return QQuickPaintedItem::updatePaintNode(node, data);
}

// Which sound, and where in it: from the clip (or the file) as it now is.
// The peaks are whatever the store has of that sound; none is made here.
void WaveformItem::resolve() {
    const WavePeaks::Source source = !m_path.isEmpty() ? WavePeaks::Source::ofFile(m_path, m_fps) : WavePeaks::Source::of(m_entry, m_fps);
    if (source.id != m_source.id) {
        m_window.reset();
        ++m_asked;
        m_asking = false;
        m_unread = false;
        m_data = source.kind == WavePeaks::Source::None ? nullptr : WavePeaks::Store::instance()->find(source.id);
    }
    m_source = source;
    QVariantMap levelled = m_entry;
    // An edit in hand: a later in point brings the sound earlier under the
    // clip, a later start takes it later; its level and fades are counted
    // from where it would start, over the length it would have.
    const auto moved = [this](const char *key) {
        return m_pending.contains(QLatin1String(key)) ? m_pending.value(QLatin1String(key)).toDouble() - m_entry.value(QLatin1String(key)).toDouble() : 0.;
    };
    m_slip = moved("in") - moved("start");
    for (const char *key : {"start", "frames"})
        if (m_pending.contains(QLatin1String(key))) levelled.insert(QLatin1String(key), m_pending.value(QLatin1String(key)));
    if (!std::isnan(m_previewDb)) {
        levelled.insert(QStringLiteral("volume"), m_previewDb);
        levelled.remove(QStringLiteral("volumeKf"));
    }
    m_gain = WavePeaks::Gain(m_path.isEmpty() ? levelled : QVariantMap());
    refresh();
}

// Where a timeline frame lies in the sound's peaks, as the clip is or as the
// edit in hand would leave it.
double WaveformItem::position(double frame) const { return m_data ? m_source.position(frame + m_slip, m_data->rate(), m_data->samples()) : 0; }

double WaveformItem::samplesPerPixel() const {
    if (!m_data || width() <= 0) return 0;
    const double ratio = window() ? window()->effectiveDevicePixelRatio() : 1.;
    return std::abs(position(m_lastFrame) - position(m_firstFrame)) / (width() * ratio);
}

// Something it shows may have changed: its state for its owner, the samples
// it would need, and the picture.
void WaveformItem::refresh() {
    const bool ready = m_data && (m_data->made() > 0 || m_data->complete()), complete = m_data && m_data->complete();
    if (ready != m_ready || complete != m_complete) {
        m_ready = ready;
        m_complete = complete;
        emit readyChanged();
    }
    wantSamples();
    update();
}

void WaveformItem::wantSamples() {
    const double zoom = samplesPerPixel();
    bool shown = false;
    if (m_data && zoom > 0 && zoom < kSampleZoom && isVisible()) {
        const qint64 samples = m_data->samples();
        double a = position(m_firstFrame), b = position(m_lastFrame);
        if (a > b) std::swap(a, b);
        a = qBound(0., a, double(samples));
        b = qBound(0., b, double(samples));
        const bool covered = m_window && m_window->first <= qint64(std::floor(a)) && m_window->first + m_window->count() >= qint64(std::ceil(b));
        // The peaks stay on screen until the samples have come.
        if (!covered && b > a && !m_unread && !m_ask.isActive()) m_ask.start();
        shown = covered;
    }
    if (shown != m_sampleView) {
        m_sampleView = shown;
        emit sampleViewChanged();
    }
}

// What each column shows of one channel (or of both as one, channel < 0):
// the span of the samples under it at the clip's level there.
QVector<WaveformItem::Column> WaveformItem::lane(int channel, int columns) const {
    QVector<Column> out(qMax(0, columns));
    const double span = m_lastFrame - m_firstFrame;
    if (!m_data || columns < 1 || !(span > 0)) return out;
    const int channels = m_data->channels();
    const qint64 samples = m_data->samples();
    const auto frameAt = [&](double column) { return m_firstFrame + span * column / columns; };
    const double zoom = std::abs(position(frameAt(1)) - position(frameAt(0)));
    const Window *window = zoom < kSampleZoom ? m_window.get() : nullptr;
    double edge = position(frameAt(0));
    for (int c = 0; c < columns; ++c) {
        const double next = position(frameAt(c + 1));
        double a = edge, b = next;
        edge = next;
        if (a > b) std::swap(a, b);
        if (b <= 0 || a >= double(samples)) continue;  // before the source's start or past its end
        WavePeaks::Span found;
        const qint64 i0 = qMax<qint64>(0, qint64(std::floor(a))), i1 = qMin(samples, qMax(i0 + 1, qint64(std::ceil(b))));
        if (window && i0 >= window->first && i1 <= window->first + window->count()) {
            float lo = 1e9f, hi = -1e9f;
            double squares = 0;
            qint64 n = 0;
            const float *pcm = window->pcm.constData();
            // From the sample before the column's first: each column reaches
            // back to where the one before it ended, so that a few samples a
            // column are still one joined shape and not a scatter of marks.
            for (qint64 i = qMax<qint64>(0, i0 - 1 - window->first); i < i1 - window->first; ++i)
                for (int k = channel < 0 ? 0 : channel; k < (channel < 0 ? channels : channel + 1) && k < channels; ++k) {
                    const float v = pcm[i * window->channels + qMin(k, window->channels - 1)];
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                    squares += double(v) * double(v);
                    ++n;
                }
            if (n > 0) found = {lo, hi, float(std::sqrt(squares / double(n))), true};
        } else {
            found = m_data->span(channel, a, b);
        }
        if (!found.any) continue;
        const float gain = float(m_gain.at(frameAt(c + 0.5)));
        out[c] = {found.lo * gain, found.hi * gain, found.rms * gain, true};
    }
    return out;
}

QVariantList WaveformItem::columns(int channel) const {
    QVariantList out;
    for (const Column &column : lane(channel, int(std::ceil(width()))))
        out.append(QVariant(QVariantList{column.lo, column.hi, column.rms, column.any}));
    return out;
}

namespace {
// A colour as a pixel of the picture is kept (premultiplied), at a share of its strength.
inline QRgb toned(const QColor &colour, double share) {
    const int a = int(std::lround(colour.alpha() * std::clamp(share, 0., 1.)));
    return qRgba(colour.red() * a / 255, colour.green() * a / 255, colour.blue() * a / 255, a);
}
// One column of the picture from `top` to `bottom` (rows, fractions of a row
// at either end covered in part) in a colour, over what is there.
inline void fillColumn(QImage &image, int x, double top, double bottom, const QColor &colour) {
    top = std::max(top, 0.);
    bottom = std::min(bottom, double(image.height()));
    if (x < 0 || x >= image.width() || bottom <= top) return;
    for (int row = int(std::floor(top)); row < int(std::ceil(bottom)); ++row) {
        const double cover = std::min(double(row + 1), bottom) - std::max(double(row), top);
        const QRgb over = toned(colour, cover);
        QRgb &pixel = reinterpret_cast<QRgb *>(image.scanLine(row))[x];
        const int keep = 255 - qAlpha(over);
        pixel = qRgba(qRed(over) + qRed(pixel) * keep / 255, qGreen(over) + qGreen(pixel) * keep / 255,
                      qBlue(over) + qBlue(pixel) * keep / 255, qAlpha(over) + qAlpha(pixel) * keep / 255);
    }
}
}  // namespace

// The picture is made a pixel of the screen at a time, here: a column is a
// pixel wide and its two ends are covered in part where the wave ends within
// a pixel. (A filled outline through the painter cost ten times as much for
// a long clip, and its edges depended on how the painter rounds.)
void WaveformItem::paint(QPainter *painter) {
    if (!m_data || width() <= 0 || height() <= 0 || !(m_lastFrame > m_firstFrame)) return;
    const double ratio = window() ? window()->effectiveDevicePixelRatio() : 1.;
    const QSize size(qMax(1, int(std::ceil(width() * ratio))), qMax(1, int(std::ceil(height() * ratio))));
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    const int channels = m_data->channels();
    const bool lanes = height() >= m_stereoHeight && (channels == 2 || m_stereoHeight <= 0);
    if (!lanes) {
        paintLane(image, -1, QRectF(0, 0, width(), height()), ratio);
    } else {
        // Left over right, a little apart.
        const double each = (height() - m_laneGap) / 2;
        paintLane(image, 0, QRectF(0, 0, width(), each), ratio);
        paintLane(image, qMin(1, channels - 1), QRectF(0, each + m_laneGap, width(), each), ratio);
    }
    painter->setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter->drawImage(QRectF(0, 0, size.width() / ratio, size.height() / ratio), image);
}

// One lane, into `image` (pixels of the screen; `rect` in the item's own units).
void WaveformItem::paintLane(QImage &image, int channel, const QRectF &rect, double ratio) const {
    const int count = image.width();
    const double zoom = samplesPerPixel();
    // In rows of the image. Centred: full scale reaches the lane's edges from
    // its middle. Rectified: it stands from the foot to the top.
    const double laneTop = rect.top() * ratio, laneBottom = rect.bottom() * ratio;
    const double middle = (laneTop + laneBottom) / 2, foot = laneBottom;
    const double reach = m_rectified ? laneBottom - laneTop - ratio : (laneBottom - laneTop) / 2 - ratio / 2;
    const double base = m_rectified ? foot : middle;
    // The line silence lies on.
    for (int x = 0; x < count; ++x) fillColumn(image, x, std::max(laneTop, base - 0.5), std::min(laneBottom, base + 0.5), QColor(m_peakColor.red(), m_peakColor.green(), m_peakColor.blue(), int(m_peakColor.alpha() * 0.45)));

    // A sample wider than a pixel: a line through the samples, with a dot on
    // each once they stand well apart.
    if (m_window && zoom > 0 && zoom < 1) {
        const double p0 = position(m_firstFrame), p1 = position(m_lastFrame);
        const qint64 from = qint64(std::floor(std::min(p0, p1))) - 1, to = qint64(std::ceil(std::max(p0, p1))) + 1;
        const int channels = m_window->channels;
        QPolygonF points;
        for (qint64 i = qMax(from, m_window->first); i <= to && i < m_window->first + m_window->count(); ++i) {
            const float *pcm = m_window->pcm.constData() + (i - m_window->first) * channels;
            float v = channel < 0 ? pcm[0] : pcm[qMin(channel, channels - 1)];
            if (channel < 0 && channels > 1 && std::abs(pcm[1]) > std::abs(v)) v = pcm[1];  // the louder side, with its sign
            // A sample sits at its own instant: the middle of its stretch.
            const double x = (double(i) + 0.5 - p0) / (p1 - p0) * count;
            const double frame = m_firstFrame + x / count * (m_lastFrame - m_firstFrame);
            const double level = std::clamp(double(m_rectified ? std::abs(v) : v) * m_gain.at(frame), -1., 1.);
            points.append(QPointF(x, base - level * reach));
        }
        QPainter line(&image);
        line.setRenderHint(QPainter::Antialiasing, true);
        line.setClipRect(QRectF(0, laneTop, count, laneBottom - laneTop));
        // (In the body's tone, the lighter of the two: here the line is all of the wave.)
        line.setPen(QPen(m_bodyColor, 1.4 * ratio, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        line.drawPolyline(points);
        if (1. / zoom >= 6) {
            line.setPen(Qt::NoPen);
            line.setBrush(m_bodyColor);
            for (const QPointF &point : std::as_const(points)) line.drawEllipse(point, 2.2 * ratio, 2.2 * ratio);
        }
        return;
    }

    // The body (the RMS) is not drawn where a column holds too few samples
    // for one to mean anything; it comes in gradually as the zoom widens, and
    // the peaks go from the body's tone to their own as it does: zoomed
    // right in the wave is one shape in the lighter tone, never a faint one.
    const double bodyShare = std::clamp((zoom - 12) / 36, 0., 1.);
    const auto between = [&](int from, int to) { return int(std::lround(from + (to - from) * bodyShare)); };
    const QColor peakTone(between(m_bodyColor.red(), m_peakColor.red()), between(m_bodyColor.green(), m_peakColor.green()),
                          between(m_bodyColor.blue(), m_peakColor.blue()), between(m_bodyColor.alpha(), m_peakColor.alpha()));
    QColor bodyTone = m_bodyColor;
    bodyTone.setAlphaF(bodyTone.alphaF() * bodyShare);
    const QVector<Column> columns = lane(channel, count);
    const int mark = qMax(2, int(std::lround(2 * ratio)));
    for (int x = 0; x < count; ++x) {
        const Column &column = columns.at(x);
        if (!column.any) continue;
        const double loud = std::max(std::abs(column.lo), std::abs(column.hi));
        double high = base - std::clamp(m_rectified ? loud : double(column.hi), -1., 1.) * reach;
        double low = m_rectified ? foot : base - std::clamp(double(column.lo), -1., 1.) * reach;
        // Never thinner than a pixel: silence is a line, and a lone click a visible one.
        if (low - high < 1) {
            const double centre = m_rectified ? foot - 0.5 : (low + high) / 2;
            high = centre - 0.5;
            low = centre + 0.5;
        }
        fillColumn(image, x, std::max(high, laneTop), std::min(low, laneBottom), peakTone);
        if (bodyShare > 0) {
            const double rms = std::clamp(double(column.rms), 0., 1.) * reach;
            fillColumn(image, x, std::max(high, m_rectified ? foot - rms : middle - rms), std::min(low, m_rectified ? foot : middle + rms), bodyTone);
        }
        // At or past full scale: marked at the lane's edge it hits.
        if (column.hi >= 1.f || (m_rectified && column.lo <= -1.f)) fillColumn(image, x, laneTop, laneTop + mark, m_clipColor);
        if (!m_rectified && column.lo <= -1.f) fillColumn(image, x, laneBottom - mark, laneBottom, m_clipColor);
    }
}
