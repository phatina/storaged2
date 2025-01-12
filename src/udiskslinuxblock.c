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

#include <libmount/libmount.h>
#include <blkid/blkid.h>

#include <blockdev/part.h>
#include <blockdev/fs.h>
#include <blockdev/crypto.h>
#include <blockdev/swap.h>

#include "udiskslogging.h"
#include "udiskslinuxblock.h"
#include "udiskslinuxblockobject.h"
#include "udiskslinuxdriveobject.h"
#include "udisksdaemon.h"
#include "udisksstate.h"
#include "udisksprivate.h"
#include "udisksconfigmanager.h"
#include "udisksdaemonutil.h"
#include "udiskslinuxprovider.h"
#include "udisksfstabentry.h"
#include "udiskscrypttabmonitor.h"
#include "udiskscrypttabentry.h"
#include "udisksdaemonutil.h"
#include "udisksbasejob.h"
#include "udiskssimplejob.h"
#include "udiskslinuxdriveata.h"
#include "udiskslinuxmdraidobject.h"
#include "udiskslinuxdevice.h"
#include "udiskslinuxpartition.h"
#include "udiskslinuxencrypted.h"
#include "udiskslinuxencryptedhelpers.h"
#include "udiskslinuxpartitiontable.h"
#include "udiskslinuxfilesystemhelpers.h"
#include "udisksutabmonitor.h"
#include "udisksutabentry.h"

/**
 * SECTION:udiskslinuxblock
 * @title: UDisksLinuxBlock
 * @short_description: Linux implementation of #UDisksBlock
 *
 * This type provides an implementation of the #UDisksBlock
 * interface on Linux.
 */

typedef struct _UDisksLinuxBlockClass   UDisksLinuxBlockClass;

/**
 * UDisksLinuxBlock:
 *
 * The #UDisksLinuxBlock structure contains only private data and should
 * only be accessed using the provided API.
 */
struct _UDisksLinuxBlock
{
  UDisksBlockSkeleton parent_instance;

  /* only allow single cryptsetup call at once */
  GMutex encrypted_lock;
};

struct _UDisksLinuxBlockClass
{
  UDisksBlockSkeletonClass parent_class;
};

static void block_iface_init (UDisksBlockIface *iface);

G_DEFINE_TYPE_WITH_CODE (UDisksLinuxBlock, udisks_linux_block, UDISKS_TYPE_BLOCK_SKELETON,
                         G_IMPLEMENT_INTERFACE (UDISKS_TYPE_BLOCK, block_iface_init));

/* ---------------------------------------------------------------------------------------------------- */

static void
udisks_linux_block_init (UDisksLinuxBlock *block)
{
  g_mutex_init (&(block->encrypted_lock));
  g_dbus_interface_skeleton_set_flags (G_DBUS_INTERFACE_SKELETON (block),
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);
}

static void
udisks_linux_block_finalize (GObject *object)
{
  UDisksLinuxBlock *block = UDISKS_LINUX_BLOCK (object);

  g_mutex_clear (&(block->encrypted_lock));

  if (G_OBJECT_CLASS (udisks_linux_block_parent_class)->finalize != NULL)
    G_OBJECT_CLASS (udisks_linux_block_parent_class)->finalize (object);
}

static void
udisks_linux_block_class_init (UDisksLinuxBlockClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = udisks_linux_block_finalize;
}

/**
 * udisks_linux_block_new:
 *
 * Creates a new #UDisksLinuxBlock instance.
 *
 * Returns: A new #UDisksLinuxBlock. Free with g_object_unref().
 */
UDisksBlock *
udisks_linux_block_new (void)
{
  return UDISKS_BLOCK (g_object_new (UDISKS_TYPE_LINUX_BLOCK,
                                     NULL));
}

/* ---------------------------------------------------------------------------------------------------- */

static gchar *
get_sysfs_attr_from_path (const gchar *sysfs_path,
                const gchar *attr)
{
  gchar *filename = NULL;
  gchar *value = NULL;
  gboolean ret = FALSE;
  GError *error = NULL;

  filename = g_strconcat (sysfs_path,
                          "/",
                          attr,
                          NULL);


  ret = g_file_get_contents (filename,
                             &value,
                             NULL,
                             &error);
  if (!ret)
    {
      udisks_debug ("Failed to read sysfs attribute %s: %s", attr, error->message);
      g_clear_error (&error);
    }

  g_free (filename);
  return value;
}

/* ---------------------------------------------------------------------------------------------------- */

