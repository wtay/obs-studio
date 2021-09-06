/* dbus-requests.h
 *
 * Copyright 2021 Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#pragma once

#include <gio/gio.h>

typedef struct _dbus_request dbus_request;

dbus_request *dbus_request_new(GCancellable *cancellable,
			       GDBusSignalCallback callback, void *user_data);
void dbus_request_free(dbus_request *request);
const char *dbus_request_get_token(dbus_request *request);

const char *dbus_get_sender_name(void);
