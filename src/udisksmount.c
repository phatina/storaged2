/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2008 David Zeuthen <zeuthen@gmail.com>
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

#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <mntent.h>

#include <glib.h>
#include <glib-object.h>

#include "udisksmount.h"
#include "udisksprivate.h"

/**
 * UDisksMount:
 *
 * The #UDisksMount structure contains only private data and should
 * only be accessed using the provided API.
 */
struct _UDisksMount
{
  GObject parent_instance;

  gchar *mount_path;
  gchar *fs_uuid;
  dev_t dev;
  UDisksMountType type;
};

typedef struct _UDisksMountClass UDisksMountClass;

struct _UDisksMountClass
{
  GObjectClass parent_class;
};

G_DEFINE_TYPE (UDisksMount, udisks_mount, G_TYPE_OBJECT);

static void
udisks_mount_finalize (GObject *object)
{
  UDisksMount *mount = UDISKS_MOUNT (object);

  g_free (mount->mount_path);
  g_free (mount->fs_uuid);

  if (G_OBJECT_CLASS (udisks_mount_parent_class)->finalize)
    G_OBJECT_CLASS (udisks_mount_parent_class)->finalize (object);
}

static void
udisks_mount_init (UDisksMount *mount)
{
}

static void
udisks_mount_class_init (UDisksMountClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize = udisks_mount_finalize;
}

UDisksMount *
_udisks_mount_new (dev_t            dev,
                   const gchar     *mount_path,
                   const gchar     *fs_uuid,
                   UDisksMountType  type)
{
  UDisksMount *mount;

  mount = UDISKS_MOUNT (g_object_new (UDISKS_TYPE_MOUNT, NULL));
  mount->dev = dev;
  mount->mount_path = g_strdup (mount_path);
  mount->fs_uuid = g_strdup (fs_uuid);
  mount->type = type;

  return mount;
}

/**
 * udisks_mount_get_mount_path:
 * @mount: A #UDisksMount
 *
 * Gets the mount path for a #UDISKS_MOUNT_TYPE_FILESYSTEM<!-- -->-type mount.
 *
 * It is a programming error to call this on any other type of #UDisksMount.
 *
 * Returns: A string owned by @mount. Do not free.
 */
const gchar *
udisks_mount_get_mount_path (UDisksMount *mount)
{
  g_return_val_if_fail (UDISKS_IS_MOUNT (mount), NULL);
  g_return_val_if_fail (mount->type == UDISKS_MOUNT_TYPE_FILESYSTEM, NULL);
  return mount->mount_path;
}

/**
 * udisks_mount_get_fs_uuid:
 * @mount: A #UDisksMount
 *
 * Gets the filesystem UUID for a #UDISKS_MOUNT_TYPE_FILESYSTEM<!-- -->-type mount.
 *
 * It is a programming error to call this on any other type of #UDisksMount.
 *
 * Returns: A string owned by @mount. Do not free.
 */
const gchar *
udisks_mount_get_fs_uuid (UDisksMount *mount)
{
  g_return_val_if_fail (UDISKS_IS_MOUNT (mount), NULL);
  g_return_val_if_fail (mount->type == UDISKS_MOUNT_TYPE_FILESYSTEM, NULL);
  return mount->fs_uuid;
}

/**
 * udisks_mount_get_dev:
 * @mount: A #UDisksMount.
 *
 * Gets the device number for @mount.
 *
 * Returns: A #dev_t.
 */
dev_t
udisks_mount_get_dev (UDisksMount *mount)
{
  g_return_val_if_fail (UDISKS_IS_MOUNT (mount), 0);
  return mount->dev;
}

/**
 * udisks_mount_compare:
 * @mount: A #UDisksMount
 * @other_mount: Another #UDisksMount.
 *
 * Comparison function for comparing two #UDisksMount objects.
 *
 * Returns: Negative value if @mount < @other_mount; zero if @mount = @other_mount; positive value if @mount > @other_mount.
 */
gint
udisks_mount_compare (UDisksMount  *mount,
                      UDisksMount  *other_mount)
{
  gint ret;

  g_return_val_if_fail (UDISKS_IS_MOUNT (mount), 0);
  g_return_val_if_fail (UDISKS_IS_MOUNT (other_mount), 0);

  ret = g_strcmp0 (mount->mount_path, other_mount->mount_path);
  if (ret != 0)
    goto out;

  ret = g_strcmp0 (mount->fs_uuid, other_mount->fs_uuid);
  if (ret != 0)
    goto out;

  ret = (other_mount->dev - mount->dev);
  if (ret != 0)
    goto out;

  ret = other_mount->type - mount->type;

 out:
  return ret;
}

/**
 * udisks_mount_get_mount_type:
 * @mount: A #UDisksMount.
 *
 * Gets the #UDisksMountType for @mount.
 *
 * Returns: A value from the #UDisksMountType enumeration.
 */
UDisksMountType
udisks_mount_get_mount_type (UDisksMount *mount)
{
  return mount->type;
}
