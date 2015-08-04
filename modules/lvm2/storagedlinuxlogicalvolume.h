/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2007-2010 David Zeuthen <zeuthen@gmail.com>
 * Copyright (C) 2013 Marius Vollmer <marius.vollmer@redhat.com>
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

#ifndef __STORAGED_LINUX_LOGICAL_VOLUME_H__
#define __STORAGED_LINUX_LOGICAL_VOLUME_H__

#include <src/storageddaemontypes.h>
#include "storagedlvm2types.h"
#include "storaged-lvm2-generated.h"

G_BEGIN_DECLS

#define STORAGED_TYPE_LINUX_LOGICAL_VOLUME         (storaged_linux_logical_volume_get_type ())
#define STORAGED_LINUX_LOGICAL_VOLUME(o)           (G_TYPE_CHECK_INSTANCE_CAST ((o), STORAGED_TYPE_LINUX_LOGICAL_VOLUME, StoragedLinuxLogicalVolume))
#define STORAGED_IS_LINUX_LOGICAL_VOLUME(o)        (G_TYPE_CHECK_INSTANCE_TYPE ((o), STORAGED_TYPE_LINUX_LOGICAL_VOLUME))

GType                  storaged_linux_logical_volume_get_type (void) G_GNUC_CONST;
StoragedLogicalVolume *storaged_linux_logical_volume_new      (void);
void                   storaged_linux_logical_volume_update   (StoragedLinuxLogicalVolume     *logical_volume,
                                                               StoragedLinuxVolumeGroupObject *group_object,
                                                               GVariant                       *info,
                                                               gboolean                       *needs_polling_ret);
void                   storaged_linux_logical_volume_update_etctabs (StoragedLinuxLogicalVolume     *logical_volume,
                                                                     StoragedLinuxVolumeGroupObject *group_object);

gboolean               storaged_linux_logical_volume_teardown_block (StoragedLogicalVolume *volume,
                                                                     StoragedDaemon        *daemon,
                                                                     GDBusMethodInvocation *invocation,
                                                                     GVariant              *options,
                                                                     GError               **error);

G_END_DECLS

#endif /* __STORAGED_LINUX_LOGICAL_VOLUME_H__ */
