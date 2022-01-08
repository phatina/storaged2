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

#ifndef __UDISKS_MOUNT_H__
#define __UDISKS_MOUNT_H__

#include "udisksdaemontypes.h"

G_BEGIN_DECLS

#define UDISKS_TYPE_MOUNT         (udisks_mount_get_type ())
#define UDISKS_MOUNT(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), UDISKS_TYPE_MOUNT, UDisksMount))
#define UDISKS_IS_MOUNT(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), UDISKS_TYPE_MOUNT))

GType            udisks_mount_get_type       (void) G_GNUC_CONST;
UDisksMountType  udisks_mount_get_mount_type (UDisksMount *mount);
const gchar     *udisks_mount_get_mount_path (UDisksMount *mount);
const gchar     *udisks_mount_get_fs_uuid    (UDisksMount *mount);
dev_t            udisks_mount_get_dev        (UDisksMount *mount);
gint             udisks_mount_compare        (UDisksMount *mount,
                                              UDisksMount *other_mount);

G_END_DECLS

#endif /* __UDISKS_MOUNT_H__ */
