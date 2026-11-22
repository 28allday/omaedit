#include "volumes.h"
#include "backgroundtasks.h"
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QLocale>
#include <QSet>
#include <QStorageInfo>
#include <QVariantMap>
#include <algorithm>
#include <utility>

Volumes::Volumes(QObject *parent) : QObject(parent) {
    m_pool.setMaxThreadCount(1);
    connect(&m_timer, &QTimer::timeout, this, &Volumes::refreshDiskFree);
    connect(&m_timer, &QTimer::timeout, this, &Volumes::refreshDrives);
    m_timer.start(30000);
    m_mounted = scanDrives(); // at once, for the first view
    m_drives = m_mounted;
    useDiskBus(QDBusConnection::systemBus());
}

void Volumes::useDiskBus(const QDBusConnection &bus) {
    delete m_disks;
    m_disks = new RemovableDisks(bus, this);
    connect(m_disks, &RemovableDisks::offersChanged, this, [this] {
        compose();
        // What is mounted changed: read the volumes again for their space.
        refreshDrives();
    });
    connect(m_disks, &RemovableDisks::finished, this,
            [this](const QString &object, const QString &action, bool ok, const QString &message, const QString &point) {
                if (ok)
                    refreshDrives();
                emit diskActionFinished(object, action, ok, message, point);
            });
    compose();
}

void Volumes::compose() {
    const QList<DiskOffer> offers = m_disks ? m_disks->offers() : QList<DiskOffer>();
    QVariantList drives = withDisks(m_mounted, offers);
    const bool available = canMountDrives();
    if (drives == m_drives && available == m_diskServiceSeen)
        return;
    m_drives = std::move(drives);
    m_diskServiceSeen = available;
    emit drivesChanged();
}

bool Volumes::refuseInUse(const QString &object, const QString &action) {
    const DiskOffer *offer = m_disks ? m_disks->offer(object) : nullptr;
    if (!offer || !m_inUse)
        return false;
    QStringList points = offer->mountPoints;
    if (action == QLatin1String("eject"))
        for (const DiskOffer &other : m_disks->offers())
            if (other.drive == offer->drive && other.object != object)
                points += other.mountPoints;
    for (const QString &point : std::as_const(points)) {
        const QString file = m_inUse(point);
        if (file.isEmpty())
            continue;
        const QString name = QFileInfo(file).fileName();
        emit diskActionFinished(object, action, false,
                                action == QLatin1String("eject")
                                    ? tr("%1 stays in: %2 on it is used by this project.").arg(offer->name, name)
                                    : tr("%1 stays mounted: %2 on it is used by this project.").arg(offer->name, name),
                                QString());
        return true;
    }
    return false;
}

QString Volumes::driveOf(const QString &path) const {
    if (path.trimmed().isEmpty())
        return QString();
    // The nearest folder that exists, as the kernel names it.
    QString at = QDir::cleanPath(QDir::current().absoluteFilePath(path.trimmed()));
    while (!QFileInfo::exists(at) && at != QStringLiteral("/"))
        at = QFileInfo(at).absolutePath();
    if (const QString real = QFileInfo(at).canonicalFilePath(); !real.isEmpty())
        at = real;
    QString best;
    bool removable = false;
    for (const QVariant &value : m_drives) {
        const QVariantMap row = value.toMap();
        if (!row.value(QStringLiteral("mounted"), true).toBool())
            continue;
        QString point = row.value(QStringLiteral("path")).toString();
        if (const QString real = QFileInfo(point).canonicalFilePath(); !real.isEmpty())
            point = real;
        if (point.isEmpty() || point.size() <= best.size())
            continue;
        const QString root = point.endsWith(QLatin1Char('/')) ? point : point + QLatin1Char('/');
        if (at != point && !at.startsWith(root))
            continue;
        best = point;
        removable = row.value(QStringLiteral("removable")).toBool();
    }
    return removable ? best : QString();
}

