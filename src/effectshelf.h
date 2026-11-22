#pragma once
#include <QObject>
#include <QStringList>
#include <QVariantList>

// The effects shelf (Edit ▸ Effects tab): favourite effect keys, the last
// eight used (most recent first), and named custom bins of keys. Presets
// ride as "preset:<name>". Kept in QSettings.
class EffectShelf : public QObject {
    Q_OBJECT
    Q_PROPERTY(QStringList favorites READ favorites NOTIFY changed)
    Q_PROPERTY(QStringList recents READ recents NOTIFY changed)
    Q_PROPERTY(QVariantList bins READ bins NOTIFY changed)
  public:
    explicit EffectShelf(QObject *parent = nullptr);
    QStringList favorites() const { return m_favorites; }
    QStringList recents() const { return m_recents; }
    QVariantList bins() const { return m_bins; } // [{name, keys}]
    Q_INVOKABLE void toggleFavorite(const QString &key);
    Q_INVOKABLE void noteUsed(const QString &key);
    Q_INVOKABLE void clearRecents();
    Q_INVOKABLE bool addBin(const QString &name);
    Q_INVOKABLE bool renameBin(const QString &name, const QString &to);
    Q_INVOKABLE void deleteBin(const QString &name);
    Q_INVOKABLE void addToBin(const QString &bin, const QString &key);
    Q_INVOKABLE void removeFromBin(const QString &bin, const QString &key);
    Q_INVOKABLE QStringList binKeys(const QString &bin) const;
    // A key that no longer exists (a deleted preset) leaves every shelf.
    void dropKey(const QString &key);
  signals:
    void changed();
    void status(const QString &text); // a line for the status bar

  private:
    void store();
    QStringList m_favorites, m_recents;
    QVariantList m_bins;
};
