#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVariantList>

// Caption files: SubRip (.srt), WebVTT (.vtt), iTT (.itt, the timed-text XML
// that video stores take) and SCC (.scc, CEA-608 broadcast captions, written
// by the 608 encoder here: two bytes a frame, pop-on, each caption loaded
// before the frame it shows on). Cues are [{start, frames, text}] in timeline
// frames at `fps`, as Timeline::parseSubtitles gives them.
namespace CaptionFormats {

// "srt", "vtt", "itt", "scc".
QStringList formats();
// ".srt" for "srt"; "" for a format that is not one.
QString extension(const QString &format);
// The format a file name's suffix names (.ttml/.xml/.dfxp read as iTT); "".
QString formatOfPath(const QString &path);

// Where each caption track's file goes for one file asked for (`stem`, the
// path without its suffix: "/x/film"): "film.srt" when there is one caption
// track and it has no language, otherwise each named by its language
// ("film.en.srt", "film.fr.srt"; "und" when it has none; "film.fr-2.srt"
// for a second track of a language).
QStringList trackPaths(const QString &stem, const QStringList &languages, const QString &format);

struct WriteOptions {
    QString language;              // BCP 47 ("en", "pt-BR"); "" or "und": not set
    QString place = QStringLiteral("bottom");  // bottom | top | centre
    QString align = QStringLiteral("centre");  // centre | left | right
    // SCC: the timecode of the cues' frame 0 (the programme's starting
    // timecode), as a frame count at `fps`. A caption is loaded before it
    // shows, so the file may start earlier, but never before 00:00:00:00.
    qint64 timecodeStart = 0;
};

// The file's bytes in `format`. Empty with `error` set when the captions
// cannot be written in it (SCC: a line wider than 32 letters that cannot be
// wrapped, more than four rows, a character 608 has no code for).
// `warning` (SCC): captions shown later than asked, for want of frames to
// load them in.
QByteArray write(const QString &format, const QVariantList &cues, double fps,
                 const WriteOptions &options, QString *error = nullptr, QString *warning = nullptr);

QString writeVtt(const QVariantList &cues, double fps, const WriteOptions &options);
QString writeItt(const QVariantList &cues, double fps, const WriteOptions &options);
QByteArray writeScc(const QVariantList &cues, double fps, const WriteOptions &options,
                    QString *error = nullptr, QString *warning = nullptr);

struct ReadResult {
    QVariantList cues;
    QString format;    // srt | vtt | itt | scc
    QString language;  // BCP 47 when the file (or its name) says; ""
};
// Any of the four, told apart by what the file holds. Times in an SCC file or
// a frame-timed iTT file at or after `timecodeStart` (the timeline's starting
// timecode, in frames) are taken from it.
ReadResult read(const QByteArray &raw, double fps, const QString &fileName = QString(),
                qint64 timecodeStart = 0);
// `frameTimed`: its times are SMPTE timecode (its programme's, perhaps).
QVariantList readItt(const QString &text, double fps, QString *language = nullptr, bool *frameTimed = nullptr);
// Pop-on, paint-on and roll-up captions of data channel 1 (CC1).
QVariantList readScc(const QString &text, double fps);

// A caption track's language as a delivered stream's tag (ISO 639-2):
// the terminology code for MP4 and MOV ("fra"; MOV's own older code where
// FFmpeg writes only that, "ger"), the bibliographic one for Matroska and
// WebM ("fre"). `fallback` when the language is not set or not known.
QString streamLanguage(const QString &bcp47, const QString &container,
                       const QString &fallback = QStringLiteral("und"));
// "fr" from "film.fr.srt" (or "film.fre.srt"), when the part before the
// suffix is a language code; "".
QString languageFromFileName(const QString &path);

// SCC's timecode of `frame` at `fps`: drop-frame ("01:00:00;00") at 29.97
// and 59.94, otherwise non-drop.
QString sccTimecode(qint64 frame, double fps);

} // namespace CaptionFormats
