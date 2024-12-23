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
#include <sys/sysmacros.h>
#include <mntent.h>

#include <glib.h>
#include <glib-object.h>

#include "udiskslogging.h"
#include "udisksmountmonitor.h"
#include "udisksmount.h"
#include "udisksprivate.h"
#include "udisksdaemonutil.h"

/* build a %Ns format string macro with N == PATH_MAX */
#define xstr(s) str(s)
#define str(s) #s
#define PATH_MAX_FMT "%" xstr(PATH_MAX) "s"

/**
 * SECTION:udisksmountmonitor
 * @title: UDisksMountMonitor
 * @short_description: Monitors mounted filesystems or in-use swap devices
 *
 * This type is used for monitoring mounted devices and swap devices
 * in use. On Linux, this is done by inspecting and monitoring the
 * <literal>/proc/self/mountinfo</literal> and
 * <literal>/proc/swaps</literal> files.
 */

/**
 * UDisksMountMonitor:
 *
 * The #UDisksMountMonitor structure contains only private data and
 * should only be accessed using the provided API.
 */
struct _UDisksMountMonitor
{
  GObject parent_instance;

  GIOChannel *mounts_channel;
  GSource *mounts_watch_source;

  GIOChannel *swaps_channel;
  GSource *swaps_watch_source;

  GList *mounts;
  GList *old_mounts;
  GMutex mounts_mutex;

  gchar *mountinfo_checksum;
  gchar *swaps_checksum;

  GMainContext *monitor_context;
};

typedef struct _UDisksMountMonitorClass UDisksMountMonitorClass;

struct _UDisksMountMonitorClass
{
  GObjectClass parent_class;

  void (*mount_added)   (UDisksMountMonitor  *monitor,
                         UDisksMount         *mount);
  void (*mount_removed) (UDisksMountMonitor  *monitor,
                         UDisksMount         *mount);
};

/*--------------------------------------------------------------------------------------------------------------*/

enum
  {
    MOUNT_ADDED_SIGNAL,
    MOUNT_REMOVED_SIGNAL,
    LAST_SIGNAL,
  };

static guint signals[LAST_SIGNAL] = { 0 };

G_DEFINE_TYPE (UDisksMountMonitor, udisks_mount_monitor, G_TYPE_OBJECT)

static void udisks_mount_monitor_ensure (UDisksMountMonitor *monitor);
static void udisks_mount_monitor_constructed (GObject *object);

static void
udisks_mount_monitor_finalize (GObject *object)
{
  UDisksMountMonitor *monitor = UDISKS_MOUNT_MONITOR (object);

  if (monitor->mounts_channel != NULL)
    g_io_channel_unref (monitor->mounts_channel);
  if (monitor->mounts_watch_source != NULL)
    g_source_destroy (monitor->mounts_watch_source);

  if (monitor->swaps_channel != NULL)
    g_io_channel_unref (monitor->swaps_channel);
  if (monitor->swaps_watch_source != NULL)
    g_source_destroy (monitor->swaps_watch_source);

  if (monitor->monitor_context != NULL)
    g_main_context_unref (monitor->monitor_context);

  g_list_free_full (monitor->mounts, g_object_unref);
  g_list_free_full (monitor->old_mounts, g_object_unref);

  g_free (monitor->mountinfo_checksum);
  g_free (monitor->swaps_checksum);

  g_mutex_clear (&monitor->mounts_mutex);

  if (G_OBJECT_CLASS (udisks_mount_monitor_parent_class)->finalize != NULL)
    G_OBJECT_CLASS (udisks_mount_monitor_parent_class)->finalize (object);
}

static void
udisks_mount_monitor_init (UDisksMountMonitor *monitor)
{
  monitor->mounts = NULL;
  monitor->old_mounts = NULL;
  g_mutex_init (&monitor->mounts_mutex);
}

