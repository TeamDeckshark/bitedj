#pragma once

#include <QString>

namespace mixxx {

/// Sysfs path of the physical USB device a block device is attached to, e.g.
/// "/sys/devices/platform/.../usb1/1-1/1-1.5" for "/dev/sda1". Empty when the
/// device is not on USB (or cannot be resolved), and always empty off Linux.
///
/// A single USB stick can carry several mounted filesystems — most commonly two
/// partitions (Rekordbox export on one, Serato/audio on the other), but also two
/// LUNs of a multi-slot reader. Those appear as unrelated block devices
/// (/dev/sda1 + /dev/sda2, or /dev/sda1 + /dev/sdb1) yet share everything above
/// the USB device node in the sysfs topology:
///   .../usb1/1-1/1-1.5/1-1.5:1.0/host0/target0:0:0/0:0:0:0/block/sda/sda1
///                     ^ cut here
/// Everything after the USB device node is the USB interface (":1.0"), the SCSI
/// host and the block device itself, all of which differ between filesystems on
/// the same stick. So truncating there yields a key that groups exactly the
/// filesystems that live on one physical drive.
QString usbDeviceNodeForBlockDevice(const QString& device);

/// usbDeviceNodeForBlockDevice() for the filesystem mounted at `mountPoint`.
/// Empty when the mount point is invalid, not ready, or not backed by a USB
/// block device.
QString usbDeviceNodeForMountPoint(const QString& mountPoint);

/// Filesystem UUID of the volume mounted at `mountPoint` — the identity that
/// survives a re-plug. Mount points do not: the automounter names them after
/// the volume label, so pulling one stick and plugging in another labelled the
/// same reuses the path (the mistake the Rekordbox device table made). Empty
/// when the volume has no UUID, is not a block device, or off Linux, in which
/// case the caller has nothing better than the mount point to key on.
QString volumeUuidForMountPoint(const QString& mountPoint);

} // namespace mixxx
