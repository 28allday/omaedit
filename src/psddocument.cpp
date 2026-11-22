#include "psddocument.h"

#include <QCoreApplication>
#include <QFile>
#include <QtEndian>

#include <new>

namespace {

// The large-document format's own limit on a side, and the most channels a
// document can have. Anything past these is damage, not a picture.
constexpr qint64 kLongestSide = 300000;
constexpr int kMostChannels = 56;
// A packed row can be no smaller than two bytes for every 128 it unpacks to,
// and a zlib stream no smaller than about a thousandth of its data: so the
// bytes that are really in the file bound what a plane may ask for.
constexpr qsizetype kMostRunLengthGain = 64;
constexpr qsizetype kMostZipGain = 1032;

QString tr(const char *text) { return QCoreApplication::translate("PsdDocument", text); }

// A bounds-checked big-endian reader: a read past the end marks the
// cursor bad and returns zero, so a damaged file fails instead of crashing.
struct Cursor {
    const uchar *base = nullptr;
    qsizetype size = 0, at = 0;
    bool ok = true;
    Cursor(const QByteArray &data, qsizetype from, qsizetype length)
        : base(reinterpret_cast<const uchar *>(data.constData())), size(data.size()), at(from) {
        if (from < 0 || length < 0 || from > data.size() || length > data.size() - from) { ok = false; size = 0; at = 0; }
        else size = from + length;
    }
    bool need(qsizetype count) { if (!ok || count < 0 || count > size - at) ok = false; return ok; }
    quint8 u8() { return need(1) ? base[at++] : 0; }
    quint16 u16() { if (!need(2)) return 0; const quint16 v = qFromBigEndian<quint16>(base + at); at += 2; return v; }
    quint32 u32() { if (!need(4)) return 0; const quint32 v = qFromBigEndian<quint32>(base + at); at += 4; return v; }
    quint64 u64() { if (!need(8)) return 0; const quint64 v = qFromBigEndian<quint64>(base + at); at += 8; return v; }
    qint16 i16() { return qint16(u16()); }
    qint32 i32() { return qint32(u32()); }
    // A section length: 8 bytes in the large-document format where asked.
    qsizetype length(bool wide) {
        const quint64 v = wide ? u64() : u32();
        if (v > quint64(size - at)) { ok = false; return 0; }
        return qsizetype(v);
    }
    QByteArray text(qsizetype count) { if (!need(count)) return {}; QByteArray out(reinterpret_cast<const char *>(base + at), count); at += count; return out; }
    void skip(qsizetype count) { if (need(count)) at += count; }
    qsizetype left() const { return ok ? size - at : 0; }
};

bool unpackRow(Cursor &in, qsizetype packed, uchar *out, qsizetype wanted) {
    const qsizetype end = in.at + packed;
    if (!in.need(packed)) return false;
    qsizetype made = 0;
    while (in.at < end && made < wanted) {
        const qint8 n = qint8(in.base[in.at++]);
        if (n >= 0) {
            const qsizetype count = qsizetype(n) + 1;
            if (count > end - in.at || count > wanted - made) return false;
            memcpy(out + made, in.base + in.at, size_t(count)); in.at += count; made += count;
        } else if (n != -128) {
            const qsizetype count = 1 - qsizetype(n);
            if (in.at >= end || count > wanted - made) return false;
            memset(out + made, in.base[in.at++], size_t(count)); made += count;
        }
    }
    in.at = end;
    return made == wanted;
}

// One channel's samples (big-endian, `bytes` per sample), top row first.
// `in` stands just after the compression word and is bounded to the data.
QByteArray plane(Cursor &in, int compression, int width, int height, int bytes, bool big) {
    if (width < 1 || height < 1 || width > kLongestSide || height > kLongestSide || bytes < 1 || bytes > 2) return {};
    const qsizetype row = qsizetype(width) * bytes, total = row * height;
    // The size comes from the file. Nothing is allocated for it until the
    // file is seen to hold enough bytes to fill it.
    const qsizetype left = in.left(), countBytes = qsizetype(height) * (big ? 4 : 2);
    if (compression == 0 ? total > left
        : compression == 1 ? countBytes > left || total / kMostRunLengthGain > left - countBytes
        : total / kMostZipGain > left) return {};
    QByteArray out(total, Qt::Uninitialized);
    auto *to = reinterpret_cast<uchar *>(out.data());
    if (compression == 0) {
        if (!in.need(total)) return {};
        memcpy(to, in.base + in.at, size_t(total)); in.at += total;
        return out;
    }
    if (compression == 1) {
        QList<qsizetype> counts; counts.reserve(height);
        for (int y = 0; y < height && in.ok; ++y) counts.append(big ? qsizetype(in.u32()) : qsizetype(in.u16()));
        if (!in.ok) return {};
        for (int y = 0; y < height; ++y) if (!unpackRow(in, counts[y], to + y * row, row)) return {};
        return out;
    }
    if (compression == 2 || compression == 3) {
        // A zlib stream; qUncompress wants the expected size in front.
        QByteArray packed(4, Qt::Uninitialized);
        qToBigEndian<quint32>(quint32(qMin<qsizetype>(total, 0x7fffffff)), packed.data());
        packed.append(reinterpret_cast<const char *>(in.base + in.at), in.left());
        const QByteArray plain = qUncompress(packed);
        if (plain.size() < total) return {};
        memcpy(to, plain.constData(), size_t(total));
        if (compression == 3) { // each row stored as differences from the sample before
            for (int y = 0; y < height; ++y) {
                uchar *line = to + y * row;
                if (bytes == 1) for (int x = 1; x < width; ++x) line[x] = uchar(line[x] + line[x - 1]);
                else if (bytes == 2) for (int x = 1; x < width; ++x)
                    qToBigEndian<quint16>(quint16(qFromBigEndian<quint16>(line + 2 * x) + qFromBigEndian<quint16>(line + 2 * x - 2)), line + 2 * x);
                else return {};
            }
        }
        return out;
    }
    return {};
}

inline int sample(const QByteArray &data, qsizetype index, int bytes) {
    const auto *p = reinterpret_cast<const uchar *>(data.constData()) + index * bytes;
    return bytes == 2 ? qFromBigEndian<quint16>(p) : *p;
}

// Photoshop stores ink amounts upside down: the largest value is no ink.
inline void inkToLight(int &r, int &g, int &b, int k, int top) {
    r = int(qint64(r) * k / top); g = int(qint64(g) * k / top); b = int(qint64(b) * k / top);
}

void put(QImage &image, int x, int y, int r, int g, int b, int a, int bytes) {
    if (bytes == 2) {
        auto *px = reinterpret_cast<quint16 *>(image.scanLine(y)) + 4 * x;
        px[0] = quint16(r); px[1] = quint16(g); px[2] = quint16(b); px[3] = quint16(a);
    } else {
        uchar *px = image.scanLine(y) + 4 * x;
        px[0] = uchar(r); px[1] = uchar(g); px[2] = uchar(b); px[3] = uchar(a);
    }
}

bool wideKey(const QByteArray &key) {
    static const QList<QByteArray> keys{"LMsk", "Lr16", "Lr32", "Layr", "Mt16", "Mt32", "Mtrn", "Alph", "FMsk", "lnk2", "FEid", "FXid", "PxSD"};
    return keys.contains(key);
}

} // namespace