static void
udisks_mount_monitor_class_init (UDisksMountMonitorClass *klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;

  gobject_class->finalize    = udisks_mount_monitor_finalize;
  gobject_class->constructed = udisks_mount_monitor_constructed;

  /**
   * UDisksMountMonitor::mount-added
   * @monitor: A #UDisksMountMonitor.
   * @mount: The #UDisksMount that was added.
   *
   * Emitted when a mount is added.
   *
   * This signal is emitted in the
   * <link linkend="g-main-context-push-thread-default">thread-default main loop</link>
   * that @monitor was created in.
   */
  signals[MOUNT_ADDED_SIGNAL] = g_signal_new ("mount-added",
                                              G_OBJECT_CLASS_TYPE (klass),
                                              G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                                              G_STRUCT_OFFSET (UDisksMountMonitorClass, mount_added),
                                              NULL,
                                              NULL,
                                              g_cclosure_marshal_VOID__OBJECT,
                                              G_TYPE_NONE,
                                              1,
                                              UDISKS_TYPE_MOUNT);

  /**
   * UDisksMountMonitor::mount-removed
   * @monitor: A #UDisksMountMonitor.
   * @mount: The #UDisksMount that was removed.
   *
   * Emitted when a mount is removed.
   *
   * This signal is emitted in the
   * <link linkend="g-main-context-push-thread-default">thread-default main loop</link>
   * that @monitor was created in.
   */
  signals[MOUNT_REMOVED_SIGNAL] = g_signal_new ("mount-removed",
                                                G_OBJECT_CLASS_TYPE (klass),
                                                G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                                                G_STRUCT_OFFSET (UDisksMountMonitorClass, mount_removed),
                                                NULL,
                                                NULL,
                                                g_cclosure_marshal_VOID__OBJECT,
                                                G_TYPE_NONE,
                                                1,
                                                UDISKS_TYPE_MOUNT);
}

static void
diff_sorted_lists (GList *list1,
                   GList *list2,
                   GCompareFunc compare,
                   GList **added,
                   GList **removed)
{
  int order;

  *added = *removed = NULL;

  while (list1 != NULL && list2 != NULL)
    {
      order = (*compare) (list1->data, list2->data);
      if (order < 0)
        {
          *removed = g_list_prepend (*removed, list1->data);
          list1 = list1->next;
        }
      else if (order > 0)
        {
          *added = g_list_prepend (*added, list2->data);
          list2 = list2->next;
        }
      else
        { /* same item */
          list1 = list1->next;
          list2 = list2->next;
        }
    }

  while (list1 != NULL)
    {
      *removed = g_list_prepend (*removed, list1->data);
      list1 = list1->next;
    }
  while (list2 != NULL)
    {
      *added = g_list_prepend (*added, list2->data);
      list2 = list2->next;
    }
}

static void
reload_mounts (UDisksMountMonitor *monitor)
{
  GList *cur_mounts;
  GList *added;
  GList *removed;
  GList *l;
  GList *old_mounts;

  udisks_mount_monitor_ensure (monitor);

  g_mutex_lock (&monitor->mounts_mutex);
  cur_mounts = g_list_copy_deep (monitor->mounts, (GCopyFunc) udisks_g_object_ref_copy, NULL);
  cur_mounts = g_list_sort (cur_mounts, (GCompareFunc) udisks_mount_compare);
  old_mounts = monitor->old_mounts;
  monitor->old_mounts = cur_mounts;
  g_mutex_unlock (&monitor->mounts_mutex);

  diff_sorted_lists (old_mounts, cur_mounts, (GCompareFunc) udisks_mount_compare, &added, &removed);

  for (l = removed; l != NULL; l = l->next)
    {
      UDisksMount *mount = UDISKS_MOUNT (l->data);
      g_signal_emit (monitor, signals[MOUNT_REMOVED_SIGNAL], 0, mount);
    }

  for (l = added; l != NULL; l = l->next)
    {
      UDisksMount *mount = UDISKS_MOUNT (l->data);
      g_signal_emit (monitor, signals[MOUNT_ADDED_SIGNAL], 0, mount);
    }

  g_list_free_full (old_mounts, g_object_unref);
  g_list_free (removed);
  g_list_free (added);
}

static gboolean
mounts_changed_event (GIOChannel *channel,
                      GIOCondition cond,
                      gpointer user_data)
{
  UDisksMountMonitor *monitor = UDISKS_MOUNT_MONITOR (user_data);
  if (cond & ~G_IO_ERR)
    goto out;
  reload_mounts (monitor);
 out:
  return TRUE;
}

static gboolean
swaps_changed_event (GIOChannel *channel,
                     GIOCondition cond,
                     gpointer user_data)
{
  UDisksMountMonitor *monitor = UDISKS_MOUNT_MONITOR (user_data);
  if (cond & ~G_IO_ERR)
    goto out;
  reload_mounts (monitor);
 out:
  return TRUE;
}

