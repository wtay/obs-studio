/* pipewire-camera.c
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
#include "pipewire.h"
#include "portal.h"

#include <util/dstr.h>

#include <fcntl.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <spa/node/keys.h>
#include <spa/pod/iter.h>
#include <spa/utils/defs.h>
#include <spa/utils/keys.h>

struct camera_object {
	struct spa_list link;

	struct obs_pipewire_camera *pw_camera;

	uint32_t id;
	uint32_t permissions;
	char *type;
	uint32_t version;

	struct pw_properties *props;
	struct pw_node_info *info;

	struct pw_proxy *proxy;
	struct spa_hook proxy_listener;
	struct spa_hook object_listener;
};

struct obs_pipewire_camera {
	GCancellable *cancellable;
	obs_pipewire_data *obs_pw;
	obs_source_t *source;
	int pipewire_fd;

	int sync_seq;

	struct pw_thread_loop *thread_loop;
	struct pw_context *context;

	struct pw_core *core;
	struct spa_hook core_listener;

	struct pw_registry *registry;
	struct spa_hook registry_listener;

	struct spa_list cameras;
	struct camera_object *current_camera;

	struct {
		const char *device_id;
	} defaults;
};

struct param {
	uint32_t id;
	struct spa_list link;
	struct spa_pod *param;
};

static void camera_destroy(struct camera_object *camera)
{
	if (!camera)
		return;

	spa_list_remove(&camera->link);
	if (camera->proxy)
		pw_proxy_destroy(camera->proxy);
	pw_properties_free(camera->props);
	bfree(camera->type);
	bfree(camera);
}

static void obs_pipewire_camera_free(struct obs_pipewire_camera *pw_camera)
{
	struct camera_object *camera;

	if (!pw_camera)
		return;

	if (pw_camera->pipewire_fd > 0) {
		close(pw_camera->pipewire_fd);
		pw_camera->pipewire_fd = 0;
	}

	spa_list_consume(camera, &pw_camera->cameras, link)
		camera_destroy(camera);

	if (pw_camera->thread_loop) {
		pw_thread_loop_wait(pw_camera->thread_loop);
		pw_thread_loop_stop(pw_camera->thread_loop);
	}

	g_cancellable_cancel(pw_camera->cancellable);
	g_clear_object(&pw_camera->cancellable);
	g_clear_pointer(&pw_camera->defaults.device_id, bfree);
	g_clear_pointer(&pw_camera->obs_pw, obs_pipewire_destroy);
	g_clear_pointer(&pw_camera->context, pw_context_destroy);
	g_clear_pointer(&pw_camera->thread_loop, pw_thread_loop_destroy);
	bfree(pw_camera);
}

static void sync_pipewire_core(struct obs_pipewire_camera *pw_camera)
{
	pw_camera->sync_seq =
		pw_core_sync(pw_camera->core, PW_ID_CORE, pw_camera->sync_seq);
	blog(LOG_INFO, "[pipewire] Sync start %u", pw_camera->sync_seq);
}

static struct camera_object *find_camera(struct obs_pipewire_camera *pw_camera,
					 uint32_t id)
{
	struct camera_object *camera;
	spa_list_for_each(camera, &pw_camera->cameras, link)
	{
		if (camera->id == id)
			return camera;
	}
	return NULL;
}

static uint32_t clear_params(struct spa_list *param_list, uint32_t id)
{
	struct param *p, *t;
	uint32_t count = 0;

	spa_list_for_each_safe(p, t, param_list, link)
	{
		if (id == SPA_ID_INVALID || p->id == id) {
			spa_list_remove(&p->link);
			free(p);
			count++;
		}
	}
	return count;
}

static struct param *add_param(struct spa_list *params, uint32_t id,
			       const struct spa_pod *param)
{
	struct param *p;

	if (id == SPA_ID_INVALID) {
		if (param == NULL || !spa_pod_is_object(param)) {
			errno = EINVAL;
			return NULL;
		}
		id = SPA_POD_OBJECT_ID(param);
	}

	p = malloc(sizeof(*p) + (param != NULL ? SPA_POD_SIZE(param) : 0));
	if (p == NULL)
		return NULL;

	p->id = id;
	if (param != NULL) {
		p->param = SPA_MEMBER(p, sizeof(struct param), struct spa_pod);
		memcpy(p->param, param, SPA_POD_SIZE(param));
	} else {
		clear_params(params, id);
		p->param = NULL;
	}
	spa_list_append(params, &p->link);

	return p;
}

static void stream_camera(struct obs_pipewire_camera *pw_camera,
			  const char *camera_card)
{
	struct camera_object *camera;

	blog(LOG_INFO, "Streaming %s", camera_card);

	spa_list_for_each(camera, &pw_camera->cameras, link)
	{
		if (!camera || !camera->info)
			continue;
		const char *card =
			spa_dict_lookup(camera->info->props, SPA_KEY_NODE_NAME);

		if (strcmp(camera_card, card) == 0) {
			blog(LOG_INFO, "Found %s", card);

			if (pw_camera->current_camera == camera)
				return;

			g_clear_pointer(&pw_camera->obs_pw,
					obs_pipewire_destroy);
			pw_camera->obs_pw = obs_pipewire_new_full(
				pw_camera->core,
				pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
						  PW_KEY_MEDIA_CATEGORY,
						  "Capture", PW_KEY_MEDIA_ROLE,
						  "Camera", NULL),
				camera->id, pw_camera->source);
			return;
		}
	}
}

/* ------------------------------------------------- */

