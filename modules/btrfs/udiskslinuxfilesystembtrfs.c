/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Peter Hatina <phatina@redhat.com>
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
 */

#include "config.h"

#include <glib/gi18n.h>

#include <src/udisksdaemon.h>
#include <src/udisksdaemonutil.h>
#include <src/udiskslinuxblockobject.h>
#include <src/udiskslinuxdevice.h>
#include <src/udiskslinuxfilesystem.h>
#include <src/udiskslogging.h>
#include <src/udisksmodule.h>
#include <src/udisksmoduleobject.h>
#include <blockdev/btrfs.h>

#include "udiskslinuxmodulebtrfs.h"
#include "udiskslinuxfilesystembtrfs.h"
#include "udisksbtrfsutil.h"

/**
 * SECTION:udiskslinuxfilesystembtrfs
 * @title: UDisksLinuxFilesystemBTRFS
 * @short_description: Linux implementation of #UDisksFilesystemBTRFS
 *
 * This type provides an implementation of #UDisksFilesystemBTRFS interface
 * on Linux.
 */

/**
 * UDisksLinuxFilesystemBTRFS:
 *
 * The #UDisksLinuxFilesystemBTRFS structure contains only private data and
 * should be only accessed using provided API.
 */
struct _UDisksLinuxFilesystemBTRFS {
  UDisksFilesystemBTRFSSkeleton parent_instance;

  UDisksLinuxModuleBTRFS *module;
  UDisksLinuxBlockObject *block_object;
};

struct _UDisksLinuxFilesystemBTRFSClass {
  UDisksFilesystemBTRFSSkeletonClass parent_class;
};

typedef gboolean (*btrfs_subvolume_func)(const gchar *mount_point, const gchar *name, const BDExtraArg **extra, GError **error);
typedef gboolean (*btrfs_device_func)(const gchar *mountpoint, const gchar *device, const BDExtraArg **extra, GError **error);

enum
{
  PROP_0,
  PROP_MODULE,
  PROP_BLOCK_OBJECT,
  N_PROPERTIES
};

static void udisks_linux_filesystem_btrfs_iface_init (UDisksFilesystemBTRFSIface *iface);
static void udisks_linux_filesystem_btrfs_module_object_iface_init (UDisksModuleObjectIface *iface);

G_DEFINE_TYPE_WITH_CODE (UDisksLinuxFilesystemBTRFS, udisks_linux_filesystem_btrfs, UDISKS_TYPE_FILESYSTEM_BTRFS_SKELETON,
                         G_IMPLEMENT_INTERFACE (UDISKS_TYPE_FILESYSTEM_BTRFS, udisks_linux_filesystem_btrfs_iface_init)
                         G_IMPLEMENT_INTERFACE (UDISKS_TYPE_MODULE_OBJECT, udisks_linux_filesystem_btrfs_module_object_iface_init));

