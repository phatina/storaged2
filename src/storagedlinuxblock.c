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

#define _GNU_SOURCE /* for O_DIRECT */

#include "config.h"
#include <glib/gi18n-lib.h>

#include <sys/types.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pwd.h>
#include <grp.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <mntent.h>

#include <glib/gstdio.h>
#include <gio/gunixfdlist.h>

#ifdef HAVE_LIBBLOCKDEV_PART
#include <blockdev/part.h>
#endif /* HAVE_LIBBLOCKDEV_PART */

#include "storagedlogging.h"
#include "storagedlinuxblock.h"
#include "storagedlinuxblockobject.h"
#include "storagedlinuxdriveobject.h"
#include "storagedlinuxfsinfo.h"
#include "storageddaemon.h"
#include "storagedstate.h"
#include "storageddaemonutil.h"
#include "storagedlinuxprovider.h"
#include "storagedfstabmonitor.h"
#include "storagedfstabentry.h"
#include "storagedcrypttabmonitor.h"
#include "storagedcrypttabentry.h"
#include "storageddaemonutil.h"
#include "storagedbasejob.h"
#include "storagedsimplejob.h"
#include "storagedlinuxdriveata.h"
#include "storagedlinuxmdraidobject.h"
#include "storagedlinuxdevice.h"
#include "storagedlinuxpartition.h"
#include "storagedlinuxencrypted.h"

/**
 * SECTION:storagedlinuxblock
 * @title: StoragedLinuxBlock
 * @short_description: Linux implementation of #StoragedBlock
 *
 * This type provides an implementation of the #StoragedBlock
 * interface on Linux.
 */

typedef struct _StoragedLinuxBlockClass   StoragedLinuxBlockClass;

/**
 * StoragedLinuxBlock:
 *
 * The #StoragedLinuxBlock structure contains only private data and should
 * only be accessed using the provided API.
 */
struct _StoragedLinuxBlock
{
  StoragedBlockSkeleton parent_instance;
};

struct _StoragedLinuxBlockClass
{
  StoragedBlockSkeletonClass parent_class;
};

static void block_iface_init (StoragedBlockIface *iface);

G_DEFINE_TYPE_WITH_CODE (StoragedLinuxBlock, storaged_linux_block, STORAGED_TYPE_BLOCK_SKELETON,
                         G_IMPLEMENT_INTERFACE (STORAGED_TYPE_BLOCK, block_iface_init));

/* ---------------------------------------------------------------------------------------------------- */

static void
storaged_linux_block_init (StoragedLinuxBlock *block)
{
  g_dbus_interface_skeleton_set_flags (G_DBUS_INTERFACE_SKELETON (block),
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);
}

static void
storaged_linux_block_class_init (StoragedLinuxBlockClass *klass)
{
}

/**
 * storaged_linux_block_new:
 *
 * Creates a new #StoragedLinuxBlock instance.
 *
 * Returns: A new #StoragedLinuxBlock. Free with g_object_unref().
 */
StoragedBlock *
storaged_linux_block_new (void)
{
  return STORAGED_BLOCK (g_object_new (STORAGED_TYPE_LINUX_BLOCK,
                                     NULL));
}

/* ---------------------------------------------------------------------------------------------------- */

static gchar *
get_sysfs_attr (GUdevDevice *device,
                const gchar *attr)
{
  gchar *filename;
  gchar *value;
  filename = g_strconcat (g_udev_device_get_sysfs_path (device),
                          "/",
                          attr,
                          NULL);
  value = NULL;
  /* don't care about errors */
  g_file_get_contents (filename,
                       &value,
                       NULL,
                       NULL);
  g_free (filename);
  return value;
}

/* ---------------------------------------------------------------------------------------------------- */

