/* pipewire-stream-video-sync.c
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

struct _obs_pipewire_stream_video {
	struct _obs_pipewire_stream obs_pw_stream;

	obs_source_t *source;

	gs_texture_t *texture;

	struct spa_video_info format;

	struct obs_pw_region crop;

	struct {
		bool visible;
		bool valid;
		int x, y;
		int hotspot_x, hotspot_y;
		int width, height;
		gs_texture_t *texture;
	} cursor;

	struct obs_video_info video_info;

	DARRAY(struct format_info) format_info;
};

/* auxiliary methods */

static void swap_texture_red_blue(gs_texture_t *texture)
{
	GLuint gl_texure = *(GLuint *)gs_texture_get_obj(texture);

	glBindTexture(GL_TEXTURE_2D, gl_texure);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
	glBindTexture(GL_TEXTURE_2D, 0);
}

/* obs_pipewire_stream methods */

static const struct _obs_pipewire_stream_impl stream_impl;

static bool
obs_pipewire_stream_is_video_stream(obs_pipewire_stream *obs_pw_stream)
{
	return obs_pw_stream->impl == &stream_impl;
}

static struct _obs_pipewire_stream_video *
video_stream_get_stream(obs_pipewire_stream *obs_pw_stream)
{
	assert(obs_pipewire_stream_is_video_stream(obs_pw_stream));
	return (struct _obs_pipewire_stream_video *)obs_pw_stream;
}

static void video_stream_process_buffer(obs_pipewire_stream *obs_pw_stream,
					struct pw_buffer *b)
{
	struct _obs_pipewire_stream_video *video_stream =
		video_stream_get_stream(obs_pw_stream);
	struct spa_meta_cursor *cursor;
	struct spa_meta_region *region;
	struct format_data format_data;
	struct spa_buffer *buffer;
	bool swap_red_blue = false;
	bool has_buffer;

	buffer = b->buffer;
	has_buffer = buffer->datas[0].chunk->size != 0;

	obs_enter_graphics();

	if (!has_buffer)
		goto read_metadata;

	if (buffer->datas[0].type == SPA_DATA_DmaBuf) {
		uint32_t planes = buffer->n_datas;
		uint32_t offsets[planes];
		uint32_t strides[planes];
		uint64_t modifiers[planes];
		int fds[planes];
		bool use_modifiers;

		blog(LOG_DEBUG,
		     "[pipewire] DMA-BUF info: fd:%ld, stride:%d, offset:%u, size:%dx%d",
		     buffer->datas[0].fd, buffer->datas[0].chunk->stride,
		     buffer->datas[0].chunk->offset,
		     video_stream->format.info.raw.size.width,
		     video_stream->format.info.raw.size.height);

		if (!lookup_format_info_from_spa_format(
			    video_stream->format.info.raw.format,
			    &format_data) ||
		    format_data.gs_format == GS_UNKNOWN) {
			blog(LOG_ERROR,
			     "[pipewire] unsupported DMA buffer format: %d",
			     video_stream->format.info.raw.format);
			goto read_metadata;
		}

		for (uint32_t plane = 0; plane < planes; plane++) {
			fds[plane] = buffer->datas[plane].fd;
			offsets[plane] = buffer->datas[plane].chunk->offset;
			strides[plane] = buffer->datas[plane].chunk->stride;
			modifiers[plane] =
				video_stream->format.info.raw.modifier;
		}

		g_clear_pointer(&video_stream->texture, gs_texture_destroy);

		use_modifiers = video_stream->format.info.raw.modifier !=
				DRM_FORMAT_MOD_INVALID;
		video_stream->texture = gs_texture_create_from_dmabuf(
			video_stream->format.info.raw.size.width,
			video_stream->format.info.raw.size.height,
			format_data.drm_format, GS_BGRX, planes, fds, strides,
			offsets, use_modifiers ? modifiers : NULL);

		if (video_stream->texture == NULL) {
			remove_modifier_from_format(
				&video_stream->format_info.da,
				obs_pipewire_stream_get_serverversion(
					&video_stream->obs_pw_stream),
				video_stream->format.info.raw.format,
				video_stream->format.info.raw.modifier);
			obs_pipewire_stream_signal_reneg(
				&video_stream->obs_pw_stream);
		}
	} else {
		blog(LOG_DEBUG, "[pipewire] Buffer has memory texture");

		if (!lookup_format_info_from_spa_format(
			    video_stream->format.info.raw.format,
			    &format_data) ||
		    format_data.gs_format == GS_UNKNOWN) {
			blog(LOG_ERROR,
			     "[pipewire] unsupported DMA buffer format: %d",
			     video_stream->format.info.raw.format);
			goto read_metadata;
		}

		g_clear_pointer(&video_stream->texture, gs_texture_destroy);
		video_stream->texture = gs_texture_create(
			video_stream->format.info.raw.size.width,
			video_stream->format.info.raw.size.height,
			format_data.gs_format, 1,
			(const uint8_t **)&buffer->datas[0].data, GS_DYNAMIC);
	}

	if (swap_red_blue)
		swap_texture_red_blue(video_stream->texture);

