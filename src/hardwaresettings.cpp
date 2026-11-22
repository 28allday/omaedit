#include "hardwaresettings.h"
#include "hardwareprobe.h"
#include "interface.h"
#include "startup.h"
#include "exportspec.h"
#include "engine/engine.h"
#include <QDir>
#include <QFile>
#include <QSettings>
#include <algorithm>

HardwareSettings::HardwareSettings(QObject *parent) : QObject(parent) {
    m_videoDecoderPreference=SuiteVideo::normalizedDecodePreference(QSettings().value("hardware/videoDecoder","auto").toString());
    m_videoDecoderDevice=QSettings().value("hardware/videoDecoderDevice").toString();
    m_videoDecoderInitialPreference=m_videoDecoderPreference;m_videoDecoderInitialDevice=m_videoDecoderDevice;
    m_interfaceRenderingPreference=SuiteQuick::normalizedInterfacePreference(QSettings().value("hardware/interfaceRendering","auto").toString());
    m_interfaceInitialPreference=m_interfaceRenderingPreference;
    const auto preference=qEnvironmentVariable("OMAEDIT_VIEWER_PROCESSING",QSettings().value("hardware/viewerProcessing","auto").toString());
    m_viewerProcessingPreference=preference=="cpu"?QString("cpu"):QString("auto");
    m_engineGpu = m_engineInitialGpu = QSettings().value("hardware/engineGpu").toString();
    m_probe = new SuiteHardware::HardwareProbe(this);
    connect(m_probe, &SuiteHardware::HardwareProbe::changed, this, [this] {
        const auto encoders = m_probe->snapshot().value("encoders").toMap();
        if (encoders != m_encoders) { m_encoders = encoders; emit encodersChanged(); }
        emit infoChanged();
    });
}
QStringList HardwareSettings::videoDecoderDevices() const {
    QStringList devices;
    if(m_videoDecoderPreference=="vulkan")
        devices=SuiteVideo::vulkanDecodeDevices();
    else if(m_videoDecoderPreference=="vaapi")
        for(const auto &name:QDir("/dev/dri").entryList({"renderD*"},QDir::System|QDir::Files,QDir::Name)) {
            SuiteVideo::DecodeOptions candidate;candidate.backend="vaapi";candidate.device="/dev/dri/"+name;
            if(SuiteVideo::hardwareRestriction(candidate).isEmpty())devices.append(candidate.device);
        }
    else if(m_videoDecoderPreference=="nvdec") {
        int count=0;
        for(const auto &name:QDir("/sys/class/drm").entryList({"renderD*"},QDir::Dirs|QDir::System,QDir::Name)) {
            QFile vendor("/sys/class/drm/"+name+"/device/vendor");if(vendor.open(QIODevice::ReadOnly) && vendor.readAll().trimmed()=="0x10de")++count;
        }
        for(int i=0;i<std::max(1,std::min(count,8));++i)devices.append(QString::number(i));
    }
    if(!m_videoDecoderDevice.isEmpty() && !devices.contains(m_videoDecoderDevice))devices.append(m_videoDecoderDevice);
    return devices;
}
void HardwareSettings::setVideoDecoderPreference(const QString &preference) {
    if(SuiteVideo::normalizedDecodePreference(preference)!=preference || preference==m_videoDecoderPreference)return;
    m_videoDecoderPreference=preference;m_videoDecoderDevice.clear();
    QSettings().setValue("hardware/videoDecoder",preference);QSettings().remove("hardware/videoDecoderDevice");
    emit videoDecoderPreferenceChanged();emit infoChanged();
}
void HardwareSettings::setVideoDecoderDevice(const QString &device) {
    if(device==m_videoDecoderDevice || (!device.isEmpty() && !videoDecoderDevices().contains(device)))return;
    m_videoDecoderDevice=device;QSettings().setValue("hardware/videoDecoderDevice",device);
    emit videoDecoderPreferenceChanged();emit infoChanged();
}
void HardwareSettings::retestVideoDecoder() {
    QSettings().remove("hardware/decodeQualification");m_videoDecoderRetest=true;
    emit videoDecoderPreferenceChanged();emit infoChanged();
}
void HardwareSettings::setInterfaceRenderingPreference(const QString &preference) {
    if(preference!="auto" && preference!="vulkan" && preference!="software")return;
    const bool retry=QSettings().contains("hardware/interfaceFailure");
    if(preference==m_interfaceRenderingPreference && !retry)return;
    QSettings().remove("hardware/interfaceFailure");
    m_interfaceRenderingPreference=preference;QSettings().setValue("hardware/interfaceRendering",preference);
    m_interfaceRenderingRestartRequired=retry || preference!=m_interfaceInitialPreference;
    emit interfaceRenderingPreferenceChanged();emit infoChanged();
}
void HardwareSettings::setViewerProcessingPreference(const QString &preference) {
    if((preference!="auto" && preference!="cpu") || preference==m_viewerProcessingPreference)return;
    m_viewerProcessingPreference=preference;QSettings().setValue("hardware/viewerProcessing",preference);
    emit viewerProcessingPreferenceChanged();emit infoChanged();
}
QVariantList HardwareSettings::engineGpus() const {
    if (m_engineGpus.isEmpty()) {
        m_engineGpus.append(QVariantMap{{"id", QString()}, {"name", tr("Automatic")}});
        const auto gpus = engine::listGpus();
        // Only those the engine can draw on, by the same rule that ranks them.
        for (int index : engine::rankGpus(gpus, {}))
            for (const auto &g : gpus)
                if (g.index == index && !g.id.empty())
                    m_engineGpus.append(QVariantMap{{"id", QString::fromStdString(g.id)}, {"name", QString::fromStdString(g.name)}});
        m_engineGpus.append(QVariantMap{{"id", QStringLiteral("cpu")}, {"name", tr("CPU")}});
    }
    QVariantList choices = m_engineGpus;
    // A GPU chosen earlier that is not here now stays in the list, so the
    // choice is seen and kept until the editor changes it.
    const bool known = std::any_of(choices.cbegin(), choices.cend(), [this](const QVariant &c) { return c.toMap().value("id").toString() == m_engineGpu; });
    if (!known) choices.insert(choices.size() - 1, QVariantMap{{"id", m_engineGpu}, {"name", tr("Not connected")}});
    return choices;
}
void HardwareSettings::setEngineGpu(const QString &id) {
    if (id == m_engineGpu) return;
    const auto choices = engineGpus();
    if (std::none_of(choices.cbegin(), choices.cend(), [&id](const QVariant &c) { return c.toMap().value("id").toString() == id; })) return;
    m_engineGpu = id;
    if (id.isEmpty()) QSettings().remove("hardware/engineGpu");
    else QSettings().setValue("hardware/engineGpu", id);
    emit engineGpuChanged(); emit infoChanged();
}
QVariantMap HardwareSettings::info() const {
    auto info=m_probe->snapshot();auto decoders=info.value("decoderDetails").toMap();
    decoders["playbackBackend"]=SuiteVideo::defaultDecodeOptions().backend;decoders["policyOnly"]=true;info["decoderDetails"]=decoders;return info;
}
void HardwareSettings::probeEncoders() {
    if (m_encoders.isEmpty() && !m_probe->encodersBusy()) m_probe->probeEncoders();
}
void HardwareSettings::probe(bool refresh) {
    if (refresh || !info().contains("gpu")) m_probe->probeGpu();
    if (refresh || m_encoders.isEmpty()) m_probe->probeEncoders();
}
QVariantMap HardwareSettings::resolvedSpec(QVariantMap spec) const {
    if (spec.value("hw").toString() == "vaapi" && spec.value("vaapiDevice").toString().isEmpty()) {
        const auto device = m_encoders.value("recommendedDevices").toMap().value(ExportSpec::videoEncoder(spec)).toString();
        if (!device.isEmpty()) spec["vaapiDevice"] = device;
        else spec.remove("vaapiDevice"); // allow FFmpeg's existing default attempt/fallback
    }
    return spec;
}
QVariantMap HardwareSettings::encodersForSpec(const QVariantMap &spec) const {
    auto encoders = m_encoders;
    // FFmpeg's encoder probe cannot see APV's: it is OpenAPV's own tool.
    if (spec.value("vcodec").toString() == "apv")
        encoders[ExportSpec::videoEncoder(spec)] = !ExportSpec::apvEncoder().isEmpty();
    if (spec.value("hw").toString() != "vaapi" || spec.value("vaapiDevice").toString().isEmpty()) return encoders;
    const QString device = spec.value("vaapiDevice").toString();
    // A positive result on another render node says nothing about this one.
    for (const auto &codec : {"h264_vaapi", "hevc_vaapi", "av1_vaapi"}) {
        bool found = false, available = false;
        for (const auto &v : info().value("encoderDetails").toList()) {
            const auto row = v.toMap();
            if (row.value("encoder").toString() == codec && row.value("device").toString() == device) {
                found = true; available |= row.value("available").toBool();
            }
        }
        if (found) encoders[codec] = available;
        else encoders.remove(codec); // untested custom node: attempt it, preserve runtime fallback
    }
    return encoders;
}
bool HardwareSettings::busy() const { return m_probe->encodersBusy() || m_probe->gpuBusy(); }
