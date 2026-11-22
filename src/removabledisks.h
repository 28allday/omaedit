#pragma once
#include <QDBusConnection>
#include <QDBusError>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>
#include <functional>

class QDBusArgument;
class QDBusMessage;

// One filesystem the desktop's disk service (udisks2) would let the person at
// the computer mount: a partition (or a whole stick with no partition table)
// on a drive that is not part of the system. Internal disks, swap, recovery
// partitions and drives with no medium in them are never offered.
struct DiskOffer {
    QString object;           // the udisks2 block object, e.g. …/block_devices/sdb1
    QString drive;            // its drive object
    QString device;           // /dev/sdb1
    QString name;             // label, else the drive's model, else "<size> Volume"
    QString fileSystem;       // vfat, exfat, ext4, crypto_LUKS…
    QStringList mountPoints;  // empty while not mounted
    qint64 size = 0;
    bool locked = false;      // an encrypted partition nobody has unlocked
    bool ejectable = false;   // the drive takes Eject (a disc, a card reader)
    bool canPowerOff = false; // the drive can be powered off (a USB stick or enclosure)
    bool mounted() const { return !mountPoints.isEmpty(); }
    bool canEject() const { return ejectable || canPowerOff; }
    bool operator==(const DiskOffer &) const = default;
};

// udisks2 over D-Bus: what can be mounted, and mounting, unmounting and
// ejecting it as the file managers do (polkit lets the logged-in user mount
// removable drives; nothing here runs a shell, sudo or pkexec). Follows the
// service's own signals (drives plugged or pulled, mounted or unmounted from
// the desktop) and never polls. With no udisks2, offers() stays empty.
class RemovableDisks : public QObject {
    Q_OBJECT
  public:
    static constexpr const char *Service = "org.freedesktop.UDisks2";

    // The system bus normally; tests hand in a private bus with a fake service.
    explicit RemovableDisks(const QDBusConnection &bus, QObject *parent = nullptr);
    bool available() const { return m_available; }
    QList<DiskOffer> offers() const { return m_offers; }
    const DiskOffer *offer(const QString &object) const;

    // Each ends in finished(); message is plain words for the status line.
    void mount(const QString &object);
    void unmount(const QString &object);
    // Unmounts every filesystem on the drive, then ejects it (or powers it
    // off when it cannot eject, as a USB enclosure).
    void eject(const QString &object);

    // A GetManagedObjects reply as plain values: object path → interface →
    // properties, byte strings as QByteArray, lists of them as a QVariantList,
    // object paths as strings.
    static QVariantMap plainObjects(const QDBusArgument &reply);
    static QList<DiskOffer> offersFrom(const QVariantMap &objects);
    static QString plainError(const QDBusError &error, const QString &action, const QString &name);

  signals:
    void offersChanged();
    // action: "mount" | "unmount" | "eject"; mountPoint is set after a mount.
    void finished(const QString &object, const QString &action, bool ok, const QString &message,
                  const QString &mountPoint);

  private slots:
    void somethingChanged(const QDBusMessage &message);

  private:
    void fetch();
    using Done = std::function<void(const QDBusMessage &reply)>;
    void call(const QString &object, const QString &interface, const QString &method,
              const QString &action, const QString &name, const Done &done);
    QDBusConnection m_bus;
    QList<DiskOffer> m_offers;
    bool m_available = false;
    bool m_fetching = false, m_fetchAgain = false;
    // Counts what this side changed: a reading asked for before a mount or
    // unmount here finished is out of date when it lands, and is read again.
    int m_changes = 0;
    void changedHere();
    QTimer m_settle;
};
