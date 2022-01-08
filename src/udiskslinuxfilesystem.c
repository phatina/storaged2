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
#include <sys/wait.h>
#include <pwd.h>
#include <grp.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <mntent.h>
#include <sys/sysmacros.h>
#ifdef HAVE_ACL
#include <sys/acl.h>
#endif
#include <errno.h>
#include <blockdev/fs.h>
#include <blockdev/utils.h>

#include <libmount/libmount.h>

#include <glib/gstdio.h>

#include "udiskslogging.h"
#include "udiskslinuxfilesystem.h"
#include "udiskslinuxfilesystemhelpers.h"
#include "udiskslinuxblockobject.h"
#include "udiskslinuxblock.h"
#include "udiskslinuxfsinfo.h"
#include "udiskslinuxprovider.h"
#include "udisksdaemon.h"
#include "udisksstate.h"
#include "udisksdaemonutil.h"
#include "udisksmountmonitor.h"
#include "udisksmount.h"
#include "udiskslinuxdevice.h"
#include "udiskssimplejob.h"
#include "udiskslinuxdriveata.h"
#include "udiskslinuxmountoptions.h"

/**
 * SECTION:udiskslinuxfilesystem
 * @title: UDisksLinuxFilesystem
 * @short_description: Linux implementation of #UDisksFilesystem
 *
 * This type provides an implementation of the #UDisksFilesystem
 * interface on Linux.
 */

typedef struct _UDisksLinuxFilesystemClass   UDisksLinuxFilesystemClass;

/**
 * UDisksLinuxFilesystem:
 *
 * The #UDisksLinuxFilesystem structure contains only private data and should
 * only be accessed using the provided API.
 */
struct _UDisksLinuxFilesystem
{
  UDisksFilesystemSkeleton parent_instance;
  GMutex lock;
};

struct _UDisksLinuxFilesystemClass
{
  UDisksFilesystemSkeletonClass parent_class;
};

static void filesystem_iface_init (UDisksFilesystemIface *iface);

G_DEFINE_TYPE_WITH_CODE (UDisksLinuxFilesystem, udisks_linux_filesystem, UDISKS_TYPE_FILESYSTEM_SKELETON,
                         G_IMPLEMENT_INTERFACE (UDISKS_TYPE_FILESYSTEM, filesystem_iface_init));

#ifdef HAVE_FHS_MEDIA
#define MOUNT_BASE "/media"
#define MOUNT_BASE_PERSISTENT TRUE
#else
#define MOUNT_BASE "/run/media"
#define MOUNT_BASE_PERSISTENT FALSE
#endif

/* ---------------------------------------------------------------------------------------------------- */

static void
udisks_linux_filesystem_finalize (GObject *object)
{
  UDisksLinuxFilesystem *filesystem = UDISKS_LINUX_FILESYSTEM (object);

  g_mutex_clear (&(filesystem->lock));

  if (G_OBJECT_CLASS (udisks_linux_filesystem_parent_class)->finalize != NULL)
    G_OBJECT_CLASS (udisks_linux_filesystem_parent_class)->finalize (object);
}

static void
udisks_linux_filesystem_init (UDisksLinuxFilesystem *filesystem)
{
  g_mutex_init (&filesystem->lock);
  g_dbus_interface_skeleton_set_flags (G_DBUS_INTERFACE_SKELETON (filesystem),
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);
}

static void
udisks_linux_filesystem_class_init (UDisksLinuxFilesystemClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize     = udisks_linux_filesystem_finalize;
}

/**
 * udisks_linux_filesystem_new:
 *
 * Creates a new #UDisksLinuxFilesystem instance.
 *
 * Returns: A new #UDisksLinuxFilesystem. Free with g_object_unref().
 */
