/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2008-2010 David Zeuthen <zeuthen@gmail.com>
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

#ifndef __UDISKS_STATE_H__
#define __UDISKS_STATE_H__

#include "udisksdaemontypes.h"

G_BEGIN_DECLS

#define UDISKS_TYPE_STATE         (udisks_state_get_type ())
#define UDISKS_STATE(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), UDISKS_TYPE_STATE, UDisksState))
#define UDISKS_IS_STATE(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), UDISKS_TYPE_STATE))

GType          udisks_state_get_type             (void) G_GNUC_CONST;
UDisksState   *udisks_state_new                  (UDisksDaemon  *daemon);
UDisksDaemon  *udisks_state_get_daemon           (UDisksState   *state);
void           udisks_state_start_cleanup        (UDisksState   *state);
void           udisks_state_stop_cleanup         (UDisksState   *state);
void           udisks_state_check                (UDisksState   *state);
void           udisks_state_check_block          (UDisksState   *state,
                                                  dev_t          block_device);
/* mounted-fs */
void           udisks_state_add_mounted_fs       (UDisksState   *state,
                                                  const gchar   *mount_point,
                                                  dev_t          block_device,
                                                  const gchar   *fs_uuid,
                                                  uid_t          uid,
                                                  gboolean       fstab_mount,
                                                  gboolean       persistent);
gchar         *udisks_state_find_mounted_fs      (UDisksState   *state,
                                                  dev_t          block_device,
                                                  const gchar   *fs_uuid,
                                                  uid_t         *out_uid,
                                                  gboolean      *out_fstab_mount);
/* unlocked-crypto-dev */
void           udisks_state_add_unlocked_crypto_dev    (UDisksState   *state,
                                                        dev_t          cleartext_device,
                                                        dev_t          crypto_device,
                                                        const gchar   *dm_uuid,
                                                        uid_t          uid);
dev_t            udisks_state_find_unlocked_crypto_dev (UDisksState   *state,
                                                        dev_t          crypto_device,
                                                        uid_t         *out_uid);
/* loop */
void             udisks_state_add_loop           (UDisksState   *state,
                                                  const gchar   *device_file,
                                                  const gchar   *backing_file,
                                                  dev_t          backing_file_device,
                                                  uid_t          uid);
gboolean         udisks_state_has_loop           (UDisksState   *state,
                                                  const gchar   *device_file,
                                                  uid_t         *out_uid);
/* mdraid */
void             udisks_state_add_mdraid         (UDisksState   *state,
                                                  dev_t          raid_device,
                                                  uid_t          uid);
gboolean         udisks_state_has_mdraid         (UDisksState   *state,
                                                  dev_t          raid_device,
                                                  uid_t         *out_uid);

/* modules */
void             udisks_state_add_module         (UDisksState   *state,
                                                  const gchar   *module_name);
void             udisks_state_clear_modules      (UDisksState   *state);
gchar          **udisks_state_get_modules        (UDisksState   *state);

G_END_DECLS

#endif /* __UDISKS_STATE_H__ */
