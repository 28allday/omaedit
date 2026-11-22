#include "beatmapper.h"
#include "audiosamplesource.h"
#include "beatmap.h"
#include "renderrequest.h"
#include "thumbnaildiskcache.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRunnable>
#include <QUuid>
#include <cmath>

BeatMapper::BeatMapper(QObject *parent) : QObject(parent) {
    m_pool.setMaxThreadCount(1); // one file at a time: each uses several threads itself
}

BeatMapper::~BeatMapper() {
    for (const auto &job : std::as_const(m_jobs)) job.stop->store(true);
    m_pool.clear();
    m_pool.waitForDone();
}

QString BeatMapper::cacheKey(const QString &path) {
    const QFileInfo file(path);
    // v2: the map has the bar. A map kept by an earlier version has none, and is made again.
    return QStringLiteral("beats-v2|%1|%2|%3").arg(file.canonicalFilePath()).arg(file.size())
        .arg(file.lastModified().toMSecsSinceEpoch());
}

QVariantMap BeatMapper::analyse(const QString &path, const std::function<bool()> &cancelled,
                                const std::function<void(int)> &progress, QString *error, int threads, bool *fromCache) {
    const auto fail = [&](const QString &why) {
        if (error) *error = why;
        return QVariantMap();
    };
    if (fromCache) *fromCache = false;
    const QFileInfo file(path);
    if (!file.isFile()) return fail(tr("The file is missing."));
    const QString key = cacheKey(path);
    const auto stamp = [&](QVariantMap result) {
        result[QStringLiteral("size")] = file.size();
        result[QStringLiteral("modified")] = file.lastModified().toMSecsSinceEpoch();
        return result;
    };
    // Mapped before, and the file is as it was: that answer.
    if (const QByteArray kept = ThumbnailDiskCache::kept(key, "beats"); !kept.isEmpty()) {
        const auto saved = QJsonDocument::fromJson(kept).object().toVariantMap();
        if (saved.contains(QStringLiteral("beats")) && saved.contains(QStringLiteral("bar")) && saved.value(QStringLiteral("duration")).toDouble() > 0) {
            if (fromCache) *fromCache = true;
            if (progress) progress(100);
            return stamp(saved);
        }
    }
    const auto stop = [&] { return cancelled && cancelled(); };
    auto request = std::make_shared<RenderRequest>(stop);
    RenderScope scope(request);
    QString problem;
    AudioSampleSource::Options options;
    options.cacheByteLimit = 64 * 1024 * 1024;
    const auto reader = AudioSampleSource::Reader::open(path, options, &problem);
    if (!reader) return fail(problem.isEmpty() ? tr("The file has no sound that can be read.") : problem);
    const int rate = BeatMap::kRate;
    const qint64 total = reader->sampleCount(rate);
    if (total <= 0) return fail(tr("The file has no sound that can be read."));
    // One channel, a few seconds at a time: a long recording is never held whole.
    BeatMap::Analysis analysis(rate, threads);
    const int block = rate * 4;
    int said = -1;
    for (qint64 at = 0; at < total; at += block) {
        if (stop()) return fail({});
        const int count = int(std::min<qint64>(block, total - at));
        const QVector<float> samples = reader->read(at, count, rate, 1, &problem);
        if (stop()) return fail({});
        if (samples.size() != count) return fail(problem.isEmpty() ? tr("The file's sound could not be read to its end.") : problem);
        analysis.push(samples.constData(), size_t(count));
        // The reading is nearly all of the work; the last few per cent are the beats themselves.
        const int percent = int(95 * (at + count) / total);
        if (progress && percent != said) progress(said = percent);
    }
    const BeatMap::Result found = analysis.finish(stop);
    if (found.cancelled || stop()) return fail({});
    QVariantList beats;
    beats.reserve(qsizetype(found.beats.size()));
    // To a tenth of a millisecond: finer than a sample at any rate in use.
    for (const double beat : found.beats) beats.append(std::round(beat * 10000.) / 10000.);
    QVariantMap result{{QStringLiteral("bpm"), std::round(found.bpm * 100.) / 100.},
                       {QStringLiteral("beats"), beats},
                       {QStringLiteral("duration"), double(total) / rate},
                       {QStringLiteral("strength"), std::round(found.strength * 1000.) / 1000.},
                       // The bar: how many beats, and which of `beats` is the first beat of one.
                       {QStringLiteral("bar"), QVariantMap{{QStringLiteral("beats"), found.beatsPerBar},
                                                           {QStringLiteral("first"), found.firstBar},
                                                           {QStringLiteral("strength"), std::round(found.barStrength * 1000.) / 1000.}}}};
    ThumbnailDiskCache::keep(key, "beats", QJsonDocument(QJsonObject::fromVariantMap(result)).toJson(QJsonDocument::Compact));
    if (progress) progress(100);
    return stamp(result);
}

