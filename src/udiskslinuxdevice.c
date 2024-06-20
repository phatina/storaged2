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

#include <sys/ioctl.h>
#include <fcntl.h>
#include <linux/cdrom.h>

#include <glib.h>
#include <glib-object.h>

#include "udiskslinuxdevice.h"
#include "udisksprivate.h"
#include "udiskslogging.h"
#include "udisksata.h"
#include "udisksdaemonutil.h"

/**
 * SECTION:udiskslinuxdevice
 * @title: UDisksLinuxDevice
 * @short_description: Low-level devices on Linux
 *
 * Types and functions used to record information obtained from the
 * udev database as well as by probing the device.
 */


typedef struct _UDisksLinuxDeviceClass UDisksLinuxDeviceClass;

struct _UDisksLinuxDeviceClass
{
  GObjectClass parent_class;
};


G_DEFINE_TYPE (UDisksLinuxDevice, udisks_linux_device, G_TYPE_OBJECT);

static void
udisks_linux_device_init (UDisksLinuxDevice *device)
{
}

static void
udisks_linux_device_finalize (GObject *object)
{
  UDisksLinuxDevice *device = UDISKS_LINUX_DEVICE (object);

  g_clear_object (&device->udev_device);
  g_free (device->ata_identify_device_data);
  g_free (device->ata_identify_packet_device_data);
  bd_nvme_controller_info_free (device->nvme_ctrl_info);
  bd_nvme_namespace_info_free (device->nvme_ns_info);

  G_OBJECT_CLASS (udisks_linux_device_parent_class)->finalize (object);
}

