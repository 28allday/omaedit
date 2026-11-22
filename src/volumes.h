#pragma once
#include "removabledisks.h"
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QTimer>
#include <QVariantList>
#include <functional>

// Disks, as the editor shows them.
//
// drives: the volumes for the Media page's storage sidebar and the file
// browser's places, {name, path, total, free, totalLabel, freeLabel,
// fraction, removable, network, fileSystem, mounted, object, canMount,
// canUnmount, canEject, locked, tip}. The mounted ones, and (from udisks2)
// the plugged-in drives that are not mounted, with path "" and fraction -1;
// object is the udisks2 block the actions take ("" for a volume udisks2
// does not offer). diskFree: the free space on the volume holding the
// project (home until it is saved), as "1.2 TB free"; diskLow when that is
// short. Both are read again every 30 s and whenever the project moves, and
// the drives at once whenever udisks2 says a drive came, went or was mounted.
class Volumes : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList drives READ drives NOTIFY drivesChanged)
    // udisks2 answers: drives can be mounted and ejected from the app.
    Q_PROPERTY(bool canMountDrives READ canMountDrives NOTIFY drivesChanged)
    Q_PROPERTY(QString diskFree READ diskFree NOTIFY diskFreeChanged)
    Q_PROPERTY(bool diskLow READ diskLow NOTIFY diskFreeChanged)
  public:
    explicit Volumes(QObject *parent = nullptr);
    ~Volumes() override;
    QVariantList drives() const { return m_drives; }
    QString diskFree() const { return m_diskFree; }
    bool diskLow() const { return m_diskLow; }
    bool canMountDrives() const { return m_disks && m_disks->available(); }
    Q_INVOKABLE void refreshDrives();
    // Through udisks2; each ends in diskActionFinished.
    Q_INVOKABLE void mount(const QString &object);
    Q_INVOKABLE void unmount(const QString &object);
    Q_INVOKABLE void eject(const QString &object);
    // A file the open project uses under a mount point ("" = none): such a
    // drive is not unmounted or ejected. The owner sets it.
    void setInUse(std::function<QString(const QString &mountPoint)> inUse) { m_inUse = std::move(inUse); }
    // The mount point of the removable drive a file or folder is on ("" when
    // it is on the computer's own disks): the deepest listed volume holding
    // it, read through links as far as the path exists, so a folder not made
    // yet is placed by the one it would be made in.
    Q_INVOKABLE QString driveOf(const QString &path) const;
    // Talk to udisks2 on this bus instead of the system's (tests).
    void useDiskBus(const QDBusConnection &bus);
    // The mounted volumes with udisks2's offers folded in (see drives).
    static QVariantList withDisks(const QVariantList &mounted, const QList<DiskOffer> &offers);
    // Where the project is now ("" = not saved); reads its free space again.
    // The owner calls this once at the start, after connecting becameLow.
    void setProjectPath(const QString &path);
    static QVariantList scanDrives();
  signals:
    void drivesChanged();
    void diskFreeChanged();
    void becameLow(const QString &free); // the project volume has just run short
    // action: mount | unmount | eject. message: plain words for the status line.
    void diskActionFinished(const QString &object, const QString &action, bool ok,
                            const QString &message, const QString &mountPoint);

  private:
    void refreshDiskFree();
    void compose();
    bool refuseInUse(const QString &object, const QString &action);
    QVariantList m_mounted, m_drives;
    bool m_drivesPending = false, m_drivesAgain = false, m_diskServiceSeen = false;
    RemovableDisks *m_disks = nullptr;
    std::function<QString(const QString &)> m_inUse;
    QString m_projectPath, m_diskFree;
    bool m_diskLow = false;
    QTimer m_timer;
    QThreadPool m_pool;
};