static gboolean
mounts_changed_idle_cb (gpointer user_data)
{
  UDisksMountMonitor *monitor = UDISKS_MOUNT_MONITOR (user_data);

  reload_mounts (monitor);

  /* remove the source */
  return FALSE;
}

static void
udisks_mount_monitor_constructed (GObject *object)
{
  UDisksMountMonitor *monitor = UDISKS_MOUNT_MONITOR (object);
  GError *error;

  monitor->monitor_context = g_main_context_ref_thread_default ();

  /* fetch initial data */
  udisks_mount_monitor_ensure (monitor);

  error = NULL;
  monitor->mounts_channel = g_io_channel_new_file ("/proc/self/mountinfo", "r", &error);
  if (monitor->mounts_channel != NULL)
    {
      monitor->mounts_watch_source = g_io_create_watch (monitor->mounts_channel, G_IO_ERR);
#if __GNUC__ >= 8
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
/* parameters of the callback depend on the source and can be different
 * from the required "generic" GSourceFunc, see:
 * https://developer.gnome.org/glib/stable/glib-The-Main-Event-Loop.html#g-source-set-callback
 */
      g_source_set_callback (monitor->mounts_watch_source, (GSourceFunc) mounts_changed_event, monitor, NULL);
#if __GNUC__ >= 8
#pragma GCC diagnostic pop
#endif
      g_source_attach (monitor->mounts_watch_source, monitor->monitor_context);
      g_source_unref (monitor->mounts_watch_source);
    }
  else
    {
      g_error ("No /proc/self/mountinfo file: %s", error->message);
      g_clear_error (&error);
    }

  error = NULL;
  monitor->swaps_channel = g_io_channel_new_file ("/proc/swaps", "r", &error);
  if (monitor->swaps_channel != NULL)
    {
      monitor->swaps_watch_source = g_io_create_watch (monitor->swaps_channel, G_IO_ERR);
#if __GNUC__ >= 8
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
/* parameters of the callback depend on the source and can be different
 * from the required "generic" GSourceFunc, see:
 * https://developer.gnome.org/glib/stable/glib-The-Main-Event-Loop.html#g-source-set-callback
 */
      g_source_set_callback (monitor->swaps_watch_source, (GSourceFunc) swaps_changed_event, monitor, NULL);
#if __GNUC__ >= 8
#pragma GCC diagnostic pop
#endif
      g_source_attach (monitor->swaps_watch_source, monitor->monitor_context);
      g_source_unref (monitor->swaps_watch_source);
    }
  else
    {
      if (!(error->domain == G_FILE_ERROR && error->code == G_FILE_ERROR_NOENT))
        {
          udisks_warning ("Error opening /proc/swaps file: %s (%s, %d)",
                          error->message, g_quark_to_string (error->domain), error->code);
        }
      g_clear_error (&error);
    }

  if (G_OBJECT_CLASS (udisks_mount_monitor_parent_class)->constructed != NULL)
    (*G_OBJECT_CLASS (udisks_mount_monitor_parent_class)->constructed) (object);
}

/**
 * udisks_mount_monitor_new:
 *
 * Creates a new #UDisksMountMonitor object.
 *
 * Signals are emitted in the <link
 * linkend="g-main-context-push-thread-default">thread-default main
 * loop</link> that this function is called from.
 *
 * Returns: A #UDisksMountMonitor. Free with g_object_unref().
 */
UDisksMountMonitor *
udisks_mount_monitor_new (void)
{
  return UDISKS_MOUNT_MONITOR (g_object_new (UDISKS_TYPE_MOUNT_MONITOR, NULL));
}

