/* pipewire-stream-video-async.c
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
#include "pipewire-decoder.h"
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

struct _obs_pipewire_stream_video {
	struct _obs_pipewire_stream obs_pw_stream;

	obs_source_t *source;

	struct spa_video_info format;
	struct pipewire_decoder decoder;

	struct obs_video_info video_info;

	DARRAY(struct format_info) format_info;
};

/* auxiliary methods */

static enum video_colorspace
video_colorspace_from_spa_color_matrix(enum spa_video_color_matrix matrix)
{
	switch (matrix) {
	case SPA_VIDEO_COLOR_MATRIX_RGB:
		return VIDEO_CS_DEFAULT;
	case SPA_VIDEO_COLOR_MATRIX_BT601:
		return VIDEO_CS_601;
	case SPA_VIDEO_COLOR_MATRIX_BT709:
		return VIDEO_CS_709;
	default:
		return VIDEO_CS_DEFAULT;
	}
}

static enum video_range_type
video_color_range_from_spa_color_range(enum spa_video_color_range colorrange)
{
	switch (colorrange) {
	case SPA_VIDEO_COLOR_RANGE_0_255:
		return VIDEO_RANGE_FULL;
	case SPA_VIDEO_COLOR_RANGE_16_235:
		return VIDEO_RANGE_PARTIAL;
	default:
		return VIDEO_RANGE_DEFAULT;
	}
}

static bool prepare_obs_frame(struct _obs_pipewire_stream_video *stream, struct spa_buffer *buffer,
			      struct obs_source_frame *frame)
{
	struct format_data format_data;

	switch (stream->format.media_subtype) {
	case SPA_MEDIA_SUBTYPE_raw:

		frame->width = stream->format.info.raw.size.width;
		frame->height = stream->format.info.raw.size.height;

		video_format_get_parameters(video_colorspace_from_spa_color_matrix(
					    stream->format.info.raw.color_matrix),
					    video_color_range_from_spa_color_range(
						    stream->format.info.raw.color_range),
					    frame->color_matrix, frame->color_range_min,
					    frame->color_range_max);

		if (!lookup_format_info_from_spa_format(stream->format.info.raw.format,
							&format_data) ||
		    format_data.video_format == VIDEO_FORMAT_NONE)
			return false;

		frame->format = format_data.video_format;
		frame->linesize[0] = SPA_ROUND_UP_N(frame->width * format_data.bpp, 4);

		for (uint32_t i = 0; i < buffer->n_datas && i < MAX_AV_PLANES; i++) {
			frame->data[i] = buffer->datas[i].data;
			if (frame->data[i] == NULL) {
				blog(LOG_ERROR, "[pipewire] Failed to access data");
				return false;
			}
		}
		break;
	default:
		if (pipewire_decode_frame(frame, buffer->datas[0].data,
						buffer->datas[0].chunk->size,
						&stream->decoder) < 0) {
			blog(LOG_ERROR, "failed to unpack jpeg or h264");
			break;
		}
		break;
	}

	blog(LOG_DEBUG, "[pipewire] Camera frame info: Format: %s, Planes: %u",
		get_video_format_name(frame->format), buffer->n_datas);
	for (uint32_t i = 0; i < buffer->n_datas && i < MAX_AV_PLANES; i++) {
		blog(LOG_DEBUG, "[pipewire] Plane %u: Dataptr:%p, Linesize:%d",
			i, frame->data[i], frame->linesize[i]);
	}
	return true;
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
	struct spa_buffer *buffer;
	bool has_buffer;

	buffer = b->buffer;
	has_buffer = buffer->datas[0].chunk->size != 0;

	if (!has_buffer)
		return;

	blog(LOG_DEBUG, "[pipewire] Buffer has memory texture");

	struct obs_source_frame out = {0};

	if (!prepare_obs_frame(video_stream, buffer, &out)) {
		blog(LOG_ERROR, "[pipewire] Couldn't prepare frame");
		return;
	}

	obs_source_output_video(video_stream->source, &out);
}

