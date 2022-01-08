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

#ifndef __UDISKS_PRIVATE_H__
#define __UDISKS_PRIVATE_H__

#include "config.h"
#include "udisksdaemontypes.h"
#include <mntent.h>
#include <libmount/libmount.h>

G_BEGIN_DECLS

UDisksMount *_udisks_mount_new (dev_t dev,
                                const gchar *mount_path,
                                const gchar *fs_uuid,
                                UDisksMountType type);

UDisksFstabEntry *_udisks_fstab_entry_new (const struct mntent *mntent);

UDisksFstabEntry *_udisks_fstab_entry_new_from_mnt_fs (struct libmnt_fs *fs);

UDisksCrypttabEntry *_udisks_crypttab_entry_new (const gchar *name,
                                                 const gchar *device,
                                                 const gchar *passphrase,
                                                 const gchar *options);

#ifdef HAVE_LIBMOUNT_UTAB
UDisksUtabEntry * _udisks_utab_entry_new (struct libmnt_fs *fs);
#endif

G_END_DECLS

#endif /* __UDISKS_PRIVATE_H__ */
