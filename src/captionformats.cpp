#include "captionformats.h"

#include "interchange.h"
#include "timeline.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <QtAlgorithms>
#include <QtMath>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <utility>

namespace CaptionFormats {

// What the captions say to the person exporting them, in their language.
class CaptionText {
    Q_DECLARE_TR_FUNCTIONS(CaptionFormats)
};

QStringList formats() {
    return {QStringLiteral("srt"), QStringLiteral("vtt"), QStringLiteral("itt"), QStringLiteral("scc")};
}

QString extension(const QString &format) {
    return formats().contains(format) ? QLatin1Char('.') + format : QString();
}

QString formatOfPath(const QString &path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("srt") || suffix == QLatin1String("vtt") || suffix == QLatin1String("scc")
        || suffix == QLatin1String("itt"))
        return suffix;
    if (suffix == QLatin1String("ttml") || suffix == QLatin1String("dfxp") || suffix == QLatin1String("xml"))
        return QStringLiteral("itt");
    return {};
}

QStringList trackPaths(const QString &stem, const QStringList &languages, const QString &format) {
    const QString suffix = extension(format).isEmpty() ? QStringLiteral(".srt") : extension(format);
    const auto unset = [](const QString &code) { return code.isEmpty() || code == QLatin1String("und"); };
    if (languages.size() == 1 && unset(languages.first())) return {stem + suffix};
    QStringList paths;
    QHash<QString, int> seen;
    for (const QString &code : languages) {
        const QString name = unset(code) ? QStringLiteral("und") : code;
        const int n = ++seen[name.toLower()];
        paths.append(stem + QLatin1Char('.') + name + (n > 1 ? QStringLiteral("-%1").arg(n) : QString()) + suffix);
    }
    return paths;
}

// ── Languages ────────────────────────────────────────────────────────────
// BCP 47's primary language subtag → ISO 639-2 terminology and bibliographic codes.
struct Iso { const char *two, *terminology, *bibliographic; };
static const Iso kIso[] = {
    {"en", "eng", "eng"}, {"fr", "fra", "fre"}, {"de", "deu", "ger"}, {"es", "spa", "spa"}, {"it", "ita", "ita"},
    {"pt", "por", "por"}, {"nl", "nld", "dut"}, {"sv", "swe", "swe"}, {"da", "dan", "dan"}, {"nb", "nob", "nob"},
    {"no", "nor", "nor"}, {"nn", "nno", "nno"}, {"fi", "fin", "fin"}, {"pl", "pol", "pol"}, {"cs", "ces", "cze"},
    {"ga", "gle", "gle"}, {"cy", "cym", "wel"}, {"el", "ell", "gre"}, {"tr", "tur", "tur"}, {"ru", "rus", "rus"},
    {"uk", "ukr", "ukr"}, {"ar", "ara", "ara"}, {"he", "heb", "heb"}, {"hi", "hin", "hin"}, {"zh", "zho", "chi"},
    {"ja", "jpn", "jpn"}, {"ko", "kor", "kor"}, {"is", "isl", "ice"}, {"sk", "slk", "slo"}, {"ro", "ron", "rum"},
    {"hu", "hun", "hun"}, {"bg", "bul", "bul"}, {"hr", "hrv", "hrv"}, {"sr", "srp", "srp"}, {"sl", "slv", "slv"},
    {"et", "est", "est"}, {"lv", "lav", "lav"}, {"lt", "lit", "lit"}, {"ca", "cat", "cat"}, {"eu", "eus", "baq"},
    {"gl", "glg", "glg"}, {"th", "tha", "tha"}, {"vi", "vie", "vie"}, {"id", "ind", "ind"}, {"ms", "msa", "may"},
    {"fa", "fas", "per"}, {"ur", "urd", "urd"}, {"bn", "ben", "ben"}, {"ta", "tam", "tam"}, {"te", "tel", "tel"},
    {"sw", "swa", "swa"}, {"af", "afr", "afr"}, {"sq", "sqi", "alb"}, {"mk", "mkd", "mac"}, {"hy", "hye", "arm"},
    {"ka", "kat", "geo"}, {"mt", "mlt", "mlt"}, {"la", "lat", "lat"}, {"gd", "gla", "gla"}, {"tl", "tgl", "tgl"},
};
// FFmpeg writes a QuickTime file's language from the old Macintosh list
// alone, where some languages have other codes than ISO 639-2/T.
static const QHash<QString, QString> &movCodes() {
    static const QHash<QString, QString> codes{
        {"deu", "ger"}, {"nld", "dut"}, {"cym", "wel"}, {"ell", "gre"}, {"zho", "chi"}, {"swe", "sve"},
        {"gle", "iri"}, {"nob", "nor"}, {"nno", "nor"}, {"isl", "ice"}, {"sqi", "alb"}, {"hye", "arm"},
        {"kat", "geo"}, {"msa", "may"}, {"eus", "baq"}, {"mkd", "mac"}, {"fas", "per"}};
    return codes;
}

static const Iso *isoOf(const QString &code) {
    const QString primary = code.section(QLatin1Char('-'), 0, 0).toLower();
    if (primary.isEmpty()) return nullptr;
    for (const Iso &iso : kIso)
        if (primary == QLatin1String(iso.two) || primary == QLatin1String(iso.terminology)
            || primary == QLatin1String(iso.bibliographic))
            return &iso;
    return nullptr;
}

QString streamLanguage(const QString &bcp47, const QString &container, const QString &fallback) {
    const Iso *iso = isoOf(bcp47);
    if (!iso) return fallback;
    const bool matroska = container == QLatin1String("mkv") || container == QLatin1String("webm");
    QString code = QString::fromLatin1(matroska ? iso->bibliographic : iso->terminology);
    if (container == QLatin1String("mov")) code = movCodes().value(code, code);
    return code;
}

