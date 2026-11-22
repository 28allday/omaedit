#pragma once
#include <QImage>
#include <QVariantMap>
#include <memory>
#include "decoder.h"

// Native-precision decode with optional hardware and mandatory CPU fallback. No integer image or integer RGB conversion is used.
// One decoder belongs to one rendering thread; repeated/sequential frames cache.
class NativeVideo {
public:
    NativeVideo();
    ~NativeVideo();
    bool open(const QString &path, QString *error, const SuiteVideo::DecodeOptions &options = SuiteVideo::defaultDecodeOptions());
    QVariantMap information() const;
    QImage frame(double seconds, const QVariantMap &interpretation, QString *error);
private:
    struct State;
    std::unique_ptr<State> d;
};
