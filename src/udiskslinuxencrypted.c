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

#include <blockdev/crypto.h>

#include <glib/gstdio.h>

#include "udiskslogging.h"
#include "udiskslinuxencrypted.h"
#include "udiskslinuxencryptedhelpers.h"
#include "udiskslinuxblockobject.h"
#include "udisksdaemon.h"
#include "udisksdaemonutil.h"
#include "udisksstate.h"
#include "udiskslinuxdevice.h"
#include "udiskslinuxblock.h"
#include "udisksfstabentry.h"
#include "udiskscrypttabentry.h"
#include "udiskscrypttabmonitor.h"
#include "udisksspawnedjob.h"
#include "udiskssimplejob.h"

#define MAX_TCRYPT_KEYFILES 256

/**
 * SECTION:udiskslinuxencrypted
 * @title: UDisksLinuxEncrypted
 * @short_description: Linux implementation of #UDisksEncrypted
 *
 * This type provides an implementation of the #UDisksEncrypted
 * interface on Linux.
 */

typedef struct _UDisksLinuxEncryptedClass   UDisksLinuxEncryptedClass;

/**
 * UDisksLinuxEncrypted:
 *
 * The #UDisksLinuxEncrypted structure contains only private data and should
 * only be accessed using the provided API.
 */
struct _UDisksLinuxEncrypted
{
  UDisksEncryptedSkeleton parent_instance;
};

struct _UDisksLinuxEncryptedClass
{
  UDisksEncryptedSkeletonClass parent_class;
};

static void encrypted_iface_init (UDisksEncryptedIface *iface);

G_DEFINE_TYPE_WITH_CODE (UDisksLinuxEncrypted, udisks_linux_encrypted, UDISKS_TYPE_ENCRYPTED_SKELETON,
                         G_IMPLEMENT_INTERFACE (UDISKS_TYPE_ENCRYPTED, encrypted_iface_init));

/* ---------------------------------------------------------------------------------------------------- */

static void
udisks_linux_encrypted_init (UDisksLinuxEncrypted *encrypted)
{
  g_dbus_interface_skeleton_set_flags (G_DBUS_INTERFACE_SKELETON (encrypted),
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);
}

static void
udisks_linux_encrypted_class_init (UDisksLinuxEncryptedClass *klass)
{
}

/**
 * udisks_linux_encrypted_new:
 *
 * Creates a new #UDisksLinuxEncrypted instance.
 *
 * Returns: A new #UDisksLinuxEncrypted. Free with g_object_unref().
 */
UDisksEncrypted *
udisks_linux_encrypted_new (void)
{
  return UDISKS_ENCRYPTED (g_object_new (UDISKS_TYPE_LINUX_ENCRYPTED,
                                         NULL));
}

/* ---------------------------------------------------------------------------------------------------- */

