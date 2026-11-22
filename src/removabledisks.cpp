#include "removabledisks.h"
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QFileInfo>
#include <QHash>
#include <QLocale>
#include <algorithm>
#include <memory>

namespace {
const QString kRoot = QStringLiteral("/org/freedesktop/UDisks2");
const QString kBlock = QStringLiteral("org.freedesktop.UDisks2.Block");
const QString kFilesystem = QStringLiteral("org.freedesktop.UDisks2.Filesystem");
const QString kEncrypted = QStringLiteral("org.freedesktop.UDisks2.Encrypted");
const QString kDrive = QStringLiteral("org.freedesktop.UDisks2.Drive");
const QString kObjectManager = QStringLiteral("org.freedesktop.DBus.ObjectManager");
const QString kProperties = QStringLiteral("org.freedesktop.DBus.Properties");
// A mount can wait on the desktop's polkit agent asking the person.
constexpr int kActionTimeoutMs = 120000;

// udisks2 keeps names and mount points as NUL-terminated byte strings.
QString bytesText(const QVariant &value) {
    QByteArray bytes = value.toByteArray();
    while (bytes.endsWith('\0'))
        bytes.chop(1);
    return QString::fromUtf8(bytes);
}

// A property value as plain Qt values; what is not needed here is dropped.
QVariant plainValue(const QVariant &value) {
    if (value.userType() == qMetaTypeId<QDBusObjectPath>())
        return value.value<QDBusObjectPath>().path();
    if (value.userType() == qMetaTypeId<QDBusVariant>())
        return plainValue(value.value<QDBusVariant>().variant());
    if (value.userType() != qMetaTypeId<QDBusArgument>())
        return value;
    const QDBusArgument arg = value.value<QDBusArgument>();
    const QString signature = arg.currentSignature();
    QVariantList list;
    if (signature == QLatin1String("aay")) {
        arg.beginArray();
        while (!arg.atEnd()) {
            QByteArray bytes;
            arg >> bytes;
            list.append(bytes);
        }
        arg.endArray();
        return list;
    }
    if (signature == QLatin1String("ao")) {
        arg.beginArray();
        while (!arg.atEnd()) {
            QDBusObjectPath path;
            arg >> path;
            list.append(path.path());
        }
        arg.endArray();
        return list;
    }
    return {};
}

QString objectPath(const QVariant &value) {
    const QString path = value.toString();
    return path == QLatin1String("/") ? QString() : path;
}
} // namespace

RemovableDisks::RemovableDisks(const QDBusConnection &bus, QObject *parent)
    : QObject(parent), m_bus(bus) {
    // Changes come in bursts (a drive, then each partition): read once after.
    m_settle.setSingleShot(true);
    m_settle.setInterval(150);
    connect(&m_settle, &QTimer::timeout, this, &RemovableDisks::fetch);
    if (!m_bus.isConnected())
        return;
    const QString service = QString::fromLatin1(Service);
    m_bus.connect(service, kRoot, kObjectManager, QStringLiteral("InterfacesAdded"), this,
                  SLOT(somethingChanged(QDBusMessage)));
    m_bus.connect(service, kRoot, kObjectManager, QStringLiteral("InterfacesRemoved"), this,
                  SLOT(somethingChanged(QDBusMessage)));
    m_bus.connect(service, QString(), kProperties, QStringLiteral("PropertiesChanged"), this,
                  SLOT(somethingChanged(QDBusMessage)));
    // The service starting late, restarting or going away.
    auto *watcher = new QDBusServiceWatcher(service, m_bus,
                                            QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, [this] { m_settle.start(); });
    fetch();
}

const DiskOffer *RemovableDisks::offer(const QString &object) const {
    for (const DiskOffer &o : m_offers)
        if (o.object == object)
            return &o;
    return nullptr;
}

void RemovableDisks::somethingChanged(const QDBusMessage &message) {
    if (message.member() == QLatin1String("PropertiesChanged")) {
        const QString interface = message.arguments().value(0).toString();
        if (interface != kFilesystem && interface != kBlock && interface != kDrive && interface != kEncrypted)
            return;
    }
    m_settle.start();
}

void RemovableDisks::changedHere() {
    ++m_changes;
    emit offersChanged();
}

