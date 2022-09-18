/* pipewire-utils-video.h
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
#include <spa/param/video/format-utils.h>

struct format_info {
	uint32_t spa_format;
	uint32_t drm_format;
	DARRAY(uint64_t) modifiers;
};

struct obs_pw_region {
	bool valid;
	int x, y;
	uint32_t width, height;
};

struct format_data {
	uint32_t spa_format;
	uint32_t drm_format;
	enum gs_color_format gs_format;
	enum video_format video_format;
	bool swap_red_blue;
	uint32_t bpp;
	const char *pretty_name;
};

bool has_effective_crop(struct obs_pw_region *crop,
			struct spa_video_info *format);

bool lookup_format_info_from_spa_format(uint32_t spa_format,
					struct format_data *out_format_data);

struct darray create_format_info_async(void);
struct darray create_format_info_sync(void);
void clear_format_info(struct darray *f_info);
void remove_modifier_from_format(struct darray *f_info,
				 struct obs_pw_version *server_version,
				 uint32_t spa_format, uint64_t modifier);

bool build_format_params(struct darray *f_info,
			 struct obs_pw_version *server_version,
			 struct obs_video_info *ovi,
			 struct spa_pod_builder *pod_builder,
			 const struct spa_pod ***param_list,
			 uint32_t *n_params);
