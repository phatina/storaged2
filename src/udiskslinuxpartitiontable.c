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
#include <fcntl.h>
#include <sys/file.h>

#include <glib/gstdio.h>

#include <blockdev/part.h>
#include <blockdev/fs.h>

#include "udiskslogging.h"
#include "udiskslinuxpartitiontable.h"
#include "udiskslinuxblockobject.h"
#include "udisksdaemon.h"
#include "udisksdaemonutil.h"
#include "udiskslinuxdevice.h"
#include "udiskslinuxblock.h"
#include "udiskslinuxpartition.h"
#include "udiskssimplejob.h"
#include "udisksstate.h"

/**
 * SECTION:udiskslinuxpartitiontable
 * @title: UDisksLinuxPartitionTable
 * @short_description: Linux implementation of #UDisksPartitionTable
 *
 * This type provides an implementation of the #UDisksPartitionTable
 * interface on Linux.
 */

typedef struct _UDisksLinuxPartitionTableClass   UDisksLinuxPartitionTableClass;

/**
 * UDisksLinuxPartitionTable:
 *
 * The #UDisksLinuxPartitionTable structure contains only private data and should
 * only be accessed using the provided API.
 */
struct _UDisksLinuxPartitionTable
{
  UDisksPartitionTableSkeleton parent_instance;
};

struct _UDisksLinuxPartitionTableClass
{
  UDisksPartitionTableSkeletonClass parent_class;
};

static void partition_table_iface_init (UDisksPartitionTableIface *iface);

G_DEFINE_TYPE_WITH_CODE (UDisksLinuxPartitionTable, udisks_linux_partition_table, UDISKS_TYPE_PARTITION_TABLE_SKELETON,
                         G_IMPLEMENT_INTERFACE (UDISKS_TYPE_PARTITION_TABLE, partition_table_iface_init));

/* ---------------------------------------------------------------------------------------------------- */

static void
udisks_linux_partition_table_init (UDisksLinuxPartitionTable *partition_table)
{
  g_dbus_interface_skeleton_set_flags (G_DBUS_INTERFACE_SKELETON (partition_table),
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);
}

static void
udisks_linux_partition_table_class_init (UDisksLinuxPartitionTableClass *klass)
{
}

/**
 * udisks_linux_partition_table_new:
 *
 * Creates a new #UDisksLinuxPartitionTable instance.
 *
 * Returns: A new #UDisksLinuxPartitionTable. Free with g_object_unref().
 */