void Volumes::mount(const QString &object) { m_disks->mount(object); }

void Volumes::unmount(const QString &object) {
    if (!refuseInUse(object, QStringLiteral("unmount")))
        m_disks->unmount(object);
}

void Volumes::eject(const QString &object) {
    if (!refuseInUse(object, QStringLiteral("eject")))
        m_disks->eject(object);
}

// A scan still running posts its result to this object: wait for it.
Volumes::~Volumes() { m_pool.waitForDone(); }

void Volumes::setProjectPath(const QString &path) {
    m_projectPath = path;
    refreshDiskFree();
}

void Volumes::refreshDrives() {
    // statvfs on every mount, network ones included: on the pool, published here.
    // A scan already running may have read the mounts before the change
    // that asks for this one: one more after it.
    if (m_drivesPending) {
        m_drivesAgain = true;
        return;
    }
    m_drivesPending = true;
    m_pool.start([this] {
        const QVariantList drives = scanDrives();
        QMetaObject::invokeMethod(this, [this, drives] {
            m_drivesPending = false;
            m_mounted = drives;
            compose();
            if (m_drivesAgain) {
                m_drivesAgain = false;
                refreshDrives();
            }
        }, Qt::QueuedConnection);
    });
}

QVariantList Volumes::scanDrives() {
    static const QSet<QString> virtualFs = {
        QStringLiteral("tmpfs"), QStringLiteral("devtmpfs"), QStringLiteral("overlay"),
        QStringLiteral("squashfs"), QStringLiteral("efivarfs"), QStringLiteral("proc"),
        QStringLiteral("sysfs"), QStringLiteral("cgroup2"), QStringLiteral("ramfs"),
        QStringLiteral("fuse.portal"), QStringLiteral("autofs"), QStringLiteral("binfmt_misc")};
    static const QSet<QString> networkFs = {
        QStringLiteral("nfs"), QStringLiteral("nfs4"), QStringLiteral("cifs"),
        QStringLiteral("smb3"), QStringLiteral("fuse.sshfs"), QStringLiteral("9p"),
        QStringLiteral("fuse.rclone"), QStringLiteral("davfs")};
    QVariantList drives;
    const QLocale locale;
    // One row per device: btrfs subvolumes (/home, /var/log, …) mount the
    // same disk several times, and the shortest mount point is the one an
    // editor recognises.
    QHash<QByteArray, QString> shortestByDevice;
    for (const QStorageInfo &volume : QStorageInfo::mountedVolumes()) {
        if (!volume.isValid() || !volume.isReady() || volume.bytesTotal() <= 0)
            continue;
        const QByteArray device = volume.device();
        if (device.isEmpty())
            continue;
        const auto it = shortestByDevice.constFind(device);
        if (it == shortestByDevice.constEnd() || volume.rootPath().length() < it->length())
            shortestByDevice.insert(device, volume.rootPath());
    }
    for (const QStorageInfo &volume : QStorageInfo::mountedVolumes()) {
        if (!volume.device().isEmpty() && shortestByDevice.value(volume.device()) != volume.rootPath())
            continue;
        if (!volume.isValid() || !volume.isReady() || volume.bytesTotal() <= 0)
            continue;
        const QString fs = QString::fromLatin1(volume.fileSystemType());
        const QString root = volume.rootPath();
        if (virtualFs.contains(fs) || root.startsWith(QStringLiteral("/proc"))
            || root.startsWith(QStringLiteral("/sys")) || root.startsWith(QStringLiteral("/dev"))
            || root.startsWith(QStringLiteral("/boot")) || root.startsWith(QStringLiteral("/snap"))
            || root.startsWith(QStringLiteral("/var/")))
            continue;
        if (root.startsWith(QStringLiteral("/run")) && !root.startsWith(QStringLiteral("/run/media")))
            continue;
        QString name = volume.name();
        if (name.isEmpty())
            name = root == QStringLiteral("/") ? QStringLiteral("System")
                                                : QFileInfo(root).fileName();
        if (name.isEmpty())
            name = root;
        QVariantMap d;
        d.insert(QStringLiteral("name"), name);
        d.insert(QStringLiteral("path"), root);
        d.insert(QStringLiteral("total"), double(volume.bytesTotal()));
        d.insert(QStringLiteral("free"), double(volume.bytesAvailable()));
        d.insert(QStringLiteral("totalLabel"), locale.formattedDataSize(volume.bytesTotal(), 1, QLocale::DataSizeSIFormat));
        d.insert(QStringLiteral("freeLabel"), locale.formattedDataSize(volume.bytesAvailable(), 1, QLocale::DataSizeSIFormat));
        d.insert(QStringLiteral("fraction"),
                 1.0 - double(volume.bytesAvailable()) / double(volume.bytesTotal()));
        d.insert(QStringLiteral("removable"),
                 root.startsWith(QStringLiteral("/run/media")) || root.startsWith(QStringLiteral("/media"))
                     || root.startsWith(QStringLiteral("/mnt")));
        d.insert(QStringLiteral("network"), networkFs.contains(fs));
        d.insert(QStringLiteral("fileSystem"), fs);
        d.insert(QStringLiteral("device"), QString::fromLocal8Bit(volume.device()));
        drives.append(d);
    }
    std::sort(drives.begin(), drives.end(), [](const QVariant &a, const QVariant &b) {
        const QString pa = a.toMap().value(QStringLiteral("path")).toString();
        const QString pb = b.toMap().value(QStringLiteral("path")).toString();
        if ((pa == QStringLiteral("/")) != (pb == QStringLiteral("/")))
            return pa == QStringLiteral("/");
        return pa < pb;
    });
    return drives;
}