UDisksFilesystem *
udisks_linux_filesystem_new (void)
{
  return UDISKS_FILESYSTEM (g_object_new (UDISKS_TYPE_LINUX_FILESYSTEM,
                                          NULL));
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_linux_filesystem_is_volume_based:
 * @filesystem: A #UDisksLinuxFilesystem.
 * @object: The enclosing #UDisksLinuxBlockObject instance.
 *
 * Indicates whether the @filesystem is bound to a traditional block
 * device number (having a corresponding device node, denoted by
 * major:minor number) or whether the @filesystem is built upon a concept
 * of 'volume' (not necessarily bound to a particular block device),
 * such as btrfs. This means the filesystem should be identified e.g.
 * by UUID typically and that the traditional device number may not
 * match a mounted filesystem.
 *
 * Returns: %FALSE in case of a traditional filesystem, %TRUE in case of a volume-based filesystem.
 */
gboolean
udisks_linux_filesystem_is_volume_based (UDisksLinuxFilesystem  *filesystem,
                                         UDisksLinuxBlockObject *object)
{
  UDisksLinuxDevice *device;
  gboolean ret;

  device = udisks_linux_block_object_get_device (object);
  g_return_val_if_fail (device != NULL, FALSE);

  ret = g_strcmp0 (g_udev_device_get_property (device->udev_device, "ID_FS_TYPE"), "btrfs") == 0 &&
        g_udev_device_get_property (device->udev_device, "ID_FS_UUID") != NULL;
  /* TODO: look also for ID_BTRFS_READY? */

  g_object_unref (device);

  return ret;
}

/**
 * udisks_linux_filesystem_find_siblings:
 * @filesystem: A #UDisksLinuxFilesystem.
 * @object: The enclosing #UDisksLinuxBlockObject instance.
 *
 * Gets a list of block objects that form a multi-disk filesystem - a volume
 * (in different perspective). Some filesystems need to be mounted first to fully
 * expose their structure and the actual attached components (block devices).
 *
 * This call reaches to the udev database as not all #UDisksLinuxBlockObject objects
 * may have finished processing their uevents or have received none at all. This
 * function helps to identify such block devices and trigger explicit uevents.
 *
 * As for the <literal>btrfs</literal> filesystem this looks for other block
 * devices carrying the particular ID_FS_UUID value of this @filesystem.
 *
 * The returned list doesn't contain @object.
 *
 * Returns: (transfer full) (element-type UDisksObject): A list of #UDisksObject instances. The returned list should be freed with g_list_free() after each element has been freed with g_object_unref().
 */
GList *
udisks_linux_filesystem_find_siblings (UDisksLinuxFilesystem  *filesystem,
                                       UDisksLinuxBlockObject *object)
{
  UDisksDaemon *daemon;
  UDisksLinuxDevice *device;
  GUdevClient *gudev_client;
  GList *list = NULL;

  g_return_val_if_fail (UDISKS_IS_LINUX_FILESYSTEM (filesystem), NULL);
  g_return_val_if_fail (UDISKS_IS_LINUX_BLOCK_OBJECT (object), NULL);

  daemon = udisks_linux_block_object_get_daemon (object);
  gudev_client = udisks_linux_provider_get_udev_client (udisks_daemon_get_linux_provider (daemon));

  /* check the actual filesystem type */
  device = udisks_linux_block_object_get_device (object);
  g_return_val_if_fail (device != NULL, NULL);

  if (g_strcmp0 (g_udev_device_get_property (device->udev_device, "ID_FS_TYPE"), "btrfs") == 0 &&
      g_udev_device_get_property (device->udev_device, "ID_FS_UUID") != NULL)
    {
      /* btrfs case */
      const gchar *fs_uuid;
      GList *devices;
      GList *l;

      /* TODO: look also for ID_BTRFS_READY? */
      fs_uuid = g_udev_device_get_property (device->udev_device, "ID_FS_UUID");

      devices = g_udev_client_query_by_subsystem (gudev_client, "block");
      for (l = devices; l != NULL; l = l->next)
        {
          GUdevDevice *d = G_UDEV_DEVICE (l->data);

          if (g_udev_device_get_is_initialized (d) &&
              g_udev_device_get_device_number (d) != g_udev_device_get_device_number (device->udev_device) &&
              g_strcmp0 (g_udev_device_get_property (d, "ID_FS_TYPE"), "btrfs") == 0 &&
              g_strcmp0 (g_udev_device_get_property (d, "ID_FS_UUID"), fs_uuid) == 0)
            {
              UDisksObject *obj;

              /* TODO: at this point there might not be any corresponding #UDisksObject yet,
               *       shall this function return a list of block device paths instead?
               */
              obj = udisks_daemon_find_block (daemon, g_udev_device_get_device_number (d));
              if (obj)
                list = g_list_append (list, obj);
            }
        }
      g_list_free_full (devices, g_object_unref);
    }

  g_object_unref (device);

  return list;
}

/* ---------------------------------------------------------------------------------------------------- */

static guint64
get_filesystem_size (UDisksLinuxBlockObject *object)
{
  guint64 size = 0;
  UDisksLinuxDevice *device;
  gchar *dev;
  const gchar *type;
  GError *error = NULL;

  device = udisks_linux_block_object_get_device (object);
  dev = udisks_linux_block_object_get_device_file (object);
  type = g_udev_device_get_property (device->udev_device, "ID_FS_TYPE");

  if (g_strcmp0 (type, "ext2") == 0)
    {
      BDFSExt2Info *info = bd_fs_ext2_get_info (dev, &error);
      if (info)
        {
          size = info->block_size * info->block_count;
          bd_fs_ext2_info_free (info);
        }
    }
  else if (g_strcmp0 (type, "ext3") == 0)
    {
      BDFSExt3Info *info = bd_fs_ext3_get_info (dev, &error);
      if (info)
        {
          size = info->block_size * info->block_count;
          bd_fs_ext3_info_free (info);
        }
    }
  else if (g_strcmp0 (type, "ext4") == 0)
    {
      BDFSExt4Info *info = bd_fs_ext4_get_info (dev, &error);
      if (info)
        {
          size = info->block_size * info->block_count;
          bd_fs_ext4_info_free (info);
        }
    }
  else if (g_strcmp0 (type, "xfs") == 0)
    {
      BDFSXfsInfo *info = bd_fs_xfs_get_info (dev, &error);
      if (info)
        {
          size = info->block_size * info->block_count;
          bd_fs_xfs_info_free (info);
        }
    }

  g_free (dev);
  g_object_unref (device);
  g_clear_error (&error);

  return size;
}

static UDisksDriveAta *
get_drive_ata (UDisksLinuxBlockObject *object)
{
  UDisksObject *drive_object = NULL;
  UDisksDriveAta *ata = NULL;
  UDisksBlock *block;

  block = udisks_object_peek_block (UDISKS_OBJECT (object));
  if (block == NULL)
    return NULL;

  drive_object = udisks_daemon_find_object (udisks_linux_block_object_get_daemon (object), udisks_block_get_drive (block));
  if (drive_object == NULL)
    return NULL;

  ata = udisks_object_get_drive_ata (drive_object);

  g_object_unref (drive_object);

  return ata;
}

/**
 * udisks_linux_filesystem_update:
 * @filesystem: A #UDisksLinuxFilesystem.
 * @object: The enclosing #UDisksLinuxBlockObject instance.
 *
 * Updates the interface.
 */
void
udisks_linux_filesystem_update (UDisksLinuxFilesystem  *filesystem,
                                UDisksLinuxBlockObject *object)
{
  UDisksMountMonitor *mount_monitor;
  UDisksLinuxDevice *device;
  UDisksDriveAta *ata = NULL;
  GPtrArray *p;
  GList *mounts;
  GList *l;
  gboolean skip_fs_size = FALSE;
  guchar pm_state;

  mount_monitor = udisks_daemon_get_mount_monitor (udisks_linux_block_object_get_daemon (object));
  device = udisks_linux_block_object_get_device (object);

  /* match by ID_FS_UUID in case of a btrfs filesystem */
  if (udisks_linux_filesystem_is_volume_based (filesystem, object))
    mounts = udisks_mount_monitor_get_mounts_for_fs_uuid (mount_monitor, g_udev_device_get_property (device->udev_device, "ID_FS_UUID"));
  else
    mounts = udisks_mount_monitor_get_mounts_for_dev (mount_monitor, g_udev_device_get_device_number (device->udev_device));

  /* we are guaranteed that the list is sorted so if there are
   * multiple mounts we'll always get the same order
   */
  p = g_ptr_array_new ();
  for (l = mounts; l != NULL; l = l->next)
    {
      UDisksMount *mount = UDISKS_MOUNT (l->data);
      if (udisks_mount_get_mount_type (mount) == UDISKS_MOUNT_TYPE_FILESYSTEM)
        g_ptr_array_add (p, (gpointer) udisks_mount_get_mount_path (mount));
    }
  g_ptr_array_add (p, NULL);
  udisks_filesystem_set_mount_points (UDISKS_FILESYSTEM (filesystem),
                                      (const gchar *const *) p->pdata);
  g_ptr_array_free (p, TRUE);
  g_list_free_full (mounts, g_object_unref);

  /* if the drive is ATA and is sleeping, skip filesystem size check to prevent
   * drive waking up - nothing has changed anyway since it's been sleeping...
   */
  ata = get_drive_ata (object);
  if (ata != NULL)
    {
      if (udisks_linux_drive_ata_get_pm_state (UDISKS_LINUX_DRIVE_ATA (ata), NULL, &pm_state))
        skip_fs_size = ! UDISKS_LINUX_DRIVE_ATA_IS_AWAKE (pm_state);
    }
  g_clear_object (&ata);

  if (! skip_fs_size)
    udisks_filesystem_set_size (UDISKS_FILESYSTEM (filesystem), get_filesystem_size (object));

  g_dbus_interface_skeleton_flush (G_DBUS_INTERFACE_SKELETON (filesystem));

  g_object_unref (device);
}

/* ---------------------------------------------------------------------------------------------------- */

static const gchar *well_known_filesystems[] =
{
  "btrfs",
  "ext2",
  "ext3",
  "ext4",
  "udf",
  "iso9660",
  "xfs",
  "jfs",
  "nilfs",
  "reiserfs",
  "reiser4",
  "msdos",
  "umsdos",
  "vfat",
  "exfat",
  "ntfs",
  NULL,
};

static gboolean
is_in_filesystem_file (const gchar *filesystems_file,
                       const gchar *fstype)
{
  gchar *filesystems = NULL;
  GError *error = NULL;
  gboolean ret = FALSE;
  gchar **lines = NULL;
  guint n;

  if (!g_file_get_contents (filesystems_file,
                            &filesystems,
                            NULL, /* gsize *out_length */
                            &error))
    {
      udisks_warning ("Error reading %s: %s (%s %d)",
                      filesystems_file,
                      error->message,
                      g_quark_to_string (error->domain),
                      error->code);
      g_clear_error (&error);
      goto out;
    }

  lines = g_strsplit (filesystems, "\n", -1);
  for (n = 0; lines != NULL && lines[n] != NULL && !ret; n++)
    {
      gchar **tokens;
      gint num_tokens;
      g_strdelimit (lines[n], " \t", ' ');
      g_strstrip (lines[n]);
      tokens = g_strsplit (lines[n], " ", -1);
      num_tokens = g_strv_length (tokens);
      if (num_tokens == 1 && g_strcmp0 (tokens[0], fstype) == 0)
        {
          ret = TRUE;
        }
      g_strfreev (tokens);
    }

 out:
  g_strfreev (lines);
  g_free (filesystems);
  return ret;
}

static gboolean
is_well_known_filesystem (const gchar *fstype)
{
  gboolean ret = FALSE;
  guint n;

  for (n = 0; well_known_filesystems[n] != NULL; n++)
    {
      if (g_strcmp0 (well_known_filesystems[n], fstype) == 0)
        {
          ret = TRUE;
          goto out;
        }
    }
 out:
  return ret;
}

/* this is not a very efficient implementation but it's very rarely
 * called so no real point in optimizing it...
 */
static gboolean
is_allowed_filesystem (const gchar *fstype)
{
  return is_well_known_filesystem (fstype) ||
    is_in_filesystem_file ("/proc/filesystems", fstype) ||
    is_in_filesystem_file ("/etc/filesystems", fstype);
}

/* ---------------------------------------------------------------------------------------------------- */

/*
 * calculate_fs_type: <internal>
 * @block: A #UDisksBlock.
 * @given_options: The a{sv} #GVariant.
 * @error: Return location for error or %NULL.
 *
 * Calculates the file system type to use.
 *
 * Returns: A valid UTF-8 string with the filesystem type (may be "auto") or %NULL if @error is set. Free with g_free().
 */
static gchar *
calculate_fs_type (UDisksBlock  *block,
                   GVariant     *given_options,
                   GError      **error)
{
  gchar *fs_type_to_use = NULL;
  const gchar *probed_fs_type = NULL;
  const gchar *requested_fs_type;

  probed_fs_type = NULL;
  if (block != NULL)
    probed_fs_type = udisks_block_get_id_type (block);

  if (g_variant_lookup (given_options,
                        "fstype",
                        "&s", &requested_fs_type) &&
      strlen (requested_fs_type) > 0)
    {
      /* If the user requests the filesystem type, error out unless the
       * filesystem type is
       *
       * - well-known [1]; or
       * - in the /proc/filesystems file; or
       * - in the /etc/filesystems file
       *
       * in that order. We do this because mount(8) on Linux allows
       * loading any arbitrary kernel module (when invoked as root) by
       * passing something appropriate to the -t option. So we have to
       * validate whatever we pass...
       *
       * See https://bugs.freedesktop.org/show_bug.cgi?id=32232 for more
       * details.
       *
       * [1] : since /etc/filesystems may be horribly out of date and
       *       not contain e.g. ext4
       */
      if (g_strcmp0 (requested_fs_type, "auto") != 0)
        {
          if (!is_allowed_filesystem (requested_fs_type))
            {
              g_set_error (error,
                           UDISKS_ERROR,
                           UDISKS_ERROR_OPTION_NOT_PERMITTED,
                           "Requested filesystem type `%s' is neither well-known nor "
                           "in /proc/filesystems nor in /etc/filesystems",
                           requested_fs_type);
              goto out;
            }
        }

      /* TODO: maybe check that it's compatible with probed_fs_type */
      fs_type_to_use = g_strdup (requested_fs_type);
    }
  else
    {
      if (probed_fs_type != NULL && strlen (probed_fs_type) > 0)
        fs_type_to_use = g_strdup (probed_fs_type);
      else
        fs_type_to_use = g_strdup ("auto");
    }

 out:
  g_assert (fs_type_to_use == NULL || g_utf8_validate (fs_type_to_use, -1, NULL));

  return fs_type_to_use;
}

/* ---------------------------------------------------------------------------------------------------- */

static gchar *
ensure_utf8 (const gchar *s)
{
  const gchar *end;
  gchar *ret;

  if (!g_utf8_validate (s, -1, &end))
    {
      gchar *tmp;
      gint pos;
      /* TODO: could possibly return a nicer UTF-8 string  */
      pos = (gint) (end - s);
      tmp = g_strndup (s, end - s);
      ret = g_strdup_printf ("%s (Invalid UTF-8 at byte %d)", tmp, pos);
      g_free (tmp);
    }
  else
    {
      ret = g_strdup (s);
    }

  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

#ifdef HAVE_ACL
static gboolean
add_acl (const gchar  *path,
         uid_t         uid,
         GError      **error)
{
  gboolean ret = FALSE;
  acl_t acl = NULL;
  acl_entry_t entry;
  acl_permset_t permset;

  acl = acl_get_file(path, ACL_TYPE_ACCESS);
  if (acl == NULL ||
      acl_create_entry (&acl, &entry) == -1 ||
      acl_set_tag_type (entry, ACL_USER) == -1 ||
      acl_set_qualifier (entry, &uid) == -1 ||
      acl_get_permset (entry, &permset) == -1 ||
      acl_add_perm (permset, ACL_READ|ACL_EXECUTE) == -1 ||
      acl_calc_mask (&acl) == -1 ||
      acl_set_file (path, ACL_TYPE_ACCESS, acl) == -1)
    {
      udisks_warning(
                   "Adding read ACL for uid %d to `%s' failed: %m",
                   (gint) uid, path);
      chown(path, uid, -1);
    }

  ret = TRUE;

  if (acl != NULL)
    acl_free (acl);
  return ret;
}
#endif

/*
 * calculate_mount_point: <internal>
 * @daemon: A #UDisksDaemon.
 * @block: A #UDisksBlock.
 * @uid: user id of the calling user
 * @gid: group id of the calling user
 * @user_name: user name of the calling user
 * @fs_type: The file system type to mount with
 * @persistent: if the mount point is persistent (survives reboot) or not
 * @error: Return location for error or %NULL.
 *
 * Calculates the mount point to use.
 *
 * Returns: A UTF-8 string with the mount point to use or %NULL if @error is set. Free with g_free().
 */
static gchar *
calculate_mount_point (UDisksDaemon  *daemon,
                       UDisksBlock   *block,
                       uid_t          uid,
                       gid_t          gid,
                       const gchar   *user_name,
                       const gchar   *fs_type,
                       gboolean      *persistent,
                       GError       **error)
{
  UDisksLinuxBlockObject *object = NULL;
  gboolean fs_shared = FALSE;
  const gchar *label = NULL;
  const gchar *uuid = NULL;
  gchar *escaped_user_name = NULL;
  gchar *mount_dir = NULL;
  gchar *mount_point = NULL;
  gchar *orig_mount_point;
  GString *str;
  gchar *s;
  guint n;

  label = NULL;
  uuid = NULL;
  if (block != NULL)
    {
      label = udisks_block_get_id_label (block);
      uuid = udisks_block_get_id_uuid (block);
    }

  object = udisks_daemon_util_dup_object (block, NULL);
  if (object != NULL)
    {
      UDisksLinuxDevice *device = udisks_linux_block_object_get_device (object);
      if (device != NULL)
        {
          if (device->udev_device != NULL)
            {
              /* TODO: maybe introduce Block:HintFilesystemShared instead of pulling it directly from the udev device */
              fs_shared = g_udev_device_get_property_as_boolean (device->udev_device, "UDISKS_FILESYSTEM_SHARED");
            }
          g_object_unref (device);
        }
    }

  /* If we know the user-name and it doesn't have any '/' character in
   * it, mount in MOUNT_BASE/$USER
   */
  if (!fs_shared && (user_name != NULL && strstr (user_name, "/") == NULL))
    {
      mount_dir = g_strdup_printf (MOUNT_BASE "/%s", user_name);
      *persistent = MOUNT_BASE_PERSISTENT;
      if (!g_file_test (mount_dir, G_FILE_TEST_EXISTS))
        {
          /* First ensure that MOUNT_BASE exists */
          if (g_mkdir (MOUNT_BASE, 0755) != 0 && errno != EEXIST)
            {
              g_set_error (error,
                           UDISKS_ERROR,
                           UDISKS_ERROR_FAILED,
                           "Error creating directory " MOUNT_BASE ": %m");
              goto out;
            }
          /* Then create the per-user MOUNT_BASE/$USER */
#ifdef HAVE_ACL
          if (g_mkdir (mount_dir, 0700) != 0 && errno != EEXIST)
#else
          if (g_mkdir (mount_dir, 0750) != 0 && errno != EEXIST)
#endif
            {
              g_set_error (error,
                           UDISKS_ERROR,
                           UDISKS_ERROR_FAILED,
                           "Error creating directory `%s': %m",
                           mount_dir);
              goto out;
            }
          /* Finally, add the read+execute ACL for $USER */
#ifdef HAVE_ACL
          if (!add_acl (mount_dir, uid, error))
            {
#else
          if (chown (mount_dir, -1, gid) == -1)
            {
               g_set_error (error, G_IO_ERROR,
                            g_io_error_from_errno (errno),
                            "Failed to change gid to %d for %s: %m",
                            (gint) gid, mount_dir);
#endif
              if (rmdir (mount_dir) != 0)
                udisks_warning ("Error calling rmdir() on %s: %m", mount_dir);
              goto out;
            }
        }
    }
  /* otherwise fall back to mounting in /media */
  if (mount_dir == NULL)
    {
      mount_dir = g_strdup ("/media");
      *persistent = TRUE;
    }

  /* NOTE: UTF-8 has the nice property that valid UTF-8 strings only contains
   *       the byte 0x2F if it's for the '/' character (U+002F SOLIDUS).
   *
   *       See http://en.wikipedia.org/wiki/UTF-8 for details.
   */
  if (label != NULL && strlen (label) > 0)
    {
      str = g_string_new (NULL);
      g_string_append_printf (str, "%s/", mount_dir);
      s = ensure_utf8 (label);
      for (n = 0; s[n] != '\0'; n++)
        {
          gint c = s[n];
          if (c == '/')
            g_string_append_c (str, '_');
          else
            g_string_append_c (str, c);
        }
      mount_point = g_string_free (str, FALSE);
      g_free (s);
    }
  else if (uuid != NULL && strlen (uuid) > 0)
    {
      str = g_string_new (NULL);
      g_string_append_printf (str, "%s/", mount_dir);
      s = ensure_utf8 (uuid);
      for (n = 0; s[n] != '\0'; n++)
        {
          gint c = s[n];
          if (c == '/')
            g_string_append_c (str, '_');
          else
            g_string_append_c (str, c);
        }
      mount_point = g_string_free (str, FALSE);
      g_free (s);
    }
  else
    {
      mount_point = g_strdup_printf ("%s/disk", mount_dir);
    }

  /* ... then uniqify the mount point */
  orig_mount_point = g_strdup (mount_point);
  n = 1;
  while (TRUE)
    {
      if (!g_file_test (mount_point, G_FILE_TEST_EXISTS))
        {
          break;
        }
      else
        {
          g_free (mount_point);
          mount_point = g_strdup_printf ("%s%u", orig_mount_point, n++);
        }
    }
  g_free (orig_mount_point);

 out:
  g_free (mount_dir);
  g_clear_object (&object);
  g_free (escaped_user_name);
  return mount_point;
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
has_option (const gchar *options,
            const gchar *option)
{
  gboolean ret = FALSE;
  gchar **tokens;
  guint n;

  tokens = g_strsplit (options, ",", -1);
  for (n = 0; tokens != NULL && tokens[n] != NULL; n++)
    {
      if (g_strcmp0 (tokens[n], option) == 0)
        {
          ret = TRUE;
          goto out;
        }
    }

 out:
  g_strfreev (tokens);
  return ret;
}

/* returns TRUE if, and only if, device is referenced in e.g. /etc/fstab
 */
static gboolean
is_system_managed (UDisksDaemon *daemon,
                   UDisksBlock  *block,
                   gchar       **out_mount_point,
                   gchar       **out_mount_options)
{
  UDisksMountMonitor *mount_monitor = udisks_daemon_get_mount_monitor (daemon);
  gboolean ret = FALSE;
  struct libmnt_table *table;
  struct libmnt_iter* iter;
  struct libmnt_fs *fs = NULL;

  table = mnt_new_table ();
  if (mnt_table_parse_fstab (table, NULL) < 0)
    {
      mnt_free_table (table);
      return FALSE;
    }

  iter = mnt_new_iter (MNT_ITER_FORWARD);
  while (mnt_table_next_fs (table, iter, &fs) == 0)
    {
      if (udisks_linux_block_matches_id (UDISKS_LINUX_BLOCK (block), mnt_fs_get_source (fs)))
        {
          /* If this block device is found in fstab, but something else is already
           * mounted on that mount point, ignore the fstab entry.
           */
          UDisksMount *mount = udisks_mount_monitor_get_mount_for_path (mount_monitor, mnt_fs_get_target (fs));
          if (mount == NULL || udisks_block_get_device_number (block) == udisks_mount_get_dev (mount))
            {
              ret = TRUE;
              if (out_mount_point != NULL)
                *out_mount_point = g_strdup (mnt_fs_get_target (fs));
              if (out_mount_options != NULL)
                *out_mount_options = mnt_fs_strdup_options (fs);
            }

          g_clear_object (&mount);
          if (ret)
            break;
        }
    }
  mnt_free_iter (iter);
  mnt_free_table (table);

  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

/* runs in thread dedicated to handling @invocation */
static gboolean
handle_mount (UDisksFilesystem      *filesystem,
              GDBusMethodInvocation *invocation,
              GVariant              *options)
{
  UDisksObject *object = NULL;
  UDisksBlock *block;
  UDisksDaemon *daemon;
  UDisksState *state = NULL;
  uid_t caller_uid;
  gid_t caller_gid;
  const gchar * const *existing_mount_points;
  const gchar *probed_fs_usage;
  gchar *fs_type_to_use = NULL;
  gchar *mount_options_to_use = NULL;
  gchar *mount_point_to_use = NULL;
  gboolean mpoint_persistent = TRUE;
  gchar *fstab_mount_options = NULL;
  gchar *caller_user_name = NULL;
  GError *error = NULL;
  const gchar *action_id = NULL;
  const gchar *message = NULL;
  gboolean system_managed = FALSE;
  gboolean success = FALSE;
  gchar *device = NULL;
  UDisksBaseJob *job = NULL;
  gboolean volume_based_fs;


  /* only allow a single call at a time */
  g_mutex_lock (&UDISKS_LINUX_FILESYSTEM (filesystem)->lock);

  object = udisks_daemon_util_dup_object (filesystem, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  block = udisks_object_peek_block (object);
  daemon = udisks_linux_block_object_get_daemon (UDISKS_LINUX_BLOCK_OBJECT (object));
  state = udisks_daemon_get_state (daemon);
  device = udisks_block_dup_device (block);

  volume_based_fs = udisks_linux_filesystem_is_volume_based (UDISKS_LINUX_FILESYSTEM (filesystem),
                                                             UDISKS_LINUX_BLOCK_OBJECT (object));

  /* perform state cleanup to avoid duplicate entries for this block device */
  udisks_linux_block_object_lock_for_cleanup (UDISKS_LINUX_BLOCK_OBJECT (object));
  udisks_state_check_block (state, udisks_linux_block_object_get_device_number (UDISKS_LINUX_BLOCK_OBJECT (object)));

  /* check if mount point is managed by e.g. /etc/fstab or similar */
  if (is_system_managed (daemon, block, &mount_point_to_use, &fstab_mount_options))
    {
      system_managed = TRUE;
    }

  /* First, fail if the device is already mounted */
  existing_mount_points = udisks_filesystem_get_mount_points (filesystem);
  if (existing_mount_points != NULL && g_strv_length ((gchar **) existing_mount_points) > 0)
    {
      GString *str;
      guint n;
      str = g_string_new (NULL);
      for (n = 0; existing_mount_points[n] != NULL; n++)
        {
          if (n > 0)
            g_string_append (str, ", ");
          g_string_append_printf (str, "`%s'", existing_mount_points[n]);
        }
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_ALREADY_MOUNTED,
                                             "Device %s is already mounted at %s.\n",
                                             device,
                                             str->str);
      g_string_free (str, TRUE);
      goto out;
    }

  if (!udisks_daemon_util_get_caller_uid_sync (daemon,
                                               invocation,
                                               NULL /* GCancellable */,
                                               &caller_uid,
                                               &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      g_clear_error (&error);
      goto out;
    }

  if (!udisks_daemon_util_get_user_info (caller_uid, &caller_gid, &caller_user_name, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      g_clear_error (&error);
      goto out;
    }

  if (system_managed)
    {
      gboolean mount_fstab_as_root = FALSE;

      if (!has_option (fstab_mount_options, "x-udisks-auth") &&
          !has_option (fstab_mount_options, "user") &&
          !has_option (fstab_mount_options, "users"))
        {
          action_id = "org.freedesktop.udisks2.filesystem-mount";
          /* Translators: Shown in authentication dialog when the user
           * requests mounting a filesystem.
           *
           * Do not translate $(drive), it's a placeholder and
           * will be replaced by the name of the drive/device in question
           */
          message = N_("Authentication is required to mount $(drive)");
          if (!udisks_daemon_util_setup_by_user (daemon, object, caller_uid))
            {
              if (udisks_block_get_hint_system (block))
                {
                  action_id = "org.freedesktop.udisks2.filesystem-mount-system";
                }
              else if (!udisks_daemon_util_on_user_seat (daemon, object, caller_uid))
                {
                  action_id = "org.freedesktop.udisks2.filesystem-mount-other-seat";
                }
            }

          if (!udisks_daemon_util_check_authorization_sync (daemon,
                                                            object,
                                                            action_id,
                                                            options,
                                                            message,
                                                            invocation))
            goto out;
          mount_fstab_as_root = TRUE;
        }

      if (!g_file_test (mount_point_to_use, G_FILE_TEST_IS_DIR))
        {
          if (g_mkdir_with_parents (mount_point_to_use, 0755) != 0)
            {
              g_dbus_method_invocation_return_error (invocation,
                                                     UDISKS_ERROR,
                                                     UDISKS_ERROR_FAILED,
                                                     "Error creating directory `%s' to be used for mounting %s: %m",
                                                     mount_point_to_use,
                                                     device);
              goto out;
            }
        }

    mount_fstab_again:
      job = udisks_daemon_launch_simple_job (daemon,
                                             UDISKS_OBJECT (object),
                                             "filesystem-mount",
                                             mount_fstab_as_root ? 0 : caller_uid,
                                             NULL /* cancellable */);

      /* XXX: using run_as_uid for root doesn't work even if the caller is already root */
      if (!mount_fstab_as_root && caller_uid != 0)
        {
          BDExtraArg uid_arg = {g_strdup ("run_as_uid"), g_strdup_printf("%d", caller_uid)};
          BDExtraArg gid_arg = {g_strdup ("run_as_gid"), g_strdup_printf("%d", caller_gid)};
          const BDExtraArg *extra_args[3] = {&uid_arg, &gid_arg, NULL};

          success = bd_fs_mount (NULL, mount_point_to_use, NULL, NULL, extra_args, &error);

          g_free (uid_arg.opt);
          g_free (uid_arg.val);
          g_free (gid_arg.opt);
          g_free (gid_arg.val);
        }
      else
        {
          success = bd_fs_mount (NULL, mount_point_to_use, NULL, NULL, NULL, &error);
        }

      if (!success)
          udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), FALSE, error->message);
      else
          udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), TRUE, NULL);

      if (!success)
        {
          if (!mount_fstab_as_root && g_error_matches (error, BD_FS_ERROR, BD_FS_ERROR_AUTH))
            {
              g_clear_error (&error);
              if (!udisks_daemon_util_check_authorization_sync (daemon,
                                                                object,
                                                                "org.freedesktop.udisks2.filesystem-fstab",
                                                                options,
                                                                /* Translators: Shown in authentication dialog when the
                                                                 * user requests mounting a filesystem that is in
                                                                 * /etc/fstab file with the x-udisks-auth option.
                                                                 *
                                                                 * Do not translate $(drive), it's a
                                                                 * placeholder and will be replaced by the name of
                                                                 * the drive/device in question
                                                                 *
                                                                 * Do not translate /etc/fstab
                                                                 */
                                                                N_("Authentication is required to mount $(drive) referenced in the /etc/fstab file"),
                                                                invocation))
                goto out;
              mount_fstab_as_root = TRUE;
              goto mount_fstab_again;
            }

          g_dbus_method_invocation_return_error (invocation,
                                                 UDISKS_ERROR,
                                                 UDISKS_ERROR_FAILED,
                                                 "Error mounting system-managed device %s: %s",
                                                 device,
                                                 error->message);
          g_clear_error (&error);
          goto out;
        }
      udisks_notice ("Mounted %s (system) at %s on behalf of uid %u",
                     device,
                     mount_point_to_use,
                     caller_uid);

      /* trigger uevent on the master device and all its siblings in case of a multidisk volume */
      udisks_linux_block_object_trigger_uevent_sync (UDISKS_LINUX_BLOCK_OBJECT (object),
                                                     UDISKS_DEFAULT_WAIT_TIMEOUT);
      if (volume_based_fs)
        {
          GList *siblings;
          GList *l;

          siblings = udisks_linux_filesystem_find_siblings (UDISKS_LINUX_FILESYSTEM (filesystem),
                                                            UDISKS_LINUX_BLOCK_OBJECT (object));
          for (l = siblings; l != NULL; l = l->next)
            {
              UDisksLinuxBlockObject *obj = UDISKS_LINUX_BLOCK_OBJECT (l->data);
              /* TODO: parallelize */
              udisks_linux_block_object_trigger_uevent_sync (obj, UDISKS_DEFAULT_WAIT_TIMEOUT);
            }
          g_list_free_full (siblings, g_object_unref);
        }

      /* update the mounted-fs file */
      udisks_state_add_mounted_fs (state,
                                   mount_point_to_use,
                                   volume_based_fs ? 0 : udisks_block_get_device_number (block),
                                   volume_based_fs ? udisks_block_get_id_uuid (block) : NULL,
                                   caller_uid,
                                   TRUE,   /* fstab_mounted */
                                   FALSE); /* persistent */

      udisks_filesystem_complete_mount (filesystem, invocation, mount_point_to_use);
      goto out;
    }

  /* Then fail if the device is not mountable - we actually allow mounting
   * devices that are not probed since since it could be that we just
   * don't have the data in the udev database but the device has a
   * filesystem *anyway*...
   *
   * For example, this applies to PC floppy devices - automatically
   * probing for media them creates annoying noise. So they won't
   * appear in the udev database.
   */
  probed_fs_usage = NULL;
  if (block != NULL)
    probed_fs_usage = udisks_block_get_id_usage (block);
  if (probed_fs_usage != NULL && strlen (probed_fs_usage) > 0 &&
      g_strcmp0 (probed_fs_usage, "filesystem") != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Cannot mount block device %s with probed usage `%s' - expected `filesystem'",
                                             device,
                                             probed_fs_usage);
      goto out;
    }

  /* calculate filesystem type (guaranteed to be valid UTF-8) */
  fs_type_to_use = calculate_fs_type (block,
                                      options,
                                      &error);
  if (fs_type_to_use == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      g_clear_error (&error);
      goto out;
    }

  /* calculate mount options (guaranteed to be valid UTF-8) */
  mount_options_to_use = udisks_linux_calculate_mount_options (daemon,
                                                               block,
                                                               caller_uid,
                                                               fs_type_to_use,
                                                               options,
                                                               &error);
  if (mount_options_to_use == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      g_clear_error (&error);
      goto out;
    }

  /* Now, check that the user is actually authorized to mount the
   * device. Need to do this before calculating a mount point since we
   * may be racing with other threads...
   */
  action_id = "org.freedesktop.udisks2.filesystem-mount";
  /* Translators: Shown in authentication dialog when the user
   * requests mounting a filesystem.
   *
   * Do not translate $(drive), it's a placeholder and
   * will be replaced by the name of the drive/device in question
   */
  message = N_("Authentication is required to mount $(drive)");
  if (!udisks_daemon_util_setup_by_user (daemon, object, caller_uid))
    {
      if (udisks_block_get_hint_system (block))
        {
          action_id = "org.freedesktop.udisks2.filesystem-mount-system";
        }
      else if (!udisks_daemon_util_on_user_seat (daemon, object, caller_uid))
        {
          action_id = "org.freedesktop.udisks2.filesystem-mount-other-seat";
        }
    }

  if (!udisks_daemon_util_check_authorization_sync (daemon,
                                                    object,
                                                    action_id,
                                                    options,
                                                    message,
                                                    invocation))
    goto out;

  /* calculate mount point (guaranteed to be valid UTF-8) */
  mount_point_to_use = calculate_mount_point (daemon,
                                              block,
                                              caller_uid,
                                              caller_gid,
                                              caller_user_name,
                                              fs_type_to_use,
                                              &mpoint_persistent,
                                              &error);
  if (mount_point_to_use == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      g_clear_error (&error);
      goto out;
    }

  /* create the mount point */
  if (g_mkdir (mount_point_to_use, 0700) != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Error creating mount point `%s': %m",
                                             mount_point_to_use);
      goto out;
    }

  job = udisks_daemon_launch_simple_job (daemon,
                                         UDISKS_OBJECT (object),
                                         "filesystem-mount",
                                         0,
                                         NULL /* cancellable */);

  if (g_strcmp0 (fs_type_to_use, "ntfs") == 0)
    {
      /* Prefer the new 'ntfs3' kernel driver and fall back to generic 'ntfs'
       * (ntfs3g or the legacy kernel 'ntfs' driver) if not available.
       */
      g_free (fs_type_to_use);
      fs_type_to_use = g_strdup ("ntfs3,ntfs");
    }

  if (!bd_fs_mount (device, mount_point_to_use, fs_type_to_use, mount_options_to_use, NULL, &error))
    {
      /* ugh, something went wrong.. we need to clean up the created mount point */
      if (g_rmdir (mount_point_to_use) != 0)
        udisks_warning ("Error removing directory %s: %m", mount_point_to_use);

      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Error mounting %s at %s: %s",
                                             device,
                                             mount_point_to_use,
                                             error->message);
      udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), FALSE, error->message);
      g_clear_error (&error);
      goto out;
    }
  else
    udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), TRUE, NULL);

  /* update the mounted-fs file */
  udisks_state_add_mounted_fs (state,
                               mount_point_to_use,
                               volume_based_fs ? 0 : udisks_block_get_device_number (block),
                               volume_based_fs ? udisks_block_get_id_uuid (block) : NULL,
                               caller_uid,
                               FALSE,  /* fstab_mounted */
                               mpoint_persistent);

  udisks_notice ("Mounted %s at %s on behalf of uid %u",
                 device,
                 mount_point_to_use,
                 caller_uid);

  /* trigger uevent on the master device and all its siblings in case of a multidisk volume */
  udisks_linux_block_object_trigger_uevent_sync (UDISKS_LINUX_BLOCK_OBJECT (object),
                                                 UDISKS_DEFAULT_WAIT_TIMEOUT);
  if (volume_based_fs)
    {
      GList *siblings;
      GList *l;

      siblings = udisks_linux_filesystem_find_siblings (UDISKS_LINUX_FILESYSTEM (filesystem),
                                                        UDISKS_LINUX_BLOCK_OBJECT (object));
      for (l = siblings; l != NULL; l = l->next)
        {
          UDisksLinuxBlockObject *obj = UDISKS_LINUX_BLOCK_OBJECT (l->data);
          /* TODO: parallelize */
          udisks_linux_block_object_trigger_uevent_sync (obj, UDISKS_DEFAULT_WAIT_TIMEOUT);
        }
      g_list_free_full (siblings, g_object_unref);
    }

  udisks_filesystem_complete_mount (filesystem, invocation, mount_point_to_use);

 out:
  if (object != NULL)
    udisks_linux_block_object_release_cleanup_lock (UDISKS_LINUX_BLOCK_OBJECT (object));
  if (state != NULL)
    udisks_state_check (state);
  g_free (fs_type_to_use);
  g_free (mount_options_to_use);
  g_free (mount_point_to_use);
  g_free (fstab_mount_options);
  g_free (caller_user_name);
  g_free (device);
  g_clear_object (&object);

  /* only allow a single call at a time */
  g_mutex_unlock (&UDISKS_LINUX_FILESYSTEM (filesystem)->lock);

  return TRUE; /* returning TRUE means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

static guint
get_error_code_for_umount (const gchar *error_message)
{
  if (strstr (error_message, "busy") != NULL)
    return UDISKS_ERROR_DEVICE_BUSY;
  else
    return UDISKS_ERROR_FAILED;
}

typedef struct
{
  const gchar *object_path;
  guint64      old_size;
  gchar       *mount_point;
} WaitForFilesystemMountPointsData;

static UDisksObject *
wait_for_filesystem_mount_points (UDisksDaemon *daemon,
                                  gpointer      user_data)
{
  WaitForFilesystemMountPointsData *data = user_data;
  UDisksObject *object = NULL;
  UDisksFilesystem *filesystem = NULL;
  const gchar * const *mount_points = NULL;

  object = udisks_daemon_find_object (daemon, data->object_path);

  if (object != NULL)
    {
      filesystem = udisks_object_peek_filesystem (object);
      if (filesystem != NULL)
        {
          mount_points = udisks_filesystem_get_mount_points (filesystem);
        }
      /* If we know which mount point should have gone, directly test for it, otherwise test if any has gone */
      if (mount_points != NULL && ((data->mount_point != NULL && g_strv_contains (mount_points, data->mount_point)) ||
          g_strv_length ((gchar **) mount_points) == data->old_size))
        {
          g_clear_object (&object);
        }
    }

  return object;
}

/* runs in thread dedicated to handling @invocation */
static gboolean
handle_unmount (UDisksFilesystem      *filesystem,
                GDBusMethodInvocation *invocation,
                GVariant              *options)
{
  UDisksObject *object;
  UDisksBlock *block;
  UDisksDaemon *daemon;
  UDisksState *state = NULL;
  gchar *mount_point = NULL;
  gchar *fstab_mount_options = NULL;
  GError *error = NULL;
  uid_t mounted_by_uid;
  uid_t caller_uid;
  gid_t caller_gid;
  const gchar *const *mount_points;
  gboolean opt_force = FALSE;
  gboolean system_managed = FALSE;
  gboolean fstab_mounted;
  gboolean success;
  UDisksBaseJob *job = NULL;
  UDisksObject *filesystem_object = NULL;
  WaitForFilesystemMountPointsData wait_data = {NULL, 0, NULL};
  gboolean volume_based_fs;

  /* only allow a single call at a time */
  g_mutex_lock (&UDISKS_LINUX_FILESYSTEM (filesystem)->lock);

  object = udisks_daemon_util_dup_object (filesystem, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  block = udisks_object_peek_block (object);
  daemon = udisks_linux_block_object_get_daemon (UDISKS_LINUX_BLOCK_OBJECT (object));
  state = udisks_daemon_get_state (daemon);

  volume_based_fs = udisks_linux_filesystem_is_volume_based (UDISKS_LINUX_FILESYSTEM (filesystem),
                                                             UDISKS_LINUX_BLOCK_OBJECT (object));
  udisks_linux_block_object_lock_for_cleanup (UDISKS_LINUX_BLOCK_OBJECT (object));
  /* trigger state cleanup so that we match actual mountpoint */
  udisks_state_check_block (state, udisks_linux_block_object_get_device_number (UDISKS_LINUX_BLOCK_OBJECT (object)));

  if (options != NULL)
    {
      g_variant_lookup (options,
                        "force",
                        "b",
                        &opt_force);
    }

  mount_points = udisks_filesystem_get_mount_points (filesystem);
  if (mount_points == NULL || g_strv_length ((gchar **) mount_points) == 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_NOT_MOUNTED,
                                             "Device `%s' is not mounted",
                                             udisks_block_get_device (block));
      goto out;
    }

  wait_data.object_path = g_dbus_object_get_object_path (G_DBUS_OBJECT (object));
  wait_data.old_size = g_strv_length ((gchar **) mount_points);

  if (!udisks_daemon_util_get_caller_uid_sync (daemon, invocation, NULL, &caller_uid, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      g_clear_error (&error);
      goto out;
    }
  if (!udisks_daemon_util_get_user_info (caller_uid, &caller_gid, NULL, &error))
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  /* check if mount point is managed by e.g. /etc/fstab or similar */
  if (is_system_managed (daemon, block, &mount_point, &fstab_mount_options))
    {
      system_managed = TRUE;
    }

  /* if system-managed (e.g. referenced in /etc/fstab or similar) and
   * with the option x-udisks-auth or user(s), just run umount(8) as the
   * calling user
   */
  if (system_managed && (has_option (fstab_mount_options, "x-udisks-auth") ||
                         has_option (fstab_mount_options, "users") ||
                         has_option (fstab_mount_options, "user")))
    {
      gboolean unmount_fstab_as_root;

      unmount_fstab_as_root = FALSE;
    unmount_fstab_again:

      job = udisks_daemon_launch_simple_job (daemon,
                                             UDISKS_OBJECT (object),
                                             "filesystem-unmount",
                                             unmount_fstab_as_root ? 0 : caller_uid,
                                             NULL);

      if (!unmount_fstab_as_root && caller_uid != 0)
        {
          BDExtraArg uid_arg = {g_strdup ("run_as_uid"), g_strdup_printf("%d", caller_uid)};
          BDExtraArg gid_arg = {g_strdup ("run_as_gid"), g_strdup_printf("%d", caller_gid)};
          const BDExtraArg *extra_args[3] = {&uid_arg, &gid_arg, NULL};

          success = bd_fs_unmount (mount_point, opt_force, FALSE, extra_args, &error);

          g_free (uid_arg.opt);
          g_free (uid_arg.val);
          g_free (gid_arg.opt);
          g_free (gid_arg.val);
        }
      else
          success = bd_fs_unmount (mount_point, opt_force, FALSE, NULL, &error);

      if (!success)
          udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), FALSE, error->message);
      else
          udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), TRUE, NULL);

      if (!success)
        {
          if (!unmount_fstab_as_root && error->code == BD_FS_ERROR_AUTH)
            {
              g_clear_error (&error);
              if (!udisks_daemon_util_check_authorization_sync (daemon,
                                                                object,
                                                                "org.freedesktop.udisks2.filesystem-fstab",
                                                                options,
                                                                /* Translators: Shown in authentication dialog when the
                                                                 * user requests unmounting a filesystem that is in
                                                                 * /etc/fstab file with the x-udisks-auth option.
                                                                 *
                                                                 * Do not translate $(drive), it's a
                                                                 * placeholder and will be replaced by the name of
                                                                 * the drive/device in question
                                                                 *
                                                                 * Do not translate /etc/fstab
                                                                 */
                                                                N_("Authentication is required to unmount $(drive) referenced in the /etc/fstab file"),
                                                                invocation))
                goto out;
              unmount_fstab_as_root = TRUE;
              goto unmount_fstab_again;
            }

          g_dbus_method_invocation_return_error (invocation,
                                                 UDISKS_ERROR,
                                                 get_error_code_for_umount (error->message),
                                                 "Error unmounting system-managed device %s: %s",
                                                 udisks_block_get_device (block),
                                                 error->message);
          g_clear_error (&error);

          goto out;
        }
      udisks_notice ("Unmounted %s (system) from %s on behalf of uid %u",
                     udisks_block_get_device (block),
                     mount_point,
                     caller_uid);
      goto waiting;
    }

  g_clear_pointer (&mount_point, g_free);
  mount_point = udisks_state_find_mounted_fs (state,
                                              volume_based_fs ? 0 : udisks_block_get_device_number (block),
                                              volume_based_fs ? udisks_block_get_id_uuid (block) : NULL,
                                              &mounted_by_uid,
                                              &fstab_mounted);
  if (mount_point == NULL)
    {
      /* allow unmounting stuff not mentioned in mounted-fs, but treat it like root mounted it */
      mounted_by_uid = 0;
    }

  if (caller_uid != 0 && (caller_uid != mounted_by_uid))
    {
      const gchar *action_id;
      const gchar *message;

      action_id = "org.freedesktop.udisks2.filesystem-unmount-others";
      /* Translators: Shown in authentication dialog when the user
       * requests unmounting a filesystem previously mounted by
       * another user.
       *
       * Do not translate $(drive), it's a placeholder and
       * will be replaced by the name of the drive/device in question
       */
      message = N_("Authentication is required to unmount $(drive) mounted by another user");

      if (!udisks_daemon_util_check_authorization_sync (daemon,
                                                        object,
                                                        action_id,
                                                        options,
                                                        message,
                                                        invocation))
        goto out;
    }

  job = udisks_daemon_launch_simple_job (daemon,
                                         UDISKS_OBJECT (object),
                                         "filesystem-unmount",
                                         0,
                                         NULL);

  if (!bd_fs_unmount (mount_point ? mount_point : udisks_block_get_device (block),
                      opt_force, FALSE, NULL, &error))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             get_error_code_for_umount (error->message),
                                             "Error unmounting %s: %s",
                                             udisks_block_get_device (block),
                                             error->message);
      udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), FALSE, error->message);
      g_clear_error (&error);
      goto out;
    }
  else
    udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), TRUE, NULL);

  /* filesystem unmounted, run the state/cleanup routines now to remove the mountpoint (if applicable) */
  udisks_state_check_block (state, udisks_linux_block_object_get_device_number (UDISKS_LINUX_BLOCK_OBJECT (object)));

  udisks_notice ("Unmounted %s on behalf of uid %u",
                 udisks_block_get_device (block),
                 caller_uid);

  waiting:
  /* trigger uevent on the master device and all its siblings in case of a multidisk volume */
  udisks_linux_block_object_trigger_uevent_sync (UDISKS_LINUX_BLOCK_OBJECT (object),
                                                 UDISKS_DEFAULT_WAIT_TIMEOUT);
  if (volume_based_fs)
    {
      GList *siblings;
      GList *l;

      siblings = udisks_linux_filesystem_find_siblings (UDISKS_LINUX_FILESYSTEM (filesystem),
                                                        UDISKS_LINUX_BLOCK_OBJECT (object));
      for (l = siblings; l != NULL; l = l->next)
        {
          UDisksLinuxBlockObject *obj = UDISKS_LINUX_BLOCK_OBJECT (l->data);
          /* TODO: parallelize */
          udisks_linux_block_object_trigger_uevent_sync (obj, UDISKS_DEFAULT_WAIT_TIMEOUT);
        }
      g_list_free_full (siblings, g_object_unref);
    }

  /* wait for mount-points update before returning from method */
  wait_data.mount_point = g_strdup (mount_point);
  filesystem_object = udisks_daemon_wait_for_object_sync (daemon,
                                                          wait_for_filesystem_mount_points,
                                                          &wait_data,
                                                          NULL,
                                                          UDISKS_DEFAULT_WAIT_TIMEOUT,
                                                          NULL);

  udisks_filesystem_complete_unmount (filesystem, invocation);

 out:
  if (object != NULL)
    udisks_linux_block_object_release_cleanup_lock (UDISKS_LINUX_BLOCK_OBJECT (object));
  if (state != NULL)
    udisks_state_check (state);
  g_free (wait_data.mount_point);
  g_free (mount_point);
  g_free (fstab_mount_options);
  g_clear_object (&object);
  g_clear_object (&filesystem_object);

  g_mutex_unlock (&UDISKS_LINUX_FILESYSTEM (filesystem)->lock);

  return TRUE;
}

