/* pipewire-utils-video.c
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

#include "pipewire-internal.h"
#include "pipewire-utils.h"
#include "pipewire-utils-video.h"

#include <util/darray.h>


#include <libdrm/drm_fourcc.h>
#include <pipewire/pipewire.h>

/* auxiliary methods */

bool has_effective_crop(struct obs_pw_region *crop,
			struct spa_video_info *format)
{
	return crop->valid && (crop->x != 0 || crop->y != 0 ||
			       crop->width < format->info.raw.size.width ||
			       crop->height < format->info.raw.size.height);
}

/* ------------------------------------------------- */

static const struct format_data supported_formats[] = {
	{
		SPA_VIDEO_FORMAT_BGRA,
		DRM_FORMAT_ARGB8888,
		GS_BGRA,
		VIDEO_FORMAT_BGRA,
		false,
		4,
		"ARGB8888",
	},
	{
		SPA_VIDEO_FORMAT_RGBA,
		DRM_FORMAT_ABGR8888,
		GS_RGBA,
		VIDEO_FORMAT_RGBA,
		false,
		4,
		"ABGR8888",
	},
	{
		SPA_VIDEO_FORMAT_BGRx,
		DRM_FORMAT_XRGB8888,
		GS_BGRX,
		VIDEO_FORMAT_BGRX,
		false,
		4,
		"XRGB8888",
	},
	{
		SPA_VIDEO_FORMAT_RGBx,
		DRM_FORMAT_XBGR8888,
		GS_BGRX,
		VIDEO_FORMAT_NONE,
		true,
		4,
		"XBGR8888",
	},
	{
		SPA_VIDEO_FORMAT_YUY2,
		DRM_FORMAT_YUYV,
		GS_UNKNOWN,
		VIDEO_FORMAT_YUY2,
		false,
		2,
		"YUYV422",
	},
};

#define N_SUPPORTED_FORMATS \
	(sizeof(supported_formats) / sizeof(supported_formats[0]))

static const uint32_t supported_formats_async[] = {
	SPA_VIDEO_FORMAT_RGBA,
	SPA_VIDEO_FORMAT_YUY2,
};

#define N_SUPPORTED_FORMATS_ASYNC \
	(sizeof(supported_formats_async) / sizeof(supported_formats_async[0]))

static const uint32_t supported_formats_sync[] = {
	SPA_VIDEO_FORMAT_BGRA,
	SPA_VIDEO_FORMAT_RGBA,
	SPA_VIDEO_FORMAT_BGRx,
	SPA_VIDEO_FORMAT_RGBx,
};

#define N_SUPPORTED_FORMATS_SYNC \
	(sizeof(supported_formats_sync) / sizeof(supported_formats_sync[0]))

bool lookup_format_info_from_spa_format(uint32_t spa_format,
					struct format_data *out_format_data)
{
	for (size_t i = 0; i < N_SUPPORTED_FORMATS; i++) {
		if (supported_formats[i].spa_format != spa_format)
			continue;

		if (out_format_data)
			*out_format_data = supported_formats[i];

		return true;
	}
	return false;
}

/* ------------------------------------------------- */

static struct spa_pod *build_format(struct spa_pod_builder *b,
				    struct obs_video_info *ovi, uint32_t format,
				    uint64_t *modifiers, size_t modifier_count)
{
	struct spa_pod_frame format_frame;

	/* Make an object of type SPA_TYPE_OBJECT_Format and id SPA_PARAM_EnumFormat.
	 * The object type is important because it defines the properties that are
	 * acceptable. The id gives more context about what the object is meant to
	 * contain. In this case we enumerate supported formats. */
	spa_pod_builder_push_object(b, &format_frame, SPA_TYPE_OBJECT_Format,
				    SPA_PARAM_EnumFormat);
	/* add media type and media subtype properties */
	spa_pod_builder_add(b, SPA_FORMAT_mediaType,
			    SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
	spa_pod_builder_add(b, SPA_FORMAT_mediaSubtype,
			    SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);

	/* formats */
	spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(format), 0);

	/* modifier */
	if (modifier_count > 0) {
		struct spa_pod_frame modifier_frame;

		/* build an enumeration of modifiers */
		spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier,
				     SPA_POD_PROP_FLAG_MANDATORY |
					     SPA_POD_PROP_FLAG_DONT_FIXATE);

		spa_pod_builder_push_choice(b, &modifier_frame, SPA_CHOICE_Enum,
					    0);

		/* The first element of choice pods is the preferred value. Here
		 * we arbitrarily pick the first modifier as the preferred one.
		 */
		spa_pod_builder_long(b, modifiers[0]);

		/* modifiers from  an array */
		for (uint32_t i = 0; i < modifier_count; i++)
			spa_pod_builder_long(b, modifiers[i]);

		spa_pod_builder_pop(b, &modifier_frame);
	}
	/* add size and framerate ranges */
	spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size,
			    SPA_POD_CHOICE_RANGE_Rectangle(
				    &SPA_RECTANGLE(320, 240), // Arbitrary
				    &SPA_RECTANGLE(1, 1),
				    &SPA_RECTANGLE(8192, 4320)),
			    SPA_FORMAT_VIDEO_framerate,
			    SPA_POD_CHOICE_RANGE_Fraction(
				    &SPA_FRACTION(ovi->fps_num, ovi->fps_den),
				    &SPA_FRACTION(0, 1), &SPA_FRACTION(360, 1)),
			    0);
	return spa_pod_builder_pop(b, &format_frame);
}