static void on_node_info_cb(void *user_data, const struct pw_node_info *info)
{
	struct camera_object *camera = user_data;
	const struct spa_dict_item *it;

	blog(LOG_INFO, "[pipewire] Updating node info for camera %d",
	     camera->id);

	info = camera->info = pw_node_info_update(camera->info, info);

	spa_dict_for_each(it, info->props)
	{
		blog(LOG_INFO, "[pipewire]     Camera id:%d  property %s = %s",
		     camera->id, it->key, it->value);
	}
}

static void on_node_param_cb(void *user_data, int seq, uint32_t id,
			     uint32_t index, uint32_t next,
			     const struct spa_pod *param)
{
	struct camera_object *camera = user_data;
	//add_param(&camera->pending_list, id, param);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.info = on_node_info_cb,
	.param = on_node_param_cb,
};

static void on_proxy_removed_cb(void *user_data)
{
	struct camera_object *camera = user_data;
	pw_proxy_destroy(camera->proxy);
}

static void on_proxy_destroy_cb(void *user_data)
{
	struct camera_object *camera = user_data;

	spa_hook_remove(&camera->proxy_listener);
	camera->proxy = NULL;
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.removed = on_proxy_removed_cb,
	.destroy = on_proxy_destroy_cb,
};

static void on_registry_global_cb(void *user_data, uint32_t id,
				  uint32_t permissions, const char *type,
				  uint32_t version,
				  const struct spa_dict *props)
{
	struct obs_pipewire_camera *pw_camera = user_data;
	struct camera_object *camera;

	if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
		return;

	camera = bzalloc(sizeof(struct camera_object));
	camera->pw_camera = pw_camera;
	camera->id = id;
	camera->permissions = permissions;
	camera->type = bstrdup(type);
	camera->version = version;
	camera->props = props ? pw_properties_new_dict(props) : NULL;

	blog(LOG_INFO, "[pipewire] Adding global %u of type %s", id, type);

	camera->proxy =
		pw_registry_bind(pw_camera->registry, id, type, version, 0);
	if (camera->proxy == NULL)
		goto bind_failed;

	pw_proxy_add_listener(camera->proxy, &camera->proxy_listener,
			      &proxy_events, camera);

	pw_proxy_add_object_listener(camera->proxy, &camera->object_listener,
				     &node_events, camera);

	spa_list_append(&pw_camera->cameras, &camera->link);

	sync_pipewire_core(pw_camera);
	return;

bind_failed:
	pw_log_error("can't bind object for %u %s/%d: %m", id, type, version);
	pw_properties_free(camera->props);
	bfree(camera);
	return;
}

static void on_registry_global_remove_cb(void *user_data, uint32_t id)
{
	struct obs_pipewire_camera *pw_camera = user_data;
	struct camera_object *camera = find_camera(pw_camera, id);

	if (camera)
		camera_destroy(camera);
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = on_registry_global_cb,
	.global_remove = on_registry_global_remove_cb,
};

static void on_core_error_cb(void *user_data, uint32_t id, int seq, int res,
			     const char *message)
{
	UNUSED_PARAMETER(seq);

	struct obs_pipewire_camera *pw_camera = user_data;

	blog(LOG_ERROR, "[pipewire] Error id:%u seq:%d res:%d (%s): %s", id,
	     seq, res, g_strerror(res), message);

	pw_thread_loop_signal(pw_camera->thread_loop, FALSE);
}

static void on_core_done_cb(void *user_data, uint32_t id, int seq)
{
	UNUSED_PARAMETER(seq);

	struct obs_pipewire_camera *pw_camera = user_data;

	if (id == PW_ID_CORE) {
		if (pw_camera->sync_seq != seq)
			return;

		blog(LOG_INFO, "[pipewire] Sync end %u/%u", pw_camera->sync_seq,
		     seq);

		stream_camera(pw_camera, pw_camera->defaults.device_id);

		pw_thread_loop_signal(pw_camera->thread_loop, FALSE);
	}
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = on_core_done_cb,
	.error = on_core_error_cb,
};

