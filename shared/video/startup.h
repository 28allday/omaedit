#pragma once
#include "decoder.h"
namespace SuiteVideo {
QString normalizedDecodePreference(const QString &value);
// Call the probe entry before constructing the application's QGuiApplication.
using DecodeProbeExtension = QVariantMap (*)(const QString &, const DecodeOptions &);
int decodeProbeMain(int argc, char **argv, DecodeProbeExtension extension = nullptr);
// Before media workers start. Preferences take effect next launch.
// `drawingDevice`: the Vulkan device (its index, as FFmpeg names it) that the
// program draws on, when it has one: tried first (see vulkanCandidates).
QVariantMap configureDecoding(const QString &drawingDevice = QString());
// The Vulkan devices to try for decoding, in order, from those that offer it:
// the one the program draws on first, since only its pictures pass to the
// drawing without leaving the GPU (another GPU's are brought down to memory
// and sent up again, each picture of each track); then the rest as found.
QStringList vulkanCandidates(QStringList decodeDevices, const QString &drawingDevice);
QVariantMap decodeStartup();
QVariantMap benchmarkDecode(const QString &path, const DecodeOptions &options);
} // namespace SuiteVideo
