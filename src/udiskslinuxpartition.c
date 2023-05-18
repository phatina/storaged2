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
#include <pwd.h>
#include <grp.h>
#include <string.h>
#include <stdlib.h>
#include <sys/sysmacros.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <glib-unix.h>

#include <glib/gstdio.h>

#include <blockdev/part.h>

#include "udiskslogging.h"
#include "udiskslinuxpartition.h"
#include "udiskslinuxblockobject.h"
#include "udisksdaemon.h"
#include "udisksdaemonutil.h"
#include "udiskslinuxdevice.h"
#include "udiskslinuxblock.h"
#include "udiskssimplejob.h"
#include "udisksstate.h"

/**
 * SECTION:udiskslinuxpartition
 * @title: UDisksLinuxPartition
 * @short_description: Linux implementation of #UDisksPartition
 *
 * This type provides an implementation of the #UDisksPartition
 * interface on Linux.
 */

typedef struct _UDisksLinuxPartitionClass   UDisksLinuxPartitionClass;

/**
 * UDisksLinuxPartition:
 *
 * The #UDisksLinuxPartition structure contains only private data and should
 * only be accessed using the provided API.
 */
struct _UDisksLinuxPartition
{
  UDisksPartitionSkeleton parent_instance;
};

struct _UDisksLinuxPartitionClass
{
  UDisksPartitionSkeletonClass parent_class;
};

static void partition_iface_init (UDisksPartitionIface *iface);

G_DEFINE_TYPE_WITH_CODE (UDisksLinuxPartition, udisks_linux_partition, UDISKS_TYPE_PARTITION_SKELETON,
                         G_IMPLEMENT_INTERFACE (UDISKS_TYPE_PARTITION, partition_iface_init));

/* ---------------------------------------------------------------------------------------------------- */

static void
udisks_linux_partition_init (UDisksLinuxPartition *partition)
{
  g_dbus_interface_skeleton_set_flags (G_DBUS_INTERFACE_SKELETON (partition),
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);
}

static void
udisks_linux_partition_class_init (UDisksLinuxPartitionClass *klass)
{
}

/**
 * udisks_linux_partition_new:
 *
 * Creates a new #UDisksLinuxPartition instance.
 *
 * Returns: A new #UDisksLinuxPartition. Free with g_object_unref().
 */
UDisksPartition *
udisks_linux_partition_new (void)
{
  return UDISKS_PARTITION (g_object_new (UDISKS_TYPE_LINUX_PARTITION,
                                         NULL));
}