QString languageFromFileName(const QString &path) {
    const QString base = QFileInfo(path).completeBaseName();
    const int dot = base.lastIndexOf(QLatin1Char('.'));
    if (dot < 0) return {};
    const QString part = base.mid(dot + 1);
    static const QRegularExpression shape(QStringLiteral("^[A-Za-z]{2,3}(-[A-Za-z0-9]{2,8})*$"));
    if (!shape.match(part).hasMatch()) return {};
    const Iso *iso = isoOf(part);
    if (!iso) return {};
    const QString rest = part.section(QLatin1Char('-'), 1);
    return rest.isEmpty() ? QString::fromLatin1(iso->two) : QString::fromLatin1(iso->two) + QLatin1Char('-') + rest;
}

// ── Time ─────────────────────────────────────────────────────────────────
// 29.97 and 59.94: NTSC rates, counted in drop-frame timecode.
static bool ntscRate(double fps) {
    const double nominal = std::round(fps * 1.001);
    return (nominal == 30.0 || nominal == 60.0) && std::abs(fps - nominal * 1000.0 / 1001.0) < 0.005;
}

QString sccTimecode(qint64 frame, double fps) {
    return Interchange::timecode(frame, fps, ntscRate(fps));
}

// TTML's frame-based time ("00:00:01:05") at the timeline's rate: drop-frame
// labels at 29.97 (dropNTSC), plain labels otherwise, both with ':'.
static QString ttmlTime(qint64 frame, double fps) {
    const bool drop = ntscRate(fps) && std::round(fps * 1.001) == 30.0;
    return Interchange::timecode(frame, fps, drop).replace(QLatin1Char(';'), QLatin1Char(':'));
}

// ── Writing ──────────────────────────────────────────────────────────────
static QString cueText(const QVariantMap &cue) {
    QString text = cue.value(QStringLiteral("text")).toString().trimmed();
    text.replace(QRegularExpression(QStringLiteral("\\n\\s*\\n+")), QStringLiteral("\n"));
    return text;
}

// "hh:mm:ss.mmm", as SubRip's stamps with a dot.
static QString vttStamp(qint64 frame, double fps) {
    const qint64 ms = qRound64(frame * 1000.0 / fps);
    return QStringLiteral("%1:%2:%3.%4")
        .arg(ms / 3600000, 2, 10, QLatin1Char('0'))
        .arg((ms / 60000) % 60, 2, 10, QLatin1Char('0'))
        .arg((ms / 1000) % 60, 2, 10, QLatin1Char('0'))
        .arg(ms % 1000, 3, 10, QLatin1Char('0'));
}

QString writeVtt(const QVariantList &cues, double fps, const WriteOptions &options) {
    if (fps <= 0) return {};
    // Where the captions sit: at the bottom, centred, needs no setting.
    QString settings;
    if (options.place == QLatin1String("top")) settings += QStringLiteral(" line:0");
    else if (options.place == QLatin1String("centre")) settings += QStringLiteral(" line:50%");
    if (options.align == QLatin1String("left")) settings += QStringLiteral(" align:start");
    else if (options.align == QLatin1String("right")) settings += QStringLiteral(" align:end");
    QString out = QStringLiteral("WEBVTT\n");
    if (!options.language.isEmpty() && options.language != QLatin1String("und"))
        out += QStringLiteral("Language: %1\n").arg(options.language);
    out += QLatin1Char('\n');
    for (const QVariant &v : cues) {
        const QVariantMap cue = v.toMap();
        const qint64 start = cue.value(QStringLiteral("start")).toLongLong();
        const qint64 frames = cue.value(QStringLiteral("frames")).toLongLong();
        QString text = cueText(cue);
        if (frames < 1 || text.isEmpty()) continue;
        text.replace(QStringLiteral("&"), QStringLiteral("&amp;")).replace(QStringLiteral("<"), QStringLiteral("&lt;"))
            .replace(QStringLiteral(">"), QStringLiteral("&gt;"));
        out += QStringLiteral("%1 --> %2%3\n%4\n\n").arg(vttStamp(start, fps), vttStamp(start + frames, fps), settings, text);
    }
    return out;
}

static QString xmlEscaped(QString text) {
    return text.replace(QLatin1Char('&'), QStringLiteral("&amp;")).replace(QLatin1Char('<'), QStringLiteral("&lt;"))
        .replace(QLatin1Char('>'), QStringLiteral("&gt;")).replace(QLatin1Char('"'), QStringLiteral("&quot;"));
}

