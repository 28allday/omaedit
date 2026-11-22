#pragma once

// What a media file is, read with libavformat: its clock, its frame shape, its
// streams and its tags. The answers follow the rules the editor's timeline has
// always used for a file (frame rate guessed from the video stream, length as
// the duration on that clock, sound-only files on the default 25 fps 720×576
// clock, stills held for kStillFrames), so a file means the same thing wherever
// it is opened.

#include <QString>
#include <QVariantMap>

namespace media {

struct Probe {
    bool valid = false;
    QString error;
    // The file's own clock and frame shape (the timeline's profile for it).
    int fpsNum = 25, fpsDen = 1;
    int frames = 0;             // its length on that clock
    int width = 720, height = 576;
    int sarNum = 16, sarDen = 15;
    bool progressive = false;
    int colourspace = 601;      // ITU matrix number: 601, 709, 2020 (0 for RGB)
    bool hasVideo = false, hasAudio = false;
    bool still = false;         // a single picture, held
    double seconds = 0;         // the container's duration
    qint64 duration = -1;       // the same in AV_TIME_BASE units (-1: none, a live source)
    qint64 animationMilliseconds = 0;  // an animated WebP's own length
    double rotation = 0;        // display rotation, degrees
    // Container metadata as the Media page reads it: fpsNum, fpsDen,
    // videoCodec, videoCodecLong, pixelFormat, bitDepth, audioCodec, channels,
    // sampleRate, sampleFormat, audioBitDepth, pixelAspect, camera keys,
    // colorSpace, created, timecode, timecodeFrames, make, model, containerTags.
    QVariantMap metadata;

    double fps() const { return fpsDen > 0 ? double(fpsNum) / fpsDen : 0; }
};

constexpr int kStillFrames = 15000;

// The file at `path` (a path on disk; generator resources such as "color:"
// are not files). Invalid, with an error, when it cannot be read.
Probe probe(const QString &path);

// The same, remembered by path, size and modification time (thread-safe).
Probe probeCached(const QString &path);
// A recording: a file with sound and no picture (a still is a picture). It
// has no frames to draw, on whatever track a timeline holds it. Asked once
// per path.
bool soundOnly(const QString &path);

// Its length in frames on another clock: a timeline's (duration on that clock,
// a still held, an animation's own length on it).
int framesOn(const Probe &probe, double fps);

// Whether a path names a still picture (by its suffix; an animated WebP is not one).
bool isStillPath(const QString &path);

}  // namespace media
