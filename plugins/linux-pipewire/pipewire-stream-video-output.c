/* pipewire-stream-video-output.c
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
#include <media-io/video-frame.h>

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

	obs_output_t *output;

	uint32_t seq;

	struct spa_video_info format;

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

static bool prepare_obs_frame(struct spa_video_info *format,
			      struct obs_source_frame *frame)
{
	struct format_data format_data;

	frame->width = format->info.raw.size.width;
	frame->height = format->info.raw.size.height;

	video_format_get_parameters(video_colorspace_from_spa_color_matrix(
					    format->info.raw.color_matrix),
				    video_color_range_from_spa_color_range(
					    format->info.raw.color_range),
				    frame->color_matrix, frame->color_range_min,
				    frame->color_range_max);

	if (!lookup_format_info_from_spa_format(format->info.raw.format,
						&format_data) ||
	    format_data.video_format == VIDEO_FORMAT_NONE)
		return false;

	frame->format = format_data.video_format;
	frame->linesize[0] = SPA_ROUND_UP_N(frame->width * format_data.bpp, 4);
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

static void video_stream_param_changed(obs_pipewire_stream *obs_pw_stream,
				       uint32_t id, const struct spa_pod *param)
{
	struct _obs_pipewire_stream_video *video_stream =
		video_stream_get_stream(obs_pw_stream);
	struct spa_pod_builder pod_builder;
	const struct spa_pod *params[2];
	uint32_t buffer_types;
	uint32_t size;
	uint32_t stride;
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

	struct format_data format_data;
	if (!lookup_format_info_from_spa_format(
		    video_stream->format.info.raw.format, &format_data)) {
		blog(LOG_ERROR, "[pipewire] Unsupported format: %u",
		     video_stream->format.info.raw.format);
		return;
	}

	struct video_scale_info vsi = {0};
	vsi.format = format_data.video_format;
	vsi.width = video_stream->format.info.raw.size.width;
	vsi.height = video_stream->format.info.raw.size.height;
	obs_output_set_video_conversion(video_stream->output, &vsi);

	stride = SPA_ROUND_UP_N(
		format_data.bpp * video_stream->format.info.raw.size.width, 4);
	size = SPA_ROUND_UP_N(stride * video_stream->format.info.raw.size.width,
			      4);

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

	pod_builder =
		SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));

	/* Video crop */
	params[0] = spa_pod_builder_add_object(
		&pod_builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop),
		SPA_PARAM_META_size,
		SPA_POD_Int(sizeof(struct spa_meta_region)));

	/* Buffer options */
	params[1] = spa_pod_builder_add_object(
		&pod_builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
		SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 1, 32),
		SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
		SPA_PARAM_BUFFERS_size, SPA_POD_Int(size),
		SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride),
		SPA_PARAM_BUFFERS_align, SPA_POD_Int(16),
		SPA_PARAM_BUFFERS_dataType, SPA_POD_Int(1 << SPA_DATA_MemPtr));

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
	return video_stream->format.info.raw.size.width;
}

static uint32_t video_stream_get_height(obs_pipewire_stream *obs_pw_stream)
{
	if (!obs_pw_stream->negotiated)
		return 0;

	struct _obs_pipewire_stream_video *video_stream =
		video_stream_get_stream(obs_pw_stream);
	return video_stream->format.info.raw.size.height;
}

static void video_stream_destroy(obs_pipewire_stream *obs_pw_stream)
{
	if (!obs_pw_stream)
		return;

	struct _obs_pipewire_stream_video *video_stream =
		video_stream_get_stream(obs_pw_stream);

	clear_format_info(&video_stream->format_info.da);
	bfree(video_stream);
}

static void video_stream_export_frame(obs_pipewire_stream *obs_pw_stream,
				      struct pw_buffer *b,
				      struct video_data *frame)
{
	struct _obs_pipewire_stream_video *video_stream =
		video_stream_get_stream(obs_pw_stream);
	struct video_frame frame_out = {0};
	struct format_data format_data;
	// check if we have a running pipewire stream
	if (pw_stream_get_state(obs_pw_stream->stream, NULL) !=
	    PW_STREAM_STATE_STREAMING) {
		blog(LOG_INFO, "No node connected");
		return;
	}

	blog(LOG_DEBUG, "exporting frame to pipewire");

	if (!lookup_format_info_from_spa_format(
		    video_stream->format.info.raw.format, &format_data)) {
		blog(LOG_WARNING, "pipewire: unsupported format");
		return;
	}

	struct spa_buffer *spa_buf = b->buffer;
	struct spa_data *d = spa_buf->datas;

	for (unsigned int i = 0; i < spa_buf->n_datas; i++) {
		if (d[i].data == NULL) {
			blog(LOG_WARNING, "pipewire: buffer not mapped");
			continue;
		}
		uint32_t stride = SPA_ROUND_UP_N(
			video_stream->format.info.raw.size.width *
				format_data.bpp,
			4);
		frame_out.data[i] = d[i].data;
		d[i].mapoffset = 0;
		d[i].maxsize =
			video_stream->format.info.raw.size.height * stride;
		d[i].flags = SPA_DATA_FLAG_READABLE;
		d[i].type = SPA_DATA_MemPtr;
		d[i].chunk->offset = 0;
		d[i].chunk->stride = stride;
		d[i].chunk->size =
			video_stream->format.info.raw.size.height * stride;
	}
	video_frame_copy(&frame_out, (struct video_frame *)frame,
			 format_data.video_format, 1);

	struct spa_meta_header *h;
	if ((h = spa_buffer_find_meta_data(spa_buf, SPA_META_Header,
					   sizeof(*h)))) {
		h->pts = frame->timestamp;
		h->flags = 0;
		h->seq = video_stream->seq++;
		h->dts_offset = 0;
	}

	blog(LOG_DEBUG, "********************");
	blog(LOG_DEBUG, "pipewire: fd %lu", d[0].fd);
	blog(LOG_DEBUG, "pipewire: dataptr %p", d[0].data);
	blog(LOG_DEBUG, "pipewire: size %d", d[0].maxsize);
	blog(LOG_DEBUG, "pipewire: stride %d", d[0].chunk->stride);
	blog(LOG_DEBUG, "pipewire: width %d",
	     video_stream->format.info.raw.size.width);
	blog(LOG_DEBUG, "pipewire: height %d",
	     video_stream->format.info.raw.size.height);
	blog(LOG_DEBUG, "********************");
}

static const struct _obs_pipewire_stream_impl stream_impl = {
	.param_changed = video_stream_param_changed,
	.build_format_params = video_stream_build_format_params,
	.destroy = video_stream_destroy,
	.get_width = video_stream_get_width,
	.get_height = video_stream_get_height,
	.export_frame = video_stream_export_frame,
};

obs_pipewire_stream *
obs_pipewire_create_stream_video_output(obs_output_t *output)
{
	struct _obs_pipewire_stream_video *video_stream;

	video_stream = bzalloc(sizeof(struct _obs_pipewire_stream_video));
	video_stream->output = output;
	video_stream->seq = 0;
	video_stream->obs_pw_stream.direction = PW_DIRECTION_OUTPUT;
	video_stream->obs_pw_stream.flags = PW_STREAM_FLAG_AUTOCONNECT |
					    PW_STREAM_FLAG_MAP_BUFFERS |
					    PW_STREAM_FLAG_DRIVER;

	obs_pipewire_stream_init(&video_stream->obs_pw_stream, &stream_impl);
	video_stream->format_info.da = create_format_info_output();

	obs_get_video_info(&video_stream->video_info);

	return &video_stream->obs_pw_stream;
}
