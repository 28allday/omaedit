#pragma once
#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

namespace SuiteHardware { class HardwareProbe; }

// What this machine can do and how the editor is asked to use it
// (Preferences ▸ Hardware, Help ▸ Diagnostics, Deliver's encoder list).
//
// encoders: what this machine's ffmpeg can encode ({name: bool,
// nvenc/vaapi: yes|no|probing}); info: the probe's full snapshot. The
// preferences live in QSettings under hardware/…; the decoder, interface and
// graphics processor ones apply at the next launch, which the
// *RestartRequired flags report.
//
// engineGpu: which graphics processor draws the picture. That is the editor's
// choice and nothing changes it for them: "" takes the best one found,
// otherwise it is one of engineGpus' ids, or "cpu".
class HardwareSettings : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap encoders READ encoders NOTIFY encodersChanged)
    Q_PROPERTY(QVariantMap info READ info NOTIFY infoChanged)
    Q_PROPERTY(QString videoDecoderPreference READ videoDecoderPreference NOTIFY videoDecoderPreferenceChanged)
    Q_PROPERTY(QString videoDecoderDevice READ videoDecoderDevice NOTIFY videoDecoderPreferenceChanged)
    Q_PROPERTY(QStringList videoDecoderDevices READ videoDecoderDevices NOTIFY videoDecoderPreferenceChanged)
    Q_PROPERTY(bool videoDecoderRestartRequired READ videoDecoderRestartRequired NOTIFY videoDecoderPreferenceChanged)
    Q_PROPERTY(QString viewerProcessingPreference READ viewerProcessingPreference NOTIFY viewerProcessingPreferenceChanged)
    Q_PROPERTY(QString interfaceRenderingPreference READ interfaceRenderingPreference NOTIFY interfaceRenderingPreferenceChanged)
    Q_PROPERTY(bool interfaceRenderingRestartRequired READ interfaceRenderingRestartRequired NOTIFY interfaceRenderingPreferenceChanged)
    Q_PROPERTY(QString engineGpu READ engineGpu NOTIFY engineGpuChanged)
    Q_PROPERTY(QVariantList engineGpus READ engineGpus NOTIFY engineGpuChanged)
    Q_PROPERTY(bool engineGpuRestartRequired READ engineGpuRestartRequired NOTIFY engineGpuChanged)
  public:
    explicit HardwareSettings(QObject *parent = nullptr);
    QVariantMap encoders() const { return m_encoders; }
    QVariantMap info() const;
    QString videoDecoderPreference() const { return m_videoDecoderPreference; }
    QString videoDecoderDevice() const { return m_videoDecoderDevice; }
    QStringList videoDecoderDevices() const;
    bool videoDecoderRestartRequired() const { return m_videoDecoderRetest || m_videoDecoderPreference!=m_videoDecoderInitialPreference || m_videoDecoderDevice!=m_videoDecoderInitialDevice; }
    QString viewerProcessingPreference() const { return m_viewerProcessingPreference; }
    QString interfaceRenderingPreference() const { return m_interfaceRenderingPreference; }
    bool interfaceRenderingRestartRequired() const { return m_interfaceRenderingRestartRequired; }
    Q_INVOKABLE void setVideoDecoderPreference(const QString &preference);
    Q_INVOKABLE void setVideoDecoderDevice(const QString &device);
    Q_INVOKABLE void retestVideoDecoder();
    Q_INVOKABLE void setInterfaceRenderingPreference(const QString &preference);
    Q_INVOKABLE void setViewerProcessingPreference(const QString &preference);
    QString engineGpu() const { return m_engineGpu; }
    // The choices in the order offered: {id, name} for Automatic (""), each
    // GPU the engine can use as this machine lists them, then the CPU.
    QVariantList engineGpus() const;
    bool engineGpuRestartRequired() const { return m_engineGpu != m_engineInitialGpu; }
    Q_INVOKABLE void setEngineGpu(const QString &id);
    Q_INVOKABLE void probeEncoders();
    Q_INVOKABLE void probe(bool refresh = false);
    // True while a probe is still running.
    bool busy() const;
    // An export spec with the VA-API device this machine recommends filled in.
    QVariantMap resolvedSpec(QVariantMap spec) const;
    // The encoder list as it stands for this spec's device.
    QVariantMap encodersForSpec(const QVariantMap &spec) const;
  signals:
    void encodersChanged();
    void infoChanged();
    void videoDecoderPreferenceChanged();
    void viewerProcessingPreferenceChanged();
    void interfaceRenderingPreferenceChanged();
    void engineGpuChanged();

  private:
    QVariantMap m_encoders;
    QString m_viewerProcessingPreference="auto";
    QString m_videoDecoderPreference="auto",m_videoDecoderInitialPreference="auto";
    QString m_videoDecoderDevice,m_videoDecoderInitialDevice;
    bool m_videoDecoderRetest=false;
    QString m_interfaceRenderingPreference="auto",m_interfaceInitialPreference="auto";
    bool m_interfaceRenderingRestartRequired=false;
    QString m_engineGpu, m_engineInitialGpu;
    mutable QVariantList m_engineGpus;  // listed once, when first asked for
    SuiteHardware::HardwareProbe *m_probe = nullptr;
};
