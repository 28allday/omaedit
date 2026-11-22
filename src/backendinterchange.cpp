#include "backend_p.h"

// ── Timeline interchange ──────────────────────────────────────────────────
QStringList Backend::timelineFormats() const { return Interchange::formats(); }
QString Backend::timelineFormatName(const QString &format) const { return Interchange::formatName(format); }
bool Backend::timelineFormatAvailable(const QString &format) const { return Interchange::available(format); }

void Backend::importTimelineDialog() {
    if (m_exporting) return;
    m_chooserMode = ChooserMode::ImportTimeline;
    m_chooser->openFile(QStringLiteral("Import Timeline"), QStringLiteral("Timelines"),
                        {QStringLiteral("*.otio"), QStringLiteral("*.edl"), QStringLiteral("*.fcpxml"), QStringLiteral("*.xml"), QStringLiteral("*.aaf")});
}

// The open timeline's rate as a fraction: the project's own when custom, else the nearest broadcast rate.
QPair<int, int> Backend::timelineRate() const {
    int fpsNum = m_projectSettings.value(QStringLiteral("fpsNum")).toInt(), fpsDen = m_projectSettings.value(QStringLiteral("fpsDen")).toInt();
    if (!m_projectSettings.value(QStringLiteral("custom")).toBool() || fpsNum <= 0 || fpsDen <= 0) {
        struct R { double fps; int num, den; };
        static const R known[] = {{23.976, 24000, 1001}, {24, 24, 1}, {25, 25, 1}, {29.97, 30000, 1001}, {30, 30, 1}, {48, 48, 1}, {50, 50, 1}, {59.94, 60000, 1001}, {60, 60, 1}};
        fpsNum = qRound(fps() * 1000); fpsDen = 1000;
        for (const R &r : known) if (qAbs(r.fps - fps()) < 0.002) { fpsNum = r.num; fpsDen = r.den; }
    }
    return {fpsNum, fpsDen};
}

QString Backend::importTimeline(const QUrl &url) {
    if (m_exporting) return tr("Wait for the current export to finish.");
    const QString path = url.toLocalFile();
    const QString format = Interchange::formatOf(path);
    if (format.isEmpty()) { const auto problem = tr("%1 is not a timeline OmaEdit reads (.otio, .edl, .fcpxml, .xml, .aaf).").arg(QFileInfo(path).fileName()); setStatus(problem); return problem; }
    QString why;
    if (!Interchange::available(format, &why)) { setStatus(why); return why; }
    m_timelineImportRateNote.clear();
    if (format == QStringLiteral("aaf")) {
        if (m_aafCancel) return tr("Wait for the current AAF task to finish.");
        beginAafTask(tr("Import AAF timeline"), QFileInfo(path).fileName());
        setStatus(tr("Reading %1…").arg(QFileInfo(path).fileName()));
        const auto cancel = m_aafCancel;
        const int generation = m_mediaGeneration;
        m_interchangePool->start(QRunnable::create([this, path, cancel, generation] {
            Interchange::Document document; QString error;
            const bool ok = Interchange::readAaf(path, &document, &error, cancel.get());
            const QVariantMap read = ok ? Interchange::toVariant(document) : QVariantMap();
            QMetaObject::invokeMethod(this, [this, ok, read, error, path, cancel, generation] {
                if (generation != m_mediaGeneration || m_projectLoadPending) {
                    endAafTask(QStringLiteral("cancelled"), tr("The project changed before the import finished."));
                    return;
                }
                const QString file = QFileInfo(path).fileName();
                if (cancel->load()) { endAafTask(QStringLiteral("cancelled"), tr("Cancelled")); setStatus(tr("Import of %1 cancelled").arg(file)); return; }
                if (!ok) { const auto problem = tr("Could not read %1: %2").arg(file, error); endAafTask(QStringLiteral("failed"), problem); setStatus(problem); emit failed(problem); return; }
                endAafTask(QStringLiteral("completed"), file);
                continueTimelineImport(read, QFileInfo(path).completeBaseName());
            }, Qt::QueuedConnection);
        }));
        return QString();
    }
    Interchange::Document document; QString error;
    // An EDL names no frame rate: the open timeline's is taken, or 25 with no timeline that has one.
    const bool edl = format == QStringLiteral("edl"), open = fps() > 0;
    const QPair<int, int> rate = edl && open ? timelineRate() : QPair<int, int>{25, 1};
    if (!Interchange::read(path, format, &document, &error, rate.first, rate.second)) { const auto problem = tr("Could not read %1: %2").arg(QFileInfo(path).fileName(), error); setStatus(problem); emit failed(problem); return problem; }
    if (edl) {
        const QString read = QString::number(document.fps(), 'f', 3).remove(QRegularExpression(QStringLiteral("\\.?0+$")));
        m_timelineImportRateNote = document.fpsNum != rate.first || document.fpsDen != rate.second
            ? tr("The EDL says drop frame and names no frame rate: it was read at %1 fps.").arg(read)
            : open ? tr("The EDL names no frame rate: it was read at %1 fps, the open timeline's.").arg(read)
                   : tr("The EDL names no frame rate: it was read at %1 fps, as no timeline with a rate is open.").arg(read);
    }
    continueTimelineImport(Interchange::toVariant(document), QFileInfo(path).completeBaseName());
    return QString();
}