static UDisksObject *
wait_for_cleartext_object (UDisksDaemon *daemon,
                           gpointer      user_data)
{
  const gchar *crypto_object_path = user_data;
  UDisksObject *ret = NULL;
  GList *objects, *l;

  objects = udisks_daemon_get_objects (daemon);
  for (l = objects; l != NULL; l = l->next)
    {
      UDisksObject *object = UDISKS_OBJECT (l->data);
      UDisksBlock *block;

      block = udisks_object_get_block (object);
      if (block != NULL)
        {
          if (g_strcmp0 (udisks_block_get_crypto_backing_device (block), crypto_object_path) == 0)
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

static void
update_child_configuration (UDisksLinuxEncrypted   *encrypted,
                            UDisksLinuxBlockObject *object)
{
  UDisksDaemon *daemon = udisks_linux_block_object_get_daemon (object);
  UDisksBlock *block = udisks_object_peek_block (UDISKS_OBJECT (object));

  udisks_encrypted_set_child_configuration
    (UDISKS_ENCRYPTED (encrypted),
     udisks_linux_find_child_configuration (daemon,
                                            udisks_block_get_id_uuid (block)));
}

static void
update_metadata_size (UDisksLinuxEncrypted   *encrypted,
                      UDisksLinuxBlockObject *object)
{
  UDisksLinuxDevice *device;
  BDCryptoLUKSInfo *info = NULL;
  GError *error = NULL;

  device = udisks_linux_block_object_get_device (object);

  info = bd_crypto_luks_info (g_udev_device_get_device_file (device->udev_device),
                              &error);
  if (!info)
    {
      udisks_warning ("Error getting '%s' metadata_size: %s (%s, %d)",
                      g_udev_device_get_device_file (device->udev_device),
                      error->message,
                      g_quark_to_string (error->domain),
                      error->code);
      g_clear_error (&error);
      udisks_encrypted_set_metadata_size (UDISKS_ENCRYPTED (encrypted), 0);
    }
  else
    udisks_encrypted_set_metadata_size (UDISKS_ENCRYPTED (encrypted), info->metadata_size);

  g_object_unref (device);
  bd_crypto_luks_info_free (info);
}

static void
update_cleartext_device (UDisksLinuxEncrypted   *encrypted,
                         UDisksLinuxBlockObject *object)
{
  UDisksObject *cleartext_object = NULL;
  UDisksDaemon *daemon = udisks_linux_block_object_get_daemon (object);
  const gchar *encrypted_path = g_dbus_object_get_object_path (G_DBUS_OBJECT (object));

  /* wait_for_cleartext is used primarily in unlock but does exactly what we
     want -- returns a cleartext object for an encrypted object */
  cleartext_object = wait_for_cleartext_object (daemon, (gpointer) encrypted_path);

  if (cleartext_object)
    {
      udisks_encrypted_set_cleartext_device (UDISKS_ENCRYPTED (encrypted),
                                             g_dbus_object_get_object_path (G_DBUS_OBJECT (cleartext_object)));
      g_object_unref (cleartext_object);
    }
  else
    {
      udisks_encrypted_set_cleartext_device (UDISKS_ENCRYPTED (encrypted), "/");
    }
}

/**
 * udisks_linux_encrypted_update:
 * @encrypted: A #UDisksLinuxEncrypted.
 * @object: The enclosing #UDisksLinuxBlockObject instance.
 *
 * Updates the interface.
 */
void
udisks_linux_encrypted_update (UDisksLinuxEncrypted   *encrypted,
                               UDisksLinuxBlockObject *object)
{
  UDisksBlock *block = udisks_object_peek_block (UDISKS_OBJECT (object));

  udisks_linux_block_encrypted_lock (block);

  update_child_configuration (encrypted, object);
  update_cleartext_device (encrypted, object);

  /* set block type according to hint_encryption_type */
  if (udisks_linux_block_is_unknown_crypto (block))
    {
      if (g_strcmp0 (udisks_encrypted_get_hint_encryption_type (UDISKS_ENCRYPTED (encrypted)), "TCRYPT") == 0)
        udisks_block_set_id_type (block, "crypto_TCRYPT");
    }

  if (udisks_linux_block_is_luks (block))
    update_metadata_size (encrypted, object);

  udisks_linux_block_encrypted_unlock (block);

  g_dbus_interface_skeleton_flush (G_DBUS_INTERFACE_SKELETON (encrypted));
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
check_crypttab (UDisksBlock  *block,
                gboolean      load_passphrase,
                gboolean     *out_found,
                gchar       **out_name,
                gchar       **out_passphrase,
                gsize        *out_passphrase_len,
                gchar       **out_options,
                GError      **error)
{
  gboolean ret = FALSE;
  GVariantIter iter;
  const gchar *type;
  GVariant *details;

  g_variant_iter_init (&iter, udisks_block_get_configuration (block));
  while (g_variant_iter_next (&iter, "(&s@a{sv})", &type, &details))
    {
      if (g_strcmp0 (type, "crypttab") == 0)
        {
          const gchar *passphrase_path;
          if (out_found != NULL)
            *out_found = TRUE;
          g_variant_lookup (details, "name", "^ay", out_name);
          g_variant_lookup (details, "options", "^ay", out_options);
          if (g_variant_lookup (details, "passphrase-path", "^&ay", &passphrase_path) &&
              strlen (passphrase_path) > 0 &&
              !g_str_has_prefix (passphrase_path, "/dev"))
            {
              if (load_passphrase)
                {
                  if (!g_file_get_contents (passphrase_path,
                                            out_passphrase,
                                            out_passphrase_len,
                                            error))
                    {
                      g_variant_unref (details);
                      goto out;
                    }
                }
            }
          ret = TRUE;
          g_variant_unref (details);
          goto out;
        }
      g_variant_unref (details);
    }

  ret = TRUE;

 out:
  return ret;
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

/* ---------------------------------------------------------------------------------------------------- */

/* runs in thread dedicated to handling @invocation */
static gboolean
handle_unlock (UDisksEncrypted        *encrypted,
               GDBusMethodInvocation  *invocation,
               const gchar            *passphrase,
               GVariant               *options)
{
  UDisksObject *object = NULL;
  UDisksBlock *block;
  UDisksDaemon *daemon;
  UDisksState *state = NULL;
  gchar *name = NULL;
  UDisksObject *cleartext_object = NULL;
  UDisksBlock *cleartext_block;
  UDisksLinuxDevice *cleartext_device = NULL;
  GError *error = NULL;
  uid_t caller_uid;
  const gchar *action_id;
  const gchar *message;
  gboolean is_in_crypttab = FALSE;
  gchar *crypttab_name = NULL;
  gchar *crypttab_passphrase = NULL;
  gsize crypttab_passphrase_len = 0;
  gchar *crypttab_options = NULL;
  gchar *device = NULL;
  gchar *old_hint_encryption_type = NULL;
  gboolean read_only = FALSE;
  gboolean is_hidden = FALSE;
  gboolean is_system = FALSE;
  guint32 pim = 0;
  GString *effective_passphrase = NULL;
  GVariant *keyfiles_variant = NULL;
  const gchar *keyfiles[MAX_TCRYPT_KEYFILES] = {};
  CryptoJobData data;
  gboolean is_luks;
  gboolean is_bitlk;
  gboolean handle_as_tcrypt;
  void *open_func;
  const gchar *uuid = NULL;
  const gchar *label = NULL;

  object = udisks_daemon_util_dup_object (encrypted, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  block = udisks_object_peek_block (object);
  daemon = udisks_linux_block_object_get_daemon (UDISKS_LINUX_BLOCK_OBJECT (object));
  state = udisks_daemon_get_state (daemon);
  is_luks = udisks_linux_block_is_luks (block);
  is_bitlk = udisks_linux_block_is_bitlk (block);
  handle_as_tcrypt = udisks_linux_block_is_tcrypt (block) || udisks_linux_block_is_unknown_crypto (block);

  udisks_linux_block_object_lock_for_cleanup (UDISKS_LINUX_BLOCK_OBJECT (object));
  udisks_state_check_block (state, udisks_linux_block_object_get_device_number (UDISKS_LINUX_BLOCK_OBJECT (object)));

  /* get TCRYPT options */
  if (handle_as_tcrypt)
    {
      g_variant_lookup (options, "hidden", "b", &is_hidden);
      g_variant_lookup (options, "system", "b", &is_system);
      g_variant_lookup (options, "pim", "u", &pim);

      /* get keyfiles */
      keyfiles_variant = g_variant_lookup_value(options, "keyfiles", G_VARIANT_TYPE_ARRAY);
      if (keyfiles_variant)
        {
          GVariantIter iter;
          const gchar *path;
          uint i = 0;

          g_variant_iter_init (&iter, keyfiles_variant);
          while (g_variant_iter_next (&iter, "&s", &path) && i < MAX_TCRYPT_KEYFILES)
            {
              keyfiles[i] = path;
              i++;
            }
        }
    }

  /* TODO: check if the device is mentioned in /etc/crypttab (see crypttab(5)) - if so use that
   *
   *       Of course cryptsetup(8) don't support that, see https://bugzilla.redhat.com/show_bug.cgi?id=692258
   */

  /* Fail if the device is not a LUKS or possible TCRYPT device */
  if (!(is_luks || is_bitlk || handle_as_tcrypt))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Device %s does not appear to be a LUKS, BITLK or TCRYPT device",
                                             udisks_block_get_device (block));
      goto out;
    }

  /* Fail if device is already unlocked */
  cleartext_object = udisks_daemon_wait_for_object_sync (daemon,
                                                         wait_for_cleartext_object,
                                                         g_strdup (g_dbus_object_get_object_path (G_DBUS_OBJECT (object))),
                                                         g_free,
                                                         0, /* timeout_seconds */
                                                         NULL); /* error */
  if (cleartext_object != NULL)
    {
      UDisksBlock *unlocked_block;
      unlocked_block = udisks_object_peek_block (cleartext_object);
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Device %s is already unlocked as %s",
                                             udisks_block_get_device (block),
                                             udisks_block_get_device (unlocked_block));
      goto out;
    }

  /* we need the uid of the caller for the unlocked-crypto-dev file */
  if (!udisks_daemon_util_get_caller_uid_sync (daemon, invocation, NULL /* GCancellable */, &caller_uid, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      g_clear_error (&error);
      goto out;
    }

  /* check if in crypttab file */
  if (!check_crypttab (block,
                       TRUE,
                       &is_in_crypttab,
                       &crypttab_name,
                       &crypttab_passphrase,
                       &crypttab_passphrase_len,
                       &crypttab_options,
                       &error))
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  /* fallback mechanism: keyfile_contents (for LUKS and BITLK) -> passphrase -> crypttab_passphrase -> TCRYPT keyfiles -> error (no key) */
  if ((is_luks || is_bitlk) && udisks_variant_lookup_binary (options, "keyfile_contents", &effective_passphrase))
    {
      /* effective_passphrase was set to keyfile_contents, nothing more to do here */
    }
  else if (passphrase && (strlen (passphrase) > 0))
    effective_passphrase = g_string_new (passphrase);
  else if (is_in_crypttab && crypttab_passphrase != NULL && crypttab_passphrase_len > 0)
    effective_passphrase = g_string_new_len (crypttab_passphrase, crypttab_passphrase_len);
  else if (keyfiles[0] != NULL)
    effective_passphrase = g_string_new (NULL);
  else
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "No key available to unlock device %s",
                                             udisks_block_get_device (block));
      goto out;
    }

  /* Now, check that the user is actually authorized to unlock the device.
   */
  action_id = "org.freedesktop.udisks2.encrypted-unlock";
  /* Translators: Shown in authentication dialog when the user
   * requests unlocking an encrypted device.
   *
   * Do not translate $(drive), it's a placeholder and
   * will be replaced by the name of the drive/device in question
   */
  message = N_("Authentication is required to unlock the encrypted device $(drive)");
  if (!udisks_daemon_util_setup_by_user (daemon, object, caller_uid))
    {
      if (is_in_crypttab && has_option (crypttab_options, "x-udisks-auth"))
        {
          action_id = "org.freedesktop.udisks2.encrypted-unlock-crypttab";
        }
      else if (udisks_block_get_hint_system (block))
        {
          action_id = "org.freedesktop.udisks2.encrypted-unlock-system";
        }
      else if (!udisks_daemon_util_on_user_seat (daemon, object, caller_uid))
        {
          action_id = "org.freedesktop.udisks2.encrypted-unlock-other-seat";
        }
    }

  if (!udisks_daemon_util_check_authorization_sync (daemon,
                                                    object,
                                                    action_id,
                                                    options,
                                                    message,
                                                    invocation))
    goto out;

  /* calculate the name to use */
  if (is_in_crypttab && crypttab_name != NULL)
    name = g_strdup (crypttab_name);
  else {
    label = udisks_block_get_id_label (block);
    if (label)
      name = g_strdup (label);
    else
      {
        if (is_luks)
          name = g_strdup_printf ("luks-%s", udisks_block_get_id_uuid (block));
        else if (is_bitlk)
          {
            uuid = udisks_block_get_id_uuid (block);
            if (uuid && g_strcmp0 (uuid, "") != 0)
              name = g_strdup_printf ("bitlk-%s", uuid);
            else
              name = g_strdup_printf ("bitlk-%" G_GUINT64_FORMAT, udisks_block_get_device_number (block));
          }
        else
          /* TCRYPT devices don't have a UUID, so we use the device number instead */
          name = g_strdup_printf ("tcrypt-%" G_GUINT64_FORMAT, udisks_block_get_device_number (block));
      }

  }

  /* save old encryption type to be able to restore it */
  old_hint_encryption_type = udisks_encrypted_dup_hint_encryption_type (encrypted);

  /* Set hint_encryption type. We have to do this before the
   * actual unlock, in order to have this set before the device
   * update triggered by the unlock. */
  if (is_luks)
    udisks_encrypted_set_hint_encryption_type (encrypted, "LUKS");
  else if (is_bitlk)
    udisks_encrypted_set_hint_encryption_type (encrypted, "BITLK");
  else
    udisks_encrypted_set_hint_encryption_type (encrypted, "TCRYPT");

  device = udisks_block_dup_device (block);

  /* unlock as read-only if specified in @options or if the device itself is read-only */
  g_variant_lookup (options, "read-only", "b", &read_only);
  if (udisks_block_get_read_only (block))
    read_only = TRUE;

  data.device = device;
  data.map_name = name;
  data.passphrase = effective_passphrase;
  data.keyfiles = keyfiles;
  data.pim = pim;
  data.hidden = is_hidden;
  data.system = is_system;
  data.read_only = read_only;

  if (is_luks)
    open_func = luks_open_job_func;
  else if (is_bitlk)
    open_func = bitlk_open_job_func;
  else
    open_func = tcrypt_open_job_func;

  udisks_linux_block_encrypted_lock (block);
  if (!udisks_daemon_launch_threaded_job_sync (daemon,
                                               object,
                                               "encrypted-unlock",
                                               caller_uid,
                                               open_func,
                                               &data,
                                               NULL, /* user_data_free_func */
                                               NULL, /* cancellable */
                                               &error))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Error unlocking %s: %s",
                                             udisks_block_get_device (block),
                                             error->message);
      g_clear_error (&error);

      /* Restore the old encryption type if the unlock failed, because
       * in this case we don't know for sure if we used the correct
       * encryption type. */
      udisks_encrypted_set_hint_encryption_type (encrypted, old_hint_encryption_type);
      udisks_linux_block_encrypted_unlock (block);
      goto out;
    }

  udisks_linux_block_encrypted_unlock (block);

  /* Determine the resulting cleartext object */
  cleartext_object = udisks_daemon_wait_for_object_sync (daemon,
                                                         wait_for_cleartext_object,
                                                         g_strdup (g_dbus_object_get_object_path (G_DBUS_OBJECT (object))),
                                                         g_free,
                                                         UDISKS_DEFAULT_WAIT_TIMEOUT,
                                                         &error);
  if (cleartext_object == NULL)
    {
      g_prefix_error (&error,
                      "Error waiting for cleartext object after unlocking '%s': ",
                      udisks_block_get_device (block));
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }
  cleartext_block = udisks_object_peek_block (cleartext_object);

  udisks_notice ("Unlocked device %s as %s",
                 udisks_block_get_device (block),
                 udisks_block_get_device (cleartext_block));

  cleartext_device = udisks_linux_block_object_get_device (UDISKS_LINUX_BLOCK_OBJECT (cleartext_object));

  /* update the unlocked-crypto-dev file */
  udisks_state_add_unlocked_crypto_dev (state,
                                        udisks_block_get_device_number (cleartext_block),
                                        udisks_block_get_device_number (block),
                                        g_udev_device_get_sysfs_attr (cleartext_device->udev_device, "dm/uuid"),
                                        caller_uid);

  /* ensure property changes are sent before the method return */
  g_dbus_interface_skeleton_flush (G_DBUS_INTERFACE_SKELETON (encrypted));

  udisks_encrypted_complete_unlock (encrypted,
                                    invocation,
                                    g_dbus_object_get_object_path (G_DBUS_OBJECT (cleartext_object)));

 out:
  if (object != NULL)
    udisks_linux_block_object_release_cleanup_lock (UDISKS_LINUX_BLOCK_OBJECT (object));
  if (state != NULL)
    udisks_state_check (state);
  g_free (device);
  g_free (crypttab_name);
  g_free (crypttab_passphrase);
  g_free (crypttab_options);
  g_free (name);
  g_free (old_hint_encryption_type);
  if (keyfiles_variant)
    g_variant_unref (keyfiles_variant);
  g_clear_object (&cleartext_device);
  g_clear_object (&cleartext_object);
  g_clear_object (&object);
  udisks_string_wipe_and_free (effective_passphrase);

  return TRUE; /* returning TRUE means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

gboolean
udisks_linux_encrypted_lock (UDisksLinuxEncrypted   *encrypted,
                             GDBusMethodInvocation  *invocation,
                             GVariant               *options,
                             GError                **error)
{
  UDisksObject *object = NULL;
  UDisksBlock *block = NULL;
  UDisksDaemon *daemon = NULL;
  UDisksState *state = NULL;
  UDisksObject *cleartext_object = NULL;
  UDisksBlock *cleartext_block = NULL;
  UDisksLinuxDevice *device = NULL;
  uid_t unlocked_by_uid;
  dev_t cleartext_device_from_file;
  uid_t caller_uid;
  gboolean ret;
  CryptoJobData data;
  GError *loc_error = NULL;
  gchar *cleartext_path = NULL;
  void *close_func;
  gboolean is_luks;
  gboolean is_bitlk;
  gboolean handle_as_tcrypt;

  object = udisks_daemon_util_dup_object (encrypted, error);
  if (object == NULL)
    {
      ret = FALSE;
      goto out;
    }

  block = udisks_object_peek_block (object);
  daemon = udisks_linux_block_object_get_daemon (UDISKS_LINUX_BLOCK_OBJECT (object));
  state = udisks_daemon_get_state (daemon);
  is_luks = udisks_linux_block_is_luks (block);
  is_bitlk = udisks_linux_block_is_bitlk (block);
  handle_as_tcrypt = udisks_linux_block_is_tcrypt (block) || udisks_linux_block_is_unknown_crypto (block);

  /* TODO: check if the device is mentioned in /etc/crypttab (see crypttab(5)) - if so use that
   *
   *       Of course cryptsetup(8) don't support that, see https://bugzilla.redhat.com/show_bug.cgi?id=692258
   */

  /* Fail if the device is not a LUKS or possible TCRYPT device */
  if (!(is_luks || is_bitlk || handle_as_tcrypt))
    {
      g_set_error (error,
                   UDISKS_ERROR,
                   UDISKS_ERROR_FAILED,
                   "Device %s does not appear to be a LUKS, BITLK or TCRYPT device",
                   udisks_block_get_device (block));
      ret = FALSE;
      goto out;
    }

  /* Fail if device is not unlocked */
  cleartext_object = udisks_daemon_wait_for_object_sync (daemon,
                                                         wait_for_cleartext_object,
                                                         g_strdup (g_dbus_object_get_object_path (G_DBUS_OBJECT (object))),
                                                         g_free,
                                                         0, /* timeout_seconds */
                                                         NULL); /* error */
  if (cleartext_object == NULL)
    {
      g_set_error (error,
                   UDISKS_ERROR,
                   UDISKS_ERROR_FAILED,
                   "Device %s is not unlocked",
                   udisks_block_get_device (block));
      ret = FALSE;
      goto out;
    }
  cleartext_block = udisks_object_peek_block (cleartext_object);

  cleartext_device_from_file = udisks_state_find_unlocked_crypto_dev (state,
                                                                      udisks_block_get_device_number (block),
                                                                      &unlocked_by_uid);
  if (cleartext_device_from_file == 0)
    {
      /* allow locking stuff not mentioned in unlocked-crypto-dev, but treat it like root unlocked it */
      unlocked_by_uid = 0;
    }

  /* we need the uid of the caller to check authorization */
  if (!udisks_daemon_util_get_caller_uid_sync (daemon,
                                               invocation,
                                               NULL /* GCancellable */,
                                               &caller_uid,
                                               error))
    {
      ret = FALSE;
      goto out;
    }

  /* Check that the user is authorized to lock the device - if he
   * already unlocked it, he is implicitly authorized...
   */
  if (caller_uid != 0 && (caller_uid != unlocked_by_uid))
    {
      if (!udisks_daemon_util_check_authorization_sync_with_error (daemon,
                                                                   object,
                                                                   "org.freedesktop.udisks2.encrypted-lock-others",
                                                                   options,
                                                                   /* Translators: Shown in authentication dialog when the user
                                                                    * requests locking an encrypted device that was previously.
                                                                    * unlocked by another user.
                                                                    *
                                                                    * Do not translate $(drive), it's a placeholder and
                                                                    * will be replaced by the name of the drive/device in question
                                                                    */
                                                                   N_("Authentication is required to lock the encrypted device $(drive) unlocked by another user"),
                                                                   invocation,
                                                                   error))
        {
          ret = FALSE;
          goto out;
        }
    }

  device = udisks_linux_block_object_get_device (UDISKS_LINUX_BLOCK_OBJECT (cleartext_object));
  data.map_name = (gchar *) g_udev_device_get_sysfs_attr (device->udev_device, "dm/name");

  if (is_luks)
    close_func = luks_close_job_func;
  else if (is_bitlk)
    close_func = bitlk_close_job_func;
  else
    close_func = tcrypt_close_job_func;

  udisks_linux_block_encrypted_lock (block);
  if (!udisks_daemon_launch_threaded_job_sync (daemon,
                                               object,
                                               "encrypted-lock",
                                               caller_uid,
                                               close_func,
                                               &data,
                                               NULL, /* user_data_free_func */
                                               NULL, /* cancellable */
                                               &loc_error))
    {
      g_set_error (error,
                   UDISKS_ERROR,
                   UDISKS_ERROR_FAILED,
                   "Error locking %s (%s): %s",
                   udisks_block_get_device (cleartext_block),
                   udisks_block_get_device (block),
                   loc_error->message);
      ret = FALSE;
      g_clear_error (&loc_error);
      udisks_linux_block_encrypted_unlock (block);
      goto out;
    }

  udisks_linux_block_encrypted_unlock (block);

  cleartext_path = g_strdup (g_dbus_object_get_object_path (G_DBUS_OBJECT (object)));
  if (! udisks_daemon_wait_for_object_to_disappear_sync (daemon,
                                                         wait_for_cleartext_object,
                                                         cleartext_path,
                                                         NULL,
                                                         UDISKS_DEFAULT_WAIT_TIMEOUT,
                                                         &loc_error))
    {
      g_set_error (error,
                   UDISKS_ERROR,
                   UDISKS_ERROR_FAILED,
                   "Error waiting for cleartext object to disappear after locking the device: %s",
                   loc_error->message);
      g_clear_error (&loc_error);
      ret = FALSE;
      goto out;
    }

  udisks_notice ("Locked device %s (was unlocked as %s)",
                 udisks_block_get_device (block),
                 udisks_block_get_device (cleartext_block));
  ret = TRUE;

 out:
  if (device != NULL)
    g_object_unref (device);
  if (cleartext_object != NULL)
    g_object_unref (cleartext_object);
  g_clear_object (&object);
  g_free (cleartext_path);

  return ret;
}

