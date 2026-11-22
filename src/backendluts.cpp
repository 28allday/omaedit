#include "backend.h"
#include "colourpipeline.h"
#include "filepicker.h"
#include "lutfilter.h"
#include "playerworker.h"
#include "thumbnailprovider.h"
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QSet>
#include <algorithm>

void Backend::setupLuts() {
    m_lutLibrary = new LutLibrary(this);
    connect(this, &Backend::setNodeLutRequested, m_worker, &PlayerWorker::setNodeLut);
    connect(m_worker, &PlayerWorker::nodeLutFinished, this, [this](bool changed, const QString &message) {
        setStatus(message); emit lutApplyFinished(changed, message);
    });
    connect(this, &Backend::previewNodeLutRequested, m_worker, &PlayerWorker::previewNodeLut);
    connect(m_worker, &PlayerWorker::lutPreviewReady, this,
            [this](int request, const QImage &before, const QImage &after, const QString &error) {
        m_lutPreviewBusy = false;
        if (request == m_lutPreviewSerial) {
            m_lutPreview.insert(QStringLiteral("loading"), false);
            m_lutPreview.insert(QStringLiteral("ready"), error.isEmpty());
            m_lutPreview.insert(QStringLiteral("error"), error);
            if (m_thumbs && error.isEmpty()) {
                m_thumbs->insert(QStringLiteral("lut:before"), before);
                m_thumbs->insert(QStringLiteral("lut:after"), after);
                m_lutPreview.insert(QStringLiteral("before"), QStringLiteral("image://thumbs/lut:before?%1").arg(request));
                m_lutPreview.insert(QStringLiteral("after"), QStringLiteral("image://thumbs/lut:after?%1").arg(request));
            }
            emit lutPreviewChanged();
        }
        sendPendingLutPreview();
    });
    connect(this, &Backend::projectChanged, this, &Backend::clearLutPreview);
}

void Backend::chooseLutFile() {
    m_chooserMode = ChooserMode::LutFile;
    m_chooser->openFile(QStringLiteral("Locate LUT"), QStringLiteral("3D LUTs"),
                       {QStringLiteral("*.cube"), QStringLiteral("*.CUBE"), QStringLiteral("*.3dl"), QStringLiteral("*.3DL")});
}

void Backend::setNodeLut(int track, int index, int node, const QString &nodeId, const QString &path, double intensity) {
    if (m_exporting) {
        const QString message = QStringLiteral("Cannot change LUTs while exporting.");
        setStatus(message); emit lutApplyFinished(false, message); return;
    }
    emit setNodeLutRequested(track, index, node, nodeId, path, intensity);
}

void Backend::requestLutPreview(int track, int index, int node, const QString &nodeId, int frame, const QString &path, double intensity) {
    ++m_lutPreviewSerial;
    m_pendingLutPreview = {{QStringLiteral("track"), track}, {QStringLiteral("index"), index},
                           {QStringLiteral("node"), node}, {QStringLiteral("nodeId"), nodeId},
                           {QStringLiteral("frame"), frame}, {QStringLiteral("path"), path},
                           {QStringLiteral("intensity"), intensity}};
    m_lutPreview = {{QStringLiteral("loading"), true}, {QStringLiteral("path"), path}, {QStringLiteral("intensity"), intensity}};
    emit lutPreviewChanged();
    sendPendingLutPreview();
}

void Backend::sendPendingLutPreview() {
    if (m_lutPreviewBusy || m_pendingLutPreview.isEmpty()) return;
    const QVariantMap pending = m_pendingLutPreview;
    m_pendingLutPreview.clear();
    m_lutPreviewBusy = true;
    emit previewNodeLutRequested(m_lutPreviewSerial, pending.value(QStringLiteral("track")).toInt(),
        pending.value(QStringLiteral("index")).toInt(), pending.value(QStringLiteral("node")).toInt(),
        pending.value(QStringLiteral("nodeId")).toString(), pending.value(QStringLiteral("frame")).toInt(),
        pending.value(QStringLiteral("path")).toString(), pending.value(QStringLiteral("intensity")).toDouble());
}

void Backend::clearLutPreview() {
    ++m_lutPreviewSerial;
    m_pendingLutPreview.clear();
    m_lutPreview.clear();
    if (m_thumbs) { m_thumbs->remove(QStringLiteral("lut:before")); m_thumbs->remove(QStringLiteral("lut:after")); }
    emit lutPreviewChanged();
}