void Backend::beginAafTask(const QString &name, const QString &file) {
    m_aafCancel = std::make_shared<std::atomic_bool>(false);
    m_aafTaskId = QStringLiteral("interchange:") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_backgroundTasks->update({{"id", m_aafTaskId}, {"name", name}, {"project", projectName()}, {"type", tr("Timeline")},
                               {"state", "running"}, {"percent", -1}, {"message", file}, {"canCancel", true}});
}

void Backend::endAafTask(const QString &state, const QString &message) {
    auto row = m_backgroundTasks->job(m_aafTaskId);
    row["state"] = state; row["message"] = message; row["canCancel"] = false;
    if (state == QStringLiteral("completed")) row["percent"] = 100;
    m_backgroundTasks->update(row);
    m_aafTaskId.clear(); m_aafCancel.reset();
}

void Backend::continueTimelineImport(const QVariantMap &read, const QString &fallbackName) {
    const Interchange::Document document = Interchange::fromVariant(read);
    m_timelineImportDocument = read;
    m_timelineImportName = document.name.isEmpty() ? fallbackName : document.name;
    // Media the library does not hold yet is imported first; files that are
    // gone still place, offline, for relinking.
    QStringList missing;
    for (const Interchange::Track &track : document.tracks) for (const Interchange::Item &item : track.items) {
        if (item.path.isEmpty() || missing.contains(item.path) || !QFileInfo::exists(item.path)) continue;
        bool known = false;
        for (const QVariant &v : m_library) if (v.toMap().value(QStringLiteral("path")).toString() == item.path && !v.toMap().value(QStringLiteral("offline")).toBool()) known = true;
        if (!known) missing.append(item.path);
    }
    if (missing.isEmpty()) { placeImportedTimeline(); return; }
    m_timelineImportTask = startMediaTask(QStringLiteral("import"), missing, {});
    if (m_timelineImportTask.isEmpty()) { placeImportedTimeline(); return; }
    setStatus(tr("Importing %1 media files for “%2”…").arg(missing.size()).arg(m_timelineImportName));
}

void Backend::placeImportedTimeline() {
    const Interchange::Document document = Interchange::fromVariant(m_timelineImportDocument);
    m_timelineImportDocument.clear(); m_timelineImportTask.clear();
    const double rate = fps() > 0 ? fps() : document.fps();
    const Interchange::Placement placement = Interchange::toEntries(document, rate, m_library);
    if (!placement.error.isEmpty()) { setStatus(placement.error); emit failed(placement.error); return; }
    QStringList problems = placement.problems + document.notes;
    if (placement.entries.isEmpty()) { const auto problem = tr("“%1” holds no clips to place.").arg(m_timelineImportName); setStatus(problem); emit timelineImported(m_timelineImportName, problems); return; }
    auto format = m_timelineFormat.isEmpty() ? m_projectSettings : m_timelineFormat;
    if (fps() <= 0) {
        format["custom"] = true;
        format["width"] = document.width; format["height"] = document.height;
        format["fpsNum"] = document.fpsNum; format["fpsDen"] = document.fpsDen;
        format["matchedProfile"] = QVariantMap();
    }
    // The file's own starting timecode, counted at the file's rate: not the
    // start of whichever timeline happens to be open.
    format["startFrame"] = int(qBound(qint64(0), document.startTimecodeFrames, qint64(std::numeric_limits<int>::max())));
    format["startRate"] = qRound(document.fps());
    emit importSequenceRequested(m_timelineImportName, placement.entries, placement.trackNames, placement.markers, format, problems);
}

