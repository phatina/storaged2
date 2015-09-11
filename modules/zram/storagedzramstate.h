/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2015 Dominika Hodovska <dhodovsk@redhat.com>
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
 */

#ifndef __STORAGED_ZRAM_STATE_H__
#define __STORAGED_ZRAM_STATE_H__

#include <glib.h>
#include <src/storageddaemontypes.h>
#include "storagedzramtypes.h"

G_BEGIN_DECLS

StoragedZRAMState  *storaged_zram_state_new  (StoragedDaemon     *daemon);
void                storaged_zram_state_free (StoragedZRAMState  *state);

G_END_DECLS

#endif /* __STORAGED_BTRFS_STATE_H__ */
