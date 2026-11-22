#include "backend.h"
#include "clipentry.h"
#include "colourpipeline.h"
#include "colormaskservice.h"
#include "filepicker.h"
#include "gradeengine.h"
#include "playerworker.h"
#include "stills.h"
#include <QFile>
#include <QImageReader>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSettings>
#include <QUuid>

QVariantMap Backend::colourPipelineInfo() const { return ColourPipeline::info(); }

void Backend::setupColorTools() {
    m_maskProvider = QSettings().value("color/maskProvider").toMap();
    m_maskService = new ColorMaskService(this);
    connect(this, &Backend::prepareColorMaskRequested, m_worker, &PlayerWorker::prepareColorMask);
    connect(this, &Backend::applyColorMaskRequested, m_worker, &PlayerWorker::applyColorMask);
    connect(m_worker, &PlayerWorker::colorMaskFrameReady, this,
            [this](int request, const QImage &image, const QVariantMap &target, const QString &error) {
                if (request != m_maskSerial)
                    return;
                if (!error.isEmpty()) {
                    m_maskJob.clear();
                    emit maskJobChanged();
                    setStatus(error);
                    return;
                }
                m_maskTarget = target;
                auto settings = m_maskProvider;
                settings["frame"] = m_maskRequest.value("frame");
                m_maskService->start(image, settings, m_maskRequest.value("kind").toString(),
                                     m_maskRequest.value("prompt").toString());
            });
    connect(m_maskService, &ColorMaskService::progress, this, [this](int percent, const QString &message) {
        m_maskJob = {{"percent", percent}, {"message", message}};
        emit maskJobChanged();
    });
    connect(m_maskService, &ColorMaskService::finished, this, [this](const QString &path, const QString &error) {
        m_maskJob.clear();
        emit maskJobChanged();
        if (!error.isEmpty())
            setStatus(error);
        else if (!m_maskTarget.isEmpty())
            emit applyColorMaskRequested(m_maskTarget, path);
        m_maskTarget.clear();
    });
    connect(this, &Backend::projectChanged, this, &Backend::cancelColorMask);
    connect(this, &Backend::shareColorRequested, m_worker, &PlayerWorker::shareColor);
    m_nodePresets = QSettings().value(QStringLiteral("color/nodePresets")).toList();
    connect(this, &Backend::colorAssetChosen, this, [this](const QString &action, const QString &path) {
        QString error;
        if (action == "exportStill") {
            if (m_stills->exportStill(m_colorAssetId, path, &error))
                setStatus(tr("Still exported with grade metadata"));
            else
                setStatus(error);
        } else if (action == "importStill") {
            const QString id = m_stills->importStill(path, &error);
            setStatus(id.isEmpty() ? error : QStringLiteral("Still and grade imported"));
        } else if (action == "exportPreset") {
            QVariantMap preset;
            for (const QVariant &v : m_nodePresets)
                if (v.toMap().value("id") == m_colorAssetId)
                    preset = v.toMap();
            QSaveFile file(path);
            const QByteArray data =
                QJsonDocument::fromVariant(QVariantMap{{"format", "omaedit.node.v1"}, {"preset", preset}}).toJson();
            setStatus(!preset.isEmpty() && file.open(QIODevice::WriteOnly) && file.write(data) == data.size() &&
                              file.commit()
                          ? QStringLiteral("Node preset exported")
                          : QStringLiteral("Could not export the node preset."));
        } else if (action == "importPreset") {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) {
                setStatus(tr("Choose a layer preset up to 4 MiB."));
                return;
            }
            const QVariantMap data = QJsonDocument::fromJson(file.readAll()).toVariant().toMap(),
                              preset = data.value("preset").toMap();
            if (data.value("format") != "omaedit.node.v1") {
                setStatus(tr("This is not an OmaEdit layer preset."));
                return;
            }
            saveNodePreset(preset.value("name").toString(), preset.value("node").toMap());
        }
    });
    connect(this, &Backend::effectPreviewRequested, m_worker, &PlayerWorker::effectPreview);
    connect(m_worker, &PlayerWorker::effectPreviewReady, this,
            [this](int request, const QImage &image, const QString &error) {
                m_effectPreviewBusy = false;
                if (request == m_effectPreviewSerial) {
                    m_effectPreview = image;
                    emit effectPreviewChanged();
                    if (!error.isEmpty())
                        setStatus(error);
                }
                sendEffectPreview();
            });
    connect(this, &Backend::projectChanged, this, &Backend::clearEffectPreview);
    connect(this, &Backend::colorMatteRequested, m_worker, &PlayerWorker::colorMatte);
    connect(m_worker, &PlayerWorker::colorMatteReady, this,
            [this](int request, const QImage &image, const QString &error) {
                m_colorMatteBusy = false;
                if (request == m_colorMatteSerial) {
                    m_colorMatte = image;
                    emit colorMatteChanged();
                    if (!error.isEmpty())
                        setStatus(error);
                }
                sendColorMatte();
            });
    connect(this, &Backend::projectChanged, this, &Backend::clearColorMatte);
}
void Backend::requestEffectPreview(int track, int index, int frame, bool matte, int width, int height) {
    ++m_effectPreviewSerial;
    m_pendingEffectPreview = {{"track", track}, {"index", index}, {"frame", frame}, {"matte", matte}, {"size", QSize(width, height)}};
    sendEffectPreview();
}
void Backend::sendEffectPreview() {
    if (m_effectPreviewBusy || m_pendingEffectPreview.isEmpty())
        return;
    const QVariantMap p = m_pendingEffectPreview;
    m_pendingEffectPreview.clear();
    m_effectPreviewBusy = true;
    emit effectPreviewRequested(m_effectPreviewSerial, p.value("track").toInt(), p.value("index").toInt(), p.value("frame").toInt(),
                                p.value("matte").toBool(), p.value("size").toSize());
}
void Backend::clearEffectPreview() {
    ++m_effectPreviewSerial;
    m_pendingEffectPreview.clear();
    m_effectPreview = {};
    emit effectPreviewChanged();
}
void Backend::requestColorMatte(int track, int index, const QString &nodeId, int frame) {
    ++m_colorMatteSerial;
    m_pendingColorMatte = {{"track", track}, {"index", index}, {"node", nodeId}, {"frame", frame}};
    sendColorMatte();
}
void Backend::sendColorMatte() {
    if (m_colorMatteBusy || m_pendingColorMatte.isEmpty())
        return;
    const QVariantMap p = m_pendingColorMatte;
    m_pendingColorMatte.clear();
    m_colorMatteBusy = true;
    emit colorMatteRequested(m_colorMatteSerial, p.value("track").toInt(), p.value("index").toInt(),
                             p.value("node").toString(), p.value("frame").toInt());
}
void Backend::clearColorMatte() {
    ++m_colorMatteSerial;
    m_pendingColorMatte.clear();
    m_colorMatte = {};
    emit colorMatteChanged();
}

