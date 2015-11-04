/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Dominika Hodovska <dhodovsk@redhat.com>
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

#include <blockdev/kbd.h>
#include <glib/gi18n.h>

#include <src/storageddaemon.h>
#include <src/storageddaemonutil.h>
#include <src/storagedlogging.h>
#include <src/storagedlinuxblockobject.h>

#include "storagedlinuxblockbcache.h"
#include "storagedbcacheutil.h"
#include "storaged-bcache-generated.h"

/**
 * SECTION:storagedlinuxblockbcache
 * @title: StoragedLinuxBlockBcache
 * @short_description: Object representing bCache device.
 *
 * Object corresponding to the Bcache device.
 */

/**
 * StoragedLinuxBlockBcache:
 *
 * The #StoragedLinuxBlockBcache structure contains only private data
 * and should only be accessed using the provided API.
 */

struct _StoragedLinuxBlockBcache {
  StoragedBlockBcacheSkeleton parent_instance;
};

struct _StoragedLinuxBlockBcacheClass {
  StoragedBlockBcacheSkeletonClass parent_class;
};

static void storaged_linux_block_bcache_iface_init (StoragedBlockBcacheIface *iface);

G_DEFINE_TYPE_WITH_CODE (StoragedLinuxBlockBcache, storaged_linux_block_bcache,
                         STORAGED_TYPE_BLOCK_BCACHE_SKELETON,
                         G_IMPLEMENT_INTERFACE (STORAGED_TYPE_BLOCK_BCACHE,
                                                storaged_linux_block_bcache_iface_init));