static gchar *
find_block_device_by_sysfs_path (GDBusObjectManagerServer *object_manager,
                                 const gchar              *sysfs_path)
{
  gchar *ret;
  GList *objects;
  GList *l;

  ret = NULL;

  objects = g_dbus_object_manager_get_objects (G_DBUS_OBJECT_MANAGER (object_manager));
  for (l = objects; l != NULL; l = l->next)
    {
      GDBusObjectSkeleton *object = G_DBUS_OBJECT_SKELETON (l->data);
      StoragedLinuxDevice *device;

      if (!STORAGED_IS_LINUX_BLOCK_OBJECT (object))
        continue;

      device = storaged_linux_block_object_get_device (STORAGED_LINUX_BLOCK_OBJECT (object));
      if (g_strcmp0 (sysfs_path, g_udev_device_get_sysfs_path (device->udev_device)) == 0)
        {
          ret = g_strdup (g_dbus_object_get_object_path (G_DBUS_OBJECT (object)));
          g_object_unref (device);
          goto out;
        }
      g_object_unref (device);
    }

 out:
  g_list_foreach (objects, (GFunc) g_object_unref, NULL);
  g_list_free (objects);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static gchar *
find_drive (GDBusObjectManagerServer  *object_manager,
            GUdevDevice               *block_device,
            StoragedDrive              **out_drive)
{
  GUdevDevice *whole_disk_block_device;
  const gchar *whole_disk_block_device_sysfs_path;
  gchar *ret;
  GList *objects;
  GList *l;

  ret = NULL;

  if (g_strcmp0 (g_udev_device_get_devtype (block_device), "disk") == 0)
    whole_disk_block_device = g_object_ref (block_device);
  else
    whole_disk_block_device = g_udev_device_get_parent_with_subsystem (block_device, "block", "disk");
  whole_disk_block_device_sysfs_path = g_udev_device_get_sysfs_path (whole_disk_block_device);

  objects = g_dbus_object_manager_get_objects (G_DBUS_OBJECT_MANAGER (object_manager));
  for (l = objects; l != NULL; l = l->next)
    {
      GDBusObjectSkeleton *object = G_DBUS_OBJECT_SKELETON (l->data);
      GList *drive_devices;
      GList *j;

      if (!STORAGED_IS_LINUX_DRIVE_OBJECT (object))
        continue;

      drive_devices = storaged_linux_drive_object_get_devices (STORAGED_LINUX_DRIVE_OBJECT (object));
      for (j = drive_devices; j != NULL; j = j->next)
        {
          StoragedLinuxDevice *drive_device = STORAGED_LINUX_DEVICE (j->data);
          const gchar *drive_sysfs_path;

          drive_sysfs_path = g_udev_device_get_sysfs_path (drive_device->udev_device);
          if (g_strcmp0 (whole_disk_block_device_sysfs_path, drive_sysfs_path) == 0)
            {
              if (out_drive != NULL)
                *out_drive = storaged_object_get_drive (STORAGED_OBJECT (object));
              ret = g_strdup (g_dbus_object_get_object_path (G_DBUS_OBJECT (object)));
              g_list_foreach (drive_devices, (GFunc) g_object_unref, NULL);
              g_list_free (drive_devices);
              goto out;
            }
        }
      g_list_foreach (drive_devices, (GFunc) g_object_unref, NULL);
      g_list_free (drive_devices);
    }

 out:
  g_list_foreach (objects, (GFunc) g_object_unref, NULL);
  g_list_free (objects);
  g_object_unref (whole_disk_block_device);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static StoragedLinuxMDRaidObject *
find_mdraid (GDBusObjectManagerServer  *object_manager,
             const gchar               *md_uuid)
{
  StoragedLinuxMDRaidObject *ret = NULL;
  GList *objects = NULL, *l;

  objects = g_dbus_object_manager_get_objects (G_DBUS_OBJECT_MANAGER (object_manager));
  for (l = objects; l != NULL; l = l->next)
    {
      GDBusObjectSkeleton *object = G_DBUS_OBJECT_SKELETON (l->data);
      if (STORAGED_IS_LINUX_MDRAID_OBJECT (object))
        {
          StoragedMDRaid *mdraid = storaged_object_get_mdraid (STORAGED_OBJECT (object));
          if (mdraid != NULL)
            {
              if (g_strcmp0 (storaged_mdraid_get_uuid (mdraid), md_uuid) == 0)
                {
                  ret = g_object_ref (object);
                  g_object_unref (mdraid);
                  goto out;
                }
              g_object_unref (mdraid);
            }
        }
    }

 out:
  g_list_foreach (objects, (GFunc) g_object_unref, NULL);
  g_list_free (objects);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
update_mdraid (StoragedLinuxBlock         *block,
               StoragedLinuxDevice        *device,
               StoragedDrive              *drive,
               GDBusObjectManagerServer *object_manager)
{
  StoragedBlock *iface = STORAGED_BLOCK (block);
  const gchar *uuid;
  const gchar *objpath_mdraid = "/";
  const gchar *objpath_mdraid_member = "/";
  StoragedLinuxMDRaidObject *object = NULL;

  uuid = g_udev_device_get_property (device->udev_device, "STORAGED_MD_UUID");
  if (uuid != NULL && strlen (uuid) > 0)
    {
      object = find_mdraid (object_manager, uuid);
      if (object != NULL)
        {
          objpath_mdraid = g_dbus_object_get_object_path (G_DBUS_OBJECT (object));
          g_clear_object (&object);
        }
    }

  uuid = g_udev_device_get_property (device->udev_device, "STORAGED_MD_MEMBER_UUID");
  if (uuid != NULL && strlen (uuid) > 0)
    {
      object = find_mdraid (object_manager, uuid);
      if (object != NULL)
        {
          objpath_mdraid_member = g_dbus_object_get_object_path (G_DBUS_OBJECT (object));
          g_clear_object (&object);
        }
    }

  storaged_block_set_mdraid (iface, objpath_mdraid);
  storaged_block_set_mdraid_member (iface, objpath_mdraid_member);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
update_hints (StoragedLinuxBlock  *block,
              StoragedLinuxDevice *device,
              StoragedDrive       *drive)
{
  StoragedBlock *iface = STORAGED_BLOCK (block);
  gboolean hint_partitionable;
  gboolean hint_system;
  gboolean hint_ignore;
  gboolean hint_auto;
  const gchar *hint_name;
  const gchar *hint_icon_name;
  const gchar *hint_symbolic_icon_name;
  const gchar *device_file;

  /* very conservative defaults */
  hint_partitionable = TRUE;
  hint_system = TRUE;
  hint_ignore = FALSE;
  hint_auto = FALSE;
  hint_name = NULL;
  hint_icon_name = NULL;
  hint_symbolic_icon_name = NULL;

  device_file = g_udev_device_get_device_file (device->udev_device);

  /* Provide easy access to _only_ the following devices
   *
   *  - anything connected via known local buses (e.g. USB or Firewire, MMC or MemoryStick)
   *  - any device with removable media
   *
   * Be careful when extending this list as we don't want to automount
   * the world when (inadvertently) connecting to a SAN.
   */
  if (drive != NULL)
    {
      const gchar *connection_bus;
      gboolean removable;
      connection_bus = storaged_drive_get_connection_bus (drive);
      removable = storaged_drive_get_media_removable (drive);
      if (removable ||
          (g_strcmp0 (connection_bus, "usb") == 0 || g_strcmp0 (connection_bus, "ieee1394") == 0) ||
          (g_str_has_prefix (device_file, "/dev/mmcblk") || g_str_has_prefix (device_file, "/dev/mspblk")))
        {
          hint_system = FALSE;
          hint_auto = TRUE;
        }
    }

  /* Floppy drives are not partitionable and should never be auto-mounted */
  if (g_str_has_prefix (device_file, "/dev/fd"))
    {
      hint_system = FALSE;
      hint_partitionable = FALSE;
      hint_auto = FALSE;
    }

  /* CD-ROM media / drives are not partitionable, at least not here on Linux */
  if (g_udev_device_get_property_as_boolean (device->udev_device, "ID_CDROM"))
    hint_partitionable = FALSE;

  /* device-mapper devices are not partitionable (TODO: for multipath, they are via kpartx(8) hacks) */
  if (g_str_has_prefix (g_udev_device_get_name (device->udev_device), "dm-"))
    hint_partitionable = FALSE;

  /* TODO: set ignore to TRUE for physical paths belonging to a drive with multiple paths */

  /* Override from udev properties, first from UDISKS_* and than from
     STORAGED_*. We assume that as long as the UDISKS_* properties
     exist, they are more correct than the STORAGED_* properties.
  */

  if (g_udev_device_has_property (device->udev_device, "UDISKS_SYSTEM"))
    hint_system = g_udev_device_get_property_as_boolean (device->udev_device, "UDISKS_SYSTEM");
  else if (g_udev_device_has_property (device->udev_device, "STORAGED_SYSTEM"))
    hint_system = g_udev_device_get_property_as_boolean (device->udev_device, "STORAGED_SYSTEM");

  if (g_udev_device_has_property (device->udev_device, "UDISKS_IGNORE"))
    hint_ignore = g_udev_device_get_property_as_boolean (device->udev_device, "UDISKS_IGNORE");
  else if (g_udev_device_has_property (device->udev_device, "STORAGED_IGNORE"))
    hint_ignore = g_udev_device_get_property_as_boolean (device->udev_device, "STORAGED_IGNORE");

  if (g_udev_device_has_property (device->udev_device, "UDISKS_AUTO"))
    hint_auto = g_udev_device_get_property_as_boolean (device->udev_device, "UDISKS_AUTO");
  else if (g_udev_device_has_property (device->udev_device, "STORAGED_AUTO"))
    hint_auto = g_udev_device_get_property_as_boolean (device->udev_device, "STORAGED_AUTO");

  if (g_udev_device_has_property (device->udev_device, "UDISKS_NAME"))
    hint_name = g_udev_device_get_property (device->udev_device, "UDISKS_NAME");
  else if (g_udev_device_has_property (device->udev_device, "STORAGED_NAME"))
    hint_name = g_udev_device_get_property (device->udev_device, "STORAGED_NAME");

  if (g_udev_device_has_property (device->udev_device, "UDISKS_ICON_NAME"))
    hint_icon_name = g_udev_device_get_property (device->udev_device, "UDISKS_ICON_NAME");
  else if (g_udev_device_has_property (device->udev_device, "STORAGED_ICON_NAME"))
    hint_icon_name = g_udev_device_get_property (device->udev_device, "STORAGED_ICON_NAME");

  if (g_udev_device_has_property (device->udev_device, "UDISKS_SYMBOLIC_ICON_NAME"))
    hint_symbolic_icon_name = g_udev_device_get_property (device->udev_device, "UDISKS_SYMBOLIC_ICON_NAME");
  else if (g_udev_device_has_property (device->udev_device, "STORAGED_SYMBOLIC_ICON_NAME"))
    hint_symbolic_icon_name = g_udev_device_get_property (device->udev_device, "STORAGED_SYMBOLIC_ICON_NAME");

  /* ... and scene! */
  storaged_block_set_hint_partitionable (iface, hint_partitionable);
  storaged_block_set_hint_system (iface, hint_system);
  storaged_block_set_hint_ignore (iface, hint_ignore);
  storaged_block_set_hint_auto (iface, hint_auto);
  storaged_block_set_hint_name (iface, hint_name);
  storaged_block_set_hint_icon_name (iface, hint_icon_name);
  storaged_block_set_hint_symbolic_icon_name (iface, hint_symbolic_icon_name);
}

/* ---------------------------------------------------------------------------------------------------- */

static GList *
find_fstab_entries_for_device (StoragedLinuxBlock *block,
                               StoragedDaemon     *daemon)
{
  GList *entries;
  GList *l;
  GList *ret;

  ret = NULL;

  /* if this is too slow, we could add lookup methods to StoragedFstabMonitor... */
  entries = storaged_fstab_monitor_get_entries (storaged_daemon_get_fstab_monitor (daemon));
  for (l = entries; l != NULL; l = l->next)
    {
      StoragedFstabEntry *entry = STORAGED_FSTAB_ENTRY (l->data);
      const gchar *const *symlinks;
      const gchar *fsname;
      const gchar *device = NULL;
      const gchar *label = NULL;
      const gchar *uuid = NULL;
      const gchar *partuuid = NULL;
      const gchar *partlabel = NULL;
      guint n;

      fsname = storaged_fstab_entry_get_fsname (entry);
      device = NULL;
      if (g_str_has_prefix (fsname, "UUID="))
        {
          uuid = fsname + 5;
        }
      else if (g_str_has_prefix (fsname, "LABEL="))
        {
          label = fsname + 6;
        }
      else if (g_str_has_prefix (fsname, "PARTUUID="))
        {
          partuuid = fsname + 9;
        }
      else if (g_str_has_prefix (fsname, "PARTLABEL="))
        {
          partlabel = fsname + 10;
        }
      else if (g_str_has_prefix (fsname, "/dev"))
        {
          device = fsname;
        }
      else
        {
          /* ignore non-device entries */
          goto continue_loop;
        }

      if (device != NULL)
        {
          if (g_strcmp0 (device, storaged_block_get_device (STORAGED_BLOCK (block))) == 0)
            {
              ret = g_list_prepend (ret, g_object_ref (entry));
            }
          else
            {
              symlinks = storaged_block_get_symlinks (STORAGED_BLOCK (block));
              if (symlinks != NULL)
                {
                  for (n = 0; symlinks[n] != NULL; n++)
                    {
                      if (g_strcmp0 (device, symlinks[n]) == 0)
                        {
                          ret = g_list_prepend (ret, g_object_ref (entry));
                        }
                    }
                }
            }
        }
      else if (label != NULL && g_strcmp0 (label, storaged_block_get_id_label (STORAGED_BLOCK (block))) == 0)
        {
          ret = g_list_prepend (ret, g_object_ref (entry));
        }
      else if (uuid != NULL && g_strcmp0 (uuid, storaged_block_get_id_uuid (STORAGED_BLOCK (block))) == 0)
        {
          ret = g_list_prepend (ret, g_object_ref (entry));
        }
      else if (partlabel != NULL || partuuid != NULL)
        {
          StoragedLinuxBlockObject *object;
          GUdevDevice *u_dev = NULL;

          object = storaged_daemon_util_dup_object (block, NULL);
          if (object == NULL)
            goto continue_loop;
          u_dev = storaged_linux_block_object_get_device (object)->udev_device;
          g_clear_object (&object);
          if (u_dev == NULL)
            goto continue_loop;
          if ((partuuid != NULL && g_strcmp0 (partuuid, g_udev_device_get_property (u_dev, "ID_PART_ENTRY_UUID")) == 0) ||
              (partlabel != NULL && g_strcmp0 (partlabel, g_udev_device_get_property (u_dev, "ID_PART_ENTRY_NAME")) == 0))
            ret = g_list_prepend (ret, g_object_ref (entry));
        }

    continue_loop:
      ;
    }

  g_list_foreach (entries, (GFunc) g_object_unref, NULL);
  g_list_free (entries);
  return ret;
}

static GList *
find_crypttab_entries_for_device (StoragedLinuxBlock *block,
                                  StoragedDaemon     *daemon)
{
  GList *entries;
  GList *l;
  GList *ret;

  ret = NULL;

  /* if this is too slow, we could add lookup methods to StoragedCrypttabMonitor... */
  entries = storaged_crypttab_monitor_get_entries (storaged_daemon_get_crypttab_monitor (daemon));
  for (l = entries; l != NULL; l = l->next)
    {
      StoragedCrypttabEntry *entry = STORAGED_CRYPTTAB_ENTRY (l->data);
      const gchar *const *symlinks;
      const gchar *device_in_entry;
      const gchar *device = NULL;
      const gchar *label = NULL;
      const gchar *uuid = NULL;
      guint n;

      device_in_entry = storaged_crypttab_entry_get_device (entry);
      if (g_str_has_prefix (device_in_entry, "UUID="))
        {
          uuid = device_in_entry + 5;
        }
      else if (g_str_has_prefix (device_in_entry, "LABEL="))
        {
          label = device_in_entry + 6;
        }
      else if (g_str_has_prefix (device_in_entry, "/dev"))
        {
          device = device_in_entry;
        }
      else
        {
          /* ignore non-device entries */
          goto continue_loop;
        }

      if (device != NULL)
        {
          if (g_strcmp0 (device, storaged_block_get_device (STORAGED_BLOCK (block))) == 0)
            {
              ret = g_list_prepend (ret, g_object_ref (entry));
            }
          else
            {
              symlinks = storaged_block_get_symlinks (STORAGED_BLOCK (block));
              if (symlinks != NULL)
                {
                  for (n = 0; symlinks[n] != NULL; n++)
                    {
                      if (g_strcmp0 (device, symlinks[n]) == 0)
                        {
                          ret = g_list_prepend (ret, g_object_ref (entry));
                        }
                    }
                }
            }
        }
      else if (label != NULL && g_strcmp0 (label, storaged_block_get_id_label (STORAGED_BLOCK (block))) == 0)
        {
          ret = g_list_prepend (ret, g_object_ref (entry));
        }
      else if (uuid != NULL && g_strcmp0 (uuid, storaged_block_get_id_uuid (STORAGED_BLOCK (block))) == 0)
        {
          ret = g_list_prepend (ret, g_object_ref (entry));
        }

    continue_loop:
      ;
    }

  g_list_foreach (entries, (GFunc) g_object_unref, NULL);
  g_list_free (entries);
  return ret;
}

static void
add_fstab_entry (GVariantBuilder    *builder,
                 StoragedFstabEntry *entry)
{
  GVariantBuilder dict_builder;
  g_variant_builder_init (&dict_builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&dict_builder, "{sv}", "fsname",
                         g_variant_new_bytestring (storaged_fstab_entry_get_fsname (entry)));
  g_variant_builder_add (&dict_builder, "{sv}", "dir",
                         g_variant_new_bytestring (storaged_fstab_entry_get_dir (entry)));
  g_variant_builder_add (&dict_builder, "{sv}", "type",
                         g_variant_new_bytestring (storaged_fstab_entry_get_fstype (entry)));
  g_variant_builder_add (&dict_builder, "{sv}", "opts",
                         g_variant_new_bytestring (storaged_fstab_entry_get_opts (entry)));
  g_variant_builder_add (&dict_builder, "{sv}", "freq",
                         g_variant_new_int32 (storaged_fstab_entry_get_freq (entry)));
  g_variant_builder_add (&dict_builder, "{sv}", "passno",
                         g_variant_new_int32 (storaged_fstab_entry_get_passno (entry)));
  g_variant_builder_add (builder,
                         "(sa{sv})",
                         "fstab", &dict_builder);
}

static gboolean
add_crypttab_entry (GVariantBuilder       *builder,
                    StoragedCrypttabEntry *entry,
                    gboolean               include_secrets,
                    GError               **error)
{
  GVariantBuilder dict_builder;
  const gchar *passphrase_path;
  const gchar *options;
  gchar *passphrase_contents;
  gsize passphrase_contents_length;

  passphrase_path = storaged_crypttab_entry_get_passphrase_path (entry);
  if (passphrase_path == NULL || g_strcmp0 (passphrase_path, "none") == 0)
    passphrase_path = "";
  passphrase_contents = NULL;
  if (!(g_strcmp0 (passphrase_path, "") == 0 || g_str_has_prefix (passphrase_path, "/dev")))
    {
      if (include_secrets)
        {
          if (!g_file_get_contents (passphrase_path,
                                    &passphrase_contents,
                                    &passphrase_contents_length,
                                    error))
            {
              g_prefix_error (error,
                              "Error loading secrets from file `%s' referenced in /etc/crypttab entry: ",
                              passphrase_path);
              return FALSE;
            }
        }
    }

  options = storaged_crypttab_entry_get_options (entry);
  if (options == NULL)
    options = "";

  g_variant_builder_init (&dict_builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&dict_builder, "{sv}", "name",
                         g_variant_new_bytestring (storaged_crypttab_entry_get_name (entry)));
  g_variant_builder_add (&dict_builder, "{sv}", "device",
                         g_variant_new_bytestring (storaged_crypttab_entry_get_device (entry)));
  g_variant_builder_add (&dict_builder, "{sv}", "passphrase-path",
                         g_variant_new_bytestring (passphrase_path));
  if (passphrase_contents != NULL)
    {
      g_variant_builder_add (&dict_builder, "{sv}", "passphrase-contents",
                             g_variant_new_bytestring (passphrase_contents));
    }
  g_variant_builder_add (&dict_builder, "{sv}", "options",
                         g_variant_new_bytestring (options));
  g_variant_builder_add (builder,
                         "(sa{sv})",
                         "crypttab", &dict_builder);
  if (passphrase_contents != NULL)
    {
      memset (passphrase_contents, '\0', passphrase_contents_length);
      g_free (passphrase_contents);
    }

  return TRUE;
}

/* returns a floating GVariant */
static GVariant *
calculate_configuration (StoragedLinuxBlock  *block,
                         StoragedDaemon      *daemon,
                         gboolean             include_secrets,
                         GError             **error)
{
  GList *entries;
  GList *l;
  GVariantBuilder builder;
  GVariant *ret;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  ret = NULL;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(sa{sv})"));
  /* First the /etc/fstab entries */
  entries = find_fstab_entries_for_device (block, daemon);
  for (l = entries; l != NULL; l = l->next)
    add_fstab_entry (&builder, STORAGED_FSTAB_ENTRY (l->data));
  g_list_foreach (entries, (GFunc) g_object_unref, NULL);
  g_list_free (entries);

  /* Then the /etc/crypttab entries */
  entries = find_crypttab_entries_for_device (block, daemon);
  for (l = entries; l != NULL; l = l->next)
    {
      if (!add_crypttab_entry (&builder, STORAGED_CRYPTTAB_ENTRY (l->data), include_secrets, error))
        {
          g_variant_builder_clear (&builder);
          g_list_foreach (entries, (GFunc) g_object_unref, NULL);
          g_list_free (entries);
          goto out;
        }
    }
  g_list_foreach (entries, (GFunc) g_object_unref, NULL);
  g_list_free (entries);

  ret = g_variant_builder_end (&builder);

 out:
  return ret;
}

static void
update_configuration (StoragedLinuxBlock  *block,
                      StoragedDaemon      *daemon)
{
  GVariant *configuration;
  GError *error;

  error = NULL;
  configuration = calculate_configuration (block, daemon, FALSE, &error);
  if (configuration == NULL)
    {
      storaged_warning ("Error loading configuration: %s (%s, %d)",
                        error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
      configuration = g_variant_new ("a(sa{sv})", NULL);
    }
  storaged_block_set_configuration (STORAGED_BLOCK (block), configuration);
}

/* ---------------------------------------------------------------------------------------------------- */

static GList *
find_fstab_entries_for_needle (const gchar    *needle,
                               StoragedDaemon *daemon)
{
  GList *entries;
  GList *l;
  GList *ret;

  ret = NULL;

  entries = storaged_fstab_monitor_get_entries (storaged_daemon_get_fstab_monitor (daemon));
  for (l = entries; l != NULL; l = l->next)
    {
      StoragedFstabEntry *entry = STORAGED_FSTAB_ENTRY (l->data);
      const gchar *opts = NULL;

      opts = storaged_fstab_entry_get_opts (entry);
      if (opts && strstr(opts, needle))
        ret = g_list_prepend (ret, g_object_ref (entry));
    }

  g_list_foreach (entries, (GFunc) g_object_unref, NULL);
  g_list_free (entries);
  return ret;
}

static GList *
find_crypttab_entries_for_needle (gchar          *needle,
                                  StoragedDaemon *daemon)
{
  GList *entries;
  GList *l;
  GList *ret;

  ret = NULL;

  entries = storaged_crypttab_monitor_get_entries (storaged_daemon_get_crypttab_monitor (daemon));
  for (l = entries; l != NULL; l = l->next)
    {
      StoragedCrypttabEntry *entry = STORAGED_CRYPTTAB_ENTRY (l->data);
      const gchar *opts = NULL;

      opts = storaged_crypttab_entry_get_options (entry);
      if (opts && strstr(opts, needle))
        ret = g_list_prepend (ret, g_object_ref (entry));
    }

  g_list_foreach (entries, (GFunc) g_object_unref, NULL);
  g_list_free (entries);
  return ret;
}

/* returns a floating GVariant */
static GVariant *
find_configurations (gchar                   *needle,
                     StoragedDaemon          *daemon,
                     gboolean                 include_secrets,
                     GError                 **error)
{
  GList *entries;
  GList *l;
  GVariantBuilder builder;
  GVariant *ret;

  storaged_debug ("Looking for %s", needle);

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  ret = NULL;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(sa{sv})"));
  /* First the /etc/fstab entries */
  entries = find_fstab_entries_for_needle (needle, daemon);
  for (l = entries; l != NULL; l = l->next)
    add_fstab_entry (&builder, STORAGED_FSTAB_ENTRY (l->data));
  g_list_foreach (entries, (GFunc) g_object_unref, NULL);
  g_list_free (entries);

  /* Then the /etc/crypttab entries */
  entries = find_crypttab_entries_for_needle (needle, daemon);
  for (l = entries; l != NULL; l = l->next)
    {
      if (!add_crypttab_entry (&builder, STORAGED_CRYPTTAB_ENTRY (l->data), include_secrets, error))
        {
          g_variant_builder_clear (&builder);
          g_list_foreach (entries, (GFunc) g_object_unref, NULL);
          g_list_free (entries);
          goto out;
        }
    }
  g_list_foreach (entries, (GFunc) g_object_unref, NULL);
  g_list_free (entries);

  ret = g_variant_builder_end (&builder);

 out:
  return ret;
}

GVariant *
storaged_linux_find_child_configuration (StoragedDaemon *daemon,
                                         const gchar *uuid)
{
  GError *error = NULL;
  gchar *needle = g_strdup_printf ("x-parent=%s", uuid);
  GVariant *res = find_configurations (needle, daemon, FALSE, &error);
  if (res == NULL)
    {
      storaged_warning ("Error loading configuration: %s (%s, %d)",
                        error->message, g_quark_to_string (error->domain), error->code);
      g_error_free (error);
      res = g_variant_new ("a(sa{sv})", NULL);
    }
  g_free (needle);
  return res;
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * storaged_linux_block_update:
 * @block: A #StoragedLinuxBlock.
 * @object: The enclosing #StoragedLinuxBlockObject instance.
 *
 * Updates the interface.
 */
void
storaged_linux_block_update (StoragedLinuxBlock        *block,
                             StoragedLinuxBlockObject *object)
{
  StoragedBlock *iface = STORAGED_BLOCK (block);
  StoragedDaemon *daemon;
  GDBusObjectManagerServer *object_manager;
  StoragedLinuxDevice *device;
  GUdevDeviceNumber dev;
  gchar *drive_object_path;
  StoragedDrive *drive;
  gchar *s;
  const gchar *device_file;
  const gchar *const *symlinks;
  const gchar *preferred_device_file;
  const gchar *id_device_file;
  gboolean media_removable = FALSE;
  guint64 size;
  gboolean media_available;
  gboolean media_change_detected;
  gboolean read_only;
  guint n;

  drive = NULL;

  device = storaged_linux_block_object_get_device (object);
  if (device == NULL)
    goto out;

  daemon = storaged_linux_block_object_get_daemon (object);
  object_manager = storaged_daemon_get_object_manager (daemon);

  dev = g_udev_device_get_device_number (device->udev_device);
  device_file = g_udev_device_get_device_file (device->udev_device);
  symlinks = g_udev_device_get_device_file_symlinks (device->udev_device);

  storaged_block_set_device (iface, device_file);
  storaged_block_set_symlinks (iface, symlinks);
  storaged_block_set_device_number (iface, dev);

  size = storaged_daemon_util_block_get_size (device->udev_device,
                                            &media_available,
                                            &media_change_detected);
  storaged_block_set_size (iface, size);

  read_only = g_udev_device_get_sysfs_attr_as_boolean (device->udev_device, "ro");
  if (!read_only && g_str_has_prefix (g_udev_device_get_name (device->udev_device), "sr"))
    read_only = TRUE;
  storaged_block_set_read_only (iface, read_only);

  /* dm-crypt
   *
   * TODO: this might not be the best way to determine if the device-mapper device
   *       is a dm-crypt device.. but unfortunately device-mapper keeps all this stuff
   *       in user-space and wants you to use libdevmapper to obtain it...
   */
  storaged_block_set_crypto_backing_device (iface, "/");
  if (g_str_has_prefix (g_udev_device_get_name (device->udev_device), "dm-"))
    {
      gchar *dm_uuid;
      dm_uuid = get_sysfs_attr (device->udev_device, "dm/uuid");
      if (dm_uuid != NULL && g_str_has_prefix (dm_uuid, "CRYPT-LUKS1"))
        {
          gchar **slaves;
          slaves = storaged_daemon_util_resolve_links (g_udev_device_get_sysfs_path (device->udev_device),
                                                     "slaves");
          if (g_strv_length (slaves) == 1)
            {
              gchar *slave_object_path;
              slave_object_path = find_block_device_by_sysfs_path (object_manager, slaves[0]);
              if (slave_object_path != NULL)
                {
                  storaged_block_set_crypto_backing_device (iface, slave_object_path);
                }
              g_free (slave_object_path);
            }
          g_strfreev (slaves);
        }
      g_free (dm_uuid);
    }

  /* Sort out preferred device... this is what UI shells should
   * display. We default to the block device name.
   *
   * This is mostly for things like device-mapper where device file is
   * a name of the form dm-%d and a symlink name conveys more
   * information.
   */
  preferred_device_file = NULL;
  if (g_str_has_prefix (device_file, "/dev/dm-"))
    {
      const gchar *dm_name;
      gchar *dm_name_dev_file = NULL;
      const gchar *dm_name_dev_file_as_symlink = NULL;

      const gchar *dm_vg_name;
      const gchar *dm_lv_name;
      gchar *dm_lvm_dev_file = NULL;

      dm_name = g_udev_device_get_property (device->udev_device, "DM_NAME");
      if (dm_name != NULL)
        dm_name_dev_file = g_strdup_printf ("/dev/mapper/%s", dm_name);

      dm_vg_name = g_udev_device_get_property (device->udev_device, "DM_VG_NAME");
      dm_lv_name = g_udev_device_get_property (device->udev_device, "DM_LV_NAME");
      if (dm_vg_name != NULL && dm_lv_name != NULL)
        dm_lvm_dev_file =  g_strdup_printf ("/dev/%s/%s", dm_vg_name, dm_lv_name);

      for (n = 0; symlinks != NULL && symlinks[n] != NULL; n++)
        {
          if (g_str_has_prefix (symlinks[n], "/dev/vg_")
              || g_strcmp0 (symlinks[n], dm_lvm_dev_file) == 0)
            {
              /* LVM2 */
              preferred_device_file = symlinks[n];
              break;
            }
          else if (g_strcmp0 (symlinks[n], dm_name_dev_file) == 0)
            {
              dm_name_dev_file_as_symlink = symlinks[n];
            }
        }
      /* fall back to /dev/mapper/$DM_NAME, if available as a symlink */
      if (preferred_device_file == NULL && dm_name_dev_file_as_symlink != NULL)
        preferred_device_file = dm_name_dev_file_as_symlink;
      g_free (dm_name_dev_file);
      g_free (dm_lvm_dev_file);
    }
  else if (g_str_has_prefix (device_file, "/dev/md"))
    {
      const gchar *md_name;

      md_name = g_udev_device_get_property (device->udev_device, "STORAGED_MD_NAME");
      if (md_name != NULL)
        {
          gchar *md_name_dev_file = NULL;
          const gchar *sep;
          const gchar *md_name_dev_file_as_symlink = NULL;
          /* skip homehost */
          sep = strstr (md_name, ":");
          if (sep != NULL)
            md_name_dev_file = g_strdup_printf ("/dev/md/%s", sep + 1);
          else
            md_name_dev_file = g_strdup_printf ("/dev/md/%s", md_name);
          for (n = 0; symlinks != NULL && symlinks[n] != NULL; n++)
            {
              if (g_str_has_prefix (symlinks[n], md_name_dev_file))
                {
                  md_name_dev_file_as_symlink = symlinks[n];
                }
            }
          /* Use /dev/md/$MD_NAME, if it's available as a symlink */
          if (preferred_device_file == NULL && md_name_dev_file_as_symlink != NULL)
            preferred_device_file = md_name_dev_file_as_symlink;
          g_free (md_name_dev_file);
        }
    }
  /* fallback to the device name */
  if (preferred_device_file == NULL)
    preferred_device_file = g_udev_device_get_device_file (device->udev_device);
  storaged_block_set_preferred_device (iface, preferred_device_file);

  /* Determine the drive this block device belongs to
   *
   * TODO: if this is slow we could have a cache or ensure that we
   * only do this once or something else
   */
  drive_object_path = find_drive (object_manager, device->udev_device, &drive);
  if (drive_object_path != NULL)
    {
      storaged_block_set_drive (iface, drive_object_path);
      g_free (drive_object_path);
    }
  else
    {
      storaged_block_set_drive (iface, "/");
    }

  if (drive != NULL)
    media_removable = storaged_drive_get_media_removable (drive);

  id_device_file = NULL;
  if (media_removable)
    {
      /* Drive with removable media: determine id by finding a
       * suitable /dev/disk/by-uuid symlink (fall back to
       * /dev/disk/by-label)
       *
       * TODO: add features to ata_id / cdrom_id in systemd to extract
       *       medium identiers (at optical discs have these) and add
       *       udev rules to create symlinks in something like
       *       /dev/disk/by-medium. Then use said symlinks to for the
       *       id_device_file
       */
      for (n = 0; symlinks != NULL && symlinks[n] != NULL; n++)
        {
          if (g_str_has_prefix (symlinks[n], "/dev/disk/by-uuid/"))
            {
              id_device_file = symlinks[n];
              break;
            }
          else if (g_str_has_prefix (symlinks[n], "/dev/disk/by-label/"))
            {
              id_device_file = symlinks[n];
            }
        }
    }
  else
    {
      /* Drive without removable media: determine id by finding a
       * suitable /dev/disk/by-id symlink
       */
      for (n = 0; symlinks != NULL && symlinks[n] != NULL; n++)
        {
          if (g_str_has_prefix (symlinks[n], "/dev/disk/by-id/"))
            {
              id_device_file = symlinks[n];
              break;
            }
        }
    }
  if (id_device_file != NULL)
    {
      gchar *id = g_strdup (id_device_file + strlen ("/dev/disk/"));
      for (n = 0; id[n] != '\0'; n++)
        {
          if (id[n] == '/' || id[n] == ' ')
            id[n] = '-';
        }
      storaged_block_set_id (iface, id);
      g_free (id);
    }
  else
    {
      storaged_block_set_id (iface, NULL);
    }

  storaged_block_set_id_usage (iface, g_udev_device_get_property (device->udev_device, "ID_FS_USAGE"));
  storaged_block_set_id_type (iface, g_udev_device_get_property (device->udev_device, "ID_FS_TYPE"));
  s = storaged_decode_udev_string (g_udev_device_get_property (device->udev_device, "ID_FS_VERSION"));
  storaged_block_set_id_version (iface, s);
  g_free (s);
  s = storaged_decode_udev_string (g_udev_device_get_property (device->udev_device, "ID_FS_LABEL_ENC"));
  storaged_block_set_id_label (iface, s);
  g_free (s);
  s = storaged_decode_udev_string (g_udev_device_get_property (device->udev_device, "ID_FS_UUID_ENC"));
  storaged_block_set_id_uuid (iface, s);
  g_free (s);

  update_hints (block, device, drive);
  update_configuration (block, daemon);
  update_mdraid (block, device, drive, object_manager);

 out:
  if (device != NULL)
    g_object_unref (device);
  if (drive != NULL)
    g_object_unref (drive);
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
handle_get_secret_configuration (StoragedBlock           *_block,
                                 GDBusMethodInvocation   *invocation,
                                 GVariant                *options)
{
  StoragedLinuxBlock *block = STORAGED_LINUX_BLOCK (_block);
  StoragedLinuxBlockObject *object;
  StoragedDaemon *daemon;
  GVariant *configuration;
  GError *error;

  error = NULL;
  object = storaged_daemon_util_dup_object (block, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = storaged_linux_block_object_get_daemon (object);

  error = NULL;
  configuration = calculate_configuration (block, daemon, TRUE, &error);
  if (configuration == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  if (!storaged_daemon_util_check_authorization_sync (daemon,
                                                    NULL,
                                                    "org.storaged.Storaged.read-system-configuration-secrets",
                                                    options,
                                                    /* Translators: This is shown in an authentcation dialog when
                                                     * the user is editing settings that involve system-level
                                                     * passwords and secrets
                                                     */
                                                    N_("Authentication is required to read system-level secrets"),
                                                    invocation))
    {
      g_variant_unref (configuration);
      goto out;
    }

  storaged_block_complete_get_secret_configuration (STORAGED_BLOCK (block),
                                                  invocation,
                                                  configuration); /* consumes floating ref */

 out:
  g_clear_object (&object);
  return TRUE; /* returning TRUE means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

static gchar *
escape_fstab (const gchar *source)
{
  GString *s;
  guint n;
  s = g_string_new (NULL);
  for (n = 0; source[n] != '\0'; n++)
    {
      switch (source[n])
        {
        case ' ':
        case '\t':
        case '\n':
        case '\\':
          g_string_append_printf (s, "\\%03o", (guint) source[n]);
          break;

        default:
          g_string_append_c (s, source[n]);
          break;
        }
    }
  return g_string_free (s, FALSE);
}

/* based on g_strcompress() */
static gchar *
unescape_fstab (const gchar *source)
{
  const gchar *p = source, *octal;
  gchar *dest = g_malloc (strlen (source) + 1);
  gchar *q = dest;

  while (*p)
    {
      if (*p == '\\')
        {
          p++;
          switch (*p)
            {
            case '\0':
              storaged_warning ("unescape_fstab: trailing \\");
              goto out;
            case '0':  case '1':  case '2':  case '3':  case '4':
            case '5':  case '6':  case '7':
              *q = 0;
              octal = p;
              while ((p < octal + 3) && (*p >= '0') && (*p <= '7'))
                {
                  *q = (*q * 8) + (*p - '0');
                  p++;
                }
              q++;
              p--;
              break;
            default:            /* Also handles \" and \\ */
              *q++ = *p;
              break;
            }
        }
      else
        *q++ = *p;
      p++;
    }
out:
  *q = 0;

  return dest;
}

/* ---------------------------------------------------------------------------------------------------- */

static gchar *
make_block_fsname (StoragedBlock *block)
{
  const gchar *uuid = storaged_block_get_id_uuid (block);

  if (uuid && *uuid)
    return g_strdup_printf ("UUID=%s", uuid);
  else
    return g_strdup (storaged_block_get_device (block));
}

static gchar *
track_parents (StoragedBlock *block, const gchar *options)
{
  StoragedObject *object = STORAGED_OBJECT (g_dbus_interface_get_object (G_DBUS_INTERFACE (block)));
  StoragedDaemon *daemon = storaged_linux_block_object_get_daemon (STORAGED_LINUX_BLOCK_OBJECT (object));

  gchar *new_options, *start, *end, *path;

  /* Remove old x-parent entries
   */
  new_options = g_strdup (options);
  start = new_options;
  while ((start = strstr (start, "x-parent=")) != NULL)
    {
      end = strchr (start, ',');
      if (end)
        strcpy (start, end+1);
      else
        *start = '\0';
    }

  /* Walk up our ancestry and give each parent a chance to be tracked.
   */
  path = g_strdup (g_dbus_object_get_object_path (G_DBUS_OBJECT (object)));
  do
    {
      gchar *uuid = NULL;
      gchar *parent_path = storaged_daemon_get_parent_for_tracking (daemon, path, &uuid);

      if (uuid && *uuid)
        {
          gchar *new;
          if (new_options && *new_options)
            new = g_strdup_printf ("%s,x-parent=%s", new_options, uuid);
          else
            new = g_strdup_printf ("x-parent=%s", uuid);
          g_free (new_options);
          new_options = new;
        }

      g_free (uuid);
      g_free (path);
      path = parent_path;
    }
  while (path);

  return new_options;
}

static gboolean
add_remove_fstab_entry (StoragedBlock *block,
                        GVariant  *remove,
                        GVariant  *add,
                        GError   **error)
{
  struct mntent mntent_remove;
  struct mntent mntent_add;
  gboolean track_parents_flag;
  gboolean ret;
  gchar *auto_fsname = NULL;
  gchar *auto_opts = NULL;
  gchar *contents;
  gchar **lines;
  GString *str;
  gboolean removed;
  guint n;

  contents = NULL;
  lines = NULL;
  str = NULL;
  ret = FALSE;

  if (remove != NULL)
    {
      if (!g_variant_lookup (remove, "fsname", "^&ay", &mntent_remove.mnt_fsname) ||
          !g_variant_lookup (remove, "dir", "^&ay", &mntent_remove.mnt_dir) ||
          !g_variant_lookup (remove, "type", "^&ay", &mntent_remove.mnt_type) ||
          !g_variant_lookup (remove, "opts", "^&ay", &mntent_remove.mnt_opts) ||
          !g_variant_lookup (remove, "freq", "i", &mntent_remove.mnt_freq) ||
          !g_variant_lookup (remove, "passno", "i", &mntent_remove.mnt_passno))
        {
          g_set_error (error,
                       STORAGED_ERROR,
                       STORAGED_ERROR_FAILED,
                       "Missing fsname, dir, type, opts, freq or passno parameter in entry to remove");
          goto out;
        }
    }

  if (add != NULL)
    {
      if (!g_variant_lookup (add, "fsname", "^&ay", &mntent_add.mnt_fsname))
        {
          auto_fsname = make_block_fsname (block);
          mntent_add.mnt_fsname = auto_fsname;
        }

      if (!g_variant_lookup (add, "dir", "^&ay", &mntent_add.mnt_dir) ||
          !g_variant_lookup (add, "type", "^&ay", &mntent_add.mnt_type) ||
          !g_variant_lookup (add, "opts", "^&ay", &mntent_add.mnt_opts) ||
          !g_variant_lookup (add, "freq", "i", &mntent_add.mnt_freq) ||
          !g_variant_lookup (add, "passno", "i", &mntent_add.mnt_passno))
        {
          g_set_error (error,
                       STORAGED_ERROR,
                       STORAGED_ERROR_FAILED,
                       "Missing dir, type, opts, freq or passno parameter in entry to add");
          goto out;
        }

      if (strlen (mntent_add.mnt_opts) == 0)
        {
          g_set_error (error,
                       STORAGED_ERROR,
                       STORAGED_ERROR_FAILED,
                       "opts must not be blank");
          goto out;
        }

      if (g_variant_lookup (add, "track-parents", "b", &track_parents_flag) &&
          track_parents_flag)
        {
          auto_opts = track_parents (block, mntent_add.mnt_opts);
          mntent_add.mnt_opts = auto_opts;
        }
    }

  if (!g_file_get_contents ("/etc/fstab",
                            &contents,
                            NULL,
                            error))
    goto out;

  lines = g_strsplit (contents, "\n", 0);

  str = g_string_new (NULL);
  removed = FALSE;
  for (n = 0; lines != NULL && lines[n] != NULL; n++)
    {
      const gchar *line = lines[n];
      if (strlen (line) == 0 && lines[n+1] == NULL)
        break;
      if (remove != NULL && !removed)
        {
          gchar parsed_fsname[512];
          gchar parsed_dir[512];
          gchar parsed_type[512];
          gchar parsed_opts[512];
          gint parsed_freq;
          gint parsed_passno;
          if (sscanf (line, "%511s %511s %511s %511s %d %d",
                      parsed_fsname,
                      parsed_dir,
                      parsed_type,
                      parsed_opts,
                      &parsed_freq,
                      &parsed_passno) == 6)
            {
              gchar *unescaped_fsname = unescape_fstab (parsed_fsname);
              gchar *unescaped_dir = unescape_fstab (parsed_dir);
              gchar *unescaped_type = unescape_fstab (parsed_type);
              gchar *unescaped_opts = unescape_fstab (parsed_opts);
              gboolean matches = FALSE;
              if (g_strcmp0 (unescaped_fsname,   mntent_remove.mnt_fsname) == 0 &&
                  g_strcmp0 (unescaped_dir,      mntent_remove.mnt_dir) == 0 &&
                  g_strcmp0 (unescaped_type,     mntent_remove.mnt_type) == 0 &&
                  g_strcmp0 (unescaped_opts,     mntent_remove.mnt_opts) == 0 &&
                  parsed_freq ==      mntent_remove.mnt_freq &&
                  parsed_passno ==    mntent_remove.mnt_passno)
                {
                  matches = TRUE;
                }
              g_free (unescaped_fsname);
              g_free (unescaped_dir);
              g_free (unescaped_type);
              g_free (unescaped_opts);
              if (matches)
                {
                  removed = TRUE;
                  continue;
                }
            }
        }
      g_string_append (str, line);
      g_string_append_c (str, '\n');
    }

  if (remove != NULL && !removed)
    {
      g_set_error (error,
                   STORAGED_ERROR,
                   STORAGED_ERROR_FAILED,
                   "Didn't find entry to remove");
      goto out;
    }

  if (add != NULL)
    {
      gchar *escaped_fsname = escape_fstab (mntent_add.mnt_fsname);
      gchar *escaped_dir = escape_fstab (mntent_add.mnt_dir);
      gchar *escaped_type = escape_fstab (mntent_add.mnt_type);
      gchar *escaped_opts = escape_fstab (mntent_add.mnt_opts);
      g_string_append_printf (str, "%s %s %s %s %d %d\n",
                              escaped_fsname,
                              escaped_dir,
                              escaped_type,
                              escaped_opts,
                              mntent_add.mnt_freq,
                              mntent_add.mnt_passno);
      g_free (escaped_fsname);
      g_free (escaped_dir);
      g_free (escaped_type);
      g_free (escaped_opts);
    }

  if (!storaged_daemon_util_file_set_contents ("/etc/fstab",
                                               str->str,
                                               -1,
                                               0644, /* mode to use if non-existant */
                                               error))
    goto out;

  ret = TRUE;

 out:
  g_free (auto_opts);
  g_free (auto_fsname);
  g_strfreev (lines);
  g_free (contents);
  if (str != NULL)
    g_string_free (str, TRUE);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
has_whitespace (const gchar *s)
{
  guint n;
  g_return_val_if_fail (s != NULL, TRUE);
  for (n = 0; s[n] != '\0'; n++)
    if (g_ascii_isspace (s[n]))
      return TRUE;
  return FALSE;
}

static gchar *
make_block_luksname (StoragedBlock *block)
{
  const gchar *uuid = storaged_block_get_id_uuid (block);

  if (uuid && *uuid)
    return g_strdup_printf ("luks-%s", uuid);

  return NULL;
}

static gboolean
add_remove_crypttab_entry (StoragedBlock *block,
                           GVariant  *remove,
                           GVariant  *add,
                           GError   **error)
{
  const gchar *remove_name = NULL;
  const gchar *remove_device = NULL;
  const gchar *remove_passphrase_path = NULL;
  const gchar *remove_options = NULL;
  const gchar *add_name = NULL;
  const gchar *add_device = NULL;
  const gchar *add_passphrase_path = NULL;
  const gchar *add_options = NULL;
  const gchar *add_passphrase_contents = NULL;
  gboolean track_parents_flag;
  gboolean ret;
  gchar *auto_name = NULL;
  gchar *auto_device = NULL;
  gchar *auto_passphrase_path = NULL;
  gchar *auto_opts = NULL;
  gchar *contents;
  gchar **lines;
  GString *str;
  gboolean removed;
  guint n;

  contents = NULL;
  lines = NULL;
  str = NULL;
  ret = FALSE;

  if (remove != NULL)
    {
      if (!g_variant_lookup (remove, "name", "^&ay", &remove_name) ||
          !g_variant_lookup (remove, "device", "^&ay", &remove_device) ||
          !g_variant_lookup (remove, "passphrase-path", "^&ay", &remove_passphrase_path) ||
          !g_variant_lookup (remove, "options", "^&ay", &remove_options))
        {
          g_set_error (error,
                       STORAGED_ERROR,
                       STORAGED_ERROR_FAILED,
                       "Missing name, device, passphrase-path, options or parameter in entry to remove");
          goto out;
        }
    }

  if (add != NULL)
    {
      if (!g_variant_lookup (add, "name", "^&ay", &add_name))
        {
          const gchar *uuid = storaged_block_get_id_uuid (block);
          if (uuid == NULL || *uuid == '\0')
            {
              g_set_error (error,
                           STORAGED_ERROR,
                           STORAGED_ERROR_FAILED,
                           "Block device has no UUID, can't determine default name");
              goto out;
            }

          auto_name = g_strdup_printf ("luks-%s", uuid);
          add_name = auto_name;
        }

      if (!g_variant_lookup (add, "device", "^&ay", &add_device))
        {
          auto_device = make_block_fsname (block);
          add_device = auto_device;
        }

      if (!g_variant_lookup (add, "options", "^&ay", &add_options) ||
          !g_variant_lookup (add, "passphrase-contents", "^&ay", &add_passphrase_contents))
        {
          g_set_error (error,
                       STORAGED_ERROR,
                       STORAGED_ERROR_FAILED,
                       "Missing passphrase-path, options or passphrase-contents parameter in entry to add");
          goto out;
        }

      if (!g_variant_lookup (add, "passphrase-path", "^&ay", &add_passphrase_path))
        {
          if (*add_passphrase_contents == '\0')
            add_passphrase_path = "";
          else
            {
              auto_passphrase_path = g_strdup_printf ("/etc/luks-keys/%s", add_name);
              add_passphrase_path = auto_passphrase_path;
            }
        }

      /* reject strings with whitespace in them */
      if (has_whitespace (add_name) ||
          has_whitespace (add_device) ||
          has_whitespace (add_passphrase_path) ||
          has_whitespace (add_options))
        {
          g_set_error (error,
                       STORAGED_ERROR,
                       STORAGED_ERROR_FAILED,
                       "One of name, device, passphrase-path or options parameter are invalid (whitespace)");
          goto out;
        }

      if (g_variant_lookup (add, "track-parents", "b", &track_parents_flag) &&
          track_parents_flag)
        {
          auto_opts = track_parents (block, add_options);
          add_options = auto_opts;
        }
    }

  if (!g_file_get_contents ("/etc/crypttab",
                            &contents,
                            NULL,
                            error))
    goto out;

  lines = g_strsplit (contents, "\n", 0);

  str = g_string_new (NULL);
  removed = FALSE;
  for (n = 0; lines != NULL && lines[n] != NULL; n++)
    {
      const gchar *line = lines[n];
      if (strlen (line) == 0 && lines[n+1] == NULL)
        break;
      if (remove != NULL && !removed)
        {
          gchar parsed_name[512];
          gchar parsed_device[512];
          gchar parsed_passphrase_path[512];
          gchar parsed_options[512];
          guint num_parsed;

          num_parsed = sscanf (line, "%511s %511s %511s %511s",
                               parsed_name, parsed_device, parsed_passphrase_path, parsed_options);
          if (num_parsed >= 2)
            {
              if (num_parsed < 3 || g_strcmp0 (parsed_passphrase_path, "none") == 0)
                strcpy (parsed_passphrase_path, "");
              if (num_parsed < 4)
                strcpy (parsed_options, "");
              if (g_strcmp0 (parsed_name,            remove_name) == 0 &&
                  g_strcmp0 (parsed_device,          remove_device) == 0 &&
                  g_strcmp0 (parsed_passphrase_path, remove_passphrase_path) == 0 &&
                  g_strcmp0 (parsed_options,         remove_options) == 0)
                {
                  /* Nuke passphrase file */
                  if (strlen (remove_passphrase_path) > 0 && !g_str_has_prefix (remove_passphrase_path, "/dev"))
                    {
                      /* Is this exploitable? No, 1. the user would have to control
                       * the /etc/crypttab file for us to delete it; and 2. editing the
                       * /etc/crypttab file requires a polkit authorization that can't
                       * be retained (e.g. the user is always asked for the password)..
                       */
                      if (unlink (remove_passphrase_path) != 0)
                        {
                          g_set_error (error,
                                       STORAGED_ERROR,
                                       STORAGED_ERROR_FAILED,
                                       "Error deleting file `%s' with passphrase",
                                       remove_passphrase_path);
                          goto out;
                        }
                    }
                  removed = TRUE;
                  continue;
                }
            }
        }
      g_string_append (str, line);
      g_string_append_c (str, '\n');
    }

  if (remove != NULL && !removed)
    {
      g_set_error (error,
                   STORAGED_ERROR,
                   STORAGED_ERROR_FAILED,
                   "Didn't find entry to remove");
      goto out;
    }

  if (add != NULL)
    {
      /* First write add_passphrase_content to add_passphrase_path,
       * if applicable..
       *
       * Is this exploitable? No, because editing the /etc/crypttab
       * file requires a polkit authorization that can't be retained
       * (e.g. the user is always asked for the password)...
       *
       * Just to be on the safe side we only allow writing into the
       * directory /etc/luks-keys if create a _new_ entry.
       */
      if (strlen (add_passphrase_path) > 0)
        {
          gchar *filename;
          if (g_strcmp0 (add_passphrase_path, remove_passphrase_path) == 0)
            {
              filename = g_strdup (add_passphrase_path);
            }
          else
            {
              if (!g_str_has_prefix (add_passphrase_path, "/etc/luks-keys/"))
                {
                  g_set_error (error,
                               STORAGED_ERROR,
                               STORAGED_ERROR_FAILED,
                               "Crypttab passphrase file can only be created in the /etc/luks-keys directory");
                  goto out;
                }
              /* ensure the directory exists */
              if (g_mkdir_with_parents ("/etc/luks-keys", 0700) != 0)
                {
                  g_set_error (error,
                               STORAGED_ERROR,
                               STORAGED_ERROR_FAILED,
                               "Error creating /etc/luks-keys directory: %m");
                  goto out;
                }
              /* avoid symlink attacks */
              filename = g_strdup_printf ("/etc/luks-keys/%s", strrchr (add_passphrase_path, '/') + 1);
            }

          /* Bail if the requested file already exists */
          if (g_file_test (filename, G_FILE_TEST_EXISTS))
            {
                  g_set_error (error,
                               STORAGED_ERROR,
                               STORAGED_ERROR_FAILED,
                               "Refusing to overwrite existing file %s",
                               filename);
                  g_free (filename);
                  goto out;
            }

          if (!storaged_daemon_util_file_set_contents (filename,
                                                       add_passphrase_contents,
                                                       -1,
                                                       0600, /* mode to use if non-existant */
                                                       error))
            {
              g_free (filename);
              goto out;
            }
          g_free (filename);
        }
      g_string_append_printf (str, "%s %s %s %s\n",
                              add_name,
                              add_device,
                              strlen (add_passphrase_path) > 0 ? add_passphrase_path : "none",
                              add_options);
    }

  if (!storaged_daemon_util_file_set_contents ("/etc/crypttab",
                                               str->str,
                                               -1,
                                               0600, /* mode to use if non-existant */
                                               error))
    goto out;

  ret = TRUE;

 out:
  g_free (auto_opts);
  g_free (auto_name);
  g_free (auto_device);
  g_free (auto_passphrase_path);
  g_strfreev (lines);
  g_free (contents);
  if (str != NULL)
    g_string_free (str, TRUE);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
handle_add_configuration_item (StoragedBlock           *_block,
                               GDBusMethodInvocation   *invocation,
                               GVariant                *item,
                               GVariant                *options)
{
  StoragedLinuxBlock *block = STORAGED_LINUX_BLOCK (_block);
  StoragedLinuxBlockObject *object;
  StoragedDaemon *daemon;
  const gchar *type;
  GVariant *details = NULL;
  GError *error;

  error = NULL;
  object = storaged_daemon_util_dup_object (block, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = storaged_linux_block_object_get_daemon (object);

  g_variant_get (item, "(&s@a{sv})", &type, &details);
  if (g_strcmp0 (type, "fstab") == 0)
    {
      if (!storaged_daemon_util_check_authorization_sync (daemon,
                                                          NULL,
                                                          "org.storaged.Storaged.modify-system-configuration",
                                                          options,
                                                          /* Translators: shown in authentication dialog - do not translate /etc/fstab */
                                                          N_("Authentication is required to add an entry to the /etc/fstab file"),
                                                          invocation))
        goto out;
      error = NULL;
      if (!add_remove_fstab_entry (_block, NULL, details, &error))
        {
          g_dbus_method_invocation_take_error (invocation, error);
          goto out;
        }
      storaged_block_complete_add_configuration_item (STORAGED_BLOCK (block), invocation);
    }
  else if (g_strcmp0 (type, "crypttab") == 0)
    {
      if (!storaged_daemon_util_check_authorization_sync (daemon,
                                                          NULL,
                                                          "org.storaged.Storaged.modify-system-configuration",
                                                          options,
                                                          /* Translators: shown in authentication dialog - do not tranlsate /etc/crypttab */
                                                          N_("Authentication is required to add an entry to the /etc/crypttab file"),
                                                          invocation))
        goto out;
      error = NULL;
      if (!add_remove_crypttab_entry (_block, NULL, details, &error))
        {
          g_dbus_method_invocation_take_error (invocation, error);
          goto out;
        }
      storaged_block_complete_add_configuration_item (STORAGED_BLOCK (block), invocation);
    }
  else
    {
      g_dbus_method_invocation_return_error (invocation,
                                             STORAGED_ERROR,
                                             STORAGED_ERROR_FAILED,
                                             "Only /etc/fstab or /etc/crypttab items can be added");
      goto out;
    }

 out:
  g_variant_unref (details);
  g_clear_object (&object);
  return TRUE; /* returning TRUE means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
handle_remove_configuration_item (StoragedBlock           *_block,
                                  GDBusMethodInvocation   *invocation,
                                  GVariant                *item,
                                  GVariant                *options)
{
  StoragedLinuxBlock *block = STORAGED_LINUX_BLOCK (_block);
  StoragedLinuxBlockObject *object;
  StoragedDaemon *daemon;
  const gchar *type;
  GVariant *details = NULL;
  GError *error;

  error = NULL;
  object = storaged_daemon_util_dup_object (block, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = storaged_linux_block_object_get_daemon (object);

  g_variant_get (item, "(&s@a{sv})", &type, &details);
  if (g_strcmp0 (type, "fstab") == 0)
    {
      if (!storaged_daemon_util_check_authorization_sync (daemon,
                                                          NULL,
                                                          "org.storaged.Storaged.modify-system-configuration",
                                                          options,
                                                          /* Translators: shown in authentication dialog - do not translate /etc/fstab */
                                                          N_("Authentication is required to remove an entry from /etc/fstab file"),
                                                          invocation))
        goto out;
      error = NULL;
      if (!add_remove_fstab_entry (_block, details, NULL, &error))
        {
          g_dbus_method_invocation_take_error (invocation, error);
          goto out;
        }
      storaged_block_complete_remove_configuration_item (STORAGED_BLOCK (block), invocation);
    }
  else if (g_strcmp0 (type, "crypttab") == 0)
    {
      if (!storaged_daemon_util_check_authorization_sync (daemon,
                                                          NULL,
                                                          "org.storaged.Storaged.modify-system-configuration",
                                                          options,
                                                          /* Translators: shown in authentication dialog - do not translate /etc/crypttab */
                                                          N_("Authentication is required to remove an entry from the /etc/crypttab file"),
                                                          invocation))
        goto out;
      error = NULL;
      if (!add_remove_crypttab_entry (_block, details, NULL, &error))
        {
          g_dbus_method_invocation_take_error (invocation, error);
          goto out;
        }
      storaged_block_complete_remove_configuration_item (STORAGED_BLOCK (block), invocation);
    }
  else
    {
      g_dbus_method_invocation_return_error (invocation,
                                             STORAGED_ERROR,
                                             STORAGED_ERROR_FAILED,
                                             "Only fstab or crypttab items can be removed");
      goto out;
    }

 out:
  g_variant_unref (details);
  g_clear_object (&object);
  return TRUE; /* returning TRUE means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
handle_update_configuration_item (StoragedBlock           *_block,
                                  GDBusMethodInvocation   *invocation,
                                  GVariant                *old_item,
                                  GVariant                *new_item,
                                  GVariant                *options)
{
  StoragedLinuxBlock *block = STORAGED_LINUX_BLOCK (_block);
  StoragedLinuxBlockObject *object;
  StoragedDaemon *daemon;
  const gchar *old_type;
  const gchar *new_type;
  GVariant *old_details = NULL;
  GVariant *new_details = NULL;
  GError *error;

  error = NULL;
  object = storaged_daemon_util_dup_object (block, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = storaged_linux_block_object_get_daemon (object);

  g_variant_get (old_item, "(&s@a{sv})", &old_type, &old_details);
  g_variant_get (new_item, "(&s@a{sv})", &new_type, &new_details);
  if (g_strcmp0 (old_type, new_type) != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             STORAGED_ERROR,
                                             STORAGED_ERROR_FAILED,
                                             "old and new item are not of the same type");
      goto out;
    }

  if (g_strcmp0 (old_type, "fstab") == 0)
    {
      if (!storaged_daemon_util_check_authorization_sync (daemon,
                                                          NULL,
                                                          "org.storaged.Storaged.modify-system-configuration",
                                                          options,
                                                          /* Translators: shown in authentication dialog - do not translate /etc/fstab */
                                                          N_("Authentication is required to modify the /etc/fstab file"),
                                                          invocation))
        goto out;
      error = NULL;
      if (!add_remove_fstab_entry (_block, old_details, new_details, &error))
        {
          g_dbus_method_invocation_take_error (invocation, error);
          goto out;
        }
      storaged_block_complete_update_configuration_item (STORAGED_BLOCK (block), invocation);
    }
  else if (g_strcmp0 (old_type, "crypttab") == 0)
    {
      if (!storaged_daemon_util_check_authorization_sync (daemon,
                                                        NULL,
                                                        "org.storaged.Storaged.modify-system-configuration",
                                                        options,
                                                        /* Translators: shown in authentication dialog - do not translate /etc/crypttab */
                                                        N_("Authentication is required to modify the /etc/crypttab file"),
                                                        invocation))
        goto out;
      error = NULL;
      if (!add_remove_crypttab_entry (_block, old_details, new_details, &error))
        {
          g_dbus_method_invocation_take_error (invocation, error);
          goto out;
        }
      storaged_block_complete_update_configuration_item (STORAGED_BLOCK (block), invocation);
    }
  else
    {
      g_dbus_method_invocation_return_error (invocation,
                                             STORAGED_ERROR,
                                             STORAGED_ERROR_FAILED,
                                             "Only fstab or crypttab items can be updated");
      goto out;
    }

 out:
  g_variant_unref (new_details);
  g_variant_unref (old_details);
  g_clear_object (&object);
  return TRUE; /* returning TRUE means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

static gchar *
subst_str (const gchar *str,
           const gchar *from,
           const gchar *to)
{
    gchar **parts;
    gchar *result;

    parts = g_strsplit (str, from, 0);
    result = g_strjoinv (to, parts);
    g_strfreev (parts);
    return result;
}


static gchar *
subst_str_and_escape (const gchar *str,
                      const gchar *from,
                      const gchar *to)
{
  gchar *quoted_and_escaped;
  gchar *ret;
  quoted_and_escaped = storaged_daemon_util_escape_and_quote (to);
  ret = subst_str (str, from, quoted_and_escaped);
  g_free (quoted_and_escaped);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

typedef struct
{
  StoragedObject **object_ptr;
  StoragedBlock **block_ptr;
  const gchar  *type;
} FormatWaitData;

/* ---------------------------------------------------------------------------------------------------- */

static void
update_to_current_block_object (StoragedDaemon *daemon,
                                FormatWaitData *data)
{
  GList *objects, *l;

  /* There might be remove/add uevent pairs at any time for
     partitions, even if they haven't really been removed and
     recreated.  Storaged will create a new StoragedObject in that
     scenario and the one we are checking here will be obsolete and
     not receive any updates anymore.

     So we catch that case, complain a bit, and carry on with the new
     StoragedObject.
  */

  if (*data->block_ptr == NULL)
    return;

  objects = storaged_daemon_get_objects (daemon);
  for (l = objects; l != NULL; l = l->next)
    {
      StoragedObject *object = STORAGED_OBJECT (l->data);
      StoragedBlock *block = NULL;

      if (object == *data->object_ptr)
        continue;

      block = storaged_object_get_block (object);
      if (block != NULL)
        {
          if (g_strcmp0 (storaged_block_get_device (block), storaged_block_get_device (*data->block_ptr)) == 0)
            {
              storaged_warning ("Block device %s was removed and readded while we were working on it",
                                storaged_block_get_device (block));
              g_object_ref (object);
              g_object_unref (*data->object_ptr);
              *data->object_ptr = object;

              g_object_unref (*data->block_ptr);
              *data->block_ptr = block;
              break;
            }
          g_object_unref (block);
        }
    }

  g_list_free_full (objects, g_object_unref);
}

/* ---------------------------------------------------------------------------------------------------- */

static StoragedObject *
wait_for_filesystem (StoragedDaemon *daemon,
                     gpointer        user_data)
{
  FormatWaitData *data = user_data;
  StoragedObject *object;
  StoragedBlock *block;
  StoragedObject *ret = NULL;
  StoragedPartitionTable *partition_table = NULL;
  gchar *id_type = NULL;
  gchar *partition_table_type = NULL;

  update_to_current_block_object (daemon, data);
  object = *data->object_ptr;
  block = *data->block_ptr;

  if (block == NULL)
    goto out;

  partition_table = storaged_object_get_partition_table (object);

  id_type = storaged_block_dup_id_type (block);

  if (g_strcmp0 (data->type, "empty") == 0)
    {
      if ((id_type == NULL || g_strcmp0 (id_type, "") == 0) && partition_table == NULL)
        {
          ret = g_object_ref (object);
          goto out;
        }
    }

  if (g_strcmp0 (id_type, data->type) == 0)
    {
      ret = g_object_ref (object);
      goto out;
    }

  if (partition_table != NULL)
    {
      partition_table_type = storaged_partition_table_dup_type_ (partition_table);
      if (g_strcmp0 (partition_table_type, data->type) == 0)
        {
          ret = g_object_ref (object);
          goto out;
        }
    }

 out:
  g_free (partition_table_type);
  g_free (id_type);
  g_clear_object (&partition_table);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static StoragedObject *
wait_for_luks_uuid (StoragedDaemon *daemon,
                    gpointer        user_data)
{
  FormatWaitData *data = user_data;
  StoragedObject *object;
  StoragedBlock *block;
  StoragedObject *ret = NULL;

  update_to_current_block_object (daemon, data);
  object = *data->object_ptr;
  block = *data->block_ptr;

  if (block == NULL)
    goto out;

  if (g_strcmp0 (storaged_block_get_id_type (block), "crypto_LUKS") != 0)
    goto out;

  ret = g_object_ref (object);

 out:
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static StoragedObject *
wait_for_luks_cleartext (StoragedDaemon *daemon,
                         gpointer        user_data)
{
  FormatWaitData *data = user_data;
  StoragedObject *ret = NULL;
  GList *objects, *l;

  update_to_current_block_object (daemon, data);

  objects = storaged_daemon_get_objects (daemon);
  for (l = objects; l != NULL; l = l->next)
    {
      StoragedObject *object = STORAGED_OBJECT (l->data);
      StoragedBlock *block = NULL;

      block = storaged_object_get_block (object);
      if (block != NULL)
        {
          if (g_strcmp0 (storaged_block_get_crypto_backing_device (block),
                         g_dbus_object_get_object_path (G_DBUS_OBJECT (*data->object_ptr))) == 0)
            {
              g_object_unref (block);
              ret = g_object_ref (object);
              goto out;
            }
          g_object_unref (block);
        }
    }

 out:
  g_list_free_full (objects, g_object_unref);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
erase_ata_device (StoragedBlock  *block,
                  StoragedObject *object,
                  StoragedDaemon *daemon,
                  uid_t           caller_uid,
                  gboolean        enhanced,
                  GError        **error)
{
  gboolean ret = FALSE;
  StoragedObject *drive_object = NULL;
  StoragedDriveAta *ata = NULL;

  drive_object = storaged_daemon_find_object (daemon, storaged_block_get_drive (block));
  if (drive_object == NULL)
    {
      g_set_error (error, STORAGED_ERROR, STORAGED_ERROR_FAILED, "No drive object");
      goto out;
    }
  ata = storaged_object_get_drive_ata (drive_object);
  if (ata == NULL)
    {
      g_set_error (error, STORAGED_ERROR, STORAGED_ERROR_FAILED, "Drive is not an ATA drive");
      goto out;
    }

  /* sleep a tiny bit here to avoid the secure erase code racing with
   * programs spawned by udev
   */
  g_usleep (500 * 1000);

  ret = storaged_linux_drive_ata_secure_erase_sync (STORAGED_LINUX_DRIVE_ATA (ata),
                                                  caller_uid,
                                                  enhanced,
                                                  error);

 out:
  g_clear_object (&ata);
  g_clear_object (&drive_object);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

#define ERASE_SIZE (1 * 1024*1024)

static gboolean
erase_device (StoragedBlock  *block,
              StoragedObject *object,
              StoragedDaemon *daemon,
              uid_t           caller_uid,
              const gchar    *erase_type,
              GError        **error)
{
  gboolean ret = FALSE;
  const gchar *device_file = NULL;
  StoragedBaseJob *job = NULL;
  gint fd = -1;
  guint64 size;
  guint64 pos;
  guchar *buf = NULL;
  gint64 time_of_last_signal;
  GError *local_error = NULL;

  if (g_strcmp0 (erase_type, "ata-secure-erase") == 0)
    {
      ret = erase_ata_device (block, object, daemon, caller_uid, FALSE, error);
      goto out;
    }
  else if (g_strcmp0 (erase_type, "ata-secure-erase-enhanced") == 0)
    {
      ret = erase_ata_device (block, object, daemon, caller_uid, TRUE, error);
      goto out;
    }
  else if (g_strcmp0 (erase_type, "zero") != 0)
    {
      g_set_error (&local_error, STORAGED_ERROR, STORAGED_ERROR_FAILED,
                   "Unknown or unsupported erase type `%s'",
                   erase_type);
      goto out;
    }

  device_file = storaged_block_get_device (block);
  fd = open (device_file, O_WRONLY | O_SYNC | O_EXCL);
  if (fd == -1)
    {
      g_set_error (&local_error, STORAGED_ERROR, STORAGED_ERROR_FAILED,
                   "Error opening device %s: %m", device_file);
      goto out;
    }

  job = storaged_daemon_launch_simple_job (daemon, object, "format-erase", caller_uid, NULL);
  storaged_base_job_set_auto_estimate (STORAGED_BASE_JOB (job), TRUE);
  storaged_job_set_progress_valid (STORAGED_JOB (job), TRUE);

  if (ioctl (fd, BLKGETSIZE64, &size) != 0)
    {
      g_set_error (&local_error, STORAGED_ERROR, STORAGED_ERROR_FAILED,
                   "Error doing BLKGETSIZE64 iotctl on %s: %m", device_file);
      goto out;
    }

  storaged_job_set_bytes (STORAGED_JOB (job), size);

  buf = g_new0 (guchar, ERASE_SIZE);
  pos = 0;
  time_of_last_signal = g_get_monotonic_time ();
  while (pos < size)
    {
      size_t to_write;
      ssize_t num_written;
      gint64 now;

      to_write = MIN (size - pos, ERASE_SIZE);
    again:
      num_written = write (fd, buf, to_write);
      if (num_written == -1 || num_written == 0)
        {
          if (errno == EINTR)
            goto again;
          g_set_error (&local_error, STORAGED_ERROR, STORAGED_ERROR_FAILED,
                       "Error writing %d bytes to %s: %m",
                       (gint) to_write, device_file);
          goto out;
        }
      pos += num_written;

      if (g_cancellable_is_cancelled (storaged_base_job_get_cancellable (job)))
        {
          g_set_error (&local_error, STORAGED_ERROR, STORAGED_ERROR_CANCELLED,
                       "Job was canceled");
          goto out;
        }

      /* only emit D-Bus signal at most once a second */
      now = g_get_monotonic_time ();
      if (now - time_of_last_signal > G_USEC_PER_SEC)
        {
          /* TODO: estimation etc. */
          storaged_job_set_progress (STORAGED_JOB (job), ((gdouble) pos) / size);
          time_of_last_signal = now;
        }
    }

  ret = TRUE;

 out:
  if (job != NULL)
    {
      if (local_error != NULL)
        storaged_simple_job_complete (STORAGED_SIMPLE_JOB (job), FALSE, local_error->message);
      else
        storaged_simple_job_complete (STORAGED_SIMPLE_JOB (job), TRUE, "");
    }
  if (local_error != NULL)
    g_propagate_error (error, local_error);
  g_free (buf);
  if (fd != -1)
    close (fd);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static const struct
{
  const gchar *table_type;
  const gchar *id_type;
  const gchar *partition_type;
} partition_types_by_id[] = {
  {"dos", "vfat",         "0x0c"},
  {"dos", "ntfs",         "0x07"},
  {"dos", "exfat",        "0x0c"},
  {"dos", "swap",         "0x82"},
  {"dos", "ext2",         "0x83"},
  {"dos", "ext3",         "0x83"},
  {"dos", "ext4",         "0x83"},
  {"dos", "xfs",          "0x83"},
  {"dos", "btrfs",        "0x83"},
  {"dos", "crypto_LUKS",  "0x83"}, /* TODO: perhaps default to LUKS-specific type */

  {"gpt", "vfat",         "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7"}, /* Microsoft Basic Data */
  {"gpt", "ntfs",         "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7"},
  {"gpt", "exfat",        "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7"},
  {"gpt", "swap",         "0657fd6d-a4ab-43c4-84e5-0933c84b4f4f"}, /* Linux Swap */
  {"gpt", "ext2",         "0fc63daf-8483-4772-8e79-3d69d8477de4"}, /* Linux Filesystem */
  {"gpt", "ext3",         "0fc63daf-8483-4772-8e79-3d69d8477de4"},
  {"gpt", "ext4",         "0fc63daf-8483-4772-8e79-3d69d8477de4"},
  {"gpt", "xfs",          "0fc63daf-8483-4772-8e79-3d69d8477de4"},
  {"gpt", "btrfs",        "0fc63daf-8483-4772-8e79-3d69d8477de4"},
  {"gpt", "crypto_LUKS",  "0fc63daf-8483-4772-8e79-3d69d8477de4"}, /* TODO: perhaps default to LUKS-specific type */
};


/* may return NULL if nothing suitable was found */
static const gchar *
determine_partition_type_for_id (const gchar *table_type,
                                 const gchar *id_type)
{
  const gchar *ret = NULL;
  guint n;

  for (n = 0; n < G_N_ELEMENTS (partition_types_by_id); n++)
    {
      if (g_strcmp0 (partition_types_by_id[n].table_type, table_type) == 0 &&
          g_strcmp0 (partition_types_by_id[n].id_type,    id_type) == 0)
        {
          ret = partition_types_by_id[n].partition_type;
          goto out;
        }
    }
 out:
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

typedef gboolean BlockWalker (StoragedDaemon *daemon,
                              StoragedBlock *block,
                              gboolean is_leaf,
                              gpointer user_data,
                              GError **error);

static StoragedPartitionTable *
peek_partition_table (StoragedDaemon *daemon,
                      StoragedPartition *partition)
{
  StoragedObject *object = storaged_daemon_find_object (daemon, storaged_partition_get_table (partition));
  return object? storaged_object_peek_partition_table (object) : NULL;
}

static GList *
get_partitions (StoragedDaemon         *daemon,
                StoragedPartitionTable *table)
{
  GList *ret = NULL;
  GDBusObject *table_object;
  const gchar *table_object_path;
  GList *l, *object_proxies = NULL;

  table_object = g_dbus_interface_get_object (G_DBUS_INTERFACE (table));
  if (table_object == NULL)
    goto out;
  table_object_path = g_dbus_object_get_object_path (table_object);

  object_proxies = storaged_daemon_get_objects (daemon);
  for (l = object_proxies; l != NULL; l = l->next)
    {
      StoragedObject *object = STORAGED_OBJECT (l->data);
      StoragedPartition *partition;

      partition = storaged_object_get_partition (object);
      if (partition == NULL)
        continue;

      if (g_strcmp0 (storaged_partition_get_table (partition), table_object_path) == 0)
        ret = g_list_prepend (ret, g_object_ref (partition));

      g_object_unref (partition);
    }
  ret = g_list_reverse (ret);
 out:
  g_list_foreach (object_proxies, (GFunc) g_object_unref, NULL);
  g_list_free (object_proxies);
  return ret;
}

static StoragedBlock *
get_cleartext_block (StoragedDaemon  *daemon,
                     StoragedBlock   *block)
{
  StoragedBlock *ret = NULL;
  GDBusObject *object;
  const gchar *object_path;
  GList *objects = NULL;
  GList *l;

  object = g_dbus_interface_get_object (G_DBUS_INTERFACE (block));
  if (object == NULL)
    goto out;

  object_path = g_dbus_object_get_object_path (object);
  objects = storaged_daemon_get_objects (daemon);
  for (l = objects; l != NULL; l = l->next)
    {
      StoragedObject *iter_object = STORAGED_OBJECT (l->data);
      StoragedBlock *iter_block;

      iter_block = storaged_object_peek_block (iter_object);
      if (iter_block == NULL)
        continue;

      if (g_strcmp0 (storaged_block_get_crypto_backing_device (iter_block), object_path) == 0)
        {
          ret = g_object_ref (iter_block);
          goto out;
        }
    }

 out:
  g_list_foreach (objects, (GFunc) g_object_unref, NULL);
  g_list_free (objects);
  return ret;
}

static gboolean
walk_block (StoragedDaemon *daemon,
            StoragedBlock *block,
            BlockWalker *walker,
            gpointer user_data,
            GError **error)
{
  StoragedObject *object;
  StoragedBlock *cleartext;
  gboolean is_leaf = TRUE;

  object = STORAGED_OBJECT (g_dbus_interface_get_object (G_DBUS_INTERFACE (block)));
  if (object != NULL)
    {
      // Recurse for all primary and extended partitions if this is a
      // partition table, or for all logical partitions if this is a
      // extended partition.

      StoragedPartitionTable *table;
      gboolean is_container;

      StoragedPartition *partition = storaged_object_peek_partition (object);
      if (partition && storaged_partition_get_is_container (partition))
        {
          table = peek_partition_table (daemon, partition);
          is_container = TRUE;
        }
      else
        {
          table = storaged_object_peek_partition_table (object);
          is_container = FALSE;
        }

      if (table)
        {
          GList *ps, *l;
          ps = get_partitions (daemon, table);
          for (l = ps; l != NULL; l = l->next)
            {
              StoragedPartition *p = STORAGED_PARTITION (l->data);
              StoragedObject *o = (StoragedObject *) g_dbus_interface_get_object (G_DBUS_INTERFACE (p));
              StoragedBlock *b = o ? storaged_object_peek_block (o) : NULL;
              if (b && !is_container == !storaged_partition_get_is_contained (p))
                {
                  is_leaf = FALSE;
                  if (!walk_block (daemon, b, walker, user_data, error))
                    {
                      g_list_free_full (ps, g_object_unref);
                      return FALSE;
                    }
                }
            }
          g_list_free_full (ps, g_object_unref);
        }
    }

  cleartext = get_cleartext_block (daemon, block);
  if (cleartext)
    {
      is_leaf = FALSE;
      if (!walk_block (daemon, cleartext, walker, user_data, error))
        {
          g_object_unref (cleartext);
          return FALSE;
        }
      g_object_unref (cleartext);
    }

  return walker (daemon, block, is_leaf, user_data, error);
}

gboolean
storaged_linux_remove_configuration (GVariant *config,
                                     GError **error)
{
  GVariantIter iter;
  const gchar *item_type;
  GVariant *details;

  storaged_debug ("Removing for teardown: %s", g_variant_print (config, FALSE));

  g_variant_iter_init (&iter, config);
  while (g_variant_iter_next (&iter, "(&s@a{sv})", &item_type, &details))
    {
      if (strcmp (item_type, "fstab") == 0)
        {
          if (!add_remove_fstab_entry (NULL, details, NULL, error))
            {
              g_variant_unref (details);
              return FALSE;
            }
        }
      else if (strcmp (item_type, "crypttab") == 0)
        {
          if (!add_remove_crypttab_entry (NULL, details, NULL, error))
            {
              g_variant_unref (details);
              return FALSE;
            }
        }
      g_variant_unref (details);
    }

  return TRUE;
}

struct TeardownData {
  GDBusMethodInvocation *invocation;
  GVariant              *options;
};

static gboolean
teardown_block_walker (StoragedDaemon *daemon,
                       StoragedBlock *block,
                       gboolean is_leaf,
                       gpointer user_data,
                       GError **error)
{
  struct TeardownData *data = user_data;
  StoragedObject *object = STORAGED_OBJECT (g_dbus_interface_get_object (G_DBUS_INTERFACE (block)));
  StoragedEncrypted *enc = storaged_object_peek_encrypted (object);

  if (enc)
    {
      StoragedBlock *cleartext = get_cleartext_block (daemon, block);
      if (cleartext)
        {
          /* The crypto backing device is unlocked and the cleartext
             device has been cleaned up.  Lock the backing device so
             that we can format or wipe it later.
          */
          if (enc && !storaged_linux_encrypted_lock (STORAGED_LINUX_ENCRYPTED (enc),
                                                     data->invocation,
                                                     data->options,
                                                     error))
            return FALSE;
        }
      else
        {
          /* The crypto backing device is locked and the cleartext
             device has not been cleaned up (since it doesn't exist).
             Remove its child configuration.
          */
          if (!storaged_linux_remove_configuration (storaged_encrypted_get_child_configuration (enc), error))
              return FALSE;
        }
    }

  return storaged_linux_remove_configuration (storaged_block_get_configuration (block), error);
}

gboolean
storaged_linux_block_teardown (StoragedBlock           *block,
                               GDBusMethodInvocation   *invocation,
                               GVariant                *options,
                               GError                 **error)
{
  StoragedObject *object = STORAGED_OBJECT (g_dbus_interface_get_object (G_DBUS_INTERFACE (block)));
  StoragedDaemon *daemon = storaged_linux_block_object_get_daemon (STORAGED_LINUX_BLOCK_OBJECT (object));
  struct TeardownData data;

  data.invocation = invocation;
  data.options = options;
  return walk_block (daemon, block, teardown_block_walker, &data, error);
}

/* ---------------------------------------------------------------------------------------------------- */

void
storaged_linux_block_handle_format (StoragedBlock           *block,
                                    GDBusMethodInvocation   *invocation,
                                    const gchar             *type,
                                    GVariant                *options,
                                    void                   (*complete)(gpointer user_data),
                                    gpointer                 complete_user_data)
{
  FormatWaitData *wait_data = NULL;
  StoragedObject *object;
  StoragedPartition *partition = NULL;
  StoragedPartitionTable *partition_table = NULL;
  StoragedObject *cleartext_object = NULL;
  StoragedBlock *cleartext_block = NULL;
  StoragedLinuxDevice *udev_cleartext_device = NULL;
  StoragedBlock *block_to_mkfs = NULL;
  StoragedObject *object_to_mkfs = NULL;
  StoragedDaemon *daemon;
  StoragedState *state;
  const gchar *action_id;
  const gchar *message;
  const FSInfo *fs_info;
  gchar *command = NULL;
  gchar *tmp;
  gchar *error_message;
  GError *error;
  int status;
  uid_t caller_uid;
  gid_t caller_gid;
  gboolean take_ownership = FALSE;
  gchar *encrypt_passphrase = NULL;
  gchar *erase_type = NULL;
  gchar *mapped_name = NULL;
  const gchar *label = NULL;
  gchar *escaped_device = NULL;
  gboolean was_partitioned = FALSE;
  StoragedInhibitCookie *inhibit_cookie = NULL;
  gboolean no_block = FALSE;
  gboolean update_partition_type = FALSE;
  const gchar *partition_type = NULL;
  GVariant *config_items = NULL;
  gboolean teardown_flag = FALSE;
#ifdef HAVE_LIBBLOCKDEV_PART
  BDPartTableType part_table_type = BD_PART_TABLE_UNDEF;
  gchar *device_name = NULL;
#endif /* HAVE_LIBBLOCKDEV_PART */

  /* We take a reference on BLOCK for the duration of this function
     since it might be swapped out for a different object while
     waiting.  See update_to_current_block_object.
  */
  g_object_ref (block);

  error = NULL;
  object = storaged_daemon_util_dup_object (block, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = storaged_linux_block_object_get_daemon (STORAGED_LINUX_BLOCK_OBJECT (object));
  state = storaged_daemon_get_state (daemon);
  command = NULL;
  error_message = NULL;

  g_variant_lookup (options, "take-ownership", "b", &take_ownership);
  g_variant_lookup (options, "encrypt.passphrase", "s", &encrypt_passphrase);
  g_variant_lookup (options, "erase", "s", &erase_type);
  g_variant_lookup (options, "no-block", "b", &no_block);
  g_variant_lookup (options, "update-partition-type", "b", &update_partition_type);
  g_variant_lookup (options, "config-items", "@a(sa{sv})", &config_items);
  g_variant_lookup (options, "tear-down", "b", &teardown_flag);

  partition = storaged_object_get_partition (object);
  if (partition != NULL)
    {
      StoragedObject *partition_table_object;

      /* Fail if partition contains a partition table (e.g. Fedora Hybrid ISO).
       * See: https://bugs.freedesktop.org/show_bug.cgi?id=76178
       */
      if (storaged_partition_get_offset (partition) == 0)
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 STORAGED_ERROR,
                                                 STORAGED_ERROR_NOT_SUPPORTED,
                                                 "This partition cannot be modified because it contains a partition table; please reinitialize layout of the whole device.");
          goto out;
        }

      partition_table_object = storaged_daemon_find_object (daemon, storaged_partition_get_table (partition));
      if (partition_table_object == NULL)
        {
          g_clear_object (&partition);
        }
      else
        {
          partition_table = storaged_object_get_partition_table (partition_table_object);
          g_clear_object (&partition_table_object);
        }
    }
  /* figure out partition type to set, if requested */
  if (update_partition_type && partition != NULL && partition_table != NULL)
    {
      partition_type = determine_partition_type_for_id (storaged_partition_table_get_type_ (partition_table),
                                                        encrypt_passphrase != NULL ? "crypto_LUKS" : type);
    }

  error = NULL;
  if (!storaged_daemon_util_get_caller_uid_sync (daemon, invocation, NULL /* GCancellable */, &caller_uid, &caller_gid, NULL, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      g_error_free (error);
      goto out;
    }

  if (g_strcmp0 (erase_type, "ata-secure-erase") == 0 ||
      g_strcmp0 (erase_type, "ata-secure-erase-enhanced") == 0)
    {
      /* Translators: Shown in authentication dialog when the user
       * requests erasing a hard disk using the SECURE ERASE UNIT
       * command.
       *
       * Do not translate $(drive), it's a placeholder and
       * will be replaced by the name of the drive/device in question
       */
      message = N_("Authentication is required to perform a secure erase of $(drive)");
      action_id = "org.storaged.Storaged.ata-secure-erase";
    }
  else
    {
      /* Translators: Shown in authentication dialog when formatting a
       * device. This includes both creating a filesystem or partition
       * table.
       *
       * Do not translate $(drive), it's a placeholder and will
       * be replaced by the name of the drive/device in question
       */
      message = N_("Authentication is required to format $(drive)");
      action_id = "org.storaged.Storaged.modify-device";
      if (!storaged_daemon_util_setup_by_user (daemon, object, caller_uid))
        {
          if (storaged_block_get_hint_system (block))
            {
              action_id = "org.storaged.Storaged.modify-device-system";
            }
          else if (!storaged_daemon_util_on_user_seat (daemon, object, caller_uid))
            {
              action_id = "org.storaged.Storaged.modify-device-other-seat";
            }
        }
    }

  /* TODO: Consider just accepting any @type and just running "mkfs -t <type>".
   *       There are some obvious security implications by doing this, though
   */
  fs_info = get_fs_info (type);
  if (fs_info == NULL || fs_info->command_create_fs == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                   STORAGED_ERROR,
                   STORAGED_ERROR_NOT_SUPPORTED,
                   "Creation of file system type %s is not supported",
                   type);
      goto out;
    }

  if (!storaged_daemon_util_check_authorization_sync (daemon,
                                                      object,
                                                      action_id,
                                                      options,
                                                      message,
                                                      invocation))
    goto out;

  if ((config_items != NULL || teardown_flag) &&
      !storaged_daemon_util_check_authorization_sync (daemon,
                                                      NULL,
                                                      "org.storaged.Storaged.modify-system-configuration",
                                                      options,
                                                      N_("Authentication is required to modify the system configuration"),
                                                      invocation))
    goto out;

  inhibit_cookie = storaged_daemon_util_inhibit_system_sync (N_("Formatting Device"));

  escaped_device = storaged_daemon_util_escape_and_quote (storaged_block_get_device (block));

  was_partitioned = (storaged_object_peek_partition_table (object) != NULL);

  if (teardown_flag)
    {
      if (!storaged_linux_block_teardown (block, invocation, options, &error))
        {
          g_dbus_method_invocation_take_error (invocation, error);
          goto out;
        }
    }

  /* First wipe the device... */
  if (!storaged_daemon_launch_spawned_job_sync (daemon,
                                              object,
                                              "format-erase", caller_uid,
                                              NULL, /* cancellable */
                                              0,    /* uid_t run_as_uid */
                                              0,    /* uid_t run_as_euid */
                                              &status,
                                              &error_message,
                                              NULL, /* input_string */
                                              "wipefs -a %s",
                                              escaped_device))
    {
      if (invocation != NULL)
        g_dbus_method_invocation_return_error (invocation,
                                               STORAGED_ERROR,
                                               STORAGED_ERROR_FAILED,
                                               "Error wiping device: %s",
                                               error_message);
      g_free (error_message);
      goto out;
    }
  /* ...then wait until this change has taken effect */
  wait_data = g_new0 (FormatWaitData, 1);
  wait_data->object_ptr = &object;
  wait_data->block_ptr = &block;
  wait_data->type = "empty";
  storaged_linux_block_object_trigger_uevent (STORAGED_LINUX_BLOCK_OBJECT (object));
  if (was_partitioned)
    storaged_linux_block_object_reread_partition_table (STORAGED_LINUX_BLOCK_OBJECT (object));
  if (storaged_daemon_wait_for_object_sync (daemon,
                                            wait_for_filesystem,
                                            wait_data,
                                            NULL,
                                            15,
                                            &error) == NULL)
    {
      g_prefix_error (&error, "Error synchronizing after initial wipe: ");
      if (invocation != NULL)
        g_dbus_method_invocation_take_error (invocation, error);
      else
        g_clear_error (&error);
      goto out;
    }

  /* complete early, if requested */
  if (no_block)
    {
      complete (complete_user_data);
      invocation = NULL;
    }

  /* Erase the device, if requested
   *
   * (but not if using encryption, we want to erase the cleartext device, see below)
   */
  if (erase_type != NULL && encrypt_passphrase == NULL)
    {
      if (!erase_device (block, object, daemon, caller_uid, erase_type, &error))
        {
          g_prefix_error (&error, "Error erasing device: ");
          if (invocation != NULL)
            g_dbus_method_invocation_take_error (invocation, error);
          else
            g_clear_error (&error);
          goto out;
        }
    }

  /* And now create the desired filesystem */
  wait_data->type = type;

  if (encrypt_passphrase != NULL)
    {
      /* Create it */
      if (!storaged_daemon_launch_spawned_job_sync (daemon,
                                                    object,
                                                    "format-mkfs", caller_uid,
                                                    NULL, /* cancellable */
                                                    0,    /* uid_t run_as_uid */
                                                    0,    /* uid_t run_as_euid */
                                                    &status,
                                                    &error_message,
                                                    encrypt_passphrase, /* input_string */
                                                    "cryptsetup luksFormat \"%s\"",
                                                    escaped_device))
        {
          if (invocation != NULL)
            g_dbus_method_invocation_return_error (invocation,
                                                   STORAGED_ERROR,
                                                   STORAGED_ERROR_FAILED,
                                                   "Error creating LUKS device: %s",
                                                   error_message);
          g_free (error_message);
          goto out;
        }

      /* Wait for the UUID to be set */
      if (storaged_daemon_wait_for_object_sync (daemon,
                                                wait_for_luks_uuid,
                                                wait_data,
                                                NULL,
                                                30,
                                                &error) == NULL)
        {
          g_prefix_error (&error, "Error waiting for LUKS UUID: ");
          if (invocation != NULL)
            g_dbus_method_invocation_take_error (invocation, error);
          else
            g_clear_error (&error);
          goto out;
        }

      /* Open it */
      mapped_name = make_block_luksname (block);
      g_assert (mapped_name != NULL);
      if (!storaged_daemon_launch_spawned_job_sync (daemon,
                                                    object,
                                                    "format-mkfs", caller_uid,
                                                    NULL, /* cancellable */
                                                    0,    /* uid_t run_as_uid */
                                                    0,    /* uid_t run_as_euid */
                                                    &status,
                                                    &error_message,
                                                    encrypt_passphrase, /* input_string */
                                                    "cryptsetup luksOpen \"%s\" %s",
                                                    escaped_device,
                                                    mapped_name))
        {
          if (invocation != NULL)
            g_dbus_method_invocation_return_error (invocation,
                                                   STORAGED_ERROR,
                                                   STORAGED_ERROR_FAILED,
                                                   "Error opening LUKS device: %s",
                                                   error_message);
          g_free (error_message);
          goto out;
        }

      /* Wait for it */
      cleartext_object = storaged_daemon_wait_for_object_sync (daemon,
                                                               wait_for_luks_cleartext,
                                                               wait_data,
                                                               NULL,
                                                               30,
                                                               &error);
      if (cleartext_object == NULL)
        {
          g_prefix_error (&error, "Error waiting for LUKS cleartext device: ");
          if (invocation != NULL)
            g_dbus_method_invocation_take_error (invocation, error);
          else
            g_clear_error (&error);
          goto out;
        }
      cleartext_block = storaged_object_get_block (cleartext_object);
      if (cleartext_block == NULL)
        {
          if (invocation != NULL)
            g_dbus_method_invocation_return_error (invocation, STORAGED_ERROR, STORAGED_ERROR_FAILED,
                                                   "LUKS cleartext device does not have block interface");
          goto out;
        }

      /* update the unlocked-luks file */
      udev_cleartext_device = storaged_linux_block_object_get_device (STORAGED_LINUX_BLOCK_OBJECT (cleartext_object));
      storaged_state_add_unlocked_luks (state,
                                        storaged_block_get_device_number (cleartext_block),
                                        storaged_block_get_device_number (block),
                                        g_udev_device_get_sysfs_attr (udev_cleartext_device->udev_device, "dm/uuid"),
                                        caller_uid);

      object_to_mkfs = cleartext_object;
      block_to_mkfs = cleartext_block;
    }
  else
    {
      object_to_mkfs = object;
      block_to_mkfs = block;
    }

  /* If using encryption, now erase the cleartext device (if requested) */
  if (encrypt_passphrase != NULL && erase_type != NULL)
    {
      if (!erase_device (block_to_mkfs, object_to_mkfs, daemon, caller_uid, erase_type, &error))
        {
          g_prefix_error (&error, "Error erasing cleartext device: ");
          if (invocation != NULL)
            g_dbus_method_invocation_take_error (invocation, error);
          else
            g_clear_error (&error);
          goto out;
        }
    }

  /* Set label, if needed */
  if (g_variant_lookup (options, "label", "&s", &label))
    {
      /* TODO: return an error if label is too long */
      if (strstr (fs_info->command_create_fs, "$LABEL") == NULL)
        {
          if (invocation != NULL)
            g_dbus_method_invocation_return_error (invocation,
                                                   STORAGED_ERROR,
                                                   STORAGED_ERROR_NOT_SUPPORTED,
                                                   "File system type %s does not support labels",
                                                   type);
          goto out;
        }
    }

#ifdef HAVE_LIBBLOCKDEV_PART
    if (g_strcmp0 (type, "dos") == 0)
      part_table_type = BD_PART_TABLE_MSDOS;
    else if (g_strcmp0 (type, "gpt") == 0)
      part_table_type = BD_PART_TABLE_GPT;
    if (part_table_type == BD_PART_TABLE_UNDEF)
#endif /* HAVE_LIBBLOCKDEV_PART */
      {
        /* Build and run mkfs shell command */
        tmp = subst_str_and_escape (fs_info->command_create_fs, "$DEVICE", storaged_block_get_device (block_to_mkfs));
        command = subst_str_and_escape (tmp, "$LABEL", label != NULL ? label : "");
        g_free (tmp);
        if (!storaged_daemon_launch_spawned_job_sync (daemon,
                                                      object_to_mkfs,
                                                      "format-mkfs", caller_uid,
                                                      NULL, /* cancellable */
                                                      0,    /* uid_t run_as_uid */
                                                      0,    /* uid_t run_as_euid */
                                                      &status,
                                                      &error_message,
                                                      NULL, /* input_string */
                                                      "%s", command))
          {
            if (invocation != NULL)
              g_dbus_method_invocation_return_error (invocation,
                                                     STORAGED_ERROR,
                                                     STORAGED_ERROR_FAILED,
                                                     "Error creating file system: %s",
                                                     error_message);
            g_free (error_message);
            goto out;
          }
      }
#ifdef HAVE_LIBBLOCKDEV_PART
    else
      {
        /* Create the partition table. */
        device_name = g_strdup (storaged_block_get_device (block));
        if (! bd_part_create_table (device_name, part_table_type, TRUE, &error))
          {
            g_dbus_method_invocation_take_error (invocation, error);
            goto out;
          }
      }
#endif /* HAVE_LIBBLOCKDEV_PART */

  /* The mkfs program may not generate all the uevents we need - so explicitly
   * trigger an event here
   */
  storaged_linux_block_object_trigger_uevent (STORAGED_LINUX_BLOCK_OBJECT (object_to_mkfs));
  wait_data->object_ptr = &object_to_mkfs;
  wait_data->block_ptr = &block_to_mkfs;
  if (storaged_daemon_wait_for_object_sync (daemon,
                                            wait_for_filesystem,
                                            wait_data,
                                            NULL,
                                            30,
                                            &error) == NULL)
    {
      g_prefix_error (&error,
                      "Error synchronizing after formatting with type `%s': ",
                      type);
      if (invocation != NULL)
        g_dbus_method_invocation_take_error (invocation, error);
      else
        g_clear_error (&error);
      goto out;
    }

  /* Change overship, if requested and supported */
  if (take_ownership && fs_info->supports_owners)
    {
      gchar tos_dir[256] = PACKAGE_LOCALSTATE_DIR "/run/storaged/block-format-tos-XXXXXX";

      if (mkdtemp (tos_dir) == NULL)
        {
          if (invocation != NULL)
            g_dbus_method_invocation_return_error (invocation, STORAGED_ERROR, STORAGED_ERROR_FAILED,
                                                   "Cannot create directory %s: %m", tos_dir);
          goto out;
        }
      if (mount (storaged_block_get_device (block_to_mkfs), tos_dir, type, 0, NULL) != 0)
        {
          if (invocation != NULL)
            g_dbus_method_invocation_return_error (invocation, STORAGED_ERROR, STORAGED_ERROR_FAILED,
                                                   "Cannot mount %s at %s: %m",
                                                   storaged_block_get_device (block_to_mkfs),
                                                   tos_dir);
          if (rmdir (tos_dir) != 0)
            {
              storaged_warning ("Error removing directory %s: %m", tos_dir);
            }
          goto out;
        }
      if (chown (tos_dir, caller_uid, caller_gid) != 0)
        {
          if (invocation != NULL)
            g_dbus_method_invocation_return_error (invocation, STORAGED_ERROR, STORAGED_ERROR_FAILED,
                                                   "Cannot chown %s to uid=%u and gid=%u: %m", tos_dir, caller_uid, caller_gid);
          if (umount (tos_dir) != 0)
            {
              storaged_warning ("Error unmounting directory %s: %m", tos_dir);
              goto out;
            }
          if (rmdir (tos_dir) != 0)
            {
              storaged_warning ("Error removing directory %s: %m", tos_dir);
            }
          goto out;
        }
      if (chmod (tos_dir, 0700) != 0)
        {
          if (invocation != NULL)
            g_dbus_method_invocation_return_error (invocation, STORAGED_ERROR, STORAGED_ERROR_FAILED,
                                                   "Cannot chmod %s to mode 0700: %m", tos_dir);
          if (umount (tos_dir) != 0)
            {
              storaged_warning ("Error unmounting directory %s: %m", tos_dir);
              goto out;
            }
          if (rmdir (tos_dir) != 0)
            {
              storaged_warning ("Error removing directory %s: %m", tos_dir);
            }
          goto out;
        }

      if (umount (tos_dir) != 0)
        {
          if (invocation != NULL)
            g_dbus_method_invocation_return_error (invocation, STORAGED_ERROR, STORAGED_ERROR_FAILED,
                                                   "Cannot unmount %s: %m", tos_dir);
          if (rmdir (tos_dir) != 0)
            {
              storaged_warning ("Error removing directory %s: %m", tos_dir);
            }
          goto out;
        }

      if (rmdir (tos_dir) != 0)
        {
          if (invocation != NULL)
            g_dbus_method_invocation_return_error (invocation, STORAGED_ERROR, STORAGED_ERROR_FAILED,
                                                   "Cannot remove directory %s: %m", tos_dir);
          goto out;
        }
    }

  /* Set the partition type, if requested */
  if (partition_type != NULL && partition != NULL)
    {
      if (g_strcmp0 (storaged_partition_get_type_ (partition), partition_type) != 0)
        {
          if (!storaged_linux_partition_set_type_sync (STORAGED_LINUX_PARTITION (partition),
                                                     partition_type,
                                                     caller_uid,
                                                     NULL, /* cancellable */
                                                     &error))
            {
              g_prefix_error (&error, "Error setting partition type after formatting: ");
              g_dbus_method_invocation_take_error (invocation, error);
              goto out;
            }
        }
    }

  /* Add configuration items */

  if (config_items)
    {
      GVariantIter iter;
      const gchar *item_type;
      GVariant *details;

      g_variant_iter_init (&iter, config_items);
      while (g_variant_iter_next (&iter, "(&s@a{sv})", &item_type, &details))
        {
          if (strcmp (item_type, "fstab") == 0)
            {
              if (!add_remove_fstab_entry (block_to_mkfs, NULL, details, &error))
                {
                  g_dbus_method_invocation_take_error (invocation, error);
                  goto out;
                }
            }
          else if (strcmp (item_type, "crypttab") == 0)
            {
              if (!add_remove_crypttab_entry (block, NULL, details, &error))
                {
                  g_dbus_method_invocation_take_error (invocation, error);
                  goto out;
                }
            }
          g_variant_unref (details);
        }
    }

  if (invocation != NULL)
    complete (complete_user_data);

 out:
  storaged_daemon_util_uninhibit_system_sync (inhibit_cookie);
  g_free (escaped_device);
  g_free (mapped_name);
  g_free (command);
  if (config_items)
    g_variant_unref (config_items);
  g_free (erase_type);
  g_free (encrypt_passphrase);
  g_clear_object (&cleartext_object);
  g_clear_object (&cleartext_block);
  g_clear_object (&udev_cleartext_device);
  g_free (wait_data);
  g_clear_object (&partition_table);
  g_clear_object (&partition);
  g_clear_object (&object);
#ifdef HAVE_LIBBLOCKDEV_PART
  g_free (device_name);
#endif /* HAVE_LIBBLOCKDEV_PART */
  g_object_unref (block);
}

struct FormatCompleteData {
  StoragedBlock *block;
  GDBusMethodInvocation *invocation;
};

static void
handle_format_complete (gpointer user_data)
{
  struct FormatCompleteData *data = user_data;
  storaged_block_complete_format (data->block, data->invocation);
}

static gboolean
handle_format (StoragedBlock           *block,
               GDBusMethodInvocation   *invocation,
               const gchar             *type,
               GVariant                *options)
{
  struct FormatCompleteData data;
  data.block = block;
  data.invocation = invocation;
  storaged_linux_block_handle_format (block, invocation, type, options,
                                      handle_format_complete, &data);

  return TRUE; /* returning true means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
handle_open_for_backup (StoragedBlock           *block,
                        GDBusMethodInvocation   *invocation,
                        GUnixFDList             *fd_list,
                        GVariant                *options)
{
  StoragedObject *object;
  StoragedDaemon *daemon;
  const gchar *action_id;
  const gchar *device;
  GUnixFDList *out_fd_list = NULL;
  GError *error;
  gint fd;

  error = NULL;
  object = storaged_daemon_util_dup_object (block, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = storaged_linux_block_object_get_daemon (STORAGED_LINUX_BLOCK_OBJECT (object));

  action_id = "org.storaged.Storaged.open-device";
  if (storaged_block_get_hint_system (block))
    action_id = "org.storaged.Storaged.open-device-system";

  if (!storaged_daemon_util_check_authorization_sync (daemon,
                                                      object,
                                                      action_id,
                                                      options,
                                                      /* Translators: Shown in authentication dialog when creating a
                                                       * disk image file.
                                                       *
                                                       * Do not translate $(drive), it's a placeholder and will
                                                       * be replaced by the name of the drive/device in question
                                                       */
                                                      N_("Authentication is required to open $(drive) for reading"),
                                                      invocation))
    goto out;


  device = storaged_block_get_device (STORAGED_BLOCK (block));

  fd = open (device, O_RDONLY | O_CLOEXEC | O_EXCL);
  if (fd == -1)
    {
      g_dbus_method_invocation_return_error (invocation, STORAGED_ERROR, STORAGED_ERROR_FAILED,
                                             "Error opening %s: %m", device);
      goto out;
    }

  out_fd_list = g_unix_fd_list_new_from_array (&fd, 1);
  storaged_block_complete_open_for_backup (block, invocation, out_fd_list, g_variant_new_handle (0));

 out:
  g_clear_object (&out_fd_list);
  g_clear_object (&object);
  return TRUE; /* returning true means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
handle_open_for_restore (StoragedBlock           *block,
                         GDBusMethodInvocation   *invocation,
                         GUnixFDList             *fd_list,
                         GVariant                *options)
{
  StoragedObject *object;
  StoragedDaemon *daemon;
  const gchar *action_id;
  const gchar *device;
  GUnixFDList *out_fd_list = NULL;
  GError *error;
  gint fd;

  error = NULL;
  object = storaged_daemon_util_dup_object (block, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = storaged_linux_block_object_get_daemon (STORAGED_LINUX_BLOCK_OBJECT (object));

  action_id = "org.storaged.Storaged.open-device";
  if (storaged_block_get_hint_system (block))
    action_id = "org.storaged.Storaged.open-device-system";

  if (!storaged_daemon_util_check_authorization_sync (daemon,
                                                      object,
                                                      action_id,
                                                      options,
                                                      /* Translators: Shown in authentication dialog when restoring
                                                       * from a disk image file.
                                                       *
                                                       * Do not translate $(drive), it's a placeholder and will
                                                       * be replaced by the name of the drive/device in question
                                                       */
                                                      N_("Authentication is required to open $(drive) for writing"),
                                                      invocation))
    goto out;


  device = storaged_block_get_device (STORAGED_BLOCK (block));

  fd = open (device, O_WRONLY | O_SYNC | O_CLOEXEC | O_EXCL);
  if (fd == -1)
    {
      g_dbus_method_invocation_return_error (invocation, STORAGED_ERROR, STORAGED_ERROR_FAILED,
                                             "Error opening %s: %m", device);
      goto out;
    }

  out_fd_list = g_unix_fd_list_new_from_array (&fd, 1);
  storaged_block_complete_open_for_restore (block, invocation, out_fd_list, g_variant_new_handle (0));

 out:
  g_clear_object (&out_fd_list);
  g_clear_object (&object);
  return TRUE; /* returning true means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
handle_open_for_benchmark (StoragedBlock           *block,
                           GDBusMethodInvocation   *invocation,
                           GUnixFDList             *fd_list,
                           GVariant                *options)
{
  StoragedObject *object;
  StoragedDaemon *daemon;
  const gchar *action_id;
  const gchar *device;
  GUnixFDList *out_fd_list = NULL;
  gboolean opt_writable = FALSE;
  GError *error;
  gint fd;
  gint open_flags;

  error = NULL;
  object = storaged_daemon_util_dup_object (block, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = storaged_linux_block_object_get_daemon (STORAGED_LINUX_BLOCK_OBJECT (object));

  action_id = "org.storaged.Storaged.open-device";
  if (storaged_block_get_hint_system (block))
    action_id = "org.storaged.Storaged.open-device-system";

  if (!storaged_daemon_util_check_authorization_sync (daemon,
                                                    object,
                                                    action_id,
                                                    options,
                                                    /* Translators: Shown in authentication dialog when an application
                                                     * wants to benchmark a device.
                                                     *
                                                     * Do not translate $(drive), it's a placeholder and will
                                                     * be replaced by the name of the drive/device in question
                                                     */
                                                    N_("Authentication is required to open $(drive) for benchmarking"),
                                                    invocation))
    goto out;

  g_variant_lookup (options, "writable", "b", &opt_writable);

  if (opt_writable)
    open_flags = O_RDWR  | O_EXCL;
  else
    open_flags = O_RDONLY;

  open_flags |= O_DIRECT | O_SYNC | O_CLOEXEC;

  device = storaged_block_get_device (STORAGED_BLOCK (block));

  fd = open (device, open_flags);
  if (fd == -1)
    {
      g_dbus_method_invocation_return_error (invocation, STORAGED_ERROR, STORAGED_ERROR_FAILED,
                                             "Error opening %s: %m", device);
      goto out;
    }

  out_fd_list = g_unix_fd_list_new_from_array (&fd, 1);
  storaged_block_complete_open_for_benchmark (block, invocation, out_fd_list, g_variant_new_handle (0));

 out:
  g_clear_object (&out_fd_list);
  g_clear_object (&object);
  return TRUE; /* returning true means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
handle_rescan (StoragedBlock           *block,
               GDBusMethodInvocation   *invocation,
               GVariant                *options)
{
  StoragedObject *object = NULL;
  StoragedLinuxDevice *device = NULL;
  StoragedDaemon *daemon;
  const gchar *action_id;
  const gchar *message;
  GError *error = NULL;

  object = storaged_daemon_util_dup_object (block, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = storaged_linux_block_object_get_daemon (STORAGED_LINUX_BLOCK_OBJECT (object));

  /* Translators: Shown in authentication dialog when an application
   * wants to rescan a device.
   *
   * Do not translate $(drive), it's a placeholder and will
   * be replaced by the name of the drive/device in question
   */
  message = N_("Authentication is required to rescan $(drive)");
  action_id = "org.storaged.Storaged.rescan";

  if (!storaged_daemon_util_check_authorization_sync (daemon,
                                                    object,
                                                    action_id,
                                                    options,
                                                    message,
                                                    invocation))
    goto out;

  device = storaged_linux_block_object_get_device (STORAGED_LINUX_BLOCK_OBJECT (object));

  storaged_linux_block_object_trigger_uevent (STORAGED_LINUX_BLOCK_OBJECT (object));
  if (g_strcmp0 (g_udev_device_get_devtype (device->udev_device), "disk") == 0)
    storaged_linux_block_object_reread_partition_table (STORAGED_LINUX_BLOCK_OBJECT (object));

  storaged_block_complete_rescan (block, invocation);

 out:
  g_clear_object (&device);
  g_clear_object (&object);
  return TRUE; /* returning true means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

static void
block_iface_init (StoragedBlockIface *iface)
{
  iface->handle_get_secret_configuration  = handle_get_secret_configuration;
  iface->handle_add_configuration_item    = handle_add_configuration_item;
  iface->handle_remove_configuration_item = handle_remove_configuration_item;
  iface->handle_update_configuration_item = handle_update_configuration_item;
  iface->handle_format                    = handle_format;
  iface->handle_open_for_backup           = handle_open_for_backup;
  iface->handle_open_for_restore          = handle_open_for_restore;
  iface->handle_open_for_benchmark        = handle_open_for_benchmark;
  iface->handle_rescan                    = handle_rescan;
}
