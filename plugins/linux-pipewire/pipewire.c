/* pipewire.c
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

#include "pipewire.h"
#include "pipewire-internal.h"
#include "pipewire-utils.h"
#include "pipewire-utils-video.h"

#include <util/darray.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include <fcntl.h>
#include <glad/glad.h>
#include <linux/dma-buf.h>
#include <libdrm/drm_fourcc.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/debug/format.h>
#include <spa/debug/types.h>
#include <spa/param/video/type-info.h>
#include <spa/utils/result.h>

#define CURSOR_META_SIZE(width, height)                                    \
	(sizeof(struct spa_meta_cursor) + sizeof(struct spa_meta_bitmap) + \
	 width * height * 4)

struct _obs_pipewire {
	int pipewire_fd;

	struct pw_thread_loop *thread_loop;
	struct pw_context *context;

	struct pw_core *core;
	struct spa_hook core_listener;
	int sync_id;

	struct obs_pw_version server_version;

	struct pw_registry *registry;
	struct spa_hook registry_listener;

	GPtrArray *streams;
};

/* auxiliary methods */

static bool parse_pw_version(struct obs_pw_version *dst, const char *version)
{
	int n_matches = sscanf(version, "%d.%d.%d", &dst->major, &dst->minor,
			       &dst->micro);
	return n_matches == 3;
}

static void update_pw_versions(obs_pipewire *obs_pw, const char *version)
{
	blog(LOG_INFO, "[pipewire] Server version: %s", version);
	blog(LOG_INFO, "[pipewire] Library version: %s",
	     pw_get_library_version());
	blog(LOG_INFO, "[pipewire] Header version: %s",
	     pw_get_headers_version());

	if (!parse_pw_version(&obs_pw->server_version, version))
		blog(LOG_WARNING, "[pipewire] failed to parse server version");
}

static void teardown_pipewire(obs_pipewire *obs_pw)
{
	if (obs_pw->thread_loop) {
		pw_thread_loop_wait(obs_pw->thread_loop);
		pw_thread_loop_stop(obs_pw->thread_loop);
	}

	g_clear_pointer(&obs_pw->context, pw_context_destroy);
	g_clear_pointer(&obs_pw->thread_loop, pw_thread_loop_destroy);

	if (obs_pw->pipewire_fd > 0) {
		close(obs_pw->pipewire_fd);
		obs_pw->pipewire_fd = 0;
	}
}

static void swap_texture_red_blue(gs_texture_t *texture)
{
	GLuint gl_texure = *(GLuint *)gs_texture_get_obj(texture);

	glBindTexture(GL_TEXTURE_2D, gl_texure);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
	glBindTexture(GL_TEXTURE_2D, 0);
}

static void renegotiate_format(void *data, uint64_t expirations)
{
	UNUSED_PARAMETER(expirations);
	obs_pipewire_stream *obs_pw_stream = (obs_pipewire_stream *)data;
	obs_pipewire *obs_pw = obs_pw_stream->obs_pw;
	const struct spa_pod **params = NULL;

	blog(LOG_INFO, "[pipewire] Renegotiating stream");

	pw_thread_loop_lock(obs_pw->thread_loop);

	uint8_t params_buffer[2048];
	struct spa_pod_builder pod_builder =
		SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
	uint32_t n_params;
	if (!obs_pw_stream->impl->build_format_params(
		    obs_pw_stream, &pod_builder,
		    &params, &n_params)) {
		teardown_pipewire(obs_pw);
		pw_thread_loop_unlock(obs_pw->thread_loop);
		return;
	}

	pw_stream_update_params(obs_pw_stream->stream, params, n_params);
	pw_thread_loop_unlock(obs_pw->thread_loop);
	bfree(params);
}

/* ------------------------------------------------- */

static inline struct pw_buffer *find_latest_buffer(struct pw_stream *stream)
{
	struct pw_buffer *b;

	/* Find the most recent buffer */
	b = NULL;
	while (true) {
		struct pw_buffer *aux = pw_stream_dequeue_buffer(stream);
		if (!aux)
			break;
		if (b)
			pw_stream_queue_buffer(stream, b);
		b = aux;
	}

	return b;
}

static void on_process_cb(void *user_data)
{
	obs_pipewire_stream *obs_pw_stream = user_data;
	struct pw_buffer *b;

	b = find_latest_buffer(obs_pw_stream->stream);
	if (!b) {
		blog(LOG_DEBUG, "[pipewire] Out of buffers!");
		return;
	}

	obs_pw_stream->impl->process_buffer(obs_pw_stream, b);

	pw_stream_queue_buffer(obs_pw_stream->stream, b);
}

static void on_param_changed_cb(void *user_data, uint32_t id,
				const struct spa_pod *param)
{
	obs_pipewire_stream *obs_pw_stream = user_data;

	obs_pw_stream->impl->param_changed(obs_pw_stream, id, param);
}