static void
storaged_linux_block_bcache_get_property (GObject     *object,
                                          guint        property_id,
                                          GValue      *value,
                                          GParamSpec  *pspec)
{
  switch (property_id)
    {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
storaged_linux_block_bcache_set_property (GObject       *object,
                                          guint          property_id,
                                          const GValue  *value,
                                          GParamSpec    *pspec)
{
  switch (property_id)
    {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
    }
}

static void
storaged_linux_block_bcache_dispose (GObject *object)
{
  if (G_OBJECT_CLASS (storaged_linux_block_bcache_parent_class))
    G_OBJECT_CLASS (storaged_linux_block_bcache_parent_class)->dispose (object);
}

static void
storaged_linux_block_bcache_finalize (GObject *object)
{
  if (G_OBJECT_CLASS (storaged_linux_block_bcache_parent_class))
    G_OBJECT_CLASS (storaged_linux_block_bcache_parent_class)->finalize (object);
}

static void
storaged_linux_block_bcache_class_init (StoragedLinuxBlockBcacheClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->get_property = storaged_linux_block_bcache_get_property;
  gobject_class->set_property = storaged_linux_block_bcache_set_property;
  gobject_class->dispose = storaged_linux_block_bcache_dispose;
  gobject_class->finalize = storaged_linux_block_bcache_finalize;
}

static void
storaged_linux_block_bcache_init (StoragedLinuxBlockBcache *self)
{
    g_dbus_interface_skeleton_set_flags (G_DBUS_INTERFACE_SKELETON (self),
                                         G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);

}

/**
 * storaged_linux_block_bcache_new:
 *
 * Creates a new #StoragedLinuxBlockBcache instance.
 *
 * Returns: A new #StoragedLinuxBlockBcache. Free with g_object_unref().
 */

StoragedLinuxBlockBcache *
storaged_linux_block_bcache_new (void)
{
  return g_object_new (STORAGED_TYPE_LINUX_BLOCK_BCACHE, NULL);
}

/**
 * storaged_linux_block_bcache_get_daemon:
 * @block: A #StoragedLinuxBlockBcache.
 *
 * Gets the daemon used by @block.
 *
 * Returns: A #StoragedDaemon. Do not free, the object is owned by @block.
 */

StoragedDaemon *
storaged_linux_block_bcache_get_daemon (StoragedLinuxBlockBcache *block)
{
  GError *error = NULL;
  StoragedLinuxBlockObject *object;
  StoragedDaemon *daemon = NULL;

  g_return_val_if_fail (STORAGED_IS_LINUX_BLOCK_BCACHE (block), NULL);

  object = storaged_daemon_util_dup_object (block, &error);
  if (object)
    {
      daemon = storaged_linux_block_object_get_daemon (object);
      g_clear_object (&object);
    }
  else
    {
      storaged_error ("%s", error->message);
      g_error_free (error);
    }

  return daemon;
}

/**
 * storaged_linux_block_bcache_update:
 * @block: A #StoragedLinuxBlockBcache
 * @object: The enclosing #StoragedLinuxBlockBcache instance.
 *
 * Updates the interface.
 *
 * Returns: %TRUE if configuration has changed, %FALSE otherwise.
 */

gboolean
storaged_linux_block_bcache_update (StoragedLinuxBlockBcache  *block,
                                    StoragedLinuxBlockObject  *object)
{
  StoragedBlockBcache *iface = STORAGED_BLOCK_BCACHE (block);
  GError *error = NULL;
  gchar *dev_file = NULL;
  gboolean rval = FALSE;
  BDKBDBcacheStats *stats;
  const gchar* mode = NULL;

  g_return_val_if_fail (STORAGED_IS_LINUX_BLOCK_BCACHE (block), FALSE);
  g_return_val_if_fail (STORAGED_IS_LINUX_BLOCK_OBJECT (object), FALSE);

  dev_file = storaged_linux_block_object_get_device_file (object);

  stats = bd_kbd_bcache_status (dev_file, &error);
  mode = bd_kbd_bcache_get_mode_str(bd_kbd_bcache_get_mode(dev_file, &error), &error);
  if (! stats || ! mode)
    {
      storaged_error ("Can't get Bcache block device info for %s", dev_file);
      rval = FALSE;
      goto out;
    }

  storaged_block_bcache_set_mode (iface, mode);
  storaged_block_bcache_set_state (iface, stats->state);
  storaged_block_bcache_set_block_size (iface, stats->block_size);
  storaged_block_bcache_set_cache_size (iface, stats->cache_size);
  storaged_block_bcache_set_cache_used (iface, stats->cache_used);
  storaged_block_bcache_set_hits (iface, stats->hits);
  storaged_block_bcache_set_misses (iface, stats->misses);
  storaged_block_bcache_set_bypass_hits (iface, stats->bypass_hits);
  storaged_block_bcache_set_bypass_misses (iface, stats->bypass_misses);
out:
  if (stats)
    bd_kbd_bcache_stats_free (stats);
  if (error)
    g_error_free (error);
  g_free (dev_file);

  return rval;
}

static gboolean
handle_bcache_destroy (StoragedBlockBcache    *block_,
                       GDBusMethodInvocation  *invocation,
                       GVariant               *options)
{
  GError *error = NULL;
  StoragedLinuxBlockBcache *block = STORAGED_LINUX_BLOCK_BCACHE (block_);
  StoragedLinuxBlockObject *object = NULL;
  gchar *devname = NULL;

  object = storaged_daemon_util_dup_object (block, &error);
  if (! object)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  /* Policy check */
  STORAGED_DAEMON_CHECK_AUTHORIZATION (storaged_linux_block_bcache_get_daemon (block),
                                       NULL,
                                       bcache_policy_action_id,
                                       options,
                                       N_("Authentication is required to destroy bcache device."),
                                       invocation);

  devname = storaged_linux_block_object_get_device_file (object);

  if (! bd_kbd_bcache_destroy (devname, &error))
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

    storaged_block_bcache_complete_bcache_destroy (block_, invocation);
out:
  g_free (devname);
  g_clear_object (&object);
  return TRUE;
}

static gboolean
handle_set_mode (StoragedBlockBcache    *block_,
                 GDBusMethodInvocation  *invocation,
                 const gchar            *arg_mode,
                 GVariant               *options)
{
  GError *error = NULL;
  StoragedLinuxBlockBcache *block = STORAGED_LINUX_BLOCK_BCACHE (block_);
  StoragedLinuxBlockObject *object = NULL;
  gchar *devname = NULL;
  BDKBDBcacheMode mode;
  gchar *modestr;

  object = storaged_daemon_util_dup_object (block, &error);
  if (! object)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  /* Policy check */
  STORAGED_DAEMON_CHECK_AUTHORIZATION (storaged_linux_block_bcache_get_daemon (block),
                                       NULL,
                                       bcache_policy_action_id,
                                       options,
                                       N_("Authentication is required to set mode of bcache device."),
                                       invocation);

  devname = storaged_linux_block_object_get_device_file (object);

  modestr = g_strdup (arg_mode);
  mode = bd_kbd_bcache_get_mode_from_str (modestr, &error);
  
  if (error != NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  if (! bd_kbd_bcache_set_mode (devname, mode, &error))
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }
    storaged_block_bcache_complete_set_mode (block_, invocation);
out:
  g_free (devname);
  g_free (modestr);
  g_clear_object (&object);
  return TRUE;
}

static void
storaged_linux_block_bcache_iface_init (StoragedBlockBcacheIface *iface)
{
  iface->handle_bcache_destroy = handle_bcache_destroy;
  iface->handle_set_mode = handle_set_mode;
}
