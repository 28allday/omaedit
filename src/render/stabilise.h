#pragma once

// Stabilisation from a clip's motion analysis (the results file its
// analysis wrote, through libvidstab): each frame's correction, smoothed as
// the clip's settings ask, read once per file and settings.

#include <QSize>
#include <QString>
#include <memory>
#include <vector>

class QImage;
struct AVFrame;

namespace render {

// A clip's motion analysis, frame by frame in order, written to its results
// file as it goes (libvidstab's detection, with the settings the editor has
// always used: accuracy 4, step 6, minimum contrast 0.3, no tripod).
class MotionAnalysis {
public:
    MotionAnalysis();
    ~MotionAnalysis();
    MotionAnalysis(const MotionAnalysis &) = delete;
    MotionAnalysis &operator=(const MotionAnalysis &) = delete;
    // Frames of `size` (each picture is scaled to it), into `results`. The
    // size is the clip's picture as the timeline's frame shows it
    // (analysisFrame); the corrections are in its pixels, so the file's name
    // carries it (analysisFileName).
    bool open(const QString &results, int shakiness, const QSize &size, QString *error);
    // The next frame, in any QImage format.
    bool add(const QImage &picture, QString *error);
    // The next frame as decoded (its own planes, as the analysis has always
    // seen them: the source's luma untouched at its own size).
    bool add(const ::AVFrame *frame, QString *error);
    // The results file complete and closed.
    bool finish(QString *error);

private:
    struct State;
    std::unique_ptr<State> d;
};

struct Correction { double x = 0, y = 0, alpha = 0, zoom = 0; };  // pixels, radians, percent
// A clip's corrections, one a frame. `picture`: the size of the clip's picture
// the analysis saw, whose pixels the shifts are in; invalid for a file from
// before its name said (it saw the picture stretched over the whole
// timeline frame).
// `zoom`: the one zoom (percent) every frame takes, the least that fills the
// picture on every frame with its correction, turn and all, so no frame
// shows an edge. `steadied`: how much of the shake the corrections take out
// (1, all of it; less only where filling every frame would take more zoom
// than the stabiliser goes to, stabiliserMostZoom).
struct Corrections : std::vector<Correction> { QSize picture; double zoom = 0, steadied = 1; };
// The most the stabiliser zooms in (percent): the picture drawn twice its size.
constexpr double stabiliserMostZoom = 50;
// Gives every correction the one zoom (and the list its zoom and steadied)
// for corrections in pixels of a `seen` picture: the least zoom that fills
// every frame (never less than none). Where that is more than the most, the
// corrections are all eased by one share, the most that the most zoom still
// fills every frame with: the picture is steadied less, smoothly, and never
// shows an edge.
void fillEveryFrame(Corrections &list, const QSize &seen);
// The frame a clip's motion is analysed in: its picture (w × h, pixels
// `ratio` times as wide as the timeline's) as the clip's scaling places it in
// the timeline's frame, so a picture of another shape is seen as it is drawn,
// not stretched.
QSize analysisFrame(const QString &clipScaling, const QString &timelineScaling, int w, int h, double ratio, const QSize &timeline);
// The name of an analysis of a picture of that size: `stem`, then the size.
QString analysisFileName(const QString &stem, const QSize &picture);
// Null with `error` set when the file cannot be read. `frame`: the timeline's
// whole frame, which a file whose name does not give its picture's size is read for.
// How many times an analysis file has been read and worked out (not found
// among those kept): for tests.
int stabilisationParses();
std::shared_ptr<const Corrections> stabilisation(const QString &results, int smoothing, const QSize &frame, QString *error);
}