/* runs in thread dedicated to handling @invocation */
static gboolean
handle_lock (UDisksEncrypted        *encrypted,
             GDBusMethodInvocation  *invocation,
             GVariant               *options)
{
  GError *error = NULL;
  UDisksObject *object = NULL;
  UDisksDaemon *daemon = NULL;
  UDisksState *state = NULL;

  object = udisks_daemon_util_dup_object (encrypted, NULL);
  if (object != NULL)
    {
      daemon = udisks_linux_block_object_get_daemon (UDISKS_LINUX_BLOCK_OBJECT (object));
      state = udisks_daemon_get_state (daemon);
      udisks_linux_block_object_lock_for_cleanup (UDISKS_LINUX_BLOCK_OBJECT (object));
      udisks_state_check_block (state, udisks_linux_block_object_get_device_number (UDISKS_LINUX_BLOCK_OBJECT (object)));
    }

  if (!udisks_linux_encrypted_lock (UDISKS_LINUX_ENCRYPTED (encrypted), invocation, options, &error))
    g_dbus_method_invocation_take_error (invocation, error);
  else
    udisks_encrypted_complete_lock (encrypted, invocation);

  if (object != NULL)
    udisks_linux_block_object_release_cleanup_lock (UDISKS_LINUX_BLOCK_OBJECT (object));
  if (state != NULL)
    udisks_state_check (state);
  g_clear_object (&object);

  return TRUE; /* returning TRUE means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

/* runs in thread dedicated to handling @invocation */
static gboolean
handle_change_passphrase (UDisksEncrypted        *encrypted,
                          GDBusMethodInvocation  *invocation,
                          const gchar            *passphrase,
                          const gchar            *new_passphrase,
                          GVariant               *options)
{
  UDisksObject *object = NULL;
  UDisksBlock *block;
  UDisksDaemon *daemon;
  UDisksState *state = NULL;
  uid_t caller_uid;
  const gchar *action_id;
  GError *error = NULL;
  gchar *device = NULL;
  CryptoJobData data = { NULL, NULL, NULL, NULL, NULL, 0, 0, FALSE, FALSE, FALSE, NULL };

  object = udisks_daemon_util_dup_object (encrypted, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  block = udisks_object_peek_block (object);
  daemon = udisks_linux_block_object_get_daemon (UDISKS_LINUX_BLOCK_OBJECT (object));
  state = udisks_daemon_get_state (daemon);

  udisks_linux_block_object_lock_for_cleanup (UDISKS_LINUX_BLOCK_OBJECT (object));
  udisks_state_check_block (state, udisks_linux_block_object_get_device_number (UDISKS_LINUX_BLOCK_OBJECT (object)));

  /* TODO: check if the device is mentioned in /etc/crypttab (see crypttab(5)) - if so use that
   *
   *       Of course cryptsetup(8) don't support that, see https://bugzilla.redhat.com/show_bug.cgi?id=692258
   */

  /* Fail if the device is not a LUKS device (changing passphrase is currently not supported for TCRYPT devices) */
  if (!udisks_linux_block_is_luks (block))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Device %s does not appear to be a LUKS device",
                                             udisks_block_get_device (block));
      goto out;
    }

  if (!udisks_daemon_util_get_caller_uid_sync (daemon, invocation, NULL /* GCancellable */, &caller_uid, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      g_clear_error (&error);
      goto out;
    }

  /* Now, check that the user is actually authorized to unlock the device.
   */
  action_id = "org.freedesktop.udisks2.encrypted-change-passphrase";
  if (udisks_block_get_hint_system (block) &&
      !(udisks_daemon_util_setup_by_user (daemon, object, caller_uid)))
    action_id = "org.freedesktop.udisks2.encrypted-change-passphrase-system";
  /*  if (is_in_crypttab)
        action_id = "org.freedesktop.udisks2.encrypted-unlock-crypttab";  */
  if (!udisks_daemon_util_check_authorization_sync (daemon,
                                                    object,
                                                    action_id,
                                                    options,
                                                    /* Translators: Shown in authentication dialog when the user
                                                     * requests unlocking an encrypted device.
                                                     *
                                                     * Do not translate $(drive), it's a placeholder and
                                                     * will be replaced by the name of the drive/device in question
                                                     */
                                                    N_("Authentication is required to unlock the encrypted device $(drive)"),
                                                    invocation))
    goto out;

  device = udisks_block_dup_device (block);
  data.device = device;

  /* handle keyfiles */
  if (!udisks_variant_lookup_binary (options, "old_keyfile_contents",
                                     &(data.passphrase)))
    data.passphrase = g_string_new (passphrase);
  if (!udisks_variant_lookup_binary (options, "new_keyfile_contents",
                                     &(data.new_passphrase)))
    data.new_passphrase = g_string_new (new_passphrase);

  udisks_linux_block_encrypted_lock (block);
  if (!udisks_daemon_launch_threaded_job_sync (daemon,
                                               object,
                                               "encrypted-modify",
                                               caller_uid,
                                               luks_change_key_job_func,
                                               &data,
                                               NULL, /* user_data_free_func */
                                               NULL, /* cancellable */
                                               &error))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Error changing passphrase on device %s: %s",
                                             udisks_block_get_device (block),
                                             error->message);
      g_clear_error (&error);
      udisks_linux_block_encrypted_unlock (block);
      goto out;
    }

  udisks_linux_block_encrypted_unlock (block);

  udisks_encrypted_complete_change_passphrase (encrypted, invocation);

 out:
  if (object != NULL)
    udisks_linux_block_object_release_cleanup_lock (UDISKS_LINUX_BLOCK_OBJECT (object));
  if (state != NULL)
    udisks_state_check (state);
  g_free (device);
  udisks_string_wipe_and_free (data.passphrase);
  udisks_string_wipe_and_free (data.new_passphrase);
  g_clear_object (&object);

  return TRUE; /* returning TRUE means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

/* runs in thread dedicated to handling method call */
static gboolean
handle_resize (UDisksEncrypted       *encrypted,
               GDBusMethodInvocation *invocation,
               guint64                size,
               GVariant              *options)
{
  UDisksObject *object = NULL;
  UDisksBlock *block;
  UDisksObject *cleartext_object = NULL;
  UDisksBlock *cleartext_block;
  UDisksDaemon *daemon;
  UDisksState *state = NULL;
  uid_t caller_uid;
  const gchar *action_id = NULL;
  const gchar *message = NULL;
  GError *error = NULL;
  UDisksBaseJob *job = NULL;
  GString *effective_passphrase = NULL;
  BDCryptoKeyslotContext *context = NULL;

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

  /* Fail if the device is not a LUKS device */
  if (!(g_strcmp0 (udisks_block_get_id_usage (block), "crypto") == 0 &&
        g_strcmp0 (udisks_block_get_id_type (block), "crypto_LUKS") == 0))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Device %s does not appear to be a LUKS device",
                                             udisks_block_get_device (block));
      goto out;
    }

  if (!udisks_daemon_util_get_caller_uid_sync (daemon, invocation, NULL /* GCancellable */, &caller_uid, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      goto out;
    }

  /* Fail if device is not unlocked */
  cleartext_object = udisks_daemon_wait_for_object_sync (daemon,
                                                         wait_for_cleartext_object,
                                                         g_strdup (g_dbus_object_get_object_path (G_DBUS_OBJECT (object))),
                                                         g_free,
                                                         0, /* timeout_seconds */
                                                         NULL); /* error */
  if (cleartext_object == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Device %s is not unlocked",
                                             udisks_block_get_device (block));
      goto out;
    }
  cleartext_block = udisks_object_peek_block (cleartext_object);

  action_id = "org.freedesktop.udisks2.modify-device";
  /* Translators: Shown in authentication dialog when the user
   * requests resizing a encrypted block device.
   *
   * Do not translate $(drive), it's a placeholder and
   * will be replaced by the name of the drive/device in question
   */
  message = N_("Authentication is required to resize the encrypted device $(drive)");
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

  /* Check that the user is actually authorized to resize the device. */
  if (! udisks_daemon_util_check_authorization_sync (daemon,
                                                     object,
                                                     action_id,
                                                     options,
                                                     message,
                                                     invocation))
    goto out;

  if (udisks_variant_lookup_binary (options, "keyfile_contents", &effective_passphrase))
    ;
  else if (udisks_variant_lookup_binary (options, "passphrase", &effective_passphrase))
    ;
  else
    effective_passphrase = NULL;

  if (effective_passphrase)
    {
      context = bd_crypto_keyslot_context_new_passphrase ((const guint8 *) effective_passphrase->str,
                                                          effective_passphrase->len,
                                                          &error);
      if (!context)
        {
          g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Error resizing encrypted device %s: %s",
                                             udisks_block_get_device (cleartext_block),
                                             error->message);
          goto out;
        }
    }

  job = udisks_daemon_launch_simple_job (daemon,
                                         UDISKS_OBJECT (object),
                                         "encrypted-resize",
                                         caller_uid,
                                         NULL);
  if (job == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                             "Failed to create a job object");
      goto out;
    }

  udisks_linux_block_encrypted_lock (block);

  /* TODO: implement progress parsing for udisks_job_set_progress(_valid) */
  if (! bd_crypto_luks_resize (udisks_block_get_device (cleartext_block),
                               size / 512,
                               context,
                               &error))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Error resizing encrypted device %s: %s",
                                             udisks_block_get_device (cleartext_block),
                                             error->message);
      udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), FALSE, error->message);
      udisks_linux_block_encrypted_unlock (block);
      goto out;
    }

  udisks_linux_block_encrypted_unlock (block);

  udisks_encrypted_complete_resize (encrypted, invocation);
  udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), TRUE, NULL);

 out:
  if (object != NULL)
    udisks_linux_block_object_release_cleanup_lock (UDISKS_LINUX_BLOCK_OBJECT (object));
  if (state != NULL)
    udisks_state_check (state);
  g_clear_object (&cleartext_object);
  g_clear_object (&object);
  g_clear_error (&error);
  udisks_string_wipe_and_free (effective_passphrase);
  bd_crypto_keyslot_context_free (context);
  return TRUE; /* returning TRUE means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

/* runs in thread dedicated to handling method call */
static gboolean
handle_convert (UDisksEncrypted       *encrypted,
                GDBusMethodInvocation *invocation,
                const gchar           *target_version,
                GVariant              *options)
{
  UDisksObject *object = NULL;
  UDisksBlock *block;
  UDisksDaemon *daemon;
  UDisksState *state = NULL;
  uid_t caller_uid;
  const gchar *action_id = NULL;
  const gchar *message = NULL;
  GError *error = NULL;
  UDisksBaseJob *job = NULL;
  BDCryptoLUKSVersion bd_target_version;

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

  /* Fail if the device is not a LUKS device */
  if (!(g_strcmp0 (udisks_block_get_id_usage (block), "crypto") == 0 &&
        g_strcmp0 (udisks_block_get_id_type (block), "crypto_LUKS") == 0))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Device %s does not appear to be a LUKS device",
                                             udisks_block_get_device (block));
      goto out;
    }

  if (!udisks_daemon_util_get_caller_uid_sync (daemon, invocation, NULL /* GCancellable */, &caller_uid, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      goto out;
    }

  action_id = "org.freedesktop.udisks2.modify-device";
  /* Translators: Shown in authentication dialog when the user
   * requests conversion of an encrypted block device.
   *
   * Do not translate $(drive), it's a placeholder and
   * will be replaced by the name of the drive/device in question
   */
  message = N_("Authentication is required to convert device $(drive) to a different LUKS version.");
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

  /* Check that the user is actually authorized to convert the device. */
  if (! udisks_daemon_util_check_authorization_sync (daemon,
                                                     object,
                                                     action_id,
                                                     options,
                                                     message,
                                                     invocation))
    goto out;

  if (g_strcmp0 (target_version, "luks1") == 0) {
      bd_target_version = BD_CRYPTO_LUKS_VERSION_LUKS1;
    } else if (g_strcmp0 (target_version, "luks2") == 0) {
      bd_target_version = BD_CRYPTO_LUKS_VERSION_LUKS2;
    } else {
      g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                             "Unsupported target LUKS version: '%s'. Only 'luks1' and 'luks2' are supported.",
                                             target_version);
      goto out;
    }

  job = udisks_daemon_launch_simple_job (daemon,
                                         UDISKS_OBJECT (object),
                                         "encrypted-convert",
                                         caller_uid,
                                         NULL);
  if (job == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                             "Failed to create a job object");
      goto out;
    }

  udisks_linux_block_encrypted_lock (block);

  if (! bd_crypto_luks_convert (udisks_block_get_device (block),
                                bd_target_version,
                                &error))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Error converting encrypted device %s: %s",
                                             udisks_block_get_device (block),
                                             error->message);
      udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), FALSE, error->message);
      udisks_linux_block_encrypted_unlock (block);
      goto out;
    }

  udisks_linux_block_encrypted_unlock (block);

  udisks_encrypted_complete_convert (encrypted, invocation);
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