bool PsdDocument::read(const QString &path, QString *error) {
    // A document too large for the memory there is must be refused like any
    // other that cannot be read, not end the app from a pool thread.
    try {
        return readFile(path, error);
    } catch (const std::bad_alloc &) {
        m_data.clear(); m_layers.clear(); m_records.clear(); m_mergedOffset = -1;
        if (error) *error = tr("There is not enough memory to read this Photoshop file.");
        return false;
    }
}

bool PsdDocument::readFile(const QString &path, QString *error) {
    auto fail = [&](const QString &message) { if (error) *error = message; m_data.clear(); m_layers.clear(); m_records.clear(); return false; };
    m_layers.clear(); m_records.clear(); m_skipped = 0; m_mergedOffset = -1; m_mergedAlpha = false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return fail(tr("The Photoshop file could not be opened."));
    if (file.size() > (qint64(1) << 31)) return fail(tr("The Photoshop file is larger than 2 GB, which is more than can be read."));
    m_data = file.readAll();
    Cursor in(m_data, 0, m_data.size());
    if (in.text(4) != "8BPS") return fail(tr("This is not a Photoshop document."));
    const int version = in.u16();
    if (version != 1 && version != 2) return fail(tr("This Photoshop file is a version that is not understood."));
    m_big = version == 2;
    in.skip(6);
    m_channels = in.u16();
    const quint32 height = in.u32(), width = in.u32();
    m_depth = in.u16(); m_mode = in.u16();
    if (!in.ok || width < 1 || height < 1 || width > 30000 || height > 30000) return fail(tr("The Photoshop file's size cannot be read."));
    if (m_channels < 1 || m_channels > kMostChannels) return fail(tr("The Photoshop file is damaged or incomplete."));
    if (m_depth != 8 && m_depth != 16) return fail(tr("Only 8-bit and 16-bit Photoshop documents can be read; this one is %1-bit.").arg(m_depth));
    if (m_mode != 1 && m_mode != 3 && m_mode != 4) return fail(tr("Only RGB, greyscale and CMYK Photoshop documents can be read."));
    m_size = QSize(int(width), int(height));
    in.skip(in.length(false)); // colour mode data
    in.skip(in.length(false)); // image resources
    const qsizetype sectionLength = in.length(m_big);
    if (!in.ok) return fail(tr("The Photoshop file is damaged or incomplete."));
    const qsizetype sectionEnd = in.at + sectionLength;
    m_mergedOffset = sectionEnd;

    // The layer list: in its own block, or for 16-bit documents inside a
    // tagged block after it.
    Cursor section(m_data, in.at, sectionLength);
    qsizetype listAt = -1, listLength = 0;
    if (sectionLength > 0) {
        const qsizetype length = section.length(m_big);
        if (length > 0) { listAt = section.at; listLength = length; }
        section.skip(length);
        section.skip(section.length(false)); // global mask
        while (section.ok && section.left() >= 12) {
            const QByteArray signature = section.text(4), key = section.text(4);
            if (signature != "8BIM" && signature != "8B64") break;
            const qsizetype length = section.length(m_big && wideKey(key));
            if (!section.ok) break;
            if (listAt < 0 && (key == "Lr16" || key == "Lr32")) { listAt = section.at; listLength = length; }
            section.skip((length + 3) & ~qsizetype(3));
        }
    }
    if (listAt < 0) return true; // a flat picture: only the merged image

    Cursor list(m_data, listAt, listLength);
    int count = list.i16();
    if (count < 0) { count = -count; m_mergedAlpha = true; }
    struct Raw { Layer layer; Record record; int section = 0; bool selfVisible = true; };
    // Where a layer or its mask sits: four signed numbers straight from the
    // file. An edge far off any canvas, or a side longer than a document can
    // be, is damage; an empty rectangle is a layer without pixels.
    const auto area = [](qint64 top, qint64 left, qint64 bottom, qint64 right, QRect *out) {
        constexpr qint64 farthest = qint64(1) << 30;
        for (qint64 edge : {top, left, bottom, right}) if (edge < -farthest || edge > farthest) return false;
        const qint64 width = right - left, height = bottom - top;
        if (width < 0 || height < 0 || width > kLongestSide || height > kLongestSide) return false;
        *out = QRect(int(left), int(top), int(width), int(height));
        return true;
    };
    // A record is never smaller than this, so a count the list has no room
    // for reserves nothing.
    constexpr qsizetype smallestRecord = 34;
    QList<Raw> raws; raws.reserve(qMin<qsizetype>(count, list.left() / smallestRecord));
    for (int i = 0; i < count && list.ok; ++i) {
        Raw raw;
        const int top = list.i32(), left = list.i32(), bottom = list.i32(), right = list.i32();
        if (!list.ok || !area(top, left, bottom, right, &raw.record.bounds)) { list.ok = false; break; }
        const int channels = list.u16();
        if (channels > kMostChannels) { list.ok = false; break; }
        for (int c = 0; c < channels && list.ok; ++c) {
            Channel channel; channel.id = list.i16();
            const quint64 length = m_big ? list.u64() : list.u32();
            channel.length = length > quint64(m_data.size()) ? -1 : qsizetype(length);
            if (channel.length < 0) list.ok = false;
            raw.record.channels.append(channel);
        }
        if (list.text(4) != "8BIM") list.ok = false;
        raw.layer.blend = QString::fromLatin1(list.text(4));
        raw.layer.opacity = list.u8();
        list.skip(1);
        raw.selfVisible = !(list.u8() & 2);
        list.skip(1);
        const qsizetype extraLength = list.length(false);
        if (!list.ok) break;
        Cursor extra(m_data, list.at, extraLength);
        list.skip(extraLength);
        const qsizetype maskLength = extra.length(false);
        if (maskLength >= 18) {
            Cursor mask(m_data, extra.at, maskLength);
            const int mt = mask.i32(), ml = mask.i32(), mb = mask.i32(), mr = mask.i32();
            if (!mask.ok || !area(mt, ml, mb, mr, &raw.record.mask)) { list.ok = false; break; }
            raw.record.maskDefault = mask.u8();
            raw.record.maskOff = (mask.u8() & 2) != 0 || !mask.ok;
        }
        extra.skip(maskLength);
        extra.skip(extra.length(false)); // blending ranges
        const int nameLength = extra.u8();
        raw.layer.name = QString::fromLatin1(extra.text(nameLength));
        extra.skip((4 - ((nameLength + 1) % 4)) % 4);
        while (extra.ok && extra.left() >= 12) {
            const QByteArray signature = extra.text(4), key = extra.text(4);
            if (signature != "8BIM" && signature != "8B64") break;
            const qsizetype length = extra.length(m_big && wideKey(key));
            if (!extra.ok) break;
            Cursor block(m_data, extra.at, length);
            if (key == "luni") {
                const quint32 letters = block.u32();
                QString name;
                for (quint32 n = 0; n < letters && block.ok; ++n) { const quint16 unit = block.u16(); if (block.ok && unit) name.append(QChar(unit)); }
                if (!name.isEmpty()) raw.layer.name = name;
            } else if (key == "lsct" || key == "lsdk") {
                raw.section = int(block.u32());
            }
            extra.skip((length + 1) & ~qsizetype(1));
        }
        raw.layer.bounds = raw.record.bounds;
        raws.append(raw);
    }
    if (!list.ok || raws.size() != count) return fail(tr("The Photoshop file's layers cannot be read."));
    // Pixel data follows the records, in the same order.
    qsizetype at = list.at;
    for (Raw &raw : raws)
        for (Channel &channel : raw.record.channels) {
            channel.offset = at;
            if (channel.length > m_data.size() - at) return fail(tr("The Photoshop file's layers cannot be read."));
            at += channel.length;
        }
    // Groups: walking from the top layer down, a group's header comes
    // first and a divider closes it.
    struct Group { QString name; bool visible; };
    QList<Group> open;
    QList<Raw> kept;
    for (qsizetype i = raws.size() - 1; i >= 0; --i) {
        Raw &raw = raws[i];
        if (raw.section == 1 || raw.section == 2) { open.append({raw.layer.name, raw.selfVisible}); continue; }
        if (raw.section == 3) { if (!open.isEmpty()) open.removeLast(); continue; }
        bool hasColour = false;
        for (const Channel &channel : std::as_const(raw.record.channels)) if (channel.id >= 0 && channel.length > 2) hasColour = true;
        if (raw.record.bounds.width() < 1 || raw.record.bounds.height() < 1 || !hasColour) { ++m_skipped; continue; }
        QStringList names; bool visible = raw.selfVisible;
        for (const Group &group : std::as_const(open)) { names.append(group.name); visible = visible && group.visible; }
        names.append(raw.layer.name);
        raw.layer.name = names.join(QStringLiteral(" / "));
        raw.layer.visible = visible;
        kept.prepend(raw);
    }
    for (const Raw &raw : std::as_const(kept)) { m_layers.append(raw.layer); m_records.append(raw.record); }
    return true;
}