static void
udisks_linux_device_class_init (UDisksLinuxDeviceClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize     = udisks_linux_device_finalize;
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean probe_ata (UDisksLinuxDevice  *device,
                           GCancellable       *cancellable,
                           GError            **error);

/**
 * udisks_linux_device_new_sync:
 * @udev_device: A #GUdevDevice.
 *
 * Creates a new #UDisksLinuxDevice from @udev_device which includes
 * probing the device for more information, if applicable.
 *
 * The calling thread may be blocked for a non-trivial amount of time
 * while the probing is underway.
 *
 * Returns: A #UDisksLinuxDevice.
 */
UDisksLinuxDevice *
udisks_linux_device_new_sync (GUdevDevice *udev_device)
{
  UDisksLinuxDevice *device;
  GError *error = NULL;

  g_return_val_if_fail (G_UDEV_IS_DEVICE (udev_device), NULL);

  device = g_object_new (UDISKS_TYPE_LINUX_DEVICE, NULL);
  device->udev_device = g_object_ref (udev_device);

  /* No point in probing on remove events */
  if (!(g_strcmp0 (g_udev_device_get_action (udev_device), "remove") == 0))
    {
      if (!udisks_linux_device_reprobe_sync (device, NULL, &error))
        goto out;
    }

 out:
  if (error != NULL)
    {
      udisks_warning ("Error probing device: %s (%s, %d)",
                      error->message, g_quark_to_string (error->domain), error->code);
      g_clear_error (&error);
    }

  return device;
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_linux_device_reprobe_sync:
 * @device: A #UDisksLinuxDevice.
 * @cancellable: (allow-none): A #GCancellable or %NULL.
 * @error: Return location for error or %NULL.
 *
 * Forcibly reprobe information on @device. The calling thread may be
 * blocked for a non-trivial amount of time while the probing is
 * underway.
 *
 * Returns: %TRUE if reprobing succeeded, %FALSE otherwise.
 */
gboolean
udisks_linux_device_reprobe_sync (UDisksLinuxDevice  *device,
                                  GCancellable       *cancellable,
                                  GError            **error)
{
  gboolean ret = FALSE;
  const gchar *device_file;

  device_file = g_udev_device_get_device_file (device->udev_device);

  /* Get IDENTIFY DEVICE / IDENTIFY PACKET DEVICE data for ATA devices */
  if (g_strcmp0 (g_udev_device_get_subsystem (device->udev_device), "block") == 0 &&
      g_strcmp0 (g_udev_device_get_devtype (device->udev_device), "disk") == 0 &&
      g_udev_device_get_property_as_boolean (device->udev_device, "ID_ATA") &&
      !g_udev_device_has_property (device->udev_device, "ID_USB_TYPE") &&
      !g_udev_device_has_property (device->udev_device, "ID_USB_DRIVER") &&
      !g_udev_device_has_property (device->udev_device, "ID_USB_MODEL"))
    {
      if (!probe_ata (device, cancellable, error))
        goto out;
    }
  else
  /* NVMe controller device */
  if (g_strcmp0 (g_udev_device_get_subsystem (device->udev_device), "nvme") == 0 &&
      g_udev_device_has_sysfs_attr (device->udev_device, "subsysnqn") &&
      g_udev_device_has_property (device->udev_device, "NVME_TRTYPE") &&
      device_file != NULL)
    {
      /* Even though the device node exists and udev has finished probing,
       * the device might not be fully usable at this point. The sysfs
       * attr 'state' indicates actual state with 'live' being the healthy state.
       *
       * Kernel 5.18 will trigger extra uevent once the controller state reaches
       * 'live' with a 'NVME_EVENT=connected' attribute attached:
       *
       *    commit 20d64911e7580f7e29c0086d67860c18307377d7
       *    Author: Martin Belanger <martin.belanger@dell.com>
       *    Date:   Tue Feb 8 14:33:45 2022 -0500
       *
       *    nvme: send uevent on connection up
       *
       * See also kernel drivers/nvme/host/core.c: nvme_sysfs_show_state()
       */

      /* TODO: shall we trigger uevent on all namespaces once NVME_EVENT=connected is received? */
      device->nvme_ctrl_info = bd_nvme_get_controller_info (device_file, error);
      if (!device->nvme_ctrl_info)
        {
          if (error && g_error_matches (*error, BD_NVME_ERROR, BD_NVME_ERROR_BUSY))
            {
              g_clear_error (error);
            }
          else
            goto out;
        }
    }
  else
  /* NVMe namespace block device */
  if (g_strcmp0 (g_udev_device_get_subsystem (device->udev_device), "block") == 0 &&
      g_strcmp0 (g_udev_device_get_devtype (device->udev_device), "disk") == 0 &&
      udisks_linux_device_subsystem_is_nvme (device) &&
      device_file != NULL)
    {
      device->nvme_ns_info = bd_nvme_get_namespace_info (device_file, error);
      if (!device->nvme_ns_info)
        goto out;
    }

  ret = TRUE;

 out:
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
probe_ata (UDisksLinuxDevice  *device,
           GCancellable       *cancellable,
           GError            **error)
{
  const gchar *device_file;
  gboolean ret = FALSE;
  gint fd = -1;
  UDisksAtaCommandInput input = {0};
  UDisksAtaCommandOutput output = {0};

  device_file = g_udev_device_get_device_file (device->udev_device);
  fd = open (device_file, O_RDONLY|O_NONBLOCK);
  if (fd == -1)
    {
      g_set_error (error, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                   "Error opening device file %s while probing ATA specifics: %m",
                   device_file);
      goto out;
    }


  if (ioctl (fd, CDROM_GET_CAPABILITY, NULL) == -1)
    {
      /* ATA8: 7.16 IDENTIFY DEVICE - ECh, PIO Data-In */
      input.command = 0xec;
      input.count = 1;
      output.buffer = g_new0 (guchar, 512);
      output.buffer_size = 512;
      if (!udisks_ata_send_command_sync (fd,
                                         -1,
                                         UDISKS_ATA_COMMAND_PROTOCOL_DRIVE_TO_HOST,
                                         &input,
                                         &output,
                                         error))
        {
          g_free (output.buffer);
          g_prefix_error (error, "Error sending ATA command IDENTIFY DEVICE to '%s': ",
                          device_file);
          goto out;
        }
      g_free (device->ata_identify_device_data);
      device->ata_identify_device_data = output.buffer;
      /* udisks_daemon_util_hexdump_debug (device->ata_identify_device_data, 512); */
    }
  else
    {
      /* ATA8: 7.17 IDENTIFY PACKET DEVICE - A1h, PIO Data-In */
      input.command = 0xa1;
      input.count = 1;
      output.buffer = g_new0 (guchar, 512);
      output.buffer_size = 512;
      if (!udisks_ata_send_command_sync (fd,
                                         -1,
                                         UDISKS_ATA_COMMAND_PROTOCOL_DRIVE_TO_HOST,
                                         &input,
                                         &output,
                                         error))
        {
          g_free (output.buffer);
          g_prefix_error (error, "Error sending ATA command IDENTIFY PACKET DEVICE to '%s': ",
                          device_file);
          goto out;
        }
      g_free (device->ata_identify_packet_device_data);
      device->ata_identify_packet_device_data = output.buffer;
      /* udisks_daemon_util_hexdump_debug (device->ata_identify_packet_device_data, 512); */
    }

  ret = TRUE;

 out:
  if (fd != -1)
    {
      if (close (fd) != 0)
        {
          udisks_warning ("Error closing fd %d for device %s: %m",
                          fd, device_file);
        }
    }
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_linux_device_read_sysfs_attr:
 * @device: A #UDisksLinuxDevice.
 * @attr: Attribute name (a path).
 * @error: Return location for error or %NULL.
 *
 * Read a sysfs attribute within the device sysfs hierarchy.
 * The @attr can be a path relative to the @device base sysfs path.
 *
 * Returns: (transfer full): Attribute contents or %NULL if unavailable. Free with g_free().
 */
gchar *
udisks_linux_device_read_sysfs_attr (UDisksLinuxDevice  *device,
                                     const gchar        *attr,
                                     GError            **error)
{
  gchar *ret = NULL;
  gchar *path;

  g_return_val_if_fail (UDISKS_IS_LINUX_DEVICE (device), NULL);
  g_return_val_if_fail (G_UDEV_IS_DEVICE (device->udev_device), NULL);
  g_return_val_if_fail (attr != NULL, NULL);

  path = g_strdup_printf ("%s/%s", g_udev_device_get_sysfs_path (device->udev_device), attr);
  if (!g_file_get_contents (path, &ret, NULL /* size */, error))
    {
      g_prefix_error (error, "Error reading sysfs attr `%s': ", path);
    }
  else
    {
      /* remove newline from the attribute */
      g_strstrip (ret);
    }
  g_free (path);

  return ret;
}

/**
 * udisks_linux_device_read_sysfs_attr_as_int:
 * @device: A #UDisksLinuxDevice.
 * @attr: Attribute name (a path).
 * @error: Return location for error or %NULL.
 *
 * Read a sysfs attribute within the device sysfs hierarchy.
 * The @attr can be a path relative to the @device base sysfs path.
 *
 * Returns: Numerical attribute value or 0 on error.
 */
gint
udisks_linux_device_read_sysfs_attr_as_int (UDisksLinuxDevice  *device,
                                            const gchar        *attr,
                                            GError            **error)
{
  gint ret = 0;
  gchar *str;

  if ((str = udisks_linux_device_read_sysfs_attr (device, attr, error)))
    ret = atoi (str);
  g_free (str);

  return ret;
}

/**
 * udisks_linux_device_read_sysfs_attr_as_uint64:
 * @device: A #UDisksLinuxDevice.
 * @attr: Attribute name (a path).
 * @error: Return location for error or %NULL.
 *
 * Read a sysfs attribute within the device sysfs hierarchy.
 * The @attr can be a path relative to the @device base sysfs path.
 *
 * Returns: Numerical attribute value or 0 on error.
 */
guint64
udisks_linux_device_read_sysfs_attr_as_uint64 (UDisksLinuxDevice  *device,
                                               const gchar        *attr,
                                               GError            **error)
{
  guint64 ret = 0;
  gchar *str;

  if ((str = udisks_linux_device_read_sysfs_attr (device, attr, error)))
    ret = g_ascii_strtoull (str, NULL, 0);
  g_free (str);

  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_linux_device_subsystem_is_nvme:
 * @device: A #UDisksLinuxDevice.
 *
 * Walks up the device hierarchy and checks if @device is part of a NVMe topology.
 *
 * Returns: %TRUE in case of a NVMe device, %FALSE otherwise.
 */
gboolean
udisks_linux_device_subsystem_is_nvme (UDisksLinuxDevice *device)
{
  GUdevDevice *parent;

  parent = g_object_ref (device->udev_device);
  while (parent)
    {
      const gchar *subsystem;
      GUdevDevice *d;

      subsystem = g_udev_device_get_subsystem (parent);
      if (subsystem && g_str_has_prefix (subsystem, "nvme"))
        {
          g_object_unref (parent);
          return TRUE;
        }
      d = parent;
      parent = g_udev_device_get_parent (d);
      g_object_unref (d);
    }

  return FALSE;
}

/**
 * udisks_linux_device_nvme_is_fabrics:
 * @device: A #UDisksLinuxDevice.
 *
 * Determines whether @device is a NVMe over Fabrics device.
 *
 * Returns: %TRUE in case of a NVMeoF device, %FALSE otherwise.
 */
gboolean
udisks_linux_device_nvme_is_fabrics (UDisksLinuxDevice *device)
{
  const gchar *transport;

  if (!udisks_linux_device_subsystem_is_nvme (device))
    return FALSE;

  transport = g_udev_device_get_sysfs_attr (device->udev_device, "transport");
  /* Consider only 'pcie' local */
  if (g_strcmp0 (transport, "rdma") == 0 ||
      g_strcmp0 (transport, "fc") == 0 ||
      g_strcmp0 (transport, "tcp") == 0 ||
      g_strcmp0 (transport, "loop") == 0)
    return TRUE;

  return FALSE;
}

/**
 * udisks_linux_device_nvme_tnvmcap_supported:
 * @device: A #UDisksLinuxDevice.
 *
 * Determines whether @device supports Capacity information
 * in the Identify Controller data structure.
 *
 * Returns: %TRUE if capacity reporting is supported, %FALSE otherwise.
 */
gboolean
udisks_linux_device_nvme_tnvmcap_supported (UDisksLinuxDevice *device)
{
  if (device->nvme_ctrl_info == NULL)
    return FALSE;

  /* FIXME: find a more reliable way to detect controller
   *        capacity reporting capability.
   */
  return device->nvme_ctrl_info->size_total > 0;
}
