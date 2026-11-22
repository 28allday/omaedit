#pragma once
#include <QList>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>

// What the meters show while sound plays. The player hands its measurements
// to the slots; nothing here is stored in a project.
//
// left / right: the playback level per channel in dBFS (-100 = silent).
// trackLevels: per track {in, post, out}, each [L, R] in dBFS, from the
// engine's level taps. routingLevels: per track {rms, gainReduction, volume,
// pan}. loudness: the master's running EBU R128 values (see
// LoudnessMeter::snapshot) with its momentary history. spectrum: 48
// log-spaced bins in dBFS.
class AudioMeters : public QObject {
    Q_OBJECT
    Q_PROPERTY(double left READ left NOTIFY levelsChanged)
    Q_PROPERTY(double right READ right NOTIFY levelsChanged)
    Q_PROPERTY(QVariantList trackLevels READ trackLevels NOTIFY trackLevelsChanged)
    Q_PROPERTY(QVariantList routingLevels READ routingLevels NOTIFY trackLevelsChanged)
    Q_PROPERTY(QVariantMap loudness READ loudness NOTIFY loudnessChanged)
    Q_PROPERTY(QVariantList loudnessHistory READ loudnessHistory NOTIFY loudnessChanged)
    Q_PROPERTY(QVariantList spectrum READ spectrum NOTIFY spectrumChanged)
  public:
    using QObject::QObject;
    double left() const { return m_left; }
    double right() const { return m_right; }
    QVariantList trackLevels() const { return m_trackLevels; }
    QVariantList routingLevels() const { return m_routingLevels; }
    QVariantMap loudness() const { return m_loudness; }
    QVariantList loudnessHistory() const { return m_loudnessHistory; }
    QVariantList spectrum() const { return m_spectrum; }
  public slots:
    void setLevels(float left, float right);
    void setTrackPeaks(const QList<float> &peaks); // six linear peaks per track
    void setRoutingLevels(const QVariantList &levels);
    void setLoudness(const QVariantMap &values, const QList<float> &history);
    void setSpectrum(const QList<float> &bins);
  signals:
    void levelsChanged();
    void trackLevelsChanged();
    void loudnessChanged();
    void spectrumChanged();

  private:
    double m_left = -100.0, m_right = -100.0;
    QVariantList m_trackLevels, m_routingLevels;
    QVariantMap m_loudness;
    QVariantList m_loudnessHistory, m_spectrum;
};