/* ---------------------------------------------------------------------------------------------------- */

/* runs in thread dedicated to handling method call */
static gboolean
handle_set_label (UDisksFilesystem      *filesystem,
                  GDBusMethodInvocation *invocation,
                  const gchar           *label,
                  GVariant              *options)
{
  UDisksBlock *block;
  UDisksObject *object;
  UDisksDaemon *daemon;
  UDisksState *state = NULL;
  const gchar *probed_fs_usage;
  const gchar *probed_fs_type;
  const FSInfo *fs_info;
  const gchar *action_id;
  const gchar *message;
  gchar *real_label = NULL;
  uid_t caller_uid;
  gchar *command;
  gint status = 0;
  gchar *out_message = NULL;
  gboolean success = FALSE;
  gchar *tmp;
  GError *error = NULL;

  object = NULL;
  daemon = NULL;
  command = NULL;

  object = udisks_daemon_util_dup_object (filesystem, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = udisks_linux_block_object_get_daemon (UDISKS_LINUX_BLOCK_OBJECT (object));
  state = udisks_daemon_get_state (daemon);
  block = udisks_object_peek_block (object);

  udisks_linux_block_object_lock_for_cleanup (UDISKS_LINUX_BLOCK_OBJECT (object));
  udisks_state_check_block (state, udisks_linux_block_object_get_device_number (UDISKS_LINUX_BLOCK_OBJECT (object)));

  if (!udisks_daemon_util_get_caller_uid_sync (daemon,
                                               invocation,
                                               NULL /* GCancellable */,
                                               &caller_uid,
                                               &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      g_clear_error (&error);
      goto out;
    }

  probed_fs_usage = udisks_block_get_id_usage (block);
  probed_fs_type = udisks_block_get_id_type (block);

  if (g_strcmp0 (probed_fs_usage, "filesystem") != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_NOT_SUPPORTED,
                                             "Cannot change label on device of type %s",
                                             probed_fs_usage);
      goto out;
    }

  fs_info = get_fs_info (probed_fs_type);

  if (fs_info == NULL || fs_info->command_change_label == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_NOT_SUPPORTED,
                                             "Don't know how to change label on device of type %s:%s",
                                             probed_fs_usage,
                                             probed_fs_type);
      goto out;
    }

  /* VFAT does not allow some characters; as dosfslabel does not enforce this,
   * check in advance; also, VFAT only knows upper-case characters, dosfslabel
   * enforces this */
  if (g_strcmp0 (probed_fs_type, "vfat") == 0)
    {
      const gchar *forbidden = "\"*/:<>?\\|";
      guint n;
      for (n = 0; forbidden[n] != 0; n++)
        {
          if (strchr (label, forbidden[n]) != NULL)
            {
              g_dbus_method_invocation_return_error (invocation,
                                                     UDISKS_ERROR,
                                                     UDISKS_ERROR_NOT_SUPPORTED,
                                                     "character '%c' not supported in VFAT labels",
                                                     forbidden[n]);
               goto out;
            }
        }

      /* we need to remember that we make a copy, so assign it to a new
       * variable, too */
      real_label = g_ascii_strup (label, -1);
      label = real_label;
    }

  /* Fail if the device is already mounted and the tools/drivers doesn't
   * support changing the label in that case
   */
  if (filesystem != NULL && !fs_info->supports_online_label_rename)
    {
      const gchar * const *existing_mount_points;
      existing_mount_points = udisks_filesystem_get_mount_points (filesystem);
      if (existing_mount_points != NULL && g_strv_length ((gchar **) existing_mount_points) > 0)
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 UDISKS_ERROR,
                                                 UDISKS_ERROR_NOT_SUPPORTED,
                                                 "Cannot change label on mounted device of type %s:%s.\n",
                                                 probed_fs_usage,
                                                 probed_fs_type);
          goto out;
        }
    }

  action_id = "org.freedesktop.udisks2.modify-device";
  /* Translators: Shown in authentication dialog when the user
   * requests changing the filesystem label.
   *
   * Do not translate $(drive), it's a placeholder and
   * will be replaced by the name of the drive/device in question
   */
  message = N_("Authentication is required to change the filesystem label on $(drive)");
  if (!udisks_daemon_util_setup_by_user (daemon, object, caller_uid))
    {
      if (udisks_block_get_hint_system (block))
        {
          action_id = "org.freedesktop.udisks2.modify-device-system";
        }
      else if (!udisks_daemon_util_on_user_seat (daemon, UDISKS_OBJECT (object), caller_uid))
        {
          action_id = "org.freedesktop.udisks2.modify-device-other-seat";
        }
    }

  /* Check that the user is actually authorized to change the
   * filesystem label.
   */
  if (!udisks_daemon_util_check_authorization_sync (daemon,
                                                    object,
                                                    action_id,
                                                    options,
                                                    message,
                                                    invocation))
    goto out;

  if (fs_info->command_clear_label != NULL && strlen (label) == 0)
    {
      command = udisks_daemon_util_subst_str_and_escape (fs_info->command_clear_label, "$DEVICE", udisks_block_get_device (block));
    }
  else
    {
      tmp = udisks_daemon_util_subst_str_and_escape (fs_info->command_change_label, "$DEVICE", udisks_block_get_device (block));
      command = udisks_daemon_util_subst_str_and_escape (tmp, "$LABEL", label);
      g_free (tmp);
    }

  success = udisks_daemon_launch_spawned_job_sync (daemon,
                                                   object,
                                                   "filesystem-modify", caller_uid,
                                                   NULL, /* cancellable */
                                                   0,    /* uid_t run_as_uid */
                                                   0,    /* uid_t run_as_euid */
                                                   &status,
                                                   &out_message,
                                                   NULL, /* input_string */
                                                   "%s", command);

  /* Label property is automatically updated after an udev change
   * event for this device, but udev sometimes returns the old label
   * so just trigger the uevent again now to be sure the property
   * has been updated.
   */
  udisks_linux_block_object_trigger_uevent_sync (UDISKS_LINUX_BLOCK_OBJECT (object),
                                                 UDISKS_DEFAULT_WAIT_TIMEOUT);

  if (success)
    udisks_filesystem_complete_set_label (filesystem, invocation);
  else
    g_dbus_method_invocation_return_error (invocation,
                                           UDISKS_ERROR,
                                           UDISKS_ERROR_FAILED,
                                           "Error setting label: %s",
                                           out_message);

 out:
  if (object != NULL)
    udisks_linux_block_object_release_cleanup_lock (UDISKS_LINUX_BLOCK_OBJECT (object));
  if (state != NULL)
    udisks_state_check (state);
  /* for some FSes we need to copy and modify label; free our copy */
  g_free (real_label);
  g_free (command);
  g_clear_object (&object);
  g_free (out_message);
  return TRUE; /* returning TRUE means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

/* runs in thread dedicated to handling method call */
static gboolean
handle_resize (UDisksFilesystem      *filesystem,
               GDBusMethodInvocation *invocation,
               guint64                size,
               GVariant              *options)
{
  UDisksBlock *block = NULL;
  UDisksObject *object = NULL;
  UDisksDaemon *daemon = NULL;
  UDisksState *state = NULL;
  const gchar *probed_fs_usage = NULL;
  const gchar *probed_fs_type = NULL;
  BDFsResizeFlags mode = 0;
  const gchar *action_id = NULL;
  const gchar *message = NULL;
  uid_t caller_uid;
  GError *error = NULL;
  UDisksBaseJob *job = NULL;
  gchar *required_utility = NULL;
  const gchar * const *existing_mount_points = NULL;

  g_mutex_lock (&UDISKS_LINUX_FILESYSTEM (filesystem)->lock);

  object = udisks_daemon_util_dup_object (filesystem, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      goto out;
    }

  daemon = udisks_linux_block_object_get_daemon (UDISKS_LINUX_BLOCK_OBJECT (object));
  state = udisks_daemon_get_state (daemon);
  block = udisks_object_peek_block (object);

  udisks_linux_block_object_lock_for_cleanup (UDISKS_LINUX_BLOCK_OBJECT (object));
  udisks_state_check_block (state, udisks_linux_block_object_get_device_number (UDISKS_LINUX_BLOCK_OBJECT (object)));

  if (! udisks_daemon_util_get_caller_uid_sync (daemon,
                                                invocation,
                                                NULL /* GCancellable */,
                                                &caller_uid,
                                                &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      goto out;
    }

  probed_fs_usage = udisks_block_get_id_usage (block);
  if (g_strcmp0 (probed_fs_usage, "filesystem") != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_NOT_SUPPORTED,
                                             "Cannot resize %s filesystem on %s",
                                             probed_fs_usage,
                                             udisks_block_get_device (block));
      goto out;
    }

  probed_fs_type = udisks_block_get_id_type (block);
  if (! bd_fs_can_resize (probed_fs_type, &mode, &required_utility, &error))
    {
      if (error != NULL)
        g_dbus_method_invocation_return_error (invocation,
                                               UDISKS_ERROR,
                                               UDISKS_ERROR_FAILED,
                                               "Cannot resize %s filesystem on %s: %s",
                                               probed_fs_type,
                                               udisks_block_get_device (block),
                                               error->message);
      else
        g_dbus_method_invocation_return_error (invocation,
                                               UDISKS_ERROR,
                                               UDISKS_ERROR_FAILED,
                                               "Cannot resize %s filesystem on %s: executable %s not found",
                                               probed_fs_type,
                                               udisks_block_get_device (block),
                                               required_utility);
      goto out;
    }

  /* it can't be known if it's shrinking or growing but check at least the mount state */
  existing_mount_points = udisks_filesystem_get_mount_points (filesystem);
  if (existing_mount_points != NULL && g_strv_length ((gchar **) existing_mount_points) > 0)
    {
      if (! (mode & BD_FS_ONLINE_SHRINK) && ! (mode & BD_FS_ONLINE_GROW))
        g_dbus_method_invocation_return_error (invocation,
                                               UDISKS_ERROR,
                                               UDISKS_ERROR_NOT_SUPPORTED,
                                               "Cannot resize %s filesystem on %s if mounted",
                                               probed_fs_usage,
                                               udisks_block_get_device (block));
    }
  else if (! (mode & BD_FS_OFFLINE_SHRINK) && ! (mode & BD_FS_OFFLINE_GROW))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_NOT_SUPPORTED,
                                             "Cannot resize %s filesystem on %s if unmounted",
                                             probed_fs_usage,
                                             udisks_block_get_device (block));
    }

  action_id = "org.freedesktop.udisks2.modify-device";
  /* Translators: Shown in authentication dialog when the user
   * requests resizing the filesystem.
   *
   * Do not translate $(drive), it's a placeholder and
   * will be replaced by the name of the drive/device in question
   */
  message = N_("Authentication is required to resize the filesystem on $(drive)");
  if (! udisks_daemon_util_setup_by_user (daemon, object, caller_uid))
    {
      if (udisks_block_get_hint_system (block))
        {
          action_id = "org.freedesktop.udisks2.modify-device-system";
        }
      else if (! udisks_daemon_util_on_user_seat (daemon, UDISKS_OBJECT (object), caller_uid))
        {
          action_id = "org.freedesktop.udisks2.modify-device-other-seat";
        }
    }

  /* Check that the user is actually authorized to resize the filesystem. */
  if (! udisks_daemon_util_check_authorization_sync (daemon,
                                                    object,
                                                    action_id,
                                                    options,
                                                    message,
                                                    invocation))
    goto out;

  job = udisks_daemon_launch_simple_job (daemon,
                                         UDISKS_OBJECT (object),
                                         "filesystem-resize",
                                         caller_uid,
                                         NULL);
  if (job == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                             "Failed to create a job object");
      goto out;
    }

  udisks_bd_thread_set_progress_for_job (UDISKS_JOB (job));
  if (! bd_fs_resize (udisks_block_get_device (block), size, &error))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Error resizing filesystem on %s: %s",
                                             udisks_block_get_device (block),
                                             error->message);
      udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), FALSE, error->message);
      goto out;
    }

  /* At least resize2fs might need another uevent after it is done.
   */
  udisks_linux_block_object_trigger_uevent_sync (UDISKS_LINUX_BLOCK_OBJECT (object),
                                                 UDISKS_DEFAULT_WAIT_TIMEOUT);

  udisks_filesystem_set_size (filesystem, get_filesystem_size (UDISKS_LINUX_BLOCK_OBJECT (object)));
  g_dbus_interface_skeleton_flush (G_DBUS_INTERFACE_SKELETON (filesystem));
  udisks_filesystem_complete_resize (filesystem, invocation);
  udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), TRUE, NULL);

 out:
  udisks_bd_thread_disable_progress ();
  if (object != NULL)
    udisks_linux_block_object_release_cleanup_lock (UDISKS_LINUX_BLOCK_OBJECT (object));
  if (state != NULL)
    udisks_state_check (state);
  g_clear_object (&object);
  g_free (required_utility);
  g_clear_error (&error);
  g_mutex_unlock (&UDISKS_LINUX_FILESYSTEM (filesystem)->lock);
  return TRUE; /* returning TRUE means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

/* runs in thread dedicated to handling method call */
static gboolean
handle_repair (UDisksFilesystem      *filesystem,
               GDBusMethodInvocation *invocation,
               GVariant              *options)
{
  UDisksBlock *block = NULL;
  UDisksObject *object = NULL;
  UDisksDaemon *daemon = NULL;
  UDisksState *state = NULL;
  const gchar *probed_fs_usage = NULL;
  const gchar *probed_fs_type = NULL;
  const gchar *action_id = NULL;
  const gchar *message = NULL;
  uid_t caller_uid;
  GError *error = NULL;
  gboolean ret = FALSE;
  UDisksBaseJob *job = NULL;
  gchar *required_utility = NULL;
  const gchar * const *existing_mount_points = NULL;

  g_mutex_lock (&UDISKS_LINUX_FILESYSTEM (filesystem)->lock);

  object = udisks_daemon_util_dup_object (filesystem, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      goto out;
    }

  daemon = udisks_linux_block_object_get_daemon (UDISKS_LINUX_BLOCK_OBJECT (object));
  state = udisks_daemon_get_state (daemon);
  block = udisks_object_peek_block (object);

  udisks_linux_block_object_lock_for_cleanup (UDISKS_LINUX_BLOCK_OBJECT (object));
  udisks_state_check_block (state, udisks_linux_block_object_get_device_number (UDISKS_LINUX_BLOCK_OBJECT (object)));

  if (! udisks_daemon_util_get_caller_uid_sync (daemon,
                                                invocation,
                                                NULL /* GCancellable */,
                                                &caller_uid,
                                                &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      goto out;
    }

  probed_fs_usage = udisks_block_get_id_usage (block);
  if (g_strcmp0 (probed_fs_usage, "filesystem") != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_NOT_SUPPORTED,
                                             "Cannot repair %s filesystem on %s",
                                             probed_fs_usage,
                                             udisks_block_get_device (block));
      goto out;
    }

  probed_fs_type = udisks_block_get_id_type (block);
  if (! bd_fs_can_repair (probed_fs_type, &required_utility, &error))
    {
      if (error != NULL)
        g_dbus_method_invocation_return_error (invocation,
                                               UDISKS_ERROR,
                                               UDISKS_ERROR_FAILED,
                                               "Cannot repair %s filesystem on %s: %s",
                                               probed_fs_type,
                                               udisks_block_get_device (block),
                                               error->message);
      else
        g_dbus_method_invocation_return_error (invocation,
                                               UDISKS_ERROR,
                                               UDISKS_ERROR_FAILED,
                                               "Cannot repair %s filesystem on %s: executable %s not found",
                                               probed_fs_type,
                                               udisks_block_get_device (block),
                                               required_utility);
      goto out;
    }

  /* check the mount state */
  existing_mount_points = udisks_filesystem_get_mount_points (filesystem);
  if (existing_mount_points != NULL && g_strv_length ((gchar **) existing_mount_points) > 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_NOT_SUPPORTED,
                                             "Cannot repair %s filesystem on %s if mounted",
                                             probed_fs_usage,
                                             udisks_block_get_device (block));
    }

  action_id = "org.freedesktop.udisks2.modify-device";
  /* Translators: Shown in authentication dialog when the user
   * requests resizing the filesystem.
   *
   * Do not translate $(drive), it's a placeholder and
   * will be replaced by the name of the drive/device in question
   */
  message = N_("Authentication is required to repair the filesystem on $(drive)");
  if (! udisks_daemon_util_setup_by_user (daemon, object, caller_uid))
    {
      if (udisks_block_get_hint_system (block))
        {
          action_id = "org.freedesktop.udisks2.modify-device-system";
        }
      else if (! udisks_daemon_util_on_user_seat (daemon, UDISKS_OBJECT (object), caller_uid))
        {
          action_id = "org.freedesktop.udisks2.modify-device-other-seat";
        }
    }

  /* Check that the user is actually authorized to repair the filesystem. */
  if (! udisks_daemon_util_check_authorization_sync (daemon,
                                                     object,
                                                     action_id,
                                                     options,
                                                     message,
                                                     invocation))
    goto out;

  job = udisks_daemon_launch_simple_job (daemon,
                                         UDISKS_OBJECT (object),
                                         "filesystem-repair",
                                         caller_uid,
                                         NULL);
  if (job == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                             "Failed to create a job object");
      goto out;
    }

  udisks_bd_thread_set_progress_for_job (UDISKS_JOB (job));
  ret = bd_fs_repair (udisks_block_get_device (block), &error);
  if (error)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Error reparing filesystem on %s: %s",
                                             udisks_block_get_device (block),
                                             error->message);
      udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), FALSE, error->message);
      goto out;
    }

  udisks_linux_block_object_trigger_uevent_sync (UDISKS_LINUX_BLOCK_OBJECT (object),
                                                 UDISKS_DEFAULT_WAIT_TIMEOUT);
  udisks_filesystem_complete_repair (filesystem, invocation, ret);
  udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), TRUE, NULL);

 out:
  udisks_bd_thread_disable_progress ();
  if (object != NULL)
    udisks_linux_block_object_release_cleanup_lock (UDISKS_LINUX_BLOCK_OBJECT (object));
  if (state != NULL)
    udisks_state_check (state);
  g_clear_object (&object);
  g_free (required_utility);
  g_clear_error (&error);
  g_mutex_unlock (&UDISKS_LINUX_FILESYSTEM (filesystem)->lock);
  return TRUE; /* returning TRUE means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

/* runs in thread dedicated to handling method call */
static gboolean
handle_check (UDisksFilesystem      *filesystem,
              GDBusMethodInvocation *invocation,
              GVariant              *options)
{
  UDisksBlock *block = NULL;
  UDisksObject *object = NULL;
  UDisksDaemon *daemon = NULL;
  UDisksState *state = NULL;
  const gchar *probed_fs_usage = NULL;
  const gchar *probed_fs_type = NULL;
  const gchar *action_id = NULL;
  const gchar *message = NULL;
  uid_t caller_uid;
  GError *error = NULL;
  gboolean ret = FALSE;
  UDisksBaseJob *job = NULL;
  gchar *required_utility = NULL;
  const gchar * const *existing_mount_points = NULL;

  g_mutex_lock (&UDISKS_LINUX_FILESYSTEM (filesystem)->lock);

  object = udisks_daemon_util_dup_object (filesystem, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      goto out;
    }

  daemon = udisks_linux_block_object_get_daemon (UDISKS_LINUX_BLOCK_OBJECT (object));
  state = udisks_daemon_get_state (daemon);
  block = udisks_object_peek_block (object);

  udisks_linux_block_object_lock_for_cleanup (UDISKS_LINUX_BLOCK_OBJECT (object));
  udisks_state_check_block (state, udisks_linux_block_object_get_device_number (UDISKS_LINUX_BLOCK_OBJECT (object)));

  if (! udisks_daemon_util_get_caller_uid_sync (daemon,
                                                invocation,
                                                NULL /* GCancellable */,
                                                &caller_uid,
                                                &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      goto out;
    }

  probed_fs_usage = udisks_block_get_id_usage (block);
  if (g_strcmp0 (probed_fs_usage, "filesystem") != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_NOT_SUPPORTED,
                                             "Cannot check %s filesystem on %s",
                                             probed_fs_usage,
                                             udisks_block_get_device (block));
      goto out;
    }

  probed_fs_type = udisks_block_get_id_type (block);
  if (! bd_fs_can_check (probed_fs_type, &required_utility, &error))
    {
      if (error != NULL)
        g_dbus_method_invocation_return_error (invocation,
                                               UDISKS_ERROR,
                                               UDISKS_ERROR_FAILED,
                                               "Cannot check %s filesystem on %s: %s",
                                               probed_fs_type,
                                               udisks_block_get_device (block),
                                               error->message);
      else
        g_dbus_method_invocation_return_error (invocation,
                                               UDISKS_ERROR,
                                               UDISKS_ERROR_FAILED,
                                               "Cannot check %s filesystem on %s: executable %s not found",
                                               probed_fs_type,
                                               udisks_block_get_device (block),
                                               required_utility);
      goto out;
    }

  /* check the mount state */
  existing_mount_points = udisks_filesystem_get_mount_points (filesystem);
  if (existing_mount_points != NULL && g_strv_length ((gchar **) existing_mount_points) > 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_NOT_SUPPORTED,
                                             "Cannot check %s filesystem on %s if mounted",
                                             probed_fs_usage,
                                             udisks_block_get_device (block));
    }

  action_id = "org.freedesktop.udisks2.modify-device";
  /* Translators: Shown in authentication dialog when the user
   * requests resizing the filesystem.
   *
   * Do not translate $(drive), it's a placeholder and
   * will be replaced by the name of the drive/device in question
   */
  message = N_("Authentication is required to check the filesystem on $(drive)");
  if (! udisks_daemon_util_setup_by_user (daemon, object, caller_uid))
    {
      if (udisks_block_get_hint_system (block))
        {
          action_id = "org.freedesktop.udisks2.modify-device-system";
        }
      else if (! udisks_daemon_util_on_user_seat (daemon, UDISKS_OBJECT (object), caller_uid))
        {
          action_id = "org.freedesktop.udisks2.modify-device-other-seat";
        }
    }

  /* Check that the user is actually authorized to check the filesystem. */
  if (! udisks_daemon_util_check_authorization_sync (daemon,
                                                     object,
                                                     action_id,
                                                     options,
                                                     message,
                                                     invocation))
    goto out;

  job = udisks_daemon_launch_simple_job (daemon,
                                         UDISKS_OBJECT (object),
                                         "filesystem-check",
                                         caller_uid,
                                         NULL);
  if (job == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                             "Failed to create a job object");
      goto out;
    }

  udisks_bd_thread_set_progress_for_job (UDISKS_JOB (job));
  ret = bd_fs_check (udisks_block_get_device (block), &error);
  if (error)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Error checking filesystem on %s: %s",
                                             udisks_block_get_device (block),
                                             error->message);
      udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), FALSE, error->message);
      goto out;
    }

  udisks_linux_block_object_trigger_uevent_sync (UDISKS_LINUX_BLOCK_OBJECT (object),
                                                 UDISKS_DEFAULT_WAIT_TIMEOUT);
  udisks_filesystem_complete_check (filesystem, invocation, ret);
  udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), TRUE, NULL);

 out:
  udisks_bd_thread_disable_progress ();
  if (object != NULL)
    udisks_linux_block_object_release_cleanup_lock (UDISKS_LINUX_BLOCK_OBJECT (object));
  if (state != NULL)
    udisks_state_check (state);
  g_clear_object (&object);
  g_free (required_utility);
  g_clear_error (&error);
  g_mutex_unlock (&UDISKS_LINUX_FILESYSTEM (filesystem)->lock);
  return TRUE; /* returning TRUE means that we handled the method invocation */
}