QString writeItt(const QVariantList &cues, double fps, const WriteOptions &options) {
    if (fps <= 0) return {};
    const bool ntsc = ntscRate(fps);
    const int nominal = ntsc ? int(std::round(fps * 1.001)) : qMax(1, int(std::round(fps)));
    // A rate such as 23.976 is its whole number of frames slowed by 1000/1001.
    const bool slowed = ntsc || std::abs(fps - std::round(fps * 1.001) * 1000.0 / 1001.0) < 0.005;
    const int frameRate = slowed ? int(std::round(fps * 1.001)) : nominal;
    const QString language = options.language.isEmpty() ? QStringLiteral("und") : options.language;
    const QString align = options.align == QLatin1String("left") ? QStringLiteral("left")
                          : options.align == QLatin1String("right") ? QStringLiteral("right") : QStringLiteral("center");
    const QString region = options.place == QLatin1String("top") ? QStringLiteral("top") : QStringLiteral("bottom");
    QString out;
    out += QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    out += QStringLiteral("<tt xmlns=\"http://www.w3.org/ns/ttml\" xmlns:tts=\"http://www.w3.org/ns/ttml#styling\""
                          " xmlns:ttm=\"http://www.w3.org/ns/ttml#metadata\" xmlns:ttp=\"http://www.w3.org/ns/ttml#parameter\""
                          " xml:lang=\"%1\" ttp:timeBase=\"smpte\" ttp:frameRate=\"%2\" ttp:frameRateMultiplier=\"%3\""
                          " ttp:dropMode=\"%4\">\n")
               .arg(xmlEscaped(language)).arg(frameRate)
               .arg(slowed ? QStringLiteral("1000 1001") : QStringLiteral("1 1"))
               .arg(ntsc && frameRate == 30 ? QStringLiteral("dropNTSC") : QStringLiteral("nonDrop"));
    out += QStringLiteral("  <head>\n"
                          "    <styling>\n"
                          "      <style xml:id=\"normal\" tts:fontFamily=\"sansSerif\" tts:fontWeight=\"normal\" tts:fontStyle=\"normal\""
                          " tts:color=\"white\" tts:fontSize=\"100%\"/>\n"
                          "    </styling>\n"
                          "    <layout>\n"
                          "      <region xml:id=\"top\" tts:origin=\"0% 0%\" tts:extent=\"100% 15%\" tts:textAlign=\"%1\" tts:displayAlign=\"before\"/>\n"
                          "      <region xml:id=\"bottom\" tts:origin=\"0% 85%\" tts:extent=\"100% 15%\" tts:textAlign=\"%1\" tts:displayAlign=\"after\"/>\n"
                          "    </layout>\n"
                          "  </head>\n").arg(align);
    out += QStringLiteral("  <body style=\"normal\" region=\"%1\">\n    <div>\n").arg(region);
    for (const QVariant &v : cues) {
        const QVariantMap cue = v.toMap();
        const int start = cue.value(QStringLiteral("start")).toInt();
        const int frames = cue.value(QStringLiteral("frames")).toInt();
        const QString text = cueText(cue);
        if (frames < 1 || text.isEmpty()) continue;
        QStringList lines = text.split(QLatin1Char('\n'));
        for (QString &line : lines) line = xmlEscaped(line.trimmed());
        out += QStringLiteral("      <p begin=\"%1\" end=\"%2\" region=\"%3\">%4</p>\n")
                   .arg(ttmlTime(start, fps), ttmlTime(start + frames, fps), region, lines.join(QStringLiteral("<br/>")));
    }
    out += QStringLiteral("    </div>\n  </body>\n</tt>\n");
    return out;
}

// ── CEA-608 ──────────────────────────────────────────────────────────────
// Each byte carries seven bits and odd parity in the eighth.
static int withParity(int byte) {
    byte &= 0x7F;
    return qPopulationCount(quint32(byte)) % 2 == 0 ? byte | 0x80 : byte;
}
static quint16 pairOf(int first, int second) { return quint16(withParity(first) << 8 | withParity(second)); }

namespace Cc {
// Data channel 1 (CC1) control codes, the first byte 0x14.
constexpr int RCL = 0x20, EDM = 0x2C, ENM = 0x2E, EOC = 0x2F, CR = 0x2D;
}

// What a character is sent as: one byte of the basic set, or a two-byte
// special (0x11) or extended (0x12, 0x13) code, the extended ones after a
// basic character a receiver without them shows instead.
struct Glyph {
    int basic = 0;          // the byte, when it is a basic character
    int first = 0, second = 0;
    char fallback = 0;
};

static const QHash<QChar, Glyph> &glyphs() {
    static const QHash<QChar, Glyph> table = [] {
        QHash<QChar, Glyph> t;
        for (int b = 0x20; b < 0x7F; ++b) t.insert(QChar(b), Glyph{b, 0, 0, 0});
        // The basic set's own letters where ASCII has others.
        for (char ascii : {'*', '\\', '^', '_', '`', '{', '|', '}', '~'}) t.remove(QChar::fromLatin1(ascii));
        const auto basic = [&t](char16_t c, int b) { t.insert(QChar(c), Glyph{b, 0, 0, 0}); };
        basic(u'á', 0x2A); basic(u'é', 0x5C); basic(u'í', 0x5E); basic(u'ó', 0x5F); basic(u'ú', 0x60);
        basic(u'ç', 0x7B); basic(u'÷', 0x7C); basic(u'Ñ', 0x7D); basic(u'ñ', 0x7E); basic(u'█', 0x7F);
        basic(u'’', 0x27); basic(u'`', 0x27);
        const char16_t special[] = u"®°½¿™¢£♪à\0èâêîôû";
        for (int i = 0; i < 16; ++i)
            if (special[i]) t.insert(QChar(special[i]), Glyph{0, 0x11, 0x30 + i, 0});
        const char16_t spanish[] = u"ÁÉÓÚÜü‘¡*'—©℠•“”ÀÂÇÈÊËëÎÏïÔÙùÛ«»";
        const char spanishFallback[] = "AEOUUu'!.'-cs.\"\"AACEEEeIIiOUuU\"\"";
        for (int i = 0; i < 32; ++i)
            if (spanish[i] != u'\'') t.insert(QChar(spanish[i]), Glyph{0, 0x12, 0x20 + i, spanishFallback[i]});
        const char16_t portuguese[] = u"ÃãÍÌìÒòÕõ{}\\^_|~ÄäÖöß¥¤¦ÅåØø┌┐└┘";
        const char portugueseFallback[] = "AaIIiOoOo()/'-!-AaOosY.!AaOo++++";
        for (int i = 0; i < 32; ++i) t.insert(QChar(portuguese[i]), Glyph{0, 0x13, 0x20 + i, portugueseFallback[i]});
        return t;
    }();
    return table;
}

// The caption's text as 608 can show it: typographic stand-ins made plain.
static QString sccPlain(QString text) {
    text.replace(QChar(0x00A0), QLatin1Char(' ')).replace(QLatin1Char('\t'), QLatin1Char(' '))
        .replace(QChar(0x2026), QStringLiteral("...")).replace(QChar(0x2013), QLatin1Char('-'))
        .replace(QChar(0x2010), QLatin1Char('-')).replace(QChar(0x2011), QLatin1Char('-'))
        .replace(QChar(0x201E), QChar(0x201C)).replace(QChar(0x2032), QLatin1Char('\''));
    return text;
}