static void connect_to_pipewire(struct obs_pipewire_camera *pw_camera,
				int pipewire_fd)
{
	pw_camera->pipewire_fd = pipewire_fd;

	pw_camera->thread_loop =
		pw_thread_loop_new("PipeWire thread loop for OBS Studio", NULL);
	pw_camera->context = pw_context_new(
		pw_thread_loop_get_loop(pw_camera->thread_loop), NULL, 0);

	if (pw_thread_loop_start(pw_camera->thread_loop) < 0) {
		blog(LOG_WARNING, "Error starting threaded mainloop");
		goto fail;
	}

	pw_thread_loop_lock(pw_camera->thread_loop);

	/* Core */
	pw_camera->core = pw_context_connect_fd(
		pw_camera->context, fcntl(pipewire_fd, F_DUPFD_CLOEXEC, 5),
		NULL, 0);
	if (!pw_camera->core) {
		blog(LOG_WARNING, "Error creating PipeWire core: %m");
		pw_thread_loop_unlock(pw_camera->thread_loop);
		goto fail;
	}

	pw_core_add_listener(pw_camera->core, &pw_camera->core_listener,
			     &core_events, pw_camera);

	/* Registry */
	pw_camera->registry =
		pw_core_get_registry(pw_camera->core, PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(pw_camera->registry,
				 &pw_camera->registry_listener,
				 &registry_events, pw_camera);

	pw_thread_loop_unlock(pw_camera->thread_loop);

	return;

fail:
	pw_thread_loop_unlock(pw_camera->thread_loop);
	obs_pipewire_camera_free(pw_camera);
}

/* ------------------------------------------------- */

static void on_pipewire_remote_opened_cb(GObject *source, GAsyncResult *res,
					 void *user_data)
{
	struct obs_pipewire_camera *pw_camera = user_data;
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

	/* TODO: create pw_context; inspect objects */
	connect_to_pipewire(pw_camera, pipewire_fd);
}

static void open_pipewire_remote(struct obs_pipewire_camera *pw_camera)
{
	GVariantBuilder options;

	g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);

	g_dbus_proxy_call_with_unix_fd_list(
		portal_get_camera_proxy(), "OpenPipeWireRemote",
		g_variant_new("(a{sv})", &options), G_DBUS_CALL_FLAGS_NONE, -1,
		NULL, pw_camera->cancellable, on_pipewire_remote_opened_cb,
		pw_camera);
}

/* ------------------------------------------------- */

static void on_access_camera_response_received_cb(
	GDBusConnection *connection, const char *sender_name,
	const char *object_path, const char *interface_name,
	const char *signal_name, GVariant *parameters, void *user_data)
{
	UNUSED_PARAMETER(connection);
	UNUSED_PARAMETER(sender_name);
	UNUSED_PARAMETER(object_path);
	UNUSED_PARAMETER(interface_name);
	UNUSED_PARAMETER(signal_name);

	g_autoptr(GVariant) result = NULL;
	struct obs_pipewire_camera *pw_camera = user_data;
	uint32_t response;

	g_variant_get(parameters, "(u@a{sv})", &response, &result);

	if (response != 0) {
		blog(LOG_WARNING,
		     "[pipewire] Failed to create session, denied or cancelled by user");
		return;
	}

	blog(LOG_INFO, "[pipewire] Successfully accessed cameras");

	open_pipewire_remote(pw_camera);
}

static void on_access_camera_finished_cb(GObject *source, GAsyncResult *res,
					 void *user_data)
{
	UNUSED_PARAMETER(user_data);

	g_autoptr(GVariant) result = NULL;
	g_autoptr(GError) error = NULL;

	result = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), res, &error);
	if (error) {
		if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			blog(LOG_ERROR, "[pipewire] Error accessing camera: %s",
			     error->message);
		return;
	}
}

static void access_camera(struct obs_pipewire_camera *pw_camera)
{
	GVariantBuilder options;
	dbus_request *request;
	const char *request_token;

	request = dbus_request_new(pw_camera->cancellable,
				   on_access_camera_response_received_cb,
				   pw_camera);

	request_token = dbus_request_get_token(request);

	g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&options, "{sv}", "handle_token",
			      g_variant_new_string(request_token));

	g_dbus_proxy_call(portal_get_camera_proxy(), "AccessCamera",
			  g_variant_new("(a{sv})", &options),
			  G_DBUS_CALL_FLAGS_NONE, -1, pw_camera->cancellable,
			  on_access_camera_finished_cb, pw_camera);
}

/* ------------------------------------------------- */

static gboolean init_pipewire_camera(struct obs_pipewire_camera *pw_camera)
{
	GDBusConnection *connection;
	GDBusProxy *proxy;

	spa_list_init(&pw_camera->cameras);
	pw_camera->cancellable = g_cancellable_new();
	connection = portal_get_dbus_connection();
	if (!connection)
		return FALSE;
	proxy = portal_get_camera_proxy();
	if (!proxy)
		return FALSE;

	blog(LOG_INFO, "PipeWire initialized (sender name: %s)",
	     dbus_get_sender_name());

	access_camera(pw_camera);

	return TRUE;
}

