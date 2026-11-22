#pragma once
#include <QImage>
#include <QString>

// Only media workers and asynchronous image requests use the disk cache.
namespace ThumbnailDiskCache {
QImage read(const QString &key);
// `packedAlready`: the picture as packed() made it, when the caller has that
// (a picture is not packed twice for two files).
void write(const QString &key, const QImage &image, const QByteArray &packedAlready = {});
// A picture as its file holds it (a PNG of what a viewer is shown).
QByteArray packed(const QImage &image);
qint64 bytes();
qint64 limit();
void setLimit(qint64 bytes);
// Starts over on the folder as a new run of the app would.
void reopen();

// Kept between runs: filmstrip tiles and waveform peaks, in a folder of their
// own beside the runs' ("kept-v1" under the thumbnail folder the Storage
// preference names; never beside the media). A key here must say exactly
// what the bytes are (the file as it is on disk, the frame, and everything
// that changes the picture or the sound): whatever is found under a key is
// shown without another look. `kind` is the file's suffix ("tile", "peaks").
//
// They share limit() with this run's thumbnails: the kept ones may fill what
// this run's leave, the least lately used going first. Any thread may call;
// files are written whole under another name and renamed, so a second running
// editor reads either all of one or none of it, and each counts the folder
// again before it removes anything.
QByteArray kept(const QString &key, const char *kind);
void keep(const QString &key, const char *kind, const QByteArray &bytes);
QImage keptImage(const QString &key, QByteArray *packedAs = nullptr);
void keepImage(const QString &key, const QImage &image, const QByteArray &packedAlready = {});
qint64 keptBytes();
// Nothing is kept or read back while off (OMAEDIT_KEEP_DECOR=0 starts it off).
void setKeeping(bool on);
bool keeping();
void clearKept();
}
