#include "filepicker.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QQmlEngine>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>

namespace {
QList<FileChooserHost *> &hosts() {
    static QList<FileChooserHost *> list;
    return list;
}
}

void FileChooser::openFile(const QString &title, const QString &filterName, const QStringList &patterns) {
    request(QStringLiteral("open"), title, filterName, patterns);
}
void FileChooser::openFiles(const QString &title, const QString &filterName, const QStringList &patterns) {
    request(QStringLiteral("multiple"), title, filterName, patterns);
}
void FileChooser::openDirectory(const QString &title) {
    request(QStringLiteral("directory"), title);
}
void FileChooser::saveFile(const QString &title, const QString &suggestedName,
                           const QString &filterName, const QStringList &patterns) {
    request(QStringLiteral("save"), title, filterName, patterns, suggestedName);
}

void FileChooser::request(const QString &mode, const QString &title, const QString &filterName,
                          const QStringList &patterns, const QString &suggestedName) {
    auto *host = FileChooserHost::forChooser(this);
    if (!host) {
        emit failed(tr("The file browser needs an open OmaEdit window."));
        return;
    }
    // Remember accepted folders per operation; LUT browsing should not displace
    // the import folder. Canceled selections leave the remembered path alone.
    const auto key = QStringLiteral("fileChooser/folders/") + QString::fromLatin1(
        QCryptographicHash::hash((mode + '\n' + title + '\n' + patterns.join(' ')).toUtf8(),
                                QCryptographicHash::Sha256).toHex());
    QString folder = QSettings().value(key).toString();
    if (!QFileInfo(folder).isDir()) folder = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    QString name = suggestedName;
    if (QDir::isAbsolutePath(name)) {
        const QFileInfo suggested(name);
        if (QFileInfo(suggested.absolutePath()).isDir()) folder = suggested.absolutePath();
        name = suggested.fileName();
    }
    QStringList filters;
    QVariantList filterPatterns;
    const auto addFilter = [&](const QString &label, const QStringList &matches) {
        filters.append(label);
        filterPatterns.append(QVariant(matches));
    };
    const auto addFormats = [&](const QStringList &matches) {
        for (const auto &pattern : matches) {
            if (pattern.startsWith("*.") && !pattern.mid(2).contains(QRegularExpression("[?*\\[\\]]")))
                addFilter(tr("%1 files (%2)").arg(pattern.mid(2).toUpper(), pattern), {pattern});
        }
    };
    if (!patterns.isEmpty()) {
        addFilter(QStringLiteral("%1 (%2)").arg(filterName.isEmpty() ? tr("Files") : filterName, patterns.join(' ')), patterns);
        if (mode != "save" && patterns.size() > 1) addFormats(patterns);
        addFilter(tr("All files (*)"), {});
    } else {
        addFilter(tr("All files (*)"), {});
        if (mode == "open" || mode == "multiple") {
            const QStringList video{"*.mp4", "*.mov", "*.mkv", "*.avi", "*.mts", "*.m2ts", "*.webm", "*.mxf", "*.m4v", "*.mpg", "*.mpeg", "*.wmv", "*.ts", "*.ogv"};
            const QStringList audio{"*.mp3", "*.wav", "*.flac", "*.aac", "*.m4a", "*.ogg", "*.aif", "*.aiff", "*.opus"};
            const QStringList images{"*.jpg", "*.jpeg", "*.png", "*.tif", "*.tiff", "*.bmp", "*.webp", "*.exr", "*.gif", "*.psd"};
            addFilter(tr("Video files"), video);
            addFilter(tr("Audio files"), audio);
            addFilter(tr("Images"), images);
            addFormats(video + audio + images);
        }
    }
    // The browser applies the suffix before checking for an existing destination, so
    // typing "project" cannot bypass the overwrite prompt for "project.omaedit".
    QString suffix = QFileInfo(name).suffix();
    if (suffix.isEmpty() && patterns.size() == 1 && patterns.first().startsWith("*.") &&
        !patterns.first().mid(2).contains(QRegularExpression(QStringLiteral("[?*\\[\\]]"))))
        suffix = patterns.first().mid(2);
    host->present(this, {{"mode", mode}, {"title", title}, {"filters", filters}, {"filterPatterns", filterPatterns}, {"patterns", patterns},
        {"folder", QUrl::fromLocalFile(folder)}, {"file", name.isEmpty() ? QUrl() : QUrl::fromLocalFile(QDir(folder).filePath(name))},
        {"suffix", suffix}, {"settingsKey", key}});
}

