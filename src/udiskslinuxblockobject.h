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

#ifndef __UDISKS_LINUX_BLOCK_OBJECT_H__
#define __UDISKS_LINUX_BLOCK_OBJECT_H__

#include "udisksdaemontypes.h"
#include <gudev/gudev.h>

G_BEGIN_DECLS

#define UDISKS_TYPE_LINUX_BLOCK_OBJECT  (udisks_linux_block_object_get_type ())
#define UDISKS_LINUX_BLOCK_OBJECT(o)    (G_TYPE_CHECK_INSTANCE_CAST ((o), UDISKS_TYPE_LINUX_BLOCK_OBJECT, UDisksLinuxBlockObject))
#define UDISKS_IS_LINUX_BLOCK_OBJECT(o) (G_TYPE_CHECK_INSTANCE_TYPE ((o), UDISKS_TYPE_LINUX_BLOCK_OBJECT))

GType                     udisks_linux_block_object_get_type   (void) G_GNUC_CONST;
UDisksLinuxBlockObject   *udisks_linux_block_object_new        (UDisksDaemon            *daemon,
                                                                UDisksLinuxDevice       *device);
void                      udisks_linux_block_object_uevent     (UDisksLinuxBlockObject  *object,
                                                                const gchar             *action,
                                                                UDisksLinuxDevice       *device);
UDisksDaemon             *udisks_linux_block_object_get_daemon (UDisksLinuxBlockObject  *object);
UDisksLinuxDevice        *udisks_linux_block_object_get_device (UDisksLinuxBlockObject  *object);
gchar                    *udisks_linux_block_object_get_device_file (UDisksLinuxBlockObject *object);
dev_t                     udisks_linux_block_object_get_device_number (UDisksLinuxBlockObject *object);

void                      udisks_linux_block_object_trigger_uevent (UDisksLinuxBlockObject  *object);
gboolean                  udisks_linux_block_object_trigger_uevent_sync (UDisksLinuxBlockObject *object,
                                                                         guint                   timeout_seconds);
gboolean                  udisks_linux_block_object_reread_partition_table (UDisksLinuxBlockObject  *object,
                                                                            GError                 **error);
gboolean                  udisks_linux_block_object_contains_filesystem (UDisksObject *object);

void                      udisks_linux_block_object_lock_for_cleanup     (UDisksLinuxBlockObject *object);
gboolean                  udisks_linux_block_object_try_lock_for_cleanup (UDisksLinuxBlockObject *object);
void                      udisks_linux_block_object_release_cleanup_lock (UDisksLinuxBlockObject *object);

int                       udisks_linux_block_object_acquire_bsd_lock     (UDisksLinuxBlockObject *object,
                                                                          gboolean                exclusive,
                                                                          GError                **error);

G_END_DECLS

#endif /* __UDISKS_LINUX_BLOCK_OBJECT_H__ */