static gboolean
check_authorization (UDisksPartition       *partition,
                     GDBusMethodInvocation *invocation,
                     GVariant              *options,
                     uid_t                 *caller_uid)
{
  UDisksDaemon *daemon = NULL;
  const gchar *action_id = NULL;
  const gchar *message = NULL;
  UDisksBlock *block = NULL;
  UDisksObject *object = NULL;
  GError *error = NULL;
  gboolean rc = FALSE;

  object = udisks_daemon_util_dup_object (partition, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = udisks_linux_block_object_get_daemon (UDISKS_LINUX_BLOCK_OBJECT (object));
  block = udisks_object_get_block (object);

  if (!udisks_daemon_util_get_caller_uid_sync (daemon,
                                               invocation,
                                               NULL /* GCancellable */,
                                               caller_uid,
                                               &error))
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  action_id = "org.freedesktop.udisks2.modify-device";
  /* Translators: Shown in authentication dialog when the user
   * requests modifying a partition (changing type, flags, name etc.).
   *
   * Do not translate $(drive), it's a placeholder and
   * will be replaced by the name of the drive/device in question
   */
  message = N_("Authentication is required to modify the partition on device $(drive)");
  if (!udisks_daemon_util_setup_by_user (daemon, object, *caller_uid))
    {
      if (udisks_block_get_hint_system (block))
        {
          action_id = "org.freedesktop.udisks2.modify-device-system";
        }
      else if (!udisks_daemon_util_on_user_seat (daemon, object, *caller_uid))
        {
          action_id = "org.freedesktop.udisks2.modify-device-other-seat";
        }
    }
  if (udisks_daemon_util_check_authorization_sync (daemon,
                                                   object,
                                                   action_id,
                                                   options,
                                                   message,
                                                   invocation))
    {
      rc = TRUE;
    }

out:
  g_clear_object (&block);
  g_clear_object (&object);

  return rc;
}

/* ---------------------------------------------------------------------------------------------------- */

static void update_partitions_list (UDisksObject           *disk_object,
                                    UDisksLinuxBlockObject *part_object)
{
  UDisksPartitionTable *table = NULL;
  gchar **partitions = NULL;
  const gchar *object_path = NULL;
  guint num_parts = 0;

  object_path = g_dbus_object_get_object_path (G_DBUS_OBJECT (part_object));
  if (object_path == NULL)
    return;

  table = udisks_object_peek_partition_table (disk_object);
  if (table == NULL)
    return;

  partitions = udisks_partition_table_dup_partitions (table);
  if (partitions != NULL && g_strv_contains ((const gchar **) partitions, object_path))
    /* this partition is already in the property */
    goto out;

  num_parts = partitions ? g_strv_length (partitions) : 0;
  partitions = g_realloc (partitions, (num_parts + 2) * sizeof (gchar *));
  partitions[num_parts] = g_strdup (object_path);
  partitions[num_parts + 1] = NULL;

  udisks_partition_table_set_partitions (table, (const gchar**) partitions);

out:
  g_strfreev (partitions);
}

/**
 * udisks_linux_partition_update:
 * @partition: A #UDisksLinuxPartition.
 * @object: The enclosing #UDisksLinuxBlockObject instance.
 *
 * Updates the interface.
 */
void
udisks_linux_partition_update (UDisksLinuxPartition   *partition,
                               UDisksLinuxBlockObject *object)
{
  UDisksObject *disk_block_object = NULL;
  UDisksLinuxDevice *device = NULL;
  guint number = 0;
  const gchar *type = NULL;
  gchar type_buf[16];
  guint64 offset = 0;
  guint64 size = 0;
  gchar *name = NULL;
  const gchar *uuid = NULL;
  guint64 flags = 0;
  const gchar *table_object_path = "/";
  gboolean is_container = FALSE;
  gboolean is_contained = FALSE;

  device = udisks_linux_block_object_get_device (object);
  if (g_udev_device_has_property (device->udev_device, "ID_PART_ENTRY_TYPE"))
    {
      const gchar *disk_string;
      number = g_udev_device_get_property_as_int (device->udev_device, "ID_PART_ENTRY_NUMBER");
      type = g_udev_device_get_property (device->udev_device, "ID_PART_ENTRY_TYPE");
      offset = g_udev_device_get_property_as_uint64 (device->udev_device, "ID_PART_ENTRY_OFFSET") * G_GUINT64_CONSTANT (512);
      size = g_udev_device_get_property_as_uint64 (device->udev_device, "ID_PART_ENTRY_SIZE") * G_GUINT64_CONSTANT (512);
      name = udisks_decode_udev_string (g_udev_device_get_property (device->udev_device, "ID_PART_ENTRY_NAME"), NULL);
      uuid = g_udev_device_get_property (device->udev_device, "ID_PART_ENTRY_UUID");
      flags = g_udev_device_get_property_as_uint64 (device->udev_device, "ID_PART_ENTRY_FLAGS");

      disk_string = g_udev_device_get_property (device->udev_device, "ID_PART_ENTRY_DISK");
      if (disk_string != NULL)
        {
          gint disk_major, disk_minor;
          if (sscanf (disk_string, "%d:%d", &disk_major, &disk_minor) == 2)
            {
              disk_block_object = udisks_daemon_find_block (udisks_linux_block_object_get_daemon (object),
                                                            makedev (disk_major, disk_minor));
            }
        }

      if (g_strcmp0 (g_udev_device_get_property (device->udev_device, "ID_PART_ENTRY_SCHEME"), "dos") == 0)
        {
          char *endp;
          guint type_as_int = strtoul (type, &endp, 0);
          if (type[0] != '\0' && *endp == '\0')
            {
              /* ensure 'dos' partition types are always of the form 0x0c (e.g. with two digits) */
              snprintf (type_buf, sizeof type_buf, "0x%02x", type_as_int);
              type = type_buf;
              if (number <= 4)
                {
                  if (type_as_int == 0x05 || type_as_int == 0x0f || type_as_int == 0x85)
                    {
                      is_container = TRUE;
                    }
                }
              else if (number >= 5)
                {
                  is_contained = TRUE;
                }
            }
        }
    }
  else
    {
      GUdevDevice *parent_device;
      number = g_udev_device_get_sysfs_attr_as_int (device->udev_device, "partition");
      offset = g_udev_device_get_sysfs_attr_as_uint64 (device->udev_device, "start") * G_GUINT64_CONSTANT (512);
      size = g_udev_device_get_sysfs_attr_as_uint64 (device->udev_device, "size") * G_GUINT64_CONSTANT (512);
      parent_device = g_udev_device_get_parent_with_subsystem (device->udev_device, "block", "disk");
      if (parent_device != NULL)
        {
          disk_block_object = udisks_daemon_find_block (udisks_linux_block_object_get_daemon (object),
                                                        g_udev_device_get_device_number (parent_device));
          g_object_unref (parent_device);
        }
    }

  if (disk_block_object != NULL) {
    table_object_path = g_dbus_object_get_object_path (G_DBUS_OBJECT (disk_block_object));
    update_partitions_list (disk_block_object, object);
  }

  udisks_partition_set_number (UDISKS_PARTITION (partition), number);
  udisks_partition_set_type_ (UDISKS_PARTITION (partition), type);
  udisks_partition_set_flags (UDISKS_PARTITION (partition), flags);
  udisks_partition_set_offset (UDISKS_PARTITION (partition), offset);
  udisks_partition_set_size (UDISKS_PARTITION (partition), size);
  udisks_partition_set_name (UDISKS_PARTITION (partition), name);
  udisks_partition_set_uuid (UDISKS_PARTITION (partition), uuid);
  udisks_partition_set_table (UDISKS_PARTITION (partition), table_object_path);
  udisks_partition_set_is_container (UDISKS_PARTITION (partition), is_container);
  udisks_partition_set_is_contained (UDISKS_PARTITION (partition), is_contained);

  g_dbus_interface_skeleton_flush (G_DBUS_INTERFACE_SKELETON (partition));

  g_free (name);
  g_clear_object (&device);
  g_clear_object (&disk_block_object);
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
handle_set_flags (UDisksPartition       *partition,
                  GDBusMethodInvocation *invocation,
                  guint64                flags,
                  GVariant              *options)
{
  UDisksBlock *block = NULL;
  UDisksObject *object = NULL;
  UDisksDaemon *daemon = NULL;
  UDisksState *state = NULL;
  gchar *device_name = NULL;
  UDisksObject *partition_table_object = NULL;
  UDisksPartitionTable *partition_table = NULL;
  UDisksBlock *partition_table_block = NULL;
  int fd = -1;
  uid_t caller_uid;
  gchar *partition_name = NULL;
  gboolean bootable = FALSE;
  GError *error = NULL;
  UDisksBaseJob *job = NULL;

  if (!check_authorization (partition, invocation, options, &caller_uid))
    {
      goto out;
    }

  object = udisks_daemon_util_dup_object (partition, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      goto out;
    }

  daemon = udisks_linux_block_object_get_daemon (UDISKS_LINUX_BLOCK_OBJECT (object));
  state = udisks_daemon_get_state (daemon);
  block = udisks_object_get_block (object);
  partition_table_object = udisks_daemon_find_object (daemon, udisks_partition_get_table (partition));
  partition_table = udisks_object_get_partition_table (partition_table_object);
  partition_table_block = udisks_object_get_block (partition_table_object);
  device_name = udisks_block_dup_device (partition_table_block);
  partition_name = udisks_block_dup_device (block);

  udisks_linux_block_object_lock_for_cleanup (UDISKS_LINUX_BLOCK_OBJECT (object));
  udisks_linux_block_object_lock_for_cleanup (UDISKS_LINUX_BLOCK_OBJECT (partition_table_object));
  udisks_state_check_block (state, udisks_linux_block_object_get_device_number (UDISKS_LINUX_BLOCK_OBJECT (object)));
  udisks_state_check_block (state, udisks_linux_block_object_get_device_number (UDISKS_LINUX_BLOCK_OBJECT (partition_table_object)));

  job = udisks_daemon_launch_simple_job (daemon,
                                         UDISKS_OBJECT (object),
                                         "partition-modify",
                                         caller_uid,
                                         NULL);

  if (job == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                             "Failed to create a job object");
      goto out;
    }

  /* hold a file descriptor open to suppress BLKRRPART generated by the tools */
  fd = udisks_linux_block_object_acquire_bsd_lock (UDISKS_LINUX_BLOCK_OBJECT (partition_table_object), TRUE, &error);
  if (fd < 0)
    {
      udisks_debug ("%s", error->message);
      g_clear_error (&error);
    }

  if (g_strcmp0 (udisks_partition_table_get_type_ (partition_table), "gpt") == 0)
    {
      if (!bd_part_set_part_attributes (device_name,
                                        partition_name,
                                        flags,
                                        &error))
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 UDISKS_ERROR,
                                                 UDISKS_ERROR_FAILED,
                                                 "Error setting partition flags on %s: %s",
                                                 udisks_block_get_device (block),
                                                 error->message);
          udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), FALSE, error->message);
          goto out;
        }
    }
  else if (g_strcmp0 (udisks_partition_table_get_type_ (partition_table), "dos") == 0)
    {
      /* 7th bit - the partition is marked as bootable */
      bootable = !!(flags & 1 << 7); /* Narrow possible values to TRUE/FALSE */

      if (!bd_part_set_part_bootable (device_name,
                                      partition_name,
                                      bootable,
                                      &error))
      {
        g_dbus_method_invocation_return_error (invocation,
                                               UDISKS_ERROR,
                                               UDISKS_ERROR_FAILED,
                                               "Error setting partition flags on %s: %s",
                                               udisks_block_get_device (block),
                                               error->message);
        udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), FALSE, error->message);
        goto out;
      }
    }
    else
      {
        g_dbus_method_invocation_return_error (invocation,
                                               UDISKS_ERROR,
                                               UDISKS_ERROR_NOT_SUPPORTED,
                                               "No support for modifying a partition a table of type `%s'",
                                               udisks_partition_table_get_type_ (partition_table));
        udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), FALSE, NULL);
        goto out;
      }

  /* unlock and re-read */
  if (fd >= 0)
    {
      close (fd);
      fd = -1;
    }
  if (!udisks_linux_block_object_reread_partition_table (UDISKS_LINUX_BLOCK_OBJECT (partition_table_object), &error))
    {
      udisks_warning ("%s", error->message);
      g_clear_error (&error);
    }
  udisks_linux_block_object_trigger_uevent_sync (UDISKS_LINUX_BLOCK_OBJECT (partition_table_object),
                                                 UDISKS_DEFAULT_WAIT_TIMEOUT);
  udisks_linux_block_object_trigger_uevent_sync (UDISKS_LINUX_BLOCK_OBJECT (object),
                                                 UDISKS_DEFAULT_WAIT_TIMEOUT);
  udisks_partition_complete_set_flags (partition, invocation);
  udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), TRUE, NULL);

 out:
  if (fd != -1)
    close (fd);
  if (partition_table_object != NULL)
    udisks_linux_block_object_release_cleanup_lock (UDISKS_LINUX_BLOCK_OBJECT (partition_table_object));
  if (object != NULL)
    udisks_linux_block_object_release_cleanup_lock (UDISKS_LINUX_BLOCK_OBJECT (object));
  if (state != NULL)
    udisks_state_check (state);
  g_free (device_name);
  g_free (partition_name);
  g_clear_error (&error);
  g_clear_object (&object);
  g_clear_object (&block);
  g_clear_object (&partition_table_object);
  g_clear_object (&partition_table);
  g_clear_object (&partition_table_block);
  g_clear_object (&object);

  return TRUE; /* returning TRUE means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

/* runs in thread dedicated to handling @invocation */
static gboolean
handle_set_name (UDisksPartition       *partition,
                 GDBusMethodInvocation *invocation,
                 const gchar           *name,
                 GVariant              *options)
{
  UDisksBlock *block = NULL;
  UDisksObject *object = NULL;
  UDisksDaemon *daemon = NULL;
  UDisksState *state = NULL;
  gchar *device_name = NULL;
  gchar *partition_name = NULL;
  UDisksObject *partition_table_object = NULL;
  UDisksPartitionTable *partition_table = NULL;
  UDisksBlock *partition_table_block = NULL;
  int fd = -1;
  uid_t caller_uid;
  GError *error = NULL;
  UDisksBaseJob *job = NULL;

  if (!check_authorization (partition, invocation, options, &caller_uid))
    {
      goto out;
    }

  object = udisks_daemon_util_dup_object (partition, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      goto out;
    }

  daemon = udisks_linux_block_object_get_daemon (UDISKS_LINUX_BLOCK_OBJECT (object));
  state = udisks_daemon_get_state (daemon);
  block = udisks_object_get_block (object);
  partition_table_object = udisks_daemon_find_object (daemon, udisks_partition_get_table (partition));
  partition_table = udisks_object_get_partition_table (partition_table_object);
  partition_table_block = udisks_object_get_block (partition_table_object);

  device_name = udisks_block_dup_device (partition_table_block);
  partition_name = udisks_block_dup_device (block);

  udisks_linux_block_object_lock_for_cleanup (UDISKS_LINUX_BLOCK_OBJECT (object));
  udisks_linux_block_object_lock_for_cleanup (UDISKS_LINUX_BLOCK_OBJECT (partition_table_object));
  udisks_state_check_block (state, udisks_linux_block_object_get_device_number (UDISKS_LINUX_BLOCK_OBJECT (object)));
  udisks_state_check_block (state, udisks_linux_block_object_get_device_number (UDISKS_LINUX_BLOCK_OBJECT (partition_table_object)));

  job = udisks_daemon_launch_simple_job (daemon,
                                         UDISKS_OBJECT (object),
                                         "partition-modify",
                                         caller_uid,
                                         NULL);

  if (job == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                             "Failed to create a job object");
      goto out;
    }

  /* hold a file descriptor open to suppress BLKRRPART generated by the tools */
  fd = udisks_linux_block_object_acquire_bsd_lock (UDISKS_LINUX_BLOCK_OBJECT (partition_table_object), TRUE, &error);
  if (fd < 0)
    {
      udisks_debug ("%s", error->message);
      g_clear_error (&error);
    }

  if (g_strcmp0 (udisks_partition_table_get_type_ (partition_table), "gpt") == 0)
    {
      if (strlen (name) > 36)
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 UDISKS_ERROR,
                                                 UDISKS_ERROR_FAILED,
                                                 "Max partition name length is 36 characters");
          udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), FALSE, NULL);
          goto out;
        }

      if (!bd_part_set_part_name (device_name,
                                  partition_name,
                                  name,
                                  &error))
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 UDISKS_ERROR,
                                                 UDISKS_ERROR_FAILED,
                                                 "Error setting partition name on %s: %s",
                                                 udisks_block_get_device (block),
                                                 error->message);
          udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), FALSE, error->message);
          goto out;
        }
    }
  else
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_NOT_SUPPORTED,
                                             "No support for modifying a partition a table of type `%s'",
                                             udisks_partition_table_get_type_ (partition_table));
      goto out;
    }

  /* unlock and re-read */
  if (fd >= 0)
    {
      close (fd);
      fd = -1;
    }
  if (!udisks_linux_block_object_reread_partition_table (UDISKS_LINUX_BLOCK_OBJECT (partition_table_object), &error))
    {
      udisks_warning ("%s", error->message);
      g_clear_error (&error);
    }
  udisks_linux_block_object_trigger_uevent_sync (UDISKS_LINUX_BLOCK_OBJECT (partition_table_object),
                                                 UDISKS_DEFAULT_WAIT_TIMEOUT);
  udisks_linux_block_object_trigger_uevent_sync (UDISKS_LINUX_BLOCK_OBJECT (object),
                                                 UDISKS_DEFAULT_WAIT_TIMEOUT);
  udisks_partition_complete_set_name (partition, invocation);
  udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), TRUE, NULL);

 out:
  if (fd != -1)
    close (fd);
  if (partition_table_object != NULL)
    udisks_linux_block_object_release_cleanup_lock (UDISKS_LINUX_BLOCK_OBJECT (partition_table_object));
  if (object != NULL)
    udisks_linux_block_object_release_cleanup_lock (UDISKS_LINUX_BLOCK_OBJECT (object));
  if (state != NULL)
    udisks_state_check (state);
  g_free (device_name);
  g_free (partition_name);
  g_clear_error (&error);
  g_clear_object (&object);
  g_clear_object (&block);
  g_clear_object (&partition_table_object);
  g_clear_object (&partition_table);
  g_clear_object (&partition_table_block);
  g_clear_object (&object);

  return TRUE; /* returning TRUE means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
is_valid_uuid (const gchar *str)
{
  gboolean ret = FALSE;
  guint groups[] = {8, 4, 4, 4, 12, 0};
  guint pos, n, m;

  if (strlen (str) != 36)
    goto out;

  pos = 0;
  for (n = 0; groups[n] != 0; n++)
    {
      if (pos > 0)
        {
          if (str[pos++] != '-')
            goto out;
        }
      for (m = 0; m < groups[n]; m++)
        {
          if (!g_ascii_isxdigit (str[pos++]))
            goto out;
        }
    }

  ret = TRUE;

 out:
  return ret;
}

/**
 * udisks_linux_partition_set_type_sync:
 * @partition: A #UDisksLinuxPartition.
 * @type: The partition type to set.
 * @lock_held: Whether to avoid acquiring an advisory BSD lock or not (e.g. chained call from Format()).
 * @caller_uid: The uid of the process requesting this change or 0.
 * @cancellable: A #GCancellable or %NULL.
 * @error: Return location for error or %NULL.
 *
 * Sets the partition type. The calling thread is blocked while the
 * operation is pending.
 *
 * Returns: %TRUE if the operation succeeded, %FALSE if error is set.
 */