// A character 608 has no code for, as the plain letter under its accent; 0: none.
static QChar baseLetter(QChar c) {
    // Letters with a stroke rather than a mark.
    static const QHash<QChar, QChar> stroked{{QChar(u'Ł'), QChar(u'L')}, {QChar(u'ł'), QChar(u'l')}, {QChar(u'Đ'), QChar(u'D')},
                                             {QChar(u'đ'), QChar(u'd')}, {QChar(u'Ħ'), QChar(u'H')}, {QChar(u'ħ'), QChar(u'h')},
                                             {QChar(u'ı'), QChar(u'i')}};
    if (stroked.contains(c)) return stroked.value(c);
    const QString d = c.decomposition();
    if (!d.isEmpty() && d.at(0).unicode() < 0x7F && d.at(0).isLetter()) return d.at(0);
    return QChar();
}

struct Unit { quint16 word; bool doubled; };

// One row's characters as 608 words (two basic characters to a word; a
// special or extended code a word of its own, sent twice). `bad`: the first
// character it cannot send.
static bool rowUnits(const QString &row, QVector<Unit> *units, QChar *bad) {
    const auto &table = glyphs();
    int pending = -1;
    const auto flush = [&] {
        if (pending >= 0) units->append({pairOf(pending, 0), false});
        pending = -1;
    };
    const auto basic = [&](int b) {
        if (pending < 0) pending = b;
        else { units->append({pairOf(pending, b), false}); pending = -1; }
    };
    for (QChar c : row) {
        auto it = table.constFind(c);
        if (it == table.constEnd()) {
            const QChar plain = baseLetter(c);
            it = plain.isNull() ? table.constEnd() : table.constFind(plain);
            if (it == table.constEnd()) { *bad = c; return false; }
        }
        const Glyph &g = *it;
        if (g.first == 0) { basic(g.basic); continue; }
        if (g.fallback) basic(g.fallback);
        flush();
        units->append({pairOf(g.first, g.second), true});
    }
    flush();
    return true;
}

// Rows of at most 32 letters, the caption's own lines wrapped at spaces.
static QStringList sccRows(const QString &text, QString *why) {
    QStringList rows;
    for (const QString &raw : text.split(QLatin1Char('\n'))) {
        const QString line = raw.simplified();
        if (line.isEmpty()) continue;
        QString row;
        for (const QString &word : line.split(QLatin1Char(' '))) {
            if (word.size() > 32) {
                *why = CaptionText::tr("“%1” is longer than the 32 letters an SCC row holds").arg(word);
                return {};
            }
            if (row.isEmpty()) row = word;
            else if (row.size() + 1 + word.size() <= 32) row += QLatin1Char(' ') + word;
            else { rows.append(row); row = word; }
        }
        if (!row.isEmpty()) rows.append(row);
    }
    if (rows.size() > 4)
        *why = CaptionText::tr("it needs %1 rows of 32 letters and SCC shows at most 4").arg(rows.size());
    return rows;
}

// The preamble address code of row 1–15, indented `indent` columns (0–28,
// a multiple of 4).
static quint16 preamble(int row, int indent) {
    static const int first[16] = {0, 0x11, 0x11, 0x12, 0x12, 0x15, 0x15, 0x16, 0x16, 0x17, 0x17, 0x10, 0x13, 0x13, 0x14, 0x14};
    static const bool lower[16] = {false, false, true, false, true, false, true, false, true, false, true, false, false, true, false, true};
    return pairOf(first[row], 0x50 | (lower[row] ? 0x20 : 0) | ((indent / 4) << 1));
}

