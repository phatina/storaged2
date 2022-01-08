/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2007-2010 David Zeuthen <zeuthen@gmail.com>
 * Copyright (C)      2016 Peter Hatina <phatina@redhat.com>
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
#include <uuid.h>
#include <blockdev/blockdev.h>

#include "udiskslogging.h"
#include "udisksdaemon.h"
#include "udisksdaemonutil.h"
#include "udisksprovider.h"
#include "udiskslinuxprovider.h"
#include "udisksmountmonitor.h"
#include "udisksmount.h"
#include "udisksbasejob.h"
#include "udisksspawnedjob.h"
#include "udisksthreadedjob.h"
#include "udiskssimplejob.h"
#include "udisksstate.h"
#include "udiskscrypttabmonitor.h"
#include "udiskscrypttabentry.h"
#include "udiskslinuxblockobject.h"
#include "udiskslinuxdevice.h"
#include "udisksmodulemanager.h"
#include "udisksmodule.h"
#include "udisksconfigmanager.h"
#include "udiskslinuxmountoptions.h"

#ifdef HAVE_LIBMOUNT_UTAB
#include "udisksutabmonitor.h"
#endif

/**
 * SECTION:udisksdaemon
 * @title: UDisksDaemon
 * @short_description: Main daemon object
 *
 * Object holding all global state.
 */

typedef struct _UDisksDaemonClass   UDisksDaemonClass;

/**
 * UDisksDaemon:
 *
 * The #UDisksDaemon structure contains only private data and should
 * only be accessed using the provided API.
 */
struct _UDisksDaemon
{
  GObject parent_instance;
  GDBusConnection *connection;
  GDBusObjectManagerServer *object_manager;

  UDisksMountMonitor *mount_monitor;

  UDisksLinuxProvider *linux_provider;

  /* may be NULL if polkit is masked */
  PolkitAuthority *authority;

  UDisksState *state;

  UDisksCrypttabMonitor *crypttab_monitor;

#ifdef HAVE_LIBMOUNT_UTAB
  UDisksUtabMonitor *utab_monitor;
#endif

  UDisksModuleManager *module_manager;

  UDisksConfigManager *config_manager;

  gboolean disable_modules;
  gboolean force_load_modules;
  gboolean uninstalled;
  gboolean enable_tcrypt;
  gchar *uuid;
};

struct _UDisksDaemonClass
{
  GObjectClass parent_class;
};

enum
{
  PROP_0,
  PROP_CONNECTION,
  PROP_OBJECT_MANAGER,
  PROP_MOUNT_MONITOR,
  PROP_CRYPTTAB_MONITOR,
  PROP_MODULE_MANAGER,
  PROP_CONFIG_MANAGER,
  PROP_DISABLE_MODULES,
  PROP_FORCE_LOAD_MODULES,
  PROP_UNINSTALLED,
  PROP_ENABLE_TCRYPT,
  PROP_UUID,
};

G_DEFINE_TYPE (UDisksDaemon, udisks_daemon, G_TYPE_OBJECT);

static void
udisks_daemon_finalize (GObject *object)
{
  UDisksDaemon *daemon = UDISKS_DAEMON (object);

  udisks_state_stop_cleanup (daemon->state);

  /* Modules use the monitors and try to reference them when cleaning up */
  udisks_module_manager_unload_modules (daemon->module_manager);

  g_clear_object (&daemon->authority);
  g_object_unref (daemon->object_manager);
  g_object_unref (daemon->linux_provider);
  g_object_unref (daemon->connection);
  g_object_unref (daemon->mount_monitor);
  g_object_unref (daemon->crypttab_monitor);
#ifdef HAVE_LIBMOUNT_UTAB
  g_object_unref (daemon->utab_monitor);
#endif
  g_clear_object (&daemon->module_manager);

  g_object_unref (daemon->state);
  g_free (daemon->uuid);

  g_clear_object (&daemon->config_manager);

  if (G_OBJECT_CLASS (udisks_daemon_parent_class)->finalize != NULL)
    G_OBJECT_CLASS (udisks_daemon_parent_class)->finalize (object);
}

