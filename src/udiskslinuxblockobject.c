/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2007-2010 David Zeuthen <zeuthen@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "config.h"
#include <glib/gi18n-lib.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>
#include <mntent.h>

#include <sys/ioctl.h>
#include <sys/file.h>
#include <linux/fs.h>

#include <string.h>
#include <stdlib.h>
#include <glib/gstdio.h>

#include "udiskslogging.h"
#include "udisksdaemon.h"
#include "udisksdaemonutil.h"
#include "udiskslinuxblockobject.h"
#include "udiskslinuxblock.h"
#include "udisksmount.h"
#include "udisksmountmonitor.h"
#include "udiskslinuxdriveobject.h"
#include "udiskslinuxdrive.h"
#include "udiskslinuxpartitiontable.h"
#include "udiskslinuxpartition.h"
#include "udiskslinuxfilesystem.h"
#include "udiskslinuxencrypted.h"
#include "udiskslinuxswapspace.h"
#include "udiskslinuxloop.h"
#include "udiskslinuxprovider.h"
#include "udiskscrypttabmonitor.h"
#include "udiskscrypttabentry.h"
#include "udiskslinuxdevice.h"
#include "udisksmodulemanager.h"
#include "udisksmodule.h"
#include "udisksmoduleobject.h"

/**
 * SECTION:udiskslinuxblockobject
 * @title: UDisksLinuxBlockObject
 * @short_description: Object representing a block device on Linux.
 *
 * Object corresponding to a block device on Linux.
 */

typedef struct _UDisksLinuxBlockObjectClass   UDisksLinuxBlockObjectClass;

/**
 * UDisksLinuxBlockObject:
 *
 * The #UDisksLinuxBlockObject structure contains only private data and
 * should only be accessed using the provided API.
 */
struct _UDisksLinuxBlockObject
{
  UDisksObjectSkeleton parent_instance;

  UDisksDaemon *daemon;
  UDisksMountMonitor *mount_monitor;

  UDisksLinuxDevice *device;
  GMutex device_mutex;

  GMutex cleanup_mutex;

  /* interface */
  UDisksBlock *iface_block_device;
  UDisksPartition *iface_partition;
  UDisksPartitionTable *iface_partition_table;
  UDisksFilesystem *iface_filesystem;
  UDisksSwapspace *iface_swapspace;
  UDisksEncrypted *iface_encrypted;
  UDisksLoop *iface_loop;
  GHashTable *module_ifaces;
};

struct _UDisksLinuxBlockObjectClass
{
  UDisksObjectSkeletonClass parent_class;
};

enum
{
  PROP_0,
  PROP_DAEMON,
  PROP_DEVICE
};

G_DEFINE_TYPE (UDisksLinuxBlockObject, udisks_linux_block_object, UDISKS_TYPE_OBJECT_SKELETON);

static void on_mount_monitor_mount_added_removed (UDisksMountMonitor  *monitor,
                                                  UDisksMount         *mount,
                                                  gpointer             user_data);

static void
udisks_linux_block_object_finalize (GObject *_object)
{
  UDisksLinuxBlockObject *object = UDISKS_LINUX_BLOCK_OBJECT (_object);

  /* note: we don't hold a ref to block->daemon or block->mount_monitor */
  g_warn_if_fail (g_signal_handlers_disconnect_by_func (object->mount_monitor, on_mount_monitor_mount_added_removed, object) == 1);

  g_object_unref (object->device);
  g_mutex_clear (&object->device_mutex);

  g_mutex_clear (&object->cleanup_mutex);

  if (object->iface_block_device != NULL)
    g_object_unref (object->iface_block_device);
  if (object->iface_partition != NULL)
    g_object_unref (object->iface_partition);
  if (object->iface_partition_table != NULL)
    g_object_unref (object->iface_partition_table);
  if (object->iface_filesystem != NULL)
    g_object_unref (object->iface_filesystem);
  if (object->iface_swapspace != NULL)
    g_object_unref (object->iface_swapspace);
  if (object->iface_encrypted != NULL)
    g_object_unref (object->iface_encrypted);
  if (object->iface_loop != NULL)
    g_object_unref (object->iface_loop);
  if (object->module_ifaces != NULL)
    g_hash_table_destroy (object->module_ifaces);

  if (G_OBJECT_CLASS (udisks_linux_block_object_parent_class)->finalize != NULL)
    G_OBJECT_CLASS (udisks_linux_block_object_parent_class)->finalize (_object);
}