/* runs in thread dedicated to handling method call */
static gboolean
handle_header_backup (UDisksEncrypted       *encrypted,
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

    /* Fail if the device is not a LUKS device */
    if (!(g_strcmp0 (udisks_block_get_id_usage (block), "crypto") == 0 &&
          g_strcmp0 (udisks_block_get_id_type (block), "crypto_LUKS") == 0))
      {
        g_dbus_method_invocation_return_error (invocation,
                                               UDISKS_ERROR,
                                               UDISKS_ERROR_FAILED,
                                               "Device %s does not appear to be a LUKS device",
                                               udisks_block_get_device (block));
        goto out;
      }

    if (!udisks_daemon_util_get_caller_uid_sync (daemon, invocation, NULL /* GCancellable */, &caller_uid, &error))
      {
        g_dbus_method_invocation_return_gerror (invocation, error);
        goto out;
      }

    job = udisks_daemon_launch_simple_job (daemon,
                                           UDISKS_OBJECT (object),
                                           "encrypted-header-backup",
                                           caller_uid,
                                           NULL);
    if (job == NULL)
      {
        g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                               "Failed to create a job object");
        goto out;
      }

    udisks_linux_block_encrypted_lock (block);

    if (!bd_crypto_luks_header_backup (udisks_block_get_device (block), backup_file, &error))
      {
        g_dbus_method_invocation_return_error (invocation,
                                               UDISKS_ERROR,
                                               UDISKS_ERROR_FAILED,
                                               "Error backing up header of encrypted device %s: %s",
                                               udisks_block_get_device (block),
                                               error->message);
        udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), FALSE, error->message);
        udisks_linux_block_encrypted_unlock (block);
        goto out;
      }

    udisks_linux_block_encrypted_unlock (block);

    udisks_encrypted_complete_header_backup (encrypted, invocation);
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