void RemovableDisks::fetch() {
    if (!m_bus.isConnected())
        return;
    if (m_fetching) {
        m_fetchAgain = true;
        return;
    }
    m_fetching = true;
    const int changes = m_changes;
    const QDBusMessage request = QDBusMessage::createMethodCall(
        QString::fromLatin1(Service), kRoot, kObjectManager, QStringLiteral("GetManagedObjects"));
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(request), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, changes] {
        watcher->deleteLater();
        m_fetching = false;
        if (changes != m_changes) {
            m_fetchAgain = false;
            fetch();
            return;
        }
        const QDBusMessage reply = watcher->reply();
        QList<DiskOffer> offers;
        const bool ok = reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty();
        if (ok)
            offers = offersFrom(plainObjects(reply.arguments().constFirst().value<QDBusArgument>()));
        m_available = ok;
        if (offers != m_offers) {
            m_offers = std::move(offers);
            emit offersChanged();
        }
        if (m_fetchAgain) {
            m_fetchAgain = false;
            fetch();
        }
    });
}

QVariantMap RemovableDisks::plainObjects(const QDBusArgument &reply) {
    // a{oa{sa{sv}}}
    QVariantMap objects;
    if (reply.currentSignature() != QLatin1String("a{oa{sa{sv}}}"))
        return objects;
    reply.beginMap();
    while (!reply.atEnd()) {
        reply.beginMapEntry();
        QDBusObjectPath path;
        reply >> path;
        QVariantMap interfaces;
        reply.beginMap();
        while (!reply.atEnd()) {
            reply.beginMapEntry();
            QString interface;
            QVariantMap properties;
            reply >> interface >> properties;
            reply.endMapEntry();
            for (auto it = properties.begin(); it != properties.end(); ++it)
                *it = plainValue(*it);
            interfaces.insert(interface, properties);
        }
        reply.endMap();
        reply.endMapEntry();
        objects.insert(path.path(), interfaces);
    }
    reply.endMap();
    return objects;
}

QList<DiskOffer> RemovableDisks::offersFrom(const QVariantMap &objects) {
    QList<DiskOffer> offers;
    const auto interfaceOf = [&](const QString &path, const QString &interface) {
        return objects.value(path).toMap().value(interface).toMap();
    };
    for (auto it = objects.constBegin(); it != objects.constEnd(); ++it) {
        const QVariantMap interfaces = it.value().toMap();
        const QVariantMap block = interfaces.value(kBlock).toMap();
        if (block.isEmpty())
            continue;
        const bool hasFilesystem = interfaces.contains(kFilesystem);
        const bool encrypted = interfaces.contains(kEncrypted);
        if ((!hasFilesystem && !encrypted) || block.value(QStringLiteral("HintIgnore")).toBool())
            continue;
        // An unlocked partition: its cleartext device has no drive of its
        // own and stands in for the encrypted one, which is not listed again.
        QVariantMap owner = block;
        if (const QString backing = objectPath(block.value(QStringLiteral("CryptoBackingDevice"))); !backing.isEmpty())
            owner = interfaceOf(backing, kBlock);
        if (encrypted && !objectPath(interfaces.value(kEncrypted).toMap().value(QStringLiteral("CleartextDevice"))).isEmpty())
            continue;
        // Internal disks (the system's own, and every non-removable one
        // udisks2 calls a system device) need an administrator: never offered.
        if (owner.value(QStringLiteral("HintSystem"), true).toBool())
            continue;
        const QString drivePath = objectPath(owner.value(QStringLiteral("Drive")));
        const QVariantMap drive = interfaceOf(drivePath, kDrive);
        if (drive.isEmpty() || !drive.value(QStringLiteral("MediaAvailable"), true).toBool())
            continue;
        DiskOffer offer;
        offer.object = it.key();
        offer.drive = drivePath;
        offer.device = bytesText(block.value(QStringLiteral("Device")));
        offer.fileSystem = block.value(QStringLiteral("IdType")).toString();
        offer.size = block.value(QStringLiteral("Size")).toLongLong();
        offer.locked = encrypted && !hasFilesystem;
        offer.ejectable = drive.value(QStringLiteral("Ejectable")).toBool();
        offer.canPowerOff = drive.value(QStringLiteral("CanPowerOff")).toBool();
        if (hasFilesystem)
            for (const QVariant &point : interfaces.value(kFilesystem).toMap().value(QStringLiteral("MountPoints")).toList())
                if (const QString text = bytesText(point); !text.isEmpty())
                    offer.mountPoints.append(text);
        offer.name = block.value(QStringLiteral("IdLabel")).toString().trimmed();
        if (offer.name.isEmpty())
            offer.name = block.value(QStringLiteral("HintName")).toString().trimmed();
        if (offer.name.isEmpty())
            offer.name = (drive.value(QStringLiteral("Vendor")).toString().trimmed() + QLatin1Char(' ')
                          + drive.value(QStringLiteral("Model")).toString().trimmed()).trimmed();
        if (offer.name.isEmpty())
            offer.name = tr("%1 Volume").arg(QLocale().formattedDataSize(offer.size, 1, QLocale::DataSizeSIFormat));
        offers.append(offer);
    }
    // Two unlabelled partitions of one drive would read the same: the
    // device names tell them apart.
    QHash<QString, int> uses;
    for (const DiskOffer &o : offers)
        ++uses[o.name];
    for (DiskOffer &o : offers)
        if (uses.value(o.name) > 1)
            o.name += QStringLiteral(" (") + QFileInfo(o.device).fileName() + QLatin1Char(')');
    std::sort(offers.begin(), offers.end(), [](const DiskOffer &a, const DiskOffer &b) {
        return a.name.localeAwareCompare(b.name) < 0 || (a.name == b.name && a.object < b.object);
    });
    return offers;
}