UDisksPartitionTable *
udisks_linux_partition_table_new (void)
{
  return UDISKS_PARTITION_TABLE (g_object_new (UDISKS_TYPE_LINUX_PARTITION_TABLE,
                                               NULL));
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_linux_partition_table_update:
 * @table: A #UDisksLinuxPartitionTable.
 * @object: The enclosing #UDisksLinuxBlockObject instance.
 *
 * Updates the interface.
 */
void
udisks_linux_partition_table_update (UDisksLinuxPartitionTable  *table,
                                     UDisksLinuxBlockObject     *object)
{
  const gchar *type = NULL;
  UDisksLinuxDevice *device = NULL;
  UDisksDaemon *daemon = NULL;
  guint num_parts = 0;
  const gchar **partition_object_paths = NULL;
  GList *partition_objects = NULL;
  GList *object_p = NULL;
  guint i = 0;

  /* update partition table type */
  device = udisks_linux_block_object_get_device (object);
  if (device != NULL)
    type = g_udev_device_get_property (device->udev_device, "ID_PART_TABLE_TYPE");

  udisks_partition_table_set_type_ (UDISKS_PARTITION_TABLE (table), type);

  /* update list of partitions */
  daemon = udisks_linux_block_object_get_daemon (UDISKS_LINUX_BLOCK_OBJECT (object));

  partition_objects = udisks_linux_partition_table_get_partitions (daemon, UDISKS_PARTITION_TABLE (table), &num_parts);

  partition_object_paths = g_new0 (const gchar *, num_parts + 1);
  for (i = 0, object_p = partition_objects; object_p != NULL; object_p = object_p->next, i++)
    {
      partition_object_paths[i] = g_dbus_object_get_object_path (g_dbus_interface_get_object (G_DBUS_INTERFACE (object_p->data)));
    }

  udisks_partition_table_set_partitions (UDISKS_PARTITION_TABLE (table),
                                         partition_object_paths);
  g_dbus_interface_skeleton_flush (G_DBUS_INTERFACE_SKELETON (table));


  g_free (partition_object_paths);
  g_clear_object (&device);
  g_list_free_full (partition_objects, g_object_unref);
}

/* ---------------------------------------------------------------------------------------------------- */

GList *
udisks_linux_partition_table_get_partitions (UDisksDaemon         *daemon,
                                             UDisksPartitionTable *table,
                                             guint                *num_partitions)
{
  GList *ret = NULL;
  GDBusObject *table_object;
  const gchar *table_object_path;
  GList *l, *object_proxies = NULL;
  *num_partitions = 0;

  table_object = g_dbus_interface_get_object (G_DBUS_INTERFACE (table));
  if (table_object == NULL)
    goto out;
  table_object_path = g_dbus_object_get_object_path (table_object);

  object_proxies = udisks_daemon_get_objects (daemon);
  for (l = object_proxies; l != NULL; l = l->next)
    {
      UDisksObject *object = UDISKS_OBJECT (l->data);
      UDisksPartition *partition;

      partition = udisks_object_get_partition (object);
      if (partition == NULL)
        continue;

      if (g_strcmp0 (udisks_partition_get_table (partition), table_object_path) == 0)
        {
          ret = g_list_prepend (ret, g_object_ref (partition));
          (*num_partitions)++;
        }

      g_object_unref (partition);
    }
  ret = g_list_reverse (ret);
 out:
  g_list_free_full (object_proxies, g_object_unref);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

typedef struct
{
  UDisksObject *partition_table_object;
  guint64       pos_to_wait_for;
  gboolean      ignore_container;
} WaitForPartitionData;

static UDisksObject *
wait_for_partition (UDisksDaemon *daemon,
                    gpointer      user_data)
{
  WaitForPartitionData *data = user_data;
  UDisksObject *ret = NULL;
  GList *objects, *l;

  objects = udisks_daemon_get_objects (daemon);
  for (l = objects; l != NULL; l = l->next)
    {
      UDisksObject *object = UDISKS_OBJECT (l->data);
      UDisksPartition *partition = udisks_object_get_partition (object);
      if (partition != NULL)
        {
          if (g_strcmp0 (udisks_partition_get_table (partition),
                         g_dbus_object_get_object_path (G_DBUS_OBJECT (data->partition_table_object))) == 0)
            {
              guint64 offset = udisks_partition_get_offset (partition);
              guint64 size = udisks_partition_get_size (partition);

              if (data->pos_to_wait_for >= offset && data->pos_to_wait_for < offset + size)
                {
                  if (!(udisks_partition_get_is_container (partition) && data->ignore_container))
                    {
                      g_object_unref (partition);
                      ret = g_object_ref (object);
                      goto out;
                    }
                }
            }
          g_object_unref (partition);
        }
    }

 out:
  g_list_free_full (objects, g_object_unref);
  return ret;
}

#define MIB_SIZE (1048576L)

static UDisksObject *
create_partition (UDisksPartitionTable   *table,
                  GDBusMethodInvocation  *invocation,
                  guint64                 offset,
                  guint64                 size,
                  const gchar            *type,
                  const gchar            *name,
                  GVariant               *options)
{
  const gchar *action_id = NULL;
  const gchar *message = NULL;
  UDisksBlock *block = NULL;
  UDisksObject *object = NULL;
  UDisksDaemon *daemon = NULL;
  UDisksState *state = NULL;
  gchar *device_name = NULL;
  WaitForPartitionData *wait_data = NULL;
  UDisksObject *partition_object = NULL;
  UDisksBlock *partition_block = NULL;
  BDPartSpec *part_spec = NULL;
  BDPartSpec *overlapping_part = NULL;
  BDPartTypeReq part_type = 0;
  gchar *table_type = NULL;
  uid_t caller_uid;
  GError *error = NULL;
  UDisksBaseJob *job = NULL;
  const gchar *partition_type = NULL;
  int fd = -1;

  object = udisks_daemon_util_dup_object (table, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      goto out;
    }

  daemon = udisks_linux_block_object_get_daemon (UDISKS_LINUX_BLOCK_OBJECT (object));
  state = udisks_daemon_get_state (daemon);

  udisks_linux_block_object_lock_for_cleanup (UDISKS_LINUX_BLOCK_OBJECT (object));
  udisks_state_check_block (state, udisks_linux_block_object_get_device_number (UDISKS_LINUX_BLOCK_OBJECT (object)));

  g_variant_lookup (options, "partition-type", "&s", &partition_type);

  block = udisks_object_get_block (object);
  if (block == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                             "Partition table object is not a block device");
      goto out;
    }

  if (!udisks_daemon_util_get_caller_uid_sync (daemon,
                                               invocation,
                                               NULL /* GCancellable */,
                                               &caller_uid,
                                               &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      goto out;
    }

  action_id = "org.freedesktop.udisks2.modify-device";
  /* Translators: Shown in authentication dialog when the user
   * requests creating a new partition.
   *
   * Do not translate $(drive), it's a placeholder and
   * will be replaced by the name of the drive/device in question
   */
  message = N_("Authentication is required to create a partition on $(drive)");
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

  if (!udisks_daemon_util_check_authorization_sync (daemon,
                                                    object,
                                                    action_id,
                                                    options,
                                                    message,
                                                    invocation))
    goto out;

  table_type = udisks_partition_table_dup_type_ (table);
  if (g_strcmp0 (table_type, "dos") == 0)
    {
      char *endp;
      gint type_as_int;

      if (strlen (name) > 0)
        {
          g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                                 "MBR partition table does not support names");
          goto out;
        }

      type_as_int = strtol (type, &endp, 0);

      /* Determine whether we are creating a primary, extended or logical partition */
      if (partition_type != NULL)
        {
          if (g_strcmp0 (partition_type, "primary") == 0)
            {
              part_type = BD_PART_TYPE_REQ_NORMAL;
            }
          else if (g_strcmp0 (partition_type, "extended") == 0)
            {
              part_type = BD_PART_TYPE_REQ_EXTENDED;
            }
          else if (g_strcmp0 (partition_type, "logical") == 0)
            {
              part_type = BD_PART_TYPE_REQ_LOGICAL;
            }
          else
            {
              g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                                     "Don't know how to create partition of type `%s'",
                                                     partition_type);
              goto out;
            }
        }
      else if (type[0] != '\0' && *endp == '\0' &&
               (type_as_int == 0x05 || type_as_int == 0x0f || type_as_int == 0x85))
        {
          part_type = BD_PART_TYPE_REQ_EXTENDED;
        }
      else
        part_type = BD_PART_TYPE_REQ_NEXT;
    }
  else if (g_strcmp0 (table_type, "gpt") == 0)
    {
      part_type = BD_PART_TYPE_REQ_NORMAL;
    }
  else
    {
      g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                             "Don't know how to create partitions this partition table of type `%s'",
                                             table_type);
      goto out;
    }

  job = udisks_daemon_launch_simple_job (daemon,
                                         UDISKS_OBJECT (object),
                                         "partition-create",
                                         caller_uid,
                                         NULL);

  if (job == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                             "Failed to create a job object");
      goto out;
    }

  /* Lock the device to prevent udev issuing a BLKRRPART ioctl
   * call until we're done with changes.
   */
  fd = udisks_linux_block_object_acquire_bsd_lock (UDISKS_LINUX_BLOCK_OBJECT (object), TRUE, &error);
  if (fd < 0)
    {
      udisks_warning ("%s", error->message);
      g_clear_error (&error);
    }

  /* Users might want to specify logical partitions start and size using size of
   * of the extended partition. If this happens we need to shift start (offset)
   * of the logical partition.
   * XXX: We really shouldn't allow creation of overlapping partitions and
   *      and just automatically fix this. But udisks currently doesn't have
   *      functions to get free regions on the disks so this is a somewhat valid
   *      use case. But we should definitely provide some functionality to get
   *      right "numbers" and stop doing this.
  */
  device_name = g_strdup (udisks_block_get_device (block));
  overlapping_part = bd_part_get_part_by_pos (device_name, offset, &error);
  if (overlapping_part != NULL && ! (overlapping_part->type & BD_PART_TYPE_FREESPACE))
    {
      /* extended partition or metadata of the extended partition */
      if (overlapping_part->type & BD_PART_TYPE_EXTENDED || overlapping_part->type & (BD_PART_TYPE_LOGICAL | BD_PART_TYPE_METADATA))
        {
          if (overlapping_part->start == offset)
            {
              /* just add 1 byte, libblockdev will adjust it */
              offset += 1;
              udisks_warning ("Requested start of the logical partition overlaps "
                              "with extended partition metadata. Start of the "
                              "partition moved to %"G_GUINT64_FORMAT".", offset);
            }
        }
      else
        {
          /* overlapping partition is not a free space nor an extended part -> error */
          g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                                 "Requested start for the new partition %"G_GUINT64_FORMAT" "
                                                 "overlaps with existing partition %s.",
                                                  offset, overlapping_part->path);
          goto out;
        }
    }
  else
    g_clear_error (&error);

  part_spec = bd_part_create_part (device_name, part_type, offset,
                                   size, BD_PART_ALIGN_OPTIMAL, &error);
  if (!part_spec)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Error creating partition on %s: %s",
                                             device_name,
                                             error->message);
      udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), FALSE, error->message);
      goto out;
    }

  /* set name if given */
  if (g_strcmp0 (table_type, "gpt") == 0 && strlen (name) > 0)
    {
      if (!bd_part_set_part_name (device_name, part_spec->path, name, &error))
        {
          g_prefix_error (&error, "Error setting name for newly created partition: ");
          g_dbus_method_invocation_return_gerror (invocation, error);
          udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), FALSE, error->message);
          goto out;
        }
    }

  /* set type if given and if not extended partition */
  if (part_spec->type != BD_PART_TYPE_EXTENDED && strlen (type) > 0)
    {
      gboolean ret = FALSE;

      if (g_strcmp0 (table_type, "gpt") == 0)
          ret = bd_part_set_part_type (device_name, part_spec->path, type, &error);
      else if (g_strcmp0 (table_type, "dos") == 0)
          ret = bd_part_set_part_id (device_name, part_spec->path, type, &error);

      if (!ret)
        {
          g_prefix_error (&error, "Error setting type for newly created partition: ");
          g_dbus_method_invocation_return_gerror (invocation, error);
          udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), FALSE, error->message);
          goto out;
        }
    }

  /* wipe the newly created partition if wanted */
  if (part_spec->type != BD_PART_TYPE_EXTENDED)
    {
      if (!bd_fs_wipe (part_spec->path, TRUE, FALSE, &error))
        {
          if (g_error_matches (error, BD_FS_ERROR, BD_FS_ERROR_NOFS))
            g_clear_error (&error);
          else
            {
              g_dbus_method_invocation_return_error (invocation,
                                                     UDISKS_ERROR,
                                                     UDISKS_ERROR_FAILED,
                                                     "Error wiping newly created partition %s: %s",
                                                     part_spec->path,
                                                     error->message);
              udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), FALSE, error->message);
              goto out;
            }
        }
    }

  /* hard work done, unlock and refresh */
  if (fd >= 0)
    {
      close (fd);
      fd = -1;
    }

  if (!udisks_linux_block_object_reread_partition_table (UDISKS_LINUX_BLOCK_OBJECT (object), &error))
    {
      udisks_warning ("%s", error->message);
      g_clear_error (&error);
    }
  udisks_linux_block_object_trigger_uevent_sync (UDISKS_LINUX_BLOCK_OBJECT (object),
                                                 UDISKS_DEFAULT_WAIT_TIMEOUT);

  /* sit and wait for the partition to show up */
  wait_data = g_new0 (WaitForPartitionData, 1);
  wait_data->partition_table_object = object;
  wait_data->ignore_container = (part_spec->type == BD_PART_TYPE_LOGICAL);
  wait_data->pos_to_wait_for = part_spec->start + (part_spec->size / 2L);
  g_warn_if_fail (wait_data->pos_to_wait_for > 0);
  partition_object = udisks_daemon_wait_for_object_sync (daemon,
                                                         wait_for_partition,
                                                         wait_data,
                                                         NULL,
                                                         UDISKS_DEFAULT_WAIT_TIMEOUT,
                                                         &error);
  if (partition_object == NULL)
    {
      g_prefix_error (&error, "Error waiting for partition to appear: ");
      g_dbus_method_invocation_return_gerror (invocation, error);
      udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), FALSE, error->message);
      goto out;
    }
  partition_block = udisks_object_get_block (partition_object);
  if (partition_block == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                             "Partition object is not a block device");
      g_clear_object (&partition_object);
      udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), FALSE, NULL);
      goto out;
    }

  udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), TRUE, NULL);

 out:
  if (fd >= 0)
    close (fd);
  if (object != NULL)
    udisks_linux_block_object_release_cleanup_lock (UDISKS_LINUX_BLOCK_OBJECT (object));
  if (state != NULL)
    udisks_state_check (state);
  g_free (table_type);
  g_free (wait_data);
  g_clear_error (&error);
  g_clear_object (&partition_block);
  g_free (device_name);
  g_clear_object (&object);
  g_clear_object (&block);
  if (part_spec)
    bd_part_spec_free (part_spec);
  if (overlapping_part)
    bd_part_spec_free (overlapping_part);
  return partition_object;
}

