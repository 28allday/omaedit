#pragma once

// OmaEdit's own encoder sink: a delivery file written with libavcodec and
// libavformat from finished pictures and mixed sound, configured from
// ExportSpec::EncoderSettings. Pictures are converted with fixed swscale
// flags, coefficients and ranges, so a delivery is the same file every time.

#include "exportspec.h"
#include "frame.h"
#include "workingframe.h"

#include <QImage>
#include <QString>
#include <memory>
#include <optional>

namespace deliver {

class Sink {
public:
    Sink();
    ~Sink();
    Sink(const Sink &) = delete;
    Sink &operator=(const Sink &) = delete;

    // Opens `path` for `settings`: a video stream of `width` × `height` at
    // fpsNum/fpsDen when the settings have video, a sound stream when they
    // have sound. `pictureColourspace` is the ITU matrix number the pictures
    // are encoded for (they come full-range).
    bool open(const QString &path, const ExportSpec::EncoderSettings &settings, int width, int height, int fpsNum, int fpsDen,
              int pictureColourspace, QString *error);
    // The next picture: 16-bit RGBA (QImage::Format_RGBA64), the size given
    // to open(), already in the delivery's encoding.
    bool writePicture(const QImage &rgba64, QString *error);
    // An OpenEXR image sequence takes each picture as scene-linear working
    // light instead (writeLinear); every other delivery takes writePicture.
    bool wantsLinear() const;
    bool writeLinear(const WorkingFrame &frame, QString *error);
    // The encoder's picture layout when the engine can write it as it is
    // (writePlanes): its format, and the matrix (ITU number) and range the
    // planes are coded with. Empty when pictures must go through writePicture.
    struct Planes {
        engine::ColourFormat format = engine::ColourFormat::NV12;
        int colourspace = 709;
        bool fullRange = false;
    };
    std::optional<Planes> planes() const;
    // The next picture as those planes, at the size given to open().
    bool writePlanes(const engine::YuvFrame &frame, QString *error);
    // Interleaved float sound at the settings' rate and channel count; any
    // count of samples.
    bool writeSound(const float *samples, int count, QString *error);
    // Everything buffered out, the file closed.
    bool finish(QString *error);
    // Pictures written so far.
    qint64 pictures() const;

private:
    struct State;
    std::unique_ptr<State> d;
};

// The encoder check. An encoder that fails to initialise can take the process
// with it, so a delivery's exact settings are tried first in a process of its
// own: this program again, opening this sink (the libraries and the code the
// delivery itself will use) on one black picture and a moment of silence.
//
// A program's main() hands its arguments to probeMain first: -1 when they are
// not a check's (and the program is then known to answer one), otherwise the
// check's exit code (0: the settings encode).
int probeMain(int argc, char **argv);
// The command that runs the check for `spec` (a normalised delivery spec) on a
// timeline of `width` × `height` at fpsNum/fpsDen, its pictures coded for
// `colourspace` (the ITU matrix number), writing `output`. Empty when this
// program's main() does not answer the check, or the rate is not known.
QStringList probeCommand(const QVariantMap &spec, int width, int height, int fpsNum, int fpsDen, int colourspace, const QString &output);

}  // namespace deliver
