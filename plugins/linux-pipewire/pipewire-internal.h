/* pipewire-internal.h
 *
 * Copyright 2020 Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
 * Copyright 2022 columbarius <co1umbarius@protonmail.com>
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

#include <obs-module.h>
#include <pipewire/pipewire.h>
#include <spa/pod/builder.h>

struct _obs_pipewire_stream;
struct _obs_pipewire;

struct obs_pw_version {
	int major;
	int minor;
	int micro;
};

struct _obs_pipewire_stream_impl {
	void (*process_buffer)(struct _obs_pipewire_stream *obs_pw_stream,
			       struct pw_buffer *b);
	void (*param_changed)(struct _obs_pipewire_stream *obs_pw_stream,
			      uint32_t id, const struct spa_pod *param);
	bool (*build_format_params)(struct _obs_pipewire_stream *obs_pw_stream,
				    struct spa_pod_builder *b,
				    const struct spa_pod ***params_list,
				    uint32_t *n_params);
	void (*destroy)(struct _obs_pipewire_stream *obs_pw_stream);
	// Video functions
	uint32_t (*get_width)(struct _obs_pipewire_stream *obs_pw_stream);
	uint32_t (*get_height)(struct _obs_pipewire_stream *obs_pw_stream);
	void (*render_video)(struct _obs_pipewire_stream *obs_pw_stream,
			     gs_effect_t *effect);
	void (*set_cursor_visible)(struct _obs_pipewire_stream *obs_pw_stream,
				   bool cursor_visible);
};

struct _obs_pipewire_stream {
	struct _obs_pipewire_stream_impl *impl;
	struct _obs_pipewire *obs_pw;

	struct pw_stream *stream;
	struct spa_hook stream_listener;
	struct spa_source *reneg;

	enum pw_direction direction;
	uint32_t flags;

	bool negotiated;
};

void obs_pipewire_stream_init(struct _obs_pipewire_stream *obs_pipewire_stream,
			      struct _obs_pipewire_stream_impl *impl);

void obs_pipewire_stream_signal_reneg(struct _obs_pipewire_stream *obs_pipewire_stream);
struct obs_pw_version *obs_pipewire_stream_get_serverversion(struct _obs_pipewire_stream *obs_pipewire_stream);