static void
udisks_daemon_get_property (GObject    *object,
                            guint       prop_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
  UDisksDaemon *daemon = UDISKS_DAEMON (object);

  switch (prop_id)
    {
    case PROP_CONNECTION:
      g_value_set_object (value, udisks_daemon_get_connection (daemon));
      break;

    case PROP_OBJECT_MANAGER:
      g_value_set_object (value, udisks_daemon_get_object_manager (daemon));
      break;

    case PROP_MOUNT_MONITOR:
      g_value_set_object (value, udisks_daemon_get_mount_monitor (daemon));
      break;

    case PROP_CRYPTTAB_MONITOR:
      g_value_set_object (value, udisks_daemon_get_crypttab_monitor (daemon));
      break;

    case PROP_MODULE_MANAGER:
      g_value_set_object (value, udisks_daemon_get_module_manager (daemon));
      break;

    case PROP_CONFIG_MANAGER:
      g_value_set_object (value, udisks_daemon_get_config_manager (daemon));
      break;

    case PROP_DISABLE_MODULES:
      g_value_set_boolean (value, udisks_daemon_get_disable_modules (daemon));
      break;

    case PROP_FORCE_LOAD_MODULES:
      g_value_set_boolean (value, udisks_daemon_get_force_load_modules (daemon));
      break;

    case PROP_UNINSTALLED:
      g_value_set_boolean (value, udisks_daemon_get_uninstalled (daemon));
      break;

    case PROP_ENABLE_TCRYPT:
      g_value_set_boolean (value, udisks_daemon_get_enable_tcrypt (daemon));
      break;

    case PROP_UUID:
      g_value_set_string (value, udisks_daemon_get_uuid (daemon));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
udisks_daemon_set_property (GObject      *object,
                            guint         prop_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
  UDisksDaemon *daemon = UDISKS_DAEMON (object);
  const gchar *s;
  uuid_t uu;

  switch (prop_id)
    {
    case PROP_CONNECTION:
      g_assert (daemon->connection == NULL);
      daemon->connection = g_value_dup_object (value);
      break;

    case PROP_DISABLE_MODULES:
      daemon->disable_modules = g_value_get_boolean (value);
      break;

    case PROP_FORCE_LOAD_MODULES:
      daemon->force_load_modules = g_value_get_boolean (value);
      break;

    case PROP_UNINSTALLED:
      daemon->uninstalled = g_value_get_boolean (value);
      break;

    case PROP_ENABLE_TCRYPT:
      daemon->enable_tcrypt = g_value_get_boolean (value);
      break;

    case PROP_UUID:
      s = g_value_get_string (value);
      if (s != NULL && uuid_parse (s, uu) == 0)
        {
          g_free (daemon->uuid);
          daemon->uuid = g_strdup (s);
        }
      else
        g_warning ("Invalid UUID string '%s'", s);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
udisks_daemon_init (UDisksDaemon *daemon)
{
}

static void
mount_monitor_on_mount_removed (UDisksMountMonitor *monitor,
                                UDisksMount        *mount,
                                gpointer            user_data)
{
  UDisksDaemon *daemon = UDISKS_DAEMON (user_data);
  udisks_state_check (daemon->state);
}

static gboolean
load_modules_in_idle_cb (gpointer user_data)
{
  UDisksDaemon *daemon = UDISKS_DAEMON (user_data);

  /* clear the state file, loading all modules anyway */
  udisks_state_clear_modules (daemon->state);

  udisks_module_manager_load_modules (daemon->module_manager);

  return G_SOURCE_REMOVE;
}

static gboolean
check_modules_state_in_idle_cb (gpointer user_data)
{
  UDisksDaemon *daemon = UDISKS_DAEMON (user_data);
  gchar **modules;
  gchar **l;
  GError *error = NULL;

  modules = udisks_state_get_modules (daemon->state);

  /* first clear the state file */
  udisks_state_clear_modules (daemon->state);

  if (modules)
    {
      if (*modules)
        g_warning ("Unclean shutdown detected, reloading modules from previous session.");

      for (l = modules; l && *l; l++)
        if (! udisks_module_manager_load_single_module (daemon->module_manager, *l, &error))
          {
            g_warning ("Error re-initializing module %s: %s", *l, error->message);
            g_clear_error (&error);
          }

      g_strfreev (modules);
    }

  return G_SOURCE_REMOVE;
}

static void
udisks_daemon_constructed (GObject *object)
{
  UDisksDaemon *daemon = UDISKS_DAEMON (object);
  GError *error;
  gboolean ret = FALSE;
  gchar uuid_buf[UUID_STR_LEN] = {0};
  uuid_t uuid;

  /* NULL means no specific so_name (implementation) */
  BDPluginSpec part_plugin = {BD_PLUGIN_PART, NULL};
  BDPluginSpec swap_plugin = {BD_PLUGIN_SWAP, NULL};
  BDPluginSpec loop_plugin = {BD_PLUGIN_LOOP, NULL};
  BDPluginSpec mdraid_plugin = {BD_PLUGIN_MDRAID, NULL};
  BDPluginSpec fs_plugin = {BD_PLUGIN_FS, NULL};
  BDPluginSpec crypto_plugin = {BD_PLUGIN_CRYPTO, NULL};

  /* The core daemon needs the part, swap, loop, mdraid, fs and crypto plugins.
     Additional plugins are required by various modules, but they make sure
     plugins are loaded themselves. */
  BDPluginSpec *plugins[] = {&part_plugin, &swap_plugin, &loop_plugin, &mdraid_plugin,
                             &fs_plugin, &crypto_plugin, NULL};
  BDPluginSpec **plugin_p = NULL;
  error = NULL;

  /* Skip runtime dependency checks when initializing libblockdev. Plugin
     shouldn't fail to load just because some if its dependencies is missing.
   */
  ret = bd_switch_init_checks (FALSE, &error);
  if (!ret)
    {
      udisks_error ("Error initializing libblockdev library: %s (%s, %d)",
                    error->message, g_quark_to_string (error->domain), error->code);
      g_clear_error (&error);
    }

  ret = bd_try_init (plugins, NULL, NULL, &error);
  if (!ret)
    {
      if (error)
        {
          udisks_error ("Error initializing libblockdev library: %s (%s, %d)",
                        error->message, g_quark_to_string (error->domain), error->code);
          g_clear_error (&error);
        }
    else
      {
        /* a missing plugin is okay, calling functions from it will fail, but
           until that happens, life will just be great */
        for (plugin_p=plugins; *plugin_p; plugin_p++)
          if (!bd_is_plugin_available ((*plugin_p)->name))
            udisks_warning ("Failed to load the '%s' libblockdev plugin",
                            bd_get_plugin_name ((*plugin_p)->name));
      }
    }

  /* Generate global UUID */
  uuid_generate (uuid);
  uuid_unparse (uuid, &uuid_buf[0]);
  daemon->uuid = g_strdup (uuid_buf);

  daemon->authority = polkit_authority_get_sync (NULL, &error);
  if (daemon->authority == NULL)
    {
      udisks_critical ("Error initializing polkit authority: %s (%s, %d)",
                    error->message, g_quark_to_string (error->domain), error->code);
      g_clear_error (&error);
    }

  daemon->object_manager = g_dbus_object_manager_server_new ("/org/freedesktop/UDisks2");

  if (!g_file_test ("/run/udisks2", G_FILE_TEST_IS_DIR))
    {
      if (g_mkdir_with_parents ("/run/udisks2", 0700) != 0)
        {
          udisks_critical ("Error creating directory %s: %m", "/run/udisks2");
        }
    }

  if (!g_file_test (PACKAGE_LOCALSTATE_DIR "/lib/udisks2", G_FILE_TEST_IS_DIR))
    {
      if (g_mkdir_with_parents (PACKAGE_LOCALSTATE_DIR "/lib/udisks2", 0700) != 0)
        {
          udisks_critical ("Error creating directory %s: %m", PACKAGE_LOCALSTATE_DIR "/lib/udisks2");
        }
    }


  if (! daemon->uninstalled)
    {
      daemon->config_manager = udisks_config_manager_new ();
      daemon->module_manager = udisks_module_manager_new (daemon);
    }
  else
    {
      daemon->config_manager = udisks_config_manager_new_uninstalled ();
      daemon->module_manager = udisks_module_manager_new_uninstalled (daemon);
    }

  daemon->mount_monitor = udisks_mount_monitor_new (daemon);

  daemon->state = udisks_state_new (daemon);

  g_signal_connect (daemon->mount_monitor,
                    "mount-removed",
                    G_CALLBACK (mount_monitor_on_mount_removed),
                    daemon);

  daemon->crypttab_monitor = udisks_crypttab_monitor_new ();
#ifdef HAVE_LIBMOUNT_UTAB
  daemon->utab_monitor = udisks_utab_monitor_new ();
#endif

  /* now add providers */
  daemon->linux_provider = udisks_linux_provider_new (daemon);
  udisks_provider_start (UDISKS_PROVIDER (daemon->linux_provider));

  /* fill in default mount options */
  g_object_set_data_full (object,
                          "mount-options",
                          udisks_linux_mount_options_get_builtin (),
                          (GDestroyNotify) g_hash_table_destroy);

  /* Load modules if requested but only once providers have started and
   * have connected on the UDisksModuleManager::modules-activated signal.
   */
  if (daemon->force_load_modules ||
      udisks_config_manager_get_load_preference (daemon->config_manager) == UDISKS_MODULE_LOAD_ONSTARTUP)
    {
      /* load all modules */
      g_idle_add (load_modules_in_idle_cb, daemon);
    } else {
      /* crash recovery - load active modules from previous session */
      g_idle_add (check_modules_state_in_idle_cb, daemon);
    }

  /* Export the ObjectManager */
  g_dbus_object_manager_server_set_connection (daemon->object_manager, daemon->connection);

  /* Start cleaning up */
  udisks_state_start_cleanup (daemon->state);
  udisks_state_check (daemon->state);

  if (G_OBJECT_CLASS (udisks_daemon_parent_class)->constructed != NULL)
    G_OBJECT_CLASS (udisks_daemon_parent_class)->constructed (object);
}


static void
udisks_daemon_class_init (UDisksDaemonClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize     = udisks_daemon_finalize;
  gobject_class->constructed  = udisks_daemon_constructed;
  gobject_class->set_property = udisks_daemon_set_property;
  gobject_class->get_property = udisks_daemon_get_property;

  /**
   * UDisksDaemon:connection:
   *
   * The #GDBusConnection the daemon is for.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_CONNECTION,
                                   g_param_spec_object ("connection",
                                                        "Connection",
                                                        "The D-Bus connection the daemon is for",
                                                        G_TYPE_DBUS_CONNECTION,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_WRITABLE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  /**
   * UDisksDaemon:object-manager:
   *
   * The #GDBusObjectManager used by the daemon
   */
  g_object_class_install_property (gobject_class,
                                   PROP_OBJECT_MANAGER,
                                   g_param_spec_object ("object-manager",
                                                        "Object Manager",
                                                        "The D-Bus Object Manager server used by the daemon",
                                                        G_TYPE_DBUS_OBJECT_MANAGER_SERVER,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_STATIC_STRINGS));

  /**
   * UDisksDaemon:mount-monitor:
   *
   * The #UDisksMountMonitor used by the daemon
   */
  g_object_class_install_property (gobject_class,
                                   PROP_MOUNT_MONITOR,
                                   g_param_spec_object ("mount-monitor",
                                                        "Mount Monitor",
                                                        "The mount monitor",
                                                        UDISKS_TYPE_MOUNT_MONITOR,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_STATIC_STRINGS));

  /**
   * UDisksDaemon:disable-modules:
   *
   * Whether modules should be disabled
   */
  g_object_class_install_property (gobject_class,
                                   PROP_DISABLE_MODULES,
                                   g_param_spec_boolean ("disable-modules",
                                                         "Disable modules",
                                                         "Whether modules should be disabled",
                                                         FALSE,
                                                         G_PARAM_READABLE |
                                                         G_PARAM_WRITABLE |
                                                         G_PARAM_CONSTRUCT_ONLY));

  /**
   * UDisksDaemon:force-load-modules:
   *
   * Whether modules should be activated upon startup
   */
  g_object_class_install_property (gobject_class,
                                   PROP_FORCE_LOAD_MODULES,
                                   g_param_spec_boolean ("force-load-modules",
                                                         "Force load modules",
                                                         "Whether modules should be activated upon startup",
                                                         FALSE,
                                                         G_PARAM_READABLE |
                                                         G_PARAM_WRITABLE |
                                                         G_PARAM_CONSTRUCT_ONLY));

  /**
   * UDisksDaemon:uninstalled:
   *
   * Loads modules from the build directory.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_UNINSTALLED,
                                   g_param_spec_boolean ("uninstalled",
                                                         "Load modules from the build directory",
                                                         "Whether the modules should be loaded from the build directory",
                                                         FALSE,
                                                         G_PARAM_READABLE |
                                                         G_PARAM_WRITABLE |
                                                         G_PARAM_CONSTRUCT_ONLY));

  /**
   * UDisksDaemon:enable-tcrypt:
   *
   * Whether devices should be tested for being TCRYPT encrypted.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_ENABLE_TCRYPT,
                                   g_param_spec_boolean ("enable-tcrypt",
                                                         "Enable TCRYPT",
                                                         "Whether devices should be tested for being TCRYPT encrypted",
                                                         FALSE,
                                                         G_PARAM_READABLE |
                                                         G_PARAM_WRITABLE |
                                                         G_PARAM_CONSTRUCT_ONLY));

  /**
   * UDisksDaemon:uuid:
   *
   * The UUID specific for this daemon instance.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_UUID,
                                   g_param_spec_string ("uuid",
                                                        "Daemon UUID",
                                                        "The UUID specific for this daemon instance",
                                                         NULL,
                                                         G_PARAM_READABLE |
                                                         G_PARAM_WRITABLE));
}

/**
 * udisks_daemon_new:
 * @connection: A #GDBusConnection.
 * @disable_modules: Indicates whether modules should never be activated.
 * @force_load_modules: Activate modules on startup (for debugging purposes).
 * @uninstalled: Loads modules from the build directory (for debugging purposes).
 * @enable_tcrypt: Checks whether devices could be TCRYPT encrypted.
 *
 * Create a new daemon object for exporting objects on @connection.
 *
 * Returns: A #UDisksDaemon object. Free with g_object_unref().
 */
UDisksDaemon *
udisks_daemon_new (GDBusConnection *connection,
                   gboolean         disable_modules,
                   gboolean         force_load_modules,
                   gboolean         uninstalled,
                   gboolean         enable_tcrypt)
{
  g_return_val_if_fail (G_IS_DBUS_CONNECTION (connection), NULL);
  return UDISKS_DAEMON (g_object_new (UDISKS_TYPE_DAEMON,
                                      "connection", connection,
                                      "disable-modules", disable_modules,
                                      "force-load-modules", force_load_modules,
                                      "uninstalled", uninstalled,
                                      "enable-tcrypt", enable_tcrypt,
                                      NULL));
}

/**
 * udisks_daemon_get_connection:
 * @daemon: A #UDisksDaemon.
 *
 * Gets the D-Bus connection used by @daemon.
 *
 * Returns: A #GDBusConnection. Do not free, the object is owned by @daemon.
 */
GDBusConnection *
udisks_daemon_get_connection (UDisksDaemon *daemon)
{
  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), NULL);
  return daemon->connection;
}

/**
 * udisks_daemon_get_object_manager:
 * @daemon: A #UDisksDaemon.
 *
 * Gets the D-Bus object manager used by @daemon.
 *
 * Returns: A #GDBusObjectManagerServer. Do not free, the object is owned by @daemon.
 */
GDBusObjectManagerServer *
udisks_daemon_get_object_manager (UDisksDaemon *daemon)
{
  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), NULL);
  return daemon->object_manager;
}

/**
 * udisks_daemon_get_mount_monitor:
 * @daemon: A #UDisksDaemon
 *
 * Gets the mount monitor used by @daemon.
 *
 * Returns: A #UDisksMountMonitor. Do not free, the object is owned by @daemon.
 */
UDisksMountMonitor *
udisks_daemon_get_mount_monitor (UDisksDaemon *daemon)
{
  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), NULL);
  return daemon->mount_monitor;
}

