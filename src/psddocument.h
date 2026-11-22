#pragma once

#include <QByteArray>
#include <QImage>
#include <QList>
#include <QRect>
#include <QString>

// Reads a Photoshop document for import: its pixel layers as separate
// pictures, and its saved whole picture with exact transparency. Layer
// effects, adjustment layers, smart filters and blend modes are not
// rendered — a layer comes in as the pixels Photoshop stored for it.
class PsdDocument {
public:
    struct Layer {
        QString name;      // "Group / Layer" for a layer inside groups
        QString blend;     // Photoshop's four-letter key ("norm", "mul ", …)
        bool visible = true; // itself and every group it sits in
        int opacity = 255; // 0..255; layerImage() applies it
        QRect bounds;      // where its pixels sit on the canvas
    };

    // Opens the file and reads its structure. On failure returns false with
    // a sentence for the editor in `error`.
    bool read(const QString &path, QString *error);

    QSize size() const { return m_size; }
    int depth() const { return m_depth; }
    // Pixel layers, bottom first. Groups, adjustment layers and other
    // layers without pixels are left out and counted in `skipped`.
    const QList<Layer> &layers() const { return m_layers; }
    int skipped() const { return m_skipped; }
    // One layer on the document's canvas, straight alpha, its mask and
    // opacity applied (RGBA8888, or RGBA64 for a 16-bit document). Null if
    // its pixels cannot be read.
    QImage layerImage(int index) const;
    // The whole picture as saved, colours taken back off Photoshop's white
    // backing so soft and transparent edges are exact. Null when the file
    // was saved without one.
    QImage merged() const;

private:
    bool readFile(const QString &path, QString *error);
    QImage readLayer(int index) const;
    QImage readMerged() const;
    struct Channel { int id = 0; qsizetype offset = 0, length = 0; };
    struct Record {
        QRect bounds, mask;
        int maskDefault = 0;
        bool maskOff = true;
        QList<Channel> channels;
    };
    QByteArray m_data;
    QSize m_size;
    int m_depth = 8, m_mode = 3, m_channels = 3, m_skipped = 0;
    bool m_big = false, m_mergedAlpha = false;
    qsizetype m_mergedOffset = -1;
    QList<Layer> m_layers;
    QList<Record> m_records;
};
