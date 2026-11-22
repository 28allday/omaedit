#pragma once

// Pictures from media files, in the form the engine takes them: Y'CbCr planes
// at the file's own bit depth with its matrix, range and transfer, or packed
// 8-bit RGB. Nothing is converted to 8-bit RGB on the way. One MediaSource
// reads one file on one thread, keeping its decoder across seeks, so playing
// forward reads on rather than seeking again.

#include "engine.h"

#include <QImage>
#include <QString>
#include <QVariantMap>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

struct AVFrame;
struct SwsContext;

namespace render {

// A decoded picture held for the engine. The planes its inputs point at stay
// valid while it lives.
class Picture {
public:
    Picture();
    ~Picture();
    Picture(const Picture &) = delete;
    Picture &operator=(const Picture &) = delete;

    // Float: a float source (EXR and the like) as straight RGBA floats, its
    // values kept whole (above 1 too).
    // Gpu: a decoder's picture still on the engine's GPU (`frame`, a Vulkan
    // frame), `yuv` saying what its planes are.
    enum class Kind { None, Yuv, Rgb8, Float, Gpu };
    Kind kind = Kind::None;
    engine::YuvInput yuv;
    engine::CpuFrame rgb8;
    engine::CpuFrame floats;
    int width = 0, height = 0;
    double pixelAspect = 1.0;  // sample aspect ratio of the file's pixels
    double seconds = 0.0;      // where in the source this picture was taken

    // Hands the picture to an engine source. False when the engine refuses it.
    // `encoded`: its values kept as encoded, for a colour program to take on
    // (the clip's OCIO input transform).
    bool upload(engine::Engine &e, int source, bool encoded = false) const;
    // A Gpu picture brought into memory, as any other decoded one (for the
    // CPU's work, or an engine that will not take it from the GPU).
    bool download(QString *error);
    // A Y'CbCr picture in memory that is only drawn at a half or a quarter of
    // its size (`by` 2 or 4), made that small here, before it is sent to an
    // engine: each sample the mean of its block, chroma taken from its own
    // grid as the engine's conversion takes it, kept as 16-bit 4:4:4. For a
    // picture whose values the engine keeps as encoded (a colour-managed
    // timeline's) that is the picture the engine's own reduction makes
    // (YuvInput::reduce), without the whole picture being copied to it
    // first. False, the picture untouched, for one this does not fit (alpha,
    // fields, a layout it does not read): the engine reduces that one.
    // `width` and `height` stay the picture's own; reducedBy says what was done.
    bool makeSmaller(int by);
    int reducedBy = 1;
    // The picture as display-encoded 16-bit RGBA, for work done on the CPU
    // (a clip's grade), through `converter`.
    QImage rgba64(SwsContext **converter, QString *error) const;

    // What the picture holds: a reference on the decoder's frame, or a copy
    // where the layout had to change.
    AVFrame *frame = nullptr;
    std::vector<uint8_t> converted;
    void reset();
};

class MediaSource {
public:
    MediaSource();
    ~MediaSource();
    // `threads`: software decoding threads, 0 for the usual share of the machine.
    bool open(const QString &path, QString *error, int threads = 0);
    // The picture shown at `seconds` into the source (its first frame at 0).
    bool read(double seconds, Picture &out, QString *error);
    QVariantMap information() const;
    QString path() const;
    // Where the last picture read was, in seconds; below zero before any.
    double position() const;

private:
    struct State;
    std::unique_ptr<State> d;
};

// Runs before each picture is read, on the reading thread, with the source's
// path: tests slow or fail decoding through it (RenderScope cancellation and
// failure), as a slow or broken decoder would. None by default.
void setReadHook(std::function<void(const QString &path)> hook);
// The hook, for a reader of its own (the source viewer's).
void announceRead(const QString &path);

// Fills `out` from a decoded frame: planes taken as they are where the engine
// reads that layout, otherwise converted once to 16-bit 4:4:4 (with alpha
// when the frame has it). Exposed for tests.
bool pictureFromFrame(const AVFrame *frame, Picture &out, SwsContext **converter, QString *error);

// Open sources for one rendering thread, found by their file.
//
// A file may be open more than once: a frame that shows two parts of one
// recording (a dissolve between them, the same file on two tracks, a nest
// beside its own media) gives each part a decoder of its own, so neither
// seeks to and fro on every frame. A file that has changed on disk (saved
// again, replaced) is noticed by its size and date and opened afresh.
//
// Bounded three ways: by how many are open, by an estimate of the pictures
// their decoders hold, and by time (one that no frame has read for a while
// closes). The least recently used goes first; one read by the frame being
// drawn is never closed.
class MediaPool {
public:
    explicit MediaPool(int capacity = 16);
    ~MediaPool();
    // Most sources open at once, most bytes of decoded pictures between
    // them (an estimate from their sizes; 0 for no such bound), the software
    // decoding threads each takes (0 for the usual share), and how many
    // times over one file may be open (the parts of it one frame shows).
    void setLimits(int capacity, qint64 bytes, int threads, int perFile = 4);
    // A source of the file, for its facts: any that is open, or a new one.
    MediaSource *source(const QString &path, QString *error);
    // The source to read the picture at `seconds` from: one that is already
    // there or just before it, else one the frame being drawn has not read
    // from, else another of its own.
    MediaSource *sourceAt(const QString &path, double seconds, QString *error);
    // The file as it is on disk now (its size and date): what is kept of a
    // picture read from it is kept for this.
    QString stamp(const QString &path);
    // Pictures read from here on are away from the playhead (a filmstrip's
    // tiles), or at it again. Away from it they are read through sources of
    // their own, so the playhead's decoders stay where playing left them and
    // play starts without each of them having to find its place again.
    void setAside(bool aside);
    // A frame has been drawn: sources unread for a while close.
    void frameDone();
    // The sources picked since the last frameDone() were picked for a frame
    // that is not being drawn after all (pictures fetched ahead for a frame
    // that never came): they are no frame's now, free to go where the next
    // one asks.
    void unclaim();
    void clear();
    // Sources open, in all and of one file: for tests.
    int open() const;
    int open(const QString &path) const;

private:
    struct Entry {
        QString path, stamp;
        std::unique_ptr<MediaSource> source;
        quint64 used = 0, frame = 0;
        qint64 bytes = 0;
        bool aside = false;  // opened for pictures away from the playhead
    };
    Entry *opened(const QString &path, const QString &stamp, QString *error);
    void dropChanged(const QString &path, const QString &stamp);
    void makeRoom();
    std::vector<Entry> m_sources;
    int m_capacity, m_threads = 0, m_perFile = 4;
    qint64 m_bytes = qint64(3) << 30;
    quint64 m_clock = 0, m_frame = 1;
    bool m_aside = false;
};

}  // namespace render