QString RemovableDisks::plainError(const QDBusError &error, const QString &action, const QString &name) {
    const QString code = error.name();
    const QString text = error.message();
    if (code.startsWith(QLatin1String("org.freedesktop.UDisks2.Error.NotAuthorized")))
        return action == QLatin1String("mount")     ? tr("You are not allowed to mount %1.").arg(name)
               : action == QLatin1String("unmount") ? tr("You are not allowed to unmount %1.").arg(name)
                                                    : tr("You are not allowed to eject %1.").arg(name);
    if (code == QLatin1String("org.freedesktop.UDisks2.Error.DeviceBusy")
        || text.contains(QLatin1String("target is busy"), Qt::CaseInsensitive))
        return tr("%1 is busy: another program has a file on it open.").arg(name);
    if (code == QLatin1String("org.freedesktop.UDisks2.Error.NotSupported")
        || text.contains(QLatin1String("unknown filesystem"), Qt::CaseInsensitive)
        || text.contains(QLatin1String("wrong fs type"), Qt::CaseInsensitive))
        return tr("%1 has no filesystem this computer knows how to read.").arg(name);
    if (code == QLatin1String("org.freedesktop.UDisks2.Error.Cancelled")
        || code == QLatin1String("org.freedesktop.UDisks2.Error.NotAuthorizedDismissed"))
        return tr("Nothing was changed: the request for %1 was cancelled.").arg(name);
    if (code == QLatin1String("org.freedesktop.DBus.Error.ServiceUnknown")
        || code == QLatin1String("org.freedesktop.DBus.Error.NameHasNoOwner")
        || code == QLatin1String("org.freedesktop.DBus.Error.NoServer")
        || code == QLatin1String("org.freedesktop.DBus.Error.Disconnected")
        || code == QLatin1String("org.freedesktop.DBus.Error.UnknownObject"))
        return tr("The disk service (udisks2) is not running, so drives cannot be mounted or ejected from OmaEdit.");
    if (code == QLatin1String("org.freedesktop.DBus.Error.NoReply")
        || code == QLatin1String("org.freedesktop.UDisks2.Error.Timedout"))
        return tr("The disk service did not answer in time about %1.").arg(name);
    const QString detail = text.isEmpty() ? code : text;
    return action == QLatin1String("mount")     ? tr("Could not mount %1: %2").arg(name, detail)
           : action == QLatin1String("unmount") ? tr("Could not unmount %1: %2").arg(name, detail)
                                                : tr("Could not eject %1: %2").arg(name, detail);
}