bool build_format_params(struct darray *f_info,
			 struct obs_pw_version *server_version,
			 struct obs_video_info *ovi,
			 struct spa_pod_builder *pod_builder,
			 const struct spa_pod ***param_list, uint32_t *n_params)
{
	DARRAY(struct format_info) format_info;
	format_info.da = *f_info;
	uint32_t params_count = 0;

	const struct spa_pod **params;
	params = bzalloc(2 * format_info.num * sizeof(struct spa_pod *));

	if (!params) {
		blog(LOG_ERROR,
		     "[pipewire] Failed to allocate memory for param pointers");
		return false;
	}

	if (!check_pw_version(server_version, 0, 3, 33))
		goto build_shm;

	for (size_t i = 0; i < format_info.num; i++) {
		if (format_info.array[i].modifiers.num == 0) {
			continue;
		}
		params[params_count++] = build_format(
			pod_builder, ovi,
			format_info.array[i].spa_format,
			format_info.array[i].modifiers.array,
			format_info.array[i].modifiers.num);
	}

build_shm:
	for (size_t i = 0; i < format_info.num; i++) {
		params[params_count++] = build_format(
			pod_builder, ovi,
			format_info.array[i].spa_format, NULL,
			0);
	}
	*param_list = params;
	*n_params = params_count;
	return true;
}

/* ------------------------------------------------- */

static bool drm_format_available(uint32_t drm_format, uint32_t *drm_formats,
				 size_t n_drm_formats)
{
	for (size_t j = 0; j < n_drm_formats; j++) {
		if (drm_format == drm_formats[j]) {
			return true;
		}
	}
	return false;
}

struct darray create_format_info_async(void)
{
	DARRAY(struct format_info) format_info;
	da_init(format_info);

	for (size_t i = 0; i < N_SUPPORTED_FORMATS_ASYNC; i++) {
		struct format_info *info;
		struct format_data tmp;
		if (!lookup_format_info_from_spa_format(
			    supported_formats_async[i], &tmp))
			continue;

		info = da_push_back_new(format_info);
		da_init(info->modifiers);
		info->spa_format = tmp.spa_format;
		info->drm_format = tmp.drm_format;
	}
	return format_info.da;
}

struct darray create_format_info_sync(void)
{
	DARRAY(struct format_info) format_info;
	da_init(format_info);

	obs_enter_graphics();

	enum gs_dmabuf_flags dmabuf_flags;
	uint32_t *drm_formats = NULL;
	size_t n_drm_formats;

	bool capabilities_queried = gs_query_dmabuf_capabilities(
		&dmabuf_flags, &drm_formats, &n_drm_formats);

	for (size_t i = 0; i < N_SUPPORTED_FORMATS_SYNC; i++) {
		struct format_info *info;
		struct format_data tmp;

		if (!lookup_format_info_from_spa_format(
			    supported_formats_sync[i], &tmp))
			continue;

		if (!drm_format_available(tmp.drm_format, drm_formats,
					  n_drm_formats))
			continue;

		info = da_push_back_new(format_info);
		da_init(info->modifiers);
		info->spa_format = tmp.spa_format;
		info->drm_format = tmp.drm_format;

		if (!capabilities_queried)
			continue;

		size_t n_modifiers;
		uint64_t *modifiers = NULL;
		if (gs_query_dmabuf_modifiers_for_format(
			    tmp.drm_format, &modifiers, &n_modifiers)) {
			da_push_back_array(info->modifiers, modifiers,
					   n_modifiers);
		}
		bfree(modifiers);

		if (dmabuf_flags &
		    GS_DMABUF_FLAG_IMPLICIT_MODIFIERS_SUPPORTED) {
			uint64_t modifier_implicit = DRM_FORMAT_MOD_INVALID;
			da_push_back(info->modifiers, &modifier_implicit);
		}
	}
	obs_leave_graphics();

	bfree(drm_formats);

	return format_info.da;
}

void clear_format_info(struct darray *f_info)
{
	DARRAY(struct format_info) format_info;
	format_info.da = *f_info;
	for (size_t i = 0; i < format_info.num; i++) {
		da_free(format_info.array[i].modifiers);
	}
	da_free(format_info);
}

void remove_modifier_from_format(struct darray *f_info,
				 struct obs_pw_version *server_version,
				 uint32_t spa_format, uint64_t modifier)
{
	DARRAY(struct format_info) format_info;
	format_info.da = *f_info;
	for (size_t i = 0; i < format_info.num; i++) {
		if (format_info.array[i].spa_format != spa_format)
			continue;

		if (!check_pw_version(server_version, 0, 3, 40)) {
			da_erase_range(format_info.array[i].modifiers, 0,
				       format_info.array[i].modifiers.num - 1);
			continue;
		}

		int idx = da_find(format_info.array[i].modifiers, &modifier, 0);
		while (idx != -1) {
			da_erase(format_info.array[i].modifiers, idx);
			idx = da_find(format_info.array[i].modifiers, &modifier,
				      0);
		}
	}
}
