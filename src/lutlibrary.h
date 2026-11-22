#pragma once
#include <QObject>
#include <QStringList>
#include <QVariantList>

// The LUTs the Color page lists: every .cube / .3dl under the chosen folder,
// the app's own folders and ~/LUTs, plus single files added by hand and the
// favourites wherever they are. Folder, files and favourites live in
// QSettings under color/….
class LutLibrary : public QObject {
    Q_OBJECT
    // [{path, name, folder, folderPath, missing, favorite}], by name.
    Q_PROPERTY(QVariantList luts READ luts NOTIFY changed)
    Q_PROPERTY(QString folder READ folder NOTIFY changed)
  public:
    explicit LutLibrary(QObject *parent = nullptr);
    QVariantList luts() const { return m_luts; }
    QString folder() const { return m_folder; }
    Q_INVOKABLE void setFolder(const QString &folder);
    Q_INVOKABLE void rescan();
    Q_INVOKABLE void addFile(const QString &path);
    Q_INVOKABLE void setFavorite(const QString &path, bool on);
  signals:
    void changed();

  private:
    QVariantList m_luts;
    QString m_folder;
    QStringList m_favorites, m_files;
};
