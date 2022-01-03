/* pipewire-capture.c
 *
 * Copyright 2020 Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
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
#include "pipewire.h"
#include "portal.h"

#include <util/dstr.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#define SESSION_PATH "/org/freedesktop/portal/desktop/session/%s/obs%u"

enum obs_pw_capture_type {
	DESKTOP_CAPTURE = 1,
	WINDOW_CAPTURE = 2,
};

struct obs_pipewire_capture {
	enum obs_pw_capture_type capture_type;
	uint32_t available_cursor_modes;
	uint32_t node_id;
	GCancellable *cancellable;
	char *session_handle;
	obs_pipewire_data *obs_pw;
	bool show_cursor;
	char *restore_token;
};

static const char *capture_type_to_string(enum obs_pw_capture_type capture_type)
{
	switch (capture_type) {
	case DESKTOP_CAPTURE:
		return "desktop";
	case WINDOW_CAPTURE:
		return "window";
	}
	return "unknown";
}

static void obs_pipewire_capture_free(struct obs_pipewire_capture *pw_capture)
{
	if (!pw_capture)
		return;

	if (pw_capture->session_handle) {
		g_dbus_connection_call(portal_get_dbus_connection(),
				       "org.freedesktop.portal.Desktop",
				       pw_capture->session_handle,
				       "org.freedesktop.portal.Session",
				       "Close", NULL, NULL,
				       G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL,
				       NULL);

		g_clear_pointer(&pw_capture->session_handle, g_free);
	}

	g_cancellable_cancel(pw_capture->cancellable);
	g_clear_object(&pw_capture->cancellable);
	g_clear_pointer(&pw_capture->obs_pw, obs_pipewire_destroy);
	g_clear_pointer(&pw_capture->restore_token, bfree);
	bfree(pw_capture);
}

static void new_session_path(char **out_path, char **out_token)
{
	static uint32_t session_token_count = 0;

	session_token_count++;

	if (out_token) {
		struct dstr str;
		dstr_init(&str);
		dstr_printf(&str, "obs%u", session_token_count);
		*out_token = str.array;
	}

	if (out_path) {
		struct dstr str;
		dstr_init(&str);
		dstr_printf(&str, SESSION_PATH, dbus_get_sender_name(),
			    session_token_count);
		*out_path = str.array;
	}
}

/* ------------------------------------------------- */

static void on_pipewire_remote_opened_cb(GObject *source, GAsyncResult *res,
					 void *user_data)
{
	struct obs_pipewire_capture *pw_capture = user_data;
	g_autoptr(GUnixFDList) fd_list = NULL;
	g_autoptr(GVariant) result = NULL;
	g_autoptr(GError) error = NULL;
	int pipewire_fd;
	int fd_index;

	result = g_dbus_proxy_call_with_unix_fd_list_finish(
		G_DBUS_PROXY(source), &fd_list, res, &error);
	if (error) {
		if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			blog(LOG_ERROR,
			     "[pipewire] Error retrieving pipewire fd: %s",
			     error->message);
		return;
	}

	g_variant_get(result, "(h)", &fd_index, &error);

	pipewire_fd = g_unix_fd_list_get(fd_list, fd_index, &error);
	if (error) {
		if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			blog(LOG_ERROR,
			     "[pipewire] Error retrieving pipewire fd: %s",
			     error->message);
		return;
	}

	pw_capture->obs_pw = obs_pipewire_new_for_node(
		pipewire_fd, pw_capture->node_id, "OBS Studio",
		pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
				  PW_KEY_MEDIA_CATEGORY, "Capture",
				  PW_KEY_MEDIA_ROLE, "Screen", NULL));
	obs_pipewire_set_show_cursor(pw_capture->obs_pw,
				     pw_capture->show_cursor);
}

static void open_pipewire_remote(struct obs_pipewire_capture *pw_capture)
{
	GVariantBuilder builder;

	g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);

	g_dbus_proxy_call_with_unix_fd_list(
		portal_get_screencast_proxy(), "OpenPipeWireRemote",
		g_variant_new("(oa{sv})", pw_capture->session_handle, &builder),
		G_DBUS_CALL_FLAGS_NONE, -1, NULL, pw_capture->cancellable,
		on_pipewire_remote_opened_cb, pw_capture);
}

/* ------------------------------------------------- */