/**
 * udisks_daemon_get_crypttab_monitor:
 * @daemon: A #UDisksDaemon
 *
 * Gets the crypttab monitor used by @daemon.
 *
 * Returns: A #UDisksCrypttabMonitor. Do not free, the object is owned by @daemon.
 */
UDisksCrypttabMonitor *
udisks_daemon_get_crypttab_monitor (UDisksDaemon *daemon)
{
  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), NULL);
  return daemon->crypttab_monitor;
}

#ifdef HAVE_LIBMOUNT_UTAB
/**
 * udisks_daemon_get_utab_monitor:
 * @daemon: A #UDisksDaemon
 *
 * Gets the utab monitor used by @daemon.
 *
 * Returns: A #UDisksUtabMonitor. Do not free, the object is owned by @daemon.
 */
UDisksUtabMonitor *
udisks_daemon_get_utab_monitor (UDisksDaemon *daemon)
{
  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), NULL);
  return daemon->utab_monitor;
}
#endif

/**
 * udisks_daemon_get_linux_provider:
 * @daemon: A #UDisksDaemon.
 *
 * Gets the Linux Provider, if any.
 *
 * Returns: A #UDisksLinuxProvider or %NULL. Do not free, the object is owned by @daemon.
 */
UDisksLinuxProvider *
udisks_daemon_get_linux_provider (UDisksDaemon *daemon)
{
  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), NULL);
  return daemon->linux_provider;
}

/**
 * udisks_daemon_get_authority:
 * @daemon: A #UDisksDaemon.
 *
 * Gets the PolicyKit authority used by @daemon.
 *
 * Returns: A #PolkitAuthority instance or %NULL if the polkit
 * authority is not available. Do not free, the object is owned by
 * @daemon.
 */
PolkitAuthority *
udisks_daemon_get_authority (UDisksDaemon *daemon)
{
  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), NULL);
  return daemon->authority;
}

/**
 * udisks_daemon_get_state:
 * @daemon: A #UDisksDaemon.
 *
 * Gets the state object used by @daemon.
 *
 * Returns: A #UDisksState instance. Do not free, the object is owned by @daemon.
 */
UDisksState *
udisks_daemon_get_state (UDisksDaemon *daemon)
{
  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), NULL);
  return daemon->state;
}

/* ---------------------------------------------------------------------------------------------------- */

typedef struct
{
  UDisksDaemon *daemon;
  UDisksInhibitCookie *inhibit_cookie;
} JobData;

static void
free_job_data (JobData *job_data)
{
  if (job_data->daemon != NULL)
    g_object_unref (job_data->daemon);
  g_free (job_data);
}