FileChooserHost::FileChooserHost(QObject *parent) : QObject(parent) { hosts().append(this); }
FileChooserHost::~FileChooserHost() {
    hosts().removeAll(this);
    if (m_active) emit m_active->canceled();
}
FileChooserHost *FileChooserHost::forChooser(FileChooser *chooser) {
    for (QObject *owner = chooser->parent(); owner; owner = owner->parent())
        for (auto *host : hosts())
            if (host->owner() == owner) return host;
    return nullptr;
}
QVariantMap FileChooserHost::pathInfo(const QString &text, const QUrl &folder, const QString &suffix) const {
    QString path = text;
    if (path.startsWith(QStringLiteral("file:"))) path = QUrl(path).toLocalFile();
    else if (path.contains(QStringLiteral("://"))) return {{"error", tr("Choose a local file or a mounted folder.")}};
    if (path == "~") path = QDir::homePath();
    else if (path.startsWith("~/")) path = QDir::homePath() + path.mid(1);
    if (path.isEmpty()) return {{"error", tr("Enter a file or folder path.")}};
    if (QDir::isRelativePath(path)) path = QDir(folder.toLocalFile()).filePath(path);
    path = QDir::cleanPath(path);
    QFileInfo file(path);
    if (!file.isDir() && !suffix.isEmpty() && file.suffix().isEmpty()) {
        path += '.' + suffix;
        file.setFile(path);
    }
    return {{"path", path}, {"url", QUrl::fromLocalFile(path)}, {"name", file.fileName()},
        {"parent", QUrl::fromLocalFile(file.absolutePath())}, {"exists", file.exists()},
        {"directory", file.isDir()}, {"readable", file.isReadable()},
        {"writable", file.exists() ? file.isWritable() : QFileInfo(file.absolutePath()).isWritable()},
        {"parentExists", QFileInfo(file.absolutePath()).isDir()}};
}
QVariantList FileChooserHost::locations() const {
    QVariantList result;
    for (const auto type : {QStandardPaths::HomeLocation, QStandardPaths::DesktopLocation,
                           QStandardPaths::DownloadLocation, QStandardPaths::DocumentsLocation,
                           QStandardPaths::MoviesLocation, QStandardPaths::MusicLocation, QStandardPaths::PicturesLocation}) {
        const QString path = QStandardPaths::writableLocation(type);
        if (path.isEmpty() || !QFileInfo(path).isDir()) continue;
        // A place goes by its folder's own name, as a file manager shows it
        // (Qt calls the videos folder "Movies" whatever it is called on disk).
        QString name = type == QStandardPaths::HomeLocation ? QString() : QFileInfo(QDir::cleanPath(path)).fileName();
        if (name.isEmpty())
            name = type == QStandardPaths::MoviesLocation ? tr("Videos") : QStandardPaths::displayName(type);
        result.append(QVariantMap{{"name", name}, {"path", path}});
    }
    return result;
}
void FileChooserHost::present(FileChooser *chooser, QVariantMap options) {
    const QPointer<FileChooser> previous = m_active;
    m_active = nullptr;
    disconnect(m_destroyed);
    emit dismissed();
    // The same caller has already updated its intent when replacing a request.
    if (previous && previous != chooser) emit previous->canceled();
    m_active = chooser;
    options.insert("id", ++m_requestId);
    m_options = options;
    m_destroyed = connect(chooser, &QObject::destroyed, this, [this] { m_active = nullptr; emit dismissed(); });
    emit requested(options);
}
void FileChooserHost::accept(int requestId, const QList<QUrl> &urls) {
    if (requestId != m_requestId || !m_active) return;
    if (urls.isEmpty()) { cancel(requestId); return; }
    const auto mode = m_options.value("mode").toString();
    for (const auto &url : urls) {
        const QFileInfo file(url.toLocalFile());
        QString error;
        const auto patterns = m_options.value("patterns").toStringList();
        if (mode == "save" && !patterns.isEmpty() && !QDir::match(patterns, file.fileName()))
            error = tr("Choose a filename matching %1.").arg(patterns.join(QStringLiteral(", ")));
        else if (!url.isLocalFile() || (mode == "directory" ? !file.isDir() :
            mode == "save" ? file.isDir() || !QFileInfo(file.absolutePath()).isDir() : !file.isFile())) {
            error = tr("The selected path is no longer available. Please choose it again.");
        }
        // Do not let a writer append a different suffix after the browser has
        // confirmed overwriting a path: that would approve the wrong file.
        if (!error.isEmpty()) {
            const QPointer<FileChooser> chooser = m_active;
            m_active = nullptr;
            disconnect(m_destroyed);
            emit dismissed();
            if (chooser) emit chooser->failed(error);
            return;
        }
    }
    const auto chosen = mode == "multiple" ? urls : QList<QUrl>{urls.first()};
    const QFileInfo first(chosen.first().toLocalFile());
    QSettings().setValue(m_options.value("settingsKey").toString(), mode == "directory" ? first.absoluteFilePath() : first.absolutePath());
    const QPointer<FileChooser> chooser = m_active;
    m_active = nullptr;
    disconnect(m_destroyed);
    emit dismissed();
    if (chooser) emit chooser->selected(chosen.first());
    if (chooser) emit chooser->selectedMany(chosen);
}
void FileChooserHost::cancel(int requestId) {
    if (requestId != m_requestId || !m_active) return;
    const QPointer<FileChooser> chooser = m_active;
    m_active = nullptr;
    disconnect(m_destroyed);
    emit dismissed();
    if (chooser) emit chooser->canceled();
}