bool BeatMapper::map(const QString &path) {
    if (m_jobs.contains(path) || !QFileInfo(path).isFile()) return false;
    Job job;
    job.stop = std::make_shared<std::atomic_bool>(false);
    job.row = {{"id", QStringLiteral("beats:") + QUuid::createUuid().toString(QUuid::WithoutBraces)},
               {"token", path}, {"name", QFileInfo(path).fileName()}, {"type", tr("Beat map")},
               {"state", m_jobs.isEmpty() ? "running" : "queued"}, {"percent", 0}, {"canCancel", true}, {"message", path}};
    m_jobs.insert(path, job);
    emit taskChanged(job.row);
    QPointer<BeatMapper> owner(this);
    const auto stop = job.stop;
    const auto yielding = m_yielding;
    m_pool.start(QRunnable::create([owner, stop, yielding, path] {
        const auto say = [&](const QVariantMap &fields) {
            QMetaObject::invokeMethod(qApp, [owner, path, fields, stop] {
                if (!owner || !owner->m_jobs.contains(path) || owner->m_jobs[path].stop != stop) return;
                auto &row = owner->m_jobs[path].row;
                for (auto it = fields.cbegin(); it != fields.cend(); ++it) row[it.key()] = it.value();
                emit owner->taskChanged(row);
            }, Qt::QueuedConnection);
        };
        if (stop->load()) return; // cancelled while it waited: its row says so already
        say({{"state", "running"}});
        QString error;
        const QVariantMap result = analyse(path, [stop] { return stop->load(); },
                                           [&](int percent) { say({{"percent", percent}}); }, &error, yielding->load() ? 1 : 0);
        QMetaObject::invokeMethod(qApp, [owner, path, result, error, stop] {
            if (!owner || !owner->m_jobs.contains(path) || owner->m_jobs[path].stop != stop) return;
            auto row = owner->m_jobs.take(path).row;
            const bool cancelled = stop->load();
            const bool none = !cancelled && error.isEmpty() && result.value("beats").toList().isEmpty();
            row["state"] = cancelled ? "cancelled" : !error.isEmpty() ? "failed" : "completed";
            row["percent"] = cancelled || !error.isEmpty() ? row.value("percent") : QVariant(100);
            row["canCancel"] = false;
            row["message"] = !error.isEmpty() ? error
                             : none ? tr("No steady beat was found.")
                             : cancelled ? QString()
                             : tr("%1 BPM, %n beat(s)", nullptr, int(result.value("beats").toList().size()))
                                   .arg(QString::number(result.value("bpm").toDouble(), 'f', 1));
            emit owner->taskChanged(row);
            emit owner->finished(path, cancelled || !error.isEmpty() ? QVariantMap() : result, error, cancelled);
        }, Qt::QueuedConnection);
    }));
    return true;
}

void BeatMapper::cancel(const QString &path) {
    const auto it = m_jobs.find(path);
    if (it == m_jobs.end()) return;
    it->stop->store(true);
    auto row = it->row;
    const auto stop = it->stop;
    m_jobs.erase(it);
    row["state"] = "cancelled";
    row["canCancel"] = false;
    emit taskChanged(row);
    emit finished(path, {}, {}, true);
}