static void on_start_response_received_cb(GDBusConnection *connection,
					  const char *sender_name,
					  const char *object_path,
					  const char *interface_name,
					  const char *signal_name,
					  GVariant *parameters, void *user_data)
{
	UNUSED_PARAMETER(connection);
	UNUSED_PARAMETER(sender_name);
	UNUSED_PARAMETER(object_path);
	UNUSED_PARAMETER(interface_name);
	UNUSED_PARAMETER(signal_name);

	struct obs_pipewire_capture *pw_capture = user_data;
	g_autoptr(GVariant) stream_properties = NULL;
	g_autoptr(GVariant) streams = NULL;
	g_autoptr(GVariant) result = NULL;
	GVariantIter iter;
	uint32_t response;
	size_t n_streams;

	g_variant_get(parameters, "(u@a{sv})", &response, &result);

	if (response != 0) {
		blog(LOG_WARNING,
		     "[pipewire] Failed to start screencast, denied or cancelled by user");
		return;
	}

	streams =
		g_variant_lookup_value(result, "streams", G_VARIANT_TYPE_ARRAY);

	g_variant_iter_init(&iter, streams);

	n_streams = g_variant_iter_n_children(&iter);
	if (n_streams != 1) {
		blog(LOG_WARNING,
		     "[pipewire] Received more than one stream when only one was expected. "
		     "This is probably a bug in the desktop portal implementation you are "
		     "using.");

		// The KDE Desktop portal implementation sometimes sends an invalid
		// response where more than one stream is attached, and only the
		// last one is the one we're looking for. This is the only known
		// buggy implementation, so let's at least try to make it work here.
		for (size_t i = 0; i < n_streams - 1; i++) {
			g_autoptr(GVariant) throwaway_properties = NULL;
			uint32_t throwaway_pipewire_node;

			g_variant_iter_loop(&iter, "(u@a{sv})",
					    &throwaway_pipewire_node,
					    &throwaway_properties);
		}
	}

	g_variant_iter_loop(&iter, "(u@a{sv})", &pw_capture->node_id,
			    &stream_properties);

	blog(LOG_INFO, "[pipewire] %s selected, setting up screencast",
	     capture_type_to_string(pw_capture->capture_type));

	open_pipewire_remote(pw_capture);
}

static void on_started_cb(GObject *source, GAsyncResult *res, void *user_data)
{
	UNUSED_PARAMETER(user_data);

	g_autoptr(GVariant) result = NULL;
	g_autoptr(GError) error = NULL;

	result = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), res, &error);
	if (error) {
		if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			blog(LOG_ERROR,
			     "[pipewire] Error selecting screencast source: %s",
			     error->message);
		return;
	}
}

static void start(struct obs_pipewire_capture *pw_capture)
{
	GVariantBuilder builder;
	dbus_request *request;
	const char *request_token;

	blog(LOG_INFO, "[pipewire] asking for %s…",
	     capture_type_to_string(pw_capture->capture_type));

	request = dbus_request_new(pw_capture->cancellable,
				   on_start_response_received_cb, pw_capture);

	request_token = dbus_request_get_token(request);

	g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&builder, "{sv}", "handle_token",
			      g_variant_new_string(request_token));

	g_dbus_proxy_call(portal_get_screencast_proxy(), "Start",
			  g_variant_new("(osa{sv})", pw_capture->session_handle,
					"", &builder),
			  G_DBUS_CALL_FLAGS_NONE, -1, pw_capture->cancellable,
			  on_started_cb, pw_capture);
}

/* ------------------------------------------------- */

static void on_select_source_response_received_cb(
	GDBusConnection *connection, const char *sender_name,
	const char *object_path, const char *interface_name,
	const char *signal_name, GVariant *parameters, void *user_data)
{
	UNUSED_PARAMETER(connection);
	UNUSED_PARAMETER(sender_name);
	UNUSED_PARAMETER(object_path);
	UNUSED_PARAMETER(interface_name);
	UNUSED_PARAMETER(signal_name);

	g_autoptr(GVariant) ret = NULL;
	struct obs_pipewire_capture *pw_capture = user_data;
	uint32_t response;

	blog(LOG_DEBUG, "[pipewire] Response to select source received");

	g_variant_get(parameters, "(u@a{sv})", &response, &ret);

	if (response != 0) {
		blog(LOG_WARNING,
		     "[pipewire] Failed to select source, denied or cancelled by user");
		return;
	}

	start(pw_capture);
}

static void on_source_selected_cb(GObject *source, GAsyncResult *res,
				  void *user_data)
{
	UNUSED_PARAMETER(user_data);

	g_autoptr(GVariant) result = NULL;
	g_autoptr(GError) error = NULL;

	result = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), res, &error);
	if (error) {
		if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			blog(LOG_ERROR,
			     "[pipewire] Error selecting screencast source: %s",
			     error->message);
		return;
	}
}