FileBrowserFilterModel::FileBrowserFilterModel(QObject *parent) : QSortFilterProxyModel(parent) {
    connect(this, &QAbstractItemModel::rowsInserted, this, &FileBrowserFilterModel::countChanged);
    connect(this, &QAbstractItemModel::rowsRemoved, this, &FileBrowserFilterModel::countChanged);
    connect(this, &QAbstractItemModel::modelReset, this, &FileBrowserFilterModel::countChanged);
    connect(this, &QAbstractItemModel::layoutChanged, this, &FileBrowserFilterModel::countChanged);
}
void FileBrowserFilterModel::setSourceModel(QAbstractItemModel *model) {
    const auto roles = model ? model->roleNames() : QHash<int, QByteArray>();
    m_nameRole = roles.key("fileName", -1);
    m_directoryRole = roles.key("fileIsDir", -1);
    QSortFilterProxyModel::setSourceModel(model);
}
void FileBrowserFilterModel::setQuery(const QString &query) {
    const auto next = query.trimmed();
    if (next == m_query) return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
#endif
    m_query = next;
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    endFilterChange(Direction::Rows);
#else
    invalidateFilter();
#endif
    emit queryChanged();
}
void FileBrowserFilterModel::setPatterns(const QStringList &patterns) {
    if (patterns == m_patterns) return;
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
#endif
    m_patterns = patterns;
    m_matchers.clear();
    for (const auto &pattern : patterns)
        m_matchers.append(QRegularExpression(QRegularExpression::wildcardToRegularExpression(pattern), QRegularExpression::CaseInsensitiveOption));
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    endFilterChange(Direction::Rows);
#else
    invalidateFilter();
#endif
    emit patternsChanged();
}
QVariant FileBrowserFilterModel::get(int row, const QString &role) const {
    if (row < 0 || row >= rowCount()) return {};
    const int key = roleNames().key(role.toUtf8(), -1);
    return key < 0 ? QVariant() : data(index(row, 0), key);
}
bool FileBrowserFilterModel::filterAcceptsRow(int row, const QModelIndex &parent) const {
    if (!sourceModel() || m_nameRole < 0) return false;
    const auto item = sourceModel()->index(row, 0, parent);
    const auto name = item.data(m_nameRole).toString();
    // Query text is literal: camera names containing [, ?, * or # stay searchable.
    if (!name.contains(m_query, Qt::CaseInsensitive)) return false;
    if (item.data(m_directoryRole).toBool() || m_matchers.isEmpty()) return true;
    for (const auto &matcher : m_matchers)
        if (matcher.match(name).hasMatch()) return true;
    return false;
}

static void registerFileChooserHost() {
    qmlRegisterType<FileChooserHost>("OmaEdit.Dialogs", 1, 0, "FileChooserHost");
    qmlRegisterType<FileBrowserFilterModel>("OmaEdit.Dialogs", 1, 0, "FileBrowserFilterModel");
}
Q_COREAPP_STARTUP_FUNCTION(registerFileChooserHost)