static void
udisks_linux_filesystem_btrfs_get_property (GObject    *object,
                                            guint       property_id,
                                            GValue     *value,
                                            GParamSpec *pspec)
{
  UDisksLinuxFilesystemBTRFS *l_fs_btrfs = UDISKS_LINUX_FILESYSTEM_BTRFS (object);

  switch (property_id)
    {
    case PROP_MODULE:
      g_value_set_object (value, UDISKS_MODULE (udisks_linux_filesystem_btrfs_get_module (l_fs_btrfs)));
      break;

    case PROP_BLOCK_OBJECT:
      g_value_set_object (value, l_fs_btrfs->block_object);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
udisks_linux_filesystem_btrfs_set_property (GObject      *object,
                                            guint         property_id,
                                            const GValue *value,
                                            GParamSpec   *pspec)
{
  UDisksLinuxFilesystemBTRFS *l_fs_btrfs = UDISKS_LINUX_FILESYSTEM_BTRFS (object);

  switch (property_id)
    {
    case PROP_MODULE:
      g_assert (l_fs_btrfs->module == NULL);
      l_fs_btrfs->module = UDISKS_LINUX_MODULE_BTRFS (g_value_dup_object (value));
      break;

    case PROP_BLOCK_OBJECT:
      g_assert (l_fs_btrfs->block_object == NULL);
      /* we don't take reference to block_object */
      l_fs_btrfs->block_object = g_value_get_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
udisks_linux_filesystem_btrfs_finalize (GObject *object)
{
  UDisksLinuxFilesystemBTRFS *l_fs_btrfs = UDISKS_LINUX_FILESYSTEM_BTRFS (object);

  /* we don't take reference to block_object */
  g_object_unref (l_fs_btrfs->module);

  if (G_OBJECT_CLASS (udisks_linux_filesystem_btrfs_parent_class))
    G_OBJECT_CLASS (udisks_linux_filesystem_btrfs_parent_class)->finalize (object);
}

static void
udisks_linux_filesystem_btrfs_class_init (UDisksLinuxFilesystemBTRFSClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->get_property = udisks_linux_filesystem_btrfs_get_property;
  gobject_class->set_property = udisks_linux_filesystem_btrfs_set_property;
  gobject_class->finalize = udisks_linux_filesystem_btrfs_finalize;

  /**
   * UDisksLinuxFilesystemBTRFS:module:
   *
   * The #UDisksModule for the object.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_MODULE,
                                   g_param_spec_object ("module",
                                                        "Module",
                                                        "The module for the object",
                                                        UDISKS_TYPE_MODULE,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_WRITABLE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));
  /**
   * UDisksLinuxFilesystemBTRFS:blockobject:
   *
   * The #UDisksLinuxBlockObject for the object.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_BLOCK_OBJECT,
                                   g_param_spec_object ("blockobject",
                                                        "Block object",
                                                        "The block object for the interface",
                                                        UDISKS_TYPE_LINUX_BLOCK_OBJECT,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_WRITABLE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));
}

static void
udisks_linux_filesystem_btrfs_init (UDisksLinuxFilesystemBTRFS *l_fs_btrfs)
{
  g_dbus_interface_skeleton_set_flags (G_DBUS_INTERFACE_SKELETON (l_fs_btrfs),
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);
}

/**
 * udisks_linux_filesystem_btrfs_new:
 * @module: A #UDisksLinuxModuleBTRFS.
 * @block_object: A #UDisksLinuxBlockObject.
 *
 * Creates a new #UDisksLinuxFilesystemBTRFS instance.
 *
 * Returns: A new #UDisksLinuxFilesystemBTRFS. Free with g_object_unref().
 */
UDisksLinuxFilesystemBTRFS *
udisks_linux_filesystem_btrfs_new (UDisksLinuxModuleBTRFS *module,
                                   UDisksLinuxBlockObject *block_object)
{
  g_return_val_if_fail (UDISKS_IS_LINUX_MODULE_BTRFS (module), NULL);
  g_return_val_if_fail (UDISKS_IS_LINUX_BLOCK_OBJECT (block_object), NULL);
  return UDISKS_LINUX_FILESYSTEM_BTRFS (g_object_new (UDISKS_TYPE_LINUX_FILESYSTEM_BTRFS,
                                                      "module", UDISKS_MODULE (module),
                                                      "blockobject", block_object,
                                                      NULL));
}

/**
 * udisks_linux_filesystem_btrfs_get_module:
 * @fs: A #UDisksLinuxFilesystemBTRFS.
 *
 * Gets the module used by @l_fs_btrfs.
 *
 * Returns: A #UDisksLinuxModuleBTRFS. Do not free, the object is owned by @l_fs_btrfs.
 */
UDisksLinuxModuleBTRFS *
udisks_linux_filesystem_btrfs_get_module (UDisksLinuxFilesystemBTRFS *l_fs_btrfs)
{
  g_return_val_if_fail (UDISKS_IS_LINUX_FILESYSTEM_BTRFS (l_fs_btrfs), NULL);
  return l_fs_btrfs->module;
}

/**
 * udisks_linux_filesystem_btrfs_update:
 * @l_fs_btrfs: A #UDisksLinuxFilesystemBTRFS.
 * @object: The enclosing #UDisksLlinuxDriveObject instance.
 *
 * Updates the interface.
 *
 * Returns: %TRUE if the configuration has changed, %FALSE otherwise.
 */
gboolean
udisks_linux_filesystem_btrfs_update (UDisksLinuxFilesystemBTRFS *l_fs_btrfs,
                                      UDisksLinuxBlockObject     *object)
{
  UDisksFilesystemBTRFS *fs_btrfs = UDISKS_FILESYSTEM_BTRFS (l_fs_btrfs);
  BDBtrfsFilesystemInfo *btrfs_info = NULL;
  GError *error = NULL;
  gchar *dev_file = NULL;
  gboolean rval = FALSE;

  g_return_val_if_fail (UDISKS_IS_LINUX_FILESYSTEM_BTRFS (fs_btrfs), FALSE);
  g_return_val_if_fail (UDISKS_IS_LINUX_BLOCK_OBJECT (object), FALSE);

  dev_file = udisks_linux_block_object_get_device_file (object);
  if (! dev_file)
    {
      rval = FALSE;
      goto out;
    }

  btrfs_info = bd_btrfs_filesystem_info (dev_file, &error);

  if (! btrfs_info)
    {
      udisks_critical ("Can't get BTRFS filesystem info for %s", dev_file);
      rval = FALSE;
      goto out;
    }

  /* Update the interface */
  udisks_filesystem_btrfs_set_label (fs_btrfs, btrfs_info->label);
  udisks_filesystem_btrfs_set_uuid (fs_btrfs, btrfs_info->uuid);
  udisks_filesystem_btrfs_set_num_devices (fs_btrfs, btrfs_info->num_devices);
  udisks_filesystem_btrfs_set_used (fs_btrfs, btrfs_info->used);

out:
  g_dbus_interface_skeleton_flush (G_DBUS_INTERFACE_SKELETON (fs_btrfs));
  if (btrfs_info)
    bd_btrfs_filesystem_info_free (btrfs_info);
  if (error)
    g_clear_error (&error);
  g_free (dev_file);

  return rval;
}

/**
 * udisks_filesystem_btrfs_get_first_mount_point:
 *
 * @fs_btrfs: A #UDisksFilesystemBTRFS.
 *
 * Returns: the first mount_point for the given BTRFS volume. Free with g_free().
 */
static gchar *
udisks_filesystem_btrfs_get_first_mount_point (UDisksFilesystemBTRFS  *fs_btrfs,
                                               GError                **error)
{
  UDisksObject *object;
  UDisksFilesystem *fs;
  const gchar * const *mount_points;
  gchar *mount_point = NULL;

  g_return_val_if_fail (UDISKS_IS_FILESYSTEM_BTRFS (fs_btrfs), NULL);

  /* Get enclosing object for this interface. */
  object = udisks_daemon_util_dup_object (fs_btrfs, error);
  g_return_val_if_fail (object, NULL);

  /* Get UDisksFilesystem. */
  fs = udisks_object_peek_filesystem (object);
  if (fs != NULL)
    {
      mount_points = udisks_filesystem_get_mount_points (fs);
      if (mount_points != NULL && *mount_points != NULL)
        mount_point = g_strdup (*mount_points);
    }

  g_object_unref (object);

  if (mount_point == NULL)
    {
      g_set_error_literal (error, UDISKS_ERROR, UDISKS_ERROR_NOT_MOUNTED,
                           "Volume not mounted");
      return NULL;
    }
  return mount_point;
}

static gboolean
handle_set_label (UDisksFilesystemBTRFS *fs_btrfs,
                  GDBusMethodInvocation *invocation,
                  const gchar           *arg_label,
                  GVariant              *arg_options)
{
  UDisksLinuxFilesystemBTRFS *l_fs_btrfs = UDISKS_LINUX_FILESYSTEM_BTRFS (fs_btrfs);
  UDisksFilesystem *filesystem;
  UDisksLinuxBlockObject *object = NULL;
  UDisksDaemon *daemon;
  GError *error = NULL;
  gchar *dev_file = NULL;
  GList *siblings;
  GList *l;

  object = udisks_daemon_util_dup_object (l_fs_btrfs, &error);
  if (! object)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = udisks_module_get_daemon (UDISKS_MODULE (l_fs_btrfs->module));
  filesystem = udisks_object_peek_filesystem (UDISKS_OBJECT (object));

  /* Policy check. */
  UDISKS_DAEMON_CHECK_AUTHORIZATION (daemon,
                                     UDISKS_OBJECT (object),
                                     BTRFS_POLICY_ACTION_ID,
                                     arg_options,
                                     N_("Authentication is required to change label for BTRFS volume"),
                                     invocation);

  /* Get the device filename (eg.: /dev/sda1) */
  dev_file = udisks_linux_block_object_get_device_file (object);
  if (! dev_file)
    {
      g_dbus_method_invocation_return_error_literal (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                                     "Cannot find the device file");
      goto out;
    }

  /* Change the label. */
  if (! bd_btrfs_change_label (dev_file, arg_label, &error))
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  /* Trigger uevent on the current filesystem and all siblings */
  udisks_linux_block_object_trigger_uevent_sync (object, UDISKS_DEFAULT_WAIT_TIMEOUT);
  if (udisks_filesystem_btrfs_get_num_devices (fs_btrfs) > 1)
    {
      siblings = udisks_linux_filesystem_find_siblings (UDISKS_LINUX_FILESYSTEM (filesystem), object);
      for (l = siblings; l != NULL; l = l->next)
        {
          UDisksLinuxBlockObject *obj = UDISKS_LINUX_BLOCK_OBJECT (l->data);
          /* TODO: parallelize */
          udisks_linux_block_object_trigger_uevent_sync (obj, UDISKS_DEFAULT_WAIT_TIMEOUT);
        }
      g_list_free_full (siblings, g_object_unref);
    }

  /* Complete DBus call. */
  udisks_filesystem_btrfs_complete_set_label (fs_btrfs, invocation);
out:
  g_clear_object (&object);
  g_free (dev_file);
  return TRUE;
}

static gboolean
btrfs_subvolume_perform_action (UDisksFilesystemBTRFS *fs_btrfs,
                                GDBusMethodInvocation *invocation,
                                btrfs_subvolume_func   subvolume_action,
                                const gchar           *arg_name,
                                GVariant              *arg_options,
                                const gchar           *polkit_message)
{
  UDisksLinuxFilesystemBTRFS *l_fs_btrfs = UDISKS_LINUX_FILESYSTEM_BTRFS (fs_btrfs);
  UDisksFilesystem *filesystem;
  UDisksLinuxBlockObject *object = NULL;
  UDisksDaemon *daemon;
  GError *error = NULL;
  gchar *mount_point = NULL;
  GList *siblings;
  GList *l;

  object = udisks_daemon_util_dup_object (l_fs_btrfs, &error);
  if (! object)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = udisks_module_get_daemon (UDISKS_MODULE (l_fs_btrfs->module));
  filesystem = udisks_object_peek_filesystem (UDISKS_OBJECT (object));

  /* Policy check. */
  UDISKS_DAEMON_CHECK_AUTHORIZATION (daemon,
                                     UDISKS_OBJECT (object),
                                     BTRFS_POLICY_ACTION_ID,
                                     arg_options,
                                     N_(polkit_message),
                                     invocation);

  /* Do we have a valid subvolume name? */
  if (! *arg_name)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Invalid subvolume name");
      goto out;
    }

  /* Get the mount point for this volume. */
  mount_point = udisks_filesystem_btrfs_get_first_mount_point (fs_btrfs, &error);
  if (! mount_point)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  /* Add/remove the subvolume. */
  if (! subvolume_action (mount_point, arg_name, NULL, &error))
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  /* Trigger uevent on the current filesystem and all siblings */
  udisks_linux_block_object_trigger_uevent_sync (object, UDISKS_DEFAULT_WAIT_TIMEOUT);
  siblings = udisks_linux_filesystem_find_siblings (UDISKS_LINUX_FILESYSTEM (filesystem), object);
  for (l = siblings; l != NULL; l = l->next)
    {
      UDisksLinuxBlockObject *obj = UDISKS_LINUX_BLOCK_OBJECT (l->data);
      /* TODO: parallelize */
      udisks_linux_block_object_trigger_uevent_sync (obj, UDISKS_DEFAULT_WAIT_TIMEOUT);
    }
  g_list_free_full (siblings, g_object_unref);

  /* Complete DBus call. */
  g_dbus_method_invocation_return_value (invocation, g_variant_new ("()"));

out:
  /* Release the resources */
  g_clear_object (&object);
  g_free (mount_point);

  /* Indicate that we handled the method invocation */
  return TRUE;
}

static gboolean
btrfs_device_perform_action (UDisksFilesystemBTRFS *fs_btrfs,
                             GDBusMethodInvocation *invocation,
                             btrfs_device_func      device_action,
                             const gchar           *arg_device,
                             GVariant              *arg_options)
{
  UDisksLinuxFilesystemBTRFS *l_fs_btrfs = UDISKS_LINUX_FILESYSTEM_BTRFS (fs_btrfs);
  UDisksFilesystem *filesystem;
  UDisksLinuxBlockObject *object = NULL;
  GError *error = NULL;
  gchar *mount_point = NULL;
  const gchar *device;
  UDisksDaemon *daemon;
  UDisksObject *new_device_object = NULL;
  UDisksBlock *new_device_block = NULL;
  GList *siblings;
  GList *l;

  object = udisks_daemon_util_dup_object (fs_btrfs, &error);
  if (! object)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = udisks_module_get_daemon (UDISKS_MODULE (l_fs_btrfs->module));
  filesystem = udisks_object_peek_filesystem (UDISKS_OBJECT (object));

  /* Policy check. */
  UDISKS_DAEMON_CHECK_AUTHORIZATION (daemon,
                                     UDISKS_OBJECT (object),
                                     BTRFS_POLICY_ACTION_ID,
                                     arg_options,
                                     N_("Authentication is required to add the device to the volume"),
                                     invocation);

  /* Get the mount point for this volume. */
  mount_point = udisks_filesystem_btrfs_get_first_mount_point (fs_btrfs, &error);
  if (! mount_point)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  new_device_object = udisks_daemon_find_object (daemon, arg_device);
  if (new_device_object == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Invalid object path %s",
                                             arg_device);
      goto out;
    }

  new_device_block = udisks_object_get_block (new_device_object);
  if (new_device_block == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Object path %s is not a block device",
                                             arg_device);
      goto out;
    }

  device = udisks_block_get_device (new_device_block);

  /* Add/remove the device to/from the volume. */
  if (! device_action (mount_point, device, NULL, &error))
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  /* Trigger uevent on the current filesystem and all siblings */
  udisks_linux_block_object_trigger_uevent_sync (object, UDISKS_DEFAULT_WAIT_TIMEOUT);
  /* Avoid potential race condition with udev or UDisks not yet finished
   * probing the newly added/removed device.
   */
  udisks_daemon_util_trigger_uevent_sync (daemon, device, NULL, UDISKS_DEFAULT_WAIT_TIMEOUT);
  siblings = udisks_linux_filesystem_find_siblings (UDISKS_LINUX_FILESYSTEM (filesystem), object);
  for (l = siblings; l != NULL; l = l->next)
    {
      UDisksLinuxBlockObject *obj = UDISKS_LINUX_BLOCK_OBJECT (l->data);
      gchar *obj_device_file;

      /* TODO: parallelize */
      obj_device_file = udisks_linux_block_object_get_device_file (obj);
      if (g_strcmp0 (obj_device_file, device) != 0)
        udisks_linux_block_object_trigger_uevent_sync (obj, UDISKS_DEFAULT_WAIT_TIMEOUT);
      g_free (obj_device_file);
    }
  g_list_free_full (siblings, g_object_unref);

  /* Complete DBus call. */
  g_dbus_method_invocation_return_value (invocation, g_variant_new ("()"));

out:
  /* Release the resources */
  g_clear_object (&object);
  g_clear_object (&new_device_object);
  g_clear_object (&new_device_block);
  g_free (mount_point);

  /* Indicate that we handled the method invocation. */
  return TRUE;
}

static gboolean
handle_add_device (UDisksFilesystemBTRFS *fs_btrfs,
                   GDBusMethodInvocation *invocation,
                   const gchar           *arg_device,
                   GVariant              *arg_options)
{
  return btrfs_device_perform_action (fs_btrfs,
                                      invocation,
                                      bd_btrfs_add_device,
                                      arg_device,
                                      arg_options);
}

static gboolean
handle_remove_device (UDisksFilesystemBTRFS *fs_btrfs,
                      GDBusMethodInvocation *invocation,
                      const gchar           *arg_device,
                      GVariant              *arg_options)
{
  return btrfs_device_perform_action (fs_btrfs,
                                      invocation,
                                      bd_btrfs_remove_device,
                                      arg_device,
                                      arg_options);
}

static gboolean
handle_create_subvolume (UDisksFilesystemBTRFS *fs_btrfs,
                         GDBusMethodInvocation *invocation,
                         const gchar           *arg_name,
                         GVariant              *arg_options)
{
  return btrfs_subvolume_perform_action (fs_btrfs,
                                         invocation,
                                         bd_btrfs_create_subvolume,
                                         arg_name,
                                         arg_options,
                                         "Authentication is required to add a new subvolume for the given BTRFS volume");
}

static gboolean
handle_remove_subvolume (UDisksFilesystemBTRFS *fs_btrfs,
                         GDBusMethodInvocation *invocation,
                         const gchar           *arg_name,
                         GVariant              *arg_options)
{
  return btrfs_subvolume_perform_action (fs_btrfs,
                                         invocation,
                                         bd_btrfs_delete_subvolume,
                                         arg_name,
                                         arg_options,
                                         "Authentication is required to remove the subvolume for the given BTRFS volume");
}

static gboolean
handle_get_subvolumes (UDisksFilesystemBTRFS *fs_btrfs,
                       GDBusMethodInvocation *invocation,
                       gboolean               arg_snapshots_only,
                       GVariant              *arg_options)
{
  UDisksLinuxFilesystemBTRFS *l_fs_btrfs = UDISKS_LINUX_FILESYSTEM_BTRFS (fs_btrfs);
  UDisksLinuxBlockObject *object = NULL;
  UDisksDaemon *daemon;
  BDBtrfsSubvolumeInfo **subvolumes_info = NULL;
  GVariant *subvolumes = NULL;
  GError *error = NULL;
  gchar *mount_point = NULL;
  gint subvolumes_cnt = 0;

  object = udisks_daemon_util_dup_object (fs_btrfs, &error);
  if (! object)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = udisks_module_get_daemon (UDISKS_MODULE (l_fs_btrfs->module));

  /* Policy check. */
  UDISKS_DAEMON_CHECK_AUTHORIZATION (daemon,
                                     UDISKS_OBJECT (object),
                                     BTRFS_POLICY_ACTION_ID,
                                     arg_options,
                                     N_("Authentication is required to change label for BTRFS volume"),
                                     invocation);

  /* Get the mount point for this volume. */
  mount_point = udisks_filesystem_btrfs_get_first_mount_point (fs_btrfs, &error);
  if (! mount_point)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  /* Get subvolume infos. */
  subvolumes_info = bd_btrfs_list_subvolumes (mount_point,
                                              arg_snapshots_only,
                                              &error);

  if (! subvolumes_info && error)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  subvolumes = btrfs_subvolumes_to_gvariant (subvolumes_info, &subvolumes_cnt);

  /* Complete DBus call. */
  udisks_filesystem_btrfs_complete_get_subvolumes (fs_btrfs,
                                                   invocation,
                                                   subvolumes,
                                                   subvolumes_cnt);

out:
  /* Release the resources */
  g_clear_object (&object);
  btrfs_free_subvolumes_info (subvolumes_info);
  g_free (mount_point);

  /* Indicate that we handled the method invocation */
  return TRUE;
}

static gboolean
handle_create_snapshot (UDisksFilesystemBTRFS  *fs_btrfs,
                        GDBusMethodInvocation  *invocation,
                        const gchar            *arg_source,
                        const gchar            *arg_dest,
                        gboolean                arg_ro,
                        GVariant               *arg_options)
{
  UDisksLinuxFilesystemBTRFS *l_fs_btrfs = UDISKS_LINUX_FILESYSTEM_BTRFS (fs_btrfs);
  UDisksLinuxBlockObject *object = NULL;
  UDisksDaemon *daemon;
  GError *error = NULL;
  gchar *source = NULL;
  gchar *dest = NULL;
  gchar *mount_point = NULL;

  object = udisks_daemon_util_dup_object (fs_btrfs, &error);
  if (! object)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = udisks_module_get_daemon (UDISKS_MODULE (l_fs_btrfs->module));

  /* Policy check. */
  UDISKS_DAEMON_CHECK_AUTHORIZATION (daemon,
                                     UDISKS_OBJECT (object),
                                     BTRFS_POLICY_ACTION_ID,
                                     arg_options,
                                     N_("Authentication is required to create a new snapshot"),
                                     invocation);

  /* Prefix source and destination directories with mount point; so that user
   * doesn't need to always type in a full path.
   */
  mount_point = udisks_filesystem_btrfs_get_first_mount_point (fs_btrfs, &error);
  if (! mount_point)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }
  source = g_build_path (G_DIR_SEPARATOR_S, mount_point, arg_source, NULL);
  dest = g_build_path (G_DIR_SEPARATOR_S, mount_point, arg_dest, NULL);

  /* Create the snapshot. */
  if (! bd_btrfs_create_snapshot (source, dest, arg_ro, NULL, &error))
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  /* Complete DBus call. */
  udisks_filesystem_btrfs_complete_create_snapshot (fs_btrfs, invocation);

out:
  /* Release the resources */
  g_clear_object (&object);
  g_free (source);
  g_free (dest);
  g_free (mount_point);

  /* Indicate that we handled the method invocation */
  return TRUE;
}

static gboolean
handle_repair (UDisksFilesystemBTRFS *fs_btrfs,
               GDBusMethodInvocation *invocation,
               GVariant              *arg_options)
{
  UDisksLinuxFilesystemBTRFS *l_fs_btrfs = UDISKS_LINUX_FILESYSTEM_BTRFS (fs_btrfs);
  UDisksFilesystem *filesystem;
  UDisksLinuxBlockObject *object = NULL;
  UDisksDaemon *daemon;
  GError *error = NULL;
  gchar *dev_file = NULL;
  GList *siblings;
  GList *l;

  object = udisks_daemon_util_dup_object (l_fs_btrfs, &error);
  if (! object)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = udisks_module_get_daemon (UDISKS_MODULE (l_fs_btrfs->module));
  filesystem = udisks_object_peek_filesystem (UDISKS_OBJECT (object));

  /* Policy check. */
  UDISKS_DAEMON_CHECK_AUTHORIZATION (daemon,
                                     UDISKS_OBJECT (object),
                                     BTRFS_POLICY_ACTION_ID,
                                     arg_options,
                                     N_("Authentication is required to check and repair the volume"),
                                     invocation);

  /* Get the device filename (eg.: /dev/sda1) */
  dev_file = udisks_linux_block_object_get_device_file (object);
  if (! dev_file)
    {
      g_dbus_method_invocation_return_error_literal (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                                     "Cannot find the device file");
      goto out;
    }

  /* Check and repair. */
  if (! bd_btrfs_repair (dev_file, NULL, &error))
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  udisks_linux_block_object_trigger_uevent_sync (object, UDISKS_DEFAULT_WAIT_TIMEOUT);
  if (udisks_filesystem_btrfs_get_num_devices (fs_btrfs) > 1)
    {
      siblings = udisks_linux_filesystem_find_siblings (UDISKS_LINUX_FILESYSTEM (filesystem), object);
      for (l = siblings; l != NULL; l = l->next)
        {
          UDisksLinuxBlockObject *obj = UDISKS_LINUX_BLOCK_OBJECT (l->data);
          /* TODO: parallelize */
          udisks_linux_block_object_trigger_uevent_sync (obj, UDISKS_DEFAULT_WAIT_TIMEOUT);
        }
      g_list_free_full (siblings, g_object_unref);
    }

  /* Complete DBus call. */
  udisks_filesystem_btrfs_complete_repair (fs_btrfs, invocation);

out:
  g_clear_object (&object);
  g_free (dev_file);

  /* Indicate that we handled the method invocation */
  return TRUE;
}

static gboolean
handle_resize (UDisksFilesystemBTRFS *fs_btrfs,
               GDBusMethodInvocation *invocation,
               guint64                arg_size,
               GVariant              *arg_options)
{
  UDisksLinuxFilesystemBTRFS *l_fs_btrfs = UDISKS_LINUX_FILESYSTEM_BTRFS (fs_btrfs);
  UDisksFilesystem *filesystem;
  UDisksLinuxBlockObject *object = NULL;
  UDisksDaemon *daemon;
  GError *error = NULL;
  gchar *mount_point = NULL;
  GList *siblings;
  GList *l;

  object = udisks_daemon_util_dup_object (l_fs_btrfs, &error);
  if (! object)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = udisks_module_get_daemon (UDISKS_MODULE (l_fs_btrfs->module));
  filesystem = udisks_object_peek_filesystem (UDISKS_OBJECT (object));

  /* Policy check. */
  UDISKS_DAEMON_CHECK_AUTHORIZATION (daemon,
                                     UDISKS_OBJECT (object),
                                     BTRFS_POLICY_ACTION_ID,
                                     arg_options,
                                     N_("Authentication is required to resize the volume"),
                                     invocation);

  /* Get the mount point for this volume. */
  mount_point = udisks_filesystem_btrfs_get_first_mount_point (fs_btrfs, &error);
  if (! mount_point)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  /* Resize the volume. */
  if (! bd_btrfs_resize (mount_point, arg_size, NULL, &error))
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  udisks_linux_block_object_trigger_uevent_sync (object, UDISKS_DEFAULT_WAIT_TIMEOUT);
  if (udisks_filesystem_btrfs_get_num_devices (fs_btrfs) > 1)
    {
      siblings = udisks_linux_filesystem_find_siblings (UDISKS_LINUX_FILESYSTEM (filesystem), object);
      for (l = siblings; l != NULL; l = l->next)
        {
          UDisksLinuxBlockObject *obj = UDISKS_LINUX_BLOCK_OBJECT (l->data);
          /* TODO: parallelize */
          udisks_linux_block_object_trigger_uevent_sync (obj, UDISKS_DEFAULT_WAIT_TIMEOUT);
        }
      g_list_free_full (siblings, g_object_unref);
    }

  /* Complete DBus call. */
  udisks_filesystem_btrfs_complete_resize (fs_btrfs, invocation);

out:
  g_clear_object (&object);
  g_free (mount_point);

  /* Indicate that we handled the method invocation */
  return TRUE;
}

static void
udisks_linux_filesystem_btrfs_iface_init (UDisksFilesystemBTRFSIface *iface)
{
  iface->handle_set_label = handle_set_label;
  iface->handle_add_device = handle_add_device;
  iface->handle_remove_device = handle_remove_device;
  iface->handle_create_subvolume = handle_create_subvolume;
  iface->handle_remove_subvolume = handle_remove_subvolume;
  iface->handle_get_subvolumes = handle_get_subvolumes;
  iface->handle_create_snapshot = handle_create_snapshot;
  iface->handle_repair = handle_repair;
  iface->handle_resize = handle_resize;
}

/* -------------------------------------------------------------------------- */

static gboolean
udisks_linux_filesystem_btrfs_module_object_process_uevent (UDisksModuleObject *module_object,
                                                            const gchar        *action,
                                                            UDisksLinuxDevice  *device,
                                                            gboolean           *keep)
{
  UDisksLinuxFilesystemBTRFS *l_fs_btrfs = UDISKS_LINUX_FILESYSTEM_BTRFS (module_object);
  const gchar *fs_type = NULL;

  g_return_val_if_fail (UDISKS_IS_LINUX_FILESYSTEM_BTRFS (module_object), FALSE);

  if (device == NULL)
    return FALSE;

  /* Check filesystem type from udev property. */
  fs_type = g_udev_device_get_property (device->udev_device, "ID_FS_TYPE");
  *keep = g_strcmp0 (fs_type, "btrfs") == 0;
  if (*keep)
    {
      udisks_linux_filesystem_btrfs_update (l_fs_btrfs, l_fs_btrfs->block_object);
    }

  return TRUE;
}

static void
udisks_linux_filesystem_btrfs_module_object_iface_init (UDisksModuleObjectIface *iface)
{
  iface->process_uevent = udisks_linux_filesystem_btrfs_module_object_process_uevent;
}
