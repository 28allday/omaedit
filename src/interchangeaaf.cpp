#include "interchange.h"
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QMutex>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <atomic>

// AAF goes through a bundled Python helper on pyaaf2 (Arch: python-aaf2),
// speaking the document's JSON form. Without the module the format reports
// itself unavailable rather than failing halfway.
namespace Interchange {
namespace {
thread_local QStringList g_lastNotes; // notes belong to the calling writer, including concurrent exports
QString python() { return QStandardPaths::findExecutable(QStringLiteral("python3")); }
bool runHelper(const QStringList &args, QString *error, const std::atomic_bool *cancel, int timeoutMs = 120000) {
    if (cancel && cancel->load()) { if (error) *error = QStringLiteral("Cancelled."); return false; }
    QTemporaryDir dir;
    QFile script(QStringLiteral(":/tools/omaedit-aaf.py"));
    const QString path = dir.filePath(QStringLiteral("omaedit-aaf.py"));
    if (!script.open(QIODevice::ReadOnly) || !QFile::exists(dir.path())) { if (error) *error = QStringLiteral("The AAF helper is missing from this build."); return false; }
    QFile out(path); if (!out.open(QIODevice::WriteOnly)) { if (error) *error = out.errorString(); return false; }
    out.write(script.readAll()); out.close();
    QProcess process;
    process.start(python(), QStringList{path} + args);
    if (!process.waitForStarted(5000)) { if (error) *error = QStringLiteral("The AAF helper did not start: %1").arg(process.errorString()); return false; }
    // Short waits, so a cancel is honoured within a tenth of a second.
    QElapsedTimer clock; clock.start();
    while (!process.waitForFinished(100)) {
        if (process.state() == QProcess::NotRunning) break;
        const bool cancelled = cancel && cancel->load();
        // A large AAF can take pyaaf2 longer than any fixed time. A caller
        // that can stop the helper decides how long to wait; only one that
        // cannot is protected by the limit.
        if (!cancelled && (cancel || clock.elapsed() < timeoutMs)) continue;
        process.kill(); process.waitForFinished(2000);
        if (error) *error = cancelled ? QStringLiteral("Cancelled.") : QStringLiteral("The AAF helper did not finish in time.");
        return false;
    }
    if (cancel && cancel->load()) { if (error) *error = QStringLiteral("Cancelled."); return false; }
    if (process.exitStatus() != QProcess::NormalExit) { if (error) *error = QStringLiteral("The AAF helper stopped unexpectedly."); return false; }
    if (process.exitCode() != 0) { if (error) *error = QString::fromUtf8(process.readAllStandardError()).trimmed().section(QLatin1Char('\n'), -1); return false; }
    return true;
}
}
namespace {
QMutex g_probeMutex;                // held while the probe runs: a second asker waits for its answer
std::atomic_int g_probeKnown{-1};   // -1 not looked yet, 0 missing, 1 there
QString g_probeReason;
QElapsedTimer g_probeGaveUp;        // when a probe last ran out of time
}
void forgetAvailability() { QMutexLocker lock(&g_probeMutex); g_probeKnown = -1; g_probeReason.clear(); g_probeGaveUp.invalidate(); }
bool availabilityKnown(const QString &format) { return format != QStringLiteral("aaf") || g_probeKnown.load() >= 0; }
bool available(const QString &format, QString *why) {
    if (format != QStringLiteral("aaf")) return true;
    // Probed once: menus ask on every open, and the answer is a process launch.
    QMutexLocker lock(&g_probeMutex);
    if (g_probeKnown < 0) {
        const QString missing = QStringLiteral("AAF needs the pyaaf2 Python module (python-aaf2).");
        // A probe that ran out of time says nothing about the module: the
        // answer is not kept, and the next ask after a while looks again.
        if (g_probeGaveUp.isValid() && g_probeGaveUp.elapsed() < 30000) { if (why) *why = missing; return false; }
        if (python().isEmpty()) { g_probeKnown = 0; g_probeReason = QStringLiteral("AAF needs python3 with the pyaaf2 module (python-aaf2)."); }
        else {
            QProcess probe; probe.start(python(), {QStringLiteral("-c"), QStringLiteral("import aaf2")});
            if (!probe.waitForFinished(10000)) {
                probe.kill(); probe.waitForFinished(2000);
                g_probeGaveUp.start();
                if (why) *why = missing;
                return false;
            }
            g_probeKnown = probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0 ? 1 : 0;
            if (!g_probeKnown) g_probeReason = missing;
        }
    }
    if (!g_probeKnown && why) *why = g_probeReason;
    return g_probeKnown == 1;
}
bool writeAaf(const Document &document, const QString &path, QString *error, const std::atomic_bool *cancel) {
    g_lastNotes.clear();
    if (error) error->clear();
    const auto fail = [&](const QString &reason) { if (error) *error = reason; return false; };
    const auto cancelled = [&] { return cancel && cancel->load(); };
    if (cancelled()) return fail(QStringLiteral("Cancelled."));
    if (requiresMappedTiming(document, error)) return false;
    QString why; if (!available(QStringLiteral("aaf"), &why)) return fail(why);
    QTemporaryDir dir;
    if (!dir.isValid()) return fail(QStringLiteral("Could not create temporary files for the AAF export."));
    const QString json = dir.filePath(QStringLiteral("timeline.json"));
    QFile file(json); if (!file.open(QIODevice::WriteOnly)) return fail(file.errorString());
    const auto data = QJsonDocument::fromVariant(toVariant(document)).toJson(QJsonDocument::Compact);
    if (file.write(data) != data.size() || !file.flush()) return fail(file.errorString());
    file.close();
    // The helper owns a private file. QSaveFile publishes a complete document
    // atomically; failed or cancelled copies preserve the previous destination.
    QSaveFile target(path);
    target.setDirectWriteFallback(false);
    if (!target.open(QIODevice::WriteOnly)) return fail(target.errorString());
    const QString staged = dir.filePath(QStringLiteral("timeline.aaf"));
    if (!runHelper({QStringLiteral("write"), json, staged}, error, cancel)) return false;
    QFile source(staged); if (!source.open(QIODevice::ReadOnly)) return fail(source.errorString());
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    for (;;) {
        if (cancelled()) return fail(QStringLiteral("Cancelled."));
        const qint64 count = source.read(buffer.data(), buffer.size());
        if (count < 0) return fail(source.errorString());
        if (!count) break;
        if (target.write(buffer.constData(), count) != count) return fail(target.errorString());
    }
    if (cancelled()) return fail(QStringLiteral("Cancelled."));
    if (!target.commit()) return fail(target.errorString());
    QFile notes(json + QStringLiteral(".notes"));
    if (notes.open(QIODevice::ReadOnly))
        g_lastNotes = QJsonDocument::fromJson(notes.readAll()).toVariant().toMap().value(QStringLiteral("notes")).toStringList();
    return true;
}
QStringList lastWriteNotes() { return g_lastNotes; }

bool readAaf(const QString &path, Document *document, QString *error, const std::atomic_bool *cancel) {
    QString why; if (!available(QStringLiteral("aaf"), &why)) { if (error) *error = std::move(why); return false; }
    QTemporaryDir dir; const QString json = dir.filePath(QStringLiteral("timeline.json"));
    if (!runHelper({QStringLiteral("read"), path, json}, error, cancel)) return false;
    QFile file(json); if (!file.open(QIODevice::ReadOnly)) { if (error) *error = file.errorString(); return false; }
    QJsonParseError parse; const auto doc = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError) { if (error) *error = parse.errorString(); return false; }
    *document = fromVariant(doc.toVariant().toMap());
    if (document->tracks.isEmpty()) { if (error) *error = QStringLiteral("The AAF holds no timeline."); return false; }
    return true;
}
} // namespace Interchange
