#include "workingframe.h"
#include "imagebands.h"
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <OpenEXR/ImfInputFile.h>
#include <OpenEXR/ImfOutputFile.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfStringAttribute.h>
#include <OpenEXR/ImfIO.h>
#include <stdexcept>

namespace {
constexpr auto tag = "omaedit.working.";
class Input : public Imf::IStream {
    QFile file;
public:
    explicit Input(const QString &path) : Imf::IStream(path.toUtf8().constData()), file(path) {
        if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot open linear image.");
    }
    bool read(char c[], int n) override {
        if (file.read(c,n) != n) throw std::runtime_error("Truncated linear image.");
        return !file.atEnd();
    }
    uint64_t tellg() override { return file.pos(); }
    void seekg(uint64_t n) override { if (!file.seek(n)) throw std::runtime_error("Cannot seek linear image."); }
};
class Output : public Imf::OStream {
public:
    QSaveFile file;
    explicit Output(const QString &path) : Imf::OStream(path.toUtf8().constData()), file(path) {
        if (!file.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot create linear image.");
    }
    void write(const char c[], int n) override { if (file.write(c,n) != n) throw std::runtime_error("Cannot write linear image."); }
    uint64_t tellp() override { return file.pos(); }
    void seekp(uint64_t n) override { if (!file.seek(n)) throw std::runtime_error("Cannot seek linear image."); }
};
QString attribute(const Imf::Header &h, const char *name) {
    const auto *a = h.findTypedAttribute<Imf::StringAttribute>(name);
    return a ? QString::fromStdString(a->value()) : QString();
}
}

QImage WorkingFrame::taggedImage() const {
    if(isNull())return {};
    // A read-only view of the same pixels with tags of its own, so tagging
    // copies nothing (writing to it copies it first, as to any shared image).
    auto *held = new QImage(m_pixels);
    QImage image(held->constBits(), held->width(), held->height(), held->bytesPerLine(), held->format(),
                 [](void *kept) { delete static_cast<QImage *>(kept); }, held);
    Bands::copyMetadata(image, m_pixels);
    for (const auto &p : {qMakePair("config",m_config),qMakePair("configHash",m_configHash),
                          qMakePair("space",m_space),qMakePair("alpha",QString("straight"))})
        image.setText(QString::fromLatin1(tag)+p.first,p.second);
    return image;
}
QImage WorkingFrame::shown(const QImage &image) {
    if (!isTagged(image)) return image;
    return fromTagged(image).display({}).convertToFormat(QImage::Format_RGBA8888);
}

bool WorkingFrame::isTagged(const QImage &image) { return !image.text(QString::fromLatin1(tag)+"space").isEmpty(); }
WorkingFrame WorkingFrame::fromTagged(const QImage &image, QString *error) {
    if (image.text(QString::fromLatin1(tag)+"alpha") != "straight") {
        if (error) *error = "Linear image alpha convention is missing or unsupported.";
        return {};
    }
    // A tagged image is the editor's own (a working frame's view, or one
    // read from a file), never a wrap of someone else's buffer: its pixels
    // are kept rather than copied, through a read-only view of them without
    // the tags (writing to it copies it first).
    if (image.format() != QImage::Format_RGBA32FPx4)
        return fromLinear(image,image.text(QString::fromLatin1(tag)+"config"),
            image.text(QString::fromLatin1(tag)+"configHash"),image.text(QString::fromLatin1(tag)+"space"),error);
    auto *held = new QImage(image);
    QImage pixels(held->constBits(), held->width(), held->height(), held->bytesPerLine(), held->format(),
                  [](void *kept) { delete static_cast<QImage *>(kept); }, held);
    for (const auto &key : image.textKeys())
        if (!key.startsWith(QString::fromLatin1(tag))) pixels.setText(key, image.text(key));
    return adoptLinear(std::move(pixels),image.text(QString::fromLatin1(tag)+"config"),
        image.text(QString::fromLatin1(tag)+"configHash"),image.text(QString::fromLatin1(tag)+"space"),error);
}
bool WorkingFrame::save(const QString &path, const QVariantMap &metadata, QString *error) const {
    if (error) error->clear();
    try {
        const auto problem = validate();
        if (!problem.isEmpty()) throw std::runtime_error(problem.toStdString());
        Output stream(path);
        {
            Imf::Header header(m_pixels.width(),m_pixels.height());
            if(!m_pixels.text("omaedit.pixelAspect").isEmpty())header.pixelAspectRatio()=m_pixels.text("omaedit.pixelAspect").toFloat();
            for (auto key : {"R","G","B","A"}) header.channels().insert(key,Imf::Channel(Imf::FLOAT));
            header.compression() = Imf::ZIP_COMPRESSION;
            header.insert("omaedit.config",Imf::StringAttribute(m_config.toStdString()));
            header.insert("omaedit.configHash",Imf::StringAttribute(m_configHash.toStdString()));
            header.insert("omaedit.space",Imf::StringAttribute(m_space.toStdString()));
            header.insert("omaedit.alpha",Imf::StringAttribute("straight"));
            header.insert("omaedit.metadata",Imf::StringAttribute(QJsonDocument::fromVariant(metadata).toJson(QJsonDocument::Compact).toStdString()));
            Imf::FrameBuffer buffer;
            int c=0;
            for (auto key : {"R","G","B","A"}) buffer.insert(key,Imf::Slice(Imf::FLOAT,
                reinterpret_cast<char *>(const_cast<uchar *>(m_pixels.constBits()))+4*c++,16,m_pixels.bytesPerLine()));
            Imf::OutputFile file(stream,header); file.setFrameBuffer(buffer); file.writePixels(m_pixels.height());
        }
        if (!stream.file.commit()) throw std::runtime_error("Cannot commit linear image.");
        return true;
    } catch (const std::exception &e) { if (error) *error=QString::fromUtf8(e.what()); return false; }
}
WorkingFrame WorkingFrame::load(const QString &path, QVariantMap *metadata, QString *error) {
    if (error) error->clear();
    if (metadata) metadata->clear();
    try {
        Input stream(path); Imf::InputFile file(stream);
        const auto &header=file.header(); const auto &box=header.dataWindow();
        const qint64 w=qint64(box.max.x)-box.min.x+1, h=qint64(box.max.y)-box.min.y+1;
        if (w<=0 || h<=0 || w>32768 || h>32768 || w*h>64000000)
            throw std::runtime_error("Linear image dimensions exceed the supported limit.");
        if (attribute(header,"omaedit.alpha") != "straight") throw std::runtime_error("Unsupported linear image alpha convention.");
        QImage pixels(int(w),int(h),QImage::Format_RGBA32FPx4);
        if (pixels.isNull()) throw std::runtime_error("Cannot allocate linear image.");
        if(header.pixelAspectRatio()!=1)pixels.setText("omaedit.pixelAspect",QString::number(header.pixelAspectRatio(),'g',16));
        Imf::FrameBuffer buffer; int c=0;
        for (auto key : {"R","G","B","A"}) {
            const auto *channel=header.channels().findChannel(key);
            if (!channel || channel->type!=Imf::FLOAT || channel->xSampling!=1 || channel->ySampling!=1)
                throw std::runtime_error("Working storage requires four full-resolution FLOAT channels.");
            buffer.insert(key,Imf::Slice::Make(Imf::FLOAT,pixels.bits()+4*c++,box,16,pixels.bytesPerLine()));
        }
        file.setFrameBuffer(buffer); file.readPixels(box.min.y,box.max.y);
        auto frame=fromLinear(pixels,attribute(header,"omaedit.config"),attribute(header,"omaedit.configHash"),attribute(header,"omaedit.space"),error);
        if (!frame.isNull() && metadata) *metadata=QJsonDocument::fromJson(attribute(header,"omaedit.metadata").toUtf8()).toVariant().toMap();
        return frame;
    } catch (const std::exception &e) { if (error) *error=QString::fromUtf8(e.what()); return {}; }
}
