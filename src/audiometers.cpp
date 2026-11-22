#include "audiometers.h"
#include <cmath>

void AudioMeters::setLevels(float left, float right) {
    if (qFuzzyCompare(m_left, double(left))
        && qFuzzyCompare(m_right, double(right)))
        return;
    m_left = left;
    m_right = right;
    emit levelsChanged();
}

static double peakDb(float linear) {
    return linear > 0.0f ? qMax(-100.0, 20.0 * std::log10(double(linear))) : -100.0;
}

void AudioMeters::setTrackPeaks(const QList<float> &peaks) {
    QVariantList out;
    const int tracks = peaks.size() / 6;
    for (int t = 0; t < tracks; ++t) {
        QVariantMap m;
        m.insert(QStringLiteral("in"), QVariantList{peakDb(peaks.at(t * 6)), peakDb(peaks.at(t * 6 + 1))});
        m.insert(QStringLiteral("post"), QVariantList{peakDb(peaks.at(t * 6 + 2)), peakDb(peaks.at(t * 6 + 3))});
        m.insert(QStringLiteral("out"), QVariantList{peakDb(peaks.at(t * 6 + 4)), peakDb(peaks.at(t * 6 + 5))});
        out.append(m);
    }
    if (out.isEmpty() && m_trackLevels.isEmpty())
        return;
    m_trackLevels = out;
    emit trackLevelsChanged();
}

void AudioMeters::setRoutingLevels(const QVariantList &levels) {
    m_routingLevels = levels;
    emit trackLevelsChanged();
}

void AudioMeters::setLoudness(const QVariantMap &values, const QList<float> &history) {
    m_loudness = values;
    m_loudnessHistory.clear();
    m_loudnessHistory.reserve(history.size());
    for (float v : history)
        m_loudnessHistory.append(double(v));
    emit loudnessChanged();
}

void AudioMeters::setSpectrum(const QList<float> &bins) {
    m_spectrum.clear();
    for (float v : bins)
        m_spectrum.append(double(v));
    emit spectrumChanged();
}