static void populate_cameras_list(struct obs_pipewire_camera *pw_camera,
				  obs_properties_t *properties)
{
	struct camera_object *camera;

	spa_list_for_each(camera, &pw_camera->cameras, link)
	{
		obs_property_t *prop;

		prop = obs_properties_get(properties, "device_id");
		obs_property_list_add_string(
			prop,
			spa_dict_lookup(camera->info->props,
					SPA_KEY_API_V4L2_CAP_CARD),
			spa_dict_lookup(camera->info->props,
					SPA_KEY_NODE_NAME));
	}
}

/* Settings callbacks */

static bool device_selected(void *data, obs_properties_t *props,
			    obs_property_t *p, obs_data_t *settings)
{
	struct obs_pipewire_camera *pw_camera = data;
	const char *device;

	device = obs_data_get_string(settings, "device_id");

	blog(LOG_INFO, "[pipewire] selected device %s", device);

	stream_camera(pw_camera, device);

	return true;
}

/* obs_source_info methods */

static const char *pipewire_camera_get_name(void *data)
{
	UNUSED_PARAMETER(data);
	return obs_module_text("PipeWireCamera");
}

static void *pipewire_camera_create(obs_data_t *settings, obs_source_t *source)
{
	struct obs_pipewire_camera *pw_camera;

	pw_camera = bzalloc(sizeof(struct obs_pipewire_camera));
	pw_camera->source = source;
	pw_camera->defaults.device_id =
		bstrdup(obs_data_get_string(settings, "device_id"));

	if (!init_pipewire_camera(pw_camera)) {
		obs_pipewire_camera_free(pw_camera);
		return NULL;
	}

	return pw_camera;
}

static void pipewire_camera_destroy(void *data)
{
	obs_pipewire_camera_free(data);
}

static void pipewire_camera_get_defaults(obs_data_t *settings) {}

static obs_properties_t *pipewire_camera_get_properties(void *data)
{
	struct obs_pipewire_camera *pw_camera = data;

	obs_properties_t *properties = obs_properties_create();

	obs_property_t *device_list = obs_properties_add_list(
		properties, "device_id",
		obs_module_text("PipeWireCameraDevice"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);

	obs_property_set_modified_callback2(device_list, device_selected,
					    pw_camera);

	populate_cameras_list(pw_camera, properties);

	return properties;
}

static void pipewire_camera_update(void *data, obs_data_t *settings) {}

static void pipewire_camera_show(void *data)
{
	struct obs_pipewire_camera *pw_camera = data;

	if (pw_camera->obs_pw)
		obs_pipewire_show(pw_camera->obs_pw);
}

static void pipewire_camera_hide(void *data)
{
	struct obs_pipewire_camera *pw_camera = data;

	if (pw_camera->obs_pw)
		obs_pipewire_hide(pw_camera->obs_pw);
}

static uint32_t pipewire_camera_get_width(void *data)
{
	struct obs_pipewire_camera *pw_camera = data;

	if (pw_camera->obs_pw)
		return obs_pipewire_get_width(pw_camera->obs_pw);
	else
		return 0;
}

static uint32_t pipewire_camera_get_height(void *data)
{
	struct obs_pipewire_camera *pw_camera = data;

	if (pw_camera->obs_pw)
		return obs_pipewire_get_height(pw_camera->obs_pw);
	else
		return 0;
}

static void pipewire_camera_video_render(void *data, gs_effect_t *effect)
{
	struct obs_pipewire_camera *pw_camera = data;

	if (pw_camera->obs_pw)
		obs_pipewire_video_render(pw_camera->obs_pw, effect);
}

void pipewire_camera_load(void)
{
	const struct obs_source_info pipewire_camera_info = {
		.id = "pipewire-camera-source",
		.type = OBS_SOURCE_TYPE_INPUT,
		.output_flags = OBS_SOURCE_ASYNC_VIDEO |
				OBS_SOURCE_DO_NOT_DUPLICATE,
		.get_name = pipewire_camera_get_name,
		.create = pipewire_camera_create,
		.destroy = pipewire_camera_destroy,
		.get_defaults = pipewire_camera_get_defaults,
		.get_properties = pipewire_camera_get_properties,
		.update = pipewire_camera_update,
		.show = pipewire_camera_show,
		.hide = pipewire_camera_hide,
		.get_width = pipewire_camera_get_width,
		.get_height = pipewire_camera_get_height,
		//.video_render = pipewire_camera_video_render,
		.icon_type = OBS_ICON_TYPE_CAMERA,
	};
	obs_register_source(&pipewire_camera_info);
}