gboolean
udisks_linux_partition_set_type_sync (UDisksLinuxPartition  *partition,
                                      const gchar           *type,
                                      gboolean               lock_held,
                                      uid_t                  caller_uid,
                                      GCancellable          *cancellable,
                                      GError               **error)
{
  gboolean ret = FALSE;
  UDisksBlock *block = NULL;
  UDisksObject *object = NULL;
  UDisksDaemon *daemon = NULL;
  UDisksState *state = NULL;
  gchar *device_name = NULL;
  UDisksObject *partition_table_object = NULL;
  UDisksPartitionTable *partition_table = NULL;
  UDisksBlock *partition_table_block = NULL;
  int fd = -1;
  gchar *partition_name = NULL;
  GError *loc_error = NULL;
  UDisksBaseJob *job = NULL;

  object = udisks_daemon_util_dup_object (partition, error);
  if (object == NULL)
    goto out;

  daemon = udisks_linux_block_object_get_daemon (UDISKS_LINUX_BLOCK_OBJECT (object));
  state = udisks_daemon_get_state (daemon);
  block = udisks_object_get_block (object);

  partition_table_object = udisks_daemon_find_object (daemon, udisks_partition_get_table (UDISKS_PARTITION (partition)));
  partition_table = udisks_object_get_partition_table (partition_table_object);
  partition_table_block = udisks_object_get_block (partition_table_object);

  device_name = udisks_block_dup_device (partition_table_block);
  partition_name = udisks_block_dup_device (block);

  if (!lock_held)
    {
      udisks_linux_block_object_lock_for_cleanup (UDISKS_LINUX_BLOCK_OBJECT (object));
      udisks_linux_block_object_lock_for_cleanup (UDISKS_LINUX_BLOCK_OBJECT (partition_table_object));
      udisks_state_check_block (state, udisks_linux_block_object_get_device_number (UDISKS_LINUX_BLOCK_OBJECT (object)));
      udisks_state_check_block (state, udisks_linux_block_object_get_device_number (UDISKS_LINUX_BLOCK_OBJECT (partition_table_object)));
    }

  job = udisks_daemon_launch_simple_job (daemon,
                                         UDISKS_OBJECT (object),
                                         "partition-modify",
                                         caller_uid,
                                         NULL);

  if (job == NULL)
    {
      g_set_error (error, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                   "Failed to create a job object");
      goto out;
    }

  /* hold a file descriptor open to suppress BLKRRPART generated by the tools */
  if (!lock_held)
    {
      fd = udisks_linux_block_object_acquire_bsd_lock (UDISKS_LINUX_BLOCK_OBJECT (partition_table_object), TRUE, &loc_error);
      if (fd < 0)
        {
          udisks_debug ("%s", loc_error->message);
          g_clear_error (&loc_error);
        }
    }

  if (g_strcmp0 (udisks_partition_table_get_type_ (partition_table), "gpt") == 0)
    {
      /* check that it's a valid GUID */
      if (!is_valid_uuid (type))
        {
          g_set_error (error,
                       UDISKS_ERROR,
                       UDISKS_ERROR_FAILED,
                       "Given type `%s' is not a valid UUID",
                       type);
          udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), FALSE, (*error)->message);
          goto out;
        }

      if (!bd_part_set_part_type (device_name, partition_name, type, &loc_error))
        {
          g_set_error (error,
                       UDISKS_ERROR,
                       UDISKS_ERROR_FAILED,
                       "Error setting partition type on %s: %s",
                       udisks_block_get_device (block),
                       loc_error->message);
          udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), FALSE, (*error)->message);
          goto out;
        }
    }
  else if (g_strcmp0 (udisks_partition_table_get_type_ (partition_table), "dos") == 0)
    {
      gchar *endp;
      guint type_as_int = strtoul (type, &endp, 0);
      if (strlen (type) == 0 || *endp != '\0')
        {
          g_set_error (error,
                       UDISKS_ERROR,
                       UDISKS_ERROR_FAILED,
                       "Given type `%s' is not a valid",
                       type);
          udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), FALSE, (*error)->message);
          goto out;
        }
      if (type_as_int == 0x05 || type_as_int == 0x0f || type_as_int == 0x85)
        {
          g_set_error (error,
                       UDISKS_ERROR,
                       UDISKS_ERROR_FAILED,
                       "Refusing to change partition type to that of an extended partition. "
                       "Delete the partition and create a new extended partition instead.");
          udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), FALSE, (*error)->message);
          goto out;
        }
      if (!bd_part_set_part_id (device_name, partition_name, type, &loc_error))
        {
          g_set_error (error,
                       UDISKS_ERROR,
                       UDISKS_ERROR_FAILED,
                       "Error setting partition type on %s: %s",
                       udisks_block_get_device (block),
                       loc_error->message);
          udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), FALSE, (*error)->message);
          goto out;
        }
    }
  else
    {
      g_set_error (error,
                   UDISKS_ERROR,
                   UDISKS_ERROR_NOT_SUPPORTED,
                   "No support for modifying a partition a table of type `%s'",
                   udisks_partition_table_get_type_ (partition_table));
      udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), FALSE, (*error)->message);
      goto out;
    }

  /* unlock and re-read */
  if (fd >= 0)
    {
      close (fd);
      fd = -1;
    }
  if (!udisks_linux_block_object_reread_partition_table (UDISKS_LINUX_BLOCK_OBJECT (partition_table_object), &loc_error))
    {
      udisks_warning ("%s", loc_error->message);
      g_clear_error (&loc_error);
    }
  udisks_linux_block_object_trigger_uevent_sync (UDISKS_LINUX_BLOCK_OBJECT (partition_table_object),
                                                 UDISKS_DEFAULT_WAIT_TIMEOUT);
  udisks_linux_block_object_trigger_uevent_sync (UDISKS_LINUX_BLOCK_OBJECT (object),
                                                 UDISKS_DEFAULT_WAIT_TIMEOUT);

  ret = TRUE;
  udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), TRUE, NULL);

 out:
  if (fd != -1)
    close (fd);
  if (!lock_held)
    {
      if (partition_table_object != NULL)
        udisks_linux_block_object_release_cleanup_lock (UDISKS_LINUX_BLOCK_OBJECT (partition_table_object));
      if (object != NULL)
        udisks_linux_block_object_release_cleanup_lock (UDISKS_LINUX_BLOCK_OBJECT (object));
      if (state != NULL)
        udisks_state_check (state);
    }
  g_free (partition_name);
  g_free (device_name);
  g_clear_object (&object);
  g_clear_object (&block);
  g_clear_object (&partition_table_object);
  g_clear_object (&partition_table);
  g_clear_object (&partition_table_block);
  g_clear_object (&object);
  g_clear_error (&loc_error);

  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

