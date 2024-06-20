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
#include <glib.h>
#include <glib/gi18n-lib.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <linux/bsg.h>
#include <scsi/scsi.h>
#include <scsi/sg.h>
#include <scsi/scsi_ioctl.h>

#include <pwd.h>
#include <grp.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <mntent.h>

#include <glib/gstdio.h>

#include "udiskslogging.h"
#include "udiskslinuxprovider.h"
#include "udiskslinuxdriveobject.h"
#include "udiskslinuxdrive.h"
#include "udiskslinuxblockobject.h"
#include "udisksdaemon.h"
#include "udisksdaemonutil.h"
#include "udiskslinuxdevice.h"
#include "udisksconfigmanager.h"

/**
 * SECTION:udiskslinuxdrive
 * @title: UDisksLinuxDrive
 * @short_description: Linux implementation of #UDisksDrive
 *
 * This type provides an implementation of the #UDisksDrive interface
 * on Linux.
 */

typedef struct _UDisksLinuxDriveClass   UDisksLinuxDriveClass;

/**
 * UDisksLinuxDrive:
 *
 * The #UDisksLinuxDrive structure contains only private data and should
 * only be accessed using the provided API.
 */
struct _UDisksLinuxDrive
{
  UDisksDriveSkeleton parent_instance;

  gint64 time_detected;
  gint64 time_media_detected;
  gchar *sort_key;
};

struct _UDisksLinuxDriveClass
{
  UDisksDriveSkeletonClass parent_class;
};

static void drive_iface_init (UDisksDriveIface *iface);

G_DEFINE_TYPE_WITH_CODE (UDisksLinuxDrive, udisks_linux_drive, UDISKS_TYPE_DRIVE_SKELETON,
                         G_IMPLEMENT_INTERFACE (UDISKS_TYPE_DRIVE, drive_iface_init));

/* ---------------------------------------------------------------------------------------------------- */

static void
udisks_linux_drive_finalize (GObject *object)
{
  UDisksLinuxDrive *drive = UDISKS_LINUX_DRIVE (object);

  g_free (drive->sort_key);

  if (G_OBJECT_CLASS (udisks_linux_drive_parent_class)->finalize != NULL)
    G_OBJECT_CLASS (udisks_linux_drive_parent_class)->finalize (object);
}

static void
udisks_linux_drive_init (UDisksLinuxDrive *drive)
{
  g_dbus_interface_skeleton_set_flags (G_DBUS_INTERFACE_SKELETON (drive),
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);
}

static void
udisks_linux_drive_class_init (UDisksLinuxDriveClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = G_OBJECT_CLASS (klass);
  gobject_class->finalize     = udisks_linux_drive_finalize;
}

/**
 * udisks_linux_drive_new:
 *
 * Creates a new #UDisksLinuxDrive instance.
 *
 * Returns: A new #UDisksLinuxDrive. Free with g_object_unref().
 */
UDisksDrive *
udisks_linux_drive_new (void)
{
  return UDISKS_DRIVE (g_object_new (UDISKS_TYPE_LINUX_DRIVE,
                                          NULL));
}

/* ---------------------------------------------------------------------------------------------------- */