static void select_source(struct obs_pipewire_capture *pw_capture)
{
	GVariantBuilder builder;
	dbus_request *request;
	const char *request_token;

	request = dbus_request_new(pw_capture->cancellable,
				   on_select_source_response_received_cb,
				   pw_capture);

	request_token = dbus_request_get_token(request);

	g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&builder, "{sv}", "types",
			      g_variant_new_uint32(pw_capture->capture_type));
	g_variant_builder_add(&builder, "{sv}", "multiple",
			      g_variant_new_boolean(FALSE));
	g_variant_builder_add(&builder, "{sv}", "handle_token",
			      g_variant_new_string(request_token));

	if (pw_capture->available_cursor_modes & 4)
		g_variant_builder_add(&builder, "{sv}", "cursor_mode",
				      g_variant_new_uint32(4));
	else if ((pw_capture->available_cursor_modes & 2) &&
		 pw_capture->show_cursor)
		g_variant_builder_add(&builder, "{sv}", "cursor_mode",
				      g_variant_new_uint32(2));
	else
		g_variant_builder_add(&builder, "{sv}", "cursor_mode",
				      g_variant_new_uint32(1));

	if (portal_get_screencast_version() >= 4) {
		g_variant_builder_add(&builder, "{sv}", "persist_mode",
				      g_variant_new_uint32(2));
		if (pw_capture->restore_token && *pw_capture->restore_token) {
			g_variant_builder_add(
				&builder, "{sv}", "restore_token",
				g_variant_new_string(
					pw_capture->restore_token));
		}
	}

	g_dbus_proxy_call(portal_get_screencast_proxy(), "SelectSources",
			  g_variant_new("(oa{sv})", pw_capture->session_handle,
					&builder),
			  G_DBUS_CALL_FLAGS_NONE, -1, pw_capture->cancellable,
			  on_source_selected_cb, pw_capture);
}

/* ------------------------------------------------- */

static void on_create_session_response_received_cb(
	GDBusConnection *connection, const char *sender_name,
	const char *object_path, const char *interface_name,
	const char *signal_name, GVariant *parameters, void *user_data)
{
	UNUSED_PARAMETER(connection);
	UNUSED_PARAMETER(sender_name);
	UNUSED_PARAMETER(object_path);
	UNUSED_PARAMETER(interface_name);
	UNUSED_PARAMETER(signal_name);

	g_autoptr(GVariant) session_handle_variant = NULL;
	g_autoptr(GVariant) result = NULL;
	struct obs_pipewire_capture *pw_capture = user_data;
	uint32_t response;

	g_variant_get(parameters, "(u@a{sv})", &response, &result);

	if (response != 0) {
		blog(LOG_WARNING,
		     "[pipewire] Failed to create session, denied or cancelled by user");
		return;
	}

	blog(LOG_INFO, "[pipewire] screencast session created");

	session_handle_variant =
		g_variant_lookup_value(result, "session_handle", NULL);
	pw_capture->session_handle =
		g_variant_dup_string(session_handle_variant, NULL);

	select_source(pw_capture);
}

static void on_session_created_cb(GObject *source, GAsyncResult *res,
				  void *user_data)
{
	UNUSED_PARAMETER(user_data);

	g_autoptr(GVariant) result = NULL;
	g_autoptr(GError) error = NULL;

	result = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), res, &error);
	if (error) {
		if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			blog(LOG_ERROR,
			     "[pipewire] Error creating screencast session: %s",
			     error->message);
		return;
	}
}

static void create_session(struct obs_pipewire_capture *pw_capture)
{
	GVariantBuilder builder;
	dbus_request *request;
	const char *request_token;
	char *session_token;

	new_session_path(NULL, &session_token);

	request = dbus_request_new(pw_capture->cancellable,
				   on_create_session_response_received_cb,
				   pw_capture);

	request_token = dbus_request_get_token(request);

	g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&builder, "{sv}", "handle_token",
			      g_variant_new_string(request_token));
	g_variant_builder_add(&builder, "{sv}", "session_handle_token",
			      g_variant_new_string(session_token));

	g_dbus_proxy_call(portal_get_screencast_proxy(), "CreateSession",
			  g_variant_new("(a{sv})", &builder),
			  G_DBUS_CALL_FLAGS_NONE, -1, pw_capture->cancellable,
			  on_session_created_cb, pw_capture);

	bfree(session_token);
}

/* ------------------------------------------------- */