static void
on_job_completed (UDisksJob    *job,
                  gboolean      success,
                  const gchar  *message,
                  gpointer      user_data)
{
  JobData *job_data = (JobData *) user_data;
  UDisksDaemon *daemon = UDISKS_DAEMON (job_data->daemon);
  UDisksObjectSkeleton *object;

  object = UDISKS_OBJECT_SKELETON (g_dbus_interface_get_object (G_DBUS_INTERFACE (job)));
  g_assert (object != NULL);

  /* Unexport job */
  g_dbus_object_manager_server_unexport (daemon->object_manager,
                                         g_dbus_object_get_object_path (G_DBUS_OBJECT (object)));
  g_object_unref (object);

  /* free the allocated job object */
  g_object_unref (job);

  /* unregister inhibitor from systemd logind */
  udisks_daemon_util_uninhibit_system_sync (job_data->inhibit_cookie);
  /* returns the reference we took when connecting to the
   * UDisksJob::completed signal in udisks_daemon_launch_{spawned,threaded}_job()
   * below
   */
  free_job_data (job_data);
}

/* ---------------------------------------------------------------------------------------------------- */

static guint job_id = 0;

/* ---------------------------------------------------------------------------------------------------- */

static UDisksBaseJob *
common_job (UDisksDaemon    *daemon,
            UDisksObject    *object,
            const gchar     *job_operation,
            uid_t            job_started_by_uid,
            gpointer         job)
{
  gchar *job_object_path;
  UDisksObjectSkeleton *job_object;
  JobData *job_data;
  gchar *operation_description;

  job_data = g_new0 (JobData, 1);
  job_data->daemon = g_object_ref (daemon);
  /* register inhibitor to systemd logind while job is running */
  operation_description = udisks_client_get_job_description_from_operation (job_operation);
  job_data->inhibit_cookie = udisks_daemon_util_inhibit_system_sync (operation_description);
  g_free (operation_description);

  if (object != NULL)
    udisks_base_job_add_object (UDISKS_BASE_JOB (job), object);

  job_object_path = g_strdup_printf ("/org/freedesktop/UDisks2/jobs/%u", g_atomic_int_add (&job_id, 1));
  job_object = udisks_object_skeleton_new (job_object_path);
  udisks_object_skeleton_set_job (job_object, UDISKS_JOB (job));
  g_free (job_object_path);

  udisks_job_set_cancelable (UDISKS_JOB (job), TRUE);
  udisks_job_set_operation (UDISKS_JOB (job), job_operation);
  udisks_job_set_started_by_uid (UDISKS_JOB (job), job_started_by_uid);

  g_dbus_object_manager_server_export (daemon->object_manager, G_DBUS_OBJECT_SKELETON (job_object));
  g_signal_connect_after (job,
                          "completed",
                          G_CALLBACK (on_job_completed),
                          job_data);

  return UDISKS_BASE_JOB (job);
}

/**
 * udisks_daemon_launch_simple_job:
 * @daemon: A #UDisksDaemon.
 * @object: (allow-none): A #UDisksObject to add to the job or %NULL.
 * @job_operation: The operation for the job.
 * @job_started_by_uid: The user who started the job.
 * @cancellable: A #GCancellable or %NULL.
 *
 * Launches a new simple job.
 *
 * The returned object will be exported on the bus until the
 * #UDisksJob::completed signal is emitted on the object. It is not
 * valid to use the returned object after this signal fires.
 *
 * Returns: A #UDisksSimpleJob object. Do not free, the object
 * belongs to @manager.
 */
UDisksBaseJob *
udisks_daemon_launch_simple_job (UDisksDaemon    *daemon,
                                 UDisksObject    *object,
                                 const gchar     *job_operation,
                                 uid_t            job_started_by_uid,
                                 GCancellable    *cancellable)
{
  UDisksSimpleJob *job;

  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), NULL);

  job = udisks_simple_job_new (daemon, cancellable);
  return common_job (daemon, object, job_operation, job_started_by_uid, job);
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_daemon_launch_threaded_job:
 * @daemon: A #UDisksDaemon.
 * @object: (allow-none): A #UDisksObject to add to the job or %NULL.
 * @job_operation: The operation for the job.
 * @job_started_by_uid: The user who started the job.
 * @job_func: The function to run in another thread.
 * @user_data: User data to pass to @job_func.
 * @user_data_free_func: Function to free @user_data with or %NULL.
 * @cancellable: A #GCancellable or %NULL.
 *
 * Launches a new job by running @job_func in a new dedicated thread.
 *
 * The job is not started automatically! Use udisks_threaded_job_start() to
 * start the job after #UDisksThreadedJob::threaded-job-completed or
 * #UDisksJob::completed signals are connected (to get notified when the job is
 * done). This is to prevent a race condition with the @job_func finishing
 * before the signals are connected in which case the signal handlers are never
 * triggered.
 *
 * Long-running jobs should periodically check @cancellable to see if
 * they have been cancelled.
 *
 * The returned object will be exported on the bus until the
 * #UDisksJob::completed signal is emitted on the object. It is not
 * valid to use the returned object after this signal fires.
 *
 * Returns: A #UDisksThreadedJob object. Do not free, the object
 * belongs to @manager.
 */
UDisksBaseJob *
udisks_daemon_launch_threaded_job  (UDisksDaemon          *daemon,
                                    UDisksObject          *object,
                                    const gchar           *job_operation,
                                    uid_t                  job_started_by_uid,
                                    UDisksThreadedJobFunc  job_func,
                                    gpointer               user_data,
                                    GDestroyNotify         user_data_free_func,
                                    GCancellable          *cancellable)
{
  UDisksThreadedJob *job;

  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), NULL);
  g_return_val_if_fail (job_func != NULL, NULL);

  job = udisks_threaded_job_new (job_func,
                                 user_data,
                                 user_data_free_func,
                                 daemon,
                                 cancellable);
  return common_job (daemon, object, job_operation, job_started_by_uid, job);
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_daemon_launch_spawned_job:
 * @daemon: A #UDisksDaemon.
 * @object: (allow-none): A #UDisksObject to add to the job or %NULL.
 * @job_operation: The operation for the job.
 * @job_started_by_uid: The user who started the job.
 * @cancellable: A #GCancellable or %NULL.
 * @run_as_uid: The #uid_t to run the command as.
 * @run_as_euid: The effective #uid_t to run the command as.
 * @input_string: A string to write to stdin of the spawned program or %NULL.
 * @command_line_format: printf()-style format for the command line to spawn.
 * @...: Arguments for @command_line_format.
 *
 * Launches a new job for @command_line_format.
 *
 * The job is not started automatically! Use udisks_spawned_job_start() to start
 * the job after #UDisksSpawnedJob::spawned-job-completed or
 * #UDisksJob::completed signals are connected (to get notified when the job is
 * done). This is to prevent a race condition with the spawned process
 * terminating before the signals are connected in which case the signal
 * handlers are never triggered.
 *
 * The returned object will be exported on the bus until the
 * #UDisksJob::completed signal is emitted on the object. It is not
 * valid to use the returned object after this signal fires.
 *
 * Returns: A #UDisksSpawnedJob object. Do not free, the object
 * belongs to @manager.
 */
UDisksBaseJob *
udisks_daemon_launch_spawned_job (UDisksDaemon    *daemon,
                                  UDisksObject    *object,
                                  const gchar     *job_operation,
                                  uid_t            job_started_by_uid,
                                  GCancellable    *cancellable,
                                  uid_t            run_as_uid,
                                  uid_t            run_as_euid,
                                  const gchar     *input_string,
                                  const gchar     *command_line_format,
                                  ...)
{
  va_list var_args;
  gchar *command_line;
  GString *input_string_as_gstring = NULL;
  UDisksBaseJob *job;

  if (input_string != NULL)
    input_string_as_gstring = g_string_new (input_string);

  va_start (var_args, command_line_format);
  command_line = g_strdup_vprintf (command_line_format, var_args);
  va_end (var_args);

  job = udisks_daemon_launch_spawned_job_gstring (daemon,
                                          object,
                                          job_operation,
                                          job_started_by_uid,
                                          cancellable,
                                          run_as_uid,
                                          run_as_euid,
                                          input_string_as_gstring,
                                          "%s",
                                          command_line);

  udisks_string_wipe_and_free (input_string_as_gstring);
  g_free (command_line);
  return job;
}