static gboolean
handle_take_ownership (UDisksFilesystem      *filesystem,
                       GDBusMethodInvocation *invocation,
                       GVariant              *options)
{
  UDisksBlock *block = NULL;
  UDisksObject *object = NULL;
  UDisksDaemon *daemon = NULL;
  UDisksState *state = NULL;
  const gchar *probed_fs_usage = NULL;
  const gchar *probed_fs_type = NULL;
  const gchar *action_id = NULL;
  const gchar *message = NULL;
  const FSInfo *fs_info = NULL;
  UDisksBaseJob *job = NULL;
  GError *error = NULL;
  gboolean recursive = FALSE;
  uid_t caller_uid;
  gid_t caller_gid;

  g_variant_lookup (options, "recursive", "b", &recursive);

  /* only allow a single call at a time */
  g_mutex_lock (&UDISKS_LINUX_FILESYSTEM (filesystem)->lock);

  object = udisks_daemon_util_dup_object (filesystem, &error);
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

  if (! udisks_daemon_util_get_caller_uid_sync (daemon,
                                                invocation,
                                                NULL /* GCancellable */,
                                                &caller_uid,
                                                &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      goto out;
    }

  if (!udisks_daemon_util_get_user_info (caller_uid, &caller_gid, NULL /* user name */, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      goto out;
    }

  probed_fs_usage = udisks_block_get_id_usage (block);
  if (g_strcmp0 (probed_fs_usage, "filesystem") != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_NOT_SUPPORTED,
                                             "Cannot take ownership of %s filesystem on %s",
                                             probed_fs_usage,
                                             udisks_block_get_device (block));
      goto out;
    }

  probed_fs_type = udisks_block_get_id_type (block);
  fs_info = get_fs_info (probed_fs_type);
  if (fs_info == NULL || !fs_info->supports_owners)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_NOT_SUPPORTED,
                                             "Filesystem %s doesn't support ownership",
                                             probed_fs_usage);
      goto out;
    }

  action_id = "org.freedesktop.udisks2.filesystem-take-ownership";
  /* Translators: Shown in authentication dialog when the user
   * requests taking ownership of the filesystem.
   *
   * Do not translate $(drive), it's a placeholder and
   * will be replaced by the name of the drive/device in question
   */
  message = N_("Authentication is required to change ownership of the filesystem on $(drive)");

  /* Check that the user is actually authorized to check the filesystem. */
  if (! udisks_daemon_util_check_authorization_sync (daemon,
                                                     object,
                                                     action_id,
                                                     options,
                                                     message,
                                                     invocation))
    goto out;

  job = udisks_daemon_launch_simple_job (daemon,
                                         UDISKS_OBJECT (object),
                                         "filesystem-modify",
                                         caller_uid,
                                         NULL);
  if (job == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                             "Failed to create a job object");
      goto out;
    }

  if (! take_filesystem_ownership (udisks_block_get_device (block),
                                   probed_fs_type,
                                   caller_uid, caller_gid,
                                   recursive,
                                   &error))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Error taking ownership of filesystem on %s: %s",
                                             udisks_block_get_device (block),
                                             error->message);
      udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), FALSE, error->message);
      goto out;
    }


  udisks_filesystem_complete_take_ownership (filesystem, invocation);
  udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), TRUE, NULL);

  out:
   if (object != NULL)
     udisks_linux_block_object_release_cleanup_lock (UDISKS_LINUX_BLOCK_OBJECT (object));
   if (state != NULL)
     udisks_state_check (state);
   g_clear_object (&object);
   g_clear_error (&error);
   g_mutex_unlock (&UDISKS_LINUX_FILESYSTEM (filesystem)->lock);
   return TRUE; /* returning TRUE means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

static void
filesystem_iface_init (UDisksFilesystemIface *iface)
{
  iface->handle_mount     = handle_mount;
  iface->handle_unmount   = handle_unmount;
  iface->handle_set_label = handle_set_label;
  iface->handle_resize    = handle_resize;
  iface->handle_repair    = handle_repair;
  iface->handle_check     = handle_check;
  iface->handle_take_ownership = handle_take_ownership;
}