/* runs in thread dedicated to handling @invocation */
static gboolean
handle_set_type (UDisksPartition       *partition,
                 GDBusMethodInvocation *invocation,
                 const gchar           *type,
                 GVariant              *options)
{
  uid_t caller_uid;
  GError *error = NULL;

  if (check_authorization (partition, invocation, options, &caller_uid))
    {
    if (!udisks_linux_partition_set_type_sync (UDISKS_LINUX_PARTITION (partition), type, FALSE, caller_uid, NULL, &error))
      {
        g_dbus_method_invocation_take_error (invocation, error);
      }
    else
      {
        udisks_partition_complete_set_type (partition, invocation);
      }
    }
  return TRUE; /* returning TRUE means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */


typedef struct
{
  const gchar *object_path;
  guint64      new_size;
} WaitForPartitionResizeData;

static UDisksObject *
wait_for_partition_resize (UDisksDaemon *daemon,
                           gpointer      user_data)
{
  WaitForPartitionResizeData *data = user_data;
  UDisksObject *object = NULL;
  UDisksPartition *partition = NULL;

  object = udisks_daemon_find_object (daemon, data->object_path);

  if (object != NULL)
    {
      partition = udisks_object_peek_partition (object);
      if (udisks_object_peek_block (object) == NULL ||
          partition == NULL || udisks_partition_get_size (partition) != data->new_size)
        {
          g_clear_object (&object);
        }
    }

  return object;
}

/* runs in thread dedicated to handling @invocation */
static gboolean
handle_resize (UDisksPartition       *partition,
               GDBusMethodInvocation *invocation,
               guint64                size,
               GVariant              *options)
{
  UDisksBlock *block = NULL;
  UDisksObject *object = NULL;
  UDisksDaemon *daemon = NULL;
  UDisksState *state = NULL;
  UDisksObject *partition_table_object = NULL;
  UDisksBlock *partition_table_block = NULL;
  uid_t caller_uid;
  GError *error = NULL;
  UDisksBaseJob *job = NULL;
  UDisksObject *partition_object = NULL;
  WaitForPartitionResizeData wait_data;
  int fd = -1;
  const gchar *part = NULL;

  if (!check_authorization (partition, invocation, options, &caller_uid))
    {
      goto out;
    }

  object = udisks_daemon_util_dup_object (partition, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      goto out;
    }

  daemon = udisks_linux_block_object_get_daemon (UDISKS_LINUX_BLOCK_OBJECT (object));
  state = udisks_daemon_get_state (daemon);
  block = udisks_object_get_block (object);
  part = udisks_block_get_device (block);
  partition_table_object = udisks_daemon_find_object (daemon, udisks_partition_get_table (partition));
  partition_table_block = udisks_object_get_block (partition_table_object);

  udisks_linux_block_object_lock_for_cleanup (UDISKS_LINUX_BLOCK_OBJECT (object));
  udisks_linux_block_object_lock_for_cleanup (UDISKS_LINUX_BLOCK_OBJECT (partition_table_object));
  udisks_state_check_block (state, udisks_linux_block_object_get_device_number (UDISKS_LINUX_BLOCK_OBJECT (object)));
  udisks_state_check_block (state, udisks_linux_block_object_get_device_number (UDISKS_LINUX_BLOCK_OBJECT (partition_table_object)));

  job = udisks_daemon_launch_simple_job (daemon,
                                         UDISKS_OBJECT (object),
                                         "partition-modify",
                                         caller_uid,
                                         NULL);

  if (job == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                             "Failed to create a job object");
      goto out;
    }

  /* hold a file descriptor open to suppress BLKRRPART generated by the tools */
  fd = udisks_linux_block_object_acquire_bsd_lock (UDISKS_LINUX_BLOCK_OBJECT (partition_table_object), TRUE, &error);
  if (fd < 0)
    {
      udisks_warning ("%s", error->message);
      g_clear_error (&error);
    }

  if (! bd_part_resize_part (udisks_block_get_device (partition_table_block),
                             udisks_block_get_device (block),
                             size, BD_PART_ALIGN_OPTIMAL, &error))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Error resizing partition %s: %s",
                                             udisks_block_get_device (block),
                                             error->message);
      udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), FALSE, error->message);
      goto out;
    }

  /* unlock and re-read */
  if (fd >= 0)
    close (fd);
  if (!udisks_linux_block_object_reread_partition_table (UDISKS_LINUX_BLOCK_OBJECT (partition_table_object), &error))
    {
      udisks_warning ("%s", error->message);
      g_clear_error (&error);
    }
  udisks_linux_block_object_trigger_uevent_sync (UDISKS_LINUX_BLOCK_OBJECT (partition_table_object),
                                                 UDISKS_DEFAULT_WAIT_TIMEOUT);
  udisks_linux_block_object_trigger_uevent_sync (UDISKS_LINUX_BLOCK_OBJECT (object),
                                                 UDISKS_DEFAULT_WAIT_TIMEOUT);

  /* Retrieve actual partition size from the kernel. This needs to be called
   * once the fd is closed and BLKRRPART issued, otherwise it would return
   * an old value.
   */
  wait_data.object_path = g_dbus_object_get_object_path (G_DBUS_OBJECT (object));
  wait_data.new_size = 0; /* hit timeout in case of error */
  fd = open (part, O_RDONLY);
  if (fd != -1) {
    if (ioctl (fd, BLKGETSIZE64, &wait_data.new_size) == -1)
        udisks_warning ("Could not query new partition size for %s", part);
    close (fd);
  } else {
    udisks_warning ("Could not open %s to query new partition size", part);
  }

  partition_object = udisks_daemon_wait_for_object_sync (daemon,
                                                         wait_for_partition_resize,
                                                         &wait_data,
                                                         NULL,
                                                         UDISKS_DEFAULT_WAIT_TIMEOUT,
                                                         NULL);

  udisks_partition_complete_resize (partition, invocation);
  udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), TRUE, NULL);

 out:
  if (fd != -1)
    close (fd);
  if (partition_table_object != NULL)
    udisks_linux_block_object_release_cleanup_lock (UDISKS_LINUX_BLOCK_OBJECT (partition_table_object));
  if (object != NULL)
    udisks_linux_block_object_release_cleanup_lock (UDISKS_LINUX_BLOCK_OBJECT (object));
  if (state != NULL)
    udisks_state_check (state);
  g_clear_error (&error);
  g_clear_object (&object);
  g_clear_object (&block);
  g_clear_object (&partition_object);
  g_clear_object (&partition_table_object);
  g_clear_object (&partition_table_block);

  return TRUE; /* returning TRUE means that we handled the method invocation */

}