/**
 * udisks_daemon_launch_spawned_job_gstring:
 * @daemon: A #UDisksDaemon.
 * @object: (allow-none): A #UDisksObject to add to the job or %NULL.
 * @job_operation: The operation for the job.
 * @job_started_by_uid: The user who started the job.
 * @cancellable: A #GCancellable or %NULL.
 * @run_as_uid: The #uid_t to run the command as.
 * @run_as_euid: The effective #uid_t to run the command as.
 * @input_string: A string to write to stdin of the spawned program or %NULL.
 * @command_line_format: printf()-style format for the command line to spawn.
 * @...: Arguments for @command_line_format.
 *
 * Launches a new job for @command_line_format.
 *
 * The job is not started automatically! Use udisks_spawned_job_start() to start
 * the job after #UDisksSpawnedJob::spawned-job-completed or
 * #UDisksJob::completed signals are connected (to get notified when the job is
 * done). This is to prevent a race condition with the spawned process
 * terminating before the signals are connected in which case the signal
 * handlers are never triggered.
 *
 * The returned object will be exported on the bus until the
 * #UDisksJob::completed signal is emitted on the object. It is not
 * valid to use the returned object after this signal fires.
 *
 * This function is the same as udisks_daemon_launch_spawned_job, with
 * the only difference that it takes a GString and is therefore able to
 * handle binary inputs that contain '\0' bytes.
 *
 * Returns: A #UDisksSpawnedJob object. Do not free, the object
 * belongs to @manager.
 */
UDisksBaseJob *
udisks_daemon_launch_spawned_job_gstring (
                                  UDisksDaemon    *daemon,
                                  UDisksObject    *object,
                                  const gchar     *job_operation,
                                  uid_t            job_started_by_uid,
                                  GCancellable    *cancellable,
                                  uid_t            run_as_uid,
                                  uid_t            run_as_euid,
                                  GString         *input_string,
                                  const gchar     *command_line_format,
                                  ...)
{
  va_list var_args;
  gchar *command_line;
  UDisksSpawnedJob *job;

  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), NULL);
  g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), NULL);
  g_return_val_if_fail (command_line_format != NULL, NULL);

  va_start (var_args, command_line_format);
  command_line = g_strdup_vprintf (command_line_format, var_args);
  va_end (var_args);
  job = udisks_spawned_job_new (command_line, input_string, run_as_uid, run_as_euid, daemon, cancellable);
  g_free (command_line);

  return common_job (daemon, object, job_operation, job_started_by_uid, job);
}

/* ---------------------------------------------------------------------------------------------------- */

typedef struct
{
  GMainContext *context;
  GMainLoop *loop;
  gboolean success;
  gint status;
  gchar *message;
} SpawnedJobSyncData;

static gboolean
spawned_job_sync_on_spawned_job_completed (UDisksSpawnedJob *job,
                                           GError           *error,
                                           gint              status,
                                           GString          *standard_output,
                                           GString          *standard_error,
                                           gpointer          user_data)
{
  SpawnedJobSyncData *data = user_data;
  data->status = status;
  return FALSE; /* let other handlers run */
}

static void
spawned_job_sync_on_completed (UDisksJob    *job,
                               gboolean      success,
                               const gchar  *message,
                               gpointer      user_data)
{
  SpawnedJobSyncData *data = user_data;
  data->success = success;
  data->message = g_strdup (message);
  g_main_loop_quit (data->loop);
}

/**
 * udisks_daemon_launch_spawned_job_sync:
 * @daemon: A #UDisksDaemon.
 * @object: (allow-none): A #UDisksObject to add to the job or %NULL.
 * @job_operation: The operation for the job.
 * @job_started_by_uid: The user who started the job.
 * @cancellable: A #GCancellable or %NULL.
 * @run_as_uid: The #uid_t to run the command as.
 * @run_as_euid: The effective #uid_t to run the command as.
 * @input_string: A string to write to stdin of the spawned program or %NULL.
 * @out_status: Return location for the @status parameter of the #UDisksSpawnedJob::spawned-job-completed signal.
 * @out_message: Return location for the @message parameter of the #UDisksJob::completed signal.
 * @command_line_format: printf()-style format for the command line to spawn.
 * @...: Arguments for @command_line_format.
 *
 * Like udisks_daemon_launch_spawned_job() but blocks the calling
 * thread until the job completes.
 *
 * Returns: The @success parameter of the #UDisksJob::completed signal.
 */
gboolean
udisks_daemon_launch_spawned_job_sync (UDisksDaemon    *daemon,
                                       UDisksObject    *object,
                                       const gchar     *job_operation,
                                       uid_t            job_started_by_uid,
                                       GCancellable    *cancellable,
                                       uid_t            run_as_uid,
                                       uid_t            run_as_euid,
                                       gint            *out_status,
                                       gchar          **out_message,
                                       const gchar     *input_string,
                                       const gchar     *command_line_format,
                                       ...)
{
  va_list var_args;
  gchar *command_line;
  GString *input_string_as_gstring = NULL;
  gboolean ret;

  if (input_string != NULL)
    input_string_as_gstring = g_string_new (input_string);

  va_start (var_args, command_line_format);
  command_line = g_strdup_vprintf (command_line_format, var_args);
  va_end (var_args);

  ret = udisks_daemon_launch_spawned_job_gstring_sync (daemon,
                                          object,
                                          job_operation,
                                          job_started_by_uid,
                                          cancellable,
                                          run_as_uid,
                                          run_as_euid,
                                          out_status,
                                          out_message,
                                          input_string_as_gstring,
                                          "%s",
                                          command_line);

  udisks_string_wipe_and_free (input_string_as_gstring);
  g_free (command_line);
  return ret;
}

/**
 * udisks_daemon_launch_spawned_job_gstring_sync:
 * @daemon: A #UDisksDaemon.
 * @object: (allow-none): A #UDisksObject to add to the job or %NULL.
 * @job_operation: The operation for the job.
 * @job_started_by_uid: The user who started the job.
 * @cancellable: A #GCancellable or %NULL.
 * @run_as_uid: The #uid_t to run the command as.
 * @run_as_euid: The effective #uid_t to run the command as.
 * @input_string: A string to write to stdin of the spawned program or %NULL.
 * @out_status: Return location for the @status parameter of the #UDisksSpawnedJob::spawned-job-completed signal.
 * @out_message: Return location for the @message parameter of the #UDisksJob::completed signal.
 * @command_line_format: printf()-style format for the command line to spawn.
 * @...: Arguments for @command_line_format.
 *
 * Like udisks_daemon_launch_spawned_job() but blocks the calling
 * thread until the job completes.
 *
 * This function is the same as udisks_daemon_launch_spawned_job_sync, with
 * the only difference that it takes a GString and is therefore able to
 * handle binary inputs that contain '\0' bytes.
 *
 * Returns: The @success parameter of the #UDisksJob::completed signal.
 */