QByteArray writeScc(const QVariantList &input, double fps, const WriteOptions &options, QString *error, QString *warning) {
    if (error) error->clear();
    if (warning) warning->clear();
    if (fps <= 0) return {};
    struct Caption { qint64 start, end; QVector<Unit> units; };
    QList<Caption> captions;
    QVariantList cues = input;
    std::stable_sort(cues.begin(), cues.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("start")).toLongLong() < b.toMap().value(QStringLiteral("start")).toLongLong();
    });
    for (const QVariant &v : std::as_const(cues)) {
        const QVariantMap cue = v.toMap();
        const qint64 start = cue.value(QStringLiteral("start")).toLongLong();
        const qint64 frames = cue.value(QStringLiteral("frames")).toLongLong();
        const QString text = sccPlain(cueText(cue));
        if (frames < 1 || text.isEmpty()) continue;
        const auto refuse = [&](const QString &why) {
            if (error)
                *error = CaptionText::tr("The caption at %1 (“%2”) cannot be written as SCC: %3.")
                             .arg(sccTimecode(start + options.timecodeStart, fps), text.simplified().left(40), why);
            return QByteArray();
        };
        QString why;
        const QStringList rows = sccRows(text, &why);
        if (!why.isEmpty()) return refuse(why);
        if (rows.isEmpty()) continue;
        // Bottom: the last rows of the fifteen; top: the first; centre: the middle.
        const int count = int(rows.size());
        const int firstRow = options.place == QLatin1String("top") ? 1
                             : options.place == QLatin1String("centre") ? (15 - count) / 2 + 1 : 16 - count;
        Caption c{start, start + frames, {}};
        c.units.append({pairOf(0x14, Cc::RCL), true});
        c.units.append({pairOf(0x14, Cc::ENM), true});
        for (int r = 0; r < count; ++r) {
            const QString &row = rows.at(r);
            const int column = options.align == QLatin1String("left") ? 0
                               : options.align == QLatin1String("right") ? 32 - int(row.size()) : (32 - int(row.size())) / 2;
            c.units.append({preamble(firstRow + r, column / 4 * 4), true});
            if (column % 4) c.units.append({pairOf(0x17, 0x20 + column % 4), true});
            QChar bad;
            if (!rowUnits(row, &c.units, &bad))
                return refuse(CaptionText::tr("SCC has no code for “%1”").arg(bad));
        }
        c.units.append({pairOf(0x14, Cc::EOC), true});
        // A caption over the next one ends where it starts.
        if (!captions.isEmpty() && captions.last().end > start) captions.last().end = start;
        captions.append(c);
    }
    // Every word at the frame it is sent on: each caption loaded so that its
    // End of Caption lands on its first frame, or as soon after as the one
    // before allows; Erase Displayed Memory where a gap follows a caption.
    QMap<qint64, quint16> sent;
    const auto freeAt = [&sent](qint64 f) { return !sent.contains(f); };
    qint64 cursor = -options.timecodeStart;
    std::optional<qint64> clearAt;
    int late = 0;
    qint64 latest = 0;
    const quint16 edm = pairOf(0x14, Cc::EDM);
    for (const Caption &c : std::as_const(captions)) {
        if (clearAt) {
            const qint64 f = qMax(*clearAt, cursor);
            if (f + 2 <= c.start) { sent.insert(f, edm); sent.insert(f + 1, edm); }
            clearAt.reset();
        }
        QVector<quint16> words;
        QVector<bool> pairStart;
        for (const Unit &u : c.units) {
            words.append(u.word); pairStart.append(u.doubled);
            if (u.doubled) { words.append(u.word); pairStart.append(false); }
        }
        const int n = int(words.size());
        QVector<qint64> at(n);
        // Backwards from the second End of Caption on the frame after the first.
        qint64 f = qMax(c.start, cursor + n - 2) + 1;
        bool fits = true;
        for (int k = n - 1; k >= 0; --k) {
            if (k > 0 && pairStart.at(k - 1)) {
                while (!(freeAt(f) && freeAt(f - 1))) --f;
                at[k] = f; at[k - 1] = f - 1; f -= 2; --k;
            } else {
                while (!freeAt(f)) --f;
                at[k] = f; --f;
            }
        }
        if (at[0] < cursor) fits = false;
        if (!fits) {
            f = cursor;
            for (int k = 0; k < n; ++k) {
                if (pairStart.at(k)) {
                    while (!(freeAt(f) && freeAt(f + 1))) ++f;
                    at[k] = f; at[k + 1] = f + 1; f += 2; ++k;
                } else {
                    while (!freeAt(f)) ++f;
                    at[k] = f++;
                }
            }
        }
        for (int k = 0; k < n; ++k) sent.insert(at[k], words.at(k));
        const qint64 shown = at[n - 2];
        if (shown > c.start) { ++late; latest = qMax(latest, shown - c.start); }
        cursor = at[n - 1] + 1;
        clearAt = c.end;
    }
    if (clearAt) {
        qint64 f = qMax(*clearAt, cursor);
        sent.insert(f, edm); sent.insert(f + 1, edm);
    }
    if (late && warning)
        *warning = CaptionText::tr("%n caption(s) shown late in the SCC file, by up to %1 frames: too little time between captions to load them.",
                      nullptr, late).arg(latest);
    QString out = QStringLiteral("Scenarist_SCC V1.0\n\n");
    qint64 previous = std::numeric_limits<qint64>::min();
    QStringList line;
    const auto endLine = [&] {
        if (!line.isEmpty()) out += line.join(QLatin1Char(' ')) + QStringLiteral("\n\n");
        line.clear();
    };
    for (auto it = sent.constBegin(); it != sent.constEnd(); ++it) {
        if (it.key() != previous + 1) {
            endLine();
            out += sccTimecode(it.key() + options.timecodeStart, fps) + QLatin1Char('\t');
        }
        line.append(QStringLiteral("%1").arg(it.value(), 4, 16, QLatin1Char('0')));
        previous = it.key();
    }
    endLine();
    return out.toLatin1();
}

QByteArray write(const QString &format, const QVariantList &cues, double fps, const WriteOptions &options,
                 QString *error, QString *warning) {
    if (error) error->clear();
    if (warning) warning->clear();
    if (format == QLatin1String("srt")) return Timeline::formatSrt(cues, fps).toUtf8();
    if (format == QLatin1String("vtt")) return writeVtt(cues, fps, options).toUtf8();
    if (format == QLatin1String("itt")) return writeItt(cues, fps, options).toUtf8();
    if (format == QLatin1String("scc")) return writeScc(cues, fps, options, error, warning);
    if (error) *error = CaptionText::tr("Unknown caption format “%1” (srt, vtt, itt or scc)").arg(format);
    return {};
}

// ── Reading SCC ──────────────────────────────────────────────────────────
// The basic set's letters where ASCII has others.
static QChar basicChar(int b) {
    switch (b) {
    case 0x2A: return QChar(u'á');
    case 0x5C: return QChar(u'é');
    case 0x5E: return QChar(u'í');
    case 0x5F: return QChar(u'ó');
    case 0x60: return QChar(u'ú');
    case 0x7B: return QChar(u'ç');
    case 0x7C: return QChar(u'÷');
    case 0x7D: return QChar(u'Ñ');
    case 0x7E: return QChar(u'ñ');
    case 0x7F: return QChar(u'█');
    default: return QChar(b);
    }
}
static QChar specialChar(int lo) {
    static const char16_t special[] = u"®°½¿™¢£♪à èâêîôû";
    return QChar(special[(lo - 0x30) & 15]);
}
static QChar extendedChar(int set, int lo) {
    static const char16_t spanish[] = u"ÁÉÓÚÜü‘¡*'—©℠•“”ÀÂÇÈÊËëÎÏïÔÙùÛ«»";
    static const char16_t portuguese[] = u"ÃãÍÌìÒòÕõ{}\\^_|~ÄäÖöß¥¤¦ÅåØø┌┐└┘";
    return QChar((set == 0x12 ? spanish : portuguese)[(lo - 0x20) & 31]);
}