	/* Video Crop */
	region = spa_buffer_find_meta_data(buffer, SPA_META_VideoCrop,
					   sizeof(*region));
	if (region && spa_meta_region_is_valid(region)) {
		blog(LOG_DEBUG,
		     "[pipewire] Crop Region available (%dx%d+%d+%d)",
		     region->region.position.x, region->region.position.y,
		     region->region.size.width, region->region.size.height);

		video_stream->crop.x = region->region.position.x;
		video_stream->crop.y = region->region.position.y;
		video_stream->crop.width = region->region.size.width;
		video_stream->crop.height = region->region.size.height;
		video_stream->crop.valid = true;
	} else {
		video_stream->crop.valid = false;
	}

read_metadata:

	/* Cursor */
	cursor = spa_buffer_find_meta_data(buffer, SPA_META_Cursor,
					   sizeof(*cursor));
	video_stream->cursor.valid = cursor && spa_meta_cursor_is_valid(cursor);
	if (video_stream->cursor.visible && video_stream->cursor.valid) {
		struct spa_meta_bitmap *bitmap = NULL;

		if (cursor->bitmap_offset)
			bitmap = SPA_MEMBER(cursor, cursor->bitmap_offset,
					    struct spa_meta_bitmap);

		if (bitmap && bitmap->size.width > 0 &&
		    bitmap->size.height > 0 &&
		    lookup_format_info_from_spa_format(bitmap->format,
						       &format_data) &&
		    format_data.gs_format != GS_UNKNOWN) {
			const uint8_t *bitmap_data;

			bitmap_data =
				SPA_MEMBER(bitmap, bitmap->offset, uint8_t);
			video_stream->cursor.hotspot_x = cursor->hotspot.x;
			video_stream->cursor.hotspot_y = cursor->hotspot.y;
			video_stream->cursor.width = bitmap->size.width;
			video_stream->cursor.height = bitmap->size.height;

			g_clear_pointer(&video_stream->cursor.texture,
					gs_texture_destroy);
			video_stream->cursor.texture =
				gs_texture_create(video_stream->cursor.width,
						  video_stream->cursor.height,
						  format_data.gs_format, 1,
						  &bitmap_data, GS_DYNAMIC);

			if (swap_red_blue)
				swap_texture_red_blue(
					video_stream->cursor.texture);
		}

		video_stream->cursor.x = cursor->position.x;
		video_stream->cursor.y = cursor->position.y;
	}

	obs_leave_graphics();
}

static void video_stream_param_changed(obs_pipewire_stream *obs_pw_stream,
				       uint32_t id, const struct spa_pod *param)
{
	struct _obs_pipewire_stream_video *video_stream =
		video_stream_get_stream(obs_pw_stream);
	struct spa_pod_builder pod_builder;
	const struct spa_pod *params[3];
	uint32_t buffer_types;
	uint8_t params_buffer[1024];
	int result;

	if (!param || id != SPA_PARAM_Format)
		return;

	result = spa_format_parse(param, &video_stream->format.media_type,
				  &video_stream->format.media_subtype);
	if (result < 0)
		return;

	if (video_stream->format.media_type != SPA_MEDIA_TYPE_video ||
	    video_stream->format.media_subtype != SPA_MEDIA_SUBTYPE_raw)
		return;

	spa_format_video_raw_parse(param, &video_stream->format.info.raw);

	buffer_types = 1 << SPA_DATA_MemPtr;
	bool has_modifier =
		spa_pod_find_prop(param, NULL, SPA_FORMAT_VIDEO_modifier) !=
		NULL;
	if (has_modifier ||
	    check_pw_version(obs_pipewire_stream_get_serverversion(
				     &video_stream->obs_pw_stream),
			     0, 3, 24))
		buffer_types |= 1 << SPA_DATA_DmaBuf;

	blog(LOG_INFO, "[pipewire] Negotiated format:");

	blog(LOG_INFO, "[pipewire]     Format: %d (%s)",
	     video_stream->format.info.raw.format,
	     spa_debug_type_find_name(spa_type_video_format,
				      video_stream->format.info.raw.format));

	if (has_modifier) {
		blog(LOG_INFO, "[pipewire]     Modifier: %" PRIu64,
		     video_stream->format.info.raw.modifier);
	}

	blog(LOG_INFO, "[pipewire]     Size: %dx%d",
	     video_stream->format.info.raw.size.width,
	     video_stream->format.info.raw.size.height);

	blog(LOG_INFO, "[pipewire]     Framerate: %d/%d",
	     video_stream->format.info.raw.framerate.num,
	     video_stream->format.info.raw.framerate.denom);

	/* Video crop */
	pod_builder =
		SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
	params[0] = spa_pod_builder_add_object(
		&pod_builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop),
		SPA_PARAM_META_size,
		SPA_POD_Int(sizeof(struct spa_meta_region)));

	/* Cursor */
	params[1] = spa_pod_builder_add_object(
		&pod_builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
		SPA_PARAM_META_size,
		SPA_POD_CHOICE_RANGE_Int(CURSOR_META_SIZE(64, 64),
					 CURSOR_META_SIZE(1, 1),
					 CURSOR_META_SIZE(1024, 1024)));

	/* Buffer options */
	params[2] = spa_pod_builder_add_object(
		&pod_builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_dataType, SPA_POD_Int(buffer_types));

	pw_stream_update_params(obs_pw_stream->stream, params, 3);

	obs_pw_stream->negotiated = true;
}