gboolean
udisks_daemon_launch_spawned_job_gstring_sync (UDisksDaemon    *daemon,
                                       UDisksObject    *object,
                                       const gchar     *job_operation,
                                       uid_t            job_started_by_uid,
                                       GCancellable    *cancellable,
                                       uid_t            run_as_uid,
                                       uid_t            run_as_euid,
                                       gint            *out_status,
                                       gchar          **out_message,
                                       GString         *input_string,
                                       const gchar     *command_line_format,
                                       ...)
{
  va_list var_args;
  gchar *command_line;
  UDisksBaseJob *job;
  SpawnedJobSyncData data;

  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), FALSE);
  g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), FALSE);
  g_return_val_if_fail (command_line_format != NULL, FALSE);

  data.context = g_main_context_new ();
  g_main_context_push_thread_default (data.context);
  data.loop = g_main_loop_new (data.context, FALSE);
  data.success = FALSE;
  data.status = 0;
  data.message = NULL;

  va_start (var_args, command_line_format);
  command_line = g_strdup_vprintf (command_line_format, var_args);
  va_end (var_args);
  job = udisks_daemon_launch_spawned_job_gstring (daemon,
                                          object,
                                          job_operation,
                                          job_started_by_uid,
                                          cancellable,
                                          run_as_uid,
                                          run_as_euid,
                                          input_string,
                                          "%s",
                                          command_line);
  g_signal_connect (job,
                    "spawned-job-completed",
                    G_CALLBACK (spawned_job_sync_on_spawned_job_completed),
                    &data);
  g_signal_connect_after (job,
                          "completed",
                          G_CALLBACK (spawned_job_sync_on_completed),
                          &data);

  udisks_spawned_job_start (UDISKS_SPAWNED_JOB (job));
  g_main_loop_run (data.loop);

  if (out_status != NULL)
    *out_status = data.status;

  if (out_message != NULL)
    *out_message = data.message;
  else
    g_free (data.message);

  g_free (command_line);
  g_main_loop_unref (data.loop);
  g_main_context_pop_thread_default (data.context);
  g_main_context_unref (data.context);

  /* note: the job object is freed in the ::completed handler */

  return data.success;
}

/* ---------------------------------------------------------------------------------------------------- */

static __thread UDisksJob *thread_job = NULL;

static void
bd_thread_progress_callback (guint64            task_id,
                             BDUtilsProgStatus  status,
                             guint8             completion,
                             gchar             *msg)
{
  if (thread_job != NULL && msg == NULL)
    {
      if (!udisks_job_get_progress_valid (UDISKS_JOB (thread_job)))
        {
          udisks_job_set_progress_valid (UDISKS_JOB (thread_job), TRUE);
        }

      udisks_job_set_progress (UDISKS_JOB (thread_job), completion / 100.0);
    }
}

void
udisks_bd_thread_set_progress_for_job (UDisksJob *job)
{
  thread_job = job;
  bd_utils_init_prog_reporting_thread (bd_thread_progress_callback, NULL);
}

void
udisks_bd_thread_disable_progress (void)
{
  thread_job = NULL;
  bd_utils_init_prog_reporting_thread (NULL, NULL);
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_daemon_launch_threaded_job_sync:
 * @daemon: A #UDisksDaemon.
 * @object: (allow-none): A #UDisksObject to add to the job or %NULL.
 * @job_operation: The operation for the job.
 * @job_started_by_uid: The user who started the job.
 * @job_func: The function to run in another thread.
 * @user_data: User data to pass to @job_func.
 * @user_data_free_func: Function to free @user_data with or %NULL.
 * @cancellable: A #GCancellable or %NULL.
 * @error: The #GError set by the #UDisksThreadedJobFunc.
 *
 * Like udisks_daemon_launch_threaded_job() but blocks the calling
 * thread until the job completes.
 *
 * Returns: The @success parameter of the #UDisksJob::completed signal.
 */
gboolean
udisks_daemon_launch_threaded_job_sync (UDisksDaemon          *daemon,
                                        UDisksObject          *object,
                                        const gchar           *job_operation,
                                        uid_t                  job_started_by_uid,
                                        UDisksThreadedJobFunc  job_func,
                                        gpointer               user_data,
                                        GDestroyNotify         user_data_free_func,
                                        GCancellable          *cancellable,
                                        GError               **error)
{
  UDisksBaseJob *job;
  gboolean job_result;

  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), FALSE);
  g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), FALSE);

  job = udisks_daemon_launch_threaded_job (daemon,
                                           object,
                                           job_operation,
                                           job_started_by_uid,
                                           job_func,
                                           user_data,
                                           user_data_free_func,
                                           cancellable);

  /* TODO: There might not be much difference between calling the job_func() right away
   *       instead of having it enclosed in a GTask since we're blocking anyway.
   *       Deeper investigation of differences required. */
  job_result = udisks_threaded_job_run_sync (UDISKS_THREADED_JOB (job), error);

  /* note: the job object is freed in the ::completed handler */
  return job_result;
}

/* ---------------------------------------------------------------------------------------------------- */

typedef struct {
  GMainContext *context;
  GMainLoop *loop;
  gboolean timed_out;
} WaitData;

static gboolean
wait_on_timed_out (gpointer user_data)
{
  WaitData *data = user_data;
  data->timed_out = TRUE;
  g_main_loop_quit (data->loop);
  return FALSE; /* remove the source */
}

static gboolean
wait_on_recheck (gpointer user_data)
{
  WaitData *data = user_data;
  g_main_loop_quit (data->loop);
  return FALSE; /* remove the source */
}

static gpointer wait_for_objects (UDisksDaemon                *daemon,
                                  UDisksDaemonWaitFuncGeneric  wait_func,
                                  gpointer                     user_data,
                                  GDestroyNotify               user_data_free_func,
                                  guint                        timeout_seconds,
                                  gboolean                     to_disappear,
                                  GError                     **error)
{
  gpointer ret;
  WaitData data;

  /* TODO: support GCancellable */

  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), NULL);
  g_return_val_if_fail (wait_func != NULL, NULL);

  ret = NULL;

  memset (&data, '\0', sizeof (data));
  data.context = NULL;
  data.loop = NULL;

  g_object_ref (daemon);

 again:
  ret = wait_func (daemon, user_data);

  if ((!to_disappear && ret == NULL && timeout_seconds > 0) ||
      (to_disappear && ret != NULL && timeout_seconds > 0))
    {
      GSource *source;

      /* sit and wait for up to @timeout_seconds if the object isn't there already */
      if (data.context == NULL)
        {
          /* TODO: this will deadlock if we are calling from the main thread... */
          data.context = g_main_context_new ();
          data.loop = g_main_loop_new (data.context, FALSE);

          source = g_timeout_source_new_seconds (timeout_seconds);
          g_source_set_priority (source, G_PRIORITY_DEFAULT);
          g_source_set_callback (source, wait_on_timed_out, &data, NULL);
          g_source_attach (source, data.context);
          g_source_unref (source);
        }

      /* TODO: do something a bit more elegant than checking every 250ms ... it's
       *       probably going to involve having each UDisksProvider emit a "changed"
       *       signal when it's time to recheck... for now this works.
       */
      source = g_timeout_source_new (250);
      g_source_set_priority (source, G_PRIORITY_DEFAULT);
      g_source_set_callback (source, wait_on_recheck, &data, NULL);
      g_source_attach (source, data.context);
      g_source_unref (source);

      g_main_loop_run (data.loop);

      if (data.timed_out)
        {
          if (to_disappear)
            g_set_error (error,
                         UDISKS_ERROR, UDISKS_ERROR_FAILED,
                         "Timed out waiting");
          else
            g_set_error (error,
                         UDISKS_ERROR, UDISKS_ERROR_FAILED,
                         "Timed out waiting for object");
        }
      else
        {
          if (to_disappear)
            g_object_unref (G_OBJECT (ret));
          goto again;
        }
    }

  if (user_data_free_func != NULL)
    user_data_free_func (user_data);

  g_object_unref (daemon);

  if (data.loop != NULL)
    g_main_loop_unref (data.loop);
  if (data.context != NULL)
    g_main_context_unref (data.context);

  return ret;
}