/* runs in thread dedicated to handling @invocation */
static gboolean
handle_create_partition (UDisksPartitionTable   *table,
                         GDBusMethodInvocation  *invocation,
                         guint64                 offset,
                         guint64                 size,
                         const gchar            *type,
                         const gchar            *name,
                         GVariant               *options)
{
  UDisksObject *partition_object;

  partition_object = create_partition (table, invocation, offset, size, type, name, options);
  if (partition_object)
    {
      udisks_partition_table_complete_create_partition (table,
                                                        invocation,
                                                        g_dbus_object_get_object_path (G_DBUS_OBJECT (partition_object)));
      g_object_unref (partition_object);
    }

  return TRUE; /* returning TRUE means that we handled the method invocation */
}

/* runs in thread dedicated to handling @invocation */
struct FormatCompleteData {
  UDisksPartitionTable *table;
  GDBusMethodInvocation *invocation;
  UDisksObject *partition_object;
};

static void
handle_format_complete (gpointer user_data)
{
  struct FormatCompleteData *data = user_data;

  udisks_partition_table_complete_create_partition (data->table,
                                                    data->invocation,
                                                    g_dbus_object_get_object_path (G_DBUS_OBJECT (data->partition_object)));
}

static gboolean
handle_create_partition_and_format (UDisksPartitionTable   *table,
                                    GDBusMethodInvocation  *invocation,
                                    guint64                 offset,
                                    guint64                 size,
                                    const gchar            *type,
                                    const gchar            *name,
                                    GVariant               *options,
                                    const gchar            *format_type,
                                    GVariant               *format_options)
{
  UDisksObject *partition_object;

  partition_object = create_partition (table, invocation, offset, size, type, name, options);
  if (partition_object)
    {
      struct FormatCompleteData data;
      data.table = table;
      data.invocation = invocation;
      data.partition_object = partition_object;
      udisks_linux_block_handle_format (udisks_object_peek_block (partition_object),
                                        invocation,
                                        format_type,
                                        format_options,
                                        handle_format_complete, &data);
      g_object_unref (partition_object);
    }

  return TRUE; /* returning TRUE means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

static void
partition_table_iface_init (UDisksPartitionTableIface *iface)
{
  iface->handle_create_partition = handle_create_partition;
  iface->handle_create_partition_and_format = handle_create_partition_and_format;
}