static void on_state_changed_cb(void *user_data, enum pw_stream_state old,
				enum pw_stream_state state, const char *error)
{
	UNUSED_PARAMETER(old);

	obs_pipewire_stream *obs_pw_stream = user_data;

	blog(LOG_INFO, "[pipewire] Stream %p state: \"%s\" (error: %s)",
	     obs_pw_stream->stream, pw_stream_state_as_string(state),
	     error ? error : "none");
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_state_changed_cb,
	.param_changed = on_param_changed_cb,
	.process = on_process_cb,
};

static void on_core_info_cb(void *user_data, const struct pw_core_info *info)
{
	obs_pipewire *obs_pw = user_data;

	update_pw_versions(obs_pw, info->version);
}

static void on_core_error_cb(void *user_data, uint32_t id, int seq, int res,
			     const char *message)
{
	obs_pipewire *obs_pw = user_data;
	UNUSED_PARAMETER(seq);

	blog(LOG_ERROR, "[pipewire] Error id:%u seq:%d res:%d (%s): %s", id,
	     seq, res, g_strerror(res), message);

	pw_thread_loop_signal(obs_pw->thread_loop, FALSE);
}

static void on_core_done_cb(void *user_data, uint32_t id, int seq)
{
	obs_pipewire *obs_pw = user_data;

	if (id == PW_ID_CORE && obs_pw->sync_id == seq)
		pw_thread_loop_signal(obs_pw->thread_loop, FALSE);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.info = on_core_info_cb,
	.done = on_core_done_cb,
	.error = on_core_error_cb,
};

/* obs_source_info methods */

obs_pipewire *
obs_pipewire_create(int pipewire_fd,
		    const struct pw_registry_events *registry_events,
		    void *user_data)
{
	obs_pipewire *obs_pw;

	obs_pw = bzalloc(sizeof(obs_pipewire));
	obs_pw->pipewire_fd = pipewire_fd;
	obs_pw->thread_loop = pw_thread_loop_new("PipeWire thread loop", NULL);
	obs_pw->context = pw_context_new(
		pw_thread_loop_get_loop(obs_pw->thread_loop), NULL, 0);

	if (pw_thread_loop_start(obs_pw->thread_loop) < 0) {
		blog(LOG_WARNING, "Error starting threaded mainloop");
		bfree(obs_pw);
		return NULL;
	}

	pw_thread_loop_lock(obs_pw->thread_loop);

	/* Core */
	if (obs_pw->pipewire_fd == -1) {
		obs_pw->core = pw_context_connect(obs_pw->context, NULL, 0);
	} else {
		obs_pw->core = pw_context_connect_fd(obs_pw->context,
						     fcntl(obs_pw->pipewire_fd,
							   F_DUPFD_CLOEXEC, 5),
						     NULL, 0);
	}
	if (!obs_pw->core) {
		blog(LOG_WARNING, "Error creating PipeWire core: %m");
		pw_thread_loop_unlock(obs_pw->thread_loop);
		bfree(obs_pw);
		return NULL;
	}

	pw_core_add_listener(obs_pw->core, &obs_pw->core_listener, &core_events,
			     obs_pw);

	// Dispatch to receive the info core event
	obs_pw->sync_id =
		pw_core_sync(obs_pw->core, PW_ID_CORE, obs_pw->sync_id);
	pw_thread_loop_wait(obs_pw->thread_loop);

	/* Registry */
	if (registry_events) {
		obs_pw->registry = pw_core_get_registry(obs_pw->core,
							PW_VERSION_REGISTRY, 0);
		pw_registry_add_listener(obs_pw->registry,
					 &obs_pw->registry_listener,
					 registry_events, user_data);
		blog(LOG_INFO, "[pipewire] Created registry %p",
		     obs_pw->registry);
	}

	pw_thread_loop_unlock(obs_pw->thread_loop);

	obs_pw->streams = g_ptr_array_new();

	return obs_pw;
}

struct pw_registry *obs_pipewire_get_registry(obs_pipewire *obs_pw)
{
	return obs_pw->registry;
}

void obs_pipewire_roundtrip(obs_pipewire *obs_pw)
{
	pw_thread_loop_lock(obs_pw->thread_loop);

	obs_pw->sync_id =
		pw_core_sync(obs_pw->core, PW_ID_CORE, obs_pw->sync_id);
	pw_thread_loop_wait(obs_pw->thread_loop);

	pw_thread_loop_unlock(obs_pw->thread_loop);
}

void obs_pipewire_destroy(obs_pipewire *obs_pw)
{
	if (!obs_pw)
		return;

	while (obs_pw->streams->len > 0) {
		obs_pipewire_stream *obs_pw_stream =
			g_ptr_array_index(obs_pw->streams, 0);
		obs_pipewire_stream_destroy(obs_pw_stream);
	}
	g_clear_pointer(&obs_pw->streams, g_ptr_array_unref);
	teardown_pipewire(obs_pw);
	bfree(obs_pw);
}

