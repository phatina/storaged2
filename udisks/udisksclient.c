/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */
/*
 * Copyright (C) 2011 David Zeuthen <zeuthen@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "config.h"
#include <glib.h>
#include <glib/gi18n-lib.h>

#include "udisksclient.h"
#include "udiskserror.h"
#include "udisks-generated.h"
#include "udisksobjectinfo.h"

/**
 * SECTION:udisksclient
 * @title: UDisksClient
 * @short_description: Utility routines for accessing the UDisks service
 *
 * #UDisksClient is used for accessing the UDisks service from a
 * client program.
 */

G_LOCK_DEFINE_STATIC (init_lock);

/**
 * UDisksClient:
 *
 * The #UDisksClient structure contains only private data and should
 * only be accessed using the provided API.
 */
struct _UDisksClient
{
  GObject parent_instance;

  gboolean is_initialized;
  GError *initialization_error;

  GDBusConnection *bus_connection;
  GDBusObjectManager *object_manager;

  GMainContext *context;

  GSource *changed_timeout_source;
};

typedef struct
{
  GObjectClass parent_class;
  GHashTable *changed_blacklist;
} UDisksClientClass;

enum
{
  PROP_0,
  PROP_OBJECT_MANAGER,
  PROP_MANAGER,
  PROP_BUS_CONNECTION
};

enum
{
  CHANGED_SIGNAL,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static void initable_iface_init       (GInitableIface      *initable_iface);
static void async_initable_iface_init (GAsyncInitableIface *async_initable_iface);

static void on_object_added (GDBusObjectManager  *manager,
                             GDBusObject         *object,
                             gpointer             user_data);

static void on_object_removed (GDBusObjectManager  *manager,
                               GDBusObject         *object,
                               gpointer             user_data);

static void on_interface_added (GDBusObjectManager  *manager,
                                GDBusObject         *object,
                                GDBusInterface      *interface,
                                gpointer             user_data);

static void on_interface_removed (GDBusObjectManager  *manager,
                                  GDBusObject         *object,
                                  GDBusInterface      *interface,
                                  gpointer             user_data);

static void on_interface_proxy_properties_changed (GDBusObjectManagerClient   *manager,
                                                   GDBusObjectProxy           *object_proxy,
                                                   GDBusProxy                 *interface_proxy,
                                                   GVariant                   *changed_properties,
                                                   const gchar *const         *invalidated_properties,
                                                   gpointer                    user_data);

static void maybe_emit_changed_now (UDisksClient *client);

static void init_interface_proxy (UDisksClient *client,
                                  GDBusProxy   *proxy);

static UDisksPartitionTypeInfo *udisks_partition_type_info_new (void);

G_DEFINE_TYPE_WITH_CODE (UDisksClient, udisks_client, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE, initable_iface_init)
                         G_IMPLEMENT_INTERFACE (G_TYPE_ASYNC_INITABLE, async_initable_iface_init)
                         );

static void
udisks_client_finalize (GObject *object)
{
  UDisksClient *client = UDISKS_CLIENT (object);

  if (client->changed_timeout_source != NULL)
    g_source_destroy (client->changed_timeout_source);

  if (client->initialization_error != NULL)
    g_clear_error (&(client->initialization_error));

  /* might be NULL if failing early in the constructor */
  if (client->object_manager != NULL)
    {
      g_signal_handlers_disconnect_by_func (client->object_manager,
                                            G_CALLBACK (on_object_added),
                                            client);
      g_signal_handlers_disconnect_by_func (client->object_manager,
                                            G_CALLBACK (on_object_removed),
                                            client);
      g_signal_handlers_disconnect_by_func (client->object_manager,
                                            G_CALLBACK (on_interface_added),
                                            client);
      g_signal_handlers_disconnect_by_func (client->object_manager,
                                            G_CALLBACK (on_interface_removed),
                                            client);
      g_signal_handlers_disconnect_by_func (client->object_manager,
                                            G_CALLBACK (on_interface_proxy_properties_changed),
                                            client);
      g_object_unref (client->object_manager);
    }

  if (client->context != NULL)
    g_main_context_unref (client->context);

  g_clear_object (&client->bus_connection);

  G_OBJECT_CLASS (udisks_client_parent_class)->finalize (object);
}

static void
udisks_client_init (UDisksClient *client)
{
  static volatile GQuark udisks_error_domain = 0;
  /* this will force associating errors in the UDISKS_ERROR error
   * domain with org.freedesktop.UDisks2.Error.* errors via
   * g_dbus_error_register_error_domain().
   */
  udisks_error_domain = UDISKS_ERROR;
  udisks_error_domain; /* shut up -Wunused-but-set-variable */
}