QVariantList readScc(const QString &text, double fps) {
    QVariantList cues;
    if (fps <= 0) return cues;
    static const QRegularExpression lineForm(QStringLiteral("^(\\d{1,2}:\\d{2}:\\d{2}([:;.,])(\\d{2}))\\s+(.*)$"));
    struct Line { QString timecode; bool drop; QVector<quint16> words; };
    QList<Line> lines;
    int highestFrame = 0;
    bool anyDrop = false;
    for (const QString &raw : text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")), Qt::SkipEmptyParts)) {
        const auto m = lineForm.match(raw.trimmed());
        if (!m.hasMatch()) continue;
        Line l{m.captured(1), m.captured(2) != QLatin1String(":"), {}};
        anyDrop = anyDrop || l.drop;
        highestFrame = qMax(highestFrame, m.captured(3).toInt());
        for (const QString &w : m.captured(4).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts)) {
            bool ok = false;
            const uint value = w.toUInt(&ok, 16);
            if (ok && w.size() == 4) l.words.append(quint16(value));
        }
        lines.append(l);
    }
    // The file's rate: the timeline's when its timecode fits it, otherwise
    // the NTSC rate SCC was made for.
    const int timelineNominal = int(std::ceil(fps - 0.001));
    double fileFps = fps;
    if (anyDrop) fileFps = (timelineNominal == 60 ? 60.0 : 30.0) * 1000.0 / 1001.0;
    else if (highestFrame >= timelineNominal) fileFps = highestFrame >= 30 ? 60000.0 / 1001.0 : 30000.0 / 1001.0;
    const bool sameRate = std::abs(fileFps - fps) < 0.001;
    const auto timelineFrame = [&](qint64 f) -> qint64 { return sameRate ? f : qRound64(f / fileFps * fps); };

    using Row = std::array<QChar, 32>;
    using Memory = std::array<Row, 16>;
    Memory displayed{}, hidden{};
    enum Mode { PopOn, PaintOn, RollUp, Text } mode = PopOn;
    int row = 15, column = 0, rollRows = 2, channel = 1;
    quint16 previousCode = 0;
    QString shown;
    qint64 since = 0, now = 0;
    // Roll-up: each line from its first letter to the next line's.
    QString rolled;
    qint64 rolledSince = 0, lineSince = 0;
    bool lineStarted = false;
    const auto emitCue = [&](qint64 from, qint64 to, const QString &words) {
        const qint64 a = timelineFrame(from), b = timelineFrame(to);
        if (words.trimmed().isEmpty() || b <= a || a < 0) return;
        cues.append(QVariantMap{{"start", int(a)}, {"frames", int(b - a)}, {"text", words}});
    };
    const auto rowText = [](const Row &r) {
        QString s;
        for (QChar c : r) s += c.isNull() ? QChar(u' ') : c;
        return s.trimmed();
    };
    const auto render = [&](const Memory &m) {
        QStringList out;
        for (int r = 1; r <= 15; ++r) { const QString s = rowText(m[r]); if (!s.isEmpty()) out.append(s); }
        return out.join(QLatin1Char('\n'));
    };
    const auto flushRoll = [&](qint64 t) {
        if (!rolled.isEmpty()) emitCue(rolledSince, t, rolled);
        rolled.clear();
    };
    const auto display = [&](qint64 t) {
        const QString nowShown = render(displayed);
        if (nowShown == shown) return;
        // Painting on adds to what is there: one caption as it grows.
        if (mode == PaintOn && !shown.isEmpty() && nowShown.startsWith(shown)) { shown = nowShown; return; }
        if (!shown.isEmpty()) emitCue(since, t, shown);
        shown = nowShown;
        since = t;
    };
    const auto put = [&](QChar c) {
        if (mode == Text || channel != 1 || row < 1 || row > 15) return;
        if (mode == RollUp && !lineStarted) {
            lineStarted = true;
            lineSince = now;
            flushRoll(now);
        }
        Memory &m = mode == PopOn ? hidden : displayed;
        m[row][column] = c;
        column = qMin(column + 1, 31);
        if (mode == PaintOn) display(now);
    };
    const auto clear = [](Memory &m) { for (Row &r : m) r.fill(QChar()); };
    qint64 lastTime = 0;
    for (const Line &l : std::as_const(lines)) {
        bool ok = false;
        const qint64 base = Interchange::timecodeFrames(l.timecode, fileFps, &ok, false);
        if (!ok) continue;
        for (int i = 0; i < l.words.size(); ++i) {
            now = base + i;
            lastTime = qMax(lastTime, now + 1);
            const int hi = (l.words.at(i) >> 8) & 0x7F, lo = l.words.at(i) & 0x7F;
            if (hi == 0 && lo == 0) continue;               // padding
            if (hi >= 0x10 && hi <= 0x1F) {
                const quint16 code = quint16(hi << 8 | lo);
                if (code == previousCode) { previousCode = 0; continue; }   // sent twice: once
                previousCode = code;
                if ((hi & 0x77) == 0x15 && lo >= 0x20 && lo <= 0x2F) { channel = 3; continue; } // field 2's
                channel = (hi & 0x08) ? 2 : 1;
                if (channel != 1) continue;
                const int h = hi & 0x77;
                if (lo >= 0x40) {                            // preamble address code
                    static const QHash<int, std::pair<int, int>> rows{{0x11, {1, 2}}, {0x12, {3, 4}}, {0x15, {5, 6}}, {0x16, {7, 8}},
                                                                     {0x17, {9, 10}}, {0x10, {11, 11}}, {0x13, {12, 13}}, {0x14, {14, 15}}};
                    if (!rows.contains(h)) continue;
                    const int target = (lo & 0x20) ? rows.value(h).second : rows.value(h).first;
                    const int indent = (lo & 0x10) ? ((lo & 0x0E) >> 1) * 4 : 0;
                    if (mode == RollUp) {
                        // The rows roll up from the new base row.
                        if (target != row) {
                            Memory moved{};
                            for (int r = 0; r < rollRows; ++r)
                                if (row - r >= 1 && target - r >= 1) moved[target - r] = displayed[row - r];
                            displayed = moved;
                        }
                    }
                    row = target;
                    column = indent;
                } else if (h == 0x11 && lo >= 0x20 && lo <= 0x2F) {
                    put(QChar(u' '));                        // a mid-row code takes a space
                } else if (h == 0x11 && lo >= 0x30) {
                    put(specialChar(lo));
                } else if ((h == 0x12 || h == 0x13) && lo >= 0x20 && lo <= 0x3F) {
                    if (column > 0) --column;                // over the character sent for it
                    put(extendedChar(h, lo));
                } else if (h == 0x17 && lo >= 0x21 && lo <= 0x23) {
                    column = qMin(column + lo - 0x20, 31);
                } else if (h == 0x14 && lo >= 0x20 && lo <= 0x2F) {
                    switch (lo) {
                    case 0x20: mode = PopOn; break;                         // RCL
                    case 0x21: {                                            // BS
                        if (column > 0) { --column; (mode == PopOn ? hidden : displayed)[row][column] = QChar(); }
                        break;
                    }
                    case 0x24: {                                            // DER
                        Memory &m = mode == PopOn ? hidden : displayed;
                        for (int c = column; c < 32; ++c) m[row][c] = QChar();
                        break;
                    }
                    case 0x25: case 0x26: case 0x27:                        // RU2–RU4
                        if (mode != RollUp) {
                            clear(displayed); clear(hidden); display(now);
                            row = 15;
                        }
                        mode = RollUp; rollRows = lo - 0x23; column = 0;
                        break;
                    case 0x29: mode = PaintOn; break;                       // RDC
                    case 0x2A: case 0x2B: mode = Text; break;               // TR, RTD
                    case 0x2C:                                              // EDM
                        if (lineStarted) { rolled = rowText(displayed[row]); rolledSince = lineSince; lineStarted = false; }
                        flushRoll(now);
                        clear(displayed);
                        display(now);
                        break;
                    case 0x2D:                                              // CR
                        if (mode == RollUp) {
                            if (lineStarted) { rolled = rowText(displayed[row]); rolledSince = lineSince; lineStarted = false; }
                            for (int r = row - rollRows + 1; r < row; ++r) if (r >= 1) displayed[r] = displayed[r + 1];
                            displayed[row].fill(QChar());
                        }
                        column = 0;
                        break;
                    case 0x2E: clear(hidden); break;                        // ENM
                    case 0x2F:                                              // EOC
                        flushRoll(now);
                        std::swap(displayed, hidden);
                        mode = PopOn;
                        display(now);
                        break;
                    default: break;
                    }
                }
                continue;
            }
            previousCode = 0;
            if (hi >= 0x20) put(basicChar(hi));
            if (lo >= 0x20) put(basicChar(lo));
        }
    }
    // What is still up when the file ends.
    if (lineStarted) { rolled = rowText(displayed[row]); rolledSince = lineSince; }
    flushRoll(qMax(lastTime, rolledSince + qint64(std::round(fileFps))));
    if (!shown.isEmpty()) emitCue(since, qMax(lastTime, since + qint64(std::round(fileFps))), shown);
    std::stable_sort(cues.begin(), cues.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("start")).toInt() < b.toMap().value(QStringLiteral("start")).toInt();
    });
    return cues;
}