namespace {
// One device by its kernel name: /dev/mapper/luks-… and /dev/dm-3 are the same.
QString sameDevice(const QString &device) {
    const QString canonical = QFileInfo(device).canonicalFilePath();
    return canonical.isEmpty() ? device : canonical;
}
bool rootFirst(const QVariant &a, const QVariant &b) {
    const QVariantMap ma = a.toMap(), mb = b.toMap();
    const bool aMounted = ma.value(QStringLiteral("mounted"), true).toBool();
    const bool bMounted = mb.value(QStringLiteral("mounted"), true).toBool();
    if (aMounted != bMounted)
        return aMounted; // the drives not mounted come after the ones that are
    if (!aMounted)
        return ma.value(QStringLiteral("name")).toString().localeAwareCompare(mb.value(QStringLiteral("name")).toString()) < 0;
    const QString pa = ma.value(QStringLiteral("path")).toString();
    const QString pb = mb.value(QStringLiteral("path")).toString();
    if ((pa == QStringLiteral("/")) != (pb == QStringLiteral("/")))
        return pa == QStringLiteral("/");
    return pa < pb;
}
} // namespace

QVariantList Volumes::withDisks(const QVariantList &mounted, const QList<DiskOffer> &offers) {
    const QLocale locale;
    QVariantList rows;
    QSet<QString> shown;
    const auto sizeLabel = [&](qint64 bytes) { return locale.formattedDataSize(bytes, 1, QLocale::DataSizeSIFormat); };
    const auto withActions = [](QVariantMap &row, const DiskOffer *offer) {
        row.insert(QStringLiteral("object"), offer ? offer->object : QString());
        row.insert(QStringLiteral("canMount"), offer && !offer->mounted() && !offer->locked);
        row.insert(QStringLiteral("canUnmount"), offer && offer->mounted());
        row.insert(QStringLiteral("canEject"), offer && offer->canEject());
        row.insert(QStringLiteral("locked"), offer && offer->locked);
    };
    for (const QVariant &value : mounted) {
        QVariantMap row = value.toMap();
        const QString path = row.value(QStringLiteral("path")).toString();
        const QString device = sameDevice(row.value(QStringLiteral("device")).toString());
        const DiskOffer *offer = nullptr;
        bool stale = false;
        for (const DiskOffer &o : offers) {
            if (o.mountPoints.contains(path)) {
                offer = &o;
                break;
            }
            // udisks2 has heard of the unmount before the volumes were read again.
            if (!device.isEmpty() && sameDevice(o.device) == device && !o.mounted())
                stale = true;
        }
        if (stale && !offer)
            continue;
        row.insert(QStringLiteral("mounted"), true);
        withActions(row, offer);
        row.insert(QStringLiteral("tip"), path);
        if (offer) {
            row.insert(QStringLiteral("removable"), true);
            shown.insert(offer->object);
        }
        rows.append(row);
    }
    for (const DiskOffer &offer : offers) {
        if (shown.contains(offer.object))
            continue;
        QVariantMap row;
        row.insert(QStringLiteral("name"), offer.name);
        row.insert(QStringLiteral("total"), double(offer.size));
        row.insert(QStringLiteral("free"), 0.0);
        row.insert(QStringLiteral("totalLabel"), sizeLabel(offer.size));
        row.insert(QStringLiteral("freeLabel"), QString());
        row.insert(QStringLiteral("fraction"), -1.0);
        row.insert(QStringLiteral("removable"), true);
        row.insert(QStringLiteral("network"), false);
        row.insert(QStringLiteral("fileSystem"), offer.fileSystem);
        row.insert(QStringLiteral("device"), offer.device);
        withActions(row, &offer);
        if (offer.mounted()) {
            // Mounted a moment ago and not read yet: its own space when the
            // mount point is the volume's root already, else just its size.
            const QString point = offer.mountPoints.constFirst();
            row.insert(QStringLiteral("path"), point);
            row.insert(QStringLiteral("mounted"), true);
            row.insert(QStringLiteral("tip"), point);
            const QStorageInfo volume(point);
            if (volume.isValid() && volume.isReady() && volume.rootPath() == point && volume.bytesTotal() > 0) {
                row.insert(QStringLiteral("total"), double(volume.bytesTotal()));
                row.insert(QStringLiteral("free"), double(volume.bytesAvailable()));
                row.insert(QStringLiteral("totalLabel"), sizeLabel(volume.bytesTotal()));
                row.insert(QStringLiteral("freeLabel"), sizeLabel(volume.bytesAvailable()));
                row.insert(QStringLiteral("fraction"), 1.0 - double(volume.bytesAvailable()) / double(volume.bytesTotal()));
            }
        } else {
            row.insert(QStringLiteral("path"), QString());
            row.insert(QStringLiteral("mounted"), false);
            row.insert(QStringLiteral("tip"),
                       offer.locked ? tr("Locked: %1 is encrypted. Unlock it from the desktop to use it here; "
                                         "OmaEdit does not ask for passphrases.").arg(offer.device)
                                    : tr("Not mounted (%1, %2). Click to mount it.").arg(offer.device, offer.fileSystem));
        }
        rows.append(row);
    }
    std::stable_sort(rows.begin(), rows.end(), rootFirst);
    return rows;
}

void Volumes::refreshDiskFree() {
    const QString where = m_projectPath.isEmpty()
                              ? QDir::homePath()
                              : QFileInfo(m_projectPath).absolutePath();
    const QStorageInfo volume(where);
    QString text;
    if (volume.isValid() && volume.isReady())
        text = QLocale().formattedDataSize(volume.bytesAvailable(), 1, QLocale::DataSizeSIFormat)
               + QStringLiteral(" free");
    const bool low = volume.isValid() && volume.isReady() && BackgroundTasks::lowSpace(volume.bytesAvailable(), volume.bytesTotal());
    if (text == m_diskFree && low == m_diskLow) return;
    if (low && !m_diskLow) emit becameLow(text);
    m_diskLow = low;
    m_diskFree = std::move(text);
    emit diskFreeChanged();
}