/* ---------------------------------------------------------------------------------------------------- */

/* runs in thread dedicated to handling @invocation */
static gboolean
handle_delete (UDisksPartition       *partition,
               GDBusMethodInvocation *invocation,
               GVariant              *options)
{
  UDisksBlock *block = NULL;
  UDisksObject *object = NULL;
  UDisksDaemon *daemon = NULL;
  UDisksState *state = NULL;
  gchar *device_name = NULL;
  gchar *partition_name = NULL;
  UDisksObject *partition_table_object = NULL;
  UDisksBlock *partition_table_block = NULL;
  uid_t caller_uid;
  gboolean teardown_flag = FALSE;
  GError *error = NULL;
  UDisksBaseJob *job = NULL;
  int fd = -1;

  g_variant_lookup (options, "tear-down", "b", &teardown_flag);

  if (!check_authorization (partition, invocation, options, &caller_uid))
    {
      goto out;
    }

  object = udisks_daemon_util_dup_object (partition, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      goto out;
    }

  daemon = udisks_linux_block_object_get_daemon (UDISKS_LINUX_BLOCK_OBJECT (object));
  state = udisks_daemon_get_state (daemon);
  block = udisks_object_get_block (object);
  partition_table_object = udisks_daemon_find_object (daemon, udisks_partition_get_table (partition));
  partition_table_block = udisks_object_get_block (partition_table_object);

  udisks_linux_block_object_lock_for_cleanup (UDISKS_LINUX_BLOCK_OBJECT (object));
  udisks_linux_block_object_lock_for_cleanup (UDISKS_LINUX_BLOCK_OBJECT (partition_table_object));
  udisks_state_check_block (state, udisks_linux_block_object_get_device_number (UDISKS_LINUX_BLOCK_OBJECT (object)));
  udisks_state_check_block (state, udisks_linux_block_object_get_device_number (UDISKS_LINUX_BLOCK_OBJECT (partition_table_object)));

  if (teardown_flag)
    {
      if (!udisks_linux_block_teardown (block, invocation, options, &error))
        {
          if (invocation != NULL)
            g_dbus_method_invocation_return_gerror (invocation, error);
          goto out;
        }
    }

  device_name = g_strdup (udisks_block_get_device (partition_table_block));
  partition_name = g_strdup (udisks_block_get_device (block));

  job = udisks_daemon_launch_simple_job (daemon,
                                         UDISKS_OBJECT (object),
                                         "partition-delete",
                                         caller_uid,
                                         NULL);

  if (job == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                             "Failed to create a job object");
      goto out;
    }

  /* hold a file descriptor open to suppress BLKRRPART generated by the tools */
  fd = udisks_linux_block_object_acquire_bsd_lock (UDISKS_LINUX_BLOCK_OBJECT (partition_table_object), TRUE, &error);
  if (fd < 0)
    {
      udisks_debug ("%s", error->message);
      g_clear_error (&error);
    }

  if (! bd_part_delete_part (device_name, partition_name, &error))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Error deleting partition %s: %s",
                                             udisks_block_get_device (block),
                                             error->message);
      udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), FALSE, error->message);
      goto out;
    }

  /* unlock and re-read */
  if (fd >= 0)
    {
      close (fd);
      fd = -1;
    }
  if (!udisks_linux_block_object_reread_partition_table (UDISKS_LINUX_BLOCK_OBJECT (partition_table_object), &error))
    {
      udisks_warning ("%s", error->message);
      g_clear_error (&error);
    }
  udisks_linux_block_object_trigger_uevent_sync (UDISKS_LINUX_BLOCK_OBJECT (partition_table_object),
                                                 UDISKS_DEFAULT_WAIT_TIMEOUT);
  /* not trigerring uevent on @object, the block device should be gone by now */

  udisks_partition_complete_delete (partition, invocation);
  udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), TRUE, NULL);

 out:
  if (fd != -1)
    close (fd);
  if (partition_table_object != NULL)
    udisks_linux_block_object_release_cleanup_lock (UDISKS_LINUX_BLOCK_OBJECT (partition_table_object));
  if (object != NULL)
    udisks_linux_block_object_release_cleanup_lock (UDISKS_LINUX_BLOCK_OBJECT (object));
  if (state != NULL)
    udisks_state_check (state);
  g_free (device_name);
  g_free (partition_name);
  g_clear_error (&error);
  g_clear_object (&object);
  g_clear_object (&block);
  g_clear_object (&partition_table_object);
  g_clear_object (&partition_table_block);
  g_clear_object (&object);

  return TRUE; /* returning TRUE means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

static void
partition_iface_init (UDisksPartitionIface *iface)
{
  iface->handle_set_flags = handle_set_flags;
  iface->handle_set_name  = handle_set_name;
  iface->handle_set_type  = handle_set_type;
  iface->handle_resize    = handle_resize;
  iface->handle_delete    = handle_delete;
}
