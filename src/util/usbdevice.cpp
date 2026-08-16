#include "util/usbdevice.h"

#include <QDir>
#include <QFileInfo>
#include <QFileInfoList>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QStringList>

namespace mixxx {

QString usbDeviceNodeForBlockDevice(const QString& device) {
    const QString name = QFileInfo(device).fileName();
    if (name.isEmpty()) {
        return QString();
    }
    const QString sysfs =
            QFileInfo(QStringLiteral("/sys/class/block/") + name).canonicalFilePath();
    if (sysfs.isEmpty()) {
        return QString();
    }
    // USB device nodes are named "<bus>-<port>[.<port>...]"; the root hub
    // ("usb1") and the interface ("1-1.5:1.0") deliberately do not match.
    static const QRegularExpression usbNodeRe(
            QStringLiteral("^\\d+-\\d+(\\.\\d+)*$"));
    const QStringList parts = sysfs.split(QLatin1Char('/'));
    int lastUsbNode = -1;
    for (int i = 0; i < parts.size(); ++i) {
        if (usbNodeRe.match(parts.at(i)).hasMatch()) {
            lastUsbNode = i;
        }
    }
    if (lastUsbNode < 0) {
        return QString();
    }
    return parts.mid(0, lastUsbNode + 1).join(QLatin1Char('/'));
}

QString usbDeviceNodeForMountPoint(const QString& mountPoint) {
    const QStorageInfo info(mountPoint);
    if (!info.isValid() || !info.isReady()) {
        return QString();
    }
    return usbDeviceNodeForBlockDevice(QString::fromUtf8(info.device()));
}

QString volumeUuidForMountPoint(const QString& mountPoint) {
    const QStorageInfo info(mountPoint);
    if (!info.isValid() || !info.isReady()) {
        return QString();
    }
    const QString device = QFileInfo(QString::fromUtf8(info.device())).canonicalFilePath();
    if (device.isEmpty()) {
        // A tmpfs or any other non-block filesystem: nothing to resolve.
        return QString();
    }
    // /dev/disk/by-uuid holds one symlink per labelled filesystem pointing at
    // its block device. Reading the directory is how blkid's cache is reached
    // without linking against libblkid or shelling out to a helper.
    const QDir byUuid(QStringLiteral("/dev/disk/by-uuid"));
    const QFileInfoList links = byUuid.entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries);
    for (const QFileInfo& link : links) {
        if (link.canonicalFilePath() == device) {
            return link.fileName();
        }
    }
    return QString();
}

} // namespace mixxx
