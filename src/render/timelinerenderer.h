#pragma once

// A timeline frame drawn by the engine, from the editor's own description of
// the timeline (entries, tracks, project settings). Each visible clip's
// picture comes from the media pool, goes to an engine source, and is placed as
// a layer (fit to the frame, cropped, flipped, transformed, faded); dissolves
// mix two clips' pictures; compound clips draw their own clips first.
//
// What it cannot draw yet it names in unsupported(), so the caller can keep
// the old path for that timeline.

#include "engine.h"
#include "framing.h"
#include "mediapool.h"
#include "workingframe.h"

#include <QImage>
#include <QSize>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <functional>
#include <memory>

namespace render {

// Not drawn at all: sound-only, switched off, a gap, or on a track that is
// switched off or holds sound.
bool pictureHidden(const QVariantMap &clip, const QVariantList &tracks);
// Whether nothing about the clip itself can let what is under it show: no
// transparency from a colour, effect, soft crop, lens or stabilising, and not
// a title, a nest or an adjustment. Where it is placed, how opaque it is at a
// frame and how it blends are for the caller to look at, and whether a file's
// own pictures hold transparency.
bool solidPicture(const QVariantMap &clip);

class TimelineRenderer {
public:
    // Draws on `engine`, which it does not own; one thread uses both.
    explicit TimelineRenderer(engine::Engine &engine);
    ~TimelineRenderer();

    // project: the timeline's settings, with "pixelAspect" filled in.
    void configure(const QVariantList &entries, const QVariantList &tracks, const QVariantMap &project,
                   const QSize &size, double fps);
    // Where a clip's pictures are read from, given its path (a proxy, by the
    // playback choice), at every depth; its own path when unset.
    void setPictureSource(std::function<QString(const QString &)> source);
    // The project's media settings by path: nested timelines' clips of the
    // same media take them (an interpreted rate, a colour space).
    void setMediaSources(const QVariantMap &sources);
    // An export of another size: each picture is made `size` by `framing`
    // (fit, fill or fit over a blurred copy) on the engine. An invalid size,
    // or the timeline's own, draws the timeline as it is.
    void setFraming(const QSize &size, Framing framing);
    // How many media sources it keeps open at once, how many bytes of
    // decoded pictures between them (an estimate; 0 for no such bound), and
    // the software decoding threads each takes (0 for the usual share): a
    // renderer that is one of several working at once takes less.
    void setDecoderLimits(int sources, qint64 bytes, int threads, int perFile = 4);
    // Frames drawn from here on are away from the playhead (MediaPool::setAside), or at it again.
    void setAside(bool aside);
    // Whether a picture that is only drawn small is converted small (it is,
    // unless OMAEDIT_REDUCE_SOURCES=0): for every renderer of the program,
    // for tests and for comparing the two.
    static void setSourceReduction(bool on);
    // Whether such a picture, decoded into memory for a colour-managed
    // timeline, is made small before it is sent to the engine rather than
    // sent whole and reduced there (it is, unless OMAEDIT_REDUCE_BEFORE_UPLOAD=0).
    static void setReductionBeforeUpload(bool on);
    // Whether a frame's decoded pictures are fetched side by side on a small
    // pool of threads (they are, unless OMAEDIT_SERIAL_LAYERS=1: one after
    // another on the render thread, as they always were, kept as the
    // reference the other is compared with), and whether the next frame's
    // are begun while this one is finished (unless OMAEDIT_LAYERS_AHEAD=0).
    // For every renderer of the program; for tests and for comparing.
    static void setLayerFetch(bool sideBySide, bool ahead);
    // Media sources open now, in all and of one file: for tests.
    int openSources(const QString &path = {}) const;
    // Empty when every visible clip can be drawn; otherwise why not.
    QString unsupported() const;
    // Composites frame `position` onto the engine's canvas.
    bool render(int position, QString *error);
    // The last frame as 8-bit RGBA (display-encoded), tagged as the viewer
    // expects; renders first. A colour-managed timeline's is its working
    // frame's tagged image instead (see working()).
    QImage image(int position, QString *error);
    // Whether the timeline is colour-managed (OCIO, as LinearRenderer decides).
    bool managed() const;
    // A colour-managed timeline's frame: linear working light, as the
    // colour-managed renderer returns it, for the viewer's and the export's
    // own OCIO transforms. Before: the pictures before the clips'
    // corrections. Matte: grades showing their view node.
    enum class Mode { Normal, Before, Matte };
    WorkingFrame working(int position, QString *error, Mode mode = Mode::Normal);
    // The same picture as the engine holds it: straight linear Rec. 709, the
    // space its steps (a key among them) work in.
    QImage linear(int position, QString *error, Mode mode = Mode::Normal);
    // A colour-managed timeline's frame as a viewer draws it, made on the
    // engine: through `program` (a display transform from working light) and
    // packed to 8 bits with colour multiplied by its alpha, so nothing more
    // is done to it before it is on screen. Tagged "omaedit.shown" and, with
    // the size it stands for (the timeline's, when drawn smaller),
    // "omaedit.fullSize"; it is for looking at, never for measuring or
    // saving. Null, with why, when the engine cannot (the caller takes
    // working() instead).
    // With `scopes`: the scopes' counts of the working picture it was made
    // from, counted on the engine through scopes->program (the project's
    // output transform; null: none) as scopes->layout lays them out. Null
    // also when the engine cannot count: nothing is then drawn at all.
    struct ScopeCount {
        std::shared_ptr<const engine::ColourProgram> program;
        engine::ScopeLayout layout;
        std::vector<uint32_t> cells;
    };
    QImage shown(int position, std::shared_ptr<const engine::ColourProgram> program, const QSize &stands,
                 QString *error, ScopeCount *scopes = nullptr);
    // A colour-managed timeline's frame as an encoder takes it, made on the
    // engine: flattened over black, through OCIO's transform from working
    // light into `space` (a delivery colour space, as WorkingFrame::output
    // names it), then `colour`'s matrix, range and layout, at the timeline's
    // size. `out` is reused from call to call.
    bool deliver(int position, const QString &space, engine::OutputColour colour, engine::YuvFrame &out,
                 QString *error);
    // The same through any program from working light (the playback
    // cache's storage curve, say).
    bool deliver(int position, std::shared_ptr<const engine::ColourProgram> program, engine::OutputColour colour,
                 engine::YuvFrame &out, QString *error);

    // Clips drawn, pictures decoded, engine sources alive: for tests.
    // (`reduced`: pictures read for the frame that were converted smaller than they are;
    // `fetched`: those of `decoded` that the side-by-side fetch had ready, `ahead`:
    // those of them begun during the frame before.)
    // `small`: those of `reduced` made small before they were sent to the engine.
    struct Stats { int clips = 0, decoded = 0, sources = 0, reduced = 0, fetched = 0, ahead = 0, small = 0; };
    Stats stats() const;

private:
    struct State;
    std::unique_ptr<State> d;
};

// Whether the editor should draw with the engine (the OMAEDIT_RENDERER
// switch while the engine is new). Read once.
bool engineRenderingRequested();
// Why the engine would not draw a timeline (empty: it would), worked out
// without a GPU, for checks before an export.
QString engineUnsupported(const QVariantList &entries, const QVariantList &tracks, const QVariantMap &project,
                          const QSize &size, double fps);

}  // namespace render
