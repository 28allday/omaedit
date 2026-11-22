#include "lutlibrary.h"
#include "colourpipeline.h"
#include "lutfilter.h"
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QVariantMap>
#include <algorithm>

LutLibrary::LutLibrary(QObject *parent) : QObject(parent) {
    QSettings settings;
    m_folder = settings.value(QStringLiteral("color/lutFolder")).toString();
    m_favorites = settings.value(QStringLiteral("color/lutFavorites")).toStringList();
    m_files = settings.value(QStringLiteral("color/lutFiles")).toStringList();
    rescan();
}

void LutLibrary::setFolder(const QString &folder) {
    m_folder = folder;
    QSettings().setValue(QStringLiteral("color/lutFolder"), folder);
    rescan();
}

void LutLibrary::addFile(const QString &path) {
    if (path.trimmed().isEmpty()) return;
    const QString absolute = QFileInfo(path).absoluteFilePath();
    if (!m_files.contains(absolute)) {
        m_files.append(absolute);
        QSettings().setValue(QStringLiteral("color/lutFiles"), m_files);
    }
    rescan();
}

void LutLibrary::setFavorite(const QString &path, bool on) {
    if (path.isEmpty()) return;
    if (on && !m_favorites.contains(path)) m_favorites.append(path);
    if (!on) m_favorites.removeAll(path);
    QSettings().setValue(QStringLiteral("color/lutFavorites"), m_favorites);
    for (QVariant &item : m_luts) {
        QVariantMap lut = item.toMap();
        lut.insert(QStringLiteral("favorite"), m_favorites.contains(lut.value(QStringLiteral("path")).toString()));
        item = lut;
    }
    emit changed();
}

void LutLibrary::rescan() {
    LutFilter::clearValidationCache();
    ColourPipeline::forgetLuts();
    QVariantList out;
    QStringList roots{QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/luts"),
                      QDir::homePath() + QStringLiteral("/LUTs"), QStringLiteral("/usr/share/omaedit/luts")};
    if (!m_folder.isEmpty()) roots.prepend(m_folder);
    QSet<QString> seen;
    auto add = [&](const QString &path, const QString &root) {
        const QFileInfo file(path);
        const QString absolute = file.absoluteFilePath();
        if (seen.contains(absolute)) return;
        seen.insert(absolute);
        const QString relative = root.isEmpty() ? QString() : QDir(root).relativeFilePath(file.absolutePath());
        const QString folder = root.isEmpty() ? file.absolutePath()
            : QFileInfo(root).fileName() + (relative == QStringLiteral(".") ? QString() : QLatin1Char('/') + relative);
        out.append(QVariantMap{{QStringLiteral("path"), absolute}, {QStringLiteral("name"), file.completeBaseName()},
                                {QStringLiteral("folder"), folder}, {QStringLiteral("folderPath"), file.absolutePath()},
                                {QStringLiteral("missing"), !file.exists()},
                                {QStringLiteral("favorite"), m_favorites.contains(absolute)}});
    };
    for (const QString &root : roots) {
        QDirIterator it(root, QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            const QString suffix = it.fileInfo().suffix().toLower();
            if (suffix == QStringLiteral("cube") || suffix == QStringLiteral("3dl")) add(it.filePath(), root);
        }
    }
    for (const QString &path : m_files) add(path, {});
    for (const QString &path : m_favorites) add(path, {});
    std::sort(out.begin(), out.end(), [](const QVariant &a, const QVariant &b) {
        const QVariantMap x = a.toMap(), y = b.toMap();
        const int order = x.value(QStringLiteral("name")).toString().localeAwareCompare(y.value(QStringLiteral("name")).toString());
        return order ? order < 0 : x.value(QStringLiteral("path")).toString() < y.value(QStringLiteral("path")).toString();
    });
    m_luts = out;
    emit changed();
}
