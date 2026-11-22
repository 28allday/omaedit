// How a waveform zoomed in past its finest peaks reads the samples themselves
// (WaveformItem::setSampleReader): a file through a reader as the mixer opens
// one, a clip's own sound through a mixer of the clip alone. The readers of
// the last two sounds are kept, so a scroll opens nothing.

#include "audiosamplesource.h"
#include "sound/mixer.h"
#include "waveformitem.h"

#include <QCoreApplication>
#include <QMutex>

namespace {
struct Kept {
    QString id;
    std::shared_ptr<AudioSampleSource::Reader> reader;
    std::shared_ptr<sound::Mixer> mixer;
};
QMutex keptMutex;
QList<Kept> kept;

std::shared_ptr<WaveformItem::Window> readSamples(const QString &id, const WavePeaks::Recipe &recipe, qint64 first, int count, int rate, int channels) {
    Kept mine;
    {
        QMutexLocker lock(&keptMutex);
        for (int i = 0; i < kept.size(); ++i)
            if (kept.at(i).id == id) { mine = kept.takeAt(i); break; }
    }
    QString error;
    auto window = std::make_shared<WaveformItem::Window>();
    window->first = first;
    window->channels = channels;
    if (recipe.kind == WavePeaks::Source::File) {
        if (!mine.reader) {
            AudioSampleSource::Options options;
            options.streamIndex = recipe.stream;
            mine.reader = AudioSampleSource::Reader::open(recipe.path, options, &error);
        }
        if (mine.reader) window->pcm = mine.reader->read(first, count, rate, channels, &error);
    } else if (recipe.kind == WavePeaks::Source::Clip) {
        if (!mine.mixer) {
            mine.mixer = std::make_shared<sound::Mixer>();
            mine.mixer->configure({{recipe.clip}, {}, {}, recipe.sources, recipe.fpsNum, recipe.fpsDen});
        }
        sound::Samples pcm;
        if (mine.mixer->unsupported().isEmpty() && mine.mixer->render(first, count, rate, channels, pcm, &error))
            window->pcm = QVector<float>(pcm.begin(), pcm.end());
    }
    if (mine.reader || mine.mixer) {
        mine.id = id;
        QMutexLocker lock(&keptMutex);
        kept.append(mine);
        while (kept.size() > 2) kept.removeFirst();
    }
    if (window->pcm.size() != qsizetype(count) * channels) return nullptr;
    return window;
}

void installSampleReader() {
    WaveformItem::setSampleReader(readSamples);
    // (Before the application's own objects go: a reader's decoder with them.)
    QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, QCoreApplication::instance(), [] {
        QMutexLocker lock(&keptMutex);
        kept.clear();
    });
}
Q_COREAPP_STARTUP_FUNCTION(installSampleReader)
}  // namespace
