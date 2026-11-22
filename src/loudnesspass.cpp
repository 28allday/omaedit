#include "loudnesspass.h"
#include "playerworker.h"

LoudnessPass::LoudnessPass(PlayerWorker *worker, QObject *parent) : QObject(parent) {
    connect(this, &LoudnessPass::analyzeRequested, worker, &PlayerWorker::analyzeLoudness);
    connect(this, &LoudnessPass::cancelRequested, worker, &PlayerWorker::cancelLoudnessAnalysis);
    connect(this, &LoudnessPass::normalizeRequested, worker, &PlayerWorker::normalizeLoudness);
    connect(worker, &PlayerWorker::loudnessAnalyzed, this, &LoudnessPass::analyzed);
}

void LoudnessPass::analyzed(int percent, const QVariantMap &values, const QList<float> &history) {
    QVariantMap a;
    a.insert(QStringLiteral("running"), percent < 100);
    a.insert(QStringLiteral("percent"), percent);
    if (percent >= 100) {
        a.insert(QStringLiteral("values"), values);
        QVariantList h;
        for (float v : history)
            h.append(double(v));
        a.insert(QStringLiteral("history"), h);
        if (values.contains("integrated")) emit message(tr("Loudness: %1 LUFS integrated, %2 dBTP")
                      .arg(values.value(QStringLiteral("integrated")).toDouble(), 0, 'f', 1)
                      .arg(values.value(QStringLiteral("truePeak")).toDouble(), 0, 'f', 1));
    } else if (m_result.contains(QStringLiteral("values"))) {
        a.insert(QStringLiteral("values"), m_result.value(QStringLiteral("values")));
        a.insert(QStringLiteral("history"), m_result.value(QStringLiteral("history")));
    }
    if (values.contains("error")) { a.remove("values"); a.remove("history"); emit message(values.value("error").toString()); }
    m_result = a;
    emit changed();
}

void LoudnessPass::invalidate() {
    if (!m_result.contains("values") || m_result.value("stale").toBool()) return;
    m_result.insert("stale", true);
    emit changed();
}

void LoudnessPass::analyze(int in, int out) {
    if ((hasTimeline && !hasTimeline()) || running())
        return;
    emit analyzeRequested(in, out);
}

void LoudnessPass::cancel() { emit cancelRequested(); }

void LoudnessPass::normalize(double targetLufs, double truePeak) {
    emit normalizeRequested(targetLufs, truePeak);
}