static gboolean
have_mount (UDisksMountMonitor *monitor,
            dev_t               dev,
            const gchar        *mount_point)
{
  GList *l;
  gboolean ret;

  ret = FALSE;

  for (l = monitor->mounts; l != NULL; l = l->next)
    {
      UDisksMount *mount = UDISKS_MOUNT (l->data);

      if (udisks_mount_get_dev (mount) != dev)
        continue;

      if (mount_point != NULL)
        {
          if (udisks_mount_get_mount_type (mount) != UDISKS_MOUNT_TYPE_FILESYSTEM)
            continue;

          if (g_strcmp0 (udisks_mount_get_mount_path (mount), mount_point) != 0)
            continue;
        }

      ret = TRUE;
      break;
    }

  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
udisks_mount_monitor_read_mountinfo (gchar  **contents,
                                     gsize   *length)
{
  GError *error = NULL;

  if (!g_file_get_contents ("/proc/self/mountinfo", contents, length, &error))
    {
      udisks_warning ("Error reading /proc/self/mountinfo: %s (%s, %d)",
                      error->message, g_quark_to_string (error->domain), error->code);
      g_clear_error (&error);
      return FALSE;
    }

  return TRUE;
}

static void
udisks_mount_monitor_parse_mountinfo (UDisksMountMonitor  *monitor,
                                      const gchar         *contents)
{
  gchar **lines;
  guint n;

  /* See Documentation/filesystems/proc.txt for the format of /proc/self/mountinfo
   *
   * Note that things like space are encoded as \020.
   */
  if (contents == NULL)
    return;

  lines = g_strsplit (contents, "\n", 0);
  for (n = 0; lines[n] != NULL; n++)
    {
      guint mount_id;
      guint parent_id;
      guint major, minor;
      gchar encoded_root[PATH_MAX + 1];
      gchar encoded_mount_point[PATH_MAX + 1];
      gchar *mount_point;
      dev_t dev;

      if (strlen (lines[n]) == 0)
        continue;

      if (sscanf (lines[n],
                  "%u %u %u:%u " PATH_MAX_FMT " " PATH_MAX_FMT,
                  &mount_id,
                  &parent_id,
                  &major,
                  &minor,
                  encoded_root,
                  encoded_mount_point) != 6)
        {
          udisks_warning ("Error parsing line '%s'", lines[n]);
          continue;
        }
      encoded_root[sizeof encoded_root - 1] = '\0';
      encoded_mount_point[sizeof encoded_mount_point - 1] = '\0';

      /* Temporary work-around for btrfs, see
       *
       *  https://bugzilla.redhat.com/show_bug.cgi?id=495152#c31
       *  http://article.gmane.org/gmane.comp.file-systems.btrfs/2851
       *
       * for details.
       */
      if (major == 0)
        {
          const gchar *sep;
          sep = strstr (lines[n], " - ");
          if (sep != NULL)
            {
              gchar fstype[PATH_MAX + 1];
              gchar mount_source[PATH_MAX + 1];
              struct stat statbuf;

              if (sscanf (sep + 3, PATH_MAX_FMT " " PATH_MAX_FMT, fstype, mount_source) != 2)
                {
                  udisks_warning ("Error parsing things past - for '%s'", lines[n]);
                  continue;
                }
              fstype[sizeof fstype - 1] = '\0';
              mount_source[sizeof mount_source - 1] = '\0';

              if (g_strcmp0 (fstype, "btrfs") != 0)
                continue;

              if (!g_str_has_prefix (mount_source, "/dev/"))
                continue;

              if (stat (mount_source, &statbuf) != 0)
                {
                  udisks_warning ("Error statting %s: %m", mount_source);
                  continue;
                }

              if (!S_ISBLK (statbuf.st_mode))
                {
                  udisks_warning ("%s is not a block device", mount_source);
                  continue;
                }

              dev = statbuf.st_rdev;
            }
          else
            {
              continue;
            }
        }
      else
        {
          dev = makedev (major, minor);
        }

      mount_point = g_strcompress (encoded_mount_point);

      /* TODO: we can probably use a hash table or something if this turns out to be slow */
      if (!have_mount (monitor, dev, mount_point))
        {
          UDisksMount *mount;
          mount = _udisks_mount_new (dev, mount_point, UDISKS_MOUNT_TYPE_FILESYSTEM);
          monitor->mounts = g_list_prepend (monitor->mounts, mount);
        }

      g_free (mount_point);
    }
  g_strfreev (lines);
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
udisks_mount_monitor_read_swaps (gchar  **contents,
                                 gsize   *length)
{
  GError *error = NULL;

  if (!g_file_get_contents ("/proc/swaps", contents, length, &error))
    {
      if (g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        {
          g_clear_error (&error);
          return TRUE;
        }
      else
        {
          udisks_warning ("Error reading /proc/swaps: %s (%s, %d)",
                          error->message, g_quark_to_string (error->domain), error->code);
          g_clear_error (&error);
          return FALSE;
        }
    }

  return TRUE;
}

static void
udisks_mount_monitor_parse_swaps (UDisksMountMonitor  *monitor,
                                  const gchar         *contents)
{
  gchar **lines;
  guint n;

  if (contents == NULL)
    return;

  lines = g_strsplit (contents, "\n", 0);
  for (n = 0; lines[n] != NULL; n++)
    {
      gchar filename[PATH_MAX + 1];
      struct stat statbuf;
      dev_t dev;

      /* skip first line of explanatory text */
      if (n == 0)
        continue;

      if (strlen (lines[n]) == 0)
        continue;

      if (sscanf (lines[n], PATH_MAX_FMT, filename) != 1)
        {
          udisks_warning ("Error parsing line '%s'", lines[n]);
          continue;
        }
      filename[sizeof filename - 1] = '\0';

      if (stat (filename, &statbuf) != 0)
        {
          udisks_warning ("Error statting %s: %m", filename);
          continue;
        }

      dev = statbuf.st_rdev;

      if (!have_mount (monitor, dev, NULL))
        {
          UDisksMount *mount;
          mount = _udisks_mount_new (dev, NULL, UDISKS_MOUNT_TYPE_SWAP);
          monitor->mounts = g_list_prepend (monitor->mounts, mount);
        }
    }
  g_strfreev (lines);
}

/* ---------------------------------------------------------------------------------------------------- */

static void
udisks_mount_monitor_ensure (UDisksMountMonitor *monitor)
{
  gchar *mountinfo_contents = NULL;
  gchar *swaps_contents = NULL;
  gsize mountinfo_length = 0;
  gsize swaps_length = 0;
  gchar *mountinfo_checksum = NULL;
  gchar *swaps_checksum = NULL;
  GSource *idle_source;
  gboolean have_mountinfo;
  gboolean have_swaps;

  g_mutex_lock (&monitor->mounts_mutex);

  have_mountinfo = udisks_mount_monitor_read_mountinfo (&mountinfo_contents, &mountinfo_length);
  have_swaps = udisks_mount_monitor_read_swaps (&swaps_contents, &swaps_length);
  if (have_mountinfo || have_swaps)
    {
      /* compute contents checksums and compare them against current cache */
      if (mountinfo_contents)
        mountinfo_checksum = g_compute_checksum_for_data (G_CHECKSUM_SHA1,
                                                          (const guchar *) mountinfo_contents,
                                                          mountinfo_length);
      if (swaps_contents)
        swaps_checksum = g_compute_checksum_for_data (G_CHECKSUM_SHA1,
                                                      (const guchar *) swaps_contents,
                                                      swaps_length);
      if (g_strcmp0 (mountinfo_checksum, monitor->mountinfo_checksum) != 0 ||
          g_strcmp0 (swaps_checksum, monitor->swaps_checksum) != 0)
        {
          g_list_free_full (monitor->mounts, g_object_unref);
          monitor->mounts = NULL;

          udisks_mount_monitor_parse_mountinfo (monitor, mountinfo_contents);
          udisks_mount_monitor_parse_swaps (monitor, swaps_contents);

          /* save current checksums */
          g_free (monitor->mountinfo_checksum);
          g_free (monitor->swaps_checksum);
          monitor->mountinfo_checksum = g_strdup (mountinfo_checksum);
          monitor->swaps_checksum = g_strdup (swaps_checksum);

          /* notify about the changes */
          idle_source = g_idle_source_new ();
          g_source_set_priority (idle_source, G_PRIORITY_DEFAULT_IDLE);
          g_source_set_callback (idle_source, (GSourceFunc) mounts_changed_idle_cb, monitor, NULL);
          g_source_attach (idle_source, monitor->monitor_context);
          g_source_unref (idle_source);
        }

        g_free (mountinfo_checksum);
        g_free (swaps_checksum);
    }
  g_free (mountinfo_contents);
  g_free (swaps_contents);

  g_mutex_unlock (&monitor->mounts_mutex);
}

/**
 * udisks_mount_monitor_get_mounts:
 * @monitor: A #UDisksMountMonitor.
 *
 * Gets all #UDisksMount objects.
 *
 * Returns: A #GList of #UDisksMount objects. The returned list must
 * be freed with g_list_free() after each element has been freed with
 * g_object_unref().
 */
GList *
udisks_mount_monitor_get_mounts (UDisksMountMonitor *monitor)
{
  GList *ret;

  ret = NULL;

  g_mutex_lock (&monitor->mounts_mutex);
  ret = g_list_copy_deep (monitor->mounts, (GCopyFunc) udisks_g_object_ref_copy, NULL);
  g_mutex_unlock (&monitor->mounts_mutex);

  /* Sort the list to ensure that shortest mount paths appear first */
  ret = g_list_sort (ret, (GCompareFunc) udisks_mount_compare);
  
  return ret;
}

/**
 * udisks_mount_monitor_get_mounts_for_dev:
 * @monitor: A #UDisksMountMonitor.
 * @dev: A #dev_t device number.
 *
 * Gets all #UDisksMount objects for @dev.
 *
 * Returns: A #GList of #UDisksMount objects. The returned list must
 * be freed with g_list_free() after each element has been freed with
 * g_object_unref().
 */
GList *
udisks_mount_monitor_get_mounts_for_dev (UDisksMountMonitor *monitor,
                                         dev_t               dev)
{
  GList *ret;
  GList *l;

  ret = NULL;

  udisks_mount_monitor_ensure (monitor);

  g_mutex_lock (&monitor->mounts_mutex);

  for (l = monitor->mounts; l != NULL; l = l->next)
    {
      UDisksMount *mount = UDISKS_MOUNT (l->data);

      if (udisks_mount_get_dev (mount) == dev)
        {
          ret = g_list_prepend (ret, g_object_ref (mount));
        }
    }

  g_mutex_unlock (&monitor->mounts_mutex);

  /* Sort the list to ensure that shortest mount paths appear first */
  ret = g_list_sort (ret, (GCompareFunc) udisks_mount_compare);

  return ret;
}

/**
 * udisks_mount_monitor_is_dev_in_use:
 * @monitor: A #UDisksMountMonitor.
 * @dev: A #dev_t device number.
 * @out_type: (out allow-none): Return location for mount type, if in use or %NULL.
 *
 * Checks if @dev is in use (e.g. mounted or swap-area in-use).
 *
 * Returns: %TRUE if in use, %FALSE otherwise.
 */
gboolean
udisks_mount_monitor_is_dev_in_use (UDisksMountMonitor  *monitor,
                                    dev_t                dev,
                                    UDisksMountType     *out_type)
{
  gboolean ret;
  GList *l;

  ret = FALSE;
  udisks_mount_monitor_ensure (monitor);

  g_mutex_lock (&monitor->mounts_mutex);

  for (l = monitor->mounts; l != NULL; l = l->next)
    {
      UDisksMount *mount = UDISKS_MOUNT (l->data);

      if (udisks_mount_get_dev (mount) == dev)
        {
          if (out_type != NULL)
            *out_type = udisks_mount_get_mount_type (mount);
          ret = TRUE;
          goto out;
        }
    }

 out:
  g_mutex_unlock (&monitor->mounts_mutex);
  return ret;
}

/**
 * udisks_mount_monitor_get_mount_for_path:
 * @monitor: A #UDisksMountMonitor.
 * @mount_path: A filesystem path on which a device may be mounted.
 *
 * Gets the #UDisksMount mounted at @mount_path, if any.
 *
 * Returns: (transfer full) (nullable): the #UDisksMount (of type
 *  #UDISKS_MOUNT_TYPE_FILESYSTEM) mounted at @mount_path, or %NULL if nothing
 *  is mounted there.
 */
UDisksMount *
udisks_mount_monitor_get_mount_for_path (UDisksMountMonitor  *monitor,
                                         const gchar         *mount_path)
{
  GList *l;

  g_return_val_if_fail (UDISKS_IS_MOUNT_MONITOR (monitor), NULL);
  g_return_val_if_fail (mount_path != NULL, NULL);

  udisks_mount_monitor_ensure (monitor);

  g_mutex_lock (&monitor->mounts_mutex);

  for (l = monitor->mounts; l != NULL; l = l->next)
    {
      UDisksMount *mount = UDISKS_MOUNT (l->data);

      if (udisks_mount_get_mount_type (mount) == UDISKS_MOUNT_TYPE_FILESYSTEM &&
          g_strcmp0 (udisks_mount_get_mount_path (mount), mount_path) == 0)
        {
          g_object_ref (mount);
          g_mutex_unlock (&monitor->mounts_mutex);
          return mount;
        }
    }

  g_mutex_unlock (&monitor->mounts_mutex);
  return NULL;
}