/* runs in thread dedicated to handling @invocation */
static gboolean
handle_reencrypt (UDisksEncrypted        *encrypted,
                  GDBusMethodInvocation  *invocation,
                  const gchar            *passphrase,
                  GVariant               *options)
{
  UDisksObject *object = NULL;
  UDisksBlock *block;
  UDisksDaemon *daemon;
  UDisksState *state = NULL;
  UDisksObject *cleartext_object = NULL;
  UDisksBlock *unlocked_block = NULL;
  uid_t caller_uid;
  GError *error = NULL;
  UDisksBaseJob *job = NULL;

  const gchar *device;
  BDCryptoKeyslotContext *context = NULL;
  guint32 key_size;
  gchar *cipher;
  gchar *cipher_mode;
  gchar *resilience;
  gchar *hash;
  guint64 max_hotzone_size;
  guint32 sector_size;
  gboolean new_volume_key;
  gboolean offline;
  gchar *pbkdf_type;
  BDCryptoLUKSPBKDF *pbkdf = NULL;
  BDCryptoLUKSReencryptParams *params = NULL;


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

  /* Fail if the device is not a LUKS2 device */
  if (!(g_strcmp0 (udisks_block_get_id_usage (block), "crypto") == 0 &&
        g_strcmp0 (udisks_block_get_id_type (block), "crypto_LUKS") == 0 &&
        g_strcmp0 (udisks_block_get_id_version (block), "2") == 0)) // not that future-proof I guess, haha
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Device %s does not appear to be a LUKS2 device",
                                             udisks_block_get_device (block));
      goto out;
    }

  if (!udisks_daemon_util_get_caller_uid_sync (daemon, invocation, NULL /* GCancellable */, &caller_uid, &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      goto out;
    }

  job = udisks_daemon_launch_simple_job (daemon,
                                         UDISKS_OBJECT (object),
                                         "encrypted-reencrypt",
                                         caller_uid,
                                         NULL);
  if (job == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                             "Failed to create a job object");
      goto out;
    }

  udisks_linux_block_encrypted_lock (block);

  // do stuff here
  cleartext_object = udisks_daemon_wait_for_object_sync (daemon,
                                                         wait_for_cleartext_object,
                                                         g_strdup (g_dbus_object_get_object_path (G_DBUS_OBJECT (object))),
                                                         g_free,
                                                         0, /* timeout_seconds */
                                                         NULL); /* error */

  if (cleartext_object == NULL)
    {
      offline = TRUE;
      device = udisks_block_get_device (block);
    }
  else
    {
      offline = FALSE;
      unlocked_block = udisks_object_peek_block (cleartext_object);
      device = udisks_block_get_device (unlocked_block);
    }

  context = bd_crypto_keyslot_context_new_passphrase ((const guint8 *) passphrase,
                                                      strlen(passphrase),
                                                      &error);
  if (!context)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Error reencrypting encrypted device %s: %s",
                                             device,
                                             error->message);
      udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), FALSE, error->message);
      udisks_linux_block_encrypted_unlock (block);
      goto out;
    }

  g_variant_lookup (options, "key-size", "u", &key_size);
  g_variant_lookup (options, "cipher", "&s", &cipher);
  g_variant_lookup (options, "cipher-mode", "&s", &cipher_mode);
  g_variant_lookup (options, "resilience", "&s", &resilience);
  g_variant_lookup (options, "hash", "&s", &hash);
  g_variant_lookup (options, "max-hotzone-size", "u", &max_hotzone_size);
  g_variant_lookup (options, "sector-size", "u", &sector_size);
  g_variant_lookup (options, "new-volume_key", "b", &new_volume_key);
  // `offline` is already determined
  g_variant_lookup (options, "pbkdf-type", "&s", &pbkdf_type);

  pbkdf = bd_crypto_luks_pbkdf_new (pbkdf_type, NULL, 0, 0, 0, 0);
  params = bd_crypto_luks_reencrypt_params_new (key_size, cipher, cipher_mode, resilience, hash, max_hotzone_size, sector_size, new_volume_key, offline, pbkdf);

  if (! bd_crypto_luks_reencrypt (device, params, context, NULL /* prog_func -- TODO */, &error))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Error reencrypting encrypted device %s: %s",
                                             device,
                                             error->message);
      udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), FALSE, error->message);
      udisks_linux_block_encrypted_unlock (block);
      goto out;
    }

  //
  udisks_linux_block_encrypted_unlock (block);

  udisks_encrypted_complete_reencrypt (encrypted, invocation);
  udisks_simple_job_complete (UDISKS_SIMPLE_JOB (job), TRUE, NULL);

 out:
  bd_crypto_luks_pbkdf_free (pbkdf);
  bd_crypto_luks_reencrypt_params_free (params);
  bd_crypto_keyslot_context_free (context);
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
encrypted_iface_init (UDisksEncryptedIface *iface)
{
  iface->handle_unlock              = handle_unlock;
  iface->handle_lock                = handle_lock;
  iface->handle_change_passphrase   = handle_change_passphrase;
  iface->handle_resize              = handle_resize;
  iface->handle_convert             = handle_convert;
  iface->handle_header_backup       = handle_header_backup;
  iface->handle_reencrypt           = handle_reencrypt;
}