void Backend::exportTimelineDialog(const QString &format) {
    if (!hasVideo() || m_exporting || !Interchange::formats().contains(format)) return;
    QString why;
    if (!Interchange::available(format, &why)) { setStatus(why); return; }
    m_chooserMode = ChooserMode::ExportTimeline; m_exportTimelineFormat = format;
    m_chooser->saveFile(QStringLiteral("Export Timeline"), projectName() + QLatin1Char('.') + (format == QStringLiteral("xmeml") ? QStringLiteral("xml") : format),
                        Interchange::formatName(format), Interchange::formatPatterns(format));
}

QString Backend::exportTimeline(const QUrl &url, const QString &format) {
    if (!hasVideo()) return tr("Nothing to export: the sequence is empty.");
    std::function<bool(const QVariantList &)> hasSpeedMap=[&](const QVariantList &list) {
        for(const auto &v:list){const auto e=v.toMap();if(e.contains(ClipKey::retime))return true;
            if(hasSpeedMap(QJsonDocument::fromJson(e.value(ClipKey::compoundJson).toString().toUtf8()).toVariant().toList()))return true;}
        return false;
    };
    if(hasSpeedMap(m_timeline) || hasSpeedMap(m_connected)) {
        const auto error=tr("Speed segments cannot be carried by timeline interchange yet. Render these clips in place, or save the native project.");
        setStatus(error);return error;
    }
    if (!Interchange::formats().contains(format)) return tr("Unknown timeline format.");
    QString why;
    if (!Interchange::available(format, &why)) { setStatus(why); return why; }
    QString path = url.toLocalFile();
    const QString suffix = format == QStringLiteral("xmeml") ? QStringLiteral("xml") : format;
    if (!path.endsWith(QLatin1Char('.') + suffix, Qt::CaseInsensitive)) path += QLatin1Char('.') + suffix;
    const auto [fpsNum, fpsDen] = timelineRate();
    QVariantMap project{{"name", projectName()}, {"fpsNum", fpsNum}, {"fpsDen", fpsDen}, {"width", videoWidth()}, {"height", videoHeight()}, {"startFrame", startFrame()}};
    const Interchange::Document document = Interchange::fromTimeline(m_timeline, m_connected, m_tracks, m_markers, project, m_library);
    if (format == QStringLiteral("aaf")) {
        if (m_aafCancel) return tr("Wait for the current AAF task to finish.");
        beginAafTask(tr("Export AAF timeline"), QFileInfo(path).fileName());
        setStatus(tr("Writing %1…").arg(QFileInfo(path).fileName()));
        const auto cancel = m_aafCancel;
        m_interchangePool->start(QRunnable::create([this, document, path, cancel] {
            QString error;
            const bool ok = Interchange::writeAaf(document, path, &error, cancel.get());
            const QStringList notes = ok ? document.notes + Interchange::lastWriteNotes() : QStringList();
            QMetaObject::invokeMethod(this, [this, ok, error, notes, path, cancel] {
                const QString file = QFileInfo(path).fileName();
                if (!ok && cancel->load()) { endAafTask(QStringLiteral("cancelled"), tr("Cancelled")); setStatus(tr("Export of %1 cancelled").arg(file)); return; }
                if (!ok) { const auto problem = tr("Could not write %1: %2").arg(file, error); endAafTask(QStringLiteral("failed"), problem); setStatus(problem); emit failed(problem); return; }
                endAafTask(QStringLiteral("completed"), path);
                setStatus(notes.isEmpty() ? tr("Exported %1 (%2)").arg(file, Interchange::formatName(QStringLiteral("aaf")))
                                          : tr("Exported %1 (%2); %3 items could not be carried").arg(file, Interchange::formatName(QStringLiteral("aaf"))).arg(notes.size()));
            }, Qt::QueuedConnection);
        }));
        return QString();
    }
    QString error;
    if (!Interchange::write(document, path, format, &error)) { const auto problem = tr("Could not write %1: %2").arg(QFileInfo(path).fileName(), error); setStatus(problem); emit failed(problem); return problem; }
    const QStringList notes = document.notes + (format == QStringLiteral("aaf") ? Interchange::lastWriteNotes() : QStringList());
    setStatus(notes.isEmpty() ? tr("Exported %1 (%2)").arg(QFileInfo(path).fileName(), Interchange::formatName(format))
                              : tr("Exported %1 (%2); %3 items could not be carried").arg(QFileInfo(path).fileName(), Interchange::formatName(format)).arg(notes.size()));
    return QString();
}