static void
udisks_client_get_property (GObject    *object,
                            guint       prop_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
  UDisksClient *client = UDISKS_CLIENT (object);

  switch (prop_id)
    {
    case PROP_OBJECT_MANAGER:
      g_value_set_object (value, udisks_client_get_object_manager (client));
      break;

    case PROP_MANAGER:
      g_value_set_object (value, udisks_client_get_manager (client));
      break;

    case PROP_BUS_CONNECTION:
      g_value_set_object (value, client->bus_connection);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
udisks_client_set_property (GObject      *object,
                            guint         prop_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
  UDisksClient *client = UDISKS_CLIENT (object);

  switch (prop_id)
    {
    case PROP_BUS_CONNECTION:
      /* Construct only. */
      g_assert (client->bus_connection == NULL);
      client->bus_connection = g_value_dup_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
udisks_client_class_init (UDisksClientClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize     = udisks_client_finalize;
  gobject_class->get_property = udisks_client_get_property;
  gobject_class->set_property = udisks_client_set_property;

  klass->changed_blacklist = g_hash_table_new (g_str_hash, g_str_equal);
  g_hash_table_insert (klass->changed_blacklist, (gpointer) "SmartSelftestPercentRemaining", NULL);
  g_hash_table_insert (klass->changed_blacklist, (gpointer) "SyncRate", NULL);
  g_hash_table_insert (klass->changed_blacklist, (gpointer) "SyncRemainingTime", NULL);

  /**
   * UDisksClient:object-manager:
   *
   * The #GDBusObjectManager used by the #UDisksClient instance.
   */
  g_object_class_install_property (gobject_class,
                                   PROP_OBJECT_MANAGER,
                                   g_param_spec_object ("object-manager",
                                                        "Object Manager",
                                                        "The GDBusObjectManager used by the UDisksClient",
                                                        G_TYPE_DBUS_OBJECT_MANAGER,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_STATIC_STRINGS));

  /**
   * UDisksClient:manager:
   *
   * The #UDisksManager interface on the well-known
   * <literal>/org/freedesktop/UDisks2/Manager</literal> object
   */
  g_object_class_install_property (gobject_class,
                                   PROP_MANAGER,
                                   g_param_spec_object ("manager",
                                                        "Manager",
                                                        "The UDisksManager",
                                                        UDISKS_TYPE_MANAGER,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_STATIC_STRINGS));

  /**
   * UDisksClient:bus-connection:
   *
   * The #GDBusConnection used to create the #UDisksClient:object-manager, or
   * %NULL if one wasn't specified during construction of the #UDisksClient (in
   * which case a system bus connection is used).
   *
   * Since: 2.9.0
   */
  g_object_class_install_property (gobject_class,
                                   PROP_BUS_CONNECTION,
                                   g_param_spec_object ("bus-connection",
                                                        "Bus Connection",
                                                        "The D-Bus connection with which to talk to udisksd",
                                                        G_TYPE_DBUS_CONNECTION,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  /**
   * UDisksClient::changed:
   * @client: A #UDisksClient.
   *
   * This signal is emitted either when an object or interface is
   * added or removed a when property has changed. Additionally,
   * multiple received signals are coalesced into a single signal that
   * is rate-limited to fire at most every 100ms.
   *
   * Note that calling udisks_client_settle() will cause this signal
   * to fire if any changes are outstanding.
   *
   * For greater detail, connect to the
   * #GDBusObjectManager::object-added,
   * #GDBusObjectManager::object-removed,
   * #GDBusObjectManager::interface-added,
   * #GDBusObjectManager::interface-removed,
   * #GDBusObjectManagerClient::interface-proxy-properties-changed and
   * signals on the #UDisksClient:object-manager object.
   */
  signals[CHANGED_SIGNAL] = g_signal_new ("changed",
                                          G_OBJECT_CLASS_TYPE (klass),
                                          G_SIGNAL_RUN_LAST,
                                          0, /* G_STRUCT_OFFSET */
                                          NULL, /* accu */
                                          NULL, /* accu data */
                                          g_cclosure_marshal_generic,
                                          G_TYPE_NONE,
                                          0);

}

/**
 * udisks_client_new:
 * @cancellable: A #GCancellable or %NULL.
 * @callback: Function that will be called when the result is ready.
 * @user_data: Data to pass to @callback.
 *
 * Asynchronously gets a #UDisksClient. When the operation is
 * finished, @callback will be invoked in the <link
 * linkend="g-main-context-push-thread-default">thread-default main
 * loop</link> of the thread you are calling this method from.
 */
void
udisks_client_new (GCancellable        *cancellable,
                   GAsyncReadyCallback  callback,
                   gpointer             user_data)
{
  g_async_initable_new_async (UDISKS_TYPE_CLIENT,
                              G_PRIORITY_DEFAULT,
                              cancellable,
                              callback,
                              user_data,
                              NULL);
}

/**
 * udisks_client_new_for_connection:
 * @connection: (nullable): a #GDBusConnection. If %NULL, a system bus
 *   connection will be used.
 * @cancellable: A #GCancellable or %NULL.
 * @callback: Function that will be called when the result is ready.
 * @user_data: Data to pass to @callback.
 *
 * Like #udisks_client_new() but takes a #GDBusConnection which will be used
 * when connecting to the UDisks daemon over D-Bus. This is useful for unit
 * tests which may want to use #GTestDBus to create a D-Bus daemon on the
 * session bus.
 *
 * Since: 2.9.0
 */
void
udisks_client_new_for_connection (GDBusConnection     *connection,
                                  GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data)
{
  g_return_if_fail (connection == NULL || G_IS_DBUS_CONNECTION (connection));

  g_async_initable_new_async (UDISKS_TYPE_CLIENT,
                              G_PRIORITY_DEFAULT,
                              cancellable,
                              callback,
                              user_data,
                              "bus-connection",
                              connection,
                              NULL);
}

static UDisksClient *
_udisks_client_new_finish (GAsyncResult  *res,
                           GError       **error)
{
  GObject *ret;
  GObject *source_object;
  source_object = g_async_result_get_source_object (res);
  ret = g_async_initable_new_finish (G_ASYNC_INITABLE (source_object), res, error);
  g_object_unref (source_object);
  if (ret != NULL)
    return UDISKS_CLIENT (ret);
  else
    return NULL;
}

/**
 * udisks_client_new_finish:
 * @res: A #GAsyncResult.
 * @error: Return location for error or %NULL.
 *
 * Finishes an operation started with udisks_client_new().
 *
 * Returns: (transfer full): A #UDisksClient or %NULL if @error is set. Free
 * with g_object_unref() when done with it.
 */
UDisksClient *
udisks_client_new_finish (GAsyncResult  *res,
                          GError       **error)
{
  return _udisks_client_new_finish (res, error);
}

/**
 * udisks_client_new_for_connection_finish:
 * @res: A #GAsyncResult.
 * @error: Return location for error or %NULL.
 *
 * Finishes an operation started with udisks_client_new_for_connection().
 *
 * Returns: (transfer full): A #UDisksClient or %NULL if @error is set. Free
 * with g_object_unref() when done with it.
 *
 * Since: 2.9.0
 */
UDisksClient *
udisks_client_new_for_connection_finish (GAsyncResult  *res,
                                         GError       **error)
{
  return _udisks_client_new_finish (res, error);
}

/**
 * udisks_client_new_sync:
 * @cancellable: (allow-none): A #GCancellable or %NULL.
 * @error: (allow-none): Return location for error or %NULL.
 *
 * Synchronously gets a #UDisksClient for the local system.
 *
 * Returns: (transfer full): A #UDisksClient or %NULL if @error is set. Free
 * with g_object_unref() when done with it.
 */
UDisksClient *
udisks_client_new_sync (GCancellable  *cancellable,
                        GError       **error)
{
  GInitable *ret;
  ret = g_initable_new (UDISKS_TYPE_CLIENT,
                        cancellable,
                        error,
                        NULL);
  if (ret != NULL)
    return UDISKS_CLIENT (ret);
  else
    return NULL;
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
initable_init (GInitable     *initable,
               GCancellable  *cancellable,
               GError       **error)
{
  UDisksClient *client = UDISKS_CLIENT (initable);
  gboolean ret;
  GList *objects, *l;
  GList *interfaces, *ll;
  GDBusConnection *bus_connection;

  ret = FALSE;

  /* Lock here so this method is idempotent, which will be useful in case
   * #UDisksClient is ever made a singleton (though care would have to be taken
   * to return a new instance when a #GDBusConnection was given by the caller).
   */
  G_LOCK (init_lock);
  if (client->is_initialized)
    {
      if (client->object_manager != NULL)
        ret = TRUE;
      else
        g_assert (client->initialization_error != NULL);
      goto out;
    }
  g_assert (client->initialization_error == NULL);

  client->context = g_main_context_get_thread_default ();
  if (client->context != NULL)
    g_main_context_ref (client->context);

  if (client->bus_connection == NULL)
    {
      bus_connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, cancellable, &client->initialization_error);
      if (bus_connection == NULL)
        goto out;
    }
  else
    {
      bus_connection = g_object_ref (client->bus_connection);
    }

  client->object_manager = udisks_object_manager_client_new_sync (bus_connection,
                                                                  G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
                                                                  "org.freedesktop.UDisks2",
                                                                  "/org/freedesktop/UDisks2",
                                                                  cancellable,
                                                                  &client->initialization_error);
  g_clear_object (&bus_connection);
  if (client->object_manager == NULL)
    goto out;

  /* init all proxies */
  objects = g_dbus_object_manager_get_objects (client->object_manager);
  for (l = objects; l != NULL; l = l->next)
    {
      interfaces = g_dbus_object_get_interfaces (G_DBUS_OBJECT (l->data));
      for (ll = interfaces; ll != NULL; ll = ll->next)
        {
          init_interface_proxy (client, G_DBUS_PROXY (ll->data));
        }
      g_list_free_full (interfaces, g_object_unref);
    }
  g_list_free_full (objects, g_object_unref);

  g_signal_connect (client->object_manager,
                    "object-added",
                    G_CALLBACK (on_object_added),
                    client);
  g_signal_connect (client->object_manager,
                    "object-removed",
                    G_CALLBACK (on_object_removed),
                    client);
  g_signal_connect (client->object_manager,
                    "interface-added",
                    G_CALLBACK (on_interface_added),
                    client);
  g_signal_connect (client->object_manager,
                    "interface-removed",
                    G_CALLBACK (on_interface_removed),
                    client);
  g_signal_connect (client->object_manager,
                    "interface-proxy-properties-changed",
                    G_CALLBACK (on_interface_proxy_properties_changed),
                    client);

  ret = TRUE;

out:
  client->is_initialized = TRUE;
  if (!ret)
    {
      g_assert (client->initialization_error != NULL);
      g_propagate_error (error, g_error_copy (client->initialization_error));
    }
  G_UNLOCK (init_lock);
  return ret;
}

static void
initable_iface_init (GInitableIface      *initable_iface)
{
  initable_iface->init = initable_init;
}

static void
async_initable_iface_init (GAsyncInitableIface *async_initable_iface)
{
  /* Use default implementation (e.g. run GInitable code in a thread) */
}

/**
 * udisks_client_get_object_manager:
 * @client: A #UDisksClient.
 *
 * Gets the #GDBusObjectManager used by @client.
 *
 * Returns: (transfer none): A #GDBusObjectManager. Do not free, the
 * instance is owned by @client.
 */
GDBusObjectManager *
udisks_client_get_object_manager (UDisksClient        *client)
{
  g_return_val_if_fail (UDISKS_IS_CLIENT (client), NULL);
  return client->object_manager;
}

/**
 * udisks_client_get_manager:
 * @client: A #UDisksClient.
 *
 * Gets the #UDisksManager interface on the well-known
 * <literal>/org/freedesktop/UDisks2/Manager</literal> object.
 *
 * Returns: (transfer none): A #UDisksManager or %NULL if the udisksd
 * daemon is not currently running. Do not free, the instance is owned
 * by @client.
 */
UDisksManager *
udisks_client_get_manager (UDisksClient *client)
{
  UDisksManager *ret = NULL;
  GDBusObject *obj;

  g_return_val_if_fail (UDISKS_IS_CLIENT (client), NULL);

  obj = g_dbus_object_manager_get_object (client->object_manager, "/org/freedesktop/UDisks2/Manager");
  if (obj == NULL)
    goto out;

  ret = udisks_object_peek_manager (UDISKS_OBJECT (obj));
  g_object_unref (obj);

 out:
  return ret;
}

/**
 * udisks_client_settle:
 * @client: A #UDisksClient.
 *
 * Blocks until all pending D-Bus messages have been delivered. Also
 * emits the (rate-limited) #UDisksClient::changed signal if changes
 * are currently pending.
 *
 * This is useful in two situations: 1. when using synchronous method
 * calls since e.g. D-Bus signals received while waiting for the reply
 * are queued up and dispatched after the synchronous call ends; and
 * 2. when using asynchronous calls where the return value references
 * a newly created object (such as the <link
 * linkend="gdbus-method-org-freedesktop-UDisks2-Manager.LoopSetup">Manager.LoopSetup()</link> method).
 */
void
udisks_client_settle (UDisksClient *client)
{
  while (g_main_context_iteration (client->context, FALSE /* may_block */))
    ;
  /* TODO: careful if on different thread... */
  maybe_emit_changed_now (client);
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_client_get_object:
 * @client: A #UDisksClient.
 * @object_path: Object path.
 *
 * Convenience function for looking up an #UDisksObject for @object_path.
 *
 * Returns: (transfer full): A #UDisksObject corresponding to
 * @object_path or %NULL if not found. The returned object must be
 * freed with g_object_unref().
 */
UDisksObject *
udisks_client_get_object (UDisksClient  *client,
                          const gchar   *object_path)
{
  g_return_val_if_fail (UDISKS_IS_CLIENT (client), NULL);
  return (UDisksObject *) g_dbus_object_manager_get_object (client->object_manager, object_path);
}

/**
 * udisks_client_peek_object:
 * @client: A #UDisksClient.
 * @object_path: Object path.
 *
 * Like udisks_client_get_object() but doesn't increase the reference
 * count on the returned #UDisksObject.
 *
 * <warning>The returned object is only valid until removed so it is only safe to use this function on the thread where @client was constructed. Use udisks_client_get_object() if on another thread.</warning>
 *
 * Returns: (transfer none): A #UDisksObject corresponding to
 * @object_path or %NULL if not found.
 */
UDisksObject *
udisks_client_peek_object (UDisksClient  *client,
                           const gchar   *object_path)
{
  UDisksObject *ret;
  ret = udisks_client_get_object (client, object_path);
  if (ret != NULL)
    g_object_unref (ret);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_client_get_block_for_label:
 * @client: A #UDisksClient.
 * @label: The label.
 *
 * Gets all the #UDisksBlock instances with the given label, if any.
 *
 * Returns: (transfer full) (element-type UDisksBlock): A list of #UDisksBlock instances. The
 *   returned list should be freed with g_list_free() after each
 *   element has been freed with g_object_unref().
 */
GList *
udisks_client_get_block_for_label (UDisksClient        *client,
                                   const gchar         *label)
{
  GList *ret = NULL;
  GList *l, *object_proxies = NULL;

  g_return_val_if_fail (UDISKS_IS_CLIENT (client), NULL);
  g_return_val_if_fail (label != NULL, NULL);

  object_proxies = g_dbus_object_manager_get_objects (client->object_manager);
  for (l = object_proxies; l != NULL; l = l->next)
    {
      UDisksObject *object = UDISKS_OBJECT (l->data);
      UDisksBlock *block;

      block = udisks_object_get_block (object);
      if (block == NULL)
        continue;

      if (g_strcmp0 (udisks_block_get_id_label (block), label) == 0)
        ret = g_list_prepend (ret, block);
      else
        g_object_unref (block);
    }

  g_list_free_full (object_proxies, g_object_unref);
  ret = g_list_reverse (ret);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_client_get_block_for_uuid:
 * @client: A #UDisksClient.
 * @uuid: The uuid.
 *
 * Gets all the #UDisksBlock instances with the given uuid, if any.
 *
 * Returns: (transfer full) (element-type UDisksBlock): A list of #UDisksBlock instances. The
 *   returned list should be freed with g_list_free() after each
 *   element has been freed with g_object_unref().
 */
GList *
udisks_client_get_block_for_uuid (UDisksClient        *client,
                                  const gchar         *uuid)
{
  GList *ret = NULL;
  GList *l, *object_proxies = NULL;

  g_return_val_if_fail (UDISKS_IS_CLIENT (client), NULL);
  g_return_val_if_fail (uuid != NULL, NULL);

  object_proxies = g_dbus_object_manager_get_objects (client->object_manager);
  for (l = object_proxies; l != NULL; l = l->next)
    {
      UDisksObject *object = UDISKS_OBJECT (l->data);
      UDisksBlock *block;

      block = udisks_object_get_block (object);
      if (block == NULL)
        continue;

      if (g_strcmp0 (udisks_block_get_id_uuid (block), uuid) == 0)
        ret = g_list_prepend (ret, block);
      else
        g_object_unref (block);
    }

  g_list_free_full (object_proxies, g_object_unref);
  ret = g_list_reverse (ret);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

/* Note: The (type guint64) is a workaround for g-i mishandling dev_t, see
 * https://bugzilla.gnome.org/show_bug.cgi?id=584517 */

/**
 * udisks_client_get_block_for_dev:
 * @client: A #UDisksClient.
 * @block_device_number: (type guint64): A #dev_t to get a #UDisksBlock for.
 *
 * Gets the #UDisksBlock corresponding to @block_device_number, if any.
 *
 * Returns: (transfer full): A #UDisksBlock or %NULL if not found.
 */
UDisksBlock *
udisks_client_get_block_for_dev (UDisksClient *client,
                                 dev_t         block_device_number)
{
  UDisksBlock *ret = NULL;
  GList *l, *object_proxies = NULL;

  g_return_val_if_fail (UDISKS_IS_CLIENT (client), NULL);

  object_proxies = g_dbus_object_manager_get_objects (client->object_manager);
  for (l = object_proxies; l != NULL; l = l->next)
    {
      UDisksObject *object = UDISKS_OBJECT (l->data);
      UDisksBlock *block;

      block = udisks_object_get_block (object);
      if (block == NULL)
        continue;

      if (udisks_block_get_device_number (block) == block_device_number)
        {
          ret = block;
          goto out;
        }
      g_object_unref (block);
    }

 out:
  g_list_free_full (object_proxies, g_object_unref);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static int
compare_blocks_by_device (gconstpointer a,
                          gconstpointer b)
{
  UDisksBlock *block_a = udisks_object_get_block (UDISKS_OBJECT (a));
  UDisksBlock *block_b = udisks_object_get_block (UDISKS_OBJECT (b));

  g_assert (block_a != NULL);
  g_assert (block_b != NULL);

  return g_strcmp0 (udisks_block_get_device (block_a),
                    udisks_block_get_device (block_b));
}

static GList *
get_top_level_blocks_for_drive (UDisksClient *client,
                                const gchar  *drive_object_path)
{
  GList *ret;
  GList *l;
  GList *object_proxies;
  GDBusObjectManager *object_manager;

  object_manager = udisks_client_get_object_manager (client);
  object_proxies = g_dbus_object_manager_get_objects (object_manager);

  ret = NULL;
  for (l = object_proxies; l != NULL; l = l->next)
    {
      UDisksObject *object = UDISKS_OBJECT (l->data);
      UDisksBlock *block;
      UDisksPartition *partition;

      block = udisks_object_get_block (object);
      partition = udisks_object_peek_partition (object);
      if (block == NULL)
        continue;

      if (g_strcmp0 (udisks_block_get_drive (block), drive_object_path) == 0 &&
          partition == NULL)
        {
          ret = g_list_append (ret, g_object_ref (object));
        }
      g_object_unref (block);
    }
  ret = g_list_sort (ret, compare_blocks_by_device);
  g_list_free_full (object_proxies, g_object_unref);
  return ret;
}

/**
 * udisks_client_get_block_for_drive:
 * @client: A #UDisksClient.
 * @drive: A #UDisksDrive.
 * @get_physical: %TRUE to get a physical device, %FALSE to get the logical device.
 *
 * Gets a block device corresponding to @drive. The returned block
 * device, if any, is for the whole disk drive, e.g. a partition block
 * device is never returned.
 *
 * Set @get_physical to %TRUE if you need a block device that you can
 * send low-level SCSI commands with (for multipath, this returns one
 * of the physical paths). Set it to %FALSE if you need a block device
 * that you can read/write data with (for multipath, this returns the
 * mapped device).
 *
 * Returns: (transfer full): A #UDisksBlock or %NULL if the requested
 * kind of block device is not available - use g_object_unref() to
 * free the returned object.
 */
UDisksBlock *
udisks_client_get_block_for_drive (UDisksClient        *client,
                                   UDisksDrive         *drive,
                                   gboolean             get_physical)
{
  UDisksBlock *ret = NULL;
  GDBusObject *object;
  GList *blocks = NULL;
  GList *l;

  g_return_val_if_fail (UDISKS_IS_CLIENT (client), NULL);
  g_return_val_if_fail (UDISKS_IS_DRIVE (drive), NULL);

  object = g_dbus_interface_get_object (G_DBUS_INTERFACE (drive));
  if (object == NULL)
    goto out;

  blocks = get_top_level_blocks_for_drive (client, g_dbus_object_get_object_path (object));
  for (l = blocks; l != NULL; l = l->next)
    {
      UDisksBlock *block = udisks_object_peek_block (UDISKS_OBJECT (l->data));
      if (block != NULL)
        {
          /* TODO: actually look at @get_physical */
          ret = g_object_ref (block);
          goto out;
        }
    }

 out:
  g_list_free_full (blocks, g_object_unref);
  return ret;
}

/**
 * udisks_client_get_drive_for_block:
 * @client: A #UDisksClient.
 * @block: A #UDisksBlock.
 *
 * Gets the #UDisksDrive that @block belongs to, if any.
 *
 * Returns: (transfer full): A #UDisksDrive or %NULL if there is no
 * #UDisksDrive for @block - free the returned object with
 * g_object_unref().
 */
UDisksDrive *
udisks_client_get_drive_for_block (UDisksClient  *client,
                                   UDisksBlock   *block)
{
  UDisksDrive *ret = NULL;
  GDBusObject *object;

  g_return_val_if_fail (UDISKS_IS_CLIENT (client), NULL);
  g_return_val_if_fail (UDISKS_IS_BLOCK (block), NULL);

  object = g_dbus_object_manager_get_object (client->object_manager, udisks_block_get_drive (block));
  if (object != NULL)
    ret = udisks_object_get_drive (UDISKS_OBJECT (object));
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_client_get_mdraid_for_block:
 * @client: A #UDisksClient.
 * @block: A #UDisksBlock.
 *
 * Gets the #UDisksMDRaid that @block is the block device for, if any.
 *
 * Returns: (transfer full): A #UDisksMDRaid or %NULL if there is no
 *   #UDisksMDRaid for @block or @block is not a MD-RAID block
 *   device. Free the returned object with g_object_unref().
 *
 * Since: 2.1
 */
UDisksMDRaid *
udisks_client_get_mdraid_for_block (UDisksClient  *client,
                                    UDisksBlock   *block)
{
  UDisksMDRaid *ret = NULL;
  GDBusObject *object;

  g_return_val_if_fail (UDISKS_IS_CLIENT (client), NULL);
  g_return_val_if_fail (UDISKS_IS_BLOCK (block), NULL);

  object = g_dbus_object_manager_get_object (client->object_manager, udisks_block_get_mdraid (block));
  if (object != NULL)
    ret = udisks_object_get_mdraid (UDISKS_OBJECT (object));
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

typedef const gchar *
(*member_retrieve_cb) (UDisksBlock *object);

static GList *
_udisks_client_get_block_or_blocks_for_mdraid (UDisksClient *client,
                                               UDisksMDRaid *raid,
                                               member_retrieve_cb member_get,
                                               gboolean only_first_one,
                                               gboolean skip_partitions)
{
  GList *ret = NULL;
  GList *l, *object_proxies = NULL;
  GDBusObject *raid_object;
  const gchar *raid_objpath;

  g_return_val_if_fail (UDISKS_IS_CLIENT (client), NULL);
  g_return_val_if_fail (UDISKS_IS_MDRAID (raid), NULL);

  raid_object = g_dbus_interface_get_object (G_DBUS_INTERFACE (raid));
  if (raid_object == NULL)
    goto out;

  raid_objpath = g_dbus_object_get_object_path (raid_object);

  object_proxies = g_dbus_object_manager_get_objects (client->object_manager);
  for (l = object_proxies; l != NULL; l = l->next)
    {
      UDisksObject *object = UDISKS_OBJECT (l->data);
      UDisksBlock *block;

      block = udisks_object_get_block (object);
      if (block == NULL)
        continue;

      /* ignore partitions */
      if (skip_partitions)
        {
          if (udisks_object_peek_partition (object) != NULL)
            {
              g_object_unref (block);
              continue;
            }
        }

      if (g_strcmp0 (member_get (block), raid_objpath) == 0)
        {
          ret = g_list_prepend (ret, block);

          if (only_first_one)
            goto out;
        }
      else
        {
          g_object_unref (block);
        }
    }

 out:
  g_list_free_full (object_proxies, g_object_unref);
  return ret;
}

/**
 * udisks_client_get_block_for_mdraid:
 * @client: A #UDisksClient.
 * @raid: A #UDisksMDRaid.
 *
 * Gets the RAID device (e.g. <filename>/dev/md0</filename>) for @raid.
 *
 * In the case of a <ulink
 * url="http://en.wikipedia.org/wiki/Split-brain_(computing)">split-brain
 * syndrome</ulink>, it is undefined which RAID device is
 * returned. For example this can happen if
 * <filename>/dev/sda</filename> and <filename>/dev/sdb</filename> are
 * components of a two-disk RAID-1 and <filename>/dev/md0</filename>
 * and <filename>/dev/md1</filename> are two degraded arrays, each one
 * using exactly one of the two devices. Use
 * udisks_client_get_all_blocks_for_mdraid() to get all RAID devices.
 *
 * Returns: (transfer full): A #UDisksBlock or %NULL if no RAID device is running.
 *
 * Since: 2.1
 */
UDisksBlock *
udisks_client_get_block_for_mdraid (UDisksClient *client,
                                    UDisksMDRaid *raid)
{
  GList *b_list = NULL;
  UDisksBlock *ret = NULL;

  b_list = _udisks_client_get_block_or_blocks_for_mdraid (client,
                                                          raid,
                                                          udisks_block_get_mdraid,
                                                          TRUE,   /* Retrieve first one */
                                                          TRUE);  /* Skip partitions */
  if (b_list)
   {
      ret = (g_list_first (b_list))->data;
      g_list_free (b_list);
   }
  return ret;
}

/**
 * udisks_client_get_all_blocks_for_mdraid:
 * @client: A #UDisksClient.
 * @raid: A #UDisksMDRaid.
 *
 * Gets all RAID devices (e.g. <filename>/dev/md0</filename> and <filename>/dev/md1</filename>) for @raid.
 *
 * This is usually only useful in <ulink
 * url="http://en.wikipedia.org/wiki/Split-brain_(computing)">split-brain
 * situations</ulink> — see udisks_client_get_block_for_mdraid() for
 * an example — and is normally used only to convey the problem in an
 * user interface.
 *
 * Returns: (transfer full) (element-type UDisksBlock): A list of #UDisksBlock instances. The
 *   returned list should be freed with g_list_free() after each
 *   element has been freed with g_object_unref().
 *
 * Since: 2.1
 */
GList *
udisks_client_get_all_blocks_for_mdraid (UDisksClient *client,
                                         UDisksMDRaid *raid)
{
  return g_list_reverse (_udisks_client_get_block_or_blocks_for_mdraid (client,
                                                                        raid,
                                                                        udisks_block_get_mdraid,
                                                                        FALSE,    /* Retrieve all */
                                                                        TRUE));   /* Skip partitions */
}

/**
 * udisks_client_get_members_for_mdraid:
 * @client: A #UDisksClient.
 * @raid: A #UDisksMDRaid.
 *
 * Gets the physical block devices that are part of @raid.
 *
 * Returns: (transfer full) (element-type UDisksBlock): A list of #UDisksBlock instances. The
 *   returned list should be freed with g_list_free() after each
 *   element has been freed with g_object_unref().
 *
 * Since: 2.1
 */
GList *
udisks_client_get_members_for_mdraid (UDisksClient *client,
                                      UDisksMDRaid *raid)
{
  return _udisks_client_get_block_or_blocks_for_mdraid (client,
                                                        raid,
                                                        udisks_block_get_mdraid_member,
                                                        FALSE,    /* Retrieve all */
                                                        FALSE);   /* Don't skip partitions */
}
/* ---------------------------------------------------------------------------------------------------- */

static void
add_item (gchar **items_str,
          const gchar *item)
{
  gchar *orig = *items_str;
  if (*items_str == NULL)
    {
      *items_str = g_strdup (item);
      g_free (orig);
    }
  else
    {
      *items_str = g_strdup_printf ("%s, %s", orig, item);
      g_free (orig);
    }
}

/**
 * udisks_client_get_partition_info:
 * @client: A #UDisksClient.
 * @partition: # #UDisksPartition.
 *
 * Gets information about @partition that is suitable to present in an
 * user interface in a single line of text.
 *
 * The returned string is localized and includes things like the
 * partition type, flags (if any) and name (if any).
 *
 * Returns: (transfer full): A string that should be freed with g_free().
 */
gchar *
udisks_client_get_partition_info (UDisksClient    *client,
                                  UDisksPartition *partition)
{
  gchar *ret = NULL;
  const gchar *type_str = NULL;
  gchar *flags_str = NULL;
  UDisksPartitionTable *table = NULL;
  guint64 flags;

  g_return_val_if_fail (UDISKS_IS_CLIENT (client), NULL);
  g_return_val_if_fail (UDISKS_IS_PARTITION (partition), NULL);

  table = udisks_client_get_partition_table (client, partition);
  if (table == NULL)
    goto out;

  flags = udisks_partition_get_flags (partition);
  if (g_strcmp0 (udisks_partition_table_get_type_ (table), "dos") == 0)
    {
      if (flags & 0x80)
        {
          /* Translators: Corresponds to the DOS/Master-Boot-Record "bootable" flag for a partition */
          add_item (&flags_str, C_("dos-part-flag", "Bootable"));
        }
    }
  else if (g_strcmp0 (udisks_partition_table_get_type_ (table), "gpt") == 0)
    {
      if (flags & (1ULL<<0))
        {
          /* Translators: Corresponds to the GPT "system" flag for a partition,
           * see http://en.wikipedia.org/wiki/GUID_Partition_Table
           */
          add_item (&flags_str, C_("gpt-part-flag", "System"));
        }
      if (flags & (1ULL<<2))
        {
          /* Translators: Corresponds to the GPT "legacy bios bootable" flag for a partition,
           * see http://en.wikipedia.org/wiki/GUID_Partition_Table
           */
          add_item (&flags_str, C_("gpt-part-flag", "Legacy BIOS Bootable"));
        }
      if (flags & (1ULL<<60))
        {
          /* Translators: Corresponds to the GPT "read-only" flag for a partition,
           * see http://en.wikipedia.org/wiki/GUID_Partition_Table
           */
          add_item (&flags_str, C_("gpt-part-flag", "Read-only"));
        }
      if (flags & (1ULL<<62))
        {
          /* Translators: Corresponds to the GPT "hidden" flag for a partition,
           * see http://en.wikipedia.org/wiki/GUID_Partition_Table
           */
          add_item (&flags_str, C_("gpt-part-flag", "Hidden"));
        }
      if (flags & (1ULL<<63))
        {
          /* Translators: Corresponds to the GPT "no automount" flag for a partition,
           * see http://en.wikipedia.org/wiki/GUID_Partition_Table
           */
          add_item (&flags_str, C_("gpt-part-flag", "No Automount"));
        }
    }

  type_str = udisks_client_get_partition_type_for_display (client,
                                                           udisks_partition_table_get_type_ (table),
                                                           udisks_partition_get_type_ (partition));
  if (type_str == NULL)
    type_str = udisks_partition_get_type_ (partition);

  if (flags_str != NULL)
    {
      /* Translators: Partition info. First %s is the type, second %s is a list of flags */
      ret = g_strdup_printf (C_("partition-info", "%s (%s)"), type_str, flags_str);
    }
  else
    {
      ret = g_strdup (type_str);
    }

  if (ret == NULL || strlen (ret) == 0)
    {
      g_free (ret);
      /* Translators: The Partition info when unknown */
      ret = g_strdup (C_("partition-info", "Unknown"));
    }

  g_free (flags_str);
  g_object_unref (table);
 out:
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_client_get_cleartext_block:
 * @client: A #UDisksClient.
 * @block: A #UDisksBlock.
 *
 * If @block is an unlocked encrypted device, gets the cleartext device.
 *
 * Returns: (transfer full): A #UDisksBlock or %NULL. Free with
 * g_object_unref() when done with it.
 */
UDisksBlock *
udisks_client_get_cleartext_block (UDisksClient  *client,
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
  objects = g_dbus_object_manager_get_objects (client->object_manager);
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

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_client_get_drive_siblings:
 * @client: A #UDisksClient
 * @drive: A #UDisksDrive.
 *
 * Gets all siblings for @drive.
 *
 * Returns: (transfer full) (element-type UDisksDrive): A list of #UDisksDrive instances. The
 *   returned list should be freed with g_list_free() after each element has been
 *   freed with g_object_unref().
 *
 * Since: 2.1
 */
GList *
udisks_client_get_drive_siblings  (UDisksClient  *client,
                                   UDisksDrive   *drive)
{
  GList *ret = NULL;
  const gchar *sibling_id = NULL;
  GList *l, *object_proxies = NULL;

  g_return_val_if_fail (UDISKS_IS_CLIENT (client), NULL);
  g_return_val_if_fail (UDISKS_IS_DRIVE (drive), NULL);

  sibling_id = udisks_drive_get_sibling_id (drive);
  if (sibling_id == NULL || strlen (sibling_id) == 0)
    goto out;

  object_proxies = g_dbus_object_manager_get_objects (client->object_manager);
  for (l = object_proxies; l != NULL; l = l->next)
    {
      UDisksObject *object = UDISKS_OBJECT (l->data);
      UDisksDrive *iter_drive;

      iter_drive = udisks_object_get_drive (object);
      if (iter_drive == NULL)
        continue;

      if (iter_drive != drive &&
          g_strcmp0 (udisks_drive_get_sibling_id (iter_drive), sibling_id) == 0)
        {
          ret = g_list_prepend (ret, g_object_ref (iter_drive));
        }

      g_object_unref (iter_drive);
    }
  ret = g_list_reverse (ret);
 out:
  g_list_free_full (object_proxies, g_object_unref);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_client_get_partitions:
 * @client: A #UDisksClient.
 * @table: A #UDisksPartitionTable.
 *
 * Gets all partitions of @table.
 *
 * Returns: (transfer full) (element-type UDisksPartition): A list of #UDisksPartition instances. The
 *   returned list should be freed with g_list_free() after each
 *   element has been freed with g_object_unref().
 */
GList *
udisks_client_get_partitions (UDisksClient         *client,
                              UDisksPartitionTable *table)
{
  GList *ret = NULL;
  GDBusObject *table_object;
  const gchar *table_object_path;
  GList *l, *object_proxies = NULL;

  g_return_val_if_fail (UDISKS_IS_CLIENT (client), NULL);
  g_return_val_if_fail (UDISKS_IS_PARTITION_TABLE (table), NULL);

  table_object = g_dbus_interface_get_object (G_DBUS_INTERFACE (table));
  if (table_object == NULL)
    goto out;
  table_object_path = g_dbus_object_get_object_path (table_object);

  object_proxies = g_dbus_object_manager_get_objects (client->object_manager);
  for (l = object_proxies; l != NULL; l = l->next)
    {
      UDisksObject *object = UDISKS_OBJECT (l->data);
      UDisksPartition *partition;

      partition = udisks_object_get_partition (object);
      if (partition == NULL)
        continue;

      if (g_strcmp0 (udisks_partition_get_table (partition), table_object_path) == 0)
        ret = g_list_prepend (ret, g_object_ref (partition));

      g_object_unref (partition);
    }
  ret = g_list_reverse (ret);
 out:
  g_list_free_full (object_proxies, g_object_unref);
  return ret;
}

/**
 * udisks_client_get_partition_table:
 * @client: A #UDisksClient.
 * @partition: A #UDisksPartition.
 *
 * Gets the #UDisksPartitionTable corresponding to @partition.
 *
 * Returns: (transfer full): A #UDisksPartitionTable or %NULL if there is no #UDisksPartitionTable for @partition.
 * Free with g_object_unref().
 */
UDisksPartitionTable *
udisks_client_get_partition_table (UDisksClient     *client,
                                   UDisksPartition  *partition)
{
  UDisksPartitionTable *ret = NULL;
  UDisksObject *object;

  g_return_val_if_fail (UDISKS_IS_CLIENT (client), NULL);
  g_return_val_if_fail (UDISKS_IS_PARTITION (partition), NULL);

  object = udisks_client_get_object (client, udisks_partition_get_table (partition));
  if (object == NULL)
    goto out;

  ret = udisks_object_get_partition_table (object);
  g_object_unref (object);

 out:
  return ret;
}

/**
 * udisks_client_get_loop_for_block:
 * @client: A #UDisksClient.
 * @block: A #UDisksBlock.
 *
 * Gets the corresponding loop interface for @block.
 *
 * This only works if @block itself is a loop device or a partition of
 * a loop device.
 *
 * Returns: (transfer full): A #UDisksLoop or %NULL. Free with g_object_unref().
 */
UDisksLoop *
udisks_client_get_loop_for_block (UDisksClient  *client,
                                  UDisksBlock   *block)
{
  UDisksPartition *partition;
  UDisksObject *object = NULL;
  UDisksLoop *ret = NULL;

  g_return_val_if_fail (UDISKS_IS_CLIENT (client), NULL);
  g_return_val_if_fail (UDISKS_IS_BLOCK (block), NULL);

  object = (UDisksObject *) g_dbus_interface_dup_object (G_DBUS_INTERFACE (block));
  if (object == NULL)
    goto out;

  ret = udisks_object_get_loop (object);
  if (ret != NULL)
    goto out;

  /* Could be we're a partition of a loop device */
  partition = udisks_object_get_partition (object);
  if (partition != NULL)
    {
      UDisksPartitionTable *partition_table;
      partition_table = udisks_client_get_partition_table (client, partition);
      if (partition_table != NULL)
        {
          UDisksObject *partition_table_object;
          partition_table_object = (UDisksObject *) g_dbus_interface_dup_object (G_DBUS_INTERFACE (partition_table));
          if (partition_table_object != NULL)
            {
              ret = udisks_object_get_loop (UDISKS_OBJECT (partition_table_object));
              g_object_unref (partition_table_object);
            }
          g_object_unref (partition_table);
        }
      g_object_unref (partition);
    }

 out:
  g_clear_object (&object);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_client_get_jobs_for_object:
 * @client: A #UDisksClient.
 * @object: A #UDisksObject.
 *
 * Gets all the #UDisksJob instances that reference @object, if any.
 *
 * Returns: (transfer full) (element-type UDisksJob): A list of #UDisksJob instances. The
 *   returned list should be freed with g_list_free() after each
 *   element has been freed with g_object_unref().
 */
GList *
udisks_client_get_jobs_for_object (UDisksClient  *client,
                                   UDisksObject  *object)
{
  GList *ret = NULL;
  const gchar *object_path;
  GList *l, *object_proxies = NULL;

  /* TODO: this is probably slow. Can optimize by maintaining a hash-table from object path to UDisksJob* */

  g_return_val_if_fail (UDISKS_IS_CLIENT (client), NULL);
  g_return_val_if_fail (UDISKS_IS_OBJECT (object), NULL);

  object_path = g_dbus_object_get_object_path (G_DBUS_OBJECT (object));

  object_proxies = g_dbus_object_manager_get_objects (client->object_manager);
  for (l = object_proxies; l != NULL; l = l->next)
    {
      UDisksObject *job_object = UDISKS_OBJECT (l->data);
      UDisksJob *job;

      job = udisks_object_get_job (job_object);
      if (job != NULL)
        {
          const gchar *const *object_paths;
          guint n;
          object_paths = udisks_job_get_objects (job);
          for (n = 0; object_paths != NULL && object_paths[n] != NULL; n++)
            {
              if (g_strcmp0 (object_paths[n], object_path) == 0)
                ret = g_list_prepend (ret, g_object_ref (job));
            }
          g_object_unref (job);
        }
    }
  ret = g_list_reverse (ret);

  g_list_free_full (object_proxies, g_object_unref);
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static void
maybe_emit_changed_now (UDisksClient *client)
{
  if (client->changed_timeout_source == NULL)
    goto out;

  g_source_destroy (client->changed_timeout_source);
  client->changed_timeout_source = NULL;

  g_signal_emit (client, signals[CHANGED_SIGNAL], 0);

 out:
  ;
}


static gboolean
on_changed_timeout (gpointer user_data)
{
  UDisksClient *client = UDISKS_CLIENT (user_data);
  client->changed_timeout_source = NULL;
  g_signal_emit (client, signals[CHANGED_SIGNAL], 0);
  return FALSE; /* remove source */
}

/**
 * udisks_client_queue_changed:
 * @client: A #UDisksClient.
 *
 * Queues up a #UDisksClient::changed signal and rate-limit it. See
 * the documentation for the #UDisksClient::changed property for more
 * information.
 *
 * Since: 2.1
 */
void
udisks_client_queue_changed (UDisksClient *client)
{
  g_return_if_fail (UDISKS_IS_CLIENT (client));

  if (client->changed_timeout_source != NULL)
    goto out;

  client->changed_timeout_source = g_timeout_source_new (100);
  g_source_set_callback (client->changed_timeout_source,
                         (GSourceFunc) on_changed_timeout,
                         client,
                         NULL); /* destroy notify */
  g_source_attach (client->changed_timeout_source, client->context);
  g_source_unref (client->changed_timeout_source);

 out:
  ;
}

static void
on_object_added (GDBusObjectManager  *manager,
                 GDBusObject         *object,
                 gpointer             user_data)
{
  UDisksClient *client = UDISKS_CLIENT (user_data);
  GList *interfaces, *l;

  interfaces = g_dbus_object_get_interfaces (object);
  for (l = interfaces; l != NULL; l = l->next)
    {
      init_interface_proxy (client, G_DBUS_PROXY (l->data));
    }
  g_list_free_full (interfaces, g_object_unref);

  udisks_client_queue_changed (client);
}

static void
on_object_removed (GDBusObjectManager  *manager,
                   GDBusObject         *object,
                   gpointer             user_data)
{
  UDisksClient *client = UDISKS_CLIENT (user_data);
  udisks_client_queue_changed (client);
}

static void
init_interface_proxy (UDisksClient *client,
                      GDBusProxy   *proxy)
{
  /* disable method timeouts */
  g_dbus_proxy_set_default_timeout (proxy, G_MAXINT);
}

static void
on_interface_added (GDBusObjectManager  *manager,
                    GDBusObject         *object,
                    GDBusInterface      *interface,
                    gpointer             user_data)
{
  UDisksClient *client = UDISKS_CLIENT (user_data);

  init_interface_proxy (client, G_DBUS_PROXY (interface));

  udisks_client_queue_changed (client);
}

static void
on_interface_removed (GDBusObjectManager  *manager,
                      GDBusObject         *object,
                      GDBusInterface      *interface,
                      gpointer             user_data)
{
  UDisksClient *client = UDISKS_CLIENT (user_data);
  udisks_client_queue_changed (client);
}

static void
on_interface_proxy_properties_changed (GDBusObjectManagerClient   *manager,
                                       GDBusObjectProxy           *object_proxy,
                                       GDBusProxy                 *interface_proxy,
                                       GVariant                   *changed_properties,
                                       const gchar *const         *invalidated_properties,
                                       gpointer                    user_data)
{
  UDisksClient *client = UDISKS_CLIENT (user_data);
  UDisksClientClass *client_class = UDISKS_CLIENT_GET_CLASS (client);

  GVariantIter iter;
  gchar *property_name = NULL;

  /* never emit the change signal for Job objects */
  if (g_strcmp0 (g_dbus_proxy_get_interface_name (interface_proxy), "org.freedesktop.UDisks2.Drive.Job") == 0)
    return;

  g_variant_iter_init (&iter, changed_properties);
  while (g_variant_iter_next (&iter, "{&sv}", &property_name, NULL))
    {
      if (! g_hash_table_contains (client_class->changed_blacklist, property_name))
        {
          /* one of the properties is not on the blacklist -> emit change signal */
          udisks_client_queue_changed (client);
          return;
        }
    }
}

/* ---------------------------------------------------------------------------------------------------- */

#define KILOBYTE_FACTOR 1000.0
#define MEGABYTE_FACTOR (1000.0 * 1000.0)
#define GIGABYTE_FACTOR (1000.0 * 1000.0 * 1000.0)
#define TERABYTE_FACTOR (1000.0 * 1000.0 * 1000.0 * 1000.0)

#define KIBIBYTE_FACTOR 1024.0
#define MEBIBYTE_FACTOR (1024.0 * 1024.0)
#define GIBIBYTE_FACTOR (1024.0 * 1024.0 * 1024.0)
#define TEBIBYTE_FACTOR (1024.0 * 1024.0 * 1024.0 * 10242.0)

static char *
get_pow2_size (guint64 size)
{
  gchar *str;
  gdouble displayed_size;
  const gchar *unit;
  gint digits;

  if (size < MEBIBYTE_FACTOR)
    {
      displayed_size = (double) size / KIBIBYTE_FACTOR;
      /* Translators: SI prefix and standard unit symbol, translate cautiously (or not at all) */
      unit = C_("byte-size-pow2", "KiB");
    }
  else if (size < GIBIBYTE_FACTOR)
    {
      displayed_size = (double) size / MEBIBYTE_FACTOR;
      /* Translators: SI prefix and standard unit symbol, translate cautiously (or not at all) */
      unit = C_("byte-size-pow2", "MiB");
    }
  else if (size < TEBIBYTE_FACTOR)
    {
      displayed_size = (double) size / GIBIBYTE_FACTOR;
      /* Translators: SI prefix and standard unit symbol, translate cautiously (or not at all) */
      unit = C_("byte-size-pow2", "GiB");
    }
  else
    {
      displayed_size = (double) size / TEBIBYTE_FACTOR;
      /* Translators: SI prefix and standard unit symbol, translate cautiously (or not at all) */
      unit = C_("byte-size-pow2", "TiB");
    }

  if (displayed_size < 10.0)
    digits = 1;
  else
    digits = 0;

  str = g_strdup_printf ("%.*f %s", digits, displayed_size, unit);

  return str;
}

static char *
get_pow10_size (guint64 size)
{
  gchar *str;
  gdouble displayed_size;
  const gchar *unit;
  gint digits;

  if (size < MEGABYTE_FACTOR)
    {
      displayed_size = (double) size / KILOBYTE_FACTOR;
      /* Translators: SI prefix and standard unit symbol, translate cautiously (or not at all) */
      unit = C_("byte-size-pow10", "KB");
    }
  else if (size < GIGABYTE_FACTOR)
    {
      displayed_size = (double) size / MEGABYTE_FACTOR;
      /* Translators: SI prefix and standard unit symbol, translate cautiously (or not at all) */
      unit = C_("byte-size-pow10", "MB");
    }
  else if (size < TERABYTE_FACTOR)
    {
      displayed_size = (double) size / GIGABYTE_FACTOR;
      /* Translators: SI prefix and standard unit symbol, translate cautiously (or not at all) */
      unit = C_("byte-size-pow10", "GB");
    }
  else
    {
      displayed_size = (double) size / TERABYTE_FACTOR;
      /* Translators: SI prefix and standard unit symbol, translate cautiously (or not at all) */
      unit = C_("byte-size-pow10", "TB");
    }

  if (displayed_size < 10.0)
    digits = 1;
  else
    digits = 0;

  str = g_strdup_printf ("%.*f %s", digits, displayed_size, unit);

  return str;
}

/**
 * udisks_client_get_size_for_display:
 * @client: A #UDisksClient.
 * @size: Size in bytes
 * @use_pow2: Whether power-of-two units should be used instead of power-of-ten units.
 * @long_string: Whether to produce a long string.
 *
 * Utility function to get a human-readable string that represents @size.
 *
 * Returns: (transfer full): A string that should be freed with g_free().
 */
gchar *
udisks_client_get_size_for_display (UDisksClient  *client,
                                    guint64        size,
                                    gboolean       use_pow2,
                                    gboolean       long_string)
{
  gchar *str;

  if (long_string)
    {
      gchar *size_str;
      size_str = g_strdup_printf ("%'" G_GUINT64_FORMAT, size);
      if (use_pow2)
        {
          gchar *pow2_str;
          pow2_str = get_pow2_size (size);
          /* Translators: The first %s is the size in power-of-2 units, e.g. '64 KiB'
           * the second %s is the size as a number e.g. '65,536' (always > 1)
           */
          str = g_strdup_printf (C_("byte-size-pow2", "%s (%s bytes)"), pow2_str, size_str);
          g_free (pow2_str);
        }
      else
        {
          gchar *pow10_str;
          pow10_str = get_pow10_size (size);
          /* Translators: The first %s is the size in power-of-10 units, e.g. '100 kB'
           * the second %s is the size as a number e.g. '100,000' (always > 1)
           */
          str = g_strdup_printf (C_("byte-size-pow10", "%s (%s bytes)"), pow10_str, size_str);
          g_free (pow10_str);
        }

      g_free (size_str);
    }
  else
    {
      if (use_pow2)
        {
          str = get_pow2_size (size);
        }
      else
        {
          str = get_pow10_size (size);
        }
    }
  return str;
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_client_get_media_compat_for_display:
 * @client: A #UDisksClient.
 * @media_compat: An array of media types.
 *
 * Gets a human-readable string of the media described by
 * @media_compat. The returned information is localized.
 *
 * Returns: (transfer full): A string that should be freed with g_free() or
 * %NULL if unknown.
 */
gchar *
udisks_client_get_media_compat_for_display (UDisksClient       *client,
                                            const gchar* const *media_compat)
{
  guint n;
  gboolean optical_cd;
  gboolean optical_dvd;
  gboolean optical_bd;
  gboolean optical_hddvd;
  GString *result;

  optical_cd = FALSE;
  optical_dvd = FALSE;
  optical_bd = FALSE;
  optical_hddvd = FALSE;

  result = g_string_new (NULL);

  for (n = 0; media_compat != NULL && media_compat[n] != NULL; n++)
    {
      const gchar *media_name;
      const gchar *media;

      media = media_compat[n];
      media_name = NULL;
      if (g_strcmp0 (media, "flash_cf") == 0)
        {
          /* Translators: This word is used to describe the media inserted into a device */
          media_name = C_("media", "CompactFlash");
        }
      else if (g_strcmp0 (media, "flash_ms") == 0)
        {
          /* Translators: This word is used to describe the media inserted into a device */
          media_name = C_("media", "MemoryStick");
        }
      else if (g_strcmp0 (media, "flash_sm") == 0)
        {
          /* Translators: This word is used to describe the media inserted into a device */
          media_name = C_("media", "SmartMedia");
        }
      else if (g_strcmp0 (media, "flash_sd") == 0)
        {
          /* Translators: This word is used to describe the media inserted into a device */
          media_name = C_("media", "SecureDigital");
        }
      else if (g_strcmp0 (media, "flash_sdhc") == 0)
        {
          /* Translators: This word is used to describe the media inserted into a device */
          media_name = C_("media", "SD High Capacity");
        }
      else if (g_strcmp0 (media, "floppy") == 0)
        {
          /* Translators: This word is used to describe the media inserted into a device */
          media_name = C_("media", "Floppy");
        }
      else if (g_strcmp0 (media, "floppy_zip") == 0)
        {
          /* Translators: This word is used to describe the media inserted into a device */
          media_name = C_("media", "Zip");
        }
      else if (g_strcmp0 (media, "floppy_jaz") == 0)
        {
          /* Translators: This word is used to describe the media inserted into a device */
          media_name = C_("media", "Jaz");
        }
      else if (g_str_has_prefix (media, "flash"))
        {
          /* Translators: This word is used to describe the media inserted into a device */
          media_name = C_("media", "Flash");
        }
      else if (g_str_has_prefix (media, "optical_cd"))
        {
          optical_cd = TRUE;
        }
      else if (g_str_has_prefix (media, "optical_dvd"))
        {
          optical_dvd = TRUE;
        }
      else if (g_str_has_prefix (media, "optical_bd"))
        {
          optical_bd = TRUE;
        }
      else if (g_str_has_prefix (media, "optical_hddvd"))
        {
          optical_hddvd = TRUE;
        }

      if (media_name != NULL)
        {
          if (result->len > 0)
            g_string_append_c (result, '/');
          g_string_append (result, media_name);
        }
    }

  if (optical_cd)
    {
      if (result->len > 0)
        g_string_append_c (result, '/');
      /* Translators: This word is used to describe the optical disc type, it may appear
       * in a slash-separated list e.g. 'CD/DVD/Blu-Ray'
       */
      g_string_append (result, C_("disc-type", "CD"));
    }
  if (optical_dvd)
    {
      if (result->len > 0)
        g_string_append_c (result, '/');
      /* Translators: This word is used to describe the optical disc type, it may appear
       * in a slash-separated list e.g. 'CD/DVD/Blu-Ray'
       */
      g_string_append (result, C_("disc-type", "DVD"));
    }
  if (optical_bd)
    {
      if (result->len > 0)
        g_string_append_c (result, '/');
      /* Translators: This word is used to describe the optical disc type, it may appear
       * in a slash-separated list e.g. 'CD/DVD/Blu-Ray'
       */
      g_string_append (result, C_("disc-type", "Blu-Ray"));
    }
  if (optical_hddvd)
    {
      if (result->len > 0)
        g_string_append_c (result, '/');
      /* Translators: This word is used to describe the optical disc type, it may appear
       * in a slash-separated list e.g. 'CD/DVD/Blu-Ray'
       */
      g_string_append (result, C_("disc-type", "HDDVD"));
    }

  if (result->len > 0)
    return g_string_free (result, FALSE);

  g_string_free (result, TRUE);
  return NULL;
}

/* ---------------------------------------------------------------------------------------------------- */

static const struct
{
  const gchar *usage;
  const gchar *type;
  const gchar *version;
  const gchar *long_name;
  const gchar *short_name;
} id_type[] =
{
  {"filesystem", "vfat",              "FAT12", NC_("fs-type", "FAT (12-bit version)"),              NC_("fs-type", "FAT")},
  {"filesystem", "vfat",              "FAT16", NC_("fs-type", "FAT (16-bit version)"),              NC_("fs-type", "FAT")},
  {"filesystem", "vfat",              "FAT32", NC_("fs-type", "FAT (32-bit version)"),              NC_("fs-type", "FAT")},
  {"filesystem", "vfat",              "*",     NC_("fs-type", "FAT (version %s)"),                  NC_("fs-type", "FAT")},
  {"filesystem", "vfat",              NULL,    NC_("fs-type", "FAT"),                               NC_("fs-type", "FAT")},
  {"filesystem", "ntfs",              "*",     NC_("fs-type", "NTFS (version %s)"),                 NC_("fs-type", "NTFS")},
  {"filesystem", "ntfs",              NULL,    NC_("fs-type", "NTFS"),                              NC_("fs-type", "NTFS")},
  {"filesystem", "hfs",               NULL,    NC_("fs-type", "HFS"),                               NC_("fs-type", "HFS")},
  {"filesystem", "hfsplus",           NULL,    NC_("fs-type", "HFS+"),                              NC_("fs-type", "HFS+")},
  {"filesystem", "apfs",              NULL,    NC_("fs-type", "APFS"),                              NC_("fs-type", "APFS")},
  {"filesystem", "ext2",              "*",     NC_("fs-type", "Ext2 (version %s)"),                 NC_("fs-type", "Ext2")},
  {"filesystem", "ext2",              NULL,    NC_("fs-type", "Ext2"),                              NC_("fs-type", "Ext2")},
  {"filesystem", "ext3",              "*",     NC_("fs-type", "Ext3 (version %s)"),                 NC_("fs-type", "Ext3")},
  {"filesystem", "ext3",              NULL,    NC_("fs-type", "Ext3"),                              NC_("fs-type", "Ext3")},
  {"filesystem", "ext4",              "*",     NC_("fs-type", "Ext4 (version %s)"),                 NC_("fs-type", "Ext4")},
  {"filesystem", "ext4",              NULL,    NC_("fs-type", "Ext4"),                              NC_("fs-type", "Ext4")},
  {"filesystem", "jdb",               "*",     NC_("fs-type", "Journal for Ext (version %s)"),      NC_("fs-type", "JDB")},
  {"filesystem", "jdb",               "*",     NC_("fs-type", "Journal for Ext"),                   NC_("fs-type", "JDB")},
  {"filesystem", "xfs",               "*",     NC_("fs-type", "XFS (version %s)"),                  NC_("fs-type", "XFS")},
  {"filesystem", "xfs",               NULL,    NC_("fs-type", "XFS"),                               NC_("fs-type", "XFS")},
  /* TODO: No ID_FS_VERSION yet for btrfs... */
  {"filesystem", "btrfs",             NULL,    NC_("fs-type", "Btrfs"),                             NC_("fs-type", "Btrfs")},
  {"filesystem", "iso9660",           "*",     NC_("fs-type", "ISO 9660 (version %s)"),             NC_("fs-type", "ISO9660")},
  {"filesystem", "iso9660",           NULL,    NC_("fs-type", "ISO 9660"),                          NC_("fs-type", "ISO9660")},
  {"filesystem", "udf",               "*",     NC_("fs-type", "UDF (version %s)"),                  NC_("fs-type", "UDF")},
  {"filesystem", "udf",               NULL,    NC_("fs-type", "UDF"),                               NC_("fs-type", "UDF")},
  {"filesystem", "exfat",             NULL,    NC_("fs-type", "exFAT"),                             NC_("fs-type", "exFAT")},
  {"filesystem", "exfat",             "*",     NC_("fs-type", "exFAT (version %s)"),                NC_("fs-type", "exFAT")},
  {"other",      "swap",              "*",     NC_("fs-type", "Swap (version %s)"),                 NC_("fs-type", "Swap")},
  {"other",      "swap",              NULL,    NC_("fs-type", "Swap"),                              NC_("fs-type", "Swap")},
  {"raid",       "LVM2_member",       "*",     NC_("fs-type", "LVM2 Physical Volume (%s)"),         NC_("fs-type", "LVM2 PV")},
  {"raid",       "LVM2_member",       NULL,    NC_("fs-type", "LVM2 Physical Volume"),              NC_("fs-type", "LVM2 PV")},
  {"raid",       "linux_raid_member", "*",     NC_("fs-type", "Linux RAID Member (version %s)"),    NC_("fs-type", "Linux RAID Member")},
  {"raid",       "linux_raid_member", NULL,    NC_("fs-type", "Linux RAID Member"),                 NC_("fs-type", "Linux RAID Member")},
  {"raid",       "zfs_member",        "*",     NC_("fs-type", "ZFS Device (ZPool version %s)"),     NC_("fs-type", "ZFS (v%s)")},
  {"raid",       "zfs_member",        NULL,    NC_("fs-type", "ZFS Device"),                        NC_("fs-type", "ZFS")},
  {"raid",       "isw_raid_member",   "*",     NC_("fs-type", "Intel Rapid Storage Technology enterprise RAID Member (version %s)"), NC_("fs-type", "Intel RSTe RAID Member (%s)")},
  {"raid",       "isw_raid_member",   NULL,    NC_("fs-type", "Intel Rapid Storage Technology enterprise RAID Member"),          NC_("fs-type", "Intel RSTe RAID Member")},
  {"crypto",     "crypto_unknown",    NULL,    NC_("fs-type", "Possibly Encrypted"),                NC_("fs-type", "Encrypted?")},
  {"crypto",     "crypto_LUKS",       "*",     NC_("fs-type", "LUKS Encryption (version %s)"),      NC_("fs-type", "LUKS")},
  {"crypto",     "crypto_LUKS",       NULL,    NC_("fs-type", "LUKS Encryption"),                   NC_("fs-type", "LUKS")},
  {"crypto",     "crypto_TCRYPT",     NULL,    NC_("fs-type", "VeraCrypt/TrueCrypt Encryption"),    NC_("fs-type", "VeraCrypt/TrueCrypt")},
  {"crypto",     "BitLocker",         NULL,    NC_("fs-type", "BitLocker Encryption"),              NC_("fs-type", "BitLocker")},
  {"filesystem", "VMFS",              "*",     NC_("fs-type", "VMFS (version %s)"),                 NC_("fs-type", "VMFS (v%s)")},
  {"filesystem", "VMFS",              NULL,    NC_("fs-type", "VMFS"),                              NC_("fs-type", "VMFS")},
  {"raid",       "VMFS_volume_member", "*",    NC_("fs-type", "VMFS Volume Member (version %s)"),   NC_("fs-type", "VMFS Member (v%s)")},
  {"raid",       "VMFS_volume_member", NULL,   NC_("fs-type", "VMFS Volume Member"),                NC_("fs-type", "VMFS Member")},
  {NULL, NULL, NULL, NULL}
};

/**
 * udisks_client_get_id_for_display:
 * @client: A #UDisksClient.
 * @usage: Usage id e.g. "filesystem" or "crypto".
 * @type: Type e.g. "ext4" or "crypto_LUKS"
 * @version: Version.
 * @long_string: Whether to produce a long string.
 *
 * Gets a human readable localized string for @usage, @type and @version.
 *
 * Returns: (transfer full): A string that should be freed with g_free().
 */
gchar *
udisks_client_get_id_for_display (UDisksClient *client,
                                  const gchar  *usage,
                                  const gchar  *type,
                                  const gchar  *version,
                                  gboolean      long_string)
{
  guint n;
  gchar *ret = NULL;

  if (usage == NULL || type == NULL || version == NULL)
    {
      g_critical ("Invalid device ID specification. Usage, type and version must not be NULL");
      goto out;
    }

  for (n = 0; id_type[n].usage != NULL; n++)
    {
      if (g_strcmp0 (id_type[n].usage, usage) == 0 &&
          g_strcmp0 (id_type[n].type, type) == 0)
        {
          if ((id_type[n].version == NULL && strlen (version) == 0))
            {
              if (long_string)
                ret = g_strdup (g_dpgettext2 (GETTEXT_PACKAGE, "fs-type", id_type[n].long_name));
              else
                ret = g_strdup (g_dpgettext2 (GETTEXT_PACKAGE, "fs-type", id_type[n].short_name));
              goto out;
            }
          else if ((g_strcmp0 (id_type[n].version, version) == 0 && strlen (version) > 0) ||
                   (g_strcmp0 (id_type[n].version, "*") == 0 && strlen (version) > 0))
            {
              /* we know better than the compiler here */
#if defined(__GNUC__) || defined(__clang__)
# if G_GNUC_CHECK_VERSION(4, 6) || __clang__
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wformat-nonliteral"
# endif
#endif
              if (long_string)
                ret = g_strdup_printf (g_dpgettext2 (GETTEXT_PACKAGE, "fs-type", id_type[n].long_name), version);
              else
                ret = g_strdup_printf (g_dpgettext2 (GETTEXT_PACKAGE, "fs-type", id_type[n].short_name), version);
              goto out;
#if defined(__GNUC__) || defined(__clang__)
# if G_GNUC_CHECK_VERSION(4, 6) || __clang__
#  pragma GCC diagnostic pop
# endif
#endif
            }
        }
    }

  if (long_string)
    {
      if (strlen (version) > 0)
        {
          /* Translators: Shown for unknown filesystem types.
           * First %s is the raw filesystem type obtained from udev, second %s is version.
           */
          ret = g_strdup_printf (C_("fs-type", "Unknown (%s %s)"), type, version);
        }
      else
        {
          if (strlen (type) > 0)
            {
              /* Translators: Shown for unknown filesystem types.
               * First %s is the raw filesystem type obtained from udev.
               */
              ret = g_strdup_printf (C_("fs-type", "Unknown (%s)"), type);
            }
          else
            {
              /* Translators: Shown for unknown filesystem types.
               */
              ret = g_strdup (C_("fs-type", "Unknown"));
            }
        }
    }
  else
    {
      if (strlen (type) > 0)
        {
          ret = g_strdup (type);
        }
      else
        {
          /* Translators: Shown for unknown filesystem types.
           */
          ret = g_strdup (C_("fs-type", "Unknown"));
        }
    }

 out:
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static const struct
{
  const gchar *type;
  const gchar *name;
} known_partition_table_types[] =
{
  /* Translators: name of partition table format */
  {"dos", N_("Master Boot Record")},
  /* Translators: name of partition table format */
  {"gpt", N_("GUID Partition Table")},
  /* Translators: name of partition table format */
  {"apm", N_("Apple Partition Map")},
  {NULL, NULL}
};

/**
 * udisks_client_get_partition_table_type_for_display:
 * @client: A #UDisksClient.
 * @partition_table_type: A partition table type e.g. 'dos' or 'gpt'.
 *
 * Gets a human readable localized string for @partition_table_type.
 *
 * Returns: (transfer none): A description of @partition_table_type or %NULL.
 */
const gchar *
udisks_client_get_partition_table_type_for_display (UDisksClient  *client,
                                                    const gchar   *partition_table_type)
{
  const gchar *ret = NULL;
  guint n;

  for (n = 0; known_partition_table_types[n].type != NULL; n++)
    {
      if (g_strcmp0 (known_partition_table_types[n].type, partition_table_type) == 0)
        {
          ret = _(known_partition_table_types[n].name);
          goto out;
        }
    }

 out:
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static const struct
{
  const gchar *type;
  const gchar *subtype;
  const gchar *name;
} known_partition_table_subtypes[] =
{
  /* Translators: name of partition table format */
  {"dos", "generic",   NC_("partition-subtype", "Generic")},
  {"dos", "linux",     NC_("partition-subtype", "Linux")},
  {"dos", "microsoft", NC_("partition-subtype", "Windows")},
  {"dos", "other",     NC_("partition-subtype", "Other")},

  {"gpt", "generic",   NC_("partition-subtype", "Generic")},
  {"gpt", "linux",     NC_("partition-subtype", "Linux")},
  {"gpt", "microsoft", NC_("partition-subtype", "Windows")},
  {"gpt", "apple",     NC_("partition-subtype", "Mac OS X")},
  {"gpt", "other",     NC_("partition-subtype", "Other")},

  {"apm", "apple",     NC_("partition-subtype", "Mac OS X")},
  {"apm", "microsoft", NC_("partition-subtype", "Windows")},
  {NULL, NULL}
};

/**
 * udisks_client_get_partition_table_subtype_for_display:
 * @client: A #UDisksClient.
 * @partition_table_type: A partition table type e.g. 'dos' or 'gpt'.
 * @partition_table_subtype: A partition table type e.g. 'dos' or 'gpt'.
 *
 * Gets a human readable localized string for @partition_table_type and @partition_table_subtype.
 *
 * Returns: (transfer none): A description of @partition_table_type and
 * @partition_table_subtype or %NULL.
 */
const gchar *
udisks_client_get_partition_table_subtype_for_display (UDisksClient  *client,
                                                       const gchar   *partition_table_type,
                                                       const gchar   *partition_table_subtype)
{
  const gchar *ret = NULL;
  guint n;

  for (n = 0; known_partition_table_subtypes[n].type != NULL; n++)
    {
      if (g_strcmp0 (known_partition_table_subtypes[n].type,    partition_table_type) == 0 &&
          g_strcmp0 (known_partition_table_subtypes[n].subtype, partition_table_subtype) == 0)
        {
          ret = g_dpgettext2 (GETTEXT_PACKAGE, "partition-subtype", known_partition_table_subtypes[n].name);
          goto out;
        }
    }

 out:
  return ret;
}

/**
 * udisks_client_get_partition_table_subtypes:
 * @client: A #UDisksClient.
 * @partition_table_type: A partition table type e.g. 'dos' or 'gpt'.
 *
 * Gets all known subtypes for @partition_table_type.
 *
 * Returns: (transfer container): A %NULL-terminated array of
 * strings. Only the container should be freed with g_free().
 */
const gchar **
udisks_client_get_partition_table_subtypes (UDisksClient   *client,
                                            const gchar    *partition_table_type)
{
  guint n;
  GPtrArray *p;

  p = g_ptr_array_new();
  for (n = 0; known_partition_table_subtypes[n].type != NULL; n++)
    {
      if (g_strcmp0 (known_partition_table_subtypes[n].type, partition_table_type) == 0)
        {
          g_ptr_array_add (p, (gpointer) known_partition_table_subtypes[n].subtype);
        }
    }
  g_ptr_array_add (p, NULL);

  return (const gchar **) g_ptr_array_free (p, FALSE);
}

/* ---------------------------------------------------------------------------------------------------- */

/* shorthand */
#define F_SWAP   UDISKS_PARTITION_TYPE_INFO_FLAGS_SWAP
#define F_RAID   UDISKS_PARTITION_TYPE_INFO_FLAGS_RAID
#define F_HIDDEN UDISKS_PARTITION_TYPE_INFO_FLAGS_HIDDEN
#define F_CONLY  UDISKS_PARTITION_TYPE_INFO_FLAGS_CREATE_ONLY
#define F_SYSTEM UDISKS_PARTITION_TYPE_INFO_FLAGS_SYSTEM

static const struct
{
  const gchar *table_type;
  const gchar *table_subtype;
  const gchar *type;
  const gchar *name;
  UDisksPartitionTypeInfoFlags flags;
} known_partition_types[] =
{
  /* see http://en.wikipedia.org/wiki/GUID_Partition_Table */

  /* Not associated with any OS */
  {"gpt", "generic",   "024dee41-33e7-11d3-9d69-0008c781f39f", NC_("part-type", "MBR Partition Scheme"), F_SYSTEM},
  {"gpt", "generic",   "c12a7328-f81f-11d2-ba4b-00a0c93ec93b", NC_("part-type", "EFI System"), F_SYSTEM},
  {"gpt", "generic",   "21686148-6449-6e6f-744e-656564454649", NC_("part-type", "BIOS Boot"), F_SYSTEM},
  /* This is also defined in the Apple and Solaris section */
  {"gpt", "generic",   "6a898cc3-1dd2-11b2-99a6-080020736631", NC_("part-type", "ZFS"), 0},
  /* Extended Boot Partition, see http://www.freedesktop.org/wiki/Specifications/BootLoaderSpec/ */
  {"gpt", "generic",   "bc13c2ff-59e6-4262-a352-b275fd6f7172", NC_("part-type", "Extended Boot Partition"), 0},
  /* Discoverable Linux Partitions, see http://systemd.io/DISCOVERABLE_PARTITIONS/ */
  {"gpt", "linux",     "6523f8ae-3eb1-4e2a-a05a-18b695ae656f", NC_("part-type", "Linux Root Partition (Alpha)"), 0},
  {"gpt", "linux",     "d27f46ed-2919-4cb8-bd25-9531f3c16534", NC_("part-type", "Linux Root Partition (ARC)"), 0},
  {"gpt", "linux",     "69dad710-2ce4-4e3c-b16c-21a1d49abed3", NC_("part-type", "Linux Root Partition (32-bit ARM)"), 0},
  {"gpt", "linux",     "b921b045-1df0-41c3-af44-4c6f280d3fae", NC_("part-type", "Linux Root Partition (64-bit ARM/AArch64)"), 0},
  {"gpt", "linux",     "993d8d3d-f80e-4225-855a-9daf8ed7ea97", NC_("part-type", "Linux Root Partition (Itanium/IA-64)"), 0},
  {"gpt", "linux",     "77055800-792c-4f94-b39a-98c91b762bb6", NC_("part-type", "Linux Root Partition (LoongArch 64-bit)"), 0},
  {"gpt", "linux",     "37c58c8a-d913-4156-a25f-48b1b64e07f0", NC_("part-type", "Linux Root Partition (32-bit MIPS LittleEndian/mipsel)"), 0},
  {"gpt", "linux",     "700bda43-7a34-4507-b179-eeb93d7a7ca3", NC_("part-type", "Linux Root Partition (64-bit MIPS LittleEndian/mips64el)"), 0},
  {"gpt", "linux",     "1aacdb3b-5444-4138-bd9e-e5c2239b2346", NC_("part-type", "Linux Root Partition (HPPA/PARISC)"), 0},
  {"gpt", "linux",     "1de3f1ef-fa98-47b5-8dcd-4a860a654d78", NC_("part-type", "Linux Root Partition (32-bit PowerPC)"), 0},
  {"gpt", "linux",     "912ade1d-a839-4913-8964-a10eee08fbd2", NC_("part-type", "Linux Root Partition (64-bit PowerPC BigEndian)"), 0},
  {"gpt", "linux",     "c31c45e6-3f39-412e-80fb-4809c4980599", NC_("part-type", "Linux Root Partition (64-bit PowerPC LittleEndian)"), 0},
  {"gpt", "linux",     "60d5a7fe-8e7d-435c-b714-3dd8162144e1", NC_("part-type", "Linux Root Partition (RISC-V 32-bit)"), 0},
  {"gpt", "linux",     "72ec70a6-cf74-40e6-bd49-4bda08e8f224", NC_("part-type", "Linux Root Partition (RISC-V 64-bit)"), 0},
  {"gpt", "linux",     "08a7acea-624c-4a20-91e8-6e0fa67d23f9", NC_("part-type", "Linux Root Partition (s390)"), 0},
  {"gpt", "linux",     "5eead9a9-fe09-4a1e-a1d7-520d00531306", NC_("part-type", "Linux Root Partition (s390x)"), 0},
  {"gpt", "linux",     "c50cdd70-3862-4cc3-90e1-809a8c93ee2c", NC_("part-type", "Linux Root Partition (TILE-Gx)"), 0},
  {"gpt", "linux",     "44479540-f297-41b2-9af7-d131d5f0458a", NC_("part-type", "Linux Root Partition (x86)"), 0},
  {"gpt", "linux",     "4f68bce3-e8cd-4db1-96e7-fbcaf984b709", NC_("part-type", "Linux Root Partition (amd64/x86_64)"), 0},
  {"gpt", "linux",     "e18cf08c-33ec-4c0d-8246-c6c6fb3da024", NC_("part-type", "Linux /usr/ Partition (Alpha)"), 0},
  {"gpt", "linux",     "7978a683-6316-4922-bbee-38bff5a2fecc", NC_("part-type", "Linux /usr/ Partition (ARC)"), 0},
  {"gpt", "linux",     "7d0359a3-02b3-4f0a-865c-654403e70625", NC_("part-type", "Linux /usr/ Partition (32-bit ARM)"), 0},
  {"gpt", "linux",     "b0e01050-ee5f-4390-949a-9101b17104e9", NC_("part-type", "Linux /usr/ Partition (64-bit ARM/AArch64)"), 0},
  {"gpt", "linux",     "4301d2a6-4e3b-4b2a-bb94-9e0b2c4225ea", NC_("part-type", "Linux /usr/ Partition (Itanium/IA-64)"), 0},
  {"gpt", "linux",     "e611c702-575c-4cbe-9a46-434fa0bf7e3f", NC_("part-type", "Linux /usr/ Partition (LoongArch 64-bit)"), 0},
  {"gpt", "linux",     "0f4868e9-9952-4706-979f-3ed3a473e947", NC_("part-type", "Linux /usr/ Partition (32-bit MIPS LittleEndian/mipsel)"), 0},
  {"gpt", "linux",     "c97c1f32-ba06-40b4-9f22-236061b08aa8", NC_("part-type", "Linux /usr/ Partition (64-bit MIPS LittleEndian/mips64el)"), 0},
  {"gpt", "linux",     "dc4a4480-6917-4262-a4ec-db9384949f25", NC_("part-type", "Linux /usr/ Partition (HPPA/PARISC)"), 0},
  {"gpt", "linux",     "7d14fec5-cc71-415d-9d6c-06bf0b3c3eaf", NC_("part-type", "Linux /usr/ Partition (32-bit PowerPC)"), 0},
  {"gpt", "linux",     "2c9739e2-f068-46b3-9fd0-01c5a9afbcca", NC_("part-type", "Linux /usr/ Partition (64-bit PowerPC BigEndian)"), 0},
  {"gpt", "linux",     "15bb03af-77e7-4d4a-b12b-c0d084f7491c", NC_("part-type", "Linux /usr/ Partition (64-bit PowerPC LittleEndian)"), 0},
  {"gpt", "linux",     "b933fb22-5c3f-4f91-af90-e2bb0fa50702", NC_("part-type", "Linux /usr/ Partition (RISC-V 32-bit)"), 0},
  {"gpt", "linux",     "beaec34b-8442-439b-a40b-984381ed097d", NC_("part-type", "Linux /usr/ Partition (RISC-V 64-bit)"), 0},
  {"gpt", "linux",     "cd0f869b-d0fb-4ca0-b141-9ea87cc78d66", NC_("part-type", "Linux /usr/ Partition (s390)"), 0},
  {"gpt", "linux",     "8a4f5770-50aa-4ed3-874a-99b710db6fea", NC_("part-type", "Linux /usr/ Partition (s390x)"), 0},
  {"gpt", "linux",     "55497029-c7c1-44cc-aa39-815ed1558630", NC_("part-type", "Linux /usr/ Partition (TILE-Gx)"), 0},
  {"gpt", "linux",     "75250d76-8cc6-458e-bd66-bd47cc81a812", NC_("part-type", "Linux /usr/ Partition (x86)"), 0},
  {"gpt", "linux",     "8484680c-9521-48c6-9c11-b0720656f69e", NC_("part-type", "Linux /usr/ Partition (amd64/x86_64)"), 0},
  {"gpt", "linux",     "fc56d9e9-e6e5-4c06-be32-e74407ce09a5", NC_("part-type", "Linux Root Verity Partition (Alpha)"), 0},
  {"gpt", "linux",     "24b2d975-0f97-4521-afa1-cd531e421b8d", NC_("part-type", "Linux Root Verity Partition (ARC)"), 0},
  {"gpt", "linux",     "7386cdf2-203c-47a9-a498-f2ecce45a2d6", NC_("part-type", "Linux Root Verity Partition (32-bit ARM)"), 0},
  {"gpt", "linux",     "df3300ce-d69f-4c92-978c-9bfb0f38d820", NC_("part-type", "Linux Root Verity Partition (64-bit ARM/AArch64)"), 0},
  {"gpt", "linux",     "86ed10d5-b607-45bb-8957-d350f23d0571", NC_("part-type", "Linux Root Verity Partition (Itanium/IA-64)"), 0},
  {"gpt", "linux",     "f3393b22-e9af-4613-a948-9d3bfbd0c535", NC_("part-type", "Linux Root Verity Partition (LoongArch 64-bit)"), 0},
  {"gpt", "linux",     "d7d150d2-2a04-4a33-8f12-16651205ff7b", NC_("part-type", "Linux Root Verity Partition (32-bit MIPS LittleEndian/mipsel)"), 0},
  {"gpt", "linux",     "16b417f8-3e06-4f57-8dd2-9b5232f41aa6", NC_("part-type", "Linux Root Verity Partition (64-bit MIPS LittleEndian/mips64el)"), 0},
  {"gpt", "linux",     "d212a430-fbc5-49f9-a983-a7feef2b8d0e", NC_("part-type", "Linux Root Verity Partition (HPPA/PARISC)"), 0},
  {"gpt", "linux",     "906bd944-4589-4aae-a4e4-dd983917446a", NC_("part-type", "Linux Root Verity Partition (64-bit PowerPC LittleEndian)"), 0},
  {"gpt", "linux",     "9225a9a3-3c19-4d89-b4f6-eeff88f17631", NC_("part-type", "Linux Root Verity Partition (64-bit PowerPC BigEndian)"), 0},
  {"gpt", "linux",     "98cfe649-1588-46dc-b2f0-add147424925", NC_("part-type", "Linux Root Verity Partition (32-bit PowerPC)"), 0},
  {"gpt", "linux",     "ae0253be-1167-4007-ac68-43926c14c5de", NC_("part-type", "Linux Root Verity Partition (RISC-V 32-bit)"), 0},
  {"gpt", "linux",     "b6ed5582-440b-4209-b8da-5ff7c419ea3d", NC_("part-type", "Linux Root Verity Partition (RISC-V 64-bit)"), 0},
  {"gpt", "linux",     "7ac63b47-b25c-463b-8df8-b4a94e6c90e1", NC_("part-type", "Linux Root Verity Partition (s390)"), 0},
  {"gpt", "linux",     "b325bfbe-c7be-4ab8-8357-139e652d2f6b", NC_("part-type", "Linux Root Verity Partition (s390x)"), 0},
  {"gpt", "linux",     "966061ec-28e4-4b2e-b4a5-1f0a825a1d84", NC_("part-type", "Linux Root Verity Partition (TILE-Gx)"), 0},
  {"gpt", "linux",     "2c7357ed-ebd2-46d9-aec1-23d437ec2bf5", NC_("part-type", "Linux Root Verity Partition (amd64/x86_64)"), 0},
  {"gpt", "linux",     "d13c5d3b-b5d1-422a-b29f-9454fdc89d76", NC_("part-type", "Linux Root Verity Partition (x86)"), 0},
  {"gpt", "linux",     "8cce0d25-c0d0-4a44-bd87-46331bf1df67", NC_("part-type", "Linux /usr/ Verity Partition (Alpha)"), 0},
  {"gpt", "linux",     "fca0598c-d880-4591-8c16-4eda05c7347c", NC_("part-type", "Linux /usr/ Verity Partition (ARC)"), 0},
  {"gpt", "linux",     "c215d751-7bcd-4649-be90-6627490a4c05", NC_("part-type", "Linux /usr/ Verity Partition (32-bit ARM)"), 0},
  {"gpt", "linux",     "6e11a4e7-fbca-4ded-b9e9-e1a512bb664e", NC_("part-type", "Linux /usr/ Verity Partition (64-bit ARM/AArch64)"), 0},
  {"gpt", "linux",     "6a491e03-3be7-4545-8e38-83320e0ea880", NC_("part-type", "Linux /usr/ Verity Partition (Itanium/IA-64)"), 0},
  {"gpt", "linux",     "f46b2c26-59ae-48f0-9106-c50ed47f673d", NC_("part-type", "Linux /usr/ Verity Partition (LoongArch 64-bit)"), 0},
  {"gpt", "linux",     "46b98d8d-b55c-4e8f-aab3-37fca7f80752", NC_("part-type", "Linux /usr/ Verity Partition (32-bit MIPS LittleEndian/mipsel)"), 0},
  {"gpt", "linux",     "3c3d61fe-b5f3-414d-bb71-8739a694a4ef", NC_("part-type", "Linux /usr/ Verity Partition (64-bit MIPS LittleEndian/mips64el)"), 0},
  {"gpt", "linux",     "5843d618-ec37-48d7-9f12-cea8e08768b2", NC_("part-type", "Linux /usr/ Verity Partition (HPPA/PARISC)"), 0},
  {"gpt", "linux",     "ee2b9983-21e8-4153-86d9-b6901a54d1ce", NC_("part-type", "Linux /usr/ Verity Partition (64-bit PowerPC LittleEndian)"), 0},
  {"gpt", "linux",     "bdb528a5-a259-475f-a87d-da53fa736a07", NC_("part-type", "Linux /usr/ Verity Partition (64-bit PowerPC BigEndian)"), 0},
  {"gpt", "linux",     "df765d00-270e-49e5-bc75-f47bb2118b09", NC_("part-type", "Linux /usr/ Verity Partition (32-bit PowerPC)"), 0},
  {"gpt", "linux",     "cb1ee4e3-8cd0-4136-a0a4-aa61a32e8730", NC_("part-type", "Linux /usr/ Verity Partition (RISC-V 32-bit)"), 0},
  {"gpt", "linux",     "8f1056be-9b05-47c4-81d6-be53128e5b54", NC_("part-type", "Linux /usr/ Verity Partition (RISC-V 64-bit)"), 0},
  {"gpt", "linux",     "b663c618-e7bc-4d6d-90aa-11b756bb1797", NC_("part-type", "Linux /usr/ Verity Partition (s390)"), 0},
  {"gpt", "linux",     "31741cc4-1a2a-4111-a581-e00b447d2d06", NC_("part-type", "Linux /usr/ Verity Partition (s390x)"), 0},
  {"gpt", "linux",     "2fb4bf56-07fa-42da-8132-6b139f2026ae", NC_("part-type", "Linux /usr/ Verity Partition (TILE-Gx)"), 0},
  {"gpt", "linux",     "77ff5f63-e7b6-4633-acf4-1565b864c0e6", NC_("part-type", "Linux /usr/ Verity Partition (amd64/x86_64)"), 0},
  {"gpt", "linux",     "8f461b0d-14ee-4e81-9aa9-049b6fb97abd", NC_("part-type", "Linux /usr/ Verity Partition (x86)"), 0},
  {"gpt", "linux",     "d46495b7-a053-414f-80f7-700c99921ef8", NC_("part-type", "Linux Root Verity Signature Partition (Alpha)"), 0},
  {"gpt", "linux",     "143a70ba-cbd3-4f06-919f-6c05683a78bc", NC_("part-type", "Linux Root Verity Signature Partition (ARC)"), 0},
  {"gpt", "linux",     "42b0455f-eb11-491d-98d3-56145ba9d037", NC_("part-type", "Linux Root Verity Signature Partition (32-bit ARM)"), 0},
  {"gpt", "linux",     "6db69de6-29f4-4758-a7a5-962190f00ce3", NC_("part-type", "Linux Root Verity Signature Partition (64-bit ARM/AArch64)"), 0},
  {"gpt", "linux",     "e98b36ee-32ba-4882-9b12-0ce14655f46a", NC_("part-type", "Linux Root Verity Signature Partition (Itanium/IA-64)"), 0},
  {"gpt", "linux",     "5afb67eb-ecc8-4f85-ae8e-ac1e7c50e7d0", NC_("part-type", "Linux Root Verity Signature Partition (LoongArch 64-bit)"), 0},
  {"gpt", "linux",     "c919cc1f-4456-4eff-918c-f75e94525ca5", NC_("part-type", "Linux Root Verity Signature Partition (32-bit MIPS LittleEndian/mipsel)"), 0},
  {"gpt", "linux",     "904e58ef-5c65-4a31-9c57-6af5fc7c5de7", NC_("part-type", "Linux Root Verity Signature Partition (64-bit MIPS LittleEndian/mips64el)"), 0},
  {"gpt", "linux",     "15de6170-65d3-431c-916e-b0dcd8393f25", NC_("part-type", "Linux Root Verity Signature Partition (HPPA/PARISC)"), 0},
  {"gpt", "linux",     "d4a236e7-e873-4c07-bf1d-bf6cf7f1c3c6", NC_("part-type", "Linux Root Verity Signature Partition (64-bit PowerPC LittleEndian)"), 0},
  {"gpt", "linux",     "f5e2c20c-45b2-4ffa-bce9-2a60737e1aaf", NC_("part-type", "Linux Root Verity Signature Partition (64-bit PowerPC BigEndian)"), 0},
  {"gpt", "linux",     "1b31b5aa-add9-463a-b2ed-bd467fc857e7", NC_("part-type", "Linux Root Verity Signature Partition (32-bit PowerPC)"), 0},
  {"gpt", "linux",     "3a112a75-8729-4380-b4cf-764d79934448", NC_("part-type", "Linux Root Verity Signature Partition (RISC-V 32-bit)"), 0},
  {"gpt", "linux",     "efe0f087-ea8d-4469-821a-4c2a96a8386a", NC_("part-type", "Linux Root Verity Signature Partition (RISC-V 64-bit)"), 0},
  {"gpt", "linux",     "3482388e-4254-435a-a241-766a065f9960", NC_("part-type", "Linux Root Verity Signature Partition (s390)"), 0},
  {"gpt", "linux",     "c80187a5-73a3-491a-901a-017c3fa953e9", NC_("part-type", "Linux Root Verity Signature Partition (s390x)"), 0},
  {"gpt", "linux",     "b3671439-97b0-4a53-90f7-2d5a8f3ad47b", NC_("part-type", "Linux Root Verity Signature Partition (TILE-Gx)"), 0},
  {"gpt", "linux",     "41092b05-9fc8-4523-994f-2def0408b176", NC_("part-type", "Linux Root Verity Signature Partition (amd64/x86_64)"), 0},
  {"gpt", "linux",     "5996fc05-109c-48de-808b-23fa0830b676", NC_("part-type", "Linux Root Verity Signature Partition (x86)"), 0},
  {"gpt", "linux",     "5c6e1c76-076a-457a-a0fe-f3b4cd21ce6e", NC_("part-type", "Linux /usr/ Verity Signature Partition (Alpha)"), 0},
  {"gpt", "linux",     "94f9a9a1-9971-427a-a400-50cb297f0f35", NC_("part-type", "Linux /usr/ Verity Signature Partition (ARC)"), 0},
  {"gpt", "linux",     "d7ff812f-37d1-4902-a810-d76ba57b975a", NC_("part-type", "Linux /usr/ Verity Signature Partition (32-bit ARM)"), 0},
  {"gpt", "linux",     "c23ce4ff-44bd-4b00-b2d4-b41b3419e02a", NC_("part-type", "Linux /usr/ Verity Signature Partition (64-bit ARM/AArch64)"), 0},
  {"gpt", "linux",     "8de58bc2-2a43-460d-b14e-a76e4a17b47f", NC_("part-type", "Linux /usr/ Verity Signature Partition (Itanium/IA-64)"), 0},
  {"gpt", "linux",     "b024f315-d330-444c-8461-44bbde524e99", NC_("part-type", "Linux /usr/ Verity Signature Partition (LoongArch 64-bit)"), 0},
  {"gpt", "linux",     "3e23ca0b-a4bc-4b4e-8087-5ab6a26aa8a9", NC_("part-type", "Linux /usr/ Verity Signature Partition (32-bit MIPS LittleEndian/mipsel)"), 0},
  {"gpt", "linux",     "f2c2c7ee-adcc-4351-b5c6-ee9816b66e16", NC_("part-type", "Linux /usr/ Verity Signature Partition (64-bit MIPS LittleEndian/mips64el)"), 0},
  {"gpt", "linux",     "450dd7d1-3224-45ec-9cf2-a43a346d71ee", NC_("part-type", "Linux /usr/ Verity Signature Partition (HPPA/PARISC)"), 0},
  {"gpt", "linux",     "c8bfbd1e-268e-4521-8bba-bf314c399557", NC_("part-type", "Linux /usr/ Verity Signature Partition (64-bit PowerPC LittleEndian)"), 0},
  {"gpt", "linux",     "0b888863-d7f8-4d9e-9766-239fce4d58af", NC_("part-type", "Linux /usr/ Verity Signature Partition (64-bit PowerPC BigEndian)"), 0},
  {"gpt", "linux",     "7007891d-d371-4a80-86a4-5cb875b9302e", NC_("part-type", "Linux /usr/ Verity Signature Partition (32-bit PowerPC)"), 0},
  {"gpt", "linux",     "c3836a13-3137-45ba-b583-b16c50fe5eb4", NC_("part-type", "Linux /usr/ Verity Signature Partition (RISC-V 32-bit)"), 0},
  {"gpt", "linux",     "d2f9000a-7a18-453f-b5cd-4d32f77a7b32", NC_("part-type", "Linux /usr/ Verity Signature Partition (RISC-V 64-bit)"), 0},
  {"gpt", "linux",     "17440e4f-a8d0-467f-a46e-3912ae6ef2c5", NC_("part-type", "Linux /usr/ Verity Signature Partition (s390)"), 0},
  {"gpt", "linux",     "3f324816-667b-46ae-86ee-9b0c0c6c11b4", NC_("part-type", "Linux /usr/ Verity Signature Partition (s390x)"), 0},
  {"gpt", "linux",     "4ede75e2-6ccc-4cc8-b9c7-70334b087510", NC_("part-type", "Linux /usr/ Verity Signature Partition (TILE-Gx)"), 0},
  {"gpt", "linux",     "e7bb33fb-06cf-4e81-8273-e543b413e2e2", NC_("part-type", "Linux /usr/ Verity Signature Partition (amd64/x86_64)"), 0},
  {"gpt", "linux",     "974a71c0-de41-43c3-be5d-5c5ccd1ad2c0", NC_("part-type", "Linux /usr/ Verity Signature Partition (x86)"), 0},
  {"gpt", "linux",     "4d21b016-b534-45c2-a9fb-5c16e091fd2d", NC_("part-type", "Linux Variable Data Partition"), 0},
  {"gpt", "linux",     "7ec6f557-3bc5-4aca-b293-16ef5df639d1", NC_("part-type", "Linux Temporary Data Partition"), 0},
  {"gpt", "linux",     "773f91ef-66d4-49b5-bd83-d683bf40ad16", NC_("part-type", "Linux Per-user Home Partition"), 0},
  {"gpt", "linux",     "933ac7e1-2eb4-4f13-b844-0e14e2aef915", NC_("part-type", "Linux Home Partition"), 0},
  {"gpt", "linux",     "3b8f8425-20e0-4f3b-907f-1a25a76f98e8", NC_("part-type", "Linux Server Data Partition"), 0},
  /* Linux */
  {"gpt", "linux",     "0657fd6d-a4ab-43c4-84e5-0933c84b4f4f", NC_("part-type", "Linux Swap"), F_SWAP},
  {"gpt", "linux",     "0fc63daf-8483-4772-8e79-3d69d8477de4", NC_("part-type", "Linux Filesystem"), 0},
  {"gpt", "linux",     "a19d880f-05fc-4d3b-a006-743f0f84911e", NC_("part-type", "Linux RAID"), F_RAID},
  {"gpt", "linux",     "e6d6d379-f507-44c2-a23c-238f2a3df928", NC_("part-type", "Linux LVM"), F_RAID},
  {"gpt", "linux",     "8da63339-0007-60c0-c436-083ac8230908", NC_("part-type", "Linux Reserved"), 0},
  /* Microsoft */
  {"gpt", "microsoft", "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7", NC_("part-type", "Basic Data"), 0},
  {"gpt", "microsoft", "e3c9e316-0b5c-4db8-817d-f92df00215ae", NC_("part-type", "Microsoft Reserved"), 0},
  {"gpt", "microsoft", "5808c8aa-7e8f-42e0-85d2-e1e90434cfb3", NC_("part-type", "Microsoft LDM metadata"), 0},
  {"gpt", "microsoft", "af9b60a0-1431-4f62-bc68-3311714a69ad", NC_("part-type", "Microsoft LDM data"), 0},
  {"gpt", "microsoft", "de94bba4-06d1-4d40-a16a-bfd50179d6ac", NC_("part-type", "Microsoft Windows Recovery Environment"), 0},
  /* Apple OS X */
  {"gpt", "apple",     "48465300-0000-11aa-aa11-00306543ecac", NC_("part-type", "Apple HFS/HFS+"), 0},
  {"gpt", "apple",     "7c3457ef-0000-11aa-aa11-00306543ecac", NC_("part-type", "Apple APFS"), 0},
  {"gpt", "apple",     "55465300-0000-11aa-aa11-00306543ecac", NC_("part-type", "Apple UFS"), 0},
  {"gpt", "apple",     "6a898cc3-1dd2-11b2-99a6-080020736631", NC_("part-type", "Apple ZFS"), 0}, /* same as ZFS */
  {"gpt", "apple",     "52414944-0000-11aa-aa11-00306543ecac", NC_("part-type", "Apple RAID"), F_RAID},
  {"gpt", "apple",     "52414944-5f4f-11aa-aa11-00306543ecac", NC_("part-type", "Apple RAID (offline)"), F_RAID},
  {"gpt", "apple",     "426f6f74-0000-11aa-aa11-00306543ecac", NC_("part-type", "Apple Boot"), F_SYSTEM},
  {"gpt", "apple",     "4c616265-6c00-11aa-aa11-00306543ecac", NC_("part-type", "Apple Label"), 0},
  {"gpt", "apple",     "5265636f-7665-11aa-aa11-00306543ecac", NC_("part-type", "Apple TV Recovery"), F_SYSTEM},
  {"gpt", "apple",     "53746f72-6167-11aa-aa11-00306543ecac", NC_("part-type", "Apple Core Storage"), F_RAID},
  /* HP-UX */
  {"gpt", "other",     "75894c1e-3aeb-11d3-b7c1-7b03a0000000", NC_("part-type", "HP-UX Data"), 0},
  {"gpt", "other",     "e2a1e728-32e3-11d6-a682-7b03a0000000", NC_("part-type", "HP-UX Service"), 0},
  /* FreeBSD */
  {"gpt", "other",     "83bd6b9d-7f41-11dc-be0b-001560b84f0f", NC_("part-type", "FreeBSD Boot"), 0},
  {"gpt", "other",     "516e7cb4-6ecf-11d6-8ff8-00022d09712b", NC_("part-type", "FreeBSD Data"), 0},
  {"gpt", "other",     "516e7cb5-6ecf-11d6-8ff8-00022d09712b", NC_("part-type", "FreeBSD Swap"), F_SWAP},
  {"gpt", "other",     "516e7cb6-6ecf-11d6-8ff8-00022d09712b", NC_("part-type", "FreeBSD UFS"), 0},
  {"gpt", "other",     "516e7cb8-6ecf-11d6-8ff8-00022d09712b", NC_("part-type", "FreeBSD Vinum"), F_RAID},
  {"gpt", "other",     "516e7cba-6ecf-11d6-8ff8-00022d09712b", NC_("part-type", "FreeBSD ZFS"), 0},
  /* Solaris */
  {"gpt", "other",     "6a82cb45-1dd2-11b2-99a6-080020736631", NC_("part-type", "Solaris Boot"), 0},
  {"gpt", "other",     "6a85cf4d-1dd2-11b2-99a6-080020736631", NC_("part-type", "Solaris Root"), 0},
  {"gpt", "other",     "6a87c46f-1dd2-11b2-99a6-080020736631", NC_("part-type", "Solaris Swap"), F_SWAP},
  {"gpt", "other",     "6a8b642b-1dd2-11b2-99a6-080020736631", NC_("part-type", "Solaris Backup"), 0},
  {"gpt", "other",     "6a898cc3-1dd2-11b2-99a6-080020736631", NC_("part-type", "Solaris /usr"), 0}, /* same as ZFS */
  {"gpt", "other",     "6a8ef2e9-1dd2-11b2-99a6-080020736631", NC_("part-type", "Solaris /var"), 0},
  {"gpt", "other",     "6a90ba39-1dd2-11b2-99a6-080020736631", NC_("part-type", "Solaris /home"), 0},
  {"gpt", "other",     "6a9283a5-1dd2-11b2-99a6-080020736631", NC_("part-type", "Solaris Alternate Sector"), 0},
  {"gpt", "other",     "6a945a3b-1dd2-11b2-99a6-080020736631", NC_("part-type", "Solaris Reserved"), 0},
  {"gpt", "other",     "6a9630d1-1dd2-11b2-99a6-080020736631", NC_("part-type", "Solaris Reserved (2)"), 0},
  {"gpt", "other",     "6a980767-1dd2-11b2-99a6-080020736631", NC_("part-type", "Solaris Reserved (3)"), 0},
  {"gpt", "other",     "6a96237f-1dd2-11b2-99a6-080020736631", NC_("part-type", "Solaris Reserved (4)"), 0},
  {"gpt", "other",     "6a8d2ac7-1dd2-11b2-99a6-080020736631", NC_("part-type", "Solaris Reserved (5)"), 0},
  /* NetBSD */
  {"gpt", "other",     "49f48d32-b10e-11dc-b99b-0019d1879648", NC_("part-type", "NetBSD Swap"), F_SWAP},
  {"gpt", "other",     "49f48d5a-b10e-11dc-b99b-0019d1879648", NC_("part-type", "NetBSD FFS"), 0},
  {"gpt", "other",     "49f48d82-b10e-11dc-b99b-0019d1879648", NC_("part-type", "NetBSD LFS"), 0},
  {"gpt", "other",     "49f48daa-b10e-11dc-b99b-0019d1879648", NC_("part-type", "NetBSD RAID"), F_RAID},
  {"gpt", "other",     "2db519c4-b10f-11dc-b99b-0019d1879648", NC_("part-type", "NetBSD Concatenated"), 0},
  {"gpt", "other",     "2db519ec-b10f-11dc-b99b-0019d1879648", NC_("part-type", "NetBSD Encrypted"), 0},
  /* VMWare, see http://blogs.vmware.com/vsphere/2011/08/vsphere-50-storage-features-part-7-gpt.html */
  {"gpt", "other",     "aa31e02a-400f-11db-9590-000c2911d1b8", NC_("part-type", "VMWare VMFS"), 0},
  {"gpt", "other",     "9d275380-40ad-11db-bf97-000c2911d1b8", NC_("part-type", "VMWare vmkcore"), 0},
  /* ChromeOS, see http://www.chromium.org/chromium-os/chromiumos-design-docs/disk-format */
  {"gpt", "other",     "cab6e88e-abf3-4102-a07a-d4bb9be3c1d3", NC_("part-type", "ChromeOS Firmware"), 0},
  {"gpt", "other",     "fe3a2a5d-4f32-41a7-b725-accc3285a309", NC_("part-type", "ChromeOS Kernel"), 0},
  {"gpt", "other",     "3cb8e202-3b7e-47dd-8a3c-7ff2a13cfcec", NC_("part-type", "ChromeOS Root Filesystem"), 0},
  {"gpt", "other",     "2e0a753d-9e48-43b0-8337-b15192cb1b5e", NC_("part-type", "ChromeOS Reserved"), 0},
  /* Intel Partition Types */
  /*     FFS = Fast Flash Standby, aka Intel Rapid start  */
  /*     http://downloadmirror.intel.com/22647/eng/Intel%20Rapid%20Start%20Technology%20Deployment%20Guide%20v1.0.pdf */
  {"gpt", "other",     "d3bfe2de-3daf-11df-ba40-e3a556d89593", NC_("part-type", "Intel FFS Reserved"), 0},

  /* see http://developer.apple.com/documentation/mac/devices/devices-126.html
   *     http://lists.apple.com/archives/Darwin-drivers/2003/May/msg00021.html */
  {"apm", "apple",     "Apple_Unix_SVR2", NC_("part-type", "Apple UFS"), 0},
  {"apm", "apple",     "Apple_HFS", NC_("part-type", "Apple HFS/HFS"), 0},
  {"apm", "apple",     "Apple_partition_map", NC_("part-type", "Apple Partition Map"), 0},
  {"apm", "apple",     "Apple_Free", NC_("part-type", "Unused"), 0},
  {"apm", "apple",     "Apple_Scratch", NC_("part-type", "Empty"), 0},
  {"apm", "apple",     "Apple_Driver", NC_("part-type", "Driver"), 0},
  {"apm", "apple",     "Apple_Driver43", NC_("part-type", "Driver 4.3"), 0},
  {"apm", "apple",     "Apple_PRODOS", NC_("part-type", "ProDOS file system"), 0},
  {"apm", "microsoft", "DOS_FAT_12", NC_("part-type", "FAT 12"), 0},
  {"apm", "microsoft", "DOS_FAT_16", NC_("part-type", "FAT 16"), 0},
  {"apm", "microsoft", "DOS_FAT_32", NC_("part-type", "FAT 32"), 0},
  {"apm", "microsoft", "Windows_FAT_16", NC_("part-type", "FAT 16 (Windows)"), 0},
  {"apm", "microsoft", "Windows_FAT_32", NC_("part-type", "FAT 32 (Windows)"), 0},

  /* see http://www.win.tue.nl/~aeb/partitions/partition_types-1.html */
  {"dos", "generic",   "0x05",  NC_("part-type", "Extended"), F_CONLY},
  {"dos", "generic",   "0xee",  NC_("part-type", "EFI GPT"), F_SYSTEM},
  {"dos", "generic",   "0xef",  NC_("part-type", "EFI (FAT-12/16/32)"), F_SYSTEM},
  {"dos", "linux",     "0x82",  NC_("part-type", "Linux swap"), F_SWAP},
  {"dos", "linux",     "0x83",  NC_("part-type", "Linux"), 0},
  {"dos", "linux",     "0x85",  NC_("part-type", "Linux Extended"), F_CONLY},
  {"dos", "linux",     "0x8e",  NC_("part-type", "Linux LVM"), F_RAID},
  {"dos", "linux",     "0xfd",  NC_("part-type", "Linux RAID auto"), F_RAID},
  {"dos", "microsoft", "0x01",  NC_("part-type", "FAT12"), 0},
  {"dos", "microsoft", "0x04",  NC_("part-type", "FAT16 <32M"), 0},
  {"dos", "microsoft", "0x06",  NC_("part-type", "FAT16"), 0},
  {"dos", "microsoft", "0x07",  NC_("part-type", "NTFS/exFAT/HPFS"), 0},
  {"dos", "microsoft", "0x0b",  NC_("part-type", "W95 FAT32"), 0},
  {"dos", "microsoft", "0x0c",  NC_("part-type", "W95 FAT32 (LBA)"), 0},
  {"dos", "microsoft", "0x0e",  NC_("part-type", "W95 FAT16 (LBA)"), 0},
  {"dos", "microsoft", "0x0f",  NC_("part-type", "W95 Ext d (LBA)"), F_CONLY},
  {"dos", "microsoft", "0x11",  NC_("part-type", "Hidden FAT12"), F_HIDDEN},
  {"dos", "microsoft", "0x14",  NC_("part-type", "Hidden FAT16 <32M"), F_HIDDEN},
  {"dos", "microsoft", "0x16",  NC_("part-type", "Hidden FAT16"), F_HIDDEN},
  {"dos", "microsoft", "0x17",  NC_("part-type", "Hidden HPFS/NTFS"), F_HIDDEN},
  {"dos", "microsoft", "0x1b",  NC_("part-type", "Hidden W95 FAT32"), F_HIDDEN},
  {"dos", "microsoft", "0x1c",  NC_("part-type", "Hidden W95 FAT32 (LBA)"), F_HIDDEN},
  {"dos", "microsoft", "0x1e",  NC_("part-type", "Hidden W95 FAT16 (LBA)"), F_HIDDEN},
  {"dos", "other",     "0x10",  NC_("part-type", "OPUS"), 0},
  {"dos", "other",     "0x12",  NC_("part-type", "Compaq diagnostics"), 0},
  {"dos", "other",     "0x3c",  NC_("part-type", "PartitionMagic"), 0},
  {"dos", "other",     "0x81",  NC_("part-type", "Minix"), 0}, /* cf. http://en.wikipedia.org/wiki/MINIX_file_system */
  {"dos", "other",     "0x84",  NC_("part-type", "Hibernation"), 0},
  {"dos", "other",     "0xa0",  NC_("part-type", "Hibernation"), 0},
  {"dos", "other",     "0xa5",  NC_("part-type", "FreeBSD"), 0},
  {"dos", "other",     "0xa6",  NC_("part-type", "OpenBSD"), 0},
  {"dos", "other",     "0xa8",  NC_("part-type", "Mac OS X"), 0},
  {"dos", "other",     "0xaf",  NC_("part-type", "Mac OS X"), 0},
  {"dos", "other",     "0xbe",  NC_("part-type", "Solaris boot"), 0},
  {"dos", "other",     "0xbf",  NC_("part-type", "Solaris"), 0},
  {"dos", "other",     "0xeb",  NC_("part-type", "BeOS BFS"), 0},
  {"dos", "other",     "0xec",  NC_("part-type", "SkyOS SkyFS"), 0},
  {NULL,  NULL, NULL}
};

/**
 * udisks_client_get_partition_type_infos:
 * @client: A #UDisksClient.
 * @partition_table_type: A partition table type e.g. 'dos' or 'gpt'.
 * @partition_table_subtype: (allow-none): A partition table subtype or %NULL to get all known types.
 *
 * Gets information about all known partition types for @partition_table_type and @partition_table_subtype.
 *
 * Returns: (transfer full) (element-type UDisksPartitionTypeInfo): A list of
 *   #UDisksPartitionTypeInfo instances. The returned list should be freed
 *   with g_list_free() after freeing each element with udisks_partition_type_info_free().
 */
GList *
udisks_client_get_partition_type_infos (UDisksClient   *client,
                                        const gchar    *partition_table_type,
                                        const gchar    *partition_table_subtype)
{
  GList *ret = NULL;
  guint n;

  for (n = 0; known_partition_types[n].name != NULL; n++)
    {
      if (g_strcmp0 (known_partition_types[n].table_type, partition_table_type) == 0 &&
          (partition_table_subtype == NULL ||
           g_strcmp0 (known_partition_types[n].table_subtype, partition_table_subtype) == 0))
        {
          UDisksPartitionTypeInfo *info = udisks_partition_type_info_new ();
          info->table_type    = known_partition_types[n].table_type;
          info->table_subtype = known_partition_types[n].table_subtype;
          info->type          = known_partition_types[n].type;
          info->flags         = known_partition_types[n].flags;
          ret = g_list_prepend (ret, info);
        }
    }
  ret = g_list_reverse (ret);
  return ret;
}

/**
 * udisks_client_get_partition_type_for_display:
 * @client: A #UDisksClient.
 * @partition_table_type: A partitioning type e.g. 'dos' or 'gpt'.
 * @partition_type: A partition type.
 *
 * Gets a human readable localized string for @partiton_table_type and @partition_type.
 *
 * Returns: (transfer none): A description of @partition_type or %NULL if
 * unknown.
 */
const gchar *
udisks_client_get_partition_type_for_display (UDisksClient  *client,
                                              const gchar   *partition_table_type,
                                              const gchar   *partition_type)
{
  const gchar *ret = NULL;
  guint n;

  for (n = 0; known_partition_types[n].name != NULL; n++)
    {
      if (g_strcmp0 (known_partition_types[n].table_type, partition_table_type) == 0 &&
          g_strcmp0 (known_partition_types[n].type, partition_type) == 0)
        {
          ret = g_dpgettext2 (GETTEXT_PACKAGE, "part-type", known_partition_types[n].name);
          goto out;
        }
    }

 out:
  return ret;
}

/**
 * udisks_client_get_partition_type_and_subtype_for_display:
 * @client: A #UDisksClient.
 * @partition_table_type: A partitioning type e.g. 'dos' or 'gpt'.
 * @partition_table_subtype: A partitioning subtype or %NULL.
 * @partition_type: A partition type.
 *
 * Like udisks_client_get_partition_type_for_display() but also takes
 * the partition table subtype into account, if available. This is
 * useful in scenarios where different subtypes is using the same
 * partition type.
 *
 * Returns: (transfer none): A description of @partition_type or %NULL if
 * unknown.
 *
 * Since: 2.1.1
 */
const gchar *
udisks_client_get_partition_type_and_subtype_for_display (UDisksClient  *client,
                                                          const gchar   *partition_table_type,
                                                          const gchar   *partition_table_subtype,
                                                          const gchar   *partition_type)
{
  const gchar *ret = NULL;
  guint n;

  for (n = 0; known_partition_types[n].name != NULL; n++)
    {
      if (g_strcmp0 (known_partition_types[n].table_type, partition_table_type) == 0 &&
          g_strcmp0 (known_partition_types[n].type, partition_type) == 0)
        {
          if (partition_table_subtype != NULL &&
              g_strcmp0 (known_partition_types[n].table_subtype, partition_table_subtype) != 0)
            continue;
          ret = g_dpgettext2 (GETTEXT_PACKAGE, "part-type", known_partition_types[n].name);
          goto out;
        }
    }

 out:
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_client_get_job_description_from_operation:
 * @operation: A job operation name.
 *
 * Gets a human-readable and localized text string describing a
 * a job @operation.
 *
 * For known job operation types, see the documentation for the
 * <link linkend="gdbus-property-org-freedesktop-UDisks2-Job.Operation">Job:Operation</link>
 * D-Bus property.
 *
 * Returns: (transfer full): A string that should be freed with g_free().
 */
gchar *
udisks_client_get_job_description_from_operation (const gchar *operation)
{
  static gsize once = 0;
  static GHashTable *hash = NULL;
  gchar *ret = NULL;

  if (g_once_init_enter (&once))
    {
      hash = g_hash_table_new (g_str_hash, g_str_equal);
      g_hash_table_insert (hash, (gpointer) "ata-smart-selftest",   (gpointer) C_("job", "SMART self-test"));
      g_hash_table_insert (hash, (gpointer) "drive-eject",          (gpointer) C_("job", "Ejecting Medium"));
      g_hash_table_insert (hash, (gpointer) "encrypted-unlock",     (gpointer) C_("job", "Unlocking Device"));
      g_hash_table_insert (hash, (gpointer) "encrypted-lock",       (gpointer) C_("job", "Locking Device"));
      g_hash_table_insert (hash, (gpointer) "encrypted-modify",     (gpointer) C_("job", "Modifying Encrypted Device"));
      g_hash_table_insert (hash, (gpointer) "encrypted-resize",     (gpointer) C_("job", "Resizing Encrypted Device"));
      g_hash_table_insert (hash, (gpointer) "encrypted-convert",    (gpointer) C_("job", "Converting Encrypted Device"));
      g_hash_table_insert (hash, (gpointer) "encrypted-header-backup",    (gpointer) C_("job", "Backing Up Header of an Encrypted Device"));
      g_hash_table_insert (hash, (gpointer) "encrypted-reencrypt",  (gpointer) C_("job", "Reencrypting an Encrypted Device"));
      g_hash_table_insert (hash, (gpointer) "block-restore-encrypted-header",    (gpointer) C_("job", "Restoring Header of an Encrypted Device"));
      g_hash_table_insert (hash, (gpointer) "block-encrypt",        (gpointer) C_("job", "Encrypting a Block Device"));
      g_hash_table_insert (hash, (gpointer) "swapspace-start",      (gpointer) C_("job", "Starting Swap Device"));
      g_hash_table_insert (hash, (gpointer) "swapspace-stop",       (gpointer) C_("job", "Stopping Swap Device"));
      g_hash_table_insert (hash, (gpointer) "swapspace-modify",     (gpointer) C_("job", "Modifying Swap Device"));
      g_hash_table_insert (hash, (gpointer) "filesystem-check",     (gpointer) C_("job", "Checking Filesystem"));
      g_hash_table_insert (hash, (gpointer) "filesystem-mount",     (gpointer) C_("job", "Mounting Filesystem"));
      g_hash_table_insert (hash, (gpointer) "filesystem-unmount",   (gpointer) C_("job", "Unmounting Filesystem"));
      g_hash_table_insert (hash, (gpointer) "filesystem-modify",    (gpointer) C_("job", "Modifying Filesystem"));
      g_hash_table_insert (hash, (gpointer) "filesystem-repair",    (gpointer) C_("job", "Repairing Filesystem"));
      g_hash_table_insert (hash, (gpointer) "filesystem-resize",    (gpointer) C_("job", "Resizing Filesystem"));
      g_hash_table_insert (hash, (gpointer) "format-erase",         (gpointer) C_("job", "Erasing Device"));
      g_hash_table_insert (hash, (gpointer) "format-mkfs",          (gpointer) C_("job", "Creating Filesystem"));
      g_hash_table_insert (hash, (gpointer) "loop-setup",           (gpointer) C_("job", "Setting Up Loop Device"));
      g_hash_table_insert (hash, (gpointer) "partition-modify",     (gpointer) C_("job", "Modifying Partition"));
      g_hash_table_insert (hash, (gpointer) "partition-delete",     (gpointer) C_("job", "Deleting Partition"));
      g_hash_table_insert (hash, (gpointer) "partition-create",     (gpointer) C_("job", "Creating Partition"));
      g_hash_table_insert (hash, (gpointer) "cleanup",              (gpointer) C_("job", "Cleaning Up"));
      g_hash_table_insert (hash, (gpointer) "ata-secure-erase",     (gpointer) C_("job", "ATA Secure Erase"));
      g_hash_table_insert (hash, (gpointer) "ata-enhanced-secure-erase", (gpointer) C_("job", "ATA Enhanced Secure Erase"));
      g_hash_table_insert (hash, (gpointer) "md-raid-stop",         (gpointer) C_("job", "Stopping RAID Array"));
      g_hash_table_insert (hash, (gpointer) "md-raid-start",        (gpointer) C_("job", "Starting RAID Array"));
      g_hash_table_insert (hash, (gpointer) "md-raid-fault-device", (gpointer) C_("job", "Marking Device as Faulty"));
      g_hash_table_insert (hash, (gpointer) "md-raid-remove-device",(gpointer) C_("job", "Removing Device from Array"));
      g_hash_table_insert (hash, (gpointer) "md-raid-add-device",   (gpointer) C_("job", "Adding Device to Array"));
      g_hash_table_insert (hash, (gpointer) "md-raid-set-bitmap",   (gpointer) C_("job", "Setting Write-Intent Bitmap"));
      g_hash_table_insert (hash, (gpointer) "md-raid-create",       (gpointer) C_("job", "Creating RAID Array"));
      g_once_init_leave (&once, (gsize) 1);
    }

  if (operation != NULL)
    ret = g_strdup (g_hash_table_lookup (hash, operation));
  if (ret == NULL)
    ret = g_strdup_printf (C_("unknown-job", "Unknown (%s)"), operation == NULL ? "(null)" : operation);

  return ret;
}

/**
 * udisks_client_get_job_description:
 * @client: A #UDisksClient.
 * @job: A #UDisksJob.
 *
 * Gets a human-readable and localized text string describing the
 * operation of @job.
 *
 * For known job types, see the documentation for the
 * <link linkend="gdbus-property-org-freedesktop-UDisks2-Job.Operation">Job:Operation</link>
 * D-Bus property.
 *
 * Returns: (transfer full): A string that should be freed with g_free().
 */
gchar *
udisks_client_get_job_description (UDisksClient   *client,
                                   UDisksJob      *job)
{
  g_return_val_if_fail (UDISKS_IS_CLIENT (client), NULL);

  return udisks_client_get_job_description_from_operation (udisks_job_get_operation (job));
}

/* ---------------------------------------------------------------------------------------------------- */

static UDisksPartitionTypeInfo *
udisks_partition_type_info_new (void)
{
  UDisksPartitionTypeInfo *ret;
  ret = g_slice_new0 (UDisksPartitionTypeInfo);
  return ret;
}

static UDisksPartitionTypeInfo *
udisks_partition_type_info_copy (UDisksPartitionTypeInfo  *info)
{
  UDisksPartitionTypeInfo *ret;
  ret = udisks_partition_type_info_new ();
  memcpy (ret, info, sizeof (UDisksPartitionTypeInfo));
  return ret;
}

/**
 * udisks_partition_type_info_free:
 * @info: A #UDisksPartitionTypeInfo.
 *
 * Frees @info.
 */
void
udisks_partition_type_info_free (UDisksPartitionTypeInfo  *info)
{
  g_slice_free (UDisksPartitionTypeInfo, info);
}

G_DEFINE_BOXED_TYPE (UDisksPartitionTypeInfo, udisks_partition_type_info, udisks_partition_type_info_copy, udisks_partition_type_info_free);