static void
update_available_cursor_modes(struct obs_pipewire_capture *pw_capture,
			      GDBusProxy *proxy)
{
	g_autoptr(GVariant) cached_cursor_modes = NULL;
	uint32_t available_cursor_modes;

	cached_cursor_modes =
		g_dbus_proxy_get_cached_property(proxy, "AvailableCursorModes");
	available_cursor_modes =
		cached_cursor_modes ? g_variant_get_uint32(cached_cursor_modes)
				    : 0;

	pw_capture->available_cursor_modes = available_cursor_modes;

	blog(LOG_INFO, "[pipewire] available cursor modes:");
	if (available_cursor_modes & 4)
		blog(LOG_INFO, "[pipewire]     - Metadata");
	if (available_cursor_modes & 2)
		blog(LOG_INFO, "[pipewire]     - Always visible");
	if (available_cursor_modes & 1)
		blog(LOG_INFO, "[pipewire]     - Hidden");
}

/* ------------------------------------------------- */

static gboolean init_pipewire_capture(struct obs_pipewire_capture *pw_capture)
{
	GDBusConnection *connection;
	GDBusProxy *proxy;

	pw_capture->cancellable = g_cancellable_new();
	connection = portal_get_dbus_connection();
	if (!connection)
		return FALSE;
	proxy = portal_get_screencast_proxy();
	if (!proxy)
		return FALSE;

	update_available_cursor_modes(pw_capture, proxy);

	blog(LOG_INFO, "PipeWire initialized (sender name: %s)",
	     dbus_get_sender_name());

	create_session(pw_capture);

	return TRUE;
}

static bool reload_session_cb(obs_properties_t *properties,
			      obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(properties);
	UNUSED_PARAMETER(property);

	struct obs_pipewire_capture *pw_capture = data;

	g_clear_pointer(&pw_capture->obs_pw, obs_pipewire_destroy);
	init_pipewire_capture(pw_capture);

	return false;
}

/* obs_source_info methods */

static const char *pipewire_desktop_capture_get_name(void *data)
{
	UNUSED_PARAMETER(data);
	return obs_module_text("PipeWireDesktopCapture");
}

static const char *pipewire_window_capture_get_name(void *data)
{
	UNUSED_PARAMETER(data);
	return obs_module_text("PipeWireWindowCapture");
}

static void *pipewire_desktop_capture_create(obs_data_t *settings,
					     obs_source_t *source)
{
	UNUSED_PARAMETER(source);
	struct obs_pipewire_capture *pw_capture;

	pw_capture = bzalloc(sizeof(struct obs_pipewire_capture));
	pw_capture->capture_type = DESKTOP_CAPTURE;
	pw_capture->show_cursor = obs_data_get_bool(settings, "ShowCursor");
	pw_capture->restore_token =
		bstrdup(obs_data_get_string(settings, "RestoreToken"));

	if (!init_pipewire_capture(pw_capture)) {
		obs_pipewire_capture_free(pw_capture);
		return NULL;
	}

	return pw_capture;
}
static void *pipewire_window_capture_create(obs_data_t *settings,
					    obs_source_t *source)
{
	UNUSED_PARAMETER(source);
	struct obs_pipewire_capture *pw_capture;

	pw_capture = bzalloc(sizeof(struct obs_pipewire_capture));
	pw_capture->capture_type = WINDOW_CAPTURE;
	pw_capture->show_cursor = obs_data_get_bool(settings, "ShowCursor");
	pw_capture->restore_token =
		bstrdup(obs_data_get_string(settings, "RestoreToken"));

	if (!init_pipewire_capture(pw_capture)) {
		obs_pipewire_capture_free(pw_capture);
		return NULL;
	}

	return pw_capture;
}

static void pipewire_capture_destroy(void *data)
{
	obs_pipewire_capture_free(data);
}

static void pipewire_capture_save(void *data, obs_data_t *settings)
{
	struct obs_pipewire_capture *pw_capture = data;
	obs_data_set_string(settings, "RestoreToken",
			    pw_capture->restore_token);
}

static void pipewire_capture_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "ShowCursor", true);
	obs_data_set_string(settings, "RestoreToken", NULL);
}

static obs_properties_t *pipewire_capture_get_properties(void *data)
{
	struct obs_pipewire_capture *pw_capture = data;
	obs_properties_t *properties;

	properties = obs_properties_create();

	switch (pw_capture->capture_type) {
	case DESKTOP_CAPTURE:
		obs_properties_add_button2(
			properties, "Reload",
			obs_module_text("PipeWireSelectMonitor"),
			reload_session_cb, pw_capture);
		break;

	case WINDOW_CAPTURE:
		obs_properties_add_button2(
			properties, "Reload",
			obs_module_text("PipeWireSelectWindow"),
			reload_session_cb, pw_capture);
		break;
	default:
		return NULL;
	}

	obs_properties_add_bool(properties, "ShowCursor",
				obs_module_text("ShowCursor"));

	return properties;
}

