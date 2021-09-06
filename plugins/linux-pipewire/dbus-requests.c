/* dbus-requests.c
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

#include "dbus-requests.h"
#include "portal.h"

#include <util/dstr.h>

#define REQUEST_PATH "/org/freedesktop/portal/desktop/request/%s/obs%u"
#define SESSION_PATH "/org/freedesktop/portal/desktop/session/%s/obs%u"

struct _dbus_request {
	char *request_path;
	char *request_token;
	GCancellable *cancellable;
	guint signal_id;
	gulong cancelled_id;
	GDBusSignalCallback callback;
	void *user_data;
};

static char *sender_name_ = NULL;

static void new_request_path(char **out_path, char **out_token)
{
	static uint32_t request_token_count = 0;

	request_token_count++;

	if (out_token) {
		struct dstr str;
		dstr_init(&str);
		dstr_printf(&str, "obs%u", request_token_count);
		*out_token = str.array;
	}

	if (out_path) {
		struct dstr str;
		dstr_init(&str);
		dstr_printf(&str, REQUEST_PATH, dbus_get_sender_name(),
			    request_token_count);
		*out_path = str.array;
	}
}

static void on_cancelled_cb(GCancellable *cancellable, void *data)
{
	UNUSED_PARAMETER(cancellable);

	dbus_request *request = data;

	blog(LOG_INFO, "[pipewire] screencast session cancelled");

	g_dbus_connection_call(portal_get_dbus_connection(),
			       "org.freedesktop.portal.Desktop",
			       request->request_path,
			       "org.freedesktop.portal.Request", "Close", NULL,
			       NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL,
			       NULL);
}

static void on_response_received_cb(GDBusConnection *connection,
				    const char *sender_name,
				    const char *object_path,
				    const char *interface_name,
				    const char *signal_name,
				    GVariant *parameters, void *user_data)
{
	g_autoptr(GVariant) result = NULL;
	dbus_request *request = user_data;

	request->callback(connection, sender_name, object_path, interface_name,
			  signal_name, parameters, request->user_data);

	dbus_request_free(request);
}

const char *dbus_get_sender_name(void)
{
	if (sender_name_ == NULL) {
		GDBusConnection *connection;
		char *aux;

		connection = portal_get_dbus_connection();
		sender_name_ = bstrdup(
			g_dbus_connection_get_unique_name(connection) + 1);

		/* Replace dots by underscores */
		while ((aux = strstr(sender_name_, ".")) != NULL)
			*aux = '_';
	}
	return sender_name_;
}

void dbus_request_free(dbus_request *request)
{
	if (!request)
		return;

	if (request->signal_id)
		g_dbus_connection_signal_unsubscribe(
			portal_get_dbus_connection(), request->signal_id);

	if (request->cancelled_id > 0)
		g_signal_handler_disconnect(request->cancellable,
					    request->cancelled_id);

	g_clear_pointer(&request->request_token, bfree);
	g_clear_pointer(&request->request_path, bfree);
	g_clear_object(&request->cancellable);
	bfree(request);
}

dbus_request *dbus_request_new(GCancellable *cancellable,
			       GDBusSignalCallback callback, void *user_data)
{
	dbus_request *request;

	request = bzalloc(sizeof(dbus_request));
	request->user_data = user_data;
	request->callback = callback;
	new_request_path(&request->request_path, &request->request_token);

	if (cancellable) {
		request->cancellable = g_object_ref(cancellable);
		request->cancelled_id =
			g_signal_connect(cancellable, "cancelled",
					 G_CALLBACK(on_cancelled_cb), request);
	}
	request->signal_id = g_dbus_connection_signal_subscribe(
		portal_get_dbus_connection(), "org.freedesktop.portal.Desktop",
		"org.freedesktop.portal.Request", "Response",
		request->request_path, NULL, G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
		on_response_received_cb, request, NULL);

	return request;
}

const char *dbus_request_get_token(dbus_request *request)
{
	return request->request_token;
}
