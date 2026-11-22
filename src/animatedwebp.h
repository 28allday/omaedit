#pragma once
#include <QFile>
#include <QFileInfo>
#include <QtEndian>
#include <QDateTime>
#include <QHash>
#include <numeric>

// Read only RIFF chunk headers: payloads can be large and are never loaded here.
namespace AnimatedWebP {
struct Info { qint64 milliseconds = 0; int tick = 0; bool animated = false; };
inline Info inspectUncached(const QString &path) {
    Info info;
    if (QFileInfo(path).suffix().compare("webp", Qt::CaseInsensitive)) return info;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return info;
    const auto header = file.read(12);
    if (header.size() != 12 || header.left(4) != "RIFF" || header.mid(8,4) != "WEBP") return info;
    const qint64 end = qMin(file.size(), qint64(qFromLittleEndian<quint32>(header.constData()+4))+8);
    while (file.pos()+8 <= end) {
        const auto chunk = file.read(8);
        const quint32 length = qFromLittleEndian<quint32>(chunk.constData()+4);
        const qint64 next = file.pos()+qint64(length)+(length & 1);
        if (next > end) return {};
        if (chunk.left(4) == "VP8X" && length >= 10) {
            const auto flags = file.read(1);
            info.animated = flags.size() == 1 && (uchar(flags[0]) & 2);
        } else if (chunk.left(4) == "ANMF" && length >= 16) {
            const auto frame = file.read(16);
            const int delay = uchar(frame[12]) | (uchar(frame[13]) << 8) | (uchar(frame[14]) << 16);
            // Match FFmpeg's default for delays at or below its 10 ms minimum.
            const int duration = delay > 10 ? delay : 100;
            info.milliseconds += duration;
            info.tick = std::gcd(info.tick, duration);
        }
        if (!file.seek(next)) return {};
    }
    return info;
}
inline Info inspect(const QString &path) {
    const QFileInfo file(path);
    if (file.suffix().compare("webp",Qt::CaseInsensitive)) return {};
    struct Cached { qint64 size; QDateTime modified; Info info; };
    thread_local QHash<QString,Cached> cache;
    const auto found=cache.constFind(path);
    if (found!=cache.cend() && found->size==file.size() && found->modified==file.lastModified()) return found->info;
    const auto info=inspectUncached(path);
    if (cache.size()>=64) cache.clear();
    cache[path]={file.size(),file.lastModified(),info};
    return info;
}
inline QString resource(const QString &path) {
    return inspect(path).animated ? "avformat:"+path : path;
}
}