// ── Reading iTT / TTML ───────────────────────────────────────────────────
QVariantList readItt(const QString &text, double fps, QString *language, bool *frameTimed) {
    QVariantList cues;
    if (fps <= 0) return cues;
    QXmlStreamReader xml(text);
    QString timeBase = QStringLiteral("media"), dropMode = QStringLiteral("nonDrop");
    double frameRate = 30.0, multiplier = 1.0, tickRate = 0.0, subFrameRate = 1.0;
    bool rateGiven = false;
    QString lang;
    QList<double> offsets{0.0};   // each open timed element's begin, in seconds
    QList<bool> timed;            // whether each open element pushed an offset
    QString words;
    double pBegin = -1, pEnd = -1;
    int inP = 0;
    const auto effectiveRate = [&] { return frameRate * multiplier; };
    static const QRegularExpression clock(QStringLiteral("^(\\d+):(\\d{2}):(\\d{2})(?:(\\.\\d+)|:(\\d+)(?:\\.(\\d+))?)?$"));
    static const QRegularExpression offset(QStringLiteral("^(\\d+(?:\\.\\d+)?)(h|ms|m|s|f|t)$"));
    const auto seconds = [&](QString t, bool *ok) -> double {
        t = t.trimmed();
        *ok = true;
        if (const auto m = clock.match(t); m.hasMatch()) {
            const qint64 h = m.captured(1).toLongLong(), mi = m.captured(2).toLongLong(), s = m.captured(3).toLongLong();
            if (!m.captured(4).isEmpty()) return h * 3600 + mi * 60 + s + m.captured(4).toDouble();
            if (m.captured(5).isEmpty()) return h * 3600 + mi * 60 + s;
            const qint64 ff = m.captured(5).toLongLong();
            const double sub = m.captured(6).isEmpty() ? 0.0 : m.captured(6).toDouble() / subFrameRate;
            if (timeBase == QLatin1String("smpte")) {
                const int nominal = qMax(1, int(std::round(frameRate)));
                qint64 frames = (h * 3600 + mi * 60 + s) * nominal + ff;
                if (dropMode == QLatin1String("dropNTSC") && (nominal == 30 || nominal == 60)) {
                    const qint64 d = nominal / 30 * 2, minutes = h * 60 + mi;
                    frames -= d * (minutes - minutes / 10);
                }
                return (frames + sub) / effectiveRate();
            }
            return h * 3600 + mi * 60 + s + (ff + sub) / effectiveRate();
        }
        if (const auto m = offset.match(t); m.hasMatch()) {
            const double v = m.captured(1).toDouble();
            const QString unit = m.captured(2);
            if (unit == QLatin1String("h")) return v * 3600;
            if (unit == QLatin1String("m")) return v * 60;
            if (unit == QLatin1String("s")) return v;
            if (unit == QLatin1String("ms")) return v / 1000;
            if (unit == QLatin1String("f")) return v / effectiveRate();
            return v / (tickRate > 0 ? tickRate : (rateGiven ? effectiveRate() * subFrameRate : 1.0));
        }
        *ok = false;
        return 0;
    };
    const auto attr = [](const QXmlStreamAttributes &a, const char *name) -> QString {
        for (const QXmlStreamAttribute &at : a)
            if (at.name() == QLatin1String(name)) return at.value().toString();
        return {};
    };
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            const QString name = xml.name().toString();
            const QXmlStreamAttributes a = xml.attributes();
            const QString elementLang = a.value(QStringLiteral("http://www.w3.org/XML/1998/namespace"), QStringLiteral("lang")).toString();
            if (lang.isEmpty() && !elementLang.isEmpty()) lang = elementLang;
            if (name == QLatin1String("tt")) {
                if (const QString v = attr(a, "timeBase"); !v.isEmpty()) timeBase = v;
                if (const QString v = attr(a, "dropMode"); !v.isEmpty()) dropMode = v;
                if (const QString v = attr(a, "frameRate"); !v.isEmpty()) { frameRate = qMax(1.0, v.toDouble()); rateGiven = true; }
                if (const QString v = attr(a, "subFrameRate"); !v.isEmpty()) subFrameRate = qMax(1.0, v.toDouble());
                if (const QString v = attr(a, "tickRate"); !v.isEmpty()) tickRate = v.toDouble();
                if (const QStringList m = attr(a, "frameRateMultiplier").split(QLatin1Char(' '), Qt::SkipEmptyParts); m.size() == 2
                    && m.at(1).toDouble() > 0)
                    multiplier = m.at(0).toDouble() / m.at(1).toDouble();
            }
            if (name == QLatin1String("br") && inP) { words += QLatin1Char('\n'); }
            bool ok = false;
            const QString beginText = attr(a, "begin");
            const double begin = beginText.isEmpty() ? 0.0 : seconds(beginText, &ok);
            const double parent = offsets.last();
            if (name == QLatin1String("p")) {
                ++inP;
                words.clear();
                pBegin = parent + (ok ? begin : 0.0);
                bool endOk = false, durOk = false;
                const double end = seconds(attr(a, "end"), &endOk);
                const double dur = seconds(attr(a, "dur"), &durOk);
                pEnd = endOk ? parent + end : (durOk ? pBegin + dur : -1);
                timed.append(false);
            } else if (!beginText.isEmpty() && ok && (name == QLatin1String("body") || name == QLatin1String("div"))) {
                offsets.append(parent + begin);
                timed.append(true);
            } else {
                timed.append(false);
            }
        } else if (token == QXmlStreamReader::Characters && inP) {
            QString t = xml.text().toString();
            t.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
            words += t;
        } else if (token == QXmlStreamReader::EndElement) {
            if (!timed.isEmpty() && timed.takeLast() && offsets.size() > 1) offsets.removeLast();
            if (xml.name() == QLatin1String("p") && inP) {
                --inP;
                QStringList lines = words.split(QLatin1Char('\n'));
                for (QString &l : lines) l = l.trimmed();
                lines.removeAll(QString());
                const qint64 a = qRound64(pBegin * fps), b = qRound64(pEnd * fps);
                if (!lines.isEmpty() && pBegin >= 0 && b > a)
                    cues.append(QVariantMap{{"start", int(a)}, {"frames", int(b - a)}, {"text", lines.join(QLatin1Char('\n'))}});
            }
        }
    }
    if (language) *language = std::move(lang);
    std::stable_sort(cues.begin(), cues.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("start")).toInt() < b.toMap().value(QStringLiteral("start")).toInt();
    });
    if (frameTimed) *frameTimed = timeBase == QLatin1String("smpte");
    return cues;
}