static bool video_stream_build_format_params(obs_pipewire_stream *obs_pw_stream,
					     struct spa_pod_builder *b,
					     const struct spa_pod ***param_list,
					     uint32_t *n_params)
{
	struct _obs_pipewire_stream_video *video_stream =
		video_stream_get_stream(obs_pw_stream);
	return build_format_params(&video_stream->format_info.da,
				   obs_pipewire_stream_get_serverversion(
					   &video_stream->obs_pw_stream),
				   &video_stream->video_info, b, param_list,
				   n_params);
}

static uint32_t video_stream_get_width(obs_pipewire_stream *obs_pw_stream)
{
	if (!obs_pw_stream->negotiated)
		return 0;

	struct _obs_pipewire_stream_video *video_stream =
		video_stream_get_stream(obs_pw_stream);
	if (video_stream->crop.valid)
		return video_stream->crop.width;
	else
		return video_stream->format.info.raw.size.width;
}

static uint32_t video_stream_get_height(obs_pipewire_stream *obs_pw_stream)
{
	if (!obs_pw_stream->negotiated)
		return 0;

	struct _obs_pipewire_stream_video *video_stream =
		video_stream_get_stream(obs_pw_stream);
	if (video_stream->crop.valid)
		return video_stream->crop.height;
	else
		return video_stream->format.info.raw.size.height;
}

static void video_stream_render_video(obs_pipewire_stream *obs_pw_stream,
				      gs_effect_t *effect)
{
	struct _obs_pipewire_stream_video *video_stream =
		video_stream_get_stream(obs_pw_stream);
	gs_eparam_t *image;

	if (!video_stream->texture)
		return;

	image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, video_stream->texture);

	if (has_effective_crop(&video_stream->crop, &video_stream->format)) {
		gs_draw_sprite_subregion(video_stream->texture, 0,
					 video_stream->crop.x,
					 video_stream->crop.y,
					 video_stream->crop.width,
					 video_stream->crop.height);
	} else {
		gs_draw_sprite(video_stream->texture, 0, 0, 0);
	}

	if (video_stream->cursor.visible && video_stream->cursor.valid &&
	    video_stream->cursor.texture) {
		float cursor_x =
			video_stream->cursor.x - video_stream->cursor.hotspot_x;
		float cursor_y =
			video_stream->cursor.y - video_stream->cursor.hotspot_y;

		gs_matrix_push();
		gs_matrix_translate3f(cursor_x, cursor_y, 0.0f);

		gs_effect_set_texture(image, video_stream->cursor.texture);
		gs_draw_sprite(video_stream->texture, 0,
			       video_stream->cursor.width,
			       video_stream->cursor.height);

		gs_matrix_pop();
	}
}

void video_stream_set_cursor_visible(obs_pipewire_stream *obs_pw_stream,
				     bool cursor_visible)
{
	struct _obs_pipewire_stream_video *video_stream =
		video_stream_get_stream(obs_pw_stream);
	video_stream->cursor.visible = cursor_visible;
}

static void video_stream_destroy(obs_pipewire_stream *obs_pw_stream)
{
	struct _obs_pipewire_stream_video *video_stream =
		video_stream_get_stream(obs_pw_stream);

	if (!obs_pw_stream)
		return;

	obs_enter_graphics();
	g_clear_pointer(&video_stream->cursor.texture, gs_texture_destroy);
	g_clear_pointer(&video_stream->texture, gs_texture_destroy);
	obs_leave_graphics();

	clear_format_info(&video_stream->format_info.da);
	bfree(video_stream);
}

static const struct _obs_pipewire_stream_impl stream_impl = {
	.process_buffer = video_stream_process_buffer,
	.param_changed = video_stream_param_changed,
	.build_format_params = video_stream_build_format_params,
	.destroy = video_stream_destroy,
	.get_width = video_stream_get_width,
	.get_height = video_stream_get_height,
	.render_video = video_stream_render_video,
	.set_cursor_visible = video_stream_set_cursor_visible,
};

obs_pipewire_stream *obs_pipewire_create_stream_video_sync(obs_source_t *source)
{
	struct _obs_pipewire_stream_video *video_stream;

	video_stream = bzalloc(sizeof(struct _obs_pipewire_stream_video));
	video_stream->source = source;
	video_stream->obs_pw_stream.direction = PW_DIRECTION_INPUT;
	video_stream->obs_pw_stream.flags = PW_STREAM_FLAG_AUTOCONNECT |
					    PW_STREAM_FLAG_MAP_BUFFERS;

	obs_pipewire_stream_init(&video_stream->obs_pw_stream, &stream_impl);
	video_stream->format_info.da = create_format_info_sync();

	obs_get_video_info(&video_stream->video_info);

	return &video_stream->obs_pw_stream;
}