void RemovableDisks::call(const QString &object, const QString &interface, const QString &method,
                          const QString &action, const QString &name, const Done &done) {
    QDBusMessage request = QDBusMessage::createMethodCall(QString::fromLatin1(Service), object, interface, method);
    request << QVariantMap(); // no options, as the file managers mount
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(request, kActionTimeoutMs), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, object, action, name, done] {
        watcher->deleteLater();
        const QDBusMessage reply = watcher->reply();
        if (reply.type() != QDBusMessage::ReplyMessage) {
            emit finished(object, action, false, plainError(watcher->error(), action, name), QString());
            return;
        }
        done(reply);
    });
}

void RemovableDisks::mount(const QString &object) {
    const DiskOffer *found = offer(object);
    if (!found) {
        emit finished(object, QStringLiteral("mount"), false,
                      m_available ? tr("That drive is no longer plugged in.")
                                  : tr("The disk service (udisks2) is not running, so drives cannot be mounted or ejected from OmaEdit."),
                      QString());
        return;
    }
    const QString name = found->name;
    if (found->locked) {
        emit finished(object, QStringLiteral("mount"), false,
                      tr("%1 is encrypted and locked. Unlock it from the desktop first; OmaEdit does not ask for passphrases.").arg(name),
                      QString());
        return;
    }
    if (found->mounted()) {
        emit finished(object, QStringLiteral("mount"), true, QString(), found->mountPoints.constFirst());
        return;
    }
    call(object, kFilesystem, QStringLiteral("Mount"), QStringLiteral("mount"), name, [this, object, name](const QDBusMessage &reply) {
        const QString point = reply.arguments().value(0).toString();
        // Shown at once; the service's own signal confirms it a moment later.
        for (DiskOffer &o : m_offers)
            if (o.object == object && !o.mountPoints.contains(point))
                o.mountPoints.prepend(point);
        changedHere();
        emit finished(object, QStringLiteral("mount"), true, tr("Mounted %1 at %2.").arg(name, point), point);
        m_settle.start();
    });
}

void RemovableDisks::unmount(const QString &object) {
    const DiskOffer *found = offer(object);
    if (!found || !found->mounted()) {
        emit finished(object, QStringLiteral("unmount"), true, QString(), QString());
        return;
    }
    const QString name = found->name;
    call(object, kFilesystem, QStringLiteral("Unmount"), QStringLiteral("unmount"), name, [this, object, name](const QDBusMessage &) {
        for (DiskOffer &o : m_offers)
            if (o.object == object)
                o.mountPoints.clear();
        changedHere();
        emit finished(object, QStringLiteral("unmount"), true, tr("Unmounted %1.").arg(name), QString());
        m_settle.start();
    });
}

void RemovableDisks::eject(const QString &object) {
    const DiskOffer *found = offer(object);
    if (!found || !found->canEject()) {
        emit finished(object, QStringLiteral("eject"), false, tr("That drive cannot be ejected from OmaEdit."), QString());
        return;
    }
    const QString name = found->name, drive = found->drive;
    const bool ejectable = found->ejectable;
    QStringList mounted;
    for (const DiskOffer &o : m_offers)
        if (o.drive == drive && o.mounted())
            mounted.append(o.object);
    // Each filesystem on the drive comes off first, one after another, then
    // the drive itself; the first refusal stops the rest.
    // The chain holds itself only while a call is out: a refusal lets it go.
    auto step = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weak = step;
    *step = [this, weak, mounted, object, name, drive, ejectable]() mutable {
        const auto self = weak.lock();
        if (!self)
            return;
        if (!mounted.isEmpty()) {
            const QString next = mounted.takeFirst();
            const DiskOffer *part = offer(next);
            call(next, kFilesystem, QStringLiteral("Unmount"), QStringLiteral("eject"), part ? part->name : name,
                 [this, self, next](const QDBusMessage &) {
                     for (DiskOffer &o : m_offers)
                         if (o.object == next)
                             o.mountPoints.clear();
                     changedHere();
                     (*self)();
                 });
            return;
        }
        call(drive, kDrive, ejectable ? QStringLiteral("Eject") : QStringLiteral("PowerOff"), QStringLiteral("eject"), name,
             [this, object, name](const QDBusMessage &) {
                 emit finished(object, QStringLiteral("eject"), true, tr("%1 can be removed now.").arg(name), QString());
                 m_settle.start();
             });
    };
    (*step)();
}