static UDisksLinuxBlockObject *
find_block_device_by_sysfs_path (GDBusObjectManagerServer *object_manager,
                                 const gchar              *sysfs_path)
{
  UDisksLinuxBlockObject *ret;
  GList *objects;
  GList *l;

  ret = NULL;

  objects = g_dbus_object_manager_get_objects (G_DBUS_OBJECT_MANAGER (object_manager));
  for (l = objects; l != NULL; l = l->next)
    {
      GDBusObjectSkeleton *object = G_DBUS_OBJECT_SKELETON (l->data);
      UDisksLinuxDevice *device;

      if (!UDISKS_IS_LINUX_BLOCK_OBJECT (object))
        continue;

      device = udisks_linux_block_object_get_device (UDISKS_LINUX_BLOCK_OBJECT (object));
      if (g_strcmp0 (sysfs_path, g_udev_device_get_sysfs_path (device->udev_device)) == 0)
        {
          ret = g_object_ref (UDISKS_LINUX_BLOCK_OBJECT (object));
          g_object_unref (device);
          goto out;
        }
      g_object_unref (device);
    }

 out:
  g_list_free_full (objects, g_object_unref);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static gchar *
find_drive (GDBusObjectManagerServer  *object_manager,
            GUdevDevice               *block_device,
            UDisksDrive              **out_drive)
{
  GUdevDevice *whole_disk_block_device;
  const gchar *whole_disk_block_device_sysfs_path;
  gchar **nvme_ctrls = NULL;
  gchar *ret;
  GList *objects = NULL;
  GList *l;

  ret = NULL;

  if (g_strcmp0 (g_udev_device_get_devtype (block_device), "disk") == 0)
    whole_disk_block_device = g_object_ref (block_device);
  else
    whole_disk_block_device = g_udev_device_get_parent_with_subsystem (block_device, "block", "disk");
  if (whole_disk_block_device == NULL)
    goto out;
  whole_disk_block_device_sysfs_path = g_udev_device_get_sysfs_path (whole_disk_block_device);

  /* check for NVMe */
  if (g_strcmp0 (g_udev_device_get_subsystem (whole_disk_block_device), "block") == 0)
    {
      GUdevDevice *parent_device;

      parent_device = g_udev_device_get_parent (whole_disk_block_device);
      if (parent_device && g_udev_device_has_sysfs_attr (parent_device, "subsysnqn") &&
          g_str_has_prefix (g_udev_device_get_subsystem (parent_device), "nvme"))
        {
          gchar *subsysnqn_p;

          /* 'parent_device' is a nvme subsystem,
           * 'whole_disk_block_device' is a namespace
           */
          subsysnqn_p = g_strdup (g_udev_device_get_sysfs_attr (parent_device, "subsysnqn"));
          if (subsysnqn_p)
            g_strchomp (subsysnqn_p);

          nvme_ctrls = bd_nvme_find_ctrls_for_ns (whole_disk_block_device_sysfs_path, subsysnqn_p,
                                                  NULL, NULL, NULL);
          g_free (subsysnqn_p);
        }
      g_clear_object (&parent_device);
    }

  objects = g_dbus_object_manager_get_objects (G_DBUS_OBJECT_MANAGER (object_manager));
  for (l = objects; l != NULL; l = l->next)
    {
      GDBusObjectSkeleton *object = G_DBUS_OBJECT_SKELETON (l->data);
      GList *drive_devices;
      GList *j;

      if (!UDISKS_IS_LINUX_DRIVE_OBJECT (object))
        continue;

      drive_devices = udisks_linux_drive_object_get_devices (UDISKS_LINUX_DRIVE_OBJECT (object));
      for (j = drive_devices; j != NULL; j = j->next)
        {
          UDisksLinuxDevice *drive_device = UDISKS_LINUX_DEVICE (j->data);
          const gchar *drive_sysfs_path;

          drive_sysfs_path = g_udev_device_get_sysfs_path (drive_device->udev_device);
          if (g_strcmp0 (whole_disk_block_device_sysfs_path, drive_sysfs_path) == 0 ||
              (nvme_ctrls && g_strv_contains ((const gchar * const *) nvme_ctrls, drive_sysfs_path)))
            {
              if (out_drive != NULL)
                *out_drive = udisks_object_get_drive (UDISKS_OBJECT (object));
              ret = g_strdup (g_dbus_object_get_object_path (G_DBUS_OBJECT (object)));
              g_list_free_full (drive_devices, g_object_unref);
              /* FIXME: NVMe namespace may be provided by multiple controllers within
               *  a NVMe subsystem, however the org.freedesktop.UDisks2.Block.Drive
               *  property may only contain single object path.
               */
              goto out;
            }
        }
      g_list_free_full (drive_devices, g_object_unref);
    }

 out:
  g_list_free_full (objects, g_object_unref);
  g_clear_object (&whole_disk_block_device);
  if (nvme_ctrls)
    g_strfreev (nvme_ctrls);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static UDisksLinuxMDRaidObject *
find_mdraid (GDBusObjectManagerServer  *object_manager,
             const gchar               *md_uuid)
{
  UDisksLinuxMDRaidObject *ret = NULL;
  GList *objects = NULL, *l;

  objects = g_dbus_object_manager_get_objects (G_DBUS_OBJECT_MANAGER (object_manager));
  for (l = objects; l != NULL; l = l->next)
    {
      GDBusObjectSkeleton *object = G_DBUS_OBJECT_SKELETON (l->data);
      if (UDISKS_IS_LINUX_MDRAID_OBJECT (object))
        {
          UDisksMDRaid *mdraid = udisks_object_get_mdraid (UDISKS_OBJECT (object));
          if (mdraid != NULL)
            {
              if (g_strcmp0 (udisks_mdraid_get_uuid (mdraid), md_uuid) == 0)
                {
                  ret = UDISKS_LINUX_MDRAID_OBJECT (g_object_ref (object));
                  g_object_unref (mdraid);
                  goto out;
                }
              g_object_unref (mdraid);
            }
        }
    }

 out:
  g_list_free_full (objects, g_object_unref);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
update_mdraid (UDisksLinuxBlock         *block,
               UDisksLinuxDevice        *device,
               UDisksDrive              *drive,
               GDBusObjectManagerServer *object_manager)
{
  UDisksBlock *iface = UDISKS_BLOCK (block);
  const gchar *uuid;
  const gchar *objpath_mdraid = "/";
  const gchar *objpath_mdraid_member = "/";
  UDisksLinuxMDRaidObject *object = NULL;

  uuid = g_udev_device_get_property (device->udev_device, "UDISKS_MD_UUID");
  if (uuid != NULL && strlen (uuid) > 0)
    {
      object = find_mdraid (object_manager, uuid);
      if (object != NULL)
        {
          objpath_mdraid = g_dbus_object_get_object_path (G_DBUS_OBJECT (object));
          g_clear_object (&object);
        }
    }

  uuid = g_udev_device_get_property (device->udev_device, "UDISKS_MD_MEMBER_UUID");
  if (uuid != NULL && strlen (uuid) > 0)
    {
      object = find_mdraid (object_manager, uuid);
      if (object != NULL)
        {
          objpath_mdraid_member = g_dbus_object_get_object_path (G_DBUS_OBJECT (object));
          g_clear_object (&object);
        }
    }

  udisks_block_set_mdraid (iface, objpath_mdraid);
  udisks_block_set_mdraid_member (iface, objpath_mdraid_member);
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_linux_block_matches_id:
 * @block: A #UDisksLinuxBlock.
 * @device_path: A device path string.
 *
 * Compares block device identifiers and returns %TRUE if match is found. The @device_path
 * argument may be a device file or a common KEY=VALUE identifier as used e.g. in /etc/fstab
 * or /etc/crypttab.
 *
 * The @device_path should be a demangled (unquoted/unencoded/unescaped) string.
 *
 * Returns: %TRUE when identifiers do match, %FALSE otherwise.
 */
gboolean
udisks_linux_block_matches_id (UDisksLinuxBlock *block,
                               const gchar      *device_path)
{
  gchar *tag_type = NULL;
  gchar *tag_val = NULL;
  gboolean ret = FALSE;

  g_return_val_if_fail (device_path != NULL && strlen (device_path) > 0, FALSE);

  if (blkid_parse_tag_string (device_path, &tag_type, &tag_val) != 0 || !tag_type || !tag_val)
    {
      /* the "NAME=value" string parsing failed, treat it as a device file */
      const gchar *const *symlinks;

      g_free (tag_type);
      g_free (tag_val);

      if (g_strcmp0 (device_path, udisks_block_get_device (UDISKS_BLOCK (block))) == 0)
        return TRUE;

      symlinks = udisks_block_get_symlinks (UDISKS_BLOCK (block));
      if (symlinks && g_strv_contains (symlinks, device_path))
        return TRUE;

      return FALSE;
    }

  ret = g_str_equal (tag_type, "UUID") && g_strcmp0 (tag_val, udisks_block_get_id_uuid (UDISKS_BLOCK (block))) == 0;
  ret = ret || (g_str_equal (tag_type, "LABEL") && g_strcmp0 (tag_val, udisks_block_get_id_label (UDISKS_BLOCK (block))) == 0);

  if (!ret && (g_str_equal (tag_type, "PARTUUID") || g_str_equal (tag_type, "PARTLABEL")))
    {
      UDisksObject *object;
      UDisksPartition *partition;

      object = udisks_daemon_util_dup_object (block, NULL);
      if (object != NULL)
        {
          partition = udisks_object_peek_partition (object);
          if (partition != NULL)
            {
              ret = (g_str_equal (tag_type, "PARTUUID")  && g_strcmp0 (tag_val, udisks_partition_get_uuid (partition)) == 0) ||
                    (g_str_equal (tag_type, "PARTLABEL") && g_strcmp0 (tag_val, udisks_partition_get_name (partition)) == 0);
            }
          g_object_unref (object);
        }
    }

  g_free (tag_type);
  g_free (tag_val);

  return ret;
}

static GList *
find_fstab_entries (UDisksDaemon     *daemon,
                    UDisksLinuxBlock *block,
                    const gchar      *needle)
{
  struct libmnt_table *table;
  struct libmnt_iter* iter;
  struct libmnt_fs *fs = NULL;
  GList *ret = NULL;

  table = mnt_new_table ();
  if (mnt_table_parse_fstab (table, NULL) < 0)
    {
      mnt_free_table (table);
      return NULL;
    }

  iter = mnt_new_iter (MNT_ITER_FORWARD);
  while (mnt_table_next_fs (table, iter, &fs) == 0)
    {
      UDisksFstabEntry *entry;

      if (block != NULL)
        {
          if (! udisks_linux_block_matches_id (block, mnt_fs_get_source (fs)))
            continue;
        }
      else if (needle != NULL)
        {
          const char *opts;

          opts = mnt_fs_get_options (fs);
          if (! opts || g_strstr_len (opts, -1, needle) == NULL)
            continue;
        }

      entry = _udisks_fstab_entry_new_from_mnt_fs (fs);
      ret = g_list_prepend (ret, entry);
    }
  mnt_free_iter (iter);
  mnt_free_table (table);

  return g_list_reverse (ret);
}

static GList *
find_crypttab_entries_for_device (UDisksLinuxBlock *block,
                                  UDisksDaemon     *daemon)
{
  GList *entries;
  GList *l;
  GList *ret;

  ret = NULL;

  /* if this is too slow, we could add lookup methods to UDisksCrypttabMonitor... */
  entries = udisks_crypttab_monitor_get_entries (udisks_daemon_get_crypttab_monitor (daemon));
  for (l = entries; l != NULL; l = l->next)
    {
      UDisksCrypttabEntry *entry = UDISKS_CRYPTTAB_ENTRY (l->data);
      const gchar *device;

      device = udisks_crypttab_entry_get_device (entry);
      if (udisks_linux_block_matches_id (block, device))
        ret = g_list_prepend (ret, g_object_ref (entry));
    }

  g_list_free_full (entries, g_object_unref);
  return ret;
}

static GList *
find_crypttab_entries_for_needle (gchar        *needle,
                                  UDisksDaemon *daemon)
{
  GList *entries;
  GList *l;
  GList *ret;

  ret = NULL;

  entries = udisks_crypttab_monitor_get_entries (udisks_daemon_get_crypttab_monitor (daemon));
  for (l = entries; l != NULL; l = l->next)
    {
      UDisksCrypttabEntry *entry = UDISKS_CRYPTTAB_ENTRY (l->data);
      const gchar *opts = NULL;

      opts = udisks_crypttab_entry_get_options (entry);
      if (opts && strstr(opts, needle))
        ret = g_list_prepend (ret, g_object_ref (entry));
    }

  g_list_free_full (entries, g_object_unref);
  return ret;
}

static GList *
find_utab_entries_for_device (UDisksLinuxBlock *block,
                              UDisksDaemon     *daemon)
{
  GSList *entries, *l;
  GList *ret;

  ret = NULL;

  /* if this is too slow, we could add lookup methods to UDisksUtabMonitor... */
  entries = udisks_utab_monitor_get_entries (udisks_daemon_get_utab_monitor (daemon));
  for (l = entries; l != NULL; l = l->next)
    {
      UDisksUtabEntry *entry = UDISKS_UTAB_ENTRY (l->data);
      const gchar *source = udisks_utab_entry_get_source (entry);

      if (!g_str_has_prefix (source, "/dev"))
        continue;

      if (udisks_linux_block_matches_id (block, source))
        ret = g_list_prepend (ret, g_object_ref (entry));
    }

  g_slist_free_full (entries, g_object_unref);
  return ret;
}

static void
add_fstab_entry (GVariantBuilder  *builder,
                 UDisksFstabEntry *entry)
{
  GVariantBuilder dict_builder;
  g_variant_builder_init (&dict_builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&dict_builder, "{sv}", "fsname",
                         g_variant_new_bytestring (udisks_fstab_entry_get_fsname (entry)));
  g_variant_builder_add (&dict_builder, "{sv}", "dir",
                         g_variant_new_bytestring (udisks_fstab_entry_get_dir (entry)));
  g_variant_builder_add (&dict_builder, "{sv}", "type",
                         g_variant_new_bytestring (udisks_fstab_entry_get_fstype (entry)));
  g_variant_builder_add (&dict_builder, "{sv}", "opts",
                         g_variant_new_bytestring (udisks_fstab_entry_get_opts (entry)));
  g_variant_builder_add (&dict_builder, "{sv}", "freq",
                         g_variant_new_int32 (udisks_fstab_entry_get_freq (entry)));
  g_variant_builder_add (&dict_builder, "{sv}", "passno",
                         g_variant_new_int32 (udisks_fstab_entry_get_passno (entry)));
  g_variant_builder_add (builder,
                         "(sa{sv})",
                         "fstab", &dict_builder);
}

static gboolean
add_crypttab_entry (GVariantBuilder       *builder,
                    UDisksCrypttabEntry   *entry,
                    gboolean               include_secrets,
                    GError               **error)
{
  GVariantBuilder dict_builder;
  const gchar *passphrase_path;
  const gchar *options;
  gchar *passphrase_contents;
  gsize passphrase_contents_length;

  passphrase_path = udisks_crypttab_entry_get_passphrase_path (entry);
  if (passphrase_path == NULL || g_strcmp0 (passphrase_path, "none") == 0 || g_strcmp0 (passphrase_path, "-") == 0)
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

  options = udisks_crypttab_entry_get_options (entry);
  if (options == NULL)
    options = "";

  g_variant_builder_init (&dict_builder, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add (&dict_builder, "{sv}", "name",
                         g_variant_new_bytestring (udisks_crypttab_entry_get_name (entry)));
  g_variant_builder_add (&dict_builder, "{sv}", "device",
                         g_variant_new_bytestring (udisks_crypttab_entry_get_device (entry)));
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
calculate_configuration (UDisksLinuxBlock  *block,
                         UDisksDaemon      *daemon,
                         gboolean           include_secrets,
                         GError           **error)
{
  GList *entries;
  GList *l;
  GVariantBuilder builder;
  GVariant *ret;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  ret = NULL;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(sa{sv})"));
  /* First the /etc/fstab entries */
  entries = find_fstab_entries (daemon, block, NULL);
  for (l = entries; l != NULL; l = l->next)
    add_fstab_entry (&builder, UDISKS_FSTAB_ENTRY (l->data));
  g_list_free_full (entries, g_object_unref);

  /* Then the /etc/crypttab entries (currently only supported for LUKS) */
  if (udisks_linux_block_is_luks (UDISKS_BLOCK (block)))
    {
      entries = find_crypttab_entries_for_device (block, daemon);
      for (l = entries; l != NULL; l = l->next)
        {
          if (!add_crypttab_entry (&builder, UDISKS_CRYPTTAB_ENTRY (l->data), include_secrets, error))
            {
              g_variant_builder_clear (&builder);
              g_list_free_full (entries, g_object_unref);
              goto out;
            }
        }
      g_list_free_full (entries, g_object_unref);
    }

  ret = g_variant_builder_end (&builder);

 out:
  return ret;
}

static gchar **
calculate_userspace_mount_options (UDisksLinuxBlock *block,
                                   UDisksDaemon     *daemon)
{
  GList *entries, *l;
  GPtrArray *ret;

  ret = g_ptr_array_new ();

  /* Get the /run/mounts/utab entries */
  entries = find_utab_entries_for_device (block, daemon);
  for (l = entries; l != NULL; l = l->next) {
    UDisksUtabEntry *entry = UDISKS_UTAB_ENTRY (l->data);
    const gchar * const *opts = udisks_utab_entry_get_opts (entry);
    for (gint i = 0; opts[i] != NULL; ++i)
      g_ptr_array_add (ret, g_strdup (opts[i]));
  }
  g_ptr_array_add (ret, NULL);
  g_list_free_full (entries, g_object_unref);

  return (gchar **) g_ptr_array_free (ret, FALSE);
}

static void
update_configuration (UDisksLinuxBlock  *block,
                      UDisksDaemon      *daemon)
{
  GVariant *configuration;
  GError *error;

  error = NULL;
  configuration = calculate_configuration (block, daemon, FALSE, &error);
  if (configuration == NULL)
    {
      udisks_warning ("Error loading configuration: %s (%s, %d)",
                      error->message, g_quark_to_string (error->domain), error->code);
      g_clear_error (&error);
      configuration = g_variant_new ("a(sa{sv})", NULL);
    }
  udisks_block_set_configuration (UDISKS_BLOCK (block), configuration);
  g_dbus_interface_skeleton_flush (G_DBUS_INTERFACE_SKELETON (block));
}

static void
update_userspace_mount_options (UDisksLinuxBlock *block,
                                UDisksDaemon     *daemon)
{
  gchar **opts;

  opts = calculate_userspace_mount_options (block, daemon);
  udisks_block_set_userspace_mount_options (UDISKS_BLOCK (block), (const gchar * const*) opts);

  g_strfreev (opts);
}

/* ---------------------------------------------------------------------------------------------------- */

/* returns a floating GVariant */
static GVariant *
find_configurations (gchar         *needle,
                     UDisksDaemon  *daemon,
                     gboolean       include_secrets,
                     GError       **error)
{
  GList *entries;
  GList *l;
  GVariantBuilder builder;
  GVariant *ret;

  udisks_debug ("Looking for %s", needle);

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  ret = NULL;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(sa{sv})"));
  /* First the /etc/fstab entries */
  entries = find_fstab_entries (daemon, NULL, needle);
  for (l = entries; l != NULL; l = l->next)
    add_fstab_entry (&builder, UDISKS_FSTAB_ENTRY (l->data));
  g_list_free_full (entries, g_object_unref);

  /* Then the /etc/crypttab entries */
  entries = find_crypttab_entries_for_needle (needle, daemon);
  for (l = entries; l != NULL; l = l->next)
    {
      if (!add_crypttab_entry (&builder, UDISKS_CRYPTTAB_ENTRY (l->data), include_secrets, error))
        {
          g_variant_builder_clear (&builder);
          g_list_free_full (entries, g_object_unref);
          goto out;
        }
    }
  g_list_free_full (entries, g_object_unref);

  ret = g_variant_builder_end (&builder);

 out:
  return ret;
}

GVariant *
udisks_linux_find_child_configuration (UDisksDaemon *daemon,
                                       const gchar  *uuid)
{
  GError *error = NULL;
  gchar *needle = g_strdup_printf ("x-parent=%s", uuid);
  GVariant *res = find_configurations (needle, daemon, FALSE, &error);
  if (res == NULL)
    {
      udisks_warning ("Error loading configuration: %s (%s, %d)",
                      error->message, g_quark_to_string (error->domain), error->code);
      g_clear_error (&error);
      res = g_variant_new ("a(sa{sv})", NULL);
    }
  g_free (needle);
  return res;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
update_hints (UDisksDaemon      *daemon,
              UDisksLinuxBlock  *block,
              UDisksLinuxDevice *device,
              UDisksDrive       *drive)
{
  UDisksBlock *iface = UDISKS_BLOCK (block);
  gboolean hint_partitionable;
  gboolean hint_system;
  gboolean hint_ignore;
  gboolean hint_auto;
  const gchar *hint_name;
  const gchar *hint_icon_name;
  const gchar *hint_symbolic_icon_name;
  const gchar *device_file;
  GList *fstab_entries;
  GList *l;

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
      connection_bus = udisks_drive_get_connection_bus (drive);
      removable = udisks_drive_get_media_removable (drive);
      if (removable ||
          g_strcmp0 (connection_bus, "usb") == 0 ||
          g_strcmp0 (connection_bus, "ieee1394") == 0 ||
          g_str_has_prefix (device_file, "/dev/msblk") ||
          g_str_has_prefix (device_file, "/dev/mspblk"))
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

  /* Check fstab entries */
  fstab_entries = find_fstab_entries (daemon, block, NULL);
  for (l = fstab_entries; l != NULL; l = l->next)
    {
      UDisksFstabEntry *entry = UDISKS_FSTAB_ENTRY (l->data);
      /* Honour the sysadmin-specified 'noauto' mount option */
      if (udisks_fstab_entry_has_opt (entry, "+noauto"))
        hint_auto = FALSE;
    }
  g_list_free_full (fstab_entries, g_object_unref);

  /* TODO: set ignore to TRUE for physical paths belonging to a drive with multiple paths */

  /* Override from UDISKS_* udev properties */
  if (g_udev_device_has_property (device->udev_device, "UDISKS_SYSTEM"))
    hint_system = g_udev_device_get_property_as_boolean (device->udev_device, "UDISKS_SYSTEM");

  if (g_udev_device_has_property (device->udev_device, "UDISKS_IGNORE"))
    hint_ignore = g_udev_device_get_property_as_boolean (device->udev_device, "UDISKS_IGNORE");

  if (g_udev_device_has_property (device->udev_device, "UDISKS_AUTO"))
    hint_auto = g_udev_device_get_property_as_boolean (device->udev_device, "UDISKS_AUTO");

  if (g_udev_device_has_property (device->udev_device, "UDISKS_NAME"))
    hint_name = g_udev_device_get_property (device->udev_device, "UDISKS_NAME");

  if (g_udev_device_has_property (device->udev_device, "UDISKS_ICON_NAME"))
    hint_icon_name = g_udev_device_get_property (device->udev_device, "UDISKS_ICON_NAME");

  if (g_udev_device_has_property (device->udev_device, "UDISKS_SYMBOLIC_ICON_NAME"))
    hint_symbolic_icon_name = g_udev_device_get_property (device->udev_device, "UDISKS_SYMBOLIC_ICON_NAME");

  /* ... and scene! */
  udisks_block_set_hint_partitionable (iface, hint_partitionable);
  udisks_block_set_hint_system (iface, hint_system);
  udisks_block_set_hint_ignore (iface, hint_ignore);
  udisks_block_set_hint_auto (iface, hint_auto);
  udisks_block_set_hint_name (iface, hint_name);
  udisks_block_set_hint_icon_name (iface, hint_icon_name);
  udisks_block_set_hint_symbolic_icon_name (iface, hint_symbolic_icon_name);
}

/* ---------------------------------------------------------------------------------------------------- */

static gchar *
get_slave_sysfs_path (const gchar *sysfs_path)
{
  gchar *ret = NULL;
  gchar **slaves;
  slaves = udisks_daemon_util_resolve_links (sysfs_path, "slaves");
  if (slaves != NULL && g_strv_length (slaves) == 1)
    {
      ret = g_strdup (slaves[0]);
    }

  g_strfreev (slaves);
  return ret;
}

/**
 * udisks_linux_block_update:
 * @block: A #UDisksLinuxBlock.
 * @object: The enclosing #UDisksLinuxBlockObject instance.
 *
 * Updates the interface.
 */
void
udisks_linux_block_update (UDisksLinuxBlock       *block,
                           UDisksLinuxBlockObject *object)
{
  UDisksBlock *iface = UDISKS_BLOCK (block);
  UDisksDaemon *daemon;
  GDBusObjectManagerServer *object_manager;
  UDisksLinuxDevice *device;
  GUdevDeviceNumber dev;
  gchar *drive_object_path;
  UDisksDrive *drive;
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
  gboolean seems_encrypted = FALSE;
  guint n;
  GError *error = NULL;

  drive = NULL;

  device = udisks_linux_block_object_get_device (object);
  if (device == NULL)
    goto out;

  daemon = udisks_linux_block_object_get_daemon (object);
  object_manager = udisks_daemon_get_object_manager (daemon);

  dev = g_udev_device_get_device_number (device->udev_device);
  device_file = g_udev_device_get_device_file (device->udev_device);
  symlinks = g_udev_device_get_device_file_symlinks (device->udev_device);

  udisks_block_set_device (iface, device_file);
  udisks_block_set_symlinks (iface, symlinks);
  udisks_block_set_device_number (iface, dev);

  size = udisks_daemon_util_block_get_size (device->udev_device,
                                            &media_available,
                                            &media_change_detected);
  udisks_block_set_size (iface, size);

  read_only = g_udev_device_get_sysfs_attr_as_boolean (device->udev_device, "ro");
  if (!read_only && g_str_has_prefix (g_udev_device_get_name (device->udev_device), "sr"))
    read_only = TRUE;
  udisks_block_set_read_only (iface, read_only);

  /* dm-crypt
   *
   * TODO: this might not be the best way to determine if the device-mapper device
   *       is a dm-crypt device.. but unfortunately device-mapper keeps all this stuff
   *       in user-space and wants you to use libdevmapper to obtain it...
   */
  udisks_block_set_crypto_backing_device (iface, "/");
  if (g_str_has_prefix (g_udev_device_get_name (device->udev_device), "dm-"))
    {
      gchar *slave_sysfs_path = g_strdup (g_udev_device_get_sysfs_path (device->udev_device));
      gchar *old_sysfs_path = NULL;

      while (slave_sysfs_path)
        {
          gchar *dm_uuid = get_sysfs_attr_from_path (slave_sysfs_path,"dm/uuid");

          old_sysfs_path = slave_sysfs_path;
          slave_sysfs_path = get_slave_sysfs_path (old_sysfs_path);
          g_free (old_sysfs_path);

          if (dm_uuid != NULL)
            {
              if (g_str_has_prefix (dm_uuid, "CRYPT-LUKS") || g_str_has_prefix (dm_uuid, "CRYPT-BITLK") || g_str_has_prefix (dm_uuid, "CRYPT-TCRYPT"))
                {
                  /* found crypted device, break here */
                  g_free (dm_uuid);
                  break;
                }
            }
        }
      if (slave_sysfs_path)
        {
          UDisksLinuxBlockObject *slave_object;
          slave_object = find_block_device_by_sysfs_path (object_manager, slave_sysfs_path);
          if (slave_object != NULL)
            {
              UDisksEncrypted *enc;

              udisks_block_set_crypto_backing_device (iface,
                                                      g_dbus_object_get_object_path (G_DBUS_OBJECT (slave_object)));

              /* also set the CleartextDevice property for the parent device */
              enc = udisks_object_peek_encrypted (UDISKS_OBJECT (slave_object));
              if (enc != NULL)
                {
                  udisks_encrypted_set_cleartext_device (UDISKS_ENCRYPTED (enc),
                                                         g_dbus_object_get_object_path (G_DBUS_OBJECT (object)));
                }

              g_object_unref (slave_object);
              g_free (slave_sysfs_path);
            }
        }
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
      for (n = 0; symlinks != NULL && symlinks[n] != NULL; n++)
        {
          if (g_str_has_prefix (symlinks[n], "/dev/md/"))
            {
              preferred_device_file = symlinks[n];
              break;
            }
        }
    }
  /* fallback to the device name */
  if (preferred_device_file == NULL)
    preferred_device_file = g_udev_device_get_device_file (device->udev_device);
  udisks_block_set_preferred_device (iface, preferred_device_file);

  /* Determine the drive this block device belongs to
   *
   * TODO: if this is slow we could have a cache or ensure that we
   * only do this once or something else
   */
  drive_object_path = find_drive (object_manager, device->udev_device, &drive);
  if (drive_object_path != NULL)
    {
      udisks_block_set_drive (iface, drive_object_path);
      g_free (drive_object_path);
    }
  else
    {
      udisks_block_set_drive (iface, "/");
    }

  if (drive != NULL)
    media_removable = udisks_drive_get_media_removable (drive);

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
      udisks_block_set_id (iface, id);
      g_free (id);
    }
  else
    {
      udisks_block_set_id (iface, NULL);
    }

  if (udisks_daemon_get_enable_tcrypt (daemon))
    {
      seems_encrypted = bd_crypto_device_seems_encrypted (device_file, &error);
      if (error != NULL)
        {
          udisks_warning ("Error determining whether device '%s' seems to be encrypted: %s (%s, %d)",
                          device_file, error->message, g_quark_to_string (error->domain), error->code);
          g_clear_error (&error);
        }
    }

  if (seems_encrypted)
    {
      udisks_block_set_id_usage (iface, "crypto");
      udisks_block_set_id_type (iface, "crypto_unknown");
    }
  else
    {
      udisks_block_set_id_usage (iface, g_udev_device_get_property (device->udev_device, "ID_FS_USAGE"));
      udisks_block_set_id_type (iface, g_udev_device_get_property (device->udev_device, "ID_FS_TYPE"));
    }

  s = udisks_decode_udev_string (g_udev_device_get_property (device->udev_device, "ID_FS_VERSION"), NULL);
  udisks_block_set_id_version (iface, s);
  g_free (s);
  s = udisks_decode_udev_string (g_udev_device_get_property (device->udev_device, "ID_FS_LABEL_ENC"),
                                 g_udev_device_get_property (device->udev_device, "ID_FS_LABEL"));
  udisks_block_set_id_label (iface, s);
  g_free (s);
  s = udisks_decode_udev_string (g_udev_device_get_property (device->udev_device, "ID_FS_UUID_ENC"),
                                 g_udev_device_get_property (device->udev_device, "ID_FS_UUID"));
  if ((!s || strlen (s) == 0) && udisks_linux_block_is_bitlk (iface))
    {
      BDCryptoBITLKInfo *bitlk_info;

      /* Attempt to retrieve bitlk uuid from the on-disk header */
      bitlk_info = bd_crypto_bitlk_info (device_file, &error);
      if (bitlk_info)
        {
          g_free (s);
          s = g_strdup (bitlk_info->uuid);
          bd_crypto_bitlk_info_free (bitlk_info);
        }
      else
        {
          g_warning ("Crypto bitlk container detected on %s but failed to parse the header: %s",
                     device_file, error->message);
          g_error_free (error);
        }
    }
  udisks_block_set_id_uuid (iface, s);
  g_free (s);

  update_hints (daemon, block, device, drive);
  update_configuration (block, daemon);
  update_userspace_mount_options (block, daemon);
  update_mdraid (block, device, drive, object_manager);

 out:
  g_dbus_interface_skeleton_flush (G_DBUS_INTERFACE_SKELETON (block));
  if (device != NULL)
    g_object_unref (device);
  if (drive != NULL)
    g_object_unref (drive);
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
handle_get_secret_configuration (UDisksBlock           *_block,
                                 GDBusMethodInvocation *invocation,
                                 GVariant              *options)
{
  UDisksLinuxBlock *block = UDISKS_LINUX_BLOCK (_block);
  UDisksLinuxBlockObject *object;
  UDisksDaemon *daemon;
  GVariant *configuration;
  GError *error;

  error = NULL;
  object = udisks_daemon_util_dup_object (block, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = udisks_linux_block_object_get_daemon (object);

  error = NULL;
  configuration = calculate_configuration (block, daemon, TRUE, &error);
  if (configuration == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  if (!udisks_daemon_util_check_authorization_sync (daemon,
                                                    NULL,
                                                    "org.freedesktop.udisks2.read-system-configuration-secrets",
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

  udisks_block_complete_get_secret_configuration (UDISKS_BLOCK (block),
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
              udisks_warning ("unescape_fstab: trailing \\");
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
make_block_fsname (UDisksBlock *block)
{
  const gchar *uuid = udisks_block_get_id_uuid (block);

  if (uuid && *uuid)
    return g_strdup_printf ("UUID=%s", uuid);
  else
    return g_strdup (udisks_block_get_device (block));
}

static gchar *
track_parents (UDisksBlock *block, const gchar *options)
{
  UDisksObject *object = UDISKS_OBJECT (g_dbus_interface_get_object (G_DBUS_INTERFACE (block)));
  UDisksDaemon *daemon = udisks_linux_block_object_get_daemon (UDISKS_LINUX_BLOCK_OBJECT (object));

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
      gchar *parent_path = udisks_daemon_get_parent_for_tracking (daemon, path, &uuid);

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
add_remove_fstab_entry (UDisksBlock *block,
                        GVariant    *remove,
                        GVariant    *add,
                        GError     **error)
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
                       UDISKS_ERROR,
                       UDISKS_ERROR_FAILED,
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
                       UDISKS_ERROR,
                       UDISKS_ERROR_FAILED,
                       "Missing dir, type, opts, freq or passno parameter in entry to add");
          goto out;
        }

      if (strlen (mntent_add.mnt_opts) == 0)
        {
          g_set_error (error,
                       UDISKS_ERROR,
                       UDISKS_ERROR_FAILED,
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
    {
      if (g_error_matches (*error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        {
          contents = g_strdup ("");
          g_clear_error (error);
        }
      else
        goto out;
    }

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
                   UDISKS_ERROR,
                   UDISKS_ERROR_FAILED,
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

  if (!udisks_daemon_util_file_set_contents ("/etc/fstab",
                                             str->str,
                                             -1,
                                             0644, /* mode to use if non-existent */
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
make_block_luksname (UDisksBlock *block, GError **error)
{
  BDCryptoLUKSInfo *info = NULL;
  gchar *ret = NULL;

  udisks_linux_block_encrypted_lock (block);
  info = bd_crypto_luks_info (udisks_block_get_device (block), error);
  udisks_linux_block_encrypted_unlock (block);

  if (info)
    {
      if (info->label && g_strcmp0 (info->label, "") != 0)
        ret = g_strdup (info->label);
      else
        ret = g_strdup_printf ("luks-%s", info->uuid);
      bd_crypto_luks_info_free (info);

      return ret;
    }
  else
    return NULL;
}

static gboolean
add_remove_crypttab_entry (UDisksBlock *block,
                           GVariant    *remove,
                           GVariant    *add,
                           GError     **error)
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
                       UDISKS_ERROR,
                       UDISKS_ERROR_FAILED,
                       "Missing name, device, passphrase-path, options or parameter in entry to remove");
          goto out;
        }
    }

  if (add != NULL)
    {
      if (!g_variant_lookup (add, "name", "^&ay", &add_name))
        {
          const gchar *uuid = udisks_block_get_id_uuid (block);
          if (uuid == NULL || *uuid == '\0')
            {
              g_set_error (error,
                           UDISKS_ERROR,
                           UDISKS_ERROR_FAILED,
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
                       UDISKS_ERROR,
                       UDISKS_ERROR_FAILED,
                       "Missing options or passphrase-contents parameter in entry to add");
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
                       UDISKS_ERROR,
                       UDISKS_ERROR_FAILED,
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
    {
      if (g_error_matches (*error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        {
          contents = g_strdup ("");
          g_clear_error (error);
        }
      else
        goto out;
    }

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
              if (num_parsed < 3 || g_strcmp0 (parsed_passphrase_path, "none") == 0 || g_strcmp0 (parsed_passphrase_path, "-") == 0)
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
                                       UDISKS_ERROR,
                                       UDISKS_ERROR_FAILED,
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
                   UDISKS_ERROR,
                   UDISKS_ERROR_FAILED,
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
                               UDISKS_ERROR,
                               UDISKS_ERROR_FAILED,
                               "Crypttab passphrase file can only be created in the /etc/luks-keys directory");
                  goto out;
                }
              /* ensure the directory exists */
              if (g_mkdir_with_parents ("/etc/luks-keys", 0700) != 0)
                {
                  g_set_error (error,
                               UDISKS_ERROR,
                               UDISKS_ERROR_FAILED,
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
                               UDISKS_ERROR,
                               UDISKS_ERROR_FAILED,
                               "Refusing to overwrite existing file %s",
                               filename);
                  g_free (filename);
                  goto out;
            }

          if (!udisks_daemon_util_file_set_contents (filename,
                                                     add_passphrase_contents,
                                                     -1,
                                                     0600, /* mode to use if non-existent */
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

  if (!udisks_daemon_util_file_set_contents ("/etc/crypttab",
                                             str->str,
                                             -1,
                                             0600, /* mode to use if non-existent */
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

static void
update_block_fstab (UDisksDaemon           *daemon,
                    UDisksLinuxBlock       *block,
                    UDisksLinuxBlockObject *object)
{
  UDisksLinuxDevice *device;
  gchar *drive_object_path;
  UDisksDrive *drive = NULL;

  update_configuration (block, daemon);

  /* hints take fstab records in the calculation */
  device = udisks_linux_block_object_get_device (object);
  drive_object_path = find_drive (udisks_daemon_get_object_manager (daemon), device->udev_device, &drive);
  update_hints (daemon, block, device, drive);
  g_free (drive_object_path);
  g_clear_object (&device);
  g_clear_object (&drive);
}

static gboolean
handle_add_configuration_item (UDisksBlock           *_block,
                               GDBusMethodInvocation *invocation,
                               GVariant              *item,
                               GVariant              *options)
{
  UDisksLinuxBlock *block = UDISKS_LINUX_BLOCK (_block);
  UDisksLinuxBlockObject *object;
  UDisksDaemon *daemon;
  const gchar *type;
  GVariant *details = NULL;
  GError *error;

  error = NULL;
  object = udisks_daemon_util_dup_object (block, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = udisks_linux_block_object_get_daemon (object);

  g_variant_get (item, "(&s@a{sv})", &type, &details);
  if (g_strcmp0 (type, "fstab") == 0)
    {
      if (!udisks_daemon_util_check_authorization_sync (daemon,
                                                        NULL,
                                                        "org.freedesktop.udisks2.modify-system-configuration",
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
      update_block_fstab (daemon, block, object);
      udisks_block_complete_add_configuration_item (UDISKS_BLOCK (block), invocation);
    }
  else if (g_strcmp0 (type, "crypttab") == 0)
    {
      if (!udisks_daemon_util_check_authorization_sync (daemon,
                                                        NULL,
                                                        "org.freedesktop.udisks2.modify-system-configuration",
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
      update_configuration (block, daemon);
      udisks_block_complete_add_configuration_item (UDISKS_BLOCK (block), invocation);
    }
  else
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
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
handle_remove_configuration_item (UDisksBlock           *_block,
                                  GDBusMethodInvocation *invocation,
                                  GVariant              *item,
                                  GVariant              *options)
{
  UDisksLinuxBlock *block = UDISKS_LINUX_BLOCK (_block);
  UDisksLinuxBlockObject *object;
  UDisksDaemon *daemon;
  const gchar *type;
  GVariant *details = NULL;
  GError *error;

  error = NULL;
  object = udisks_daemon_util_dup_object (block, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = udisks_linux_block_object_get_daemon (object);

  g_variant_get (item, "(&s@a{sv})", &type, &details);
  if (g_strcmp0 (type, "fstab") == 0)
    {
      if (!udisks_daemon_util_check_authorization_sync (daemon,
                                                        NULL,
                                                        "org.freedesktop.udisks2.modify-system-configuration",
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
      update_block_fstab (daemon, block, object);
      udisks_block_complete_remove_configuration_item (UDISKS_BLOCK (block), invocation);
    }
  else if (g_strcmp0 (type, "crypttab") == 0)
    {
      if (!udisks_daemon_util_check_authorization_sync (daemon,
                                                        NULL,
                                                        "org.freedesktop.udisks2.modify-system-configuration",
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
      update_configuration (block, daemon);
      udisks_block_complete_remove_configuration_item (UDISKS_BLOCK (block), invocation);
    }
  else
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
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
handle_update_configuration_item (UDisksBlock           *_block,
                                  GDBusMethodInvocation *invocation,
                                  GVariant              *old_item,
                                  GVariant              *new_item,
                                  GVariant              *options)
{
  UDisksLinuxBlock *block = UDISKS_LINUX_BLOCK (_block);
  UDisksLinuxBlockObject *object;
  UDisksDaemon *daemon;
  const gchar *old_type;
  const gchar *new_type;
  GVariant *old_details = NULL;
  GVariant *new_details = NULL;
  GError *error;

  error = NULL;
  object = udisks_daemon_util_dup_object (block, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = udisks_linux_block_object_get_daemon (object);

  g_variant_get (old_item, "(&s@a{sv})", &old_type, &old_details);
  g_variant_get (new_item, "(&s@a{sv})", &new_type, &new_details);
  if (g_strcmp0 (old_type, new_type) != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "old and new item are not of the same type");
      goto out;
    }

  if (g_strcmp0 (old_type, "fstab") == 0)
    {
      if (!udisks_daemon_util_check_authorization_sync (daemon,
                                                        NULL,
                                                        "org.freedesktop.udisks2.modify-system-configuration",
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
      update_block_fstab (daemon, block, object);
      udisks_block_complete_update_configuration_item (UDISKS_BLOCK (block), invocation);
    }
  else if (g_strcmp0 (old_type, "crypttab") == 0)
    {
      if (!udisks_daemon_util_check_authorization_sync (daemon,
                                                        NULL,
                                                        "org.freedesktop.udisks2.modify-system-configuration",
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
      update_configuration (block, daemon);
      udisks_block_complete_update_configuration_item (UDISKS_BLOCK (block), invocation);
    }
  else
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
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

typedef struct
{
  UDisksObject *object;
  const gchar  *type;
} FormatWaitData;

/* ---------------------------------------------------------------------------------------------------- */

static UDisksObject *
wait_for_filesystem (UDisksDaemon *daemon,
                     gpointer      user_data)
{
  FormatWaitData *data = user_data;
  UDisksObject *ret = NULL;
  UDisksBlock *block = NULL;
  UDisksPartitionTable *partition_table = NULL;
  UDisksFilesystem *filesystem = NULL;
  gchar *id_type = NULL;
  gchar *partition_table_type = NULL;

  block = udisks_object_get_block (data->object);
  if (block == NULL)
    goto out;

  partition_table = udisks_object_get_partition_table (data->object);
  filesystem = udisks_object_get_filesystem (data->object);

  id_type = udisks_block_dup_id_type (block);

  if (g_strcmp0 (data->type, "empty") == 0)
    {
      if ((id_type == NULL || g_strcmp0 (id_type, "") == 0 ||
           g_strcmp0 (id_type, "crypto_unknown") == 0) && partition_table == NULL)
        {
          ret = g_object_ref (data->object);
          goto out;
        }
    }

  if (g_strcmp0 (id_type, data->type) == 0)
    {
      /* check that we should expect a filesystem and wait for corresponding interface
       * to be exported on the object */
      if (g_strcmp0 (data->type, "empty") == 0 ||
          ! udisks_linux_block_object_contains_filesystem (data->object) ||
          filesystem != NULL)
        {
          ret = g_object_ref (data->object);
          goto out;
        }
    }

  if (partition_table != NULL)
    {
      partition_table_type = udisks_partition_table_dup_type_ (partition_table);
      if (g_strcmp0 (partition_table_type, data->type) == 0)
        {
          ret = g_object_ref (data->object);
          goto out;
        }
    }

 out:
  g_free (partition_table_type);
  g_free (id_type);
  g_clear_object (&partition_table);
  g_clear_object (&filesystem);
  g_clear_object (&block);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static UDisksObject *
wait_for_luks_uuid (UDisksDaemon *daemon,
                    gpointer      user_data)
{
  FormatWaitData *data = user_data;
  UDisksObject *ret = NULL;
  UDisksBlock *block = NULL;

  block = udisks_object_get_block (data->object);
  if (block == NULL)
    goto out;

  if (g_strcmp0 (udisks_block_get_id_type (block), "crypto_LUKS") != 0)
    goto out;

  ret = g_object_ref (data->object);

 out:
  g_clear_object (&block);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static UDisksObject *
wait_for_luks_cleartext (UDisksDaemon *daemon,
                         gpointer      user_data)
{
  FormatWaitData *data = user_data;
  UDisksObject *ret = NULL;
  GList *objects, *l;

  objects = udisks_daemon_get_objects (daemon);
  for (l = objects; l != NULL; l = l->next)
    {
      UDisksObject *object = UDISKS_OBJECT (l->data);
      UDisksBlock *block = NULL;

      block = udisks_object_get_block (object);
      if (block != NULL)
        {
          if (g_strcmp0 (udisks_block_get_crypto_backing_device (block),
                         g_dbus_object_get_object_path (G_DBUS_OBJECT (data->object))) == 0)
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
erase_ata_device (UDisksBlock   *block,
                  UDisksObject  *object,
                  UDisksDaemon  *daemon,
                  uid_t          caller_uid,
                  gboolean       enhanced,
                  GError       **error)
{
  gboolean ret = FALSE;
  UDisksObject *drive_object = NULL;
  UDisksLinuxBlockObject *block_object = NULL;
  UDisksDriveAta *ata = NULL;

  drive_object = udisks_daemon_find_object (daemon, udisks_block_get_drive (block));
  if (drive_object == NULL)
    {
      g_set_error (error, UDISKS_ERROR, UDISKS_ERROR_FAILED, "No drive object");
      goto out;
    }
  ata = udisks_object_get_drive_ata (drive_object);
  if (ata == NULL)
    {
      g_set_error (error, UDISKS_ERROR, UDISKS_ERROR_FAILED, "Drive is not an ATA drive");
      goto out;
    }

  /* Reverse check to ensure we're erasing whole block device and not a partition */
  block_object = udisks_linux_drive_object_get_block (UDISKS_LINUX_DRIVE_OBJECT (drive_object), FALSE /* get_hw */);
  if (block_object == NULL)
    {
      g_set_error (error, UDISKS_ERROR, UDISKS_ERROR_FAILED, "Couldn't find a block device for the drive to erase");
      goto out;
    }
  if (g_strcmp0 (g_dbus_object_get_object_path (G_DBUS_OBJECT (object)),
                 g_dbus_object_get_object_path (G_DBUS_OBJECT (block_object))) != 0)
    {
      g_set_error (error, UDISKS_ERROR, UDISKS_ERROR_FAILED, "ATA secure erase needs to be performed on a whole block device");
      goto out;
    }

  ret = udisks_linux_drive_ata_secure_erase_sync (UDISKS_LINUX_DRIVE_ATA (ata),
                                                  caller_uid,
                                                  enhanced,
                                                  error);

 out:
  g_clear_object (&ata);
  g_clear_object (&drive_object);
  g_clear_object (&block_object);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

#define ERASE_SIZE (1 * 1024*1024)

static gboolean
erase_device (UDisksBlock   *block,
              UDisksObject  *object,
              UDisksDaemon  *daemon,
              uid_t          caller_uid,
              const gchar   *erase_type,
              GError       **error)
{
  gboolean ret = FALSE;
  const gchar *device_file = NULL;
  UDisksBaseJob *job = NULL;
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
      g_set_error (&local_error, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                   "Unknown or unsupported erase type `%s'",
                   erase_type);
      goto out;
    }

  device_file = udisks_block_get_device (block);
  fd = open (device_file, O_WRONLY | O_SYNC | O_EXCL);
  if (fd == -1)
    {
      g_set_error (&local_error, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                   "Error opening device %s for erase: %m", device_file);
      goto out;
    }

  job = udisks_daemon_launch_simple_job (daemon, object, "format-erase", caller_uid, NULL);
  udisks_base_job_set_auto_estimate (UDISKS_BASE_JOB (job), TRUE);
  udisks_job_set_progress_valid (UDISKS_JOB (job), TRUE);

  if (ioctl (fd, BLKGETSIZE64, &size) != 0)
    {
      g_set_error (&local_error, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                   "Error doing BLKGETSIZE64 iotctl on %s: %m", device_file);
      goto out;
    }

  udisks_job_set_bytes (UDISKS_JOB (job), size);

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
          g_set_error (&local_error, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                       "Error writing %d bytes to %s: %m",
                       (gint) to_write, device_file);
          goto out;
        }
      pos += num_written;

      if (g_cancellable_is_cancelled (udisks_base_job_get_cancellable (job)))
        {
          g_set_error (&local_error, UDISKS_ERROR, UDISKS_ERROR_CANCELLED,
                       "Job was canceled");
          goto out;
        }

      /* only emit D-Bus signal at most once a second */
      now = g_get_monotonic_time ();
      if (now - time_of_last_signal > G_USEC_PER_SEC)
        {
          /* TODO: estimation etc. */
          udisks_job_set_progress (UDISKS_JOB (job), ((gdouble) pos) / size);
          time_of_last_signal = now;
        }
    }

  ret = TRUE;

 out:
  if (job != NULL)
    {
      if (local_error != NULL)
        udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), FALSE, local_error->message);
      else
        udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), TRUE, "");
    }
  if (local_error != NULL)
    g_propagate_error (error, local_error);
  g_free (buf);
  if (fd != -1)
    close (fd);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

typedef gboolean BlockWalker (UDisksDaemon *daemon,
                              UDisksBlock  *block,
                              gboolean      is_leaf,
                              gpointer      user_data,
                              GError      **error);

static UDisksPartitionTable *
peek_partition_table (UDisksDaemon    *daemon,
                      UDisksPartition *partition)
{
  UDisksObject *object;
  UDisksPartitionTable *pt;

  object = udisks_daemon_find_object (daemon, udisks_partition_get_table (partition));
  pt = object ? udisks_object_peek_partition_table (object) : NULL;

  g_clear_object (&object);
  return pt;
}

static UDisksBlock *
get_cleartext_block (UDisksDaemon  *daemon,
                     UDisksBlock   *block)
{
  UDisksBlock *ret = NULL;
  GDBusObject *object;
  const gchar *object_path;
  GList *objects = NULL;
  GList *l;

  object = g_dbus_interface_get_object (G_DBUS_INTERFACE (block));
  if (object == NULL)
    goto out;

  object_path = g_dbus_object_get_object_path (object);
  objects = udisks_daemon_get_objects (daemon);
  for (l = objects; l != NULL; l = l->next)
    {
      UDisksObject *iter_object = UDISKS_OBJECT (l->data);
      UDisksBlock *iter_block;

      iter_block = udisks_object_peek_block (iter_object);
      if (iter_block == NULL)
        continue;

      if (g_strcmp0 (udisks_block_get_crypto_backing_device (iter_block), object_path) == 0)
        {
          ret = g_object_ref (iter_block);
          goto out;
        }
    }

 out:
  g_list_free_full (objects, g_object_unref);
  return ret;
}

static gboolean
walk_block (UDisksDaemon  *daemon,
            UDisksBlock   *block,
            BlockWalker   *walker,
            gpointer       user_data,
            GError       **error)
{
  UDisksObject *object;
  UDisksBlock *cleartext;
  gboolean is_leaf = TRUE;
  guint num_parts = 0;

  object = UDISKS_OBJECT (g_dbus_interface_get_object (G_DBUS_INTERFACE (block)));
  if (object != NULL)
    {
      /* Recurse for all primary and extended partitions if this is a
       * partition table, or for all logical partitions if this is a
       * extended partition. */

      UDisksPartitionTable *table;
      gboolean is_container;

      UDisksPartition *partition = udisks_object_peek_partition (object);
      if (partition && udisks_partition_get_is_container (partition))
        {
          table = peek_partition_table (daemon, partition);
          is_container = TRUE;
        }
      else
        {
          table = udisks_object_peek_partition_table (object);
          is_container = FALSE;
        }

      if (table)
        {
          GList *ps, *l;
          ps = udisks_linux_partition_table_get_partitions (daemon, table, &num_parts);
          for (l = ps; l != NULL; l = l->next)
            {
              UDisksPartition *p = UDISKS_PARTITION (l->data);
              UDisksObject *o = (UDisksObject *) g_dbus_interface_get_object (G_DBUS_INTERFACE (p));
              UDisksBlock *b = o ? udisks_object_peek_block (o) : NULL;
              if (b && !is_container == !udisks_partition_get_is_contained (p))
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
udisks_linux_remove_configuration (GVariant  *config,
                                   GError   **error)
{
  GVariantIter iter;
  const gchar *item_type;
  GVariant *details;

  udisks_debug ("Removing for teardown: %s", g_variant_print (config, FALSE));

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
teardown_block_walker (UDisksDaemon  *daemon,
                       UDisksBlock   *block,
                       gboolean       is_leaf,
                       gpointer       user_data,
                       GError       **error)
{
  struct TeardownData *data = user_data;
  UDisksObject *object = UDISKS_OBJECT (g_dbus_interface_get_object (G_DBUS_INTERFACE (block)));
  UDisksEncrypted *enc = udisks_object_peek_encrypted (object);

  if (enc)
    {
      UDisksBlock *cleartext = get_cleartext_block (daemon, block);
      if (cleartext)
        {
          /* The crypto backing device is unlocked and the cleartext
             device has been cleaned up.  Lock the backing device so
             that we can format or wipe it later.
          */
          g_object_unref (cleartext);
          if (enc && !udisks_linux_encrypted_lock (UDISKS_LINUX_ENCRYPTED (enc),
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
          if (!udisks_linux_remove_configuration (udisks_encrypted_get_child_configuration (enc), error))
              return FALSE;
        }
    }

  return udisks_linux_remove_configuration (udisks_block_get_configuration (block), error);
}

gboolean
udisks_linux_block_teardown (UDisksBlock               *block,
                             GDBusMethodInvocation     *invocation,
                             GVariant                  *options,
                             GError                   **error)
{
  UDisksObject *object = UDISKS_OBJECT (g_dbus_interface_get_object (G_DBUS_INTERFACE (block)));
  UDisksDaemon *daemon = udisks_linux_block_object_get_daemon (UDISKS_LINUX_BLOCK_OBJECT (object));
  struct TeardownData data;

  data.invocation = invocation;
  data.options = options;
  return walk_block (daemon, block, teardown_block_walker, &data, error);
}

/* ---------------------------------------------------------------------------------------------------- */

gboolean
udisks_linux_block_is_luks (UDisksBlock *block)
{
  return g_strcmp0 (udisks_block_get_id_usage (block), "crypto") == 0 &&
         g_strcmp0 (udisks_block_get_id_type (block), "crypto_LUKS") == 0;
}

gboolean
udisks_linux_block_is_tcrypt (UDisksBlock *block)
{
  return g_strcmp0 (udisks_block_get_id_usage (block), "crypto") == 0 &&
         g_strcmp0 (udisks_block_get_id_type (block), "crypto_TCRYPT") == 0;
}

gboolean
udisks_linux_block_is_bitlk (UDisksBlock *block)
{
  return g_strcmp0 (udisks_block_get_id_usage (block), "crypto") == 0 &&
         g_strcmp0 (udisks_block_get_id_type (block), "BitLocker") == 0;
}

gboolean
udisks_linux_block_is_unknown_crypto (UDisksBlock *block)
{
  return g_strcmp0 (udisks_block_get_id_usage (block), "crypto") == 0 &&
         g_strcmp0 (udisks_block_get_id_type (block), "crypto_unknown") == 0;
}

/* ---------------------------------------------------------------------------------------------------- */

void
udisks_linux_block_encrypted_lock (UDisksBlock *block)
{
  UDisksLinuxBlock *block_iface = UDISKS_LINUX_BLOCK (block);
  g_mutex_lock (&block_iface->encrypted_lock);
}

void
udisks_linux_block_encrypted_unlock (UDisksBlock *block)
{
  UDisksLinuxBlock *block_iface = UDISKS_LINUX_BLOCK (block);
  g_mutex_unlock (&block_iface->encrypted_lock);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
trigger_uevent_on_nested_partitions (UDisksDaemon           *daemon,
                                     UDisksLinuxBlockObject *object)
{
  UDisksLinuxDevice *block_device;
  GUdevClient *gudev_client;
  GUdevEnumerator *gudev_enumerator;
  const gchar *sysfs_path;
  GList *list;
  GList *l;

  block_device = udisks_linux_block_object_get_device (object);
  if (block_device == NULL)
    return;

  sysfs_path = g_udev_device_get_sysfs_path (block_device->udev_device);

  /* Can't be quite sure that block objects for all the partitions have been created
   * at this point, also the UDisksPartitionTable.Partitions property is filled from
   * a list of exported objects on the object manager filtered by a device path.
   * Reaching to udev db directly instead.
   */
  gudev_client = udisks_linux_provider_get_udev_client (udisks_daemon_get_linux_provider (daemon));
  gudev_enumerator = g_udev_enumerator_new (gudev_client);

  g_udev_enumerator_add_match_sysfs_attr (gudev_enumerator, "partition", "1");

  list = g_udev_enumerator_execute (gudev_enumerator);
  for (l = list; l; l = g_list_next (l))
    {
      GUdevDevice *parent;

      parent = g_udev_device_get_parent (l->data);
      if (parent)
        {
          if (g_strcmp0 (g_udev_device_get_sysfs_path (parent), sysfs_path) == 0)
            udisks_daemon_util_trigger_uevent_sync (daemon, NULL, g_udev_device_get_sysfs_path (l->data), UDISKS_DEFAULT_WAIT_TIMEOUT);
          g_object_unref (parent);
        }
    }
  g_list_free_full (list, g_object_unref);

  g_object_unref (gudev_enumerator);
  g_object_unref (block_device);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
handle_format_failure (GDBusMethodInvocation *invocation,
                       GError *error)
{
  udisks_warning ("%s", error->message);
  if (invocation != NULL)
    g_dbus_method_invocation_take_error (invocation, error);
  else
    g_error_free (error);
}

static gboolean
format_check_auth (UDisksDaemon          *daemon,
                   UDisksBlock           *block,
                   UDisksObject          *object,
                   uid_t                  caller_uid,
                   GVariant              *options,
                   GDBusMethodInvocation *invocation,
                   gboolean               secure_erase,
                   gboolean               format_extra_args,
                   gboolean               modify_system_configuration)
{
  const gchar *action_id;
  const gchar *message;

  if (secure_erase)
    {
      /* Translators: Shown in authentication dialog when the user
       * requests erasing a hard disk using the SECURE ERASE UNIT
       * command.
       *
       * Do not translate $(drive), it's a placeholder and
       * will be replaced by the name of the drive/device in question
       */
      message = N_("Authentication is required to perform a secure erase of $(drive)");
      action_id = "org.freedesktop.udisks2.ata-secure-erase";
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
      action_id = format_extra_args ? "org.freedesktop.udisks2.modify-device-system" :
                                      "org.freedesktop.udisks2.modify-device";
      if (!udisks_daemon_util_setup_by_user (daemon, object, caller_uid))
        {
          if (udisks_block_get_hint_system (block))
            {
              action_id = "org.freedesktop.udisks2.modify-device-system";
            }
          else if (!udisks_daemon_util_on_user_seat (daemon, object, caller_uid))
            {
              action_id = "org.freedesktop.udisks2.modify-device-other-seat";
            }
        }
    }

  if (!udisks_daemon_util_check_authorization_sync (daemon,
                                                    object,
                                                    action_id,
                                                    options,
                                                    message,
                                                    invocation))
    return FALSE;

  if (modify_system_configuration &&
      !udisks_daemon_util_check_authorization_sync (daemon,
                                                    NULL,
                                                    "org.freedesktop.udisks2.modify-system-configuration",
                                                    options,
                                                    N_("Authentication is required to modify the system configuration"),
                                                    invocation))
    return FALSE;

  return TRUE;
}

/* remove dashes for certain filesystem types */
static gchar *
reformat_uuid_string (const gchar *arg_uuid, const gchar *fs_type)
{
  if (arg_uuid == NULL)
    return NULL;
  if (g_strcmp0 (fs_type, "vfat") == 0 ||
      g_strcmp0 (fs_type, "exfat") == 0 ||
      g_strcmp0 (fs_type, "ntfs") == 0 ||
      g_strcmp0 (fs_type, "udf") == 0)
      return udisks_daemon_util_subst_str (arg_uuid, "-", NULL);
  return g_strdup (arg_uuid);
}

static gboolean
format_pre_checks (const gchar         *fs_type,
                   const gchar         *label,
                   const gchar         *uuid,
                   const BDFSFeatures **fs_features,
                   GError             **error)
{
  GError *l_error = NULL;
  gchar *required_utility = NULL;

  if (g_strcmp0 (fs_type, "dos") == 0 ||
      g_strcmp0 (fs_type, "gpt") == 0 ||
      g_strcmp0 (fs_type, "empty") == 0 ||
      g_strcmp0 (fs_type, "swap") == 0)
    return TRUE;

  /* TODO: swap deps are checked on plugin initialization, subject to change */

  *fs_features = bd_fs_features (fs_type, &l_error);
  if (! *fs_features)
    {
      g_set_error_literal (error, UDISKS_ERROR, UDISKS_ERROR_NOT_SUPPORTED,
                           l_error->message);
      g_clear_error (&l_error);
      return FALSE;
    }

  if (!bd_fs_can_mkfs (fs_type, NULL, &required_utility, &l_error))
    {
      if (l_error != NULL)
        {
          g_set_error_literal (error, UDISKS_ERROR, UDISKS_ERROR_NOT_SUPPORTED,
                               l_error->message);
          g_clear_error (&l_error);
          return FALSE;
        }

      g_set_error (error, UDISKS_ERROR, UDISKS_ERROR_NOT_SUPPORTED,
                   "Creation of file system type %s is not supported: executable %s not found",
                   fs_type, required_utility);
      g_free (required_utility);
      return FALSE;
    }

  if (label != NULL)
    {
      if (((*fs_features)->mkfs & BD_FS_MKFS_LABEL) == 0)
        {
          g_set_error (error, UDISKS_ERROR, UDISKS_ERROR_NOT_SUPPORTED,
                       "File system type %s does not support labels", fs_type);
          return FALSE;
        }
      if (! bd_fs_check_label (fs_type, label, &l_error))
        {
          if (! g_error_matches (l_error, BD_FS_ERROR, BD_FS_ERROR_NOT_SUPPORTED))
            {
              g_set_error_literal (error, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                   l_error->message);
              g_error_free (l_error);
              return FALSE;
            }
          g_clear_error (&l_error);
        }
    }

  if (uuid != NULL)
    {
      if (((*fs_features)->mkfs & BD_FS_MKFS_UUID) == 0 &&
          !bd_fs_can_set_uuid (fs_type, NULL, NULL))
        {
          g_set_error (error, UDISKS_ERROR, UDISKS_ERROR_NOT_SUPPORTED,
                       "File system type %s does not support setting UUID", fs_type);
          return FALSE;
        }
      if (! bd_fs_check_uuid (fs_type, uuid, &l_error))
        {
          if (! g_error_matches (l_error, BD_FS_ERROR, BD_FS_ERROR_NOT_SUPPORTED))
            {
              g_set_error_literal (error, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                   l_error->message);
              g_error_free (l_error);
              return FALSE;
            }
          g_clear_error (&l_error);
        }
    }

  return TRUE;
}

static gboolean
format_add_config_items (UDisksBlock  *filesystem_block,
                         UDisksBlock  *container_block,
                         GVariant     *config_items,
                         GError      **error)
{
  GVariantIter iter;
  const gchar *item_type;
  GVariant *details;
  gboolean ret = TRUE;

  g_variant_iter_init (&iter, config_items);
  while (g_variant_iter_next (&iter, "(&s@a{sv})", &item_type, &details))
    {
      if (strcmp (item_type, "fstab") == 0)
        ret = add_remove_fstab_entry (filesystem_block, NULL, details, error);
      else if (strcmp (item_type, "crypttab") == 0)
        ret = add_remove_crypttab_entry (container_block, NULL, details, error);
      g_variant_unref (details);
      if (! ret)
        break;
    }

  return ret;
}

static gboolean
format_wipe (UDisksDaemon  *daemon,
             UDisksBlock   *block,
             UDisksObject  *object,
             GError       **error)
{
  UDisksObject *filesystem_object;
  UDisksPartitionTable *partition_table;
  FormatWaitData wait_data = { 0, };
  GError *l_error = NULL;

  partition_table = udisks_object_peek_partition_table (object);
  if (!bd_fs_clean (udisks_block_get_device (block), FALSE, &l_error))
    {
      g_set_error (error, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                   "Error wiping device: %s", l_error->message);
      g_error_free (l_error);
      return FALSE;
    }

  /* wait until this change has taken effect */
  if (partition_table != NULL &&
      !udisks_linux_block_object_reread_partition_table (UDISKS_LINUX_BLOCK_OBJECT (object), &l_error))
    {
      udisks_warning ("%s", l_error->message);
      g_clear_error (&l_error);
    }
  udisks_linux_block_object_trigger_uevent_sync (UDISKS_LINUX_BLOCK_OBJECT (object),
                                                 UDISKS_DEFAULT_WAIT_TIMEOUT);
  wait_data.object = object;
  wait_data.type = "empty";
  filesystem_object = udisks_daemon_wait_for_object_sync (daemon,
                                                          wait_for_filesystem,
                                                          &wait_data,
                                                          NULL,
                                                          UDISKS_DEFAULT_WAIT_TIMEOUT,
                                                          error);
  if (filesystem_object == NULL)
    {
      g_prefix_error (error, "Error synchronizing after initial wipe: ");
      return FALSE;
    }
  g_object_unref (filesystem_object);

  return TRUE;
}


static gboolean
format_create_luks (UDisksDaemon  *daemon,
                    UDisksBlock   *block,
                    UDisksObject  *object,
                    uid_t          caller_uid,
                    GString       *encrypt_passphrase,
                    const gchar   *encrypt_type,
                    const gchar   *encrypt_pbkdf,
                    guint32        encrypt_memory,
                    guint32        encrypt_iterations,
                    guint32        encrypt_time,
                    guint32        encrypt_threads,
                    const gchar   *encrypt_label,
                    UDisksBlock  **block_to_mkfs,
                    UDisksObject **object_to_mkfs,
                    GError       **error)
{
  UDisksConfigManager *config_manager;
  UDisksState *state = NULL;
  UDisksObject *luks_uuid_object;
  UDisksObject *cleartext_object = NULL;
  UDisksBlock *cleartext_block = NULL;
  UDisksLinuxDevice *udev_cleartext_device = NULL;
  CryptoJobData crypto_job_data = { 0, };
  FormatWaitData wait_data = { 0, };
  GError *l_error = NULL;

  config_manager = udisks_daemon_get_config_manager (daemon);
  state = udisks_daemon_get_state (daemon);

  crypto_job_data.device = udisks_block_get_device (block);
  crypto_job_data.passphrase = encrypt_passphrase;
  if (encrypt_type != NULL)
    crypto_job_data.type = encrypt_type;
  else
    crypto_job_data.type = udisks_config_manager_get_encryption (config_manager);
  crypto_job_data.pbkdf = encrypt_pbkdf;
  crypto_job_data.memory = encrypt_memory;
  crypto_job_data.iterations = encrypt_iterations;
  crypto_job_data.time = encrypt_time;
  crypto_job_data.threads = encrypt_threads;
  crypto_job_data.label = encrypt_label;

  /* Create it */
  udisks_linux_block_encrypted_lock (block);
  if (!udisks_daemon_launch_threaded_job_sync (daemon,
                                               object,
                                               "format-mkfs",
                                               caller_uid,
                                               luks_format_job_func,
                                               &crypto_job_data,
                                               NULL, /* user_data_free_func */
                                               NULL, /* cancellable */
                                               &l_error))
    {
      g_set_error (error, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                   "Error creating LUKS device: %s", l_error->message);
      g_error_free (l_error);
      udisks_linux_block_encrypted_unlock (block);
      return FALSE;
    }
  udisks_linux_block_encrypted_unlock (block);

  /* Wait for the UUID to be set */
  wait_data.object = object;
  wait_data.type = NULL;
  luks_uuid_object = udisks_daemon_wait_for_object_sync (daemon,
                                                         wait_for_luks_uuid,
                                                         &wait_data,
                                                         NULL,
                                                         UDISKS_DEFAULT_WAIT_TIMEOUT,
                                                         error);
  if (luks_uuid_object == NULL)
    {
      g_prefix_error (error, "Error waiting for LUKS UUID: ");
      return FALSE;
    }
  g_object_unref (luks_uuid_object);

  /* Open it */
  crypto_job_data.read_only = FALSE;
  crypto_job_data.map_name = make_block_luksname (block, error);
  if (!crypto_job_data.map_name)
    {
      g_prefix_error (error, "Failed to get LUKS UUID: ");
      return FALSE;
    }

  udisks_linux_block_encrypted_lock (block);
  if (!udisks_daemon_launch_threaded_job_sync (daemon,
                                               object,
                                               "format-mkfs",
                                               caller_uid,
                                               luks_open_job_func,
                                               &crypto_job_data,
                                               NULL, /* user_data_free_func */
                                               NULL, /* cancellable */
                                               &l_error))
    {
      g_set_error (error, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                   "Error opening LUKS device: %s", l_error->message);
      g_error_free (l_error);
      g_free (crypto_job_data.map_name);
      udisks_linux_block_encrypted_unlock (block);
      return FALSE;
    }
  udisks_linux_block_encrypted_unlock (block);
  g_free (crypto_job_data.map_name);

  /* Wait for it */
  cleartext_object = udisks_daemon_wait_for_object_sync (daemon,
                                                         wait_for_luks_cleartext,
                                                         &wait_data,
                                                         NULL,
                                                         UDISKS_DEFAULT_WAIT_TIMEOUT,
                                                         error);
  if (cleartext_object == NULL)
    {
      g_prefix_error (error, "Error waiting for LUKS cleartext device: ");
      return FALSE;
    }
  cleartext_block = udisks_object_get_block (cleartext_object);
  if (cleartext_block == NULL)
    {
      g_set_error (error, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                   "LUKS cleartext device does not have block interface");
      g_object_unref (cleartext_object);
      return FALSE;
    }

  /* update the unlocked-crypto-dev file */
  udev_cleartext_device = udisks_linux_block_object_get_device (UDISKS_LINUX_BLOCK_OBJECT (cleartext_object));
  udisks_state_add_unlocked_crypto_dev (state,
                                        udisks_block_get_device_number (cleartext_block),
                                        udisks_block_get_device_number (block),
                                        g_udev_device_get_sysfs_attr (udev_cleartext_device->udev_device, "dm/uuid"),
                                        caller_uid);

  *object_to_mkfs = cleartext_object;
  *block_to_mkfs = cleartext_block;

  g_object_unref (udev_cleartext_device);
  return TRUE;
}

static gboolean
format_set_partition_type (UDisksPartition       *partition,
                           UDisksPartitionTable  *partition_table,
                           const gchar           *fs_type,
                           gboolean               encrypted,
                           const BDFSFeatures    *fs_features,
                           uid_t                  caller_uid,
                           GError               **error)
{
  const gchar *part_table_type;
  const gchar *partition_type = NULL;

  part_table_type = udisks_partition_table_get_type_ (partition_table);
  if (g_strcmp0 (part_table_type, "gpt") == 0)
    {
      if (encrypted)
        partition_type = "ca7d7ccb-63ed-4c53-861c-1742536059cc";
      else if (g_strcmp0 (fs_type, "swap") == 0)
        partition_type = "0657fd6d-a4ab-43c4-84e5-0933c84b4f4f";
      else if (fs_features != NULL)
        partition_type = fs_features->partition_type;
    }
  else if (g_strcmp0 (part_table_type, "dos") == 0)
    {
      if (encrypted)
        partition_type = "0xe8";
      else if (g_strcmp0 (fs_type, "swap") == 0)
        partition_type = "0x82";
      else if (fs_features != NULL)
        partition_type = fs_features->partition_id;
    }

  if (partition_type == NULL)
    return TRUE;   /* do nothing */

  if (g_strcmp0 (udisks_partition_get_type_ (partition), partition_type) != 0)
    {
      if (!udisks_linux_partition_set_type_sync (UDISKS_LINUX_PARTITION (partition),
                                                 partition_type,
                                                 caller_uid,
                                                 NULL, /* cancellable */
                                                 error))
        {
          g_prefix_error (error, "Error setting partition type after formatting: ");
          return FALSE;
        }
    }

  return TRUE;
}

typedef struct {
  const gchar *device;
  const gchar *type;
  const gchar *label;
  const gchar *uuid;
  const BDExtraArg **extra_args;
  gboolean dry_run;
  gboolean no_discard;
} FormatJobData;

static gboolean
format_job_func (UDisksThreadedJob  *job,
                 GCancellable       *cancellable,
                 gpointer            user_data,
                 GError            **error)
{
  FormatJobData *data = user_data;
  BDFSMkfsOptions options = { 0, };
  GError *l_error = NULL;

  udisks_job_set_cancelable (UDISKS_JOB (job), FALSE);

  /* special case for swap */
  if (g_strcmp0 (data->type, "swap") == 0)
    {
      if (! bd_swap_mkswap (data->device, data->label, data->uuid, NULL, &l_error))
        {
          g_set_error (error, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                       "Error creating swap: %s", l_error->message);
          g_error_free (l_error);
          return FALSE;
        }
      return TRUE;
    }

  /* real filesystems */
  options.label = data->label;
  options.uuid = data->uuid;
  options.dry_run = data->dry_run;
  options.no_discard = data->no_discard;
  options.no_pt = TRUE;  /* disable creation of a protective part table */

  if (! bd_fs_mkfs (data->device, data->type, &options, data->extra_args, &l_error))
    {
      g_set_error (error, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                   "Error creating filesystem '%s': %s",
                   data->type, l_error->message);
      g_error_free (l_error);
      return FALSE;
    }

  return TRUE;
}


void
udisks_linux_block_handle_format (UDisksBlock             *block,
                                  GDBusMethodInvocation   *invocation,
                                  const gchar             *type,
                                  GVariant                *options,
                                  void                   (*complete)(gpointer user_data),
                                  gpointer                 complete_user_data)
{
  UDisksObject *object;
  UDisksPartition *partition = NULL;
  UDisksPartitionTable *partition_table = NULL;
  UDisksBlock *block_to_mkfs = NULL;
  UDisksObject *object_to_mkfs = NULL;
  UDisksObject *filesystem_object;
  UDisksDaemon *daemon;
  UDisksState *state = NULL;
  const BDFSFeatures *fs_features = NULL;
  FormatWaitData wait_data = { 0, };
  GError *error = NULL;
  uid_t caller_uid;
  gid_t caller_gid;
  /* D-Bus options */
  gboolean take_ownership = FALSE;
  GString *encrypt_passphrase = NULL;
  const gchar *encrypt_type = NULL;
  const gchar *encrypt_pbkdf = NULL;
  guint32 encrypt_memory = 0;
  guint32 encrypt_iterations = 0;
  guint32 encrypt_time = 0;
  guint32 encrypt_threads = 0;
  const gchar *encrypt_label = NULL;
  const gchar *erase_type = NULL;
  gboolean no_block = FALSE;
  gboolean update_partition_type = FALSE;
  gboolean dry_run_first = FALSE;
  GVariant *config_items = NULL;
  gboolean teardown_flag = FALSE;
  gboolean no_discard_flag = FALSE;
  const gchar *label = NULL;
  const gchar *arg_uuid = NULL;
  gchar *uuid = NULL;
  gchar **mkfs_args = NULL;
  BDExtraArg **extra_args = NULL;

  object = udisks_daemon_util_dup_object (block, &error);
  if (object == NULL)
    {
      handle_format_failure (invocation, error);
      goto out;
    }

  daemon = udisks_linux_block_object_get_daemon (UDISKS_LINUX_BLOCK_OBJECT (object));
  state = udisks_daemon_get_state (daemon);

  udisks_linux_block_object_lock_for_cleanup (UDISKS_LINUX_BLOCK_OBJECT (object));
  udisks_state_check_block (state, udisks_linux_block_object_get_device_number (UDISKS_LINUX_BLOCK_OBJECT (object)));

  g_variant_lookup (options, "take-ownership", "b", &take_ownership);
  udisks_variant_lookup_binary (options, "encrypt.passphrase", &encrypt_passphrase);
  g_variant_lookup (options, "encrypt.type", "&s", &encrypt_type);
  g_variant_lookup (options, "encrypt.pbkdf", "&s", &encrypt_pbkdf);
  g_variant_lookup (options, "encrypt.memory", "u", &encrypt_memory);
  g_variant_lookup (options, "encrypt.iterations", "u", &encrypt_iterations);
  g_variant_lookup (options, "encrypt.time", "u", &encrypt_time);
  g_variant_lookup (options, "encrypt.threads", "u", &encrypt_threads);
  g_variant_lookup (options, "encrypt.label", "&s", &encrypt_label);
  g_variant_lookup (options, "erase", "&s", &erase_type);
  g_variant_lookup (options, "no-block", "b", &no_block);
  g_variant_lookup (options, "update-partition-type", "b", &update_partition_type);
  g_variant_lookup (options, "dry-run-first", "b", &dry_run_first);
  g_variant_lookup (options, "config-items", "@a(sa{sv})", &config_items);
  g_variant_lookup (options, "tear-down", "b", &teardown_flag);
  g_variant_lookup (options, "no-discard", "b", &no_discard_flag);
  g_variant_lookup (options, "label", "&s", &label);
  g_variant_lookup (options, "uuid", "&s", &arg_uuid);
  g_variant_lookup (options, "mkfs-args", "^a&s", &mkfs_args);
  uuid = reformat_uuid_string (arg_uuid, type);

  partition = udisks_object_get_partition (object);
  if (partition != NULL)
    {
      UDisksObject *partition_table_object;

      /* Fail if partition contains a partition table (e.g. Fedora Hybrid ISO).
       * See: https://bugs.freedesktop.org/show_bug.cgi?id=76178
       */
      if (udisks_partition_get_offset (partition) == 0)
        {
          g_set_error (&error, UDISKS_ERROR, UDISKS_ERROR_NOT_SUPPORTED,
                       "This partition cannot be modified because it contains a partition table; please reinitialize layout of the whole device.");
          handle_format_failure (invocation, error);
          goto out;
        }

      partition_table_object = udisks_daemon_find_object (daemon, udisks_partition_get_table (partition));
      if (partition_table_object == NULL)
        {
          g_clear_object (&partition);
        }
      else
        {
          partition_table = udisks_object_get_partition_table (partition_table_object);
          g_clear_object (&partition_table_object);
        }
    }

  if (!udisks_daemon_util_get_caller_uid_sync (daemon, invocation, NULL /* GCancellable */, &caller_uid, &error))
    {
      handle_format_failure (invocation, error);
      goto out;
    }

  if (!udisks_daemon_util_get_user_info (caller_uid, &caller_gid, NULL /* user name */, &error))
    {
      handle_format_failure (invocation, error);
      goto out;
    }

  /* Check for filesystem type support */
  if (!format_pre_checks (type, label, uuid, &fs_features, &error))
    {
      handle_format_failure (invocation, error);
      goto out;
    }

  if (mkfs_args != NULL)
    {
      guint i, nargs = g_strv_length (mkfs_args);
      extra_args = g_new0 (BDExtraArg *, nargs + 1);
      for (i = 0; i < nargs; i++)
        {
          extra_args[i] = bd_extra_arg_new (mkfs_args[i], NULL);
        }
    }

  /* Check the required authorization */
  if (!format_check_auth (daemon,
                          block,
                          object,
                          caller_uid,
                          options,
                          invocation,
                          g_strcmp0 (erase_type, "ata-secure-erase") == 0 || g_strcmp0 (erase_type, "ata-secure-erase-enhanced") == 0,
                          extra_args != NULL,
                          config_items != NULL || teardown_flag))
    goto out;

  /* Tear down the device structure if requested */
  if (teardown_flag)
    {
      if (!udisks_linux_block_teardown (block, invocation, options, &error))
        {
          handle_format_failure (invocation, error);
          goto out;
        }
    }

  /* First wipe the device... */
  if (!format_wipe (daemon, block, object, &error))
    {
      handle_format_failure (invocation, error);
      goto out;
    }

  /* If requested, check whether the ultimate filesystem creation
     will succeed before actually getting to work.
  */
  if (dry_run_first && fs_features != NULL &&
      (fs_features->mkfs & BD_FS_MKFS_DRY_RUN) == BD_FS_MKFS_DRY_RUN)
    {
      FormatJobData format_job_data = { 0, };

      format_job_data.device = udisks_block_get_device (block);
      format_job_data.type = type;
      format_job_data.label = label;
      format_job_data.uuid = uuid;
      format_job_data.extra_args = (const BDExtraArg **) extra_args;
      format_job_data.dry_run = TRUE;
      format_job_data.no_discard = no_discard_flag;

      if (!udisks_daemon_launch_threaded_job_sync (daemon,
                                                   object,
                                                   "format-mkfs",
                                                   caller_uid,
                                                   format_job_func,
                                                   &format_job_data,
                                                   NULL, /* user_data_free_func */
                                                   NULL, /* cancellable */
                                                   &error))
        {
          handle_format_failure (invocation, error);
          goto out;
        }
    }

  /* Create LUKS, if requested */
  if (encrypt_passphrase != NULL)
    {
      if (!format_create_luks (daemon,
                               block,
                               object,
                               caller_uid,
                               encrypt_passphrase,
                               encrypt_type,
                               encrypt_pbkdf,
                               encrypt_memory,
                               encrypt_iterations,
                               encrypt_time,
                               encrypt_threads,
                               encrypt_label,
                               &block_to_mkfs,
                               &object_to_mkfs,
                               &error))
        {
          handle_format_failure (invocation, error);
          goto out;
        }
    }
  else
    {
      object_to_mkfs = g_object_ref (object);
      block_to_mkfs = g_object_ref (block);
    }

  /* Complete early, if requested */
  if (no_block)
    {
      complete (complete_user_data);
      invocation = NULL;
    }

  /* Erase the device, if requested */
  if (erase_type != NULL)
    {
      if (!erase_device (block_to_mkfs, object_to_mkfs, daemon, caller_uid, erase_type, &error))
        {
          g_prefix_error (&error, "Error erasing device: ");
          handle_format_failure (invocation, error);
          goto out;
        }
    }

  /* Perform the actual mkfs, partition table creation or wipe depending on @type */
  if (g_strcmp0 (type, "dos") == 0 || g_strcmp0 (type, "gpt") == 0)
    {
      BDPartTableType part_table_type = BD_PART_TABLE_UNDEF;

      /* Create the partition table. */
      if (g_strcmp0 (type, "dos") == 0)
        part_table_type = BD_PART_TABLE_MSDOS;
      else if (g_strcmp0 (type, "gpt") == 0)
        part_table_type = BD_PART_TABLE_GPT;
      if (! bd_part_create_table (udisks_block_get_device (block_to_mkfs), part_table_type, TRUE, &error))
        {
          handle_format_failure (invocation, error);
          goto out;
        }
    }
  else if (g_strcmp0 (type, "empty") == 0)
    {
      /* Only perform another wipe on LUKS devices, otherwise the device has been already wiped earlier. */
      if (encrypt_passphrase != NULL)
        {
          GError *l_error = NULL;

          if (!bd_fs_clean (udisks_block_get_device (block_to_mkfs), FALSE, &l_error))
            {
              g_set_error (&error, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                           "Error wiping device: %s", l_error->message);
              g_error_free (l_error);
              handle_format_failure (invocation, error);
              goto out;
            }
        }
    }
  else
    {
      /* Create the final filesystem */
      FormatJobData format_job_data = { 0, };

      format_job_data.device = udisks_block_get_device (block_to_mkfs);
      format_job_data.type = type;
      format_job_data.label = label;
      format_job_data.uuid = uuid;
      format_job_data.extra_args = (const BDExtraArg **) extra_args;
      format_job_data.dry_run = FALSE;
      format_job_data.no_discard = no_discard_flag;

      if (!udisks_daemon_launch_threaded_job_sync (daemon,
                                                   object,
                                                   "format-mkfs",
                                                   caller_uid,
                                                   format_job_func,
                                                   &format_job_data,
                                                   NULL, /* user_data_free_func */
                                                   NULL, /* cancellable */
                                                   &error))
        {
          handle_format_failure (invocation, error);
          goto out;
        }
    }

  /* Set filesystem UUID if not supported atomically during mkfs */
  if (arg_uuid && fs_features &&
      (fs_features->mkfs & BD_FS_MKFS_UUID) == 0 &&
      bd_fs_can_set_uuid (type, NULL, NULL))
    {
      if (! bd_fs_set_uuid (udisks_block_get_device (block_to_mkfs), uuid, type, &error))
        {
          handle_format_failure (invocation, error);
          goto out;
        }
    }

  /* Set the partition type, if requested */
  if (update_partition_type && partition != NULL && partition_table != NULL)
    {
      if (! format_set_partition_type (partition,
                                       partition_table,
                                       type,
                                       encrypt_passphrase != NULL,
                                       fs_features,
                                       caller_uid,
                                       &error))
        {
          handle_format_failure (invocation, error);
          goto out;
        }
    }

  /* The mkfs program may not generate all the uevents we need - so explicitly
   * trigger an event here
   */
  if (fs_features && (fs_features->features & BD_FS_FEATURE_PARTITION_TABLE) == BD_FS_FEATURE_PARTITION_TABLE &&
      !udisks_linux_block_object_reread_partition_table (UDISKS_LINUX_BLOCK_OBJECT (object), &error))
    {
      udisks_warning ("%s", error->message);
      g_clear_error (&error);
    }
  udisks_linux_block_object_trigger_uevent_sync (UDISKS_LINUX_BLOCK_OBJECT (object_to_mkfs),
                                                 UDISKS_DEFAULT_WAIT_TIMEOUT);

  /* In case a protective partition table was potentially created, trigger uevents
   * on all nested partitions. */
  if (fs_features && (fs_features->features & BD_FS_FEATURE_PARTITION_TABLE) == BD_FS_FEATURE_PARTITION_TABLE)
    {
      trigger_uevent_on_nested_partitions (daemon, UDISKS_LINUX_BLOCK_OBJECT (object));
    }

  /* Wait for the desired filesystem interface */
  wait_data.object = object_to_mkfs;
  wait_data.type = type;
  filesystem_object = udisks_daemon_wait_for_object_sync (daemon,
                                                          wait_for_filesystem,
                                                          &wait_data,
                                                          NULL,
                                                          UDISKS_DEFAULT_WAIT_TIMEOUT,
                                                          &error);
  if (filesystem_object == NULL)
    {
      g_prefix_error (&error, "Error synchronizing after formatting with type `%s': ", type);
      handle_format_failure (invocation, error);
      goto out;
    }
  g_object_unref (filesystem_object);

  /* Change ownership, if requested and supported */
  if (take_ownership && fs_features != NULL &&
      (fs_features->features & BD_FS_FEATURE_OWNERS) == BD_FS_FEATURE_OWNERS)
    {
      if (!take_filesystem_ownership (udisks_block_get_device (block_to_mkfs),
                                      type, caller_uid, caller_gid, FALSE, &error))
        {
          g_prefix_error (&error, "Failed to take ownership of the newly created filesystem: ");
          handle_format_failure (invocation, error);
          goto out;
        }
    }

  /* Add configuration items */
  if (config_items)
    {
      if (!format_add_config_items (block_to_mkfs, block, config_items, &error))
        {
          handle_format_failure (invocation, error);
          goto out;
        }
      update_configuration (UDISKS_LINUX_BLOCK (block), daemon);
    }

  if (invocation != NULL)
    complete (complete_user_data);

 out:
  if (object != NULL)
    udisks_linux_block_object_release_cleanup_lock (UDISKS_LINUX_BLOCK_OBJECT (object));
  if (state != NULL)
    udisks_state_check (state);
  if (config_items)
    g_variant_unref (config_items);
  udisks_string_wipe_and_free (encrypt_passphrase);
  g_free (mkfs_args);
  g_free (uuid);
  bd_extra_arg_list_free (extra_args);
  g_clear_object (&block_to_mkfs);
  g_clear_object (&object_to_mkfs);
  g_clear_object (&partition_table);
  g_clear_object (&partition);
  g_clear_object (&object);
}

struct FormatCompleteData {
  UDisksBlock *block;
  GDBusMethodInvocation *invocation;
};

static void
handle_format_complete (gpointer user_data)
{
  struct FormatCompleteData *data = user_data;
  udisks_block_complete_format (data->block, data->invocation);
}

static gboolean
handle_format (UDisksBlock           *block,
               GDBusMethodInvocation *invocation,
               const gchar           *type,
               GVariant              *options)
{
  struct FormatCompleteData data;
  data.block = block;
  data.invocation = invocation;
  udisks_linux_block_handle_format (block, invocation, type, options,
                                    handle_format_complete, &data);

  return TRUE; /* returning true means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

static gint
open_device (const gchar      *device,
             const gchar      *mode,
             gint              flags,
             GError          **error)
{
  gint fd = -1;

  if (flags & O_RDWR || flags & O_RDONLY || flags & O_WRONLY)
    {
      g_set_error (error, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                   "Using 'O_RDWR', 'O_RDONLY' and 'O_WRONLY' flags is not permitted. "
                   "Use 'mode' argument instead.");
      goto out;
    }

  if (g_strcmp0 (mode, "r") == 0)
    flags |= O_RDONLY;
  else if (g_strcmp0 (mode, "w") == 0)
    flags |= O_WRONLY;
  else if (g_strcmp0 (mode, "rw") == 0)
    flags |= O_RDWR;
  else
    {
      g_set_error (error, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                   "Unknown mode '%s'", mode);
      goto out;
    }

  fd = open (device, flags);
  if (fd == -1)
    {
      g_set_error (error, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                   "Error opening device %s: %m", device);
      goto out;
    }

 out:
  return fd;
}

/* ---------------------------------------------------------------------------------------------------- */
static gboolean
handle_open_for_backup (UDisksBlock           *block,
                        GDBusMethodInvocation *invocation,
                        GUnixFDList           *fd_list,
                        GVariant              *options)
{
  UDisksObject *object;
  UDisksDaemon *daemon;
  UDisksState *state = NULL;
  const gchar *action_id;
  const gchar *device;
  GUnixFDList *out_fd_list = NULL;
  GError *error = NULL;
  gint fd = -1;

  object = udisks_daemon_util_dup_object (block, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = udisks_linux_block_object_get_daemon (UDISKS_LINUX_BLOCK_OBJECT (object));
  state = udisks_daemon_get_state (daemon);

  udisks_linux_block_object_lock_for_cleanup (UDISKS_LINUX_BLOCK_OBJECT (object));
  udisks_state_check_block (state, udisks_linux_block_object_get_device_number (UDISKS_LINUX_BLOCK_OBJECT (object)));

  action_id = "org.freedesktop.udisks2.open-device";
  if (udisks_block_get_hint_system (block))
    action_id = "org.freedesktop.udisks2.open-device-system";

  if (!udisks_daemon_util_check_authorization_sync (daemon,
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

  device = udisks_block_get_device (UDISKS_BLOCK (block));

  fd = open_device (device, "r", O_CLOEXEC | O_EXCL, &error);
  if (fd == -1)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  out_fd_list = g_unix_fd_list_new_from_array (&fd, 1);
  udisks_block_complete_open_for_backup (block, invocation, out_fd_list, g_variant_new_handle (0));

 out:
  if (object != NULL)
    udisks_linux_block_object_release_cleanup_lock (UDISKS_LINUX_BLOCK_OBJECT (object));
  if (state != NULL)
    udisks_state_check (state);
  g_clear_object (&out_fd_list);
  g_clear_object (&object);
  return TRUE; /* returning true means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
handle_open_for_restore (UDisksBlock           *block,
                         GDBusMethodInvocation *invocation,
                         GUnixFDList           *fd_list,
                         GVariant              *options)
{
  UDisksObject *object;
  UDisksDaemon *daemon;
  UDisksState *state = NULL;
  const gchar *action_id;
  const gchar *device;
  GUnixFDList *out_fd_list = NULL;
  GError *error;
  gint fd;

  error = NULL;
  object = udisks_daemon_util_dup_object (block, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = udisks_linux_block_object_get_daemon (UDISKS_LINUX_BLOCK_OBJECT (object));
  state = udisks_daemon_get_state (daemon);

  udisks_linux_block_object_lock_for_cleanup (UDISKS_LINUX_BLOCK_OBJECT (object));
  udisks_state_check_block (state, udisks_linux_block_object_get_device_number (UDISKS_LINUX_BLOCK_OBJECT (object)));

  action_id = "org.freedesktop.udisks2.open-device";
  if (udisks_block_get_hint_system (block))
    action_id = "org.freedesktop.udisks2.open-device-system";

  if (!udisks_daemon_util_check_authorization_sync (daemon,
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


  device = udisks_block_get_device (UDISKS_BLOCK (block));

  fd = open_device (device, "w", O_SYNC | O_CLOEXEC | O_EXCL, &error);
  if (fd == -1)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  out_fd_list = g_unix_fd_list_new_from_array (&fd, 1);
  udisks_block_complete_open_for_restore (block, invocation, out_fd_list, g_variant_new_handle (0));

 out:
  if (object != NULL)
    udisks_linux_block_object_release_cleanup_lock (UDISKS_LINUX_BLOCK_OBJECT (object));
  if (state != NULL)
    udisks_state_check (state);
  g_clear_object (&out_fd_list);
  g_clear_object (&object);
  return TRUE; /* returning true means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
handle_open_for_benchmark (UDisksBlock           *block,
                           GDBusMethodInvocation *invocation,
                           GUnixFDList           *fd_list,
                           GVariant              *options)
{
  UDisksObject *object;
  UDisksDaemon *daemon;
  UDisksState *state = NULL;
  const gchar *action_id;
  const gchar *device;
  GUnixFDList *out_fd_list = NULL;
  gboolean opt_writable = FALSE;
  GError *error = NULL;
  gint fd = -1;
  const gchar *open_mode = NULL;
  gint open_flags;

  object = udisks_daemon_util_dup_object (block, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = udisks_linux_block_object_get_daemon (UDISKS_LINUX_BLOCK_OBJECT (object));
  state = udisks_daemon_get_state (daemon);

  udisks_linux_block_object_lock_for_cleanup (UDISKS_LINUX_BLOCK_OBJECT (object));
  udisks_state_check_block (state, udisks_linux_block_object_get_device_number (UDISKS_LINUX_BLOCK_OBJECT (object)));

  action_id = "org.freedesktop.udisks2.open-device";
  if (udisks_block_get_hint_system (block))
    action_id = "org.freedesktop.udisks2.open-device-system";

  if (!udisks_daemon_util_check_authorization_sync (daemon,
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

  open_flags = O_DIRECT | O_SYNC | O_CLOEXEC;
  if (opt_writable)
    {
      open_flags |= O_EXCL;
      open_mode = "rw";
    }
  else
    open_mode = "r";

  device = udisks_block_get_device (UDISKS_BLOCK (block));

  fd = open_device (device, open_mode, open_flags, &error);
  if (fd == -1)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  out_fd_list = g_unix_fd_list_new_from_array (&fd, 1);
  udisks_block_complete_open_for_benchmark (block, invocation, out_fd_list, g_variant_new_handle (0));

 out:
  if (object != NULL)
    udisks_linux_block_object_release_cleanup_lock (UDISKS_LINUX_BLOCK_OBJECT (object));
  if (state != NULL)
    udisks_state_check (state);
  g_clear_object (&out_fd_list);
  g_clear_object (&object);
  return TRUE; /* returning true means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
handle_open_device (UDisksBlock           *block,
                    GDBusMethodInvocation *invocation,
                    GUnixFDList           *fd_list,
                    const gchar           *mode,
                    GVariant              *options)
{
  UDisksObject *object;
  UDisksDaemon *daemon;
  UDisksState *state = NULL;
  const gchar *action_id;
  const gchar *device;
  GUnixFDList *out_fd_list = NULL;
  GError *error = NULL;
  gint fd = -1;
  gint flags = 0;

  object = udisks_daemon_util_dup_object (block, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = udisks_linux_block_object_get_daemon (UDISKS_LINUX_BLOCK_OBJECT (object));
  state = udisks_daemon_get_state (daemon);

  udisks_linux_block_object_lock_for_cleanup (UDISKS_LINUX_BLOCK_OBJECT (object));
  udisks_state_check_block (state, udisks_linux_block_object_get_device_number (UDISKS_LINUX_BLOCK_OBJECT (object)));

  action_id = "org.freedesktop.udisks2.open-device";
  if (udisks_block_get_hint_system (block))
    action_id = "org.freedesktop.udisks2.open-device-system";

  if (!udisks_daemon_util_check_authorization_sync (daemon,
                                                    object,
                                                    action_id,
                                                    options,
                                                    /* Translators: Shown in authentication dialog when an application
                                                     * wants to benchmark a device.
                                                     *
                                                     * Do not translate $(drive), it's a placeholder and will
                                                     * be replaced by the name of the drive/device in question
                                                     */
                                                    N_("Authentication is required to open $(drive)."),
                                                    invocation))
    goto out;

  device = udisks_block_get_device (UDISKS_BLOCK (block));

  g_variant_lookup (options, "flags", "i", &flags);

  fd = open_device (device, mode, flags, &error);
  if (fd == -1)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  out_fd_list = g_unix_fd_list_new_from_array (&fd, 1);
  udisks_block_complete_open_device (block, invocation, out_fd_list, g_variant_new_handle (0));

 out:
  if (object != NULL)
    udisks_linux_block_object_release_cleanup_lock (UDISKS_LINUX_BLOCK_OBJECT (object));
  if (state != NULL)
    udisks_state_check (state);
  g_clear_object (&out_fd_list);
  g_clear_object (&object);
  return TRUE; /* returning true means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
handle_rescan (UDisksBlock           *block,
               GDBusMethodInvocation *invocation,
               GVariant              *options)
{
  UDisksObject *object = NULL;
  UDisksLinuxDevice *device = NULL;
  UDisksDaemon *daemon;
  const gchar *action_id;
  const gchar *message;
  GError *error = NULL;

  object = udisks_daemon_util_dup_object (block, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = udisks_linux_block_object_get_daemon (UDISKS_LINUX_BLOCK_OBJECT (object));

  /* Translators: Shown in authentication dialog when an application
   * wants to rescan a device.
   *
   * Do not translate $(drive), it's a placeholder and will
   * be replaced by the name of the drive/device in question
   */
  message = N_("Authentication is required to rescan $(drive)");
  action_id = "org.freedesktop.udisks2.rescan";

  if (!udisks_daemon_util_check_authorization_sync (daemon,
                                                    object,
                                                    action_id,
                                                    options,
                                                    message,
                                                    invocation))
    goto out;

  device = udisks_linux_block_object_get_device (UDISKS_LINUX_BLOCK_OBJECT (object));

  udisks_linux_block_object_trigger_uevent_sync (UDISKS_LINUX_BLOCK_OBJECT (object),
                                                 UDISKS_DEFAULT_WAIT_TIMEOUT);
  if (g_strcmp0 (g_udev_device_get_devtype (device->udev_device), "disk") == 0 &&
      !udisks_linux_block_object_reread_partition_table (UDISKS_LINUX_BLOCK_OBJECT (object), &error))
    {
      udisks_warning ("%s", error->message);
      g_clear_error (&error);
    }

  udisks_block_complete_rescan (block, invocation);

 out:
  g_clear_object (&device);
  g_clear_object (&object);
  return TRUE; /* returning true means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

/* runs in thread dedicated to handling method call */
static gboolean
handle_restore_encrypted_header (UDisksBlock           *encrypted,
                                 GDBusMethodInvocation *invocation,
                                 const gchar           *backup_file,
                                 GVariant              *options)
{
    UDisksObject *object = NULL;
    UDisksBlock *block;
    UDisksDaemon *daemon;
    UDisksState *state = NULL;
    uid_t caller_uid;
    GError *error = NULL;
    UDisksBaseJob *job = NULL;

    object = udisks_daemon_util_dup_object (encrypted, &error);
    if (object == NULL)
      {
        g_dbus_method_invocation_return_gerror (invocation, error);
        goto out;
      }

    block = udisks_object_peek_block (object);
    daemon = udisks_linux_block_object_get_daemon (UDISKS_LINUX_BLOCK_OBJECT (object));
    state = udisks_daemon_get_state (daemon);

    udisks_linux_block_object_lock_for_cleanup (UDISKS_LINUX_BLOCK_OBJECT (object));
    udisks_state_check_block (state, udisks_linux_block_object_get_device_number (UDISKS_LINUX_BLOCK_OBJECT (object)));

    if (!udisks_daemon_util_get_caller_uid_sync (daemon, invocation, NULL /* GCancellable */, &caller_uid, &error))
      {
        g_dbus_method_invocation_return_gerror (invocation, error);
        goto out;
      }

    job = udisks_daemon_launch_simple_job (daemon,
                                           UDISKS_OBJECT (object),
                                           "block-restore-encrypted-header",
                                           caller_uid,
                                           NULL);
    if (job == NULL)
      {
        g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                               "Failed to create a job object");
        goto out;
      }

    udisks_linux_block_encrypted_lock (block);

    if (!bd_crypto_luks_header_restore (udisks_block_get_device (block), backup_file, &error))
      {
        g_dbus_method_invocation_return_error (invocation,
                                               UDISKS_ERROR,
                                               UDISKS_ERROR_FAILED,
                                               "Error restoring header of encrypted device %s: %s",
                                               udisks_block_get_device (block),
                                               error->message);
        udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), FALSE, error->message);
        udisks_linux_block_encrypted_unlock (block);
        goto out;
      }

    udisks_linux_block_encrypted_unlock (block);

    udisks_block_complete_restore_encrypted_header (encrypted, invocation);
    udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), TRUE, NULL);

    out:
    if (object != NULL)
        udisks_linux_block_object_release_cleanup_lock (UDISKS_LINUX_BLOCK_OBJECT (object));
    if (state != NULL)
        udisks_state_check (state);
    g_clear_object (&object);
    g_clear_error (&error);
    return TRUE; /* returning TRUE means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

static void
block_iface_init (UDisksBlockIface *iface)
{
  iface->handle_get_secret_configuration  = handle_get_secret_configuration;
  iface->handle_add_configuration_item    = handle_add_configuration_item;
  iface->handle_remove_configuration_item = handle_remove_configuration_item;
  iface->handle_update_configuration_item = handle_update_configuration_item;
  iface->handle_format                    = handle_format;
  iface->handle_open_for_backup           = handle_open_for_backup;
  iface->handle_open_for_restore          = handle_open_for_restore;
  iface->handle_open_for_benchmark        = handle_open_for_benchmark;
  iface->handle_open_device               = handle_open_device;
  iface->handle_rescan                    = handle_rescan;
  iface->handle_restore_encrypted_header  = handle_restore_encrypted_header;
}
