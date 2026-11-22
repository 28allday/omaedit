#pragma once
#include "latestqueue.h"
#include <QElapsedTimer>
#include <QImage>
#include <QTimer>
#include <memory>
#include <vector>

namespace ScopeProcessing {
// Immutable decoded pixels. Colour conversion and reduction belong to the worker.
// maximumParts caps the scope's own threads (0 = all): continuous playback
// keeps most cores for decoding and display.
// `counts`: the frame's scopes were counted where it was drawn (see Counted):
// only the painting is left to do, and the frame is not measured.
struct Request { QImage frame; QVariantMap options; QString outputSpace; int maximumParts=0;
                 std::shared_ptr<const std::vector<quint32>> counts; };
struct Result { QList<QImage> images; QString error; double milliseconds=0; qint64 frameKey=0; };
Result render(const Request &request,const std::atomic<bool> &cancelled);
// The options the scopes are measured with: the editor's, the rasters'
// sizes and the project's output space. The same for a frame measured here
// and for counts made where the frame was drawn.
QVariantMap measured(const QVariantMap &options,const QString &outputSpace);
// Scopes counted on the engine travel beside the picture they were counted
// of. That picture is one drawn for the viewer alone (8 bits, at its size),
// which cannot be measured; its counts are of the working picture it was made
// from. The worker's thread attaches them as it draws the picture, the GUI
// thread asks for them when the picture reaches it; a lock guards the few
// kept (the newest: pictures in the playback queue and on screen).
struct Counted { QVariantMap options; std::shared_ptr<const std::vector<quint32>> cells; };
void attach(const QImage &shown,const Counted &counted);
Counted counted(const QImage &shown);
}
Q_DECLARE_METATYPE(ScopeProcessing::Result)

// One active computation and one replaceable pending frame. All public calls and
// signals belong to the owning GUI thread; input images are implicitly shared.
class ScopeProcessor : public QObject {
    Q_OBJECT
public:
    using Render=std::function<ScopeProcessing::Result(const ScopeProcessing::Request &,const std::atomic<bool> &)>;
    explicit ScopeProcessor(QObject *parent=nullptr,Render render=ScopeProcessing::render);
    ~ScopeProcessor() override;
    // Continuous playback may publish a completed older frame in the same
    // context to avoid starvation. Paused edits/seeks require the latest request.
    void submit(const ScopeProcessing::Request &request,bool continuous=false);
    void cancel();
    QVariantMap information() const;
    static int playbackParts(); // scope threads while playing (OMAEDIT_SCOPE_PLAYBACK_PARTS)
signals:
    void ready(const ScopeProcessing::Result &result);
private:
    void schedule();
    SuiteProcessing::LatestQueue m_queue;
    QTimer m_timer;
    QElapsedTimer m_clock;
    Render m_render;
    ScopeProcessing::Request m_pending;
    QVariantMap m_options;
    QString m_outputSpace;
    quint64 m_epoch=0,m_submitted=0,m_coalesced=0,m_delivered=0;
    qsizetype m_runningBytes=0;
    bool m_hasPending=false,m_hasContext=false,m_stopping=false;
    double m_lastProcessingMs=0,m_lastLatencyMs=0;
    QElapsedTimer m_pendingClock;
};