static void video_stream_param_changed(obs_pipewire_stream *obs_pw_stream,
				       uint32_t id, const struct spa_pod *param)
{
	struct _obs_pipewire_stream_video *video_stream =
		video_stream_get_stream(obs_pw_stream);
	struct spa_pod_builder pod_builder;
	const struct spa_pod *params[2];
	uint32_t buffer_types;
	uint8_t params_buffer[1024];
	int result;
	struct spa_rectangle size;
	struct spa_fraction rate;

	if (!param || id != SPA_PARAM_Format)
		return;

	spa_zero(video_stream->format);
	result = spa_format_parse(param, &video_stream->format.media_type,
				  &video_stream->format.media_subtype);
	if (result < 0)
		return;

	if (video_stream->format.media_type != SPA_MEDIA_TYPE_video)
		return;

	switch (video_stream->format.media_subtype) {
	case SPA_MEDIA_SUBTYPE_raw:
		if (spa_format_video_raw_parse(param, &video_stream->format.info.raw) < 0)
			return;

		buffer_types = 1 << SPA_DATA_MemPtr;

		blog(LOG_INFO, "[pipewire] Negotiated format:");
		blog(LOG_INFO, "[pipewire]     Format: %d (%s)",
		     video_stream->format.info.raw.format,
		     spa_debug_type_find_name(spa_type_video_format,
					      video_stream->format.info.raw.format));
		blog(LOG_INFO, "[pipewire]     Size: %dx%d",
		     video_stream->format.info.raw.size.width,
		     video_stream->format.info.raw.size.height);
		blog(LOG_INFO, "[pipewire]     Framerate: %d/%d",
		     video_stream->format.info.raw.framerate.num,
		     video_stream->format.info.raw.framerate.denom);
		break;
	case SPA_MEDIA_SUBTYPE_mjpg:
		if (spa_format_video_mjpg_parse(param, &video_stream->format.info.mjpg) < 0)
			return;

		blog(LOG_INFO, "[pipewire] Negotiated format:");
		blog(LOG_INFO, "[pipewire]     Format: (MJPG)");
		blog(LOG_INFO, "[pipewire]     Size: %dx%d",
		     video_stream->format.info.mjpg.size.width,
		     video_stream->format.info.mjpg.size.height);
		blog(LOG_INFO, "[pipewire]     Framerate: %d/%d",
		     video_stream->format.info.mjpg.framerate.num,
		     video_stream->format.info.mjpg.framerate.denom);

		break;
	case SPA_MEDIA_SUBTYPE_h264:
		if (spa_format_video_h264_parse(param, &video_stream->format.info.h264) < 0)
			return;

		blog(LOG_INFO, "[pipewire] Negotiated format:");
		blog(LOG_INFO, "[pipewire]     Format: (H264)");
		blog(LOG_INFO, "[pipewire]     Size: %dx%d",
		     video_stream->format.info.h264.size.width,
		     video_stream->format.info.h264.size.height);
		blog(LOG_INFO, "[pipewire]     Framerate: %d/%d",
		     video_stream->format.info.h264.framerate.num,
		     video_stream->format.info.h264.framerate.denom);

		break;
	default:
		return;
	}
	if (video_stream->format.media_subtype != SPA_MEDIA_SUBTYPE_raw) {
		if (pipewire_init_decoder(&video_stream->decoder, video_stream->format.media_subtype) < 0) {
			blog(LOG_ERROR, "Failed to initialize decoder");
			return;
		}
	}

	/* Video crop */
	pod_builder =
		SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
	params[0] = spa_pod_builder_add_object(
		&pod_builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop),
		SPA_PARAM_META_size,
		SPA_POD_Int(sizeof(struct spa_meta_region)));

	/* Buffer options */
	params[1] = spa_pod_builder_add_object(
		&pod_builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_dataType, SPA_POD_Int(buffer_types));

	pw_stream_update_params(obs_pw_stream->stream, params, 2);

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

	switch (video_stream->format.media_subtype) {
	case SPA_MEDIA_SUBTYPE_raw:
		return video_stream->format.info.raw.size.width;
	case SPA_MEDIA_SUBTYPE_mjpg:
		return video_stream->format.info.mjpg.size.width;
	case SPA_MEDIA_SUBTYPE_h264:
		return video_stream->format.info.h264.size.width;
	default:
		return 0;
	}
}

static uint32_t video_stream_get_height(obs_pipewire_stream *obs_pw_stream)
{
	if (!obs_pw_stream->negotiated)
		return 0;

	struct _obs_pipewire_stream_video *video_stream =
		video_stream_get_stream(obs_pw_stream);
	switch (video_stream->format.media_subtype) {
	case SPA_MEDIA_SUBTYPE_raw:
		return video_stream->format.info.raw.size.height;
	case SPA_MEDIA_SUBTYPE_mjpg:
		return video_stream->format.info.mjpg.size.height;
	case SPA_MEDIA_SUBTYPE_h264:
		return video_stream->format.info.h264.size.height;
	default:
		return 0;
	}
}

static void video_stream_destroy(obs_pipewire_stream *obs_pw_stream)
{
	if (!obs_pw_stream)
		return;

	struct _obs_pipewire_stream_video *video_stream =
		video_stream_get_stream(obs_pw_stream);
	obs_source_output_video(video_stream->source, NULL);

	if (video_stream->format.media_subtype != SPA_MEDIA_SUBTYPE_raw) {
                pipewire_destroy_decoder(&video_stream->decoder);
        }

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
};

obs_pipewire_stream *obs_pipewire_create_stream_video_async(obs_source_t *source)
{
	struct _obs_pipewire_stream_video *video_stream;

	video_stream = bzalloc(sizeof(struct _obs_pipewire_stream_video));
	video_stream->source = source;
	video_stream->obs_pw_stream.direction = PW_DIRECTION_INPUT;
	video_stream->obs_pw_stream.flags = PW_STREAM_FLAG_AUTOCONNECT |
					    PW_STREAM_FLAG_MAP_BUFFERS;

	obs_pipewire_stream_init(&video_stream->obs_pw_stream, &stream_impl);
	video_stream->format_info.da = create_format_info_async();

	obs_get_video_info(&video_stream->video_info);

	return &video_stream->obs_pw_stream;
}