void Backend::chooseColorAsset(const QString &action, const QString &id) {
    m_colorAssetAction = action;
    m_colorAssetId = id;
    m_chooserMode = ChooserMode::ColorAsset;
    if (action == QStringLiteral("matte"))
        m_chooser->openFile(
            QStringLiteral("External matte"), QStringLiteral("Matte images"),
            {QStringLiteral("*.png"), QStringLiteral("*.tif"), QStringLiteral("*.tiff"), QStringLiteral("*.jpg")});
    else if (action == "importStill")
        m_chooser->openFile(QStringLiteral("Import still and grade"), QStringLiteral("OmaEdit stills"),
                            {QStringLiteral("*.exr"),QStringLiteral("*.png")});
    else if (action == "exportStill")
        m_chooser->saveFile(QStringLiteral("Export still and grade"),
                            WorkingFrame::isTagged(m_stills->image(id)) ? QStringLiteral("Grade.exr") : QStringLiteral("Grade.png"),
                            QStringLiteral("Linear EXR or sRGB PNG with grade metadata"), {QStringLiteral("*.exr"),QStringLiteral("*.png")});
    else if (action == "importPreset")
        m_chooser->openFile(QStringLiteral("Import node preset"), QStringLiteral("Node presets"),
                            {QStringLiteral("*.omanode")});
    else if (action == "exportPreset")
        m_chooser->saveFile(QStringLiteral("Export node preset"), QStringLiteral("Node.omanode"),
                            QStringLiteral("Node presets"), {QStringLiteral("*.omanode")});
}

