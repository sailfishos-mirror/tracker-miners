/*
 *
 * Copyright (C) 2021, Nishit Patel <nishitlimbani130@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 */

#ifndef __TRACKER_CLI_UTILS_H_
#define __TRACKER_CLI_UTILS_H_

#include <glib.h>
#include <gio/gio.h>


GList* tracker_cli_get_error_keyfiles (void);

gboolean tracker_cli_check_inside_build_tree (const gchar* argv0);

#endif /* __TRACKER_CLI_UTILS_H__ */