/**
 * udisks_daemon_wait_for_object_sync:
 * @daemon: A #UDisksDaemon.
 * @wait_func: Function to check for desired object.
 * @user_data: User data to pass to @wait_func.
 * @user_data_free_func: (allow-none): Function to free @user_data or %NULL.
 * @timeout_seconds: Maximum time to wait for the object (in seconds) or 0 to never wait.
 * @error: (allow-none): Return location for error or %NULL.
 *
 * Blocks the calling thread until an object picked by @wait_func is
 * available or until @timeout_seconds has passed (in which case the
 * function fails with %UDISKS_ERROR_TIMED_OUT).
 *
 * Note that @wait_func will be called from time to time - for example
 * if there is a device event.
 *
 * Returns: (transfer full): The object picked by @wait_func or %NULL if @error is set.
 */
UDisksObject *
udisks_daemon_wait_for_object_sync (UDisksDaemon               *daemon,
                                    UDisksDaemonWaitFuncObject  wait_func,
                                    gpointer                    user_data,
                                    GDestroyNotify              user_data_free_func,
                                    guint                       timeout_seconds,
                                    GError                      **error)
{
  return (UDisksObject *) wait_for_objects (daemon,
                                            (UDisksDaemonWaitFuncGeneric) wait_func,
                                            user_data,
                                            user_data_free_func,
                                            timeout_seconds,
                                            FALSE, /* to_disappear */
                                            error);
}

/**
 * udisks_daemon_wait_for_objects_sync:
 * @daemon: A #UDisksDaemon.
 * @wait_func: Function to check for desired object.
 * @user_data: User data to pass to @wait_func.
 * @user_data_free_func: (allow-none): Function to free @user_data or %NULL.
 * @timeout_seconds: Maximum time to wait for the object (in seconds) or 0 to never wait.
 * @error: (allow-none): Return location for error or %NULL.
 *
 * Blocks the calling thread until one or more objects picked by @wait_func
 * is/are available or until @timeout_seconds has passed (in which case the
 * function fails with %UDISKS_ERROR_TIMED_OUT).
 *
 * Note that @wait_func will be called from time to time - for example
 * if there is a device event.
 *
 * Returns: (transfer full): The objects picked by @wait_func or %NULL if @error is set.
 */
UDisksObject **
udisks_daemon_wait_for_objects_sync (UDisksDaemon                 *daemon,
                                     UDisksDaemonWaitFuncObjects   wait_func,
                                     gpointer                      user_data,
                                     GDestroyNotify                user_data_free_func,
                                     guint                         timeout_seconds,
                                     GError                      **error)
{
  return (UDisksObject **) wait_for_objects (daemon,
                                             (UDisksDaemonWaitFuncGeneric) wait_func,
                                             user_data,
                                             user_data_free_func,
                                             timeout_seconds,
                                             FALSE, /* to_disappear */
                                             error);
}


/**
 * udisks_daemon_wait_for_object_to_disappear_sync:
 * @daemon: A #UDisksDaemon.
 * @wait_func: Function to check for desired object.
 * @user_data: User data to pass to @wait_func.
 * @user_data_free_func: (allow-none): Function to free @user_data or %NULL.
 * @timeout_seconds: Maximum time to wait for the object to disappear (in seconds) or 0 to never wait.
 * @error: (allow-none): Return location for error or %NULL.
 *
 * Blocks the calling thread until an object picked by @wait_func disappears or
 * until @timeout_seconds has passed (in which case the function fails with
 * %UDISKS_ERROR_TIMED_OUT).
 *
 * Note that @wait_func will be called from time to time - for example
 * if there is a device event. For consistency @wait_func is supposed
 * to return full reference to an existing object; udisks_daemon_wait_for_object_to_disappear_sync()
 * will take care of dropping the reference after each iteration.
 *
 * Returns: (transfer full): Whether the object picked by @wait_func disappeared or not (@error is set).
 */
gboolean
udisks_daemon_wait_for_object_to_disappear_sync (UDisksDaemon               *daemon,
                                                 UDisksDaemonWaitFuncObject  wait_func,
                                                 gpointer                    user_data,
                                                 GDestroyNotify              user_data_free_func,
                                                 guint                       timeout_seconds,
                                                 GError                      **error)
{
  UDisksObject *object;

  object = (UDisksObject *) wait_for_objects (daemon,
                                              (UDisksDaemonWaitFuncGeneric) wait_func,
                                              user_data,
                                              user_data_free_func,
                                              timeout_seconds,
                                              TRUE, /* to_disappear */
                                              error);
  if (object != NULL)
    g_object_unref (object);

  return NULL == object;
}


/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_daemon_find_block:
 * @daemon: A #UDisksDaemon.
 * @block_device_number: A #dev_t with the device number to find.
 *
 * Finds a block device with the number given by @block_device_number.
 *
 * Returns: (transfer full): A #UDisksObject or %NULL if not found. Free with g_object_unref().
 */
UDisksObject *
udisks_daemon_find_block (UDisksDaemon *daemon,
                          dev_t         block_device_number)
{
  UDisksObject *ret = NULL;
  GList *objects, *l;

  objects = g_dbus_object_manager_get_objects (G_DBUS_OBJECT_MANAGER (daemon->object_manager));
  for (l = objects; l != NULL; l = l->next)
    {
      UDisksObject *object = UDISKS_OBJECT (l->data);
      UDisksBlock *block;

      block = udisks_object_peek_block (object);
      if (block == NULL)
        continue;

      if (udisks_block_get_device_number (block) == block_device_number)
        {
          ret = g_object_ref (object);
          goto out;
        }
    }
 out:
  g_list_free_full (objects, g_object_unref);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_daemon_find_block_by_device_file:
 * @daemon: A #UDisksDaemon.
 * @device_file: A device file.
 *
 * Finds a block device with device file given by @device_file.
 *
 * Returns: (transfer full): A #UDisksObject or %NULL if not found. Free with g_object_unref().
 */
UDisksObject *
udisks_daemon_find_block_by_device_file (UDisksDaemon *daemon,
                                         const gchar  *device_file)
{
  UDisksObject *ret = NULL;
  GList *objects, *l;

  objects = g_dbus_object_manager_get_objects (G_DBUS_OBJECT_MANAGER (daemon->object_manager));
  for (l = objects; l != NULL; l = l->next)
    {
      UDisksObject *object = UDISKS_OBJECT (l->data);
      UDisksBlock *block;

      block = udisks_object_peek_block (object);
      if (block == NULL)
        continue;

      if (g_strcmp0 (udisks_block_get_device (block), device_file) == 0)
        {
          ret = g_object_ref (object);
          goto out;
        }
    }
 out:
  g_list_free_full (objects, g_object_unref);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_daemon_find_block_by_sysfs_path:
 * @daemon: A #UDisksDaemon.
 * @sysfs_path: A sysfs path.
 *
 * Finds a block device with a sysfs path given by @sysfs_path.
 *
 * Returns: (transfer full): A #UDisksObject or %NULL if not found. Free with g_object_unref().
 */
UDisksObject *
udisks_daemon_find_block_by_sysfs_path (UDisksDaemon *daemon,
                                        const gchar  *sysfs_path)
{
  UDisksObject *ret = NULL;
  GList *objects, *l;

  objects = g_dbus_object_manager_get_objects (G_DBUS_OBJECT_MANAGER (daemon->object_manager));
  for (l = objects; l != NULL; l = l->next)
    {
      UDisksObject *object = UDISKS_OBJECT (l->data);
      UDisksLinuxDevice *device;

      if (!UDISKS_IS_LINUX_BLOCK_OBJECT (object))
        continue;

      device = udisks_linux_block_object_get_device (UDISKS_LINUX_BLOCK_OBJECT (object));
      if (device == NULL)
        continue;

      if (g_strcmp0 (g_udev_device_get_sysfs_path (device->udev_device), sysfs_path) == 0)
        {
          g_object_unref (device);
          ret = g_object_ref (object);
          goto out;
        }
      g_object_unref (device);
    }
 out:
  g_list_free_full (objects, g_object_unref);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_daemon_find_object:
 * @daemon: A #UDisksDaemon.
 * @object_path: A #dev_t with the device number to find.
 *
 * Finds an exported object with the object path given by @object_path.
 *
 * Returns: (transfer full): A #UDisksObject or %NULL if not found. Free with g_object_unref().
 */
UDisksObject *
udisks_daemon_find_object (UDisksDaemon *daemon,
                           const gchar  *object_path)
{
  return (UDisksObject *) g_dbus_object_manager_get_object (G_DBUS_OBJECT_MANAGER (daemon->object_manager),
                                                            object_path);
}

/**
 * udisks_daemon_get_objects:
 * @daemon: A #UDisksDaemon.
 *
 * Gets all D-Bus objects exported by @daemon.
 *
 * Returns: (transfer full) (element-type UDisksObject): A list of #UDisksObject instances. The returned list should be freed with g_list_free() after each element has been freed with g_object_unref().
 */
GList *
udisks_daemon_get_objects (UDisksDaemon *daemon)
{
  return g_dbus_object_manager_get_objects (G_DBUS_OBJECT_MANAGER (daemon->object_manager));
}

/**
 * udisks_daemon_get_module_manager:
 * @daemon: A #UDisksDaemon.
 *
 * Gets the module manager used by @daemon.
 *
 * Returns: A #UDisksModuleManager. Do not free, the object is owned by @daemon.
 */
UDisksModuleManager *
udisks_daemon_get_module_manager (UDisksDaemon *daemon)
{
  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), NULL);
  return daemon->module_manager;
}

/**
 * udisks_daemon_get_config_manager:
 * @daemon: A #UDisksDaemon.
 *
 * Gets the config manager used by @daemon.
 *
 * Returns: A #UDisksConfigManager. Do not free, the object is owned by @daemon.
 */
UDisksConfigManager *
udisks_daemon_get_config_manager (UDisksDaemon *daemon)
{
  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), NULL);
  return daemon->config_manager;
}