QString Backend::saveNodePreset(const QString &name, const QVariantMap &node) {
    if (name.trimmed().isEmpty() || node.value("primaries").toMap().isEmpty()) {
        setStatus(tr("Name the layer preset and select a correction."));
        return {};
    }
    const QVariantMap grade = GradeEngine::freshCopy({{"nodes", QVariantList{node}}});
    const QString error = GradeEngine::validate(grade);
    if (!error.isEmpty()) {
        setStatus(error);
        return {};
    }
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_nodePresets.append(
        QVariantMap{{"id", id}, {"name", name.trimmed()}, {"node", grade.value("nodes").toList().first()}});
    QSettings().setValue(QStringLiteral("color/nodePresets"), m_nodePresets);
    emit nodePresetsChanged();
    setStatus(tr("Layer preset saved"));
    return id;
}
void Backend::removeNodePreset(const QString &id) {
    for (int i = m_nodePresets.size() - 1; i >= 0; --i)
        if (m_nodePresets[i].toMap().value("id") == id)
            m_nodePresets.removeAt(i);
    QSettings().setValue(QStringLiteral("color/nodePresets"), m_nodePresets);
    emit nodePresetsChanged();
}
void Backend::applyNodePreset(const QString &id, int track, int index, int afterNode, const QString &parentPath) {
    if (m_exporting)
        return;
    for (const QVariant &v : m_nodePresets)
        if (v.toMap().value("id") == id) {
            emit pasteColorRequested(track, index, QStringLiteral("node"), v.toMap().value("node").toMap(), afterNode,
                                     parentPath);
            return;
        }
    setStatus(tr("That layer preset no longer exists."));
}

void Backend::trackWindowRange(int track, int index, int node, int window, int frame, int direction, bool singleFrame,
                               const QString &nodePath) {
    if (m_exporting || track < 0 || index < 0)
        return;
    setStatus(tr("Tracking window…"));
    emit trackWindowRangeRequested(track, index, node, window, frame, direction, singleFrame, nodePath);
}
void Backend::stopWindowTracking(bool keep) { emit stopWindowTrackingRequested(keep); }

void Backend::shareColor(int track, int index, const QString &action, const QString &name,
                         const QVariantList &addresses) {
    if (!m_exporting)
        emit shareColorRequested(track, index, action, name, addresses);
}
QVariantList Backend::colorGroups() const {
    QVariantList result;
    QSet<QString> ids;
    auto collect = [&](const QVariantList &entries) {
        for (const QVariant &v : entries) {
            const auto link = v.toMap().value(ClipKey::grade).toMap().value("sharing").toMap();
            const QString id = link.value("id").toString();
            if (!id.isEmpty() && !ids.contains(id)) {
                ids.insert(id);
                result << link;
            }
        }
    };
    collect(m_timeline);
    collect(m_connected);
    for (const QVariant &v : m_sequences)
        collect(v.toMap().value("snapshot").toList());
    return result;
}

void Backend::setMaskProvider(const QVariantMap &settings) {
    m_maskProvider = settings;
    QSettings().setValue("color/maskProvider", settings);
    emit maskProviderChanged();
}
void Backend::generateColorMask(int track, int index, const QString &nodePath, int frame, const QString &kind,
                                const QString &prompt) {
    if (!kMaskProviderOffered) {
        setStatus(tr("The face / object mask provider is not offered in this version"));
        return;
    }
    if (m_exporting || !m_maskJob.isEmpty() || m_maskService->busy())
        return;
    m_maskRequest = {{"kind", kind}, {"prompt", prompt}, {"frame", frame}};
    m_maskJob = {{"percent", 0}, {"message", QStringLiteral("Preparing mask frame…")}};
    emit maskJobChanged();
    emit prepareColorMaskRequested(++m_maskSerial, track, index, nodePath, frame);
}
void Backend::cancelColorMask() {
    ++m_maskSerial;
    m_maskTarget.clear();
    m_maskJob.clear();
    emit maskJobChanged();
    if (m_maskService)
        m_maskService->cancel();
}