static void
udisks_linux_block_object_get_property (GObject    *__object,
                                        guint       prop_id,
                                        GValue     *value,
                                        GParamSpec *pspec)
{
  UDisksLinuxBlockObject *object = UDISKS_LINUX_BLOCK_OBJECT (__object);

  switch (prop_id)
    {
    case PROP_DAEMON:
      g_value_set_object (value, udisks_linux_block_object_get_daemon (object));
      break;

    case PROP_DEVICE:
      g_value_set_object (value, udisks_linux_block_object_get_device (object));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
udisks_linux_block_object_set_property (GObject      *__object,
                                        guint         prop_id,
                                        const GValue *value,
                                        GParamSpec   *pspec)
{
  UDisksLinuxBlockObject *object = UDISKS_LINUX_BLOCK_OBJECT (__object);

  switch (prop_id)
    {
    case PROP_DAEMON:
      g_assert (object->daemon == NULL);
      /* we don't take a reference to the daemon */
      object->daemon = g_value_get_object (value);
      break;

    case PROP_DEVICE:
      g_assert (object->device == NULL);
      object->device = g_value_dup_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}


static void
udisks_linux_block_object_init (UDisksLinuxBlockObject *object)
{
}

static void
udisks_linux_block_object_constructed (GObject *_object)
{
  UDisksLinuxBlockObject *object = UDISKS_LINUX_BLOCK_OBJECT (_object);
  GString *str;
  UDisksBlock *block = NULL;
  UDisksPartition *partition = NULL;

  g_mutex_init (&object->device_mutex);
  g_mutex_init (&object->cleanup_mutex);

  object->module_ifaces = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_object_unref);

  object->mount_monitor = udisks_daemon_get_mount_monitor (object->daemon);
  g_signal_connect (object->mount_monitor,
                    "mount-added",
                    G_CALLBACK (on_mount_monitor_mount_added_removed),
                    object);
  g_signal_connect (object->mount_monitor,
                    "mount-removed",
                    G_CALLBACK (on_mount_monitor_mount_added_removed),
                    object);

  /* initial coldplug */
  udisks_linux_block_object_uevent (object, "add", NULL);

  /* compute the object path */
  str = g_string_new ("/org/freedesktop/UDisks2/block_devices/");
  udisks_safe_append_to_object_path (str, g_udev_device_get_name (object->device->udev_device));
  g_dbus_object_skeleton_set_object_path (G_DBUS_OBJECT_SKELETON (object), str->str);
  g_string_free (str, TRUE);

  block = udisks_object_peek_block (UDISKS_OBJECT (object));
  if (block && g_strcmp0 (udisks_block_get_crypto_backing_device (block), "/") != 0)
    udisks_linux_block_object_uevent (object, "change", NULL);

  /* run update for partitions again -- it sets the "Partitions" property
   * on the PartitionTable interface and we need the object path for that
   */
  partition = udisks_object_peek_partition (UDISKS_OBJECT (object));
  if (partition)
    udisks_linux_block_object_uevent (object, "change", NULL);

  if (G_OBJECT_CLASS (udisks_linux_block_object_parent_class)->constructed != NULL)
    G_OBJECT_CLASS (udisks_linux_block_object_parent_class)->constructed (_object);
}

static void
udisks_linux_block_object_class_init (UDisksLinuxBlockObjectClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize     = udisks_linux_block_object_finalize;
  gobject_class->constructed  = udisks_linux_block_object_constructed;
  gobject_class->set_property = udisks_linux_block_object_set_property;
  gobject_class->get_property = udisks_linux_block_object_get_property;

  /**
   * UDisksLinuxBlockObject:daemon:
   *
   * The #UDisksDaemon the object is for.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_DAEMON,
                                   g_param_spec_object ("daemon",
                                                        "Daemon",
                                                        "The daemon the object is for",
                                                        UDISKS_TYPE_DAEMON,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_WRITABLE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  /**
   * UDisksLinuxBlockObject:device:
   *
   * The #UDisksLinuxDevice for the object. Connect to the #GObject::notify
   * signal to get notified whenever this is updated.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_DEVICE,
                                   g_param_spec_object ("device",
                                                        "Device",
                                                        "The device for the object",
                                                        UDISKS_TYPE_LINUX_DEVICE,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_WRITABLE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

}

/**
 * udisks_linux_block_object_new:
 * @daemon: A #UDisksDaemon.
 * @device: The #UDisksLinuxDevice for the device.
 *
 * Create a new block object.
 *
 * Returns: A #UDisksLinuxBlockObject object. Free with g_object_unref().
 */
UDisksLinuxBlockObject *
udisks_linux_block_object_new (UDisksDaemon      *daemon,
                               UDisksLinuxDevice *device)
{
  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), NULL);
  return UDISKS_LINUX_BLOCK_OBJECT (g_object_new (UDISKS_TYPE_LINUX_BLOCK_OBJECT,
                                                  "daemon", daemon,
                                                  "device", device,
                                                  NULL));
}

/**
 * udisks_linux_block_object_get_daemon:
 * @object: A #UDisksLinuxBlockObject.
 *
 * Gets the daemon used by @object.
 *
 * Returns: A #UDisksDaemon. Do not free, the object is owned by @object.
 */
UDisksDaemon *
udisks_linux_block_object_get_daemon (UDisksLinuxBlockObject *object)
{
  g_return_val_if_fail (UDISKS_IS_LINUX_BLOCK_OBJECT (object), NULL);
  return object->daemon;
}

/**
 * udisks_linux_block_object_get_device:
 * @object: A #UDisksLinuxBlockObject.
 *
 * Gets the current #UDisksLinuxDevice for @object. Connect to
 * #GObject::notify to track changes to the #UDisksLinuxBlockObject:device
 * property.
 *
 * Returns: A #UDisksLinuxDevice. Free with g_object_unref().
 */
UDisksLinuxDevice *
udisks_linux_block_object_get_device (UDisksLinuxBlockObject *object)
{
  UDisksLinuxDevice *device;

  g_return_val_if_fail (UDISKS_IS_LINUX_BLOCK_OBJECT (object), NULL);

  g_mutex_lock (&object->device_mutex);
  device = g_object_ref (object->device);
  g_mutex_unlock (&object->device_mutex);

  return device;
}

/**
 * udisks_linux_block_object_get_device_file:
 * @object: A #UDisksLinuxBlockObject.
 *
 * Gets the device path for this object (eg. /dev/sda1).
 *
 * Returns: A device path. Free with g_free().
 */
gchar *
udisks_linux_block_object_get_device_file (UDisksLinuxBlockObject *object)
{
  UDisksLinuxDevice *device = NULL;
  gchar *device_file = NULL;

  g_return_val_if_fail (UDISKS_IS_LINUX_BLOCK_OBJECT (object), NULL);

  /* Return the device filename (eg.: /dev/sda1) */
  device = udisks_linux_block_object_get_device (object);
  device_file = g_strdup (g_udev_device_get_device_file (device->udev_device));

  /* Free resources. */
  g_object_unref (device);

  return device_file;
}

/**
 * udisks_linux_block_object_get_device_number:
 * @object: A #UDisksLinuxBlockObject.
 *
 * Gets the device number for this object.
 *
 * Returns: A dev_t device number or zero in case of an error.
 */
dev_t
udisks_linux_block_object_get_device_number (UDisksLinuxBlockObject *object)
{
  UDisksLinuxDevice *device;
  dev_t device_number;

  g_return_val_if_fail (UDISKS_IS_LINUX_BLOCK_OBJECT (object), 0);

  device = udisks_linux_block_object_get_device (object);
  device_number = g_udev_device_get_device_number (device->udev_device);

  /* Free resources. */
  g_object_unref (device);

  return device_number;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
update_iface (UDisksObject                     *object,
              const gchar                      *uevent_action,
              UDisksObjectHasInterfaceFunc      has_func,
              UDisksObjectConnectInterfaceFunc  connect_func,
              UDisksObjectUpdateInterfaceFunc   update_func,
              GType                             skeleton_type,
              gpointer                          _interface_pointer)
{
  gboolean has;
  gboolean add;
  GDBusInterface **interface_pointer = _interface_pointer;

  g_return_if_fail (object != NULL);
  g_return_if_fail (has_func != NULL);
  g_return_if_fail (update_func != NULL);
  g_return_if_fail (g_type_is_a (skeleton_type, G_TYPE_OBJECT));
  g_return_if_fail (g_type_is_a (skeleton_type, G_TYPE_DBUS_INTERFACE));
  g_return_if_fail (interface_pointer != NULL);
  g_return_if_fail (*interface_pointer == NULL || G_IS_DBUS_INTERFACE (*interface_pointer));

  add = FALSE;
  has = has_func (object);
  if (*interface_pointer == NULL)
    {
      if (has)
        {
          *interface_pointer = g_object_new (skeleton_type, NULL);
          g_warn_if_fail (*interface_pointer != NULL);
          if (connect_func != NULL)
            connect_func (object);
          add = TRUE;
        }
    }
  else
    {
      if (!has)
        {
          g_dbus_object_skeleton_remove_interface (G_DBUS_OBJECT_SKELETON (object),
                                                   G_DBUS_INTERFACE_SKELETON (*interface_pointer));
          g_object_unref (*interface_pointer);
          *interface_pointer = NULL;
        }
    }

  if (*interface_pointer != NULL)
    {
      update_func (object, uevent_action, G_DBUS_INTERFACE (*interface_pointer));
      if (add)
        g_dbus_object_skeleton_add_interface (G_DBUS_OBJECT_SKELETON (object),
                                              G_DBUS_INTERFACE_SKELETON (*interface_pointer));
    }
}

/* ---------------------------------------------------------------------------------------------------- */
/* org.freedesktop.UDisks2.Block */

static gboolean
block_device_check (UDisksObject *object)
{
  return TRUE;
}

static void
block_device_connect (UDisksObject *object)
{
}

static gboolean
block_device_update (UDisksObject   *object,
                     const gchar    *uevent_action,
                     GDBusInterface *_iface)
{
  udisks_linux_block_update (UDISKS_LINUX_BLOCK (_iface), UDISKS_LINUX_BLOCK_OBJECT (object));
  return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
disk_is_partitioned_by_kernel (GUdevDevice *device)
{
  gboolean ret = FALSE;
  GDir *dir = NULL;
  const gchar *name;
  const gchar *device_name;

  g_return_val_if_fail (g_strcmp0 (g_udev_device_get_devtype (device), "disk") == 0, FALSE);

  dir = g_dir_open (g_udev_device_get_sysfs_path (device), 0 /* flags */, NULL /* GError */);
  if (dir == NULL)
    goto out;

  device_name = g_udev_device_get_name (device);
  while ((name = g_dir_read_name (dir)) != NULL)
    {
      /* TODO: could check that it's a block device - for now, just
       * checking the name suffices
       */
      if (g_str_has_prefix (name, device_name))
        {
          ret = TRUE;
          goto out;
        }
    }

 out:
  if (dir != NULL)
    g_dir_close (dir);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */
/* org.freedesktop.UDisks2.PartitionTable */

static gboolean
partition_table_check (UDisksObject *object)
{
  UDisksLinuxBlockObject *block_object = UDISKS_LINUX_BLOCK_OBJECT (object);
  gboolean ret = FALSE;

  /* only consider whole disks, never partitions */
  if (g_strcmp0 (g_udev_device_get_devtype (block_object->device->udev_device), "disk") != 0)
    goto out;

  /* if blkid(8) already identified the device as a partition table, it's all good */
  if (g_udev_device_has_property (block_object->device->udev_device, "ID_PART_TABLE_TYPE"))
    {
      /* however, if blkid(8) also think that we're a filesystem... then don't
       * mark us as a partition table ... except if we are partitioned by the
       * kernel
       *
       * (see udisks_linux_block_object_contains_filesystem() for the similar case
       * where we don't pretend to be a filesystem)
       */
      if (g_strcmp0 (g_udev_device_get_property (block_object->device->udev_device, "ID_FS_USAGE"), "filesystem") == 0)
        {
          if (!disk_is_partitioned_by_kernel (block_object->device->udev_device))
            {
              goto out;
            }
        }
      ret = TRUE;
      goto out;
    }

  /* Note that blkid(8) might not detect all partition table
   * formats that the kernel knows about.... so we need to
   * double check...
   *
   * Fortunately, note that the kernel guarantees that all children
   * block devices that are partitions are created before the uevent
   * for the parent block device.... so if the parent block device has
   * children... then it must be partitioned by the kernel, hence it
   * must contain a partition table.
   */
  if (disk_is_partitioned_by_kernel (block_object->device->udev_device))
    {
      ret = TRUE;
      goto out;
    }

 out:
  return ret;
}

static void
partition_table_connect (UDisksObject *object)
{
}

static gboolean
partition_table_update (UDisksObject   *object,
                        const gchar    *uevent_action,
                        GDBusInterface *_iface)
{
  udisks_linux_partition_table_update (UDISKS_LINUX_PARTITION_TABLE (_iface), UDISKS_LINUX_BLOCK_OBJECT (object));
  return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */
/* org.freedesktop.UDisks2.Partition */

static gboolean
partition_check (UDisksObject *object)
{
  UDisksLinuxBlockObject *block_object = UDISKS_LINUX_BLOCK_OBJECT (object);
  gboolean ret = FALSE;

  /* could be partitioned by the kernel */
  if (g_strcmp0 (g_udev_device_get_devtype (block_object->device->udev_device), "partition") == 0)
    {
      ret = TRUE;
      goto out;
    }

  /* if blkid(8) already identified the device as a partition, it's all good */
  if (g_udev_device_has_property (block_object->device->udev_device, "ID_PART_ENTRY_SCHEME"))
    {
      ret = TRUE;
      goto out;
    }

 out:
  return ret;
}

static void
partition_connect (UDisksObject *object)
{
}

static gboolean
partition_update (UDisksObject   *object,
                  const gchar    *uevent_action,
                  GDBusInterface *_iface)
{
  udisks_linux_partition_update (UDISKS_LINUX_PARTITION (_iface), UDISKS_LINUX_BLOCK_OBJECT (object));
  return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */
/* org.freedesktop.UDisks2.Filesystem */

static gboolean
drive_does_not_detect_media_change (UDisksLinuxBlockObject *object)
{
  gboolean ret = FALSE;
  UDisksObject *drive_object;

  drive_object = udisks_daemon_find_object (object->daemon, udisks_block_get_drive (object->iface_block_device));
  if (drive_object != NULL)
    {
      UDisksDrive *drive = udisks_object_get_drive (drive_object);
      if (drive != NULL)
        {
          ret = ! udisks_drive_get_media_change_detected (drive);
          g_object_unref (drive);
        }
      g_object_unref (drive_object);
    }
  return ret;
}


static gboolean
contains_filesystem_locked (UDisksLinuxBlockObject *block_object, UDisksLinuxDevice *device)
{
  gboolean ret = FALSE;
  gboolean detected_as_filesystem = FALSE;
  UDisksMountType mount_type;

  /* if blkid(8) has detected the device as a filesystem, trust that */
  if (g_strcmp0 (udisks_block_get_id_usage (block_object->iface_block_device), "filesystem") == 0)
    {
      detected_as_filesystem = TRUE;
      /* except, if we are a whole-disk device and the kernel has already partitioned us...
       * in that case, don't pretend we're a filesystem
       *
       * (see partition_table_check() above for the similar case where we don't pretend
       * to be a partition table)
       */
      if (g_strcmp0 (g_udev_device_get_devtype (device->udev_device), "disk") == 0 &&
          disk_is_partitioned_by_kernel (device->udev_device))
        {
          detected_as_filesystem = FALSE;
        }
    }

  if (drive_does_not_detect_media_change (block_object) ||
      detected_as_filesystem ||
      (udisks_mount_monitor_is_dev_in_use (block_object->mount_monitor,
                                           g_udev_device_get_device_number (device->udev_device),
                                           &mount_type) &&
       mount_type == UDISKS_MOUNT_TYPE_FILESYSTEM))
    ret = TRUE;

  return ret;
}

static gboolean
contains_filesystem (UDisksObject *object)
{
  UDisksLinuxBlockObject *block_object = UDISKS_LINUX_BLOCK_OBJECT (object);

  return contains_filesystem_locked (block_object, block_object->device);
}

/**
 * udisks_linux_block_object_contains_filesystem:
 * @object: A #UDisksLinuxBlockObject.
 *
 * Check whether filesystem has been detected on the block device.
 *
 * Returns: %TRUE if the block device contains filesystem, %FALSE otherwise.
 */
gboolean
udisks_linux_block_object_contains_filesystem (UDisksObject *object)
{
  UDisksLinuxBlockObject *block_object;
  UDisksLinuxDevice *device;
  gboolean ret;

  g_return_val_if_fail (UDISKS_IS_LINUX_BLOCK_OBJECT (object), FALSE);
  block_object = UDISKS_LINUX_BLOCK_OBJECT (object);

  g_mutex_lock (&block_object->device_mutex);
  device = g_object_ref (block_object->device);
  g_mutex_unlock (&block_object->device_mutex);

  ret = contains_filesystem_locked (block_object, device);

  g_object_unref (device);

  return ret;
}


static void
filesystem_connect (UDisksObject *object)
{
}

static gboolean
filesystem_update (UDisksObject   *object,
                   const gchar    *uevent_action,
                   GDBusInterface *_iface)
{
  udisks_linux_filesystem_update (UDISKS_LINUX_FILESYSTEM (_iface), UDISKS_LINUX_BLOCK_OBJECT (object));
  return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */
/* org.freedesktop.UDisks2.Swapspace */

static gboolean
swapspace_check (UDisksObject *object)
{
  UDisksLinuxBlockObject *block_object = UDISKS_LINUX_BLOCK_OBJECT (object);
  gboolean ret;
  UDisksMountType mount_type;

  ret = FALSE;
  if ((g_strcmp0 (udisks_block_get_id_usage (block_object->iface_block_device), "other") == 0 &&
       g_strcmp0 (udisks_block_get_id_type (block_object->iface_block_device), "swap") == 0)
      || (udisks_mount_monitor_is_dev_in_use (block_object->mount_monitor,
                                              g_udev_device_get_device_number (block_object->device->udev_device),
                                              &mount_type)
          && mount_type == UDISKS_MOUNT_TYPE_SWAP))
    ret = TRUE;

  return ret;
}

static void
swapspace_connect (UDisksObject *object)
{
}

static gboolean
swapspace_update (UDisksObject   *object,
                  const gchar    *uevent_action,
                  GDBusInterface *_iface)
{
  udisks_linux_swapspace_update (UDISKS_LINUX_SWAPSPACE (_iface), UDISKS_LINUX_BLOCK_OBJECT (object));
  return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
encrypted_check (UDisksObject *object)
{
  UDisksBlock *block = udisks_object_peek_block (object);

  return udisks_linux_block_is_luks (block) ||
         udisks_linux_block_is_tcrypt (block) ||
         udisks_linux_block_is_bitlk (block) ||
         udisks_linux_block_is_unknown_crypto (block);
}

static void
encrypted_connect (UDisksObject *object)
{
}

static gboolean
encrypted_update (UDisksObject   *object,
                  const gchar    *uevent_action,
                  GDBusInterface *_iface)
{
  udisks_linux_encrypted_update (UDISKS_LINUX_ENCRYPTED (_iface), UDISKS_LINUX_BLOCK_OBJECT (object));
  return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
loop_check (UDisksObject *object)
{
  UDisksLinuxBlockObject *block_object = UDISKS_LINUX_BLOCK_OBJECT (object);
  gboolean ret;

  ret = FALSE;
  if (g_str_has_prefix (g_udev_device_get_name (block_object->device->udev_device), "loop") &&
      g_strcmp0 (g_udev_device_get_devtype (block_object->device->udev_device), "disk") == 0)
    ret = TRUE;

  return ret;
}

static void
loop_connect (UDisksObject *object)
{
}

static gboolean
loop_update (UDisksObject   *object,
             const gchar    *uevent_action,
             GDBusInterface *_iface)
{
  udisks_linux_loop_update (UDISKS_LINUX_LOOP (_iface), UDISKS_LINUX_BLOCK_OBJECT (object));
  return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_linux_block_object_uevent:
 * @object: A #UDisksLinuxBlockObject.
 * @action: Uevent action or %NULL
 * @device: A new #UDisksLinuxDevice device object or %NULL if the device hasn't changed.
 *
 * Updates all information on interfaces on @object as a result of incoming uevent processing.
 */
void
udisks_linux_block_object_uevent (UDisksLinuxBlockObject *object,
                                  const gchar            *action,
                                  UDisksLinuxDevice      *device)
{
  UDisksModuleManager *module_manager;
  GList *modules;
  GList *l;

  g_return_if_fail (UDISKS_IS_LINUX_BLOCK_OBJECT (object));
  g_return_if_fail (device == NULL || UDISKS_IS_LINUX_DEVICE (device));

  if (device != NULL)
    {
      g_mutex_lock (&object->device_mutex);
      g_object_unref (object->device);
      object->device = g_object_ref (device);
      g_mutex_unlock (&object->device_mutex);
      g_object_notify (G_OBJECT (object), "device");
    }

  update_iface (UDISKS_OBJECT (object), action, block_device_check, block_device_connect, block_device_update,
                UDISKS_TYPE_LINUX_BLOCK, &object->iface_block_device);
  g_warn_if_fail (object->iface_block_device != NULL);
  update_iface (UDISKS_OBJECT (object), action, contains_filesystem, filesystem_connect, filesystem_update,
                UDISKS_TYPE_LINUX_FILESYSTEM, &object->iface_filesystem);
  update_iface (UDISKS_OBJECT (object), action, swapspace_check, swapspace_connect, swapspace_update,
                UDISKS_TYPE_LINUX_SWAPSPACE, &object->iface_swapspace);
  update_iface (UDISKS_OBJECT (object), action, encrypted_check, encrypted_connect, encrypted_update,
                UDISKS_TYPE_LINUX_ENCRYPTED, &object->iface_encrypted);
  update_iface (UDISKS_OBJECT (object), action, loop_check, loop_connect, loop_update,
                UDISKS_TYPE_LINUX_LOOP, &object->iface_loop);
  update_iface (UDISKS_OBJECT (object), action, partition_table_check, partition_table_connect, partition_table_update,
                UDISKS_TYPE_LINUX_PARTITION_TABLE, &object->iface_partition_table);
  update_iface (UDISKS_OBJECT (object), action, partition_check, partition_connect, partition_update,
                UDISKS_TYPE_LINUX_PARTITION, &object->iface_partition);

  /* Attach interfaces from modules */
  module_manager = udisks_daemon_get_module_manager (object->daemon);
  modules = udisks_module_manager_get_modules (module_manager);
  for (l = modules; l; l = g_list_next (l))
    {
      UDisksModule *module = l->data;
      GType *types;

      types = udisks_module_get_block_object_interface_types (module);
      for (; types && *types; types++)
        {
          GDBusInterfaceSkeleton *interface;
          gboolean keep = TRUE;

          interface = g_hash_table_lookup (object->module_ifaces, GSIZE_TO_POINTER (*types));
          if (interface != NULL)
            {
              /* ask the existing instance to process the uevent */
              if (udisks_module_object_process_uevent (UDISKS_MODULE_OBJECT (interface),
                                                       action, object->device, &keep))
                {
                  if (! keep)
                    {
                      g_dbus_object_skeleton_remove_interface (G_DBUS_OBJECT_SKELETON (object), interface);
                      g_hash_table_remove (object->module_ifaces, GSIZE_TO_POINTER (*types));
                    }
                }
            }
          else
            {
              /* try create new interface and see if the module is interested in this object */
              interface = udisks_module_new_block_object_interface (module, object, *types);
              if (interface)
                {
                  /* do coldplug after creation */
                  udisks_module_object_process_uevent (UDISKS_MODULE_OBJECT (interface), action, object->device, &keep);
                  g_dbus_object_skeleton_add_interface (G_DBUS_OBJECT_SKELETON (object), interface);
                  g_warn_if_fail (g_hash_table_replace (object->module_ifaces, GSIZE_TO_POINTER (*types), interface));
                }
            }
        }
    }
  g_list_free_full (modules, g_object_unref);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
on_mount_monitor_mount_added_removed (UDisksMountMonitor  *monitor,
                                      UDisksMount         *mount,
                                      gpointer             user_data)
{
  UDisksLinuxBlockObject *object;
  UDisksFilesystem *fs;
  gboolean related;

  if (udisks_mount_get_mount_type (mount) != UDISKS_MOUNT_TYPE_FILESYSTEM)
    return;

  object = UDISKS_LINUX_BLOCK_OBJECT (g_object_ref (user_data));

  related = udisks_mount_get_dev (mount) == g_udev_device_get_device_number (object->device->udev_device);
  if (!related && udisks_mount_get_fs_uuid (mount) != NULL)
    {
      fs = udisks_object_peek_filesystem (UDISKS_OBJECT (object));
      if (fs != NULL)
        {
          /* Assume identification by filesystem UUID */
          if (udisks_linux_filesystem_is_volume_based (UDISKS_LINUX_FILESYSTEM (fs), object))
            related = g_strcmp0 (g_udev_device_get_property (object->device->udev_device, "ID_FS_UUID"), udisks_mount_get_fs_uuid (mount)) == 0;
        }
    }

  if (related)
    udisks_linux_block_object_uevent (object, NULL, NULL);

  g_object_unref (object);
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_linux_block_object_trigger_uevent:
 * @object: A #UDisksLinuxBlockObject.
 *
 * Triggers a 'change' uevent in the kernel.
 *
 * Refer to udisks_daemon_util_trigger_uevent() for detailed description.
 */
void
udisks_linux_block_object_trigger_uevent (UDisksLinuxBlockObject *object)
{
  UDisksLinuxDevice *device;
  const gchar *sysfs_path;

  g_return_if_fail (UDISKS_IS_LINUX_BLOCK_OBJECT (object));

  device = udisks_linux_block_object_get_device (object);
  sysfs_path = g_udev_device_get_sysfs_path (device->udev_device);
  udisks_daemon_util_trigger_uevent (object->daemon, NULL, sysfs_path);
  g_object_unref (device);
}

/**
 * udisks_linux_block_object_trigger_uevent_sync:
 * @object: A #UDisksLinuxBlockObject.
 * @timeout_seconds: Maximum time to wait for the uevent (in seconds).
 *
 * Triggers a 'change' uevent in the kernel and waits until it's received and
 * processed through the uevent queue.
 *
 * This is a convenient wrapper around udisks_daemon_util_trigger_uevent_sync().
 * Refer to this function for detailed documentation.
 *
 * Returns: %TRUE if the uevent has been successfully received, %FALSE otherwise
 * or when the kernel version is too old.
 */
gboolean
udisks_linux_block_object_trigger_uevent_sync (UDisksLinuxBlockObject *object,
                                               guint                   timeout_seconds)
{
  UDisksLinuxDevice *device;
  const gchar *sysfs_path;
  gboolean ret;

  g_return_val_if_fail (UDISKS_IS_LINUX_BLOCK_OBJECT (object), FALSE);

  device = udisks_linux_block_object_get_device (object);
  sysfs_path = g_udev_device_get_sysfs_path (device->udev_device);
  ret = udisks_daemon_util_trigger_uevent_sync (object->daemon, NULL, sysfs_path, timeout_seconds);
  g_object_unref (device);

  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_linux_block_object_reread_partition_table:
 * @object: A #UDisksLinuxBlockObject.
 * @error: Return location for error.
 *
 * Requests the kernel to re-read the partition table for @object.
 *
 * The events from any change this may cause will bubble up from the
 * kernel through the udev stack and will eventually be received by
 * the udisks daemon process itself. This method does not wait for the
 * event to be received.
 *
 * Returns: %TRUE if the partition reread request was issued successfully
 * or %FALSE when error occurred with @error set.
 */
gboolean
udisks_linux_block_object_reread_partition_table (UDisksLinuxBlockObject  *object,
                                                  GError                 **error)
{
  UDisksLinuxDevice *device;
  const gchar *device_file;
  gint fd;
  gboolean ret = TRUE;

  g_return_val_if_fail (UDISKS_IS_LINUX_BLOCK_OBJECT (object), FALSE);
  g_warn_if_fail (!error || !*error);

  device = udisks_linux_block_object_get_device (object);
  device_file = g_udev_device_get_device_file (device->udev_device);
  fd = open (device_file, O_RDONLY);
  if (fd == -1)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                   "Error opening %s while re-reading partition table: %m", device_file);
      ret = FALSE;
    }
  else
    {
      gint num_tries = 0;

      /* acquire an exclusive BSD lock to prevent udev probes.
       * See also https://systemd.io/BLOCK_DEVICE_LOCKING
       */
      while (flock (fd, LOCK_EX | LOCK_NB) != 0)
        {
          g_usleep (100 * 1000); /* microseconds */
          if (num_tries++ > 5)
            break;
        }

      if (ioctl (fd, BLKRRPART) != 0)
        {
          g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                       "Error re-reading partition table (BLKRRPART ioctl) on %s: %m", device_file);
          ret = FALSE;
        }
      close (fd);
    }

  g_object_unref (device);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_linux_block_object_try_lock_for_cleanup:
 * @object: A #UDisksLinuxBlockObject.
 *
 * Attempts to lock the block device @object for cleanup. Typically used by the cleanup
 * thread to check whether the block device is busy and cleanup can be performed.
 *
 * Returns: %TRUE if lock has been acquired, %FALSE when the block device is busy.
 */
gboolean
udisks_linux_block_object_try_lock_for_cleanup (UDisksLinuxBlockObject *object)
{
  g_return_val_if_fail (UDISKS_IS_LINUX_BLOCK_OBJECT (object), FALSE);

  return g_mutex_trylock (&object->cleanup_mutex);
}

/**
 * udisks_linux_block_object_lock_for_cleanup:
 * @object: A #UDisksLinuxBlockObject.
 *
 * Acquires the "cleanup" lock of the block device @object. Typically used to signal
 * the cleanup thread that the block device is busy.
 */
void
udisks_linux_block_object_lock_for_cleanup (UDisksLinuxBlockObject *object)
{
  g_return_if_fail (UDISKS_IS_LINUX_BLOCK_OBJECT (object));

  g_mutex_lock (&object->cleanup_mutex);
}

/**
 * udisks_linux_block_object_release_cleanup_lock:
 * @object: A #UDisksLinuxBlockObject.
 *
 * Releases a lock previously acquired by udisks_linux_block_object_lock_for_cleanup().
 */
void
udisks_linux_block_object_release_cleanup_lock (UDisksLinuxBlockObject *object)
{
  g_return_if_fail (UDISKS_IS_LINUX_BLOCK_OBJECT (object));

  g_mutex_unlock (&object->cleanup_mutex);
}