QImage PsdDocument::layerImage(int index) const {
    try {
        return readLayer(index);
    } catch (const std::bad_alloc &) {
        return {};
    }
}

QImage PsdDocument::readLayer(int index) const {
    if (index < 0 || index >= m_records.size()) return {};
    const Record &record = m_records[index];
    const int bytes = m_depth / 8, top = bytes == 2 ? 65535 : 255;
    const int colours = m_mode == 3 ? 3 : m_mode == 4 ? 4 : 1;
    QList<QByteArray> colour(colours);
    QByteArray alpha, mask;
    for (const Channel &channel : record.channels) {
        if (channel.id < -2 || channel.id >= colours || channel.length < 2) continue;
        Cursor in(m_data, channel.offset, channel.length);
        const int compression = in.u16();
        const QRect area = channel.id == -2 ? record.mask : record.bounds;
        if (area.width() < 1 || area.height() < 1) continue;
        const QByteArray data = plane(in, compression, area.width(), area.height(), bytes, m_big);
        if (data.isEmpty()) { if (channel.id >= 0) return {}; continue; }
        if (channel.id == -1) alpha = data; else if (channel.id == -2) mask = data; else colour[channel.id] = data;
    }
    for (const QByteArray &data : std::as_const(colour)) if (data.isEmpty()) return {};
    if (record.maskOff) mask.clear();
    QImage image(m_size, bytes == 2 ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    if (image.isNull()) return {};
    image.fill(Qt::transparent);
    const QRect seen = record.bounds.intersected(QRect(QPoint(0, 0), m_size));
    const int opacity = m_layers[index].opacity;
    for (int y = seen.top(); y <= seen.bottom(); ++y)
        for (int x = seen.left(); x <= seen.right(); ++x) {
            const qsizetype at = qsizetype(y - record.bounds.top()) * record.bounds.width() + (x - record.bounds.left());
            int r = sample(colour[0], at, bytes), g = r, b = r;
            if (colours >= 3) { g = sample(colour[1], at, bytes); b = sample(colour[2], at, bytes); }
            if (colours == 4) inkToLight(r, g, b, sample(colour[3], at, bytes), top);
            qint64 a = alpha.isEmpty() ? top : sample(alpha, at, bytes);
            if (!mask.isEmpty()) {
                const int m = record.mask.contains(x, y)
                    ? sample(mask, qsizetype(y - record.mask.top()) * record.mask.width() + (x - record.mask.left()), bytes)
                    : (bytes == 2 ? record.maskDefault * 257 : record.maskDefault);
                a = a * m / top;
            }
            a = a * opacity / 255;
            put(image, x, y, r, g, b, int(a), bytes);
        }
    return image;
}

QImage PsdDocument::merged() const {
    try {
        return readMerged();
    } catch (const std::bad_alloc &) {
        return {};
    }
}

QImage PsdDocument::readMerged() const {
    if (m_mergedOffset < 0 || m_mergedOffset >= m_data.size()) return {};
    const int bytes = m_depth / 8, top = bytes == 2 ? 65535 : 255;
    const int colours = m_mode == 3 ? 3 : m_mode == 4 ? 4 : 1;
    if (m_channels < colours || m_channels > kMostChannels) return {};
    const int width = m_size.width(), height = m_size.height();
    const bool withAlpha = m_mergedAlpha && m_channels > colours;
    const int wanted = colours + (withAlpha ? 1 : 0);
    Cursor in(m_data, m_mergedOffset, m_data.size() - m_mergedOffset);
    const int compression = in.u16();
    QList<QByteArray> planes;
    if (compression == 1) {
        // Every channel's row sizes come first, then the rows.
        // The table must be in the file before room is made for it.
        const qsizetype rows = qsizetype(m_channels) * height;
        if (rows * (m_big ? 4 : 2) > in.left()) return {};
        QList<qsizetype> counts; counts.reserve(rows);
        for (qsizetype i = 0; i < rows && in.ok; ++i) counts.append(m_big ? qsizetype(in.u32()) : qsizetype(in.u16()));
        if (!in.ok) return {};
        const qsizetype row = qsizetype(width) * bytes;
        for (int c = 0; c < wanted; ++c) {
            if (row * height / kMostRunLengthGain > in.left()) return {};
            QByteArray data(row * height, Qt::Uninitialized);
            for (int y = 0; y < height; ++y)
                if (!unpackRow(in, counts[qsizetype(c) * height + y], reinterpret_cast<uchar *>(data.data()) + y * row, row)) return {};
            planes.append(data);
        }
    } else if (compression == 0) {
        for (int c = 0; c < wanted; ++c) {
            const QByteArray data = plane(in, 0, width, height, bytes, m_big);
            if (data.isEmpty()) return {};
            planes.append(data);
        }
    } else return {};
    QImage image(m_size, bytes == 2 ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    if (image.isNull()) return {};
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const qsizetype at = qsizetype(y) * width + x;
            int r = sample(planes[0], at, bytes), g = r, b = r;
            if (colours >= 3) { g = sample(planes[1], at, bytes); b = sample(planes[2], at, bytes); }
            if (colours == 4) inkToLight(r, g, b, sample(planes[3], at, bytes), top);
            int a = top;
            if (withAlpha) {
                a = sample(planes[colours], at, bytes);
                // Saved over white: take the white back out of soft pixels.
                auto lift = [&](int v) { return a > 0 ? int(qBound<qint64>(qint64(0), (qint64(v) - top + a) * top / a, qint64(top))) : 0; };
                r = lift(r); g = lift(g); b = lift(b);
            }
            put(image, x, y, r, g, b, a, bytes);
        }
    return image;
}