static void
set_id (UDisksDrive *iface)
{
  GString *id;
  const gchar *vendor;
  const gchar *model;
  const gchar *serial;
  const gchar *wwn;
  guint n;

  vendor = udisks_drive_get_vendor (iface);
  model = udisks_drive_get_model (iface);
  serial = udisks_drive_get_serial (iface);
  wwn = udisks_drive_get_wwn (iface);

  id = g_string_new (NULL);

  /* VENDOR-MODEL-[SERIAL|WWN] */

  if (vendor != NULL && strlen (vendor) > 0)
    {
      g_string_append (id, vendor);
    }
  if (model != NULL && strlen (model) > 0)
    {
      if (id->len > 0)
        g_string_append_c (id, '-');
      g_string_append (id, model);
    }

  if (serial != NULL && strlen (serial) > 0)
    {
      if (id->len > 0)
        g_string_append_c (id, '-');
      g_string_append (id, serial);
    }
  else if (wwn != NULL && strlen (wwn) > 0)
    {
      if (id->len > 0)
        g_string_append_c (id, '-');
      g_string_append (id, wwn);
    }
  else
    {
      g_string_set_size (id, 0);
    }

  for (n = 0; n < id->len; n++)
    {
      if (id->str[n] == '/' || id->str[n] == ' ')
        id->str[n] = '-';
    }

  udisks_drive_set_id (iface, id->str);

  g_string_free (id, TRUE);
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
_g_variant_equal0 (GVariant *a, GVariant *b)
{
  gboolean ret = FALSE;
  if (a == NULL && b == NULL)
    {
      ret = TRUE;
      goto out;
    }
  if (a == NULL || b == NULL)
    goto out;
  ret = g_variant_equal (a, b);
out:
  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

typedef struct {
  const gchar *asv_key;
  const gchar *group;
  const gchar *key;
  const GVariantType *type;
} VariantKeyfileMapping;

static const VariantKeyfileMapping drive_configuration_mapping[5] = {
  {"ata-pm-standby",             "ATA", "StandbyTimeout",       G_VARIANT_TYPE_INT32},
  {"ata-apm-level",              "ATA", "APMLevel",             G_VARIANT_TYPE_INT32},
  {"ata-aam-level",              "ATA", "AAMLevel",             G_VARIANT_TYPE_INT32},
  {"ata-write-cache-enabled",    "ATA", "WriteCacheEnabled",    G_VARIANT_TYPE_BOOLEAN},
  {"ata-read-lookahead-enabled", "ATA", "ReadLookaheadEnabled", G_VARIANT_TYPE_BOOLEAN},
};

/* ---------------------------------------------------------------------------------------------------- */

static gchar *
configuration_get_path (UDisksLinuxDrive *drive,
                        UDisksDaemon     *daemon)
{
  UDisksConfigManager *config_manager;
  const gchar *id;
  gchar *id_config_file;
  gchar *path;

  config_manager = udisks_daemon_get_config_manager (daemon);

  id = udisks_drive_get_id (UDISKS_DRIVE (drive));
  if (id == NULL || strlen (id) == 0)
    return NULL;

  id_config_file = g_strdup_printf ("%s.conf", id);
  path = g_build_filename (udisks_config_manager_get_config_dir (config_manager),
                           id_config_file,
                           NULL);
  g_free (id_config_file);

  return path;
}

/* returns TRUE if configuration changed */
static gboolean
update_configuration (UDisksLinuxDrive       *drive,
                      UDisksLinuxDriveObject *object)
{
  UDisksDaemon *daemon;
  GKeyFile *key_file = NULL;
  gboolean ret = FALSE;
  gchar *path = NULL;
  GError *error = NULL;
  GVariant *value = NULL;
  GVariantBuilder builder;
  GVariant *old_value;
  guint n;

  daemon = udisks_linux_drive_object_get_daemon (object);

  path = configuration_get_path (drive, daemon);
  if (path == NULL)
    goto out;

  key_file = g_key_file_new ();
  if (!g_key_file_load_from_file (key_file,
                                  path,
                                  G_KEY_FILE_NONE,
                                  &error))
    {
      if (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        {
          udisks_critical ("Error loading drive config file: %s (%s, %d)",
                        error->message, g_quark_to_string (error->domain), error->code);
        }
      g_clear_error (&error);
      goto out;
    }

  g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
  for (n = 0; n < G_N_ELEMENTS (drive_configuration_mapping); n++)
    {
      const VariantKeyfileMapping *mapping = &drive_configuration_mapping[n];

      if (!g_key_file_has_key (key_file, mapping->group, mapping->key, NULL))
        continue;

      if (g_variant_type_equal (mapping->type, G_VARIANT_TYPE_INT32))
        {
          gint32 int_value = g_key_file_get_integer (key_file, mapping->group, mapping->key, &error);
          if (error != NULL)
            {
              udisks_critical ("Error parsing int32 key %s in group %s in drive config file %s: %s (%s, %d)",
                            mapping->key, mapping->group, path,
                            error->message, g_quark_to_string (error->domain), error->code);
              g_clear_error (&error);
            }
          else
            {
              g_variant_builder_add (&builder, "{sv}", mapping->asv_key, g_variant_new_int32 (int_value));
            }
        }
      else if (g_variant_type_equal (mapping->type, G_VARIANT_TYPE_BOOLEAN))
        {
          gboolean bool_value = g_key_file_get_boolean (key_file, mapping->group, mapping->key, &error);
          if (error != NULL)
            {
              udisks_critical ("Error parsing boolean key %s in group %s in drive config file %s: %s (%s, %d)",
                            mapping->key, mapping->group, path,
                            error->message, g_quark_to_string (error->domain), error->code);
              g_clear_error (&error);
            }
          else
            {
              g_variant_builder_add (&builder, "{sv}", mapping->asv_key, g_variant_new_boolean (bool_value));
            }
        }
      else
        {
          g_assert_not_reached ();
        }
    }

  value = g_variant_ref_sink (g_variant_builder_end (&builder));

 out:
  g_free (path);

  old_value = udisks_drive_get_configuration (UDISKS_DRIVE (drive));
  if (!_g_variant_equal0 (old_value, value))
    ret = TRUE;
  udisks_drive_set_configuration (UDISKS_DRIVE (drive), value);

  if (key_file != NULL)
    g_key_file_free (key_file);
  if (value != NULL)
    g_variant_unref (value);

  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

static const struct
{
  const gchar *udev_property;
  const gchar *media_name;
  gboolean force_non_removable;
  gboolean force_removable;
} drive_media_mapping[] =
{
  { "ID_DRIVE_THUMB", "thumb", TRUE, FALSE },
  { "ID_DRIVE_FLASH", "flash", FALSE, TRUE },
  { "ID_DRIVE_FLASH_CF", "flash_cf", FALSE, TRUE },
  { "ID_DRIVE_FLASH_MS", "flash_ms", FALSE, TRUE },
  { "ID_DRIVE_FLASH_SM", "flash_sm", FALSE, TRUE },
  { "ID_DRIVE_FLASH_SD", "flash_sd", FALSE, TRUE },
  { "ID_DRIVE_FLASH_SDHC", "flash_sdhc", FALSE, TRUE },
  { "ID_DRIVE_FLASH_SDXC", "flash_sdxc", FALSE, TRUE },
  { "ID_DRIVE_FLASH_SDIO", "flash_sdio", FALSE, TRUE },
  { "ID_DRIVE_FLASH_SD_COMBO", "flash_sd_combo", FALSE, TRUE },
  { "ID_DRIVE_FLASH_MMC", "flash_mmc", TRUE, FALSE },
  { "ID_DRIVE_FLOPPY", "floppy", FALSE, TRUE },
  { "ID_DRIVE_FLOPPY_ZIP", "floppy_zip", FALSE, TRUE },
  { "ID_DRIVE_FLOPPY_JAZ", "floppy_jaz", FALSE, TRUE },
  { "ID_CDROM", "optical_cd", FALSE, TRUE },
  { "ID_CDROM_CD_R", "optical_cd_r", FALSE, TRUE },
  { "ID_CDROM_CD_RW", "optical_cd_rw", FALSE, TRUE },
  { "ID_CDROM_DVD", "optical_dvd", FALSE, TRUE },
  { "ID_CDROM_DVD_R", "optical_dvd_r", FALSE, TRUE },
  { "ID_CDROM_DVD_RW", "optical_dvd_rw", FALSE, TRUE },
  { "ID_CDROM_DVD_RAM", "optical_dvd_ram", FALSE, TRUE },
  { "ID_CDROM_DVD_PLUS_R", "optical_dvd_plus_r", FALSE, TRUE },
  { "ID_CDROM_DVD_PLUS_RW", "optical_dvd_plus_rw", FALSE, TRUE },
  { "ID_CDROM_DVD_PLUS_R_DL", "optical_dvd_plus_r_dl", FALSE, TRUE },
  { "ID_CDROM_DVD_PLUS_RW_DL", "optical_dvd_plus_rw_dl", FALSE, TRUE },
  { "ID_CDROM_BD", "optical_bd", FALSE, TRUE },
  { "ID_CDROM_BD_R", "optical_bd_r", FALSE, TRUE },
  { "ID_CDROM_BD_RE", "optical_bd_re", FALSE, TRUE },
  { "ID_CDROM_HDDVD", "optical_hddvd", FALSE, TRUE },
  { "ID_CDROM_HDDVD_R", "optical_hddvd_r", FALSE, TRUE },
  { "ID_CDROM_HDDVD_RW", "optical_hddvd_rw", FALSE, TRUE },
  { "ID_CDROM_MO", "optical_mo", FALSE, TRUE },
  { "ID_CDROM_MRW", "optical_mrw", FALSE, TRUE },
  { "ID_CDROM_MRW_W", "optical_mrw_w", FALSE, TRUE },
  { NULL, NULL, FALSE, FALSE }
};

static const struct
{
  const gchar *udev_property;
  const gchar *media_name;
} media_mapping[] =
{
  { "ID_DRIVE_MEDIA_FLASH", "flash" },
  { "ID_DRIVE_MEDIA_FLASH_CF", "flash_cf" },
  { "ID_DRIVE_MEDIA_FLASH_MS", "flash_ms" },
  { "ID_DRIVE_MEDIA_FLASH_SM", "flash_sm" },
  { "ID_DRIVE_MEDIA_FLASH_SD", "flash_sd" },
  { "ID_DRIVE_MEDIA_FLASH_SDHC", "flash_sdhc" },
  { "ID_DRIVE_MEDIA_FLASH_SDXC", "flash_sdxc" },
  { "ID_DRIVE_MEDIA_FLASH_SDIO", "flash_sdio" },
  { "ID_DRIVE_MEDIA_FLASH_MMC", "flash_mmc" },
  { "ID_DRIVE_MEDIA_FLOPPY", "floppy" },
  { "ID_DRIVE_MEDIA_FLOPPY_ZIP", "floppy_zip" },
  { "ID_DRIVE_MEDIA_FLOPPY_JAZ", "floppy_jaz" },
  { "ID_CDROM_MEDIA_CD", "optical_cd" },
  { "ID_CDROM_MEDIA_CD_R", "optical_cd_r" },
  { "ID_CDROM_MEDIA_CD_RW", "optical_cd_rw" },
  { "ID_CDROM_MEDIA_DVD", "optical_dvd" },
  { "ID_CDROM_MEDIA_DVD_R", "optical_dvd_r" },
  { "ID_CDROM_MEDIA_DVD_RW", "optical_dvd_rw" },
  { "ID_CDROM_MEDIA_DVD_RAM", "optical_dvd_ram" },
  { "ID_CDROM_MEDIA_DVD_PLUS_R", "optical_dvd_plus_r" },
  { "ID_CDROM_MEDIA_DVD_PLUS_RW", "optical_dvd_plus_rw" },
  { "ID_CDROM_MEDIA_DVD_PLUS_R_DL", "optical_dvd_plus_r_dl" },
  { "ID_CDROM_MEDIA_DVD_PLUS_RW_DL", "optical_dvd_plus_rw_dl" },
  { "ID_CDROM_MEDIA_BD", "optical_bd" },
  { "ID_CDROM_MEDIA_BD_R", "optical_bd_r" },
  { "ID_CDROM_MEDIA_BD_RE", "optical_bd_re" },
  { "ID_CDROM_MEDIA_HDDVD", "optical_hddvd" },
  { "ID_CDROM_MEDIA_HDDVD_R", "optical_hddvd_r" },
  { "ID_CDROM_MEDIA_HDDVD_RW", "optical_hddvd_rw" },
  { "ID_CDROM_MEDIA_MO", "optical_mo" },
  { "ID_CDROM_MEDIA_MRW", "optical_mrw" },
  { "ID_CDROM_MEDIA_MRW_W", "optical_mrw_w" },
  { NULL, NULL }
};

static gint
ptr_str_array_compare (const gchar **a,
                       const gchar **b)
{
  return g_strcmp0 (*a, *b);
}

static gboolean
find_removable (GUdevDevice *device)
{
  GUdevDevice *parent;

  parent = g_object_ref (device);
  while (parent)
  {
    GUdevDevice *d;
    const gchar * const *removable;
    removable = g_udev_device_get_sysfs_attr_as_strv (parent, "removable");
    if (removable && removable[0])
    {
      if (g_strcmp0 (removable[0], "removable") == 0)
      {
        g_object_unref (parent);
        return TRUE;
      }
    }

    d = parent;
    parent = g_udev_device_get_parent (d);
    g_object_unref (d);
  }

  return FALSE;
}

static void
set_media (UDisksDrive       *iface,
           UDisksLinuxDevice *device,
           gboolean           is_pc_floppy_drive)
{
  guint n;
  GPtrArray *media_compat_array;
  const gchar *media_in_drive;
  gboolean is_disc = FALSE;
  gboolean disc_is_blank = FALSE;
  guint disc_session_count = 0;
  guint disc_track_count = 0;
  guint disc_track_count_audio = 0;
  guint disc_track_count_data = 0;
  gboolean force_non_removable = FALSE;
  gboolean force_removable = FALSE;
  gboolean ejectable = FALSE;
  gboolean removable = FALSE;

  media_compat_array = g_ptr_array_new ();
  for (n = 0; drive_media_mapping[n].udev_property != NULL; n++)
    {
      if (g_udev_device_get_property_as_boolean (device->udev_device, drive_media_mapping[n].udev_property))
        {
          g_ptr_array_add (media_compat_array, (gpointer) drive_media_mapping[n].media_name);
          if (drive_media_mapping[n].force_non_removable)
            force_non_removable = TRUE;
          if (drive_media_mapping[n].force_removable)
            force_removable = TRUE;
        }
    }
  g_ptr_array_sort (media_compat_array, (GCompareFunc) ptr_str_array_compare);
  g_ptr_array_add (media_compat_array, NULL);

  if (udisks_linux_device_subsystem_is_nvme (device))
    {
      removable = find_removable (device->udev_device);
      ejectable = FALSE;
    }
  else
    {
      removable = ejectable = g_udev_device_get_sysfs_attr_as_boolean (device->udev_device, "removable");
    }

  if (force_non_removable)
    removable = FALSE;
  if (force_removable)
    removable = TRUE;
  udisks_drive_set_media_removable (iface, removable);
  if (is_pc_floppy_drive)
    ejectable = FALSE;
  /* MMC/SD: disrespect the sysfs 'removable' attr as it usually doesn't reflect reality */
  if (g_str_has_prefix (g_udev_device_get_name (device->udev_device), "mmcblk"))
    ejectable = removable;
  udisks_drive_set_ejectable (iface, ejectable);

  media_in_drive = NULL;
  if (udisks_drive_get_media_available (iface))
    {
      for (n = 0; media_mapping[n].udev_property != NULL; n++)
        {
          if (g_udev_device_get_property_as_boolean (device->udev_device, media_mapping[n].udev_property))
            {
              media_in_drive = media_mapping[n].media_name;
              break;
            }
        }
      /* If the media isn't set (from e.g. udev rules), just pick the first one in media_compat - note
       * that this may be NULL (if we don't know what media is compatible with the drive) which is OK.
       */
      if (media_in_drive == NULL)
        media_in_drive = ((const gchar **) media_compat_array->pdata)[0];
    }
  udisks_drive_set_media_compatibility (iface, (const gchar* const *) media_compat_array->pdata);
  udisks_drive_set_media (iface, media_in_drive);
  g_ptr_array_free (media_compat_array, TRUE);

  if (g_udev_device_get_property_as_boolean (device->udev_device, "ID_CDROM_MEDIA"))
    {
      const gchar *state;
      is_disc = TRUE;
      state = g_udev_device_get_property (device->udev_device, "ID_CDROM_MEDIA_STATE");
      if (g_strcmp0 (state, "blank") == 0)
        disc_is_blank = TRUE;
      disc_session_count = g_udev_device_get_property_as_int (device->udev_device, "ID_CDROM_MEDIA_SESSION_COUNT");
      disc_track_count = g_udev_device_get_property_as_int (device->udev_device, "ID_CDROM_MEDIA_TRACK_COUNT");
      disc_track_count_audio = g_udev_device_get_property_as_int (device->udev_device, "ID_CDROM_MEDIA_TRACK_COUNT_AUDIO");
      disc_track_count_data = g_udev_device_get_property_as_int (device->udev_device, "ID_CDROM_MEDIA_TRACK_COUNT_DATA");
    }
  udisks_drive_set_optical (iface, is_disc);
  udisks_drive_set_optical_blank (iface, disc_is_blank);
  udisks_drive_set_optical_num_sessions (iface, disc_session_count);
  udisks_drive_set_optical_num_tracks (iface, disc_track_count);
  udisks_drive_set_optical_num_audio_tracks (iface, disc_track_count_audio);
  udisks_drive_set_optical_num_data_tracks (iface, disc_track_count_data);
}

static void
set_rotation_rate (UDisksDrive       *iface,
                   UDisksLinuxDevice *device)
{
  gint rate;

  if (!g_udev_device_get_sysfs_attr_as_boolean (device->udev_device, "queue/rotational"))
    {
      rate = 0;
    }
  else
    {
      rate = -1;
      if (device->ata_identify_device_data != NULL)
        {
          guint word_217 = 0;

          /* ATA8: 7.16 IDENTIFY DEVICE - ECh, PIO Data-In - Table 29 IDENTIFY DEVICE data
           *
           * Table 37 - Nominal Media Rotation Rate:
           *
           *  0000h        Rate not reported
           *  0001h        Non-rotating media (e.g., solid state device)
           *  0002h-0400h  Reserved
           *  0401h-FFFEh  Nominal media rotation rate in rotations per minute (rpm) (e.g., 7200 rpm = 1C20h)
           *  FFFFh        Reserved
           */
          word_217 = udisks_ata_identify_get_word (device->ata_identify_device_data, 217);
          if (word_217 == 0x0001)
            rate = 0;
          else if (word_217 >= 0x0401 && word_217 <= 0xfffe)
            rate = word_217;
        }
    }
  udisks_drive_set_rotation_rate (iface, rate);
}

static void
set_connection_bus (UDisksDrive       *iface,
                    UDisksLinuxDevice *device)
{
  GUdevDevice *parent;
  gboolean can_power_off = FALSE;
  gchar *sibling_id = NULL;

  /* note: @device may vary - it can be any path for drive */

  udisks_drive_set_connection_bus (iface, "");
  parent = g_udev_device_get_parent_with_subsystem (device->udev_device, "usb", "usb_interface");
  if (parent != NULL)
    {
      /* TODO: should probably check that it's a storage interface */
      udisks_drive_set_connection_bus (iface, "usb");
      sibling_id = g_strdup (g_udev_device_get_sysfs_path (parent));
      g_object_unref (parent);
      can_power_off = TRUE;
      goto out;
    }

  parent = g_udev_device_get_parent_with_subsystem (device->udev_device, "firewire", NULL);
  if (parent != NULL)
    {
      /* TODO: should probably check that it's a storage interface */
      udisks_drive_set_connection_bus (iface, "ieee1394");
      g_object_unref (parent);
      goto out;
    }

  if (g_str_has_prefix (g_udev_device_get_name (device->udev_device), "mmcblk"))
    {
      udisks_drive_set_connection_bus (iface, "sdio");
      goto out;
    }

 out:
  /* Make it possible to override if a drive can be powered off */
  if (g_udev_device_has_property (device->udev_device, "UDISKS_CAN_POWER_OFF"))
    can_power_off = g_udev_device_get_property_as_boolean (device->udev_device, "UDISKS_CAN_POWER_OFF");
  udisks_drive_set_can_power_off (iface, can_power_off);
  udisks_drive_set_sibling_id (iface, sibling_id);
  g_free (sibling_id);
}

static void
set_media_time_detected (UDisksLinuxDrive  *drive,
                         UDisksLinuxDevice *device,
                         gboolean           is_pc_floppy_drive,
                         gboolean           coldplug)
{
  UDisksDrive *iface = UDISKS_DRIVE (drive);
  gint64 now;

  now = g_get_real_time ();

  /* First, initialize time_detected */
  if (drive->time_detected == 0)
    {
      if (coldplug)
        {
          drive->time_detected = now - g_udev_device_get_usec_since_initialized (device->udev_device);
        }
      else
        {
          drive->time_detected = now;
        }
    }

  if (!g_udev_device_get_sysfs_attr_as_boolean (device->udev_device, "removable") || is_pc_floppy_drive)
    {
      drive->time_media_detected = drive->time_detected;
    }
  else
    {
      if (!udisks_drive_get_media_available (iface))
        {
          /* no media currently available */
          drive->time_media_detected = 0;
        }
      else
        {
          /* media currently available */
          if (drive->time_media_detected == 0)
            {
              if (coldplug)
                {
                  drive->time_media_detected = drive->time_detected;
                }
              else
                {
                  drive->time_media_detected = now;
                }
            }
        }
    }

  udisks_drive_set_time_detected (iface, drive->time_detected);
  udisks_drive_set_time_media_detected (iface, drive->time_media_detected);
}

static gchar *
append_fixedup_sd (const gchar *prefix,
                   const gchar *device_name)
{
  guint num_alphas, n;
  GString *str;

  g_return_val_if_fail (g_str_has_prefix (device_name, "sd"), NULL);

  /* make sure sdaa comes after e.g. sdz by inserting up to 5 '_' characters
   * between sd and a in sda...
   */
  for (num_alphas = 0; g_ascii_isalpha (device_name[num_alphas + 2]); num_alphas++)
    ;
  str = g_string_new (prefix);
  g_string_append (str, "sd");
  for (n = 0; n < 5 - num_alphas; n++)
    g_string_append_c (str, '_');

  g_string_append (str, device_name + 2);

  return g_string_free (str, FALSE);
}

/**
 * udisks_linux_drive_update:
 * @drive: A #UDisksLinuxDrive.
 * @object: The enclosing #UDisksLinuxDriveObject instance.
 *
 * Updates the interface.
 *
 * Returns: %TRUE if configuration has changed, %FALSE otherwise.
 */
gboolean
udisks_linux_drive_update (UDisksLinuxDrive       *drive,
                           UDisksLinuxDriveObject *object)
{
  gboolean ret = FALSE;
  UDisksDrive *iface = UDISKS_DRIVE (drive);
  UDisksLinuxDevice *device = NULL;
  const gchar *serial = NULL;
  guint64 size = 0;
  gboolean media_available = TRUE;
  gboolean media_change_detected = TRUE;
  gboolean is_pc_floppy_drive = FALSE;
  gboolean removable_hint = FALSE;
  UDisksDaemon *daemon;
  UDisksLinuxProvider *provider;
  gboolean coldplug = FALSE;
  const gchar *seat;

  if (object == NULL)
      goto out;

  device = udisks_linux_drive_object_get_device (object, TRUE /* get_hw */);
  if (device == NULL)
    goto out;

  daemon = udisks_linux_drive_object_get_daemon (object);
  provider = udisks_daemon_get_linux_provider (daemon);
  coldplug = udisks_linux_provider_get_coldplug (provider);

  if (g_udev_device_get_property_as_boolean (device->udev_device, "ID_DRIVE_FLOPPY") ||
      g_str_has_prefix (g_udev_device_get_name (device->udev_device), "fd"))
    is_pc_floppy_drive = TRUE;

  /* this is the _almost_ the same for both ATA and SCSI devices (cf. udev's ata_id and scsi_id)
   * but we special case since there are subtle differences...
   */
  if (g_udev_device_get_property_as_boolean (device->udev_device, "ID_ATA"))
    {
      const gchar *model;

      model = g_udev_device_get_property (device->udev_device, "ID_MODEL_ENC");
      if (model != NULL)
        {
          gchar *s;
          s = udisks_decode_udev_string (model,
                                         g_udev_device_get_property (device->udev_device, "ID_MODEL"));
          g_strstrip (s);
          udisks_drive_set_model (iface, s);
          g_free (s);
        }

      udisks_drive_set_vendor (iface, "");
      udisks_drive_set_revision (iface, g_udev_device_get_property (device->udev_device, "ID_REVISION"));
      udisks_drive_set_wwn (iface, g_udev_device_get_property (device->udev_device, "ID_WWN_WITH_EXTENSION"));
    }
  else if (g_udev_device_get_property_as_boolean (device->udev_device, "ID_SCSI"))
    {
      const gchar *vendor;
      const gchar *model;

      vendor = g_udev_device_get_property (device->udev_device, "ID_VENDOR_ENC");
      if (vendor != NULL)
        {
          gchar *s;
          s = udisks_decode_udev_string (vendor,
                                         g_udev_device_get_property (device->udev_device, "ID_VENDOR"));
          g_strstrip (s);
          udisks_drive_set_vendor (iface, s);
          g_free (s);
        }

      model = g_udev_device_get_property (device->udev_device, "ID_MODEL_ENC");
      if (model != NULL)
        {
          gchar *s;
          s = udisks_decode_udev_string (model,
                                         g_udev_device_get_property (device->udev_device, "ID_MODEL"));
          g_strstrip (s);
          udisks_drive_set_model (iface, s);
          g_free (s);
        }

      udisks_drive_set_revision (iface, g_udev_device_get_property (device->udev_device, "ID_REVISION"));
      serial = g_udev_device_get_property (device->udev_device, "ID_SCSI_SERIAL");
      udisks_drive_set_wwn (iface, g_udev_device_get_property (device->udev_device, "ID_WWN_WITH_EXTENSION"));
    }
  else if (g_str_has_prefix (g_udev_device_get_name (device->udev_device), "mmcblk"))
    {
      /* sigh, mmc is non-standard and using ID_NAME instead of ID_MODEL.. */
      udisks_drive_set_model (iface, g_udev_device_get_property (device->udev_device, "ID_NAME"));
      /* TODO:
       *  - lookup Vendor from manfid and oemid in sysfs
       *  - lookup Revision from fwrev and hwrev in sysfs
       */
    }
  else if (udisks_linux_device_subsystem_is_nvme (device))
    {
      gchar *model = NULL;
      gchar *firmware = NULL;

      serial = g_udev_device_get_sysfs_attr (device->udev_device, "serial");
      model = g_strdup (g_udev_device_get_sysfs_attr (device->udev_device, "model"));
      firmware = g_strdup (g_udev_device_get_sysfs_attr (device->udev_device, "firmware_rev"));

      /* Since we issue the 'NVMe Identify Controller' command to the drive which
       * is an actual ioctl and the drive may be busy or in a temporary offline state,
       * leaving us with missing data, it's better to leave old information around
       * instead of clearing to blank.
       */
      if (device->nvme_ctrl_info)
        {
          /* TODO: decode nvme_ctrl_info's Vendor ID */
          /* TODO: rotation rate - add support for NVMe Log Page – Rotational Media Information Log to libblockdev first */
          if (model == NULL || strlen (model) == 0)
            {
              g_free (model);
              model = g_strdup (device->nvme_ctrl_info->model_number);
            }
          if (firmware == NULL || strlen (firmware) == 0)
            {
              g_free (firmware);
              firmware = g_strdup (device->nvme_ctrl_info->firmware_ver);
            }
          if (device->nvme_ctrl_info->serial_number && strlen (device->nvme_ctrl_info->serial_number) > 0)
            serial = device->nvme_ctrl_info->serial_number;
          size = device->nvme_ctrl_info->size_total;
          /* WWN is namespace specific, leaving unset */
        }
      else
        {
          udisks_debug ("Missing probed NVMe controller information for %s",
                        g_udev_device_get_sysfs_path (device->udev_device));
        }
      if (model)
        {
          g_strstrip (model);
          udisks_drive_set_model (iface, model);
          g_free (model);
        }
      if (firmware)
        {
          g_strstrip (firmware);
          udisks_drive_set_revision (iface, firmware);
          g_free (firmware);
        }
    }
  else
    {
      const gchar *vendor;
      const gchar *model;
      const gchar *name;

      name = g_udev_device_get_name (device->udev_device);

      /* generic fallback... */
      vendor = g_udev_device_get_property (device->udev_device, "ID_VENDOR_ENC");
      if (vendor != NULL)
        {
          gchar *s;
          s = udisks_decode_udev_string (vendor,
                                         g_udev_device_get_property (device->udev_device, "ID_VENDOR"));
          g_strstrip (s);
          udisks_drive_set_vendor (iface, s);
          g_free (s);
        }
      else
        {
          vendor = g_udev_device_get_property (device->udev_device, "ID_VENDOR");
          if (vendor != NULL)
            {
              udisks_drive_set_vendor (iface, vendor);
            }
          /* workaround for missing ID_VENDOR for floppy drives */
          else if (is_pc_floppy_drive)
            {
              udisks_drive_set_vendor (iface, "");
            }
          /* workaround for missing ID_VENDOR on virtio-blk */
          else if (g_str_has_prefix (name, "vd"))
            {
              /* TODO: could lookup the vendor sysfs attr on the virtio object */
              udisks_drive_set_vendor (iface, "");
            }
        }

      model = g_udev_device_get_property (device->udev_device, "ID_MODEL_ENC");
      if (model != NULL)
        {
          gchar *s;
          s = udisks_decode_udev_string (model,
                                         g_udev_device_get_property (device->udev_device, "ID_MODEL"));
          g_strstrip (s);
          udisks_drive_set_model (iface, s);
          g_free (s);
        }
      else
        {
          model = g_udev_device_get_property (device->udev_device, "ID_MODEL");
          if (model != NULL)
            {
              udisks_drive_set_model (iface, model);
            }
          /* workaround for missing ID_MODEL for floppy drives */
          else if (g_str_has_prefix (name, "fd"))
            {
              udisks_drive_set_model (iface, "Floppy Drive");
            }
          /* workaround for missing ID_MODEL on virtio-blk */
          else if (g_str_has_prefix (name, "vd"))
            {
              udisks_drive_set_model (iface, "VirtIO Disk");
            }
        }

      udisks_drive_set_revision (iface, g_udev_device_get_property (device->udev_device, "ID_REVISION"));
      if (g_udev_device_has_property (device->udev_device, "ID_WWN_WITH_EXTENSION"))
        udisks_drive_set_wwn (iface, g_udev_device_get_property (device->udev_device, "ID_WWN_WITH_EXTENSION"));
      else
        udisks_drive_set_wwn (iface, g_udev_device_get_property (device->udev_device, "ID_WWN"));
    }

  /* try to find a serial number if we don't have one yet */
  if (serial == NULL)
    serial = g_udev_device_get_property (device->udev_device, "ID_SERIAL_SHORT");
  if (serial == NULL)
    serial = g_udev_device_get_property (device->udev_device, "ID_SERIAL");
  udisks_drive_set_serial (iface, serial);

  /* common bits go here */
  if (size == 0)
    size = udisks_daemon_util_block_get_size (device->udev_device,
                                              &media_available,
                                              &media_change_detected);
  udisks_drive_set_size (iface, size);
  udisks_drive_set_media_available (iface, media_available);
  udisks_drive_set_media_change_detected (iface, media_change_detected);
  set_media (iface, device, is_pc_floppy_drive);
  set_rotation_rate (iface, device);
  set_connection_bus (iface, device);

  if (udisks_drive_get_media_removable (iface) ||
      g_strcmp0 (udisks_drive_get_connection_bus (iface), "usb") == 0 ||
      g_strcmp0 (udisks_drive_get_connection_bus (iface), "ieee1394") == 0)
    removable_hint = TRUE;
  udisks_drive_set_removable (iface, removable_hint);

  seat = g_udev_device_get_property (device->udev_device, "ID_SEAT");
  /* assume seat0 if not set */
  if (seat == NULL || strlen (seat) == 0)
    seat = "seat0";
  udisks_drive_set_seat (iface, seat);

  set_media_time_detected (drive, device, is_pc_floppy_drive, coldplug);

  /* calculate sort-key  */
  if (drive->sort_key == NULL)
    {
      if (coldplug)
        {
          const gchar *device_name;
          /* TODO: adjust device_name for better sort order (so e.g. sdaa comes after sdz) */
          device_name = g_udev_device_get_name (device->udev_device);
          if (udisks_drive_get_removable (iface))
            {
              /* make sure fd* BEFORE sr* BEFORE sd* */
              if (g_str_has_prefix (device_name, "fd"))
                {
                  drive->sort_key = g_strdup_printf ("00coldplug/10removable/%s", device_name);
                }
              else if (g_str_has_prefix (device_name, "sr"))
                {
                  drive->sort_key = g_strdup_printf ("00coldplug/11removable/%s", device_name);
                }
              else if (g_str_has_prefix (device_name, "sd"))
                {
                  drive->sort_key = append_fixedup_sd ("00coldplug/12removable/", device_name);
                }
              else
                {
                  drive->sort_key = g_strdup_printf ("00coldplug/12removable/%s", device_name);
                }
            }
          else
            {
              if (g_str_has_prefix (device_name, "sd"))
                drive->sort_key = append_fixedup_sd ("00coldplug/00fixed/", device_name);
              else
                drive->sort_key = g_strdup_printf ("00coldplug/00fixed/%s", device_name);
            }
        }
      else
        {
          drive->sort_key = g_strdup_printf ("01hotplug/%" G_GINT64_FORMAT, drive->time_detected);
        }
      udisks_drive_set_sort_key (iface, drive->sort_key);
    }

  set_id (iface);

  ret = update_configuration (drive, object);

 out:
  g_dbus_interface_skeleton_flush (G_DBUS_INTERFACE_SKELETON (drive));
  if (device != NULL)
    g_clear_object (&device);

  return ret;
}

/* ---------------------------------------------------------------------------------------------------- */

/**
 * udisks_linux_drive_recalculate_nvme_size:
 * @drive: A #UDisksLinuxDrive.
 * @object: The enclosing #UDisksLinuxDriveObject instance.
 *
 * Find all block objects pointing to this drive, calculate
 * NVMe namespace capacity numbers and update this interface.
 */
void
udisks_linux_drive_recalculate_nvme_size (UDisksLinuxDrive       *drive,
                                          UDisksLinuxDriveObject *object)
{
  UDisksDaemon *daemon;
  GDBusObjectManagerServer *object_manager;
  GList *objects = NULL;
  GList *l;
  const gchar *obj_path;
  guint64 size_total = 0;

  daemon = udisks_linux_drive_object_get_daemon (object);
  object_manager = udisks_daemon_get_object_manager (daemon);
  obj_path = g_dbus_object_get_object_path (G_DBUS_OBJECT (object));

  objects = g_dbus_object_manager_get_objects (G_DBUS_OBJECT_MANAGER (object_manager));
  for (l = objects; l != NULL; l = l->next)
    {
      UDisksObject *o = l->data;
      UDisksBlock *block;
      UDisksLinuxDevice *device;

      if (!UDISKS_IS_LINUX_BLOCK_OBJECT (o))
        continue;

      block = udisks_object_get_block (o);
      if (!block)
          continue;

      if (g_strcmp0 (udisks_block_get_drive (block), obj_path) != 0)
        {
          g_object_unref (block);
          continue;
        }

      device = udisks_linux_block_object_get_device (UDISKS_LINUX_BLOCK_OBJECT (o));
      if (device && device->nvme_ns_info &&
          device->nvme_ns_info->current_lba_format.data_size > 0)
        {
          /* Namespace Size >= Namespace Capacity >= Namespace Utilization */
          size_total += (guint64) device->nvme_ns_info->nsize *
                        (guint64) device->nvme_ns_info->current_lba_format.data_size;
        }
      g_clear_object (&device);
      g_object_unref (block);
    }
  g_list_free_full (objects, g_object_unref);

  udisks_drive_set_size (UDISKS_DRIVE (drive), size_total);
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
handle_eject (UDisksDrive           *_drive,
              GDBusMethodInvocation *invocation,
              GVariant              *options)
{
  UDisksLinuxDrive *drive = UDISKS_LINUX_DRIVE (_drive);
  UDisksLinuxDriveObject *object;
  UDisksLinuxBlockObject *block_object = NULL;
  UDisksBlock *block = NULL;
  UDisksDaemon *daemon = NULL;
  const gchar *action_id;
  const gchar *message;
  gchar *error_message = NULL;
  GError *error = NULL;
  gchar *escaped_device = NULL;
  uid_t caller_uid;

  object = udisks_daemon_util_dup_object (drive, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = udisks_linux_drive_object_get_daemon (object);
  block_object = udisks_linux_drive_object_get_block (object, FALSE);
  if (block_object == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Unable to find block device for drive");
      goto out;
    }
  block = udisks_object_peek_block (UDISKS_OBJECT (block_object));

  /* refuse to eject if drive appears to be in use */
  if (!udisks_linux_drive_object_is_not_in_use (object, NULL, &error))
    {
      g_prefix_error (&error, "Cannot eject drive in use: ");
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  error = NULL;
  if (!udisks_daemon_util_get_caller_uid_sync (daemon,
                                               invocation,
                                               NULL /* GCancellable */,
                                               &caller_uid,
                                               &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      g_clear_error (&error);
      goto out;
    }

  /* Translators: Shown in authentication dialog when the user
   * requests ejecting media from a drive.
   *
   * Do not translate $(drive), it's a placeholder and
   * will be replaced by the name of the drive/device in question
   */
  message = N_("Authentication is required to eject $(drive)");
  action_id = "org.freedesktop.udisks2.eject-media";
  if (udisks_block_get_hint_system (block))
    {
      action_id = "org.freedesktop.udisks2.eject-media-system";
    }
  else if (!udisks_daemon_util_on_user_seat (daemon, UDISKS_OBJECT (object), caller_uid))
    {
      action_id = "org.freedesktop.udisks2.eject-media-other-seat";
    }

  /* Check that the user is actually authorized */
  if (!udisks_daemon_util_check_authorization_sync (daemon,
                                                    UDISKS_OBJECT (block_object),
                                                    action_id,
                                                    options,
                                                    message,
                                                    invocation))
    goto out;

  escaped_device = g_shell_quote (udisks_block_get_device (block));

  if (!udisks_daemon_launch_spawned_job_sync (daemon,
                                              UDISKS_OBJECT (object),
                                              "drive-eject", caller_uid,
                                              NULL, /* GCancellable */
                                              0,    /* uid_t run_as_uid */
                                              0,    /* uid_t run_as_euid */
                                              NULL, /* gint *out_status */
                                              &error_message,
                                              NULL,  /* input_string */
                                              "eject %s",
                                              escaped_device))
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Error ejecting %s: %s",
                                             udisks_block_get_device (block),
                                             error_message);
      goto out;
    }

  udisks_drive_complete_eject (UDISKS_DRIVE (drive), invocation);

 out:
  g_free (escaped_device);
  g_clear_object (&block_object);
  g_free (error_message);
  g_clear_object (&object);
  return TRUE; /* returning TRUE means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
handle_set_configuration (UDisksDrive           *_drive,
                          GDBusMethodInvocation *invocation,
                          GVariant              *configuration,
                          GVariant              *options)
{
  UDisksLinuxDrive *drive = UDISKS_LINUX_DRIVE (_drive);
  UDisksDaemon *daemon;
  UDisksLinuxDriveObject *object;
  const gchar *action_id;
  const gchar *message;
  GKeyFile *key_file = NULL;
  GError *error = NULL;
  gchar *path = NULL;
  gchar *data = NULL;
  gsize data_len;
  guint n;

  object = udisks_daemon_util_dup_object (drive, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = udisks_linux_drive_object_get_daemon (object);

  /* Translators: Shown in authentication dialog when the user
   * changes settings for a drive.
   *
   * Do not translate $(drive), it's a placeholder and will be
   * replaced by the name of the drive/device in question
   */
  message = N_("Authentication is required to configure settings for $(drive)");
  action_id = "org.freedesktop.udisks2.modify-drive-settings";

  /* Check that the user is actually authorized */
  if (!udisks_daemon_util_check_authorization_sync (daemon,
                                                    UDISKS_OBJECT (object),
                                                    action_id,
                                                    options,
                                                    message,
                                                    invocation))
    goto out;

  path = configuration_get_path (drive, daemon);
  if (path == NULL)
    {
      g_dbus_method_invocation_return_error (invocation, UDISKS_ERROR, UDISKS_ERROR_FAILED,
                                             "Drive has no persistent unique id");
      goto out;
    }

  key_file = g_key_file_new ();
  if (!g_key_file_load_from_file (key_file,
                                  path,
                                  G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS,
                                  &error))
    {
      if (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        {
          g_dbus_method_invocation_take_error (invocation, error);
          goto out;
        }
      /* not a problem, just create a new file */
      g_key_file_set_comment (key_file,
                              NULL, /* group_name */
                              NULL, /* key */
                              " See udisks(8) for the format of this file.",
                              NULL);
      g_clear_error (&error);
    }

  for (n = 0; n < G_N_ELEMENTS (drive_configuration_mapping); n++)
    {
      const VariantKeyfileMapping *mapping = &drive_configuration_mapping[n];
      GVariant *value = NULL;

      value = g_variant_lookup_value (configuration, mapping->asv_key, mapping->type);
      if (value == NULL)
        {
          g_key_file_remove_key (key_file, mapping->group, mapping->key, NULL);
        }
      else
        {
          if (g_variant_type_equal (mapping->type, G_VARIANT_TYPE_INT32))
            {
              g_key_file_set_integer (key_file, mapping->group, mapping->key, g_variant_get_int32 (value));
            }
          else if (g_variant_type_equal (mapping->type, G_VARIANT_TYPE_BOOLEAN))
            {
              g_key_file_set_boolean (key_file, mapping->group, mapping->key, g_variant_get_boolean (value));
            }
          else
            {
              g_assert_not_reached ();
            }
        }
    }

  data = g_key_file_to_data (key_file, &data_len, NULL);

  if (!udisks_daemon_util_file_set_contents (path,
                                             data,
                                             data_len,
                                             0600, /* mode to use if non-existent */
                                             &error))
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }


  udisks_drive_complete_set_configuration (UDISKS_DRIVE (drive), invocation);

 out:
  g_free (data);
  g_free (path);
  g_clear_object (&object);
  if (key_file != NULL)
    g_key_file_free (key_file);
  return TRUE; /* returning TRUE means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

/* TODO: move to udisksscsi.[ch] similar what we do for ATA with udisksata.[ch] */

static gboolean
send_scsi_command_sync (gint      fd,
                        guint8   *cdb,
                        gsize     cdb_len,
                        GError  **error)
{
  struct sg_io_v4 io_v4;
  uint8_t sense[32];
  gboolean ret = FALSE;
  gint rc;
  gint timeout_msec = 30000; /* 30 seconds */

  g_return_val_if_fail (fd != -1, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  /* See http://sg.danny.cz/sg/sg_io.html and http://www.tldp.org/HOWTO/SCSI-Generic-HOWTO/index.html
   * for detailed information about how the SG_IO ioctl work
   */

  memset (sense, 0, sizeof (sense));
  memset (&io_v4, 0, sizeof (io_v4));
  io_v4.guard = 'Q';
  io_v4.protocol = BSG_PROTOCOL_SCSI;
  io_v4.subprotocol = BSG_SUB_PROTOCOL_SCSI_CMD;
  io_v4.request_len = cdb_len;
  io_v4.request = (uintptr_t) cdb;
  io_v4.max_response_len = sizeof (sense);
  io_v4.response = (uintptr_t) sense;
  io_v4.timeout = timeout_msec;

  rc = ioctl (fd, SG_IO, &io_v4);
  if (rc != 0)
    {
      /* could be that the driver doesn't do version 4, try version 3 */
      if (errno == EINVAL)
        {
          struct sg_io_hdr io_hdr;
          memset (&io_hdr, 0, sizeof (struct sg_io_hdr));
          io_hdr.interface_id = 'S';
          io_hdr.cmdp = (unsigned char*) cdb;
          io_hdr.cmd_len = cdb_len;
          io_hdr.dxfer_direction = SG_DXFER_NONE;
          io_hdr.sbp = sense;
          io_hdr.mx_sb_len = sizeof (sense);
          io_hdr.timeout = timeout_msec;

          rc = ioctl (fd, SG_IO, &io_hdr);
          if (rc != 0)
            {
              g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                           "SGIO v3 ioctl failed (v4 not supported): %m");
              goto out;
            }
          else
            {
              if (!(io_hdr.status == 0 &&
                    io_hdr.host_status == 0 &&
                    io_hdr.driver_status == 0))
                {
                  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Non-GOOD SCSI status from SGIO v3 ioctl: "
                               "status=%d host_status=%d driver_status=%d",
                               io_hdr.status,
                               io_hdr.host_status,
                               io_hdr.driver_status);
                  goto out;
                }
            }
        }
      else
        {
          g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                       "SGIO v4 ioctl failed: %m");
          goto out;
        }
    }
  else
    {
      if (!(io_v4.device_status == 0 &&
            io_v4.transport_status == 0 &&
            io_v4.driver_status == 0))
        {
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                       "Non-GOOD SCSI status from SGIO v4 ioctl: "
                       "device_status=%u transport_status=%u driver_status=%u",
                       io_v4.device_status,
                       io_v4.transport_status,
                       io_v4.driver_status);
          goto out;
        }
    }

  ret = TRUE;

 out:
  return ret;
}

static gboolean
send_scsi_synchronize_cache_command_sync (gint      fd,
                                          GError  **error)
{
  uint8_t cdb[10];

  /* SBC3 (SCSI Block Commands), 5.18 SYNCHRONIZE CACHE (10) command
   */
  memset (cdb, 0, sizeof cdb);
  cdb[0] = 0x35;                        /* OPERATION CODE: SYNCHRONIZE CACHE (10) */

  return send_scsi_command_sync (fd, cdb, sizeof cdb, error);
}

static gboolean
send_scsi_start_stop_unit_command_sync (gint      fd,
                                        GError  **error)
{
  uint8_t cdb[6];

  /* SBC3 (SCSI Block Commands), 5.20 START STOP UNIT command
   */
  memset (cdb, 0, sizeof cdb);
  cdb[0] = 0x1b;                        /* OPERATION CODE: START STOP UNIT */

  return send_scsi_command_sync (fd, cdb, sizeof cdb, error);
}

/* ---------------------------------------------------------------------------------------------------- */

static gboolean
handle_power_off (UDisksDrive           *_drive,
                  GDBusMethodInvocation *invocation,
                  GVariant              *options)
{
  UDisksLinuxDrive *drive = UDISKS_LINUX_DRIVE (_drive);
  UDisksLinuxDevice *device = NULL;
  gchar *remove_path = NULL;
  FILE *f;
  GUdevDevice *usb_device = NULL;
  UDisksLinuxDriveObject *object;
  UDisksLinuxBlockObject *block_object = NULL;
  GList *blocks_to_sync = NULL;
  UDisksBlock *block = NULL;
  UDisksDaemon *daemon = NULL;
  const gchar *action_id;
  const gchar *message;
  gchar *error_message = NULL;
  GError *error = NULL;
  uid_t caller_uid;
  GList *sibling_objects = NULL, *l;
  gint fd = -1;

  object = udisks_daemon_util_dup_object (drive, &error);
  if (object == NULL)
    {
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }

  daemon = udisks_linux_drive_object_get_daemon (object);
  block_object = udisks_linux_drive_object_get_block (object, FALSE);
  if (block_object == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Unable to find block device for drive");
      goto out;
    }
  block = udisks_object_peek_block (UDISKS_OBJECT (block_object));
  blocks_to_sync = g_list_prepend (blocks_to_sync, g_object_ref (block));

  sibling_objects = udisks_linux_drive_object_get_siblings (object);

  /* refuse if drive - or one of its siblings - appears to be in use */
  if (!udisks_linux_drive_object_is_not_in_use (object, NULL, &error))
    {
      g_prefix_error (&error, "The drive in use: ");
      g_dbus_method_invocation_take_error (invocation, error);
      goto out;
    }
  for (l = sibling_objects; l != NULL; l = l->next)
    {
      UDisksLinuxDriveObject *sibling_object = UDISKS_LINUX_DRIVE_OBJECT (l->data);
      UDisksLinuxBlockObject *sibling_block_object;

      if (!udisks_linux_drive_object_is_not_in_use (sibling_object, NULL, &error))
        {
          g_prefix_error (&error, "A drive that is part of the same device is in use: ");
          g_dbus_method_invocation_take_error (invocation, error);
          goto out;
        }

      sibling_block_object = udisks_linux_drive_object_get_block (sibling_object, FALSE); /* get_hw */
      if (sibling_block_object != NULL)
        {
          UDisksBlock *sibling_block = udisks_object_get_block (UDISKS_OBJECT (sibling_block_object));
          if (sibling_block != NULL)
            blocks_to_sync = g_list_prepend (blocks_to_sync, sibling_block);
          g_object_unref (sibling_block_object);
        }
    }

  error = NULL;
  if (!udisks_daemon_util_get_caller_uid_sync (daemon,
                                               invocation,
                                               NULL /* GCancellable */,
                                               &caller_uid,
                                               &error))
    {
      g_dbus_method_invocation_return_gerror (invocation, error);
      g_clear_error (&error);
      goto out;
    }

  /* Translators: Shown in authentication dialog when the user
   * requests ejecting media from a drive.
   *
   * Do not translate $(drive), it's a placeholder and
   * will be replaced by the name of the drive/device in question
   */
  message = N_("Authentication is required to power off $(drive)");
  action_id = "org.freedesktop.udisks2.power-off-drive";
  if (udisks_block_get_hint_system (block))
    {
      action_id = "org.freedesktop.udisks2.power-off-drive-system";
    }
  else if (!udisks_daemon_util_on_user_seat (daemon, UDISKS_OBJECT (object), caller_uid))
    {
      action_id = "org.freedesktop.udisks2.power-off-drive-other-seat";
    }

  /* Check that the user is actually authorized */
  if (!udisks_daemon_util_check_authorization_sync (daemon,
                                                    UDISKS_OBJECT (block_object),
                                                    action_id,
                                                    options,
                                                    message,
                                                    invocation))
    goto out;

  /* sync all block devices */
  for (l = blocks_to_sync; l != NULL ; l = l->next)
    {
      UDisksBlock *block_to_sync = UDISKS_BLOCK (l->data);
      const gchar *device_file;
      gint device_fd;
      device_file = udisks_block_get_device (block_to_sync);
      device_fd = open (device_file, O_RDONLY|O_NONBLOCK|O_EXCL);
      if (device_fd == -1)
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 UDISKS_ERROR,
                                                 UDISKS_ERROR_FAILED,
                                                 "Error opening %s for fsync: %m",
                                                 device_file);
          goto out;
        }
      if (fsync (device_fd) != 0)
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 UDISKS_ERROR,
                                                 UDISKS_ERROR_FAILED,
                                                 "Error syncing  %s: %m",
                                                 device_file);
          close (device_fd);
          goto out;
        }
      if (close (device_fd) != 0)
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 UDISKS_ERROR,
                                                 UDISKS_ERROR_FAILED,
                                                 "Error closing %s (after syncing): %m",
                                                 device_file);
          goto out;
        }
    }

  /* Send the "SCSI SYNCHRONIZE CACHE" and then the "SCSI START STOP
   * UNIT" command to request that the unit be stopped. Don't treat
   * failures as fatal. In fact some USB-attached hard-disks fails
   * with one or both of these commands, probably due to the SCSI/SATA
   * translation layer.
   */
  fd = open (udisks_block_get_device (block), O_RDONLY|O_NONBLOCK|O_EXCL);
  if (fd == -1)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Error opening %s for cache synchronize: %m",
                                             udisks_block_get_device (block));
      goto out;
    }

  if (!send_scsi_synchronize_cache_command_sync (fd, &error))
    {
      udisks_warning ("Ignoring SCSI command SYNCHRONIZE CACHE failure (%s) on %s",
                      error->message,
                      udisks_block_get_device (block));
      g_clear_error (&error);
    }
  else
    {
      udisks_notice ("Successfully sent SCSI command SYNCHRONIZE CACHE to %s",
                     udisks_block_get_device (block));
    }

  if (!send_scsi_start_stop_unit_command_sync (fd, &error))
    {
      udisks_warning ("Ignoring SCSI command START STOP UNIT failure (%s) on %s",
                      error->message,
                      udisks_block_get_device (block));
      g_clear_error (&error);
    }
  else
    {
      udisks_notice ("Successfully sent SCSI command START STOP UNIT to %s",
                     udisks_block_get_device (block));
    }

  if (close (fd) != 0)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Error closing %s: %m",
                                             udisks_block_get_device (block));
      goto out;
    }
  fd = -1;

  device = udisks_linux_drive_object_get_device (object, TRUE /* get_hw */);
  if (device == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "No device");
      goto out;
    }
  usb_device = g_udev_device_get_parent_with_subsystem (device->udev_device, "usb", "usb_device");
  if (usb_device == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "No usb device");
      goto out;
    }

  /* http://git.kernel.org/?p=linux/kernel/git/torvalds/linux.git;a=commit;h=253e05724f9230910344357b1142ad8642ff9f5a */
  remove_path = g_strdup_printf ("%s/remove", g_udev_device_get_sysfs_path (usb_device));
  f = fopen (remove_path, "w");
  if (f == NULL)
    {
      g_dbus_method_invocation_return_error (invocation,
                                             UDISKS_ERROR,
                                             UDISKS_ERROR_FAILED,
                                             "Error opening %s for device removal: %m",
                                             remove_path);
      goto out;
    }
  else
    {
      gchar contents[1] = {'1'};
      if (fwrite (contents, 1, 1, f) != 1)
        {
          g_dbus_method_invocation_return_error (invocation,
                                                 UDISKS_ERROR,
                                                 UDISKS_ERROR_FAILED,
                                                 "Error writing to sysfs file %s: %m",
                                                 remove_path);
          fclose (f);
          goto out;
        }
    }
  fclose (f);
  udisks_notice ("Powered off %s - successfully wrote to sysfs path %s",
                 udisks_block_get_device (block),
                 remove_path);

  udisks_drive_complete_power_off (UDISKS_DRIVE (drive), invocation);

 out:
  if (fd != -1)
    {
      if (close (fd) != 0)
        {
          udisks_warning ("Error closing device: %m");
        }
    }
  g_list_free_full (blocks_to_sync, g_object_unref);
  g_list_free_full (sibling_objects, g_object_unref);
  g_free (remove_path);
  g_clear_object (&usb_device);
  g_clear_object (&device);
  g_clear_object (&block_object);
  g_free (error_message);
  g_clear_object (&object);
  return TRUE; /* returning TRUE means that we handled the method invocation */
}

/* ---------------------------------------------------------------------------------------------------- */

static void
drive_iface_init (UDisksDriveIface *iface)
{
  iface->handle_eject = handle_eject;
  iface->handle_set_configuration = handle_set_configuration;
  iface->handle_power_off = handle_power_off;
}