static void pipewire_capture_update(void *data, obs_data_t *settings)
{
	struct obs_pipewire_capture *pw_capture = data;

	obs_pipewire_set_show_cursor(pw_capture->obs_pw,
				     obs_data_get_bool(settings, "ShowCursor"));
}

static void pipewire_capture_show(void *data)
{
	struct obs_pipewire_capture *pw_capture = data;

	if (pw_capture->obs_pw)
		obs_pipewire_show(pw_capture->obs_pw);
}

static void pipewire_capture_hide(void *data)
{
	struct obs_pipewire_capture *pw_capture = data;

	if (pw_capture->obs_pw)
		obs_pipewire_hide(pw_capture->obs_pw);
}

static uint32_t pipewire_capture_get_width(void *data)
{
	struct obs_pipewire_capture *pw_capture = data;

	if (pw_capture->obs_pw)
		return obs_pipewire_get_width(pw_capture->obs_pw);
	else
		return 0;
}

static uint32_t pipewire_capture_get_height(void *data)
{
	struct obs_pipewire_capture *pw_capture = data;

	if (pw_capture->obs_pw)
		return obs_pipewire_get_height(pw_capture->obs_pw);
	else
		return 0;
}

static void pipewire_capture_video_render(void *data, gs_effect_t *effect)
{
	struct obs_pipewire_capture *pw_capture = data;

	if (pw_capture->obs_pw)
		obs_pipewire_video_render(pw_capture->obs_pw, effect);
}

static bool initialized = false;

void pipewire_capture_load(void)
{
	uint32_t available_capture_types = portal_get_available_capture_types();
	bool desktop_capture_available =
		(available_capture_types & PORTAL_CAPTURE_TYPE_MONITOR) != 0;
	bool window_capture_available =
		(available_capture_types & PORTAL_CAPTURE_TYPE_WINDOW) != 0;

	if (available_capture_types == 0) {
		blog(LOG_INFO, "[pipewire] No captures available");
		return;
	}

	blog(LOG_INFO, "[pipewire] Available captures:");
	if (desktop_capture_available)
		blog(LOG_INFO, "[pipewire]     - Desktop capture");
	if (window_capture_available)
		blog(LOG_INFO, "[pipewire]     - Window capture");

	// Desktop capture
	const struct obs_source_info pipewire_desktop_capture_info = {
		.id = "pipewire-desktop-capture-source",
		.type = OBS_SOURCE_TYPE_INPUT,
		.output_flags = OBS_SOURCE_VIDEO,
		.get_name = pipewire_desktop_capture_get_name,
		.create = pipewire_desktop_capture_create,
		.destroy = pipewire_capture_destroy,
		.save = pipewire_capture_save,
		.get_defaults = pipewire_capture_get_defaults,
		.get_properties = pipewire_capture_get_properties,
		.update = pipewire_capture_update,
		.show = pipewire_capture_show,
		.hide = pipewire_capture_hide,
		.get_width = pipewire_capture_get_width,
		.get_height = pipewire_capture_get_height,
		.video_render = pipewire_capture_video_render,
		.icon_type = OBS_ICON_TYPE_DESKTOP_CAPTURE,
	};
	if (desktop_capture_available)
		obs_register_source(&pipewire_desktop_capture_info);

	// Window capture
	const struct obs_source_info pipewire_window_capture_info = {
		.id = "pipewire-window-capture-source",
		.type = OBS_SOURCE_TYPE_INPUT,
		.output_flags = OBS_SOURCE_VIDEO,
		.get_name = pipewire_window_capture_get_name,
		.create = pipewire_window_capture_create,
		.destroy = pipewire_capture_destroy,
		.save = pipewire_capture_save,
		.get_defaults = pipewire_capture_get_defaults,
		.get_properties = pipewire_capture_get_properties,
		.update = pipewire_capture_update,
		.show = pipewire_capture_show,
		.hide = pipewire_capture_hide,
		.get_width = pipewire_capture_get_width,
		.get_height = pipewire_capture_get_height,
		.video_render = pipewire_capture_video_render,
		.icon_type = OBS_ICON_TYPE_WINDOW_CAPTURE,
	};
	if (window_capture_available)
		obs_register_source(&pipewire_window_capture_info);
}