bool obs_pipewire_connect_stream(obs_pipewire *obs_pw,
				 obs_pipewire_stream *obs_pw_stream,
				 int pipewire_node, const char *stream_name,
				 struct pw_properties *stream_properties)
{
	struct spa_pod_builder pod_builder;
	const struct spa_pod **params = NULL;
	uint32_t n_params;
	uint8_t params_buffer[2048];

	obs_pw_stream->obs_pw = obs_pw;

	pw_thread_loop_lock(obs_pw->thread_loop);

	/* Signal to renegotiate */
	obs_pw_stream->reneg =
		pw_loop_add_event(pw_thread_loop_get_loop(obs_pw->thread_loop),
				  renegotiate_format, obs_pw);
	blog(LOG_DEBUG, "[pipewire] registered event %p", obs_pw_stream->reneg);

	/* Stream */
	obs_pw_stream->stream =
		pw_stream_new(obs_pw->core, stream_name, stream_properties);
	pw_stream_add_listener(obs_pw_stream->stream,
			       &obs_pw_stream->stream_listener, &stream_events,
			       obs_pw_stream);
	blog(LOG_INFO, "[pipewire] Created stream %p", obs_pw_stream->stream);

	/* Stream parameters */
	pod_builder =
		SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));

	if (!obs_pw_stream->impl->build_format_params(
		    obs_pw_stream, &pod_builder,
		    &params, &n_params)) {
		pw_thread_loop_unlock(obs_pw->thread_loop);
		bfree(obs_pw_stream);
		return false;
	}

	pw_stream_connect(obs_pw_stream->stream, obs_pw_stream->direction,
			  pipewire_node, obs_pw_stream->flags, params,
			  n_params);

	blog(LOG_INFO, "[pipewire] Playing stream %p", obs_pw_stream->stream);

	pw_thread_loop_unlock(obs_pw->thread_loop);
	bfree(params);

	g_ptr_array_add(obs_pw->streams, obs_pw_stream);

	return true;
}

void obs_pipewire_stream_init(struct _obs_pipewire_stream *obs_pw_stream,
			      struct _obs_pipewire_stream_impl *impl)
{
	obs_pw_stream->impl = impl;
}

void obs_pipewire_stream_signal_reneg(obs_pipewire_stream *obs_pw_stream)
{
	pw_loop_signal_event(
		pw_thread_loop_get_loop(obs_pw_stream->obs_pw->thread_loop),
		obs_pw_stream->reneg);
}

struct obs_pw_version *
obs_pipewire_stream_get_serverversion(obs_pipewire_stream *obs_pw_stream)
{
	return &obs_pw_stream->obs_pw->server_version;
}

void obs_pipewire_stream_show(obs_pipewire_stream *obs_pw_stream)
{
	if (obs_pw_stream->stream)
		pw_stream_set_active(obs_pw_stream->stream, true);
}

void obs_pipewire_stream_hide(obs_pipewire_stream *obs_pw_stream)
{
	if (obs_pw_stream->stream)
		pw_stream_set_active(obs_pw_stream->stream, false);
}

uint32_t obs_pipewire_stream_get_width(obs_pipewire_stream *obs_pw_stream)
{
	if (obs_pw_stream->negotiated && obs_pw_stream->impl->get_width)
		return obs_pw_stream->impl->get_width(obs_pw_stream);

	return 0;
}

uint32_t obs_pipewire_stream_get_height(obs_pipewire_stream *obs_pw_stream)
{
	if (obs_pw_stream->negotiated && obs_pw_stream->impl->get_height)
		return obs_pw_stream->impl->get_height(obs_pw_stream);

	return 0;
}

void obs_pipewire_stream_video_render(obs_pipewire_stream *obs_pw_stream,
				      gs_effect_t *effect)
{
	if (obs_pw_stream->impl->render_video)
		obs_pw_stream->impl->render_video(obs_pw_stream, effect);
}

void obs_pipewire_stream_set_cursor_visible(obs_pipewire_stream *obs_pw_stream,
					    bool cursor_visible)
{
	if (obs_pw_stream->impl->set_cursor_visible)
		obs_pw_stream->impl->set_cursor_visible(obs_pw_stream,
							cursor_visible);
}

void obs_pipewire_stream_destroy(obs_pipewire_stream *obs_pw_stream)
{
	if (!obs_pw_stream)
		return;

	g_ptr_array_remove(obs_pw_stream->obs_pw->streams, obs_pw_stream);
	if (obs_pw_stream->stream)
		pw_stream_disconnect(obs_pw_stream->stream);
	g_clear_pointer(&obs_pw_stream->stream, pw_stream_destroy);

	obs_pw_stream->impl->destroy(obs_pw_stream);
}