/**
 * udisks_daemon_get_disable_modules:
 * @daemon: A #UDisksDaemon.
 *
 * Gets @daemon setting whether modules should never be loaded.
 *
 * Returns: %TRUE if --disable-modules commandline switch has been specified.
 */
gboolean
udisks_daemon_get_disable_modules (UDisksDaemon*daemon)
{
  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), FALSE);
  return daemon->disable_modules;
}

/**
 * udisks_daemon_get_force_load_modules:
 * @daemon: A #UDisksDaemon.
 *
 * Gets @daemon setting whether modules should be activated upon start.
 *
 * Returns: %TRUE if --force-load-modules commandline switch has been specified.
 */
gboolean
udisks_daemon_get_force_load_modules (UDisksDaemon *daemon)
{
  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), FALSE);
  return daemon->force_load_modules;
}

/**
 * udisks_daemon_get_uninstalled:
 * @daemon: A #UDisksDaemon.
 *
 * Gets @daemon setting whether the modules should be loaded from the build
 * directory.
 *
 * Returns: %TRUE if --uninstalled commandline switch has been specified.
 */
gboolean
udisks_daemon_get_uninstalled (UDisksDaemon *daemon)
{
  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), FALSE);
  return daemon->uninstalled;
}

/**
 * udisks_daemon_get_enable_tcrypt:
 * @daemon: A #UDisksDaemon.
 *
 * Gets @daemon setting whether devices should be tested for being TCRYPT
 * encrypted.
 *
 * Returns: %TRUE if "/etc/udisks2/tcrypt.conf" was an existing file during
 * daemon startup.
 */
gboolean
udisks_daemon_get_enable_tcrypt (UDisksDaemon *daemon)
{
  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), FALSE);
  return daemon->enable_tcrypt;
}

/**
 * udisks_daemon_get_uuid:
 * @daemon: A #UDisksDaemon.
 *
 * Gets the UUID string specific to this @daemon instance.
 *
 * Returns: the UUID string. Do not free, the string is owned by @daemon.
 */
const gchar *
udisks_daemon_get_uuid (UDisksDaemon *daemon)
{
  g_return_val_if_fail (UDISKS_IS_DAEMON (daemon), NULL);
  return daemon->uuid;
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_daemon_get_parent_for_tracking:
 * @daemon: A #UDisksDaemon.
 * @path: object path of a child to find parent of
 * @uuid: a pointer to return parent UUID string
 *
 * Finds parent block device and returns its object path and UUID.
 * If the return value is %NULL, the value of @uuid has not been changed.
 *
 * Returns: (transfer full): object path of the parent device. Free with g_free().
 */
gchar *
udisks_daemon_get_parent_for_tracking (UDisksDaemon  *daemon,
                                       const gchar   *path,
                                       gchar        **uuid)
{
  const gchar *parent_path = NULL;
  const gchar *parent_uuid = NULL;

  UDisksObject *object = NULL;
  UDisksObject *crypto_object = NULL;
  UDisksObject *mdraid_object = NULL;
  UDisksObject *table_object = NULL;

  UDisksBlock *block;
  UDisksBlock *crypto_block;
  UDisksMDRaid *mdraid;
  UDisksPartition *partition;
  UDisksBlock *table_block;

  GList *modules;
  GList *l;
  gchar *path_ret = NULL;

  object = udisks_daemon_find_object (daemon, path);
  if (object == NULL)
    goto out;

  block = udisks_object_peek_block (object);
  if (block)
    {
      crypto_object = udisks_daemon_find_object (daemon, udisks_block_get_crypto_backing_device (block));
      if (crypto_object)
        {
          crypto_block = udisks_object_peek_block (crypto_object);
          if (crypto_block)
            {
              parent_uuid = udisks_block_get_id_uuid (crypto_block);
              parent_path = udisks_block_get_crypto_backing_device (block);
              goto out;
            }
        }

      mdraid_object = udisks_daemon_find_object (daemon, udisks_block_get_mdraid (block));
      if (mdraid_object)
        {
          mdraid = udisks_object_peek_mdraid (mdraid_object);
          if (mdraid)
            {
              parent_uuid = udisks_mdraid_get_uuid (mdraid);
              parent_path = udisks_block_get_mdraid (block);
              goto out;
            }
        }

      partition = udisks_object_peek_partition (object);
      if (partition)
        {
          table_object = udisks_daemon_find_object (daemon, udisks_partition_get_table (partition));
          if (table_object)
            {
              table_block = udisks_object_peek_block (table_object);
              if (table_block)
                {
                  /* We don't want to track partition tables because
                     they can't be 'closed' in a way that their
                     children temporarily invisible.
                  */
                  parent_uuid = NULL;
                  parent_path = udisks_partition_get_table (partition);
                  goto out;
                }
            }
        }
    }

 out:
  g_clear_object (&object);
  g_clear_object (&crypto_object);
  g_clear_object (&mdraid_object);
  g_clear_object (&table_object);

  if (parent_path)
    {
      if (uuid)
        *uuid = g_strdup (parent_uuid);
      return g_strdup (parent_path);
    }

  modules = udisks_module_manager_get_modules (daemon->module_manager);
  for (l = modules; l != NULL; l = l->next)
    {
      UDisksModule *module = l->data;

      path_ret = udisks_module_track_parent (module, path, uuid);
      if (path_ret)
        break;
    }
  g_list_free_full (modules, g_object_unref);

  if (path_ret)
    return path_ret;

  return NULL;
}

/* ---------------------------------------------------------------------------------------------------- */
