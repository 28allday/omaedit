#include "lookuptables.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QRegularExpression>
#include <QTextStream>
#include <cmath>

namespace render {
namespace {
QString tr(const char *text) { return QCoreApplication::translate("LookupTables", text); }

std::shared_ptr<const engine::LookupTable> read(const QString &path, bool tiles, QString *why) {
    auto table = std::make_shared<engine::LookupTable>();
    auto bad = [&](const char *text) { if (why) *why = tr(text); return std::shared_ptr<const engine::LookupTable>(); };
    if (path.endsWith(QLatin1String(".cube"), Qt::CaseInsensitive)) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return bad("The LUT file cannot be read.");
        int size1 = 0, size3 = 0;
        QTextStream in(&f);
        static const QRegularExpression space(QStringLiteral("\\s+"));
        while (!in.atEnd()) {
            const QString line = in.readLine().trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
            // Spaces or tabs between the numbers, as files are written.
            const QStringList w = line.split(space, Qt::SkipEmptyParts);
            const QString word = w.first().toUpper();
            if (word == QLatin1String("LUT_3D_SIZE") && w.size() == 2) size3 = w[1].toInt();
            else if (word == QLatin1String("LUT_1D_SIZE") && w.size() == 2) size1 = w[1].toInt();
            else if ((word == QLatin1String("DOMAIN_MIN") || word == QLatin1String("DOMAIN_MAX")) && w.size() == 4)
                for (int i = 0; i < 3; ++i) (word == QLatin1String("DOMAIN_MIN") ? table->domainMin : table->domainMax)[i] = w[i + 1].toFloat();
            else if ((word == QLatin1String("LUT_1D_INPUT_RANGE") || word == QLatin1String("LUT_3D_INPUT_RANGE")) && w.size() == 3)
                for (int i = 0; i < 3; ++i) { table->domainMin[i] = w[1].toFloat(); table->domainMax[i] = w[2].toFloat(); }
            else if (word.at(0).isDigit() || word.at(0) == QLatin1Char('-') || word.at(0) == QLatin1Char('.') || word.at(0) == QLatin1Char('+')) {
                if (w.size() != 3) return bad("The LUT file has a line that is not three numbers.");
                // No more than the largest table there can be (128³).
                if (table->rgb.size() >= size_t(128) * 128 * 128 * 3) return bad("The LUT file does not have as many entries as its size says.");
                for (const QString &n : w) {
                    bool ok = false;
                    const float v = n.toFloat(&ok);
                    if (!ok || !std::isfinite(v)) return bad("The LUT file has a line that is not three numbers.");
                    table->rgb.push_back(v);
                }
            }
        }
        if ((size1 > 0) == (size3 > 0)) return bad("The LUT file must have one size, 1D or 3D.");
        table->cube = size3 > 0;
        table->size = table->cube ? size3 : size1;
        if (table->size < 2 || (table->cube ? table->size > 128 : table->size > 16384)) return bad("The LUT file's size is not supported.");
        const size_t entries = table->cube ? size_t(table->size) * table->size * table->size : size_t(table->size);
        if (table->rgb.size() != entries * 3) return bad("The LUT file does not have as many entries as its size says.");
        for (int i = 0; i < 3; ++i) {
            // An endless domain places every value nowhere (its scale is zero).
            if (!std::isfinite(table->domainMin[i]) || !std::isfinite(table->domainMax[i])) return bad("The LUT file's domain is not a range of numbers.");
            if (!(table->domainMax[i] > table->domainMin[i])) return bad("The LUT file's domain is empty.");
        }
        return table;
    }
    QImage image(path);
    if (image.isNull()) return bad("The LUT picture cannot be read.");
    image = image.convertToFormat(QImage::Format_RGB32);
    const int w = image.width(), h = image.height();
    int n = 0, layout = 0;  // 0 Hald, 1 tiles, 2 strip across, 3 strip down
    if (w == h * h) { n = h; layout = 2; }
    else if (h == w * w) { n = w; layout = 3; }
    else if (w == h) {
        const int l = int(std::lround(std::cbrt(double(w))));
        if (l * l * l == w) { n = l * l; layout = tiles ? 1 : 0; }
    }
    if (n < 2 || n > 128) return bad("The LUT picture is not a Hald, tiled or strip LUT.");
    table->size = n;
    table->rgb.resize(size_t(n) * n * n * 3);
    const int perRow = int(std::lround(std::sqrt(double(n))));
    for (int b = 0; b < n; ++b) for (int g = 0; g < n; ++g) for (int r = 0; r < n; ++r) {
        int x = 0, y = 0;
        const size_t i = (size_t(b) * n + g) * n + r;
        switch (layout) {
        case 0: x = int(i % size_t(w)); y = int(i / size_t(w)); break;
        case 1: x = r + (b % perRow) * n; y = g + (b / perRow) * n; break;
        case 2: x = r + b * n; y = g; break;
        default: x = r; y = g + b * n; break;
        }
        const QRgb c = image.pixel(x, y);
        table->rgb[i * 3] = qRed(c) / 255.f; table->rgb[i * 3 + 1] = qGreen(c) / 255.f; table->rgb[i * 3 + 2] = qBlue(c) / 255.f;
    }
    return table;
}
}  // namespace

std::shared_ptr<const engine::LookupTable> loadLookupTable(const QString &path, bool tiles, QString *error) {
    struct Kept { QDateTime modified; qint64 size = -1; std::shared_ptr<const engine::LookupTable> table; };
    static QMutex mutex;
    static QHash<QString, Kept> kept;
    if (path.isEmpty()) { if (error) *error = tr("No LUT file chosen."); return nullptr; }
    const QFileInfo info(path);
    const QString key = path + (tiles ? QStringLiteral("|tiles") : QStringLiteral("|hald"));
    {
        QMutexLocker lock(&mutex);
        const auto it = kept.constFind(key);
        if (it != kept.constEnd() && it->modified == info.lastModified() && it->size == info.size()) return it->table;
    }
    auto table = read(path, tiles, error);
    if (!table) return nullptr;
    QMutexLocker lock(&mutex);
    if (kept.size() > 32) kept.clear();
    kept.insert(key, {info.lastModified(), info.size(), table});
    return table;
}

}  // namespace render
