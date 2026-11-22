#pragma once
#include <QList>
#include <QObject>
#include <QVariantMap>
#include <QtNumeric>
#include <functional>

class PlayerWorker;

// The offline loudness pass over the timeline's mix (Audio ▸ Loudness):
// measure a range faster than real time, and normalise the master to a
// target from what was measured. The player does the work on its thread;
// this asks for it and keeps the answer.
class LoudnessPass : public QObject {
    Q_OBJECT
    // {running, percent, values, history, stale}: values and history as
    // LoudnessMeter reports them; stale once the mix changed after measuring.
    Q_PROPERTY(QVariantMap result READ result NOTIFY changed)
  public:
    explicit LoudnessPass(PlayerWorker *worker, QObject *parent = nullptr);
    QVariantMap result() const { return m_result; }
    bool running() const { return m_result.value(QStringLiteral("running")).toBool(); }
    // Whether there is a timeline to measure; the owner says.
    std::function<bool()> hasTimeline;
    Q_INVOKABLE void analyze(int in = -1, int out = -1);
    Q_INVOKABLE void cancel();
    // Master volume += (target − integrated), from the last analysis or the
    // live meter. Not undoable (the master is not an edit). A true peak
    // above truePeak (-1 dBTP when not given) is reported.
    Q_INVOKABLE void normalize(double targetLufs, double truePeak = qQNaN());
    // The mix changed: what was measured no longer describes it.
    void invalidate();
  signals:
    void changed();
    void message(const QString &text); // a line for the status bar
    void analyzeRequested(int in, int out);
    void cancelRequested();
    void normalizeRequested(double target, double peakCeiling);

  private:
    void analyzed(int percent, const QVariantMap &values, const QList<float> &history);
    QVariantMap m_result;
};