ReadResult read(const QByteArray &raw, double fps, const QString &fileName, qint64 timecodeStart) {
    ReadResult result;
    QString text = QString::fromUtf8(raw);
    if (text.contains(QChar::ReplacementCharacter)) text = QString::fromLatin1(raw);   // not UTF-8: an old Latin-1 file
    if (text.startsWith(QChar(0xFEFF))) text.remove(0, 1);
    const QString head = text.left(512).trimmed();
    bool programmeTimed = false;
    if (head.startsWith(QStringLiteral("Scenarist_SCC"))) {
        result.format = QStringLiteral("scc");
        result.cues = readScc(text, fps);
        programmeTimed = true;
    } else if (head.startsWith(QLatin1Char('<')) && head.contains(QStringLiteral("<tt"))) {
        result.format = QStringLiteral("itt");
        result.cues = readItt(text, fps, &result.language, &programmeTimed);
    } else {
        result.format = head.startsWith(QStringLiteral("WEBVTT")) ? QStringLiteral("vtt") : QStringLiteral("srt");
        result.cues = Timeline::parseSubtitles(text, fps);
        if (result.format == QLatin1String("vtt")) {
            // "Language: fr" among the header's lines, before the first blank one.
            for (const QString &line : text.section(QRegularExpression(QStringLiteral("\\r?\\n\\s*\\r?\\n")), 0, 0).split(QLatin1Char('\n')))
                if (line.trimmed().startsWith(QStringLiteral("Language:"), Qt::CaseInsensitive))
                    result.language = line.section(QLatin1Char(':'), 1).trimmed();
        }
    }
    // Times written from the programme's starting timecode count from the timeline's first frame.
    if (programmeTimed && timecodeStart > 0 && !result.cues.isEmpty()
        && result.cues.first().toMap().value(QStringLiteral("start")).toLongLong() >= timecodeStart)
        for (QVariant &v : result.cues) {
            QVariantMap cue = v.toMap();
            cue.insert(QStringLiteral("start"), int(cue.value(QStringLiteral("start")).toLongLong() - timecodeStart));
            v = cue;
        }
    if (result.language.isEmpty() || result.language == QLatin1String("und"))
        result.language = languageFromFileName(fileName);
    return result;
}

} // namespace CaptionFormats
