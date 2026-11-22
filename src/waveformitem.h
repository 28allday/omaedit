#pragma once

// The one picture of a sound's waveform, wherever one is shown (a clip on the
// Edit timeline, a clip on the Audio page, a bin clip, the Media page): for
// each column of the screen (a pixel wide) the span from the lowest to the
// highest sample under it, filled, its neighbours beside it with no gap, with
// the RMS inside it in a second tone; about a centre line (the default) or standing from the foot
// (rectified); two lanes for stereo when there is room, one otherwise. The
// same picture at every zoom: coarse peaks zoomed out, finer ones zoomed in,
// the samples themselves past the finest, a line through them once a sample
// is wider than a pixel.
//
// It reads WavePeaks::Store (it makes no peaks: PlayerWorker's peak thread
// does) and draws only itself: its owner lays it over the part of the clip
// on screen and says which timeline frames its two edges are.

#include "wavepeaks.h"

#include <QColor>
#include <QImage>
#include <QQuickPaintedItem>
#include <QTimer>
#include <functional>

class WaveformItem : public QQuickPaintedItem {
    Q_OBJECT
    // The clip whose sound it is (its file, its in point, its speed, its
    // level and fades), or a file on its own from its first frame.
    Q_PROPERTY(QVariantMap entry READ entry WRITE setEntry NOTIFY sourceChanged)
    Q_PROPERTY(QString path READ path WRITE setPath NOTIFY sourceChanged)
    Q_PROPERTY(double fps READ fps WRITE setFps NOTIFY sourceChanged)
    // The clip as an edit in hand (a trim, a slip, a slide being dragged)
    // would leave it, where that differs from the entry: its "start", its
    // "in" point, its "frames". The sound drawn is still the entry's own (no
    // peaks are made for an edit that may never be made); where each frame
    // lies in it, and the clip's level and fades, are the pending clip's.
    Q_PROPERTY(QVariantMap pending READ pending WRITE setPending NOTIFY sourceChanged)
    // The timeline frames at its left and right edges.
    Q_PROPERTY(double firstFrame READ firstFrame WRITE setFirstFrame NOTIFY viewChanged)
    Q_PROPERTY(double lastFrame READ lastFrame WRITE setLastFrame NOTIFY viewChanged)
    // Standing from the foot, absolute; otherwise about a centre line.
    Q_PROPERTY(bool rectified READ rectified WRITE setRectified NOTIFY lookChanged)
    // Stereo is drawn as two lanes from this height up (0: always two lanes,
    // a mono sound's one channel in both), `laneGap` apart.
    Q_PROPERTY(double stereoHeight READ stereoHeight WRITE setStereoHeight NOTIFY lookChanged)
    Q_PROPERTY(double laneGap READ laneGap WRITE setLaneGap NOTIFY lookChanged)
    // The clip's level in place of the entry's while it is being dragged (NaN: the entry's).
    Q_PROPERTY(double previewDb READ previewDb WRITE setPreviewDb NOTIFY sourceChanged)
    Q_PROPERTY(QColor peakColor READ peakColor WRITE setPeakColor NOTIFY lookChanged)
    Q_PROPERTY(QColor bodyColor READ bodyColor WRITE setBodyColor NOTIFY lookChanged)
    Q_PROPERTY(QColor clipColor READ clipColor WRITE setClipColor NOTIFY lookChanged)
    // Some of the sound's peaks are made; all of them are.
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(bool complete READ complete NOTIFY readyChanged)
    Q_PROPERTY(int channels READ channels NOTIFY readyChanged)
    // Whether the samples themselves are on screen (zoomed in past the peaks).
    Q_PROPERTY(bool sampleView READ sampleView NOTIFY sampleViewChanged)

public:
    explicit WaveformItem(QQuickItem *parent = nullptr);
    ~WaveformItem() override;
    void paint(QPainter *painter) override;

    QVariantMap entry() const { return m_entry; }
    void setEntry(const QVariantMap &entry);
    QString path() const { return m_path; }
    void setPath(const QString &path);
    double fps() const { return m_fps; }
    void setFps(double fps);
    QVariantMap pending() const { return m_pending; }
    void setPending(const QVariantMap &pending);
    double firstFrame() const { return m_firstFrame; }
    void setFirstFrame(double frame);
    double lastFrame() const { return m_lastFrame; }
    void setLastFrame(double frame);
    bool rectified() const { return m_rectified; }
    void setRectified(bool on);
    double stereoHeight() const { return m_stereoHeight; }
    void setStereoHeight(double height);
    double laneGap() const { return m_laneGap; }
    void setLaneGap(double gap);
    double previewDb() const { return m_previewDb; }
    void setPreviewDb(double db);
    QColor peakColor() const { return m_peakColor; }
    void setPeakColor(const QColor &colour);
    QColor bodyColor() const { return m_bodyColor; }
    void setBodyColor(const QColor &colour);
    QColor clipColor() const { return m_clipColor; }
    void setClipColor(const QColor &colour);
    bool ready() const { return m_ready; }
    bool complete() const { return m_complete; }
    int channels() const { return m_data ? m_data->channels() : 0; }
    bool sampleView() const { return m_sampleView; }

    // A stretch of a sound's own samples, read for a picture zoomed in.
    struct Window {
        qint64 first = 0;
        int channels = 1;
        QVector<float> pcm;  // interleaved
        qint64 count() const { return pcm.size() / qMax(1, channels); }
    };
    // How a stretch of a sound's samples is read (on a thread of the item's
    // own, one read at a time); null when it cannot be. Set once, at start.
    using SampleReader = std::function<std::shared_ptr<Window>(const QString &id, const WavePeaks::Recipe &recipe, qint64 first, int count, int rate, int channels)>;
    static void setSampleReader(const SampleReader &reader);
    // What each column of a lane shows, for tests: the span drawn (after the
    // clip's level), by column of the item at one column a pixel.
    Q_INVOKABLE QVariantList columns(int channel = -1) const;

signals:
    void sourceChanged();
    void viewChanged();
    void lookChanged();
    void readyChanged();
    void sampleViewChanged();

protected:
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;
    QSGNode *updatePaintNode(QSGNode *node, UpdatePaintNodeData *data) override;

private:
    struct Column { float lo = 0, hi = 0, rms = 0; bool any = false; };
    void resolve();
    void refresh();
    void wantSamples();
    double samplesPerPixel() const;
    double position(double frame) const;
    QVector<Column> lane(int channel, int columns) const;
    void paintLane(QImage &image, int channel, const QRectF &rect, double ratio) const;

    QVariantMap m_entry, m_pending;
    double m_slip = 0;  // frames the sound lies earlier under the pending clip than under the entry
    QString m_path;
    double m_fps = 25, m_firstFrame = 0, m_lastFrame = 0;
    bool m_rectified = false;
    double m_stereoHeight = 40, m_laneGap = 1;
    double m_previewDb = qQNaN();
    QColor m_peakColor{Qt::white}, m_bodyColor{Qt::gray}, m_clipColor{Qt::red};
    WavePeaks::Source m_source;
    WavePeaks::Gain m_gain;
    std::shared_ptr<WavePeaks::Data> m_data;
    std::shared_ptr<const Window> m_window;
    bool m_ready = false, m_complete = false, m_sampleView = false;
    bool m_asking = false;
    bool m_unread = false;  // its samples could not be read
    quint64 m_asked = 0;  // the request in hand: an answer to an earlier one is dropped
    QTimer m_ask;
};
